#include "v8trace/http_drain.h"
#include <iostream>

using namespace v8trace;
namespace {
unsigned checks = 0;
void check(bool condition, const char* message) { ++checks; if (!condition) throw std::runtime_error(message); }
template<class Action> void rejected(Action action, const char* message) { bool failed = false; try { action(); } catch (const std::exception&) { failed = true; } check(failed, message); }
std::string header() { return "{\"constants\":{\"logEventTypes\":{\"REQUEST_ALIVE\":7},\"logSourceType\":{\"URL_REQUEST\":11},\"logEventPhase\":{\"PHASE_BEGIN\":1,\"PHASE_END\":2}},\n\"events\": [\n"; }
std::string event(int phase, int id = 1) { return dump({{"type", 7}, {"phase", phase}, {"source", {{"id", id}, {"type", 11}}}}); }
void append(const fs::path& file, const std::string& data) { std::ofstream out(file, std::ios::binary | std::ios::app); out << data; if (!out) throw std::runtime_error("Fixture write failed"); }
}
int main(int argc, char** argv) {
  auto root = fs::temp_directory_path() / ("v8trace-http-drain-" + uuid()); fs::create_directory(root);
  try {
    NativeHttpDrain::Clock::time_point zero;
    auto file = root / "netlog.json";
    NativeHttpDrain drain(file, 1048576);
    drain.poll(zero); check(!drain.quiet(zero + std::chrono::seconds(10)), "absent witness is not quiescence");
    write_file(file, header() + event(1)); drain.poll(zero);
    check(drain.status().at("activeRequests") == 1, "complete event without final newline consumed");
    check(!drain.quiet(zero + std::chrono::seconds(10)), "stalled request prevents false quiet");
    append(file, ",\n" + event(2).substr(0, 20)); drain.poll(zero + std::chrono::seconds(1));
    check(!drain.quiet(zero + std::chrono::seconds(10)), "partial end event never removes active request");
    append(file, event(2).substr(20)); drain.poll(zero + std::chrono::seconds(2));
    check(drain.status().at("activeRequests") == 0 && !drain.quiet(zero + std::chrono::seconds(3)), "quiet interval starts at observed terminal");
    drain.poll(zero + std::chrono::seconds(4)); check(drain.quiet(zero + std::chrono::seconds(4)), "terminal plus quiet is acknowledged");
    check(drain.status().at("requestStarts") == 1 && drain.status().at("requestEnds") == 1, "same events are not reread or double counted");
    append(file, ",\n" + event(1, 2) + ",\n" + event(1, 3) + ",\n" + event(2, 2) + ",\n"); drain.poll(zero + std::chrono::seconds(5));
    check(drain.status().at("activeRequests") == 1, "all concurrent request identities must end");
    append(file, event(2, 3) + ",\n"); drain.poll(zero + std::chrono::seconds(6));
    check(drain.quiet(zero + std::chrono::seconds(8)), "all concurrent terminals and quiet settle");
    write_file(file, "{}"); rejected([&] { drain.poll(); }, "witness replacement/truncation rejected");
    auto duplicate = root / "duplicate.json"; write_file(duplicate, header() + event(1) + ",\n" + event(1) + ",\n");
    NativeHttpDrain repeated(duplicate, 1048576); rejected([&] { repeated.poll(); }, "duplicate request begin is not silently ignored");
    NativeHttpDrain tiny(duplicate, 4); rejected([&] { tiny.poll(); }, "raw witness byte budget is retained");
    auto wrong = root / "wrong.json"; write_file(wrong, "{\"constants\":{},\n\"events\": [\n");
    NativeHttpDrain invalid(wrong, 1048576); rejected([&] { invalid.poll(); }, "unknown native constants never treated as empty traffic");
    auto initial = root / "initial.json"; write_file(initial, header() + event(2, 9) + ",\n");
    NativeHttpDrain before(initial, 1048576); before.poll(zero);
    check(before.status().at("initialRequestEnds") == 1, "pre-witness lifetime ends remain explicit");
    check(before.status().at("completeCaptureInferred") == false, "network quiet is not blanket collection completeness");
    auto alias = root / "invented-name.json"; auto invented = header(); invented.replace(invented.find("REQUEST_ALIVE"), 13, "URL_REQUEST_ALIVE"); write_file(alias, invented);
    NativeHttpDrain unknown_name(alias, 1048576); rejected([&] { unknown_name.poll(); }, "invented event name is not an empty traffic witness");
    auto other_source = root / "other-source.json"; auto mismatch = parse(event(1)); mismatch["source"]["type"] = 12; write_file(other_source, header() + dump(mismatch) + ",\n");
    NativeHttpDrain invalid_source(other_source, 1048576); rejected([&] { invalid_source.poll(); }, "lifetime event from a different source type rejected");
    auto footer = root / "native-footer.json"; write_file(footer, header() + event(1) + ",\n" + event(2) + "],\n\"polledData\":{");
    NativeHttpDrain finalizing(footer, 1048576); finalizing.poll(zero);
    check(!finalizing.quiet(zero + std::chrono::seconds(2)), "unfinished native footer is not an empty witness");
    append(footer, "}}"); finalizing.poll(zero);
    check(finalizing.quiet(zero + std::chrono::seconds(2)) && finalizing.status().at("footerVerified") == true, "attached array closure and complete native metadata footer consumed");
    append(footer, "\n{}\n"); rejected([&] { finalizing.poll(); }, "data after a sealed native footer rejected");
    auto wrong_footer = root / "wrong-footer.json"; write_file(wrong_footer, header() + event(1) + ",\n" + event(2) + "],\n\"otherMetadata\":{}}\n");
    NativeHttpDrain unexpected_footer(wrong_footer, 1048576); rejected([&] { unexpected_footer.poll(); }, "unknown footer is not silently dropped");
    auto separate_footer = root / "separate-footer.json"; write_file(separate_footer, header() + event(1) + ",\n" + event(2) + "],\n\"polledData\":{\"network\":{\"state\":\"idle\"}}\n");
    NativeHttpDrain separate(separate_footer, 1048576); separate.poll(zero);
    check(!separate.quiet(zero + std::chrono::seconds(2)), "validated metadata still requires native root closure");
    append(separate_footer, "}\n"); separate.poll(zero);
    check(separate.quiet(zero + std::chrono::seconds(2)), "actual native two-line metadata footer validated");
    if (argc != 1) {
      check(argc == 3 && std::string_view(argv[1]) == "--witness", "unknown regression arguments rejected");
      NativeHttpDrain recorded(path(argv[2]), 67108864); recorded.poll(zero);
      check(recorded.status().at("headerVerified") == true && recorded.status().at("requestStarts").get<uint64_t>() > 0, "actual pinned Chromium constants and request lifetimes consumed");
      check(recorded.quiet(zero + std::chrono::seconds(2)), "sealed real witness lifetimes close without suppressing any parser failure");
      std::cout << "ACTUAL_WITNESS " << dump(recorded.status()) << '\n';
    }
    fs::remove_all(root); std::cout << "PASS " << checks << " checks\n"; return 0;
  } catch (const std::exception& error) { std::cerr << "FAIL " << checks << ": " << error.what() << '\n'; fs::remove_all(root); return 1; }
}
