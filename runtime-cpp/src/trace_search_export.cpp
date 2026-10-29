#include "trace_search_internal.h"
#include <algorithm>
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace v8trace::trace_search {
namespace {
class Temporary {
 public:
  explicit Temporary(fs::path file) : file_(std::move(file)) {
#ifdef _WIN32
    handle_ = CreateFileW(file_.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr); require(handle_!=INVALID_HANDLE_VALUE,"Create private export temporary failed");
#else
    fd_ = ::open(file_.c_str(),O_CREAT|O_EXCL|O_WRONLY|O_NOFOLLOW,0600); require(fd_>=0,"Create private export temporary failed");
#endif
  }
  ~Temporary() { close(); std::error_code error; fs::remove(file_,error); }
  void write(const char* bytes,size_t length) {
    while (length) {
#ifdef _WIN32
      DWORD count=0; require(WriteFile(handle_,bytes,DWORD(std::min<size_t>(length,1048576)),&count,nullptr)&&count,"Export write failed");
#else
      auto count=::write(fd_,bytes,length); if(count<0&&errno==EINTR)continue; require(count>0,"Export write failed");
#endif
      bytes+=count; length-=size_t(count);
    }
  }
  void finish() {
#ifdef _WIN32
    require(FlushFileBuffers(handle_)!=0,"Export synchronization failed");
#else
    require(::fsync(fd_)==0,"Export synchronization failed");
#endif
    close();
  }
  const fs::path& file() const { return file_; }
 private:
  void close() {
#ifdef _WIN32
    if(handle_!=INVALID_HANDLE_VALUE){CloseHandle(handle_);handle_=INVALID_HANDLE_VALUE;}
#else
    if(fd_>=0){::close(fd_);fd_=-1;}
#endif
  }
  fs::path file_;
#ifdef _WIN32
  HANDLE handle_=INVALID_HANDLE_VALUE;
#else
  int fd_=-1;
#endif
};
void validate_destination(const fs::path& destination,const fs::path& parent,const TraceSearchConfig& config,const Source& source,bool overwrite) {
  require(fs::canonical(destination.parent_path())==parent && (fs::equivalent(config.export_root,parent) || inside(config.export_root,parent)),"Export parent changed or leaves configured root");
  require(!fs::is_symlink(fs::symlink_status(destination)),"Export destination must not be a symbolic link");
  if(fs::exists(destination)) {
    require(overwrite && fs::is_regular_file(destination),"Export destination exists or is not regular");
    require(inside(config.export_root,fs::canonical(destination)) && !fs::equivalent(destination,source.file),"Export destination overlaps source or leaves root");
  }
}
}
Json export_index(const LineIndex& index,const TraceSearchConfig& config,const Json& request,const std::atomic_bool* stop) {
  auto start=number(request,"start_line"),count=number(request,"line_count");
  require(start && start-1<=index.line_count,"Line is outside the file; EOF is line_count + 1");
  require(!config.max_export_lines || count<=config.max_export_lines,"Export line count exceeds configured limit");
  require(field(request,"output_path").is_string()&&!str(request,"output_path").empty(),"An output_path string is required");
  require(str(request,"output_path").find('\0')==std::string::npos,"Export path contains NUL");
  bool overwrite=false; if(request.contains("overwrite")){require(request["overwrite"].is_boolean(),"Expected boolean: overwrite");overwrite=request["overwrite"].get<bool>();}
  auto destination=path(str(request,"output_path")); if(!destination.is_absolute())destination=config.export_root/destination;
  require(!destination.filename().empty() && destination.filename()!="." && destination.filename()!="..","Invalid export filename");
#ifdef _WIN32
  auto filename=utf8(destination.filename());require(filename.find_first_of(":*?\"<>|") == std::string::npos && filename.back()!='.' && filename.back()!=' ',"Invalid Windows export filename or alternate stream");
  auto stem=lower(trim(filename.substr(0,filename.find('.'))));const std::set<std::string> devices{"con","prn","aux","nul","conin$","conout$"};bool reserved=devices.contains(stem);
  if(stem.starts_with("com")||stem.starts_with("lpt")){auto number=stem.substr(3);reserved=reserved||(number.size()==1&&number[0]>='1'&&number[0]<='9')||number=="¹"||number=="²"||number=="³";}require(!reserved,"Reserved Windows export filename");
#endif
  auto parent=fs::canonical(destination.parent_path()); destination=parent/destination.filename(); validate_destination(destination,parent,config,index.source,overwrite);
  auto selected=std::min(count,index.line_count-(start-1)); auto begin=index.offset(start,stop),end=index.offset(start+selected,stop); require(end>=begin,"Invalid export span");
  index.source.verify(); std::ifstream input(index.source.file,std::ios::binary); require(bool(input),"Open export source failed");input.seekg(std::streamoff(begin));require(bool(input),"Seek export source failed");
  Temporary temporary(parent/(".v8trace-export-"+uuid()+".tmp")); std::vector<char> buffer(8*1024*1024); uint64_t copied=0;
  while(copied<end-begin){cancelled(stop);auto length=size_t(std::min<uint64_t>(buffer.size(),end-begin-copied));input.read(buffer.data(),std::streamsize(length));require(size_t(input.gcount())==length,"Source changed or export read failed");temporary.write(buffer.data(),length);copied+=length;}
  cancelled(stop);index.source.verify();temporary.finish();validate_destination(destination,parent,config,index.source,overwrite);cancelled(stop);publish(temporary.file(),destination,overwrite);
  return {{"start_line",start},{"end_line",selected?Json(start+selected-1):Json(nullptr)},{"exported_lines",selected},{"bytes_written",copied},{"output_path",utf8(destination)},{"eof",start+selected>index.line_count}};
}
}
