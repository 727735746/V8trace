#include "v8trace/core.h"
#include "../../native/v8trace-function-cache.h"
#include "../../native/v8trace-call-ledger.h"
#include <chrono>
#include <iostream>

using namespace v8trace;
namespace {
unsigned checks = 0;
void check(bool value, const char* name) { ++checks; if (!value) throw std::runtime_error(name); }
Json event(std::string name, Json args, int sequence = 0, int thread = 29) {
  return {{"name", name}, {"pid", 17}, {"tid", thread}, {"ts", sequence + 1}, {"cat", "disabled-by-default-v8.jscall"}, {"args", args}};
}
Json dictionary(std::string id = "1", std::string name = "echoFull") {
  Json function{{"type", "heap-reference"}, {"address", "0x100"}, {"instanceType", 42}, {"identityScope", "address-at-observation"}};
  Json metadata{{"functionName", name}, {"functionStart", 0}, {"scriptId", 7}, {"source", {{"known", true}, {"url", "https://fixture.test/target.js"}}}, {"functionValue", function}};
  return event("V8TraceFunction", {{"protocol", "v8trace/function/1"}, {"producerId", "1"}, {"functionId", id}, {"isolateId", "ab"}, {"metadata", dump(metadata)}});
}
Json call(unsigned sequence, std::string phase, std::string id, bool values = true, std::string function = "1") {
  Json args{{"protocol", "v8trace/call/2"}, {"producerId", "1"}, {"phase", phase}, {"callId", id}, {"functionId", function}, {"isolateId", "ab"}, {"producerSeq", sequence}, {"frameAddress", "0x200"}, {"functionAddress", "0x300"}, {"functionInstanceType", 43}, {"contextId", 5}, {"argc", 1}, {"valuesCaptured", values}};
  if (values) {
    Json payload;
    if (phase == "entry" || phase == "argument") payload = {{"arguments", Json::array({{{"type", "string"}, {"value", str(parse(R"({"s":"完整\ud800\u0000END"})"), "s")}}})}, {"receiver", {{"type", "undefined"}}}};
    else if (phase == "resume" || phase == "suspend") payload = {{"generator", {{"type", "heap-reference"}, {"address", "400"}}}, {"accumulator", {{"type", "undefined"}}}};
    else payload = {{"result", {{"type", "number"}, {"value", 7}}}};
    args["values"] = dump(payload);
  }
  args["causal"]=dump(Json{{"protocol","v8trace/causality/1"},{"producerId","causal-producer"},{"epoch","1"},{"taskId","9007199254740993"},{"known",true}});
  return event("V8TraceCall", args, sequence);
}
Json stats(unsigned selected, unsigned emitted, bool final = true) {
  return event("V8TraceCallStats", {{"protocol", "v8trace/call-stats/1"}, {"epoch", 1}, {"producerId", "1"}, {"producerThreadId", 29}, {"attemptedCalls", selected}, {"selectedCalls", selected}, {"filteredCalls", 0}, {"emittedEvents", emitted}, {"final", final}}, 90, 99);
}
Json run_import(const fs::path& root, Json events, bool values = true, Json overrides = Json::object()) {
  static unsigned run = 0; auto directory = root / std::to_string(++run); fs::create_directory(directory);
  auto file = directory / "trace.json"; write_json(file, {{"traceEvents", events}});
  Json options{{"jsCallDetail", true}, {"jsCallValues", values}, {"categories", Json::array({"jscall"})}, {"maxBytes", 8388608}, {"maxEvents", 10000}, {"detailBackend", {{"callStatsProtocol", "v8trace/call-stats/1"}}}};
  options.update(overrides);
  Store store(directory / "capture", options); { Trace trace(file); import_detail(trace, store, options); }
  return store.finalize({{"backend", "chromium-native"}});
}
bool gap(const Json& summary, std::string reason) { for (auto& item : summary.at("gaps")) if (str(item, "reason") == reason) return true; return false; }
void ledger_checks() {
  using namespace v8trace_call_ledger;
  Ledger ledger;
  auto disabled = ledger.Register(1);
  { WriteGuard guard(ledger, disabled); check(!guard, "ledger rejects admission before tracing starts"); }
  ledger.Start();
  std::vector<std::thread> threads;
  for (int worker = 0; worker < 4; ++worker) threads.emplace_back([&, worker] {
    auto producer = ledger.Register(worker + 10);
    for (unsigned i = 0; i < 10000; ++i) {
      WriteGuard guard(ledger, producer);
      if (!guard) continue;
      ++producer->attempted;
      if (i % 3) { ++producer->selected; producer->emitted += 2; }
      else ++producer->filtered;
    }
  });
  for (auto& thread : threads) thread.join();
  auto snapshots = ledger.Stop(); uint64_t total = 0;
  check(snapshots.size() == 4, "ledger retains all producer lifetimes");
  for (const auto& snapshot : snapshots) { check(snapshot.final && snapshot.attempted == snapshot.selected + snapshot.filtered && snapshot.emitted == snapshot.selected * 2, "concurrent ledger counters close exactly"); total += snapshot.attempted; }
  check(total == 40000, "concurrent ledger does not sample or cap calls");
  auto producer = ledger.Register(42);
  { WriteGuard guard(ledger, producer); check(!guard, "ledger rejects late writes after stop"); }
  ledger.Start(); producer = ledger.Register(42);
  auto reused = ledger.Register(42); check(producer->producer_id != reused->producer_id, "ledger separates reused OS thread IDs");
  std::atomic<bool> entered{false}, release{false};
  std::thread writer([&] { WriteGuard guard(ledger, producer); ++producer->attempted; ++producer->selected; ++producer->emitted; entered.store(true, std::memory_order_release); while (!release.load(std::memory_order_acquire)) std::this_thread::yield(); ++producer->emitted; });
  while (!entered.load(std::memory_order_acquire)) std::this_thread::yield();
  auto racing = ledger.Stop(std::chrono::milliseconds(2)); release.store(true, std::memory_order_release); writer.join();
  check(!racing[0].final, "timed-out in-flight snapshot never claims finality");
  check(ledger.Stop()[0].final && ledger.Stop()[0].emitted == 2, "quiescent tail has exact final count");
  ledger.Start(); auto pinned = ledger.Register(55); std::weak_ptr<Producer> weak = pinned;
  { WriteGuard guard(ledger, pinned); pinned.reset(); ledger.Stop(std::chrono::milliseconds(0)); ledger.Start(); check(!weak.expired(), "active guard pins producer across epoch reset"); }
  check(weak.expired(), "retired producer is released after active guard exits");
  ledger.Stop();
}
}
int main() {
  auto temporary = fs::canonical(fs::temp_directory_path());
  auto root = temporary / ("v8trace-jscall-test-" + uuid()); fs::create_directory(root);
  auto cleanup = [&] { if (root.parent_path() != temporary || !utf8(root.filename()).starts_with("v8trace-jscall-test-")) throw std::runtime_error("Test cleanup path escaped owned directory"); fs::remove_all(root); };
  try {
    ledger_checks();
    v8trace_cache::FunctionCache<std::string, 2> cache;
    v8trace_cache::FunctionKey a{1, 10, 7}, b{1, 11, 7}, c{2, 10, 7};
    check(!cache.Find(a, 100, 200), "initial cache miss"); cache.Put(a, 100, 200, "A");
    check(*cache.Find(a, 100, 200) == "A", "stable key hit"); cache.Put(b, 101, 200, "B");
    cache.Find(a, 100, 200); cache.Put(c, 100, 200, "C");
    check(!cache.Find(b, 101, 200) && cache.Find(a, 100, 200), "LRU evicts least recent, not entire cache");
    check(*cache.Find(c, 100, 200) == "C", "isolate identity separation");
    check(!cache.Find(a, 999, 200), "GC/name stamp invalidation"); cache.Put(a, 999, 200, "renamed");
    check(!cache.Find(a, 999, 201), "source stamp invalidation");
    cache.Put(a, 100, 200, "A");
    auto after_gc = a; after_gc.gc_epoch = 1;
    check(!cache.Find(after_gc, 100, 200), "GC epoch prevents address-stamp ABA");
    cache.Put(after_gc, 100, 200, "after-GC");
    check(*cache.Find(a, 100, 200) == "A" && *cache.Find(after_gc, 100, 200) == "after-GC", "metadata generations never alias");
    auto started = std::chrono::steady_clock::now(); uint64_t hits = 0;
    for (unsigned i = 0; i < 1000000; ++i) hits += cache.Find(a, 100, 200) != nullptr;
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
    check(hits == 1000000 && cache.size() <= 2, "million-call exact cache hits remain bounded");

    auto events = Json::array({call(2, "return", "1"), stats(1, 2), call(1, "entry", "1"), dictionary()});
    auto summary = run_import(root, events); check(summary.at("criticalGapCount") == 0 && summary.at("eventCount") == 2, "out-of-order dictionary and calls decode losslessly");
    auto data = read_file(root / "1/capture/events/jscall.jsonl"); auto entry = parse(data.substr(0, data.find('\n'))).at("payload");
    check(entry.at("arguments")[0]["value"] == parse(R"({"s":"完整\ud800\u0000END"})").at("s"), "full lone-surrogate/NUL value roundtrip");
    check(entry.at("function").at("address") == "0x300" && entry.at("function").at("instanceType") == 43, "closure identity is observed per call, never cached");
    check(entry.at("functionName") == "echoFull" && entry.at("source").at("url") == "https://fixture.test/target.js", "dictionary source reconstructed");
    check(entry.at("causal").at("taskId")=="9007199254740993" && entry.at("causal").at("protocol")=="v8trace/causality/1", "compact causal annotation decoded without integer precision loss");
    auto bad_causal=call(1,"entry","1");bad_causal["args"]["causal"]="{";
    check(gap(run_import(root,Json::array({dictionary(),bad_causal,call(2,"return","1"),stats(1,2)})),"detail-invalid-native-record"),"malformed compact causal context is an explicit decoding gap");
    check(run_import(root, Json::array({dictionary(), call(1, "entry", "1", false), call(2, "return", "1", false), stats(1, 2)}), false).at("criticalGapCount") == 0, "explicit structure-only mode");
    Json lexical{{"protocol", "v8trace/closure/1"}, {"scopeBudget", 32}, {"bindingBudget", 4096}, {"valueBudget", {{"depth", 2}, {"nodes", 256}, {"properties", 4096}}}, {"invokesUserCode", false}, {"functionLocalsIncluded", false}, {"valueCacheAcrossCalls", false}, {"nativeGlobalExcluded", true}, {"dynamicOrModuleBoundary", false}, {"coverage", "context-allocated-chain"}, {"scopesObserved", 0}, {"bindingsObserved", 0}, {"scopes", Json::array()}};
    Json closure_options{{"closureBindings", true}, {"closureScopes", 32}, {"closureBindingsLimit", 4096}, {"closureDepth", 2}, {"closureNodes", 256}, {"closureProperties", 4096}};
    auto closure_entry = call(1, "entry", "1", false), closure_return = call(2, "return", "1", false);
    closure_entry["args"]["closure"] = dump(lexical); closure_return["args"]["closure"] = dump(lexical);
    check(run_import(root, Json::array({dictionary(), closure_entry, closure_return, stats(1, 2)}), false, closure_options).at("criticalGapCount") == 0, "native compact closure annotation decodes independently of arguments");
    auto malformed_closure = closure_entry; malformed_closure["args"]["closure"] = "{";
    check(run_import(root, Json::array({dictionary(), malformed_closure, closure_return, stats(1, 2)}), false, closure_options).at("criticalGapCount") > 0, "malformed compact closure annotation rejected");
    check(run_import(root, Json::array({dictionary(), call(1, "entry", "1", false), closure_return, stats(1, 2)}), false, closure_options).at("criticalGapCount") > 0, "missing selected closure snapshot detected");
    check(run_import(root, Json::array({dictionary(), call(1, "entry", "1", false), call(2, "return", "1", false), stats(1, 2)})).at("criticalGapCount") > 0, "full-value omission rejected");
    check(run_import(root, Json::array({call(1, "entry", "1"), stats(1, 1)})).at("criticalGapCount") > 0, "missing dictionary rejected");
    check(run_import(root, Json::array({dictionary(), dictionary("1", "changed"), call(1, "entry", "1"), stats(1, 1)})).at("criticalGapCount") > 0, "conflicting dictionary rejected");
    check(gap(run_import(root, Json::array({dictionary(), call(1, "entry", "1")})), "jscall-writer-stats-missing"), "missing final receipt rejected");
    check(gap(run_import(root, Json::array({dictionary(), call(1, "entry", "1"), stats(1, 2)})), "jscall-writer-count-mismatch"), "missing tail record rejected");
    check(gap(run_import(root, Json::array({dictionary(), call(1, "entry", "1"), stats(1, 1, false)})), "jscall-writer-nonfinal"), "racing stop remains incomplete");
    auto corrupt = call(1, "entry", "1"); corrupt["args"]["values"] = "{";
    check(run_import(root, Json::array({dictionary(), corrupt, stats(1, 1)})).at("criticalGapCount") > 0, "corrupt value payload rejected");
    auto scoped = normalize_filter({{"jsCallDetail", true}, {"jsValueScriptInclude", "target.js"}, {"jsValueFunctionExclude", "helper*"}});
    check(str(scoped, "token").starts_with("2;") && scoped.at("valueRules") == true, "independent value filter protocol");
    bool rejected = false; try { normalize_filter({{"jsCallDetail", true}, {"jsCallValues", false}, {"jsValueScriptInclude", "target.js"}}); } catch (...) { rejected = true; }
    check(rejected, "disabled values reject contradictory filters");
    auto recursive = call(2, "entry", "2"); recursive["args"]["parentCallId"] = "1"; recursive["args"]["parentExact"] = true;
    check(run_import(root, Json::array({dictionary(), call(1, "entry", "1"), recursive, call(3, "return", "2"), call(4, "return", "1"), stats(2, 4)})).at("criticalGapCount") == 0, "recursive activation IDs remain distinct");
    check(run_import(root, Json::array({dictionary(), call(1, "resume", "1"), call(2, "argument", "1"), call(3, "suspend", "1"), stats(1, 3)})).at("criticalGapCount") == 0, "generator activation segment pairing");
    Json thrown{{"schema", "v8trace/detail/1"}, {"channel", "jscall"}, {"producerId", "1"}, {"isolateId", "ab"}, {"callId", "1"}, {"producerSeq", 2}, {"phase", "throw"}, {"exception", {{"type", "string"}, {"value", "FULL_EXCEPTION"}}}};
    check(run_import(root, Json::array({dictionary(), call(1, "entry", "1"), event("V8TraceDetail", {{"record", dump(thrown)}}, 2), stats(1, 2)})).at("criticalGapCount") == 0, "compact entry pairs with unwind exception record");
    check(gap(run_import(root, Json::array({dictionary(), call(1, "entry", "1"), call(3, "return", "1"), stats(1, 2)})), "detail-producer-sequence-gap"), "closed counts do not excuse sequence loss");
    auto again = Json::array({dictionary("1", "newThread"), call(1, "entry", "1"), call(2, "return", "1"), stats(1, 2)});
    for (auto& row : again) row["args"]["producerId"] = "2";
    auto reused = Json::array({dictionary(), call(1, "entry", "1"), call(2, "return", "1"), stats(1, 2)}); for (auto& row : again) reused.push_back(row);
    check(run_import(root, reused).at("criticalGapCount") == 0, "OS thread ID reuse does not merge producer lifetimes");
    auto reserved = call(1, "entry", "1"); reserved["args"]["values"] = R"({"functionName":"tampered"})";
    check(run_import(root, Json::array({dictionary(), reserved, stats(1, 1)})).at("criticalGapCount") > 0, "value payload cannot replace dictionary metadata");
    Json native_entry{{"schema", "v8trace/detail/1"}, {"channel", "native"}, {"isolateId", "ab"}, {"producerSeq", 1}, {"callId", "1"}, {"phase", "entry"}};
    auto native_exit = native_entry; native_exit["producerSeq"] = 2; native_exit["phase"] = "return";
    auto native_events = Json::array({event("V8TraceDetail", {{"record", dump(native_entry)}}, 1), event("V8TraceDetail", {{"record", dump(native_exit)}}, 2)});
    for (auto& row : native_events) row["cat"] = "disabled-by-default-v8.native";
    check(run_import(root, native_events, true, {{"jsCallDetail", false}, {"nativeDetail", true}, {"categories", Json::array({"native"})}}).at("criticalGapCount") == 0, "Native-only capture does not require JSCall statistics");
    cleanup();
    std::cout << "PASS " << checks << " checks; metadata cache 1000000 hits in " << elapsed << " us (synthetic cache-only, not browser speedup)\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << "FAIL " << checks << ": " << e.what() << '\n'; cleanup(); return 1; }
}
