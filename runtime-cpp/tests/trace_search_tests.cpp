#include "v8trace/trace_search.h"
#include "../src/trace_search_internal.h"
#include <iostream>
#include <limits>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#endif

using namespace v8trace;
namespace {
unsigned checks = 0;
void check(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
template<class F> void rejected(F function, const char* message) { bool failed = false; try { function(); } catch (const std::exception&) { failed = true; } check(failed, message); }
Json wait(TraceSearchEngine& engine) {
  auto end = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (true) {
    auto status = engine.status(); if (str(status,"state") == "ready") return status;
    if (str(status,"state") == "failed") throw std::runtime_error("Index failed: " + str(status,"error"));
    check(std::chrono::steady_clock::now() < end, "Index deadline"); std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}
void cleanup(const fs::path& root) {
  if (!fs::exists(root)) return;
  auto actual = fs::canonical(root), temporary = fs::canonical(fs::temp_directory_path());
  if (actual.parent_path() != temporary || !utf8(actual.filename()).starts_with("v8trace-search-test-")) throw std::runtime_error("Test cleanup boundary changed");
  fs::remove_all(actual);
}
}
int main() {
  const auto root = fs::temp_directory_path() / ("v8trace-search-test-" + uuid());
  fs::create_directories(root / "sources");
  try {
    {
    TraceSearchConfig config; config.source_root = root / "sources"; config.index_dir = root / "index"; config.export_root = root / "exports"; config.checkpoint_bytes = 65536;
    write_file(root/"outside.log","outside"); write_file(config.source_root/"lines.log","alpha\r\nbeta\n\n尾部"); write_file(config.source_root/"empty.log","");
    write_file(config.source_root/"invalid.log",std::string("bad\xff\n\xe2\x82",7));
    TraceSearchEngine engine(config); check(engine.status()["state"] == "no_file", "Initial state");
    rejected([&]{engine.open({{"path",utf8(root/"outside.log")}});}, "Source root confinement");
    rejected([&]{engine.open({{"path","lines.log"},{"force_rebuild",nullptr}});}, "Null required boolean rejected");
    rejected([&]{engine.open({{"path",std::string("lines.log\0ignored",17)}});},"Embedded NUL source path rejected");
    engine.open({{"path","lines.log"}}); auto ready = wait(engine); check(ready["line_count"] == 4, "LF count without final newline");
    check(engine.read({{"start_line",1},{"line_count",1}})["content"] == "alpha\r\n", "Read preserves CRLF");
    check(engine.read({{"start_line",2},{"line_count",2}})["content"] == "beta\n\n", "Read empty interior line");
    auto zero = engine.read({{"start_line",2},{"line_count",0}}); check(zero["returned_lines"] == 0 && zero["next_line"] == 2 && zero["end_line"].is_null() && zero["eof"] == false, "Zero-count cursor");
    auto eof = engine.read({{"start_line",5},{"line_count",10}}); check(eof["eof"] == true && eof["returned_lines"] == 0 && eof["next_line"].is_null(), "EOF N+1");
    rejected([&]{engine.read({{"start_line",0},{"line_count",1}});}, "Reject zero-based line");
    rejected([&]{engine.read({{"start_line",6},{"line_count",0}});}, "Reject beyond EOF");
    rejected([&]{engine.open({{"path","empty.log"}});}, "Single active file");
    check(engine.open({{"path","lines.log"}})["index_reused"] == true, "Same-ready reuse");
    auto index_file = path(str(ready,"index_path")); engine.close();
    check(engine.open({{"path","lines.log"}})["index_reused"] == true, "Persistent index reuse"); engine.close();
    { std::fstream file(index_file,std::ios::binary|std::ios::in|std::ios::out); file.seekp(8); file.put('X'); }
    check(engine.open({{"path","lines.log"}})["index_reused"] == false, "Corrupt index rebuild"); wait(engine); engine.close();
    engine.open({{"path","empty.log"}}); check(wait(engine)["line_count"] == 0, "Empty file has no synthetic line");
    check(engine.read({{"start_line",1},{"line_count",0}})["eof"] == true, "Empty EOF"); engine.close();
    engine.open({{"path","invalid.log"}}); wait(engine); auto invalid = engine.read({{"start_line",1},{"line_count",2}});
    check(invalid["lossy_utf8"] == true && invalid["source_bytes"] == 7 && invalid["content"] == std::string("bad\xef\xbf\xbd\n\xef\xbf\xbd"), "Lossy UTF-8 maximal subparts"); engine.close();
    write_file(config.source_root/"large-line.log",std::string(200000,'x')+"\nend\n");
    auto limited = config; limited.max_content_bytes = 16; TraceSearchEngine bounded(limited); bounded.open({{"path","large-line.log"}}); auto large = wait(bounded);
    check(large["line_count"] == 2 && large["checkpoint_count"] == 2, "Long line keeps sparse index");
    rejected([&]{bounded.read({{"start_line",1},{"line_count",1}});}, "Oversized read rejected before full allocation");
    check(bounded.read({{"start_line",2},{"line_count",1}})["content"] == "end\n", "Read after long-line checkpoint"); bounded.close();
    {
      auto query=[](std::string pattern,uint64_t start=1,uint64_t scan=std::numeric_limits<uint64_t>::max()){return Json{{"pattern",std::move(pattern)},{"start_line",start},{"max_scan_lines",scan}};};
      engine.open({{"path","lines.log"}});wait(engine);
      auto hits=engine.search(query("a"));check(hits["matches"].size()==2&&hits["matches"][0]["content"]=="alpha"&&hits["matches"][1]["line"]==2&&hits["scanned_lines"]==4&&hits["eof"]==true,"Literal bytes, strip CRLF, ordered scan");
      auto cap=query("a");cap["max_matches"]=1;auto page=engine.search(cap);check(page["matches"].size()==1&&page["next_line"]==2&&page["scanned_lines"]==1&&page["match_limit_reached"]==true&&!page["scan_limit_reached"].get<bool>(),"Match pagination");
      auto limited_scan=engine.search(query("absent",2,1));check(limited_scan["next_line"]==3&&limited_scan["scanned_lines"]==1&&limited_scan["scan_limit_reached"]==true,"Scan pagination");
      auto no_scan=engine.search(query("a",2,0));check(no_scan["matches"].empty()&&no_scan["next_line"]==2&&no_scan["scan_limit_reached"]==true,"Zero scan cursor");
      auto at_eof=engine.search(query("a",5,0));check(at_eof["eof"]==true&&at_eof["next_line"].is_null(),"Search EOF");
      rejected([&]{engine.search(query(""));},"Empty literal rejected");
      auto bad_regex=query("(",1,0);bad_regex["regex"]=true;rejected([&]{engine.search(bad_regex);},"Regex compiled even with zero scan");
      auto regex=query("^b.ta$");regex["regex"]=true;check(engine.search(regex)["matches"].size()==1,"RE2 regex mode");
      regex["pattern"]="(?<=b)eta";rejected([&]{engine.search(regex);},"Unsupported lookbehind rejected");
      std::atomic_bool stopped{true};rejected([&]{engine.search(query("a"),&stopped);},"Search cancellation");
      auto exported=engine.export_lines({{"start_line",1},{"line_count",2},{"output_path","raw.log"}});check(exported["bytes_written"]==12&&read_file(config.export_root/"raw.log")=="alpha\r\nbeta\n","Export preserves raw CRLF");
      rejected([&]{engine.export_lines({{"start_line",1},{"line_count",1},{"output_path","raw.log"}});},"Export refuses existing destination");
      engine.export_lines({{"start_line",3},{"line_count",2},{"output_path","raw.log"},{"overwrite",true}});check(read_file(config.export_root/"raw.log")=="\n尾部","Atomic overwrite preserves tail");
      rejected([&]{engine.export_lines({{"start_line",1},{"line_count",1},{"output_path","../outside.log"},{"overwrite",true}});},"Export root escape rejected");
#ifdef _WIN32
      rejected([&]{engine.export_lines({{"start_line",1},{"line_count",1},{"output_path","raw.log:stream"}});},"Windows alternate stream rejected");
      rejected([&]{engine.export_lines({{"start_line",1},{"line_count",1},{"output_path","NUL .txt"}});},"Reserved Windows device rejected");
#endif
      rejected([&]{engine.export_lines({{"start_line",1},{"line_count",0},{"output_path","missing/child.log"}});},"Export parent must exist");
      fs::create_hard_link(config.source_root/"lines.log",config.export_root/"source-link.log");rejected([&]{engine.export_lines({{"start_line",1},{"line_count",1},{"output_path","source-link.log"},{"overwrite",true}});},"Hard-linked source overwrite rejected");
      rejected([&]{engine.export_lines({{"start_line",1},{"line_count",2},{"output_path","cancelled.log"}},&stopped);},"Export cancellation");check(!fs::exists(config.export_root/"cancelled.log"),"Cancelled export not published");
      auto empty_export=engine.export_lines({{"start_line",5},{"line_count",0},{"output_path","empty.log"}});check(empty_export["bytes_written"]==0&&empty_export["end_line"].is_null()&&empty_export["eof"]==true,"Zero-byte EOF export");engine.close();
      auto overlap=config;overlap.export_root=config.source_root;TraceSearchEngine overlapping(overlap);overlapping.open({{"path","lines.log"}});wait(overlapping);rejected([&]{overlapping.export_lines({{"start_line",1},{"line_count",1},{"output_path","lines.log"},{"overwrite",true}});},"Source overwrite rejected");overlapping.close();
      write_file(config.source_root/"content.log","hit\n123hit\nhit\n");auto short_config=config;short_config.max_content_bytes=5;TraceSearchEngine small(short_config);small.open({{"path","content.log"}});wait(small);auto content_page=small.search(query("hit"));check(content_page["matches"].size()==1&&content_page["scanned_lines"]==1&&content_page["next_line"]==2&&content_page["content_limit_reached"]==true,"Content cap excludes entire matching line");rejected([&]{small.search(query("hit",2));},"First oversized match rejected");small.close();
      write_file(config.source_root/"unicode.log","Äpfel\näpfel\nx\ry\r");engine.open({{"path","unicode.log"}});wait(engine);auto unicode=query("ÄPFEL");unicode["case_sensitive"]=false;check(engine.search(unicode)["matches"].size()==2,"Unicode case-insensitive literal");auto cr=engine.search(query("x\ry"));check(cr["matches"][0]["content"]=="x\ry","Interior CR retained, EOF CR stripped");engine.close();
      engine.open({{"path","invalid.log"}});wait(engine);auto invalid_hit=engine.search(query("bad"));check(invalid_hit["lossy_utf8"]==true&&invalid_hit["matches"][0]["content"]=="bad\xef\xbf\xbd","Match raw bytes before lossy output");check(engine.search(query("absent"))["lossy_utf8"]==false,"Unmatched invalid bytes do not mark result lossy");engine.close();
      auto short_line=config;short_line.max_line_bytes=32;TraceSearchEngine line_limit(short_line);line_limit.open({{"path","large-line.log"}});wait(line_limit);rejected([&]{line_limit.search(query("absent"));},"Nonmatching oversized line rejected");line_limit.close();
      std::string parallel;for(unsigned n=0;n<18000;++n)parallel+=std::to_string(n)+std::string(30,'x')+(n%37==0?" needle\n":" other\n");write_file(config.source_root/"parallel.log",parallel);
      auto single_config=config;single_config.search_threads=1;auto multi_config=config;multi_config.search_threads=4;multi_config.search_memory_bytes=4294967296ULL;
      TraceSearchEngine single(single_config),multi(multi_config);single.open({{"path","parallel.log"}});wait(single);multi.open({{"path","parallel.log"}});wait(multi);
      auto compare=query("needle");compare["max_matches"]=1000;auto one=single.search(compare),many=multi.search(compare);check(many["worker_count"].get<unsigned>()>1,"Parallel checkpoints used");one.erase("worker_count");many.erase("worker_count");check(one==many,"Sequential/parallel exact ordered equivalence");
      compare["max_matches"]=7;one=single.search(compare);many=multi.search(compare);one.erase("worker_count");many.erase("worker_count");check(one==many,"Parallel early cap equivalence");single.close();multi.close();
      for(const auto& entry:fs::directory_iterator(config.export_root))check(!utf8(entry.path().filename()).starts_with(".v8trace-export-"),"No export temporary remains");
    }
    write_file(config.source_root/"resume.log",std::string(70000,'a')+"\n"+std::string(70000,'b')+"\nlast");
    auto source = trace_search::inspect_source(config.source_root/"resume.log",fs::canonical(config.source_root)); trace_search::IndexProgress progress;
    auto normalized = config; normalized.index_dir = fs::canonical(config.index_dir);
    auto first = trace_search::build_index(source,normalized,false,progress); check(first->line_count == 3, "Initial resume fixture");
    auto directory = first->index_file.parent_path(); auto meta = load_json(directory/"snapshot.json");
    meta["payload"]["state"] = "building"; meta["payload"]["lineCount"] = uint64_t(0); meta["sha256"] = sha256(dump(meta["payload"])); write_json(directory/"snapshot.json",meta);
    fs::copy_file(first->index_file,directory/"lines.partial"); { std::ofstream tail(directory/"lines.partial",std::ios::binary|std::ios::app); tail << "bad"; }
    trace_search::IndexProgress resumed; auto resumed_index = trace_search::build_index(source,normalized,false,resumed);
    check(resumed.resume_offset.load() > 0 && resumed_index->line_count == 3, "Resume longest valid CRC prefix without double count");
    check(resumed_index->offset(3) == 140002, "Resumed exact boundary");
    auto maximum = std::numeric_limits<uint64_t>::max(); check(trace_search::number({{"n",maximum}},"n") == maximum, "Full uint64 request range");
    const uint64_t high = (uint64_t(1)<<32)+17; auto sparse = config.source_root/"offset64.log";
#ifdef _WIN32
    auto handle = CreateFileW(sparse.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr); check(handle != INVALID_HANDLE_VALUE,"Create sparse test");
    DWORD returned = 0; bool sparse_ok = DeviceIoControl(handle,FSCTL_SET_SPARSE,nullptr,0,nullptr,0,&returned,nullptr) != 0;
    if (!sparse_ok) { CloseHandle(handle); throw std::runtime_error("Sparse-file test requires sparse-capable temporary filesystem"); }
    LARGE_INTEGER position; position.QuadPart = high; bool seek_ok = SetFilePointerEx(handle,position,nullptr,FILE_BEGIN) != 0; DWORD written = 0; bool write_ok = seek_ok && WriteFile(handle,"\nTAIL",5,&written,nullptr) && written==5; CloseHandle(handle); check(write_ok,"Write high-offset sparse tail");
#else
    {std::ofstream file(sparse,std::ios::binary);file.seekp(std::streamoff(high));file << "\nTAIL";}
#endif
    trace_search::LineIndex synthetic; synthetic.source = trace_search::inspect_source(sparse,fs::canonical(config.source_root)); synthetic.line_count=2;synthetic.checkpoints={{1,0},{2,high+1}};
    check(synthetic.offset(2)==high+1 && synthetic.offset(3)==high+5,"64-bit indexed positions");
    {std::ifstream file(sparse,std::ios::binary);file.seekg(std::streamoff(synthetic.offset(2)));std::string tail(4,'\0');file.read(tail.data(),4);check(tail=="TAIL","Actual seek above 4 GiB");}
    auto high_query=trace_search::search_index(synthetic,config,{{"pattern","TAIL"},{"start_line",2},{"max_scan_lines",1}},nullptr);check(high_query["matches"][0]["line"]==2&&high_query["matches"][0]["content"]=="TAIL","Search above 4 GiB");
    auto high_export=trace_search::export_index(synthetic,config,{{"start_line",2},{"line_count",1},{"output_path","high-tail.log"}},nullptr);check(high_export["bytes_written"]==4&&read_file(config.export_root/"high-tail.log")=="TAIL","Raw export above 4 GiB");
    engine.open({{"path","lines.log"}});wait(engine);{std::ofstream changed(config.source_root/"lines.log",std::ios::binary|std::ios::app);changed << "\nchanged";}
    rejected([&]{engine.read({{"start_line",1},{"line_count",1}});},"Changed source rejected");check(engine.status()["state"]=="failed","Changed snapshot marked failed");engine.close();
    engine.open({{"path","resume.log"},{"force_rebuild",true}});engine.close();check(engine.status()["state"]=="no_file","Close cancels build admission");
    engine.open({{"path","empty.log"}});wait(engine);check(engine.status()["line_count"]==0,"Stale cancelled job cannot publish");engine.close();
    std::atomic_bool cancelled{true};engine.open({{"path","empty.log"}});wait(engine);rejected([&]{engine.read({{"start_line",1},{"line_count",0}},&cancelled);},"Read cancellation");engine.close();
    }
    cleanup(root); std::cout << checks << " checks passed\n"; return 0;
  } catch(const std::exception& error) { std::cerr << error.what() << "\nOwned test fixture: " << utf8(root) << '\n'; try {cleanup(root);}catch(...){} return 1; }
}
