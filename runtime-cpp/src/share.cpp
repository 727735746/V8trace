#include "v8trace/share.h"
#include <algorithm>
#include <array>
#include <charconv>
#ifdef _WIN32
#include <windows.h>
#endif

namespace v8trace {
namespace {
constexpr uint64_t record_budget = 1048576, file_budget = 20000, path_budget = 4194304;
void require_share(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void check_cancel(const std::atomic_bool* cancel) { if (cancel && cancel->load()) throw std::runtime_error("Sharing copy cancelled; originals retained"); }
std::string folded(std::string value) { for (char& c : value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A'; return value; }
std::string pointer_key(std::string value) { std::string out; for (char c : value) out += c == '~' ? "~0" : c == '/' ? "~1" : std::string(1, c); return out; }
Json coverage() { return {{"json", "exact member-name rules recursively, including objects in arrays; ASCII case folding"}, {"recordByteLimit", record_budget}, {"depthLimit", 128},
    {"serialization", "JSON is normalized to compact UTF-8 with LF; integers must fit int64/uint64, floating point uses runtime double semantics; original formatting is not preserved"}, {"summaryAnnotations", "root summary.json/export.summary.json are tagged derived-sharing-copy / derived-not-assessed after field rules"},
    {"unsupported", Json::array({"embedded JSON/URLs/credentials inside strings", "name/value header-pair interpretation", "encoded/encrypted/compressed data", "binary/media/source bodies", "semantic VM transformations", "file names and provenance metadata"})}, {"pathSamples", "at most 100, each at most 4096 bytes; oversized paths replaced by explicit display-boundary marker, counts retained"},
    {"references", "original reference fields are not followed, repaired or rebound; omitted blobs remain unavailable"}, {"notOriginalCapture", true}, {"notReplayValidated", true}, {"securePathBackend", "local Windows drive; no reparse components"}}; }
void numeric_coverage(std::string_view data) {
    bool quoted = false, escaped = false;
    for (size_t at = 0; at < data.size(); ++at) {
        char c = data[at];
        if (quoted) { if (escaped) escaped = false; else if (c == '\\') escaped = true; else if (c == '"') quoted = false; continue; }
        if (c == '"') { quoted = true; continue; }
        if (c != '-' && (c < '0' || c > '9')) continue;
        auto start = at; while (at < data.size() && std::string_view("-+0123456789.eE").find(data[at]) != std::string_view::npos) ++at;
        auto number = data.substr(start, at - start);
        if (number.find_first_of(".eE") == number.npos) {
            std::errc error;
            if (number.starts_with('-')) { int64_t value; error = std::from_chars(number.data(), number.data() + number.size(), value).ec; }
            else { uint64_t value; error = std::from_chars(number.data(), number.data() + number.size(), value).ec; }
            require_share(error != std::errc::result_out_of_range, "JSON integer exceeds exact sharing numeric coverage");
        }
        --at;
    }
}
void transform(Json& value, const Json& settings, Json& counts, Json& examples, const std::string& location, unsigned depth, const std::atomic_bool* cancel) {
    require_share(depth <= 128, "JSON nesting exceeds sharing field coverage"); check_cancel(cancel);
    if (value.is_array()) { size_t at = 0; for (auto& item : value) transform(item, settings, counts, examples, location + "/" + std::to_string(at++), depth + 1, cancel); }
    else if (value.is_object()) for (auto it = value.begin(); it != value.end();) {
        auto key = folded(it.key()); const Json* rule = nullptr;
        for (const auto& candidate : settings.at("rules")) if (candidate.at("field") == key) { rule = &candidate; break; }
        auto where = location + "/" + pointer_key(it.key());
        if (rule) {
            counts[key] = field(counts, key, 0).get<uint64_t>() + 1;
            if (examples.size() < 100) examples.push_back(where.size() <= 4096 ? where : "[affected path exceeds 4096-byte display boundary; count retained]");
            if (rule->at("action") == "remove") it = value.erase(it); else { it.value() = rule->at("value"); ++it; }
        } else { transform(it.value(), settings, counts, examples, where, depth + 1, cancel); ++it; }
    }
}
#ifdef _WIN32
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    Handle() = default;
    explicit Handle(HANDLE handle) : value(handle) { require_share(value != INVALID_HANDLE_VALUE, "Sharing copy path/handle open failed"); }
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
fs::path local_path(const fs::path& input) {
    require_share(!input.empty() && utf8(input).find('\0') == std::string::npos, "Empty or NUL sharing path");
    for (const auto& part : input) require_share(part != "..", "Sharing paths must not contain parent traversal");
    auto value = fs::absolute(input).lexically_normal();
    auto root = value.root_name().native(); require_share(root.size() == 2 && root[1] == L':' && value.has_root_directory(), "Sharing copy requires a local drive path");
    for (const auto& part : value.relative_path()) { auto name = part.native(); require_share(name.find_first_of(L":*?\"<>|") == name.npos && !name.empty() && name.back() != L'.' && name.back() != L' ', "Invalid sharing path component"); }
    return value;
}
class Directories {
    std::vector<std::unique_ptr<Handle>> handles;
    std::set<fs::path> held;
public:
    void lock(const fs::path& value, bool own = true) {
        fs::path current = value.root_path();
        auto acquire = [&](bool final) {
            auto marker = current / (final ? L".exclusive-namespace" : L".ancestor"); if (held.contains(marker)) return;
            auto handle = std::make_unique<Handle>(CreateFileW(io_path(current).c_str(), FILE_READ_ATTRIBUTES, final ? FILE_SHARE_READ : FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            BY_HANDLE_FILE_INFORMATION info{}; require_share(GetFileInformationByHandle(handle->value, &info) && (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT), "Sharing directory is not ordinary or is a reparse point");
            held.insert(marker); handles.push_back(std::move(handle));
        };
        acquire(value == current && own);
        for (const auto& part : value.relative_path()) { current /= part; acquire(current == value && own); }
    }
    void create(const fs::path& directory) {
        require_share(CreateDirectoryW(io_path(directory).c_str(), nullptr) != 0, "Sharing destination directory already exists or creation failed"); lock(directory);
    }
};
class Input {
    Handle handle;
public:
    fs::path file; uint64_t bytes;
    explicit Input(fs::path value) : handle(CreateFileW(io_path(value).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)), file(std::move(value)) {
        BY_HANDLE_FILE_INFORMATION info{}; require_share(GetFileInformationByHandle(handle.value, &info) && !(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) && GetFileType(handle.value) == FILE_TYPE_DISK, "Sharing input is not an ordinary disk file");
        bytes = uint64_t(info.nFileSizeHigh) << 32 | info.nFileSizeLow;
    }
    size_t read(char* data, size_t capacity) { DWORD count = 0; require_share(ReadFile(handle.value, data, DWORD(capacity), &count, nullptr) != 0, "Sharing input read failed"); return count; }
    void rewind() { LARGE_INTEGER start{}; require_share(SetFilePointerEx(handle.value, start, nullptr, FILE_BEGIN) != 0, "Sharing input seek failed"); }
    std::string hash(const std::atomic_bool* cancel) { rewind(); auto result = reader_sha256([&](char* data, size_t size) { check_cancel(cancel); return read(data, size); }); rewind(); return result; }
};
class Output {
    Handle handle;
public:
    fs::path file; uint64_t bytes = 0;
    explicit Output(fs::path value) : handle(CreateFileW(io_path(value).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)), file(std::move(value)) {}
    void write(std::string_view value) { while (!value.empty()) { DWORD count = 0; require_share(WriteFile(handle.value, value.data(), DWORD(std::min<size_t>(value.size(), 1048576)), &count, nullptr) && count, "Sharing output write failed"); bytes += count; value.remove_prefix(count); } }
    void finish() { require_share(FlushFileBuffers(handle.value) != 0, "Sharing output synchronization failed"); }
};
class InputBuffer : public std::streambuf {
    Input& input; std::array<char, 65536> buffer;
    int_type underflow() override { auto size = input.read(buffer.data(), buffer.size()); if (!size) return traits_type::eof(); setg(buffer.data(), buffer.data(), buffer.data() + size); return traits_type::to_int_type(*gptr()); }
public:
    explicit InputBuffer(Input& source) : input(source) {}
};
struct File { fs::path relative; Json report; };
struct Plan { fs::path source; Json settings, counts = Json::object(), examples = Json::array(); std::vector<File> files; std::string binding; uint64_t bytes = 0; };
bool record_line(std::istream& input, std::string& line, const std::atomic_bool* cancel) {
    line.clear(); char c;
    while (input.get(c)) { if (c == '\n') return true; require_share(line.size() < record_budget, "JSONL record exceeds sharing byte coverage"); line += c; if (line.size() % 65536 == 0) check_cancel(cancel); }
    require_share(input.eof(), "Sharing input stream failed"); return !line.empty();
}
Json process(Input& input, const fs::path& relative, const Json& settings, Json& examples, Output* output, const std::atomic_bool* cancel) {
    auto extension = folded(utf8(relative.extension())); Json counts = Json::object(); uint64_t records = 0;
    bool json = extension == ".json", jsonl = extension == ".jsonl";
    if (!json && !jsonl) {
        if (settings.at("copyOpaque") != true) return {{"disposition", "omitted"}, {"reason", "opaque-content-not-selected"}, {"fieldCounts", counts}, {"records", 0}};
        if (output) { std::array<char, 65536> buffer; size_t size; while ((size = input.read(buffer.data(), buffer.size()))) { check_cancel(cancel); output->write({buffer.data(), size}); } }
        return {{"disposition", "copied-verbatim-unprocessed"}, {"reason", "explicit-copyOpaque; no field processing"}, {"fieldCounts", counts}, {"records", 0}};
    }
    auto record = [&](std::string data) {
        if (!records && data.starts_with("\xef\xbb\xbf")) data.erase(0, 3);
        if (trim(data).empty()) { if (output) output->write("\n"); return; }
        numeric_coverage(data); auto value = parse(data); transform(value, settings, counts, examples, utf8(relative) + ":" + std::to_string(records + 1), 0, cancel);
        if (value.is_object() && (relative == "summary.json" || relative == "export.summary.json")) {
            value["sharingCopy"] = {{"protocol", "v8trace/sharing-copy/1"}, {"derivedOnly", true}, {"originalReferencesNotRebound", true}};
            value["integrityConclusion"] = "derived-not-assessed"; value["captureConclusion"] = "derived-sharing-copy";
        }
        if (output) output->write(dump(value) + "\n"); ++records;
    };
    if (json) { require_share(input.bytes <= record_budget, "JSON file exceeds sharing byte coverage"); std::string data(size_t(input.bytes), '\0'); size_t used = 0; while (used < data.size()) { check_cancel(cancel); auto count = input.read(data.data() + used, std::min<size_t>(65536, data.size() - used)); require_share(count != 0, "Short sharing JSON file"); used += count; } record(std::move(data)); }
    else { InputBuffer buffer(input); std::istream stream(&buffer); std::string line; while (record_line(stream, line, cancel)) { check_cancel(cancel); record(std::move(line)); } }
    return {{"disposition", "processed-json"}, {"fieldCounts", counts}, {"records", records}};
}
Plan scan(const fs::path& source, const Json& settings, Directories& directories, const Sink& progress, const std::atomic_bool* cancel) {
    Plan plan; plan.source = local_path(source); plan.settings = share_field_settings(settings); directories.lock(plan.source);
    uint64_t entries = 0, names = 0;
    for (fs::recursive_directory_iterator it(plan.source), end; it != end; ++it) {
        check_cancel(cancel); require_share(++entries <= file_budget, "Sharing inventory exceeds 20000 entries");
        auto attributes = GetFileAttributesW(io_path(it->path()).c_str()); require_share(attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT), "Sharing source reparse entry rejected");
        auto relative = local_path(it->path()).lexically_relative(plan.source); names += utf8(relative).size(); require_share(names <= path_budget, "Sharing inventory path bytes exceed budget");
        if (attributes & FILE_ATTRIBUTE_DIRECTORY) directories.lock(it->path());
        else { Input input(it->path()); require_share(input.bytes <= UINT64_MAX - plan.bytes, "Sharing source byte total overflows"); plan.files.push_back({relative, {{"sourcePath", utf8(relative)}, {"sourceBytes", input.bytes}, {"sourceSha256", input.hash(cancel)}}}); plan.bytes += input.bytes; }
    }
    require_share(std::any_of(plan.files.begin(), plan.files.end(), [](const File& f) { return f.relative == "summary.json" || f.relative == "export.summary.json"; }), "Choose a capture or original export with a root summary");
    std::sort(plan.files.begin(), plan.files.end(), [](const File& a, const File& b) { return a.relative < b.relative; });
    size_t processed_files = 0;
    for (auto& file : plan.files) {
        check_cancel(cancel); Input input(plan.source / file.relative);
        require_share(input.bytes == file.report.at("sourceBytes").get<uint64_t>() && input.hash(cancel) == str(file.report, "sourceSha256"), "Sharing source changed during preview");
        auto examples = plan.examples;
        try { file.report.update(process(input, file.relative, plan.settings, plan.examples, nullptr, cancel)); }
        catch (const std::exception& error) { check_cancel(cancel); plan.examples = std::move(examples); file.report.update({{"disposition", "omitted"}, {"reason", std::string("JSON coverage: ") + error.what()}, {"fieldCounts", Json::object()}, {"records", 0}}); }
        for (auto it = file.report["fieldCounts"].begin(); it != file.report["fieldCounts"].end(); ++it) plan.counts[it.key()] = field(plan.counts, it.key(), 0).get<uint64_t>() + it.value().get<uint64_t>();
        ++processed_files; if (progress && (processed_files == 1 || processed_files % 64 == 0 || processed_files == plan.files.size())) progress({{"files", processed_files}, {"source", utf8(file.relative)}});
    }
    Json snapshot = Json::array(); for (const auto& file : plan.files) snapshot.push_back(file.report);
    plan.binding = sha256(dump(Json{{"protocol", "v8trace/share-preview-binding/1"}, {"source", utf8(plan.source)}, {"settings", plan.settings}, {"files", snapshot}, {"coverage", coverage()}}));
    return plan;
}
Json report(const Plan& plan) {
    Json dispositions = Json::object(), sample = Json::array();
    for (const auto& file : plan.files) { auto kind = str(file.report, "disposition"); dispositions[kind] = field(dispositions, kind, 0).get<uint64_t>() + 1; if (sample.size() < 20) sample.push_back(file.report); }
    return {{"protocol", "v8trace/share-preview/1"}, {"source", utf8(plan.source)}, {"settings", plan.settings}, {"previewBinding", plan.binding}, {"files", plan.files.size()}, {"sourceBytes", plan.bytes}, {"dispositions", dispositions}, {"affectedFieldCounts", plan.counts}, {"affectedPathsSample", plan.examples}, {"filesSample", sample}, {"coverage", coverage()}, {"originalsModified", false}};
}
#endif
}
Json share_field_settings(const Json& settings) {
    require_share(settings.is_object() && str(settings, "protocol") == "v8trace/share-fields/1", "Expected v8trace/share-fields/1 settings");
    for (auto it = settings.begin(); it != settings.end(); ++it) require_share(it.key() == "protocol" || it.key() == "rules" || it.key() == "copyOpaque", "Unknown sharing field setting");
    auto rules = field(settings, "rules"); require_share(rules.is_array() && rules.size() <= 64, "Sharing rules must be an array of at most 64 entries");
    Json normalized = Json::array(); std::set<std::string> names;
    for (const auto& rule : rules) {
        require_share(rule.is_object(), "Sharing rule must be an object");
        for (auto it = rule.begin(); it != rule.end(); ++it) require_share(it.key() == "field" || it.key() == "action" || it.key() == "value", "Unknown sharing rule option");
        require_share(field(rule, "field").is_string(), "Sharing field must be a string"); auto key = folded(str(rule, "field"));
        require_share(!key.empty() && key.size() <= 128 && key.find('\0') == key.npos && names.insert(key).second, "Empty, duplicate or oversized sharing field");
        auto action = str(rule, "action"); require_share(action == "replace" || action == "remove", "Sharing action must be replace or remove");
        Json item{{"field", key}, {"action", action}};
        if (action == "replace") { require_share(field(rule, "value").is_string() && str(rule, "value").size() <= 256, "Sharing replacement must be a string of at most 256 bytes"); item["value"] = rule.at("value"); }
        else require_share(!rule.contains("value"), "Remove rule does not use a replacement value");
        normalized.push_back(item);
    }
    auto opaque = field(settings, "copyOpaque", false); require_share(opaque.is_boolean(), "copyOpaque must be boolean");
    return {{"protocol", "v8trace/share-fields/1"}, {"rules", normalized}, {"copyOpaque", opaque}};
}
Json preview_share_copy(const fs::path& source, const Json& settings, const Sink& progress, const std::atomic_bool* cancel) {
#ifdef _WIN32
    Directories directories; return report(scan(source, settings, directories, progress, cancel));
#else
    (void)source; (void)settings; (void)progress; (void)cancel; throw std::runtime_error("Sharing copy secure local path backend currently requires Windows");
#endif
}
Json create_share_copy(const fs::path& source, const fs::path& destination, const Json& settings, std::string_view binding, const Sink& progress, const std::atomic_bool* cancel) {
#ifdef _WIN32
    Directories directories; auto plan = scan(source, settings, directories, progress, cancel); check_cancel(cancel);
    require_share(binding.size() == 64 && plan.binding == binding, "Sharing preview changed; review fresh field/source coverage first");
    auto output = local_path(destination); directories.lock(output.parent_path(), false);
    require_share(output != plan.source && !inside(plan.source, output) && !inside(output, plan.source), "Sharing destination overlaps original evidence");
    directories.create(output);
    try {
        { Output started(output / "share.started.json"); started.write(dump(Json{{"protocol", "v8trace/sharing-copy/1"}, {"state", "started-not-complete"}, {"previewBinding", plan.binding}, {"source", utf8(plan.source)}, {"createdAt", now()}}) + "\n"); started.finish(); }
        directories.create(output / "content"); std::set<fs::path> created{output, output / "content"};
        auto directory = [&](const fs::path& relative) { auto current = output / "content"; for (const auto& part : relative) { current /= part; if (created.insert(current).second) directories.create(current); } };
        Json files = Json::array();
        for (auto& file : plan.files) {
            check_cancel(cancel); Input input(plan.source / file.relative);
            require_share(input.bytes == file.report.at("sourceBytes").get<uint64_t>() && input.hash(cancel) == str(file.report, "sourceSha256"), "Sharing source changed after preview");
            auto item = file.report;
            if (str(item, "disposition") != "omitted") {
                directory(file.relative.parent_path()); auto target = output / "content" / file.relative; Output copy(target); Json examples = Json::array();
                auto processed = process(input, file.relative, plan.settings, examples, &copy, cancel); copy.finish();
                require_share(processed.at("fieldCounts") == item.at("fieldCounts") && processed.at("records") == item.at("records"), "Sharing field coverage changed during copy");
                item["copyPath"] = "content/" + utf8(file.relative); item["copyBytes"] = copy.bytes; item["copySha256"] = file_sha256(io_path(target));
            }
            files.push_back(item); if (progress && (files.size() == 1 || files.size() % 64 == 0 || files.size() == plan.files.size())) progress({{"copiedFiles", files.size()}, {"source", utf8(file.relative)}});
        }
        check_cancel(cancel); auto final = scan(source, settings, directories, {}, cancel);
        require_share(final.binding == plan.binding, "Sharing source inventory changed during copy; partial destination retained");
        auto manifest = report(plan); manifest["protocol"] = "v8trace/sharing-copy/1"; manifest["state"] = "complete-derived-copy"; manifest["createdAt"] = now(); manifest["output"] = utf8(output); manifest["sourceSnapshotVerified"] = true; manifest["files"] = std::move(files);
        std::string manifest_hash;
        { Output file(output / "share.manifest.json"); file.write(dump(manifest, 2) + "\n"); file.finish(); manifest_hash = file_sha256(io_path(file.file)); }
        return {{"protocol", "v8trace/sharing-copy/1"}, {"state", "complete-derived-copy"}, {"output", utf8(output)}, {"manifestSha256", manifest_hash}, {"sourcePreviewBinding", plan.binding}, {"originalsModified", false}, {"coverage", coverage()}};
    } catch (const std::exception& error) {
        try { Output failed(output / "share.failed.json"); failed.write(dump(Json{{"protocol", "v8trace/sharing-copy/1"}, {"state", cancel && cancel->load() ? "cancelled" : "failed"}, {"error", error.what()}, {"source", utf8(plan.source)}, {"previewBinding", plan.binding}, {"originalsModified", false}, {"partialDestinationRetained", true}}) + "\n"); failed.finish(); } catch (...) {}
        throw;
    }
#else
    (void)source; (void)destination; (void)settings; (void)binding; (void)progress; (void)cancel; throw std::runtime_error("Sharing copy secure local path backend currently requires Windows");
#endif
}
}
