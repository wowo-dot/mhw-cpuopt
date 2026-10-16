#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace cpuopt::frame_budget {
// Research switch frame_budget (toggles.hpp): where each frame of the game
// thread goes, independent of the 300 fps cap and of the GPU.
//
// Astera Loader's al.frame BEGIN/END wrap Main:Update (sub_141AF2B60). Inside
// it, per frame, on the game thread:
// - the limiter: sMain's frame timing (sub_14225F9F0, called first) holds
//   the frame to the cap; it Sleeps when 2 ms or more remain and otherwise
//   spins on QueryPerformanceCounter;
// - the job rounds: every sMain::executeJob (timed in the executeJob detour);
// - the Present path: cD3DRender::present, which waits on the GPU's fence of
//   two frames back in present12 and then calls the swapchain's Present
//   (whose own time al.d3d12 reports);
// - the rest: the game thread's own serial work.
// Per frame this records: the interval, update (BEGIN to END), limiter,
// rounds, present path, swapchain Present, gpu wait (present path minus the
// swapchain call), serial (update minus limiter, rounds and present path)
// and the CPU frame (interval minus limiter and gpu wait): what a frame
// costs the CPU with no cap and an infinitely fast GPU.
//
// Detours (al.hooks; neither is in the DLSS mod's hook list): the limiter's
// function and cD3DRender::present, both from their entries; tested exe
// c2ebbbd2..., clean image a78ea69b..., each matched once.
inline constexpr std::string_view timing_signature =
    "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 60 80 B9 8C 00 00 00 00 48 8B D9 8B 81 D0 00 00 00 89 81 CC 00 00 00";
inline constexpr std::string_view present_signature =
    "48 89 5C 24 18 48 89 7C 24 20 41 56 48 81 EC 80 00 00 00 41 0F B6 D8 44 8B F2 48 8B F9 E8 ?? ?? ?? ?? 84 C0 74 14 44 0F B6 C3 41 8B D6 "
    "48 8B CF E8";

struct Discovery {
    bool found{};
    const char* reason{"not searched"};
    unsigned counts[2]{};  // timing, present
    std::uintptr_t timing{}, present{};
};
Discovery discover(const std::uintptr_t* timing, unsigned timing_count, const std::uintptr_t* present, unsigned present_count) noexcept;
std::string discovery_event(const Discovery&, std::uintptr_t image_base, double t_ms, std::string_view scanner);

using Timing = std::uint64_t(__fastcall*)(std::uintptr_t smain);
using Present = std::uint64_t(__fastcall*)(std::uintptr_t render, std::uint32_t a2, std::uint8_t a3);
std::uint64_t __fastcall timing_detour(std::uintptr_t smain);
std::uint64_t __fastcall present_detour(std::uintptr_t render, std::uint32_t a2, std::uint8_t a3);
Timing* timing_next() noexcept;
Present* present_next() noexcept;

// From the game thread: a job round's cycles (executeJob detour), the
// swapchain Present's QPC ticks (al.d3d12), al.frame BEGIN and END.
void add_round(std::uint64_t cycles) noexcept;
void add_swapchain(std::int64_t qpc_ticks) noexcept;
void on_begin(std::int64_t qpc) noexcept;
void on_end(std::int64_t qpc) noexcept;
// Measuring on or off. Any thread.
void set_active(bool on) noexcept;  // measuring; the timing detour also serves limiter.hpp
bool active() noexcept;
// "budget": per frame, means and p50/p95 in ms, since the last call.
void append(std::string& out);
// The same window as plain numbers (the benchmark): frames, and each part's
// mean in ms. Starts a new window like append.
struct Totals {
    std::uint64_t frames{};
    double cpu_frame_ms{}, update_ms{}, limiter_ms{}, rounds_ms{}, present_ms{}, serial_ms{};
};
Totals take_totals() noexcept;
}  // namespace cpuopt::frame_budget
