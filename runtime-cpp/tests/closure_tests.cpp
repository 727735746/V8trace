#include "v8trace/core.h"
#include <iostream>

using namespace v8trace;
namespace {
unsigned checks = 0;
void check(bool condition, const char* label) { ++checks; if (!condition) throw std::runtime_error(label); }
template<class F> void rejected(F action, const char* label) { bool failed = false; try { action(); } catch (const std::exception&) { failed = true; } check(failed, label); }
Json snapshot(Json value) {
  return {{"protocol", "v8trace/closure/1"}, {"scopeBudget", 32}, {"bindingBudget", 4096}, {"valueBudget", {{"depth", 2}, {"nodes", 256}, {"properties", 4096}}}, {"invokesUserCode", false}, {"functionLocalsIncluded", false}, {"valueCacheAcrossCalls", false}, {"nativeGlobalExcluded", true}, {"dynamicOrModuleBoundary", false}, {"coverage", "context-allocated-chain"}, {"scopesObserved", 1}, {"bindingsObserved", 1}, {"scopes", Json::array({{{"depth", 0}, {"scopeType", "FUNCTION_SCOPE"}, {"address", "0x400"}, {"bindings", Json::array({{{"name", {{"type", "string"}, {"value", "table"}}}, {"slot", 2}, {"mode", 1}, {"value", value}}})}}})}};
}
}
int main() {
  auto temporary = fs::canonical(fs::temp_directory_path()); auto root = temporary / ("v8trace-closure-" + uuid()); fs::create_directory(root);
  auto cleanup = [&] { if (root.parent_path() != temporary || !utf8(root.filename()).starts_with("v8trace-closure-")) throw std::runtime_error("Invalid closure test cleanup boundary"); fs::remove_all(root); };
  try {
    auto assets = path(V8TRACE_CLOSURE_ASSETS);
    Json input{{"url", "https://example.test/"}, {"output", utf8(root / "capture")}, {"jsCallDetail", true}, {"closureBindings", true}, {"detailScriptInclude", "target.js"}, {"jsCallValues", false}};
    auto options = validate_options(input, assets, false);
    check(options.at("jsCallValues") == false && options.at("closureBindings") == true, "closure independent of argument values");
    auto invalid = input; invalid.erase("detailScriptInclude"); rejected([&] { validate_options(invalid, assets, false); }, "closure needs target scope");
    invalid["detailScriptInclude"] = "*"; rejected([&] { validate_options(invalid, assets, false); }, "global-only closure scope rejected");
    invalid = input; invalid["jsCallDetail"] = false; rejected([&] { validate_options(invalid, assets, false); }, "closure needs JSCall");
    invalid = input; invalid["closureNodes"] = 4097; rejected([&] { validate_options(invalid, assets, false); }, "closure graph bound enforced");
    auto unicode = parse(R"({"type":"string","value":"中文\ud800\u0000😀"})");
    Json first{{"phase", "entry"}, {"closure", snapshot(unicode)}};
    auto status = validate_closure_record(first, options); check(status.at("captured") == true && status.at("bindings") == 1 && status.at("boundaries").empty(), "full lexical snapshot validated");
    auto second = first; second["closure"]["scopes"][0]["bindings"][0]["value"] = {{"type", "number"}, {"value", 7}};
    check(dump(first) != dump(second) && validate_closure_record(second, options).at("captured") == true, "different captured state with same call metadata retained");
    check(parse(dump(first)) == first, "Unicode lone surrogate and NUL binding values retained");
    auto boundary = first; boundary["closure"]["coverage"] = "partial-budget-or-layout";
    check(!validate_closure_record(boundary, options).at("boundaries").empty(), "binding budget boundary explicit");
    boundary = first; boundary["closure"]["dynamicOrModuleBoundary"] = true; check(!validate_closure_record(boundary, options).at("boundaries").empty(), "module and dynamic boundary explicit");
    boundary = first; boundary["closure"]["scopes"][0]["bindings"][0]["value"] = {{"type", "unavailable"}, {"reason", "detached-context-cell"}};
    check(validate_closure_record(boundary, options).at("boundaries")[0]["reason"] == "detached-context-cell", "detached ContextCell preserved");
    check(validate_closure_record({{"phase", "throw"}}, options).at("captured") == false, "unwound lexical state not fabricated");
    rejected([&] { validate_closure_record({{"phase", "entry"}}, options); }, "missing entry snapshot rejected");
    invalid = first; invalid["closure"]["valueCacheAcrossCalls"] = true; rejected([&] { validate_closure_record(invalid, options); }, "cross-call mutable cache rejected");
    invalid = first; invalid["closure"]["bindingsObserved"] = 2; rejected([&] { validate_closure_record(invalid, options); }, "binding count mismatch rejected");
    invalid = first; invalid["closure"]["valueBudget"]["depth"] = 1; rejected([&] { validate_closure_record(invalid, options); }, "unselected graph budget rejected");
    for (auto phase : {"resume", "suspend", "return", "argument", "throw"}) { auto value = first; value["phase"] = phase; check(validate_closure_record(value, options).at("captured") == true, "existing generator activation phase and pre-unwind snapshot supported"); }
    { Store store(root / "launch", options); auto launch = launch_config(options, store); auto args = dump(launch.at("args")); check(args.find("--trace-jscall-closure") != args.npos && args.find("--trace-closure-bindings=4096") != args.npos, "closure flags wired to native launch"); }
    cleanup(); std::cout << "PASS " << checks << " checks\n"; return 0;
  } catch (const std::exception& error) { std::cerr << "FAIL " << checks << ": " << error.what() << '\n'; cleanup(); return 1; }
}
