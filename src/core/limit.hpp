#pragma once
#include <string>

namespace cpuopt::limit {
// What sets the frame rate right now, for the Mods page's "Limited by" line.
// From the game thread's own CPU time per frame (its cycle count: no sleep
// or wait for the GPU counts, its spinning does) against the frame time:
// when the game thread runs nearly the whole frame, the CPU is the limit;
// when it waits for much of it, something else is (the graphics card,
// V-Sync, a frame rate limit). Thresholds from the measurements of
// 2026-10-16/17 (engine notes): CPU-limited halves ran at 0.94 or more of
// what the game thread alone allowed, GPU-limited ones at 0.90 or less (the
// 13700K's), mixed ones in between.
enum class Kind { unknown, cpu, even, gpu, vsync, frame_cap };
struct Input {
    double fps{};        // frames per second, measured
    double busy_ms{};    // the game thread's CPU time per frame
    double cap_fps{};    // the engine's frame cap now (0: unknown)
    bool vsync{};        // the game's V-Sync option is on
};
struct Verdict {
    Kind kind{Kind::unknown};
    double cpu_fps{};  // what the game thread alone would allow
};
Verdict classify(const Input& in) noexcept;
// The page's text, at most 30 characters: "CPU", "graphics card (CPU: 280 fps)",
// "V-Sync (CPU: 280 fps)", "frame rate limit", "CPU and GPU about even",
// "measuring".
std::string text(const Verdict& v);
}  // namespace cpuopt::limit
