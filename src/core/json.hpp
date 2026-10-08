#pragma once
#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>

namespace cpuopt::json {
// Appends a quoted JSON string. Control characters are escaped; other bytes
// pass through, so the input must be UTF-8.
inline void string(std::string& out, std::string_view s) {
    out += '"';
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += ch;
        } else if (c < 0x20) {
            static constexpr char digits[] = "0123456789abcdef";
            out += "\\u00";
            out += digits[c >> 4];
            out += digits[c & 15];
        } else {
            out += ch;
        }
    }
    out += '"';
}
inline void number(std::string& out, std::uint64_t v) {
    char buffer[24];
    const auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), v);
    out.append(buffer, error == std::errc{} ? end : buffer);
}
inline void number(std::string& out, std::int64_t v) {
    char buffer[24];
    const auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), v);
    out.append(buffer, error == std::errc{} ? end : buffer);
}
// Fixed-point decimal with `places` digits after the point.
inline void fixed(std::string& out, double v, int places) {
    char buffer[48];
    const auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), v, std::chars_format::fixed, places);
    out.append(buffer, error == std::errc{} ? end : buffer);
}
// "key": prefix, with a leading comma unless the object was just opened.
inline void key(std::string& out, std::string_view k) {
    if (!out.empty() && out.back() != '{' && out.back() != '[') out += ',';
    string(out, k);
    out += ':';
}
}  // namespace cpuopt::json
