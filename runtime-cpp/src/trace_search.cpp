#include "trace_search_internal.h"
#include <algorithm>
#include <array>
#include <limits>

namespace v8trace {
using namespace trace_search;
namespace {
Json empty_status() {
  return {{"state","no_file"},{"path",nullptr},{"index_path",nullptr},{"source_size",nullptr},{"bytes_indexed",nullptr},{"progress_percent",nullptr},{"lines_seen",nullptr},{"line_count",nullptr},{"checkpoint_count",nullptr},{"elapsed_seconds",nullptr},{"bytes_per_second",nullptr},{"eta_seconds",nullptr},{"error",nullptr}};
}
bool strict_bool(const Json& request, const char* key, bool fallback) {
  if (!request.contains(key)) return fallback;
  require(request.at(key).is_boolean(), std::string("Expected boolean: ") + key); return request.at(key).get<bool>();
}
uint64_t selected_count(const LineIndex& index, uint64_t start, uint64_t count) {
  require(start > 0 && start - 1 <= index.line_count, "Line is outside the file; EOF is line_count + 1");
  return std::min(count, index.line_count - (start - 1));
}
Json next_line(const LineIndex& index, uint64_t start, uint64_t count) {
  auto next = start + count; return next <= index.line_count ? Json(next) : Json(nullptr);
}
}
struct TraceSearchEngine::Impl {
  TraceSearchConfig config;
  mutable std::mutex state_mutex;
  std::mutex operations;
  std::string state = "no_file", error;
  uint64_t generation = 0;
  std::optional<Source> source;
  std::shared_ptr<LineIndex> ready;
  struct Job {
    uint64_t generation = 0;
    std::shared_ptr<IndexProgress> progress = std::make_shared<IndexProgress>();
    std::thread thread;
  };
  std::vector<std::shared_ptr<Job>> jobs;
  std::shared_ptr<Job> current_job;
  struct Gate {
    std::mutex mutex;
    std::condition_variable available;
    unsigned used = 0, maximum = 4;
  };
  std::shared_ptr<Gate> gate = std::make_shared<Gate>();
  struct Access {
    std::shared_ptr<LineIndex> index;
    std::shared_ptr<void> permit;
  };
  explicit Impl(TraceSearchConfig value) : config(std::move(value)) {
    require(!config.source_root.empty() && fs::is_directory(config.source_root), "An existing source root is required");
    config.source_root = fs::canonical(config.source_root);
    require(!config.index_dir.empty() && !config.export_root.empty(), "Index and export roots are required");
    config.index_dir = io_path(config.index_dir);
    fs::create_directories(config.index_dir); fs::create_directories(config.export_root);
    config.index_dir = io_path(fs::canonical(config.index_dir)); config.export_root = fs::canonical(config.export_root);
    require(config.checkpoint_bytes >= 65536 && config.checkpoint_bytes <= 1073741824, "Checkpoint interval must be 64 KiB through 1 GiB");
    require(config.max_read_lines && config.max_matches && config.max_content_bytes && config.max_line_bytes && config.max_pattern_bytes && config.search_memory_bytes, "Query limits must be positive");
    require(config.max_content_bytes <= 536870912 && config.max_line_bytes <= 2147483648ULL, "Configured query buffers exceed supported bounded range");
    require(config.max_matches <= 1000000 && config.max_pattern_bytes <= 1048576, "Configured match or pattern limits exceed supported bounded range");
    require(config.query_concurrency && config.query_concurrency <= 64 && config.search_threads <= 32, "Invalid query concurrency or search thread count");
    gate->maximum = config.query_concurrency;
  }
  ~Impl() {
    for (auto& job : jobs) job->progress->cancel = true;
    for (auto& job : jobs) if (job->thread.joinable()) job->thread.join();
  }
  Json status() const {
    std::lock_guard lock(state_mutex); auto output = empty_status(); output["state"] = state;
    if (source) { output["path"] = utf8(source->file); output["source_size"] = source->size; }
    if (state == "ready" && ready) {
      output["index_path"] = utf8(logical_path(ready->index_file)); output["bytes_indexed"] = ready->source.size;
      output["progress_percent"] = 100.0; output["lines_seen"] = ready->line_count; output["line_count"] = ready->line_count; output["checkpoint_count"] = ready->checkpoints.size();
    } else if (state == "indexing" && current_job && source) {
      const auto& progress = *current_job->progress; auto bytes = progress.bytes.load();
      double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - progress.started).count();
      double rate = elapsed > 0 ? double(bytes) / elapsed : 0;
      output["bytes_indexed"] = bytes; output["lines_seen"] = progress.lines.load();
      output["progress_percent"] = source->size ? std::min(100.0, 100.0 * double(bytes) / double(source->size)) : 100.0;
      output["elapsed_seconds"] = elapsed; output["bytes_per_second"] = rate;
      if (rate > 0) output["eta_seconds"] = double(source->size - std::min(source->size, bytes)) / rate;
    } else if (state == "failed") output["error"] = error;
    return output;
  }
  void reap() {
    for (auto it = jobs.begin(); it != jobs.end();) {
      if ((*it)->progress->finished.load()) { if ((*it)->thread.joinable()) (*it)->thread.join(); it = jobs.erase(it); }
      else ++it;
    }
  }
  Access access(const std::atomic_bool* stop) {
    auto shared = gate;
    {
      std::unique_lock lock(shared->mutex);
      while (shared->used >= shared->maximum) { cancelled(stop); shared->available.wait_for(lock, std::chrono::milliseconds(50)); }
      cancelled(stop); ++shared->used;
    }
    auto permit = std::shared_ptr<void>(reinterpret_cast<void*>(1), [shared](void*) { std::lock_guard lock(shared->mutex); --shared->used; shared->available.notify_one(); });
    std::lock_guard lock(state_mutex); require(state == "ready" && ready, "File is not ready: " + state);
    return {ready, std::move(permit)};
  }
  void verify(const std::shared_ptr<LineIndex>& snapshot) {
    try { snapshot->source.verify(); }
    catch (const std::exception& exception) {
      std::lock_guard lock(state_mutex);
      if (ready == snapshot) { state = "failed"; error = lossy_utf8(exception.what()).first; }
      throw;
    }
  }
};
TraceSearchEngine::TraceSearchEngine(TraceSearchConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}
TraceSearchEngine::~TraceSearchEngine() = default;
Json TraceSearchEngine::status() const { return impl_->status(); }
Json TraceSearchEngine::open(const Json& request) {
  require(request.is_object() && field(request,"path").is_string() && !str(request,"path").empty(), "A source path string is required");
  require(str(request,"path").find('\0')==std::string::npos,"Source path contains NUL");
  auto force = strict_bool(request, "force_rebuild", false); std::lock_guard operation(impl_->operations); impl_->reap();
  auto requested = path(str(request, "path")); if (!requested.is_absolute()) requested = impl_->config.source_root / requested;
  auto inspected = inspect_source(requested, impl_->config.source_root);
  bool already_indexing = false, reused_ready = false;
  {
    std::lock_guard lock(impl_->state_mutex);
    if (impl_->state == "ready" || impl_->state == "indexing") {
      require(impl_->source && fs::equivalent(impl_->source->file, inspected.file), "Another file is active; call close_file first");
      if (!force && impl_->state == "indexing") already_indexing = true;
      if (!force && impl_->state == "ready" && impl_->source->identity() == inspected.identity()) reused_ready = true;
    }
  }
  if (already_indexing || reused_ready) return {{"index_reused", reused_ready}, {"status", status()}};
  if (impl_->current_job) impl_->current_job->progress->cancel = true;
  auto predecessors = impl_->jobs;
  std::shared_ptr<LineIndex> existing;
  if (!force) existing = load_index(inspected, impl_->config);
  if (!existing) require(impl_->jobs.size() < 4, "Previous index builds are stopping; retry shortly");
  {
    std::lock_guard lock(impl_->state_mutex); ++impl_->generation; impl_->source = inspected; impl_->error.clear(); impl_->ready = existing; impl_->state = existing ? "ready" : "indexing"; impl_->current_job.reset();
  }
  if (existing) return {{"index_reused", true}, {"status", status()}};
  auto job = std::make_shared<Impl::Job>(); job->generation = impl_->generation;
  {
    std::lock_guard lock(impl_->state_mutex); impl_->current_job = job;
  }
  impl_->jobs.push_back(job);
  try {
    job->thread = std::thread([owner = impl_.get(), job, inspected, force, predecessors] {
      try {
        for (const auto& previous : predecessors) while (!previous->progress->finished.load()) { cancelled(&job->progress->cancel); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        auto result = build_index(inspected, owner->config, force, *job->progress);
        std::lock_guard lock(owner->state_mutex);
        if (owner->generation == job->generation && owner->state == "indexing") { owner->ready = std::move(result); owner->state = "ready"; }
      } catch (const std::exception& exception) {
        std::lock_guard lock(owner->state_mutex);
        if (owner->generation == job->generation && owner->state == "indexing") { owner->state = "failed"; owner->error = lossy_utf8(exception.what()).first; }
      }
      job->progress->finished = true;
    });
  } catch (const std::exception& exception) {
    job->progress->finished = true; std::lock_guard lock(impl_->state_mutex); impl_->state = "failed"; impl_->error = exception.what(); throw;
  }
  return {{"index_reused", false}, {"status", status()}};
}
Json TraceSearchEngine::close() {
  std::lock_guard operation(impl_->operations);
  {
    std::lock_guard lock(impl_->state_mutex); ++impl_->generation;
    if (impl_->current_job) impl_->current_job->progress->cancel = true;
    impl_->current_job.reset(); impl_->ready.reset(); impl_->source.reset(); impl_->error.clear(); impl_->state = "no_file";
  }
  impl_->reap(); return status();
}
Json TraceSearchEngine::read(const Json& request, const std::atomic_bool* stop) {
  auto start = number(request, "start_line"), count = number(request, "line_count");
  require(start > 0 && count <= impl_->config.max_read_lines, "Read range exceeds configured line limit or is zero-based");
  auto access = impl_->access(stop); auto index = access.index; impl_->verify(index);
  auto selected = selected_count(*index, start, count); auto begin = index->offset(start, stop), end = index->offset(start + selected, stop);
  require(end >= begin && end - begin <= impl_->config.max_content_bytes, "Read content exceeds byte limit; request fewer lines or export_lines");
  std::ifstream input(index->source.file, std::ios::binary); require(bool(input), "Open source for reading failed"); input.seekg(std::streamoff(begin)); require(bool(input), "Seek source for reading failed");
  std::string bytes(size_t(end - begin), '\0'); size_t position = 0;
  while (position < bytes.size()) {
    cancelled(stop); auto length = std::min<size_t>(1048576, bytes.size() - position); input.read(bytes.data() + position, std::streamsize(length)); require(size_t(input.gcount()) == length, "Source changed or read failed"); position += length;
  }
  cancelled(stop); impl_->verify(index); auto decoded = lossy_utf8(bytes); auto next = next_line(*index, start, selected);
  return {{"start_line",start},{"end_line",selected ? Json(start + selected - 1) : Json(nullptr)},{"returned_lines",selected},{"source_bytes",bytes.size()},{"content",std::move(decoded.first)},{"lossy_utf8",decoded.second},{"next_line",next},{"eof",next.is_null()}};
}
Json TraceSearchEngine::search(const Json& request, const std::atomic_bool* stop) {
  auto access = impl_->access(stop); impl_->verify(access.index);
  auto result = search_index(*access.index, impl_->config, request, stop);
  cancelled(stop); impl_->verify(access.index); return result;
}
Json TraceSearchEngine::export_lines(const Json& request, const std::atomic_bool* stop) {
  auto access = impl_->access(stop); impl_->verify(access.index);
  return export_index(*access.index, impl_->config, request, stop);
}
}
