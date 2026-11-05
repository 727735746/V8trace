#include "v8trace/core.h"
#include <algorithm>
#include <iostream>

using namespace v8trace;
namespace {
unsigned checks = 0;
void check(bool condition, const char* message) { ++checks; if (!condition) throw std::runtime_error(message); }
constexpr uint64_t wide = 9007199254740993ULL;
const std::string producer = "0123456789abcdef0123456789abcdef";
Json fields(const std::string& kind, uint64_t sequence) {
  Json record{{"schema", "v8trace/causality/1"}, {"protocol", "v8trace/causality/1"}, {"producerId", producer},
      {"kind", kind}, {"epoch", std::to_string(wide)}, {"sequence", std::to_string(sequence)},
      {"taskId", std::to_string(wide + 1)}, {"operationId", std::to_string(wide + 2)}, {"resourceId", std::to_string(wide + 3)},
      {"taskKind", "async-operation"}, {"relatedTaskId", std::to_string(wide + 4)}, {"pid", 71}, {"tid", "9"}};
  if (kind == "http-root") record["dataJson"] = dump({{"kind", kind}, {"url", "https://fixture.test/请求"}, {"bytes", "UTF8-完整"}, {"stack", Json::array({{{"line", 12}, {"name", "callback"}}})}});
  else record["reason"] = kind == "coverage-gap" ? "task-posting-unobserved" : "v8trace/causality-model/3";
  return record;
}
Json native(Json record, bool compact, uint64_t index) {
  if (compact) {
    record["storageProtocol"] = "v8trace/causal-event/2";
    for (auto key : {"epoch", "sequence", "taskId", "operationId", "resourceId", "relatedTaskId", "tid"}) record[key] = decimal(str(record, key));
  }
  return {{"name", compact ? "V8TraceCausalEvent" : "V8TraceCausality"}, {"cat", "disabled-by-default-v8.causality"},
      {"pid", 71}, {"tid", 9}, {"ts", 100 + index}, {"args", compact ? record : Json{{"record", dump(record)}}}};
}
}
int main() {
  auto root = fs::temp_directory_path() / ("v8trace-causal-storage-" + uuid()); fs::create_directory(root);
  try {
    Json expected = Json::array(), legacy = Json::array(), compact = Json::array();
    for (const auto* kind : {"session-start", "registered", "invocation-start", "http-root", "coverage-gap", "invocation-end", "local-subtree-closed"}) {
      auto record = fields(kind, wide + expected.size()); expected.push_back(record);
      legacy.push_back(native(record, false, expected.size())); compact.push_back(native(record, true, expected.size()));
    }
    auto run = [&](Json rows, const std::string& name, bool enabled = true) {
      auto source = root / (name + ".json"); write_json(source, {{"traceEvents", rows}});
      Json options{{"httpRootTrace", enabled}, {"maxBytes", 16777216}, {"maxEvents", 1000}, {"categories", Json::array({"causality"})}};
      Store store(root / name, options); { Trace trace(source); import_causality(trace, store, options); }
      return store.finalize({{"backend", "chromium-native"}, {"captureConclusion", "collected"}});
    };
    check(run(legacy, "legacy").at("eventCount") == expected.size(), "legacy JSON-string stream is retained");
    auto summary = run(compact, "compact");
    check(summary.at("eventCount") == expected.size() && summary.at("criticalGapCount") == 0, "all compact events are admitted without filtering");
    size_t count = 0;
    lines(root / "compact/events/causality.jsonl", [&](const Json& row) {
      auto record = row.at("payload"); record.erase("storageProtocol");
      check(record == expected.at(count++), "wide identities, JSON metadata, reasons and Unicode equal legacy semantics exactly");
      check(row.at("processId") == 71 && row.at("threadId") == 9, "native process/thread evidence retained");
    });
    check(count == expected.size(), "compact metadata and coverage gaps were not dropped");
    auto causal = load_json(root / "compact/causality-summary.json");
    check(causal.at("completeCausality") == false, "successful decoding is not a root completeness assertion");
    check(run(compact, "not-selected", false).at("eventCount") == 0, "switch-off remains unselected");
    auto invalid = compact; invalid[0]["args"]["storageProtocol"] = "unknown";
    check(run(invalid, "bad-protocol").at("criticalGapCount") == 1, "unknown compact protocol remains critical instead of being guessed");
    invalid = compact; invalid[0]["args"]["epoch"] = -1;
    check(run(invalid, "bad-epoch").at("criticalGapCount") == 1, "invalid numeric epoch is not repaired into a plausible identity");
    std::reverse(compact.begin(), compact.end());
    check(run(compact, "reordered").at("eventCount") == expected.size(), "out-of-order source events preserve all producer sequence identities");
    count = 0; lines(root / "reordered/events/causality.jsonl", [&](const Json& row) {
      check(str(row.at("payload"), "sequence") == std::to_string(wide + count++), "wide producer sequences order exactly without floating point rounding");
    });
    fs::remove_all(root); std::cout << "PASS " << checks << " checks\n"; return 0;
  } catch (const std::exception& error) { std::cerr << "FAIL " << checks << ": " << error.what() << '\n'; fs::remove_all(root); return 1; }
}
