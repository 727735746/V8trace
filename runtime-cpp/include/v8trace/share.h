#pragma once
#include "v8trace/core.h"

namespace v8trace {
Json share_field_settings(const Json& settings);
Json preview_share_copy(const fs::path& source, const Json& settings, const Sink& progress = {}, const std::atomic_bool* cancel = nullptr);
Json create_share_copy(const fs::path& source, const fs::path& output, const Json& settings, std::string_view preview_binding, const Sink& progress = {}, const std::atomic_bool* cancel = nullptr);
}
