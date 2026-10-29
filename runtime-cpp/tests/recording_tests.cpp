#include "v8trace/recording.h"
#include <future>
#include <iostream>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

using namespace v8trace;
namespace {
unsigned checks = 0;
void check(bool value, const char* name) { ++checks; if (!value) throw std::runtime_error(name); }
template<class F> void rejected(F fn, const char* name) { bool failed = false; try { fn(); } catch (const std::exception&) { failed = true; } check(failed, name); }
void state_checks() {
  RecordingControl control;
  check(str(control.status(), "state") == "starting" && !control.paused(), "initial recording state");
  rejected([&] { control.request(true, "early"); }, "transition before startup acknowledgment rejected");
  control.ready(); auto first = control.request(true, "pause");
  check(str(control.status(), "state") == "pausing" && !control.paused(), "pause waits for producer acknowledgment");
  rejected([&] { control.request(false, "racing"); }, "second pending transition rejected");
  auto stale = first; ++stale.generation;
  rejected([&] { control.confirm(stale); }, "stale acknowledgment rejected");
  control.confirm(first); check(control.paused() && str(control.status(), "state") == "paused", "pause acknowledged");
  rejected([&] { control.confirm(first); }, "duplicate acknowledgment rejected");
  for (unsigned i = 0; i < 100; ++i) { auto request = control.request(i % 2 == 1, std::to_string(i)); auto observed = std::async(std::launch::async, [&] { return control.pending(); }).get(); check(observed && observed->generation == request.generation, "thread observes consistent pending request"); control.confirm(request); }
  check(control.paused(), "repeated pause resume final state");
  auto same = control.request(true, "idempotent"); control.confirm(same); check(control.paused(), "same-state acknowledgment is valid");
  rejected([&] { control.request(false, ""); }, "empty request id rejected");
  rejected([&] { control.request(false, std::string(65, 'A')); }, "oversized request id rejected");
  control.finish(); check(str(control.status(), "state") == "finished" && field(control.status(), "ready") == false, "finished control not actionable");
  rejected([&] { control.request(false, "late"); }, "late request rejected");
  RecordingControl paused(true); paused.ready(); check(paused.paused(), "start paused"); paused.finish(true); check(str(paused.status(), "state") == "failed", "failed capture state");
}
void clock_checks() {
  using namespace std::chrono;
  RecordingClock::Clock::time_point zero;
  RecordingClock clock(true, zero);
  check(clock.active_ms(zero + seconds(10)) == 0 && clock.paused_ms(zero + seconds(10)) == 10000, "initial pause consumes no duration");
  clock.update(false, zero + seconds(10));
  clock.update(true, zero + seconds(12));
  check(clock.active_ms(zero + seconds(50)) == 2000 && clock.paused_ms(zero + seconds(50)) == 48000, "active duration frozen while paused");
  clock.update(false, zero + seconds(50)); clock.update(true, zero + seconds(53));
  check(clock.active_ms(zero + seconds(60)) == 5000 && clock.paused_ms(zero + seconds(60)) == 55000, "active intervals accumulate across resumes");
  rejected([&] { clock.update(false, zero); }, "backwards clock rejected");
}
void merge_checks(const fs::path& root) {
  auto first = root / "first.json", second = root / "second.json";
  auto trace = [](const std::string& name, int timestamp, bool stats = true) { Json value{{"traceEvents", Json::array({{{"name", name}, {"ph", "I"}, {"ts", timestamp}, {"pid", 1}, {"tid", 1}, {"args", {{"full", "中文完整值"}}}}})}}; if (stats) value["metadata"] = {{"trace_processor_stats", {{"traced_final_flush_succeeded", 1}}}}; return value; };
  write_json(first, trace("first", 200)); write_json(second, trace("second", 1)); auto combined = root / "combined.json";
  auto report = merge_recording_traces({first, second}, combined, 1048576);
  check(report.at("eventCount") == 2 && field(report, "streaming") == true, "all events merged streaming");
  auto value = load_json(combined); check(value.at("traceEvents")[0].at("v8traceWindow") == 1 && value.at("traceEvents")[1].at("v8traceWindow") == 2, "windows tagged without changing raw args");
  check(value.at("traceEvents")[1].at("args").at("full") == "中文完整值", "full Unicode value preserved");
  check(field(diagnostics(value), "finalFlushSucceeded") == true, "all final flush statistics retained");
  Trace sorted(combined); Json names = Json::array(); sorted.each("generic", [&](const Json& row) { names.push_back(row.at("event").at("name")); });
  check(names == Json::array({"first", "second"}), "recording window order precedes reset timestamps");
  rejected([&] { merge_recording_traces({first}, combined, 1048576); }, "existing output never overwritten");
  rejected([&] { merge_recording_traces({first, first}, root / "duplicate.json", 1048576); }, "duplicate inputs rejected");
  rejected([&] { merge_recording_traces({first}, root / "tiny.json", 5); }, "raw byte budget enforced");
  write_json(second, trace("no-stats", 1, false)); auto missing = root / "missing.json"; merge_recording_traces({first, second}, missing, 1048576);
  check(field(diagnostics(load_json(missing)), "finalFlushSucceeded") == false, "missing per-window statistics never reported as complete");
  check(diagnostics(load_json(missing)).at("losses").empty(), "missing flush proof never invents a failed flush");
  auto unknown = trace("unverified", 1); unknown["metadata"]["trace_processor_stats"] = {{"traced_final_flush_succeeded", 0}, {"traced_final_flush_failed", 0}};
  write_json(second, unknown); auto unverified = root / "unverified.json"; merge_recording_traces({first, second}, unverified, 1048576);
  auto unverified_stats = diagnostics(load_json(unverified));
  check(field(unverified_stats, "finalFlushSucceeded") == false && unverified_stats.at("losses").empty(), "zero raw outcomes remain unverified rather than failed");
  unknown["metadata"]["trace_processor_stats"]["traced_final_flush_failed"] = 1;
  write_json(second, unknown); auto failed = root / "failed-flush.json"; merge_recording_traces({first, second}, failed, 1048576);
  check(diagnostics(load_json(failed)).at("losses").size() == 1 && field(diagnostics(load_json(failed)), "finalFlushSucceeded") == false, "actual failed flush retained exactly once");
  auto loss = trace("loss", 1); loss["metadata"]["trace_processor_stats"]["data_loss_test"] = 2; write_json(second, loss); auto lost = root / "lost.json"; merge_recording_traces({first, second}, lost, 1048576);
  check(diagnostics(load_json(lost)).at("losses").size() == 1, "per-window losses retained recursively");
  write_file(second, "{\"traceEvents\":[],\"traceEvents\":[]}"); rejected([&] { merge_recording_traces({second}, root / "bad.json", 1048576); }, "duplicate JSON keys rejected");
  write_file(second, "{\"traceEvents\":[{}"); rejected([&] { merge_recording_traces({second}, root / "partial.json", 1048576); }, "unfinished windows rejected and originals retained");
  Json netlog{{"constants", {{"logEventTypes", {{"REQUEST", 1}}}}}, {"events", Json::array({{{"type", 1}, {"params", {{"body", "FULL"}}}}})}};
  write_json(first, netlog); write_json(second, netlog); auto netfile = root / "netlog.json"; merge_recording_traces({first, second}, netfile, 1048576, "events");
  check(load_json(netfile).at("events").size() == 2 && load_json(netfile).at("constants") == netlog.at("constants"), "NetLog events and constants merged losslessly");
  netlog["constants"]["logEventTypes"]["REQUEST"] = 2; write_json(second, netlog); rejected([&] { merge_recording_traces({first, second}, root / "different.json", 1048576, "events"); }, "changed NetLog constants rejected");
}
int native_fixture(const fs::path& root, const std::string& mode) {
  auto config = load_json(root / "native-control/config.json");
#ifdef _WIN32
  uint64_t pid = GetCurrentProcessId();
#else
  uint64_t pid = uint64_t(getpid());
#endif
  uint64_t sequence = 0, window = 0; bool paused = true; Json last;
  auto publish = [&](const std::string& state) {
    Json status{{"protocol", "v8trace/recording/1"}, {"captureId", config.at("captureId")}, {"token", mode == "wrong-token" ? Json("WRONG") : config.at("token")}, {"browserPid", pid}, {"sequence", std::to_string(sequence)}, {"state", state}, {"paused", paused}};
    if (!last.is_null()) status["lastWindow"] = last;
    auto file = root / "native-control/status.json", temporary = root / "native-control/status.tmp"; write_file(temporary, dump(status));
#ifdef _WIN32
    if (mode == "locked-status" && sequence == 0) {
      auto locked = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (locked == INVALID_HANDLE_VALUE) throw std::runtime_error("Fixture locked status open failed");
      auto bytes = dump(status); DWORD written = 0;
      bool success = WriteFile(locked, bytes.data(), DWORD(bytes.size()), &written, nullptr) && written == bytes.size() && FlushFileBuffers(locked);
      std::this_thread::sleep_for(std::chrono::milliseconds(300)); CloseHandle(locked); fs::remove(temporary);
      if (!success) throw std::runtime_error("Fixture locked status write failed");
      return;
    }
    if (!MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("Fixture atomic status failed");
#else
    fs::rename(temporary, file);
#endif
  };
  auto close = [&] {
    auto trace = "trace-" + std::to_string(window) + ".json", netlog = "netlog-" + std::to_string(window) + ".json";
    write_json(root / "native-output" / trace, {{"traceEvents", Json::array({{{"name", "fixture-window"}, {"ts", window}, {"args", {{"full", "完整记录-END"}}}}})}, {"metadata", {{"trace_processor_stats", {{"traced_final_flush_succeeded", 1}}}}}});
    write_json(root / "native-output" / netlog, {{"events", Json::array({{{"type", 1}, {"params", {{"full", "FULL_NETLOG_END"}}}}})}, {"constants", {{"logEventTypes", {{"FIXTURE", 1}}}}}});
    last = {{"index", std::to_string(mode == "skipped-window" ? window + 1 : window)}, {"trace", mode == "outside-file" ? "../outside.json" : trace}, {"netlog", netlog}}; paused = true;
  };
  publish("paused"); auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline) {
    auto request_file = root / "native-control" / ("request-" + std::to_string(sequence + 1) + ".json");
    if (!fs::exists(request_file)) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); continue; }
    auto request = load_json(request_file); ++sequence;
    if (str(request, "command") == "finish") { if (!paused) close(); publish("finished"); return 0; }
    if (str(request, "command") == "pause" && !paused) close();
    if (str(request, "command") == "resume" && paused) { ++window; paused = false; }
    publish(paused ? "paused" : "recording");
  }
  return 3;
}
void native_checks(const fs::path& root, const std::string& mode) {
  Json options{{"durationSeconds", 2}, {"startPaused", true}, {"traceFlushTimeout", 3000}, {"maxBytes", 1048576}, {"categories", Json::array({"http"})}};
  Store store(root / mode, options);
  store.json("native-control/config.json", {{"captureId", store.capture_id}, {"token", sha256("fixture")}}); store.filename("native-output/unused");
  Process process; process.start(executable_path(), {"--native-fixture", utf8(store.root), mode}, store.root, store.filename("fixture-stdout.log"), store.filename("fixture-stderr.log"));
  auto pid = process.id(); std::atomic_bool stop = false; RecordingControl control(true); Json states = Json::array();
  auto result = capture_native_recording(options, store, process, stop, control, [&](Json value) {
    if (str(value, "type") != "recording-state") return; states.push_back(value);
    auto generation = field(value, "generation", 0).get<uint64_t>();
    if (str(value, "state") == "paused" && generation == 0) control.request(false, "resume-one");
    else if (str(value, "state") == "recording" && generation == 1) control.request(true, "pause-one");
    else if (str(value, "state") == "paused" && generation == 2) control.request(false, "resume-two");
    else if (str(value, "state") == "recording" && generation == 3) stop = true;
  });
  if (!process.poll()) process.kill();
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3); while (!process.poll() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  check(process.id() == pid && process.poll().has_value(), "native control keeps one owned process and releases it");
  if (mode == "normal" || mode == "locked-status") {
    check(str(result, "error").empty() && result.at("windowCount") == 2, "native IPC opens and closes two windows");
    check(str(result, "stopReason") == "interrupted" && states.size() == 5, "native acknowledgments complete and stop saves active window");
    check(result.at("traceMerge").at("eventCount") == 2 && result.at("netLogMerge").at("eventCount") == 2, "native trace and NetLog windows merged");
    check(fs::exists(store.filename("native-output/trace-1.json")) && fs::exists(store.filename("native-output/trace-2.json")), "native original windows retained");
    check(field(control.status(), "ready") == false && str(control.status(), "state") == "finished", "native finished state settled");
#ifdef _WIN32
    if (mode == "locked-status") check(result.at("statusReadSharingRetries") > 0, "transient Windows sharing lock retried within acknowledgment deadline");
#endif
  } else {
    check(!str(result, "error").empty() && str(control.status(), "state") == "failed", "invalid native acknowledgment fails explicitly");
    check(!result.contains("traceMerge"), "invalid native acknowledgment is never imported");
  }
}
void option_checks(const fs::path& root) {
  auto assets = path(V8TRACE_RECORDING_ASSETS);
  Json input{{"url", "http://fixture.test/"}, {"chromePath", utf8(executable_path())}, {"output", utf8(root / "native-plan")}, {"transport", "native"}, {"recordingControl", true}, {"startPaused", true}, {"hooks", true}, {"scripts", true}, {"netLog", true}};
  auto options = validate_options(input, assets, false);
  check(options.at("recordingControl") == true && options.at("startPaused") == true, "recording options normalized");
  Store store(root / "native-launch", options); auto launch = launch_config(options, store); auto args = launch.at("args").get<std::vector<std::string>>();
  check(args.back() == "about:blank", "native target navigation waits for initial recording acknowledgment");
  bool local_control = false, hook_gate = false, forbidden = false;
  for (auto& arg : args) { local_control |= arg.starts_with("--v8-trace-recording-control="); hook_gate |= arg == "--v8-trace-env-hooks"; forbidden |= arg.starts_with("--trace-config-file=") || arg.starts_with("--log-net-log=") || arg.starts_with("--remote-debugging"); }
  check(local_control && hook_gate && !forbidden, "native recording has no CDP or independent always-on trace/NetLog flags");
  auto config = load_json(store.filename("native-control/config.json")); check(str(config, "protocol") == "v8trace/recording/1" && str(config, "captureId") == store.capture_id && str(config, "token").size() == 64 && config.at("startPaused") == true, "native configuration identity and initial pause");
  input["recordingControl"] = false; rejected([&] { validate_options(input, assets, false); }, "start paused requires enabled control");
  input["recordingControl"] = "yes"; rejected([&] { validate_options(input, assets, false); }, "recording control requires boolean");
  input["recordingControl"] = true; input["transport"] = "cdp"; input["netLog"] = false; options = validate_options(input, assets, false); check(capabilities(options).at("recording.control") == "cdp-domain-pause-resume", "CDP recording capability declared");
  input["startPaused"] = false; input.erase("recordingControl"); input.erase("startPaused"); options = validate_options(input, assets, false); check(options.at("recordingControl") == false && options.at("startPaused") == false, "legacy CLI configs keep prior always-on behavior");
}
}
int main(int argc, char** argv) {
  if (argc == 4 && std::string(argv[1]) == "--native-fixture") return native_fixture(path(argv[2]), argv[3]);
  auto root = fs::temp_directory_path() / ("v8trace-recording-unit-" + uuid());
  try { fs::create_directory(root); state_checks(); clock_checks(); merge_checks(root); option_checks(root); for (auto mode : {"normal", "locked-status", "wrong-token", "skipped-window", "outside-file"}) native_checks(root, mode); fs::remove_all(root); std::cout << dump(Json{{"status", "PASS"}, {"checks", checks}, {"nativeIpcFixtureOnly", true}}) << '\n'; return 0; }
  catch (const std::exception& e) { std::error_code ignored; fs::remove_all(root, ignored); std::cerr << e.what() << '\n'; return 1; }
}
