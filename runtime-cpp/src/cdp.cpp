#include "v8trace/core.h"
#include "v8trace/state.h"
#include "v8trace/recording.h"
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXNetSystem.h>
#include <condition_variable>
#include <algorithm>
#include <deque>
#include <mutex>
#include <thread>

namespace v8trace {
namespace {
using Clock = std::chrono::steady_clock;
class Cdp {
 public:
  Cdp(std::string url, uint64_t timeout, uint64_t maximum, Store& store, bool raw) : timeout_(timeout), maximum_(maximum), store_(store), raw_(raw) {
    ix::initNetSystem(); socket_.setUrl(url); socket_.setExtraHeaders({{"Origin", "http://127.0.0.1"}}); socket_.disableAutomaticReconnection(); socket_.disablePerMessageDeflate();
    socket_.setOnMessageCallback([this](const ix::WebSocketMessagePtr& message) {
      std::lock_guard lock(mutex_);
      try {
        if (message->type == ix::WebSocketMessageType::Open) open_ = true;
        else if (message->type == ix::WebSocketMessageType::Close || message->type == ix::WebSocketMessageType::Error) error_ = "CDP connection closed: " + message->errorInfo.reason;
        else if (message->type == ix::WebSocketMessageType::Message) {
          if (message->str.size() > maximum_) throw std::runtime_error("Single CDP message exceeds byte budget");
          auto value = parse(message->str);
          if (value.contains("id")) {
            const auto id = value.at("id").get<uint64_t>();
            replies_.insert_or_assign(id, std::move(value));
          }
          else { if (queued_bytes_ + message->str.size() > maximum_) throw std::runtime_error("CDP event queue byte budget exceeded; raw evidence retained"); queued_bytes_ += message->str.size(); events_.emplace_back(std::move(value), message->str.size()); }
        }
      } catch (const std::exception& e) { error_ = e.what(); }
      condition_.notify_all();
    });
    socket_.start(); std::unique_lock lock(mutex_); if (!condition_.wait_for(lock, std::chrono::milliseconds(timeout_), [&] { return open_ || !error_.empty(); }) || !open_) { lock.unlock(); socket_.stop(); throw std::runtime_error(error_.empty() ? "CDP WebSocket connect timeout" : error_); }
  }
  ~Cdp() { socket_.stop(); ix::uninitNetSystem(); }
  Json call(std::string_view method, Json params = Json::object(), std::string session = "") {
    auto id = ++sequence_; Json request{{"id", id}, {"method", method}, {"params", std::move(params)}}; if (!session.empty()) request["sessionId"] = session;
    if (raw_) store_.append("cdp.jsonl", {{"observedAt", now()}, {"direction", "out"}, {"message", request}});
    if (!socket_.send(dump(request)).success) throw std::runtime_error("CDP send failed");
    auto deadline = Clock::now() + std::chrono::milliseconds(timeout_); std::unique_lock lock(mutex_);
    auto urgent = [&] { return std::find_if(events_.begin(), events_.end(), [&](const auto& event) { return !servicing_pause_ && pause_predicate_ && pause_predicate_(event.first); }); };
    while (!replies_.contains(id) && error_.empty()) {
      auto item = urgent();
      if (item != events_.end()) {
        queued_bytes_ -= item->second; auto event = std::move(item->first); events_.erase(item); lock.unlock(); servicing_pause_ = true;
        try { pause_handler_(event); } catch (...) { servicing_pause_ = false; throw; }
        servicing_pause_ = false; lock.lock(); continue;
      }
      if (!condition_.wait_until(lock, deadline, [&] { return replies_.contains(id) || !error_.empty() || urgent() != events_.end(); })) {
        Json queued = Json::array(); for (const auto& event : events_) if (str(event.first, "method") == "Debugger.paused") queued.push_back(event.first);
        lock.unlock(); store_.append("catalog/cdp-command-timeouts.jsonl", {{"method", method}, {"sessionId", session}, {"queuedPauses", queued}});
        throw std::runtime_error("CDP command timeout: " + std::string(method));
      }
    }
    if (!replies_.contains(id)) throw std::runtime_error(error_); auto reply = std::move(replies_.at(id)); replies_.erase(id); lock.unlock();
    if (raw_) store_.append("cdp.jsonl", {{"observedAt", now()}, {"direction", "in"}, {"message", reply}});
    if (reply.contains("error")) throw std::runtime_error(std::string(method) + ": " + dump(reply["error"])); return field(reply, "result", Json::object());
  }
  std::optional<Json> next() { std::lock_guard lock(mutex_); if (!events_.empty()) { auto pair = std::move(events_.front()); events_.pop_front(); queued_bytes_ -= pair.second; return std::move(pair.first); } if (!error_.empty()) throw std::runtime_error(error_); return std::nullopt; }
  std::optional<Json> bootstrap(const std::string& session, const std::string& parent, const std::string& target) {
    std::lock_guard lock(mutex_);
    auto item = std::find_if(events_.begin(), events_.end(), [&](const auto& value) {
      const auto& event = value.first; auto id = str(event, "sessionId"), method = str(event, "method"); auto params = field(event, "params");
      return (id == session && (method == "Debugger.scriptParsed" || method == "Debugger.scriptFailedToParse")) ||
             ((id == session || (id == parent && method == "Network.requestWillBeSent")) && method.starts_with("Network.") && str(params, "requestId") == target);
    });
    if (item == events_.end()) return std::nullopt;
    queued_bytes_ -= item->second; auto event = std::move(item->first); events_.erase(item); return event;
  }
  void recording(bool enabled) { raw_ = enabled && raw_requested_; }
  void raw_requested(bool enabled) { raw_requested_ = enabled; recording(enabled); }
  void service_pauses(std::function<bool(const Json&)> predicate, Sink handler) { pause_predicate_ = std::move(predicate); pause_handler_ = std::move(handler); }
 private:
  ix::WebSocket socket_; uint64_t timeout_, maximum_, sequence_ = 0, queued_bytes_ = 0; Store& store_; bool raw_, raw_requested_ = false, open_ = false;
  std::mutex mutex_; std::condition_variable condition_; std::string error_; std::map<uint64_t, Json> replies_; std::deque<std::pair<Json, size_t>> events_;
  std::function<bool(const Json&)> pause_predicate_; Sink pause_handler_; bool servicing_pause_ = false;
};
}
Json cdp_worker_request_handoff(const Json& worker, const Json& response, const Json& request, uint64_t window, bool controlled) {
  auto info = field(worker, "info"); auto url = str(info, "url"), type = str(info, "type"), target = str(worker, "targetId"), parent = str(worker, "parentSessionId");
  if (!std::set<std::string>{"worker", "shared_worker", "service_worker"}.contains(type) || target.empty() || parent.empty() || str(worker, "sessionId").empty()) return nullptr;
  if (str(request, "sessionId") != parent || str(response, "requestId") != target || str(request, "requestId") != target || str(request, "conclusion") != "pending") return nullptr;
  if (url.empty() || str(field(request, "request"), "url") != url || str(field(response, "response"), "url") != url || str(field(request, "request"), "method") != "GET") return nullptr;
  if (!str(info, "parentId").empty() && str(info, "parentId") != str(request, "targetId")) return nullptr;
  if (controlled && field(request, "recordingWindow") != window) return nullptr;
  return {{"kind", "observed-worker-main-resource-handoff"}, {"requestId", target}, {"requestSessionId", parent}, {"responseSessionId", worker.at("sessionId")}, {"requestTargetId", field(request, "targetId")}, {"responseTargetId", target}, {"url", url}};
}
bool cdp_owned_source_pause(const Json& event, const Json& session) {
  return str(event, "method") == "Debugger.paused" && str(field(event, "params"), "reason") == "instrumentation" && !str(session, "sourceBreakpoint").empty() && !str(event, "sessionId").empty() && str(event, "sessionId") == str(session, "sessionId");
}
void capture_cdp(const Json& o, Store& store, Process& process, std::atomic_bool& stop, const Sink& progress, Json& result, RecordingControl* control) {
  bool recording = !control || !control->paused(); RecordingClock clock(true);
  auto launch = launch_config(o, store); auto args = launch.at("args").get<std::vector<std::string>>(); args.erase(std::remove_if(args.begin(), args.end(), [](const std::string& arg) { return arg.starts_with("--trace-config-file=") || arg == "--trace-startup-format=json"; }), args.end()); args.back() = "about:blank"; args.push_back("--remote-debugging-port=0"); args.push_back("--remote-debugging-address=127.0.0.1"); args.push_back("--remote-allow-origins=http://127.0.0.1");
  store.json("launch.json", {{"runtime", "cpp"}, {"transport", "cdp"}, {"executable", o.at("chromePath")}, {"args", args}, {"options", o}, {"automaticFallback", false}});
  process.start(path(str(o, "chromePath")), args, store.root, store.filename("native-output/browser-stdout.log"), store.filename("native-output/browser-stderr.log"));
  auto active_file = store.filename("browser-profile/DevToolsActivePort"); auto deadline = Clock::now() + std::chrono::milliseconds(o.at("commandTimeout").get<uint64_t>()); std::string websocket;
  while (Clock::now() < deadline && !stop) { if (auto exit = process.poll()) throw std::runtime_error("Chrome exited before CDP startup: " + dump(*exit)); if (fs::exists(active_file)) { auto parts = split(read_file(active_file, 65536), '\n'); if (parts.size() >= 2 && !trim(parts[0]).empty() && parts[1].starts_with("/devtools/browser/")) { auto port = decimal(trim(parts[0])); if (port > 65535 || !port) throw std::runtime_error("Invalid CDP active port"); websocket = "ws://127.0.0.1:" + std::to_string(port) + trim(parts[1]); break; } } std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
  if (websocket.empty()) throw std::runtime_error("Chrome CDP startup timeout or cancelled"); Cdp cdp(websocket, o.at("commandTimeout").get<uint64_t>(), o.at("maxBytes").get<uint64_t>(), store, recording && o.at("rawCdp") == true); result["cdpConnected"] = true; auto version = cdp.call("Browser.getVersion"); store.json("browser.json", {{"runtime", "cpp"}, {"transport", "cdp"}, {"version", version}, {"executable", o.at("chromePath")}, {"processId", process.id()}});
  cdp.raw_requested(o.at("rawCdp") == true); cdp.recording(recording);
  std::map<std::string, Json> sessions, targets, requests, cached_bodies, worker_streams, pending_sources; std::map<std::string, uint64_t> received_lengths; std::set<std::string> written, written_scripts; uint64_t window = 0, worker_stream_bytes = 0; auto binding = "__v8trace_" + sha256(uuid()).substr(0, 24), gate = binding + "_recording"; std::string hook_source;
  if (o.at("hooks") == true) { auto asset = path(str(o, "assets")); hook_source = '(' + read_file(asset / "diagnostic-hooks.js", 1048576) + ")(" + dump(binding) + ',' + text(o.at("maxEvents")) + (control ? ",function(){return globalThis[" + dump(gate) + "]===true;}" : "") + ");\n//# sourceURL=v8trace://diagnostic-hooks"; }
  auto new_document_source = [&] { return (control ? "globalThis[" + dump(gate) + "]=" + (recording ? "true;" : "false;") : "") + hook_source; };
  auto scope = [&](const std::string& session) { Json value{{"sessionId", session}, {"targetId", field(sessions[session], "targetId")}}; if (control) value["recordingWindow"] = window; return value; };
  auto optional = [&](const std::string& method, Json params, const std::string& session, bool critical = true) -> Json { try { return cdp.call(method, std::move(params), session); } catch (const std::exception& e) { store.gap("cdp-domain-command", {{"method", method}, {"sessionId", session}, {"error", e.what()}}, critical); return nullptr; } };
  auto enable_sources = [&](const std::string& session) {
    cdp.call("Debugger.enable", {{"maxScriptsCacheSize", std::min<uint64_t>(o.at("maxBodyBytes").get<uint64_t>() * 4, o.at("maxBytes").get<uint64_t>())}}, session);
    cdp.call("Debugger.setBlackboxPatterns", {{"patterns", Json::array({"^v8trace://"})}}, session);
    sessions[session]["sourceBreakpoint"] = cdp.call("Debugger.setInstrumentationBreakpoint", {{"instrumentation", "beforeScriptExecution"}}, session).at("breakpointId");
  };
  if (o.at("scripts") == true) result["scriptCapturePolicy"] = "cdp-before-script-execution-and-held-target-bootstrap;collector-v8trace-sources-blackboxed;timing-affected";
  if (o.at("http") == true) result["workerBodyCapturePolicy"] = "cdp-before-worker-main-execution;buffered-and-streamed-network-bytes;loading-finished-length-verified;timing-affected";
  auto write_request = [&](const std::string& key) { if (!written.insert(key).second) return; auto row = requests.at(key); store.append("catalog/resources.jsonl", row); store.event("http", "request-complete", row, scope(str(row, "sessionId"))); };
  auto fetch_body = [&](const std::string& session, const Json& id) {
    auto body = cdp.call("Network.getResponseBody", {{"requestId", id}}, session); auto binary = field(body, "base64Encoded") == true;
    auto bytes = binary ? unbase64(str(body, "body")) : replacement_utf8(str(body, "body"));
    if (bytes.size() > o.at("maxBodyBytes").get<uint64_t>()) throw std::runtime_error("Response body exceeds byte budget");
    auto blob = store.blob(bytes, binary ? "cdp-decoded-bytes" : "cdp-decoded-text-utf8");
    return Json{{"body", blob}, {"bodyConclusion", blob.is_null() ? "capture-budget" : "captured"}, {"bodySessionId", session}};
  };
  auto append_worker_bytes = [&](const std::string& key, const std::string& bytes) {
    auto& stream = worker_streams.at(key); auto size = stream.at("bytes").get<uint64_t>();
    if (bytes.size() > o.at("maxBodyBytes").get<uint64_t>() - size || bytes.size() > o.at("maxBytes").get<uint64_t>() - worker_stream_bytes) throw std::runtime_error("Worker response body exceeds byte budget");
    std::ofstream output(store.filename(str(stream, "file")), std::ios::binary | std::ios::app);
    output.write(bytes.data(), std::streamsize(bytes.size())); output.flush();
    if (!output) throw std::runtime_error("Worker response stream write failed");
    stream["bytes"] = size + bytes.size(); worker_stream_bytes += bytes.size();
  };
  std::function<void(const Json&)> on_event;
  on_event = [&](const Json& event) {
    if (recording && o.at("rawCdp") == true) store.append("cdp.jsonl", {{"observedAt", now()}, {"direction", "in"}, {"message", event}});
    auto method = str(event, "method"), session = str(event, "sessionId"); auto params = field(event, "params", Json::object());
    if (method == "Target.attachedToTarget") {
      auto id = str(params, "sessionId"); auto info = params.at("targetInfo"); sessions[id] = {{"sessionId", id}, {"targetId", info.at("targetId")}, {"info", info}, {"parentSessionId", session}, {"contexts", Json::array()}, {"ready", false}}; targets[str(info, "targetId")] = id; if (recording) store.event("lifecycle", "target-attached", params, scope(id));
      try {
        auto type = str(info, "type"); if (!std::set<std::string>{"page", "iframe", "worker", "shared_worker", "service_worker"}.contains(type) || str(info, "url").starts_with("chrome-extension:")) { optional("Runtime.runIfWaitingForDebugger", Json::object(), id); return; }
        cdp.call("Runtime.enable", Json::object(), id);
        if (o.at("opcode") == true || o.at("jsCallDetail") == true || o.at("nativeDetail") == true || o.at("wasm") == true) { sessions[id]["isolateId"] = cdp.call("Runtime.getIsolateId", Json::object(), id).at("id"); cdp.call("Runtime.evaluate", {{"expression", "(function v8traceScopeProbe(){return Math.max(1,2);})()\n//# sourceURL=v8trace://opcode-probe"}, {"silent", true}}, id); }
        if (recording && (o.at("scripts") == true || (o.at("http") == true && type != "page" && type != "iframe"))) enable_sources(id);
        if (recording && o.at("http") == true) optional("Network.enable", {{"maxTotalBufferSize", o.at("maxBodyBytes").get<uint64_t>() * 4}, {"maxResourceBufferSize", o.at("maxBodyBytes")}, {"maxPostDataSize", o.at("maxBodyBytes")}}, id);
        optional("Target.setAutoAttach", {{"autoAttach", true}, {"waitForDebuggerOnStart", true}, {"flatten", true}}, id);
        if (o.at("hooks") == true) cdp.call("Runtime.addBinding", {{"name", binding}}, id);
        if (type == "page" || type == "iframe") { cdp.call("Page.enable", Json::object(), id); if (o.at("hooks") == true) sessions[id]["hookScript"] = cdp.call("Page.addScriptToEvaluateOnNewDocument", {{"source", new_document_source()}}, id).at("identifier"); }
        else if (o.at("hooks") == true) { auto response = cdp.call("Runtime.evaluate", {{"expression", new_document_source()}}, id); if (response.contains("exceptionDetails")) throw std::runtime_error("Worker hook installation raised exception"); }
        if (recording && o.at("cpuProfile") == true && !optional("Profiler.enable", Json::object(), id).is_null()) { cdp.call("Profiler.start", Json::object(), id); sessions[id]["profiling"] = true; }
        if (!recording) { optional("Runtime.disable", Json::object(), id); if (type == "page" || type == "iframe") optional("Page.disable", Json::object(), id); }
        sessions[id]["ready"] = true;
      } catch (const std::exception& e) { sessions[id]["error"] = e.what(); store.gap("target-configuration", {{"targetId", info.at("targetId")}, {"error", e.what()}}); }
      if (recording) while (auto bootstrap = cdp.bootstrap(id, session, str(info, "targetId"))) on_event(*bootstrap);
      optional("Runtime.runIfWaitingForDebugger", Json::object(), id); sessions[id]["configured"] = true; return;
    }
    if (method == "Target.detachedFromTarget") { auto id = str(params, "sessionId"); sessions[id]["detached"] = true; if (field(sessions[id], "profiling") == true) store.gap("profile-target-detached", {{"sessionId", id}}); if (recording) store.event("lifecycle", "target-detached", params, scope(id)); return; }
    if (sessions.contains(session) && cdp_owned_source_pause(event, sessions.at(session))) {
      if (recording) store.append("catalog/source-pauses.jsonl", {{"scope", scope(session)}, {"pause", params}, {"policy", "collector-owned-before-script-execution"}});
      auto script_id = str(field(params, "data"), "scriptId"), source_key = session + '/' + script_id;
      if (recording && o.at("scripts") == true && !script_id.empty() && !written_scripts.contains(source_key) && !pending_sources.contains(source_key)) {
        try { pending_sources[source_key] = cdp.call("Debugger.getScriptSource", {{"scriptId", script_id}}, session); }
        catch (const std::exception& error) { store.gap("script-source-unavailable", {{"sessionId", session}, {"scriptId", script_id}, {"pause", params}, {"error", error.what()}}); }
      }
      const auto& state = sessions.at(session); auto info = field(state, "info"); auto target = str(state, "targetId"), key = session + '/' + target;
      if (recording && o.at("http") == true && std::set<std::string>{"worker", "shared_worker", "service_worker"}.contains(str(info, "type")) && !target.empty() && str(field(params, "data"), "url") == str(info, "url") && !cached_bodies.contains(key) && !worker_streams.contains(key) && !written.contains(key)) {
        try { cached_bodies[key] = fetch_body(session, target); cached_bodies[key]["bodyReadPhase"] = "before-worker-main-script-execution"; }
        catch (const std::exception& error) {
          try {
            auto stream = cdp.call("Network.streamResourceContent", {{"requestId", target}}, session);
            worker_streams[key] = {{"file", "native-output/cdp-worker-body-" + uuid() + ".bin"}, {"bytes", 0}, {"sessionId", session}, {"requestId", target}, {"url", str(info, "url")}, {"recordingWindow", window}};
            append_worker_bytes(key, unbase64(str(stream, "bufferedData")));
          } catch (const std::exception& stream_error) { worker_streams.erase(key); store.append("catalog/worker-body-prefetch.jsonl", {{"scope", scope(session)}, {"requestId", target}, {"error", error.what()}, {"streamError", stream_error.what()}, {"conclusion", "pending-normal-loading-finished-retrieval"}}); }
        }
      }
      cdp.call("Debugger.resume", Json::object(), session); return;
    }
    if (!recording) return;
    if (method == "Runtime.executionContextCreated") { auto context_id = params.at("context").at("id"); if (sessions.contains(session)) sessions[session]["contexts"].push_back(context_id); if (control && o.at("hooks") == true) optional("Runtime.evaluate", {{"expression", "globalThis[" + dump(gate) + "]=true\n//# sourceURL=v8trace://recording-gate"}, {"contextId", context_id}, {"silent", true}}, session, false); if (o.at("contextEvents") == true) store.append("catalog/contexts.jsonl", {{"sessionId", session}, {"method", method}, {"context", params.at("context")}}); }
    if (method == "Runtime.executionContextsCleared" && sessions.contains(session)) sessions[session]["contexts"] = Json::array();
    if (method == "Runtime.executionContextDestroyed" && sessions.contains(session)) { auto& contexts = sessions[session]["contexts"]; contexts.erase(std::remove(contexts.begin(), contexts.end(), field(params, "executionContextId")), contexts.end()); }
    if (method.starts_with("Runtime.executionContext") || method.starts_with("Page.frame")) store.event("lifecycle", method, params, scope(session));
    else if (method == "Runtime.consoleAPICalled") store.event("console", "api", params, scope(session));
    else if (method == "Runtime.exceptionThrown") store.event("exception", "thrown", params, scope(session));
    else if (method == "Runtime.bindingCalled" && str(params, "name") == binding) { try { auto payload = parse(str(params, "payload")); auto kind = str(payload, "kind"); if (!std::set<std::string>{"ready", "installed", "install-failed", "enter", "return", "throw", "gap"}.contains(kind)) throw std::runtime_error("Unknown hook event kind"); if (kind == "gap" || kind == "install-failed") store.gap("diagnostic-hook-gap", {{"sessionId", session}, {"payload", payload}}); auto context = scope(session); context.update({{"contextId", field(params, "executionContextId")}, {"fidelity", "diagnostic-js-hooks"}, {"trust", "page-observer-untrusted"}}); store.event("env", kind, payload, context); } catch (const std::exception& e) { store.gap("invalid-hook-event", {{"error", e.what()}}); } }
    else if (o.at("scripts") == true && (method == "Debugger.scriptParsed" || method == "Debugger.scriptFailedToParse") && str(params, "url") != "v8trace://diagnostic-hooks") {
      try { auto key = session + '/' + str(params, "scriptId"); auto source = pending_sources.contains(key) ? std::move(pending_sources.at(key)) : cdp.call("Debugger.getScriptSource", {{"scriptId", params.at("scriptId")}}, session); pending_sources.erase(key); Json row = scope(session); row.update(params); if (source.contains("scriptSource")) { auto text_source = str(source, "scriptSource"); row["source"] = store.blob(replacement_utf8(text_source), "cdp-script-source-utf8"); row["sourceUtf16"] = store.blob(utf16le(text_source), "cdp-script-source-utf16le-lossless"); } if (source.contains("bytecode")) row["bytecode"] = store.blob(unbase64(str(source, "bytecode")), "cdp-wasm-bytecode"); store.append("catalog/scripts.jsonl", row); written_scripts.insert(key); auto context = scope(session); context["contextId"] = field(params, "executionContextId"); store.event("script", "parsed", row, context); } catch (const std::exception& e) { store.gap("script-source-unavailable", {{"sessionId", session}, {"scriptId", field(params, "scriptId")}, {"script", params}, {"error", e.what()}}); }
    } else if (o.at("http") == true && method.starts_with("Network.")) {
      auto context = scope(session); context["requestId"] = field(params, "requestId"); store.event("http", method, params, context); auto key = session + '/' + str(params, "requestId");
      if (method == "Network.dataReceived") {
        auto length = field(params, "dataLength");
        if ((!length.is_number_unsigned() && !length.is_number_integer()) || (length.is_number_integer() && !length.is_number_unsigned() && length.get<int64_t>() < 0)) throw std::runtime_error("Invalid CDP response data length");
        auto count = length.get<uint64_t>(); auto& total = received_lengths[key];
        if (count > UINT64_MAX - total) throw std::runtime_error("CDP response data length overflow"); total += count;
        if (worker_streams.contains(key) && params.contains("data")) append_worker_bytes(key, unbase64(str(params, "data")));
      }
      if (method == "Network.responseReceived" && !requests.contains(key) && sessions.contains(session)) {
        auto parent_key = str(sessions.at(session), "parentSessionId") + '/' + str(params, "requestId");
        if (requests.contains(parent_key) && !written.contains(parent_key)) {
          auto handoff = cdp_worker_request_handoff(sessions.at(session), params, requests.at(parent_key), window, control != nullptr);
          if (!handoff.is_null()) { auto row = std::move(requests.at(parent_key)); requests.erase(parent_key); row["requestHandoff"] = handoff; row["responseSessionId"] = session; row["responseTargetId"] = field(sessions.at(session), "targetId"); requests[key] = std::move(row); }
        }
      }
      if (method == "Network.requestWillBeSent") {
        uint64_t hop = 0; if (requests.contains(key)) { hop = field(requests[key], "hop", 0).get<uint64_t>() + 1; if (params.contains("redirectResponse")) { requests[key]["response"] = params.at("redirectResponse"); requests[key]["conclusion"] = "redirect"; requests[key]["bodyConclusion"] = "redirect-body-not-exposed-by-cdp"; write_request(key); } else if (!written.contains(key)) { requests[key]["conclusion"] = "reused-request-id"; write_request(key); store.gap("request-id-reused", context); } }
        Json row = scope(session); row.update({{"requestId", params.at("requestId")}, {"hop", hop}, {"type", field(params, "type")}, {"frameId", field(params, "frameId")}, {"loaderId", field(params, "loaderId")}, {"timestamp", field(params, "timestamp")}, {"wallTime", field(params, "wallTime")}, {"initiator", field(params, "initiator")}, {"request", params.at("request")}, {"conclusion", "pending"}}); auto request = params.at("request");
        if (request.contains("postData")) { auto bytes = replacement_utf8(str(request, "postData")); if (bytes.size() <= o.at("maxBodyBytes").get<uint64_t>()) row["postBody"] = store.blob(bytes, "cdp-request-post-data-utf8"); else { row["postBodyConclusion"] = "body-budget"; store.gap("request-body-budget", context); } }
        else if (field(request, "hasPostData") == true) { try { auto post = cdp.call("Network.getRequestPostData", {{"requestId", params.at("requestId")}}, session); auto bytes = field(post, "base64Encoded") == true ? unbase64(str(post, "postData")) : replacement_utf8(str(post, "postData")); if (bytes.size() > o.at("maxBodyBytes").get<uint64_t>()) throw std::runtime_error("Request body exceeds byte budget"); row["postBody"] = store.blob(bytes, "cdp-request-post-data-utf8"); row["postBodyConclusion"] = "captured-multipart-files-may-be-omitted"; } catch (const std::exception& e) { row["postBodyConclusion"] = "cdp-unavailable"; store.gap("request-post-data-unavailable", {{"sessionId", session}, {"error", e.what()}}); } }
        requests[key] = row; written.erase(key);
      } else if (method == "Network.responseReceived") { if (!requests.contains(key)) { Json row = scope(session); row.update({{"requestId", params.at("requestId")}, {"hop", 0}, {"type", field(params, "type")}, {"request", {{"url", field(params.at("response"), "url")}, {"method", nullptr}}}, {"conclusion", "pending"}}); requests[key] = row; store.gap("response-without-request-metadata", context); } requests[key]["response"] = params.at("response"); requests[key]["type"] = field(params, "type"); }
      else if (method == "Network.loadingFailed" && requests.contains(key)) { requests[key]["conclusion"] = "failed"; requests[key]["failure"] = params; write_request(key); }
      else if (method == "Network.loadingFinished" && requests.contains(key) && !written.contains(key)) { auto& row = requests[key]; row["finished"] = params; row["conclusion"] = "complete"; auto status = field(field(row, "response"), "status"); if (str(row.at("request"), "method") == "HEAD" || status == 204 || status == 205 || status == 304) row["bodyConclusion"] = "no-body-by-http-semantics";
        else try {
          if (worker_streams.contains(key)) {
            const auto& stream = worker_streams.at(key);
            if (stream.at("recordingWindow") != window || stream.at("bytes").get<uint64_t>() != received_lengths[key] || fs::file_size(store.filename(str(stream, "file"))) != received_lengths[key]) throw std::runtime_error("Worker streamed body does not match complete Network.dataReceived byte count and recording window");
            auto blob = store.blob_file(store.filename(str(stream, "file")), "cdp-decoded-network-bytes");
            row.update({{"body", blob}, {"bodyConclusion", blob.is_null() ? "capture-budget" : "captured"}, {"bodySessionId", session}, {"bodyReadPhase", "before-worker-main-script-execution-network-stream"}, {"bodyStreamIntegrity", {{"receivedBytes", received_lengths[key]}, {"storedBytes", stream.at("bytes")}, {"loadingFinishedObserved", true}}}});
            store.append("catalog/worker-body-streams.jsonl", {{"scope", context}, {"stream", stream}, {"integrity", row.at("bodyStreamIntegrity")}, {"body", blob}}); worker_streams.erase(key);
          } else if (cached_bodies.contains(key)) { row.update(cached_bodies.at(key)); cached_bodies.erase(key); } else row.update(fetch_body(session, params.at("requestId")));
        } catch (const std::exception& e) { row["bodyConclusion"] = "cdp-unavailable"; row["bodyError"] = e.what(); store.gap("response-body-unavailable", {{"sessionId", session}, {"requestId", params.at("requestId")}, {"error", e.what()}}); } received_lengths.erase(key); write_request(key);
      }
    }
  };
  cdp.service_pauses([&](const Json& event) { auto session = str(event, "sessionId"); return sessions.contains(session) && cdp_owned_source_pause(event, sessions.at(session)); }, on_event);
  auto pump = [&] { auto event = cdp.next(); if (event) { try { on_event(*event); } catch (const std::exception& e) { store.json("cdp-event-error.json", {{"event", *event}, {"error", e.what()}}); throw std::runtime_error("CDP event " + str(*event, "method") + ": " + e.what()); } return true; } return false; };
  bool tracing = false; uint64_t raw_trace_bytes = 0; std::vector<fs::path> trace_files; Json windows = Json::array(); std::string window_started;
  auto state_changed = [&] { if (!control) return; auto state = control->status(); state.update({{"type", "recording-state"}, {"recordingActiveMilliseconds", clock.active_ms()}, {"recordingPausedMilliseconds", clock.paused_ms()}}); store.append("recording-control.jsonl", {{"observedAt", now()}, {"state", state}}); if (progress) progress(state); };
  auto start_trace = [&] {
    ++window; window_started = now();
    if (!state_selected(o) && !env_selected(o) && o.at("nativeTimeline") != true && o.at("opcode") != true && o.at("jsCallDetail") != true && o.at("nativeDetail") != true && o.at("wasm") != true && o.at("redirectBodies") != true) return;
    Json categories = Json::array(); if (o.at("nativeTimeline") == true) { categories.push_back("v8"); categories.push_back("blink.bindings"); categories.push_back("loading"); categories.push_back("devtools.timeline"); }
    if (env_selected(o)) categories.push_back("disabled-by-default-v8.envprobe");
    if (state_selected(o)) { categories.push_back("disabled-by-default-v8.state"); for (const auto& channel : selected_state_channels(o)) categories.push_back("disabled-by-default-v8." + text(channel)); }
    if (field(o, "wasmModules", false) == true || field(o, "vmpProbe", false) == true) categories.push_back("disabled-by-default-v8.aux");
    for (auto [key, channel] : std::vector<std::pair<std::string, std::string>>{{"opcode", "opcode"}, {"jsCallDetail", "jscall"}, {"nativeDetail", "native"}, {"wasm", "wasm"}, {"redirectBodies", "http"}}) if (o.at(key) == true) categories.push_back("disabled-by-default-v8." + channel);
    cdp.call("Tracing.start", {{"transferMode", "ReturnAsStream"}, {"traceConfig", {{"recordMode", "recordUntilFull"}, {"traceBufferSizeInKb", std::clamp<uint64_t>(o.at("maxBytes").get<uint64_t>() / 1024, 4096, 1048576)}, {"includedCategories", categories}, {"excludedCategories", Json::array({"*"})}, {"enableSampling", false}, {"enableArgumentFilter", false}}}}); tracing = true;
  };
  auto stop_trace = [&] {
    if (tracing) {
      cdp.call("Tracing.end"); Json complete; auto flush_deadline = Clock::now() + std::chrono::milliseconds(o.at("traceFlushTimeout").get<uint64_t>());
      while (Clock::now() < flush_deadline) { auto event = cdp.next(); if (event) { if (str(*event, "method") == "Tracing.tracingComplete") { complete = field(*event, "params"); break; } on_event(*event); } else std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
      if (str(complete, "stream").empty()) throw std::runtime_error("Tracing completed without stream"); if (field(complete, "dataLossOccurred") == true) store.gap("native-trace-data-loss");
      auto handle = str(complete, "stream"); auto file = store.filename("native-output/cdp-trace-" + std::to_string(window) + ".json"); std::ofstream output(file, std::ios::binary);
      while (true) { auto part = cdp.call("IO.read", {{"handle", handle}, {"size", 65536}}); auto bytes = field(part, "base64Encoded") == true ? unbase64(str(part, "data")) : str(part, "data"); if (bytes.size() > o.at("maxBytes").get<uint64_t>() - raw_trace_bytes) throw std::runtime_error("Recording trace windows exceed total byte budget"); raw_trace_bytes += bytes.size(); output.write(bytes.data(), std::streamsize(bytes.size())); if (!output) throw std::runtime_error("CDP trace write failed"); if (field(part, "eof") == true) break; }
      output.flush(); if (!output) throw std::runtime_error("CDP trace flush failed"); output.close(); cdp.call("IO.close", {{"handle", handle}}); trace_files.push_back(file); tracing = false;
    }
    if (!window_started.empty()) { windows.push_back({{"index", window}, {"startedAt", window_started}, {"finishedAt", now()}}); window_started.clear(); store.json("recording-windows.json", {{"protocol", "v8trace/recording/1"}, {"windows", windows}, {"outsideWindowsCaptured", false}, {"browserKeptRunningAcrossWindows", true}}); }
  };
  auto stop_profiles = [&] { for (auto& [id, state] : sessions) if (field(state, "profiling") == true && field(state, "detached") != true) { auto profile = optional("Profiler.stop", Json::object(), id); state["profiling"] = false; if (!profile.is_null()) { auto blob = store.blob(dump(profile.at("profile")), "cdp-cpu-profile-json"); auto context = scope(id); context["recordingWindow"] = window; store.event("jscall", "cpu-profile", {{"profile", blob}, {"fidelity", "sampling"}, {"argumentsCaptured", false}, {"returnsCaptured", false}}, context); } } };
  auto domains = [&](bool enabled) {
    for (auto& [id, state] : sessions) {
      if (field(state, "ready") != true || field(state, "detached") == true) continue;
      if (enabled) {
        cdp.call("Runtime.discardConsoleEntries", Json::object(), id);
        cdp.call("Runtime.enable", Json::object(), id);
        auto source_type = str(state.at("info"), "type");
        if (o.at("scripts") == true || (o.at("http") == true && source_type != "page" && source_type != "iframe")) enable_sources(id);
        if (o.at("http") == true) cdp.call("Network.enable", {{"maxTotalBufferSize", o.at("maxBodyBytes").get<uint64_t>() * 4}, {"maxResourceBufferSize", o.at("maxBodyBytes")}, {"maxPostDataSize", o.at("maxBodyBytes")}}, id);
        if (o.at("cpuProfile") == true) { cdp.call("Profiler.enable", Json::object(), id); cdp.call("Profiler.start", Json::object(), id); state["profiling"] = true; }
      }
      if (o.at("hooks") == true) {
        auto expression = "globalThis[" + dump(gate) + "]=" + (enabled ? "true" : "false") + "\n//# sourceURL=v8trace://recording-gate";
        cdp.call("Runtime.evaluate", {{"expression", expression}, {"silent", true}}, id);
        for (auto& context : state.at("contexts")) optional("Runtime.evaluate", {{"expression", expression}, {"contextId", context}, {"silent", true}}, id, false);
        if (state.contains("hookScript")) { cdp.call("Page.removeScriptToEvaluateOnNewDocument", {{"identifier", state.at("hookScript")}}, id); state["hookScript"] = cdp.call("Page.addScriptToEvaluateOnNewDocument", {{"source", new_document_source()}}, id).at("identifier"); }
      }
      auto type = str(state.at("info"), "type"); if (type == "page" || type == "iframe") cdp.call(enabled ? "Page.enable" : "Page.disable", Json::object(), id);
      if (!enabled) { if (o.at("http") == true) cdp.call("Network.disable", Json::object(), id); if (state.contains("sourceBreakpoint")) { cdp.call("Debugger.disable", Json::object(), id); state.erase("sourceBreakpoint"); } cdp.call("Runtime.disable", Json::object(), id); }
    }
  };
  auto close_requests = [&](const std::string& conclusion) { for (auto& [key, row] : requests) if (!written.contains(key)) { row["conclusion"] = conclusion; row["bodyConclusion"] = "outside-recording-window"; write_request(key); store.gap("request-crosses-recording-window", {{"url", field(field(row, "request"), "url")}, {"window", window}}, false); } requests.clear(); written.clear(); cached_bodies.clear(); worker_streams.clear(); received_lengths.clear(); pending_sources.clear(); written_scripts.clear(); };
  std::string failure;
  try {
    if (recording) start_trace();
    cdp.call("Target.setDiscoverTargets", {{"discover", true}}); cdp.call("Target.setAutoAttach", {{"autoAttach", true}, {"waitForDebuggerOnStart", true}, {"flatten", true}}); auto created = cdp.call("Target.createTarget", {{"url", "about:blank"}}); auto target = str(created, "targetId"); deadline = Clock::now() + std::chrono::milliseconds(o.at("commandTimeout").get<uint64_t>());
    while (Clock::now() < deadline && (!targets.contains(target) || field(sessions[text(targets[target])], "configured") != true)) { if (!pump()) std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    if (!targets.contains(target) || field(sessions[text(targets[target])], "ready") != true) throw std::runtime_error("Main target setup failed"); auto main_session = text(targets[target]); auto navigation = cdp.call("Page.navigate", {{"url", o.at("url")}}, main_session); if (!str(navigation, "errorText").empty()) throw std::runtime_error("Navigation failed: " + str(navigation, "errorText")); if (recording) store.event("lifecycle", "navigation-requested", navigation, scope(main_session));
    clock.update(!recording); if (control) { control->ready(); state_changed(); }
    auto expression_at = Clock::now(); bool condition_met = false;
    while (clock.active_ms() < o.at("durationSeconds").get<uint64_t>() * 1000) {
      if (stop) { result["stopReason"] = "interrupted"; break; } if (process.poll()) throw std::runtime_error("Chrome exited during CDP capture"); if (!store.accepting) { result["stopReason"] = "budget"; break; }
      if (control) if (auto request = control->pending()) {
        clock.update(true);
        if (request->paused && recording) { recording = false; cdp.recording(false); domains(false); stop_profiles(); stop_trace(); close_requests("open-at-pause"); }
        else if (!request->paused && !recording) { while (pump()) { if (stop) break; } if (stop) break; recording = true; start_trace(); domains(true); cdp.recording(true); }
        control->confirm(*request); clock.update(!recording); state_changed();
      }
      if (progress) { auto value = store.progress(recording ? "capturing" : "paused"); value.update({{"recordingState", recording ? "recording" : "paused"}, {"recordingActiveMilliseconds", clock.active_ms()}, {"recordingPausedMilliseconds", clock.paused_ms()}}); progress(value); }
      if (!pump()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
      if (recording && !str(o, "untilExpression").empty() && Clock::now() >= expression_at) { auto value = cdp.call("Runtime.evaluate", {{"expression", str(o, "untilExpression") + "\n//# sourceURL=v8trace://explicit-user-evaluation"}, {"returnByValue", true}}, main_session); result["pageExpressionsExecuted"] = true; if (value.contains("exceptionDetails")) throw std::runtime_error("Stop expression raised exception"); if (field(field(value, "result"), "value") == true) { condition_met = true; result["stopReason"] = "condition"; break; } expression_at = Clock::now() + std::chrono::milliseconds(250); }
    }
    if (!str(o, "untilExpression").empty() && !condition_met && str(result, "stopReason") == "duration") store.gap("stop-condition-timeout", {{"expression", o.at("untilExpression")}});
    if (!str(o, "resultExpression").empty()) { auto value = cdp.call("Runtime.evaluate", {{"expression", str(o, "resultExpression") + "\n//# sourceURL=v8trace://explicit-user-evaluation"}, {"returnByValue", true}}, main_session); result["pageExpressionsExecuted"] = true; if (value.contains("exceptionDetails")) throw std::runtime_error("Result expression raised exception"); value["observation"] = "explicit-user-evaluation"; store.json("page-result.json", value); }
  } catch (const std::exception& e) { failure = e.what(); }
  try {
    clock.update(true); if (control) { recording = false; cdp.recording(false); domains(false); }
    stop_profiles(); stop_trace();
    if (!trace_files.empty()) {
      auto file = store.filename("native-output/cdp-trace.json");
      if (control) result["recordingTraceMerge"] = merge_recording_traces(trace_files, file, o.at("maxBytes").get<uint64_t>()); else fs::copy_file(trace_files.front(), file);
      auto blob = store.blob_file(file, "chrome-performance-trace-json"); store.json("native-trace.json", {{"blob", blob}, {"transport", "cdp"}}); Trace trace(file); import_trace(trace, store, o);
    }
    if (!control) { auto drain_end = Clock::now() + std::chrono::seconds(1); while (Clock::now() < drain_end && pump()) {} }
  } catch (const std::exception& e) { store.gap("cdp-drain-error", {{"error", e.what()}}); if (failure.empty()) failure = e.what(); }
  if (control) { control->finish(!failure.empty()); state_changed(); result["recording"] = {{"protocol", "v8trace/recording/1"}, {"windowCount", windows.size()}, {"activeMilliseconds", clock.active_ms()}, {"pausedMilliseconds", clock.paused_ms()}, {"outsideWindowsCaptured", false}, {"stopReason", field(result, "stopReason")}}; store.json("recording-summary.json", result.at("recording")); if (windows.empty() && failure.empty()) result["captureConclusion"] = "no-recording-windows"; }
  for (auto& [key, row] : requests) if (!written.contains(key)) { row["conclusion"] = "open-at-stop"; row["bodyConclusion"] = "not-finished-before-stop"; write_request(key); store.gap("request-open-at-stop", {{"url", field(field(row, "request"), "url")}}, false); }
  Json catalog = Json::array(); for (auto& [id, state] : sessions) catalog.push_back(state); store.json("catalog/targets.json", catalog); optional("Browser.close", Json::object(), "", false);
  if (!failure.empty()) throw std::runtime_error(failure);
}
}
