#include "v8trace/core.h"
#include "v8trace/state.h"
#include <cmath>
#include <regex>

namespace v8trace {
namespace {
void need(bool condition, const char* error) { if (!condition) throw std::runtime_error(error); }
Json scope(const Json& event, const Json& record, const std::string& fidelity) { return {{"contextId", field(record, "contextId")}, {"fidelity", fidelity}}; }
std::string producer(const Json& event, const Json& record, bool channel) { auto value = text(field(event, "pid")) + '/' + text(field(event, "tid")); if (str(record, "channel") == "jscall" && record.contains("producerId")) value += "/instance:" + str(record, "producerId"); return channel ? value + '/' + str(record, "channel") : value; }
void coordinates(const Json& event, Json& record) { need(safe_uint(field(event, "pid")) && safe_uint(field(event, "tid")) && field(event, "ts").is_number(), "Invalid native process/thread/timestamp"); record["processId"] = event.at("pid"); record["threadId"] = event.at("tid"); record["traceTimestampMicroseconds"] = event.at("ts"); record["recordingWindow"] = field(event, "v8traceWindow", 0); record["scopeConclusion"] = "native-scope-only"; }
struct Sequence { uint64_t first = 0, last = 0; };
void sequence(Store& store, std::map<std::string, Sequence>& producers, const std::string& key, uint64_t n, const std::string& prefix) {
  auto& state = producers[key]; if (!state.first) { state.first = n; if (n != 1) store.gap(prefix + "-producer-prefix-missing", {{"producer", key}, {"firstSequence", n}}); }
  else if (n <= state.last) store.gap(prefix + "-duplicate-producer-sequence", {{"producer", key}, {"sequence", n}});
  else if (n != state.last + 1) store.gap(prefix + "-producer-sequence-gap", {{"producer", key}, {"after", state.last}, {"before", n}});
  state.last = n;
}
}
void import_detail(Trace& trace, Store& store, const Json& o) {
  std::set<std::string> enabled; Json counts = Json::object(); for (auto [key, channel] : std::vector<std::pair<std::string, std::string>>{{"jsCallDetail", "jscall"}, {"nativeDetail", "native"}, {"wasm", "wasm"}}) if (field(o, key, false) == true) { enabled.insert(channel); counts[channel] = 0; }
  if (enabled.empty()) return;
  Json filter = field(o, "detailFilter", Json::object()), filter_producers = Json::object();
  if (field(filter, "active", false) == true) {
    trace.each("filter", [&](const Json& row) { try { auto record = row.at("record"), event = row.at("event"); need(str(record, "protocol") == "v8trace/detail-filter/1" && field(record, "valid") == true && field(record, "token") == filter.at("token"), "Invalid producer filter identity"); for (auto name : {"jscallAccepted", "jscallFiltered", "nativeAccepted", "nativeFiltered", "unknownSource"}) need(safe_uint(field(record, name)), "Invalid filter counter"); auto key = producer(event, record, false); record["processId"] = event.at("pid"); record["threadId"] = event.at("tid"); record["traceTimestampMicroseconds"] = event.at("ts"); if (!filter_producers.contains(key) || event.at("ts") >= filter_producers[key].at("traceTimestampMicroseconds")) filter_producers[key] = record; } catch (const std::exception& e) { store.gap("detail-filter-producer-invalid", {{"error", e.what()}}); } });
    if (filter_producers.empty()) store.gap("detail-filter-producer-status-missing"); Json producers = Json::array(); for (auto& item : filter_producers) producers.push_back(item); store.json("detail-filter-summary.json", {{"protocol", "v8trace/detail-filter/1"}, {"selection", filter}, {"producers", producers}, {"countsLowerBound", true}});
  }
  std::map<std::string, Sequence> producers; std::set<std::string> active, observations; uint64_t malformed = 0;
  std::map<std::string, uint64_t> call_events, call_entries; bool compact = false; uint64_t closure_snapshots = 0, closure_bindings = 0, closure_boundaries = 0;
  trace.each("detail", [&](const Json& row) {
    try {
      auto record = row.at("record"), event = row.at("event"); auto channel = str(record, "channel"), phase = str(record, "phase");
      need(str(record, "schema") == "v8trace/detail/1" && enabled.contains(channel) && str(event, "cat").find("disabled-by-default-v8." + channel) != std::string::npos && field(record, "isolateId").is_string() && safe_uint(field(record, "producerSeq"), true), "Malformed native detailed record"); coordinates(event, record);
      if (record.contains("compactDecodeError")) throw std::runtime_error(str(record, "compactDecodeError"));
      if (channel == "jscall") {
        if (field(o, "closureBindings", false) == true) {
          auto status = validate_closure_record(record, o);
          if (field(status, "captured") == true) { ++closure_snapshots; closure_bindings += status.at("bindings").get<uint64_t>(); }
          for (auto boundary : status.at("boundaries")) { ++closure_boundaries; boundary.update({{"callId", field(record, "callId")}, {"processId", event.at("pid")}, {"producerSeq", record.at("producerSeq")}}); store.gap("jscall-closure-boundary", boundary, field(boundary, "critical", true) == true); }
        }
        auto origin = producer(event, record, false); ++call_events[origin];
        if (phase == "entry" || phase == "resume") ++call_entries[origin];
        compact = compact || str(record, "storageProtocol") == "v8trace/call/2";
        if (str(record, "storageProtocol") == "v8trace/call/2") {
          need(field(record, "valuesCaptured").is_boolean() && safe_uint(field(record, "argc")) && std::regex_match(str(record, "producerId"), std::regex("[1-9][0-9]*")) && std::regex_match(str(record, "functionId"), std::regex("[1-9][0-9]*")), "Compact call value scope or producer identity missing");
          if (field(o, "jsCallValues", true) == false) need(record.at("valuesCaptured") == false, "Structure-only capture unexpectedly observed values");
          else if (field(filter, "valueRules", false) == false) need(record.at("valuesCaptured") == true, "Selected full-value call omitted values");
          if (record.at("valuesCaptured") == true) {
            if (phase == "entry" || phase == "argument") need(field(record, "arguments").is_array() && record.at("arguments").size() == record.at("argc").get<uint64_t>() && field(record, "receiver").is_object(), "Full JSCall actual arguments missing");
            if (phase == "return") need(field(record, "result").is_object(), "Full JSCall return value missing");
            if (phase == "resume" || phase == "suspend") need(field(record, "generator").is_object() && field(record, "accumulator").is_object(), "Full generator activation values missing");
          }
        }
      }
      auto key = producer(event, record, true); sequence(store, producers, key, record.at("producerSeq").get<uint64_t>(), "detail");
      if (field(o, "objectSnapshots", false) == true && record.contains("objectObservation")) { auto config = record.at("objectObservation"); need(str(config, "schema") == "v8trace/objects/1" && field(config, "enabled") == true && field(config, "depth") == o.at("objectDepth") && field(config, "nodes") == o.at("objectNodes") && field(config, "properties") == o.at("objectProperties") && field(config, "invokesGetters") == false && field(config, "invokesProxyTraps") == false, "Object observation config mismatch"); observations.insert(key); }
      if (channel == "wasm") {
        need(phase == "instruction" && str(record, "engine") == "DrumBrake" && safe_uint(field(record, "pc")) && safe_uint(field(record, "functionIndex")) && safe_uint(field(record, "opcode")) && field(record, "opcodeName").is_string() && field(record, "state").is_array(), "Malformed executed WASM instruction");
        for (auto& slot : record.at("state")) need(safe_uint(field(slot, "index")) && field(slot, "type").is_string() && std::regex_match(str(slot, "rawHex"), std::regex("([a-f0-9]{2})+")), "Malformed WASM typed value");
      } else {
        need(std::set<std::string>{"entry", "return", "throw", "argument", "resume", "suspend"}.contains(phase) && std::regex_match(str(record, "callId"), std::regex("[1-9][0-9]*")), "Malformed call lifecycle");
        if (phase == "entry" && record.contains("arguments")) need(record.at("arguments").is_array(), "Malformed actual call arguments");
        auto call = key + '/' + str(record, "isolateId") + '/' + str(record, "callId");
        if (phase == "entry" || phase == "resume") { if (!active.insert(call).second) store.gap("detail-duplicate-call-entry", {{"callKey", call}}); }
        else if (!active.contains(call)) store.gap("detail-call-entry-missing", {{"callKey", call}, {"phase", phase}});
        else if (phase == "return" || phase == "throw" || phase == "suspend") active.erase(call);
        if (str(field(record, "exception"), "type") == "unobserved") store.gap("detail-unobserved-unwind-exception", {{"callKey", call}});
        if (field(record, "argumentsComplete") == false) store.gap("detail-native-arguments-incomplete", {{"callKey", call}, {"boundary", field(record, "boundary")}});
      }
      if (store.event(channel, channel == "wasm" ? "instruction" : "call-" + phase, record, scope(event, record, "native-" + channel + "-execution"))) counts[channel] = counts.at(channel).get<uint64_t>() + 1;
    } catch (const std::exception& e) { ++malformed; store.gap("detail-invalid-native-record", {{"error", e.what()}, {"event", row.at("event")}}); }
  });
  for (auto& [key, value] : producers) if (field(o, "objectSnapshots", false) == true && !observations.contains(key)) store.gap("detail-object-observation-config-missing", {{"producer", key}});
  Json writer_stats = Json::object();
  if (enabled.contains("jscall")) trace.each("call-stats", [&](const Json& row) {
    try {
      auto record = row.at("record"), event = row.at("event");
      need(str(record, "protocol") == "v8trace/call-stats/1" && safe_uint(field(record, "producerThreadId"), true) && safe_uint(field(record, "epoch"), true) && field(record, "final").is_boolean(), "Invalid JSCall producer statistics");
      for (auto name : {"attemptedCalls", "selectedCalls", "filteredCalls", "emittedEvents"}) need(safe_uint(field(record, name)), "Invalid JSCall producer counter");
      need(std::regex_match(str(record, "producerId"), std::regex("[1-9][0-9]*")), "JSCall producer instance missing");
      auto key = text(field(event, "pid")) + '/' + text(record.at("producerThreadId")) + "/instance:" + str(record, "producerId");
      need(!writer_stats.contains(key), "Duplicate JSCall producer statistics");
      writer_stats[key] = record;
      if (record.at("final") != true) store.gap("jscall-writer-nonfinal", {{"producer", key}});
      if (record.at("attemptedCalls").get<uint64_t>() != record.at("selectedCalls").get<uint64_t>() + record.at("filteredCalls").get<uint64_t>() || record.at("emittedEvents") != call_events[key] || record.at("selectedCalls") != call_entries[key]) store.gap("jscall-writer-count-mismatch", {{"producer", key}, {"stats", record}, {"decodedEvents", call_events[key]}, {"decodedEntries", call_entries[key]}});
    } catch (const std::exception& e) { store.gap("jscall-writer-stats-invalid", {{"error", e.what()}}); }
  });
  if (enabled.contains("jscall") && (compact || str(field(o, "detailBackend"), "callStatsProtocol") == "v8trace/call-stats/1")) {
    for (const auto& [key, count] : call_events) if (count && !writer_stats.contains(key)) store.gap("jscall-writer-stats-missing", {{"producer", key}});
    if (writer_stats.empty()) store.gap("jscall-final-statistics-unavailable");
    store.json("jscall-writer-summary.json", {{"protocol", "v8trace/call-stats/1"}, {"producers", writer_stats}, {"decodedEvents", call_events}, {"decodedEntries", call_entries}, {"silentlyDropsEvents", false}});
  }
  for (auto& channel : enabled) if (channel != "wasm" && counts[channel] == 0) { bool zero = !filter_producers.empty(), filtered = false; for (auto& record : filter_producers) { zero = zero && field(record, channel + "Accepted", 0) == 0; filtered = filtered || field(record, channel + "Filtered", 0) > 0; } store.gap(zero && filtered ? "detail-filter-zero-matches" : "detail-probed-channel-stream-missing", {{"channel", channel}}, !(zero && filtered)); }
  if (!active.empty()) store.gap("detail-active-calls-at-capture-boundary", {{"count", active.size()}, {"calls", active}}, false);
  store.json("detail-summary.json", {{"protocol", "v8trace/detail/1"}, {"callStorageProtocol", compact ? "v8trace/call/2" : "legacy-json"}, {"jsCallValues", field(o, "jsCallValues", true)}, {"selected", enabled}, {"counts", counts}, {"malformed", malformed}, {"producers", producers.size()}, {"activeCallsAtStop", active.size()}, {"normalizationOrder", "producer-local sequence; original native timestamps retained without invented cross-thread order"}, {"objectRepresentation", "own-storage observations; per-node boundaries retained; no getter or proxy invocation"}, {"wasmExecutionMode", "DrumBrake with merged instructions and register optimization disabled"}});
  if (field(o, "closureBindings", false) == true) store.json("closure-summary.json", {{"protocol", "v8trace/closure/1"}, {"snapshots", closure_snapshots}, {"bindings", closure_bindings}, {"boundaries", closure_boundaries}, {"selectedCallScope", field(o, "detailFilter")}, {"scope", "context-allocated-lexical-chain-not-complete-local-state"}, {"generatorPolicy", "existing-activation-segments"}, {"valuesCachedAcrossCalls", false}});
}
void import_trace(Trace& trace, Store& store, const Json& o) {
  if (field(o, "opcode", false) == true) {
    std::map<std::string, Sequence> producers; uint64_t entries = 0, exits = 0, malformed = 0;
    trace.each("opcode", [&](const Json& row) { try { auto record = row.at("record"), event = row.at("event"); auto phase = str(record, "phase"); need(str(record, "schema") == "v8trace/opcode/1" && str(record, "engine") == "Ignition" && (phase == "entry" || phase == "exit") && safe_uint(field(record, "producerSeq"), true) && safe_uint(field(record, "dispatchOffset")) && safe_uint(field(record, "instructionOffset")) && field(record, "opcode").is_string() && field(record, "isolateId").is_string() && str(event, "cat").find("disabled-by-default-v8.opcode") != std::string::npos, "Malformed Opcode execution"); auto bytes = field(record, "bytes"); need(bytes.is_array() && !bytes.empty(), "Opcode bytes missing"); for (auto& b : bytes) need(safe_uint(b) && b <= 255, "Invalid Opcode byte"); coordinates(event, record); sequence(store, producers, producer(event, record, false), record.at("producerSeq").get<uint64_t>(), "opcode"); record["diagnostic"] = str(record, "scriptUrl").starts_with("v8trace://"); if (store.event("opcode", "instruction-" + phase, record, scope(event, record, "native-ignition-execution"))) { if (phase == "entry") ++entries; else ++exits; } } catch (const std::exception& e) { ++malformed; store.gap("opcode-invalid-native-record", {{"error", e.what()}}); } });
    if (!entries) store.gap("opcode-execution-stream-missing", {{"protocol", "v8trace/opcode/1"}}); store.json("opcode-summary.json", {{"protocol", "v8trace/opcode/1"}, {"entries", entries}, {"exits", exits}, {"malformed", malformed}, {"producers", producers.size()}, {"fidelity", "native-ignition-execution"}, {"heapReferenceIdentity", "address-at-observation"}});
  }
  import_causality(trace, store, o);
  import_detail(trace, store, o);
  import_environment(trace, store, o);
  import_state(trace, store, o);
  if (str(o, "transport") == "native" || field(o, "redirectBodies", false) == true) import_browser(trace, store, o);
  if (field(o, "nativeTimeline", false) == true) trace.each("generic", [&](const Json& row) { auto event = row.at("event"); auto cat = str(event, "cat"); if (cat.find("v8") != cat.npos || cat.find("blink.bindings") != cat.npos || cat.find("loading") != cat.npos || cat.find("devtools.timeline") != cat.npos) store.event("native", "performance-event", event, {{"fidelity", "performance-events"}}); });
}
}
