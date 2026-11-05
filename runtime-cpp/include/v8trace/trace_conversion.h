#pragma once
#include "v8trace/core.h"

namespace v8trace {
Json verify_perfetto_converter(const fs::path& directory);
Json convert_perfetto_trace(const fs::path& directory, const fs::path& input, const fs::path& output,
                            uint64_t maximum_bytes, uint64_t timeout_ms, const Sink& progress = {});
}
