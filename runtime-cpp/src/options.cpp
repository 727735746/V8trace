#include "v8trace/core.h"
#include "v8trace/edition.h"
#include "v8trace/state.h"
#include "v8trace/vmp_probe.h"
#include <algorithm>
#include <regex>

namespace v8trace {
namespace {
const std::vector<std::string> booleans{"httpRootTrace", "domTrace", "cryptoTrace", "headless", "disableExtensions", "hooks", "cpuProfile", "nativeTimeline", "opcode", "jsCallDetail", "jsCallValues", "nativeDetail", "wasm", "objectSnapshots", "correlations", "http", "redirectBodies", "scripts", "contextEvents", "consoleEvents", "exceptionEvents", "rawCdp", "netLog", "recordingControl", "startPaused", "envAbsentProbe", "envInterfaceProbe", "envHostMutation", "envInventory"};
const std::vector<std::pair<std::string, std::string>> browser_options{{"hooks", "env"}, {"http", "http"}, {"scripts", "script"}, {"consoleEvents", "console"}, {"exceptionEvents", "exception"}};
bool enabled(const Json& value, std::string_view key) { return field(value, key, false) == true; }
bool hash_string(const Json& value) { return value.is_string() && std::regex_match(text(value), std::regex("[a-f0-9]{64}")); }
void require(bool value, std::string_view error) { if (!value) throw std::runtime_error(std::string(error)); }
void supported(const Json& descriptor, std::string_view name, const std::string& channel) { auto features = field(descriptor, name, Json::array()); require(features.is_array() && std::find(features.begin(), features.end(), Json(channel)) != features.end(), "Browser descriptor does not advertise selected channel: " + channel); }
std::vector<std::string> details(const Json& o) { std::vector<std::string> out; for (auto [key, channel] : std::vector<std::pair<std::string, std::string>>{{"jsCallDetail", "jscall"}, {"nativeDetail", "native"}, {"wasm", "wasm"}}) if (enabled(o, key)) out.push_back(channel); return out; }
}
Json presets(const fs::path& assets) {
  auto catalog = load_json(assets / "scenario_presets.json", 1048576).at("presets");
  for (auto& preset : catalog) {
    auto blocked = edition_blocked_options(preset.at("options"));
    preset["available"] = blocked.empty(); preset["restrictedOptions"] = blocked;
    bool advanced = false;
    for (const auto& key : professional_capture_options()) if (field(preset.at("options"), text(key), false) == true) advanced = true;
    preset["requiredEdition"] = advanced ? "professional" : "community";
  }
  return catalog;
}
Json native_kinds() { Json out = Json::array(); const char* ids[]{"builtin-js", "builtin-cpp", "runtime", "api-operation", "api-getter", "api-setter", "api-interceptor"}; const char* labels[]{"JS 可见 builtin", "C++ builtin", "V8 Runtime", "API 方法 / 构造", "API 属性读取", "API 属性写入", "API 拦截器 / 枚举"}; for (size_t i = 0; i < 7; ++i) out.push_back({{"id", ids[i]}, {"label", labels[i]}}); return out; }
Json normalize_filter(const Json& options) {
  const std::vector<std::pair<std::string, std::string>> fields{{"detailScriptInclude", "S+"}, {"detailScriptExclude", "S-"}, {"jsFunctionInclude", "J+"}, {"jsFunctionExclude", "J-"}, {"nativeNameInclude", "N+"}, {"nativeNameExclude", "N-"}, {"jsValueScriptInclude", "D+"}, {"jsValueScriptExclude", "D-"}, {"jsValueFunctionInclude", "F+"}, {"jsValueFunctionExclude", "F-"}};
  Json rules = Json::object(); size_t count = 0; std::string rule_token; bool value_rules = false;
  constexpr char hex[] = "0123456789abcdef";
  for (auto& [name, key] : fields) {
    auto input = field(options, name, ""); require(input.is_string() && text(input).find('\0') == std::string::npos, name + " must be text without NUL"); rules[name] = Json::array();
    for (auto pattern : split(text(input), '\n')) { pattern = trim(pattern); if (pattern.empty()) continue; auto data = utf16le(pattern); require(data.size() <= 4096 && ++count <= 128, "Detail filter exceeds 128 rules or 2048 UTF-16 units"); rules[name].push_back(pattern); value_rules = value_rules || name.starts_with("jsValue"); rule_token += key + '='; for (auto b : data) { rule_token += hex[uint8_t(b) >> 4]; rule_token += hex[uint8_t(b) & 15]; } rule_token += ';'; }
  }
  auto kind_value = field(options, "nativeKinds", "all"); require(kind_value.is_string(), "nativeKinds must be text"); auto kind_text = text(kind_value); auto kinds = split(kind_text, ','); unsigned mask = 0; std::set<std::string> unique;
  auto catalog = native_kinds();
  for (auto& kind : kinds) { kind = trim(kind); require(!kind.empty() && unique.insert(kind).second, "Invalid nativeKinds"); if (kind == "all") { require(kinds.size() == 1, "all must be the only nativeKind"); mask = 127; } else { bool found = false; for (size_t i = 0; i < catalog.size(); ++i) if (str(catalog[i], "id") == kind) { mask |= 1u << i; found = true; } require(found, "Unknown Native kind"); } }
  auto exclude = field(options, "detailExcludeInternal", false); require(exclude.is_boolean(), "detailExcludeInternal must be boolean"); auto unknown = str(options, "detailUnknownSource", rules["detailScriptInclude"].empty() ? "include" : "exclude"); require(unknown == "include" || unknown == "exclude", "Invalid detailUnknownSource");
  const bool active = count || kind_text != "all" || exclude == true || unknown != "include";
  require(!active || enabled(options, "jsCallDetail") || enabled(options, "nativeDetail"), "Detail filtering requires JSCall or Native detail");
  require(!value_rules || (enabled(options, "jsCallDetail") && field(options, "jsCallValues", true) == true), "JSCall value filters require JSCall values");
  std::string token = std::string(value_rules ? "2;K=" : "1;K=") + std::to_string(mask) + ";U=" + (unknown == "include" ? "1" : "0") + ";X=" + (exclude == true ? "1" : "0") + ";" + rule_token;
  require(token.size() <= 16384, "Encoded filter exceeds 16 KiB");
  return {{"protocol", value_rules ? "v8trace/detail-filter/2" : "v8trace/detail-filter/1"}, {"active", active}, {"valueRules", value_rules}, {"token", active ? token : ""}, {"rules", rules}, {"nativeKinds", kinds}, {"excludeInternal", exclude}, {"unknownSource", unknown}, {"matching", "exact-or-star-question-glob; basename when script pattern has no slash"}, {"filteringStage", "native-before-parameter-serialization"}, {"selectedCallsRetainFullValues", field(options, "jsCallValues", true) == true && !value_rules}, {"selectedValueCallsRetainFullValues", true}};
}
Json verify_backend(const fs::path& executable) {
  auto binary = fs::canonical(executable); fs::path root = binary.parent_path(), descriptor_file = root / "v8trace-opcode.json";
  if (!fs::exists(descriptor_file) && binary.parent_path().filename() == "MacOS") { root = binary.parent_path().parent_path(); descriptor_file = root / "Resources" / "v8trace-opcode.json"; }
  auto descriptor = load_json(descriptor_file, 65536);
  require(field(descriptor, "schemaVersion") == 1 && str(descriptor, "protocol") == "v8trace/opcode/1" && str(descriptor, "category") == "disabled-by-default-v8.opcode" && str(descriptor, "backend") == "chromium-opcode", "Unsupported Chromium descriptor");
  auto hashes = field(descriptor, "binaryHashes"); require(hashes.is_object() && !hashes.empty(), "Missing Chromium binary hashes");
  bool executable_verified = false, engine_verified = false;
  for (auto it = hashes.begin(); it != hashes.end(); ++it) {
    auto relative = path(it.key()); require(!relative.is_absolute() && it.key().find('\\') == std::string::npos && hash_string(it.value()), "Invalid browser binary identity");
    auto file = fs::canonical(root / relative); require(inside(root, file), "Browser binary hash path leaves package"); require(file_sha256(file) == text(it.value()), "Browser binary SHA256 mismatch: " + it.key());
    if (file == binary) executable_verified = true;
    if (relative.filename() == "chrome.dll" || relative.filename() == "Chromium Framework" || relative.filename() == "Google Chrome Framework") engine_verified = true;
  }
  require(executable_verified && engine_verified, "Descriptor must identify executable and Chrome engine library");
  validate_browser_edition(descriptor);
  return descriptor;
}
Json validate_options(Json input, const fs::path& assets, bool verify_browser) {
  require(input.is_object(), "Options must be an object");
  bool requires_scope = false;
  if (input.contains("preset")) { auto id = str(input, "preset"); bool found = false; for (auto& preset : presets(assets)) if (str(preset, "id") == id) { Json merged = preset.at("options"); merged.update(input); input = merged; requires_scope = field(preset, "requiresScriptScope", false) == true; input["scenario"] = {{"id", id}, {"label", preset.at("label")}, {"stage", field(preset, "stage", "")}, {"requiresScriptScope", requires_scope}}; found = true; break; } require(found, "Unknown capture preset"); }
  validate_edition_options(input);
  auto interpret_only=field(input,"interpretOnly",false);require(interpret_only.is_boolean(),"interpretOnly must be boolean");
  Json o{{"transport", "native"}, {"durationSeconds", 30}, {"headless", false}, {"hooks", false}, {"cpuProfile", false}, {"nativeTimeline", false}, {"opcode", false}, {"jsCallDetail", false}, {"nativeDetail", false}, {"wasm", false}, {"objectSnapshots", false}, {"correlations", false}, {"objectDepth", 2}, {"objectNodes", 64}, {"objectProperties", 256}, {"http", false}, {"redirectBodies", false}, {"scripts", false}, {"contextEvents", false}, {"consoleEvents", false}, {"exceptionEvents", false}, {"rawCdp", false}, {"netLog", false}, {"maxBytes", 268435456ULL}, {"maxEvents", 100000}, {"maxBodyBytes", 16777216}, {"commandTimeout", 15000}, {"traceFlushTimeout", 120000}};
  o["httpRootTrace"] = false;
  o["domTrace"] = false;o["cryptoTrace"] = false;
  o.update(input); o["interpretOnly"]=interpret_only; const bool native = str(o, "transport") == "native";
  require(field(o,"httpRootTrace").is_boolean(), "httpRootTrace must be boolean");
  require(!enabled(o,"httpRootTrace") || (native && enabled(o,"http")), "HTTP root instrumentation requires native transport and HTTP capture");
  for(auto key : {"domTrace","cryptoTrace"}) {
    require(field(o,key).is_boolean(),"DOM/crypto trace switches must be boolean");
    require(!enabled(o,key) || enabled(o,"httpRootTrace"),"Native DOM/crypto checkpoints require HTTP-root causality capture");
  }
  if (!o.contains("disableExtensions")) o["disableExtensions"] = false;
  for (auto key : {"chunkSpool", "streamTCP"}) { if (!o.contains(key)) o[key] = false; require(o[key].is_boolean(), "Stream switch must be boolean"); }
  if (o.at("streamTCP") == true) o["chunkSpool"] = true;
  for (auto [key, value] : std::vector<std::pair<std::string, uint64_t>>{{"streamPort", 17880}, {"streamSpoolMaxBytes", 1099511627776ULL}, {"streamDrainTimeout", 5000}}) if (!o.contains(key)) o[key] = value;
  require(safe_uint(o["streamPort"], true) && o["streamPort"] <= 65535, "Stream port must be 1 through 65535");
  require(safe_uint(o["streamSpoolMaxBytes"], true) && o["streamSpoolMaxBytes"] >= 1048576, "Stream spool budget must be at least 1 MiB");
  require(safe_uint(o["streamDrainTimeout"]) && o["streamDrainTimeout"] <= 120000, "Stream drain timeout must be at most 120 seconds");
  for (auto key : {"streamHost", "streamTokenFile", "streamSpoolPath"}) if (o.contains(key)) require(o[key].is_string() && text(o[key]).find('\0') == std::string::npos, "Invalid stream text option");
  if (!o.contains("recordingControl")) o["recordingControl"] = false;
  if (!o.contains("startPaused")) o["startPaused"] = false;
  if (!o.contains("jsCallValues")) o["jsCallValues"] = true;
  for (auto key : {"envAbsentProbe", "envInterfaceProbe", "envHostMutation", "envInventory", "closureBindings"}) if (!o.contains(key)) o[key] = false;
  require(o["closureBindings"].is_boolean(), "closureBindings must be boolean");
  for (auto [key, channel] : state_channels()) { if (!o.contains(key)) o[key] = false; require(o[key].is_boolean(), "State channel must be boolean: " + key); }
  for (auto [key, value] : std::vector<std::pair<std::string, unsigned>>{{"vmpDepth",2},{"vmpNodes",256},{"vmpProperties",4096},{"vmpTypedArrayBytes",16777216},{"vmpMaxActive",4096}}) if (!o.contains(key)) o[key] = value;
  for (auto [key, maximum] : std::vector<std::pair<std::string, unsigned>>{{"vmpDepth",8},{"vmpNodes",4096},{"vmpProperties",65536},{"vmpTypedArrayBytes",67108864},{"vmpMaxActive",65536}}) require(safe_uint(o.at(key), true) && o[key] <= maximum, "Invalid explicit VM observation budget: " + key);
  if (enabled(o, "vmpProbe")) { require(enabled(o, "scripts"), "Confirmed VM probes require full script source capture"); o["vmpProbeConfig"] = normalize_vmp_probe(o); }
  for (auto [key, value] : std::vector<std::pair<std::string, unsigned>>{{"closureScopes", 32}, {"closureBindingsLimit", 4096}, {"closureDepth", 2}, {"closureNodes", 256}, {"closureProperties", 4096}}) if (!o.contains(key)) o[key] = value;
  for (auto [key, maximum] : std::vector<std::pair<std::string, unsigned>>{{"closureScopes", 128}, {"closureBindingsLimit", 1048576}, {"closureDepth", 8}, {"closureNodes", 4096}, {"closureProperties", 65536}}) require(safe_uint(o.at(key), true) && o[key] <= maximum, "Invalid closure observation budget: " + key);
  require(!enabled(o, "closureBindings") || enabled(o, "jsCallDetail"), "Closure bindings require selected JSCall detail");
  if (!o.contains("envInventoryProperties")) o["envInventoryProperties"] = 65536;
  require(safe_uint(o["envInventoryProperties"], true) && o["envInventoryProperties"] <= 1048576, "envInventoryProperties must be 1 through 1048576");
  if (!o.contains("writeBufferBytes")) o["writeBufferBytes"] = 8388608;
  if (!o.contains("writeBatchBytes")) o["writeBatchBytes"] = 1048576;
  require(safe_uint(o["writeBufferBytes"], true) && o["writeBufferBytes"] >= 4096 && o["writeBufferBytes"] <= 268435456, "writeBufferBytes must be 4 KiB through 256 MiB");
  require(safe_uint(o["writeBatchBytes"], true) && o["writeBatchBytes"] >= 4096 && o["writeBatchBytes"] <= 4194304 && o["writeBatchBytes"] <= o["writeBufferBytes"], "writeBatchBytes must be 4 KiB through 4 MiB and not exceed writeBufferBytes");
  require(native || str(o, "transport") == "cdp", "transport must be native or cdp");
  for (auto& name : booleans) require(o.at(name).is_boolean(), name + " must be boolean");
  require(!enabled(o, "startPaused") || enabled(o, "recordingControl"), "startPaused requires recordingControl");
  for (auto& name : {"durationSeconds", "maxBytes", "maxEvents", "maxBodyBytes", "commandTimeout", "traceFlushTimeout"}) require(safe_uint(o.at(name), true), std::string(name) + " must be a positive safe integer");
  require(o["durationSeconds"] <= 86400, "durationSeconds exceeds 86400");
  for (auto [name, maximum] : std::vector<std::pair<std::string, int>>{{"objectDepth", 8}, {"objectNodes", 4096}, {"objectProperties", 65536}}) require(safe_uint(o.at(name), true) && o[name] <= maximum, "Object snapshot budget invalid: " + name);
  auto url = str(o, "url"); require(std::regex_match(url, std::regex("https?://[^/\\s?#]+[^\\r\\n]*", std::regex::icase)) && url.find('\0') == std::string::npos, "URL must be HTTP/HTTPS without control characters");
  require(!str(o, "output").empty(), "Capture output required"); o["output"] = utf8(fs::absolute(path(str(o, "output"))).lexically_normal()); require(!fs::exists(path(str(o, "output"))), "Capture output already exists");
  if (enabled(o, "chunkSpool")) {
    auto output = path(str(o, "output")), spool = fs::absolute(path(str(o, "streamSpoolPath", utf8(output) + ".stream"))).lexically_normal();
    require(!inside(output, spool) && !inside(spool, output) && !fs::exists(spool), "Chunk spool must be a new separate directory"); o["streamSpoolPath"] = utf8(spool);
  }
  if (enabled(o, "streamTCP")) { auto host = str(o, "streamHost", "127.0.0.1"); require(!host.empty() && host.find_first_of(" \r\n\t") == host.npos, "Invalid streaming TCP host"); if (host != "127.0.0.1" && host != "::1" && host != "localhost") require(!str(o, "streamTokenFile").empty() && fs::is_regular_file(path(str(o, "streamTokenFile"))), "Remote streaming TCP requires a token file"); }
  if (native) { for (auto name : {"cpuProfile", "contextEvents", "rawCdp"}) require(!enabled(o, name), std::string(name) + " requires CDP; no automatic fallback"); require(str(o, "untilExpression").empty() && str(o, "resultExpression").empty(), "Page expressions require CDP"); }
  else require(!enabled(o, "netLog"), "NetLog requires native capture");
  require(!enabled(o, "redirectBodies") || enabled(o, "http"), "Redirect bodies require HTTP");
  require(!enabled(o, "correlations") || (enabled(o, "jsCallDetail") && enabled(o, "scripts") && enabled(o, "http")), "Correlations require JSCall, script and HTTP");
  auto selected = details(o); require(!enabled(o, "objectSnapshots") || !selected.empty() || state_selected(o) || (native && (enabled(o, "consoleEvents") || enabled(o, "exceptionEvents"))), "Object snapshots require a detailed channel");
  require(!enabled(o, "objectSnapshots") || state_selected(o) || (enabled(o, "jsCallDetail") && enabled(o, "jsCallValues")) || enabled(o, "nativeDetail") || enabled(o, "wasm") || (native && (enabled(o, "consoleEvents") || enabled(o, "exceptionEvents"))), "Object snapshots require at least one value-capture channel");
  o["detailFilter"] = normalize_filter(o);
  if (enabled(o, "closureBindings")) {
    bool concrete = false;
    for (auto field : {"detailScriptInclude", "jsFunctionInclude"}) for (auto& rule : o["detailFilter"]["rules"][field]) { require(text(rule).find_first_not_of("*? \t\r\n") != std::string::npos, "Closure observation scope requires concrete target rules, not global-only wildcards"); concrete = true; }
    require(concrete, "Closure observation requires an explicit script or function include scope");
  }
  o["envFilter"] = normalize_env_filter(o);
  if (env_selected(o)) o["environmentExecutionPolicy"] = "complete-selected-Ignition-host-observations; JS optimization disabled; no JS wrapper";
  if (requires_scope && (enabled(o, "jsCallDetail") || enabled(o, "nativeDetail"))) {
    const auto& rules = o["detailFilter"]["rules"]["detailScriptInclude"];
    require(!rules.empty(), "Selected preset requires a concrete target script include scope");
    for (const auto& rule : rules) require(text(rule).find_first_not_of("*? \t\r\n") != std::string::npos, "Target script scope must not contain global-only wildcard rules");
  }
  Json categories = Json::array(); for (auto [category, active] : std::vector<std::pair<std::string, bool>>{{"http", enabled(o, "http") || enabled(o, "netLog")}, {"script", enabled(o, "scripts")}, {"env", enabled(o, "hooks")}, {"lifecycle", enabled(o, "contextEvents")}, {"console", enabled(o, "consoleEvents")}, {"exception", enabled(o, "exceptionEvents")}, {"jscall", enabled(o, "cpuProfile") || enabled(o, "jsCallDetail")}, {"native", enabled(o, "nativeTimeline") || enabled(o, "nativeDetail")}, {"opcode", enabled(o, "opcode")}, {"wasm", enabled(o, "wasm")}}) if (active) categories.push_back(category);
  if (enabled(o,"httpRootTrace")) categories.push_back("causality");
  if (env_selected(o) && std::find(categories.begin(), categories.end(), Json("env")) == categories.end()) categories.push_back("env");
  for (const auto& channel : selected_state_channels(o)) categories.push_back(channel);
  require(!categories.empty() || enabled(o, "rawCdp"), "Select at least one channel"); o["categories"] = categories;
  if (verify_browser) {
    auto chrome = path(str(o, "chromePath")); require(!chrome.empty() && fs::is_regular_file(chrome), "Specify an existing Chrome executable"); o["chromePath"] = utf8(fs::canonical(chrome));
    std::vector<std::string> browser_channels; if (native) for (auto& [key, channel] : browser_options) if (enabled(o, key)) browser_channels.push_back(channel); if (!native && enabled(o, "redirectBodies")) browser_channels.push_back("http");
    const bool instrumented = enabled(o, "httpRootTrace") || enabled(o, "opcode") || !selected.empty() || !browser_channels.empty() || env_selected(o) || state_selected(o) || (native && enabled(o, "recordingControl"));
    if (instrumented) {
      auto descriptor = verify_backend(chrome); o["browserIdentity"] = descriptor;
      if(enabled(o,"httpRootTrace")) {
        require(str(descriptor,"causalityProtocol")=="v8trace/causality/1" && hash_string(field(descriptor,"causalityPatchSha256")), "HTTP root instrumentation requires matching custom Chromium causality backend");
        require(str(descriptor,"causalityModelProtocol")=="v8trace/causality-model/2"||str(descriptor,"causalityModelProtocol")=="v8trace/causality-model/3", "HTTP root instrumentation requires native causality provenance model 2 or 3");
        for(auto feature : {"http-root-stack","native-tasks","v8-microtasks","blink-resources","worker-lifetime","ipcz-wire","gc-registration-lifetime","request-call-positions"}) supported(descriptor,"causalityFeatures",feature);
        for(auto feature : {"promise-settlement-provenance","collector-internal-isolation"}) supported(descriptor,"causalityFeatures",feature);
        require(str(descriptor,"callPositionProtocol")=="v8trace/call-position/1","HTTP request function positions require matching native call identity backend");
        if(enabled(o,"domTrace")) supported(descriptor,"causalityFeatures","native-dom-journal");
        if(enabled(o,"cryptoTrace")) {
          supported(descriptor,"causalityFeatures","native-webcrypto-copied-inputs");
          supported(descriptor,"causalityFeatures","native-webcrypto-result-buffers");
        }
        o["causalityBackend"]=descriptor;
      }
      if (state_selected(o)) {
        require(str(descriptor, "stateProtocol") == "v8trace/state/1" && str(descriptor, "stateStatsProtocol") == "v8trace/state-stats/1" && str(descriptor, "identityProtocol") == "v8trace/identity/1" && hash_string(field(descriptor, "statePatchSha256")) && hash_string(field(descriptor, "identityPatchSha256")), "Specialized state channels require matching custom Chromium; no page wrapper fallback");
        for (const auto& channel : selected_state_channels(o)) supported(descriptor, "stateFeatures", text(channel));
        o["stateBackend"] = descriptor;
        if (enabled(o, "wasmModules") || enabled(o, "vmpProbe")) require(str(descriptor, "auxProtocol") == "v8trace/aux/1" && hash_string(field(descriptor, "auxPatchSha256")), "Selected browser lacks native module/VM evidence channels");
        if(enabled(o,"vmpProbe"))require(str(descriptor,"v8Revision")==str(o.at("vmpProbeConfig").at("spec"),"v8Revision"),"VM descriptor engine revision differs from selected browser");
        if(enabled(o,"vmpProbe"))require(str(descriptor,"vmpConfigTransport")=="chromium-switch-v1","Selected browser lacks lossless VM configuration switch transport");
        if(enabled(o,"vmpProbe")&&str(o.at("vmpProbeConfig").at("spec"),"bytecodeHashPolicy","raw")=="embedded-feedback-zeroed-v1")require(str(descriptor,"bytecodeIdentityProtocol")=="v8trace/bytecode-identity/1","Selected browser lacks stable embedded-feedback bytecode identities");
      }
      if (enabled(o, "closureBindings")) require(str(descriptor, "closureProtocol") == "v8trace/closure/1" && hash_string(field(descriptor, "closurePatchSha256")) && str(descriptor, "callProtocol") == "v8trace/call/2", "Selected browser lacks native lexical binding snapshots");
      if (env_selected(o)) {
        require(str(descriptor, "envProbeProtocol") == "v8trace/env/1" && str(descriptor, "envStatsProtocol") == "v8trace/env-stats/1" && hash_string(field(descriptor, "envProbePatchSha256")), "Native environment probes require newly built custom Chromium");
        require(str(descriptor,"envBuiltinProtocol")=="v8trace/env-builtin/1","Native environment probes require the updated producer and reflection builtin continuation backend");
        for (auto [key, feature] : std::vector<std::pair<std::string, std::string>>{{"envAbsentProbe", "absent"}, {"envInterfaceProbe", "interfaces"}, {"envHostMutation", "mutations"}, {"envInventory", "inventory"}}) if (enabled(o, key)) supported(descriptor, "envProbeFeatures", feature);
        o["envBackend"] = descriptor;
      }
      if (native && enabled(o, "recordingControl")) require(str(descriptor, "recordingControlProtocol") == "v8trace/recording/1" && hash_string(field(descriptor, "recordingControlPatchSha256")), "Selected browser has no native pause/resume protocol; choose the new custom Chromium or disable recordingControl");
      if (enabled(o, "opcode")) o["opcodeBackend"] = descriptor;
      if (!selected.empty()) { require(str(descriptor, "detailProtocol") == "v8trace/detail/1", "Chromium detail protocol missing"); for (auto& channel : selected) supported(descriptor, "detailFeatures", channel); o["detailBackend"] = descriptor; }
      if (!browser_channels.empty()) { require(str(descriptor, "browserProtocol") == "v8trace/browser/1" && hash_string(field(descriptor, "browserPatchSha256")), "Chromium browser channel protocol missing"); for (auto& channel : browser_channels) supported(descriptor, "browserFeatures", channel); o["browserBackend"] = descriptor; }
      if (enabled(o["detailFilter"], "active")) require(str(descriptor, "detailFilterProtocol") == "v8trace/detail-filter/1" && hash_string(field(descriptor, "detailFilterPatchSha256")), "Producer-side filter requires compatible Chromium");
      if ((enabled(o, "jsCallDetail") && !enabled(o, "jsCallValues")) || enabled(o["detailFilter"], "valueRules")) require(str(descriptor, "callProtocol") == "v8trace/call/2", "JSCall structure/value separation requires newly built compact-call Chromium");
      if (enabled(o["detailFilter"], "valueRules")) require(str(descriptor, "callValueFilterProtocol") == "v8trace/detail-filter/2", "JSCall value-range filter protocol missing");
      if (enabled(o, "jsCallDetail") && str(descriptor, "callProtocol") == "v8trace/call/2") require(str(descriptor, "functionDictionaryProtocol") == "v8trace/function/1" && str(descriptor, "callStatsProtocol") == "v8trace/call-stats/1", "Incomplete compact-call backend declaration");
      if (enabled(o, "objectSnapshots")) require(str(descriptor, "objectSnapshotProtocol") == "v8trace/objects/1" && hash_string(field(descriptor, "objectSwitchPatchSha256")), "Object snapshot protocol missing");
      if (enabled(o, "redirectBodies")) require(str(descriptor, "redirectBodyProtocol") == "v8trace/redirect-body/1", "Redirect body protocol missing");
    }
    o["backend"] = native ? (instrumented ? "chromium-native" : "chrome-native") : (instrumented ? "chromium-opcode" : "chrome-cdp");
  }
  auto required = field(o, "require", Json::array()); if (required.is_string()) required = split(text(required), ','); require(required.is_array(), "require must be array or comma-separated text"); auto coverage = capabilities(o); for (auto& name : required) { auto value = str(coverage, text(name), "unavailable"); require(value != "disabled" && value != "unavailable", "Required capability unavailable: " + text(name)); }
  o["edition"] = edition_id();
  return o;
}
Json capabilities(const Json& o) { const bool native = str(o, "transport") == "native"; Json out; auto set = [&](std::string key, bool active, std::string value) { out[key] = active ? value : "disabled"; };
  set("causality",enabled(o,"httpRootTrace"),"native-http-root-task-resource-wire-graph; collector-proof-required");
  set("dom",enabled(o,"domTrace"),"native-dom-baseline-and-incremental-journal; raw-attributes-no-layout");
  set("crypto.checkpoints",enabled(o,"cryptoTrace"),"native-webcrypto-copied-inputs-and-result-checkpoints; not-custom-VM-semantics");
  set("execution.interpreter",enabled(o,"interpretOnly"),"explicit-JS-Ignition-comparison-not-WASM-jitless");
  for (auto [key, name] : std::vector<std::pair<std::string, std::string>>{{"envAbsentProbe", "env.absent"}, {"envInterfaceProbe", "env.interfaces"}, {"envHostMutation", "env.mutations"}, {"envInventory", "env.inventory"}}) set(name, enabled(o, key), "native-Ignition-host-observation");
  set("jscall.closure", enabled(o, "closureBindings"), "native-context-allocated-lexical-bindings");
  set("websocket", enabled(o, "webSocket"), "native-renderer-decoded-payload-and-fragments"); set("indexeddb", enabled(o, "indexedDB"), "native-serialized-values-with-explicit-external-boundaries"); set("cookie", enabled(o, "cookies"), "native-js-cookie-api-and-cookie-store-changes"); set("storage", enabled(o, "storage"), "native-storage-api-and-notifications");
  set("state.call-identity", state_selected(o), "observed-shared-identities-or-explicit-unresolved");
  set("logs.chunk-spool",enabled(o,"chunkSpool"),"durable-zstd-chunks-with-independent-commit");set("logs.stream-tcp",enabled(o,"streamTCP"),"host-produced-sealed-chunks-not-V8-producer-TCP");
  set("wasm.modules", enabled(o, "wasmModules"), "native-complete-compile-inputs-without-forcing-DrumBrake"); set("vmp.probe", enabled(o, "vmpProbe"), "native-explicitly-confirmed-state-sites");
  set("recording.control", enabled(o, "recordingControl"), native ? "native-local-pause-resume" : "cdp-domain-pause-resume");
  set("http", enabled(o, "http"), native ? "native-network-renderer-body-streams" : "cdp-decoded-body"); set("script", enabled(o, "scripts"), native ? "native-v8-source" : "cdp-source"); set("console", enabled(o, "consoleEvents"), native ? "native-console-api" : "cdp-console-api"); set("exception", enabled(o, "exceptionEvents"), native ? "native-unhandled-exceptions" : "cdp-exceptions"); set("env.detail", enabled(o, "hooks"), native ? "native-injected-diagnostic-js-hooks" : "diagnostic-js-hooks"); set("native.detail", enabled(o, "nativeDetail"), "native-api-runtime-builtin-values"); set("jscall.detail", enabled(o, "jsCallDetail"), "native-function-entry-return"); set("jscall.values", enabled(o, "jsCallDetail") && field(o, "jsCallValues", true) == true, enabled(field(o, "detailFilter"), "valueRules") ? "native-selected-call-values" : "native-selected-full-values"); set("wasm.execution", enabled(o, "wasm"), "native-drumbrake-execution"); set("opcode", enabled(o, "opcode"), "native-ignition-execution"); set("objects.snapshot", enabled(o, "objectSnapshots"), "native-own-storage-observation"); set("native.timeline", enabled(o, "nativeTimeline"), "performance-events"); set("jscall.sample", enabled(o, "cpuProfile"), "cpu-sampling"); set("context", enabled(o, "contextEvents"), "cdp-lifecycle"); set("http.redirect-body", enabled(o, "redirectBodies"), "native-bounded-transaction-drain"); set("network.diagnostic", enabled(o, "netLog"), "native-netlog-raw-events-not-decoded-http-bodies"); set("evidence.correlations", enabled(o, "correlations"), "observed-identities-and-labeled-candidates"); return out; }
Json launch_config(const Json& o, Store& store) {
  const bool controlled = str(o, "transport") == "native" && enabled(o, "recordingControl");
  Json categories = Json::array(); if (enabled(o, "nativeTimeline")) { categories.push_back("v8"); categories.push_back("blink.bindings"); categories.push_back("loading"); categories.push_back("devtools.timeline"); }
  if (env_selected(o)) categories.push_back("disabled-by-default-v8.envprobe");
  if (enabled(o,"httpRootTrace")) categories.push_back("disabled-by-default-v8.causality");
  if (enabled(o,"domTrace")) categories.push_back("disabled-by-default-v8.dom");
  if (enabled(o,"cryptoTrace")) categories.push_back("disabled-by-default-v8.crypto");
  if (state_selected(o)) { categories.push_back("disabled-by-default-v8.state"); for (const auto& channel : selected_state_channels(o)) categories.push_back("disabled-by-default-v8." + text(channel)); }
  if (enabled(o, "wasmModules") || enabled(o, "vmpProbe")) categories.push_back("disabled-by-default-v8.aux");
  if (enabled(o, "opcode")) categories.push_back("disabled-by-default-v8.opcode"); for (auto& channel : details(o)) categories.push_back("disabled-by-default-v8." + channel); for (auto& [key, channel] : browser_options) if (enabled(o, key)) categories.push_back("disabled-by-default-v8." + channel);
  Json config{{"startup_duration", o.at("durationSeconds")}, {"result_file", utf8(store.filename("native-output/trace.json"))}, {"trace_config", {{"record_mode", "record-until-full"}, {"trace_buffer_size_in_kb", std::clamp<uint64_t>(o.at("maxBytes").get<uint64_t>() / 1024, 4096, 1048576)}, {"included_categories", categories}, {"excluded_categories", Json::array({"*"})}}}};
  std::vector<std::string> args{"--user-data-dir=" + utf8(store.filename("browser-profile")), "--no-first-run", "--no-default-browser-check"}; std::vector<std::string> features;
  if (enabled(o, "disableExtensions")) args.push_back("--disable-extensions");
  const bool high_volume = enabled(o,"httpRootTrace") || enabled(o,"opcode") || !details(o).empty() || enabled(o,"vmpProbe");
  const auto identity = field(o,"browserIdentity");
  const bool expanded = str(identity,"traceTransportProtocol")=="v8trace/trace-transport/1" && field(identity,"traceSharedMemoryMaxBytes",0)==33554432;
  // Older Chromium rejects serialized shared-memory regions above 8 MiB.
  if (!categories.empty()) features.push_back("kPerfettoBackendParams:shared_memory_size_bytes/" + std::to_string(high_volume && expanded ? 33554432 : 8388608) + "/page_size_bytes/" + std::to_string(high_volume ? 32768 : 4096));
  if (enabled(o, "redirectBodies")) features.push_back("V8TraceRedirectBodyCapture:max_bytes/" + std::to_string(std::min<uint64_t>(o.at("maxBodyBytes").get<uint64_t>(), 1073741824)) + "/timeout_ms/5000");
  if (!features.empty()) { std::string feature; for (auto& part : features) { if (!feature.empty()) feature += ','; feature += part; } args.push_back("--enable-features=" + feature); }
  if (enabled(o, "headless")) args.push_back("--headless=new");
  if (!categories.empty()) {
    store.json("native-output/trace-config.json", config);
    if (!controlled) { args.push_back("--trace-config-file=" + utf8(store.filename("native-output/trace-config.json"))); args.push_back("--trace-startup-format=json"); }
  }
  if (controlled) {
    auto control_file = store.filename("native-control/config.json");
    store.json("native-control/config.json", {{"protocol", "v8trace/recording/1"}, {"captureId", store.capture_id}, {"token", sha256(uuid() + uuid())}, {"outputDirectory", utf8(store.filename("native-output"))}, {"url", o.at("url")}, {"startPaused", o.at("startPaused")}, {"netLog", o.at("netLog")}, {"trace", !categories.empty()}, {"traceConfig", config.at("trace_config")}});
    args.push_back("--v8-trace-recording-control=" + utf8(control_file));
    if (enabled(o, "hooks")) args.push_back("--v8-trace-env-hooks");
  }
  std::string flags; for (auto [name, flag] : std::vector<std::pair<std::string, std::string>>{{"opcode", "--trace-opcode"}, {"jsCallDetail", "--trace-jscall-detail"}, {"nativeDetail", "--trace-native-detail"}, {"wasm", "--trace-wasm-detail"}}) if (enabled(o, name)) { if (!flags.empty()) flags += ' '; flags += flag; }
  if (enabled(o, "interpretOnly")) flags += " --no-sparkplug --no-maglev --no-turbofan --no-enable-lazy-source-positions";
  if (enabled(o,"httpRootTrace")) flags += " --trace-causality";
  if (enabled(o, "wasmModules")) flags += " --trace-wasm-modules";
  if (enabled(o, "vmpProbe")) {
    flags += " --trace-vmp";
    args.push_back("--v8-trace-vmp-config=" + str(o.at("vmpProbeConfig"), "token"));
    for (auto [key, flag] : std::vector<std::pair<std::string, std::string>>{{"vmpDepth","depth"},{"vmpNodes","nodes"},{"vmpProperties","properties"},{"vmpTypedArrayBytes","typed-bytes"},{"vmpMaxActive","max-active"}}) flags += " --trace-vmp-" + flag + '=' + text(o.at(key));
  }
  if (enabled(field(o, "detailFilter"), "active")) { if (!flags.empty()) flags += ' '; flags += "--trace-detail-filter=" + str(o["detailFilter"], "token"); }
  if (enabled(o, "jsCallDetail") && !enabled(o, "jsCallValues")) flags += " --no-trace-jscall-values";
  if (enabled(o, "closureBindings")) {
    flags += " --trace-jscall-closure";
    for (auto [key, flag] : std::vector<std::pair<std::string, std::string>>{{"closureScopes", "scopes"}, {"closureBindingsLimit", "bindings"}, {"closureDepth", "depth"}, {"closureNodes", "nodes"}, {"closureProperties", "properties"}}) flags += " --trace-closure-" + flag + '=' + text(o.at(key));
  }
  for (auto [key, flag] : std::vector<std::pair<std::string, std::string>>{{"envAbsentProbe", "absent"}, {"envInterfaceProbe", "interfaces"}, {"envHostMutation", "mutations"}, {"envInventory", "inventory"}}) if (enabled(o, key)) flags += " --trace-env-" + flag;
  if (env_selected(o)) {
    flags += " --trace-env-inventory-properties=" + text(o.at("envInventoryProperties"));
    if (enabled(field(o, "envFilter"), "active")) flags += " --trace-env-filter=" + str(o.at("envFilter"), "token");
  }
  if (!flags.empty()) args.push_back("--js-flags=" + flags);
  if (enabled(o, "objectSnapshots")) for (auto [key, flag] : std::vector<std::pair<std::string, std::string>>{{"objectDepth", "depth"}, {"objectNodes", "nodes"}, {"objectProperties", "properties"}}) args.push_back("--v8-trace-object-" + flag + '=' + text(o.at(key)));
  if (enabled(o, "netLog") && !controlled) { args.push_back("--log-net-log=" + utf8(store.filename("native-output/netlog.json"))); args.push_back("--net-log-capture-mode=Everything"); args.push_back("--net-log-duration=" + text(o.at("durationSeconds"))); }
  args.push_back(controlled ? "about:blank" : str(o, "url")); return {{"args", args}, {"config", config}, {"tracing", !categories.empty()}};
}
Json render_prompt(const fs::path& assets, const Json& command) {
  if (!edition_prompt_available(str(command, "templateId"))) require_professional("JSVMP/WASM internal instrumentation case template");
  auto catalog = load_json(assets / "case_prompt_templates.json", 1048576); Json selected; for (auto& item : catalog.at("templates")) if (item.at("id") == field(command, "templateId")) selected = item; require(!selected.is_null(), "Unknown prompt template"); auto fields = field(command, "fields", Json::object()); require(fields.is_object(), "Prompt fields must be object");
  Json values{{"templateLabel", selected.at("label")}}; for (auto [key, limit] : std::vector<std::pair<std::string, size_t>>{{"url", 2048}, {"goal", 4096}, {"caseDirectory", 1024}}) { auto value = field(fields, key, ""); require(value.is_string() && utf16le(text(value)).size() <= limit * 2 && text(value).find('\0') == std::string::npos, "Invalid prompt field: " + key); values[key] = trim(text(value)); }
  require(str(values, "caseDirectory").find_first_of("\r\n") == std::string::npos, "Case directory must be one line"); auto url = str(values, "url"); require(url.empty() || std::regex_match(url, std::regex("https?://[^\\s]+", std::regex::icase)), "Prompt URL must be HTTP/HTTPS"); if (url.empty()) values["url"] = "TARGET_URL（填写目标网站）"; if (str(values, "goal").empty()) values["goal"] = selected.at("defaultGoal"); if (str(values, "caseDirectory").empty()) values["caseDirectory"] = "case/SITE_CASE";
  std::string source; for (auto group : {catalog.at("header"), catalog.at("collection"), selected.at("approach"), catalog.at("handoff")}) for (auto& line : group) { if (!source.empty()) source += '\n'; source += text(line); }
  std::string output = "当前执行版本：" + str(edition_info(), "label") + "。先以实际编译内核与浏览器预检报告为能力依据。\n";
  if (!professional_edition()) output += "Community 保留完整补环境取证、目标 JSCall/Native 原始值、HTTP 正文及基础搜索/MCP。后文涉及 Opcode、WASM 内部、JSVMP 探针或 HTTP 根图的专业采集步骤不属于本次可用开关；如已有相应原始日志，可基础检索。保持实际范围与缺口，不静默删减配置或引入浏览器自动化兜底。\n\n";
  size_t cursor = 0; while (true) { auto start = source.find("{{", cursor); if (start == source.npos) { output += source.substr(cursor); break; } output += source.substr(cursor, start - cursor); auto end = source.find("}}", start + 2); require(end != source.npos, "Unfinished prompt substitution"); auto key = source.substr(start + 2, end - start - 2); require(values.contains(key), "Unknown prompt substitution"); output += text(values[key]); cursor = end + 2; }
  return {{"templateId", selected.at("id")}, {"label", selected.at("label")}, {"text", output}, {"kind", "task-template"}, {"executesCapture", false}, {"executesReproduction", false}};
}
}
