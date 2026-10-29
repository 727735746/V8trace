#include "trace_search_internal.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <zlib.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace v8trace::trace_search {
void require(bool value, std::string_view message) { if (!value) throw std::runtime_error(std::string(message)); }
void cancelled(const std::atomic_bool* stop) { if (stop && stop->load()) throw std::runtime_error("query cancelled"); }
uint64_t number(const Json& value, std::string_view key, std::optional<uint64_t> fallback) {
  auto item = field(value, key);
  if (item.is_null() && fallback) return *fallback;
  require(item.is_number_unsigned() || (item.is_number_integer() && item.get<int64_t>() >= 0), "Expected nonnegative integer: " + std::string(key));
  return item.get<uint64_t>();
}
bool boolean(const Json& value, std::string_view key, bool fallback) {
  auto item = field(value, key); if (item.is_null()) return fallback;
  require(item.is_boolean(), "Expected boolean: " + std::string(key)); return item.get<bool>();
}
std::pair<std::string, bool> lossy_utf8(std::string_view bytes) {
  bool exceeded = false; return lossy_utf8_limited(bytes, std::numeric_limits<uint64_t>::max(), exceeded);
}
std::pair<std::string, bool> lossy_utf8_limited(std::string_view bytes, uint64_t limit, bool& exceeded) {
  std::string out; out.reserve(size_t(std::min<uint64_t>(bytes.size(), limit))); bool lossy = false; exceeded = false;
  for (size_t i = 0; i < bytes.size();) {
    const auto c = uint8_t(bytes[i]); size_t length = c < 128 ? 1 : c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
    size_t valid = 1; bool good = length != 0;
    for (size_t n = 1; good && n < length; ++n) {
      if (i + n >= bytes.size()) { good = false; break; }
      auto next = uint8_t(bytes[i + n]);
      bool continuation = next >= 0x80 && next <= 0xbf;
      if (n == 1) continuation = continuation && !(c == 0xe0 && next < 0xa0) && !(c == 0xed && next >= 0xa0) && !(c == 0xf0 && next < 0x90) && !(c == 0xf4 && next >= 0x90);
      if (!continuation) { good = false; break; }
      ++valid;
    }
    auto added = good ? length : 3;
    if (added > limit - out.size()) { exceeded = true; return {{}, lossy}; }
    if (good) { out.append(bytes.substr(i, length)); i += length; }
    else { out.append("\xef\xbf\xbd"); lossy = true; i += valid; }
  }
  return {std::move(out), lossy};
}
namespace {
constexpr std::string_view magic = "V8TSIDX2";
constexpr uint64_t header_bytes = 8, record_bytes = 20;
void le(std::string& output, uint64_t value, unsigned bytes) { for (unsigned i = 0; i < bytes; ++i) output += char(value >> (i * 8)); }
uint64_t un_le(std::string_view bytes) { uint64_t value = 0; for (size_t i = 0; i < bytes.size(); ++i) value |= uint64_t(uint8_t(bytes[i])) << (i * 8); return value; }
uint32_t checksum(std::string_view value) { return uint32_t(crc32(0, reinterpret_cast<const Bytef*>(value.data()), uInt(value.size()))); }
std::string encoded(const Checkpoint& item) {
  std::string bytes; le(bytes, item.line, 8); le(bytes, item.offset, 8); le(bytes, checksum(bytes), 4); return bytes;
}
std::string file_identity(const fs::path& file) {
#ifdef _WIN32
  HANDLE handle = CreateFileW(file.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  require(handle != INVALID_HANDLE_VALUE, "Open source identity failed");
  BY_HANDLE_FILE_INFORMATION info{}; bool ok = GetFileInformationByHandle(handle, &info); CloseHandle(handle); require(ok, "Read source identity failed");
  return std::to_string(info.dwVolumeSerialNumber) + ":" + std::to_string(info.nFileIndexHigh) + ":" + std::to_string(info.nFileIndexLow);
#else
  struct stat info{}; require(::stat(file.c_str(), &info) == 0, "Read source identity failed");
  return std::to_string(info.st_dev) + ":" + std::to_string(info.st_ino);
#endif
}
std::string sample(const fs::path& file, uint64_t size) {
  std::ifstream input(file, std::ios::binary); require(bool(input), "Open source samples failed");
  std::string samples; le(samples, size, 8);
  auto take = [&](uint64_t offset, size_t count) {
    input.clear(); input.seekg(std::streamoff(offset)); require(bool(input), "Seek source samples failed");
    std::string bytes(count, '\0'); input.read(bytes.data(), std::streamsize(count)); require(size_t(input.gcount()) == count, "Source changed during sampling");
    le(samples, offset, 8); le(samples, count, 8); samples += bytes;
  };
  take(0, size_t(std::min<uint64_t>(size, 65536)));
  if (size > 65536) take(size - 65536, 65536);
  return sha256(samples);
}
struct Paths { fs::path directory, metadata, complete, partial, lock; };
Paths paths(const Source& source, const TraceSearchConfig& config) {
  auto key = sha256(dump(source.identity()) + "/" + std::to_string(config.checkpoint_bytes));
  auto directory = io_path(config.index_dir) / key; fs::create_directories(directory);
  require(inside(config.index_dir, fs::canonical(directory)), "Index directory leaves configured root");
  Paths result{directory, directory / "snapshot.json", directory / "lines.v8idx", directory / "lines.partial", directory / "build.lock"};
  for (const auto& item : {result.metadata, result.complete, result.partial, result.lock}) {
    require(!fs::is_symlink(fs::symlink_status(item)), "Index cache entries must not be symbolic links");
    require(!fs::exists(item) || (fs::is_regular_file(item) && fs::hard_link_count(item) == 1), "Index cache entries must be private regular files");
    require(!fs::exists(item) || !fs::equivalent(item, source.file), "Index path overlaps source");
  }
  return result;
}
class IndexLock {
 public:
  explicit IndexLock(const fs::path& file) {
#ifdef _WIN32
    handle_ = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    require(handle_ != INVALID_HANDLE_VALUE, "Index build is busy");
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle_, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) { CloseHandle(handle_); handle_ = INVALID_HANDLE_VALUE; throw std::runtime_error("Invalid index lock path"); }
#else
    fd_ = ::open(file.c_str(), O_CREAT | O_RDWR | O_NOFOLLOW, 0600); require(fd_ >= 0, "Open index lock failed");
    if (::flock(fd_, LOCK_EX | LOCK_NB)) { ::close(fd_); fd_ = -1; throw std::runtime_error("Index build is busy"); }
#endif
  }
  ~IndexLock() {
#ifdef _WIN32
    if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
#else
    if (fd_ >= 0) { ::flock(fd_, LOCK_UN); ::close(fd_); }
#endif
  }
 private:
#ifdef _WIN32
  HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
  int fd_ = -1;
#endif
};
std::optional<Json> metadata(const Paths& files, const Source& source, uint64_t interval) {
  try {
    auto envelope = load_json(files.metadata, 65536); auto payload = envelope.at("payload");
    if (str(envelope, "sha256") != sha256(dump(payload)) || field(payload, "identity") != source.identity() || field(payload, "checkpointBytes") != interval || str(payload, "format") != magic) return {};
    return payload;
  } catch (const std::exception&) { return {}; }
}
void save_metadata(const Paths& files, const Source& source, uint64_t interval, const std::string& state, uint64_t count, uint64_t lines) {
  Json payload{{"format", magic}, {"identity", source.identity()}, {"checkpointBytes", interval}, {"state", state}, {"checkpointCount", count}, {"lineCount", lines}};
  auto temporary = files.directory / ("snapshot-" + uuid() + ".tmp");
  try { write_json(temporary, {{"payload", payload}, {"sha256", sha256(dump(payload))}}); sync_file(temporary); publish(temporary, files.metadata, true); }
  catch (...) { std::error_code error; fs::remove(temporary, error); throw; }
}
std::vector<Checkpoint> records(const fs::path& file, const Source& source, bool partial) {
  std::ifstream input(file, std::ios::binary); if (!input) return {};
  std::array<char, 8> header{}; input.read(header.data(), header.size());
  if (input.gcount() != std::streamsize(header.size()) || std::string_view(header.data(), header.size()) != magic) return {};
  std::vector<Checkpoint> output; std::array<char, 20> buffer{};
  while (input.read(buffer.data(), buffer.size()) || input.gcount()) {
    if (input.gcount() != std::streamsize(buffer.size())) { if (partial) break; return {}; }
    std::string_view bytes(buffer.data(), buffer.size()); Checkpoint item{un_le(bytes.substr(0, 8)), un_le(bytes.substr(8, 8))};
    if (checksum(bytes.substr(0, 16)) != un_le(bytes.substr(16, 4)) || !item.line || item.offset >= source.size ||
        (output.empty() && (item.line != 1 || item.offset != 0)) ||
        (!output.empty() && (item.line <= output.back().line || item.offset <= output.back().offset))) {
      if (partial) break; return {};
    }
    output.push_back(item);
  }
  require(!input.bad(), "Read index records failed"); return output;
}
}
void sync_file(const fs::path& file) {
#ifdef _WIN32
  HANDLE handle = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  require(handle != INVALID_HANDLE_VALUE, "Open file for synchronization failed");
  bool ok = FlushFileBuffers(handle); CloseHandle(handle); require(ok, "File synchronization failed");
#else
  int fd = ::open(file.c_str(), O_WRONLY); require(fd >= 0, "Open file for synchronization failed");
  int result = ::fsync(fd); ::close(fd); require(result == 0, "File synchronization failed");
#endif
}
void publish(const fs::path& temporary, const fs::path& destination, bool overwrite) {
#ifdef _WIN32
  require(MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH | (overwrite ? MOVEFILE_REPLACE_EXISTING : 0)) != 0, "Atomic file publication failed or destination exists");
#else
  if (overwrite) require(::rename(temporary.c_str(), destination.c_str()) == 0, "Atomic replacement failed");
  else { require(::link(temporary.c_str(), destination.c_str()) == 0, "Atomic publication failed or destination exists"); require(::unlink(temporary.c_str()) == 0, "Remove published temporary link failed"); }
#endif
}
Json Source::identity() const { return {{"size", size}, {"modified", modified}, {"fileId", file_id}, {"pathHash", path_hash}, {"sampleHash", sample_hash}}; }
void Source::verify(bool samples) const {
  require(fs::is_regular_file(file) && fs::file_size(file) == size && fs::last_write_time(file).time_since_epoch().count() == modified && file_identity(file) == file_id, "Source file changed; reopen an immutable snapshot");
  if (samples) require(sample(file, size) == sample_hash, "Source file samples changed; rebuild index");
}
Source inspect_source(const fs::path& file, const fs::path& allowed_root) {
  auto resolved = fs::canonical(file); require(inside(allowed_root, resolved) && fs::is_regular_file(resolved), "Source file is outside the configured root or is not regular");
  auto name = utf8(resolved);
#ifdef _WIN32
  name = lower(name);
#endif
  Source result{resolved, fs::file_size(resolved), fs::last_write_time(resolved).time_since_epoch().count(), file_identity(resolved), sha256(name), {}};
  require(result.size <= uint64_t(std::numeric_limits<int64_t>::max()), "Source exceeds stream offset range");
  result.sample_hash = sample(resolved, result.size); result.verify(); return result;
}
std::shared_ptr<LineIndex> load_index(const Source& source, const TraceSearchConfig& config) {
  auto files = paths(source, config); auto meta = metadata(files, source, config.checkpoint_bytes);
  if (!meta || str(*meta, "state") != "complete" || !fs::is_regular_file(files.complete)) return {};
  { std::ifstream input(files.complete, std::ios::binary); std::array<char,8> header{}; input.read(header.data(), header.size()); if (input.gcount() != 8 || std::string_view(header.data(),header.size()) != magic) return {}; }
  auto entries = records(files.complete, source, false); auto count = field(*meta, "checkpointCount");
  auto lines = field(*meta, "lineCount"); if (!count.is_number_unsigned() || !lines.is_number_unsigned()) return {};
  if (lines.get<uint64_t>() > source.size) return {};
  if (count.get<uint64_t>() != entries.size() || fs::file_size(files.complete) != header_bytes + record_bytes * entries.size() ||
      (source.size && (entries.empty() || !lines.get<uint64_t>())) || (!source.size && (lines.get<uint64_t>() || !entries.empty())) ||
      (!entries.empty() && entries.back().line > lines.get<uint64_t>())) return {};
  source.verify(true);
  auto output = std::make_shared<LineIndex>(); output->source = source; output->index_file = files.complete; output->interval = config.checkpoint_bytes; output->line_count = lines.get<uint64_t>(); output->checkpoints = std::move(entries); return output;
}
std::shared_ptr<LineIndex> build_index(const Source& source, const TraceSearchConfig& config, bool force, IndexProgress& progress) {
  auto files = paths(source, config); IndexLock lock(files.lock);
  if (!force) if (auto existing = load_index(source, config)) return existing;
  auto meta = metadata(files, source, config.checkpoint_bytes); std::vector<Checkpoint> entries;
  if (!force && meta && str(*meta, "state") == "building") entries = records(files.partial, source, true);
  if (entries.empty()) {
    std::ofstream initial(files.partial, std::ios::binary | std::ios::trunc); require(bool(initial), "Create partial index failed"); initial.write(magic.data(), std::streamsize(magic.size()));
    if (source.size) { entries.push_back({1, 0}); auto bytes = encoded(entries.back()); initial.write(bytes.data(), std::streamsize(bytes.size())); }
    initial.flush(); require(bool(initial), "Write partial index header failed");
  } else fs::resize_file(files.partial, header_bytes + record_bytes * entries.size());
  save_metadata(files, source, config.checkpoint_bytes, "building", entries.size(), 0);
  std::ofstream output(files.partial, std::ios::binary | std::ios::app); require(bool(output), "Open partial index failed");
  std::ifstream input(source.file, std::ios::binary); require(bool(input), "Open source for indexing failed");
  uint64_t offset = entries.empty() ? 0 : entries.back().offset, completed = entries.empty() ? 0 : entries.back().line - 1;
  input.seekg(std::streamoff(offset)); require(bool(input), "Seek resumable source failed");
  std::vector<char> buffer(32 * 1024 * 1024); bool last_lf = true;
  progress.bytes = offset; progress.lines = completed; progress.resume_offset = offset;
  while (offset < source.size) {
    if (progress.cancel) { output.flush(); require(bool(output), "Flush cancelled index failed"); throw std::runtime_error("indexing cancelled; valid checkpoints retained"); }
    auto length = size_t(std::min<uint64_t>(buffer.size(), source.size - offset)); input.read(buffer.data(), std::streamsize(length));
    require(size_t(input.gcount()) == length, "Source changed or indexing read failed");
    const char* at = buffer.data(); const char* end = at + length;
    while (at != end) {
      auto found = static_cast<const char*>(std::memchr(at, '\n', size_t(end - at))); if (!found) break;
      ++completed; auto next = offset + uint64_t(found - buffer.data()) + 1;
      if (next < source.size && next - entries.back().offset >= config.checkpoint_bytes) {
        entries.push_back({completed + 1, next}); auto bytes = encoded(entries.back()); output.write(bytes.data(), std::streamsize(bytes.size()));
        output.flush(); require(bool(output), "Flush index checkpoint failed");
      }
      at = found + 1;
    }
    last_lf = buffer[length - 1] == '\n'; offset += length; progress.bytes = offset; progress.lines = completed;
  }
  auto total = completed + (source.size && !last_lf ? 1 : 0);
  output.flush(); require(bool(output), "Flush completed index failed"); output.close(); source.verify(true);
  if (progress.cancel) throw std::runtime_error("indexing cancelled; valid checkpoints retained");
  sync_file(files.partial); publish(files.partial, files.complete, true); save_metadata(files, source, config.checkpoint_bytes, "complete", entries.size(), total);
  auto result = std::make_shared<LineIndex>(); result->source = source; result->index_file = files.complete; result->line_count = total; result->interval = config.checkpoint_bytes; result->checkpoints = std::move(entries); progress.lines = total; return result;
}
uint64_t LineIndex::offset(uint64_t line, const std::atomic_bool* stop) const {
  require(line > 0 && line - 1 <= line_count, "Line is outside the file; EOF is line_count + 1");
  cancelled(stop); if (line - 1 == line_count) return source.size;
  auto it = std::upper_bound(checkpoints.begin(), checkpoints.end(), line, [](uint64_t value, const Checkpoint& entry) { return value < entry.line; });
  require(it != checkpoints.begin(), "Missing line checkpoint"); --it; if (it->line == line) return it->offset;
  std::ifstream input(source.file, std::ios::binary); require(bool(input), "Open indexed source failed"); input.seekg(std::streamoff(it->offset)); require(bool(input), "Seek indexed source failed");
  std::vector<char> buffer(1024 * 1024); uint64_t position = it->offset, current = it->line;
  while (position < source.size) {
    cancelled(stop); auto length = size_t(std::min<uint64_t>(buffer.size(), source.size - position)); input.read(buffer.data(), std::streamsize(length)); require(size_t(input.gcount()) == length, "Indexed source changed or read failed");
    auto at = buffer.data(); const auto end = at + length;
    while (at != end) { auto found = static_cast<char*>(std::memchr(at, '\n', size_t(end - at))); if (!found) break; if (++current == line) return position + uint64_t(found - buffer.data()) + 1; at = found + 1; }
    position += length;
  }
  throw std::runtime_error("Indexed line boundary not found");
}
}
