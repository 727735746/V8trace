#include "v8trace/recording.h"
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
  const auto token = str(config, "token");
  const auto status_file = store.filename("native-control/status.json");
  std::vector<fs::path> traces, netlogs; Json windows = Json::array(), last_status;
  uint64_t wire_sequence = 0, closed_windows = 0, closed_raw_bytes = 0, status_read_retries = 0;
  RecordingClock clock(true);
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
        auto accept_file = [&](const std::string& key, const std::string& prefix, std::vector<fs::path>& files) {
          auto name = str(closed, key); if (name.empty()) return;
          if (name != prefix + '-' + std::to_string(index) + ".json") throw std::runtime_error("Recording output path invalid");
          auto file = fs::canonical(store.filename("native-output/" + name));
          if (!inside(fs::canonical(store.root), file) || !fs::is_regular_file(file)) throw std::runtime_error("Recording window file missing or outside capture");
          auto length = fs::file_size(file); if (length > o.at("maxBytes").get<uint64_t>() - closed_raw_bytes) throw std::runtime_error("Native recording windows exceed total raw byte budget"); closed_raw_bytes += length;
          files.push_back(file);
        };
        accept_file("trace", "trace", traces); accept_file("netlog", "netlog", netlogs);
        windows.push_back(closed); closed_windows = index;
        store.json("recording-windows.json", {{"protocol", "v8trace/recording/1"}, {"windows", windows}, {"outsideWindowsCaptured", false}, {"browserKeptRunningAcrossWindows", true}});
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
    auto deadline = Clock::now() + std::chrono::milliseconds(o.at("traceFlushTimeout").get<uint64_t>());
    while (Clock::now() < deadline) {
      if (auto status = read_status()) {
        auto state = str(*status, "state");
        if ((decimal(str(*status, "sequence")) == sequence && (state == "recording" || state == "paused" || state == "finished")) || state == "finished") return status;
      }
      if (auto exit = process.poll()) throw std::runtime_error("Chrome exited before recording acknowledgment: " + dump(*exit));
      if (abort_on_stop && stop) return std::nullopt;
      report(phase); std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    throw std::runtime_error("Browser recording control timeout: " + phase);
  };
  std::string reason = "duration", error;
  try {
    auto initial = await(0, "launching", true);
    if (initial && str(*initial, "state") != "finished") {
      if (field(*initial, "paused") != field(o, "startPaused", false)) throw std::runtime_error("Browser initial recording state mismatch");
      clock.update(control.paused()); control.ready(); state_changed();
      while (!stop && store.accepting && clock.active_ms() < o.at("durationSeconds").get<uint64_t>() * 1000) {
        if (!control.paused()) { auto live = store.filename("native-output/netlog-" + std::to_string(closed_windows + 1) + ".json"); if (fs::exists(live) && fs::file_size(live) > o.at("maxBytes").get<uint64_t>() - closed_raw_bytes) throw std::runtime_error("Native active NetLog exceeds raw byte budget; evidence retained"); }
        if (auto status = read_status(); status && str(*status, "state") == "finished") { reason = "browser-closed"; break; }
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
      auto sequence = send("finish");
      auto final = await(sequence, "draining", false);
      if (!final || str(*final, "state") != "finished") throw std::runtime_error("Browser recording finish was not confirmed");
    }
  } catch (const std::exception& failure) { error = failure.what(); clock.update(true); }
  control.finish(!error.empty()); state_changed();
  store.json("recording-windows.json", {{"protocol", "v8trace/recording/1"}, {"windows", windows}, {"outsideWindowsCaptured", false}, {"browserKeptRunningAcrossWindows", true}});
  Json result{{"protocol", "v8trace/recording/1"}, {"windowCount", closed_windows}, {"activeMilliseconds", clock.active_ms()}, {"pausedMilliseconds", clock.paused_ms()}, {"statusReadSharingRetries", status_read_retries}, {"outsideWindowsCaptured", false}, {"stopReason", reason}, {"error", error}};
  if (!traces.empty()) result["traceMerge"] = merge_recording_traces(traces, store.filename("native-output/trace.json"), o.at("maxBytes").get<uint64_t>());
  if (!netlogs.empty()) result["netLogMerge"] = merge_recording_traces(netlogs, store.filename("native-output/netlog.json"), o.at("maxBytes").get<uint64_t>(), "events");
  store.json("recording-summary.json", result); return result;
}
}
