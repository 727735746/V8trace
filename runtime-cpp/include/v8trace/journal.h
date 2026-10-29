#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace v8trace {
struct JournalStats {
  uint64_t enqueued_rows = 0, written_rows = 0, enqueued_bytes = 0, written_bytes = 0;
  uint64_t write_batches = 0, flushes = 0, backpressure_waits = 0, oversized_rows = 0;
  uint64_t pending_bytes = 0, high_water_bytes = 0;
  uint64_t pending_rows = 0, high_water_rows = 0, capacity_bytes = 0, batch_bytes = 0, maximum_pending_rows = 0;
};
class JournalWriter {
 public:
  using Write = std::function<void(std::string_view, std::string_view)>;
  using Flush = std::function<void()>;
  JournalWriter(uint64_t capacity, uint64_t batch_bytes, Write write, Flush flush);
  ~JournalWriter();
  JournalWriter(const JournalWriter&) = delete;
  JournalWriter& operator=(const JournalWriter&) = delete;
  void append(std::string target, std::string data);
  void flush();
  void finish();
  JournalStats stats() const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
