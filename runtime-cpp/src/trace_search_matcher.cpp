#include "trace_search_internal.h"
#include <algorithm>
#include <cstring>
#include <future>
#include <limits>
#include <re2/re2.h>

namespace v8trace::trace_search {
namespace {
struct Range { uint64_t line, count, begin, end; };
struct Hit { uint64_t line; std::string text; bool lossy = false, overflow = false; };
struct Chunk { std::vector<Hit> hits; std::exception_ptr error; };
struct Batch {
  std::atomic_bool stop{false};
  std::vector<std::future<Chunk>> futures;
  ~Batch() { stop = true; for (auto& future : futures) if (future.valid()) future.wait(); }
};
std::vector<Range> ranges(const LineIndex& index, uint64_t start, uint64_t count, unsigned threads, const std::atomic_bool* stop) {
  if (!count) return {};
  auto begin = index.offset(start, stop), end = index.offset(start + count, stop);
  std::vector<Checkpoint> boundaries{{start,begin}};
  for (unsigned part = 1; part < threads; ++part) {
    auto target = begin + (end - begin) / threads * part;
    auto it = std::lower_bound(index.checkpoints.begin(), index.checkpoints.end(), target, [](const Checkpoint& entry, uint64_t value){ return entry.offset < value; });
    if (it != index.checkpoints.end() && it->line > boundaries.back().line && it->line < start + count) boundaries.push_back(*it);
  }
  boundaries.push_back({start + count,end}); std::vector<Range> output;
  for (size_t n = 1; n < boundaries.size(); ++n) output.push_back({boundaries[n-1].line,boundaries[n].line-boundaries[n-1].line,boundaries[n-1].offset,boundaries[n].offset});
  return output;
}
Chunk scan(const Source& source, const Range& range, const TraceSearchConfig& config, const re2::RE2& matcher, uint64_t max_matches, const std::atomic_bool* caller, const std::atomic_bool& local) {
  Chunk output;
  try {
    auto check = [&]{ cancelled(caller); cancelled(&local); };
    std::ifstream input(source.file,std::ios::binary); require(bool(input),"Open search source failed"); input.seekg(std::streamoff(range.begin)); require(bool(input),"Seek search source failed");
    std::vector<char> buffer(4*1024*1024); std::string line; line.reserve(4096); uint64_t position = range.begin, current = range.line, content_bytes = 0; bool done = false;
    auto emit = [&] {
      check(); auto text = std::string_view(line); if (!text.empty() && text.back()=='\n') text.remove_suffix(1); if (!text.empty() && text.back()=='\r') text.remove_suffix(1);
      if (re2::RE2::PartialMatch(absl::string_view(text.data(),text.size()),matcher)) {
        bool exceeded = false; auto decoded = lossy_utf8_limited(text,config.max_content_bytes-content_bytes,exceeded);
        content_bytes += decoded.first.size(); output.hits.push_back({current,std::move(decoded.first),decoded.second,exceeded});
        done = exceeded || output.hits.size() >= max_matches;
      }
      ++current; line.clear();
    };
    while (position < range.end && !done) {
      check(); auto length = size_t(std::min<uint64_t>(buffer.size(),range.end-position)); input.read(buffer.data(),std::streamsize(length)); require(size_t(input.gcount())==length,"Source changed or search read failed");
      const auto* at = buffer.data(); const auto* end = at+length;
      while (at < end && !done) {
        auto found = static_cast<const char*>(std::memchr(at,'\n',size_t(end-at))); auto finish = found ? found+1 : end; auto amount = size_t(finish-at);
        require(amount <= config.max_line_bytes-line.size(),"Search line exceeds max_line_bytes at line "+std::to_string(current)+"; export the raw range instead");
        line.append(at,amount); at = finish; if (found) emit();
      }
      position += length;
    }
    if (!done && !line.empty()) emit();
    if (!done) require(current == range.line+range.count,"Search line boundaries changed");
  } catch (...) { output.error = std::current_exception(); }
  return output;
}
}
Json search_index(const LineIndex& index, const TraceSearchConfig& config, const Json& request, const std::atomic_bool* stop) {
  auto start = number(request,"start_line"), requested = number(request,"max_scan_lines"), maximum = number(request,"max_matches",100);
  require(start && start-1 <= index.line_count,"Line is outside the file; EOF is line_count + 1");
  require(maximum && maximum <= config.max_matches,"max_matches exceeds configured range");
  require(field(request,"pattern").is_string(),"A pattern string is required"); auto pattern = str(request,"pattern");
  require(pattern.size() <= config.max_pattern_bytes,"Pattern exceeds configured byte limit");
  bool regex = boolean(request,"regex",false), sensitive = boolean(request,"case_sensitive",true);
  require(regex || !pattern.empty(),"Empty literal pattern is not allowed");
  require(!lossy_utf8(pattern).second,"Pattern must be valid UTF-8");
  re2::RE2::Options options; options.set_log_errors(false); options.set_never_capture(true); options.set_max_mem(16*1024*1024); options.set_case_sensitive(sensitive); options.set_literal(!regex);
  if (!regex && sensitive) options.set_encoding(re2::RE2::Options::EncodingLatin1);
  re2::RE2 matcher(pattern,options); require(matcher.ok(),"Invalid RE2 pattern: "+matcher.error());
  auto selected = std::min(requested,index.line_count-(start-1));
  auto threads = config.search_threads ? config.search_threads : std::min(8u,std::max(1u,std::thread::hardware_concurrency()));
  const uint64_t per_worker = 4*1024*1024 + config.max_line_bytes + 2*config.max_content_bytes + config.max_matches*128;
  auto affordable = std::max<uint64_t>(1,config.search_memory_bytes/config.query_concurrency/per_worker); threads = unsigned(std::min<uint64_t>(threads,affordable));
  auto blocks = ranges(index,start,selected,threads,stop); Batch batch;
  for (const auto& block : blocks) batch.futures.push_back(std::async(std::launch::async,[&,block]{ return scan(index.source,block,config,matcher,maximum,stop,batch.stop); }));
  Json matches = Json::array(); uint64_t bytes = 0, scanned = selected, cursor = start+selected; bool lossy = false, match_limit = false, content_limit = false, halted = false;
  for (auto& future : batch.futures) {
    auto chunk = future.get(); cancelled(stop);
    for (auto& hit : chunk.hits) {
      if (hit.overflow || hit.text.size() > config.max_content_bytes-bytes) {
        require(!matches.empty(),"First matched line exceeds content limit; export_lines preserves the raw bytes");
        scanned = hit.line-start; cursor = hit.line; content_limit = true; halted = true; break;
      }
      bytes += hit.text.size(); lossy = lossy || hit.lossy; auto line = hit.line; matches.push_back({{"line",line},{"content",std::move(hit.text)}});
      if (matches.size() >= maximum) { scanned = line-start+1; cursor = line+1; match_limit = cursor<=index.line_count; halted = true; break; }
    }
    if (halted) { batch.stop = true; break; }
    if (chunk.error) std::rethrow_exception(chunk.error);
  }
  cancelled(stop); bool eof = cursor > index.line_count;
  return {{"mode",regex?"regex":"literal"},{"regex_engine","RE2 2025-11-05"},{"start_line",start},{"scanned_lines",scanned},{"matches",std::move(matches)},{"lossy_utf8",lossy},{"next_line",eof?Json(nullptr):Json(cursor)},{"eof",eof},{"scan_limit_reached",!halted&&!eof},{"match_limit_reached",match_limit},{"content_limit_reached",content_limit},{"worker_count",blocks.size()}};
}
}
