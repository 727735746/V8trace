#include "v8trace/core.h"

namespace v8trace {
Json validate_closure_record(const Json& record, const Json& options) {
  auto require = [](bool valid, const char* message) { if (!valid) throw std::runtime_error(message); };
  if (!record.contains("closure")) {
    require(str(record, "phase") == "throw", "Selected call has no lexical binding snapshot");
    return {{"captured", false}, {"boundaries", Json::array({{{"reason", "native-frame-unwind"}, {"critical", false}}})}};
  }
  auto snapshot = record.at("closure");
  require(str(snapshot, "protocol") == "v8trace/closure/1" && field(snapshot, "invokesUserCode") == false && field(snapshot, "functionLocalsIncluded") == false && field(snapshot, "valueCacheAcrossCalls") == false && field(snapshot, "nativeGlobalExcluded") == true && field(snapshot, "scopes").is_array(), "Malformed lexical observation protocol or policy");
  require(field(snapshot, "scopeBudget") == options.at("closureScopes") && field(snapshot, "bindingBudget") == options.at("closureBindingsLimit"), "Lexical observation budget differs from selection");
  auto values = field(snapshot, "valueBudget");
  for (auto [option, name] : std::vector<std::pair<std::string, std::string>>{{"closureDepth", "depth"}, {"closureNodes", "nodes"}, {"closureProperties", "properties"}}) require(field(values, name) == options.at(option), "Lexical value graph budget differs from selection");
  uint64_t scopes = 0, bindings = 0; Json boundaries = Json::array();
  for (const auto& scope : snapshot.at("scopes")) {
    require(field(scope, "depth") == scopes++ && field(scope, "scopeType").is_string() && field(scope, "bindings").is_array() && field(scope, "address").is_string(), "Malformed lexical scope");
    for (const auto& binding : scope.at("bindings")) {
      ++bindings; require(field(binding, "name").is_object() && safe_uint(field(binding, "slot")) && field(binding, "value").is_object() && field(binding, "mode").is_number_integer(), "Malformed lexical binding");
    }
  }
  require(scopes == field(snapshot, "scopesObserved") && bindings == field(snapshot, "bindingsObserved") && scopes <= options.at("closureScopes").get<uint64_t>() && bindings <= options.at("closureBindingsLimit").get<uint64_t>(), "Lexical observation count mismatch");
  require(str(snapshot, "coverage") == "context-allocated-chain" || str(snapshot, "coverage") == "partial-budget-or-layout", "Unknown lexical observation coverage");
  if (str(snapshot, "coverage") != "context-allocated-chain") boundaries.push_back({{"reason", "scope-or-binding-budget-or-layout"}, {"critical", true}});
  if (field(snapshot, "dynamicOrModuleBoundary") == true) boundaries.push_back({{"reason", "dynamic-or-module-state-not-resolved"}, {"critical", true}});
  std::function<void(const Json&, std::string)> visit = [&](const Json& value, std::string pointer) {
    if (value.is_object()) {
      if (str(value, "type") == "unavailable" || str(value, "coverage") == "partial") boundaries.push_back({{"reason", str(value, "reason", "value-graph-budget")}, {"pointer", pointer}, {"critical", true}});
      for (auto it = value.begin(); it != value.end(); ++it) visit(it.value(), pointer + '/' + it.key());
    } else if (value.is_array()) for (size_t i = 0; i < value.size(); ++i) visit(value[i], pointer + '/' + std::to_string(i));
  }; visit(snapshot.at("scopes"), "/scopes");
  return {{"captured", true}, {"scopes", scopes}, {"bindings", bindings}, {"boundaries", boundaries}};
}
}
