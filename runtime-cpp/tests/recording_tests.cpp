#include "v8trace/recording.h"
#include <algorithm>
#include <future>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
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
std::optional<Json> fixture_request(const fs::path& file) {
#ifdef _WIN32
  auto handle = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    auto error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND || error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION) return std::nullopt;
    throw std::system_error(int(error), std::system_category(), "Fixture command open failed");
  }
  struct File { HANDLE value; ~File() { CloseHandle(value); } } owned{handle};
  LARGE_INTEGER size;
  if (!GetFileSizeEx(handle, &size) || size.QuadPart < 0 || size.QuadPart > 1048576) throw std::runtime_error("Fixture command size invalid");
  std::string bytes(size_t(size.QuadPart), '\0'); DWORD read = 0;
  if (!ReadFile(handle, bytes.data(), DWORD(bytes.size()), &read, nullptr) || read != bytes.size()) throw std::runtime_error("Fixture command short read");
  return parse(bytes);
#else
  if (!fs::exists(file)) return std::nullopt;
  return load_json(file, 1048576);
#endif
}
void fixture_request_checks(const fs::path& root) {
  auto file = root / "fixture-request.json";
  check(!fixture_request(file), "fixture command absent is pending");
  Json expected{{"protocol", "v8trace/recording/1"}, {"sequence", "1"}, {"command", "finish"}};
  write_json(file, expected);
#ifdef _WIN32
  auto locked = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  check(locked != INVALID_HANDLE_VALUE, "fixture command lock acquired");
  auto pending = fixture_request(file); CloseHandle(locked);
  check(!pending, "transient command sharing lock stays pending within the fixture deadline");
#endif
  check(fixture_request(file) == expected, "fixture command resumes with exact published bytes");
  write_file(file, "{invalid");
  rejected([&] { fixture_request(file); }, "malformed fixture command is not a transient sharing lock");
}
#ifdef _WIN32
void publish_fixture_status(const fs::path& temporary, const fs::path& file) {
  for (unsigned attempt = 0;; ++attempt) {
    if (MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return;
    auto error = GetLastError();
    if (attempt == 49 || (error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION && error != ERROR_LOCK_VIOLATION)) throw std::system_error(int(error), std::system_category(), "Fixture atomic status failed");
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}
void fixture_publication_checks(const fs::path& root) {
  auto file = root / "publish-status.json", temporary = root / "publish-status.tmp";
  write_file(file, "OLD"); write_file(temporary, "NEW");
  auto locked = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  check(locked != INVALID_HANDLE_VALUE, "fixture publication lock opens");
  check(!MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH), "fixture publication lock really blocks replacement");
  auto release = std::async(std::launch::async, [locked] { std::this_thread::sleep_for(std::chrono::milliseconds(75)); CloseHandle(locked); });
  publish_fixture_status(temporary, file); release.get();
  check(read_file(file) == "NEW" && !fs::exists(temporary), "fixture publication retries preserve exact bytes and atomic move");
  write_file(temporary, "NEXT");
  locked = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  check(locked != INVALID_HANDLE_VALUE, "fixture permanent publication lock opens");
  bool failed = false;
  try { publish_fixture_status(temporary, file); }
  catch (const std::system_error&) { failed = true; }
  CloseHandle(locked);
  check(failed && read_file(file) == "NEW" && read_file(temporary) == "NEXT", "fixture permanent sharing conflict stays failed with both files retained");
  fs::remove(temporary);
  failed = false;
  try { publish_fixture_status(temporary, file); }
  catch (const std::system_error& error) { failed = error.code().value() == ERROR_FILE_NOT_FOUND; }
  check(failed && read_file(file) == "NEW", "fixture missing source retains original Win32 error and destination");
}
#else
void fixture_publication_checks(const fs::path&) {}
#endif
int native_fixture(const fs::path& root, const std::string& mode) {
  auto config = load_json(root / "native-control/config.json");
#ifdef _WIN32
  uint64_t pid = GetCurrentProcessId();
#else
  uint64_t pid = uint64_t(getpid());
#endif
  uint64_t sequence = 0, window = 0; bool paused = true, drain_requested = false; Json last;
  const bool draining = mode == "http-drain" || mode == "http-drain-stalled";
  const Json drain_constants{{"logEventTypes", {{"REQUEST_ALIVE", 7}}}, {"logSourceType", {{"URL_REQUEST", 11}}}, {"logEventPhase", {{"PHASE_BEGIN", 1}, {"PHASE_END", 2}}}};
  auto request_event = [](unsigned phase) { return Json{{"type", 7}, {"phase", phase}, {"source", {{"type", 11}, {"id", 1}}}}; };
  auto publish = [&](const std::string& state) {
    Json status{{"protocol", "v8trace/recording/1"}, {"captureId", config.at("captureId")}, {"token", mode == "wrong-token" ? Json("WRONG") : config.at("token")}, {"browserPid", pid}, {"sequence", std::to_string(sequence)}, {"state", state}, {"paused", paused}};
    if (!last.is_null()) status["lastWindow"] = last;
    if (drain_requested) status["httpDrain"] = {{"protocol", "v8trace/http-drain/1"}, {"navigationCommitted", true}};
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
    publish_fixture_status(temporary, file);
#else
    fs::rename(temporary, file);
#endif
  };
  auto close = [&] {
    auto trace = "trace-" + std::to_string(window) + ".json", netlog = "netlog-" + std::to_string(window) + ".json";
    write_json(root / "native-output" / trace, {{"traceEvents", Json::array({{{"name", "fixture-window"}, {"ts", window}, {"args", {{"full", "完整记录-END"}}}}})}, {"metadata", {{"trace_processor_stats", {{"traced_final_flush_succeeded", 1}}}}}});
    if (mode == "empty-trace") write_file(root / "native-output" / trace, "");
    if (mode == "malformed-trace") write_file(root / "native-output" / trace, "{\"traceEvents\":[");
    write_json(root / "native-output" / netlog, {{"events", Json::array({{{"type", 1}, {"params", {{"full", "FULL_NETLOG_END"}}}}})}, {"constants", {{"logEventTypes", {{"FIXTURE", 1}}}}}});
    if (draining) {
      Json events = Json::array({request_event(1)}); if (mode != "http-drain-stalled" || window == 1) events.push_back(request_event(2));
      write_json(root / "native-output" / netlog, {{"constants", drain_constants}, {"events", events}});
    }
    last = {{"index", std::to_string(mode == "skipped-window" ? window + 1 : window)}, {"trace", mode == "outside-file" ? "../outside.json" : trace}, {"netlog", netlog}}; paused = true;
  };
  publish("paused"); auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline) {
    auto request_file = root / "native-control" / ("request-" + std::to_string(sequence + 1) + ".json");
    auto observed = fixture_request(request_file);
    if (!observed) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); continue; }
    auto request = std::move(*observed); ++sequence;
    if (str(request, "command") == "drain") {
      if (!draining || paused) return 4;
      drain_requested = true;
      if (mode == "http-drain") { std::ofstream out(root / "native-output" / ("netlog-" + std::to_string(window) + ".json"), std::ios::binary | std::ios::app); out << dump(request_event(2)) << ",\n"; if (!out) return 5; }
      publish("draining-http"); continue;
    }
    if (str(request, "command") == "finish") { if (!paused) close(); publish("finished"); return 0; }
    if (str(request, "command") == "pause" && !paused) close();
    if (str(request, "command") == "resume" && paused) {
      ++window; paused = false;
      if (draining) write_file(root / "native-output" / ("netlog-" + std::to_string(window) + ".json"), "{\"constants\":" + dump(drain_constants) + ",\n\"events\": [\n" + dump(request_event(1)) + ",\n");
    }
    publish(paused ? "paused" : "recording");
  }
  return 3;
}
void native_checks(const fs::path& root, const std::string& mode) {
  Json options{{"durationSeconds", 2}, {"startPaused", true}, {"traceFlushTimeout", 3000}, {"maxBytes", 1048576}, {"categories", Json::array({"http"})}};
  if (mode == "protobuf-mismatch") options["browserIdentity"] = {{"nativeTraceExportProtocol", "v8trace/perfetto-file-export/1"}};
  if (mode == "streaming-ack-mismatch") options["browserIdentity"] = {{"nativeTraceExportProtocol", "v8trace/perfetto-file-export/2"}};
  Store store(root / mode, options);
  const bool draining = mode == "http-drain" || mode == "http-drain-stalled";
  store.json("native-control/config.json", {{"captureId", store.capture_id}, {"token", sha256("fixture")}, {"httpDrain", draining}}); store.filename("native-output/unused");
  if (mode == "protobuf-mismatch") {
    auto config = load_json(store.filename("native-control/config.json")); config["traceFormat"] = "perfetto-protobuf"; config["traceExportProtocol"] = "v8trace/perfetto-file-export/1";
    store.json("native-control/config.json", config);
  }
  if (mode == "streaming-ack-mismatch") {
    auto config = load_json(store.filename("native-control/config.json"));
    config["traceFormat"] = "perfetto-protobuf"; config["traceExportProtocol"] = "v8trace/perfetto-file-export/2";
    config["maxTraceFileBytes"] = std::to_string(options.at("maxBytes").get<uint64_t>());
    store.json("native-control/config.json", config);
  }
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
  store.json("fixture-result.json", {{"mode", mode}, {"result", result}, {"states", states}, {"control", control.status()}});
  if (!process.poll()) process.kill();
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3); while (!process.poll() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  check(process.id() == pid && process.poll().has_value(), "native control keeps one owned process and releases it");
  if (mode == "normal" || mode == "locked-status" || draining) {
    if (!str(result, "error").empty() || field(result, "windowCount") != 2) std::cerr << dump(Json{{"mode", mode}, {"result", result}, {"states", states}, {"capture", utf8(store.root)}}) << '\n';
    check(str(result, "error").empty() && result.at("windowCount") == 2, "native IPC opens and closes two windows");
    check(str(result, "stopReason") == "interrupted" && states.size() == 5, "native acknowledgments complete and stop saves active window");
    check(result.at("traceMerge").at("eventCount") == 2 && result.at("netLogMerge").at("eventCount") == (draining ? (mode == "http-drain-stalled" ? 3 : 4) : 2), "native trace and NetLog windows merged");
    if (draining) {
      check(result.at("httpDrain").at("navigationCommitted") == true, "native drain navigation acknowledgment required");
      check(result.at("httpDrain").at("quiescenceVerified") == (mode == "http-drain"), "live network lifetime witness decides drain, not a navigation alone");
      check(store.critical_gaps == (mode == "http-drain-stalled" ? 1 : 0), "stalled request remains a critical drain timeout");
    }
    check(fs::exists(store.filename("native-output/trace-1.json")) && fs::exists(store.filename("native-output/trace-2.json")), "native original windows retained");
    check(field(control.status(), "ready") == false && str(control.status(), "state") == "finished", "native finished state settled");
#ifdef _WIN32
    if (mode == "locked-status") check(result.at("statusReadSharingRetries") > 0, "transient Windows sharing lock retried within acknowledgment deadline");
#endif
  } else if (mode == "empty-trace" || mode == "malformed-trace") {
    check(!str(result, "error").empty() && str(control.status(), "state") == "failed", "empty or malformed export settles as failed, not finished");
    check(!result.contains("traceMerge") && result.contains("netLogMerge"), "failed trace preserves the valid independent NetLog merge");
    check(result.at("netLogMerge").at("eventCount") == (mode == "empty-trace" ? 1 : 2), "all acknowledged independent NetLog records retained after trace failure");
    check(load_json(store.filename("recording-summary.json")) == result, "trace failure still publishes its exact recording summary");
    if (mode == "empty-trace") check(str(result, "error").find("export is empty") != std::string::npos && fs::file_size(store.filename("native-output/trace-1.json")) == 0, "empty raw export diagnosed explicitly and retained unchanged");
    else check(result.contains("traceMergeError") && read_file(store.filename("native-output/trace-1.json")) == "{\"traceEvents\":[", "malformed raw export and merge error retained unchanged");
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
  options["transport"] = "native"; options["http"] = true; options["netLog"] = true;
  options["browserIdentity"] = {{"httpDrainProtocol", "v8trace/http-drain/1"}};
  Store coordinated(root / "native-http-drain-launch", options); auto automatic = launch_config(options, coordinated);
  check(automatic.at("controlled") == true && automatic.at("httpDrain") == true && options.at("recordingControl") == false, "supported HTTP drain coordinates native finish without enabling user pause controls");
  auto automatic_config = load_json(coordinated.filename("native-control/config.json"));
  auto automatic_args = automatic.at("args").get<std::vector<std::string>>();
  check(automatic.at("startupTraceHandoff") == true && automatic_config.at("startupTraceHandoff") == true && automatic.at("config").at("startup_duration") == 0, "automatic HTTP capture arms a startup buffer without an independent consumer timer");
  check(std::find(automatic_args.begin(), automatic_args.end(), "--trace-startup-owner=system") != automatic_args.end() && automatic_config.at("traceConfig") == automatic.at("config").at("trace_config"), "startup producer and controlled consumer share the exact trace configuration");
  auto manual_options = options; manual_options["recordingControl"] = true;
  Store manual(root / "native-manual-no-startup", manual_options); auto manual_launch = launch_config(manual_options, manual);
  auto manual_args = manual_launch.at("args").get<std::vector<std::string>>();
  check(manual_launch.at("startupTraceHandoff") == false && std::find(manual_args.begin(), manual_args.end(), "--trace-startup-owner=system") == manual_args.end(), "explicit user recording controls never capture a startup prefix outside their selected windows");
  options["netLog"] = false; Store no_witness(root / "native-no-witness", options);
  check(launch_config(options, no_witness).at("httpDrain") == false, "HTTP drain never silently enables an unselected network witness");
  options["netLog"] = true; options["browserIdentity"] = Json::object(); Store legacy(root / "native-legacy-drain", options);
  check(launch_config(options, legacy).at("httpDrain") == false, "legacy browser is not sent an unsupported native command");
  options["browserIdentity"] = {{"nativeFlushTimeoutProtocol", "v8trace/native-flush-timeout/1"}};
  Store timeout(root / "native-flush-timeout", options); auto timed = launch_config(options, timeout).at("args").get<std::vector<std::string>>();
  auto timeout_flag = "--v8-trace-flush-timeout-ms=" + text(options.at("traceFlushTimeout"));
  check(std::find(timed.begin(), timed.end(), timeout_flag) != timed.end(), "configured flush budget reaches the native tracing service transport");
  options["traceFlushTimeout"] = 99; Store invalid_timeout(root / "native-invalid-flush-timeout", options);
  rejected([&] { launch_config(options, invalid_timeout); }, "unsupported native flush budget is rejected rather than silently using a different timeout");
}
void protobuf_option_checks(const fs::path& root) {
  Json input{{"url", "http://fixture.test/"}, {"chromePath", utf8(executable_path())}, {"output", utf8(root / "protobuf-plan")},
      {"transport", "native"}, {"recordingControl", true}, {"startPaused", true}, {"scripts", true}, {"netLog", true}};
  auto options = validate_options(input, path(V8TRACE_RECORDING_ASSETS), false);
  options["browserIdentity"] = {{"nativeTraceExportProtocol", "v8trace/perfetto-file-export/1"}, {"httpDrainProtocol", "v8trace/http-drain/1"}};
  Store explicit_store(root / "protobuf-explicit", options); auto explicit_launch = launch_config(options, explicit_store);
  auto explicit_args = explicit_launch.at("args").get<std::vector<std::string>>();
  check(std::find(explicit_args.begin(), explicit_args.end(), "--v8-trace-protobuf-export") != explicit_args.end(), "source-bound controlled native trace selects streaming protobuf export");
  auto config = load_json(explicit_store.filename("native-control/config.json"));
  check(config.at("traceFormat") == "perfetto-protobuf" && config.at("traceExportProtocol") == "v8trace/perfetto-file-export/1", "native output format and protocol recorded explicitly");
  check(explicit_launch.at("startupTraceHandoff") == false && std::none_of(explicit_args.begin(), explicit_args.end(), [](const std::string& arg) { return arg.starts_with("--trace-startup-format="); }), "explicit pause control does not acquire a hidden startup prefix");
  options["recordingControl"] = false; options["startPaused"] = false; options["http"] = true;
  Store automatic_store(root / "protobuf-automatic", options); auto automatic = launch_config(options, automatic_store);
  auto arguments = automatic.at("args").get<std::vector<std::string>>();
  check(std::find(arguments.begin(), arguments.end(), "--trace-startup-format=proto") != arguments.end() && std::find(arguments.begin(), arguments.end(), "--trace-startup-owner=system") != arguments.end(), "automatic startup buffer uses the same protobuf consumer format");
  check(std::none_of(arguments.begin(), arguments.end(), [](const std::string& arg) { return arg.starts_with("--log-net-log=") || arg.starts_with("--remote-debugging"); }), "protobuf export adds no independent NetLog or DevTools consumer");
  options["browserIdentity"]["nativeTraceExportProtocol"] = "v8trace/perfetto-file-export/2";
  Store streaming_store(root / "protobuf-live-file", options); auto streaming = launch_config(options, streaming_store);
  auto streaming_config = load_json(streaming_store.filename("native-control/config.json"));
  check(streaming_config.at("traceExportProtocol") == "v8trace/perfetto-file-export/2" &&
      streaming_config.at("maxTraceFileBytes") == std::to_string(options.at("maxBytes").get<uint64_t>()), "source-bound continuous Native file export carries its exact existing byte budget");
  check(streaming.at("config").at("trace_config").at("trace_buffer_size_in_kb") == automatic.at("config").at("trace_config").at("trace_buffer_size_in_kb"), "continuous Native file draining never enlarges the trace buffer");
  streaming_config["maxTraceFileBytes"] = "1"; streaming_store.json("native-control/config.json", streaming_config);
  Process unstarted; RecordingControl bounded(true); std::atomic_bool stop = false;
  rejected([&] { capture_native_recording(options, streaming_store, unstarted, stop, bounded, {}); }, "mismatched continuous raw-file budget is rejected before native I/O");
  options["browserIdentity"].erase("nativeTraceExportProtocol");
  Store legacy_store(root / "protobuf-legacy", options); auto legacy = launch_config(options, legacy_store).at("args").get<std::vector<std::string>>();
  check(std::find(legacy.begin(), legacy.end(), "--v8-trace-protobuf-export") == legacy.end() && std::find(legacy.begin(), legacy.end(), "--trace-startup-format=json") != legacy.end(), "legacy browser never receives an unsupported protobuf request");
}
}
int main(int argc, char** argv) {
  if (argc == 4 && std::string(argv[1]) == "--native-fixture") {
    try { return native_fixture(path(argv[2]), argv[3]); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 2; }
  }
  auto root = fs::temp_directory_path() / ("v8trace-recording-unit-" + uuid());
  const bool protobuf_converter = fs::is_regular_file(executable_path().parent_path() / "trace-converter.json");
  try { fs::create_directory(root); state_checks(); clock_checks(); merge_checks(root); option_checks(root); if (protobuf_converter) protobuf_option_checks(root); fixture_request_checks(root); fixture_publication_checks(root); std::vector<std::string> modes{"normal", "locked-status", "wrong-token", "skipped-window", "outside-file", "http-drain", "http-drain-stalled", "empty-trace", "malformed-trace", "protobuf-mismatch", "streaming-ack-mismatch"}; if (!protobuf_converter) modes.erase(std::remove_if(modes.begin(), modes.end(), [](const std::string& mode) { return mode == "protobuf-mismatch" || mode == "streaming-ack-mismatch"; }), modes.end()); for (const auto& mode : modes) native_checks(root, mode); fs::remove_all(root); std::cout << dump(Json{{"status", "PASS"}, {"checks", checks}, {"nativeIpcFixtureOnly", true}, {"protobufConverter", protobuf_converter}}) << '\n'; return 0; }
  catch (const std::exception& e) { std::cerr << e.what() << "; fixture evidence retained at " << utf8(root) << '\n'; return 1; }
}
