#include "v8trace/core.h"

namespace v8trace {
void import_causality(Trace& trace,Store& store,const Json& options) {
  if(field(options,"httpRootTrace",false)!=true) return;
  uint64_t records=0,invalid=0;
  auto accept=[&](const Json& row) {
    const auto& record=row.at("record");const auto& event=row.at("event");
    if(str(record,"channel")!="causality" && str(event,"name")!="V8TraceCausality") return;
    auto schema=str(record,"schema");
    bool valid=record.is_object() && (schema=="v8trace/browser/1" || schema=="v8trace/causality/1") && str(record,"protocol")=="v8trace/causality/1" && !str(record,"producerId").empty() && field(record,"epoch").is_string() && !str(record,"kind").empty() && str(event,"cat").find("disabled-by-default-v8.causality")!=std::string::npos;
    if(!valid) {++invalid;store.gap("causality-invalid-native-record",{{"nativeIndex",row.at("index")}});}
    Json scope{{"processId",field(event,"pid")},{"threadId",field(event,"tid")},{"traceTimestampMicroseconds",field(event,"ts")},{"recordingWindow",field(event,"v8traceWindow",0)},{"nativeIndex",row.at("index")},{"fidelity","native-causal-ledger"},{"envelopeValid",valid}};
    if(store.event("causality",str(record,"kind","invalid-native-record"),record,scope)) ++records;
  };
  trace.each("browser",accept);trace.each("causality",accept);
  if(!records)store.gap("causality-stream-missing");
  store.json("causality-summary.json",{{"protocol","v8trace/causality/1"},{"records",records},{"invalid",invalid},{"completeCausality",false},{"completion","per-root-collector-proof-required"},{"rawEnvelopesPreserved",true},{"crossThreadOrderInferred",false}});
}
}
