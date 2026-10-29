#include "v8trace/index.h"
#include <sqlite3.h>
#include <algorithm>
#include <cmath>
#include <climits>
#include <stdexcept>

namespace v8trace {
struct DiskIndex::Impl {
  fs::path owner, directory;
  sqlite3* database = nullptr;
  struct Prepared { sqlite3_stmt* statement = nullptr; bool busy = false; };
  std::map<std::string, Prepared> cache;
  uint64_t queries = 0, rows = 0, maximum_row_bytes = 0, full_scan_steps = 0;
  explicit Impl(const fs::path& base) : owner(io_path(fs::canonical(io_path(base)))), directory(owner / ("analysis-cpp-" + uuid())) {
    if (!inside(owner, directory) || !fs::create_directory(directory)) throw std::runtime_error("Create owned index directory failed");
    try {
      auto name = (directory / "index.sqlite").u8string();
      const char* vfs = nullptr;
#ifdef _WIN32
      vfs = "win32-longpath";
#endif
      if (sqlite3_open_v2(reinterpret_cast<const char*>(name.c_str()), &database, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, vfs) != SQLITE_OK) fail("Open index");
      sqlite3_extended_result_codes(database, 1);
      sqlite3_db_config(database, SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 0, nullptr);
    } catch (...) { close(); cleanup(); throw; }
  }
  [[noreturn]] void fail(std::string_view operation) const { throw std::runtime_error(std::string(operation) + ": " + (database ? sqlite3_errmsg(database) : "SQLite allocation failed")); }
  void close() noexcept {
    for (auto& [key, entry] : cache) sqlite3_finalize(entry.statement);
    cache.clear(); if (database) sqlite3_close_v2(database); database = nullptr;
  }
  void cleanup() noexcept {
    std::error_code error;
    auto resolved = fs::weakly_canonical(directory, error);
    if (!error && inside(owner, resolved) && utf8(resolved.filename()).starts_with("analysis-cpp-")) fs::remove_all(io_path(resolved), error);
  }
  ~Impl() { close(); cleanup(); }
  Prepared& prepare(std::string_view sql) {
    auto key = std::string(sql); auto found = cache.find(key);
    if (found == cache.end()) {
      if (cache.size() >= 64) throw std::runtime_error("Index prepared-statement budget exceeded");
      sqlite3_stmt* statement = nullptr;
      if (sqlite3_prepare_v3(database, key.c_str(), -1, SQLITE_PREPARE_PERSISTENT, &statement, nullptr) != SQLITE_OK) fail("Prepare index query");
      found = cache.emplace(std::move(key), Prepared{statement, false}).first;
    }
    if (found->second.busy) throw std::runtime_error("Recursive use of index cursor");
    return found->second;
  }
};
DiskIndex::DiskIndex(const fs::path& owner) : impl_(std::make_unique<Impl>(owner)) {
  exec("PRAGMA cache_size=-8192; PRAGMA temp_store=FILE; PRAGMA mmap_size=0; PRAGMA journal_mode=DELETE; PRAGMA synchronous=NORMAL; PRAGMA trusted_schema=OFF; BEGIN");
}
DiskIndex::~DiskIndex() = default;
void DiskIndex::exec(std::string_view sql) {
  if (sqlite3_exec(impl_->database, std::string(sql).c_str(), nullptr, nullptr, nullptr) != SQLITE_OK) impl_->fail("Execute index operation");
}
void DiskIndex::each(std::string_view sql, const Json& parameters, const Sink& visit) {
  if (!parameters.is_array()) throw std::runtime_error("Index parameters must be an array");
  auto& prepared = impl_->prepare(sql); auto statement = prepared.statement;
  prepared.busy = true;
  struct Reset { Impl::Prepared& prepared; ~Reset() { sqlite3_reset(prepared.statement); sqlite3_clear_bindings(prepared.statement); prepared.busy = false; } } reset{prepared};
  if (parameters.size() != size_t(sqlite3_bind_parameter_count(statement))) throw std::runtime_error("Index parameter count mismatch");
  int index = 0;
  for (auto& value : parameters) {
    ++index; int status;
    if (value.is_null()) status = sqlite3_bind_null(statement, index);
    else if (value.is_number_unsigned()) {
      if (value.get<uint64_t>() > INT64_MAX) throw std::runtime_error("Index numeric parameter exceeds int64; encode identities as BLOB");
      status = sqlite3_bind_int64(statement, index, int64_t(value.get<uint64_t>()));
    } else if (value.is_number_integer()) status = sqlite3_bind_int64(statement, index, value.get<int64_t>());
    else if (value.is_number_float()) { auto number = value.get<double>(); if (!std::isfinite(number)) throw std::runtime_error("Non-finite index parameter"); status = sqlite3_bind_double(statement, index, number); }
    else {
      auto bytes = value.is_string() ? text(value) : dump(value);
      status = sqlite3_bind_blob64(statement, index, bytes.data(), bytes.size(), SQLITE_TRANSIENT);
    }
    if (status != SQLITE_OK) impl_->fail("Bind index parameter");
  }
  ++impl_->queries;
  for (;;) {
    auto result = sqlite3_step(statement);
    if (result == SQLITE_DONE) break;
    if (result != SQLITE_ROW) impl_->fail("Read/write index");
    Json row = Json::array(); uint64_t bytes = 0;
    for (int column = 0; column < sqlite3_column_count(statement); ++column) {
      auto type = sqlite3_column_type(statement, column);
      if (type == SQLITE_NULL) row.push_back(nullptr);
      else if (type == SQLITE_INTEGER) row.push_back(sqlite3_column_int64(statement, column));
      else if (type == SQLITE_FLOAT) row.push_back(sqlite3_column_double(statement, column));
      else {
        auto data = static_cast<const char*>(sqlite3_column_blob(statement, column)); auto length = sqlite3_column_bytes(statement, column);
        bytes += uint64_t(length); row.push_back(std::string(data ? data : "", size_t(length)));
      }
    }
    ++impl_->rows; impl_->maximum_row_bytes = std::max(impl_->maximum_row_bytes, bytes); if (visit) visit(row);
  }
  impl_->full_scan_steps += uint64_t(sqlite3_stmt_status(statement, SQLITE_STMTSTATUS_FULLSCAN_STEP, 1));
}
void DiskIndex::run(std::string_view sql, const Json& parameters) { each(sql, parameters, {}); }
std::optional<Json> DiskIndex::one(std::string_view sql, const Json& parameters) {
  std::optional<Json> output;
  each(sql, parameters, [&](const Json& row) { if (output) throw std::runtime_error("Expected one index row"); output = row; }); return output;
}
int64_t DiskIndex::last_id() const { return sqlite3_last_insert_rowid(impl_->database); }
int64_t DiskIndex::changes() const { return sqlite3_changes64(impl_->database); }
void DiskIndex::checkpoint() { exec("COMMIT; BEGIN"); }
void DiskIndex::release_queries() {
  for (const auto& [key, entry] : impl_->cache) if (entry.busy) throw std::runtime_error("Release of active index cursor");
  for (auto& [key, entry] : impl_->cache) sqlite3_finalize(entry.statement);
  impl_->cache.clear();
}
Json DiskIndex::stats() const {
  int current = 0, high = 0; sqlite3_db_status(impl_->database, SQLITE_DBSTATUS_CACHE_USED, &current, &high, 0);
  return {{"protocol", "v8trace/disk-index/1"}, {"sqliteVersion", sqlite3_libversion()}, {"cacheBudgetBytes", 8388608}, {"cacheUsedBytes", current}, {"maximumValueBytes", sqlite3_limit(impl_->database, SQLITE_LIMIT_LENGTH, -1)}, {"mmap", false}, {"tempStore", "file"}, {"queries", impl_->queries}, {"rowsRead", impl_->rows}, {"fullScanSteps", impl_->full_scan_steps}, {"maximumRowBytes", impl_->maximum_row_bytes}, {"databaseBytes", fs::file_size(impl_->directory / "index.sqlite")}, {"cleanup", "owned-directory-only"}};
}

JsonArrayOutput::JsonArrayOutput(const fs::path& file, std::string_view prefix) : output_(file, std::ios::binary | std::ios::trunc) {
  if (!output_) throw std::runtime_error("Open streamed JSON output failed: " + utf8(file)); output_.write(prefix.data(), std::streamsize(prefix.size()));
}
JsonArrayOutput::~JsonArrayOutput() = default;
void JsonArrayOutput::raw_item(std::string_view value) {
  begin_item(); fragment(value);
}
void JsonArrayOutput::begin_item() {
  if (finished_) throw std::runtime_error("JSON array already finished");
  if (count_) output_ << ',';
  if (!output_) throw std::runtime_error("Streamed JSON write failed"); ++count_;
}
void JsonArrayOutput::fragment(std::string_view value) {
  if (finished_) throw std::runtime_error("JSON array already finished");
  output_.write(value.data(), std::streamsize(value.size()));
  if (!output_) throw std::runtime_error("Streamed JSON write failed");
}
void JsonArrayOutput::append(const Json& value) { raw_item(dump(value)); }
void JsonArrayOutput::finish(std::string_view suffix) {
  if (finished_) throw std::runtime_error("JSON array already finished"); output_.write(suffix.data(), std::streamsize(suffix.size())); output_.flush();
  if (!output_) throw std::runtime_error("Streamed JSON flush failed"); finished_ = true;
}
}
