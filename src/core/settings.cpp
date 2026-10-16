#include "settings.hpp"
#include "toggles.hpp"
#include <array>
#include <charconv>
#include <cstdio>
#include <vector>

namespace cpuopt {
namespace {
constexpr std::uint32_t key_ctrl = 0x100, key_shift = 0x200, key_alt = 0x400;  // CMFM_KEY_* bits

enum class Kind { flag, key, number };
struct Field {
    const char* key;
    Kind kind;
    unsigned min, max;  // numbers
    const char* comment;
};
// In file order. The default file adds its headings before these keys.
constexpr Field fields[] = {
    {"enabled", Kind::flag, 0, 0, "all optimizations below at once; toggle_key flips it in game"},
    {"toggle_key", Kind::key, 0, 0, "hotkey for enabled, e.g. Ctrl+F9 (None: no hotkey)"},
    {"effect_stores", Kind::flag, 0, 0, "effects write their vertex data without reading GPU memory back"},
    {"job_order", Kind::flag, 0, 0, "the slowest draw jobs start first, so job rounds end sooner"},
    {"server_time", Kind::flag, 0, 0, "Steam's server time is read once a second, not several times a frame"},
    {"unit_threshold", Kind::flag, 0, 0, "more update work goes to the job threads (40 us threshold; the game's is 60)"},
    {"cache_ccd", Kind::flag, 0, 0, "on CPUs with a 3D V-Cache die (7950X3D, 9950X3D...) the game stays on it"},
    {"parallel_contacts", Kind::flag, 0, 0, "hit detection runs on all job threads, as the engine meant to"},
    {"sleep_limiter", Kind::flag, 0, 0, "with a frame limit, the game sleeps instead of spinning while it waits"},
    {"remove_fps_cap", Kind::flag, 0, 0, "lifts the engine's 300 fps limit (only with the in-game frame rate at no limit)"},
    {"limiter_margin_us", Kind::number, 50, 2000, "sleep_limiter: wakes this long before the frame is due (50 to 2000)"},
    {"unit_us", Kind::number, 10, 200, "unit_threshold: the threshold in microseconds (10 to 200)"},
    {"benchmark_s", Kind::number, 1, 60, "the Mods page's benchmark: seconds per half of each of its 8 pairs (1 to 60)"},
};
constexpr const char* heading_before[] = {
    "# cpuopt: CPU optimizations for Monster Hunter: World (an Astera Loader plugin).\n"
    "# Change these here or in the game's Mods menu (with CMFM). Switches: on or off.\n\n",
    nullptr, "\n# Optimizations\n", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, "\n# Experimental\n", "\n# Advanced\n", nullptr, nullptr,
};
static_assert(std::size(heading_before) == std::size(fields));

bool* flag_of(Settings& s, std::string_view key) {
    if (key == "enabled") return &s.enabled;
    if (key == "effect_stores") return &s.effect_stores;
    if (key == "job_order") return &s.job_order;
    if (key == "server_time") return &s.server_time;
    if (key == "unit_threshold") return &s.unit_threshold;
    if (key == "cache_ccd") return &s.cache_ccd;
    if (key == "parallel_contacts") return &s.parallel_contacts;
    if (key == "sleep_limiter") return &s.sleep_limiter;
    if (key == "remove_fps_cap") return &s.remove_fps_cap;
    return nullptr;
}
unsigned* number_of(Settings& s, std::string_view key) {
    if (key == "limiter_margin_us") return &s.limiter_margin_us;
    if (key == "unit_us") return &s.unit_us;
    if (key == "benchmark_s") return &s.benchmark_s;
    return nullptr;
}
const Field* field_of(std::string_view key) {
    for (const auto& f : fields)
        if (key == f.key) return &f;
    return nullptr;
}

bool space(char c) { return c == ' ' || c == '\t' || c == '\r'; }
std::string_view trim(std::string_view s) {
    while (!s.empty() && (space(s.front()) || s.front() == '\xEF' || s.front() == '\xBB' || s.front() == '\xBF')) s.remove_prefix(1);
    while (!s.empty() && space(s.back())) s.remove_suffix(1);
    return s;
}
char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }
bool same(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (lower(a[i]) != lower(b[i])) return false;
    return true;
}
std::optional<bool> parse_flag(std::string_view v) {
    for (const auto* yes : {"on", "true", "yes", "1"})
        if (same(v, yes)) return true;
    for (const auto* no : {"off", "false", "no", "0"})
        if (same(v, no)) return false;
    return std::nullopt;
}
std::optional<unsigned> parse_number(std::string_view v) {
    unsigned n{};
    const auto [end, error] = std::from_chars(v.data(), v.data() + v.size(), n);
    if (error != std::errc{} || end != v.data() + v.size() || v.empty()) return std::nullopt;
    return n;
}

// One line split into its parts: key, and the value's place in the line.
struct Line {
    std::string_view key, value;
    std::size_t value_begin{}, value_end{};  // in the line
    bool setting{};
};
Line split(std::string_view line) {
    Line l;
    const auto body = trim(line);
    if (body.empty() || body.front() == '#' || body.front() == ';' || body.front() == '[') return l;
    const auto eq = line.find('=');
    if (eq == std::string_view::npos) return l;
    l.key = trim(line.substr(0, eq));
    auto begin = eq + 1;
    while (begin < line.size() && space(line[begin])) ++begin;
    auto end = line.find_first_of(";#", begin);
    if (end == std::string_view::npos) end = line.size();
    while (end > begin && space(line[end - 1])) --end;
    l.value_begin = begin;
    l.value_end = end;
    l.value = line.substr(begin, end - begin);
    l.setting = true;
    return l;
}

std::string value_text(const Settings& s, const Field& f) {
    auto copy = s;
    if (f.kind == Kind::flag) return *flag_of(copy, f.key) ? "on" : "off";
    if (f.kind == Kind::key) return key_name(s.toggle_key);
    return std::to_string(*number_of(copy, f.key));
}
std::string full_line(const Settings& s, const Field& f) {
    auto line = std::string(f.key) + " = " + value_text(s, f);
    if (line.size() < 24) line.append(24 - line.size(), ' ');
    return line + " ; " + f.comment;
}

struct KeyName {
    const char* name;
    std::uint32_t vk;
};
constexpr KeyName key_names[] = {
    {"Backspace", 0x08}, {"Tab", 0x09}, {"Enter", 0x0D}, {"Pause", 0x13}, {"CapsLock", 0x14}, {"Esc", 0x1B},
    {"Space", 0x20}, {"PageUp", 0x21}, {"PageDown", 0x22}, {"End", 0x23}, {"Home", 0x24}, {"Left", 0x25},
    {"Up", 0x26}, {"Right", 0x27}, {"Down", 0x28}, {"PrintScreen", 0x2C}, {"Insert", 0x2D}, {"Delete", 0x2E},
    {"Mouse3", 0x04}, {"Mouse4", 0x05}, {"Mouse5", 0x06}, {"NumpadMultiply", 0x6A}, {"NumpadAdd", 0x6B},
    {"NumpadSubtract", 0x6D}, {"NumpadDecimal", 0x6E}, {"NumpadDivide", 0x6F}, {"NumLock", 0x90}, {"ScrollLock", 0x91},
};
}  // namespace

Settings parse_settings(std::string_view text, std::string* problems) {
    Settings s;
    unsigned number = 0;
    while (!text.empty()) {
        ++number;
        const auto end = text.find('\n');
        const auto line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        const auto l = split(line);
        if (!l.setting) continue;
        const auto* f = field_of(l.key);
        bool ok = f != nullptr;
        if (ok && f->kind == Kind::flag) {
            const auto v = parse_flag(l.value);
            ok = v.has_value();
            if (ok) *flag_of(s, l.key) = *v;
        } else if (ok && f->kind == Kind::key) {
            const auto v = parse_key(l.value);
            ok = v.has_value();
            if (ok) s.toggle_key = *v;
        } else if (ok) {
            const auto v = parse_number(l.value);
            ok = v && *v >= f->min && *v <= f->max;
            if (ok) *number_of(s, l.key) = *v;
        }
        if (!ok && problems) {
            char head[32];
            std::snprintf(head, sizeof(head), "line %u: ", number);
            *problems += head;
            *problems += f ? "bad value for " : "unknown key ";
            problems->append(l.key);
            *problems += '\n';
        }
    }
    return s;
}

std::string write_settings(std::string_view text, const Settings& s) {
    std::string out;
    if (trim(text).empty()) {
        for (std::size_t i = 0; i < std::size(fields); ++i) {
            if (heading_before[i]) out += heading_before[i];
            out += full_line(s, fields[i]);
            out += '\n';
        }
        return out;
    }
    std::array<bool, std::size(fields)> seen{};
    while (!text.empty()) {
        const auto end = text.find('\n');
        const auto line = text.substr(0, end);
        const bool newline = end != std::string_view::npos;
        text = newline ? text.substr(end + 1) : std::string_view{};
        const auto l = split(line);
        const auto* f = l.setting ? field_of(l.key) : nullptr;
        if (f) {
            seen[static_cast<std::size_t>(f - fields)] = true;
            const auto value = value_text(s, *f);
            out.append(line.substr(0, l.value_begin));
            out += value;
            // A comment after the value keeps its column while there is room.
            auto rest = line.substr(l.value_end);
            std::size_t gap = 0;
            while (gap < rest.size() && rest[gap] == ' ') ++gap;
            if (gap && gap < rest.size() && (rest[gap] == ';' || rest[gap] == '#')) {
                const auto column = gap + (l.value_end - l.value_begin);
                out.append(column > value.size() + 1 ? column - value.size() : 1, ' ');
                rest.remove_prefix(gap);
            }
            out.append(rest);
        } else {
            out.append(line);
        }
        if (newline) out += '\n';
    }
    const bool crlf = out.find("\r\n") != std::string::npos;
    for (std::size_t i = 0; i < std::size(fields); ++i) {
        if (seen[i]) continue;
        if (!out.empty() && out.back() != '\n') out += crlf ? "\r\n" : "\n";
        out += full_line(s, fields[i]);
        out += crlf ? "\r\n" : "\n";
    }
    return out;
}

std::optional<std::string> ini_lookup(std::string_view text, std::string_view key) {
    while (!text.empty()) {
        const auto end = text.find('\n');
        const auto line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        const auto l = split(line);
        if (l.setting && same(l.key, key)) return std::string(l.value);
    }
    return std::nullopt;
}

unsigned toggles_of(const Settings& s) noexcept {
    unsigned t = s.remove_fps_cap ? unsigned(uncap) : 0U;  // not under the master switch
    if (!s.enabled) return t;
    if (s.effect_stores) t |= plain_prim_stores;
    if (s.job_order) t |= order_jobs;
    if (s.server_time) t |= reuse_server_time;
    if (s.unit_threshold) t |= unit_threshold;
    if (s.cache_ccd) t |= cache_ccd;
    if (s.parallel_contacts) t |= parallel_contacts;
    if (s.sleep_limiter) t |= sleep_limiter;
    return t;
}

std::string key_name(std::uint32_t key) {
    const auto vk = key & 0xFFU;
    if (!vk) return "None";
    std::string out;
    if (key & key_ctrl) out += "Ctrl+";
    if (key & key_shift) out += "Shift+";
    if (key & key_alt) out += "Alt+";
    char buffer[16];
    if (vk >= 0x70 && vk <= 0x87) {
        std::snprintf(buffer, sizeof(buffer), "F%u", vk - 0x6F);
    } else if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) {
        std::snprintf(buffer, sizeof(buffer), "%c", static_cast<char>(vk));
    } else if (vk >= 0x60 && vk <= 0x69) {
        std::snprintf(buffer, sizeof(buffer), "Numpad%u", vk - 0x60);
    } else {
        std::snprintf(buffer, sizeof(buffer), "VK 0x%02X", vk);
        for (const auto& k : key_names)
            if (k.vk == vk) std::snprintf(buffer, sizeof(buffer), "%s", k.name);
    }
    return out + buffer;
}

std::optional<std::uint32_t> parse_key(std::string_view name) {
    name = trim(name);
    if (same(name, "none") || name.empty()) return 0U;
    std::uint32_t mods = 0;
    for (;;) {
        const auto plus = name.find('+');
        if (plus == std::string_view::npos || plus + 1 >= name.size()) break;
        const auto part = trim(name.substr(0, plus));
        if (same(part, "ctrl")) mods |= key_ctrl;
        else if (same(part, "shift")) mods |= key_shift;
        else if (same(part, "alt")) mods |= key_alt;
        else return std::nullopt;
        name = trim(name.substr(plus + 1));
    }
    std::uint32_t vk = 0;
    if (name.size() == 1 && ((name[0] >= '0' && name[0] <= '9') || (lower(name[0]) >= 'a' && lower(name[0]) <= 'z'))) {
        vk = static_cast<std::uint32_t>(name[0] >= 'a' && name[0] <= 'z' ? name[0] - 'a' + 'A' : name[0]);
    } else if (name.size() >= 2 && lower(name[0]) == 'f') {
        const auto n = parse_number(name.substr(1));
        if (n && *n >= 1 && *n <= 24) vk = 0x6F + *n;
    } else if (name.size() == 7 && same(name.substr(0, 6), "numpad") && name[6] >= '0' && name[6] <= '9') {
        vk = 0x60U + static_cast<std::uint32_t>(name[6] - '0');
    } else if (name.size() > 5 && same(name.substr(0, 5), "vk 0x")) {
        unsigned n{};
        const auto hex = name.substr(5);
        const auto [end, error] = std::from_chars(hex.data(), hex.data() + hex.size(), n, 16);
        if (error == std::errc{} && end == hex.data() + hex.size() && n > 0 && n < 0x100) vk = n;
    } else {
        for (const auto& k : key_names)
            if (same(name, k.name)) vk = k.vk;
    }
    if (!vk) return std::nullopt;
    return vk | mods;
}
}  // namespace cpuopt
