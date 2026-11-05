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
std::string header_hex(std::string_view bytes){std::string result;const char* digits="0123456789abcdef";for(unsigned char value:bytes){result+=digits[value>>4];result+=digits[value&15];}return result;}
Json wire_fixture(bool multiplexed=false,unsigned attempts=1,bool cached=false,bool failed=false){
  Json rows=Json::array();
  auto add=[&](const char* kind,Json record){record.update({{"schema","v8trace/browser/1"},{"producer","chromium"},{"producerSeq",std::to_string(rows.size()+1)},{"channel","http"},{"kind",kind}});rows.push_back({{"name","V8TraceBrowser"},{"cat","disabled-by-default-v8.http"},{"pid",71},{"tid",4},{"ts",100+rows.size()},{"args",{{"record",dump(record)}}}});};
  add("request",{{"requestId","wire1"},{"layer","network"},{"url","https://wire.fixture.test/wire"},{"method","GET"},{"headers",{{"X-Application","before-send"}}},{"requestBodyPresent",false},{"netLogSource",{{"id","44"},{"type",1}}}});
  for(unsigned attempt=1;attempt<=attempts;++attempt){
    Json pairs=Json::array(),fields=Json::object();
    auto pair=[&](const std::string& name,const std::string& bytes,bool valid=true){Json value=valid?Json(bytes):Json(nullptr);pairs.push_back({{"name",name},{"value",value},{"nameBytesHex",header_hex(name)},{"valueBytesHex",header_hex(bytes)}});fields[name]=value;};
    if(multiplexed){pair(":method","GET");pair(":scheme","https");pair(":authority","wire.fixture.test");pair(":path","/wire");}
    else{pair("Host","wire.fixture.test");pair("Connection","keep-alive");pair("Content-Length","0");}
    pair("X-Repeat","one");pair("X-Repeat","two");pair("X-Attempt",std::to_string(attempt));pair("X-Binary",std::string(1,char(255)),false);
    std::string line=multiplexed?"":"GET /wire HTTP/1.1\r\n";
    add("request-wire-headers",{{"requestId","wire1"},{"wireHeadersProtocol","v8trace/http-wire-headers/1"},{"wireAttempt",std::to_string(attempt)},{"headers",fields},{"headerPairs",pairs},{"requestLine",line},{"requestLineBytesHex",header_hex(line)}});
  }
  add("request-headers",{{"requestId","wire1"},{"headers",{{"Host","application-not-wire"}}}});
  if(!failed){add("response",{{"requestId","wire1"},{"status",200},{"headers",{{"content-type","text/plain"}}}});add("body-chunk",{{"requestId","wire1"},{"direction","response"},{"offset","0"},{"bytes",5},{"base64",base64("hello")}});}
  add("complete",{{"requestId","wire1"},{"success",!failed},{"errorCode",failed?-2:0},{"bytes",failed?"0":"5"},{"wireHeadersProtocol","v8trace/http-wire-headers/1"},{"wireHeaderCount",std::to_string(attempts)},{"wasCached",cached},{"networkAccessed",!cached&&!failed}});
  return rows;
}
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
    options["browserIdentity"]={{"httpWireHeadersProtocol","v8trace/http-wire-headers/1"}};
    check(run(wire_fixture(),"wire-http1").at("criticalGapCount")==0,"actual wire header fields retained without pre-send overwrite");
    lines(root/"wire-http1/catalog/resources.jsonl",[&](const Json& row){resource=row;});
    check(resource.at("request").at("headers").at("Host")=="wire.fixture.test"&&resource.at("request").at("headers").at("Content-Length")=="0","transport-added headers are observed, never inferred");
    check(resource.at("request").at("headersConclusion")=="observed-wire"&&resource.at("request").at("requestLine")=="GET /wire HTTP/1.1\r\n","socket boundary and exact non-multiplexed request line explicit");
    check(resource.at("request").at("headerPairs").at(3).at("value")=="one"&&resource.at("request").at("headerPairs").at(4).at("value")=="two","duplicate header order is not flattened");
    check(resource.at("request").at("headerPairs").back().at("value").is_null()&&resource.at("request").at("headerPairs").back().at("valueBytesHex")=="ff","non-UTF8 wire bytes retained without fabricated text");
    check(resource.at("request").at("netLogSource").at("id")=="44","native NetLog request identity retained");
    bad=wire_fixture();edit(bad,"request-wire-headers",[](Json& r){std::map<std::string,Json> sorted;for(auto it=r.at("headers").begin();it!=r.at("headers").end();++it)sorted.emplace(it.key(),it.value());Json fields=Json::object();for(const auto& [key,value]:sorted)fields[key]=value;r["headers"]=std::move(fields);});
    check(run(bad,"wire-native-sorted-dictionary").at("criticalGapCount")==0,"native sorted dictionary is semantically equal to wire-ordered pairs");
    lines(root/"wire-native-sorted-dictionary/catalog/resources.jsonl",[&](const Json& row){resource=row;});
    check(resource.at("request").at("headerPairs").at(0).at("name")=="Host"&&resource.at("request").at("headerPairs").at(3).at("value")=="one"&&resource.at("request").at("headerPairs").at(4).at("value")=="two","semantic dictionary comparison never sorts or flattens authoritative duplicate wire pairs");
    check(run(wire_fixture(false,2),"wire-retry").at("criticalGapCount")==0,"all socket-boundary retry attempts observed");
    lines(root/"wire-retry/catalog/resources.jsonl",[&](const Json& row){resource=row;});
    check(resource.at("wireHeaderAttempts").size()==2&&resource.at("wireHeaderAttempts").at(0).at("headers").at("X-Attempt")=="1"&&resource.at("request").at("headers").at("X-Attempt")=="2","retry history survives while compatibility view uses last observed headers");
    check(run(wire_fixture(true),"wire-multiplexed").at("criticalGapCount")==0,"multiplexed pseudo headers accepted as native fields");
    lines(root/"wire-multiplexed/catalog/resources.jsonl",[&](const Json& row){resource=row;});
    check(resource.at("request").at("headers").at(":authority")=="wire.fixture.test"&&!resource.at("request").at("headers").contains("Host")&&resource.at("request").at("requestLine")=="","no invented HTTP1 host or request line on multiplexed transport");
    check(run(wire_fixture(false,0,true),"wire-cache").at("criticalGapCount")==0,"observed successful no-network cache result has no invented send");
    lines(root/"wire-cache/catalog/resources.jsonl",[&](const Json& row){resource=row;});
    check(resource.at("request").at("headersConclusion")=="cached-without-network"&&resource.at("wireHeaderAttempts").empty(),"cache evidence is distinct from observed wire headers");
    check(run(wire_fixture(false,0,false,true),"wire-failed-before-observation").at("criticalGapCount")==0,"observed request failure does not invent missing wire fields");
    bad=wire_fixture(false,0);check(run(bad,"wire-unobserved-success").at("criticalGapCount")>0,"successful network transfer requires observed socket-boundary headers");
    lines(root/"wire-unobserved-success/catalog/resources.jsonl",[&](const Json& row){resource=row;});
    check(resource.at("body").is_object()&&read_file(root/"wire-unobserved-success"/str(resource.at("body"),"path"))=="hello","header gap preserves independently verified body bytes");
    bad=wire_fixture();edit(bad,"complete",[](Json& r){r["wireHeaderCount"]="2";});check(run(bad,"wire-final-count-gap").at("criticalGapCount")>0,"missing wire attempt is critical");
    bad=wire_fixture();edit(bad,"request-wire-headers",[](Json& r){r["wireAttempt"]="2";});check(run(bad,"wire-attempt-gap").at("criticalGapCount")>0,"wire attempt sequence gap rejected");
    bad=wire_fixture();edit(bad,"request-wire-headers",[](Json& r){r["wireHeadersProtocol"]="unknown";});check(run(bad,"wire-unknown-protocol").at("criticalGapCount")>0,"unknown wire protocol rejected");
    bad=wire_fixture();edit(bad,"request-wire-headers",[](Json& r){r["headerPairs"][0]["valueBytesHex"]="00";});check(run(bad,"wire-value-byte-conflict").at("criticalGapCount")>0,"wire value text and byte conflict rejected");
    bad=wire_fixture();edit(bad,"request-wire-headers",[](Json& r){r["headers"]["Host"]="different";});check(run(bad,"wire-dictionary-conflict").at("criticalGapCount")>0,"wire dictionary and ordered pairs conflict rejected");
    bad=wire_fixture();edit(bad,"complete",[](Json& r){r.erase("wireHeadersProtocol");});check(run(bad,"wire-missing-completion-protocol").at("criticalGapCount")>0,"declared producer requires final wire protocol proof");
    bad=wire_fixture();edit(bad,"complete",[](Json& r){r.erase("networkAccessed");});check(run(bad,"wire-missing-transport-proof").at("criticalGapCount")>0,"missing final transport proof never treated as no-network cache");
    bad=wire_fixture(false,0,true);edit(bad,"complete",[](Json& r){r["networkAccessed"]=true;});check(run(bad,"wire-cache-revalidation-unobserved").at("criticalGapCount")>0,"cache flag alone never excuses missing revalidation headers");
    {
      const std::string source="const marker='全文';\n/*"+std::string(200000,'s')+"*/\n"; Json rows=Json::array();
      for(unsigned index=1;index<=3;++index){Json record{{"schema","v8trace/browser/1"},{"producer","chromium"},{"producerSeq",std::to_string(index)},{"channel","script"},{"kind","compiled"},{"scriptId",index},{"source",source},{"url","https://wire.fixture.test/large.js"}};rows.push_back({{"name","V8TraceBrowser"},{"cat","disabled-by-default-v8.script"},{"pid",71},{"tid",4},{"ts",90+index},{"args",{{"record",dump(record)}}}});}
      for(auto& row:wire_fixture())rows.push_back(row);
      write_json(root/"script-http-budget.json",{{"traceEvents",rows}});auto bounded=options;bounded["scripts"]=true;bounded["maxBytes"]=700000;bounded["categories"]=Json::array({"script","http"});
      Store store(root/"script-http-budget",bounded);{Trace trace(root/"script-http-budget.json");import_browser(trace,store,bounded);}auto observed=store.finalize({{"backend","chromium-native"},{"captureConclusion","collected"}});
      check(observed.at("criticalGapCount")==0&&observed.at("counts").at("script")==3,"large repeated script sources never starve HTTP inside the unchanged byte budget");
      unsigned script_events=0;lines(root/"script-http-budget/events/script.jsonl",[&](const Json& row){const auto& record=row.at("payload");check(record.at("sourceStorageProtocol")=="v8trace/native-script-source/2"&&record.at("source").is_object(),"native script events use explicit audited source references");check(read_file(root/"script-http-budget"/str(record.at("source"),"path"))==source&&read_file(root/"script-http-budget"/str(record.at("sourceUtf16"),"path"))==utf16le(source),"UTF8 source and exact UTF16 code units remain lossless");++script_events;});
      check(script_events==3&&fs::file_size(root/"script-http-budget/events/script.jsonl")<10000,"every source event remains while multi-megabyte inline duplication is eliminated");
      lines(root/"script-http-budget/catalog/resources.jsonl",[&](const Json& row){resource=row;});check(resource.at("conclusion")=="complete"&&read_file(root/"script-http-budget"/str(resource.at("body"),"path"))=="hello","following HTTP body and actual wire fields survive source-heavy collection");
    }
    {
      options.erase("browserIdentity");
      std::string name="long-http-spool-";
      const auto spool_suffix=path("native-output")/"body-cpp-00000000-0000-4000-8000-000000000000"/std::string(64,'a');
      while((root/name/spool_suffix).native().size()<270)name+='x';
      check((root/name/spool_suffix).native().size()>=260,"HTTP spool regression crosses the legacy Windows file path limit");
      auto original=wire_fixture();
      auto normal=fixture(false);
      for(auto& row:normal)row["pid"]=72;
      original.insert(original.end(),normal.begin(),normal.end());
      auto long_path_summary=run(original,name+"-complete");
      if(long_path_summary.at("criticalGapCount")!=0)throw std::runtime_error("long-path redirect body imports without gaps: "+dump(long_path_summary.at("gaps")));
      lines(root/(name+"-complete")/"catalog/resources.jsonl",[&](const Json& row){if(str(row,"requestId")=="72/n1")resource=row;});
      check(resource.at("body").at("bytes")==prefix.size()&&read_file(io_path(root/(name+"-complete")/str(resource.at("body"),"path")))==prefix,"long-path spool retains exact complete binary body bytes");
      original=wire_fixture();
      auto cancelled=fixture(true);
      for(auto& row:cancelled)row["pid"]=72;
      original.insert(original.end(),cancelled.begin(),cancelled.end());
      check(run(original,name+"-cancelled").at("criticalGapCount")==0,"long-path cancelled redirect imports without gaps");
      lines(root/(name+"-cancelled")/"catalog/resources.jsonl",[&](const Json& row){if(str(row,"requestId")=="72/n1")resource=row;});
      check(resource.at("body").is_null()&&read_file(root/(name+"-cancelled")/str(resource.at("bodyPrefix"),"path"))==prefix,"long-path spool retains cancelled binary prefix without claiming a complete body");
      for(const auto& capture:{name+"-complete",name+"-cancelled"})for(const auto& entry:fs::directory_iterator(io_path(root/capture/"native-output")))check(!utf8(entry.path().filename()).starts_with("body-cpp-"),"owned HTTP spool directory removed after import");
    }
    fs::remove_all(root);std::cout<<"PASS "<<checks<<" checks\n";return 0;
  }catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<": "<<error.what()<<'\n';fs::remove_all(io_path(root));return 1;}
}
