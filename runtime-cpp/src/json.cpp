#include "v8trace/core.h"
#include <charconv>
#include <cmath>
#include <sstream>

namespace v8trace {
namespace {
void encode(std::string& out, uint32_t c) {
  if (c < 0x80) out += char(c);
  else if (c < 0x800) { out += char(0xc0 | (c >> 6)); out += char(0x80 | (c & 63)); }
  else if (c < 0x10000) { out += char(0xe0 | (c >> 12)); out += char(0x80 | ((c >> 6) & 63)); out += char(0x80 | (c & 63)); }
  else { out += char(0xf0 | (c >> 18)); out += char(0x80 | ((c >> 12) & 63)); out += char(0x80 | ((c >> 6) & 63)); out += char(0x80 | (c & 63)); }
}
uint32_t decode(std::string_view input, size_t& i) {
  auto a = uint8_t(input.at(i++));
  if (a < 128) return a;
  unsigned count = a >= 0xf0 ? 3 : a >= 0xe0 ? 2 : a >= 0xc2 ? 1 : 99;
  if (count == 99 || i + count > input.size()) throw std::runtime_error("Invalid UTF-8 input");
  uint32_t c = a & (count == 3 ? 7 : count == 2 ? 15 : 31);
  for (unsigned n = 0; n < count; ++n) { auto b = uint8_t(input[i++]); if ((b & 0xc0) != 0x80) throw std::runtime_error("Invalid UTF-8 continuation"); c = (c << 6) | (b & 63); }
  if (c > 0x10ffff || (count == 1 && c < 0x80) || (count == 2 && c < 0x800) || (count == 3 && c < 0x10000)) throw std::runtime_error("Noncanonical UTF-8 input");
  return c;
}
void quote(std::string& out, std::string_view s) {
  constexpr char hex[] = "0123456789abcdef";
  out += '"';
  for (size_t i = 0; i < s.size();) {
    size_t start = i; auto c = decode(s, i);
    if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
    else if (c < 32 || (c >= 0xd800 && c <= 0xdfff)) { out += "\\u"; for (int shift = 12; shift >= 0; shift -= 4) out += hex[(c >> shift) & 15]; }
    else out.append(s.substr(start, i - start));
  }
  out += '"';
}
void serialize(std::string& out, const Json& value, int indent, int depth) {
  auto pad = [&](int level) { if (indent >= 0) { out += '\n'; out.append(size_t(level * indent), ' '); } };
  if (value.is_string()) quote(out, value.get_ref<const std::string&>());
  else if (value.is_object() || value.is_array()) {
    const bool object = value.is_object(); out += object ? '{' : '['; bool first = true;
    for (auto it = value.begin(); it != value.end(); ++it) {
      if (!first) out += ','; pad(depth + 1); first = false;
      if (object) { quote(out, it.key()); out += indent >= 0 ? ": " : ":"; }
      serialize(out, it.value(), indent, depth + 1);
    }
    if (!first) pad(depth); out += object ? '}' : ']';
  } else out += value.dump();
}
}
JsonReader::JsonReader(std::istream& input) : input_(input) {}
int JsonReader::peek() { return input_.peek(); }
void JsonReader::white() { while (peek() == ' ' || peek() == '\r' || peek() == '\n' || peek() == '\t') input_.get(); }
void JsonReader::expect(char c) { white(); if (input_.get() != c) throw std::runtime_error("Invalid or unfinished JSON: expected " + std::string(1, c)); }
bool JsonReader::ended() { white(); return peek() == EOF; }
std::string JsonReader::string() {
  expect('"'); std::string out;
  auto unit = [&]() { uint32_t c = 0; for (int n = 0; n < 4; ++n) { int h = input_.get(); int d = h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : -1; if (d < 0) throw std::runtime_error("Invalid JSON Unicode escape"); c = c * 16 + d; } return c; };
  std::optional<uint32_t> high;
  auto append = [&](uint32_t c) { if (high) { if (c >= 0xdc00 && c <= 0xdfff) { encode(out, 0x10000 + ((*high - 0xd800) << 10) + c - 0xdc00); high.reset(); return; } encode(out, *high); high.reset(); } if (c >= 0xd800 && c <= 0xdbff) high = c; else encode(out, c); };
  while (true) {
    int c = input_.get(); if (c == EOF || c < 32) throw std::runtime_error("Unfinished JSON string");
    if (c == '"') break;
    if (c == '\\') {
      c = input_.get();
      if (c == 'u') append(unit());
      else { std::string_view keys = "\"\\/bfnrt", vals = "\"\\/\b\f\n\r\t"; auto at = keys.find(char(c)); if (c == EOF || at == keys.npos) throw std::runtime_error("Invalid JSON escape"); append(uint8_t(vals[at])); }
    } else {
      if (high) { encode(out, *high); high.reset(); }
      out += char(c);
    }
  }
  if (high) encode(out, *high);
  for (size_t i = 0; i < out.size();) decode(out, i);
  return out;
}
Json JsonReader::value() {
  white(); if (++depth_ > 512) throw std::runtime_error("JSON nesting exceeds 512");
  struct Depth { size_t& n; ~Depth() { --n; } } guard{depth_};
  int first = peek();
  if (first == '"') return string();
  if (first == '{' || first == '[') {
    const bool object = first == '{'; input_.get(); Json out = object ? Json::object() : Json::array(); white();
    if (peek() == (object ? '}' : ']')) { input_.get(); return out; }
    while (true) {
      if (object) { auto key = string(); expect(':'); if (out.contains(key)) throw std::runtime_error("Duplicate JSON object key"); out[key] = value(); }
      else out.push_back(value());
      white(); if (peek() == (object ? '}' : ']')) { input_.get(); return out; } expect(',');
    }
  }
  if (first == 't' || first == 'f' || first == 'n') {
    std::string_view literal = first == 't' ? "true" : first == 'f' ? "false" : "null";
    for (char c : literal) if (input_.get() != c) throw std::runtime_error("Invalid JSON literal");
    return first == 'n' ? Json(nullptr) : Json(first == 't');
  }
  std::string number;
  while (peek() == '-' || peek() == '+' || peek() == '.' || peek() == 'e' || peek() == 'E' || (peek() >= '0' && peek() <= '9')) number += char(input_.get());
  if (number.empty()) throw std::runtime_error("Invalid or unfinished JSON value");
  return Json::parse(number);
}
Json parse(std::string_view data) { std::istringstream input{std::string(data)}; JsonReader reader(input); auto result = reader.value(); if (!reader.ended()) throw std::runtime_error("Trailing JSON input"); return result; }
std::string dump(const Json& value, int indent) { std::string out; serialize(out, value, indent, 0); return out; }
Json load_json(const fs::path& file, uint64_t maximum) { return parse(read_file(file, maximum)); }
void lines(const fs::path& file, const Sink& visit) { if (!fs::exists(file)) return; std::ifstream input(file, std::ios::binary); if (!input) throw std::runtime_error("Open JSONL failed: " + utf8(file)); std::string line; while (std::getline(input, line)) if (!trim(line).empty()) visit(parse(line)); if (input.bad()) throw std::runtime_error("JSONL read failed"); }
std::string utf16le(std::string_view input) { std::string out; auto append = [&](uint32_t c) { out += char(c & 255); out += char(c >> 8); }; for (size_t i = 0; i < input.size();) { auto c = decode(input, i); if (c > 0xffff) { c -= 0x10000; append(0xd800 | (c >> 10)); append(0xdc00 | (c & 1023)); } else append(c); } return out; }
std::string replacement_utf8(std::string_view input) { std::string out; for (size_t i = 0; i < input.size();) { auto c = decode(input, i); encode(out, c >= 0xd800 && c <= 0xdfff ? 0xfffd : c); } return out; }
std::string text(const Json& value) { return value.is_string() ? value.get<std::string>() : dump(value); }
Json field(const Json& value, std::string_view key, Json fallback) { auto it = value.find(std::string(key)); return it == value.end() ? fallback : *it; }
std::string str(const Json& value, std::string_view key, std::string fallback) { auto item = field(value, key); return item.is_string() ? item.get<std::string>() : fallback; }
std::vector<std::string> split(std::string_view value, char separator) { std::vector<std::string> out; size_t at = 0; while (true) { auto end = value.find(separator, at); out.emplace_back(value.substr(at, end == value.npos ? value.size() - at : end - at)); if (end == value.npos) return out; at = end + 1; } }
std::string trim(std::string s) { auto first = s.find_first_not_of(" \r\n\t"); if (first == s.npos) return ""; return s.substr(first, s.find_last_not_of(" \r\n\t") - first + 1); }
std::string lower(std::string s) { for (char& c : s) if (c >= 'A' && c <= 'Z') c += 'a' - 'A'; return s; }
bool safe_uint(const Json& value, bool positive) { return value.is_number_integer() && value >= (positive ? 1 : 0) && value <= 9007199254740991ULL; }
uint64_t decimal(std::string_view s) { uint64_t n = 0; auto result = std::from_chars(s.data(), s.data() + s.size(), n); if (s.empty() || (s.size() > 1 && s[0] == '0') || result.ec != std::errc{} || result.ptr != s.data() + s.size()) throw std::runtime_error("Invalid decimal counter"); return n; }
}
