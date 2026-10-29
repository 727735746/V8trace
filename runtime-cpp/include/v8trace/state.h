#pragma once
#include "v8trace/core.h"

namespace v8trace {
const std::vector<std::pair<std::string, std::string>>& state_channels();
bool state_selected(const Json& options);
Json selected_state_channels(const Json& options);
void import_state(Trace&, Store&, const Json& options);
Json correlate_state(Store& store);
}
