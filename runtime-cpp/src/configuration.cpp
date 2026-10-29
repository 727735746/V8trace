#include "v8trace/studio.h"
#include "v8trace/edition.h"
#include <algorithm>

namespace v8trace {
namespace {
void require_config(bool value, const char* error) { if (!value) throw std::runtime_error(error); }
Json merge_objects(Json base, const Json& patch, unsigned depth = 0) {
  require_config(depth <= 64, "Configuration nesting exceeds 64");
  if (!base.is_object() || !patch.is_object()) return patch;
  for (auto it = patch.begin(); it != patch.end(); ++it) base[it.key()] = merge_objects(field(base, it.key()), it.value(), depth + 1);
  return base;
}
Json unknown_options(const Json& options) {
  auto known = supported_options(); Json unknown = Json::array();
  for (auto it = options.begin(); it != options.end(); ++it) if (std::find(known.begin(), known.end(), Json(it.key())) == known.end()) unknown.push_back(it.key());
  return unknown;
}
Json summary_value(const Json& value) {
  auto result = event_preview(value, 2048);
  return {{"preview", result.at("detail")}, {"previewLimited", result.at("truncated")}, {"sha256", sha256(dump(value))}};
}
Json clipped(const Json& value, size_t& remaining, size_t& nodes, bool& limited, unsigned depth) {
  if (!nodes || remaining < 32 || depth > 12) { limited = true; return "[仅界面预览：其余值按需读取]"; }
  --nodes;
  if (value.is_string()) {
    const auto& raw = value.get_ref<const std::string&>(); auto bytes = std::min(raw.size(), remaining / 6);
    while (bytes && bytes < raw.size() && (uint8_t(raw[bytes]) & 0xc0) == 0x80) --bytes;
    remaining -= bytes * 6; limited |= bytes < raw.size(); return raw.substr(0, bytes);
  }
  if (!value.is_structured()) { remaining -= std::min<size_t>(remaining, 32); return value; }
  Json output = value.is_object() ? Json::object() : Json::array();
  for (auto it = value.begin(); it != value.end(); ++it) {
    if (!nodes || remaining < 64) { limited = true; break; }
    if (value.is_object()) {
      if (it.key().size() * 6 > remaining) { limited = true; break; }
      remaining -= it.key().size() * 6; output[it.key()] = clipped(it.value(), remaining, nodes, limited, depth + 1);
    } else output.push_back(clipped(it.value(), remaining, nodes, limited, depth + 1));
  }
  return output;
}
}
Json supported_options() {
  return split("url,output,chromePath,transport,httpRootTrace,domTrace,cryptoTrace,preset,headless,interpretOnly,durationSeconds,maxEvents,maxBytes,maxBodyBytes,commandTimeout,traceFlushTimeout,hooks,cpuProfile,nativeTimeline,opcode,jsCallDetail,jsCallValues,nativeDetail,wasm,wasmModules,vmpProbe,vmpProbeFile,vmpProbeSpec,vmpDepth,vmpNodes,vmpProperties,vmpTypedArrayBytes,vmpMaxActive,objectSnapshots,objectDepth,objectNodes,objectProperties,correlations,http,redirectBodies,scripts,contextEvents,consoleEvents,exceptionEvents,rawCdp,netLog,recordingControl,startPaused,webSocket,indexedDB,cookies,storage,closureBindings,closureScopes,closureBindingsLimit,closureDepth,closureNodes,closureProperties,envAbsentProbe,envInterfaceProbe,envHostMutation,envInventory,envInventoryProperties,envScriptInclude,envScriptExclude,envUnknownSource,envExcludeInternal,detailScriptInclude,detailScriptExclude,jsFunctionInclude,jsFunctionExclude,nativeNameInclude,nativeNameExclude,jsValueScriptInclude,jsValueScriptExclude,jsValueFunctionInclude,jsValueFunctionExclude,nativeKinds,detailExcludeInternal,detailUnknownSource,writeBufferBytes,writeBatchBytes,chunkSpool,streamTCP,streamHost,streamPort,streamTokenFile,streamSpoolPath,streamSpoolMaxBytes,streamDrainTimeout,untilExpression,resultExpression,require", ',');
}
Json event_preview(const Json& event, size_t maximum) {
  auto budget = maximum > 512 ? maximum - 512 : maximum; size_t nodes = 256; bool limited = false;
  auto projected = clipped(event, budget, nodes, limited, 0); auto detail = dump(projected, 2);
  Json row; if (event.is_object()) for (auto key : {"seq", "observedAt", "category", "kind", "sessionId", "contextId"}) if (event.contains(key) && !event.at(key).is_structured() && text(event.at(key)).size() < 1024) row[key] = event.at(key);
  row["detail"] = detail; row["truncated"] = limited; return row;
}
Json load_configuration(const fs::path& file) {
  auto bytes=read_file(file,1048576);if(bytes.starts_with("\xef\xbb\xbf"))bytes.erase(0,3);return parse(bytes);
}
Json configuration_options(const Json& document) {
  require_config(document.is_object(), "Configuration must be an object");
  Json options = document;
  if (document.contains("protocol")) {
    require_config(str(document, "protocol") == "v8trace/config/1" && field(document, "schemaVersion") == 1, "Unsupported configuration protocol/version");
    require_config(str(document, "pathBase") == "application-working-directory", "Configuration path base must be explicit; paths are not rewritten on import");
    options = field(document, "options");
  }
  require_config(options.is_object() && options.size() <= 512, "Configuration options must be an object with at most 512 fields");
  require_config(dump(options).size() <= 1048576, "Configuration exceeds 1 MiB");
  validate_edition_options(options);
  return options;
}
Json configuration_document(const Json& options) {
  return {{"protocol", "v8trace/config/1"}, {"schemaVersion", 1}, {"edition", edition_id()}, {"pathBase", "application-working-directory"}, {"options", configuration_options(options)}};
}
Json configuration_preview(const Json& current, const Json& incoming, std::string_view mode) {
  require_config(mode == "replace" || mode == "merge", "Configuration mode must be replace or merge");
  auto before = configuration_options(current), patch = configuration_options(incoming);
  auto after = configuration_options(mode == "merge" ? merge_objects(before, patch) : patch);
  std::set<std::string> keys; for (auto it = before.begin(); it != before.end(); ++it) keys.insert(it.key()); for (auto it = after.begin(); it != after.end(); ++it) keys.insert(it.key());
  Json changes = Json::array();
  for (const auto& key : keys) if (before.contains(key) != after.contains(key) || field(before, key) != field(after, key)) {
    Json row{{"field", key}, {"beforePresent", before.contains(key)}, {"afterPresent", after.contains(key)}};
    if (before.contains(key)) row["before"] = summary_value(before.at(key)); if (after.contains(key)) row["after"] = summary_value(after.at(key)); changes.push_back(row);
  }
  return {{"protocol", "v8trace/config-preview/1"}, {"mode", mode}, {"document", configuration_document(after)}, {"changes", changes}, {"unknownFields", unknown_options(after)}, {"writesFiles", false}, {"launchesBrowser", false}, {"pathsRewritten", false}};
}
Json preflight_capture(const Json& input, const fs::path& assets, bool verify_browser) {
  Json result{{"protocol", "v8trace/preflight/1"}, {"edition", edition_info()}, {"baseDirectory", utf8(fs::current_path())}, {"launchesBrowser", false}, {"writesCapture", false}, {"browserVerified", false}, {"verificationScope", "configuration and selected descriptor hashes; not browser startup or page execution"}};
  try {
    auto raw = configuration_options(input); result["rawOptions"] = raw; result["unknownFields"] = unknown_options(raw);
    auto normalized = validate_options(raw, assets, verify_browser); result["ok"] = true; result["browserVerified"] = verify_browser;
    result["plan"] = normalized; result["capabilities"] = capabilities(normalized);
    result["scopes"] = {{"detail", normalized.at("detailFilter")}, {"environment", normalized.at("envFilter")}, {"viewerFilter", "display-only; never changes producer selection"}};
    result["warnings"] = Json::array(); if(normalized.at("interpretOnly")==true)result["warnings"].push_back("显式 JS 纯解释执行对照；不自动开启详细调用，也不把 WebAssembly 设为 jitless。");
    if (normalized.at("opcode") == true || normalized.at("jsCallDetail") == true || normalized.at("nativeDetail") == true || env_selected(normalized) || normalized.at("vmpProbe") == true) result["warnings"].push_back("逐次原生插装或环境探针存在执行/序列化开销；生产端范围与预算见 plan，不等于零影响。");
    if (!result["unknownFields"].empty()) result["warnings"].push_back("未知字段完整保留，但当前内核不执行这些字段；请逐项核对，勿当成已经启用的能力。");
  } catch (const std::exception& error) { result["ok"] = false; result["error"] = error.what(); }
  return result;
}
}
