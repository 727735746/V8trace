#include "v8trace/core.h"
#include "v8trace/edition.h"
#include "v8trace/recording.h"
#include "v8trace/stream.h"
#include "v8trace/vmp_probe.h"
#include "v8trace/studio.h"
#include "v8trace/mcp.h"
#include "v8trace/causality.h"
#include "v8trace/causal_queries.h"
#include "v8trace/share.h"
#include <csignal>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>

namespace v8trace {
namespace {
std::mutex output_mutex;
std::atomic_bool interrupted = false;
std::atomic_bool file_output_failed = false;
bool file_output_only = false;
fs::path file_output_path;
std::ofstream file_output_stream;
void capture_output_path(const Json& options) {
  if (!file_output_only || !field(options,"output").is_string() || str(options,"output").empty()) return;
  auto target=fs::absolute(path(str(options,"output"))).lexically_normal();
  if (!target.has_filename()) target=target.parent_path();
  if (target.empty() || target==target.root_path()) return;
  target += ".runtime.jsonl";file_output_path=std::move(target);
}
void send(const Json& value) {
  std::lock_guard lock(output_mutex);
  if (file_output_only) {
    if (file_output_failed) return;
    try {
      if (!file_output_stream.is_open()) {
        fs::create_directories(file_output_path.parent_path());
        file_output_stream.open(file_output_path,std::ios::binary|std::ios::app);
      }
      file_output_stream<<dump(Json{{"protocol","v8trace/runtime-log/1"},{"observedAt",now()},{"record",value}})<<'\n';file_output_stream.flush();
      if (!file_output_stream) throw std::runtime_error("Runtime log write failed");
    } catch (...) {file_output_failed=true;interrupted=true;}
    return;
  }
  std::cout<<dump(value)<<'\n';std::cout.flush();
}
void interrupt(int) { interrupted = true; }
void send_identity(Json value) { value["edition"] = edition_info(); send(value); }
Json preview(const Json& event) { return event_preview(event); }
}
int worker(const fs::path& assets) {
  std::atomic_bool active = false, cancel = false; std::thread task; std::shared_ptr<RecordingControl> recording; std::unique_ptr<EvidenceViewer> viewer; std::unique_ptr<CausalityEngine> causal;
  std::unique_ptr<EvidenceViewer> causal_viewer;
  auto join = [&] { if (task.joinable()) task.join(); };
  auto templates = load_json(assets / "case_prompt_templates.json", 1048576).at("templates"); Json choices = Json::array(); for (auto& item : templates) if (edition_prompt_available(str(item,"id"))) choices.push_back({{"id", item.at("id")}, {"label", item.at("label")}, {"description", item.at("description")}});
  send_identity({{"type", "ready"}, {"protocol", 1}, {"runtime", "cpp"}, {"runtimeVersion", V8TRACE_VERSION}, {"backend", "dual-mode"}, {"transports", Json::array({"native", "cdp"})}, {"presets", presets(assets)}, {"casePromptTemplates", choices}, {"detailFilterKinds", native_kinds()}, {"studioProtocol", "v8trace/studio/1"}, {"sharingCopyProtocol", "v8trace/sharing-copy/1"}, {"supportedOptions", supported_options()}, {"recordingControl", {{"protocol", "v8trace/recording/1"}, {"transports", Json::array({"native", "cdp"})}}}, {"tcp", {{"protocol", "v8trace/tcp/1"}, {"compression", "zstd"}, {"resume", true}, {"phase", "host-sealed-chunks-and-finalized-capture"}}}});
  std::string line;
  while (std::getline(std::cin, line)) {
    Json id; std::string command;
    try {
      if (line.size() > 4194304) throw std::runtime_error("GUI command exceeds 4 MiB"); auto message = parse(line); id = field(message, "id"); if (!id.is_string() || text(id).empty() || text(id).size() > 64) throw std::runtime_error("Command ID required"); command = str(message, "command");
      if (command.starts_with("causality-") && command != "causality-close") require_professional("HTTP-root causal analysis");
      if (command == "stop" || command == "shutdown") { cancel = true; send({{"type", "stopping"}, {"id", id}, {"active", active.load()}}); if (command == "shutdown") break; continue; }
      if (command == "case-prompt") { auto result = render_prompt(assets, message); result["type"] = "case-prompt"; result["id"] = id; send(result); continue; }
      if (command == "pause-recording" || command == "resume-recording") { if (!active || !recording) throw std::runtime_error("No controllable capture is active"); recording->request(command == "pause-recording", text(id)); auto status = recording->status(); status.update({{"type", "recording-pending"}, {"id", id}}); send(status); continue; }
      if (active) throw std::runtime_error("A task is already running"); join();
      if (command == "config-preview") { auto result=configuration_preview(field(message,"current",Json::object()),field(message,"incoming",Json::object()),str(message,"mode","replace")); result["structuralPreflight"]=preflight_capture(result.at("document"),assets,false);result.update({{"type","config-preview"},{"id",id}});send(result); }
      else if (command == "viewer-close") { viewer.reset();send({{"type","viewer-closed"},{"id",id}}); }
      else if (command == "causality-close") { causal.reset();causal_viewer.reset();send({{"type","causality-closed"},{"id",id}}); }
      else if (command == "preflight" || command == "events" || command == "share-preview" || command == "share-copy" || command.starts_with("viewer-") || command.starts_with("causality-")) {
        auto request=message;cancel=false;active=true;
        task=std::thread([&,request,id,command]{
          try {
            Json result;
            if(command.starts_with("causality-")) {
              if(command=="causality-index") {
                causal.reset();causal_viewer.reset();causal=std::make_unique<CausalityEngine>(path(str(request,"capture")),fs::temp_directory_path(),[&](const Json& item){auto progress=item;progress.update({{"id",id},{"type","causality-progress"}});send(progress);},&cancel);result=causal->stats();
              } else {
                if(!causal) throw std::runtime_error("Build the causal graph first");
                auto options=field(request,"options",Json::object());
                if(command=="causality-roots") result=causal->roots(options,&cancel);
                else if(command=="causality-root") result=causal->root(options,&cancel);
                else if(command=="causality-nodes") result=causal->nodes(options,&cancel);
                else if(command=="causality-checkpoints") result=causal->checkpoints(options,&cancel);
                else if(command=="causality-functions") {
                  if(!causal_viewer) causal_viewer=std::make_unique<EvidenceViewer>(path(str(causal->stats(),"capture")),[&](const Json& item){auto progress=item;progress.update({{"id",id},{"type","causality-progress"}});send(progress);},&cancel);
                  result=causal_functions(*causal,*causal_viewer,options,&cancel);
                }
                else if(command=="causality-function-read") {if(!causal_viewer)throw std::runtime_error("Query request functions first");result=causal_viewer->read(options);}
                else throw std::runtime_error("Unknown causal graph command");
              }
            }
            else if(command=="share-preview" || command=="share-copy") {
              auto observer=[&](const Json& item){auto progress=item;progress.update({{"type","share-progress"},{"id",id}});send(progress);};
              auto source=path(str(request,"source"));auto settings=field(request,"settings",Json::object());
              result=command=="share-preview"?preview_share_copy(source,settings,observer,&cancel):create_share_copy(source,path(str(request,"output")),settings,str(request,"previewBinding"),observer,&cancel);
            }
            else if(command=="preflight")result=preflight_capture(field(request,"options",Json::object()),assets);
            else if(command=="events"){
              auto directory=fs::canonical(path(str(request,"capture")));if(!viewer||str(viewer->stats(),"root")!=utf8(directory))viewer=std::make_unique<EvidenceViewer>(directory,Sink{},&cancel);
              auto page=viewer->query({{"category",request.at("category")},{"limit",field(request,"limit",200)},{"direction","backward"}});Json previews=Json::array();auto& rows=page.at("rows");
              for(auto it=rows.rbegin();it!=rows.rend();++it){auto row=*it;row["detail"]=field(row,"preview","");row["truncated"]=field(row,"previewLimited",true);previews.push_back(row);}result={{"replace",true},{"events",previews}};
            }
            else if(command=="viewer-index"){viewer.reset();viewer=std::make_unique<EvidenceViewer>(path(str(request,"capture")),[&](const Json& item){auto progress=item;progress["id"]=id;send(progress);},&cancel);result=viewer->stats();}
            else {if(!viewer)throw std::runtime_error("Build an evidence index first");auto options=field(request,"options",Json::object());if(command=="viewer-query")result=viewer->query(options);else if(command=="viewer-read")result=viewer->read(options);else if(command=="viewer-copy")result=viewer->copy(options);else throw std::runtime_error("Unknown viewer command");}
            result.update({{"type",command},{"id",id}});active=false;send(result);
          }catch(const std::exception& error){active=false;send({{"type",command=="events"?"error":"studio-error"},{"id",id},{"command",command},{"error",error.what()}});}
        });
      }
      else if (command == "start") {
        viewer.reset();causal.reset();causal_viewer.reset();
        auto options = validate_options(field(message, "options", Json::object()), assets); recording = options.at("recordingControl") == true ? std::make_shared<RecordingControl>(options.at("startPaused") == true) : nullptr; auto control = recording; cancel = false; active = true; send({{"type", "accepted"}, {"id", id}, {"output", options.at("output")}});
        task = std::thread([&, options, id, control] {
          Json previews = Json::array(); uint64_t omitted = 0; auto previous = std::chrono::steady_clock::now(); std::string previous_phase;
          auto flush = [&] { if (!previews.empty()) { send({{"type", "events"}, {"id", id}, {"events", previews}, {"droppedPreviews", omitted}}); previews = Json::array(); } };
          try { auto summary = capture(options, assets, cancel, [&](Json progress) { if (str(progress, "type") == "recording-state") { progress["id"] = field(progress, "ready") == true && !str(progress, "requestId").empty() ? progress.at("requestId") : id; send(progress); return; } auto point = std::chrono::steady_clock::now(); auto phase = str(progress, "phase"); if (phase == previous_phase && point - previous < std::chrono::milliseconds(250)) return; previous = point; previous_phase = std::move(phase); progress.update({{"type", "progress"}, {"id", id}, {"droppedPreviews", omitted}}); send(progress); flush(); }, [&](const Json& event) { if (previews.size() < 100) previews.push_back(preview(event)); else ++omitted; }, control.get()); flush(); Json completed{{"type", str(summary, "captureConclusion") == "failed" ? "error" : "completed"}, {"id", id}, {"output", options.at("output")}, {"summary", summary}, {"droppedPreviews", omitted}}; if (summary.contains("error")) completed["error"] = summary["error"]; send(completed); }
          catch (const std::exception& e) { if (control) control->finish(true); send({{"type", "error"}, {"id", id}, {"error", e.what()}, {"output", options.at("output")}}); } active = false;
        });
      } else if (command == "tcp-send") {
        auto directory = path(str(message, "capture")); auto options = field(message, "options", Json::object()); recording.reset(); cancel = false; active = true;
        task = std::thread([&, directory, options, id] { try { auto receipt = tcp_send(directory, options, [&](Json value) { value["id"] = id; send(value); }, &cancel); send({{"type", "transferred"}, {"id", id}, {"summary", receipt}, {"output", utf8(directory)}}); } catch (const std::exception& e) { send({{"type", "error"}, {"id", id}, {"error", e.what()}}); } active = false; });
      } else if (command == "inspect") { auto inspected = inspect_capture(path(str(message, "capture"))); send({{"type", "inspected"}, {"id", id}, {"output", inspected.at("root")}, {"summary", inspected.at("summary")}}); }
      else if (command == "export") { auto output = fs::absolute(path(str(message, "output"))); auto summary = export_case(path(str(message, "capture")), output); send({{"type", "exported"}, {"id", id}, {"output", utf8(output)}, {"summary", summary}}); }
      else throw std::runtime_error("Unknown GUI command: " + command);
    } catch (const std::exception& e) { send({{"type", command == "case-prompt" ? "case-prompt-error" : (command == "pause-recording" || command == "resume-recording") ? "recording-control-error" : (command == "config-preview" || command == "preflight" || command.starts_with("viewer-") || command.starts_with("causality-")) ? "studio-error" : "error"}, {"id", id}, {"error", e.what()}}); }
  }
  cancel = true; join(); return 0;
}
int runtime_main(int argc, char** argv) {
  auto arguments = platform_arguments(argc, argv); std::signal(SIGINT, interrupt); std::signal(SIGTERM, interrupt);
  Json flags = Json::object(); std::string command = arguments.size() > 1 ? arguments[1] : "help";
  file_output_only=command=="capture"||command=="import";
  if(file_output_only)file_output_path=fs::absolute("v8trace-runtime.jsonl");
  for (size_t i = 2; i < arguments.size(); ++i) { if (!arguments[i].starts_with("--")) throw std::runtime_error("Expected --option"); auto key = arguments[i].substr(2); if (key == "once") flags[key] = true; else { if (++i >= arguments.size()) throw std::runtime_error("Missing option value"); flags[key] = arguments[i]; } }
  auto root = executable_path().parent_path().parent_path(); auto assets = flags.contains("assets") ? path(str(flags, "assets")) : root / "app" / "v8-trace";
  if (command == "--version" || command == "version") { send_identity({{"runtime", "cpp"}, {"version", V8TRACE_VERSION}, {"platform", platform_name()}, {"tcpProtocol", "v8trace/tcp/1"}}); return 0; }
  if (command == "--worker" || command == "worker") return worker(assets);
  if (command == "mcp") return run_mcp(flags,interrupted);
  if (command == "vmp-discover") {
    require_professional("JSVMP discovery");
    Json options = Json::object();
    if (flags.contains("site")) options["site"] = flags.at("site");
    for (auto key : {"maxCandidates", "maxPendingReads"}) if (flags.contains(key)) options[key] = decimal(str(flags, key));
    auto report = discover_vmp(path(str(flags, "trace")), path(str(flags, "output")), options); send(report); return 0;
  }
  if (command == "preflight") { auto report=preflight_capture(load_configuration(path(str(flags,"config"))),assets);send(report);return report.at("ok")==true?0:2; }
  if (command == "config-preview") { auto report=configuration_preview(load_configuration(path(str(flags,"current"))),load_configuration(path(str(flags,"incoming"))),str(flags,"mode","replace"));if(flags.contains("output"))write_file(path(str(flags,"output")),dump(report.at("document"),2)+"\n",true);send(report);return 0; }
  if (command == "query") { EvidenceViewer viewer(path(str(flags,"capture")));Json options=Json::object();for(auto key:{"category","script","function","request","window","call","operation","activation","process","isolate","producer","session","context","url","identity","causalProducer","causalEpoch","causalTask","callExact","parentCall","httpResource","after"})if(flags.contains(key))options[key]=flags.at(key);if(flags.contains("limit"))options["limit"]=decimal(str(flags,"limit"));send(viewer.query(options));return 0; }
  if (command == "capture" || command == "plan" || command == "import") {
    auto options = configuration_options(load_configuration(path(str(flags, "config")))); if (flags.contains("output")) options["output"] = flags.at("output");
    capture_output_path(options);
    if (command == "plan") { auto normalized = validate_options(options, assets); send(normalized); return 0; }
    if (command == "capture") { auto summary = capture(options, assets, interrupted, send); send(summary); if (str(summary, "captureConclusion") == "failed") return 2; if (flags.contains("tcp-host")) { Json tcp_options{{"host", flags.at("tcp-host")}, {"port", flags.contains("tcp-port") ? Json(decimal(str(flags, "tcp-port"))) : Json(17880)}}; if (flags.contains("token-file")) tcp_options["tokenFile"] = flags.at("token-file"); auto receipt = tcp_send(path(str(options, "output")), tcp_options, send, &interrupted); write_json(path(str(options, "output")) / "tcp-transfer.json", receipt); send(receipt); } return 0; }
    auto o = validate_options(options, assets, false); auto source = fs::canonical(path(str(flags, "trace"))); if(!fs::is_regular_file(source))throw std::runtime_error("Import trace must be a regular file"); auto output = path(str(o, "output")); fs::create_directories(output.parent_path()); Store store(output, o); auto raw = store.filename("native-output/trace.json"); fs::copy_file(source, raw);
    store.json("capture-options.json", {{"schemaVersion",1},{"protocol","v8trace/capture-options/1"},{"options",o}});
    Trace trace(raw); if (!trace.has_array) throw std::runtime_error("Trace has no traceEvents"); auto stats = diagnostics(trace.properties);
    if (field(stats, "finalFlushSucceeded") != true) store.gap("native-trace-flush-not-verified", stats);
    for (auto& loss : stats.at("losses")) store.gap("native-trace-data-loss", loss);
    for (const auto& counter : stats.at("globalCounters")) store.gap("native-trace-service-global-counter", counter, false);
    store.json("native-decoder.json", {{"runtime", "cpp"}, {"eventCount", trace.event_count}, {"ordering", trace.sort_statistics}, {"streaming", true}, {"externalSortMemoryBytes", 8388608}});
    import_trace(trace, store, o); if (o.at("correlations") == true) correlate(store); auto summary = store.finalize({{"backend", "chromium-native"}, {"transport", "native"}, {"runtime", "cpp"}, {"captureConclusion", "imported"}, {"sourceTraceSha256", file_sha256(raw)}, {"selectedCategories", o.at("categories")}}); send(summary); return 0;
  }
  if (command == "inspect") { send(inspect_capture(path(str(flags, "capture")))); return 0; }
  if (command == "share-preview" || command == "share-copy") { auto settings=load_json(path(str(flags,"settings")),65536);auto source=path(str(flags,"source"));send(command=="share-preview"?preview_share_copy(source,settings,{},&interrupted):create_share_copy(source,path(str(flags,"output")),settings,str(flags,"binding"),{},&interrupted));return 0; }
  if (command == "stream-pack") { send(stream_pack(path(str(flags, "capture")), path(str(flags, "spool")))); return 0; }
  if (command == "vmp-spec") { auto spec=vmp_spec_template(path(str(flags,"source")),str(flags,"url")); write_file(path(str(flags,"output")),dump(spec,2)+"\n",true);send({{"protocol","v8trace/vmp-spec/1"},{"confirmed",false},{"output",str(flags,"output")},{"requiresExplicitConfirmation",true}});return 0; }
  if (command == "stream-unpack") { send(stream_unpack(path(str(flags, "spool")), path(str(flags, "output")), str(flags, "stream-id"))); return 0; }
  if (command == "stream-decode") { send(stream_decode(path(str(flags, "chunk")), path(str(flags, "output")))); return 0; }
  if (command == "stream-send") { auto options = flags; if (flags.contains("port")) options["port"] = decimal(str(flags, "port")); if (flags.contains("token-file")) options["tokenFile"] = flags.at("token-file"); send(stream_send(path(str(flags, "spool")), options, send, &interrupted)); return 0; }
  if (command == "export") { send(export_case(path(str(flags, "capture")), path(str(flags, "output")))); return 0; }
  if (command == "send" || command == "receive") { Json options = flags; for (auto key : {"port", "maxBytes"}) if (flags.contains(key)) options[key] = decimal(str(flags, key)); if (flags.contains("token-file")) options["tokenFile"] = flags.at("token-file"); if (command == "send") { auto directory = path(str(flags, "capture")); auto receipt = tcp_send(directory, options, send, &interrupted); write_json(directory / "tcp-transfer.json", receipt); send(receipt); return 0; } return tcp_receive(options, interrupted, send); }
  std::cout << "v8trace-runtime capture|plan --config FILE [--assets DIR]\n"
    "v8trace-runtime mcp --transport stdio|http --source-root DIR [--index-dir DIR] [--export-root DIR] [--port 17881] [--token-file FILE]\n"
    "v8trace-runtime preflight --config FILE [--assets DIR]\n"
    "v8trace-runtime config-preview --current FILE --incoming FILE --mode merge|replace [--output NEW_JSON]\n"
    "v8trace-runtime query --capture DIR [--script URL_OR_ID] [--function NAME_OR_ID] [--request ID] [--window ID] [--after ID] [--limit 100]\n"
    "v8trace-runtime vmp-discover --trace FILE --output NEW_DIR [--site CANDIDATE_SHA256]\n"
    "v8trace-runtime vmp-spec --source FILE --url SCRIPT_URL --output NEW_JSON\n"
    "v8trace-runtime import --config FILE --trace FILE --output DIR\n"
    "v8trace-runtime inspect|export --capture DIR [--output DIR]\n"
    "v8trace-runtime send --capture DIR --host HOST --port 17880 [--token-file FILE]\n"
    "v8trace-runtime receive --root DIR --bind 127.0.0.1 --port 17880 [--once]\n"
    "v8trace-runtime stream-pack --capture DIR --spool NEW_DIR\n"
    "v8trace-runtime stream-unpack --spool DIR --output NEW_DIR [--stream-id ID]\n"
    "v8trace-runtime stream-send --spool DIR --host HOST --port 17880 [--token-file FILE]\n"
    "v8trace-runtime stream-decode --chunk DIR --output NEW_FILE\n"
    "v8trace-runtime --worker [--assets DIR]\n"; return command == "help" || command == "--help" ? 0 : 1;
}
}
int main(int argc, char** argv) { try { auto result=v8trace::runtime_main(argc,argv);return v8trace::file_output_failed?74:result; } catch (const std::exception& e) {auto error=v8trace::Json{{"type","error"},{"error",e.what()}};if(v8trace::file_output_only){v8trace::send(error);return v8trace::file_output_failed?74:1;}std::cerr<<v8trace::dump(error)<<'\n';return 1;} }
