#include "v8trace/scenario_contract.h"
#include "v8trace/edition.h"
#include <algorithm>

namespace v8trace {
namespace {
void need_contract(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
bool active(const Json& options, std::string_view name) { return field(options, name, false) == true; }
Json load_catalog(const fs::path& assets) {
  auto file = assets / "scenario_presets.json";
  auto bytes = read_file(file, 1048576); auto catalog = parse(bytes);
  need_contract(field(catalog, "schemaVersion") == 1 && field(catalog, "presets").is_array(), "Invalid scenario catalog");
  auto definition = field(catalog, "contract");
  need_contract(str(definition, "protocol") == "v8trace/scenario-contract/1", "Scenario contract missing or incompatible; update runtime and app assets together");
  for (auto key : {"producers", "dependencies", "budgets", "scopes"}) need_contract(field(definition, key).is_array(), "Scenario contract fields must be arrays");
  std::set<std::string> ids;
  for (const auto& preset : catalog.at("presets")) need_contract(!str(preset, "id").empty() && ids.insert(str(preset, "id")).second && field(preset, "options").is_object(), "Scenario presets require unique identities and option objects");
  catalog["catalogSha256"] = sha256(bytes); return catalog;
}
Json build_contract(const Json& original, const Json& catalog) {
  auto options = original; Json selected; auto id = str(original, "preset");
  if (!id.empty()) {
    for (const auto& preset : catalog.at("presets")) if (str(preset, "id") == id) { selected = preset; options = preset.at("options"); options.update(original); break; }
    need_contract(!selected.is_null(), "Unknown scenario contract preset");
  }
  const auto& definition = catalog.at("contract"); auto coverage = capabilities(options);
  Json producers = Json::array(), dependencies = Json::array(), budgets = Json::object(), scopes = Json::object(), overrides = Json::array();
  for (const auto& producer : definition.at("producers")) {
    auto option = str(producer, "option"), capability = str(producer, "capability");
    need_contract(!option.empty() && !capability.empty(), "Invalid scenario producer definition");
    if (active(options, option)) {
      auto implementation = str(coverage, capability, "unavailable");
      producers.push_back({{"option", option}, {"capability", capability}, {"producer", implementation}, {"label", str(producer, "label")},
        {"satisfied", implementation != "disabled" && implementation != "unavailable"}});
    }
  }
  for (const auto& dependency : definition.at("dependencies")) {
    auto option = str(dependency, "option"); if (!active(options, option)) continue;
    auto required = field(dependency, "requires", Json::array()); need_contract(required.is_array(), "Invalid scenario dependency definition");
    bool satisfied = true; for (const auto& name : required) satisfied = satisfied && active(options, text(name));
    dependencies.push_back({{"option", option}, {"requires", required}, {"satisfied", satisfied}, {"code", str(dependency, "code")}});
  }
  for (const auto& name : definition.at("budgets")) if (options.contains(text(name))) budgets[text(name)] = options.at(text(name));
  for (const auto& name : definition.at("scopes")) if (options.contains(text(name))) scopes[text(name)] = options.at(text(name));
  if (!selected.is_null()) for (auto it = selected.at("options").begin(); it != selected.at("options").end(); ++it)
    if (original.contains(it.key()) && original.at(it.key()) != it.value()) overrides.push_back(it.key());
  auto restricted = edition_blocked_options(options);
  return {{"protocol", "v8trace/scenario-contract/1"}, {"available", true}, {"edition", edition_id()}, {"editionAllowed", restricted.empty()}, {"restrictedOptions", restricted}, {"catalogVersion", str(catalog, "catalogVersion")},
    {"catalogSha256", catalog.at("catalogSha256")}, {"presetId", id}, {"scopeRequired", field(selected, "requiresScriptScope", false)},
    {"requiredProducers", producers}, {"dependencies", dependencies}, {"budgets", budgets}, {"scopes", scopes},
    {"overriddenPresetFields", overrides}, {"browserVerified", false}, {"verificationScope", "effective selected options; not page execution or evidence completeness"}};
}
Json option_subset(const Json& options, const std::vector<std::string>& names) {
  Json result = Json::object(); for (const auto& name : names) if (options.contains(name)) result[name] = options.at(name); return result;
}
}

Json scenario_catalog(const fs::path& assets) {
  auto catalog = load_catalog(assets);
  for (auto& preset : catalog.at("presets")) { auto options = preset.at("options"); options["preset"] = preset.at("id"); preset["contract"] = build_contract(options, catalog); }
  return catalog;
}
Json scenario_contract(const Json& options, const fs::path& assets) {
  if (!fs::is_regular_file(assets / "scenario_presets.json"))
    return {{"protocol", "v8trace/scenario-contract/1"}, {"available", false}, {"browserVerified", false}, {"reason", "Scenario catalog absent; explicit options still undergo engine validation"}};
  return build_contract(options, load_catalog(assets));
}
Json gap_diagnostic(const Json& gap, const Json& options) {
    auto reason = str(gap, "reason", "unknown"); auto details = field(gap, "details", Json::object());
    auto critical = field(gap, "critical");
    std::vector<std::string> related; Json suggested = Json::object(); std::string action = "打开原始缺口记录，核对实际来源与阶段。";
    if (reason == "capture-byte-budget") related = {"maxBytes"};
    else if (reason == "capture-event-budget") related = {"maxEvents"};
    else if (reason.find("body-budget") != std::string::npos || reason.find("body-byte-budget") != std::string::npos) related = {"maxBodyBytes"};
    else if (reason.find("redirect") != std::string::npos && reason.find("body") != std::string::npos) {
      related = {"http", "redirectBodies", "maxBodyBytes"}; suggested = {{"http", true}, {"redirectBodies", true}};
      action = "核对重定向正文生产者、响应状态和正文结束回执；参数预览不代表网络失败已解决。";
    } else if (reason == "response-body-unavailable" || reason.find("response-body-missing") != std::string::npos) {
      related = {"http", "redirectBodies", "maxBodyBytes"}; action = "核对实际请求状态、正文生产回执与错误；不将所有正文缺口归因于同一个开关。";
    }
    if (related.size() == 1) {
      auto name = related.front(); auto current = field(options, name);
      if (safe_uint(current, true)) { auto value = current.get<uint64_t>(); constexpr uint64_t limit = 9007199254740991ULL; if (value < limit) suggested[name] = value > limit / 2 ? limit : value * 2; }
      action = "核对资源余量后再调整预算；增加预算只对下一次明确启动的采集生效。";
    }
    Json identity = Json::object(); if (details.is_object()) for (auto key : {"requestId", "sessionId", "processId", "isolateId", "source", "file", "url", "producer", "phase"})
      if (details.contains(key) && !details.at(key).is_structured()) identity[key] = details.at(key);
    return {{"protocol", "v8trace/gap-diagnostic/1"}, {"gapSeq", field(gap, "gapSeq")}, {"reason", reason}, {"critical", critical},
      {"stage", str(details, "phase", str(details, "stage", "unknown"))}, {"source", str(details, "source", str(details, "producer", "unknown"))},
      {"observedIdentity", identity}, {"evidenceFile", "gaps.jsonl"}, {"sourceOptionsFile", "capture-options.json"},
      {"relatedOptions", option_subset(options, related)}, {"suggestedOptions", suggested}, {"action", action}, {"requiresExplicitReview", true},
      {"suggestionScope", "option patch only; review against source capture options and choose a new output before recapture"}, {"automaticallyApplied", false}};
}
Json capture_diagnostics(const Json& summary, const Json& options) {
  Json findings = Json::array(); uint64_t observed_critical = 0; size_t observed = 0;
  auto rows = field(summary, "gaps", Json::array());
  if (rows.is_array()) for (const auto& gap : rows) {
    if (!gap.is_object()) continue; ++observed;
    if (field(gap, "critical") == true) ++observed_critical;
    findings.push_back(gap_diagnostic(gap, options));
  }
  auto reported = field(summary, "criticalGapCount", 0);
  auto reported_critical = safe_uint(reported) ? reported.get<uint64_t>() : 0;
  return {{"protocol", "v8trace/capture-diagnostics/1"}, {"captureConclusion", field(summary, "captureConclusion")},
    {"integrityConclusion", field(summary, "integrityConclusion")}, {"reportedCriticalGaps", reported_critical},
    {"observedCriticalGaps", observed_critical}, {"unlistedCriticalGaps", reported_critical > observed_critical ? reported_critical - observed_critical : 0},
    {"observedGapRecords", observed}, {"findings", findings}, {"allGapRecords", "gaps.jsonl"},
    {"fullEvidenceNavigation", "index gaps.jsonl and read original byte pages"}, {"sourceModified", false}, {"automaticallyAppliesOptions", false}};
}
}
