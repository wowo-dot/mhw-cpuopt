#pragma once
#include <cstdint>
#include <string>

namespace cpuopt::limiter {
// Toggle sleep_limiter (toggles.hpp): the engine's frame limiter sleeps
// instead of spinning.
//
// sMain's frame timing (sub_14225F9F0, frame_budget's timing detour) holds
// each frame to 1/mMaxFps: it Sleeps (whole ms, minus one) only when 2 ms or
// more remain and otherwise spins on QueryPerformanceCounter. At a 300 fps
// cap with the kept switches about 1.9 ms remain per frame, all of it spun.
// The switch waits on a high-resolution waitable timer in the detour, before
// the game's function runs, until control.ini margin_us before the deadline
// the function computes; the function then finds that little left and spins
// it. Nothing of the game's own timing is changed or skipped: its entry
// comes later, as if the frame's work had taken longer.
//
// sMain fields read (all written by sub_14225F9F0 only, on the game thread):
inline constexpr std::uint32_t scale_offset = 0x58;      // float: the game rate (60) its counters use
inline constexpr std::uint32_t max_fps_offset = 0x5C;    // float: mMaxFps
inline constexpr std::uint32_t mode_offset = 0x60;       // byte: nonzero = the fixed-step branch (left alone)
inline constexpr std::uint32_t scaled_offset = 0x70;     // qword: QPC ticks times the rate, summed
inline constexpr std::uint32_t reference_offset = 0x78;  // qword: that sum when the frame's wait began
inline constexpr std::uint32_t last_qpc_offset = 0x80;   // qword: QPC at its last reading

// The game's remaining wait in seconds at QPC now, from the fields above, as
// sub_14225F9F0 computes it; false when the fields do not fit (not the
// normal branch, no cap, an unexpected rate).
bool remaining(std::uintptr_t smain, std::int64_t now, std::int64_t frequency, double& seconds) noexcept;

void set_active(bool enable, unsigned margin_us) noexcept;  // any thread
bool active() noexcept;
void before(std::uintptr_t smain) noexcept;  // the timing detour, game thread
// "limiter": {active, state, frames, waits, wait {mean, p50, p99}, late {mean,
// p50, p99, max}, past_margin} into an open JSON object, and resets the window.
void append(std::string& out);
}  // namespace cpuopt::limiter
