#pragma once
#include "v8trace/causality.h"
#include "v8trace/studio.h"
namespace v8trace {
Json causal_functions(CausalityEngine&,EvidenceViewer&,const Json&,std::atomic_bool* cancel=nullptr);
}
