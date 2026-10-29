#pragma once
#include "v8trace/core.h"
namespace v8trace {
class ChunkSpool {
 public:
  ChunkSpool(const fs::path& directory, const std::string& capture_id, const Json& options = {}, Sink progress = {});
  ~ChunkSpool();
  ChunkSpool(const ChunkSpool&) = delete;
  ChunkSpool& operator=(const ChunkSpool&) = delete;
  void append(std::string_view relative, std::string_view bytes);
  void flush();
  Json seal(const fs::path& capture);
  Json status() const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
Json stream_pack(const fs::path& capture, const fs::path& spool);
Json stream_unpack(const fs::path& spool, const fs::path& output, const std::string& stream_id = "");
Json stream_send(const fs::path& spool, const Json& options, const Sink& progress = {}, const std::atomic_bool* stop = nullptr);
Json stream_decode(const fs::path& chunk, const fs::path& output);
}
