#include "v8trace/core.h"
#include <iostream>
#include <stdexcept>

using namespace v8trace;
namespace {
unsigned checks = 0;
void check(bool condition, const char* label) { ++checks; if (!condition) throw std::runtime_error(label); }
template<class F> void rejected(F action, const char* label) { bool failed = false; try { action(); } catch (const std::exception&) { failed = true; } check(failed, label); }
}
int main() {
  auto root = fs::temp_directory_path() / ("v8trace-integrity-" + uuid()); fs::create_directory(root);
  try {
    auto capture = root / "capture";
    std::string large(70005, 'x'); large += "中文😀";
    Json summary;
    {
      Store store(capture, {{"maxBytes", 64}, {"maxEvents", 1}});
      check(!store.event("http", "request", {{"value", large}}), "event budget closes admission");
      check(!store.accepting, "normal admission closed");
      for (unsigned i = 0; i < 1500; ++i) store.gap("中文缺口", {{"index", i}, {"value", i == 900 ? large : "full"}}, i % 3 == 0);
      summary = store.finalize({{"backend", "chromium-native"}, {"captureConclusion", "collected"}});
    }
    check(summary.at("gaps").size() == 200, "bounded compatible summary");
    check(summary.at("omittedGapCount") == 1301, "omitted means summary only");
    check(summary.at("totalGapCount") == 1501, "all gaps counted");
    check(summary.at("criticalGapCount") == 501 && summary.at("integrityConclusion") == "partial", "critical semantics unchanged");
    check(summary.at("gapJournal").at("outsideCaptureBudget") == true, "diagnostics independent from exhausted budget");
    check(summary.at("gapJournal").at("drained") == true && summary.at("journalWriter").at("pendingBytes") == 0, "journal drained");
    uint64_t count = 0;
    lines(capture / "gaps.jsonl", [&](const Json& row) {
      ++count; check(row.at("gapSeq") == count, "gap sequence exact");
      if (count > 1) check(row.at("details").at("index") == count - 2, "all gap identities retained");
      if (count == 902) check(str(row.at("details"), "value") == large, "large Unicode gap beyond summary retained");
    });
    check(count == 1501, "all gap rows present");
    check(file_sha256(capture / "gaps.jsonl") == str(summary["gapJournal"], "sha256"), "full gap file hash");
    check(fs::file_size(capture / "gaps.jsonl") == summary["gapJournal"]["bytes"].get<uint64_t>(), "full gap file byte count");
    auto exported = export_case(capture, root / "export");
    check(file_sha256(root / "export/gaps.jsonl") == file_sha256(capture / "gaps.jsonl"), "case export retains full gap journal");
    check(exported["captureIntegrity"] == "partial", "export keeps partial");
    write_file(capture / "gaps.jsonl", "tampered\n");
    rejected([&] { export_case(capture, root / "bad-export"); }, "tampered gap journal rejected before export");
    check(!fs::exists(root / "bad-export"), "invalid ledger creates no exported package");
    {
      Store empty(root / "empty", Json::object()); auto result = empty.finalize({{"backend", "chromium-native"}});
      check(result["totalGapCount"] == 0 && fs::file_size(root / "empty/gaps.jsonl") == 0, "empty ledger explicit");
      check(str(result["gapJournal"], "sha256") == sha256(""), "empty ledger hash");
    }
    {
      auto many = root / "large-gaps"; Store store(many, Json::object());
      for (unsigned i = 0; i < 200; ++i) store.gap("large-diagnostic", {{"index", i}, {"value", large}});
      auto result = store.finalize({{"backend", "chromium-native"}});
      check(result["gaps"].size() < 200 && result["omittedGapCount"] > 0, "summary bounded by bytes as well as rows");
      check(result["gapJournal"]["records"] == 200, "all oversized diagnostics retained");
      check(fs::file_size(many / "summary.json") < 8388608, "summary remains inspectable");
      check(inspect_capture(many)["summary"]["totalGapCount"] == 200, "large diagnostic capture inspectable");
      uint64_t rows = 0; lines(many / "gaps.jsonl", [&](const Json& row) { ++rows; check(str(row.at("details"), "value") == large, "full large diagnostic value preserved"); });
      check(rows == 200, "all large diagnostic rows present");
    }
    {
      Store broken(root / "broken", Json::object()); auto target = broken.filename("gaps.jsonl"); fs::remove(target); fs::create_directory(target);
      rejected([&] { broken.gap("io-error"); broken.finalize({{"backend", "chromium-native"}}); }, "diagnostic write error propagates");
      check(!fs::exists(root / "broken/summary.json"), "no complete summary after diagnostic write failure");
    }
    {
      Store store(root / "browser-ack-timeout", Json::object());
      const std::string header = "[38764:1936:1027/001108.396:ERROR:services\\tracing\\perfetto\\consumer_host.cc:309] ";
      const std::string message = "Timed out waiting for processes to ack BeginTracing: 36444 39360 39680 \r\n";
      std::string original(65536 - header.size() - 20, char(0xff));
      original += '\n'; const auto offset = original.size(); original += header + message;
      original.append(70000, '\0');
      auto raw = store.filename("native-output/browser-stderr.log"); write_file(raw, original);
      const auto hash = file_sha256(raw);
      import_browser_stdio(store);
      auto result = store.finalize({{"backend", "chromium-native"}, {"captureConclusion", "collected"}});
      check(result.at("integrityConclusion") == "partial" && result.at("criticalGapCount") == 1,
            "Native startup ACK timeout is critical even with successful final flush");
      auto gap = result.at("gaps").at(0);
      check(str(gap, "reason") == "native-tracing-start-ack-timeout"
            && str(gap.at("details"), "evidence") == "native-output/browser-stderr.log"
            && gap.at("details").at("byteOffset") == offset,
            "Native startup ACK gap retains its exact raw source location across read boundaries");
      std::string decoded; lines(store.filename("browser-stderr.jsonl"), [&](const Json& row) { decoded += unbase64(str(row, "textBase64")); });
      check(decoded == original && file_sha256(raw) == hash, "Browser binary stdout and stderr remain lossless and unchanged");
    }
    {
      Store store(root / "browser-benign-stderr", Json::object());
      write_file(store.filename("native-output/browser-stderr.log"),
          "[1:2:1027/000000.000:ERROR:google_apis/gcm/engine.cc:4] DEPRECATED_ENDPOINT\n"
          "[1:2:1027/000000.000:INFO:CONSOLE:1] Timed out waiting for processes to ack BeginTracing: 123\n"
          "[1:2:1027/000000.000:INFO:CONSOLE:1] [3:4:1027/0:ERROR:services/tracing/perfetto/consumer_host.cc:309] Timed out waiting for processes to ack BeginTracing: 123\n");
      import_browser_stdio(store);
      check(store.finalize({{"backend", "chromium-native"}}).at("criticalGapCount") == 0,
            "Unrelated browser errors and page console strings are not native ACK failures");
    }
    {
      Store store(root / "browser-fatal-stderr", Json::object());
      write_file(store.filename("native-output/browser-stderr.log"), std::string(65532, 'x') + ":FATAL:source.cc:1] actual fatal\n");
      import_browser_stdio(store);
      auto result = store.finalize({{"backend", "chromium-native"}});
      check(result.at("criticalGapCount") == 1 && str(result.at("gaps").at(0), "reason") == "native-browser-process-fatal",
            "Native fatal detection survives chunk splits");
    }
    fs::remove_all(root); std::cout << "PASS " << checks << " checks\n"; return 0;
  } catch (const std::exception& error) { std::cerr << "FAIL " << checks << ": " << error.what() << '\n'; fs::remove_all(root); return 1; }
}
