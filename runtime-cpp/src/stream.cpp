#include "v8trace/stream.h"
#include "v8trace/index.h"
#include <algorithm>
#include <array>
#include <condition_variable>
#include <iomanip>
#include <mutex>
#include <regex>
#include <sstream>
#include <thread>
#include <zstd.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace v8trace {
namespace {
constexpr uint64_t chunk_size = 1048576;
void require(bool valid, std::string_view error) { if (!valid) throw std::runtime_error(std::string(error)); }
bool digest(const Json& value) { return value.is_string() && std::regex_match(text(value), std::regex("[a-f0-9]{64}")); }
std::string ordinal(uint64_t value) { std::ostringstream out; out << std::setfill('0') << std::setw(20) << value; return out.str(); }
fs::path safe(const fs::path& root, const std::string& relative) {
  auto name = path(relative); require(!relative.empty() && !name.is_absolute() && !name.has_root_name() && relative.find_first_of(":\\") == relative.npos, "Unsafe stream path");
  for (const auto& part : name) { auto value = utf8(part); require(value != "." && value != ".." && !value.empty(), "Unsafe stream path component"); }
  auto target = (root / name).lexically_normal(); require(inside(root, target), "Stream path leaves root"); return target;
}
void durable(const fs::path& file) {
#ifdef _WIN32
  auto handle = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  require(handle != INVALID_HANDLE_VALUE, "Open stream durability handle failed"); auto ok = FlushFileBuffers(handle); CloseHandle(handle); require(ok != 0, "Flush stream file failed");
#else
  auto fd = ::open(file.c_str(), O_WRONLY | O_NOFOLLOW); require(fd >= 0, "Open stream durability handle failed"); auto result = fsync(fd); ::close(fd); require(result == 0, "Flush stream file failed");
#endif
}
void replace(const fs::path& source, const fs::path& destination) {
#ifdef _WIN32
  require(MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0, "Atomic stream replacement failed");
#else
  fs::rename(source, destination); auto fd = ::open(destination.parent_path().c_str(), O_RDONLY | O_DIRECTORY); if (fd >= 0) { auto result = fsync(fd); ::close(fd); require(result == 0, "Flush stream directory failed"); }
#endif
}
void atomic_json(const fs::path& file, const Json& value) { auto temporary = file; temporary += ".new"; write_json(temporary, value); durable(temporary); replace(temporary, file); }
std::string encode(std::string_view bytes) { std::string result(ZSTD_compressBound(bytes.size()), '\0'); auto n = ZSTD_compress(result.data(), result.size(), bytes.data(), bytes.size(), 3); require(!ZSTD_isError(n), "Stream compression failed"); result.resize(n); return result; }
std::string decode(const fs::path& directory, const Json& record) {
  require(str(record, "protocol") == "v8trace/chunk/1" && str(record, "codec") == "zstd-3" && safe_uint(field(record, "rawBytes"), true) && record.at("rawBytes") <= chunk_size && digest(field(record, "rawSha256")) && digest(field(record, "encodedSha256")), "Invalid stream chunk metadata");
  auto file = safe(directory, "payload.zst"); require(fs::is_regular_file(file) && fs::file_size(file) <= ZSTD_compressBound(chunk_size), "Stream encoded chunk size invalid");
  auto encoded = read_file(file); require(sha256(encoded) == str(record, "encodedSha256"), "Stream encoded SHA256 mismatch");
  std::string bytes(record.at("rawBytes").get<size_t>(), '\0'); auto n = ZSTD_decompress(bytes.data(), bytes.size(), encoded.data(), encoded.size()); require(!ZSTD_isError(n) && n == bytes.size() && sha256(bytes) == str(record, "rawSha256"), "Stream decoded length/SHA256 mismatch"); return bytes;
}
Json transport_options(const Json& options) { Json result = options; for (auto [source, target] : std::vector<std::pair<std::string, std::string>>{{"streamHost", "host"}, {"streamPort", "port"}, {"streamTokenFile", "tokenFile"}}) if (options.contains(source)) result[target] = options.at(source); return result; }
void scan(const fs::path& root, const std::function<void(const fs::directory_entry&)>& visit) {
  for (auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator(); ++it) {
    auto name = utf8(it->path().filename());
    if (it->is_directory() && (name == "browser-profile" || name == ".incoming" || name.ends_with(".new") || name.starts_with("analysis-cpp-") || name.starts_with("decode-cpp-") || name.starts_with("state-spool-"))) { it.disable_recursion_pending(); continue; }
    require(!fs::is_symlink(it->symlink_status()), "Stream source symlink rejected"); if (it->is_regular_file()) visit(*it);
  }
}
}
struct ChunkSpool::Impl {
  fs::path root;
  std::string id;
  std::unique_ptr<DiskIndex> index;
  uint64_t sequence = 0, raw_bytes = 0, encoded_bytes = 0, maximum;
  std::atomic_uint64_t sealed_sequence = 0, acknowledged = 0;
  std::atomic_bool sealed = false, done = false, cancel = false;
  bool online;
  Json options;
  Sink progress;
  std::thread sender;
  std::mutex writer_mutex;
  std::map<std::string, std::string> pending;
  uint64_t pending_bytes = 0;
  mutable std::mutex mutex;
  std::condition_variable changed;
  std::string error;
  Impl(const fs::path& output, std::string capture_id, Json config, Sink notify) : root(fs::absolute(output)), id(std::move(capture_id)), maximum(field(config, "streamSpoolMaxBytes", 1099511627776ULL).get<uint64_t>()), online(field(config, "streamTCP", false) == true), options(transport_options(config)), progress(std::move(notify)) {
    require(fs::create_directory(root), "Stream spool must be a new directory"); root = fs::canonical(root);
    fs::create_directories(root / "chunks"); fs::create_directory(root / "acks"); index = std::make_unique<DiskIndex>(root); index->exec("CREATE TABLE files(path BLOB PRIMARY KEY,bytes INTEGER NOT NULL)");
    save("recording");
    if (online) sender = std::thread([this] { run(); });
  }
  ~Impl() { cancel = true; done = true; changed.notify_all(); if (sender.joinable()) sender.join(); if (!sealed) { try { flush(); save("uncommitted"); } catch (...) {} } }
  Json status() const { std::lock_guard lock(mutex); return {{"protocol", "v8trace/chunk-stream/1"}, {"streamId", id}, {"spool", utf8(root)}, {"sealedChunks", sealed_sequence.load()}, {"acknowledgedChunks", acknowledged.load()}, {"localCommitted", sealed.load()}, {"remoteCommitted", sealed && acknowledged == sealed_sequence + 1}, {"tcpEnabled", online}, {"error", error}, {"sourceRetained", true}, {"boundary", "host-journal-as-produced; native-raw-after-browser-flush"}}; }
  void save(const char* phase) { auto value = status(); value["phase"] = phase; value["rawBytes"] = raw_bytes; value["encodedBytes"] = encoded_bytes; value["chunkBytes"] = chunk_size; atomic_json(root / "spool.json", value); }
  void write_parts(std::string_view relative, std::string_view bytes) {
    require(!sealed, "Stream already committed"); auto name = std::string(relative); safe(root, name); auto row = index->one("SELECT bytes FROM files WHERE path=?", Json::array({name})); uint64_t offset = row ? row->at(0).get<uint64_t>() : 0;
    while (!bytes.empty()) {
      auto part = bytes.substr(0, size_t(std::min<uint64_t>(chunk_size, bytes.size()))); auto encoded = encode(part);
      require(encoded.size() <= maximum - std::min(maximum, encoded_bytes), "Stream spool byte budget exceeded; source retained");
      auto seq = sequence + 1; auto temporary = root / "chunks" / (ordinal(seq) + ".new"); auto destination = root / "chunks" / ordinal(seq); require(fs::create_directory(temporary), "Stream chunk directory already exists");
      write_file(temporary / "payload.zst", encoded, true); durable(temporary / "payload.zst");
      Json record{{"protocol", "v8trace/chunk/1"}, {"streamId", id}, {"sequence", seq}, {"path", name}, {"offset", offset}, {"rawBytes", part.size()}, {"rawSha256", sha256(part)}, {"codec", "zstd-3"}, {"encodedBytes", encoded.size()}, {"encodedSha256", sha256(encoded)}};
      write_json(temporary / "chunk.json", record); durable(temporary / "chunk.json"); replace(temporary, destination);
      offset += part.size(); raw_bytes += part.size(); encoded_bytes += encoded.size(); sequence = seq;
      index->run("INSERT INTO files VALUES(?,?) ON CONFLICT(path) DO UPDATE SET bytes=excluded.bytes", Json::array({name, offset}));
      sealed_sequence = seq; save("recording"); changed.notify_all(); bytes.remove_prefix(part.size());
    }
  }
  void flush_pending() {
    for (auto& [name, bytes] : pending) if (!bytes.empty()) write_parts(name, bytes);
    pending.clear(); pending_bytes = 0;
  }
  void flush() { std::lock_guard lock(writer_mutex); flush_pending(); }
  void buffer_append(std::string_view relative, std::string_view bytes) {
    require(!sealed, "Stream already committed"); auto name = std::string(relative); safe(root, name);
    if (pending.size() >= 16 && !pending.contains(name)) flush_pending();
    while (!bytes.empty()) {
      auto& buffer = pending[name]; auto count = size_t(std::min<uint64_t>(chunk_size - buffer.size(), bytes.size())); buffer.append(bytes.substr(0, count)); pending_bytes += count; bytes.remove_prefix(count);
      if (buffer.size() == chunk_size) { write_parts(name, buffer); pending_bytes -= buffer.size(); pending.erase(name); }
      if (pending_bytes >= 8388608) flush_pending();
    }
  }
  void append(std::string_view relative, std::string_view bytes) { std::lock_guard lock(writer_mutex); buffer_append(relative, bytes); }
  void run() {
    uint64_t next = 1;
    while (!cancel) {
      auto available = sealed_sequence.load(); bool commit = sealed && next == available + 1;
      if (next > available && !commit) { if (done) break; { std::unique_lock lock(mutex); changed.wait_for(lock, std::chrono::milliseconds(1000)); } try { flush(); } catch (const std::exception& exception) { std::lock_guard lock(mutex); error = exception.what(); cancel = true; } continue; }
      if (sealed && next > available + 1) break;
      auto directory = commit ? root / "commit" : root / "chunks" / ordinal(next);
      try {
        auto receipt = tcp_send(directory, options, {}, &cancel); atomic_json(root / "acks" / (ordinal(next) + ".json"), receipt); acknowledged = next; ++next;
        { std::lock_guard lock(mutex); error.clear(); }
        if (progress) progress(status());
      } catch (const std::exception& exception) { { std::lock_guard lock(mutex); error = exception.what(); } if (done || cancel) break; std::unique_lock lock(mutex); changed.wait_for(lock, std::chrono::milliseconds(500)); }
    }
  }
  Json seal(const fs::path& directory) {
    std::unique_lock writer_lock(writer_mutex); flush_pending();
    auto capture = fs::canonical(directory); require(!inside(capture, root) && !inside(root, capture), "Stream spool and capture must be separate trees");
    auto commit = root / "commit.new"; require(fs::create_directory(commit), "Stream commit already exists"); std::ofstream manifest(commit / "files.jsonl", std::ios::binary); require(bool(manifest), "Create stream file manifest failed");
    uint64_t files = 0;
    scan(capture, [&](const fs::directory_entry& entry) {
      auto relative = entry.path().lexically_relative(capture);
      auto name = utf8(relative); auto source = safe(capture, name); auto length = fs::file_size(source); auto row = index->one("SELECT bytes FROM files WHERE path=?", Json::array({name}));
      if (row) require(row->at(0) == length, "Append-only journal changed before stream commit");
      else { std::ifstream input(source, std::ios::binary); require(bool(input), "Open stream source failed"); std::string buffer(size_t(chunk_size), '\0'); while (input) { input.read(buffer.data(), buffer.size()); auto n = input.gcount(); if (n) buffer_append(name, std::string_view(buffer.data(), size_t(n))); } require(input.eof(), "Stream source read failed"); }
      manifest << dump(Json{{"path", name}, {"bytes", length}, {"sha256", file_sha256(source)}}) << '\n'; require(bool(manifest), "Write stream file manifest failed"); ++files;
    });
    flush_pending(); manifest.flush(); require(bool(manifest), "Flush stream file manifest failed"); manifest.close(); durable(commit / "files.jsonl");
    Json value{{"protocol", "v8trace/chunk-commit/1"}, {"streamId", id}, {"chunkCount", sequence}, {"files", files}, {"filesSha256", file_sha256(commit / "files.jsonl")}, {"profileIncluded", false}, {"localCommit", true}, {"completeCaptureClaimed", false}};
    write_json(commit / "stream-commit.json", value); durable(commit / "stream-commit.json"); replace(commit, root / "commit"); sealed = true; save("sealed"); writer_lock.unlock(); changed.notify_all();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(field(options, "streamDrainTimeout", 5000).get<uint64_t>());
    while (online && acknowledged < sequence + 1 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    done = true; cancel = true; changed.notify_all(); if (sender.joinable()) sender.join(); save("sealed"); return status();
  }
};
ChunkSpool::ChunkSpool(const fs::path& directory, const std::string& capture_id, const Json& options, Sink progress) : impl_(std::make_unique<Impl>(directory, capture_id, options, std::move(progress))) {}
ChunkSpool::~ChunkSpool() = default;
void ChunkSpool::append(std::string_view relative, std::string_view bytes) { impl_->append(relative, bytes); }
void ChunkSpool::flush() { impl_->flush(); }
Json ChunkSpool::seal(const fs::path& capture) { return impl_->seal(capture); }
Json ChunkSpool::status() const { return impl_->status(); }
Json stream_pack(const fs::path& capture, const fs::path& spool) { auto root = fs::canonical(capture); require(!inside(root, fs::absolute(spool)), "Archive must stay outside capture"); ChunkSpool value(spool, uuid()); return value.seal(root); }
Json stream_decode(const fs::path& chunk, const fs::path& output) { auto directory = fs::canonical(chunk); auto record = load_json(directory / "chunk.json", 65536); auto bytes = decode(directory, record); write_file(output, bytes, true); return {{"protocol", "v8trace/chunk/1"}, {"path", record.at("path")}, {"offset", record.at("offset")}, {"bytes", bytes.size()}, {"sha256", sha256(bytes)}, {"output", utf8(fs::absolute(output))}}; }
Json stream_send(const fs::path& spool, const Json& options, const Sink& progress, const std::atomic_bool* stop) {
  auto root = fs::canonical(spool); auto state = load_json(root / "spool.json", 65536); require(str(state, "protocol") == "v8trace/chunk-stream/1", "Invalid stream spool"); auto count = field(state, "sealedChunks"); require(safe_uint(count), "Invalid stream chunk count"); auto transport = transport_options(options); uint64_t acknowledged = 0;
  for (uint64_t i = 1; i <= count.get<uint64_t>(); ++i) { auto directory = safe(root, "chunks/" + ordinal(i)); auto record = load_json(directory / "chunk.json", 65536); require(record.at("sequence") == i && str(record, "streamId") == str(state, "streamId"), "Stream chunk identity differs"); decode(directory, record); auto receipt = tcp_send(directory, transport, progress, stop); atomic_json(root / "acks" / (ordinal(i) + ".json"), receipt); acknowledged = i; }
  bool commit = field(state, "localCommitted") == true;
  if (commit) { auto receipt = tcp_send(root / "commit", transport, progress, stop); atomic_json(root / "acks" / (ordinal(acknowledged + 1) + ".json"), receipt); }
  Json result{{"protocol", "v8trace/chunk-stream/1"}, {"streamId", state.at("streamId")}, {"acknowledgedChunks", acknowledged}, {"remoteCommitted", commit}, {"sourceRetained", true}}; atomic_json(root / "transfer.json", result); return result;
}
Json stream_unpack(const fs::path& spool, const fs::path& output, const std::string& selected_id) {
  auto root = fs::canonical(spool); require(!fs::exists(output), "Stream output must be new"); auto parent = fs::canonical(fs::absolute(output).parent_path()); require(!inside(root, parent), "Stream output must stay outside spool");
  auto temporary = parent / ("stream-restore-" + uuid()); require(fs::create_directory(temporary), "Create stream reconstruction failed");
  struct Cleanup { fs::path root; ~Cleanup() { std::error_code error; fs::remove_all(root, error); } } cleanup{temporary};
  DiskIndex index(temporary); index.exec("CREATE TABLE chunks(seq INTEGER PRIMARY KEY,path BLOB NOT NULL,offset INTEGER NOT NULL,directory BLOB NOT NULL,record BLOB NOT NULL,used INTEGER NOT NULL DEFAULT 0); CREATE UNIQUE INDEX positions ON chunks(path,offset)");
  Json commit; fs::path manifest; std::string id = selected_id;
  scan(root, [&](const fs::directory_entry& entry) {
    auto name = entry.path().filename(); if (name != "stream-commit.json") return;
    auto value = load_json(entry.path(), 65536); if (!selected_id.empty() && str(value, "streamId") != selected_id) return;
    require(commit.is_null(), "Multiple stream commits; select exactly one stream ID"); require(str(value, "protocol") == "v8trace/chunk-commit/1" && digest(field(value, "filesSha256")) && safe_uint(field(value, "chunkCount")) && safe_uint(field(value, "files")), "Invalid stream commit"); commit = value; manifest = entry.path().parent_path() / "files.jsonl"; id = str(value, "streamId");
  });
  require(!commit.is_null(), "Stream has no final commit; partial spool retained"); require(file_sha256(manifest) == str(commit, "filesSha256"), "Stream manifest SHA256 mismatch");
  uint64_t chunks = 0;
  scan(root, [&](const fs::directory_entry& entry) {
    if (entry.path().filename() != "chunk.json") return;
    auto record = load_json(entry.path(), 65536); if (str(record, "streamId") != id) return;
    require(safe_uint(field(record, "sequence"), true) && safe_uint(field(record, "offset")) && record.at("sequence") <= commit.at("chunkCount"), "Invalid stream sequence or offset"); safe(temporary, str(record, "path"));
    index.run("INSERT INTO chunks(seq,path,offset,directory,record) VALUES(?,?,?,?,?)", Json::array({record.at("sequence"), record.at("path"), record.at("offset"), utf8(entry.path().parent_path()), dump(record)})); ++chunks;
  });
  require(chunks == commit.at("chunkCount"), "Stream has missing chunks"); auto data = temporary / "data"; fs::create_directory(data); uint64_t files = 0;
  lines(manifest, [&](const Json& file) {
    require(safe_uint(field(file, "bytes")) && digest(field(file, "sha256")), "Invalid stream file metadata"); auto destination = safe(data, str(file, "path")); require(!fs::exists(destination), "Duplicate stream destination"); fs::create_directories(destination.parent_path()); std::ofstream target(destination, std::ios::binary); require(bool(target), "Open stream destination failed"); uint64_t offset = 0;
    index.each("SELECT offset,directory,record FROM chunks WHERE path=? ORDER BY offset", Json::array({file.at("path")}), [&](const Json& row) { require(row.at(0) == offset, "Stream file has a chunk hole"); auto record = parse(text(row.at(2))); auto bytes = decode(path(text(row.at(1))), record); require(bytes.size() <= file.at("bytes").get<uint64_t>() - std::min(offset, file.at("bytes").get<uint64_t>()), "Stream file exceeds final size"); target.write(bytes.data(), std::streamsize(bytes.size())); require(bool(target), "Write stream reconstruction failed"); offset += bytes.size(); });
    target.flush(); require(bool(target), "Flush stream reconstruction failed"); target.close(); require(offset == file.at("bytes") && file_sha256(destination) == str(file, "sha256"), "Stream reconstructed file SHA256/length mismatch"); index.run("UPDATE chunks SET used=1 WHERE path=?", Json::array({file.at("path")})); ++files;
  });
  require(files == commit.at("files") && index.one("SELECT COUNT(*) FROM chunks WHERE used=0")->at(0) == 0, "Stream final file/chunk count mismatch"); fs::rename(data, output); return {{"protocol", "v8trace/chunk-stream/1"}, {"streamId", id}, {"conclusion", "reconstructed-and-sha256-verified"}, {"files", files}, {"chunks", chunks}, {"output", utf8(fs::absolute(output))}, {"sourceRetained", true}};
}
}
