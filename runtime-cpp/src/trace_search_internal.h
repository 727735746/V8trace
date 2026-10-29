#pragma once
#include "v8trace/trace_search.h"
#include <condition_variable>
#include <mutex>
#include <thread>

namespace v8trace::trace_search {
void require(bool value, std::string_view message);
void cancelled(const std::atomic_bool* stop);
uint64_t number(const Json& value, std::string_view key, std::optional<uint64_t> fallback = {});
bool boolean(const Json& value, std::string_view key, bool fallback);
std::pair<std::string, bool> lossy_utf8(std::string_view bytes);
std::pair<std::string, bool> lossy_utf8_limited(std::string_view bytes, uint64_t limit, bool& exceeded);
void publish(const fs::path& temporary, const fs::path& destination, bool overwrite);
void sync_file(const fs::path& file);
struct Source {
  fs::path file;
  uint64_t size = 0;
  int64_t modified = 0;
  std::string file_id, path_hash, sample_hash;
  Json identity() const;
  void verify(bool samples = false) const;
};
Source inspect_source(const fs::path& file, const fs::path& allowed_root);
struct Checkpoint { uint64_t line = 1, offset = 0; };
struct LineIndex {
  Source source;
  fs::path index_file;
  uint64_t line_count = 0, interval = 0;
  std::vector<Checkpoint> checkpoints;
  uint64_t offset(uint64_t line, const std::atomic_bool* stop = nullptr) const;
};
struct IndexProgress {
  std::atomic<uint64_t> bytes{0}, lines{0}, resume_offset{0};
  std::atomic_bool cancel{false}, finished{false};
  std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
};
std::shared_ptr<LineIndex> load_index(const Source& source, const TraceSearchConfig& config);
std::shared_ptr<LineIndex> build_index(const Source& source, const TraceSearchConfig& config,
                                     bool force, IndexProgress& progress);
Json search_index(const LineIndex& index, const TraceSearchConfig& config, const Json& request, const std::atomic_bool* stop);
Json export_index(const LineIndex& index, const TraceSearchConfig& config, const Json& request, const std::atomic_bool* stop);
}
