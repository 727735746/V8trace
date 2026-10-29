#pragma once
#include "v8trace/trace_search.h"
#include <map>

namespace v8trace {
struct McpConfig {
  TraceSearchConfig search;
  fs::path token_file;
  std::string bind = "127.0.0.1";
  unsigned port = 17881, connections = 16;
  uint64_t max_request_bytes = 1048576, max_response_bytes = 33554432;
};
struct McpContext {
  bool http = false;
  std::string legacy_version;
  std::map<std::string,std::string> headers;
};
struct McpReply { int status = 200; std::optional<Json> body; };
class McpServer {
 public:
  explicit McpServer(McpConfig config);
  ~McpServer();
  McpReply dispatch(const Json& request, McpContext& context, const std::shared_ptr<std::atomic_bool>& cancel, bool validate_only = false);
  std::string encode(McpReply& reply) const;
  const McpConfig& config() const;
  Json info() const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
Json mcp_error(const Json& id,int code,std::string message,Json data=nullptr);
int mcp_stdio(McpServer& server,std::atomic_bool& stop);
int mcp_http(McpServer& server,std::atomic_bool& stop);
int run_mcp(const Json& flags,std::atomic_bool& stop);
}
