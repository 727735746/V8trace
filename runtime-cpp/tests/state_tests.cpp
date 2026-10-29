#include "v8trace/state.h"
#include "v8trace/edition.h"
#include <algorithm>
#include <iostream>
using namespace v8trace;
namespace {
unsigned checks = 0;
void check(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
template<class F> void reject(F action, const char* message) { bool failed = false; try { action(); } catch (...) { failed = true; } check(failed, message); }
Json identity() { return {{"protocol", "v8trace/identity/1"}, {"isolateId", "a"}, {"realmId", "1"}, {"jsCallId", "99"}, {"nativeCallId", "0"}, {"callProducerId", "7"}, {"callEpoch", "1"}}; }
Json event(std::string name, Json record, uint64_t seq = 0) { if (seq) record.update({{"protocol", "v8trace/state/1"}, {"producerId", "2"}, {"epoch", "1"}, {"producerSeq", std::to_string(seq)}}); return {{"name", name}, {"cat", "disabled-by-default-v8.state"}, {"pid", 32}, {"tid", 7}, {"ts", 100}, {"args", {{"record", dump(record)}}}}; }
Json inline_bytes(std::string_view bytes) { return {{"type", "inline-bytes"}, {"base64", base64(bytes)}, {"bytes", std::to_string(bytes.size())}}; }
const std::string payload = [] { std::string data; for (size_t i = 0; i < 100123; ++i) data.push_back(char(i % 256)); return data; }();
Json fixture(const Json& options) {
  Json events = Json::array({event("V8TraceStateSession", {{"protocol", "v8trace/state-session/1"}, {"epoch", "1"}, {"phase", "start"}, {"selected", selected_state_channels(options)}})});
  uint64_t seq = 0;
  auto add = [&](const char* channel, const char* kind, std::string id, Json extra = Json::object()) { Json record{{"channel", channel}, {"kind", kind}, {"operationId", id}, {"identity", identity()}, {"executionContextToken", "context-actual"}}; record.update(extra); events.push_back(event("V8TraceState", record, ++seq)); };
  add("storage", "operation", "1", {{"api", "Storage.setItem"}, {"key", inline_bytes(std::string("k\0", 2))}, {"value", inline_bytes("v")}});
  add("cookie", "operation", "2", {{"api", "Document.cookie"}, {"result", inline_bytes("key=value")}});
  add("indexeddb", "api-entry", "3");
  add("indexeddb", "transaction-start", "4", {{"transactionId", "20"}, {"connectionId", "10"}, {"parentOperationId", "3"}});
  add("indexeddb", "request-start", "5", {{"transactionId", "20"}, {"requestId", "30"}, {"parentOperationId", "3"}});
  for (size_t offset = 0; offset < payload.size(); offset += 32768) add("indexeddb", "value-chunk", "6", {{"contentId", "6/wire/unique"}, {"offset", std::to_string(offset)}, {"base64", base64(std::string_view(payload).substr(offset, 32768))}});
  add("indexeddb", "request-success", "6", {{"requestId", "30"}, {"result", {{"valueId", "40"}, {"wire", {{"type", "byte-reference"}, {"contentId", "6/wire/unique"}, {"bytes", std::to_string(payload.size())}, {"representation", "serialized-value"}}}}}});
  add("indexeddb", "api-exit", "3", {{"exception", false}});
  add("indexeddb", "transaction-complete", "7", {{"transactionId", "20"}});
  add("websocket", "connect-attempt", "8", {{"connectionId", "50"}});
  add("websocket", "send", "9", {{"connectionId", "50"}, {"originOperationId", "8"}, {"bytes", inline_bytes(std::string("\0\xff", 2))}});
  add("websocket", "closed", "10", {{"connectionId", "50"}, {"code", 1000}});
  events.push_back(event("V8TraceStateStats", {{"protocol", "v8trace/state-stats/1"}, {"producerThreadId", "7"}, {"producerId", "2"}, {"epoch", "1"}, {"final", true}, {"attempted", std::to_string(seq)}, {"emittedEvents", std::to_string(seq)}}));
  events.push_back(event("V8TraceStateSession", {{"protocol", "v8trace/state-session/1"}, {"epoch", "1"}, {"phase", "stop"}}));
  return events;
}
}
int main() {
  auto root = fs::temp_directory_path() / ("v8trace-state-tests-" + uuid()); fs::create_directory(root);
  try {
    auto assets = path(V8TRACE_STATE_ASSETS);
    Json input{{"url", "https://example.test/"}, {"output", utf8(root / "unused")}, {"webSocket", true}, {"indexedDB", true}, {"cookies", true}, {"storage", true}, {"maxEvents", 100000}, {"maxBytes", 100000000}};
    auto options = validate_options(input, assets, false);
    check(options.at("categories") == Json::array({"websocket", "indexeddb", "cookie", "storage"}), "four independent state categories");
    check(state_selected(options) && options.at("hooks") == false && options.at("opcode") == false && options.at("jsCallDetail") == false, "state needs no page wrapper or interpreter-only option");
    for (const auto& [key, channel] : state_channels()) { if (key == "wasmModules" || key == "vmpProbe") continue; auto one = input; for (auto [other, ignored] : state_channels()) one[other] = other == key; check(validate_options(one, assets, false).at("categories") == Json::array({channel}), "individual state switch"); one[key] = "true"; reject([&] { validate_options(one, assets, false); }, "state boolean type"); }
    { Store store(root / "launch", options); auto launch = launch_config(options, store); auto config = dump(launch); check(config.find("disabled-by-default-v8.state") != std::string::npos && config.find("disabled-by-default-v8.websocket") != std::string::npos, "native state categories configured"); check(config.find("--trace-js-calls") == std::string::npos && config.find("--jitless") == std::string::npos, "state does not force detailed calls or no JIT"); }
    fs::create_directory(root / "browser"); write_file(root / "browser/chrome.exe", "fixture"); write_file(root / "browser/chrome.dll", "engine");
    Json descriptor{{"schemaVersion", 1}, {"backend", "chromium-opcode"}, {"protocol", "v8trace/opcode/1"}, {"category", "disabled-by-default-v8.opcode"}, {"binaryHashes", {{"chrome.exe", file_sha256(root / "browser/chrome.exe")}, {"chrome.dll", file_sha256(root / "browser/chrome.dll")}}}};
    descriptor["edition"] = edition_id();
    auto verified = input; verified["chromePath"] = utf8(root / "browser/chrome.exe"); write_json(root / "browser/v8trace-opcode.json", descriptor);
    reject([&] { validate_options(verified, assets, true); }, "old state backend rejected before launch");
    descriptor.update({{"stateProtocol", "v8trace/state/1"}, {"stateStatsProtocol", "v8trace/state-stats/1"}, {"identityProtocol", "v8trace/identity/1"}, {"statePatchSha256", std::string(64, 'a')}, {"identityPatchSha256", std::string(64, 'b')}, {"stateFeatures", selected_state_channels(options)}}); write_json(root / "browser/v8trace-opcode.json", descriptor);
    check(validate_options(verified, assets, true).contains("stateBackend"), "native state identity verified"); verified["transport"] = "cdp"; check(validate_options(verified, assets, true).contains("stateBackend"), "CDP uses same native state backend");
    descriptor["stateFeatures"] = Json::array({"websocket"}); write_json(root / "browser/v8trace-opcode.json", descriptor); reject([&] { validate_options(verified, assets, true); }, "state feature checked per switch");
    auto run = [&](Json events, const std::string& name, bool calls = true) {
      write_json(root / (name + ".json"), {{"traceEvents", events}}); auto store_options = options; if (calls) store_options["categories"].push_back("jscall"); Store store(root / name, store_options);
      if (calls) store.event("jscall", "call", {{"phase", "entry"}, {"callId", "99"}, {"producerId", "7"}, {"isolateId", "a"}, {"processId", 32}, {"threadId", 7}});
      { Trace trace(root / (name + ".json")); import_state(trace, store, options); }
      return store.finalize({{"backend", "chromium-native"}, {"captureConclusion", "collected"}});
    };
    auto baseline = fixture(options); auto summary = run(baseline, "valid");
    check(summary.at("integrityConclusion") == "complete", "complete specialized native fixture");
    check(load_json(root / "valid/native-state-summary.json").at("values").at("spooledBytes") == payload.size(), "chunk byte statistics");
    check(read_file(root / "valid/blobs/sha256" / sha256(payload)) == payload, "large binary value byte exact including NUL");
    auto correlation = load_json(root / "valid/state-correlation-summary.json"); check(correlation.at("observed") > 10 && correlation.at("unresolved") == 0, "actual shared native and caller identities");
    bool key = false; lines(root / "valid/events/storage.jsonl", [&](const Json& row) { key = unbase64(str(row.at("payload").at("key"), "base64")) == std::string("k\0", 2); }); check(key, "inline original bytes retained");
    auto reverse = baseline; std::reverse(reverse.begin(), reverse.end()); check(run(reverse, "reverse").at("integrityConclusion") == "complete", "external producer ordering");
    auto windows = baseline; for (auto row : baseline) { row["v8traceWindow"] = 2; windows.push_back(row); } check(run(windows, "windows").at("integrityConclusion") == "complete", "pause separates sequence scopes"); check(load_json(root / "windows/state-correlation-summary.json").at("unresolved") > 0, "same IDs from another recording window never borrowed");
    run(baseline, "no-calls", false); check(load_json(root / "no-calls/state-correlation-summary.json").at("unresolved") > 0, "same timestamps without actual selected call stay unresolved");
    auto changed = baseline; changed.erase(6); check(run(changed, "missing").at("integrityConclusion") == "partial", "chunk hole and producer loss detected");
    changed = baseline; changed.erase(changed.end() - 2); check(run(changed, "no-stats").at("integrityConclusion") == "partial", "tail receipt mandatory");
    changed = baseline; changed.erase(changed.begin()); check(run(changed, "no-start").at("integrityConclusion") == "partial", "session start mandatory");
    changed = baseline; changed.erase(changed.end() - 1); check(run(changed, "no-stop").at("integrityConclusion") == "partial", "session stop mandatory");
    auto modify = [&](size_t index, const std::function<void(Json&)>& update) { auto value = baseline; auto record = parse(str(value[index]["args"], "record")); update(record); value[index]["args"]["record"] = dump(record); return value; };
    check(run(modify(1, [](Json& value) { value["externalBlobCount"] = "1"; }), "external-blob").at("integrityConclusion") == "partial", "external blob bytes not impersonated by metadata");
    check(run(modify(1, [](Json& value) { value["externalTokenCount"] = "1"; }), "external-token").at("integrityConclusion") == "partial", "filesystem tokens explicit gap");
    check(run(modify(1, [](Json& value) { value["channel"] = "unknown"; }), "unselected").at("integrityConclusion") == "partial", "unknown native channel rejected");
    check(run(modify(1, [](Json& value) { value["key"]["representation"] = "utf16le"; value["key"]["utf16Length"] = "2"; value["key"]["null"] = false; }), "utf16-mismatch").at("integrityConclusion") == "partial", "exact UTF16 length checked");
    check(run(modify(baseline.size() - 2, [](Json& value) { value["attempted"] = "999"; }), "writer-loss").at("integrityConclusion") == "partial", "writer admission loss detected");
    check(run(modify(baseline.size() - 2, [](Json& value) { value["final"] = false; }), "active-writer").at("integrityConclusion") == "partial", "nonquiescent writer incomplete");
    auto result = export_case(root / "valid", root / "export"); check(fs::exists(root / "export/native-state-summary.json") && fs::exists(root / "export/websocket.jsonl") && fs::exists(root / "export/state-correlations.jsonl"), "export carries state channels and receipts");
    check(read_file(root/"export/blobs/sha256"/sha256(payload))==payload,"export preserves specialized-state blob references, not just JSONL files");
    for (const auto& capture : fs::directory_iterator(root)) if (capture.is_directory()) for (const auto& file : fs::directory_iterator(capture)) check(!utf8(file.path().filename()).starts_with("state-spool-") && !utf8(file.path().filename()).starts_with("analysis-cpp-"), "owned temporary spool removed");
    fs::remove_all(root); std::cout << "PASS " << checks << " checks\n"; return 0;
  } catch (const std::exception& error) { std::cerr << "FAIL " << checks << ": " << error.what() << '\n'; fs::remove_all(root); return 1; }
}
