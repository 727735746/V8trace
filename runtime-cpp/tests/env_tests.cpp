#include "v8trace/core.h"
#include "v8trace/edition.h"
#include <algorithm>
#include <iostream>

using namespace v8trace;
namespace {
unsigned checks = 0;
void check(bool value, const char* label) { ++checks; if (!value) throw std::runtime_error(label); }
template<class F> void rejected(F action, const char* label) { bool failed = false; try { action(); } catch (const std::exception&) { failed = true; } check(failed, label); }
Json event(Json record, bool stats = false) { return {{"name", stats ? "V8TraceEnvStats" : "V8TraceEnv"}, {"cat", "disabled-by-default-v8.envprobe"}, {"pid", 32}, {"tid", stats ? 9 : 7}, {"ts", 100}, {"args", {{"record", dump(record)}}}}; }
Json fixture(const Json& options) {
  Json events = Json::array(); uint64_t seq = 0;
  auto add = [&](Json record) { record.update({{"protocol", "v8trace/env/1"}, {"producerId", "1"}, {"epoch", 2}, {"realmId", 1}, {"isolateId", "a"}, {"gcEpoch", 10}, {"producerSeq", ++seq}, {"identityUnchanged", true}, {"source", {{"known", true}, {"url", "https://example.test/target.js"}, {"scriptId", 4}}}}); events.push_back(event(record)); };
  add({{"phase", "configuration"}, {"engine", "Ignition"}, {"absent", true}, {"interfaces", true}, {"mutations", true}, {"inventory", true}, {"filter", str(options.at("envFilter"), "token")}, {"propertyBudget", 65536}, {"invokesUserCode", false}});
  add({{"phase", "inventory"}, {"interfaces", Json::array({{{"name", "Example"}, {"prototype", {{"properties", Json::array()}}}}})}, {"global", {{"properties", Json::array()}}}, {"coverage", "declared-raw-storage"}, {"lazyAccessorsEvaluated", false}, {"virtualPropertiesEnumerated", false}});
  add({{"phase", "property-entry"}, {"operationId", "1"}, {"operation", "get-named"}, {"receiver", {{"type", "heap-reference"}, {"address", "123"}}}, {"key", {{"type", "string"}, {"value", "missing"}}}, {"presenceBefore", "unknown-dynamic-host"}});
  add({{"phase", "property-exit"}, {"operationId", "1"}, {"result", {{"type", "undefined"}}}});
  add({{"phase", "query-entry"}, {"operationId", "2"}, {"operation", "own-descriptor"}, {"receiver", {{"type", "heap-reference"}}}, {"key", {{"type", "string"}, {"value", "definedButUndefined"}}}});
  add({{"phase", "query-exit"}, {"operationId", "2"}, {"outcome", "present"}, {"descriptor", {{"value", {{"type", "undefined"}}}, {"enumerable", false}}}});
  events.push_back(event({{"protocol", "v8trace/env-stats/1"}, {"producerThreadId", 7}, {"producerId", "1"}, {"epoch", 2}, {"final", true}, {"attempted", 2}, {"selected", 2}, {"filtered", 0}, {"emittedEvents", 6}}, true));
  return events;
}
}
int main() {
  auto root = fs::temp_directory_path() / ("v8trace-env-tests-" + uuid()); fs::create_directory(root);
  try {
    auto assets = path(V8TRACE_ENV_ASSETS);
    Json input{{"url", "https://example.test/"}, {"output", utf8(root / "unused")}, {"envAbsentProbe", true}, {"envInterfaceProbe", true}, {"envHostMutation", true}, {"envInventory", true}, {"envScriptInclude", "target.js"}, {"maxEvents", 100000}, {"maxBytes", 100000000}};
    auto options = validate_options(input, assets, false);
    check(env_selected(options) && options.at("categories") == Json::array({"env"}), "standalone environment channels");
    check(options.at("hooks") == false && options.at("jsCallDetail") == false, "no implicit JS Hook or JSCall");
    check(str(options.at("envFilter"), "unknownSource") == "exclude", "scoped environment filter excludes unknown by default");
    check(!str(options.at("envFilter"), "token").empty() && options.at("detailFilter").at("active") == false, "independent source filter");
    check(capabilities(options).at("env.inventory") == "native-Ignition-host-observation", "environment capability");
    auto invalid = input; invalid["envInventoryProperties"] = 1048577; rejected([&] { validate_options(invalid, assets, false); }, "explicit inventory maximum");
    invalid = input; invalid["envAbsentProbe"] = "true"; rejected([&] { validate_options(invalid, assets, false); }, "boolean type enforced");
    invalid = input; for (auto key : {"envAbsentProbe", "envInterfaceProbe", "envHostMutation", "envInventory"}) invalid[key] = false;
    rejected([&] { validate_options(invalid, assets, false); }, "filter without probe rejected");
    fs::create_directory(root / "browser"); write_file(root / "browser/chrome.exe", "fixture executable"); write_file(root / "browser/chrome.dll", "fixture library");
    Json descriptor{{"schemaVersion", 1}, {"backend", "chromium-opcode"}, {"protocol", "v8trace/opcode/1"}, {"category", "disabled-by-default-v8.opcode"}, {"binaryHashes", {{"chrome.exe", file_sha256(root / "browser/chrome.exe")}, {"chrome.dll", file_sha256(root / "browser/chrome.dll")}}}};
    descriptor["edition"] = edition_id();
    auto verified = input; verified["chromePath"] = utf8(root / "browser/chrome.exe"); write_json(root / "browser/v8trace-opcode.json", descriptor);
    rejected([&] { validate_options(verified, assets, true); }, "old browser environment capability rejected before launch");
    descriptor.update({{"envProbeProtocol", "v8trace/env/1"}, {"envStatsProtocol", "v8trace/env-stats/1"}, {"envProbePatchSha256", std::string(64, 'a')}, {"envProbeFeatures", Json::array({"absent", "interfaces", "mutations", "inventory"})}}); write_json(root / "browser/v8trace-opcode.json", descriptor);
    rejected([&]{validate_options(verified,assets,true);},"old environment producer without builtin/continuation support rejected");
    descriptor["envBuiltinProtocol"]="v8trace/env-builtin/1";write_json(root/"browser/v8trace-opcode.json",descriptor);
    check(validate_options(verified, assets, true).contains("envBackend"), "complete fixture descriptor accepted");
    descriptor["envProbeFeatures"] = Json::array({"absent"}); write_json(root / "browser/v8trace-opcode.json", descriptor);
    rejected([&] { validate_options(verified, assets, true); }, "selected environment feature checked independently");
    { Store launch(root / "launch", options); auto planned = launch_config(options, launch); auto args = dump(planned.at("args")); check(args.find("--trace-env-absent") != args.npos && args.find("--trace-env-filter=") != args.npos, "producer flag wiring"); check(dump(planned.at("config")).find("disabled-by-default-v8.envprobe") != std::string::npos, "native trace category"); }
    auto baseline = fixture(options);
    auto run = [&](Json events, const std::string& name) {
      auto file = root / (name + ".json"); write_json(file, {{"traceEvents", events}});
      Store store(root / name, options); { Trace trace(file); import_environment(trace, store, options); }
      return store.finalize({{"backend", "chromium-native"}, {"captureConclusion", "collected"}});
    };
    auto summary = run(baseline, "valid"); check(summary.at("integrityConclusion") == "complete", "complete native environment fixture");
    check(summary.at("eventCount") == 6, "all environment records retained");
    auto reversed = baseline; std::reverse(reversed.begin(), reversed.end()); check(run(reversed, "reversed").at("integrityConclusion") == "complete", "producer sequence normalizes shuffled records");
    auto windows = baseline; for (auto value : baseline) { value["v8traceWindow"] = 2; windows.push_back(value); }
    check(run(windows, "windows").at("integrityConclusion") == "complete", "pause windows isolate producer sequences");
    auto missing = baseline; missing.erase(3); check(run(missing, "missing").at("integrityConclusion") == "partial", "middle event loss detected");
    missing = baseline; missing.erase(6); check(run(missing, "stats-missing").at("integrityConclusion") == "partial", "missing producer final statistics detected");
    missing = baseline; auto raw = parse(str(missing[6]["args"], "record")); raw["emittedEvents"] = 8; missing[6]["args"]["record"] = dump(raw); check(run(missing, "tail").at("integrityConclusion") == "partial", "tail loss detected by writer count");
    missing = baseline; raw = parse(str(missing[0]["args"], "record")); raw["mutations"] = false; missing[0]["args"]["record"] = dump(raw); check(run(missing, "config-mismatch").at("integrityConclusion") == "partial", "selected channel mismatch detected");
    missing = baseline; raw = parse(str(missing[1]["args"], "record")); raw["coverage"] = "partial-budget"; missing[1]["args"]["record"] = dump(raw); check(run(missing, "inventory-budget").at("integrityConclusion") == "partial", "inventory budget boundary explicit");
    missing = baseline; raw = parse(str(missing[6]["args"], "record")); raw["final"] = false; missing[6]["args"]["record"] = dump(raw); check(run(missing, "writer-active").at("integrityConclusion") == "partial", "nonquiescent writer detected");
    missing = baseline; raw = parse(str(missing[6]["args"], "record")); raw["attempted"] = 3; raw["selected"] = 3; missing[6]["args"]["record"] = dump(raw); check(run(missing, "operation-tail").at("integrityConclusion") == "partial", "selected operation lost before serialization detected");
    uint64_t rows = 0; lines(root / "valid/events/env.jsonl", [&](const Json& row) { ++rows; if (str(row, "kind") == "native-query-exit") check(row.at("payload").at("outcome") == "present", "defined undefined remains present"); }); check(rows == 6, "full event log rows");
    auto exported = export_case(root / "valid", root / "export"); check(fs::exists(root / "export/native-env-summary.json") && fs::exists(root / "export/env-inventory.jsonl"), "handoff carries native inventory");
    check(load_json(root / "export/env.requirements.json").at("completeEnvironmentModel") == false, "observations not advertised as environment implementation");
    for (const auto& entry : fs::directory_iterator(root)) check(!entry.path().filename().string().starts_with("analysis-cpp-") && !entry.path().filename().string().starts_with("decode-cpp-"), "temporary index removed");
    fs::remove_all(root); std::cout << "PASS " << checks << " checks\n"; return 0;
  } catch (const std::exception& error) { std::cerr << "FAIL " << checks << ": " << error.what() << '\n'; fs::remove_all(root); return 1; }
}
