#pragma once
#include "v8trace/core.h"

namespace v8trace {
class NativeHttpDrain {
 public:
  using Clock = std::chrono::steady_clock;
  NativeHttpDrain(fs::path file, uint64_t maximum_bytes);
  void poll(Clock::time_point at = Clock::now());
  bool quiet(Clock::time_point at = Clock::now()) const;
  Json status() const;
 private:
  void line(std::string data, Clock::time_point at);
  fs::path file_;
  uint64_t maximum_bytes_, offset_ = 0, alive_type_ = 0, request_source_ = 0, begin_ = 0, end_ = 0;
  uint64_t starts_ = 0, ends_ = 0, initial_ends_ = 0;
  bool header_ = false, events_ = false, events_closed_ = false, footer_expected_ = false, footer_value_verified_ = false, footer_verified_ = false;
  std::string pending_;
  std::set<std::string> active_;
  Clock::time_point last_activity_{};
};
}
