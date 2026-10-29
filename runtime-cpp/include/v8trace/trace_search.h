#pragma once
#include "v8trace/core.h"

namespace v8trace {
struct TraceSearchConfig {
  fs::path source_root, index_dir, export_root;
  uint64_t checkpoint_bytes = 8388608, max_read_lines = 100000;
  uint64_t max_matches = 10000, max_content_bytes = 16777216;
  uint64_t max_line_bytes = 67108864, max_pattern_bytes = 16384;
  uint64_t max_export_lines = 100000000, search_memory_bytes = 1073741824;
  unsigned search_threads = 0, query_concurrency = 4;
};
class TraceSearchEngine {
 public:
  explicit TraceSearchEngine(TraceSearchConfig config);
  ~TraceSearchEngine();
  TraceSearchEngine(const TraceSearchEngine&) = delete;
  TraceSearchEngine& operator=(const TraceSearchEngine&) = delete;
  Json open(const Json& request);
  Json status() const;
  Json close();
  Json read(const Json& request, const std::atomic_bool* cancel = nullptr);
  Json search(const Json& request, const std::atomic_bool* cancel = nullptr);
  Json export_lines(const Json& request, const std::atomic_bool* cancel = nullptr);
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
