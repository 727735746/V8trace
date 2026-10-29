#include "v8trace/core.h"
#include "v8trace/index.h"

namespace v8trace {
namespace {
std::string owner(const Json& row) { return dump(str(row, "sessionId").empty() ? Json::array({"native", field(row, "processId"), field(row, "isolateId")}) : Json::array({"cdp", field(row, "sessionId")})); }
std::string script_scope(const Json& row, const Json& id) { return owner(row) + '/' + text(id); }
std::string call_scope(const Json& row, const std::string& category) { return dump(Json::array({field(row, "processId"), field(row, "threadId"), field(row, "isolateId"), category, field(row, "callId")})); }
std::string thread_scope(const Json& row) { return dump(Json::array({field(row, "processId"), field(row, "threadId"), field(row, "isolateId"), field(row, "contextId")})); }
std::string resource_id(const Json& row) { return "resource:" + dump(Json::array({field(row, "sessionId"), field(row, "processId"), field(row, "requestId"), field(row, "hop", 0)})); }
Json decoded(const Json& row, size_t column = 0) { return parse(text(row.at(column))); }
Json identifiers(DiskIndex& index, std::string_view query, const Json& parameters) {
  Json output = Json::array(); index.each(query, parameters, [&](const Json& row) { output.push_back(row.at(0)); }); return output;
}
int64_t scope_id(DiskIndex& index, const std::string& scope) {
  index.run("INSERT OR IGNORE INTO scopes(scope) VALUES(?)", Json::array({scope}));
  return index.one("SELECT id FROM scopes WHERE scope=?", Json::array({scope}))->at(0).get<int64_t>();
}
}

void correlate(Store& store) {
  store.flush();
  for (auto name : {"calls", "script-index", "hook-spans", "correlations"}) write_file(store.filename("catalog/" + std::string(name) + ".jsonl"), "", true);
  DiskIndex index(store.root);
  index.exec("CREATE TABLE scripts(seq INTEGER PRIMARY KEY, scope BLOB, identity BLOB UNIQUE, body_key BLOB, data BLOB); CREATE INDEX script_scope ON scripts(scope);"
             "CREATE TABLE aliases(scope BLOB, alias BLOB, PRIMARY KEY(scope,alias)) WITHOUT ROWID;"
             "CREATE TABLE calls(id INTEGER PRIMARY KEY, activation BLOB UNIQUE, scope BLOB, data BLOB);"
             "CREATE TABLE active(scope BLOB PRIMARY KEY, call_id INTEGER) WITHOUT ROWID;"
             "CREATE TABLE scopes(id INTEGER PRIMARY KEY, scope BLOB UNIQUE);"
             "CREATE VIRTUAL TABLE intervals USING rtree(id,t0,t1,s0,s1);"
             "CREATE TABLE spans(identity BLOB PRIMARY KEY,data BLOB) WITHOUT ROWID;"
             "CREATE TABLE resources(seq INTEGER PRIMARY KEY,identity BLOB,request_key BLOB,body_key BLOB,content_key BLOB,network INTEGER,data BLOB);"
             "CREATE INDEX resource_request ON resources(request_key); CREATE INDEX resource_body ON resources(body_key); CREATE INDEX resource_content ON resources(content_key,network)");
  Json summary{{"schemaVersion", 1}, {"protocol", "v8trace/correlations/1"}, {"calls", 0}, {"scripts", 0}, {"edges", 0}, {"unresolved", 0}, {"ambiguous", 0}, {"completeCausality", false}, {"inputIntegrity", store.critical_gaps ? "partial" : "complete"}, {"budgetExhausted", false}, {"rules", Json::array({"Exact producer/session and script ID; ambiguous sources are not chosen", "Hook parents use explicit realm-local span IDs", "Synchronous enclosure is not an asynchronous causal parent", "Cross-layer HTTP content equality gives candidates, not shared identities"})}};
  auto increment = [&](const std::string& key) { summary[key] = summary.at(key).get<uint64_t>() + 1; };
  auto write = [&](std::string_view file, const Json& row) {
    if (summary["budgetExhausted"] == true) return false;
    if (store.append(file, row)) return true;
    summary["budgetExhausted"] = true; store.gap("correlation-output-budget", {{"file", file}}); return false;
  };
  auto edge = [&](const std::string& from, const std::string& relation, const Json& targets, const std::string& evidence, bool inferred, const std::string& reason) {
    std::string conclusion = targets.empty() ? "unresolved" : relation == "synchronous-call-enclosure" ? "observed-set" : targets.size() > 1 ? "ambiguous" : "matched";
    if (write("catalog/correlations.jsonl", {{"from", from}, {"relation", relation}, {"targets", targets}, {"conclusion", conclusion}, {"evidence", evidence}, {"inferred", inferred}, {"causalParentEstablished", false}, {"reason", reason.empty() ? Json(nullptr) : Json(reason)}})) {
      increment("edges"); if (conclusion == "unresolved") increment("unresolved"); if (conclusion == "ambiguous") increment("ambiguous");
    }
  };
  uint64_t ordinal = 0;
  lines(store.filename("catalog/scripts.jsonl"), [&](const Json& row) {
    ++ordinal;
    if (field(row, "isolateId").is_string() && row.contains("isolateAddress")) index.run("INSERT OR IGNORE INTO aliases VALUES(?,?)", Json::array({dump(Json::array({field(row, "processId"), row.at("isolateAddress")})), str(row, "isolateId")}));
    auto id = field(row, "scriptId"); if (id.is_null()) return;
    auto scope = script_scope(row, id);
    auto identity = "script:" + dump(Json::array({scope, field(field(row, "sourceUtf16"), "sha256", field(field(row, "source"), "sha256"))}));
    Json node{{"id", identity}, {"scope", scope}, {"sessionId", field(row, "sessionId")}, {"processId", field(row, "processId")}, {"isolateId", field(row, "isolateId")}, {"scriptId", id}, {"url", field(row, "url")}, {"sourceSha256", field(field(row, "source"), "sha256")}, {"sourceUtf16Sha256", field(field(row, "sourceUtf16"), "sha256")}, {"catalogRow", ordinal}};
    index.run("INSERT OR IGNORE INTO scripts(scope,identity,body_key,data) VALUES(?,?,?,?)", Json::array({scope, identity, dump(Json::array({field(node, "url"), field(node, "sourceSha256")})), node}));
    if (index.changes() && write("catalog/script-index.jsonl", node)) increment("scripts");
  });
  index.checkpoint();
  for (auto category : {"jscall", "native"}) {
    index.exec("DELETE FROM active");
    lines(store.filename("events/" + std::string(category) + ".jsonl"), [&](const Json& event) {
      auto row = field(event, "payload"); auto phase = str(row, "phase");
      if (str(row, "callId").empty() || str(row, "isolateId").empty()) return;
      auto key = call_scope(row, category);
      if (phase == "entry" || phase == "resume") {
        Json node{{"id", "call:" + key + '/' + text(event.at("seq"))}, {"callId", row.at("callId")}, {"category", category}, {"sessionId", field(event, "sessionId")}, {"processId", field(row, "processId")}, {"threadId", field(row, "threadId")}, {"isolateId", field(row, "isolateId")}, {"contextId", field(row, "contextId")}, {"name", field(row, "functionName", field(row, "name", field(row, "boundary")))}, {"boundary", field(row, "boundary")}, {"scriptId", field(row, "scriptId")}, {"functionStart", field(row, "functionStart")}, {"start", field(row, "traceTimestampMicroseconds")}, {"end", nullptr}, {"entryPhase", phase}, {"completion", "open-at-capture-boundary"}, {"valueEvents", Json::array({event.at("seq")})}, {"eventFile", "events/" + std::string(category) + ".jsonl"}};
        index.run("INSERT INTO calls(activation,scope,data) VALUES(?,?,?)", Json::array({str(node, "id"), thread_scope(node), node}));
        auto call_id = index.last_id(); index.run("INSERT OR REPLACE INTO active VALUES(?,?)", Json::array({key, call_id}));
        auto script_id = field(row, "scriptId");
        if (!script_id.is_null()) {
          row["sessionId"] = field(event, "sessionId");
          auto targets = identifiers(index, "SELECT identity FROM scripts WHERE scope=? ORDER BY seq", Json::array({script_scope(row, script_id)}));
          edge(str(node, "id"), "call-source", targets, "producer/session-and-script-id", false, targets.empty() ? "source-not-observed" : "");
        }
      } else if (phase == "argument" || phase == "return" || phase == "throw" || phase == "suspend") {
        auto found = index.one("SELECT c.id,c.data FROM active a JOIN calls c ON c.id=a.call_id WHERE a.scope=?", Json::array({key}));
        if (!found) return;
        auto node = decoded(*found, 1); node["valueEvents"].push_back(event.at("seq"));
        if (phase != "argument") { node["end"] = field(row, "traceTimestampMicroseconds"); node["completion"] = phase; index.run("DELETE FROM active WHERE scope=?", Json::array({key})); }
        index.run("UPDATE calls SET data=? WHERE id=?", Json::array({node, found->at(0)}));
      }
    });
    index.checkpoint();
  }
  index.each("SELECT id,data FROM calls ORDER BY id", Json::array(), [&](const Json& row) {
    auto node = decoded(row, 1); if (write("catalog/calls.jsonl", node)) increment("calls");
    if (field(node, "start").is_number() && field(node, "end").is_number() && node["end"] > node["start"] && !field(node, "contextId").is_null()) {
      auto scope = scope_id(index, thread_scope(node));
      index.run("INSERT INTO intervals VALUES(?,?,?,?,?)", Json::array({row.at(0), node.at("start").get<double>(), node.at("end").get<double>(), scope, scope}));
    }
  });
  index.checkpoint();
  lines(store.filename("events/env.jsonl"), [&](const Json& event) {
    auto row = field(event, "payload"); auto kind = str(event, "kind"); if (field(row, "spanId").is_null()) return;
    auto realm = str(event, "sessionId").empty() ? dump(Json::array({field(event, "processId"), field(event, "isolateAddress"), field(event, "realmId"), field(event, "contextId")})) : dump(Json::array({field(event, "sessionId"), field(event, "contextId")}));
    auto id = "hook:" + realm + '/' + text(row.at("spanId"));
    if (kind == "enter") {
      Json span{{"id", id}, {"realm", realm}, {"binding", field(row, "binding")}, {"parentSpanId", field(row, "parentSpanId")}, {"processId", field(event, "processId")}, {"threadId", field(event, "threadId")}, {"isolateId", field(event, "isolateId")}, {"contextId", field(event, "contextId")}, {"start", field(event, "traceTimestampMicroseconds")}, {"end", nullptr}, {"completion", nullptr}, {"valueEvents", Json::array({event.at("seq")})}, {"eventFile", "events/env.jsonl"}};
      if (span["isolateId"].is_null()) {
        auto aliases = identifiers(index, "SELECT alias FROM aliases WHERE scope=? ORDER BY alias LIMIT 2", Json::array({dump(Json::array({field(event, "processId"), field(event, "isolateAddress")}))}));
        if (aliases.size() == 1) span["isolateId"] = aliases.at(0);
      }
      index.run("INSERT OR REPLACE INTO spans VALUES(?,?)", Json::array({id, span}));
    } else if (kind == "return" || kind == "throw") {
      auto found = index.one("SELECT data FROM spans WHERE identity=?", Json::array({id})); if (!found) return;
      auto span = decoded(*found); span["end"] = field(event, "traceTimestampMicroseconds"); span["completion"] = kind; span["valueEvents"].push_back(event.at("seq"));
      index.run("UPDATE spans SET data=? WHERE identity=?", Json::array({span, id}));
    }
  });
  index.checkpoint();
  index.each("SELECT identity,data FROM spans ORDER BY identity", Json::array(), [&](const Json& row) {
    auto id = text(row.at(0)); auto span = decoded(row, 1); write("catalog/hook-spans.jsonl", span);
    auto parent = field(span, "parentSpanId");
    if (!parent.is_null()) {
      auto target = "hook:" + str(span, "realm") + '/' + text(parent);
      bool observed = index.one("SELECT 1 FROM spans WHERE identity=?", Json::array({target})).has_value();
      edge(id, "hook-parent", observed ? Json::array({target}) : Json::array(), "explicit-parentSpanId", false, observed ? "" : "reported-parent-not-observed");
    }
    Json enclosing = Json::array(); auto timestamp = field(span, "start");
    auto scope = index.one("SELECT id FROM scopes WHERE scope=?", Json::array({thread_scope(span)}));
    if (timestamp.is_number() && scope) {
      auto instant = timestamp.get<double>(); auto scope_number = scope->at(0).get<double>();
      // Inclusive float32 candidates followed by exact JSON comparisons preserve strict boundaries and int64 timestamps.
      index.each("SELECT c.data FROM intervals r CROSS JOIN calls c ON c.id=r.id WHERE r.t0<=? AND r.t1>=? AND r.s0<=? AND r.s1>=? AND c.scope=? ORDER BY c.id", Json::array({instant, instant, scope_number, scope_number, thread_scope(span)}), [&](const Json& match) {
        auto node = decoded(match); if (node["start"] < timestamp && node["end"] > timestamp) enclosing.push_back(node.at("id"));
      });
    }
    edge(id, "synchronous-call-enclosure", enclosing, "same-pid-thread-isolate-context-and-strict-interval", true, enclosing.empty() ? "no-closed-enclosing-activation" : "enclosing-activation-set-not-exclusive-causal-parent");
  });
  lines(store.filename("catalog/resources.jsonl"), [&](const Json& row) {
    auto body = field(row, "body"), request = field(row, "request"); Json body_key, content_key;
    if (!body.is_null()) body_key = dump(Json::array({field(request, "url"), field(body, "sha256")}));
    if (!body.is_null() && str(row, "bodyConclusion") == "complete" && str(row, "postBodyConclusion") == "complete" && (field(row, "postBody").is_object() || field(request, "hasPostData") == false)) content_key = dump(Json::array({field(request, "url"), field(request, "method"), field(field(row, "postBody"), "sha256", "no-post-body"), field(body, "sha256")}));
    index.run("INSERT INTO resources(identity,request_key,body_key,content_key,network,data) VALUES(?,?,?,?,?,?)", Json::array({resource_id(row), dump(Json::array({field(row, "sessionId"), field(row, "processId"), field(row, "requestId")})), body_key, content_key, str(row, "nativeLayer") == "network" ? 1 : 0, row}));
  });
  index.checkpoint();
  index.each("SELECT identity,body_key FROM scripts ORDER BY seq", Json::array(), [&](const Json& source) {
    auto matches = identifiers(index, "SELECT identity FROM resources WHERE body_key=? ORDER BY seq", Json::array({source.at(1)}));
    edge(text(source.at(0)), "source-http-content", matches, "equal-url-and-source-body-sha256", false, "content-equality-not-load-causality");
  });
  index.each("SELECT identity,content_key FROM resources WHERE content_key IS NOT NULL AND network=0 ORDER BY content_key,seq", Json::array(), [&](const Json& row) {
    auto matches = identifiers(index, "SELECT identity FROM resources WHERE content_key=? AND network=1 ORDER BY seq", Json::array({row.at(1)}));
    edge(text(row.at(0)), "cross-layer-request-candidate", matches, "equal-url-method-request-body-response-body-sha256", true, "no-shared-renderer-network-request-id");
  });
  lines(store.filename("events/http.jsonl"), [&](const Json& event) {
    auto kind = str(event, "kind"); if (kind != "request" && kind != "Network.requestWillBeSent") return;
    auto row = field(event, "payload"); bool native = str(event, "sessionId").empty();
    auto request = native ? Json(text(field(event, "processId")) + '/' + str(row, "requestId")) : field(row, "requestId");
    auto matches = identifiers(index, "SELECT identity FROM resources WHERE request_key=? ORDER BY seq", Json::array({dump(Json::array({field(event, "sessionId"), field(event, "processId"), request}))}));
    auto id = "request:" + dump(Json::array({field(event, "sessionId"), field(event, "processId"), field(row, "requestId"), event.at("seq")}));
    edge(id, "request-resource", matches, "producer/session-and-request-id", false, "resource-catalog-with-completion-and-body-references");
    if (native) edge(id, "request-hook-candidate", Json::array(), "native-scope", true, "no-shared-request-hook-id; async boundary unresolved");
  });
  summary["diskIndex"] = index.stats(); store.json("correlation-summary.json", summary);
}
}
