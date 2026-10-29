#include "v8trace/edition.h"
#include "v8trace/causality.h"
#include "v8trace/causal_queries.h"
#include "v8trace/vmp_probe.h"

namespace v8trace {
namespace {
[[noreturn]] void advanced_analysis() { throw std::runtime_error("Professional edition required: advanced HTTP-root/JSVMP analysis; basic raw evidence search remains available"); }
}
struct CausalityEngine::Impl {};
CausalityEngine::CausalityEngine(fs::path, fs::path, Sink, std::atomic_bool*) { advanced_analysis(); }
CausalityEngine::~CausalityEngine() = default;
Json CausalityEngine::stats(std::atomic_bool*) const { advanced_analysis(); }
Json CausalityEngine::roots(const Json&, std::atomic_bool*) { advanced_analysis(); }
Json CausalityEngine::root(const Json&, std::atomic_bool*) { advanced_analysis(); }
Json CausalityEngine::nodes(const Json&, std::atomic_bool*) { advanced_analysis(); }
Json CausalityEngine::checkpoints(const Json&, std::atomic_bool*) { advanced_analysis(); }
Json causal_functions(CausalityEngine&, EvidenceViewer&, const Json&, std::atomic_bool*) { advanced_analysis(); }
Json discover_vmp(const fs::path&, const fs::path&, const Json&) { advanced_analysis(); }
Json normalize_vmp_probe(const Json& options) { if (field(options, "vmpProbe", false) == false) return nullptr; advanced_analysis(); }
Json vmp_spec_template(const fs::path&, const std::string&) { advanced_analysis(); }
Json validate_vmp_record(const Json&, const Json&) { advanced_analysis(); }
}
