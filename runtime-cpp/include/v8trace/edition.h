#pragma once
#include "v8trace/core.h"

namespace v8trace {
bool professional_edition() noexcept;
std::string_view edition_id() noexcept;
Json edition_info();
Json professional_capture_options();
Json edition_blocked_options(const Json& options);
bool edition_prompt_available(std::string_view id) noexcept;
void validate_edition_options(const Json& options);
void require_professional(std::string_view feature);
void validate_browser_edition(const Json& descriptor);
}
