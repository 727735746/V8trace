#pragma once
#include "v8trace/core.h"

namespace v8trace {
class DiskIndex {
 public:
  explicit DiskIndex(const fs::path& owner);
  ~DiskIndex();
  DiskIndex(const DiskIndex&) = delete;
  DiskIndex& operator=(const DiskIndex&) = delete;
  void exec(std::string_view sql);
  void run(std::string_view sql, const Json& parameters = Json::array());
  void each(std::string_view sql, const Json& parameters, const Sink& visit);
  std::optional<Json> one(std::string_view sql, const Json& parameters = Json::array());
  int64_t last_id() const;
  int64_t changes() const;
  void checkpoint();
  void release_queries();
  Json stats() const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class JsonArrayOutput {
 public:
  explicit JsonArrayOutput(const fs::path& file, std::string_view prefix = "[");
  ~JsonArrayOutput();
  void append(const Json& value);
  void raw_item(std::string_view value);
  void begin_item();
  void fragment(std::string_view value);
  void finish(std::string_view suffix = "]\n");
  uint64_t count() const { return count_; }
 private:
  std::ofstream output_;
  uint64_t count_ = 0;
  bool finished_ = false;
};
}
