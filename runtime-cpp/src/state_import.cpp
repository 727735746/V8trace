#include "v8trace/state.h"
#include "v8trace/index.h"
#include "v8trace/vmp_probe.h"
#include <array>
#include <regex>

namespace v8trace {
namespace {
void state_require(bool valid, std::string_view message) { if (!valid) throw std::runtime_error(std::string(message)); }
uint64_t number(const Json& value) { if (value.is_string()) return decimal(text(value)); state_require(safe_uint(value), "Invalid native state integer"); return value.get<uint64_t>(); }
std::string family(const Json& record) { auto value = str(record, "producerFamily", "blink"); state_require(value == "blink" || value == "v8-aux", "Unknown state producer family"); return value; }
std::string session_key(const Json& event, const Json& record) {
  state_require(safe_uint(field(event, "pid")) && number(field(record, "epoch")) > 0, "Invalid native state session identity");
  return text(field(event, "v8traceWindow", 0)) + '/' + text(event.at("pid")) + '/' + family(record) + '/' + str(record, "epoch");
}
std::string producer_key(const Json& event, const Json& record, bool stats = false) {
  auto thread = stats ? field(record, "producerThreadId") : field(event, "tid");
  state_require(number(thread) > 0 && number(field(record, "producerId")) > 0, "Invalid native state producer identity");
  return session_key(event, record) + '/' + std::to_string(number(thread)) + '/' + str(record, "producerId");
}
class ValueSpool {
 public:
  ValueSpool(Store& store, DiskIndex& index, uint64_t maximum) : store_(store), index_(index), root_(fs::canonical(store.root)), directory_(root_ / ("state-spool-" + uuid())), maximum_(maximum) {
    if (!fs::create_directory(directory_)) throw std::runtime_error("Create native state spool failed");
    data_.open(directory_ / "parts.bin", std::ios::binary | std::ios::trunc); if (!data_) { std::error_code error; fs::remove_all(directory_, error); throw std::runtime_error("Create native state byte spool failed"); }
    try { index_.exec("CREATE TABLE parts(k BLOB NOT NULL,position INTEGER NOT NULL,physical INTEGER NOT NULL,bytes INTEGER NOT NULL,digest BLOB NOT NULL,PRIMARY KEY(k,position)); CREATE TABLE values_cache(k BLOB NOT NULL,expected INTEGER NOT NULL,record BLOB NOT NULL,PRIMARY KEY(k,expected))"); }
    catch (...) { data_.close(); std::error_code error; fs::remove_all(directory_, error); throw; }
  }
  ~ValueSpool() { data_.close(); std::error_code error; if (directory_.parent_path() == root_ && utf8(directory_.filename()).starts_with("state-spool-")) fs::remove_all(directory_, error); }
  void chunk(const std::string& key, const Json& record, const Json& event) {
    auto bytes = unbase64(str(record, "base64")); auto position = number(field(record, "offset"));
    state_require(!bytes.empty() && bytes.size() <= 32768 && position <= maximum_ && bytes.size() <= maximum_ - position, "Native state chunk exceeds explicit capture budget");
    if (bytes.size() > maximum_ - written_) throw std::runtime_error("Native state aggregate byte spool exceeds capture budget");
    auto previous = index_.one("SELECT digest FROM parts WHERE k=? AND position=?", Json::array({key, position}));
    if (previous) { store_.gap("state-duplicate-value-chunk", {{"content", key}, {"offset", position}, {"differentBytes", text(previous->at(0)) != sha256(bytes)}, {"event", event}}); return; }
    data_.write(bytes.data(), std::streamsize(bytes.size())); if (!data_) throw std::runtime_error("Write native state byte spool failed");
    index_.run("INSERT INTO parts VALUES(?,?,?,?,?)", Json::array({key, position, written_, bytes.size(), sha256(bytes)})); written_ += bytes.size(); ++chunks_;
    store_.append("catalog/state-chunks.jsonl", {{"content", key}, {"offset", position}, {"bytes", bytes.size()}, {"sha256", sha256(bytes)}, {"processId", event.at("pid")}, {"threadId", event.at("tid")}, {"timestamp", field(event, "ts")}});
  }
  void seal() { data_.flush(); if (!data_) throw std::runtime_error("Flush native state spool failed"); data_.close(); }
  Json resolve(const std::string& prefix, Json value) {
    if (value.is_array()) { for (auto& item : value) item = resolve(prefix, std::move(item)); return value; }
    if (!value.is_object()) return value;
    auto type = str(value, "type");
    if (type == "inline-bytes") {
      auto bytes = unbase64(str(value, "base64")); auto expected = number(field(value, "bytes"));
      state_require(bytes.size() == expected && expected <= 1024, "Invalid native inline byte length");
      if (str(value, "representation") == "utf16le") state_require(number(field(value, "utf16Length")) <= UINT64_MAX / 2 && number(value.at("utf16Length")) * 2 == expected && field(value, "null").is_boolean(), "Native inline UTF-16 length mismatch");
      inline_bytes_ += bytes.size(); return value;
    }
    if (type == "byte-reference") {
      auto content = str(value, "contentId"); state_require(!content.empty(), "Native state content reference missing");
      auto key = prefix + '/' + content; auto expected = number(field(value, "bytes"));
      state_require(expected <= maximum_, "Native state value exceeds selected capture budget");
      if (str(value, "representation") == "utf16le") state_require(number(field(value, "utf16Length")) <= UINT64_MAX / 2 && number(value.at("utf16Length")) * 2 == expected && field(value, "null").is_boolean(), "Native referenced UTF-16 length mismatch");
      auto cached = index_.one("SELECT record FROM values_cache WHERE k=? AND expected=?", Json::array({key, expected})); Json result;
      if (cached) result = parse(text(cached->at(0)));
      else {
        auto file = directory_ / "joined.bin"; std::ofstream joined(file, std::ios::binary | std::ios::trunc); std::ifstream source(directory_ / "parts.bin", std::ios::binary); if (!joined || !source) throw std::runtime_error("Open native state assembly failed");
        uint64_t next = 0; bool complete = true; std::array<char, 32768> buffer;
        index_.each("SELECT position,physical,bytes,digest FROM parts WHERE k=? ORDER BY position", Json::array({key}), [&](const Json& row) {
          auto position = row.at(0).get<uint64_t>(), physical = row.at(1).get<uint64_t>(), bytes = row.at(2).get<uint64_t>();
          if (position != next || bytes > expected - std::min(expected, next)) { complete = false; return; }
          source.clear(); source.seekg(std::streamoff(physical)); source.read(buffer.data(), std::streamsize(bytes)); if (source.gcount() != std::streamsize(bytes)) throw std::runtime_error("Read native state assembly failed");
          state_require(sha256(std::string_view(buffer.data(), size_t(bytes))) == text(row.at(3)), "Native state spool hash mismatch");
          joined.write(buffer.data(), std::streamsize(bytes)); if (!joined) throw std::runtime_error("Write native state assembly failed"); next += bytes;
        });
        joined.flush(); if (!joined) throw std::runtime_error("Flush native state assembly failed"); joined.close(); source.close(); complete = complete && next == expected;
        result = {{"blob", store_.blob_file(file, str(value, "representation", "native-state-bytes"))}, {"availableBytes", next}, {"coverage", complete ? "complete" : "partial"}};
        if (!complete) store_.gap("state-value-fragments-missing", {{"content", key}, {"expectedBytes", expected}, {"availableBytes", next}});
        index_.run("INSERT INTO values_cache VALUES(?,?,?)", Json::array({key, expected, dump(result)}));
      }
      value.update(result); ++references_; return value;
    }
    for (auto it = value.begin(); it != value.end(); ++it) it.value() = resolve(prefix, std::move(it.value()));
    return value;
  }
  Json statistics() const { return {{"chunks", chunks_}, {"spooledBytes", written_}, {"references", references_}, {"inlineBytes", inline_bytes_}, {"smallValuesRetainedInline", true}, {"cleanup", "owned-state-spool-directory-only"}}; }
 private:
  Store& store_;
  DiskIndex& index_;
  fs::path root_, directory_;
  std::ofstream data_;
  uint64_t maximum_, written_ = 0, chunks_ = 0, references_ = 0, inline_bytes_ = 0;
};
}
void import_state(Trace& trace, Store& store, const Json& options) {
  if (!state_selected(options)) return;
  auto selected = selected_state_channels(options); std::set<std::string> channels; for (const auto& channel : selected) channels.insert(text(channel));
  DiskIndex index(store.root); ValueSpool values(store, index, number(field(options, "maxBytes", 268435456)));
  index.exec("CREATE TABLE sessions(k BLOB PRIMARY KEY,started INTEGER NOT NULL,stopped INTEGER NOT NULL); CREATE TABLE producers(k BLOB PRIMARY KEY,s BLOB NOT NULL,last INTEGER NOT NULL,events INTEGER NOT NULL,stats INTEGER NOT NULL); CREATE TABLE records(seq INTEGER PRIMARY KEY,s BLOB NOT NULL,channel BLOB NOT NULL,event BLOB NOT NULL,data BLOB NOT NULL); CREATE TABLE activities(k BLOB PRIMARY KEY,s BLOB NOT NULL,kind BLOB NOT NULL,started INTEGER NOT NULL,ended INTEGER NOT NULL)");
  uint64_t malformed = 0, raw_events = 0, metadata_records = 0, final_stats = 0, sessions = 0, vmp_states = 0, unselected_sessions = 0, unselected_stats = 0; Json counts = Json::object(); std::set<std::string> observed_selection;
  index.exec("CREATE TABLE wasm_dedup(hash BLOB PRIMARY KEY,bytes INTEGER NOT NULL)");
  index.exec("CREATE TABLE vm_pairs(k BLOB PRIMARY KEY,entry BLOB,exit BLOB,exception BLOB)");
  auto family_channels = [&](const std::string& source) { std::set<std::string> result; for (const auto& channel : channels) if ((channel == "wasm-module" || channel == "vmp") == (source == "v8-aux")) result.insert(channel); return result; };
  trace.each("state-session", [&](const Json& row) {
    auto record = row.at("record"), event = row.at("event");
    try {
      state_require(str(record, "protocol") == "v8trace/state-session/1", "Native state session protocol mismatch"); auto key = session_key(event, record), phase = str(record, "phase");
      if (family_channels(family(record)).empty()) {
        state_require(phase == "stop" || (phase == "start" && field(record, "selected").is_array() && record.at("selected").empty()), "Unselected state family has invalid session selection or phase");
        state_require(!record.contains("selected") || (record.at("selected").is_array() && record.at("selected").empty()), "Unselected state family advertised selected channels");
        store.append("native-state-unselected-sessions.jsonl", {{"scope", key}, {"record", record}, {"processId", event.at("pid")}}); ++unselected_sessions; return;
      }
      index.run("INSERT OR IGNORE INTO sessions VALUES(?,0,0)", Json::array({key}));
      auto previous = *index.one("SELECT started,stopped FROM sessions WHERE k=?", Json::array({key}));
      if (phase == "start") {
        state_require(previous.at(0) == 0 && field(record, "selected").is_array(), "Duplicate/malformed state session start");
        std::set<std::string> native; for (const auto& item : record.at("selected")) native.insert(text(item)); state_require(native == family_channels(family(record)), "Native state session selection differs from options"); observed_selection.insert(native.begin(), native.end());
        if (native.contains("vmp")) state_require(str(record, "vmpConfigToken") == str(options.at("vmpProbeConfig"), "token") && field(record, "vmpConfigValid") == true, "Native confirmed VM descriptor is invalid or differs from options");
        if(native.contains("vmp"))for(auto [key,name]:std::vector<std::pair<std::string,std::string>>{{"vmpDepth","depth"},{"vmpNodes","nodes"},{"vmpProperties","properties"},{"vmpTypedArrayBytes","typedArrayBytes"},{"vmpMaxActive","maxActive"}})state_require(field(field(record,"vmpBudget"),name)==options.at(key),"Native VM observation budget differs from options");
        index.run("UPDATE sessions SET started=1 WHERE k=?", Json::array({key})); ++sessions;
      } else if (phase == "stop") { state_require(previous.at(1) == 0, "Duplicate state session stop"); index.run("UPDATE sessions SET stopped=1 WHERE k=?", Json::array({key})); }
      else throw std::runtime_error("Unknown native state session phase");
      store.append("native-state-sessions.jsonl", {{"scope", key}, {"record", record}, {"processId", event.at("pid")}});
    } catch (const std::exception& error) { ++malformed; store.gap("state-invalid-session", {{"error", error.what()}, {"event", event}}); }
  });
  trace.each("state", [&](const Json& row) {
    auto record = row.at("record"), event = row.at("event");
    try {
      auto channel = str(record, "channel"), kind = str(record, "kind");
      state_require(str(record, "protocol") == "v8trace/state/1" && channels.contains(channel) && !kind.empty() && field(event, "ts").is_number() && safe_uint(field(event, "tid")), "Malformed/unselected native state event");
      state_require(family_channels(family(record)).contains(channel), "State channel came from the wrong producer family");
      auto session = session_key(event, record), producer = producer_key(event, record); auto sequence = number(field(record, "producerSeq")); state_require(sequence > 0, "Invalid state sequence");
      index.run("INSERT OR IGNORE INTO producers VALUES(?,?,0,0,0)", Json::array({producer, session})); auto previous = *index.one("SELECT last FROM producers WHERE k=?", Json::array({producer}));
      if (sequence != previous.at(0).get<uint64_t>() + 1) store.gap("state-producer-sequence-gap", {{"producer", producer}, {"after", previous.at(0)}, {"before", sequence}});
      index.run("UPDATE producers SET last=?,events=events+1 WHERE k=?", Json::array({sequence, producer})); ++raw_events;
      if (kind == "value-chunk") { state_require(!str(record, "contentId").empty(), "State chunk content ID missing"); values.chunk(session + '/' + channel + '/' + str(record, "contentId"), record, event); }
      else {
        state_require(number(field(record, "operationId")) > 0, "Native state operation ID missing");
        if (record.contains("identity")) {
          const auto& identity = record.at("identity");
          state_require(str(identity, "protocol") == "v8trace/identity/1" && std::regex_match(str(identity, "isolateId"), std::regex("[a-f0-9]{1,16}")), "Invalid native shared identity protocol");
          for (auto key : {"realmId", "jsCallId", "nativeCallId", "callProducerId", "callEpoch"}) number(field(identity, key));
          if (number(identity.at("jsCallId"))) state_require(number(identity.at("callProducerId")) > 0 && number(identity.at("callEpoch")) > 0, "Selected shared JS call requires producer and epoch");
        }
        index.run("INSERT INTO records(s,channel,event,data) VALUES(?,?,?,?)", Json::array({session, channel, dump(event), dump(record)}));
      }
    } catch (const std::exception& error) { ++malformed; store.gap("state-invalid-native-record", {{"error", error.what()}, {"event", event}}); }
  });
  values.seal();
  index.each("SELECT seq,s,channel,event,data FROM records ORDER BY seq", Json::array(), [&](const Json& row) {
    auto record = parse(text(row.at(4))), event = parse(text(row.at(3))); auto session = text(row.at(1)), channel = text(row.at(2)), kind = str(record, "kind");
    try {
      record = values.resolve(session + '/' + channel, std::move(record));
      if (record.contains("v8ValueJSON")) record["exceptionValue"] = parse(str(record, "v8ValueJSON"));
      std::function<void(const Json&, std::string)> external = [&](const Json& value, std::string pointer) {
        if (value.is_object()) {
          if ((value.contains("externalBlobCount") && number(value.at("externalBlobCount")) > 0) || (value.contains("externalTokenCount") && number(value.at("externalTokenCount")) > 0)) store.gap("state-external-value-payload-unobserved", {{"scope", session}, {"operationId", record.at("operationId")}, {"pointer", pointer}, {"externalBlobCount", field(value, "externalBlobCount", "0")}, {"externalTokenCount", field(value, "externalTokenCount", "0")}});
          for (auto it = value.begin(); it != value.end(); ++it) external(it.value(), pointer + '/' + it.key());
        } else if (value.is_array()) for (size_t i = 0; i < value.size(); ++i) external(value[i], pointer + '/' + std::to_string(i));
      }; external(record, "");
      record["processId"] = event.at("pid"); record["threadId"] = event.at("tid"); record["traceTimestampMicroseconds"] = event.at("ts"); record["recordingWindow"] = field(event, "v8traceWindow", 0);
      if (channel == "wasm-module" && kind == "module-bytes") {
        state_require(record.contains("wire") && str(record.at("wire"), "coverage") == "complete", "WASM module byte reference incomplete");
        if(str(record,"byteCoverage").starts_with("prefix"))store.gap("wasm-module-stream-prefix-only",{{"record",record}});else state_require(str(record,"byteCoverage")=="complete-input"||str(record,"byteCoverage")=="complete-rejected-input-observation","Unknown WASM byte coverage");
        auto blob = record.at("wire").at("blob"); state_require(blob.is_object(), "WASM module blob budget exceeded");
        if (!index.one("SELECT 1 FROM wasm_dedup WHERE hash=?", Json::array({blob.at("sha256")}))) { index.run("INSERT INTO wasm_dedup VALUES(?,?)", Json::array({blob.at("sha256"), blob.at("bytes")})); store.append("catalog/wasm-module-index.jsonl", blob); }
        store.append("catalog/wasm-modules.jsonl", {{"processId", event.at("pid")}, {"recordingWindow", field(event, "v8traceWindow", 0)}, {"operationId", record.at("operationId")}, {"moduleId", field(record, "moduleId")}, {"observation", field(record, "observation")}, {"byteCoverage", field(record, "byteCoverage")}, {"urlBase64", field(record, "urlBase64")}, {"wire", blob}});
      }
      if (channel == "vmp") {
        auto validation = validate_vmp_record(record, options); if (field(validation, "boundary", false) == true) store.gap("vmp-observation-boundary", {{"validation", validation}, {"record", record}}, field(validation, "critical", true) == true);
        if (kind == "vm-state") {
          ++vmp_states;record["confirmedSemantics"]=validation.at("confirmedSemantics");auto phase=str(record,"phase"),key=session+"/vmp/"+str(record,"operationId");index.run("INSERT OR IGNORE INTO vm_pairs VALUES(?,NULL,NULL,NULL)",Json::array({key}));
          Json reference{{"processId",record.at("processId")},{"threadId",record.at("threadId")},{"recordingWindow",record.at("recordingWindow")},{"producerId",record.at("producerId")},{"producerSeq",record.at("producerSeq")},{"activationId",record.at("activationId")},{"operationId",record.at("operationId")},{"semantics",record.at("confirmedSemantics")}};
          auto column=phase=="entry"?"entry":phase=="exception"?"exception":"exit";index.run("UPDATE vm_pairs SET "+std::string(column)+"=? WHERE k=?",Json::array({dump(reference),key}));
        }
        store.append("catalog/vmp-probes.jsonl", {{"record", record}, {"validation", validation}, {"semantics", "explicit-user-confirmed-storage-selectors-not-inferred-opcodes"}});
      }
      auto activity = [&](std::string type, const Json& id, bool start, bool end) {
        auto value = str(Json{{"v", id}}, "v"); if (value.empty() || value == "0") return;
        auto key = session + '/' + type + '/' + value;
        index.run("INSERT OR IGNORE INTO activities VALUES(?,?,?,0,0)", Json::array({key, session, type}));
        index.run("UPDATE activities SET started=MAX(started,?),ended=MAX(ended,?) WHERE k=?", Json::array({start ? 1 : 0, end ? 1 : 0, key}));
      };
      if (kind == "api-entry" || kind == "api-exit") activity("api", record.at("operationId"), kind == "api-entry", kind == "api-exit");
      if (channel == "vmp" && kind == "vm-state") activity("vm-instruction", record.at("operationId"), str(record,"phase") == "entry", str(record,"phase") == "exit" || str(record,"phase") == "function-return" || str(record,"phase") == "exception");
      if (channel == "indexeddb" && record.contains("requestId")) activity("request", record.at("requestId"), kind == "request-start", kind == "request-success" || kind == "request-error" || kind == "request-abort" || kind == "request-context-destroyed");
      if (channel == "indexeddb" && record.contains("transactionId")) activity("transaction", record.at("transactionId"), kind == "transaction-start", kind == "transaction-complete" || kind == "transaction-abort");
      if (channel == "websocket" && record.contains("connectionId")) activity("websocket", record.at("connectionId"), kind == "connect-attempt", kind == "closed" || kind == "disconnect" || kind == "failure" || kind == "handshake-failure");
      store.append("catalog/state-operations.jsonl", record);
      if (store.event(channel, kind, record, {{"processId", event.at("pid")}, {"threadId", event.at("tid")}, {"executionContextToken", field(record, "executionContextToken")}, {"fidelity", "native-specialized-state"}})) { ++metadata_records; counts[channel] = field(counts, channel, 0).get<uint64_t>() + 1; }
    } catch (const std::exception& error) { ++malformed; store.gap("state-normalization-failed", {{"error", error.what()}, {"event", event}}); }
  });
  index.each("SELECT DISTINCT parts.k FROM parts LEFT JOIN values_cache ON parts.k=values_cache.k WHERE values_cache.k IS NULL", Json::array(), [&](const Json& row) { store.gap("state-orphan-value-chunks", {{"content", row.at(0)}, {"reason", "metadata-reference-unavailable"}}); });
  trace.each("state-stats", [&](const Json& row) {
    auto record = row.at("record"), event = row.at("event");
    try {
      state_require(str(record, "protocol") == "v8trace/state-stats/1" && field(record, "final").is_boolean(), "Native state final statistics missing protocol");
      auto key = producer_key(event, record, true); auto emitted = number(field(record, "emittedEvents")), attempted = number(field(record, "attempted")); state_require(attempted >= emitted, "Native state producer counters invalid");
      if (family_channels(family(record)).empty()) {
        state_require(!attempted && !emitted && record.at("final") == true, "Unselected state producer recorded activity");
        store.append("native-state-unselected-producers.jsonl", {{"scope", key}, {"record", record}}); ++unselected_stats; return;
      }
      if (!attempted && !emitted) index.run("INSERT OR IGNORE INTO producers VALUES(?,?,0,0,0)", Json::array({key, session_key(event, record)}));
      auto previous = index.one("SELECT last,events,stats FROM producers WHERE k=?", Json::array({key}));
      state_require(previous.has_value() && previous->at(2) == 0, "State statistics without producer or duplicate receipt");
      if (previous->at(0) != emitted || previous->at(1) != emitted || attempted != emitted || record.at("final") != true) store.gap("state-writer-counter-mismatch", {{"producer", key}, {"decoded", *previous}, {"writer", record}});
      index.run("UPDATE producers SET stats=1 WHERE k=?", Json::array({key})); store.append("native-state-producers.jsonl", {{"scope", key}, {"record", record}}); ++final_stats;
    } catch (const std::exception& error) { ++malformed; store.gap("state-invalid-final-statistics", {{"error", error.what()}, {"event", event}}); }
  });
  index.each("SELECT k,started,stopped FROM sessions WHERE started=0 OR stopped=0", Json::array(), [&](const Json& row) { store.gap("state-session-boundary-missing", {{"scope", row.at(0)}, {"started", row.at(1)}, {"stopped", row.at(2)}}); });
  index.each("SELECT k,s FROM producers WHERE stats=0", Json::array(), [&](const Json& row) { store.gap("state-final-statistics-missing", {{"producer", row.at(0)}}); });
  index.each("SELECT k,s FROM producers", Json::array(), [&](const Json& row) { if (!index.one("SELECT 1 FROM sessions WHERE k=? AND started=1 AND stopped=1", Json::array({row.at(1)}))) store.gap("state-producer-session-unverified", {{"producer", row.at(0)}, {"session", row.at(1)}}); });
  uint64_t boundaries = 0;
  index.each("SELECT k,kind,started,ended FROM activities WHERE started=0 OR ended=0", Json::array(), [&](const Json& row) { ++boundaries; store.gap("state-activity-window-boundary", {{"activity", row.at(0)}, {"kind", row.at(1)}, {"started", row.at(2)}, {"ended", row.at(3)}, {"reconstructedOutsideWindow", false}}, false); });
  if (!sessions) store.gap("state-session-stream-unavailable");
  for (const auto& channel : channels) if (!observed_selection.contains(channel)) store.gap("state-selected-channel-session-missing", {{"channel", channel}});
  if (field(options, "vmpProbe", false) == true && !vmp_states) store.gap("vmp-confirmed-site-not-observed", {{"specId", field(options.at("vmpProbeConfig"), "id")}});
  uint64_t vm_pairs=0,vm_paired=0;index.each("SELECT k,entry,exit,exception FROM vm_pairs ORDER BY k",Json::array(),[&](const Json& row){auto parse_ref=[](const Json& item){return item.is_null()?Json(nullptr):parse(text(item));};auto entry=parse_ref(row[1]),exit=parse_ref(row[2]),exception=parse_ref(row[3]);bool paired=!entry.is_null()&&(!exit.is_null()||!exception.is_null());store.append("catalog/vmp-instructions.jsonl",{{"scope",row[0]},{"entry",entry},{"exit",exit},{"exception",exception},{"conclusion",paired?"observed-boundary-pair":"window-or-descriptor-boundary"},{"algorithmRestored",false}});++vm_pairs;if(paired)++vm_paired;});
  if(field(options,"vmpProbe",false)==true)store.json("vmp-summary.json",{{"protocol","v8trace/vmp-probe/1"},{"specId",field(options.at("vmpProbeConfig"),"id")},{"stateSnapshots",vmp_states},{"instructionsObserved",vm_pairs},{"pairedBoundaries",vm_paired},{"algorithmRestored",false},{"sourceRewritten",false}});
  store.json("native-state-summary.json", {{"protocol", "v8trace/state/1"}, {"selected", selected}, {"rawEvents", raw_events}, {"metadataRecords", metadata_records}, {"counts", counts}, {"malformed", malformed}, {"sessions", sessions}, {"unselectedFamilySessionRecords", unselected_sessions}, {"unselectedFamilyEmptyReceipts", unselected_stats}, {"finalProducerReceipts", final_stats}, {"activityBoundaries", boundaries}, {"values", values.statistics()}, {"diskIndex", index.stats()}, {"methodIdentityChanged", false}, {"usesPageWrappers", false}, {"cdpRole", "drive-and-trace-transport-only; state-source-stays-native"}, {"causality", "only-actual-shared-native-operation-call-request-connection-transaction-identities"}, {"websocketScope", "renderer-decoded-payload-not-TLS-or-masked-wire-frames"}, {"cookieScope", "Document-and-CookieStore-API-and-native-store-changes-not-full-initial-inventory"}});
  correlate_state(store);
}
}
