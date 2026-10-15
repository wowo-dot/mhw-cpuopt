#pragma once
#include "../core/histogram.hpp"
#include <cstdint>
#include <string>

namespace cpuopt::frame_events {
// Exact per-frame timing from Astera Loader's callbacks. The callbacks only
// count into atomics: no allocation, no locks, no logging.
//
// - al.frame BEGIN/END on the game's frame thread: the interval between
//   BEGINs (the game's frame time) and BEGIN to END (the game update itself).
//   At BEGIN, one read of sRender's frame counter (frame_clock.hpp), so a
//   window also tells how many frames the renderer finished.
// - al.d3d12 PRE/POST_PRESENT on the present thread: the interval between the
//   game's Present calls (AL's own time stamps) and the time spent inside
//   Present (long calls mean the GPU or the flip queue holds the CPU back).
void on_begin(std::int64_t qpc, std::uint32_t thread) noexcept;
void on_end(std::int64_t qpc) noexcept;
void on_present(std::int64_t present_qpc, std::int64_t previous_qpc, std::uint32_t thread) noexcept;
void on_present_returned(std::int64_t present_qpc, std::int64_t now_qpc) noexcept;
// The game's own frame counter at a BEGIN, when frame_clock has a target.
void on_render_counter(std::uint64_t value) noexcept;

struct Window {
    std::uint64_t updates{}, presents{};
    IntervalHistogram::Summary update_interval, present_interval;
    DurationHistogram::Summary update_work, present_call;
    std::uint64_t render_frames{}, render_counter{};  // sRender counter advance; last value
    bool render_seen{};
    std::uint32_t game_thread{}, present_thread{};
};
Window take() noexcept;
std::uint32_t game_thread() noexcept;
std::uint32_t present_thread() noexcept;
// Appends "update", "present" and "render" objects to an open JSON object.
void append(std::string& out, const Window& w, double window_ms);
}  // namespace cpuopt::frame_events
