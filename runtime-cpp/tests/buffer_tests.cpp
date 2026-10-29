#include "v8trace/core.h"
#include "v8trace/journal.h"
#include <future>
#include <iostream>
#include <thread>

using namespace v8trace;
namespace {
unsigned checks = 0;
void check(bool value, const char* name) { ++checks; if (!value) throw std::runtime_error(name); }
template<class F> void rejected(F fn, const char* name) { bool failed = false; try { fn(); } catch (const std::exception&) { failed = true; } check(failed, name); }
void producer_checks() {
  std::map<std::string, std::string> files;
  JournalWriter writer(4096, 1024, [&](std::string_view file, std::string_view bytes) { files[std::string(file)] += bytes; }, [] {});
  std::vector<std::jthread> producers;
  for (unsigned p = 0; p < 4; ++p) producers.emplace_back([&, p] { for (unsigned i = 0; i < 1000; ++i) writer.append(std::to_string(p), std::to_string(i) + '\n'); });
  for (auto& thread : producers) thread.join(); writer.flush();
  bool exact = true;
  for (unsigned p = 0; p < 4; ++p) { std::string expected; for (unsigned i = 0; i < 1000; ++i) expected += std::to_string(i) + '\n'; exact = exact && files[std::to_string(p)] == expected; }
  check(exact, "four producers retain complete per-producer order");
  writer.append("tail", "TAIL"); writer.finish(); auto stats = writer.stats();
  check(files["tail"] == "TAIL" && stats.enqueued_rows == 4001 && stats.written_rows == 4001, "finish drains tail exactly");
  check(stats.enqueued_bytes == stats.written_bytes && stats.pending_bytes == 0 && stats.high_water_bytes <= 4096, "in-flight admission respects byte bound");
  check(stats.high_water_rows <= stats.maximum_pending_rows, "small rows also respect queue metadata bound");
  rejected([&] { writer.append("tail", "late"); }, "closed writer rejects append"); writer.flush(); writer.finish();
}
void batching_checks() {
  std::promise<void> entered, release; auto entry = entered.get_future(); auto released = release.get_future().share();
  bool first = true; std::string result;
  JournalWriter writer(1048576, 65536, [&](std::string_view, std::string_view bytes) { if (first) { first = false; entered.set_value(); released.wait(); } result += bytes; }, [] {});
  writer.append("file", std::string(256, 'A')); bool began = entry.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
  for (unsigned i = 0; i < 300; ++i) writer.append("file", std::string(256, 'A'));
  release.set_value(); writer.finish(); auto stats = writer.stats();
  check(began && result == std::string(301 * 256, 'A'), "batched output retains every byte");
  check(stats.enqueued_rows == 301 && stats.write_batches == 3, "consecutive rows coalesce into bounded batches");
  std::cout << dump(Json{{"batchingProbe", true}, {"rows", stats.enqueued_rows}, {"sinkWrites", stats.write_batches}, {"highWaterBytes", stats.high_water_bytes}}) << '\n';
}
void blocking(bool fail) {
  std::promise<void> entered, release; auto entered_future = entered.get_future(); auto released = release.get_future().share();
  bool first = true, producer_failed = false;
  std::string result;
  JournalWriter writer(64, 32, [&](std::string_view, std::string_view bytes) {
    if (first) { first = false; entered.set_value(); released.wait(); if (fail) throw std::runtime_error("injected write failure"); }
    result += bytes;
  }, [] {});
  writer.append("file", std::string(64, 'A'));
  bool began = entered_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
  std::jthread producer([&] { try { writer.append("file", "TAIL"); } catch (const std::exception&) { producer_failed = true; } });
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!writer.stats().backpressure_waits && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
  bool waited = writer.stats().backpressure_waits == 1 && writer.stats().pending_bytes == 64;
  release.set_value(); producer.join();
  check(began && waited, "slow in-flight batch applies backpressure without eviction");
  if (fail) { rejected([&] { writer.flush(); }, "flush propagates worker write error"); rejected([&] { writer.finish(); }, "finish propagates worker write error"); check(producer_failed, "failed writer wakes and rejects blocked producer"); }
  else { writer.finish(); check(!producer_failed && result == std::string(64, 'A') + "TAIL", "backpressure retains exact tail"); }
}
void large_and_failure() {
  std::string bytes; JournalWriter writer(32, 16, [&](std::string_view, std::string_view value) { bytes += value; }, [] {});
  auto large = std::string(70000, 'Z') + "END"; writer.append("large", large); writer.append("large", "TAIL"); writer.finish(); auto stats = writer.stats();
  check(bytes == large + "TAIL" && stats.oversized_rows == 1 && stats.high_water_bytes == large.size(), "oversized record is exclusive and never truncated");
  JournalWriter failed(4096, 1024, [](std::string_view, std::string_view) {}, [] { throw std::runtime_error("injected flush failure"); });
  failed.append("file", "row\n"); rejected([&] { failed.flush(); }, "sink flush error is propagated");
  rejected([] { JournalWriter invalid(8, 9, [](auto, auto) {}, [] {}); }, "batch larger than capacity rejected");
}
void store_checks(const fs::path& root) {
  Json config{{"url", "https://fixture.test/"}, {"output", utf8(root / "uncreated")}, {"scripts", true}};
  auto normalized = validate_options(config, {}, false);
  check(normalized["writeBufferBytes"] == 8388608 && normalized["writeBatchBytes"] == 1048576, "default write buffer stays separate from log budget");
  for (Json value : {Json(false), Json(-1), Json(0), Json(1.5), Json(4095), Json(268435457)}) {
    auto invalid = config; invalid["writeBufferBytes"] = value; rejected([&] { validate_options(invalid, {}, false); }, "invalid write buffer rejected");
  }
  auto invalid_batch = config; invalid_batch["writeBufferBytes"] = 4096; invalid_batch["writeBatchBytes"] = 8192;
  rejected([&] { validate_options(invalid_batch, {}, false); }, "batch exceeding buffer rejected before capture");
  Json options{{"maxBytes", 16777216}, {"maxEvents", 1000}, {"writeBufferBytes", 4096}, {"writeBatchBytes", 4096}};
  Store store(root / "capture", options);
  auto marker = parse(R"({"s":"完整\ud800\u0000END"})"); marker["large"] = std::string(70000, 'Z');
  for (unsigned i = 0; i < 70; ++i) check(store.append("events/channel-" + std::to_string(i) + ".jsonl", marker), "store accepts complete journal row");
  store.flush(); check(load_json(store.filename("events/channel-0.jsonl")) == marker, "flush barrier makes full Unicode row visible");
  rejected([&] { store.append("../outside.jsonl", marker); }, "journal rejects traversal before queue admission");
  rejected([&] { store.append("../outside.jsonl", marker); }, "failed validation is never cached as an allowed path");
  auto summary = store.finalize(Json::object()); auto stats = summary.at("journalWriter");
  check(stats["drained"] == true && stats["writtenRows"] == 70 && stats["oversizedRows"] == 70, "store final summary records exact buffer receipt");
  check(stats["enqueuedBytes"] == stats["writtenBytes"], "journal receipt preserves full bytes");
  { Store unfinished(root / "destructor", options); unfinished.append("tail.jsonl", marker); }
  check(load_json(root / "destructor/tail.jsonl") == marker, "normal destruction drains journal before closing handles");
  { Store bad(root / "failure", options); fs::create_directories(bad.filename("blocked.jsonl")); bad.append("blocked.jsonl", marker); rejected([&] { bad.finalize(Json::object()); }, "disk open failure never yields a complete summary"); check(!fs::exists(root / "failure/summary.json"), "write failure does not publish success receipt"); }
}
}
int main() {
  auto root = fs::temp_directory_path() / ("v8trace-buffer-test-" + uuid()); fs::create_directory(root);
  try { producer_checks(); batching_checks(); blocking(false); blocking(true); large_and_failure(); store_checks(root); fs::remove_all(root); std::cout << "PASS " << checks << " checks; owned temporary files removed\n"; return 0; }
  catch (const std::exception& error) { std::cerr << "FAIL " << checks << ": " << error.what() << '\n'; fs::remove_all(root); return 1; }
}
