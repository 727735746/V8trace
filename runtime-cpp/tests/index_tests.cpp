#include "v8trace/core.h"
#include "v8trace/index.h"
#include <chrono>
#include <iostream>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

namespace v8trace { void legacy_correlate(Store&); Json legacy_export_case(const fs::path&, const fs::path&); }
using namespace v8trace;
namespace {
unsigned checks = 0;
void check(bool value, const char* label) { ++checks; if (!value) throw std::runtime_error(label); }
template<class F> void rejected(F action, const char* label) { bool failed = false; try { action(); } catch (const std::exception&) { failed = true; } check(failed, label); }
uint64_t peak_memory() {
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS memory{}; if (!GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory))) throw std::runtime_error("Memory measurement failed"); return uint64_t(memory.PeakWorkingSetSize);
#else
  rusage usage{}; if (getrusage(RUSAGE_SELF, &usage)) throw std::runtime_error("Memory measurement failed");
#ifdef __APPLE__
  return uint64_t(usage.ru_maxrss);
#else
  return uint64_t(usage.ru_maxrss) * 1024;
#endif
#endif
}
void no_spool(const fs::path& directory) {
  for (auto& entry : fs::directory_iterator(directory)) check(!utf8(entry.path().filename()).starts_with("analysis-cpp-"), "owned index directory removed");
}
Json collect(const fs::path& file) { Json rows = Json::array(); lines(file, [&](const Json& row) { rows.push_back(row); }); return rows; }
void fixture(Store& store, unsigned count, bool detailed) {
  auto body = store.blob("body-完整-😀", "fixture-body");
  Json script{{"processId", 1}, {"isolateId", "isolate"}, {"isolateAddress", "0x100"}, {"scriptId", 1}, {"url", "https://fixture.test/target.js"}, {"isModule", false}, {"source", body}};
  store.append("catalog/scripts.jsonl", script);
  if (detailed) {
    store.append("catalog/scripts.jsonl", script);
    script["source"] = store.blob("alternate", "fixture-body"); script["isModule"] = true; store.append("catalog/scripts.jsonl", script);
  }
  for (unsigned i = 0; i < count; ++i) {
    auto category = detailed && i % 3 == 0 ? "native" : "jscall";
    Json call{{"callId", std::to_string(i % 7)}, {"isolateId", "isolate"}, {"processId", 1}, {"threadId", 2}, {"contextId", "context"}, {"scriptId", 1}, {"functionStart", 0}, {"functionName", "函数"}, {"phase", "entry"}, {"traceTimestampMicroseconds", i * 20ULL + 100}};
    store.event(category, "call", call);
    if (detailed) { call["phase"] = "argument"; call["traceTimestampMicroseconds"] = i * 20ULL + 101; store.event(category, "call", call); }
    if (!detailed || i % 11) {
      call["phase"] = detailed && i % 5 == 0 ? "suspend" : "return"; call["traceTimestampMicroseconds"] = i * 20ULL + 110; store.event(category, "call", call);
      if (detailed && i % 5 == 0) { call["phase"] = "resume"; call["traceTimestampMicroseconds"] = i * 20ULL + 112; store.event(category, "call", call); call["phase"] = "throw"; call["traceTimestampMicroseconds"] = i * 20ULL + 118; store.event(category, "call", call); }
    }
    Json scope{{"processId", 1}, {"threadId", 2}, {"isolateId", detailed ? Json(nullptr) : Json("isolate")}, {"isolateAddress", "0x100"}, {"realmId", "realm"}, {"contextId", "context"}, {"traceTimestampMicroseconds", i * 20ULL + 105}};
    Json hook{{"spanId", std::to_string(detailed ? i % 31 : i)}, {"binding", "Document.cookie"}, {"operation", "get"}, {"parentSpanId", detailed && i ? Json(std::to_string((i - 1) % 31)) : Json(nullptr)}};
    store.event("env", "enter", hook, scope); scope["traceTimestampMicroseconds"] = i * 20ULL + 106; store.event("env", "return", hook, scope);
    auto url = detailed ? "https://fixture.test/" + std::to_string(i % 9) + ".js" : "https://fixture.test/" + std::to_string(i) + ".js";
    Json resource{{"processId", 1}, {"requestId", "1/" + std::to_string(i)}, {"type", detailed && i % 7 == 0 ? "Script" : "Fetch"}, {"nativeLayer", detailed && i % 2 == 0 ? "network" : "renderer"}, {"body", body}, {"bodyConclusion", "complete"}, {"postBodyConclusion", "complete"}, {"conclusion", "complete"}, {"request", {{"url", url}, {"method", "GET"}, {"hasPostData", false}}}, {"response", {{"status", 200}, {"mimeType", "application/javascript"}, {"headers", {{"Content-Type", "application/javascript"}, {"Content-Length", "99"}}}}}};
    if (detailed) resource["response"]["headers"]["X-Revision"] = std::to_string(i % 2);
    store.append("catalog/resources.jsonl", resource); store.event("http", "request", {{"requestId", std::to_string(i)}}, {{"processId", 1}});
  }
  if (detailed) {
    uint64_t start = 18014398509481985ULL;
    Json call{{"callId", "large-time"}, {"isolateId", "isolate"}, {"processId", 1}, {"threadId", 2}, {"contextId", "context"}, {"scriptId", 1}, {"phase", "entry"}, {"traceTimestampMicroseconds", start}};
    store.event("jscall", "call", call); call["phase"] = "return"; call["traceTimestampMicroseconds"] = start + 5; store.event("jscall", "call", call);
    for (unsigned i = 0; i < 7; ++i) store.event("env", "enter", {{"spanId", "boundary-" + std::to_string(i)}, {"binding", "boundary"}}, {{"processId", 1}, {"threadId", 2}, {"isolateId", "isolate"}, {"contextId", "context"}, {"realmId", "realm"}, {"traceTimestampMicroseconds", start + i}});
    Json resource{{"processId", 1}, {"requestId", "1/target"}, {"type", "Script"}, {"nativeLayer", "network"}, {"body", body}, {"bodyConclusion", "complete"}, {"postBodyConclusion", "complete"}, {"conclusion", "complete"}, {"request", {{"url", "https://fixture.test/target.js"}, {"method", "GET"}, {"hasPostData", false}}}, {"response", {{"status", 200}, {"mimeType", "application/javascript"}}}};
    store.append("catalog/resources.jsonl", resource);
  }
}
void index_contract(const fs::path& root) {
  {
    DiskIndex index(root); index.exec("CREATE TABLE data(key BLOB PRIMARY KEY,value BLOB) WITHOUT ROWID");
    auto value = parse("{\"value\":\"\\ud800\\u0000中文\",\"number\":18446744073709551615}");
    std::string key("a\0b", 3); index.run("INSERT INTO data VALUES(?,?)", Json::array({key, value}));
    auto row = index.one("SELECT key,value FROM data WHERE key=?", Json::array({key}));
    check(text(row->at(0)) == key && parse(text(row->at(1))) == value, "BLOB keys and exact values survive index");
    rejected([&] { index.one("SELECT value FROM data WHERE key=?", Json::array()); }, "parameter count checked");
    rejected([&] { index.run("SELECT missing FROM data"); }, "SQLite errors propagate");
    rejected([&] { index.each("SELECT value FROM data", Json::array(), [&](const Json&) { index.run("SELECT value FROM data"); }); }, "recursive cursor rejected");
    check(index.one("SELECT value FROM data").has_value(), "failed cursor reset");
    check(index.one("PRAGMA cache_size")->at(0) == -8192, "bounded configured cache");
    check(index.one("PRAGMA temp_store")->at(0) == 1, "file backed temporary sorting");
    check(text(index.one("PRAGMA quick_check")->at(0)) == "ok", "index structurally valid");
  }
  no_spool(root);
  rejected([&] { DiskIndex failed(root); failed.exec("INVALID SQL"); }, "exception destroys owned index");
  no_spool(root);
  {
    JsonArrayOutput array(root / "stream.json"); array.append({{"中文", "full"}}); array.begin_item(); array.fragment("{\"nested\":["); array.fragment("1,2,3]}"); array.finish();
    check(load_json(root / "stream.json") == Json::array({Json{{"中文", "full"}}, Json{{"nested", Json::array({1, 2, 3})}}}), "nested streamed JSON exact");
  }
}
void differential(const fs::path& root) {
  Json options{{"maxBytes", 1073741824ULL}, {"maxEvents", 1000000}};
  Store legacy(root / "legacy", options), indexed(root / "indexed", options);
  fixture(legacy, 120, true); fixture(indexed, 120, true);
  legacy_correlate(legacy); correlate(indexed);
  legacy.flush(); indexed.flush();
  for (auto name : {"calls", "script-index", "hook-spans", "correlations"}) check(collect(legacy.root / "catalog" / (std::string(name) + ".jsonl")) == collect(indexed.root / "catalog" / (std::string(name) + ".jsonl")), "legacy correlation semantics exact");
  auto old_summary = load_json(legacy.root / "correlation-summary.json"), new_summary = load_json(indexed.root / "correlation-summary.json"); new_summary.erase("diskIndex");
  check(old_summary == new_summary, "correlation summary compatible");
  legacy.finalize({{"backend", "chromium-native"}}); indexed.finalize({{"backend", "chromium-native"}});
  auto a = legacy_export_case(legacy.root, root / "legacy-export"), b = export_case(indexed.root, root / "indexed-export");
  for (auto name : {"resources.json", "scripts.json", "env.requirements.json", "resource.omissions.json", "resource.conflicts.json"}) check(load_json(root / "legacy-export" / name) == load_json(root / "indexed-export" / name), "legacy exported JSON semantics exact");
  check(collect(root / "legacy-export/replay.network.jsonl") == collect(root / "indexed-export/replay.network.jsonl"), "network replay exact");
  a.erase("captureId"); b.erase("captureId"); b.erase("diskIndex"); check(a == b, "export summary compatible");
  no_spool(root); no_spool(indexed.root);
}
Json scale(const fs::path& root, unsigned count) {
  auto start = std::chrono::steady_clock::now();
  Store store(root / "scale", {{"maxBytes", 2147483648ULL}, {"maxEvents", 2000000}}); fixture(store, count, false);
  auto generated = std::chrono::steady_clock::now(); correlate(store); auto correlated = std::chrono::steady_clock::now();
  auto metadata = load_json(store.root / "correlation-summary.json");
  check(metadata["calls"] == count, "scale calls complete"); check(metadata["budgetExhausted"] == false, "scale no output omission");
  check(metadata["diskIndex"]["fullScanSteps"].get<uint64_t>() < uint64_t(count) * 30 + 100, "no quadratic correlation full scans");
  auto summary = store.finalize({{"backend", "chromium-native"}}); check(summary["integrityConclusion"] == "complete", "scale integrity complete");
  auto exported = export_case(store.root, root / "scale-export"); auto finished = std::chrono::steady_clock::now();
  check(exported["networkRecords"] == count && exported["resources"] == count, "scale export complete");
  no_spool(root); no_spool(store.root);
  auto seconds = [](auto begin, auto end) { return std::chrono::duration<double>(end - begin).count(); };
  return {{"count", count}, {"generationSeconds", seconds(start, generated)}, {"correlationSeconds", seconds(generated, correlated)}, {"exportSeconds", seconds(correlated, finished)}, {"peakWorkingSetBytes", peak_memory()}, {"correlation", metadata}, {"export", exported}};
}
}
int main(int argc, char** argv) {
  auto parent = fs::canonical(fs::temp_directory_path()); auto root = parent / ("v8trace-index-" + uuid()); fs::create_directory(root);
  auto cleanup = [&] { auto target = fs::weakly_canonical(root); if (!inside(parent, target) || !utf8(target.filename()).starts_with("v8trace-index-")) throw std::runtime_error("Cleanup leaves owned fixture directory"); fs::remove_all(target); };
  try {
    Json result;
    if (argc == 3 && std::string_view(argv[1]) == "--scale") result = scale(root, unsigned(decimal(argv[2])));
    else { index_contract(root); differential(root); result = scale(root, 1000); }
    cleanup(); std::cout << "PASS " << checks << " checks\n" << dump(result) << '\n'; return 0;
  } catch (const std::exception& error) { std::cerr << "FAIL " << checks << ": " << error.what() << '\n'; cleanup(); return 1; }
}
