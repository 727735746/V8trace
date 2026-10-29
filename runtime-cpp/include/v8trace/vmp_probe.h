#pragma once
#include "v8trace/core.h"
namespace v8trace {
Json normalize_vmp_probe(const Json&);
Json vmp_spec_template(const fs::path&, const std::string&);
Json validate_vmp_record(const Json&, const Json&);
}
