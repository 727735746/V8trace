#pragma once
#include "v8trace/core.h"
#include <mutex>

namespace v8trace {
class RecordingControl {
 public:
  struct Request { uint64_t generation; bool paused; std::string id; };
  explicit RecordingControl(bool initially_paused = false);
  Request request(bool paused, std::string id);
  std::optional<Request> pending() const;
  void ready();
  void confirm(const Request& request);
  void finish(bool failed = false);
  bool paused() const;
  Json status() const;
 private:
  mutable std::mutex mutex_;
  bool paused_, ready_ = false, finished_ = false, failed_ = false;
  uint64_t generation_ = 0;
  std::optional<Request> pending_;
  std::string last_id_;
};

class RecordingClock {
 public:
  using Clock = std::chrono::steady_clock;
  explicit RecordingClock(bool paused, Clock::time_point at = Clock::now());
  void update(bool paused, Clock::time_point at = Clock::now());
  uint64_t active_ms(Clock::time_point at = Clock::now()) const;
  uint64_t paused_ms(Clock::time_point at = Clock::now()) const;
 private:
  Clock::time_point previous_;
  Clock::duration active_{}, paused_time_{};
  bool paused_;
};

Json merge_recording_traces(const std::vector<fs::path>& files, const fs::path& output,
                            uint64_t maximum_bytes, const std::string& array_key = "traceEvents");
Json capture_native_recording(const Json& options, Store& store, Process& process,
                              std::atomic_bool& stop, RecordingControl& control, const Sink& progress);
}
