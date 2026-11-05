#include "v8trace/recording.h"
#include "v8trace/trace_conversion.h"
#include "v8trace/http_drain.h"
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif

namespace v8trace {
namespace {
std::optional<Json> read_recording_status(const fs::path& file, uint64_t& sharing_retries) {
#ifdef _WIN32
  auto handle = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    auto error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return std::nullopt;
    if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION) { ++sharing_retries; return std::nullopt; }
    throw std::system_error(int(error), std::system_category(), "Recording status open failed");
  }
  struct File { HANDLE handle; ~File() { CloseHandle(handle); } } owned{handle};
  LARGE_INTEGER length;
  if (!GetFileSizeEx(handle, &length)) throw std::system_error(int(GetLastError()), std::system_category(), "Recording status size failed");
  if (length.QuadPart < 0 || length.QuadPart > 1048576) throw std::runtime_error("Recording status exceeds byte budget");
  std::string bytes(size_t(length.QuadPart), '\0'); DWORD read = 0;
  if (!ReadFile(handle, bytes.data(), DWORD(bytes.size()), &read, nullptr) || read != bytes.size()) throw std::runtime_error("Recording status short read");
  return parse(bytes);
#else
  if (!fs::exists(file)) return std::nullopt;
  return load_json(file, 1048576);
#endif
}
}

Json capture_native_recording(const Json& o, Store& store, Process& process,
                              std::atomic_bool& stop, RecordingControl& control, const Sink& progress) {
  using Clock = std::chrono::steady_clock;
  const auto config = load_json(store.filename("native-control/config.json"), 1048576);
  const bool startup_handoff = field(config, "startupTraceHandoff", false) == true;
  const bool protobuf_export = str(config, "traceFormat", "json") == "perfetto-protobuf";
  const auto export_protocol = str(config, "traceExportProtocol");
  const bool file_streaming = protobuf_export && export_protocol == "v8trace/perfetto-file-export/2";
  if (str(config, "traceFormat", "json") != "json" && !protobuf_export) throw std::runtime_error("Native recording trace format is unsupported");
  if (protobuf_export && ((export_protocol != "v8trace/perfetto-file-export/1" && export_protocol != "v8trace/perfetto-file-export/2") || str(field(o, "browserIdentity"), "nativeTraceExportProtocol") != export_protocol)) throw std::runtime_error("Native protobuf export is not source-bound to this browser");
  if (file_streaming && str(config, "maxTraceFileBytes") != std::to_string(o.at("maxBytes").get<uint64_t>())) throw std::runtime_error("Native streaming raw byte bound differs from the capture budget");
  const auto token = str(config, "token");
  const auto status_file = store.filename("native-control/status.json");
  std::vector<fs::path> traces, netlogs; Json windows = Json::array(), last_status;
  uint64_t wire_sequence = 0, closed_windows = 0, closed_raw_bytes = 0, status_read_retries = 0;
  RecordingClock clock(true);
  Json live_file_witness = Json::array();
  auto last_file_sample = Clock::now() - std::chrono::seconds(1);
  auto report = [&](const std::string& phase) {
    if (!progress) return;
    auto value = store.progress(phase);
    value.update({{"recordingState", phase}, {"recordingActiveMilliseconds", clock.active_ms()}, {"recordingPausedMilliseconds", clock.paused_ms()}});
    progress(value);
  };
  auto state_changed = [&] {
    auto state = control.status(); state["type"] = "recording-state";
    state["recordingActiveMilliseconds"] = clock.active_ms(); state["recordingPausedMilliseconds"] = clock.paused_ms();
    store.append("recording-control.jsonl", {{"observedAt", now()}, {"state", state}});
    if (progress) progress(state);
  };
  auto read_status = [&]() -> std::optional<Json> {
    auto observed = read_recording_status(status_file, status_read_retries);
    if (!observed) return std::nullopt;
    auto status = std::move(*observed);
    if (str(status, "protocol") != "v8trace/recording/1" || str(status, "captureId") != store.capture_id || str(status, "token") != token || field(status, "browserPid") != process.id()) throw std::runtime_error("Recording browser acknowledgment identity mismatch");
    decimal(str(status, "sequence"));
    if (str(status, "state") == "error") throw std::runtime_error("Browser recording control failed: " + str(status, "error"));
    auto closed = field(status, "lastWindow");
    if (closed.is_object()) {
      auto index = decimal(str(closed, "index"));
      if (index > closed_windows) {
        if (index != closed_windows + 1) throw std::runtime_error("Recording window index skipped");
        if (protobuf_export && (str(closed, "traceFormat") != "perfetto-protobuf" || str(closed, "traceExportProtocol") != export_protocol)) throw std::runtime_error("Native protobuf export acknowledgment format is unproven");
        if (file_streaming && (str(closed, "fileStreamingProtocol") != "v8trace/native-live-file/1" || field(closed, "fileWritePeriodMilliseconds") != 1000 || str(closed, "maxTraceFileBytes") != str(config, "maxTraceFileBytes"))) throw std::runtime_error("Native streaming file drain/byte-bound acknowledgment is unproven");
        if (!protobuf_export && str(closed, "traceFormat", "json") != "json") throw std::runtime_error("Native recording window format differs from its configuration");
        auto accept_file = [&](const std::string& key, const std::string& prefix, std::vector<fs::path>& files) {
          auto name = str(closed, key); if (name.empty()) return;
          auto suffix = key == "trace" && protobuf_export ? ".pftrace" : ".json";
          if (name != prefix + '-' + std::to_string(index) + suffix) throw std::runtime_error("Recording output path invalid");
          auto file = fs::canonical(store.filename("native-output/" + name));
          if (!inside(fs::canonical(store.root), file) || !fs::is_regular_file(file)) throw std::runtime_error("Recording window file missing or outside capture");
          auto length = fs::file_size(file); if (length > o.at("maxBytes").get<uint64_t>() - closed_raw_bytes) throw std::runtime_error("Native recording windows exceed total raw byte budget"); closed_raw_bytes += length;
          if (!length) throw std::runtime_error("Native " + key + " recording window export is empty");
          files.push_back(file);
        };
        accept_file("netlog", "netlog", netlogs); accept_file("trace", "trace", traces);
        windows.push_back(closed); closed_windows = index;
        store.json("recording-windows.json", {{"protocol", "v8trace/recording/1"}, {"windows", windows}, {"outsideWindowsCaptured", startup_handoff}, {"startupTraceHandoffConfigured", startup_handoff}, {"browserKeptRunningAcrossWindows", true}});
      }
    }
    last_status = status; return status;
  };
  auto send = [&](const std::string& command) {
    if (wire_sequence == UINT64_MAX) throw std::runtime_error("Recording command sequence exhausted");
    auto sequence = ++wire_sequence;
    Json request{{"protocol", "v8trace/recording/1"}, {"captureId", store.capture_id}, {"token", token}, {"sequence", std::to_string(sequence)}, {"command", command}};
    auto temporary = store.filename("native-control/request-" + uuid() + ".tmp");
    write_file(temporary, dump(request), true);
    auto destination = store.filename("native-control/request-" + std::to_string(sequence) + ".json");
    try { fs::rename(temporary, destination); }
    catch (...) { std::error_code ignored; fs::remove(temporary, ignored); throw; }
    return sequence;
  };
  auto await = [&](uint64_t sequence, const std::string& phase, bool abort_on_stop) -> std::optional<Json> {
    auto budget = o.at("traceFlushTimeout").get<uint64_t>();
    if (phase == "draining" && field(config, "nativeFlushTimeoutConfigured", false) == true)
      budget = budget * 2 + field(o, "commandTimeout", 30000).get<uint64_t>();
    auto deadline = Clock::now() + std::chrono::milliseconds(budget);
    while (Clock::now() < deadline) {
      if (auto status = read_status()) {
        auto state = str(*status, "state");
        if ((decimal(str(*status, "sequence")) == sequence && (state == "recording" || state == "paused" || state == "finished" || state == "draining-http")) || state == "finished") return status;
      }
      if (auto exit = process.poll()) throw std::runtime_error("Chrome exited before recording acknowledgment: " + dump(*exit));
      if (abort_on_stop && stop) return std::nullopt;
      report(phase); std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    throw std::runtime_error("Browser recording control timeout: " + phase);
  };
  std::string reason = "duration", error; Json http_drain;
  try {
    auto initial = await(0, "launching", true);
    if (initial && str(*initial, "state") != "finished") {
      if (field(*initial, "paused") != field(o, "startPaused", false)) throw std::runtime_error("Browser initial recording state mismatch");
      clock.update(control.paused()); control.ready(); state_changed();
      while (!stop && store.accepting && clock.active_ms() < o.at("durationSeconds").get<uint64_t>() * 1000) {
        if (!control.paused()) {
          auto available = o.at("maxBytes").get<uint64_t>() - closed_raw_bytes;
          auto live = store.filename("native-output/netlog-" + std::to_string(closed_windows + 1) + ".json");
          auto netlog_bytes = fs::exists(live) ? fs::file_size(live) : 0;
          if (netlog_bytes > available) throw std::runtime_error("Native active NetLog exceeds raw byte budget; evidence retained");
          if (file_streaming) {
            auto trace = store.filename("native-output/trace-" + std::to_string(closed_windows + 1) + ".pftrace");
            if (fs::exists(trace) && fs::file_size(trace) > available - netlog_bytes) throw std::runtime_error("Native live trace/NetLog exceed remaining raw byte budget; evidence retained");
          }
        }
        if (auto status = read_status(); status && str(*status, "state") == "finished") { reason = "browser-closed"; break; }
        if (file_streaming && str(last_status, "state") == "recording" && Clock::now() - last_file_sample >= std::chrono::seconds(1)) {
          last_file_sample = Clock::now();
          auto name = "trace-" + std::to_string(closed_windows + 1) + ".pftrace";
          auto file = store.filename("native-output/" + name);
          if (fs::is_regular_file(file) && !fs::is_symlink(file) && fs::file_size(file) > 0) {
            auto length = fs::file_size(file);
            if (live_file_witness.empty() || live_file_witness.back().at("window") != closed_windows + 1)
              live_file_witness.push_back({{"window", closed_windows + 1}, {"file", name}, {"firstBytes", length}, {"firstObservedAt", now()}, {"samples", 0}});
            auto& observed = live_file_witness.back();
            observed["samples"] = observed.at("samples").get<uint64_t>() + 1;
            observed["lastBytes"] = length; observed["lastObservedAt"] = now();
            observed["nativeState"] = "recording"; observed["observedBeforeFinishRequest"] = true;
          }
        }
        if (auto request = control.pending()) {
          clock.update(true);
          auto sequence = send(request->paused ? "pause" : "resume");
          auto status = await(sequence, request->paused ? "pausing" : "resuming", true);
          if (!status) break;
          if (str(*status, "state") == "finished") { reason = "browser-closed"; break; }
          if (field(*status, "paused") != request->paused) throw std::runtime_error("Recording acknowledgment state differs from request");
          control.confirm(*request); clock.update(request->paused); state_changed();
        }
        report(control.paused() ? "paused" : "capturing");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
    }
    if (stop) reason = "interrupted"; else if (!store.accepting) reason = "budget";
    clock.update(true);
    if (str(last_status, "state") != "finished") {
      if (field(config, "httpDrain", false) == true && field(last_status, "paused", true) == false) {
        auto sequence = send("drain");
        auto observed = await(sequence, "draining-http", false);
        if (!observed || str(*observed, "state") != "draining-http" || field(field(*observed, "httpDrain"), "navigationCommitted") != true)
          throw std::runtime_error("Native HTTP navigation drain was not confirmed");
        NativeHttpDrain witness(store.filename("native-output/netlog-" + std::to_string(closed_windows + 1) + ".json"), o.at("maxBytes").get<uint64_t>());
        auto deadline = Clock::now() + std::chrono::milliseconds(o.at("traceFlushTimeout").get<uint64_t>());
        bool settled = false;
        try {
          while (Clock::now() < deadline) {
            witness.poll();
            if (witness.quiet()) { settled = true; break; }
            if (auto exit = process.poll()) throw std::runtime_error("Chrome exited during HTTP drain: " + dump(*exit));
            report("draining-http"); std::this_thread::sleep_for(std::chrono::milliseconds(100));
          }
          if (!settled) store.gap("http-drain-timeout", witness.status());
        } catch (const std::exception& failure) { store.gap("http-drain-witness-failed", {{"error", failure.what()}, {"witness", witness.status()}}); }
        http_drain = witness.status(); http_drain["navigationCommitted"] = true; http_drain["quiescenceVerified"] = settled;
        store.json("http-drain.json", http_drain);
      }
      auto sequence = send("finish");
      auto final = await(sequence, "draining", false);
      if (!final || str(*final, "state") != "finished") throw std::runtime_error("Browser recording finish was not confirmed");
    }
  } catch (const std::exception& failure) { error = failure.what(); clock.update(true); }
  store.json("recording-windows.json", {{"protocol", "v8trace/recording/1"}, {"windows", windows}, {"outsideWindowsCaptured", startup_handoff}, {"startupTraceHandoffConfigured", startup_handoff}, {"browserKeptRunningAcrossWindows", true}});
  Json result{{"protocol", "v8trace/recording/1"}, {"windowCount", closed_windows}, {"activeMilliseconds", clock.active_ms()}, {"pausedMilliseconds", clock.paused_ms()}, {"statusReadSharingRetries", status_read_retries}, {"outsideWindowsCaptured", startup_handoff}, {"stopReason", reason}, {"error", error}};
  result["traceFormat"] = protobuf_export ? "perfetto-protobuf" : "json";
  if (protobuf_export) result["traceExportProtocol"] = export_protocol;
  if (file_streaming) result["nativeLiveFileWitness"] = {{"protocol", "v8trace/native-live-file/1"}, {"windows", live_file_witness}, {"configurationAloneIsNotStreamingProof", true}};
  if (startup_handoff) result["startupTraceHandoff"] = {{"protocol", "v8trace/http-startup-handoff/1"}, {"configured", true}, {"owner", "system"}, {"independentConsumer", false}, {"scope", "Initial native startup buffer is adopted into the single controlled trace; its observed records may precede the regular window start. Explicit user pause/resume never enables this path."}, {"adoptionInferredFromConfiguration", false}};
  if (!http_drain.is_null()) result["httpDrain"] = http_drain;
  if (field(config, "nativeFlushTimeoutConfigured", false) == true)
    result["finishAcknowledgmentBudgetMilliseconds"] = o.at("traceFlushTimeout").get<uint64_t>() * 2 + field(o, "commandTimeout", 30000).get<uint64_t>();
  if (!traces.empty()) {
    try {
      std::vector<fs::path> decoded;
      if (protobuf_export) {
        result["traceConversions"] = Json::array();
        for (const auto& raw : traces) {
          auto output = raw.parent_path() / (utf8(raw.stem()) + ".decoded.json");
          auto timeout = o.at("traceFlushTimeout").get<uint64_t>() * 2 + field(o, "commandTimeout", 30000).get<uint64_t>();
          auto converting = [&](const Json& value) { if (progress) { auto update = store.progress("converting-native-trace"); update.update(value); progress(update); } };
          result["traceConversions"].push_back(convert_perfetto_trace(executable_path().parent_path(), raw, output, o.at("maxBytes").get<uint64_t>(), timeout, converting));
          decoded.push_back(output);
        }
      } else decoded = traces;
      result["traceMerge"] = merge_recording_traces(decoded, store.filename("native-output/trace.json"), o.at("maxBytes").get<uint64_t>());
      if (protobuf_export) for (auto& conversion : result["traceConversions"]) conversion["fullJsonValidation"] = "completed-by-stream-merge";
    }
    catch (const std::exception& failure) { result["traceMergeError"] = {{"error", failure.what()}, {"sourceFileCount", traces.size()}}; if (error.empty()) error = "Native trace merge failed: " + std::string(failure.what()); }
  }
  if (!netlogs.empty()) {
    try { result["netLogMerge"] = merge_recording_traces(netlogs, store.filename("native-output/netlog.json"), o.at("maxBytes").get<uint64_t>(), "events"); }
    catch (const std::exception& failure) { result["netLogMergeError"] = {{"error", failure.what()}, {"sourceFileCount", netlogs.size()}}; if (error.empty()) error = "Native NetLog merge failed: " + std::string(failure.what()); }
  }
  result["error"] = error;
  control.finish(!error.empty()); state_changed();
  store.json("recording-summary.json", result); return result;
}
}
