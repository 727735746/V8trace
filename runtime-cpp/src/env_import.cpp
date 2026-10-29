#include "v8trace/core.h"
#include "v8trace/index.h"
#include <regex>

namespace v8trace {
namespace {
void require_env(bool valid, std::string_view message) { if (!valid) throw std::runtime_error(std::string(message)); }
std::string env_producer(const Json& event, const Json& record, bool stats) {
  auto thread = field(stats ? record : event, stats ? "producerThreadId" : "tid");
  require_env(safe_uint(field(event, "pid")) && safe_uint(thread) && safe_uint(field(record, "epoch"), true) && std::regex_match(str(record, "producerId"), std::regex("[1-9][0-9]*")), "Invalid environment producer identity");
  return text(field(event, "v8traceWindow", 0)) + '/' + text(event.at("pid")) + '/' + text(thread) + '/' + str(record, "producerId") + '/' + text(record.at("epoch"));
}
}
void import_environment(Trace& trace, Store& store, const Json& options) {
  if (!env_selected(options)) return;
  DiskIndex database(store.root);
  database.exec("CREATE TABLE producers(k BLOB PRIMARY KEY,last INTEGER NOT NULL,events INTEGER NOT NULL,configured INTEGER NOT NULL,stats INTEGER NOT NULL,entries INTEGER NOT NULL); CREATE TABLE pending(k BLOB NOT NULL,id BLOB NOT NULL,kind BLOB NOT NULL,record BLOB NOT NULL,PRIMARY KEY(k,id))");
  uint64_t events = 0, inventories = 0, malformed = 0, statistics = 0, pending_at_stop = 0;
  Json counts = Json::object();
  trace.each("env-probe", [&](const Json& row) {
    auto record = row.at("record"), event = row.at("event");
    try {
      require_env(str(record, "protocol") == "v8trace/env/1" && field(record, "identityUnchanged") == true && field(record, "source").is_object() && safe_uint(field(record, "realmId")) && safe_uint(field(record, "gcEpoch")) && !str(record, "isolateId").empty(), "Malformed native environment envelope");
      require_env(str(event, "cat").find("disabled-by-default-v8.envprobe") != std::string::npos && field(event, "ts").is_number(), "Invalid native environment trace coordinates");
      auto key = env_producer(event, record, false), phase = str(record, "phase");
      auto sequence = field(record, "producerSeq"); require_env(safe_uint(sequence, true), "Invalid environment sequence");
      database.run("INSERT OR IGNORE INTO producers VALUES(?,0,0,0,0,0)", Json::array({key}));
      auto previous = *database.one("SELECT last,configured FROM producers WHERE k=?", Json::array({key}));
      auto last = previous.at(0).get<uint64_t>(); auto next = sequence.get<uint64_t>();
      if (next != last + 1) store.gap("env-producer-sequence-gap", {{"producer", key}, {"after", last}, {"before", next}});
      database.run("UPDATE producers SET last=?,events=events+1 WHERE k=?", Json::array({next, key}));
      if (phase == "configuration") {
        require_env(previous.at(1) == 0 && str(record, "engine") == "Ignition" && field(record, "invokesUserCode") == false && field(record, "propertyBudget") == field(options, "envInventoryProperties", 65536), "Invalid native environment configuration");
        for (auto [option, flag] : std::vector<std::pair<std::string, std::string>>{{"envAbsentProbe", "absent"}, {"envInterfaceProbe", "interfaces"}, {"envHostMutation", "mutations"}, {"envInventory", "inventory"}}) require_env(field(record, flag) == field(options, option, false), "Native environment selection differs from capture options");
        auto expected_filter = field(options, "envFilter", normalize_env_filter(options));
        require_env(str(record, "filter") == str(expected_filter, "token"), "Native environment source filter differs from capture options");
        database.run("UPDATE producers SET configured=1 WHERE k=?", Json::array({key}));
      } else {
        require_env(previous.at(1) == 1, "Environment configuration missing before observations");
        if (phase == "inventory") {
          require_env(field(options, "envInventory", false) == true && field(record, "interfaces").is_array() && field(record, "global").is_object() && field(record, "lazyAccessorsEvaluated") == false && field(record, "virtualPropertiesEnumerated") == false, "Malformed environment inventory");
          if (str(record, "coverage") != "declared-raw-storage") store.gap("env-inventory-explicit-budget", {{"producer", key}, {"realmId", record.at("realmId")}, {"coverage", field(record, "coverage")}, {"propertyBudget", field(record, "propertyBudget")}});
          store.append("catalog/env-inventory.jsonl", {{"processId", event.at("pid")}, {"threadId", event.at("tid")}, {"record", record}}); ++inventories;
        } else if (phase == "property-entry" || phase == "query-entry") {
          auto id = str(record, "operationId"); require_env(std::regex_match(id, std::regex("[1-9][0-9]*")) && record.contains("receiver") && record.contains("key") && !str(record, "operation").empty(), "Malformed environment operation entry");
          if (database.one("SELECT 1 AS present FROM pending WHERE k=? AND id=?", Json::array({key, id}))) store.gap("env-duplicate-operation-entry", {{"producer", key}, {"operationId", id}});
          database.run("INSERT OR REPLACE INTO pending VALUES(?,?,?,?)", Json::array({key, id, phase, dump(record)}));
          database.run("UPDATE producers SET entries=entries+1 WHERE k=?", Json::array({key}));
        } else if (phase == "property-exit" || phase == "property-interrupted" || phase == "query-exit") {
          auto id = str(record, "operationId"); auto entry = database.one("SELECT kind FROM pending WHERE k=? AND id=?", Json::array({key, id}));
          if (!entry) store.gap("env-operation-entry-missing", {{"producer", key}, {"record", record}});
          else require_env(text(entry->at(0)) == (phase.starts_with("property-") ? "property-entry" : "query-entry"), "Environment operation kind mismatch");
          if (phase == "property-exit") require_env(record.contains("result") || record.contains("accumulatorAfter"), "Environment result/accumulator observation missing");
          if (phase == "property-interrupted") store.gap("env-operation-interrupted", {{"producer", key}, {"record", record}}, false);
          database.run("DELETE FROM pending WHERE k=? AND id=?", Json::array({key, id}));
        } else throw std::runtime_error("Unknown native environment phase");
      }
      record["processId"] = event.at("pid"); record["threadId"] = event.at("tid"); record["traceTimestampMicroseconds"] = event.at("ts");
      if (store.event("env", "native-" + phase, record, {{"realmId", record.at("realmId")}, {"isolateId", record.at("isolateId")}, {"processId", event.at("pid")}, {"threadId", event.at("tid")}, {"fidelity", "native-host-property-observation"}})) { ++events; counts[phase] = field(counts, phase, 0).get<uint64_t>() + 1; }
    } catch (const std::exception& error) { ++malformed; store.gap("env-invalid-native-record", {{"error", error.what()}, {"event", event}}); }
  });
  trace.each("env-stats", [&](const Json& row) {
    auto record = row.at("record"), event = row.at("event");
    try {
      auto key = env_producer(event, record, true);
      require_env(str(record, "protocol") == "v8trace/env-stats/1" && field(record, "final").is_boolean(), "Malformed native environment statistics");
      for (auto name : {"attempted", "selected", "filtered", "emittedEvents"}) require_env(safe_uint(field(record, name)), "Invalid environment producer counter");
      require_env(record.at("attempted") >= record.at("selected") && record.at("attempted").get<uint64_t>() - record.at("selected").get<uint64_t>() == record.at("filtered").get<uint64_t>(), "Environment producer selection counters differ");
      auto producer = database.one("SELECT last,events,configured,stats,entries FROM producers WHERE k=?", Json::array({key}));
      if (!producer) {
        require_env(record.at("emittedEvents") == 0, "All environment producer records missing");
        require_env(record.at("selected") == 0, "Selected environment operations missing entirely");
        database.run("INSERT INTO producers VALUES(?,0,0,0,1,0)", Json::array({key}));
      } else {
        require_env(producer->at(3) == 0, "Duplicate environment final statistics");
        if (producer->at(1) != record.at("emittedEvents") || producer->at(0) != record.at("emittedEvents")) store.gap("env-writer-counter-mismatch", {{"producer", key}, {"decoded", *producer}, {"writer", record}});
        if (producer->at(4) != record.at("selected")) store.gap("env-selected-operation-missing", {{"producer", key}, {"decodedEntries", producer->at(4)}, {"writer", record}});
        database.run("UPDATE producers SET stats=1 WHERE k=?", Json::array({key}));
      }
      if (record.at("final") != true) store.gap("env-writer-not-quiescent", {{"producer", key}, {"statistics", record}});
      store.append("native-env-producers.jsonl", {{"producer", key}, {"statistics", record}}); ++statistics;
    } catch (const std::exception& error) { ++malformed; store.gap("env-invalid-producer-statistics", {{"error", error.what()}, {"event", event}}); }
  });
  database.each("SELECT k,events FROM producers WHERE stats=0", Json::array(), [&](const Json& row) { store.gap("env-final-statistics-missing", row); });
  database.each("SELECT k,id,record FROM pending ORDER BY k,id", Json::array(), [&](const Json& row) { ++pending_at_stop; store.gap("env-operation-open-at-window-boundary", {{"producer", row.at(0)}, {"operationId", row.at(1)}, {"entry", parse(text(row.at(2)))}}, false); });
  if (!statistics) store.gap("env-observation-stream-unavailable");
  if (field(options, "envInventory", false) == true && !inventories) store.gap("env-inventory-stream-unavailable");
  store.json("native-env-summary.json", {{"protocol", "v8trace/env/1"}, {"events", events}, {"inventories", inventories}, {"counts", counts}, {"malformed", malformed}, {"finalStatistics", statistics}, {"openOperations", pending_at_stop}, {"filter", field(options, "envFilter")}, {"diskIndex", database.stats()}, {"methodIdentityChanged", false}, {"executionPolicy", "Ignition; JS optimization disabled"}, {"inventoryScope", "raw named storage; no getter, Proxy, lazy constructor or virtual property evaluation"}, {"presenceScope", "actual native query result or raw chain before interpreted access; dynamic/indexed presence stays unknown"}, {"observationBoundaries", "super/enumerated bytecodes and unobserved host implementation state are not inferred"}});
}
}
