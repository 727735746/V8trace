#include "v8trace/trace_search.h"
#include "v8trace/index.h"
#include "../src/trace_search_internal.h"
#include <iostream>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif

using namespace v8trace;
namespace {
unsigned checks = 0;
void check(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
template<class F> void rejected(F action, const char* message) { bool failed=false; try { action(); } catch(const std::exception&) { failed=true; } check(failed,message); }
Json wait(TraceSearchEngine& engine) {
  auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
  while(true) {
    auto status=engine.status(); auto state=str(status,"state");
    if(state=="ready")return status;
    if(state=="failed")throw std::runtime_error(str(status,"error"));
    check(std::chrono::steady_clock::now()<deadline,"Long index deadline");
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}
void cleanup(const fs::path& root) {
  auto actual=fs::weakly_canonical(io_path(root));
  auto parent=fs::canonical(io_path(fs::temp_directory_path()));
  check(actual.parent_path()==parent && utf8(actual.filename()).starts_with("v8trace-long-index-test-"),"Owned cleanup boundary");
  fs::remove_all(io_path(actual));
}
}
int main() {
  const auto root=fs::temp_directory_path()/("v8trace-long-index-test-"+uuid());
  try {
    fs::create_directories(root/"source");
    auto cache=root/path("中文 索引");
    while(cache.native().size()<420)cache/=path("目录很深 但不缩短缓存路径-0123456789");
    check(io_path(io_path(cache))==io_path(cache),"Extended path is idempotent");
    check(logical_path(io_path(cache))==fs::absolute(cache).lexically_normal(),"Logical path roundtrip");
    check(inside(root,io_path(cache)) && inside(io_path(root),cache),"Ordinary/extended containment agrees");
    check(!inside(root,io_path(root.parent_path()/"outside")),"Extended sibling stays outside");
#ifdef _WIN32
    auto unc=path("//fixture/share/中文/index");
    check(io_path(unc).native().starts_with(L"\\\\?\\UNC\\"),"UNC uses native namespace");
    check(logical_path(io_path(unc)).lexically_normal()==unc.lexically_normal(),"UNC logical roundtrip without network access");
    rejected([]{io_path(path("//./PhysicalDrive0"));},"Device namespace rejected");
    rejected([]{io_path(path("//?/GLOBALROOT/Device/HarddiskVolume1"));},"Global namespace rejected");
    rejected([]{io_path(fs::path(L"G:\\"+std::wstring(32767,L'x')));},"Unicode total path budget enforced");
#endif
    const std::string bytes="alpha\r\nbeta\xff\nlast\n";
    auto source=root/"source"/path("中文原文.log");
    write_file(source,bytes); auto original=file_sha256(source);
    TraceSearchConfig config;config.source_root=root/"source";config.index_dir=cache;config.export_root=root/"exports";config.checkpoint_bytes=65536;
    {
      TraceSearchEngine engine(config);engine.open({{"path",utf8(source.filename())}});auto status=wait(engine);
      check(status["line_count"]==3,"Long-root line index built");
      auto index=path(str(status,"index_path"));
      check(index==logical_path(index) && fs::is_regular_file(io_path(index)),"Public index path stays ordinary");
      check(index.parent_path().filename().native().size()==64,"Full cache identity hash retained");
      check(engine.read({{"start_line",1},{"line_count",3}})["source_bytes"]==bytes.size(),"Long cache read has all bytes");
      check(engine.search({{"pattern","alpha"},{"start_line",1},{"max_scan_lines",3}})["matches"].size()==1,"Long cache search");
      auto exported=engine.export_lines({{"start_line",1},{"line_count",3},{"output_path","raw.log"}});
      check(read_file(path(str(exported,"output_path")))==bytes,"Raw bytes exported unchanged");
      engine.close();
      check(engine.open({{"path",utf8(source.filename())}})["index_reused"]==true,"Persistent long cache reused");
      engine.close();
      {std::fstream damaged(io_path(index),std::ios::binary|std::ios::in|std::ios::out);check(bool(damaged),"Open long index for corruption fixture");damaged.seekp(8);damaged.put('X');}
      check(engine.open({{"path",utf8(source.filename())}})["index_reused"]==false,"Corrupt long cache rebuilt");wait(engine);engine.close();
#ifdef _WIN32
      auto lock=CreateFileW(io_path(index.parent_path()/"build.lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
      check(lock!=INVALID_HANDLE_VALUE,"Own long cache lock");
      try {
        engine.open({{"path",utf8(source.filename())},{"force_rebuild",true}});
        auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
        while(str(engine.status(),"state")=="indexing" && std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(2));
        check(str(engine.status(),"state")=="failed" && str(engine.status(),"error").find("busy")!=std::string::npos,"Exclusive lock conflict retained");
      } catch(...) {CloseHandle(lock);throw;}
      CloseHandle(lock);engine.close();
#endif
    }
    {
      auto resume=root/"source"/"resume.log";write_file(resume,std::string(70000,'a')+"\n"+std::string(70000,'b')+"\nlast");
      auto inspected=trace_search::inspect_source(resume,config.source_root);trace_search::IndexProgress progress;
      auto first=trace_search::build_index(inspected,config,false,progress);auto directory=first->index_file.parent_path();
      auto meta=load_json(directory/"snapshot.json");meta["payload"]["state"]="building";meta["payload"]["lineCount"]=uint64_t(0);meta["sha256"]=sha256(dump(meta["payload"]));write_json(directory/"snapshot.json",meta);
      fs::copy_file(first->index_file,directory/"lines.partial");{std::ofstream tail(directory/"lines.partial",std::ios::binary|std::ios::app);tail<<"bad";}
      trace_search::IndexProgress stopped;stopped.cancel=true;rejected([&]{trace_search::build_index(inspected,config,false,stopped);},"Long-root cancelled index retains checkpoints");
      trace_search::IndexProgress resumed;auto restored=trace_search::build_index(inspected,config,false,resumed);
      check(resumed.resume_offset.load()>0 && restored->line_count==3 && restored->offset(3)==140002,"Long-root CRC-prefix resume");
    }
    {
      DiskIndex database(cache);
      database.exec("CREATE TABLE long_probe(value BLOB)");
      database.each("INSERT INTO long_probe VALUES(?)",Json::array({"中文"}),{});
      unsigned rows=0;database.each("SELECT value FROM long_probe",Json::array(),[&](const Json& row){check(row[0]=="中文","Long-root SQLite query");++rows;});
      check(rows==1,"Long-root SQLite row count");
    }
    for(const auto& item:fs::directory_iterator(io_path(cache)))check(!utf8(item.path().filename()).starts_with("analysis-cpp-"),"Owned SQLite directory removed");
    check(file_sha256(source)==original,"Source bytes unchanged");
    auto length=cache.native().size();cleanup(root);
    std::cout<<dump(Json{{"status","passed"},{"checks",checks},{"indexPathNativeUnits",length},{"uncScope","conversion-only; no live share"}})<<'\n';return 0;
  } catch(const std::exception& error) {
    try{cleanup(root);}catch(...){}
    std::cerr<<error.what()<<'\n';return 1;
  }
}
