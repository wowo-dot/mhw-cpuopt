#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace cpuopt {
// Player mode's settings: cpuopt.ini next to the DLL, edited by hand or
// through CMFM's Mods page. Lines are "key = value", with an optional
// "; comment" after the value; '#' or ';' at the start makes a comment line.
// Switches take on/off (also true/false, yes/no, 1/0).
//
// `enabled` is the master switch over the optimizations (its hotkey,
// toggle_key, flips it in game); remove_fps_cap is a separate, experimental
// switch that the master leaves alone, so an on/off comparison keeps the
// same frame limit in both states.
struct Settings {
    bool enabled{true};
    std::uint32_t toggle_key{};  // a CMFM KEY value: virtual-key code | CMFM_KEY_CTRL/SHIFT/ALT bits; 0 none
    bool effect_stores{true};      // plain_prim_stores
    bool job_order{true};          // order_jobs, mode 2
    bool server_time{true};        // reuse_server_time
    bool unit_threshold{true};     // unit_threshold
    bool cache_ccd{true};          // cache_ccd
    bool parallel_contacts{true};  // parallel_contacts, contact pass
    bool sleep_limiter{true};      // sleep_limiter
    bool remove_fps_cap{false};    // uncap, only from the game's "no limit" 300
    unsigned limiter_margin_us{1000};  // 50..2000
    unsigned unit_us{40};              // 10..200
    unsigned benchmark_s{15};          // 1..60: seconds per half of each benchmark pair
    bool operator==(const Settings&) const = default;
};

// Known keys with good values are read; anything else is skipped (the
// default stays) and named in `problems` when given ("line 4: ...").
Settings parse_settings(std::string_view text, std::string* problems = nullptr);
// `text` with the known keys set to `s`: their lines keep everything but the
// value (spacing, and a comment after it at the same column when there is
// room), missing keys are appended with their comment,
// all other lines stay as they are. An empty `text` gives the full default
// file with `s`'s values.
std::string write_settings(std::string_view text, const Settings& s);
// The runtime toggles (toggles.hpp) these settings stand for.
unsigned toggles_of(const Settings& s) noexcept;
// Key names as written in the file: "None", "F9", "Ctrl+Shift+K", "Mouse4",
// "Numpad5", "VK 0xBA" for keys without a name here.
std::string key_name(std::uint32_t key);
std::optional<std::uint32_t> parse_key(std::string_view name);
}  // namespace cpuopt
