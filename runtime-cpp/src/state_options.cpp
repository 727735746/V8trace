#include "v8trace/state.h"

namespace v8trace {
const std::vector<std::pair<std::string, std::string>>& state_channels() {
  static const std::vector<std::pair<std::string, std::string>> values{{"webSocket", "websocket"}, {"indexedDB", "indexeddb"}, {"cookies", "cookie"}, {"storage", "storage"}, {"wasmModules", "wasm-module"}, {"vmpProbe", "vmp"}};
  return values;
}
bool state_selected(const Json& options) { for (const auto& [key, channel] : state_channels()) if (field(options, key, false) == true) return true; return false; }
Json selected_state_channels(const Json& options) { Json values = Json::array(); for (const auto& [key, channel] : state_channels()) if (field(options, key, false) == true) values.push_back(channel); return values; }
}
