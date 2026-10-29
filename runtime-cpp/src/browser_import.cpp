#include "v8trace/core.h"
#include <algorithm>
#include <array>
#include <regex>
#include <zlib.h>
#include <brotli/decode.h>
#include <zstd.h>

namespace v8trace {
namespace {
void need(bool value, const char* error) { if (!value) throw std::runtime_error(error); }
std::string header(const Json& headers, std::string_view key) { if (headers.is_object()) for (auto it = headers.begin(); it != headers.end(); ++it) if (lower(it.key()) == key) return text(it.value()); return ""; }
std::string decode_body(std::string data, const std::string& encodings, uint64_t maximum) {
  auto types = split(encodings.empty() ? "identity" : encodings, ','); std::reverse(types.begin(), types.end());
  for (auto type : types) {
    type = lower(trim(type)); if (type == "identity") continue;
    std::string output; std::array<char, 65536> chunk{};
    auto append = [&](size_t length) { if (length > maximum - output.size()) throw std::runtime_error("Decoded HTTP body exceeds byte budget"); output.append(chunk.data(), length); };
    if (type == "gzip" || type == "deflate") {
      z_stream stream{}; if (inflateInit2(&stream, type == "gzip" ? 31 : 15) != Z_OK) throw std::runtime_error("HTTP inflater initialization failed"); struct End { z_stream& value; ~End() { inflateEnd(&value); } } end{stream};
      size_t at = 0; int code = Z_OK;
      do {
        if (!stream.avail_in && at < data.size()) { auto size = std::min<size_t>(data.size() - at, 65536); stream.next_in = reinterpret_cast<Bytef*>(data.data() + at); stream.avail_in = uInt(size); at += size; }
        stream.next_out = reinterpret_cast<Bytef*>(chunk.data()); stream.avail_out = uInt(chunk.size()); code = inflate(&stream, Z_NO_FLUSH); append(chunk.size() - stream.avail_out);
        if (code == Z_STREAM_END && type == "gzip" && (stream.avail_in || at < data.size())) { if (inflateReset(&stream) != Z_OK) throw std::runtime_error("Gzip member reset failed"); code = Z_OK; }
        else if (code != Z_OK && code != Z_STREAM_END) throw std::runtime_error("Invalid or unfinished compressed HTTP body");
        if (code == Z_OK && !stream.avail_in && at == data.size() && stream.avail_out) throw std::runtime_error("Unfinished compressed HTTP body");
      } while (code != Z_STREAM_END);
      if (stream.avail_in || at != data.size()) throw std::runtime_error("Trailing compressed HTTP bytes");
    } else if (type == "br") {
      auto state = BrotliDecoderCreateInstance(nullptr, nullptr, nullptr); if (!state) throw std::runtime_error("Brotli initialization failed"); struct End { BrotliDecoderState* value; ~End() { BrotliDecoderDestroyInstance(value); } } end{state}; const uint8_t* input = reinterpret_cast<const uint8_t*>(data.data()); size_t available = data.size();
      while (true) { size_t out_size = chunk.size(); auto out = reinterpret_cast<uint8_t*>(chunk.data()); auto code = BrotliDecoderDecompressStream(state, &available, &input, &out_size, &out, nullptr); append(chunk.size() - out_size); if (code == BROTLI_DECODER_RESULT_SUCCESS) { if (available) throw std::runtime_error("Trailing Brotli bytes"); break; } if (code != BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT) throw std::runtime_error("Invalid or unfinished Brotli body"); }
    } else if (type == "zstd") {
      auto state = ZSTD_createDStream(); if (!state) throw std::runtime_error("Zstd initialization failed"); struct End { ZSTD_DStream* value; ~End() { ZSTD_freeDStream(value); } } end{state}; ZSTD_initDStream(state); ZSTD_inBuffer input{data.data(), data.size(), 0}; size_t remaining = 1;
      while (input.pos < input.size || remaining) { ZSTD_outBuffer out{chunk.data(), chunk.size(), 0}; auto before = input.pos; remaining = ZSTD_decompressStream(state, &out, &input); if (ZSTD_isError(remaining)) throw std::runtime_error("Invalid Zstd HTTP body"); append(out.pos); if (before == input.pos && !out.pos && remaining) throw std::runtime_error("Unfinished Zstd HTTP body"); }
    } else throw std::runtime_error("Unsupported content encoding: " + type);
    data = std::move(output);
  }
  return data;
}
struct Sequence {
  uint64_t prefix = 0; std::set<uint64_t> pending;
  bool insert(uint64_t n) { if (n <= prefix || pending.contains(n)) return false; if (n == prefix + 1) { prefix = n; while (pending.erase(prefix + 1)) ++prefix; } else pending.insert(n); return true; }
};
struct Request {
  std::string key;
  Json scope, request, response, completion, redirect, redirect_drain, response_end, request_end, upload, reuse, actual_headers, redirect_capture, redirect_end, upload_attempts;
  std::vector<std::string> errors;
  std::optional<double> started, finished;
  uint64_t request_bytes = 0, response_bytes = 0;
  bool request_budget = false, response_budget = false;
  fs::path request_file, response_file;
};
class BodySpool {
 public:
  explicit BodySpool(Store& store) : root(store.filename("native-output/body-cpp-" + uuid())) { if (!fs::create_directory(root)) throw std::runtime_error("Create body spool failed"); }
  ~BodySpool() { handles.clear(); std::error_code error; fs::remove_all(root, error); }
  void append(fs::path& file, const std::string& key, std::string_view bytes) { if (file.empty()) file = root / sha256(key); auto name = utf8(file); auto it = handles.find(name); if (it == handles.end()) { if (handles.size() >= 32) handles.erase(handles.begin()); it = handles.emplace(name, std::ofstream(file, std::ios::binary | std::ios::app)).first; } it->second.write(bytes.data(), std::streamsize(bytes.size())); if (!it->second) throw std::runtime_error("HTTP body spool write failed"); }
  void flush() { for (auto& [name, handle] : handles) { handle.flush(); if (!handle) throw std::runtime_error("HTTP spool flush failed"); } handles.clear(); }
 private:
  fs::path root; std::map<std::string, std::ofstream> handles;
};
}
void import_browser(Trace& trace, Store& store, const Json& o) {
  const bool cdp = str(o, "transport") == "cdp"; std::set<std::string> enabled; Json counts = Json::object();
  for (auto [key, channel] : std::vector<std::pair<std::string, std::string>>{{"hooks", "env"}, {"http", "http"}, {"scripts", "script"}, {"consoleEvents", "console"}, {"exceptionEvents", "exception"}}) if (field(o, key, false) == true && (!cdp || (channel == "http" && field(o, "redirectBodies", false) == true))) { enabled.insert(channel); counts[channel] = 0; }
  if (enabled.empty()) return;
  std::map<std::string, Sequence> sequences, realm_sequences; std::map<std::string, std::pair<bool, bool>> realms;
  std::map<std::string, Request> requests; std::map<std::string, std::vector<std::string>> uploads; std::set<std::string> scripts, object_observers; uint64_t invalid = 0;
  BodySpool spool(store); auto maximum = o.at("maxBodyBytes").get<uint64_t>();
  auto bad = [&](Request& value, const std::string& reason, Json details = Json::object()) { value.errors.push_back(reason); details["requestId"] = value.key; store.gap(reason, std::move(details)); };
  trace.each("browser", [&](const Json& row) {
    auto record = row.at("record"), event = row.at("event");
    try {
      auto channel = str(record, "channel"), kind = str(record, "kind"); if (channel=="causality" || (cdp && channel != "http")) return;
      need(str(record, "schema") == "v8trace/browser/1" && enabled.contains(channel) && str(event, "cat").find("disabled-by-default-v8." + channel) != std::string::npos && (str(record, "producer") == "v8" || str(record, "producer") == "chromium") && safe_uint(field(event, "pid")) && safe_uint(field(event, "tid")) && field(event, "ts").is_number(), "Invalid native browser record");
      auto sequence = decimal(str(record, "producerSeq")); need(sequence > 0, "Invalid browser sequence");
      auto owner = text(event.at("pid")) + '/' + text(event.at("tid")); auto key = owner + '/' + str(record, "producer") + '/' + channel;
      if (!sequences[key].insert(sequence)) store.gap("browser-duplicate-sequence", {{"key", key}, {"sequence", str(record, "producerSeq")}});
      Json scope{{"processId", event.at("pid")}, {"threadId", event.at("tid")}, {"traceTimestampMicroseconds", event.at("ts")}, {"contextId", field(record, "contextId")}, {"isolateId", field(record, "isolateId")}, {"isolateAddress", field(record, "isolateAddress")}, {"realmId", field(record, "realmId")}, {"fidelity", "native-" + channel}, {"scopeConclusion", "native-scope-only"}};
      if (field(o, "objectSnapshots", false) == true && (channel == "console" || channel == "exception")) { auto config = field(record, "objectObservation"); need(str(config, "schema") == "v8trace/objects/1" && field(config, "enabled") == true && field(config, "depth") == o.at("objectDepth") && field(config, "nodes") == o.at("objectNodes") && field(config, "properties") == o.at("objectProperties") && field(config, "invokesGetters") == false && field(config, "invokesProxyTraps") == false, "Browser object observation config mismatch"); object_observers.insert(owner + '/' + channel); }
      if (channel == "console" && safe_uint(field(record, "typeCode"))) { static const std::vector<std::string> names{"log", "debug", "info", "error", "warn", "dir", "dirxml", "table", "trace", "startGroup", "startGroupCollapsed", "endGroup", "clear", "assert", "timeEnd", "count"}; auto code = record.at("typeCode").get<uint64_t>(); record["apiType"] = code < names.size() ? names[size_t(code)] : "unknown-" + std::to_string(code); }
      if (channel == "env") {
        if (kind == "hook-skipped") need(str(record, "reason") == "privileged-browser-realm" && field(record, "identityChanged") == false && std::regex_search(str(record, "url"), std::regex("^(chrome|chrome-untrusted|chrome-extension|devtools)://")), "Unexpected hook scope exclusion");
        else {
          auto realm = text(event.at("pid")) + '/' + text(field(record, "realmId")); auto& status = realms[realm];
          if (kind == "hook-install-failed") store.gap("env-native-install-failed", scope);
          if (kind == "hook-installed") status.first = true;
          if (kind == "hook") { auto hook = parse(str(record, "hookRecord")); need(safe_uint(field(hook, "producerSeq"), true) && field(hook, "kind").is_string(), "Invalid env hook payload"); if (!realm_sequences[realm].insert(hook.at("producerSeq").get<uint64_t>())) store.gap("env-duplicate-sequence", {{"realm", realm}}); auto hook_kind = str(hook, "kind"); if (hook_kind == "ready") status.second = true; if (hook_kind == "budget" || hook_kind == "gap" || hook_kind == "install-failed") store.gap("env-producer-gap", {{"scope", scope}, {"hook", hook}}); hook.update({{"url", field(record, "url")}, {"transport", "native"}, {"identityChanged", true}}); if (store.event("env", hook_kind, hook, scope)) counts["env"] = counts.at("env").get<uint64_t>() + 1; return; }
        }
      } else if (channel == "script") {
        need(field(record, "source").is_string() && field(record, "url").is_string(), "Native script source missing"); auto source = str(record, "source"), units = utf16le(source); auto identity = text(event.at("pid")) + '/' + text(field(record, "isolateAddress")) + '/' + text(field(record, "scriptId")) + '/' + sha256(units) + '/' + kind;
        if (scripts.insert(identity).second) { Json catalog = scope; for (auto name : {"scriptId", "url", "kind", "compilationKind", "isModule", "lineOffset", "columnOffset"}) catalog[name] = field(record, name); catalog["source"] = store.blob(replacement_utf8(source), "native-script-source-utf8"); catalog["sourceUtf16"] = store.blob(units, "native-script-source-utf16le-lossless"); catalog["sourceCodeUnits"] = units.size() / 2; store.append("catalog/scripts.jsonl", catalog); }
      } else if (channel == "http") {
        need(field(record, "requestId").is_string(), "HTTP request ID missing"); auto request_key = text(event.at("pid")) + '/' + str(record, "requestId"); auto& value = requests[request_key]; if (value.key.empty()) { value.key = request_key; value.scope = scope; }
        if (kind == "request") { value.request = record; value.started = event.at("ts").get<double>(); }
        else if (kind == "request-headers") value.actual_headers = field(record, "headers");
        else if (kind == "response") value.response = record;
        else if (kind == "redirect") value.redirect = record;
        else if (kind == "reuse-resource") value.reuse = record;
        else if (kind == "upload-init") { value.upload = record; value.started = event.at("ts").get<double>(); uploads[text(event.at("pid")) + '/' + str(record, "uploadStreamId")].push_back(request_key); }
        else if (kind == "redirect-body-start") { need(str(record, "bodyProtocol") == "v8trace/redirect-body/1" && field(record, "url").is_string() && str(record, "bodyRepresentation") == "encoded-network-bytes", "Invalid redirect body start"); if (!value.redirect_drain.is_null()) bad(value, "http-redirect-body-duplicate-start"); value.redirect_drain = record; }
        else if (kind == "body-chunk") {
          auto direction = str(record, "direction"); need(direction == "request" || direction == "response", "Invalid body direction"); auto bytes = unbase64(str(record, "base64")); auto offset = decimal(str(record, "offset")); need(safe_uint(field(record, "bytes")) && bytes.size() == record.at("bytes").get<uint64_t>(), "Body byte count mismatch");
          auto& count = direction == "request" ? value.request_bytes : value.response_bytes; auto& budget = direction == "request" ? value.request_budget : value.response_budget; auto& file = direction == "request" ? value.request_file : value.response_file;
          if (offset != count) bad(value, "http-body-offset-gap", {{"direction", direction}, {"expected", count}, {"actual", offset}}); need(offset <= UINT64_MAX - bytes.size(), "HTTP offset overflow"); count = offset + bytes.size();
          if (count > maximum) { if (!budget) bad(value, "http-body-budget", {{"direction", direction}, {"limit", maximum}}); budget = true; }
          else if (!budget) spool.append(file, request_key + '/' + direction, bytes);
        } else if (kind == "request-body-end") value.request_end = record;
        else if (kind == "response-body-end") { if (!value.redirect_drain.is_null()) { need(str(record, "bodyProtocol") == "v8trace/redirect-body/1" && field(record, "complete").is_boolean() && field(record, "errorCode").is_number_integer() && field(record, "reason").is_string(), "Invalid redirect body end"); decimal(str(record, "bytes")); if (!value.response_end.is_null()) bad(value, "http-redirect-body-duplicate-end"); } value.response_end = record; }
        else if (kind == "complete") { if (!value.completion.is_null()) bad(value, "http-duplicate-completion"); value.completion = record; value.finished = event.at("ts").get<double>(); }
        else if (kind == "body-tee-failed") bad(value, "http-native-tee-failed");
      }
      if (store.event(channel, kind, record, scope)) counts[channel] = counts.at(channel).get<uint64_t>() + 1;
    } catch (const std::exception& e) { ++invalid; store.gap("browser-normalization-error", {{"channel", field(record, "channel")}, {"kind", field(record, "kind")}, {"error", e.what()}}); }
  });
  spool.flush();
  for (auto& [key, state] : sequences) { uint64_t expected = state.prefix + 1; for (auto n : state.pending) { if (n != expected) store.gap("browser-producer-gap", {{"key", key}, {"expected", std::to_string(expected)}, {"actual", std::to_string(n)}}); expected = n + 1; } }
  for (auto& [key, value] : requests) if (!value.reuse.is_null() && !value.response_bytes) {
    std::vector<Request*> candidates; for (auto& [other_key, other] : requests) if (field(other.scope, "processId") == field(value.scope, "processId") && str(other.request, "layer") == "resource" && field(other.request, "resourceIdentifier") == field(value.reuse, "sourceIdentifier") && field(other.completion, "success") == true) candidates.push_back(&other);
    if (candidates.size() == 1) { auto& original = *candidates[0]; value.response_file = original.response_file; value.response_bytes = original.response_bytes; value.response_budget = original.response_budget; if (field(original.completion, "pipe") == true) value.response_end = original.response_end; if (value.completion.is_null()) value.completion = Json::object(); value.completion["bytes"] = std::to_string(original.response_bytes); value.completion["pipe"] = field(original.completion, "pipe"); }
  }
  std::map<std::string, std::string> attached;
  auto attach = [&](Request& value, const std::string& drain_key, bool callback) {
    if (auto found = attached.find(drain_key); found != attached.end()) {
      if (found->second != value.key) bad(value, "http-redirect-body-multiple-parents");
      return;
    }
    if (!value.redirect_capture.is_null()) { bad(value, "http-redirect-body-multiple-streams"); return; }
    auto found = requests.find(drain_key); Request* drain = found == requests.end() ? nullptr : &found->second;
    bool valid = drain && str(drain->redirect_drain, "bodyProtocol") == "v8trace/redirect-body/1"
        && field(drain->redirect_drain, "url") == field(value.request, "url")
        && str(drain->response_end, "bodyProtocol") == "v8trace/redirect-body/1";
    if (callback) valid = valid && str(value.redirect, "redirectBodyProtocol") == "v8trace/redirect-body/1";
    if (drain && (drain->redirect_drain.contains("parentRequestId") || drain->redirect_drain.contains("parentRequestProtocol")))
      valid = valid && str(drain->redirect_drain, "parentRequestProtocol") == "v8trace/http-parent/1"
          && str(drain->redirect_drain, "parentRequestId") == str(value.request, "requestId");
    if (drain) attached.emplace(drain_key, value.key);
    value.redirect_capture = {{"requestId", drain_key}, {"protocol", "v8trace/redirect-body/1"}, {"attachment", callback ? "redirect-callback" : "native-parent-identity"},
        {"complete", false}, {"abortedByRequest", false}, {"reason", valid ? field(drain->response_end, "reason") : Json("missing-or-mismatched-stream")},
        {"errorCode", drain ? field(drain->response_end, "errorCode") : Json(nullptr)}};
    bool aborted = false;
    if (valid) {
      value.response_file = drain->response_file; value.response_bytes = drain->response_bytes; value.response_budget = drain->response_budget;
      value.redirect_end = drain->response_end; value.errors.insert(value.errors.end(), drain->errors.begin(), drain->errors.end());
      bool bytes_valid = decimal(str(drain->response_end, "bytes")) == drain->response_bytes && drain->errors.empty() && !drain->response_budget;
      value.redirect_capture["complete"] = bytes_valid && field(drain->response_end, "complete") == true
          && field(drain->response_end, "errorCode") == 0 && str(drain->response_end, "reason") == "eof";
      aborted = bytes_valid && field(value.completion, "success") == false && field(value.completion, "errorCode").is_number_integer()
          && field(value.completion, "errorCode").get<int64_t>() < 0 && field(drain->response_end, "complete") == false
          && field(drain->response_end, "errorCode") == -3 && str(drain->response_end, "reason") == "cancelled";
      value.redirect_capture["abortedByRequest"] = aborted;
      if (value.response.is_null() && field(drain->redirect_drain, "response").is_object()) value.response = drain->redirect_drain.at("response");
      if (!value.response.is_null()) value.response["bodyRepresentation"] = "encoded-network-bytes";
    }
    if (!callback && field(value.completion, "success") != false) bad(value, "http-redirect-notification-unobserved");
    if (field(value.redirect_capture, "complete") != true && !aborted) bad(value, "http-redirect-body-capture-incomplete", {{"capture", value.redirect_capture}});
  };
  for (auto& [key, value] : requests) if (str(value.request, "layer") == "network" && !str(value.redirect, "redirectBodyId").empty())
    attach(value, text(field(value.scope, "processId")) + '/' + str(value.redirect, "redirectBodyId"), true);
  for (auto& [key, value] : requests) if (!value.redirect_drain.is_null() && !str(value.redirect_drain, "parentRequestId").empty()) {
    auto parent_key = text(field(value.scope, "processId")) + '/' + str(value.redirect_drain, "parentRequestId"); auto parent = requests.find(parent_key);
    if (str(value.redirect_drain, "parentRequestProtocol") != "v8trace/http-parent/1" || parent == requests.end() || str(parent->second.request, "layer") != "network")
      bad(value, "http-redirect-parent-not-observed", {{"parentRequestId", parent_key}});
    else attach(parent->second, key, false);
  }
  auto body = [&](Request& value, bool request, const Json& end, int64_t expected = -1) -> std::optional<std::string> {
    auto bytes = request ? value.request_bytes : value.response_bytes; auto budget = request ? value.request_budget : value.response_budget; auto file = request ? value.request_file : value.response_file;
    if (budget || !value.errors.empty() || end.is_null() || field(end, "complete") == false) return std::nullopt;
    if ((end.contains("bytes") && decimal(str(end, "bytes")) != bytes) || (expected >= 0 && uint64_t(expected) != bytes)) return std::nullopt;
    auto data = file.empty() ? std::string{} : read_file(file, maximum); if (data.size() != bytes) return std::nullopt; return data;
  };
  uint64_t request_count = 0;
  for (auto& [key, value] : requests) {
    if (!value.redirect_drain.is_null() && !attached.contains(key)) bad(value, "http-redirect-body-unattached", {{"url", field(value.redirect_drain, "url")}});
    if (value.request.is_null()) continue; ++request_count;
    try {
      auto& request = value.request; auto& response = value.response; auto& completion = value.completion; std::optional<std::string> post; bool post_complete = true;
      if (str(request, "layer") == "network") {
        auto upload_id = str(request, "uploadStreamId"); if (!upload_id.empty()) { value.upload_attempts = Json::array(); Request* last = nullptr; auto candidates = uploads[text(field(value.scope, "processId")) + '/' + upload_id]; for (auto& upload_key : candidates) { auto& upload = requests.at(upload_key); if (upload.started && value.started && *upload.started >= *value.started && (!value.finished || *upload.started <= *value.finished)) { value.upload_attempts.push_back({{"requestId", upload.key}, {"complete", field(upload.request_end, "complete")}, {"bytes", upload.request_bytes}}); if (field(upload.request_end, "complete") == true) last = &upload; } } if (last) post = body(*last, true, last->request_end); post_complete = post.has_value(); if (!post_complete) bad(value, "http-upload-not-finalized"); }
      } else { post = body(value, true, value.request_end); post_complete = field(request, "requestBodyPresent") != true || post.has_value(); }
      int64_t expected = -1; auto expected_value = field(completion, "expectedDecodedBytes"); if (expected_value.is_string()) { auto s = text(expected_value); if (s != "-1") expected = int64_t(decimal(s)); } else if (safe_uint(expected_value)) expected = expected_value.get<int64_t>();
      Json end = value.redirect_end; if (end.is_null()) end = field(completion, "pipe") == true ? value.response_end : completion.is_null() ? Json(nullptr) : Json{{"complete", field(completion, "success")}, {"bytes", field(completion, "bytes")}};
      auto response_bytes = body(value, false, end, expected); auto length = header(field(response, "headers"), "content-length");
      if (!value.redirect.is_null() && str(request, "layer") == "network" && field(value.redirect_capture, "complete") != true && !value.response_bytes && str(request, "method") != "HEAD" && length != "0") { response_bytes.reset(); bad(value, "http-redirect-body-not-observed", {{"contentLength", length.empty() ? Json(nullptr) : Json(length)}}); }
      Json encoded;
      if (response_bytes && str(response, "bodyRepresentation") == "encoded-network-bytes") { encoded = store.blob(*response_bytes, "native-http-encoded-network-body"); try { response_bytes = decode_body(std::move(*response_bytes), header(field(response, "headers"), "content-encoding"), maximum); } catch (const std::exception& e) { response_bytes.reset(); bad(value, "http-content-decoding", {{"error", e.what()}}); } }
      auto mime = str(response, "mimeType"); if (mime.empty()) mime = trim(split(header(field(response, "headers"), "content-type"), ';')[0]);
      std::string type = str(request, "layer") == "document" || mime == "text/html" ? "Document" : mime.find("javascript") != mime.npos || mime.find("ecmascript") != mime.npos ? "Script" : mime == "text/css" ? "Stylesheet" : mime == "application/wasm" ? "Wasm" : mime.starts_with("font/") ? "Font" : "Fetch";
      if (completion.is_null() && value.redirect.is_null()) bad(value, "http-request-open-at-flush"); else if (field(completion, "success") == true && !response_bytes && str(request, "method") != "HEAD" && field(response, "status") != 204 && field(response, "status") != 205 && field(response, "status") != 304) bad(value, "http-response-incomplete", {{"expected", expected}, {"capturedBytes", value.response_bytes}});
      Json body_prefix;
      if (!response_bytes && value.response_bytes && !value.response_budget && value.errors.empty() && !value.response_file.empty()) {
        auto prefix = read_file(value.response_file, maximum);
        if (prefix.size() == value.response_bytes) body_prefix = store.blob(prefix, str(response, "bodyRepresentation") == "encoded-network-bytes" ? "native-http-encoded-network-prefix" : "native-http-decoded-prefix");
      }
      Json record = value.scope; Json final_response = response; if (!final_response.is_null()) final_response["mimeType"] = mime;
      record["resourceIdentifier"]=field(request,"resourceIdentifier");
      record["completion"] = completion; record["bodyPrefix"] = body_prefix;
      record.update({{"requestId", value.key}, {"nativeLayer", field(request, "layer")}, {"request", {{"url", field(request, "url")}, {"method", field(request, "method")}, {"headers", value.actual_headers.is_null() ? field(request, "headers") : value.actual_headers}, {"hasPostData", field(request, "requestBodyPresent")}}}, {"response", final_response}, {"type", type}, {"body", response_bytes ? store.blob(*response_bytes, "native-http-decoded-body") : Json(nullptr)}, {"encodedBody", encoded}, {"postBody", post ? store.blob(*post, "native-http-upload-bytes") : Json(nullptr)}, {"postBodyConclusion", post_complete ? "complete" : "external-upload-not-observed-at-this-layer"}, {"bodyConclusion", response_bytes ? "complete" : field(completion, "success") == true ? "not-observed-at-this-layer" : "failed-or-open"}, {"conclusion", field(completion, "success") == true && response_bytes && post_complete && value.errors.empty() ? "complete" : field(completion, "success") == false ? "failed" : "partial"}, {"startedAtTraceMicroseconds", value.started ? Json(*value.started) : Json(nullptr)}, {"finishedAtTraceMicroseconds", value.finished ? Json(*value.finished) : Json(nullptr)}, {"uploadAttempts", value.upload_attempts}, {"redirect", value.redirect}, {"redirectBodyCapture", value.redirect_capture}});
      store.append("catalog/resources.jsonl", record); store.event("http", "request-complete", record, value.scope);
    } catch (const std::exception& e) { ++invalid; store.gap("browser-normalization-error", {{"requestId", value.key}, {"error", e.what()}}); }
  }
  for (auto& [realm, state] : realms) if (!state.first || !state.second || !realm_sequences[realm].pending.empty()) store.gap("env-realm-incomplete", {{"realm", realm}, {"installed", state.first}, {"ready", state.second}});
  if (enabled.contains("env") && realms.empty()) store.gap("env-no-realms"); if (enabled.contains("script") && counts["script"] == 0) store.gap("script-no-native-source"); if (enabled.contains("http") && !request_count) store.gap("http-no-native-requests");
  store.json("browser-channels-summary.json", {{"protocol", "v8trace/browser/1"}, {"counts", counts}, {"invalid", invalid}, {"producerCount", sequences.size()}, {"realmCount", realms.size()}, {"requests", request_count}, {"scripts", scripts.size()}, {"scopeMapping", "native-pid-tid-isolate-context-only"}, {"cdpConnected", cdp}, {"identityChanged", field(o, "hooks", false)}, {"objectObservationProducers", object_observers.size()}});
}
}
