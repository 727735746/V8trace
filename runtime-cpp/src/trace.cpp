#include "v8trace/core.h"
#include <algorithm>
#include <array>
#include <queue>
#include <regex>

namespace v8trace {
namespace {
Json decoded(Json event, uint64_t index) {
  Json record;
  try { auto args = field(event, "args", Json::object()); auto raw = field(args, "record"); record = raw.is_string() ? parse(text(raw)) : raw; } catch (...) {}
  auto name = str(event, "name");
  if (name == "V8TraceFunction" || name == "V8TraceCall" || name == "V8TraceCallStats") record = field(event, "args", Json::object());
  std::string producer = text(field(event, "pid")) + '/' + text(field(event, "tid"));
  if ((name == "V8TraceDetail" && str(record, "channel") == "jscall") || name == "V8TraceFunction" || name == "V8TraceCall") if (record.contains("producerId")) producer += "/instance:" + str(record, "producerId");
  if (name == "V8TraceDetail") producer += '/' + str(record, "channel");
  if (name == "V8TraceFunction" || name == "V8TraceCall") producer += '/' + str(record, "isolateId") + '/' + str(record, "functionId");
  if (name == "V8TraceBrowser") producer += '/' + str(record, "producer") + '/' + str(record, "channel");
  if (name == "V8TraceEnv") producer += "/env/" + str(record, "producerId") + '/' + text(field(record, "epoch"));
  if (name == "V8TraceState" || name == "V8TraceAux") producer += "/state/" + str(record, "producerId") + '/' + str(record, "epoch");
  Json sequence = field(record, "producerSeq", 0); uint64_t seq = 0;
  try { if (sequence.is_string()) seq = decimal(text(sequence)); else if (safe_uint(sequence)) seq = sequence.get<uint64_t>(); } catch (...) {}
  return {{"event", std::move(event)}, {"record", std::move(record)}, {"index", index}, {"producer", producer}, {"sequence", seq}};
}
bool compare(const Json& a, const Json& b, std::string_view group) {
  auto aw = field(a.at("event"), "v8traceWindow", 0), bw = field(b.at("event"), "v8traceWindow", 0);
  if (aw != bw) return aw < bw;
  if (group == "browser") {
    auto at = field(a.at("event"), "ts", 0.0), bt = field(b.at("event"), "ts", 0.0);
    if (at != bt) return at < bt;
    // Cross-producer ties keep arrival order; inventing a process order implies causality.
    return a.at("index") < b.at("index");
  }
  if (a.at("producer") != b.at("producer")) return a.at("producer") < b.at("producer");
  if (a.at("sequence") != b.at("sequence")) return a.at("sequence") < b.at("sequence");
  return a.at("index") < b.at("index");
}
Json sort_projection(const Json& row) {
  const auto& event = row.at("event");
  return {{"event", {{"v8traceWindow", field(event, "v8traceWindow", 0)}, {"ts", field(event, "ts", 0.0)}}},
          {"producer", row.at("producer")}, {"sequence", row.at("sequence")}, {"index", row.at("index")}};
}
void sort_file(const fs::path& directory, const std::string& group) {
  auto source = directory / (group + ".jsonl"); if (!fs::exists(source)) return;
  std::vector<fs::path> runs; std::vector<Json> batch; uint64_t bytes = 0;
  auto save = [&] { if (batch.empty()) return; std::stable_sort(batch.begin(), batch.end(), [&](auto& a, auto& b) { return compare(a, b, group); }); auto name = directory / (group + "-run-" + std::to_string(runs.size())); std::ofstream out(name, std::ios::binary); if (!out) throw std::runtime_error("Sort run open failed"); for (auto& row : batch) out << dump(row) << '\n'; out.flush(); if (!out) throw std::runtime_error("Sort run write failed"); runs.push_back(name); batch.clear(); bytes = 0; };
  lines(source, [&](const Json& row) { bytes += dump(row).size(); batch.push_back(row); if (bytes >= 8 * 1024 * 1024) save(); }); save();
  if (runs.empty()) return;
  unsigned pass = 0;
  while (runs.size() > 1) {
    std::vector<fs::path> next;
    for (size_t start = 0; start < runs.size(); start += 16) {
      auto end = std::min(start + 16, runs.size()); std::vector<std::ifstream> inputs; std::vector<std::optional<Json>> heads;
      for (size_t i = start; i < end; ++i) { inputs.emplace_back(runs[i], std::ios::binary); if (!inputs.back()) throw std::runtime_error("Sort merge input failed"); std::string line; heads.push_back(std::getline(inputs.back(), line) ? std::optional<Json>(parse(line)) : std::nullopt); }
      auto target = directory / (group + "-pass-" + std::to_string(pass) + '-' + std::to_string(start)); std::ofstream out(target, std::ios::binary); if (!out) throw std::runtime_error("Sort merge output failed");
      while (true) { size_t best = heads.size(); for (size_t i = 0; i < heads.size(); ++i) if (heads[i] && (best == heads.size() || compare(*heads[i], *heads[best], group))) best = i; if (best == heads.size()) break; out << dump(*heads[best]) << '\n'; std::string line; if (std::getline(inputs[best], line)) heads[best] = parse(line); else { if (!inputs[best].eof()) throw std::runtime_error("Sort merge read failed"); heads[best].reset(); } }
      out.flush(); if (!out) throw std::runtime_error("Sort merge write failed"); inputs.clear(); for (size_t i = start; i < end; ++i) fs::remove(runs[i]); next.push_back(target);
    }
    runs = std::move(next); ++pass;
  }
  fs::remove(source); fs::rename(runs[0], source);
}
void expand_calls(const fs::path& directory) {
  if (!fs::exists(directory / "compact.jsonl")) return;
  std::ofstream output(directory / "detail.jsonl", std::ios::binary | std::ios::app);
  if (!output) throw std::runtime_error("Compact call decoder output open failed");
  std::string previous;
  Json metadata;
  bool conflicting = false;
  lines(directory / "compact.jsonl", [&](const Json& row) {
    const auto& event = row.at("event"); const auto& raw = row.at("record");
    auto key = str(row, "producer");
    if (key != previous) { previous = key; metadata = nullptr; conflicting = false; }
    if (str(event, "name") == "V8TraceFunction") {
      try {
        if (str(raw, "protocol") != "v8trace/function/1") throw std::runtime_error("Function dictionary protocol mismatch");
        auto value = parse(str(raw, "metadata"));
        if (!value.is_object() || !field(value, "functionName").is_string() || !field(value, "functionStart").is_number_integer() || !field(value, "scriptId").is_number_integer() || !field(value, "source").is_object()) throw std::runtime_error("Malformed function dictionary");
        if (!metadata.is_null() && value != metadata) conflicting = true;
        metadata = std::move(value);
      } catch (...) { conflicting = true; }
      return;
    }
    Json record = metadata.is_object() ? metadata : Json::object();
    std::string error;
    try {
      if (metadata.is_null() || conflicting) throw std::runtime_error(conflicting ? "Conflicting or malformed function dictionary" : "Function dictionary missing");
      if (str(raw, "protocol") != "v8trace/call/2" || !field(raw, "valuesCaptured").is_boolean()) throw std::runtime_error("Compact call protocol mismatch");
      if (raw.at("valuesCaptured") == true) {
        auto values = parse(str(raw, "values")); if (!values.is_object()) throw std::runtime_error("Malformed call values");
        constexpr std::array<std::string_view, 6> value_fields{"receiver", "arguments", "result", "generator", "accumulator", "boundary"};
        for (auto it = values.begin(); it != values.end(); ++it) if (std::find(value_fields.begin(), value_fields.end(), it.key()) == value_fields.end()) throw std::runtime_error("Reserved field in call value payload");
        record.update(values);
      } else if (raw.contains("values")) throw std::runtime_error("Values present in structure-only call");
      if (raw.contains("objectObservation")) record["objectObservation"] = parse(str(raw, "objectObservation"));
      if (raw.contains("closure")) record["closure"] = parse(str(raw, "closure"));
      if (raw.contains("causal")) {
        auto causal=raw.at("causal").is_string()?parse(text(raw.at("causal"))):raw.at("causal");
        if(!causal.is_object() || str(causal,"protocol")!="v8trace/causality/1") throw std::runtime_error("Invalid compact causal context");
        record["causal"]=std::move(causal);
      }
      if (!field(metadata, "functionValue").is_object() || !field(raw, "functionAddress").is_string() || !safe_uint(field(raw, "functionInstanceType"))) throw std::runtime_error("Function identity observation missing");
      record["function"] = metadata.at("functionValue");
      record["function"]["address"] = raw.at("functionAddress");
      record["function"]["instanceType"] = raw.at("functionInstanceType");
      record.erase("functionValue");
    } catch (const std::exception& e) { error = e.what(); }
    for (auto key_name : {"phase", "callId", "functionId", "frameAddress", "isolateId", "producerSeq", "producerId", "contextId", "valuesCaptured", "boundary", "argc", "parentCallId", "parentExact"}) if (raw.contains(key_name)) record[key_name] = raw.at(key_name);
    record["schema"] = "v8trace/detail/1"; record["channel"] = "jscall"; record["storageProtocol"] = "v8trace/call/2";
    if (!record.contains("contextId")) record["contextId"] = nullptr;
    if (!error.empty()) record["compactDecodeError"] = error;
    auto normalized = event; normalized["name"] = "V8TraceDetail"; normalized["args"] = {{"record", record}};
    output << dump(decoded(std::move(normalized), row.at("index").get<uint64_t>())) << '\n';
    if (!output) throw std::runtime_error("Compact call decoder write failed");
  });
  output.flush(); if (!output) throw std::runtime_error("Compact call decoder flush failed");
}
}
Trace::Trace(const fs::path& file, std::string array_key) {
  directory_ = file.parent_path() / ("decode-cpp-" + uuid()); if (!fs::create_directory(directory_)) throw std::runtime_error("Create decoder temporary directory failed");
  try {
    std::ifstream input(file, std::ios::binary); if (!input) throw std::runtime_error("Open trace failed"); JsonReader reader(input); reader.expect('{'); std::map<std::string, std::ofstream> outputs; std::set<std::string> keys; reader.white();
    std::map<std::string, Json> previous_keys; std::map<std::string, bool> ordered; std::map<std::string, uint64_t> raw_rows;
    if (reader.peek() != '}') while (true) {
      auto key = reader.value(); if (!key.is_string()) throw std::runtime_error("Trace root key must be string"); auto name = text(key); if (!keys.insert(name).second) throw std::runtime_error("Duplicate trace root key"); reader.expect(':');
      if (name == array_key) {
        has_array = true; reader.expect('['); reader.white();
        if (reader.peek() != ']') while (true) {
          auto event = reader.value(); auto event_name = str(event, "name"); std::string group = (event_name == "V8TraceState" || event_name == "V8TraceAux") ? "state" : (event_name == "V8TraceStateStats" || event_name == "V8TraceAuxStats") ? "state-stats" : (event_name == "V8TraceStateSession" || event_name == "V8TraceAuxSession") ? "state-session" : event_name == "V8TraceEnv" ? "env-probe" : event_name == "V8TraceEnvStats" ? "env-stats" : (event_name == "V8TraceCall" || event_name == "V8TraceFunction") ? "compact" : event_name == "V8TraceCallStats" ? "call-stats" : event_name == "V8TraceDetail" ? "detail" : event_name == "V8TraceCausality" ? "causality" : event_name == "V8TraceBrowser" ? "browser" : event_name == "V8TraceOpcode" ? "opcode" : event_name == "V8TraceFilter" ? "filter" : "generic";
          auto it = outputs.find(group); if (it == outputs.end()) it = outputs.emplace(group, std::ofstream(directory_ / (group + ".jsonl"), std::ios::binary)).first;
          auto row = decoded(std::move(event), event_count++); auto projection = sort_projection(row); auto previous = previous_keys.find(group);
          if (previous == previous_keys.end()) ordered[group] = true;
          else if (compare(projection, previous->second, group)) ordered[group] = false;
          previous_keys[group] = std::move(projection); ++raw_rows[group];
          it->second << dump(row) << '\n'; if (!it->second) throw std::runtime_error("Trace spool write failed");
          reader.white(); if (reader.peek() == ']') break; reader.expect(',');
        }
        reader.expect(']');
      } else properties[name] = reader.value();
      reader.white(); if (reader.peek() == '}') break; reader.expect(',');
    }
    reader.expect('}'); if (!reader.ended()) throw std::runtime_error("Trailing trace JSON");
    for (auto& [key, output] : outputs) { output.flush(); if (!output) throw std::runtime_error("Trace spool flush failed"); } outputs.clear();
    auto sort_group = [&](const std::string& group, bool force = false) {
      if (!fs::exists(directory_ / (group + ".jsonl"))) return;
      bool skip = !force && ordered[group];
      sort_statistics[group] = {{"rawRows", raw_rows[group]}, {"sourceOrderValidated", ordered[group]}, {"sortSkipped", skip}, {"expandedCallsRequireSort", force}};
      if (!skip) sort_file(directory_, group);
    };
    sort_group("compact"); bool compact = fs::exists(directory_ / "compact.jsonl"); expand_calls(directory_);
    for (auto group : {"detail", "browser", "causality", "opcode", "env-probe", "state"}) sort_group(group, group == std::string_view("detail") && compact);
  } catch (...) { fs::remove_all(directory_); throw; }
}
Trace::~Trace() { std::error_code error; fs::remove_all(directory_, error); }
void Trace::each(std::string_view group, const Sink& visit) const { lines(directory_ / (std::string(group) + ".jsonl"), visit); }
Json diagnostics(const Json& properties) {
  auto metadata = field(properties, "metadata", Json::object()); auto stats = field(metadata, "trace_processor_stats"); Json losses = Json::array(), global = Json::array();
  std::regex loss("^(data_loss_.*|.*packet_loss|packet_skipped_seq_needs_incremental_state_invalid|chunks_(discarded|overwritten)|bytes_overwritten|patches_failed|abi_violations|incremental_sequences_dropped|traced_(flushes_failed|final_flush_failed|invalid_packets)|traced_buf_(chunks_(discarded|overwritten)|bytes_overwritten|patches_failed|abi_violations)|json_(parser|tokenizer)_failure|track_event_(parser_errors|tokenizer_errors))$");
  std::function<void(const Json&, std::string, std::string)> visit = [&](const Json& value, std::string location, std::string name) {
    if (value.is_array()) { for (size_t i = 0; i < value.size(); ++i) visit(value[i], location + '.' + std::to_string(i), name); return; }
    if (value.is_object()) { for (auto it = value.begin(); it != value.end(); ++it) visit(it.value(), location.empty() ? it.key() : location + '.' + it.key(), it.key()); return; }
    if (!value.is_number() || value <= 0) return;
    if (name == "traced_chunks_discarded" || name == "traced_patches_discarded") global.push_back({{"stat", location}, {"value", value}, {"scope", "tracing-service-lifetime"}, {"attributedToThisRecordingWindow", false}});
    else if (std::regex_match(name, loss)) losses.push_back({{"stat", location}, {"value", value}});
  }; visit(stats, "", "");
  auto final = field(stats, "traced_final_flush_succeeded", 0);
  return {{"statsAvailable", stats.is_object()}, {"finalFlushSucceeded", safe_uint(final) && final.get<uint64_t>() > 0}, {"losses", losses}, {"globalCounters", global}};
}
}
