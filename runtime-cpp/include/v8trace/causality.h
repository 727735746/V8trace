#pragma once
#include "v8trace/core.h"

namespace v8trace {
class CausalityEngine {
 public:
  CausalityEngine(fs::path capture, fs::path index_directory, Sink progress = {}, std::atomic_bool* cancel = nullptr);
  ~CausalityEngine();
  CausalityEngine(const CausalityEngine&) = delete;
  CausalityEngine& operator=(const CausalityEngine&) = delete;
  Json stats(std::atomic_bool* cancel = nullptr) const;
  Json roots(const Json& options = Json::object(), std::atomic_bool* cancel = nullptr);
  Json root(const Json& options, std::atomic_bool* cancel = nullptr);
  Json nodes(const Json& options, std::atomic_bool* cancel = nullptr);
  Json checkpoints(const Json& options, std::atomic_bool* cancel = nullptr);
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
