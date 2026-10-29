#include "v8trace/core.h"
#include "v8trace/edition.h"
#include "v8trace/journal.h"
#include "v8trace/stream.h"

namespace v8trace {
struct Store::Journal {
  fs::path root;
  std::set<std::string> validated_paths;
  std::map<std::string, std::ofstream> handles;
  uint64_t gap_summary_bytes = 0;
  JournalWriter writer;
  Journal(fs::path directory, uint64_t capacity, uint64_t batch, ChunkSpool* spool)
      : root(std::move(directory)), writer(capacity, batch, [this, spool](std::string_view relative, std::string_view data) {
        auto key = std::string(relative); auto it = handles.find(key);
        if (it == handles.end()) {
          if (handles.size() >= 32) { auto old = handles.begin(); old->second.flush(); if (!old->second) throw std::runtime_error("Capture journal eviction flush failed: " + old->first); handles.erase(old); }
          it = handles.emplace(key, std::ofstream(root / path(key), std::ios::binary | std::ios::app)).first;
          if (!it->second) throw std::runtime_error("Open capture journal failed: " + key);
        }
        it->second.write(data.data(), std::streamsize(data.size())); if (!it->second) throw std::runtime_error("Capture append failed: " + key);
        if (spool) spool->append(relative, data);
      }, [this, spool] { for (auto& [key, handle] : handles) { handle.flush(); if (!handle) throw std::runtime_error("Capture flush failed: " + key); } if (spool) spool->flush(); }) {}
};
Store::Store(const fs::path& output, const Json& options, Sink observer) : root(fs::absolute(output)), capture_id(uuid()),
  maximum_bytes_(options.value("maxBytes", 268435456ULL)), maximum_events_(options.value("maxEvents", 100000ULL)), observer_(std::move(observer)) {
  if (!fs::create_directory(root)) { if (fs::exists(root)) throw std::runtime_error("Capture output already exists: " + utf8(root)); throw std::runtime_error("Create capture directory failed"); }
  root = fs::canonical(root);
  for (const auto& name : field(options, "categories", Json::array())) categories_.insert(text(name));
  if (field(options, "chunkSpool", false) == true) { auto directory = path(str(options, "streamSpoolPath", utf8(root) + ".stream")); if (inside(root, fs::absolute(directory)) || inside(fs::absolute(directory), root)) throw std::runtime_error("Chunk spool must stay outside capture"); stream_ = std::make_unique<ChunkSpool>(directory, capture_id, options); }
  journal_ = std::make_unique<Journal>(root, options.value("writeBufferBytes", 8388608ULL), options.value("writeBatchBytes", 1048576ULL), stream_.get());
  write_file(filename("gaps.jsonl"), "", true);
}
Store::~Store() = default;
fs::path Store::filename(std::string_view relative) const { auto file = (root / path(relative)).lexically_normal(); if (!inside(root, file)) throw std::runtime_error("Capture path leaves output directory"); fs::create_directories(file.parent_path()); return file; }
bool Store::reserve(uint64_t length) { if (!accepting) return false; if (length > maximum_bytes_ - bytes) { accepting = false; gap("capture-byte-budget", {{"maxBytes", maximum_bytes_}, {"nextBytes", length}}); return false; } bytes += length; return true; }
bool Store::append(std::string_view relative, const Json& value) {
  auto data = dump(value) + "\n"; if (!reserve(data.size())) return false;
  auto key = std::string(relative);
  if (!journal_->validated_paths.contains(key)) { filename(relative); if (journal_->validated_paths.size() >= 64) journal_->validated_paths.erase(journal_->validated_paths.begin()); journal_->validated_paths.insert(key); }
  journal_->writer.append(std::move(key), std::move(data)); return true;
}
bool Store::event(std::string_view category, std::string_view kind, const Json& payload, Json context) {
  if (!categories_.empty() && !categories_.contains(std::string(category))) return false;
  if (!accepting) return false;
  if (sequence >= maximum_events_) { accepting = false; gap("capture-event-budget", {{"maxEvents", maximum_events_}}); return false; }
  Json record{{"schema", "v8trace/event/1"}, {"captureId", capture_id}, {"seq", sequence + 1}, {"observedAt", now()}, {"category", category}, {"kind", kind}};
  record.update(context); record["payload"] = payload;
  if (!append("events/" + std::string(category) + ".jsonl", record)) return false;
  ++sequence; auto key = std::string(category); counts[key] = counts.value(key, 0ULL) + 1;
  if (observer_) observer_(record); return true;
}
Json Store::blob(std::string_view data, std::string_view representation) { auto digest = sha256(data); auto relative = "blobs/sha256/" + digest;
  if (!blobs_.contains(digest)) { if (!reserve(data.size())) return nullptr; write_file(filename(relative), data, true); blobs_.insert(digest); }
  return {{"path", relative}, {"sha256", digest}, {"bytes", data.size()}, {"representation", representation}};
}
Json Store::blob_file(const fs::path& file, std::string_view representation) { if (!inside(root, file)) throw std::runtime_error("Raw capture leaves output"); auto digest = file_sha256(file); auto relative = "blobs/sha256/" + digest; auto length = fs::file_size(file);
  if (!blobs_.contains(digest)) { if (!reserve(length)) return nullptr; auto target = filename(relative); std::error_code error;
    if (fs::exists(target) && file_sha256(target) != digest) throw std::runtime_error("Existing blob content differs from SHA256");
    if (representation == "chrome-native-startup-trace-json" || representation == "chrome-native-netlog-everything-json") fs::create_hard_link(file, target, error);
    if (!fs::exists(target)) fs::copy_file(file, target); blobs_.insert(digest); }
  return {{"path", relative}, {"sha256", digest}, {"bytes", length}, {"representation", representation}};
}
void Store::json(std::string_view relative, const Json& value) { write_json(filename(relative), value); }
void Store::gap(std::string_view reason, Json details, bool critical) {
  Json record{{"gapSeq", gap_records_ + 1}, {"reason", reason}, {"critical", critical}, {"details", std::move(details)}, {"observedAt", now()}};
  auto data = dump(record) + "\n";
  auto length = data.size();
  journal_->writer.append("gaps.jsonl", std::move(data));
  ++gap_records_; diagnostic_bytes_ += length;
  if (critical) ++critical_gaps;
  if (gaps.size() < 200 && length <= 4194304 - journal_->gap_summary_bytes) { journal_->gap_summary_bytes += length; gaps.push_back(std::move(record)); }
  else ++omitted_gaps;
}
Json Store::progress(std::string_view phase) const { Json value{{"phase", phase}, {"output", utf8(root)}, {"captureId", capture_id}, {"counts", counts}, {"eventCount", sequence}, {"recordedBytes", bytes}, {"criticalGapCount", critical_gaps}, {"gapCount", gaps.size() + omitted_gaps}}; if (stream_) value["chunkStream"] = stream_->status(); return value; }
void Store::flush() { journal_->writer.flush(); }
Json Store::finalize(Json record) {
  record["edition"] = edition_info();
  accepting = false; journal_->writer.finish(); journal_->handles.clear(); auto stats = journal_->writer.stats();
  Json writer{{"protocol", "v8trace/journal-writer/1"}, {"boundary", "host-after-value-serialization"}, {"policy", "bounded-blocking-no-drop"}, {"capacityBytes", stats.capacity_bytes}, {"batchBytes", stats.batch_bytes}, {"maximumPendingRows", stats.maximum_pending_rows}, {"enqueuedRows", stats.enqueued_rows}, {"writtenRows", stats.written_rows}, {"enqueuedBytes", stats.enqueued_bytes}, {"writtenBytes", stats.written_bytes}, {"writeBatches", stats.write_batches}, {"flushes", stats.flushes}, {"backpressureWaits", stats.backpressure_waits}, {"oversizedRows", stats.oversized_rows}, {"highWaterBytes", stats.high_water_bytes}, {"highWaterRows", stats.high_water_rows}, {"pendingBytes", stats.pending_bytes}, {"drained", stats.enqueued_rows == stats.written_rows && stats.pending_bytes == 0}, {"durability", "stream-flush-not-power-loss-fsync"}};
  Json gap_journal{{"protocol", "v8trace/gaps/1"}, {"path", "gaps.jsonl"}, {"records", gap_records_}, {"bytes", diagnostic_bytes_}, {"sha256", file_sha256(filename("gaps.jsonl"))}, {"summaryLimit", 200}, {"summaryByteBudget", 4194304}, {"summaryBytes", journal_->gap_summary_bytes}, {"outsideCaptureBudget", true}, {"drained", writer.at("drained")}};
  Json summary{{"schemaVersion", 1}, {"captureId", capture_id}}; summary.update(record); summary.update({{"counts", counts}, {"eventCount", sequence}, {"recordedBytes", bytes}, {"diagnosticBytes", diagnostic_bytes_}, {"blobCount", blobs_.size()}, {"integrityConclusion", critical_gaps ? "partial" : "complete"}, {"criticalGapCount", critical_gaps}, {"totalGapCount", gap_records_}, {"omittedGapCount", omitted_gaps}, {"gaps", gaps}, {"gapJournal", gap_journal}, {"journalWriter", writer}}); if (stream_) summary["chunkStream"] = {{"protocol", "v8trace/chunk-stream/1"}, {"spool", stream_->status().at("spool")}, {"statusFile", "spool.json"}, {"receipt", "chunk-stream-transfer.json"}, {"captureIntegrityIndependentOfNetwork", true}}; json("gaps.json", gaps); json("summary.json", summary); if (stream_) { try { json("chunk-stream-transfer.json", stream_->seal(root)); } catch (const std::exception& error) { json("chunk-stream-transfer.json", {{"protocol", "v8trace/chunk-stream/1"}, {"localCommitted", false}, {"remoteCommitted", false}, {"error", error.what()}, {"sourceRetained", true}}); } } return summary;
}
}
