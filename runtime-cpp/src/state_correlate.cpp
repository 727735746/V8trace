#include "v8trace/state.h"
#include "v8trace/index.h"

namespace v8trace {
Json correlate_state(Store& store) {
  store.flush(); DiskIndex index(store.root);
  index.exec("CREATE TABLE calls(k BLOB NOT NULL,node BLOB NOT NULL); CREATE INDEX calls_key ON calls(k); CREATE TABLE operations(k BLOB PRIMARY KEY,node BLOB NOT NULL); CREATE TABLE entities(k BLOB PRIMARY KEY,node BLOB NOT NULL)");
  for (auto channel : {"jscall", "native"}) lines(store.root / "events" / (std::string(channel) + ".jsonl"), [&](const Json& event) {
    auto payload = field(event, "payload"); auto phase = str(payload, "phase");
    if (phase != "entry" && phase != "resume") return;
    auto call = str(payload, "callId"), isolate = str(payload, "isolateId");
    if (call.empty() || isolate.empty() || !payload.contains("processId") || !payload.contains("threadId")) return;
    auto key = text(field(payload, "recordingWindow", 0)) + '/' + std::string(channel) + '/' + text(payload.at("processId")) + '/' + text(payload.at("threadId")) + '/' + isolate + '/';
    if (std::string_view(channel) == "jscall") { if (str(payload, "producerId").empty()) return; key += str(payload, "producerId") + '/'; }
    key += call;
    Json node{{"category", channel}, {"eventSeq", field(event, "seq")}, {"callId", call}, {"functionName", field(payload, "functionName")}, {"source", field(payload, "source")}, {"processId", payload.at("processId")}, {"threadId", payload.at("threadId")}, {"isolateId", isolate}};
    index.run("INSERT INTO calls VALUES(?,?)", Json::array({key, dump(node)}));
  });
  auto scope = [](const Json& record) { return text(field(record, "recordingWindow", 0)) + '/' + text(field(record, "processId")) + '/' + str(record, "producerFamily", "blink") + '/' + str(record, "epoch"); };
  auto identities = [&](const Json& record, const std::function<void(const std::string&, const std::string&)>& visit) {
    std::function<void(const Json&)> walk = [&](const Json& value) {
      if (value.is_object()) for (auto it = value.begin(); it != value.end(); ++it) {
        if (it.key() == "requestId" || it.key() == "connectionId" || it.key() == "transactionId" || it.key() == "cursorId" || it.key() == "valueId" || it.key()=="moduleId" || it.key()=="compilationId") {
          auto id = it.value().is_string() ? text(it.value()) : ""; if (!id.empty() && ((it.key()=="compilationId"&&id.find_first_not_of("0123456789")==id.npos)||id!="0")&&id!="-1") visit(it.key(), id);
        } else walk(it.value());
      } else if (value.is_array()) for (const auto& item : value) walk(item);
    }; walk(record);
  };
  lines(store.root / "catalog/state-operations.jsonl", [&](const Json& record) {
    auto key = scope(record) + '/' + str(record, "operationId");
    Json node{{"operationId", record.at("operationId")}, {"kind", field(record, "kind")}, {"channel", field(record, "channel")}, {"scope", scope(record)}};
    index.run("INSERT OR IGNORE INTO operations VALUES(?,?)", Json::array({key, dump(node)}));
    identities(record, [&](const std::string& type, const std::string& id) {
      auto entity_key = scope(record) + '/' + str(record, "channel") + '/' + type + '/' + id;
      Json entity{{"entityType", type}, {"entityId", id}, {"scope", scope(record)}, {"channel", field(record, "channel")}, {"definition", "first-observed-native-entity-not-inferred-creation"}};
      index.run("INSERT OR IGNORE INTO entities VALUES(?,?)", Json::array({entity_key, dump(entity)}));
    });
  });
  uint64_t observed = 0, unresolved = 0;
  auto emit = [&](Json edge) { if (str(edge, "status") == "observed") ++observed; else ++unresolved; store.append("catalog/state-correlations.jsonl", edge); };
  lines(store.root / "catalog/state-operations.jsonl", [&](const Json& record) {
    auto origin = scope(record) + '/' + str(record, "operationId"); auto identity = field(record, "identity"); bool claimed_call = false;
    for (auto [field_name, category] : std::vector<std::pair<std::string, std::string>>{{"jsCallId", "jscall"}, {"nativeCallId", "native"}}) {
      auto call = str(identity, field_name); if (call.empty() || call == "0") continue; claimed_call = true;
      auto key = text(field(record, "recordingWindow", 0)) + '/' + category + '/' + text(field(record, "processId")) + '/' + text(field(record, "threadId")) + '/' + str(identity, "isolateId") + '/';
      if (category == "jscall") key += str(identity, "callProducerId") + '/'; key += call;
      auto count = *index.one("SELECT COUNT(*) FROM calls WHERE k=?", Json::array({key})); auto total = count.at(0).get<uint64_t>();
      Json edge{{"protocol", "v8trace/state-correlation/1"}, {"from", origin}, {"relation", "observed-in-" + category}, {"sharedKey", key}, {"status", total == 1 ? "observed" : "unresolved"}, {"basis", "actual-runtime-shared-call-id"}, {"candidates", total}, {"usesTimestampProximity", false}};
      if (total == 1) edge["to"] = parse(text(index.one("SELECT node FROM calls WHERE k=?", Json::array({key}))->at(0)));
      else edge["reason"] = total ? "shared-id-not-unique-in-input" : "selected-call-record-unavailable";
      emit(std::move(edge));
    }
    if (!claimed_call) emit({{"protocol", "v8trace/state-correlation/1"}, {"from", origin}, {"relation", "call-origin"}, {"status", "unresolved"}, {"reason", "no-active-selected-call-identity"}, {"usesTimestampProximity", false}});
    for (auto field_name : {"parentOperationId", "originOperationId"}) {
      auto parent = str(record, field_name); if (parent.empty() || parent == "0") continue;
      auto node = index.one("SELECT node FROM operations WHERE k=?", Json::array({scope(record) + '/' + parent}));
      Json edge{{"protocol", "v8trace/state-correlation/1"}, {"from", origin}, {"relation", field_name}, {"status", node ? "observed" : "unresolved"}, {"basis", "native-scoped-operation-or-carried-message-id"}, {"usesTimestampProximity", false}};
      if (node) edge["to"] = parse(text(node->at(0))); else { edge["targetId"] = parent; edge["reason"] = "origin-outside-observed-window-or-missing"; }
      emit(std::move(edge));
    }
    identities(record, [&](const std::string& type, const std::string& id) {
      auto node = index.one("SELECT node FROM entities WHERE k=?", Json::array({scope(record) + '/' + str(record, "channel") + '/' + type + '/' + id}));
      emit({{"protocol", "v8trace/state-correlation/1"}, {"from", origin}, {"relation", "observed-entity-membership"}, {"status", node ? "observed" : "unresolved"}, {"basis", "actual-native-entity-id-not-an-inferred-cause"}, {"to", node ? parse(text(node->at(0))) : Json(nullptr)}, {"usesTimestampProximity", false}});
    });
  });
  Json result{{"protocol", "v8trace/state-correlation/1"}, {"observed", observed}, {"unresolved", unresolved}, {"diskIndex", index.stats()}, {"usesTimestampProximity", false}};
  store.json("state-correlation-summary.json", result); return result;
}
}
