#include "v8trace/core.h"
#include "v8trace/edition.h"
#include <iostream>
#include <stdexcept>

using namespace v8trace;
namespace {
unsigned checks = 0;
void check(bool value, const std::string& name) { ++checks; if (!value) throw std::runtime_error(name); }
template<class F> void rejected(F fn, const std::string& name) { bool failed = false; try { fn(); } catch (const std::exception&) { failed = true; } check(failed, name); }
}

int main() {
  try {
    const auto assets = path(V8TRACE_CATALOG_ASSETS);
    const auto catalog = presets(assets);
    check(catalog.size() == 21, "21 presets");
    const std::map<std::string, std::string> categories{
      {"capture-baseline", "http,script,console,exception"},
      {"env-protocol", "http,script,env,console,exception"},
      {"jscall-structure", "http,script,console,exception,jscall"},
      {"env-target-detail", "http,script,env,console,exception,jscall,native"},
      {"sign-call", "http,script,console,exception,jscall,native"},
      {"native-target", "http,script,console,exception,native"},
      {"snapshot-target", "http,script,console,exception,jscall,native"},
      {"jsvmp-opcode", "http,script,console,exception,opcode"},
      {"wasm-execution", "http,script,console,exception,wasm"},
      {"http-protocol", "http,script,console,exception"},
      {"exception-call", "script,console,exception,jscall"},
      {"cdp-performance", "http,script,lifecycle,console,exception,jscall,native"},
      {"all-short", "http,script,env,console,exception,jscall,native,opcode,wasm"},
      {"env-native-probes", "http,script,console,exception,env"},
      {"closure-target", "http,script,console,exception,jscall"},
      {"session-state", "http,script,console,exception,cookie,storage"},
      {"websocket-state", "http,script,console,exception,websocket"},
      {"indexeddb-state", "http,script,console,exception,indexeddb"},
      {"wasm-modules", "http,script,console,exception,wasm-module"},
      {"large-log-spool", "http,script,console,exception"},
      {"http-root-chain", "http,script,console,exception,jscall,native,opcode,causality"}
    };
    const std::set<std::string> scoped{"env-target-detail", "sign-call", "native-target", "snapshot-target", "closure-target"};
    const std::vector<std::string> rule_keys{"detailScriptInclude", "detailScriptExclude", "jsFunctionInclude", "jsFunctionExclude", "nativeNameInclude", "nativeNameExclude", "jsValueScriptInclude", "jsValueScriptExclude", "jsValueFunctionInclude", "jsValueFunctionExclude"};
    std::set<std::string> seen;
    for (const auto& preset : catalog) {
      const auto id = str(preset, "id"); const auto& defaults = preset.at("options");
      check(seen.insert(id).second && categories.contains(id), id + " stable unique id");
      check(!str(preset, "stage").empty() && !str(preset, "risk").empty(), id + " stage/risk");
      check(preset.at("requiresScriptScope") == scoped.contains(id), id + " scope contract");
      Json input{{"preset", id}, {"url", "https://example.test/"}, {"output", utf8(fs::temp_directory_path() / ("v8trace-catalog-" + uuid()))}};
      if (scoped.contains(id)) {
        rejected([&] { validate_options(input, assets, false); }, id + " blank scope");
        for (const auto& scope : {"*", "?* ?", "target.js\n*", " \n \t"}) {
          auto invalid = input; invalid["detailScriptInclude"] = scope;
          rejected([&] { validate_options(invalid, assets, false); }, id + " invalid/global union scope");
        }
        input["detailScriptInclude"] = "*target*.js\nhttps://example.test/assets/*.js*";
      }
      if (preset.at("available") == false) {
        check(!professional_edition() && preset.at("requiredEdition") == "professional", id + " compiled edition restriction");
        rejected([&] { validate_options(input, assets, false); }, id + " rejects rather than downgrades");
        continue;
      }
      const auto normalized = validate_options(input, assets, false);
      check(normalized.at("categories") == Json(split(categories.at(id), ',')), id + " selected channels");
      check(normalized.at("scenario").at("id") == id && normalized.at("scenario").at("stage") == preset.at("stage"), id + " provenance");
      check(normalized.at("scenario").at("requiresScriptScope") == scoped.contains(id), id + " scope provenance");
      check(normalized.at("writeBufferBytes") == 8388608 && normalized.at("writeBatchBytes") == 1048576, id + " explicit lossless writer");
      check(normalized.at("rawCdp") == false && normalized.at("headless") == false, id + " no hidden raw/headless");
      check(normalized.at("jsCallValues") == (id != "jscall-structure" && id != "exception-call"), id + " call values mode");
      check(normalized.at("opcode") == (id == "jsvmp-opcode" || id == "all-short" || id == "http-root-chain"), id + " opcode opt-in");
      check(normalized.at("wasm") == (id == "wasm-execution" || id == "all-short"), id + " WASM opt-in");
      check(normalized.at("objectSnapshots") == (id == "snapshot-target" || id == "all-short" || id == "http-root-chain"), id + " snapshots opt-in");
      for(auto key:{"httpRootTrace","domTrace","cryptoTrace"}) check(normalized.at(key)==(id=="http-root-chain"),id+" explicit request instrumentation opt-in");
      check(normalized.at("transport") == (id == "cdp-performance" ? "cdp" : "native"), id + " transport contract");
      for (const auto& key : rule_keys) {
        check(defaults.contains(key) && defaults.at(key).is_string(), id + " complete filter state " + key);
        check(normalized.at(key) == (key == "detailScriptInclude" && scoped.contains(id) ? input.at(key) : defaults.at(key)), id + " filter application " + key);
      }
      if (id == "env-protocol") for (const auto& key : {"jsCallDetail", "nativeDetail", "objectSnapshots", "opcode", "correlations"}) check(normalized.at(key) == false, "environment stage is not all-detail");
      if (id == "native-target") check(normalized.at("jsCallDetail") == false && normalized.at("correlations") == false, "native independent");
      if (scoped.contains(id)) {
        auto custom = input;
        custom["jsCallDetail"] = false; custom["nativeDetail"] = false; custom["correlations"] = false; custom["objectSnapshots"] = false; custom["closureBindings"] = false;
        for (const auto& key : rule_keys) custom[key] = "";
        custom["detailUnknownSource"] = "include";
        check(validate_options(custom, assets, false).at("detailFilter").at("active") == false, id + " explicit disabled filters override preset");
      }
    }
    const auto prompts = load_json(assets / "case_prompt_templates.json", 1048576).at("templates");
    const std::set<std::string> prompt_ids{"env-reproduction", "purecalc-restoration", "evidence-handoff", "jsvmp-evidence", "capture-performance", "wasm-evidence"};
    check(prompts.size() == prompt_ids.size(), "6 prompt templates"); seen.clear();
    for (const auto& prompt : prompts) {
      const auto id = str(prompt, "id"); check(prompt_ids.contains(id) && seen.insert(id).second, id + " unique template id");
      if (!edition_prompt_available(id)) { rejected([&] { render_prompt(assets, {{"templateId", id}}); }, id + " unavailable professional workflow is explicit");continue; }
      const auto rendered = render_prompt(assets, {{"templateId", id}, {"fields", {{"url", "https://example.test/"}, {"goal", "中文😀\n保留 {{goal}} 字面量"}, {"caseDirectory", "case/中文案例"}}}});
      const auto body = str(rendered, "text");
      check(body.size() > 8000, id + " detailed handoff");
      for (const auto& value : {"https://example.test/", "中文😀\n保留 {{goal}} 字面量", "case/中文案例", "UTF-8", "CLEANUP.md"}) check(body.find(value) != std::string::npos, id + " exact field/handoff content");
      check(rendered.at("executesCapture") == false && rendered.at("executesReproduction") == false, id + " rendering does not execute");
      check(!str(render_prompt(assets, {{"templateId", id}}), "text").empty(), id + " default render");
      rejected([&] { render_prompt(assets, {{"templateId", id}, {"fields", {{"url", "file:///invalid"}}}}); }, id + " URL negative control");
    }
    rejected([&] { render_prompt(assets, {{"templateId", "missing-template"}}); }, "unknown template");
    std::cout << "PASS " << checks << " catalog checks\n"; return 0;
  } catch (const std::exception& error) { std::cerr << "FAIL " << checks << ": " << error.what() << '\n'; return 1; }
}
