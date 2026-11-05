#include "v8trace/trace_conversion.h"
#include "v8trace/recording.h"
#include <iostream>

using namespace v8trace;
namespace {
unsigned checks = 0;
void check(bool value, const char* name) { ++checks; if (!value) throw std::runtime_error(name); }
template<class F> void rejected(F fn, const char* name) { bool failed = false; try { fn(); } catch (const std::exception&) { failed = true; } check(failed, name); }
std::string varint(uint64_t value) {
  std::string result;
  do { auto next = uint8_t(value & 127); value >>= 7; result += char(next | (value ? 128 : 0)); } while (value);
  return result;
}
std::string number(unsigned field, uint64_t value) { return varint(uint64_t(field) << 3) + varint(value); }
std::string bytes(unsigned field, std::string_view value) { return varint((uint64_t(field) << 3) | 2) + varint(value.size()) + std::string(value); }
void conversion_checks(const fs::path& root) {
  auto directory = executable_path().parent_path();
  auto tool = verify_perfetto_converter(directory);
  check(tool.at("binary").at("sha256") == "9ac8c04ca44cc22f0aa6eff2ff8d25c3a87011b48aca554e7a10f677cbbfaaa0", "actual converter equals its pinned upstream Windows binary");
  const std::string payload = dump(Json{{"producerSeq", "18446744073709551615"}, {"text", "中文完整值"}});
  auto arg = bytes(10, "dataJson") + bytes(6, payload);
  auto record = bytes(10, "record") + bytes(6, payload);
  auto maximum = bytes(10, "maximum") + number(3, UINT64_MAX);
  auto descriptor = number(1, 41) + bytes(4, number(1, 3) + number(2, 7));
  auto event = number(9, 3) + number(11, 41) + bytes(23, "converter-protobuf-fixture") + bytes(22, "v8trace.test") + bytes(4, arg) + bytes(4, record) + bytes(4, maximum);
  auto raw_bytes = bytes(1, number(10, 1) + number(13, 1) + bytes(60, descriptor))
      + bytes(1, number(10, 1) + number(8, 1200000) + bytes(11, event));
  auto raw = root / "fixture.pftrace", decoded = root / "fixture.decoded.json";
  write_file(raw, raw_bytes, true);
  auto report = convert_perfetto_trace(directory, raw, decoded, 1048576, 10000);
  check(report.at("rawUnchanged") == true && read_file(raw) == raw_bytes, "actual raw protobuf bytes retained unchanged");
  check(report.at("converter") == tool && report.at("exit").at("code") == 0, "successful child conversion binds the actual converter and exit code");
  check(report.at("fullJsonValidation") == "required-by-subsequent-stream-merge", "child success alone is not full parse or browser completeness");
  auto value = load_json(decoded); bool found = false;
  for (const auto& row : value.at("traceEvents")) if (str(row, "name") == "converter-protobuf-fixture") {
    found = true;
    check(row.at("args").at("dataJson") == payload, "Unicode and exact uint64 decimal strings survive protobuf conversion");
    check(row.at("args").at("record") == payload && row.at("args").at("maximum").get<uint64_t>() == UINT64_MAX, "native browser string annotations and typed uint64 retain exact values");
    check(row.at("pid") == 3 && row.at("tid") == 7 && row.at("ts") == 1200, "process/thread identities and absolute microsecond clock survive conversion");
  }
  check(found, "actual protobuf fixture event decoded");
  auto merged = root / "fixture.merged.json";
  auto merge = merge_recording_traces({decoded}, merged, 1048576);
  check(merge.at("eventCount").get<uint64_t>() > 0, "converted JSON fully validates through the existing streaming merge");
  check(field(diagnostics(load_json(merged)), "finalFlushSucceeded") == false, "synthetic input does not invent native final-flush proof");
  rejected([&] { convert_perfetto_trace(directory, raw, decoded, 1048576, 10000); }, "existing decoded evidence is never overwritten");
  check(file_sha256(raw) == sha256(raw_bytes), "negative conversion attempt keeps raw bytes");
  rejected([&] { convert_perfetto_trace(directory, raw, root / "bad-bound.json", raw_bytes.size() - 1, 10000); }, "raw byte bound enforced before child launch");
  rejected([&] { convert_perfetto_trace(directory, raw, root / "bad-deadline.json", 1048576, 99); }, "out-of-contract conversion deadline rejected");
  auto empty = root / "empty.pftrace"; write_file(empty, "", true);
  rejected([&] { convert_perfetto_trace(directory, empty, root / "empty.json", 1048576, 10000); }, "empty native protobuf never converted as successful data");
  auto bad_tools = root / "bad-tools"; fs::create_directory(bad_tools);
  rejected([&] { verify_perfetto_converter(bad_tools); }, "missing converter manifest rejected");
  write_file(bad_tools / "trace-converter.json", read_file(directory / "trace-converter.json"), true);
  write_file(bad_tools / "trace_processor_shell.exe", "different-binary", true);
  rejected([&] { verify_perfetto_converter(bad_tools); }, "wrong binary bytes rejected without executing them");
  auto corrupt = root / "corrupt.pftrace"; write_file(corrupt, std::string(1, char(128)), true);
  rejected([&] { convert_perfetto_trace(directory, corrupt, root / "corrupt.json", 1048576, 10000); }, "actual converter parse failure remains failure");
  check(fs::exists(root / "corrupt.json.converter.stderr.log") && read_file(corrupt) == std::string(1, char(128)), "failed child stderr and corrupt raw bytes retained");
  rejected([&] { convert_perfetto_trace(directory, raw, root / "too-small.json", raw_bytes.size(), 10000); }, "decoded byte overflow remains explicit even after a successful child exit");
}
}
int main() {
  auto root = fs::temp_directory_path() / ("v8trace-trace-conversion-" + uuid());
  try {
    fs::create_directory(root); conversion_checks(root); fs::remove_all(root);
    std::cout << dump(Json{{"status", "PASS"}, {"checks", checks}, {"actualPinnedConverter", true}, {"browserRuntimeAccepted", false}}) << '\n'; return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << "; evidence retained at " << utf8(root) << '\n'; return 1; }
}
