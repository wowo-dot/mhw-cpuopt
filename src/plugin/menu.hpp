#pragma once
#include <cstdint>
#include <string>

namespace cpuopt::menu {
// Player mode's page in CMFM's Mods menu (third_party/cmfm, cmfm.menu 1.2;
// optional: without CMFM players edit cpuopt.ini). One page, "CPU
// optimizations": the master switch and its hotkey, a live status line, one
// switch per optimization, and the experimental fps-cap switch. Every row
// reads and writes runtime::settings(); the runtime's thread applies and
// saves (CMFM's callbacks run on the game's menu thread and stay short).

// The rows that can be unavailable on this game or CPU.
enum Row : unsigned { effect_stores, job_order, server_time, unit_threshold, cache_ccd, parallel_contacts, sleep_limiter, remove_fps_cap, row_count };
struct Availability {
    const char* why[row_count]{};  // nullptr: available; else greyed with this reason
};
// `cmfm`: the cmfm.menu interface (nullptr without CMFM). False with the reason.
bool add(const void* cmfm, const Availability& a, const char* version, std::string& why) noexcept;
// al.frame BEGIN: the frame rate on the status line.
void on_frame(std::int64_t qpc) noexcept;
// The status line's text ("264 fps, all 7 optimizations on"); for tests.
std::string status_text();
}  // namespace cpuopt::menu
