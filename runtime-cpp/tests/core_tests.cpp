#include "v8trace/core.h"
#include <iostream>
#include <stdexcept>

using namespace v8trace;
namespace {
unsigned checks = 0;
void check(bool value, const char* name) { ++checks; if (!value) throw std::runtime_error(name); }
template<class F> void rejected(F fn, const char* name) { bool failed = false; try { fn(); } catch (const std::exception&) { failed = true; } check(failed, name); }
}
int main() {
  auto root = fs::temp_directory_path() / ("v8trace-cpp-test-" + uuid()); fs::create_directory(root);
  try {
    auto json = parse(R"({"a":"\ud800\u0000中\udfff","emoji":"\ud83d\ude00","n":18446744073709551615})");
    check(parse(dump(json)) == json, "lossless lone surrogate roundtrip"); check(utf16le(str(json, "a")).size() == 8, "UTF16 exact units"); check(str(json, "emoji").size() == 4, "surrogate pair normalization"); check(json["n"].get<uint64_t>() == UINT64_MAX, "uint64 exact");
    rejected([] { parse("{\"a\":1,\"a\":2}"); }, "duplicate key"); rejected([] { parse("[1,]"); }, "trailing comma"); rejected([] { parse("1x"); }, "trailing input"); rejected([] { parse("\"\\u12xx\""); }, "invalid Unicode");
    check(sha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA256 known answer"); std::string all; for (int i = 0; i < 256; ++i) all += char(i); check(unbase64(base64(all)) == all, "base64 all bytes"); rejected([] { unbase64("AB=="); }, "base64 canonical");
    Json o{{"maxBytes", 1048576}, {"maxEvents", 20}, {"categories", Json::array({"jscall"})}}; Store store(root / path("中文日志"), o); check(store.event("jscall", "call-entry", json), "store Unicode record"); check(!store.event("http", "request", {}), "selected categories"); auto blob = store.blob(all, "binary"); check(blob == store.blob(all, "binary"), "blob dedup"); check(file_sha256(store.filename(str(blob, "path"))) == str(blob, "sha256"), "blob exact hash"); rejected([&] { store.filename("../outside"); }, "path traversal"); auto summary = store.finalize({{"backend", "chromium-native"}}); check(summary["eventCount"] == 1 && summary["blobCount"] == 1, "store counters");
    auto filter = normalize_filter({{"jsCallDetail", true}, {"detailScriptInclude", "a.js\n目录/😀.js"}, {"nativeKinds", "api-getter,runtime"}}); check(str(filter, "token").starts_with("1;K=20;U=0;X=0;S+="), "producer filter token"); rejected([] { normalize_filter({{"jsCallDetail", true}, {"nativeKinds", "all,runtime"}}); }, "invalid kinds");
    auto trace_file = root / "trace.json"; Json events = Json::array(); for (int i : {3, 1, 2}) events.push_back({{"name", "V8TraceDetail"}, {"pid", 1}, {"tid", 2}, {"ts", i}, {"args", {{"record", dump(Json{{"producerSeq", i}, {"channel", "jscall"}})}}}}); write_json(trace_file, {{"traceEvents", events}, {"metadata", {{"trace_processor_stats", {{"traced_final_flush_succeeded", 1}}}}}});
    { Trace trace(trace_file); check(trace.has_array && trace.event_count == 3, "streaming trace parser"); int expected = 1; trace.each("detail", [&](const Json& row) { check(row.at("record").at("producerSeq") == expected++, "producer-local sort"); }); check(diagnostics(trace.properties)["finalFlushSucceeded"] == true, "flush metadata"); }
    auto opcode = [](uint64_t sequence, int window) { return Json{{"name", "V8TraceOpcode"}, {"pid", 1}, {"tid", 2}, {"ts", 0}, {"v8traceWindow", window}, {"args", {{"record", dump(Json{{"producerSeq", std::to_string(sequence)}})}}}}; };
    auto ordered_file = root / "ordered.json"; write_json(ordered_file, {{"traceEvents", Json::array({opcode(UINT64_MAX - 1, 1), opcode(UINT64_MAX, 1)})}}); auto original = file_sha256(ordered_file);
    { Trace trace(ordered_file); check(trace.sort_statistics.at("opcode").at("sortSkipped") == true, "validated monotone producer sequence skips sorting without a lossy cutoff"); uint64_t expected = UINT64_MAX - 1; trace.each("opcode", [&](const Json& row) { check(row.at("sequence") == expected++, "uint64 sequence precision retained on fast path"); }); }
    check(file_sha256(ordered_file) == original, "fast path leaves original trace bytes unchanged");
    write_json(ordered_file, {{"traceEvents", Json::array({opcode(2, 1), opcode(1, 1)})}});
    { Trace trace(ordered_file); check(trace.sort_statistics.at("opcode").at("sortSkipped") == false, "out of order producer falls back to bounded external sort"); unsigned expected = 1; trace.each("opcode", [&](const Json& row) { check(row.at("sequence") == expected++, "fallback retains complete sorted events"); }); }
    write_json(ordered_file, {{"traceEvents", Json::array({opcode(2, 1), opcode(1, 2)})}});
    { Trace trace(ordered_file); check(trace.sort_statistics.at("opcode").at("sortSkipped") == true, "new recording window precedes sequence reset in monotone validation"); }
    auto dropped=diagnostics({{"metadata",{{"trace_processor_stats",{{"traced_buf",Json::array({{{"trace_writer_packet_loss",1},{"sequence_packet_loss",1},{"buffer_size",4194304}}})},{"packet_skipped_seq_needs_incremental_state_invalid",53}}}}}});
    check(dropped["losses"].size()==3,"nested Perfetto buffer arrays and invalid incremental state losses remain critical evidence");
    check(dump(dropped).find("traced_buf.0.trace_writer_packet_loss")!=std::string::npos,"indexed buffer loss coordinates retained");
    auto scoped = diagnostics({{"metadata", {{"trace_processor_stats", {{"traced_chunks_discarded", 5}, {"traced_patches_discarded", 2}, {"traced_buf_chunks_discarded", Json::array({0,3})}, {"traced_invalid_packets", 1}, {"traced_final_flush_succeeded", 1}}}}}});
    check(scoped.at("globalCounters").size() == 2 && scoped.at("losses").size() == 2, "global service lifetime counters separated from this trace buffer loss and invalid packets");
    check(dump(scoped.at("losses")).find("traced_buf_chunks_discarded.1") != std::string::npos, "scalar buffer statistic array retains actual loss coordinates");
    check(diagnostics({{"metadata", {{"trace_processor_stats", {{"traced_final_flush_succeeded", "1"}}}}}}).at("finalFlushSucceeded") == false, "string flush outcome never impersonates numeric proof");
    Json worker{{"sessionId","child"},{"targetId","worker-id"},{"parentSessionId","parent"},{"info",{{"type","worker"},{"url","blob:https://fixture.test/one"},{"parentId","page-id"}}}};
    Json response{{"requestId","worker-id"},{"response",{{"url","blob:https://fixture.test/one"}}}};
    Json request{{"sessionId","parent"},{"targetId","page-id"},{"requestId","worker-id"},{"recordingWindow",2},{"conclusion","pending"},{"request",{{"url","blob:https://fixture.test/one"},{"method","GET"},{"headers",{{"Cookie","original-full-value"}}}}}};
    auto original_request=dump(request);auto handoff=cdp_worker_request_handoff(worker,response,request,2,true);
    check(str(handoff,"kind")=="observed-worker-main-resource-handoff"&&str(handoff,"requestSessionId")=="parent"&&str(handoff,"responseSessionId")=="child"&&dump(request)==original_request,"worker handoff preserves original request and records both observed sessions");
    auto wrong=worker;wrong["parentSessionId"]="sibling";check(cdp_worker_request_handoff(wrong,response,request,2,true).is_null(),"same URL never joins unrelated sessions");
    wrong=worker;wrong["targetId"]="other";check(cdp_worker_request_handoff(wrong,response,request,2,true).is_null(),"worker target must equal main resource request id");
    wrong=worker;wrong["info"]["type"]="iframe";check(cdp_worker_request_handoff(wrong,response,request,2,true).is_null(),"worker rule does not guess iframe requests");
    wrong=response;wrong["response"]["url"]="blob:https://fixture.test/other";check(cdp_worker_request_handoff(worker,wrong,request,2,true).is_null(),"response URL must exactly match original request");
    wrong=request;wrong["targetId"]="other-page";check(cdp_worker_request_handoff(worker,response,wrong,2,true).is_null(),"observed parent target must match");
    wrong=request;wrong["conclusion"]="complete";check(cdp_worker_request_handoff(worker,response,wrong,2,true).is_null(),"completed request is not replayed across sessions");
    check(cdp_worker_request_handoff(worker,response,request,3,true).is_null(),"request handoff never crosses recording windows");
    wrong=request;wrong["request"]["method"]="POST";check(cdp_worker_request_handoff(worker,response,wrong,2,true).is_null(),"main script handoff does not invent POST semantics");
    Json own_session{{"sessionId","one"},{"sourceBreakpoint","8:beforeScriptExecution"}};
    Json paused{{"method","Debugger.paused"},{"sessionId","one"},{"params",{{"reason","instrumentation"},{"data",{{"scriptId","9"}}}}}};
    check(cdp_owned_source_pause(paused,own_session),"own before-execution pause can be serviced during another command");
    auto unrelated=paused;unrelated["params"]["reason"]="other";check(!cdp_owned_source_pause(unrelated,own_session),"foreign instrumentation or user debugger pause is never automatically resumed");
    unrelated=paused;unrelated["sessionId"]="other";check(!cdp_owned_source_pause(unrelated,own_session),"pause servicing cannot cross session identity");
    unrelated=own_session;unrelated.erase("sourceBreakpoint");check(!cdp_owned_source_pause(paused,unrelated),"session without installed own breakpoint is never resumed");
    unrelated=paused;unrelated["method"]="Debugger.scriptParsed";check(!cdp_owned_source_pause(unrelated,own_session),"normal events are not reordered by command wait");
    Json launch_input{{"url","http://fixture.test/"},{"output",utf8(root/"uncreated-output")},{"http",true}};
    auto normal=validate_options(launch_input,root,false);check(normal.at("disableExtensions")==false,"normal browser capture retains extension behavior by default");
    { Store launcher(root/"launch-options",normal);auto ordinary=launch_config(normal,launcher).at("args");check(std::find(ordinary.begin(),ordinary.end(),Json("--disable-extensions"))==ordinary.end(),"default launch has no extension isolation switch");normal["disableExtensions"]=true;auto isolated=launch_config(normal,launcher).at("args");check(std::find(isolated.begin(),isolated.end(),Json("--disable-extensions"))!=isolated.end(),"explicit extension isolation propagates to the real Chrome command"); }
    launch_input["disableExtensions"]="true";rejected([&]{validate_options(launch_input,root,false);},"extension isolation requires an actual boolean");
    for (auto& entry : fs::directory_iterator(root)) check(!utf8(entry.path().filename()).starts_with("decode-cpp-"), "owned spool cleanup");
    write_file(root / "bad.json", "{\"traceEvents\":["); rejected([&] { Trace trace(root / "bad.json"); }, "unfinished trace"); for (auto& entry : fs::directory_iterator(root)) check(!utf8(entry.path().filename()).starts_with("decode-cpp-"), "failure spool cleanup");
    fs::remove_all(root); std::cout << "PASS " << checks << " checks\n"; return 0;
  } catch (const std::exception& e) { std::cerr << "FAIL " << checks << ": " << e.what() << '\n'; fs::remove_all(root); return 1; }
}
