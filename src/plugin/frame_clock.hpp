#pragma once
#include "../core/histogram.hpp"
#include <windows.h>
#include <cstdint>
#include <string>
#include <string_view>

namespace cpuopt::frame_clock {
// sRender's 64-bit frame counter, found without hooks. The signature is the
// renderer's frame-parity read (mov rax,[rip+sRender]; mov r9,1/3 magic;
// mov r8,[rax+counter]); the singleton RVA and the counter offset are taken
// from its operands. Tested build (clean image a78ea69b...): site 0x23922C2,
// singleton 0x51C4480, counter +0x1F210 (incremented at 0x22932DB, set in
// sRender::ctor at 0x2290E78).
inline constexpr std::string_view signature = "48 8B 05 ?? ?? ?? ?? 49 B9 AB AA AA AA AA AA AA AA 4C 8B 80 ?? ?? ?? ??";
inline constexpr unsigned match_capacity = 8;

struct Discovery {
    bool found{};
    unsigned matches{};
    std::uint32_t site_rva{}, singleton_rva{}, counter_offset{};
    bool expected_values{};  // equal to the tested build's values
    const char* reason{"not searched"};
};
// Our own search of the loaded image's executable sections (no Astera Loader
// scanner, and the tests). Returns how many matches there are; stores the
// first `capacity` addresses.
unsigned scan(std::uintptr_t image_base, std::uintptr_t* matches, unsigned capacity) noexcept;
// Checks the matches (all must agree) and derives the singleton and offset.
// `count` is the total number of matches, `matches` holds the first ones.
Discovery discover(std::uintptr_t image_base, const std::uintptr_t* matches, unsigned count) noexcept;
std::string discovery_event(const Discovery&, double t_ms, std::string_view scanner);

// Where the counter is, once found. Reads are valid from any thread after this.
void set_target(std::uintptr_t image_base, const Discovery&) noexcept;
bool has_target() noexcept;
// One read. `validated` is the caller's own cache of the renderer pointer it
// checked last (start with 0); false while there is no renderer.
bool read(std::uintptr_t& validated, std::uint64_t& value) noexcept;

// Fallback when Astera Loader cannot follow the game's frames: a thread
// polls the counter about every 1 ms (high-resolution waitable timer) and
// times each change. Frame counts are exact; single intervals carry the poll
// latency (reported per window).
void start_poller() noexcept;
void stop_poller() noexcept;
DWORD poller_thread() noexcept;

struct Window {
    bool renderer{};                       // the renderer existed at the end of the window
    std::uint64_t counter{}, frames{};     // counter at the end; counter advance in the window
    std::uint64_t polls{}, multi_step{};   // polls; changes of more than one frame between polls
    std::uint64_t max_poll_gap_us{}, poll_gap_total_us{};
    IntervalHistogram::Summary intervals;  // observed single-frame intervals
};
Window take() noexcept;
}  // namespace cpuopt::frame_clock
