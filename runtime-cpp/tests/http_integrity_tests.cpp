#include "v8trace/core.h"
#include <algorithm>
#include <iostream>
using namespace v8trace;
namespace {
unsigned checks=0;
void check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
const std::string url="https://background.fixture.test/redirect";
const std::string prefix=std::string("partial\0",8)+char(255);
Json fixture(bool cancelled){
  Json rows=Json::array();
  auto add=[&](const char* kind,Json record){record.update({{"schema","v8trace/browser/1"},{"producer","chromium"},{"producerSeq",std::to_string(rows.size()+1)},{"channel","http"},{"kind",kind}});rows.push_back({{"name","V8TraceBrowser"},{"cat","disabled-by-default-v8.http"},{"pid",71},{"tid",4},{"ts",100+rows.size()},{"args",{{"record",dump(record)}}}});};
  add("request",{{"requestId","n1"},{"layer","network"},{"url",url},{"method","GET"},{"headers",Json::object()},{"requestBodyPresent",false}});
  Json response{{"status",302},{"statusText","Found"},{"url",url},{"bodyRepresentation","encoded-network-bytes"},{"headers",{{"content-type","application/octet-stream"},{"location","https://background.fixture.test/final"}}}};
  add("redirect-body-start",{{"requestId","d1"},{"bodyProtocol","v8trace/redirect-body/1"},{"parentRequestProtocol","v8trace/http-parent/1"},{"parentRequestId","n1"},{"url",url},{"bodyRepresentation","encoded-network-bytes"},{"response",response}});
  add("body-chunk",{{"requestId","d1"},{"direction","response"},{"offset","0"},{"bytes",prefix.size()},{"base64",base64(prefix)}});
  if(!cancelled){
    add("response-body-end",{{"requestId","d1"},{"bodyProtocol","v8trace/redirect-body/1"},{"complete",true},{"bytes",std::to_string(prefix.size())},{"errorCode",0},{"reason","eof"}});
    response["requestId"]="n1";add("response",response);
    add("redirect",{{"requestId","n1"},{"redirectBodyId","d1"},{"redirectBodyProtocol","v8trace/redirect-body/1"},{"nextUrl","https://background.fixture.test/final"}});
  }
  add("complete",{{"requestId","n1"},{"success",!cancelled},{"errorCode",cancelled?-2:0},{"bytes","0"},{"termination","url-loader-completed"}});
  if(cancelled)add("response-body-end",{{"requestId","d1"},{"bodyProtocol","v8trace/redirect-body/1"},{"complete",false},{"bytes",std::to_string(prefix.size())},{"errorCode",-3},{"reason","cancelled"}});
  return rows;
}
void edit(Json& rows,const char* kind,const std::function<void(Json&)>& action){for(auto& event:rows){auto record=parse(str(event.at("args"),"record"));if(str(record,"kind")==kind){action(record);event["args"]["record"]=dump(record);return;}}throw std::runtime_error("Missing fixture edit point");}
}
int main(){
  const auto root=fs::temp_directory_path()/("v8trace-http-integrity-"+uuid());fs::create_directory(root);
  try{
    Json options{{"transport","native"},{"http",true},{"maxBodyBytes",1048576},{"maxBytes",16777216},{"maxEvents",10000},{"categories",Json::array({"http"})}};
    auto run=[&](Json rows,const std::string& name){write_json(root/(name+".json"),{{"traceEvents",rows}});Store store(root/name,options);{Trace trace(root/(name+".json"));import_browser(trace,store,options);}return store.finalize({{"backend","chromium-native"},{"captureConclusion","collected"}});};
    auto summary=run(fixture(false),"normal");check(summary.at("criticalGapCount")==0,"normal linked redirect complete");
    summary=run(fixture(true),"cancelled-before-notify");check(summary.at("criticalGapCount")==0,"native failed request owns cancelled redirect stream before redirect callback");
    Json resource;lines(root/"cancelled-before-notify/catalog/resources.jsonl",[&](const Json& row){resource=row;});
    check(resource.at("conclusion")=="failed"&&resource.at("body").is_null(),"cancelled transfer remains failed, never full body or successful redirect");
    check(resource.at("redirect").is_null()&&resource.at("response").at("status")==302,"pre-notification redirect headers retained without fabricated redirect callback");
    check(resource.at("bodyPrefix").at("bytes")==prefix.size()&&read_file(root/"cancelled-before-notify"/str(resource.at("bodyPrefix"),"path"))==prefix,"all observed cancelled body prefix bytes retained losslessly");
    export_case(root/"cancelled-before-notify",root/"export");Json exported;lines(root/"export/replay.network.jsonl",[&](const Json& row){exported=row;});
    check(read_file(root/"export"/str(exported.at("bodyPrefix"),"path"))==prefix,"handoff export audits copies and rewrites failed-transfer prefix reference");
    auto bad=fixture(true);edit(bad,"redirect-body-start",[](Json& r){r.erase("parentRequestId");r.erase("parentRequestProtocol");});check(run(bad,"no-parent").at("criticalGapCount")>0,"legacy orphan not guessed from address URL or time");
    bad=fixture(true);edit(bad,"redirect-body-start",[](Json& r){r["parentRequestId"]="other";});check(run(bad,"wrong-parent").at("criticalGapCount")>0,"unknown native parent remains critical");
    bad=fixture(true);edit(bad,"redirect-body-start",[](Json& r){r["url"]="https://other.fixture.test/";});check(run(bad,"wrong-url").at("criticalGapCount")>0,"strong parent must agree with exact request URL");
    bad=fixture(true);edit(bad,"redirect-body-start",[](Json& r){r["parentRequestProtocol"]="unknown";});check(run(bad,"wrong-protocol").at("criticalGapCount")>0,"unrecognized parent protocol not trusted");
    bad=fixture(true);bad.erase(bad.end()-1);check(run(bad,"missing-end").at("criticalGapCount")>0,"parent failure does not excuse missing stream end");
    bad=fixture(true);edit(bad,"complete",[](Json& r){r["success"]=true;r["errorCode"]=0;});check(run(bad,"cancelled-success").at("criticalGapCount")>0,"stream cancellation on successful parent remains incomplete");
    bad=fixture(true);edit(bad,"response-body-end",[](Json& r){r["reason"]="timeout";r["errorCode"]=-7;});check(run(bad,"timeout").at("criticalGapCount")>0,"instrumentation drain timeout not reclassified as ordinary request abort");
    bad=fixture(true);edit(bad,"response-body-end",[](Json& r){r["bytes"]="0";});check(run(bad,"end-count-mismatch").at("criticalGapCount")>0,"aborted prefix end count checked");
    bad=fixture(false);edit(bad,"redirect-body-start",[](Json& r){r["parentRequestId"]="other";});check(run(bad,"conflicting-links").at("criticalGapCount")>0,"redirect callback cannot override conflicting native parent identity");
    bad=fixture(false);edit(bad,"body-chunk",[](Json& r){r["offset"]="1";});check(run(bad,"offset-gap").at("criticalGapCount")>0,"body prefix offset gap never hidden by parent attachment");
    bad=fixture(true);auto last=bad.at(bad.size()-2);auto record=parse(str(last.at("args"),"record"));record["producerSeq"]=std::to_string(bad.size()+1);last["args"]["record"]=dump(record);last["ts"]=100+bad.size();bad.push_back(last);check(run(bad,"duplicate-complete").at("criticalGapCount")>0,"duplicate request completion rejected");
    bad=fixture(true);bad.erase(bad.end()-2);check(run(bad,"open-parent").at("criticalGapCount")>0,"genuinely open request at flush stays partial");
    fs::remove_all(root);std::cout<<"PASS "<<checks<<" checks\n";return 0;
  }catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<": "<<error.what()<<'\n';fs::remove_all(root);return 1;}
}
