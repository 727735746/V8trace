#include "v8trace/edition.h"
#include "v8trace/edition_build.h"

namespace v8trace {
namespace {
constexpr const char* professional_options[]{"opcode", "wasm", "vmpProbe", "httpRootTrace", "domTrace", "cryptoTrace"};
}
bool professional_edition() noexcept { return V8TRACE_BUILD_PROFESSIONAL != 0; }
bool edition_prompt_available(std::string_view id) noexcept { return professional_edition() || (id != "jsvmp-evidence" && id != "wasm-evidence"); }
std::string_view edition_id() noexcept { return V8TRACE_BUILD_EDITION; }
Json professional_capture_options() {
  Json keys = Json::array(); for (auto key : professional_options) keys.push_back(key); return keys;
}
Json edition_info() {
  Json restricted = Json::array();
  if (!professional_edition()) for (auto key : professional_options) restricted.push_back(key);
  return {{"protocol", "v8trace/edition/1"}, {"id", edition_id()},
    {"label", professional_edition() ? "Professional 专业版" : "Community 社区版"},
    {"sourceLicense", professional_edition() ? "proprietary" : "Apache-2.0"},
    {"buildTimeIdentity", true}, {"restrictedCaptureOptions", restricted}, {"professionalCaptureOptions", professional_capture_options()},
    {"advancedAnalysis", professional_edition()}, {"environmentWorkflow", "complete"},
    {"selectedValues", "full"}, {"commercialTruncation", false},
    {"logSchema", "shared"}, {"readsProfessionalLogs", true}};
}
Json edition_blocked_options(const Json& options) {
  Json blocked = Json::array();
  if (!professional_edition()) for (auto key : professional_options)
    if (options.contains(key) && options.at(key) != false) blocked.push_back(key);
  return blocked;
}
void validate_edition_options(const Json& options) {
  if (options.contains("edition") && options.at("edition") != std::string(edition_id()))
    throw std::runtime_error("edition is fixed at build time: " + std::string(edition_id()));
  auto blocked = edition_blocked_options(options);
  if (!blocked.empty()) throw std::runtime_error("Community edition: Professional capture options selected: " + dump(blocked) + "; configuration was not downgraded");
}
void require_professional(std::string_view feature) {
  if (!professional_edition()) throw std::runtime_error("Professional edition required: " + std::string(feature));
}
void validate_browser_edition(const Json& descriptor) {
  // Descriptors predating edition separation belong to the existing full build.
  auto browser = str(descriptor, "edition", "professional");
  if (browser != edition_id()) throw std::runtime_error("Browser edition mismatch: host=" + std::string(edition_id()) + ", browser=" + browser);
}
}
