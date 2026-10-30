#include "v8trace/core.h"
#include <array>
#include <iomanip>
#include <random>
#include <sstream>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#include <aclapi.h>
#include <sddl.h>
#include <shellapi.h>
#else
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#include <mach-o/dyld.h>
#else
#include <openssl/evp.h>
#endif
#endif

namespace v8trace {
namespace {
#ifdef _WIN32
std::wstring wide(std::string_view s) { auto p = path(s); return p.native(); }
std::wstring argument(std::string_view s) {
  std::wstring out = L"\""; unsigned slashes = 0;
  for (auto c : wide(s)) { if (c == L'\\') ++slashes; else { out.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\'); slashes = 0; out += c; } }
  out.append(slashes * 2, L'\\'); return out + L'"';
}
void check(bool good, std::string_view operation) { if (!good) throw std::runtime_error(std::string(operation) + ": Win32 " + std::to_string(GetLastError())); }
#endif
class Digest {
 public:
  Digest() {
#ifdef _WIN32
    if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 || BCryptCreateHash(algorithm_, &hash_, nullptr, 0, nullptr, 0, 0) < 0) throw std::runtime_error("SHA256 initialization failed");
#elif defined(__APPLE__)
    CC_SHA256_Init(&context_);
#else
    context_ = EVP_MD_CTX_new(); if (!context_ || EVP_DigestInit_ex(context_, EVP_sha256(), nullptr) != 1) throw std::runtime_error("SHA256 initialization failed");
#endif
  }
  ~Digest() {
#ifdef _WIN32
    if (hash_) BCryptDestroyHash(hash_); if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0);
#elif !defined(__APPLE__)
    EVP_MD_CTX_free(context_);
#endif
  }
  void update(std::string_view data) {
#ifdef _WIN32
    if (data.size() > ULONG_MAX || BCryptHashData(hash_, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())), ULONG(data.size()), 0) < 0) throw std::runtime_error("SHA256 update failed");
#elif defined(__APPLE__)
    CC_SHA256_Update(&context_, data.data(), CC_LONG(data.size()));
#else
    if (EVP_DigestUpdate(context_, data.data(), data.size()) != 1) throw std::runtime_error("SHA256 update failed");
#endif
  }
  std::string finish() {
    std::array<unsigned char, 32> result{};
#ifdef _WIN32
    if (BCryptFinishHash(hash_, result.data(), ULONG(result.size()), 0) < 0) throw std::runtime_error("SHA256 finish failed");
#elif defined(__APPLE__)
    CC_SHA256_Final(result.data(), &context_);
#else
    unsigned length = 0; if (EVP_DigestFinal_ex(context_, result.data(), &length) != 1 || length != 32) throw std::runtime_error("SHA256 finish failed");
#endif
    constexpr char digits[] = "0123456789abcdef"; std::string out; for (auto b : result) { out += digits[b >> 4]; out += digits[b & 15]; } return out;
  }
 private:
#ifdef _WIN32
  BCRYPT_ALG_HANDLE algorithm_ = nullptr;
  BCRYPT_HASH_HANDLE hash_ = nullptr;
#elif defined(__APPLE__)
  CC_SHA256_CTX context_{};
#else
  EVP_MD_CTX* context_ = nullptr;
#endif
};
}
fs::path path(std::string_view value) { return fs::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size())); }
std::string utf8(const fs::path& value) { auto s = value.generic_u8string(); return {reinterpret_cast<const char*>(s.data()), s.size()}; }
fs::path logical_path(const fs::path& value) {
#ifdef _WIN32
  auto preferred = value; preferred.make_preferred(); const auto& name = preferred.native();
  if (name.size() >= 8 && CompareStringOrdinal(name.c_str(), 8, L"\\\\?\\UNC\\", 8, TRUE) == CSTR_EQUAL) return fs::path(L"\\\\" + name.substr(8));
  if (name.size() >= 7 && name.starts_with(L"\\\\?\\") && name[5] == L':' && name[6] == L'\\') return fs::path(name.substr(4));
#endif
  return value;
}
fs::path io_path(const fs::path& value) {
#ifdef _WIN32
  if (value.empty()) throw std::runtime_error("Empty filesystem path");
  auto absolute = fs::absolute(logical_path(value)).lexically_normal(); absolute.make_preferred(); auto name = absolute.native();
  if (name.find(L'\0') != name.npos || name.starts_with(L"\\\\.\\") || name.starts_with(L"\\\\?\\")) throw std::runtime_error("Unsupported Windows filesystem namespace");
  if (name.starts_with(L"\\\\")) name = L"\\\\?\\UNC\\" + name.substr(2);
  else if (name.size() >= 3 && name[1] == L':' && name[2] == L'\\') name = L"\\\\?\\" + name;
  else throw std::runtime_error("Expected an absolute drive or UNC filesystem path");
  if (name.size() >= 32767) throw std::runtime_error("Windows filesystem path exceeds Unicode length limit");
  return fs::path(name);
#else
  return value;
#endif
}
bool inside(const fs::path& root, const fs::path& file) { auto r = fs::weakly_canonical(io_path(root)), f = fs::weakly_canonical(io_path(file)); auto ri = r.begin(), fi = f.begin(); for (; ri != r.end(); ++ri, ++fi) { if (fi == f.end()) return false;
#ifdef _WIN32
  if (lower(utf8(*ri)) != lower(utf8(*fi))) return false;
#else
  if (*ri != *fi) return false;
#endif
  } return fi != f.end(); }
std::string read_file(const fs::path& file, uint64_t maximum) { auto length = fs::file_size(file); if (length > maximum || length > SIZE_MAX) throw std::runtime_error("File exceeds byte budget: " + utf8(file)); std::ifstream input(file, std::ios::binary); if (!input) throw std::runtime_error("Open failed: " + utf8(file)); std::string data(size_t(length), '\0'); input.read(data.data(), std::streamsize(length)); if (uint64_t(input.gcount()) != length) throw std::runtime_error("Short file read"); return data; }
void write_file(const fs::path& file, std::string_view data, bool exclusive) {
  if (exclusive && fs::exists(file)) throw std::runtime_error("Output already exists: " + utf8(file));
  fs::create_directories(file.parent_path()); std::ofstream output(file, std::ios::binary | std::ios::trunc); if (!output) throw std::runtime_error("Open output failed: " + utf8(file)); output.write(data.data(), std::streamsize(data.size())); output.flush(); if (!output) throw std::runtime_error("Short file write: " + utf8(file));
}
void write_json(const fs::path& file, const Json& value) {
  auto temp = file; temp += ".tmp-" + uuid(); write_file(temp, dump(value, 2) + "\n", true);
#ifdef _WIN32
  if (!MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { fs::remove(temp); throw std::runtime_error("Atomic JSON replacement failed"); }
#else
  fs::rename(temp, file);
#endif
}
std::string now() { auto point = std::chrono::system_clock::now(); auto time = std::chrono::system_clock::to_time_t(point); std::tm result{};
#ifdef _WIN32
  gmtime_s(&result, &time);
#else
  gmtime_r(&time, &result);
#endif
  std::ostringstream out; out << std::put_time(&result, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3) << std::setfill('0') << (std::chrono::duration_cast<std::chrono::milliseconds>(point.time_since_epoch()).count() % 1000) << 'Z'; return out.str(); }
std::string uuid() { std::random_device random; std::array<unsigned char, 16> bytes{}; for (auto& c : bytes) c = static_cast<unsigned char>(random()); bytes[6] = (bytes[6] & 15) | 64; bytes[8] = (bytes[8] & 63) | 128; std::string out; constexpr char h[] = "0123456789abcdef"; for (size_t i = 0; i < bytes.size(); ++i) { if (i == 4 || i == 6 || i == 8 || i == 10) out += '-'; out += h[bytes[i] >> 4]; out += h[bytes[i] & 15]; } return out; }
std::string sha256(std::string_view data) { Digest digest; digest.update(data); return digest.finish(); }
std::string reader_sha256(const std::function<size_t(char*, size_t)>& reader) { std::array<char, 65536> buffer; Digest digest; size_t count; while ((count = reader(buffer.data(), buffer.size()))) { if (count > buffer.size()) throw std::runtime_error("Hash reader exceeded requested capacity"); digest.update({buffer.data(), count}); } return digest.finish(); }
std::string file_sha256(const fs::path& file) { std::ifstream input(file, std::ios::binary); if (!input) throw std::runtime_error("Hash open failed: " + utf8(file)); std::array<char, 65536> buffer; Digest digest; while (input) { input.read(buffer.data(), buffer.size()); digest.update({buffer.data(), size_t(input.gcount())}); } if (!input.eof()) throw std::runtime_error("Hash read failed"); return digest.finish(); }
std::string base64(std::string_view data) { constexpr char h[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"; std::string out; for (size_t i = 0; i < data.size(); i += 3) { uint32_t n = uint8_t(data[i]) << 16; if (i + 1 < data.size()) n |= uint8_t(data[i + 1]) << 8; if (i + 2 < data.size()) n |= uint8_t(data[i + 2]); out += h[(n >> 18) & 63]; out += h[(n >> 12) & 63]; out += i + 1 < data.size() ? h[(n >> 6) & 63] : '='; out += i + 2 < data.size() ? h[n & 63] : '='; } return out; }
std::string unbase64(std::string_view data) { if (data.size() % 4) throw std::runtime_error("Invalid base64 length"); std::string out; constexpr std::string_view h = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"; for (size_t i = 0; i < data.size(); i += 4) { uint32_t n = 0; for (int j = 0; j < 4; ++j) { auto c = data[i + j]; auto at = h.find(c); if (at == h.npos && c != '=') throw std::runtime_error("Invalid base64 character"); n = (n << 6) | uint32_t(c == '=' ? 0 : at); } out += char(n >> 16); if (data[i + 2] != '=') out += char(n >> 8); if (data[i + 3] != '=') out += char(n); } if (base64(out) != data) throw std::runtime_error("Noncanonical base64"); return out; }

struct Process::Impl {
  uint64_t pid = 0; std::optional<Json> exit;
#ifdef _WIN32
  HANDLE process = nullptr, job = nullptr;
#endif
};
Process::Process() : impl_(std::make_unique<Impl>()) {}
Process::~Process() { if (impl_->pid && !poll()) { try { kill(); } catch (...) {} }
#ifdef _WIN32
  if (impl_->process) CloseHandle(impl_->process); if (impl_->job) CloseHandle(impl_->job);
#endif
}
void Process::start(const fs::path& executable, const std::vector<std::string>& args, const fs::path& directory, const fs::path& stdout_file, const fs::path& stderr_file) {
  if (impl_->pid) throw std::runtime_error("Process already started");
  std::map<std::string, std::string> observations;
  for (auto& arg : args) for (auto [flag, name] : std::vector<std::pair<std::string, std::string>>{{"--v8-trace-object-depth=", "V8_TRACE_OBJECT_DEPTH"}, {"--v8-trace-object-nodes=", "V8_TRACE_OBJECT_NODES"}, {"--v8-trace-object-properties=", "V8_TRACE_OBJECT_PROPERTIES"}}) if (arg.starts_with(flag)) observations[name] = arg.substr(flag.size());
#ifdef _WIN32
  std::wstring environment; auto inherited = GetEnvironmentStringsW(); if (!inherited) throw std::runtime_error("Child environment lookup failed"); std::map<std::wstring, std::wstring> variables;
  for (auto item = inherited; *item; item += wcslen(item) + 1) { std::wstring value(item); auto equal = value.find(L'=', value[0] == L'=' ? 1 : 0); if (equal == value.npos) continue; auto name = value.substr(0, equal); auto normalized = lower(utf8(name)); if (normalized != "v8_trace_object_depth" && normalized != "v8_trace_object_nodes" && normalized != "v8_trace_object_properties") variables[name] = value.substr(equal + 1); }
  FreeEnvironmentStringsW(inherited); for (auto& [name, value] : observations) variables[wide(name)] = wide(value); for (auto& [name, value] : variables) { environment += name + L'=' + value; environment += L'\0'; } environment += L'\0';
  SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  auto output = CreateFileW(stdout_file.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  auto error = CreateFileW(stderr_file.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  auto input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr);
  if (output == INVALID_HANDLE_VALUE || error == INVALID_HANDLE_VALUE || input == INVALID_HANDLE_VALUE) { if (output != INVALID_HANDLE_VALUE) CloseHandle(output); if (error != INVALID_HANDLE_VALUE) CloseHandle(error); if (input != INVALID_HANDLE_VALUE) CloseHandle(input); throw std::runtime_error("Browser stdio open failed"); }
  std::wstring command = argument(utf8(executable)); for (auto& arg : args) command += L" " + argument(arg);
  STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup); startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES; startup.StartupInfo.hStdInput = input; startup.StartupInfo.hStdOutput = output; startup.StartupInfo.hStdError = error;
  SIZE_T length = 0; InitializeProcThreadAttributeList(nullptr, 1, 0, &length); std::vector<unsigned char> storage(length); startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
  check(InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &length), "Initialize handle list"); HANDLE handles[] = {input, output, error};
  check(UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof(handles), nullptr, nullptr), "Restrict inherited handles");
  PROCESS_INFORMATION information{}; auto ok = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE, EXTENDED_STARTUPINFO_PRESENT | CREATE_NEW_PROCESS_GROUP | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT, environment.data(), directory.c_str(), &startup.StartupInfo, &information); auto code = GetLastError(); DeleteProcThreadAttributeList(startup.lpAttributeList); CloseHandle(input); CloseHandle(output); CloseHandle(error);
  if (!ok) throw std::runtime_error("CreateProcess failed: " + std::to_string(code));
  impl_->process = information.hProcess; impl_->pid = information.dwProcessId; impl_->job = CreateJobObjectW(nullptr, nullptr);
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!impl_->job || !SetInformationJobObject(impl_->job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) || !AssignProcessToJobObject(impl_->job, impl_->process)) { TerminateProcess(impl_->process, 1); CloseHandle(information.hThread); throw std::runtime_error("Owned browser job initialization failed"); }
  ResumeThread(information.hThread); CloseHandle(information.hThread);
#else
  posix_spawn_file_actions_t actions; posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, stdout_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, stderr_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  posix_spawn_file_actions_addchdir_np(&actions, directory.c_str());
  posix_spawnattr_t attributes; posix_spawnattr_init(&attributes); posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP); posix_spawnattr_setpgroup(&attributes, 0);
  std::vector<std::string> values{utf8(executable)}; values.insert(values.end(), args.begin(), args.end()); std::vector<char*> arguments; for (auto& value : values) arguments.push_back(value.data()); arguments.push_back(nullptr);
  std::vector<std::string> environment; for (auto item = environ; *item; ++item) { std::string value(*item); auto name = value.substr(0, value.find('=')); if (name != "V8_TRACE_OBJECT_DEPTH" && name != "V8_TRACE_OBJECT_NODES" && name != "V8_TRACE_OBJECT_PROPERTIES") environment.push_back(std::move(value)); } for (auto& [name, value] : observations) environment.push_back(name + '=' + value); std::vector<char*> entries; for (auto& value : environment) entries.push_back(value.data()); entries.push_back(nullptr);
  pid_t pid = 0; auto result = posix_spawn(&pid, executable.c_str(), &actions, &attributes, arguments.data(), entries.data()); posix_spawn_file_actions_destroy(&actions); posix_spawnattr_destroy(&attributes); if (result) throw std::runtime_error("posix_spawn failed: " + std::to_string(result)); impl_->pid = uint64_t(pid);
#endif
}
std::optional<Json> Process::poll() { if (!impl_->pid) return std::nullopt; if (impl_->exit) return impl_->exit;
#ifdef _WIN32
  if (WaitForSingleObject(impl_->process, 0) == WAIT_TIMEOUT) return std::nullopt; DWORD code = 0; check(GetExitCodeProcess(impl_->process, &code), "Get process exit"); impl_->exit = Json{{"code", uint64_t(code)}, {"signal", nullptr}};
#else
  int status = 0; auto result = waitpid(pid_t(impl_->pid), &status, WNOHANG); if (!result) return std::nullopt; if (result < 0) throw std::runtime_error("waitpid failed"); impl_->exit = Json{{"code", WIFEXITED(status) ? Json(WEXITSTATUS(status)) : Json(nullptr)}, {"signal", WIFSIGNALED(status) ? Json(WTERMSIG(status)) : Json(nullptr)}};
#endif
  return impl_->exit; }
void Process::close() { if (!impl_->pid || poll()) return;
#ifdef _WIN32
  EnumWindows([](HWND window, LPARAM parameter) -> BOOL { auto pid = static_cast<DWORD>(parameter); DWORD owner = 0; GetWindowThreadProcessId(window, &owner); if (owner == pid) PostMessageW(window, WM_CLOSE, 0, 0); return TRUE; }, LPARAM(impl_->pid));
#else
  ::kill(-pid_t(impl_->pid), SIGTERM);
#endif
}
void Process::kill() { if (!impl_->pid) return;
#ifdef _WIN32
  if (impl_->job) check(TerminateJobObject(impl_->job, 1), "Terminate owned job");
#else
  if (::kill(-pid_t(impl_->pid), SIGKILL) && errno != ESRCH) throw std::runtime_error("Kill owned process group failed");
#endif
}
uint64_t Process::id() const { return impl_->pid; }
fs::path executable_path() {
#ifdef _WIN32
  std::wstring buffer(32768, L'\0'); auto n = GetModuleFileNameW(nullptr, buffer.data(), DWORD(buffer.size())); if (!n || n >= buffer.size()) throw std::runtime_error("Executable path lookup failed"); buffer.resize(n); return buffer;
#elif defined(__APPLE__)
  uint32_t length = 0; _NSGetExecutablePath(nullptr, &length); std::string buffer(length, '\0'); if (_NSGetExecutablePath(buffer.data(), &length)) throw std::runtime_error("Executable path lookup failed"); return fs::canonical(buffer.c_str());
#else
  return fs::canonical("/proc/self/exe");
#endif
}
std::vector<std::string> platform_arguments(int argc, char** argv) {
  std::vector<std::string> out;
#ifdef _WIN32
  int count = 0; auto args = CommandLineToArgvW(GetCommandLineW(), &count); if (!args) throw std::runtime_error("Argument decoding failed"); for (int i = 0; i < count; ++i) out.push_back(utf8(args[i])); LocalFree(args); (void)argc; (void)argv;
#else
  for (int i = 0; i < argc; ++i) out.emplace_back(argv[i]);
#endif
  return out;
}
std::string platform_name() {
#ifdef _WIN32
  return "windows";
#elif defined(__APPLE__)
  return "macos";
#else
  return "linux";
#endif
}
void prepare_permissions(const fs::path& executable) {
#ifdef _WIN32
  PSID sid = nullptr; if (!ConvertStringSidToSidW(L"S-1-15-2-2", &sid)) throw std::runtime_error("LPAC SID lookup failed");
  PACL acl = nullptr, updated = nullptr; PSECURITY_DESCRIPTOR security = nullptr; auto directory = executable.parent_path();
  auto result = GetNamedSecurityInfoW(directory.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &acl, nullptr, &security);
  bool permitted = false;
  if (!result && acl) {
    TRUSTEEW trustee{}; BuildTrusteeWithSidW(&trustee, sid); ACCESS_MASK effective = 0, inheritable = 0;
    auto lookup = GetEffectiveRightsFromAclW(acl, &trustee, &effective);
    GENERIC_MAPPING mapping{FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS};
    for (DWORD index = 0; index < acl->AceCount; ++index) {
      void* entry = nullptr; if (!GetAce(acl, index, &entry)) break;
      auto* header = static_cast<ACE_HEADER*>(entry);
      if (header->AceType != ACCESS_ALLOWED_ACE_TYPE || (header->AceFlags & (CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE)) != (CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE)) continue;
      auto* allowed = static_cast<ACCESS_ALLOWED_ACE*>(entry);
      if (!EqualSid(&allowed->SidStart, sid)) continue;
      auto mask = allowed->Mask; MapGenericMask(&mask, &mapping); inheritable |= mask;
    }
    MapGenericMask(&effective, &mapping); auto required = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;
    permitted = !lookup && (effective & required) == required && (inheritable & required) == required;
  }
  if (!result && !permitted) { EXPLICIT_ACCESSW access{}; access.grfAccessPermissions = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE; access.grfAccessMode = GRANT_ACCESS; access.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT; access.Trustee.TrusteeForm = TRUSTEE_IS_SID; access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(sid); result = SetEntriesInAclW(1, &access, acl, &updated); if (!result) result = SetNamedSecurityInfoW(const_cast<LPWSTR>(directory.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, updated, nullptr); }
  if (updated) LocalFree(updated); if (security) LocalFree(security); LocalFree(sid); if (result) throw std::runtime_error("Browser LPAC ACL initialization failed: " + std::to_string(result));
#else
  if (::access(executable.c_str(), X_OK)) throw std::runtime_error("Browser executable lacks execute permission");
#endif
}
}
