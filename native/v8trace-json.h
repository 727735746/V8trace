#ifndef V8_RUNTIME_V8TRACE_JSON_H_
#define V8_RUNTIME_V8TRACE_JSON_H_

#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>

namespace v8trace_json {

template <typename T>
struct Numeric { T value; int base = 10; };

template <typename T>
  requires ((std::is_integral_v<T> && !std::is_same_v<T, bool>) || std::is_floating_point_v<T>)
Numeric<T> Number(T value) { return {value}; }

template <typename T>
  requires (std::is_integral_v<T> && !std::is_same_v<T, bool>)
Numeric<T> Hex(T value) { return {value, 16}; }

template <typename T>
std::ostream& operator<<(std::ostream& out, Numeric<T> number) {
  std::array<char, 64> buffer;
  std::to_chars_result result;
  if constexpr (std::is_integral_v<T>) {
    result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), number.value, number.base);
  } else {
    result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), number.value,
                           std::chars_format::general, std::numeric_limits<T>::max_digits10);
  }
  if (result.ec != std::errc()) out.setstate(std::ios::badbit);
  else out.write(buffer.data(), static_cast<std::streamsize>(result.ptr - buffer.data()));
  return out;
}

template <typename Next>
void WriteUtf16(std::ostream& out, size_t length, Next next) {
  std::array<char, 4096> buffer;
  size_t used = 0;
  constexpr char hex[] = "0123456789abcdef";
  auto append = [&](char value) {
    buffer[used++] = value;
    if (used == buffer.size()) { out.write(buffer.data(), static_cast<std::streamsize>(used)); used = 0; }
  };
  append('"');
  for (size_t i = 0; i < length; ++i) {
    uint16_t ch = next();
    if (ch == '"' || ch == '\\') { append('\\'); append(static_cast<char>(ch)); }
    else if (ch >= 32 && ch < 127) append(static_cast<char>(ch));
    else {
      append('\\'); append('u');
      append(hex[(ch >> 12) & 15]); append(hex[(ch >> 8) & 15]);
      append(hex[(ch >> 4) & 15]); append(hex[ch & 15]);
    }
  }
  append('"');
  if (used) out.write(buffer.data(), static_cast<std::streamsize>(used));
}

inline std::string EncodeUtf16(std::u16string_view value) {
  size_t cursor = 0;
  std::ostringstream out;
  WriteUtf16(out, value.size(), [&] { return static_cast<uint16_t>(value[cursor++]); });
  return out.str();
}

}

#endif
