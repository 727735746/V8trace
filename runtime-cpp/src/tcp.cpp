#include "v8trace/core.h"
#include <array>
#include <algorithm>
#include <cstring>
#include <regex>
#include <thread>
#include <zstd.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <io.h>
#else
#include <netdb.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#endif

namespace v8trace {
namespace {
#ifdef _WIN32
using Handle = SOCKET;
constexpr Handle invalid = INVALID_SOCKET;
void close_socket(Handle handle) { closesocket(handle); }
#else
using Handle = int;
constexpr Handle invalid = -1;
void close_socket(Handle handle) { ::close(handle); }
#endif
struct Network {
  Network() {
#ifdef _WIN32
    WSADATA value{}; if (WSAStartup(MAKEWORD(2, 2), &value)) throw std::runtime_error("TCP initialization failed");
#endif
  }
  ~Network() {
#ifdef _WIN32
    WSACleanup();
#endif
  }
};
class Socket {
 public:
  Handle handle = invalid;
  const std::atomic_bool* stop = nullptr;
  Socket() = default;
  explicit Socket(Handle value) : handle(value) { configure(); }
  ~Socket() { if (handle != invalid) close_socket(handle); }
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  void configure() {
#ifdef _WIN32
    DWORD timeout = 15000; setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)); setsockopt(handle, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    timeval timeout{15, 0}; setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)); setsockopt(handle, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#ifdef SO_NOSIGPIPE
    int flag = 1; setsockopt(handle, SOL_SOCKET, SO_NOSIGPIPE, &flag, sizeof(flag));
#endif
#endif
  }
  void connect(const std::string& host, uint16_t port) {
    addrinfo hints{}; hints.ai_socktype = SOCK_STREAM; hints.ai_family = AF_UNSPEC; addrinfo* addresses = nullptr; auto service = std::to_string(port); if (getaddrinfo(host.c_str(), service.c_str(), &hints, &addresses)) throw std::runtime_error("TCP host lookup failed");
    for (auto address = addresses; address; address = address->ai_next) {
      if (stop && *stop) break; handle = socket(address->ai_family, address->ai_socktype, address->ai_protocol); if (handle == invalid) continue;
#ifdef _WIN32
      u_long nonblocking = 1; ioctlsocket(handle, FIONBIO, &nonblocking);
#else
      auto mode = fcntl(handle, F_GETFL, 0); fcntl(handle, F_SETFL, mode | O_NONBLOCK);
#endif
      auto connected = ::connect(handle, address->ai_addr, int(address->ai_addrlen)) == 0;
      auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
      while (!connected && std::chrono::steady_clock::now() < deadline && !(stop && *stop)) { fd_set writable; FD_ZERO(&writable); FD_SET(handle, &writable); timeval timeout{0, 200000}; auto ready = select(int(handle + 1), nullptr, &writable, nullptr, &timeout); if (ready < 0) break; if (!ready) continue; int code = 0;
#ifdef _WIN32
        int length = sizeof(code);
#else
        socklen_t length = sizeof(code);
#endif
        if (!getsockopt(handle, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&code), &length) && !code) connected = true; break;
      }
#ifdef _WIN32
      nonblocking = 0; ioctlsocket(handle, FIONBIO, &nonblocking);
#else
      fcntl(handle, F_SETFL, mode);
#endif
      if (connected) { configure(); break; } close_socket(handle); handle = invalid;
    }
    freeaddrinfo(addresses); if (handle == invalid) throw std::runtime_error("TCP connection failed: " + host);
  }
  void send(std::string_view data) { size_t at = 0; while (at < data.size()) {
#ifdef MSG_NOSIGNAL
    constexpr int flags = MSG_NOSIGNAL;
#else
    constexpr int flags = 0;
#endif
    if (stop && *stop) throw std::runtime_error("TCP transfer cancelled; source retained"); auto n = ::send(handle, data.data() + at, int(std::min<size_t>(data.size() - at, 65536)), flags); if (n <= 0) throw std::runtime_error("TCP send failed or timed out"); at += size_t(n); } }
  std::string read(size_t length) { std::string data(length, '\0'); size_t at = 0; while (at < length) { if (stop && *stop) throw std::runtime_error("TCP transfer cancelled; source retained"); auto n = recv(handle, data.data() + at, int(std::min<size_t>(length - at, 65536)), 0); if (n <= 0) throw std::runtime_error("TCP peer closed or timed out"); at += size_t(n); } return data; }
  void frame(Json header, std::string_view payload = {}) {
    if (payload.size() > 8 * 1024 * 1024) throw std::runtime_error("TCP payload exceeds 8 MiB"); header["wireBytes"] = payload.size(); auto data = dump(header); if (data.size() > 65536) throw std::runtime_error("TCP header exceeds 64 KiB"); std::string prefix = "V8T1"; auto n = uint32_t(data.size()); for (int shift = 24; shift >= 0; shift -= 8) prefix += char(n >> shift); send(prefix); send(data); send(payload);
  }
  std::pair<Json, std::string> frame() { auto prefix = read(8); if (prefix.substr(0, 4) != "V8T1") throw std::runtime_error("TCP protocol magic mismatch"); uint32_t size = 0; for (size_t i = 4; i < 8; ++i) size = (size << 8) | uint8_t(prefix[i]); if (!size || size > 65536) throw std::runtime_error("TCP header length invalid"); auto header = parse(read(size)); auto length = field(header, "wireBytes"); if (!safe_uint(length) || length > 8 * 1024 * 1024) throw std::runtime_error("TCP payload length invalid"); return {header, read(length.get<size_t>())}; }
  Json ack() { auto [header, payload] = frame(); if (!payload.empty() || str(header, "op") != "ack") throw std::runtime_error("TCP acknowledgement failed: " + str(header, "error", dump(header))); return header; }
};
bool loopback(const std::string& host) { return host == "127.0.0.1" || host == "::1" || host == "localhost"; }
std::string token(const Json& options) { auto file = str(options, "tokenFile"); if (file.empty()) return ""; auto value = trim(read_file(path(file), 4096)); if (value.size() < 16) throw std::runtime_error("TCP token must contain at least 16 bytes"); return value; }
fs::path safe_file(const fs::path& root, const std::string& name) {
  auto relative = path(name); if (name.empty() || name.find('\\') != name.npos || name.find(':') != name.npos || relative.is_absolute() || relative.has_root_name()) throw std::runtime_error("Invalid TCP relative path");
  for (auto& part : relative) {
    auto component = utf8(part); if (component == "." || component == ".." || component.empty() || component.back() == '.' || component.back() == ' ' || component.find_first_of("<>\"|?*") != component.npos || std::any_of(component.begin(), component.end(), [](unsigned char c) { return c < 32; })) throw std::runtime_error("TCP unsafe path component rejected");
    auto stem = lower(component.substr(0, component.find('.'))); if (std::regex_match(stem, std::regex("con|prn|aux|nul|com[1-9]|lpt[1-9]"))) throw std::runtime_error("TCP Windows device path rejected");
  }
  auto file = root / relative; if (!inside(root, file)) throw std::runtime_error("TCP path leaves destination"); auto current = root; for (auto& part : relative) { current /= part; if (fs::is_symlink(fs::symlink_status(current))) throw std::runtime_error("TCP symlink path rejected"); }
  return file;
}
void durable_append(const fs::path& file, std::string_view bytes, uint64_t offset) {
#ifdef _WIN32
  auto handle = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr); if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("TCP destination open failed"); struct Close { HANDLE h; ~Close() { CloseHandle(h); } } close{handle}; LARGE_INTEGER position; position.QuadPart = LONGLONG(offset); if (!SetFilePointerEx(handle, position, nullptr, FILE_BEGIN)) throw std::runtime_error("TCP destination seek failed"); size_t at = 0; while (at < bytes.size()) { DWORD written = 0; if (!WriteFile(handle, bytes.data() + at, DWORD(bytes.size() - at), &written, nullptr) || !written) throw std::runtime_error("TCP destination write failed"); at += written; } if (!FlushFileBuffers(handle)) throw std::runtime_error("TCP destination flush failed");
#else
  int handle = ::open(file.c_str(), O_WRONLY | O_NOFOLLOW); if (handle < 0) throw std::runtime_error("TCP destination open failed"); struct Close { int h; ~Close() { ::close(h); } } close{handle}; size_t at = 0; while (at < bytes.size()) { auto n = pwrite(handle, bytes.data() + at, bytes.size() - at, off_t(offset + at)); if (n <= 0) throw std::runtime_error("TCP destination write failed"); at += size_t(n); } if (fsync(handle)) throw std::runtime_error("TCP destination flush failed");
#endif
}
void handle_client(Socket& socket, const fs::path& root, const std::string& secret, uint64_t quota, const Sink& progress, bool& completed) {
  auto [hello, data] = socket.frame(); if (str(hello, "op") != "hello" || str(hello, "protocol") != "v8trace/tcp/1" || str(hello, "token") != secret) throw std::runtime_error("TCP handshake rejected"); auto id = str(hello, "id"); if (id.size() != 36 || id.find_first_not_of("0123456789abcdef-") != id.npos) throw std::runtime_error("TCP transfer ID invalid");
  auto incoming = root / ".incoming" / id; auto destination = root / id; fs::create_directories(incoming); if (!inside(root, incoming)) throw std::runtime_error("TCP incoming path leaves root"); auto manifest = parse(data); if (!manifest.is_array() || manifest.size() > 100000) throw std::runtime_error("TCP manifest invalid"); std::map<std::string, Json> files; uint64_t total = 0;
  std::set<std::string> portable_names;
  for (auto& file : manifest) { auto name = str(file, "path"); safe_file(incoming / "data", name); if (!safe_uint(field(file, "bytes")) || file.at("bytes").get<uint64_t>() > quota - total || !std::regex_match(str(file, "sha256"), std::regex("[a-f0-9]{64}")) || !portable_names.insert(lower(name)).second || !files.emplace(name, file).second) throw std::runtime_error("TCP manifest file invalid, case-colliding or quota exceeded"); total += file.at("bytes").get<uint64_t>(); }
  auto identity = sha256(data); if (str(hello, "manifestSha256") != identity) throw std::runtime_error("TCP manifest SHA256 mismatch");
  if (fs::exists(incoming / "manifest.json")) { if (read_file(incoming / "manifest.json") != data) throw std::runtime_error("TCP resumed manifest changed"); } else write_file(incoming / "manifest.json", data, true);
  if (fs::exists(destination)) { for (auto& [name, item] : files) { auto file = safe_file(destination, name); if (!fs::is_regular_file(file) || fs::file_size(file) != item.at("bytes") || file_sha256(file) != str(item, "sha256")) throw std::runtime_error("TCP committed destination differs"); } socket.frame({{"op", "ack"}, {"complete", true}, {"destination", utf8(destination)}}); completed = true; return; }
  fs::create_directories(incoming / "data"); socket.frame({{"op", "ack"}, {"complete", false}}); std::string active;
  while (true) {
    auto [header, payload] = socket.frame(); auto op = str(header, "op");
    if (op == "file") { active = str(header, "path"); if (!files.contains(active) || !payload.empty()) throw std::runtime_error("TCP unknown file"); auto file = safe_file(incoming / "data", active); fs::create_directories(file.parent_path()); if (!fs::exists(file)) write_file(file, "", true); auto size = fs::file_size(file); if (size > files.at(active).at("bytes").get<uint64_t>()) throw std::runtime_error("TCP partial file exceeds manifest"); if (size == files.at(active).at("bytes").get<uint64_t>() && file_sha256(file) != str(files.at(active), "sha256")) { write_file(file, ""); size = 0; } socket.frame({{"op", "ack"}, {"offset", size}}); }
    else if (op == "chunk") {
      if (active.empty() || !safe_uint(field(header, "offset")) || !safe_uint(field(header, "rawBytes")) || header.at("rawBytes") > 4 * 1024 * 1024 || str(header, "codec") != "zstd") throw std::runtime_error("TCP chunk header invalid");
      auto file = safe_file(incoming / "data", active); auto offset = header.at("offset").get<uint64_t>(); auto expected = files.at(active).at("bytes").get<uint64_t>(); auto length = header.at("rawBytes").get<size_t>(); if (offset != fs::file_size(file) || offset > expected || length > expected - offset || !length) throw std::runtime_error("TCP chunk offset invalid");
      std::string bytes(length, '\0'); auto result = ZSTD_decompress(bytes.data(), bytes.size(), payload.data(), payload.size()); if (ZSTD_isError(result) || result != length || sha256(bytes) != str(header, "sha256")) throw std::runtime_error("TCP chunk integrity mismatch"); durable_append(file, bytes, offset); socket.frame({{"op", "ack"}, {"offset", offset + length}});
      if (progress) progress({{"type", "transfer-progress"}, {"id", id}, {"path", active}, {"offset", offset + length}, {"bytes", expected}});
    } else if (op == "end-file") { if (active.empty()) throw std::runtime_error("TCP file not active"); auto file = safe_file(incoming / "data", active); auto& item = files.at(active); if (fs::file_size(file) != item.at("bytes") || file_sha256(file) != str(item, "sha256")) throw std::runtime_error("TCP file integrity mismatch"); socket.frame({{"op", "ack"}}); active.clear(); }
    else if (op == "commit") { if (!active.empty() || !payload.empty()) throw std::runtime_error("TCP transfer incomplete"); for (auto& [name, item] : files) { auto file = safe_file(incoming / "data", name); if (!fs::is_regular_file(file) || fs::file_size(file) != item.at("bytes") || file_sha256(file) != str(item, "sha256")) throw std::runtime_error("TCP final integrity mismatch"); } fs::rename(incoming / "data", destination); write_json(incoming / "receipt.json", {{"protocol", "v8trace/tcp/1"}, {"manifestSha256", identity}, {"completedAt", now()}, {"destination", utf8(destination)}}); socket.frame({{"op", "ack"}, {"complete", true}, {"destination", utf8(destination)}}); completed = true; return; }
    else throw std::runtime_error("Unknown TCP operation");
  }
}
}
Json tcp_send(const fs::path& directory, const Json& options, const Sink& progress, const std::atomic_bool* stop) {
  Network network; auto root = fs::canonical(directory); if (!fs::is_directory(root)) throw std::runtime_error("TCP capture directory missing"); auto host = str(options, "host", "127.0.0.1"), secret = token(options); if (!loopback(host) && secret.empty()) throw std::runtime_error("Remote TCP requires tokenFile"); auto port_value = field(options, "port", 17880); if (!safe_uint(port_value, true) || port_value > 65535) throw std::runtime_error("TCP port invalid"); auto port = port_value.get<uint16_t>();
  std::vector<fs::path> paths; for (auto& entry : fs::recursive_directory_iterator(root)) { if (fs::is_symlink(entry.symlink_status())) throw std::runtime_error("TCP source symlink rejected"); if (entry.is_regular_file()) { auto relative = entry.path().lexically_relative(root); if (*relative.begin() == "browser-profile") continue; if (relative == "tcp-transfer.json") continue; paths.push_back(relative); } }
  std::sort(paths.begin(), paths.end()); Json manifest = Json::array(); uint64_t total = 0; for (auto& relative : paths) { auto file = safe_file(root, utf8(relative)); auto length = fs::file_size(file); total += length; manifest.push_back({{"path", utf8(relative)}, {"bytes", length}, {"sha256", file_sha256(file)}}); }
  auto data = dump(manifest); if (data.size() > 8 * 1024 * 1024) throw std::runtime_error("TCP manifest exceeds 8 MiB"); auto identity = sha256(data); std::string id = str(options, "transferId"); if (id.empty()) id = identity.substr(0, 8) + '-' + identity.substr(8, 4) + '-' + identity.substr(12, 4) + '-' + identity.substr(16, 4) + '-' + identity.substr(20, 12);
  uint64_t wire = 0; std::string destination, error;
  for (unsigned attempt = 0; attempt < 4; ++attempt) {
    try {
      Socket socket; socket.stop = stop; socket.connect(host, port); socket.frame({{"op", "hello"}, {"protocol", "v8trace/tcp/1"}, {"id", id}, {"token", secret}, {"manifestSha256", identity}}, data); auto hello = socket.ack();
      if (field(hello, "complete") == true) destination = str(hello, "destination");
      else {
        for (auto& item : manifest) {
          auto file = safe_file(root, str(item, "path")); if (fs::file_size(file) != item.at("bytes")) throw std::runtime_error("TCP source changed during transfer"); socket.frame({{"op", "file"}, {"path", item.at("path")}}); auto offset = socket.ack().at("offset").get<uint64_t>(); auto length = item.at("bytes").get<uint64_t>(); if (offset > length) throw std::runtime_error("TCP resume offset exceeds source"); std::ifstream input(file, std::ios::binary); input.seekg(std::streamoff(offset));
          while (offset < length) { std::string chunk(size_t(std::min<uint64_t>(4 * 1024 * 1024, length - offset)), '\0'); input.read(chunk.data(), std::streamsize(chunk.size())); if (size_t(input.gcount()) != chunk.size()) throw std::runtime_error("TCP source short read"); std::string encoded(ZSTD_compressBound(chunk.size()), '\0'); auto n = ZSTD_compress(encoded.data(), encoded.size(), chunk.data(), chunk.size(), 3); if (ZSTD_isError(n)) throw std::runtime_error("TCP compression failed"); encoded.resize(n); socket.frame({{"op", "chunk"}, {"offset", offset}, {"rawBytes", chunk.size()}, {"codec", "zstd"}, {"sha256", sha256(chunk)}}, encoded); wire += encoded.size(); auto ack = socket.ack(); offset += chunk.size(); if (ack.at("offset") != offset) throw std::runtime_error("TCP acknowledged wrong offset"); if (progress) progress({{"type", "transfer-progress"}, {"id", id}, {"path", item.at("path")}, {"offset", offset}, {"bytes", length}, {"compressedPayloadBytes", wire}}); }
          socket.frame({{"op", "end-file"}}); socket.ack();
        }
        socket.frame({{"op", "commit"}}); auto receipt = socket.ack(); if (field(receipt, "complete") != true) throw std::runtime_error("TCP commit not acknowledged"); destination = str(receipt, "destination");
      }
      return {{"protocol", "v8trace/tcp/1"}, {"id", id}, {"conclusion", "transferred-and-sha256-verified"}, {"files", manifest.size()}, {"sourceBytes", total}, {"compressedPayloadBytesThisRun", wire}, {"compression", "zstd-3-lossless"}, {"destination", destination}, {"sourceRetained", true}, {"profileTransferred", false}, {"tls", false}, {"attempts", attempt + 1}};
    } catch (const std::exception& e) { error = e.what(); if (stop && *stop) throw; if (progress) progress({{"type", "transfer-retry"}, {"attempt", attempt + 1}, {"error", error}, {"sourceRetained", true}}); if (attempt < 3) std::this_thread::sleep_for(std::chrono::milliseconds(200 * (attempt + 1))); }
  }
  throw std::runtime_error("TCP transfer failed; source retained: " + error);
}
int tcp_receive(const Json& options, std::atomic_bool& stop, const Sink& progress) {
  Network network; auto bind = str(options, "bind", "127.0.0.1"), secret = token(options); if (!loopback(bind) && secret.empty()) throw std::runtime_error("Non-loopback TCP listener requires tokenFile"); auto root_text = str(options, "root"); if (root_text.empty()) throw std::runtime_error("TCP receiver root required"); auto root = fs::absolute(path(root_text)); fs::create_directories(root); root = fs::canonical(root); auto port = field(options, "port", 17880); if (!safe_uint(port, true) || port > 65535) throw std::runtime_error("TCP port invalid");
  addrinfo hints{}; hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM; hints.ai_flags = AI_PASSIVE; addrinfo* addresses = nullptr; auto service = text(port); if (getaddrinfo(bind.c_str(), service.c_str(), &hints, &addresses)) throw std::runtime_error("TCP bind address invalid"); Socket listener;
  for (auto address = addresses; address; address = address->ai_next) { listener.handle = socket(address->ai_family, address->ai_socktype, address->ai_protocol); if (listener.handle == invalid) continue; int flag = 1;
#ifdef _WIN32
    setsockopt(listener.handle, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&flag), sizeof(flag));
#else
    setsockopt(listener.handle, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
#endif
    if (::bind(listener.handle, address->ai_addr, int(address->ai_addrlen)) == 0 && listen(listener.handle, 8) == 0) break; close_socket(listener.handle); listener.handle = invalid; } freeaddrinfo(addresses); if (listener.handle == invalid) throw std::runtime_error("TCP listener bind failed"); if (progress) progress({{"type", "listening"}, {"bind", bind}, {"port", port}, {"root", utf8(root)}, {"tls", false}});
  while (!stop) { fd_set readable; FD_ZERO(&readable); FD_SET(listener.handle, &readable); timeval timeout{0, 200000}; auto ready = select(int(listener.handle + 1), &readable, nullptr, nullptr, &timeout); if (ready < 0) { if (stop) break; throw std::runtime_error("TCP listener select failed"); } if (!ready) continue; Socket client(accept(listener.handle, nullptr, nullptr)); if (client.handle == invalid) continue; bool complete = false; try { handle_client(client, root, secret, field(options, "maxBytes", 1099511627776ULL).get<uint64_t>(), progress, complete); } catch (const std::exception& e) { try { client.frame({{"op", "error"}, {"error", e.what()}}); } catch (...) {} if (progress) progress({{"type", "transfer-error"}, {"error", e.what()}, {"partialRetained", true}}); } if (complete && field(options, "once", false) == true) return 0; }
  return 0;
}
}
