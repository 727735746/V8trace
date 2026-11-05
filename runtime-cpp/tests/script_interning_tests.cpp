#include "v8trace/core.h"
#include <iostream>

using namespace v8trace;
namespace {
unsigned checks = 0;
void check(bool value, const char* reason) { ++checks; if (!value) throw std::runtime_error(reason); }
Json event(const std::string& literal, uint64_t index, bool interned) {
  Json record{{"schema", "v8trace/browser/1"}, {"channel", "script"}, {"producer", "v8"},
      {"producerSeq", std::to_string(index)}, {"kind", "compile-input"}, {"compilationKind", "fixture"},
      {"scriptId", index}, {"url", "https://fixture.test/source.js"}, {"isModule", false}};
  Json args;
  if (interned) {
    record.update({{"nativeSourceProtocol", "v8trace/native-script-interning/1"},
        {"nativeSourceJsonBytes", literal.size()}, {"nativeSourceJsonSha256", sha256(literal)}});
    args["v8traceSourceJson"] = literal;
  } else record["source"] = parse(literal);
  args["record"] = dump(record);
  return {{"name", "V8TraceBrowser"}, {"cat", "disabled-by-default-v8.script"},
      {"pid", 71}, {"tid", 9}, {"ts", 100+index}, {"args", std::move(args)}};
}
}

int main() {
  const auto root = fs::temp_directory_path() / ("v8trace-script-interning-"+uuid());
  fs::create_directory(root);
  try {
    std::vector<std::string> literals{"\"\"", R"("ASCII\"\\\u0000\u000a")",
        R"("\u2603\ud83d\ude00\ud800Z\udfff")", '"'+std::string(262144, 'x')+'"'};
    Json legacy = Json::array(), interned = Json::array(), expected = Json::array();
    for (const auto& literal : literals) {
      auto index = expected.size()+1;
      expected.push_back(parse(literal)); legacy.push_back(event(literal,index,false)); interned.push_back(event(literal,index,true));
    }
    for (const auto& [name, rows] : std::vector<std::pair<std::string, Json>>{{"legacy",legacy},{"interned",interned}}) {
      auto file = root/(name+".json"); write_json(file, {{"traceEvents",rows}});
      size_t count = 0;
      { Trace trace(file); trace.each("browser", [&](const Json& row) {
          const auto& record = row.at("record");
          check(record.at("source")==expected.at(count), "source reconstructs exactly before normalization");
          check(utf16le(str(record,"source"))==utf16le(text(expected.at(count))), "NUL, Unicode and lone surrogates preserve UTF16 units");
          check(str(record,"producerSeq")==std::to_string(++count), "all source events and producer identities remain");
        });
        check(count==expected.size(), "no source event filtering or collapse");
      }
      Json options{{"scripts",true},{"maxEvents",1000},{"maxBytes",16777216},{"maxBodyBytes",8388608}};
      Store store(root/name,options);
      { Trace trace(file); import_browser(trace,store,options); }
      auto summary=store.finalize({{"backend","chromium-native"},{"captureConclusion","collected"}});
      check(summary.at("eventCount")==expected.size()&&summary.at("criticalGapCount")==0,"existing normalized script stream remains complete");
      count=0;
      lines(root/name/"events/script.jsonl",[&](const Json& row){
        const auto& payload=row.at("payload");
        check(str(payload,"sourceStorageProtocol")=="v8trace/native-script-source/2","existing blob storage contract is unchanged");
        const auto bytes=read_file(root/name/str(payload.at("sourceUtf16"),"path"));
        check(bytes==utf16le(text(expected.at(count++))),"normalized source blob contains the same exact UTF16");
      });
      check(count==expected.size(),"every normalized compile-input event is retained");
    }
    auto reject = [&](const std::string& name, Json row) {
      auto file=root/(name+".json"); write_json(file,{{"traceEvents",Json::array({row})}});
      bool failed=false;try {Trace trace(file);}catch(const std::exception&){failed=true;}
      check(failed,"malformed interning evidence is rejected rather than guessed");
    };
    auto mutate = [&](const std::string& name, auto change) {
      auto row=interned.at(1);auto record=parse(str(row.at("args"),"record"));
      change(row,record);row["args"]["record"]=dump(record);reject(name,std::move(row));
    };
    mutate("missing-definition",[](Json& row,Json&){row["args"].erase("v8traceSourceJson");});
    mutate("unknown-protocol",[](Json&,Json& r){r["nativeSourceProtocol"]="unknown";});
    mutate("missing-protocol",[](Json&,Json& r){r.erase("nativeSourceProtocol");});
    mutate("conflicting-inline",[](Json&,Json& r){r["source"]="conflict";});
    mutate("wrong-channel",[](Json&,Json& r){r["channel"]="http";});
    mutate("wrong-byte-count",[](Json&,Json& r){r["nativeSourceJsonBytes"]=1;});
    mutate("string-byte-count",[](Json&,Json& r){r["nativeSourceJsonBytes"]="1";});
    mutate("wrong-digest",[](Json&,Json& r){r["nativeSourceJsonSha256"]=std::string(64,'0');});
    mutate("non-string-annotation",[](Json& row,Json&){row["args"]["v8traceSourceJson"]=12;});
    mutate("non-string-source",[](Json& row,Json& r){row["args"]["v8traceSourceJson"]="{}";r["nativeSourceJsonBytes"]=2;r["nativeSourceJsonSha256"]=sha256("{}");});
    mutate("unfinished-source",[](Json& row,Json& r){const std::string raw="\"unfinished";row["args"]["v8traceSourceJson"]=raw;r["nativeSourceJsonBytes"]=raw.size();r["nativeSourceJsonSha256"]=sha256(raw);});
    const auto resolved=fs::weakly_canonical(root);
    check(resolved.parent_path()==fs::weakly_canonical(fs::temp_directory_path())&&resolved.filename().string().starts_with("v8trace-script-interning-"),"cleanup is scoped to this owned test root");
    fs::remove_all(resolved);
    std::cout<<"PASS "<<checks<<" checks\n";return 0;
  } catch(const std::exception& error) {
    std::cerr<<"FAIL "<<checks<<": "<<error.what()<<"; retained="<<utf8(root)<<'\n';return 1;
  }
}
