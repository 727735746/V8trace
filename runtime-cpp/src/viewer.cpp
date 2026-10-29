#include "v8trace/studio.h"
#include "v8trace/index.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <regex>
#include <streambuf>

namespace v8trace {
namespace {
void viewer_require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int64_t stamp(const fs::path& file) { return fs::last_write_time(file).time_since_epoch().count(); }
std::string prefix(std::string_view input, size_t maximum) {
  size_t count = std::min(input.size(), maximum); while (count && count < input.size() && (uint8_t(input[count]) & 0xc0) == 0x80) --count;
  return std::string(input.substr(0, count));
}
class RangeBuffer : public std::streambuf {
 public:
  RangeBuffer(std::ifstream& input, uint64_t offset, uint64_t length, const std::atomic_bool* cancel = nullptr) : source(input), remaining(length), stop(cancel) { source.clear(); source.seekg(std::streamoff(offset)); viewer_require(bool(source), "Seek evidence record failed"); }
  std::string preview;
 protected:
  int_type underflow() override {
    if(stop&&stop->load())throw std::runtime_error("Evidence index cancelled; source capture retained");
    if (!remaining) return traits_type::eof(); auto count = size_t(std::min<uint64_t>(remaining, buffer.size()));
    source.read(buffer.data(), std::streamsize(count)); viewer_require(size_t(source.gcount()) == count, "Evidence record changed or is unfinished"); remaining -= count;
    if (preview.size() < 2048) preview.append(buffer.data(), std::min(count, 2048 - preview.size()));
    setg(buffer.data(), buffer.data(), buffer.data() + count); return traits_type::to_int_type(*gptr());
  }
 private:
  std::ifstream& source; uint64_t remaining; const std::atomic_bool* stop; std::array<char, 32768> buffer;
};
struct Projection {
  Json metadata = Json::object(), references = Json::array();
  std::set<std::pair<std::string, std::string>> terms;
  uint64_t boundaries = 0;
};
class ProjectionReader {
 public:
  ProjectionReader(std::istream& stream, std::string category) : input(stream), category_(std::move(category)) {}
  Projection run() { white(); value(0); white(); viewer_require(input.peek() == EOF, "Trailing evidence record input");
    auto resource=str(result.metadata,"resourceIdentifier"),layer=str(result.metadata,"nativeLayer",str(result.metadata,"layer"));
    if(!resource.empty()&&!layer.empty()&&result.metadata.contains("processId"))term("httpResource",text(result.metadata.at("processId"))+"/"+layer+"/"+resource);
    return std::move(result); }
 private:
  std::istream& input; std::string category_; Projection result; std::vector<std::string> route;
  void white() { while (input.peek() == ' ' || input.peek() == '\t' || input.peek() == '\r' || input.peek() == '\n') input.get(); }
  void expect(char c) { white(); viewer_require(input.get() == c, "Invalid evidence JSON structure"); }
  std::optional<Json> quoted(bool collect) {
    expect('"'); std::string raw; if (collect) raw += '"'; bool overflow = false;
    auto add = [&](int c) { if (collect && !overflow) { if (raw.size() < 65536) raw += char(c); else { raw.clear(); overflow = true; ++result.boundaries; } } };
    while (true) {
      auto c = input.get(); viewer_require(c >= 32, "Unfinished evidence JSON string"); add(c); if (c == '"') break;
      if (c == '\\') {
        c = input.get(); add(c); viewer_require(c != EOF && std::string_view("\"\\/bfnrtu").find(char(c)) != std::string_view::npos, "Invalid evidence JSON escape");
        if (c == 'u') for (int i = 0; i < 4; ++i) { c = input.get(); add(c); viewer_require(c != EOF && std::isxdigit(uint8_t(c)), "Invalid evidence Unicode escape"); }
      }
    }
    if (!collect || overflow) return std::nullopt; return parse(raw);
  }
  bool wanted() const {
    if (route.empty()) return false; auto key = route.back();
    if(route.size()==3 && route[0]=="payload" && route[1]=="causal") return key=="producerId" || key=="epoch" || key=="taskId" || key=="known";
    if (key == "path" || key == "sha256" || key == "bytes" || key == "representation") return true;
    static const std::set<std::string> keys{"seq","observedAt","category","kind","scriptUrl","url","scriptId","functionId","functionName","functionStart","requestId","callId","parentCallId","recordingWindow","operationId","activationId","processId","isolateId","producerId","threadId","sessionId","contextId","identity","from","relation","targets","phase","producerSeq","valuesCaptured","resourceIdentifier","nativeLayer","layer","functionIndex","moduleId","opcodeName","opcode","pc"};
    if ((!keys.contains(key) && !(key=="#"&&route.size()>1&&route[route.size()-2]=="targets")) || route.size() > 4) return false;
    static const std::set<std::string> containers{"payload","request","response","source","context","scope","function","script","entry","exit","targets","#"};
    for (size_t i = 0; i + 1 < route.size(); ++i) if (!containers.contains(route[i])) return false;
    return true;
  }
  void term(const std::string& field, const Json& scalar) {
    auto text_value = text(scalar); if (text_value.size() > 8192 || result.terms.size() >= 512) { ++result.boundaries; return; }
    result.terms.emplace(field, text_value);
  }
  void observe(const Json& scalar) {
    if (route.empty() || scalar.is_null()) return; auto key = route.back();
    if(route.size()==3 && route[0]=="payload" && route[1]=="causal") {
      std::string name=key=="producerId"?"causalProducer":key=="epoch"?"causalEpoch":key=="taskId"?"causalTask":"causalKnown";
      result.metadata[name]=scalar;if(name!="causalKnown") term(name,scalar);return;
    }
    if(key=="callId")term("callExact",scalar);
    if(key=="parentCallId")term("parentCall",scalar);
    static const std::map<std::string,std::string> fields{{"scriptUrl","script"},{"scriptId","script"},{"functionId","function"},{"functionName","function"},{"requestId","request"},{"callId","call"},{"parentCallId","call"},{"operationId","operation"},{"activationId","activation"},{"recordingWindow","window"},{"processId","process"},{"isolateId","isolate"},{"producerId","producer"},{"sessionId","session"},{"contextId","context"},{"url","url"},{"identity","identity"},{"from","identity"},{"targets","identity"},{"#","identity"}};
    if (auto found = fields.find(key); found != fields.end()) term(found->second, scalar);
    if (key == "url" && (category_ == "script" || category_ == "catalog/scripts" || category_ == "catalog/script-index")) term("script", scalar);
    if (key != "path" && key != "sha256" && key != "bytes" && key != "representation" && !result.metadata.contains(key)) {
      if(scalar.is_string()&&scalar.get_ref<const std::string&>().size()>8192){++result.boundaries;return;}
      result.metadata[key] = scalar;
    }
  }
  std::optional<Json> value(unsigned depth) {
    viewer_require(depth <= 512, "Evidence JSON nesting exceeds 512"); white(); auto c = input.peek();
    if (c == '{') {
      input.get(); white(); Json reference = Json::object(); if (input.peek() == '}') { input.get(); return std::nullopt; }
      while (true) {
        auto key = quoted(true); expect(':'); auto name = key ? text(*key) : "[oversize-key]"; route.push_back(name); auto scalar = value(depth + 1); route.pop_back();
        if (scalar && (name == "path" || name == "sha256" || name == "bytes" || name == "representation")) reference[name] = *scalar;
        white(); if (input.peek() == '}') { input.get(); break; } expect(',');
      }
      if (str(reference,"path").starts_with("blobs/sha256/") && std::regex_match(str(reference,"sha256"),std::regex("[a-f0-9]{64}")) && safe_uint(field(reference,"bytes"))) {
        if(str(reference,"representation").size()>128){reference["representation"]=prefix(str(reference,"representation"),128);++result.boundaries;}
        if (result.references.size() < 64) result.references.push_back(reference); else ++result.boundaries;
      }
      return std::nullopt;
    }
    if (c == '[') {
      input.get(); white(); if (input.peek() == ']') { input.get(); return std::nullopt; }
      while (true) { route.push_back("#"); value(depth + 1); route.pop_back(); white(); if (input.peek() == ']') { input.get(); break; } expect(','); } return std::nullopt;
    }
    bool selected = wanted(); std::optional<Json> scalar;
    if (c == '"') scalar = quoted(selected);
    else {
      std::string raw; while (input.peek() != EOF && std::string_view(",]} \t\r\n").find(char(input.peek())) == std::string_view::npos) { viewer_require(raw.size() < 1024, "Evidence scalar exceeds grammar budget"); raw += char(input.get()); }
      auto parsed = parse(raw); viewer_require(!parsed.is_structured() && !parsed.is_string(), "Invalid evidence scalar"); if (selected) scalar = parsed;
    }
    if (scalar) observe(*scalar); return scalar;
  }
};
template<class F> void ranges(const fs::path& file, F visit, const std::atomic_bool* stop) {
  std::ifstream input(file,std::ios::binary); viewer_require(bool(input),"Open evidence journal failed"); std::array<char,65536> buffer; uint64_t position=0,start=0,line=1;bool content=false;
  while(input){ input.read(buffer.data(),buffer.size());auto length=size_t(input.gcount());
    for(size_t i=0;i<length;++i,++position)if(buffer[i]=='\n'){if(content)visit(start,position-start,line);start=position+1;++line;content=false;}else if(buffer[i]!=' '&&buffer[i]!='\t'&&buffer[i]!='\r')content=true;
    if(stop&&stop->load())throw std::runtime_error("Evidence index cancelled; source capture retained");
  }
  viewer_require(input.eof(),"Read evidence journal failed"); if(content)visit(start,position-start,line);
}
}
struct EvidenceViewer::Impl {
  fs::path root; DiskIndex index; uint64_t records=0,boundaries=0; std::string generation=uuid();
  struct File {fs::path path;uint64_t bytes;int64_t modified;std::string hash;}; std::vector<File> files;
  Impl(const fs::path& capture,const Sink& progress,const std::atomic_bool* stop,const fs::path& index_owner):root(fs::canonical(capture)),index(index_owner.empty()?fs::temp_directory_path():index_owner) {
    viewer_require(fs::is_regular_file(root/"summary.json"),"Open a finalized capture summary before indexing");
    index.exec("CREATE TABLE records(id INTEGER PRIMARY KEY,file INTEGER,offset INTEGER,bytes INTEGER,line INTEGER,category BLOB,data BLOB);CREATE INDEX categories ON records(category,id);CREATE TABLE terms(row INTEGER,field BLOB,value BLOB,PRIMARY KEY(row,field,value)) WITHOUT ROWID;CREATE INDEX lookups ON terms(field,value,row)");
    std::vector<fs::path> journals;
    for(auto directory:{root,root/"events",root/"catalog"})if(fs::is_directory(directory))for(auto& entry:fs::directory_iterator(directory))if(entry.is_regular_file()&&(entry.path().extension()==".jsonl"||(directory==root&&entry.path().extension()==".json")))journals.push_back(entry.path());
    std::sort(journals.begin(),journals.end());viewer_require(journals.size()<=4096,"Viewer journal count exceeds explicit budget");
    for(const auto& journal:journals){
      auto canonical=fs::canonical(journal);viewer_require(inside(root,canonical),"Evidence journal leaves capture");auto file_id=files.size();files.push_back({canonical,fs::file_size(canonical),stamp(canonical),{}});auto& file=files.back();
      bool report=journal.extension()==".json";std::ifstream input(canonical,std::ios::binary);auto category=utf8(fs::relative(journal,root));category.resize(category.size()-(report?5:6));if(category.starts_with("events/"))category.erase(0,7);if(report)category="report/"+category;
      auto record=[&](uint64_t offset,uint64_t bytes,uint64_t line){
        RangeBuffer range(input,offset,bytes,stop);std::istream stream(&range);ProjectionReader reader(stream,category);auto projected=reader.run();
        auto row=projected.metadata;row["category"]=str(row,"category",category);row["kind"]=str(row,"kind",category);row["source"]=utf8(fs::relative(journal,root));row["line"]=line;row["bytes"]=bytes;
        row["preview"]=replacement_utf8(prefix(range.preview,2000));row["previewLimited"]=bytes>range.preview.size();row["indexBoundaryCount"]=projected.boundaries;row["blobRefs"]=projected.references;
        index.run("INSERT INTO records(file,offset,bytes,line,category,data) VALUES(?,?,?,?,?,?)",Json::array({file_id,offset,bytes,line,row["category"],row}));auto id=index.last_id();
        for(const auto& [field_name,value]:projected.terms)index.run("INSERT OR IGNORE INTO terms VALUES(?,?,?)",Json::array({id,field_name,value}));++records;boundaries+=projected.boundaries;
        if(progress&&records%1000==0)progress({{"type","viewer-progress"},{"records",records},{"source",row["source"]}});
      };
      if(report)record(0,file.bytes,1);else ranges(canonical,record,stop);
      unchanged(file);file.hash=file_sha256(canonical);unchanged(file);
    }
    index.checkpoint();
  }
  void unchanged(const File& file) const {viewer_require(fs::is_regular_file(file.path)&&fs::canonical(file.path)==file.path&&fs::file_size(file.path)==file.bytes&&stamp(file.path)==file.modified,"Evidence changed; rebuild the viewer index");}
  void stable() const {for(const auto& file:files)unchanged(file);}
  struct Range {fs::path file;uint64_t offset,length;std::optional<size_t> file_id;};
  Range selected(const Json& options) {
    viewer_require(!options.contains("generation")||str(options,"generation")==generation,"Stale viewer generation; select a row from the current index");
    if(options.contains("rowId")){
      auto id=decimal(text(options.at("rowId")));viewer_require(id&&id<=INT64_MAX,"Invalid viewer row identity");auto row=index.one("SELECT file,offset,bytes FROM records WHERE id=?",Json::array({id}));viewer_require(row.has_value(),"Viewer row not found");auto number=row->at(0).get<size_t>();unchanged(files.at(number));return {files[number].path,row->at(1).get<uint64_t>(),row->at(2).get<uint64_t>(),number};
    }
    auto ref=field(options,"reference");auto relative=path(str(ref,"path"));auto hash=str(ref,"sha256");viewer_require(std::regex_match(hash,std::regex("[a-f0-9]{64}"))&&utf8(relative)=="blobs/sha256/"+hash,"Select a real content-addressed blob reference");
    auto file=fs::canonical(root/relative);viewer_require(inside(root,file)&&safe_uint(field(ref,"bytes"))&&fs::file_size(file)==ref.at("bytes").get<uint64_t>()&&file_sha256(file)==hash,"Evidence blob identity/hash mismatch");return {file,0,fs::file_size(file),std::nullopt};
  }
};
EvidenceViewer::EvidenceViewer(const fs::path& capture,const Sink& progress,const std::atomic_bool* stop,const fs::path& index_owner):impl_(std::make_unique<Impl>(capture,progress,stop,index_owner)){}
EvidenceViewer::~EvidenceViewer()=default;
Json EvidenceViewer::stats() const {return {{"protocol","v8trace/viewer/1"},{"generation",impl_->generation},{"root",utf8(impl_->root)},{"records",impl_->records},{"files",impl_->files.size()},{"indexBoundaryCount",impl_->boundaries},{"sourceModified",false},{"index",impl_->index.stats()},{"ordering","stable-file-order-and-line; not invented global execution order"}};}
Json EvidenceViewer::query(const Json& options) {
  impl_->stable();auto limit=field(options,"limit",100);viewer_require(safe_uint(limit,true)&&limit<=200,"Viewer page limit must be 1 through 200");bool backward=str(options,"direction","forward")=="backward";viewer_require(backward||str(options,"direction","forward")=="forward","Invalid viewer page direction");auto after=decimal(str(options,"after",backward?std::to_string(INT64_MAX):"0"));viewer_require(after<=INT64_MAX,"Invalid viewer cursor");
  std::string sql=std::string("SELECT id,data,file FROM records r WHERE r.id")+(backward?"<?":">?");Json parameters=Json::array({after});
  if(!str(options,"category").empty()){sql+=" AND category=?";parameters.push_back(options.at("category"));}
  for(auto field_name:{"script","function","request","window","call","operation","activation","process","isolate","producer","session","context","url","identity","causalProducer","causalEpoch","causalTask","callExact","parentCall","httpResource"})if(!str(options,field_name).empty()){
    auto value=options.at(field_name);viewer_require(value.is_string()&&text(value).size()<=8192,"Invalid exact viewer filter");sql+=" AND EXISTS(SELECT 1 FROM terms t WHERE t.row=r.id AND t.field=? AND t.value=?)";parameters.push_back(field_name);parameters.push_back(value);
  }
  sql+=backward?" ORDER BY r.id DESC LIMIT ?":" ORDER BY r.id LIMIT ?";parameters.push_back(limit.get<uint64_t>()+1);Json rows=Json::array();bool more=false;std::string cursor=std::to_string(after);uint64_t response_bytes=0;
  impl_->index.each(sql,parameters,[&](const Json& item){if(more)return;auto encoded=text(item.at(1));if(rows.size()==limit.get<size_t>()||(!rows.empty()&&response_bytes+encoded.size()>2097152)){more=true;return;}auto row=parse(encoded);row["rowId"]=text(item.at(0));row["generation"]=impl_->generation;row["sourceFileSha256"]=impl_->files.at(item.at(2).get<size_t>()).hash;cursor=text(item.at(0));response_bytes+=encoded.size()+256;rows.push_back(row);});
  impl_->index.release_queries();
  return {{"protocol","v8trace/viewer/1"},{"generation",impl_->generation},{"rows",rows},{"nextCursor",cursor},{"hasMore",more},{"records",impl_->records},{"filteringStage","viewer-only; exact metadata, never producer filtering"},{"indexBoundaryCount",impl_->boundaries}};
}
Json EvidenceViewer::read(const Json& options) {
  auto range=impl_->selected(options);auto offset=field(options,"offset",0),limit=field(options,"limit",65536);viewer_require(safe_uint(offset)&&safe_uint(limit,true)&&limit<=65536&&offset.get<uint64_t>()<=range.length,"Invalid bounded evidence read");
  auto begin=offset.get<uint64_t>(),count=std::min(limit.get<uint64_t>(),range.length-begin);std::ifstream source(range.file,std::ios::binary);source.seekg(std::streamoff(range.offset+begin));std::string bytes(size_t(count),'\0');source.read(bytes.data(),std::streamsize(count));viewer_require(uint64_t(source.gcount())==count,"Short evidence read");
  return {{"generation",impl_->generation},{"offset",begin},{"bytes",count},{"totalBytes",range.length},{"base64",base64(bytes)},{"nextOffset",begin+count},{"hasMore",begin+count<range.length},{"displayEncoding","raw bytes; UTF-8 preview may replace boundary code units; base64 is exact"}};
}
Json EvidenceViewer::copy(const Json& options) {
  auto range=impl_->selected(options);auto target=fs::absolute(path(str(options,"output"))).lexically_normal();viewer_require(!str(options,"output").empty()&&!inside(impl_->root,fs::weakly_canonical(target))&&!fs::exists(target),"Save full evidence to a new file outside the capture");
  if(range.file_id)viewer_require(file_sha256(range.file)==impl_->files.at(*range.file_id).hash,"Evidence file hash changed; rebuild index");
  fs::create_directories(target.parent_path());auto temporary=target;temporary+=".partial-"+uuid();write_file(temporary,"",true);
  try {
    std::ifstream source(range.file,std::ios::binary);std::ofstream output(temporary,std::ios::binary|std::ios::app);source.seekg(std::streamoff(range.offset));std::array<char,65536> buffer;uint64_t remaining=range.length;
    while(remaining){auto count=std::min<uint64_t>(remaining,buffer.size());source.read(buffer.data(),std::streamsize(count));viewer_require(uint64_t(source.gcount())==count,"Short full evidence copy");output.write(buffer.data(),std::streamsize(count));viewer_require(bool(output),"Write full evidence failed");remaining-=count;}output.flush();viewer_require(bool(output),"Flush full evidence failed");output.close();if(range.file_id)impl_->unchanged(impl_->files.at(*range.file_id));
    auto hash=file_sha256(temporary);std::error_code error;fs::create_hard_link(temporary,target,error);if(error)fs::copy_file(temporary,target,fs::copy_options::none);fs::remove(temporary);return {{"output",utf8(target)},{"bytes",range.length},{"sha256",hash},{"sourceModified",false}};
  }catch(...){std::error_code ignored;fs::remove(temporary,ignored);throw;}
}
}
