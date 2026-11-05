#include "v8trace/trace_conversion.h"
#include <fstream>
#include <regex>
#include <thread>

namespace v8trace {
namespace {
void require_conversion(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
Json file_reference(const fs::path& file) {
  return {{"path", utf8(file)}, {"bytes", fs::file_size(file)}, {"sha256", file_sha256(file)}};
}
}
Json verify_perfetto_converter(const fs::path& directory) {
  auto manifest = directory / "trace-converter.json", executable = directory / "trace_processor_shell.exe";
  require_conversion(fs::is_regular_file(manifest) && !fs::is_symlink(manifest), "Pinned Perfetto converter manifest is missing or not ordinary");
  auto pin = load_json(manifest, 16384);
  require_conversion(str(pin, "protocol") == "v8trace/perfetto-converter/1" && str(pin, "platform") == "windows-amd64" && str(pin, "fileName") == "trace_processor_shell.exe", "Pinned Perfetto converter identity is invalid");
  require_conversion(safe_uint(field(pin, "bytes"), true) && pin.at("bytes").get<uint64_t>() <= 67108864 && std::regex_match(str(pin, "sha256"), std::regex("[a-f0-9]{64}")) && !str(pin, "version").empty(), "Pinned Perfetto converter byte identity is invalid");
  require_conversion(fs::is_regular_file(executable) && !fs::is_symlink(executable) && fs::file_size(executable) == pin.at("bytes").get<uint64_t>() && file_sha256(executable) == str(pin, "sha256"), "Pinned Perfetto converter binary hash or length differs");
  return {{"protocol", "v8trace/perfetto-converter/1"}, {"manifest", file_reference(manifest)}, {"binary", file_reference(executable)}, {"version", pin.at("version")}};
}
Json convert_perfetto_trace(const fs::path& directory, const fs::path& input, const fs::path& output,
                            uint64_t maximum_bytes, uint64_t timeout_ms, const Sink& progress) {
  auto tool = verify_perfetto_converter(directory);
  require_conversion(maximum_bytes > 0 && timeout_ms >= 100 && timeout_ms <= 10800000, "Native trace conversion bounds are invalid");
  require_conversion(fs::is_regular_file(input) && !fs::is_symlink(input) && fs::file_size(input) > 0 && fs::file_size(input) <= maximum_bytes, "Raw Perfetto trace is missing, empty or outside its byte bound");
  require_conversion(!fs::exists(output) && fs::is_directory(output.parent_path()) && input != output, "Decoded trace output must be a fresh owned file");
  auto raw = file_reference(input);
  auto stdout_file = output; stdout_file += ".converter.stdout.log";
  auto stderr_file = output; stderr_file += ".converter.stderr.log";
  require_conversion(!fs::exists(stdout_file) && !fs::exists(stderr_file), "Converter diagnostic outputs already exist");
  Process process;
  auto started = std::chrono::steady_clock::now(), deadline = started + std::chrono::milliseconds(timeout_ms);
  std::string quoted;
  for (char value : utf8(output)) { quoted += value; if (value == '\'') quoted += value; }
  auto query = "SELECT EXPORT_JSON('" + quoted + "');";
  process.start(directory / "trace_processor_shell.exe", {"-Q", query, utf8(input)}, output.parent_path(), stdout_file, stderr_file);
  auto stop_converter = [&] {
    process.kill();
    const auto reap_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!process.poll()) {
      if (std::chrono::steady_clock::now() >= reap_deadline) throw std::runtime_error("Owned Native converter did not exit after termination; raw trace and diagnostics retained");
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  };
  std::optional<Json> exit;
  while (!(exit = process.poll())) {
    auto length = fs::is_regular_file(output) ? fs::file_size(output) : 0;
    if (length > maximum_bytes) { stop_converter(); throw std::runtime_error("Decoded Native trace exceeded its explicit byte bound; raw and partial conversion retained"); }
    if (std::chrono::steady_clock::now() >= deadline) { stop_converter(); throw std::runtime_error("Native trace conversion exceeded its bounded deadline; raw trace and diagnostics retained"); }
    if (progress) progress({{"phase", "converting-native-trace"}, {"rawBytes", raw.at("bytes")}, {"decodedBytes", length}});
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  require_conversion(field(*exit, "code") == 0 && field(*exit, "signal").is_null(), "Pinned Native trace converter failed; raw trace and child diagnostics retained");
  require_conversion(fs::is_regular_file(output) && !fs::is_symlink(output) && fs::file_size(output) > 0 && fs::file_size(output) <= maximum_bytes, "Decoded Native trace is empty or outside its byte bound");
  require_conversion(file_reference(input) == raw, "Raw Native trace changed during conversion");
  require_conversion(verify_perfetto_converter(directory) == tool, "Pinned Native trace converter changed during conversion");
  std::ifstream stream(io_path(output), std::ios::binary); JsonReader reader(stream); reader.white(); reader.expect('{');
  return {{"protocol", "v8trace/native-trace-conversion/1"}, {"rawFormat", "perfetto-protobuf"}, {"raw", raw}, {"decoded", file_reference(output)},
      {"converter", tool}, {"exit", *exit}, {"timeoutMilliseconds", timeout_ms},
      {"exportMethod", "PerfettoSQL SELECT EXPORT_JSON"}, {"querySha256", sha256(query)},
      {"elapsedMilliseconds", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count()},
      {"stdout", file_reference(stdout_file)}, {"stderr", file_reference(stderr_file)},
      {"rawUnchanged", true}, {"fullJsonValidation", "required-by-subsequent-stream-merge"}, {"browserSandboxModified", false}};
}
}
