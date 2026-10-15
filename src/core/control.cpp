#include "control.hpp"
#include <charconv>

namespace cpuopt {
namespace {
std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\xEF' ||
                          s.front() == '\xBB' || s.front() == '\xBF'))  // UTF-8 BOM bytes
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}
std::optional<unsigned> number(std::string_view s) noexcept {
    int base = 10;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s.remove_prefix(2);
        base = 16;
    }
    unsigned value{};
    const auto [end, error] = std::from_chars(s.data(), s.data() + s.size(), value, base);
    if (error != std::errc{} || end != s.data() + s.size() || s.empty()) return std::nullopt;
    return value;
}
bool label_char(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
}
}  // namespace

std::optional<Control> parse_control(std::string_view text) {
    Control c;
    while (!text.empty()) {
        const auto end = text.find('\n');
        auto line = trim(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        if (line.empty() || line.front() == ';' || line.front() == '#' || line.front() == '[') continue;
        const auto eq = line.find('=');
        if (eq == std::string_view::npos) return std::nullopt;
        const auto key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        if (key == "toggles") {
            c.toggles = number(value);
            if (!c.toggles) return std::nullopt;
        } else if (key == "window_ms") {
            c.window_ms = number(value);
            if (!c.window_ms || *c.window_ms < 1000 || *c.window_ms > 60000) return std::nullopt;
        } else if (key == "job_threads") {
            c.job_threads = number(value);
            if (!c.job_threads || *c.job_threads < 1 || *c.job_threads > 64) return std::nullopt;
        } else if (key == "job_order") {
            c.job_order = number(value);
            if (!c.job_order || *c.job_order < 1 || *c.job_order > 2) return std::nullopt;
        } else if (key == "order_us") {
            c.order_us = number(value);
            if (!c.order_us || *c.order_us < 1 || *c.order_us > 10000) return std::nullopt;
        } else if (key == "inline_us") {
            c.inline_us = number(value);
            if (!c.inline_us || *c.inline_us < 1 || *c.inline_us > 1000) return std::nullopt;
        } else if (key == "time_ms") {
            c.time_ms = number(value);
            if (!c.time_ms || *c.time_ms < 10 || *c.time_ms > 60000) return std::nullopt;
        } else if (key == "spin_us") {
            c.spin_us = number(value);
            if (!c.spin_us || *c.spin_us < 10 || *c.spin_us > 5000) return std::nullopt;
        } else if (key == "marker") {
            if (value.size() > 64) return std::nullopt;
            for (const char ch : value)
                if (!label_char(ch)) return std::nullopt;
            c.marker = std::string(value);
        } else {
            return std::nullopt;
        }
    }
    return c;
}
}  // namespace cpuopt
