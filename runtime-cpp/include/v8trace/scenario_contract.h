#pragma once
#include "v8trace/core.h"

namespace v8trace {
Json scenario_catalog(const fs::path& assets);
Json scenario_contract(const Json& options, const fs::path& assets);
Json gap_diagnostic(const Json& gap, const Json& options);
Json capture_diagnostics(const Json& summary, const Json& options);
}
