#pragma once
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace v8trace {
using Json = nlohmann::ordered_json;
namespace fs = std::filesystem;
using Sink = std::function<void(const Json&)>;
class RecordingControl;
std::string text(const Json& value);
std::string str(const Json& value, std::string_view key, std::string fallback = "");
Json field(const Json& value, std::string_view key, Json fallback = nullptr);
std::string now();
std::string uuid();
fs::path path(std::string_view utf8);
std::string utf8(const fs::path& value);
fs::path io_path(const fs::path& value);
fs::path logical_path(const fs::path& value);
bool inside(const fs::path& root, const fs::path& file);
std::string read_file(const fs::path& file, uint64_t maximum = UINT64_MAX);
void write_file(const fs::path& file, std::string_view data, bool exclusive = false);
void write_json(const fs::path& file, const Json& value);
std::string sha256(std::string_view bytes);
std::string file_sha256(const fs::path& file);
std::string reader_sha256(const std::function<size_t(char*, size_t)>& reader);
std::string base64(std::string_view bytes);
std::string unbase64(std::string_view encoded);
std::string utf16le(std::string_view wtf8);
std::string replacement_utf8(std::string_view wtf8);
std::vector<std::string> split(std::string_view value, char separator);
std::string trim(std::string value);
std::string lower(std::string value);
bool safe_uint(const Json& value, bool positive = false);
uint64_t decimal(std::string_view value);

class JsonReader {
 public:
  explicit JsonReader(std::istream& input);
  Json value();
  void white();
  void expect(char value);
  int peek();
  bool ended();
 private:
  std::istream& input_;
  size_t depth_ = 0;
  std::string string();
};
Json parse(std::string_view data);
std::string dump(const Json& value, int indent = -1);
Json load_json(const fs::path& file, uint64_t maximum = UINT64_MAX);
void lines(const fs::path& file, const Sink& visit);

class Process {
 public:
  Process();
  ~Process();
  Process(const Process&) = delete;
  Process& operator=(const Process&) = delete;
  void start(const fs::path& executable, const std::vector<std::string>& arguments,
             const fs::path& directory, const fs::path& stdout_file, const fs::path& stderr_file);
  std::optional<Json> poll();
  void close();
  void kill();
  uint64_t id() const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
fs::path executable_path();
std::vector<std::string> platform_arguments(int argc, char** argv);
std::string platform_name();
void prepare_permissions(const fs::path& executable);

class ChunkSpool;
class Store {
 public:
  Store(const fs::path& output, const Json& options, Sink observer = {});
  ~Store();
  fs::path filename(std::string_view relative) const;
  bool append(std::string_view relative, const Json& value);
  bool event(std::string_view category, std::string_view kind, const Json& payload, Json context = Json::object());
  Json blob(std::string_view bytes, std::string_view representation);
  Json blob_file(const fs::path& file, std::string_view representation);
  void json(std::string_view relative, const Json& value);
  void gap(std::string_view reason, Json details = Json::object(), bool critical = true);
  Json progress(std::string_view phase) const;
  Json finalize(Json record);
  void flush();
  fs::path root;
  std::string capture_id;
  uint64_t bytes = 0, sequence = 0, critical_gaps = 0, omitted_gaps = 0;
  Json counts = Json::object(), gaps = Json::array();
  bool accepting = true;
 private:
  uint64_t maximum_bytes_, maximum_events_;
  uint64_t gap_records_ = 0, diagnostic_bytes_ = 0;
  Json diagnostic_options_;
  std::set<std::string> categories_, blobs_;
  struct Journal;
  std::unique_ptr<ChunkSpool> stream_;
  std::unique_ptr<Journal> journal_;
  Sink observer_;
  bool reserve(uint64_t length);
};

Json validate_options(Json input, const fs::path& assets, bool verify_browser = true);
Json verify_backend(const fs::path& executable);
Json normalize_filter(const Json& options);
Json native_kinds();
Json capabilities(const Json& options);
Json presets(const fs::path& assets);
Json render_prompt(const fs::path& assets, const Json& command);
Json launch_config(const Json& options, Store& store);

class Trace {
 public:
  explicit Trace(const fs::path& file, std::string array_key = "traceEvents");
  ~Trace();
  Trace(const Trace&) = delete;
  Trace& operator=(const Trace&) = delete;
  void each(std::string_view group, const Sink& visit) const;
  Json properties = Json::object();
  Json sort_statistics = Json::object();
  bool has_array = false;
  uint64_t event_count = 0;
 private:
  fs::path directory_;
};
Json diagnostics(const Json& properties);
void import_browser_stdio(Store& store);
Json discover_vmp(const fs::path& trace, const fs::path& output, const Json& options = Json::object());
void import_causality(Trace& trace, Store& store, const Json& options);
void import_trace(Trace& trace, Store& store, const Json& options);
void import_browser(Trace& trace, Store& store, const Json& options);
void import_detail(Trace& trace, Store& store, const Json& options);
bool env_selected(const Json& options);
Json normalize_env_filter(const Json& options);
void import_environment(Trace& trace, Store& store, const Json& options);
Json validate_closure_record(const Json& record, const Json& options);
void correlate(Store& store);
Json inspect_capture(const fs::path& directory);
Json export_case(const fs::path& directory, const fs::path& output);
Json capture(Json options, const fs::path& assets, std::atomic_bool& stop, Sink progress = {}, Sink events = {}, RecordingControl* control = nullptr);
void capture_cdp(const Json& options, Store& store, Process& process, std::atomic_bool& stop, const Sink& progress, Json& result, RecordingControl* control = nullptr);
Json cdp_worker_request_handoff(const Json& worker, const Json& response, const Json& request, uint64_t window, bool controlled);
bool cdp_owned_source_pause(const Json& event, const Json& session);
Json tcp_send(const fs::path& directory, const Json& options, const Sink& progress = {}, const std::atomic_bool* stop = nullptr);
int tcp_receive(const Json& options, std::atomic_bool& stop, const Sink& progress = {});
int worker(const fs::path& assets);
}
