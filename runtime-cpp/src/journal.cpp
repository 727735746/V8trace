#include "v8trace/journal.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace v8trace {
struct JournalWriter::Impl {
  struct Barrier { bool done = false; };
  struct Item { std::string target, data; std::shared_ptr<Barrier> barrier; };
  uint64_t capacity, batch_bytes, barriers = 0;
  Write write;
  Flush flush_sink;
  mutable std::mutex mutex;
  std::condition_variable changed;
  std::deque<Item> queue;
  JournalStats counters;
  std::exception_ptr failure;
  bool closed = false, finished = false;
  std::thread worker;

  Impl(uint64_t capacity_value, uint64_t batch_value, Write writer, Flush flusher)
      : capacity(capacity_value), batch_bytes(batch_value), write(std::move(writer)), flush_sink(std::move(flusher)) {
    if (!capacity || !batch_bytes || batch_bytes > capacity || !write || !flush_sink) throw std::runtime_error("Invalid journal buffer configuration");
    counters.capacity_bytes = capacity; counters.batch_bytes = batch_bytes; counters.maximum_pending_rows = std::clamp<uint64_t>(capacity / 1024, 64, 65536);
    worker = std::thread([this] { run(); });
  }
  void check() const { if (failure) std::rethrow_exception(failure); }
  void run() noexcept {
    try {
      for (;;) {
        Item item; uint64_t rows = 0, bytes = 0;
        {
          std::unique_lock lock(mutex);
          const bool was_idle = queue.empty();
          changed.wait(lock, [&] { return closed || !queue.empty(); });
          if (queue.empty()) break;
          if (was_idle && !closed && !barriers && counters.pending_bytes < batch_bytes && counters.pending_rows < counters.maximum_pending_rows)
            changed.wait_for(lock, std::chrono::milliseconds(5), [&] { return closed || barriers || counters.pending_bytes >= batch_bytes || counters.pending_rows >= counters.maximum_pending_rows; });
          item = std::move(queue.front()); queue.pop_front();
          if (item.barrier) --barriers;
          else {
            rows = 1; bytes = item.data.size();
            while (!queue.empty() && !queue.front().barrier && queue.front().target == item.target &&
                   bytes < batch_bytes && queue.front().data.size() <= batch_bytes - bytes) {
              item.data += queue.front().data; bytes += queue.front().data.size(); ++rows; queue.pop_front();
            }
          }
        }
        if (item.barrier) {
          flush_sink();
          { std::lock_guard lock(mutex); ++counters.flushes; item.barrier->done = true; }
        } else {
          write(item.target, item.data);
          // Admission includes the in-flight batch, not only the queue.
          { std::lock_guard lock(mutex); counters.pending_bytes -= bytes; counters.pending_rows -= rows; counters.written_rows += rows; counters.written_bytes += bytes; ++counters.write_batches; }
        }
        changed.notify_all();
      }
      flush_sink();
      { std::lock_guard lock(mutex); ++counters.flushes; finished = true; }
    } catch (...) {
      std::lock_guard lock(mutex); failure = std::current_exception(); closed = true; finished = true; queue.clear();
    }
    changed.notify_all();
  }
};
JournalWriter::JournalWriter(uint64_t capacity, uint64_t batch_bytes, Write write, Flush flush)
    : impl_(std::make_unique<Impl>(capacity, batch_bytes, std::move(write), std::move(flush))) {}
JournalWriter::~JournalWriter() { try { finish(); } catch (...) {} }
void JournalWriter::append(std::string target, std::string data) {
  if (target.empty() || data.empty()) throw std::runtime_error("Empty journal record");
  auto& state = *impl_; auto size = uint64_t(data.size());
  std::unique_lock lock(state.mutex);
  auto ready = [&] {
    if (state.closed || state.failure) return true;
    // A single oversized row is admitted alone; it is never cut to fit.
    if (state.counters.pending_rows >= state.counters.maximum_pending_rows) return false;
    return size > state.capacity ? state.counters.pending_bytes == 0 : state.counters.pending_bytes <= state.capacity - size;
  };
  if (!ready()) { ++state.counters.backpressure_waits; state.changed.wait(lock, ready); }
  state.check(); if (state.closed) throw std::runtime_error("Journal writer is closed");
  state.queue.push_back({std::move(target), std::move(data), {}});
  ++state.counters.enqueued_rows; state.counters.enqueued_bytes += size; state.counters.pending_bytes += size;
  ++state.counters.pending_rows; state.counters.high_water_rows = std::max(state.counters.high_water_rows, state.counters.pending_rows);
  state.counters.high_water_bytes = std::max(state.counters.high_water_bytes, state.counters.pending_bytes);
  if (size > state.capacity) ++state.counters.oversized_rows;
  lock.unlock(); state.changed.notify_all();
}
void JournalWriter::flush() {
  auto& state = *impl_; std::unique_lock lock(state.mutex); state.check();
  if (state.closed) { state.changed.wait(lock, [&] { return state.finished || state.failure; }); state.check(); return; }
  auto barrier = std::make_shared<Impl::Barrier>(); state.queue.push_back({{}, {}, barrier}); ++state.barriers;
  state.changed.notify_all(); state.changed.wait(lock, [&] { return barrier->done || state.failure; }); state.check();
}
void JournalWriter::finish() {
  auto& state = *impl_;
  { std::lock_guard lock(state.mutex); state.closed = true; }
  state.changed.notify_all(); if (state.worker.joinable()) state.worker.join();
  std::lock_guard lock(state.mutex); state.check();
}
JournalStats JournalWriter::stats() const { std::lock_guard lock(impl_->mutex); return impl_->counters; }
}
