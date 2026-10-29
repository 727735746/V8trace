#include "v8trace/edition.h"
#include "v8trace/studio.h"
#include <iostream>

using namespace v8trace;
namespace {
unsigned checks = 0;
void check(bool condition, const std::string& message) { ++checks; if (!condition) throw std::runtime_error(message); }
template<class F> void rejected(F action, const std::string& message) {
  bool failed = false; try { action(); } catch (const std::exception&) { failed = true; } check(failed, message);
}
}
int main() {
  try {
    auto assets = path(V8TRACE_EDITION_ASSETS);
    Json input{{"url", "https://fixture.test/"}, {"output", utf8(fs::temp_directory_path() / ("v8trace-edition-" + uuid()))}, {"http", true}};
    auto identity = edition_info();
    check(identity.at("id") == std::string(edition_id()) && identity.at("buildTimeIdentity") == true, "immutable compiled identity");
    check(identity.at("commercialTruncation") == false && identity.at("selectedValues") == "full", "no commercial loss of evidence");
    for (auto preset : {"env-protocol", "env-target-detail"}) {
      auto options = input; options["preset"] = preset; options["detailScriptInclude"] = "fixture.js";
      if (std::string_view(preset) == "env-protocol") options.erase("detailScriptInclude");
      auto plan = validate_options(options, assets, false);
      for (auto key : {"http", "scripts", "hooks", "redirectBodies", "jsCallValues"}) check(plan.at(key) == true, std::string(preset) + " complete " + key);
      const auto originals = load_json(assets / "scenario_presets.json", 1048576).at("presets");
      for (const auto& original : originals) if (original.at("id") == preset)
        for (auto key : {"maxBytes", "maxEvents", "maxBodyBytes"}) check(plan.at(key) == original.at("options").at(key), "unchanged preset budget " + std::string(key));
      if (std::string_view(preset) == "env-target-detail") {
        check(plan.at("jsCallDetail") == true && plan.at("nativeDetail") == true, "full targeted calls");
        check(plan.at("detailFilter").at("selectedCallsRetainFullValues") == true, "selected calls retain full original values");
      }
    }
    auto complete = input;
    for (auto key : {"wasmModules", "envAbsentProbe", "envInterfaceProbe", "envHostMutation", "envInventory", "cookies", "storage", "indexedDB", "webSocket", "chunkSpool", "recordingControl", "objectSnapshots", "jsCallDetail", "nativeDetail", "closureBindings"}) complete[key] = true;
    complete["detailScriptInclude"] = "fixture.js";
    check(validate_options(complete, assets, false).at("closureBindings") == true, "all environment evidence channels remain available");
    auto explicit_budget = input; explicit_budget.update({{"maxBytes", 4294967296ULL}, {"maxEvents", 5000000}, {"maxBodyBytes", 100000000}});
    auto budget = validate_options(explicit_budget, assets, false);
    for (auto key : {"maxBytes", "maxEvents", "maxBodyBytes"}) check(budget.at(key) == explicit_budget.at(key), "no edition-specific budget " + std::string(key));
    for (const auto& key : professional_capture_options()) {
      auto options = input; options[text(key)] = true;
      check(edition_blocked_options(options).empty() == professional_edition(), "per-option compile policy " + text(key));
      if (!professional_edition()) {
        rejected([&] { validate_options(options, assets, false); }, "direct option rejected " + text(key));
        rejected([&] { configuration_preview(input, options, "merge"); }, "import rejected without downgrade " + text(key));
        auto report = preflight_capture(options, assets, false);
        check(report.at("ok") == false && str(report, "error").find(text(key)) != std::string::npos, "preflight explains exact blocked option");
      }
      options[text(key)] = false; validate_edition_options(options); check(true, "disabled professional setting may roundtrip");
    }
    unsigned unavailable = 0;
    for (const auto& preset : presets(assets)) {
      if (preset.at("available") == false) {
        ++unavailable; auto options = input; options["preset"] = preset.at("id");
        rejected([&] { validate_options(options, assets, false); }, "preset enforces backend policy");
      }
      if (preset.at("id") == "env-protocol" || preset.at("id") == "env-target-detail") check(preset.at("available") == true && preset.at("requiredEdition") == "community", "environment presets always supported");
    }
    check(unavailable == (professional_edition() ? 0 : 4), "all 21 presets classified");
    auto spoof = input; spoof["edition"] = professional_edition() ? "community" : "professional";
    rejected([&] { validate_options(spoof, assets, false); }, "configuration never changes compiled edition");
    validate_browser_edition({{"edition", edition_id()}});
    rejected([&] { validate_browser_edition({{"edition", professional_edition() ? "community" : "professional"}}); }, "browser pairing rejects opposite edition");
    if (professional_edition()) validate_browser_edition(Json::object());
    else rejected([&] { validate_browser_edition(Json::object()); }, "legacy full browser is not a community browser");
    std::cout << "edition " << edition_id() << ": " << checks << " checks passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
