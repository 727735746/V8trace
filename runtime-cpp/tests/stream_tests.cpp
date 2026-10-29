#include "v8trace/stream.h"
#include <iostream>
#include <thread>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#endif
using namespace v8trace;
using namespace std::string_literals;
using namespace std::string_view_literals;
namespace {
unsigned checks = 0;
void check(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
template<class F> void reject(F action, const char* message) { bool failed = false; try { action(); } catch (...) { failed = true; } check(failed, message); }
unsigned free_port() {
#ifdef _WIN32
  WSADATA data; if (WSAStartup(MAKEWORD(2, 2), &data)) throw std::runtime_error("test WSAStartup");
#endif
  auto socket = ::socket(AF_INET, SOCK_STREAM, 0); sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); if (::bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address))) throw std::runtime_error("test ephemeral bind");
#ifdef _WIN32
  int size = sizeof(address); getsockname(socket, reinterpret_cast<sockaddr*>(&address), &size); closesocket(socket); WSACleanup();
#else
  socklen_t size = sizeof(address); getsockname(socket, reinterpret_cast<sockaddr*>(&address), &size); ::close(socket);
#endif
  return ntohs(address.sin_port);
}
std::string binary() { std::string bytes; uint32_t state = 123; for (unsigned i = 0; i < 2200123; ++i) { state ^= state << 13; state ^= state >> 17; state ^= state << 5; bytes.push_back(char(state)); } return bytes; }
}
int main() {
  auto root = fs::temp_directory_path() / ("v8trace-stream-tests-" + uuid()); fs::create_directory(root);
  try {
    auto source = root / "source"; fs::create_directory(source); fs::create_directory(source / "browser-profile"); write_file(source / "browser-profile/private.txt", "profile not transmitted");
    auto bytes = binary(); write_file(source / "完整字节.bin", bytes); write_file(source / "empty", ""); write_file(source / "unicode.txt", "中文😀\n");
    auto archive = root / "archive"; auto packed = stream_pack(source, archive); check(packed.at("localCommitted") == true && packed.at("remoteCommitted") == false, "local archive commit differs from remote ACK");
    auto reconstructed = root / "reconstructed"; auto result = stream_unpack(archive, reconstructed); check(result.at("files") == 3 && read_file(reconstructed / "完整字节.bin") == bytes && read_file(reconstructed / "unicode.txt") == "中文😀\n", "byte exact bounded chunk reconstruction"); check(!fs::exists(reconstructed / "browser-profile"), "profile excluded");
    reject([&] { stream_unpack(archive, reconstructed); }, "existing output protected");
    auto damaged = root / "damaged"; fs::copy(archive, damaged, fs::copy_options::recursive); auto payload = damaged / "chunks/00000000000000000001/payload.zst"; auto original = read_file(payload); write_file(payload, "bad"); reject([&] { stream_unpack(damaged, root / "bad-output"); }, "encoded corruption rejected"); check(!fs::exists(root / "bad-output"), "failed restore not committed"); write_file(payload, original);
    fs::remove_all(damaged / "chunks/00000000000000000001"); reject([&] { stream_unpack(damaged, root / "missing-output"); }, "missing chunk rejected");
    auto uncommitted = root / "uncommitted"; { ChunkSpool spool(uncommitted, uuid()); spool.append("a", "first\0bytes"sv); spool.flush(); check(spool.status().at("sealedChunks") == 1 && spool.status().at("localCommitted") == false, "durable partial chunks distinguish final commit"); } reject([&] { stream_unpack(uncommitted, root / "partial-output"); }, "strong-stop style uncommitted stream stays partial"); check(fs::exists(uncommitted / "chunks/00000000000000000001/payload.zst"), "uncommitted evidence retained");
    fs::rename(archive / "commit", archive / "commit.new"); reject([&] { stream_unpack(archive, root / "premature"); }, "unsealed final directory ignored"); fs::rename(archive / "commit.new", archive / "commit");
    { auto append_source = root / "append-source"; fs::create_directory(append_source); ChunkSpool spool(root / "append-stream", uuid()); std::string expected;
      for (unsigned i = 0; i < 10000; ++i) { std::string line = "中文\0"s + std::to_string(i) + '\n'; expected += line; spool.append("events/log.jsonl", line); }
      check(spool.status().at("sealedChunks") == 0, "small rows coalesced not fsync per row"); fs::create_directory(append_source / "events"); write_file(append_source / "events/log.jsonl", expected); spool.seal(append_source); stream_unpack(root / "append-stream", root / "append-restored"); check(read_file(root / "append-restored/events/log.jsonl") == expected, "append fragments retain NUL and per-file offsets"); }
    { auto changed = root / "changed"; fs::create_directory(changed); write_file(changed / "log", "other"); ChunkSpool spool(root / "changed-stream", uuid()); spool.append("log", "abcdef"); reject([&] { spool.seal(changed); }, "changed append-only source rejected"); check(read_file(changed / "log") == "other", "failed seal keeps original"); }
    { ChunkSpool limited(root / "limited", uuid(), {{"streamSpoolMaxBytes", 1000}}); reject([&] { limited.append("binary", bytes); limited.flush(); }, "explicit spool budget enforced"); }
    { Json options{{"chunkSpool", true}, {"maxBytes", 100000000}, {"maxEvents", 100000}}; Store store(root / "integrated", options);
      store.event("env", "unicode", {{"value", parse("\"\\ud800\\u0000中文\"")}}); store.gap("explicit-fixture-gap", {{"value", "完整"}}, false);
      auto raw = store.filename("native-output/trace.json"); write_file(raw, bytes); auto blob = store.blob_file(raw, "chrome-native-startup-trace-json"); check(file_sha256(store.root / str(blob, "path")) == sha256(bytes), "finalized raw blob retains SHA256");
      auto summary = store.finalize({{"backend", "chromium-native"}}); check(summary.at("integrityConclusion") == "complete", "network-independent capture integrity"); check(load_json(store.root / "chunk-stream-transfer.json").at("localCommitted") == true, "Store journal and final raw files seal together");
      stream_unpack(root / "integrated.stream", root / "integrated-restored"); check(read_file(root / "integrated-restored/events/env.jsonl") == read_file(store.root / "events/env.jsonl"), "Store journal event byte exact after reconstruction"); check(read_file(root / "integrated-restored/gaps.jsonl") == read_file(store.root / "gaps.jsonl"), "full gap ledger included"); }
    auto port = free_port(); std::atomic_bool stop = false, ready = false; std::exception_ptr server_error;
    std::thread server([&] { try { tcp_receive({{"root", utf8(root / "received")}, {"port", port}}, stop, [&](const Json& row) { if (str(row, "type") == "listening") ready = true; }); } catch (...) { server_error = std::current_exception(); ready = true; } });
    struct Stop { std::atomic_bool& stop; std::thread& thread; ~Stop() { stop = true; if (thread.joinable()) thread.join(); } } guard{stop, server};
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5); while (!ready && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10)); check(ready, "receiver started"); if (server_error) std::rethrow_exception(server_error);
    auto live_source = root / "live-source"; fs::create_directory(live_source); write_file(live_source / "log", bytes);
    auto live_spool = root / "live-spool"; { ChunkSpool spool(live_spool, uuid(), {{"streamTCP", true}, {"streamPort", port}, {"streamDrainTimeout", 10000}}); spool.append("log", std::string_view(bytes).substr(0, 100)); spool.flush();
      deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10); while (spool.status().at("acknowledgedChunks") == 0 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10)); check(spool.status().at("acknowledgedChunks") == 1 && spool.status().at("remoteCommitted") == false, "TCP receives chunks before capture finalization"); spool.append("log", std::string_view(bytes).substr(100)); auto receipt = spool.seal(live_source); check(receipt.at("remoteCommitted") == true, "separate final commit ACK"); }
    auto repeated = stream_send(live_spool, {{"port", port}}); check(repeated.at("remoteCommitted") == true, "already ACKed immutable transfer resumes idempotently"); stream_unpack(root / "received", root / "received-restored"); check(read_file(root / "received-restored/log") == bytes, "remote chunks fully reconstruct original");
    stop = true; server.join(); if (server_error) std::rethrow_exception(server_error);
    for (const auto& entry : fs::directory_iterator(root)) check(!utf8(entry.path().filename()).starts_with("stream-restore-"), "owned failed restore temp cleaned");
    fs::remove_all(root); std::cout << "PASS " << checks << " checks\n"; return 0;
  } catch (const std::exception& error) { std::cerr << "FAIL " << checks << ": " << error.what() << '\n'; fs::remove_all(root); return 1; }
}
