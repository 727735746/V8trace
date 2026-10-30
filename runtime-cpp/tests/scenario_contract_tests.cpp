#include "v8trace/scenario_contract.h"
#include "v8trace/studio.h"
#include "v8trace/edition.h"
#include <iostream>

using namespace v8trace;
namespace {
unsigned checks = 0;
void check(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
template<class F> void reject(F action, const char* message) { bool failed = false; try { action(); } catch (const std::exception&) { failed = true; } check(failed, message); }
}
int main() {
  auto root = fs::temp_directory_path() / ("v8trace-scenario-" + uuid()); fs::create_directory(root);
  try {
    auto assets = path(V8TRACE_SCENARIO_ASSETS); auto catalog = scenario_catalog(assets);
    check(catalog.at("presets").size() == 21, "all presets have a contract");
    Json base{{"url", "https://fixture.test/"}, {"output", utf8(root / "not-created")}};
    for (const auto& preset : presets(assets)) {
      auto input = base; input["preset"] = preset.at("id");
      if (field(preset, "requiresScriptScope") == true) input["detailScriptInclude"] = "fixture.js";
      auto before = input; auto report = preflight_capture(input, assets, false);
      if (preset.at("available") == false) {
        check(report.at("ok") == false && report.at("scenarioContract").at("editionAllowed") == false, "edition rejection retains requested contract");
        continue;
      }
      check(report.at("ok") == true && report.at("rawOptions") == input && input == before, "structural preflight preserves input");
      auto plan = report.at("plan"); auto contract = plan.at("scenarioContract");
      check(contract == report.at("scenarioContract") && contract.at("browserVerified") == false, "plan/preflight share effective contract");
      check(contract.at("catalogSha256") == catalog.at("catalogSha256"), "raw catalog hash binds contract");
      for (const auto& producer : contract.at("requiredProducers"))
        check(producer.at("satisfied") == true && producer.at("producer") == capabilities(plan).at(str(producer, "capability")), "required producer matches actual engine coverage");
      for (const auto& dependency : contract.at("dependencies")) check(dependency.at("satisfied") == true, "all effective dependencies satisfied");
      for (auto it = contract.at("budgets").begin(); it != contract.at("budgets").end(); ++it) check(plan.at(it.key()) == it.value(), "exact effective budgets");
      auto explicit_input = preset.at("options"); explicit_input.update(input);
      check(validate_options(explicit_input, assets, false) == plan, "explicit UI-style options and preset CLI options normalize identically");
      if (field(preset, "requiresScriptScope") == true) {
        auto missing = input; missing.erase("detailScriptInclude"); reject([&] { validate_options(missing, assets, false); }, "required scope not silently expanded");
        missing["detailScriptInclude"] = "*\n??"; reject([&] { validate_options(missing, assets, false); }, "global scope rejected");
      }
      if (str(preset.at("options"), "transport") == "native" && field(preset.at("options"), "http") == true)
        check(plan.at("redirectBodies") == true, "native HTTP presets retain redirect body producer");
    }
    auto input = base; input["preset"] = "env-protocol"; input["maxBytes"] = 123456789; input["customField"] = Json{{"value", "完整😀"}};
    auto report = preflight_capture(input, assets, false);
    check(report.at("ok") == true && report.at("plan").at("scenarioContract").at("budgets").at("maxBytes") == 123456789, "non-MiB budget stays exact");
    check(report.at("unknownFields") == Json::array({"customField"}) && report.at("plan").at("customField") == input.at("customField"), "unknown fields retained and disclosed");
    check(!fs::exists(root / "not-created"), "preflight creates no capture");
    input["chromePath"] = utf8(root / "missing-browser.exe");
    report = preflight_capture(input, assets);
    check(report.at("ok") == false && report.at("browserVerified") == false && report.at("scenarioContract").at("browserVerified") == false, "missing browser never claims verified");
    auto manual = base; manual["http"] = true;
    check(validate_options(manual, root, false).at("scenarioContract").at("available") == false, "manual legacy assets retain engine validation");
    auto bad_catalog = catalog;
    for (auto& producer : bad_catalog["contract"]["producers"]) if (str(producer, "option") == "http") producer["capability"] = "fixture.missing";
    write_json(root / "scenario_presets.json", bad_catalog);
    reject([&] { validate_options(manual, root, false); }, "unknown required producer explicitly rejected");
    bad_catalog["contract"].erase("budgets"); write_json(root / "scenario_presets.json", bad_catalog);
    reject([&] { scenario_catalog(root); }, "incompatible assets fail clearly");
    auto options = Json{{"http", true}, {"redirectBodies", false}, {"maxBytes", 123456789}, {"maxEvents", 100000}, {"maxBodyBytes", 65536}};
    Json gap{{"gapSeq", 9}, {"reason", "response-body-unavailable"}, {"critical", true}, {"details", {{"requestId", "req-7"}, {"source", "network-service"}, {"phase", "body-read"}}}};
    auto diagnostic = gap_diagnostic(gap, options);
    check(diagnostic.at("source") == "network-service" && diagnostic.at("stage") == "body-read" && diagnostic.at("observedIdentity").at("requestId") == "req-7", "source/stage/identity are observed");
    check(diagnostic.at("suggestedOptions").empty(), "generic body failure not misdiagnosed as redirect switch");
    gap["reason"] = "redirect-body-missing"; diagnostic = gap_diagnostic(gap, options);
    check(diagnostic.at("suggestedOptions").at("redirectBodies") == true && diagnostic.at("automaticallyApplied") == false, "redirect suggestion explicit, never applied");
    gap["reason"] = "capture-byte-budget"; diagnostic = gap_diagnostic(gap, options);
    check(diagnostic.at("suggestedOptions").at("maxBytes") == 246913578 && options.at("maxBytes") == 123456789, "budget patch exact and source unchanged");
    gap["reason"] = "fixture-unknown"; gap["details"] = Json::object(); diagnostic = gap_diagnostic(gap, options);
    check(diagnostic.at("source") == "unknown" && diagnostic.at("stage") == "unknown" && diagnostic.at("suggestedOptions").empty(), "missing evidence stays unknown");
    for (const auto& conclusion : {"collected", "failed"}) {
      auto summary = Json{{"captureConclusion", conclusion}, {"integrityConclusion", "complete"}, {"criticalGapCount", 0}, {"gaps", Json::array()}};
      auto before = summary; diagnostic = capture_diagnostics(summary, options);
      check(summary == before && diagnostic.at("captureConclusion") == conclusion && diagnostic.at("integrityConclusion") == "complete", "diagnostics never rewrite failed/complete conclusions");
    }
    {
      Store store(root / "capture", options);
      for (unsigned i = 0; i < 251; ++i) store.gap("fixture-unknown", {{"phase", "observed-phase"}, {"raw", std::string("原始\0值", 10)}});
      auto summary = store.finalize({{"captureConclusion", "failed"}});
      check(summary.at("captureConclusion") == "failed" && summary.at("integrityConclusion") == "partial" && summary.at("criticalGapCount") == 251, "critical gaps still reject complete");
      check(summary.at("diagnostics").at("findings").size() == 200 && summary.at("diagnostics").at("unlistedCriticalGaps") == 51, "bounded summary explicitly accounts for omitted critical rows");
      unsigned rows = 0; lines(store.filename("gaps.jsonl"), [&](const Json& row) {
        ++rows; check(row.at("details").at("raw") == std::string("原始\0值", 10), "raw gap value retained");
        check(row.at("diagnostic").at("gapSeq") == rows && row.at("diagnostic").at("stage") == "observed-phase", "all journal gaps have actionable or explicit unknown diagnostics");
      });
      check(rows == 251 && file_sha256(store.filename("gaps.jsonl")) == str(summary.at("gapJournal"), "sha256"), "full journal and hash remain bound");
    }
    fs::remove_all(root); std::cout << "PASS " << checks << " scenario-contract checks\n"; return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL " << error.what() << "\nfixture " << utf8(root) << "\n"; return 1;
  }
}
