#pragma once
#include "v8trace/core.h"

namespace v8trace {
Json supported_options();
Json load_configuration(const fs::path& file);
Json configuration_options(const Json& document);
Json configuration_document(const Json& options);
Json configuration_preview(const Json& current, const Json& incoming, std::string_view mode);
Json preflight_capture(const Json& options, const fs::path& assets, bool verify_browser = true);
Json event_preview(const Json& event, size_t maximum = 16000);

class EvidenceViewer {
 public:
  explicit EvidenceViewer(const fs::path& capture, const Sink& progress = {}, const std::atomic_bool* stop = nullptr, const fs::path& index_owner = {});
  ~EvidenceViewer();
  EvidenceViewer(const EvidenceViewer&) = delete;
  EvidenceViewer& operator=(const EvidenceViewer&) = delete;
  Json query(const Json& options);
  Json read(const Json& options);
  Json copy(const Json& options);
  Json stats() const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
