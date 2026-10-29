#include "v8trace/core.h"
#include "v8trace/recording.h"
#include <thread>
#include <array>
#include <regex>

namespace v8trace {
namespace {
using Clock = std::chrono::steady_clock;
void pause(unsigned milliseconds = 100) { std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds)); }
bool finalized(const fs::path& file) { if (!fs::exists(file) || !fs::file_size(file)) return false; std::ifstream input(file, std::ios::binary); auto size = fs::file_size(file); input.seekg(std::streamoff(size > 4096 ? size - 4096 : 0)); std::string tail((std::istreambuf_iterator<char>(input)), {}); auto at = tail.find_last_not_of(" \r\n\t"); return at != tail.npos && tail[at] == '}'; }
std::unique_ptr<Trace> await_trace(const fs::path& file, const Json& options, const Sink& progress, Store& store, const std::string& array_key = "traceEvents") {
  auto deadline = Clock::now() + std::chrono::milliseconds(options.at("traceFlushTimeout").get<uint64_t>()); std::string error; uint64_t previous = UINT64_MAX;
  while (Clock::now() < deadline) {
    if (fs::exists(file)) { auto size = fs::file_size(file); if (size > options.at("maxBytes").get<uint64_t>()) throw std::runtime_error("Raw native trace exceeds byte budget"); if (size == previous && finalized(file)) { try { if (progress) { auto decoding = store.progress("decoding-native-trace"); decoding["rawFile"] = utf8(file.filename()); decoding["rawBytes"] = size; progress(decoding); } return std::make_unique<Trace>(file, array_key); } catch (const std::exception& e) { if (field(options, "recordingControl", false) == true) throw; error = e.what(); } } previous = size; }
    if (progress) progress(store.progress("draining")); pause(200);
  }
  throw std::runtime_error("Native output not finalized: " + utf8(file.filename()) + (error.empty() ? "" : ": " + error));
}
}
void import_browser_stdio(Store& store) {
  static const std::regex ack_timeout(R"((^|[\r\n])(\[[0-9]+:[0-9]+:[0-9/.:]+:ERROR:services[\\/]tracing[\\/]perfetto[\\/]consumer_host\.cc:[0-9]+\] Timed out waiting for processes to ack BeginTracing:))");
  for (auto name : {"stdout", "stderr"}) {
    const auto evidence = "native-output/browser-" + std::string(name) + ".log";
    auto file = store.filename(evidence); if (!fs::exists(file)) continue;
    std::ifstream input(file, std::ios::binary); std::array<char, 65536> buffer{};
    std::string tail; uint64_t consumed = 0; bool ack_reported = false;
    while (input) {
      input.read(buffer.data(), buffer.size()); auto bytes = std::string(buffer.data(), size_t(input.gcount()));
      if (bytes.empty()) continue;
      store.append("browser-" + std::string(name) + ".jsonl", {{"observedAt", now()}, {"textBase64", base64(bytes)}, {"representation", "browser-stdio-original-bytes"}});
      auto combined = tail + bytes;
      if (combined.find(":FATAL:") != combined.npos || combined.find("# Fatal error in ") != combined.npos)
        store.gap("native-browser-process-fatal", {{"stream", name}, {"evidence", evidence}});
      std::smatch match;
      if (!ack_reported && std::regex_search(combined, match, ack_timeout)) {
        store.gap("native-tracing-start-ack-timeout", {{"stream", name}, {"evidence", evidence},
            {"byteOffset", consumed - tail.size() + uint64_t(match.position(2))},
            {"startupAcknowledgementVerified", false}, {"occurrence", "first-per-stream"}});
        ack_reported = true;
      }
      consumed += bytes.size();
      tail = combined.substr(combined.size() > 4096 ? combined.size() - 4096 : 0);
    }
    if (!input.eof()) throw std::runtime_error("Browser stdio read failed: " + evidence);
  }
}
Json capture(Json input, const fs::path& assets, std::atomic_bool& stop, Sink progress, Sink events, RecordingControl* control) {
  auto o = validate_options(std::move(input), assets); o["assets"] = utf8(assets); auto output = path(str(o, "output")); fs::create_directories(output.parent_path()); Store store(output, o, std::move(events)); Process process;
  store.json("capture-options.json", {{"schemaVersion",1},{"protocol","v8trace/capture-options/1"},{"options",o}});
  const bool native = str(o, "transport") == "native"; Json result{{"backend", o.at("backend")}, {"transport", o.at("transport")}, {"runtime", "cpp"}, {"runtimeVersion", V8TRACE_VERSION}, {"platform", platform_name()}, {"startedAt", now()}, {"url", o.at("url")}, {"stopReason", "duration"}, {"captureConclusion", "collected"}, {"capabilities", capabilities(o)}, {"selectedCategories", o.at("categories")}, {"profileRetained", true}, {"baselineEligible", false}, {"automaticFallback", false}, {"remoteDebugging", !native}, {"cdpConnected", false}, {"rawCdp", o.at("rawCdp")}, {"pageExpressionsExecuted", false}, {"envHooksInjected", o.at("hooks")}};
  for (auto key : {"causalityBackend", "scenario", "browserIdentity", "opcodeBackend", "detailBackend", "browserBackend", "envBackend", "environmentExecutionPolicy", "stateBackend"}) if (o.contains(key)) result[key] = o.at(key);
  auto report = [&](const std::string& phase) { if (progress) progress(store.progress(phase)); };
  if(o.contains("vmpProbeConfig"))store.json("vmp-probe-spec.json",o.at("vmpProbeConfig"));
  RecordingControl local_control(o.at("startPaused") == true);
  if (o.at("recordingControl") == true && !control) control = &local_control;
  if (o.at("recordingControl") != true) control = nullptr;
  try {
    if (stop) throw std::runtime_error("Capture cancelled before browser launch"); report("permissions"); prepare_permissions(path(str(o, "chromePath"))); store.json("capabilities.json", {{"runtime", "cpp"}, {"capabilities", capabilities(o)}, {"transport", o.at("transport")}});
    if (stop) throw std::runtime_error("Capture cancelled before browser launch");
    if (!native) capture_cdp(o, store, process, stop, progress, result, control);
    else {
      auto launch = launch_config(o, store); store.json("launch.json", {{"backend", o.at("backend")}, {"transport", "native"}, {"runtime", "cpp"}, {"executable", o.at("chromePath")}, {"args", launch.at("args")}, {"options", o}, {"remoteDebugging", false}, {"cdpConnected", false}, {"automaticFallback", false}});
      report("launching"); process.start(path(str(o, "chromePath")), launch.at("args").get<std::vector<std::string>>(), store.root, store.filename("native-output/browser-stdout.log"), store.filename("native-output/browser-stderr.log")); store.json("browser.json", {{"executable", o.at("chromePath")}, {"processId", process.id()}, {"transport", "native"}, {"remoteDebugging", false}, {"runtime", "cpp"}, {"identity", field(o, "browserIdentity")}});
      if (o.at("recordingControl") == true) {
        result["recording"] = capture_native_recording(o, store, process, stop, *control, progress);
        result["stopReason"] = result["recording"].at("stopReason");
        if (!str(result["recording"], "error").empty()) { result["captureConclusion"] = "failed"; result["error"] = result["recording"].at("error"); store.gap("recording-control-failed", {{"error", result["error"]}}); }
        else if (result["recording"].at("windowCount") == 0) result["captureConclusion"] = "no-recording-windows";
      } else {
        auto end = Clock::now() + std::chrono::seconds(o.at("durationSeconds").get<uint64_t>());
        while (Clock::now() < end) { report("capturing"); if (stop) { result["stopReason"] = "interrupted"; process.close(); break; } if (auto exit = process.poll()) { result["browserExit"] = *exit; if (field(*exit, "code") != 0 || !field(*exit, "signal").is_null()) throw std::runtime_error("Chrome exited during native capture: " + dump(*exit)); result["stopReason"] = "browser-closed"; break; } pause(200); }
      }
      report("draining");
      if (launch.at("tracing") == true && (o.at("recordingControl") != true || result["recording"].contains("traceMerge"))) {
        auto trace = await_trace(store.filename("native-output/trace.json"), o, progress, store); if (!trace->has_array) throw std::runtime_error("Native trace has no traceEvents array"); auto stats = diagnostics(trace->properties); result["traceDiagnostics"] = stats;
        if (field(stats, "statsAvailable") != true || field(stats, "finalFlushSucceeded") != true) store.gap("native-trace-flush-not-verified", stats); for (auto& loss : stats.at("losses")) store.gap("native-trace-data-loss", loss);
        for (const auto& counter : stats.at("globalCounters")) store.gap("native-trace-service-global-counter", counter, false);
        auto blob = store.blob_file(store.filename("native-output/trace.json"), "chrome-native-startup-trace-json"); store.json("native-trace.json", {{"blob", blob}, {"transport", "native"}, {"rawFile", "native-output/trace.json"}, {"diagnostics", stats}}); import_trace(*trace, store, o); store.json("browser-metadata.json", field(trace->properties, "metadata", Json::object())); store.json("native-decoder.json", {{"runtime", "cpp"}, {"eventCount", trace->event_count}, {"streaming", true}, {"ordering", trace->sort_statistics}, {"externalSortMemoryBytes", 8388608}, {"mergeFanIn", 16}, {"temporarySpoolCleanup", "RAII-owned-only"}});
      }
      if (o.at("netLog") == true && (o.at("recordingControl") != true || result["recording"].contains("netLogMerge"))) { auto trace = await_trace(store.filename("native-output/netlog.json"), o, progress, store, "events"); if (!trace->has_array || !trace->properties.contains("constants")) throw std::runtime_error("NetLog events/constants missing"); auto blob = store.blob_file(store.filename("native-output/netlog.json"), "chrome-native-netlog-everything-json"); result["networkDiagnostic"] = {{"blob", blob}, {"events", trace->event_count}, {"decodedHttpBodiesCaptured", false}}; Json names = Json::object(); auto types = field(trace->properties["constants"], "logEventTypes", Json::object()); for (auto it = types.begin(); it != types.end(); ++it) names[text(it.value())] = it.key(); trace->each("generic", [&](const Json& row) { auto event = row.at("event"); auto type = text(field(event, "type")); event["eventName"] = field(names, type, type); event["decodedHttpBodiesCaptured"] = false; store.event("http", "netlog-event", event); }); store.json("netlog.json", result["networkDiagnostic"]); }
    }
  } catch (const std::exception& e) { if (control) control->finish(true); result["captureConclusion"] = "failed"; result["error"] = e.what(); store.gap("capture-error", {{"error", e.what()}, {"transport", o.at("transport")}}); }
  report("draining");
  if (process.id()) {
    try { process.close(); auto end = Clock::now() + std::chrono::seconds(3); while (!process.poll() && Clock::now() < end) pause(50); if (!process.poll()) { process.kill(); auto stats = field(result, "traceDiagnostics"); store.gap("owned-browser-forced-close-after-flush", {{"processId", process.id()}}, native && field(stats, "finalFlushSucceeded") != true); end = Clock::now() + std::chrono::seconds(3); while (!process.poll() && Clock::now() < end) pause(50); } if (auto exit = process.poll()) result["browserExit"] = *exit; }
    catch (const std::exception& e) { store.gap("owned-browser-close-error", {{"error", e.what()}}); }
  }
  import_browser_stdio(store);
  if (o.at("correlations") == true) try { report("correlating"); correlate(store); result["evidenceCorrelations"] = load_json(store.filename("correlation-summary.json")); } catch (const std::exception& e) { store.gap("correlation-build-failed", {{"error", e.what()}}); }
  result["finishedAt"] = now(); auto summary = store.finalize(result); report(str(result, "captureConclusion") == "failed" ? "failed" : "completed"); return summary;
}
}
