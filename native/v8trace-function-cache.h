#ifndef V8_RUNTIME_V8TRACE_FUNCTION_CACHE_H_
#define V8_RUNTIME_V8TRACE_FUNCTION_CACHE_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <unordered_map>
#include <utility>

namespace v8trace_cache {
struct FunctionKey {
  uint64_t isolate;
  int32_t unique_id, script_id;
  uint32_t gc_epoch = 0;
  bool operator==(const FunctionKey&) const = default;
};
struct FunctionHash {
  size_t operator()(const FunctionKey& key) const {
    auto hash = std::hash<uint64_t>{}(key.isolate);
    hash ^= std::hash<int32_t>{}(key.unique_id) + (hash << 6) + (hash >> 2);
    hash ^= std::hash<int32_t>{}(key.script_id) + (hash << 6) + (hash >> 2);
    return hash ^ (std::hash<uint32_t>{}(key.gc_epoch) + (hash << 6) + (hash >> 2));
  }
};
template <typename Value, size_t Capacity>
class FunctionCache {
  static_assert(Capacity > 0);
 public:
  FunctionCache() = default;
  FunctionCache(const FunctionCache&) = delete;
  FunctionCache& operator=(const FunctionCache&) = delete;
  Value* Find(FunctionKey key, uintptr_t name_stamp, uintptr_t url_stamp) {
    auto found = entries_.find(key);
    if (found == entries_.end()) return nullptr;
    auto entry = found->second;
    // Stamps only invalidate metadata; they are never dereferenced after a GC.
    if (entry->name_stamp != name_stamp || entry->url_stamp != url_stamp) {
      order_.erase(entry); entries_.erase(found); return nullptr;
    }
    order_.splice(order_.begin(), order_, entry);
    return &entry->value;
  }
  Value& Put(FunctionKey key, uintptr_t name_stamp, uintptr_t url_stamp, Value value) {
    auto found = entries_.find(key);
    if (found != entries_.end()) { order_.erase(found->second); entries_.erase(found); }
    if (order_.size() >= Capacity) { entries_.erase(order_.back().key); order_.pop_back(); }
    order_.push_front({key, name_stamp, url_stamp, std::move(value)});
    entries_[key] = order_.begin();
    return order_.front().value;
  }
  size_t size() const { return entries_.size(); }
 private:
  struct Entry { FunctionKey key; uintptr_t name_stamp, url_stamp; Value value; };
  std::list<Entry> order_;
  std::unordered_map<FunctionKey, typename std::list<Entry>::iterator, FunctionHash> entries_;
};
}
#endif
