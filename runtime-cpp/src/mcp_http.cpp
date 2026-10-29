#include "v8trace/mcp.h"
#include "trace_search_internal.h"
#include <ixwebsocket/IXSocket.h>
#include <ixwebsocket/IXSocketConnect.h>
#include <ixwebsocket/IXNetSystem.h>
#include <algorithm>
#include <array>
#include <future>
#include <iostream>
#include <limits>
#include <sstream>

namespace v8trace {
using trace_search::require;
namespace {
using Clock=std::chrono::steady_clock;
struct HttpFailure : std::runtime_error {int status;HttpFailure(int code,std::string message):std::runtime_error(std::move(message)),status(code){}};
void http_require(bool condition,int status,std::string message){if(!condition)throw HttpFailure(status,std::move(message));}
bool same_token(std::string_view expected,std::string_view received){unsigned difference=unsigned(expected.size()^received.size());for(size_t n=0;n<expected.size();++n)difference|=uint8_t(expected[n])^uint8_t(n<received.size()?received[n]:0);return difference==0;}
bool write(ix::Socket& socket,std::string_view value,const std::atomic_bool& stop){
  auto deadline=Clock::now()+std::chrono::seconds(30);size_t sent=0;
  while(sent<value.size()){
    if(stop||Clock::now()>deadline)return false;
    auto count=socket.send(const_cast<char*>(value.data()+sent),std::min<size_t>(65536,value.size()-sent));
    if(count>0)sent+=size_t(count);
    else if(count==0||!ix::Socket::isWaitNeeded())return false;
    else {
      // Windows FD_WRITE is edge-triggered: wait only after send reports backpressure.
      auto ready=socket.isReadyToWrite(50);if(ready!=ix::PollResultType::Timeout&&ready!=ix::PollResultType::ReadyForWrite)return false;
    }
  }
  return true;
}
std::string reason(int status){switch(status){case 200:return "OK";case 202:return "Accepted";case 400:return "Bad Request";case 401:return "Unauthorized";case 403:return "Forbidden";case 404:return "Not Found";case 405:return "Method Not Allowed";case 406:return "Not Acceptable";case 408:return "Request Timeout";case 413:return "Content Too Large";case 415:return "Unsupported Media Type";case 429:return "Too Many Requests";default:return "Internal Server Error";}}
bool response(ix::Socket& socket,int status,std::string_view body,const std::atomic_bool& stop,std::string extra=""){
  auto header="HTTP/1.1 "+std::to_string(status)+" "+reason(status)+"\r\nConnection: close\r\nCache-Control: no-store\r\nContent-Type: application/json; charset=utf-8\r\nX-Content-Type-Options: nosniff\r\nContent-Length: "+std::to_string(body.size())+"\r\n"+extra+"\r\n";
  return write(socket,header,stop)&&write(socket,body,stop);
}
void drain_rejected(ix::Socket& socket,const std::atomic_bool& stop){
  // Bounded draining lets Windows deliver an early error instead of resetting over unread body bytes.
  auto deadline=Clock::now()+std::chrono::milliseconds(50);size_t drained=0;std::array<char,4096> buffer{};
  while(!stop&&Clock::now()<deadline&&drained<65536){auto ready=socket.isReadyToRead(5);if(ready==ix::PollResultType::Timeout)continue;if(ready!=ix::PollResultType::ReadyForRead)return;auto count=socket.recv(buffer.data(),buffer.size());if(count>0)drained+=size_t(count);else if(count==0||!ix::Socket::isWaitNeeded())return;}
}
struct Request {std::string method,target,body;std::map<std::string,std::string> headers;};
Request request(ix::Socket& socket,uint64_t maximum,const std::atomic_bool& stop){
  std::string bytes;std::array<char,4096> buffer{};auto deadline=Clock::now()+std::chrono::seconds(15);size_t boundary=std::string::npos;uint64_t wanted=0;Request result;
  while(true){
    http_require(!stop&&Clock::now()<deadline,408,"HTTP request timed out or server is stopping");
    if(boundary!=std::string::npos&&bytes.size()>=wanted)break;
    auto ready=socket.isReadyToRead(50);if(ready==ix::PollResultType::Timeout)continue;http_require(ready==ix::PollResultType::ReadyForRead,400,"HTTP socket read failed");auto count=socket.recv(buffer.data(),buffer.size());
    if(count<0&&ix::Socket::isWaitNeeded())continue;http_require(count>0,400,"HTTP client disconnected before complete request");bytes.append(buffer.data(),size_t(count));
    http_require(bytes.size()<=maximum+32768,413,"HTTP request exceeds byte budget");
    if(boundary==std::string::npos){
      boundary=bytes.find("\r\n\r\n");http_require(boundary==std::string::npos?bytes.size()<=32768:boundary+4<=32768,413,"HTTP headers exceed byte budget");if(boundary==std::string::npos)continue;
      std::istringstream input(bytes.substr(0,boundary));std::string first,protocol,extra;std::getline(input,first);if(!first.empty()&&first.back()=='\r')first.pop_back();std::istringstream fields(first);fields>>result.method>>result.target>>protocol;http_require(protocol=="HTTP/1.1"&&!(fields>>extra),400,"Invalid HTTP request line");
      for(std::string line;std::getline(input,line);){if(!line.empty()&&line.back()=='\r')line.pop_back();auto colon=line.find(':');http_require(colon!=std::string::npos&&colon>0,400,"Malformed HTTP header");auto name=line.substr(0,colon);for(unsigned char c:name)http_require(std::isalnum(c)||std::string_view("!#$%&'*+-.^_`|~").find(char(c))!=std::string_view::npos,400,"Invalid HTTP header name");name=lower(name);auto value=trim(line.substr(colon+1));for(unsigned char c:value)http_require(c=='\t'||(c>=32&&c!=127),400,"Invalid HTTP header value");http_require(!result.headers.contains(name),400,"Duplicate HTTP headers are not accepted");result.headers[name]=std::move(value);}
      http_require(!result.headers.contains("transfer-encoding"),400,"Transfer-Encoding is not accepted; supply a bounded Content-Length");http_require(!result.headers.contains("expect"),400,"Expect header is not accepted");
      if(result.headers.contains("content-length")){const auto& length=result.headers.at("content-length");http_require(!length.empty()&&std::all_of(length.begin(),length.end(),[](unsigned char c){return c>='0'&&c<='9';}),400,"Invalid Content-Length");try{wanted=decimal(length);}catch(...){throw HttpFailure(413,"Content-Length exceeds budget");}}
      else http_require(result.method!="POST",400,"POST requires Content-Length");http_require(wanted<=maximum,413,"HTTP body exceeds byte budget");wanted+=boundary+4;
    }
  }
  http_require(bytes.size()==wanted,400,"HTTP pipelining or excess body bytes are not accepted");result.body=bytes.substr(boundary+4);return result;
}
bool accepts(std::string header,std::string_view type){for(auto part:split(lower(header),',')){auto semi=part.find(';');if(trim(part.substr(0,semi))==type)return true;}return false;}
class Server final {
 public:
  Server(McpServer& service,std::atomic_bool& stop,std::string token):service_(service),stop_(stop),token_(std::move(token)){}
  ~Server(){stop();}
  std::pair<bool,std::string> listen(){
    try{
      auto native=::socket(AF_INET,SOCK_STREAM,0);require(native!=ix::socket_t(-1),"Create MCP listener failed");if(uint64_t(native)>uint64_t(std::numeric_limits<int>::max())){
#ifdef _WIN32
        closesocket(native);
#else
        ::close(native);
#endif
        throw std::runtime_error("MCP socket handle exceeds transport range");
      }
      listener_=int(native);int enabled=1;
#ifdef _WIN32
      require(setsockopt(listener_,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,reinterpret_cast<const char*>(&enabled),sizeof(enabled))==0,"MCP listener requires exclusive address ownership");
#else
      require(setsockopt(listener_,SOL_SOCKET,SO_REUSEADDR,&enabled,sizeof(enabled))==0,"Configure MCP listener failed");
#endif
      sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(uint16_t(service_.config().port));require(ix::inet_pton(AF_INET,service_.config().bind.c_str(),&address.sin_addr)==1,"Invalid MCP listener address");require(::bind(listener_,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0,"MCP port is already in use or bind failed");require(::listen(listener_,16)==0,"Listen for MCP failed");ix::SocketConnect::configure(listener_);return {true,{}};
    }catch(const std::exception& error){return {false,error.what()};}
  }
  void run(){
    while(!stop_){
      for(auto it=connections_.begin();it!=connections_.end();)if((*it)->done){(*it)->thread.join();it=connections_.erase(it);}else++it;
      auto ready=ix::Socket::poll(true,50,listener_,nullptr);if(ready==ix::PollResultType::Timeout)continue;require(ready==ix::PollResultType::ReadyForRead,"MCP listener poll failed");auto native=::accept(listener_,nullptr,nullptr);if(native==ix::socket_t(-1)){require(ix::Socket::isWaitNeeded(),"MCP accept failed");continue;}
      if(uint64_t(native)>uint64_t(std::numeric_limits<int>::max())){
#ifdef _WIN32
        closesocket(native);
#else
        ::close(native);
#endif
        continue;
      }
      auto socket=std::make_unique<ix::Socket>(int(native));if(connections_.size()>=service_.config().connections)continue;std::string error;if(!socket->init(error))continue;ix::SocketConnect::configure(int(native));auto connection=std::make_shared<Connection>();connections_.push_back(connection);
      try{connection->thread=std::thread([this,connection,socket=std::move(socket)]()mutable{handleConnection(std::move(socket));connection->done=true;});}catch(...){connections_.pop_back();throw;}
    }
  }
  void stop(){stop_=true;if(listener_!=-1){ix::Socket::closeSocket(listener_);listener_=-1;}for(auto& connection:connections_)if(connection->thread.joinable())connection->thread.join();connections_.clear();}
 private:
  struct Connection{std::atomic_bool done{false};std::thread thread;};
  McpServer& service_;std::atomic_bool& stop_;std::string token_;int listener_=-1;std::vector<std::shared_ptr<Connection>> connections_;
  void check_access(const Request& request){
    auto authority=service_.config().bind+":"+std::to_string(service_.config().port);auto host=request.headers.find("host");http_require(host!=request.headers.end()&&(lower(host->second)==authority||(service_.config().bind=="127.0.0.1"&&lower(host->second)=="localhost:"+std::to_string(service_.config().port))),403,"HTTP Host is not allowed");
    if(auto origin=request.headers.find("origin");origin!=request.headers.end())http_require(origin->second=="http://"+authority||(service_.config().bind=="127.0.0.1"&&origin->second=="http://localhost:"+std::to_string(service_.config().port)),403,"HTTP Origin is not allowed");
    auto authorization=request.headers.find("authorization");http_require(authorization!=request.headers.end()&&authorization->second.size()>=7&&lower(authorization->second.substr(0,7))=="bearer "&&same_token(token_,authorization->second.substr(7)),401,"Bearer authentication required");
  }
  void handleConnection(std::unique_ptr<ix::Socket> socket){
    auto cancel=std::make_shared<std::atomic_bool>(false);std::future<McpReply> job;bool stream=false,rejected=false;
    try{
      auto incoming=request(*socket,service_.config().max_request_bytes,stop_);check_access(incoming);
      if(incoming.target=="/healthz"&&incoming.method=="GET"){response(*socket,200,dump(service_.info()),stop_);}
      else if(incoming.target=="/shutdown"&&incoming.method=="POST"){response(*socket,200,"{\"stopping\":true}",stop_);stop_=true;}
      else{
        http_require(incoming.target=="/mcp",404,"Endpoint not found");http_require(incoming.method=="POST",405,"Use POST /mcp");http_require(accepts(incoming.headers["content-type"],"application/json"),415,"Content-Type must be application/json");http_require(accepts(incoming.headers["accept"],"application/json")&&accepts(incoming.headers["accept"],"text/event-stream"),406,"Accept must include application/json and text/event-stream");
        Json message;try{message=parse(incoming.body);}catch(...){auto reply=mcp_error(nullptr,-32700,"Parse error");response(*socket,400,dump(reply),stop_);throw HttpFailure(0,"");}
        McpContext context;context.http=true;context.headers=std::move(incoming.headers);
        if(str(message,"method")!="tools/call"){auto reply=service_.dispatch(message,context,cancel);auto wire=service_.encode(reply);response(*socket,reply.status,wire,stop_);}
        else{
          auto validated=service_.dispatch(message,context,cancel,true);if(validated.body){auto wire=service_.encode(validated);response(*socket,validated.status,wire,stop_);}
          else{
            job=std::async(std::launch::async,[&,message,context]()mutable{return service_.dispatch(message,context,cancel);});
            require(write(*socket,"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-store\r\nConnection: close\r\nX-Content-Type-Options: nosniff\r\n\r\n",stop_),"MCP stream disconnected");stream=true;auto heartbeat=Clock::now();
            while(job.wait_for(std::chrono::milliseconds(50))!=std::future_status::ready){
              require(!stop_,"MCP server is stopping");auto ready=socket->isReadyToRead(0);if(ready==ix::PollResultType::ReadyForRead){char byte;auto count=socket->recv(&byte,1);require(count<0&&ix::Socket::isWaitNeeded(),"MCP response stream closed or unexpected pipelined data");}else require(ready==ix::PollResultType::Timeout,"MCP response stream disconnected");
              if(Clock::now()-heartbeat>=std::chrono::seconds(1)){require(write(*socket,": keepalive\n\n",stop_),"MCP response stream disconnected");heartbeat=Clock::now();}
            }
            auto reply=job.get();auto wire=service_.encode(reply);write(*socket,"event: message\ndata: "+wire+"\n\n",stop_);
          }
        }
      }
    }catch(const HttpFailure& error){*cancel=true;if(error.status&&!stream){rejected=true;response(*socket,error.status,dump(mcp_error(nullptr,-32600,error.what())),stop_,error.status==401?"WWW-Authenticate: Bearer realm=\"v8trace-mcp\"\r\n":error.status==405?"Allow: POST\r\n":"");}}
    catch(const std::exception&){*cancel=true;if(!stream)response(*socket,500,dump(mcp_error(nullptr,-32603,"MCP transport failed")),stop_);}
    *cancel=true;if(job.valid())job.wait();if(rejected)drain_rejected(*socket,stop_);socket->close();
  }
};
}
int mcp_http(McpServer& service,std::atomic_bool& stop){
  require(!service.config().token_file.empty(),"HTTP MCP requires --token-file (stdio needs no token)");auto token=read_file(service.config().token_file,4096);if(token.starts_with("\xef\xbb\xbf"))token.erase(0,3);token=trim(token);require(token.size()>=24&&token.size()<=256&&std::all_of(token.begin(),token.end(),[](unsigned char c){return c>=33&&c<=126;}),"MCP token must contain 24 through 256 printable ASCII bytes");
  auto parts=split(service.config().bind,'.');require(parts.size()==4&&service.config().bind!="0.0.0.0","Bind a specific IPv4 address; default is 127.0.0.1");for(auto part:parts)require(!part.empty()&&decimal(part)<=255,"Invalid IPv4 bind address");
  require(ix::initNetSystem(),"Initialize MCP sockets failed");
  {Server server(service,stop,std::move(token));auto listening=server.listen();require(listening.first,"MCP listen failed: "+listening.second);auto info=service.info();info["type"]="mcp-ready";info["endpoint"]="http://"+service.config().bind+":"+std::to_string(service.config().port)+"/mcp";std::cerr<<dump(info)<<'\n';std::cerr.flush();server.run();}
  ix::uninitNetSystem();return 0;
}
}
