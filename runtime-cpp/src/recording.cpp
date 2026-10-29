#include "v8trace/recording.h"
#include <algorithm>

namespace v8trace {
RecordingControl::RecordingControl(bool initially_paused) : paused_(initially_paused) {}
RecordingControl::Request RecordingControl::request(bool paused, std::string id) {
  std::lock_guard lock(mutex_);
  if (!ready_ || finished_) throw std::runtime_error("Recording control is not ready");
  if (pending_) throw std::runtime_error("Another recording transition is pending");
  if (id.empty() || id.size() > 64) throw std::runtime_error("Recording request ID invalid");
  if (generation_ == UINT64_MAX) throw std::runtime_error("Recording generation exhausted");
  pending_ = Request{++generation_, paused, std::move(id)}; return *pending_;
}
std::optional<RecordingControl::Request> RecordingControl::pending() const { std::lock_guard lock(mutex_); return pending_; }
void RecordingControl::ready() { std::lock_guard lock(mutex_); if (finished_) throw std::runtime_error("Recording already finished"); ready_ = true; }
void RecordingControl::confirm(const Request& request) {
  std::lock_guard lock(mutex_);
  if (!pending_ || pending_->generation != request.generation || pending_->id != request.id || pending_->paused != request.paused || finished_) throw std::runtime_error("Stale recording acknowledgment");
  paused_ = request.paused; last_id_ = request.id; pending_.reset();
}
void RecordingControl::finish(bool failed) { std::lock_guard lock(mutex_); finished_ = true; failed_ = failed; pending_.reset(); }
bool RecordingControl::paused() const { std::lock_guard lock(mutex_); return paused_; }
Json RecordingControl::status() const {
  std::lock_guard lock(mutex_);
  auto state = finished_ ? (failed_ ? "failed" : "finished") : !ready_ ? "starting" : pending_ ? (pending_->paused ? "pausing" : "resuming") : paused_ ? "paused" : "recording";
  return {{"protocol", "v8trace/recording/1"}, {"state", state}, {"paused", paused_}, {"ready", ready_ && !finished_}, {"pending", pending_.has_value()}, {"generation", generation_}, {"requestId", pending_ ? pending_->id : last_id_}};
}
RecordingClock::RecordingClock(bool paused, Clock::time_point at) : previous_(at), paused_(paused) {}
void RecordingClock::update(bool paused, Clock::time_point at) {
  if (at < previous_) throw std::runtime_error("Recording clock moved backwards");
  (paused_ ? paused_time_ : active_) += at - previous_; previous_ = at; paused_ = paused;
}
uint64_t RecordingClock::active_ms(Clock::time_point at) const { return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(active_ + (!paused_ ? std::max(at - previous_, Clock::duration::zero()) : Clock::duration::zero())).count()); }
uint64_t RecordingClock::paused_ms(Clock::time_point at) const { return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(paused_time_ + (paused_ ? std::max(at - previous_, Clock::duration::zero()) : Clock::duration::zero())).count()); }

Json merge_recording_traces(const std::vector<fs::path>& files, const fs::path& output,
                            uint64_t maximum_bytes, const std::string& array_key) {
  if (array_key != "traceEvents" && array_key != "events") throw std::runtime_error("Invalid recording trace array");
  if (fs::exists(output) || fs::is_symlink(output)) throw std::runtime_error("Merged recording output must be a new file");
  uint64_t input_bytes = 0, output_bytes = 0, records = 0;
  std::set<fs::path> unique;
  for (const auto& file : files) {
    auto canonical = fs::canonical(file);
    if (canonical == fs::absolute(output).lexically_normal() || !unique.insert(canonical).second) throw std::runtime_error("Duplicate or self-referencing recording trace");
    auto bytes = fs::file_size(file);
    if (bytes > maximum_bytes - input_bytes) throw std::runtime_error("Recording windows exceed total raw byte budget");
    input_bytes += bytes;
  }
  std::ofstream merged(output, std::ios::binary | std::ios::trunc);
  if (!merged) throw std::runtime_error("Merged recording trace open failed");
  auto emit = [&](std::string_view data) {
    if (data.size() > maximum_bytes - output_bytes) throw std::runtime_error("Merged recording trace exceeds byte budget");
    merged.write(data.data(), std::streamsize(data.size())); if (!merged) throw std::runtime_error("Merged recording trace write failed"); output_bytes += data.size();
  };
  emit("{\"" + array_key + "\":[");
  Json windows = Json::array(), all_stats = Json::object(), constants;
  bool complete = true;
  for (size_t window = 0; window < files.size(); ++window) {
    std::ifstream input(files[window], std::ios::binary); JsonReader reader(input); reader.expect('{'); reader.white();
    Json properties = Json::object(); std::set<std::string> keys; bool found = false;
    while (reader.peek() != '}') {
      if (!keys.empty()) reader.expect(','); auto key = reader.value();
      if (!key.is_string() || !keys.insert(text(key)).second) throw std::runtime_error("Duplicate or invalid recording trace key");
      reader.expect(':');
      if (text(key) == array_key) {
        found = true; reader.expect('['); reader.white(); bool first = true;
        while (reader.peek() != ']') {
          if (!first) reader.expect(','); first = false; auto event = reader.value();
          if (!event.is_object()) throw std::runtime_error("Recording trace event must be an object");
          if (event.contains("v8traceWindow")) throw std::runtime_error("Input trace contains reserved recording window field");
          event["v8traceWindow"] = window + 1;
          if (records++) emit(","); emit(dump(event));
          reader.white();
        }
        reader.expect(']');
      } else properties[text(key)] = reader.value();
      reader.white();
    }
    reader.expect('}'); if (!reader.ended() || !found) throw std::runtime_error("Recording trace is incomplete");
    auto stats = diagnostics(properties); complete = complete && field(stats, "statsAvailable") == true && field(stats, "finalFlushSucceeded") == true;
    windows.push_back({{"window", window + 1}, {"file", utf8(files[window].filename())}, {"diagnostics", stats}, {"metadata", field(properties, "metadata", Json::object())}});
    all_stats[std::to_string(window + 1)] = field(field(properties, "metadata"), "trace_processor_stats", Json::object());
    if (array_key == "events") {
      auto current = field(properties, "constants");
      if (!current.is_object()) throw std::runtime_error("Recording NetLog constants missing");
      if (constants.is_null()) constants = current;
      else if (field(current, "logEventTypes") != field(constants, "logEventTypes")) throw std::runtime_error("Recording NetLog event constants changed");
    }
  }
  emit("]");
  if (array_key == "traceEvents") emit(",\"metadata\":" + dump(Json{{"v8traceRecordingWindows", windows}, {"trace_processor_stats", {{"traced_final_flush_succeeded", complete ? 1 : 0}, {"recording_windows", all_stats}}}}));
  else emit(",\"constants\":" + dump(constants.is_null() ? Json::object() : constants));
  emit("}"); merged.flush(); if (!merged) throw std::runtime_error("Merged recording trace flush failed");
  return {{"windows", windows}, {"eventCount", records}, {"inputBytes", input_bytes}, {"outputBytes", output_bytes}, {"streaming", true}, {"originalWindowFilesRetained", true}};
}
}
