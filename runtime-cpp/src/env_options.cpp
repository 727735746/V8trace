#include "v8trace/core.h"

namespace v8trace {
bool env_selected(const Json& options) {
  for (auto key : {"envAbsentProbe", "envInterfaceProbe", "envHostMutation", "envInventory"}) if (field(options, key, false) == true) return true;
  return false;
}
Json normalize_env_filter(const Json& options) {
  Json input{{"jsCallDetail", true}, {"detailScriptInclude", field(options, "envScriptInclude", "")}, {"detailScriptExclude", field(options, "envScriptExclude", "")}, {"detailExcludeInternal", field(options, "envExcludeInternal", false)}};
  if (options.contains("envUnknownSource")) input["detailUnknownSource"] = options.at("envUnknownSource");
  auto filter = normalize_filter(input);
  if (field(filter, "active") == true && !env_selected(options)) throw std::runtime_error("Environment source filter requires a native environment probe");
  return filter;
}
}
