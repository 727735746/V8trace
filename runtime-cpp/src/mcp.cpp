#include "v8trace/mcp.h"
#include "v8trace/edition.h"
#include "v8trace/causality.h"
#include "v8trace/causal_queries.h"
#include "v8trace/studio.h"
#include "trace_search_internal.h"
#include <algorithm>
#include <future>
#include <iostream>
#include <limits>
#include <mutex>

namespace v8trace {
using trace_search::require;
namespace {
McpConfig index_io_config(McpConfig value) { value.search.index_dir = io_path(value.search.index_dir); return value; }
constexpr std::string_view modern = "2026-07-28", legacy = "2025-11-25";
constexpr std::string_view version_key = "io.modelcontextprotocol/protocolVersion";
const std::string instructions = "C++ trace search. One explicit process-wide active line file and one capture; close before switching. Open immutable finalized logs. Poll get_file_status until ready. Line numbers are 1-based; next_line is the continuation cursor, never a silently truncated success. Start with small bounded queries. Search does not execute log content. V8 metadata links are evidence, not proof of complete synchronous/asynchronous causality or restored algorithms. Exact raw bytes are available through export_lines or v8trace_read_evidence base64.";
struct RpcFailure : std::runtime_error {
  int code,status; Json data;
  RpcFailure(int c,std::string message,int s=400,Json d=nullptr):std::runtime_error(std::move(message)),code(c),status(s),data(std::move(d)){}
};
void rpc_require(bool condition,int code,const std::string& message,int status=400,Json data=nullptr){if(!condition)throw RpcFailure(code,message,status,std::move(data));}
Json schema(std::string type,std::string description="") { Json value{{"type",std::move(type)}}; if(!description.empty())value["description"]=std::move(description);return value; }
Json integer(uint64_t minimum=0){return {{"type","integer"},{"minimum",minimum}};}
Json nullable(Json value){value["type"]=Json::array({value.at("type"),"null"});return value;}
Json tool(std::string name,std::string description,Json properties,Json required,bool readonly=true){
  return {{"name",std::move(name)},{"description",std::move(description)},{"inputSchema",{{"type","object"},{"properties",std::move(properties)},{"required",std::move(required)}}},{"annotations",{{"readOnlyHint",readonly},{"destructiveHint",!readonly},{"openWorldHint",false}}}};
}
Json tools(){
  auto empty=Json::array();Json list=Json::array();auto start=integer(1),count=integer();
  list.push_back(tool("open_file","Open one immutable log under source-root; background sparse index. Poll get_file_status. Close before another file.",{{"path",schema("string")},{"force_rebuild",schema("boolean")}},Json::array({"path"})));
  list.push_back(tool("get_file_status","Index state, bytes, line count, speed, ETA or failure. One active line file per server process.",Json::object(),empty));
  list.push_back(tool("close_file","Cancel current index admission and release active file; existing read snapshots finish, reusable cache retained.",Json::object(),empty));
  list.push_back(tool("read_lines","Read exact line span with original CRLF/LF retained; lossy_utf8 declares invalid-byte replacement. Use small spans or export raw bytes.",{{"start_line",start},{"line_count",count}},Json::array({"start_line","line_count"})));
  list.push_back(tool("search_lines","Ordered literal or bounded RE2 UTF-8 regex search. No cross-line matching. Regex is RE2, not the Rust regex dialect. Resume at next_line; all limit flags are explicit.",{{"pattern",schema("string")},{"start_line",start},{"max_scan_lines",count},{"max_matches",nullable(integer(1))},{"regex",nullable(schema("boolean"))},{"case_sensitive",nullable(schema("boolean"))}},Json::array({"pattern","start_line","max_scan_lines"})));
  list.push_back(tool("export_lines","Atomically export raw line bytes only under export-root. Parent must exist. Existing destination requires overwrite=true. Source is never overwritten.",{{"start_line",start},{"line_count",count},{"output_path",schema("string")},{"overwrite",schema("boolean")}},Json::array({"start_line","line_count","output_path"}),false));
  list.push_back(tool("v8trace_open_capture","Index finalized V8Trace capture under source-root. Long call is cancellable; v8trace_capture_status reports progress. Does not launch a browser.",{{"capture",schema("string")}},Json::array({"capture"})));
  list.push_back(tool("v8trace_capture_status","Current capture index state/progress and real indexing boundaries; no invented causal completion.",Json::object(),empty));
  list.push_back(tool("v8trace_close_capture","Cancel pending capture indexing and release the capture; existing queries retain their snapshot.",Json::object(),empty));
  Json filters=Json::object();for(auto key:{"category","script","function","request","window","call","operation","activation","process","isolate","producer","session","context","url","identity","causalProducer","causalEpoch","causalTask","callExact","parentCall","httpResource","after"})filters[key]=schema("string");filters["limit"]={{"type","integer"},{"minimum",1},{"maximum",200}};filters["direction"]={{"type","string"},{"enum",Json::array({"forward","backward"})}};
  list.push_back(tool("v8trace_query","Exact metadata filters for JSCall/opcode/native/HTTP/script evidence. Returns source file, line, hash, rowId, generation, preview boundaries and nextCursor. These are viewer-only filters, not producer instrumentation.",filters,empty));
  list.push_back(tool("v8trace_read_evidence","Read a rowId or validated content-addressed blob reference in exact base64 chunks (up to 65536 bytes). Pass generation from query to reject stale rows; use nextOffset until hasMore=false.",{{"rowId",schema("string")},{"generation",schema("string")},{"reference",schema("object")},{"offset",integer()},{"limit",{{"type","integer"},{"minimum",1},{"maximum",65536}}}},empty));
  if (!professional_edition()) return list;
  Json causal_page{{"limit",{{"type","integer"},{"minimum",1},{"maximum",500}}},{"cursor",schema("string")},{"offset",integer()},{"generation",schema("string")}};
  list.push_back(tool("v8trace_causal_roots","Index native HTTP-root causal evidence and page observed roots with real completion boundaries. Read-only source; no idle-based completion or algorithm restoration claim.",causal_page,empty));
  auto causal_root=causal_page;causal_root["rootId"]=schema("string");
  causal_root["direction"]={{"type","string"},{"enum",Json::array({"upstream","downstream"})}};
  list.push_back(tool("v8trace_causal_root","Inspect an exact opaque HTTP root, pending nodes, proof status and reasons. Pass rootId and generation from causal_roots.",causal_root,Json::array({"rootId","generation"})));
  list.push_back(tool("v8trace_causal_nodes","Page exact root descendants including sync/async/resource and matched native wire edges. Missing/ambiguous bridges remain explicit gaps.",causal_root,Json::array({"rootId","generation"})));
  causal_root["kindPrefix"]={{"type","string"},{"enum",Json::array({"","crypto-","dom-","http-"})}};
  list.push_back(tool("v8trace_causal_checkpoints","Page observed crypto copied-input/result, DOM and HTTP checkpoints on the request's proven ancestor path by default. Includes exact source references and raw byte chunks. Structural provenance is not full parameter dataflow or custom-VM semantic recovery.",causal_root,Json::array({"rootId","generation"})));
  auto function_options=causal_root;function_options.erase("offset");function_options.erase("kindPrefix");function_options["limit"]={{"type","integer"},{"minimum",1},{"maximum",200}};function_options["channel"]={{"type","string"},{"enum",Json::array({"jscall","native","opcode","wasm","http"})}};function_options["includeAfterRequest"]=schema("boolean");function_options["script"]=schema("string");function_options["function"]=schema("string");
  list.push_back(tool("v8trace_request_functions","Page every recorded selected function/method activation on an HTTP root's exact sync/async causal path, including recursion and repeated calls. Use rowId and row generation with read_evidence for full arguments/results/exceptions. Request position is exact only where producer-local counters prove it; unknown positions remain explicit.",function_options,Json::array({"rootId","generation"})));
  return list;
}
void validate_arguments(const Json& definition,const Json& arguments){
  rpc_require(arguments.is_object(),-32602,"Tool arguments must be an object");auto s=definition.at("inputSchema");
  for(const auto& key:s.at("required"))rpc_require(arguments.contains(key.get<std::string>()),-32602,"Missing argument: "+key.get<std::string>());
  for(const auto& [key,rule]:s.at("properties").items()){
    if(!arguments.contains(key))continue;const auto& item=arguments.at(key);auto type=rule.at("type");
    auto accepts=[&](const std::string& expected){if(expected=="null")return item.is_null();if(expected=="string")return item.is_string();if(expected=="boolean")return item.is_boolean();if(expected=="object")return item.is_object();if(expected=="integer")return item.is_number_integer();return false;};
    bool valid=false;if(type.is_array()){for(const auto& choice:type)valid=valid||accepts(choice.get<std::string>());}else valid=accepts(type.get<std::string>());
    rpc_require(valid,-32602,"Invalid argument type: "+key);
    if(item.is_number_integer()){
      if(rule.contains("minimum"))rpc_require((item.is_number_unsigned()||item.get<int64_t>()>=0)&&item.get<uint64_t>()>=rule.at("minimum").get<uint64_t>(),-32602,"Argument below minimum: "+key);
      if(rule.contains("maximum"))rpc_require(item.get<uint64_t>()<=rule.at("maximum").get<uint64_t>(),-32602,"Argument above maximum: "+key);
    }
    if(rule.contains("enum"))rpc_require(std::find(rule.at("enum").begin(),rule.at("enum").end(),item)!=rule.at("enum").end(),-32602,"Invalid argument value: "+key);
  }
}
std::string decoded_header(std::string value){
  if(value.starts_with("=?base64?")&&value.ends_with("?=")){
    try{auto encoded=value.substr(9,value.size()-11);auto decoded=unbase64(encoded);rpc_require(base64(decoded)==encoded,-32020,"Malformed encoded Mcp-Name header");return decoded;}catch(const RpcFailure&){throw;}catch(...){throw RpcFailure(-32020,"Malformed encoded Mcp-Name header");}
  }
  return value;
}
std::string era(const Json& request,McpContext& context){
  auto params=field(request,"params",Json::object()),meta=field(params,"_meta",Json::object());auto version=str(meta,version_key);auto method=str(request,"method");
  const bool is_modern=!version.empty()||method=="server/discover"||(context.http&&context.headers["mcp-protocol-version"]==modern);
  if(is_modern){
    rpc_require(!version.empty(),-32602,"Missing per-request protocol version metadata");
    if(context.http){
      rpc_require(context.headers["mcp-protocol-version"]==version&&context.headers["mcp-method"]==method,-32020,"Required protocol/method headers are missing or differ from body");
      if(method=="tools/call")rpc_require(context.headers.contains("mcp-name")&&decoded_header(context.headers["mcp-name"])==str(params,"name"),-32020,"Required Mcp-Name header is missing or differs from body");
    }
    rpc_require(version==modern,-32022,"Unsupported protocol version",400,{{"supported",Json::array({modern})},{"requested",version}});
    auto info=field(meta,"io.modelcontextprotocol/clientInfo");rpc_require(info.is_object()&&field(info,"name").is_string()&&field(info,"version").is_string()&&field(meta,"io.modelcontextprotocol/clientCapabilities").is_object(),-32602,"Per-request clientInfo and clientCapabilities metadata are required");
    return std::string(modern);
  }
  if(method=="initialize")return std::string(legacy);
  auto requested=context.http?context.headers["mcp-protocol-version"]:context.legacy_version;
  rpc_require(requested==legacy,-32602,"Initialize a 2025-11-25 client or supply modern per-request metadata");return requested;
}
}
Json mcp_error(const Json& id,int code,std::string message,Json data){Json error{{"code",code},{"message",std::move(message)}};if(!data.is_null())error["data"]=std::move(data);return {{"jsonrpc","2.0"},{"id",id},{"error",std::move(error)}};}
struct McpServer::Impl {
  McpConfig config; TraceSearchEngine engine; Json definitions=tools();
  struct Viewer { std::unique_ptr<EvidenceViewer> value; std::unique_ptr<CausalityEngine> causal; std::mutex mutex; };
  mutable std::mutex mutex;std::shared_ptr<Viewer> viewer;std::shared_ptr<std::atomic_bool> build_cancel;Json capture_status{{"state","no_capture"}};uint64_t generation=0;bool building=false;
  std::atomic<uint64_t> active{0},completed{0},cancelled{0};
  explicit Impl(McpConfig value):config(index_io_config(std::move(value))),engine(config.search){config.search.source_root=fs::canonical(config.search.source_root);config.search.index_dir=io_path(fs::canonical(config.search.index_dir));config.search.export_root=fs::canonical(config.search.export_root);}
  Json status() const {std::lock_guard lock(mutex);return capture_status;}
  Json capture_open(const Json& arguments,const std::shared_ptr<std::atomic_bool>& stop){
    require(str(arguments,"capture").find('\0')==std::string::npos,"Capture path contains NUL");auto requested=path(str(arguments,"capture"));if(!requested.is_absolute())requested=config.search.source_root/requested;auto root=fs::canonical(requested);
    require((fs::equivalent(root,config.search.source_root)||inside(config.search.source_root,root))&&fs::is_directory(root),"Capture leaves source root or is not a directory");uint64_t id;
    {std::lock_guard lock(mutex);require(!building,"A capture index is already building or stopping");require(!viewer,"Close the active capture before opening another");building=true;build_cancel=stop;id=++generation;capture_status={{"state","indexing"},{"root",utf8(root)},{"records",0}};}
    try{
      auto slot=std::make_shared<Viewer>();slot->value=std::make_unique<EvidenceViewer>(root,[&](const Json& progress){std::lock_guard lock(mutex);if(generation==id)capture_status.update(progress);},stop.get(),config.search.index_dir);trace_search::cancelled(stop.get());auto stats=slot->value->stats();stats["indexOwner"]=utf8(logical_path(config.search.index_dir));stats["completeCausality"]=false;stats["algorithmRestored"]=false;
      {std::lock_guard lock(mutex);require(generation==id,"Capture index was closed before publication");building=false;build_cancel.reset();viewer=slot;capture_status=stats;capture_status["state"]="ready";}
      return stats;
    }catch(const std::exception& error){std::lock_guard lock(mutex);if(build_cancel==stop){building=false;build_cancel.reset();}if(generation==id)capture_status={{"state","failed"},{"root",utf8(root)},{"error",trace_search::lossy_utf8(error.what()).first}};throw;}
  }
  Json call(const std::string& name,const Json& arguments,const std::shared_ptr<std::atomic_bool>& stop){
    if(name=="open_file")return engine.open(arguments);if(name=="get_file_status")return engine.status();if(name=="close_file")return engine.close();if(name=="read_lines")return engine.read(arguments,stop.get());if(name=="search_lines")return engine.search(arguments,stop.get());if(name=="export_lines")return engine.export_lines(arguments,stop.get());
    if(name=="v8trace_open_capture")return capture_open(arguments,stop);if(name=="v8trace_capture_status")return status();
    if(name=="v8trace_close_capture"){std::shared_ptr<Viewer> previous;{std::lock_guard lock(mutex);++generation;if(build_cancel)*build_cancel=true;previous=std::move(viewer);capture_status={{"state","no_capture"}};}return status();}
    std::shared_ptr<Viewer> snapshot;{std::lock_guard lock(mutex);snapshot=viewer;}require(bool(snapshot),"Capture is not ready");std::unique_lock lock(snapshot->mutex,std::defer_lock);while(!lock.try_lock()){trace_search::cancelled(stop.get());std::this_thread::sleep_for(std::chrono::milliseconds(20));}trace_search::cancelled(stop.get());
    if(name.starts_with("v8trace_causal_") || name=="v8trace_request_functions") {
      require_professional("HTTP-root causal analysis");
      if(!snapshot->causal) snapshot->causal=std::make_unique<CausalityEngine>(path(str(snapshot->value->stats(),"root")),config.search.index_dir,Sink{},stop.get());
      if(name=="v8trace_causal_roots")return snapshot->causal->roots(arguments,stop.get());
      if(name=="v8trace_causal_root")return snapshot->causal->root(arguments,stop.get());
      if(name=="v8trace_causal_nodes")return snapshot->causal->nodes(arguments,stop.get());
      if(name=="v8trace_causal_checkpoints")return snapshot->causal->checkpoints(arguments,stop.get());
      if(name=="v8trace_request_functions")return causal_functions(*snapshot->causal,*snapshot->value,arguments,stop.get());
    }
    if(name=="v8trace_query")return snapshot->value->query(arguments);
    if(name=="v8trace_read_evidence"){require(arguments.contains("rowId")!=arguments.contains("reference"),"Select exactly one rowId or blob reference");if(arguments.contains("rowId"))require(arguments.contains("generation"),"Pass generation from the selected row");return snapshot->value->read(arguments);}
    throw std::runtime_error("Unknown tool");
  }
};
McpServer::McpServer(McpConfig config):impl_(std::make_unique<Impl>(std::move(config))) {require(impl_->config.max_request_bytes>=1024&&impl_->config.max_request_bytes<=16777216&&impl_->config.max_response_bytes>=4096&&impl_->config.max_response_bytes<=536870912,"Invalid MCP wire budgets");}
McpServer::~McpServer()=default;
const McpConfig& McpServer::config()const{return impl_->config;}
Json McpServer::info()const{return {{"runtime","cpp"},{"server","v8trace-search"},{"version",V8TRACE_VERSION},{"edition",edition_info()},{"sourceRoot",utf8(config().search.source_root)},{"indexRoot",utf8(logical_path(config().search.index_dir))},{"exportRoot",utf8(config().search.export_root)},{"maxRequestBytes",config().max_request_bytes},{"maxResponseBytes",config().max_response_bytes},{"protocolVersions",Json::array({modern,legacy})},{"tools",impl_->definitions.size()},{"activeRequests",impl_->active.load()},{"completedRequests",impl_->completed.load()},{"cancelledRequests",impl_->cancelled.load()}};}
McpReply McpServer::dispatch(const Json& request,McpContext& context,const std::shared_ptr<std::atomic_bool>& stop,bool validate_only){
  Json id=nullptr;
  try{
    rpc_require(request.is_object()&&str(request,"jsonrpc")=="2.0"&&field(request,"method").is_string(),-32600,"Invalid JSON-RPC request");if(request.contains("id")){auto candidate=request.at("id");rpc_require(candidate.is_string()||candidate.is_number_integer(),-32600,"Request id must be a string or integer");rpc_require(!candidate.is_string()||candidate.get_ref<const std::string&>().size()<=256,-32600,"Request id is too long");id=std::move(candidate);}
    auto params=field(request,"params",Json::object());rpc_require(params.is_object(),-32602,"Request params must be an object");auto protocol=era(request,context);bool newer=protocol==modern;auto method=str(request,"method");
    if(!request.contains("id")){rpc_require(method.starts_with("notifications/"),-32600,"Only protocol notifications may omit id");return {202,{}};}
    Json result;
    if(method=="server/discover")result={{"supportedVersions",Json::array({modern})},{"capabilities",{{"tools",Json::object()}}},{"_meta",{{"io.modelcontextprotocol/serverInfo",{{"name","v8trace-search"},{"version",V8TRACE_VERSION}}}}},{"instructions",instructions}};
    else if(method=="initialize"){
      rpc_require(!newer&&field(params,"protocolVersion").is_string()&&field(params,"capabilities").is_object()&&field(params,"clientInfo").is_object(),-32602,"Invalid legacy initialize parameters");context.legacy_version=std::string(legacy);result={{"protocolVersion",legacy},{"capabilities",{{"tools",Json::object()}}},{"serverInfo",{{"name","v8trace-search"},{"version",V8TRACE_VERSION}}},{"instructions",instructions}};
    }else if(method=="ping")result=Json::object();
    else if(method=="tools/list"){rpc_require(!params.contains("cursor")||params["cursor"].is_null(),-32602,"This bounded tool list has no continuation cursor");result={{"tools",impl_->definitions}};}
    else if(method=="tools/call"){
      auto name=str(params,"name");auto definition=std::find_if(impl_->definitions.begin(),impl_->definitions.end(),[&](const Json& item){return str(item,"name")==name;});rpc_require(definition!=impl_->definitions.end(),-32602,"Unknown tool name");auto arguments=field(params,"arguments",Json::object());validate_arguments(*definition,arguments);
      if(validate_only)return {200,{}};
      ++impl_->active;
      try{trace_search::cancelled(stop.get());auto data=impl_->call(name,arguments,stop);result={{"content",Json::array({{{"type","text"},{"text",dump(data)}}})},{"structuredContent",std::move(data)},{"isError",false}};}
      catch(const std::exception& error){auto message=trace_search::lossy_utf8(error.what()).first;result={{"content",Json::array({{{"type","text"},{"text",message}}})},{"structuredContent",{{"error",message},{"cancelled",stop->load()}}},{"isError",true}};}
      if(stop->load())++impl_->cancelled;--impl_->active;++impl_->completed;
    }else throw RpcFailure(-32601,"Method not found",404);
    if(newer)result["resultType"]="complete";return {200,Json{{"jsonrpc","2.0"},{"id",id},{"result",std::move(result)}}};
  }catch(const RpcFailure& error){return {error.status,mcp_error(id,error.code,error.what(),error.data)};}
  catch(const std::exception& error){return {500,mcp_error(id,-32603,trace_search::lossy_utf8(error.what()).first)};}
}
std::string McpServer::encode(McpReply& reply)const{
  if(!reply.body)return {};auto encoded=dump(*reply.body);if(encoded.size()>config().max_response_bytes){auto id=field(*reply.body,"id");bool newer=str(field(*reply.body,"result"),"resultType")=="complete";reply.status=200;reply.body=Json{{"jsonrpc","2.0"},{"id",id},{"result",{{"isError",true},{"structuredContent",{{"error","mcp-response-byte-budget"},{"maxResponseBytes",config().max_response_bytes}}},{"content",Json::array({{{"type","text"},{"text","Response exceeds MCP byte budget; request fewer lines/matches or export_lines. No raw source was truncated."}}})}}}};if(newer)(*reply.body)["result"]["resultType"]="complete";encoded=dump(*reply.body);}return encoded;
}
int mcp_stdio(McpServer& server,std::atomic_bool& stop){
  struct Job{std::string key;std::shared_ptr<std::atomic_bool> cancel=std::make_shared<std::atomic_bool>(false);std::atomic_bool done{false};std::thread thread;};
  std::vector<std::shared_ptr<Job>> jobs;std::map<std::string,std::shared_ptr<Job>> active;std::mutex output;McpContext channel;
  auto send=[&](McpReply reply){auto wire=server.encode(reply);if(wire.empty())return;std::lock_guard lock(output);std::cout<<wire<<'\n';std::cout.flush();};
  auto reap=[&]{for(auto it=jobs.begin();it!=jobs.end();)if((*it)->done){if((*it)->thread.joinable())(*it)->thread.join();active.erase((*it)->key);it=jobs.erase(it);}else++it;};
  std::string line;bool oversized=false;char c;
  while(!stop&&std::cin.get(c)){
    if(c!='\n'){if(line.size()<server.config().max_request_bytes&&!oversized)line+=c;else oversized=true;continue;}
    reap();if(oversized){send({400,mcp_error(nullptr,-32600,"Request exceeds MCP byte budget")});line.clear();oversized=false;continue;}
    try{
      auto request=parse(line);line.clear();auto method=str(request,"method");
      if(method=="notifications/cancelled"&&!request.contains("id")){auto id=field(field(request,"params"),"requestId");auto found=active.find(dump(id));if(found!=active.end())*found->second->cancel=true;continue;}
      if(method=="initialize"||method=="server/discover"||!request.contains("id")){send(server.dispatch(request,channel,std::make_shared<std::atomic_bool>(false)));continue;}
      auto key=dump(field(request,"id"));if(active.contains(key)){send({400,mcp_error(field(request,"id"),-32600,"Duplicate in-flight request id")});continue;}if(jobs.size()>=server.config().connections){send({429,mcp_error(field(request,"id"),-32603,"MCP request concurrency budget is busy; retry later")});continue;}
      auto job=std::make_shared<Job>();job->key=key;auto context=channel;active[key]=job;jobs.push_back(job);
      try{job->thread=std::thread([&,job,request=std::move(request),context]()mutable{auto reply=server.dispatch(request,context,job->cancel);job->done=true;send(std::move(reply));});}
      catch(const std::exception&){job->done=true;active.erase(key);send({500,mcp_error(field(request,"id"),-32603,"Start MCP worker thread failed")});}
    }catch(const std::exception&){line.clear();send({400,mcp_error(nullptr,-32700,"Parse error")});}
  }
  for(auto& job:jobs)*job->cancel=true;for(auto& job:jobs)if(job->thread.joinable())job->thread.join();return 0;
}
int run_mcp(const Json& flags,std::atomic_bool& stop){
  const std::set<std::string> accepted{"transport","source-root","index-dir","export-root","checkpoint-bytes","search-threads","query-concurrency","max-content-bytes","bind","port","token-file"};for(const auto& [key,value]:flags.items()){(void)value;require(accepted.contains(key),"Unknown MCP option: "+key);}
  McpConfig config;require(flags.contains("source-root"),"mcp requires --source-root");config.search.source_root=fs::absolute(path(str(flags,"source-root")));config.search.index_dir=fs::absolute(path(str(flags,"index-dir",".v8trace-search-index")));config.search.export_root=fs::absolute(path(str(flags,"export-root","trace-exports")));
  if(flags.contains("checkpoint-bytes"))config.search.checkpoint_bytes=decimal(str(flags,"checkpoint-bytes"));if(flags.contains("search-threads")){auto n=decimal(str(flags,"search-threads"));require(n<=32,"search-threads must be 0 through 32");config.search.search_threads=unsigned(n);}if(flags.contains("query-concurrency")){auto n=decimal(str(flags,"query-concurrency"));require(n>0&&n<=64,"query-concurrency must be 1 through 64");config.search.query_concurrency=unsigned(n);}if(flags.contains("max-content-bytes"))config.search.max_content_bytes=decimal(str(flags,"max-content-bytes"));
  config.bind=str(flags,"bind","127.0.0.1");if(flags.contains("port")){auto port=decimal(str(flags,"port"));require(port>0&&port<=65535,"MCP port must be 1 through 65535");config.port=unsigned(port);}if(flags.contains("token-file"))config.token_file=path(str(flags,"token-file"));McpServer server(std::move(config));auto transport=str(flags,"transport","stdio");if(transport=="stdio")return mcp_stdio(server,stop);require(transport=="http","MCP transport must be stdio or http");return mcp_http(server,stop);
}
}
