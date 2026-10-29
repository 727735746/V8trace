#ifndef V8_RUNTIME_V8TRACE_CALL_LEDGER_H_
#define V8_RUNTIME_V8TRACE_CALL_LEDGER_H_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace v8trace_call_ledger {
struct Producer {
  uint64_t epoch, producer_id;
  int thread_id;
  std::atomic<uint64_t> writing{0}, attempted{0}, selected{0}, filtered{0}, emitted{0};
  Producer(uint64_t generation, uint64_t id, int thread) : epoch(generation), producer_id(id), thread_id(thread) {}
};
struct Snapshot {
  uint64_t epoch, producer_id;
  int thread_id;
  bool final;
  uint64_t attempted, selected, filtered, emitted;
};
class Ledger {
 public:
  std::atomic<uint64_t> epoch{1};
  std::atomic<bool> accepting{false};
  std::shared_ptr<Producer> Register(int thread_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto producer = std::make_shared<Producer>(epoch.load(std::memory_order_relaxed), ++next_producer_, thread_id);
    producers_.push_back(producer); return producer;
  }
  void Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (accepting.load(std::memory_order_acquire)) return;
    producers_.clear(); epoch.fetch_add(1, std::memory_order_release);
    accepting.store(true, std::memory_order_seq_cst);
  }
  std::vector<Snapshot> Stop(std::chrono::milliseconds timeout = std::chrono::milliseconds(250)) {
    accepting.store(false, std::memory_order_seq_cst);
    std::vector<std::shared_ptr<Producer>> producers;
    { std::lock_guard<std::mutex> lock(mutex_); producers = producers_; }
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      bool idle = true;
      for (const auto& producer : producers) idle = idle && producer->writing.load(std::memory_order_seq_cst) == 0;
      if (idle) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::vector<Snapshot> snapshots;
    for (const auto& producer : producers) {
      bool final = producer->writing.load(std::memory_order_seq_cst) == 0;
      snapshots.push_back({producer->epoch, producer->producer_id, producer->thread_id, final,
          producer->attempted.load(std::memory_order_relaxed), producer->selected.load(std::memory_order_relaxed),
          producer->filtered.load(std::memory_order_relaxed), producer->emitted.load(std::memory_order_relaxed)});
    }
    return snapshots;
  }
 private:
  uint64_t next_producer_ = 0;
  std::mutex mutex_;
  std::vector<std::shared_ptr<Producer>> producers_;
};
class WriteGuard {
 public:
  WriteGuard(Ledger& ledger, std::shared_ptr<Producer> producer) : producer_(std::move(producer)) {
    // Admission and the stop gate share a total order; the final zero synchronizes counter reads.
    producer_->writing.fetch_add(1, std::memory_order_seq_cst);
    active_ = ledger.accepting.load(std::memory_order_seq_cst);
  }
  ~WriteGuard() { producer_->writing.fetch_sub(1, std::memory_order_release); }
  WriteGuard(const WriteGuard&) = delete;
  WriteGuard& operator=(const WriteGuard&) = delete;
  explicit operator bool() const { return active_; }
 private:
  std::shared_ptr<Producer> producer_;
  bool active_;
};
}
#endif
