#include "v8trace/http_drain.h"
#include <algorithm>
#include <array>

namespace v8trace {
NativeHttpDrain::NativeHttpDrain(fs::path file, uint64_t maximum_bytes)
    : file_(std::move(file)), maximum_bytes_(maximum_bytes) {}
void NativeHttpDrain::line(std::string data, Clock::time_point at) {
  data = trim(data);
  if (data.empty() || data == ",") return;
  if (data.front() == ',') data = trim(data.substr(1));
  if (!header_) {
    if (data.back() != ',') throw std::runtime_error("Native HTTP drain constants header is unfinished");
    auto constants = parse(data.substr(0, data.size() - 1) + "}").at("constants");
    auto types = constants.at("logEventTypes"), phases = constants.at("logEventPhase");
    alive_type_ = types.at("REQUEST_ALIVE").get<uint64_t>();
    request_source_ = constants.at("logSourceType").at("URL_REQUEST").get<uint64_t>();
    begin_ = phases.at("PHASE_BEGIN").get<uint64_t>(); end_ = phases.at("PHASE_END").get<uint64_t>();
    if (begin_ == end_) throw std::runtime_error("Native HTTP drain phases conflict");
    header_ = true; last_activity_ = at; return;
  }
  if (!events_) {
    if (data != "\"events\": [") throw std::runtime_error("Native HTTP drain events array missing");
    events_ = true; return;
  }
  if (events_closed_) {
    if (footer_verified_) throw std::runtime_error("Native HTTP drain has data after its footer");
    if (data == "}") {
      if (footer_expected_ && !footer_value_verified_) throw std::runtime_error("Native HTTP drain metadata footer missing");
      footer_verified_ = true; return;
    }
    constexpr std::string_view prefix = "\"polledData\":";
    if (!footer_expected_ || footer_value_verified_ || !data.starts_with(prefix)) throw std::runtime_error("Native HTTP drain footer differs");
    bool value_complete = false;
    try { value_complete = parse(data.substr(prefix.size())).is_object(); } catch (const std::exception&) {}
    if (value_complete) { footer_value_verified_ = true; return; }
    auto footer = parse("{" + data);
    if (!footer.is_object() || footer.size() != 1 || !footer.contains("polledData") || !footer.at("polledData").is_object())
      throw std::runtime_error("Native HTTP drain footer differs");
    footer_value_verified_ = true; footer_verified_ = true; return;
  }
  if (data == "]}" || data == "]" || data == "],") {
    events_closed_ = true; footer_expected_ = data == "],"; footer_verified_ = data == "]}"; return;
  }
  bool closes_events = data.ends_with("],") || data.ends_with("]}");
  bool closes_root = data.ends_with("]}");
  if (closes_events) data.resize(data.size() - 2);
  if (data.back() == ',') data.pop_back();
  auto record = parse(data);
  if (closes_events) { events_closed_ = true; footer_expected_ = !closes_root; footer_verified_ = closes_root; }
  if (record.at("type") != alive_type_) return;
  const auto& source = record.at("source");
  if (source.at("type") != request_source_) throw std::runtime_error("Native request lifetime source type differs");
  auto key = text(source.at("type")) + '/' + text(source.at("id"));
  if (record.at("phase") == begin_) {
    if (active_.size() >= 1000000 || !active_.insert(key).second) throw std::runtime_error("Native HTTP drain duplicate or excessive live request");
    ++starts_;
  } else if (record.at("phase") == end_) {
    if (!active_.erase(key)) ++initial_ends_;
    ++ends_;
  } else throw std::runtime_error("Native HTTP drain request phase invalid");
  last_activity_ = at;
}
void NativeHttpDrain::poll(Clock::time_point at) {
  if (!fs::exists(file_)) return;
  auto length = fs::file_size(file_);
  if (length < offset_ || length > maximum_bytes_) throw std::runtime_error("Native HTTP drain witness shrank or exceeded byte budget");
  std::ifstream input(file_, std::ios::binary); if (!input) throw std::runtime_error("Native HTTP drain witness open failed");
  input.seekg(std::streamoff(offset_)); std::array<char, 65536> buffer{};
  while (input) {
    input.read(buffer.data(), buffer.size()); auto size = size_t(input.gcount());
    if (size > maximum_bytes_ - offset_) throw std::runtime_error("Native HTTP drain growing witness exceeds byte budget");
    offset_ += size;
    for (size_t index = 0; index < size; ++index) {
      if (buffer[index] == '\n') { line(std::move(pending_), at); pending_.clear(); }
      else {
        if (pending_.size() >= std::min<uint64_t>(maximum_bytes_, 16777216)) throw std::runtime_error("Native HTTP drain event exceeds parser budget");
        pending_ += buffer[index];
      }
    }
  }
  if (!input.eof()) throw std::runtime_error("Native HTTP drain witness read failed");
  auto tail = trim(pending_);
  if (events_ && !tail.empty()) {
    bool complete = false;
    if (events_closed_) {
      if (tail == "}") complete = true;
      else {
        try { complete = parse("{" + tail).is_object(); } catch (const std::exception&) {}
        constexpr std::string_view prefix = "\"polledData\":";
        if (!complete && tail.starts_with(prefix)) {
          try { complete = parse(tail.substr(prefix.size())).is_object(); } catch (const std::exception&) {}
        }
      }
    } else if (tail == "]}" || tail == "]" || tail == "],") complete = true;
    else if (tail.front() == '{') {
      auto candidate = tail;
      if (candidate.ends_with("],") || candidate.ends_with("]}")) candidate.resize(candidate.size() - 2);
      if (!candidate.empty() && candidate.back() == ',') candidate.pop_back();
      try { complete = parse(candidate).is_object(); } catch (const std::exception&) {}
    }
    if (complete) { line(std::move(tail), at); pending_.clear(); }
  }
}
bool NativeHttpDrain::quiet(Clock::time_point at) const {
  return header_ && events_ && (!events_closed_ || footer_verified_) && active_.empty() && trim(pending_).empty()
      && at >= last_activity_ && at - last_activity_ >= std::chrono::milliseconds(1500);
}
Json NativeHttpDrain::status() const {
  return {{"protocol", "v8trace/http-drain/1"}, {"witness", "native-netlog-url-request-lifetime"},
      {"headerVerified", header_ && events_}, {"bytesRead", offset_}, {"requestStarts", starts_}, {"requestEnds", ends_},
      {"initialRequestEnds", initial_ends_}, {"activeRequests", active_.size()}, {"unfinishedEventBytes", pending_.size()},
      {"eventsArrayClosed", events_closed_}, {"footerVerified", footer_verified_},
      {"minimumQuietMilliseconds", 1500}, {"completeCaptureInferred", false}};
}
}
