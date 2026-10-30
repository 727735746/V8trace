#include "v8trace/studio.h"
#include "v8trace/edition.h"
#include <iostream>
using namespace v8trace;
namespace {
unsigned checks=0;
void check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
template<class F>void reject(F action,const char* message){bool failed=false;try{action();}catch(...){failed=true;}check(failed,message);}
}
int main(){
  auto root=fs::temp_directory_path()/("v8trace-studio-tests-"+uuid());fs::create_directory(root);
  try{
    Json current{{"output",".\\logs\\中文"},{"transport","native"},{"maxBytes",123456789},{"custom",{{"one",1},{"two",2}}},{"list",Json::array({1,2})}};
    auto incoming=configuration_document({{"transport","cdp"},{"custom",{{"one",3}}},{"list",Json::array({7})},{"longText",std::string(65536,'x')+"中文😀"}});
    auto merged=configuration_preview(current,incoming,"merge");auto options=configuration_options(merged.at("document"));
    check(options.at("custom").at("two")==2&&options.at("custom").at("one")==3&&options.at("list")==Json::array({7}),"recursive objects merge; arrays replace");
    check(options.at("output")==current.at("output")&&options.at("maxBytes")==123456789&&options.at("transport")=="cdp","paths/exact byte budgets preserved; transport only explicitly changed");
    check(merged.at("unknownFields").size()==3&&merged.at("writesFiles")==false&&merged.at("launchesBrowser")==false,"unknown options explicit, preview has no side effects");
    check(configuration_options(configuration_document(options))==options&&current.at("custom").at("one")==1,"roundtrip/cancel leave original untouched");
    write_file(root/"配置.json",std::string("\xef\xbb\xbf")+dump(incoming),true);check(load_configuration(root/"配置.json")==incoming,"UTF-8 BOM accepted only at configuration file boundary");
    auto replaced=configuration_preview(current,incoming,"replace");check(!configuration_options(replaced.at("document")).contains("output"),"replace does not merge hidden old options");
    auto bad=incoming;bad["schemaVersion"]=2;reject([&]{configuration_options(bad);},"future schema rejected");bad=incoming;bad["pathBase"]="file-parent";reject([&]{configuration_options(bad);},"path base not silently rewritten");
    reject([&]{configuration_preview(current,incoming,"guess");},"unknown merge mode rejected");
    Json capture_options{{"url","https://fixture.test/"},{"output",utf8(root/"new-capture")},{"transport","cdp"},{"http",true},{"customNewSwitch",true}};
    auto preflight=preflight_capture(capture_options,path(V8TRACE_STUDIO_ASSETS),false);check(preflight.at("ok")==true&&preflight.at("rawOptions")==capture_options&&!fs::exists(root/"new-capture"),"preflight preserves raw options and creates no capture");
    check(preflight.at("unknownFields")==Json::array({"customNewSwitch"}),"preflight exposes unimplemented options");
    capture_options["chromePath"]=utf8(root/"missing.exe");preflight=preflight_capture(capture_options,path(V8TRACE_STUDIO_ASSETS));check(preflight.at("ok")==false&&preflight.at("browserVerified")==false,"missing/old browser fails before launch");
    auto old=root/"old-chrome";fs::create_directory(old);write_file(old/"chrome.exe","fixture exe",true);write_file(old/"chrome.dll","fixture engine",true);
    write_json(old/"v8trace-opcode.json",{{"schemaVersion",1},{"protocol","v8trace/opcode/1"},{"category","disabled-by-default-v8.opcode"},{"backend","chromium-opcode"},{"binaryHashes",{{"chrome.exe",file_sha256(old/"chrome.exe")},{"chrome.dll",file_sha256(old/"chrome.dll")}}}});
    auto descriptor=load_json(old/"v8trace-opcode.json",65536);descriptor["edition"]=edition_id();write_json(old/"v8trace-opcode.json",descriptor);
    capture_options["chromePath"]=utf8(old/"chrome.exe");capture_options["envAbsentProbe"]=true;preflight=preflight_capture(capture_options,path(V8TRACE_STUDIO_ASSETS));check(preflight.at("ok")==false&&str(preflight,"error").find("environment probes")!=std::string::npos&&!fs::exists(root/"new-capture"),"real old descriptor hashes verified, missing native protocol rejected without spawn");
    auto huge=Json{{"seq",UINT64_MAX},{"payload",{{"value",std::string(8*1024*1024,'A')}}}};auto preview=event_preview(huge);check(preview.at("truncated")==true&&str(preview,"detail").size()<32768&&huge.at("payload").at("value").get_ref<const std::string&>().size()==8*1024*1024,"preview bounded before serialization; full value unchanged");
    for(auto channel:{"envAbsentProbe","scripts","http"}){Json input{{"url","https://fixture.test/"},{"output",utf8(root/(std::string("shmem-")+channel))},{"http",false},{"scripts",false},{"consoleEvents",false},{"exceptionEvents",false}};input[channel]=true;auto options=validate_options(input,path(V8TRACE_STUDIO_ASSETS),false);Store store(root/(std::string("shmem-")+channel),options);check(dump(launch_config(options,store)).find("kPerfettoBackendParams:shared_memory_size_bytes/8388608")!=std::string::npos,"env/source/body-only tracing also reserves producer shared memory");}
    {
      for(auto channel:{"httpRootTrace","opcode","nativeDetail","jsCallDetail"}) {
        auto directory=root/(std::string("shmem-heavy-")+channel);
        Json input{{"url","https://fixture.test/"},{"output",utf8(directory)},{"http",true}};input[channel]=true;
        if (!professional_edition() && !edition_blocked_options(input).empty()) { check(preflight_capture(input,path(V8TRACE_STUDIO_ASSETS),false).at("ok")==false,"community rejects professional producer selection");continue; }
        auto options=validate_options(input,path(V8TRACE_STUDIO_ASSETS),false);Store heavy(directory,options);
        auto launch=launch_config(options,heavy);
        check(dump(launch.at("args")).find("kPerfettoBackendParams:shared_memory_size_bytes/8388608")!=std::string::npos,"detailed producer shared memory stays within pinned Chromium shared-memory serialization limit");
        check(dump(launch.at("args")).find("/page_size_bytes/32768")!=std::string::npos,"high-volume native channels use larger supported pages without raising the shared-memory limit");
        options["browserIdentity"]={{"traceTransportProtocol","v8trace/trace-transport/1"},{"traceSharedMemoryMaxBytes",33554432}};
        check(dump(launch_config(options,heavy).at("args")).find("shared_memory_size_bytes/33554432/page_size_bytes/32768")!=std::string::npos,"expanded producer buffers require an explicit compatible browser transport descriptor");
        options["browserIdentity"]["traceSharedMemoryMaxBytes"]=67108864;
        check(dump(launch_config(options,heavy).at("args")).find("shared_memory_size_bytes/8388608/")!=std::string::npos,"unknown transport capacity never expands an older Chromium memory limit");
        options["browserIdentity"]["traceSharedMemoryMaxBytes"]=33554432;options["browserIdentity"]["traceTransportProtocol"]="unknown";
        check(dump(launch_config(options,heavy).at("args")).find("shared_memory_size_bytes/8388608/")!=std::string::npos,"unknown transport protocol remains compatible with original Chromium");
      }
      auto light=validate_options({{"url","https://fixture.test/"},{"output",utf8(root/"light-expanded-plan")},{"http",true}},path(V8TRACE_STUDIO_ASSETS),false);light["browserIdentity"]={{"traceTransportProtocol","v8trace/trace-transport/1"},{"traceSharedMemoryMaxBytes",33554432}};Store light_store(root/"light-expanded-plan",light);
      check(dump(launch_config(light,light_store).at("args")).find("shared_memory_size_bytes/8388608/page_size_bytes/4096")!=std::string::npos,"lightweight capture keeps original memory and page sizes even with an expanded browser");
      auto options=validate_options({{"url","https://fixture.test/"},{"output",utf8(root/"paint-plan")},{"nativeTimeline",true}},path(V8TRACE_STUDIO_ASSETS),false);Store store(root/"paint-plan",options);auto launch=launch_config(options,store);check(dump(launch).find("devtools.timeline")!=std::string::npos&&dump(launch).find("loading")!=std::string::npos,"native timeline includes original Chromium loading/paint categories without CDP");
      auto interpreted=options;interpreted["interpretOnly"]=true;auto arguments=dump(launch_config(interpreted,store));check(arguments.find("--no-sparkplug")!=arguments.npos&&arguments.find("--no-maglev")!=arguments.npos&&arguments.find("--no-turbofan")!=arguments.npos&&arguments.find("--trace-opcode")==arguments.npos&&arguments.find("--trace-jscall")==arguments.npos&&arguments.find("--jitless")==arguments.npos,"explicit interpreter comparison has no detailed logging or WASM jitless side effect");
      auto file=root/"paint-trace.json";write_json(file,{{"traceEvents",Json::array({{{"name","firstContentfulPaint"},{"cat","loading,rail,devtools.timeline"},{"pid",1},{"tid",2},{"ts",10},{"ph","R"},{"args",{{"frame","actual-frame"},{"data",{{"navigationId","actual-navigation"}}}}}}})}});Trace trace(file);import_trace(trace,store,options);store.flush();unsigned paints=0;lines(store.filename("events/native.jsonl"),[&](const Json& row){if(str(row.at("payload"),"name")=="firstContentfulPaint")++paints;});check(paints==1,"original paint marker/frame/navigation identities preserved, not inferred from HTTP");
    }
    auto capture=root/"capture";fs::create_directories(capture/"events");fs::create_directories(capture/"catalog");write_json(capture/"summary.json",{{"captureConclusion","completed"},{"integrityConclusion","complete"}});
    std::string binary("\0\xff中文",8);auto hash=sha256(binary);fs::create_directories(capture/"blobs/sha256");write_file(capture/"blobs/sha256"/hash,binary,true);Json ref{{"path","blobs/sha256/"+hash},{"sha256",hash},{"bytes",binary.size()},{"representation","fixture"}};
    Json first{{"seq",UINT64_MAX},{"category","jscall"},{"kind","call-enter"},{"payload",{{"scriptId",7},{"scriptUrl","https://fixture.test/中文.js"},{"functionId","9"},{"functionName","计算"},{"sourceUtf16Sha256",std::string(64,'a')},{"functionBytecodeSha256",std::string(64,'b')},{"bytecodeSha256",std::string(64,'c')},{"sourceSha256Verified",true},{"bytecodeSha256Verified",true},{"bytecodeHashPolicy","raw"},{"requestId",std::string("req\0id",6)},{"recordingWindow",2},{"value",std::string(8*1024*1024,'Z')},{"source",ref}}}};
    auto original=dump(first);write_file(capture/"events/jscall.jsonl",original+"\n"+dump({{"seq",2},{"category","jscall"},{"kind","call-exit"},{"payload",{{"functionId","9"},{"scriptId",7},{"recordingWindow",3}}}})+"\n",true);
    write_file(capture/"catalog/scripts.jsonl",dump({{"scriptId",7},{"url","https://fixture.test/中文.js"},{"source",ref}})+"\n",true);auto file_hash=file_sha256(capture/"events/jscall.jsonl");
    auto long_url="https://fixture.test/"+std::string(6000,'a');write_file(capture/"catalog/long.jsonl",dump({{"scriptId",11},{"scriptUrl",long_url},{"requestId",std::string(9000,'b')}})+"\n",true);
    {
      EvidenceViewer viewer(capture);check(viewer.stats().at("records")==5&&viewer.stats().at("sourceModified")==false,"all source rows/reports indexed once without modification");
      auto long_row=viewer.query({{"script",long_url}}).at("rows").at(0);check(long_row.at("scriptUrl")==long_url&&!long_row.contains("requestId")&&long_row.at("indexBoundaryCount")>0,"identity metadata is exact or absent with a boundary, never a truncated navigation target");
      auto page=viewer.query({{"category","jscall"},{"script","7"},{"limit",1}});check(page.at("rows").size()==1&&page.at("hasMore")==true,"exact script filter with keyset pagination");
      auto row=page.at("rows").at(0);check(row.at("seq")==UINT64_MAX&&row.at("blobRefs").size()==1&&str(row,"preview").size()<=2048,"uint64/real blob refs and bounded list projection");
      check(row.at("sourceFileSha256")==file_hash,"source SHA-256 retained with offset reference");
      check(row.at("sourceUtf16Sha256")==std::string(64,'a')&&row.at("functionBytecodeSha256")==std::string(64,'b')&&row.at("bytecodeSha256")==std::string(64,'c')&&row.at("sourceSha256Verified")==true&&row.at("bytecodeSha256Verified")==true&&row.at("bytecodeHashPolicy")=="raw","observed VM fingerprint/verification metadata retained, never inferred");
      reject([&]{viewer.read({{"rowId",row.at("rowId")},{"generation","old-generation"}});},"stale row identities rejected after index rebuild");
      for(unsigned mask=0;mask<100;++mask){Json filters=Json::object();unsigned bit=0;for(auto name:{"script","function","request","window","call","process","producer"})if(mask&(1u<<bit++))filters[name]="fixture-missing";viewer.query(filters);}check(true,"varied query combinations do not exhaust prepared-statement cache");
      auto next=viewer.query({{"category","jscall"},{"function","9"},{"after",page.at("nextCursor")},{"limit",1}});check(next.at("rows").size()==1&&next.at("hasMore")==false,"next page stable, no duplicate rows");
      check(viewer.query({{"function","计算"},{"window","2"}}).at("rows").size()==1&&viewer.query({{"request",std::string("req\0id",6)}}).at("rows").size()==1,"Unicode function, recording window, NUL identities exact");
      check(viewer.query({{"script","https://fixture.test/中文.js"}}).at("rows").size()==2,"source jump returns actual call and source catalog observations");
      auto part=viewer.read({{"rowId",row.at("rowId")},{"offset",0},{"limit",65536}});check(part.at("bytes")==65536&&part.at("hasMore")==true&&unbase64(str(part,"base64"))==original.substr(0,65536),"giant row read only one exact byte page");
      auto saved=viewer.copy({{"rowId",row.at("rowId")},{"output",utf8(root/"full-event.json")}});check(saved.at("bytes")==original.size()&&read_file(root/"full-event.json")==original,"full row copied exactly without list truncation");
      check(unbase64(str(viewer.read({{"reference",ref}}),"base64"))==binary,"verified source/value bytes accessible on demand");
      auto wrong=ref;wrong["path"]="../outside";reject([&]{viewer.read({{"reference",wrong}});},"blob traversal rejected");
      reject([&]{viewer.copy({{"rowId",row.at("rowId")},{"output",utf8(capture/"new.json")}});},"read-only capture boundary");
      reject([&]{viewer.query({{"limit",201}});},"bounded page budget");
      write_file(capture/"events/jscall.jsonl","{}\n");reject([&]{viewer.query(Json::object());},"changed journal requires explicit rebuild");
    }
    check(file_sha256(capture/"blobs/sha256"/hash)==hash&&file_hash==sha256(original+"\n"+dump({{"seq",2},{"category","jscall"},{"kind","call-exit"},{"payload",{{"functionId","9"},{"scriptId",7},{"recordingWindow",3}}}})+"\n"),"viewer never rewrote capture content");
    fs::remove_all(root);std::cout<<"PASS "<<checks<<" checks\n";return 0;
  }catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<": "<<error.what()<<'\n';fs::remove_all(root);return 1;}
}
