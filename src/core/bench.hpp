#pragma once
#include <cstdint>
#include <span>
#include <string>

namespace cpuopt::bench {
// Player mode's benchmark (the Mods page's "Run benchmark"): the chosen
// optimizations off and on in turns, in one spot, as `pairs` pairs of
// windows ordered off-on, on-off, off-on... so a slow drift in the scene
// falls on both sides. The first part of every window (settle_ms) is not
// counted: the switches take a few frames to apply.
inline constexpr unsigned pairs = 8, windows = 2 * pairs;
bool window_on(unsigned i) noexcept;           // the optimizations' state in window i (0 to windows - 1)
double settle_ms(double window_ms) noexcept;   // 3 s, or a fifth of a shorter window
double countdown_ms(double window_ms) noexcept;  // from the menu closing to the first window: 10 s or one window

// One measured window. Frame times from al.frame's BEGINs; CPU times per
// frame from frame_budget (the game thread outside the frame limiter and the
// Present call) and from the process's times (all threads).
struct Window {
    bool on{};
    double seconds{};      // counted time
    std::uint64_t frames{};
    double p50_ms{}, p99_ms{}, max_ms{};  // frame time
    std::uint64_t cpu_frames{};           // frames frame_budget measured (0: not measured)
    double cpu_ms{};                      // the game thread's CPU time per frame
    double process_ms{};                  // all threads' CPU time per frame (0: not measured)
    double fps() const noexcept { return seconds > 0 ? double(frames) / seconds : 0.0; }
};

// Medians over each state's windows; gains as on / off - 1.
struct Result {
    unsigned pairs{};  // pairs with frames in both windows
    double off_fps{}, on_fps{}, gain{};
    double gain_low{}, gain_high{};  // the pairs' smallest and largest gain
    double off_p99{}, on_p99{};
    bool cpu{};  // the game thread's CPU time measured in every window
    double off_cpu{}, on_cpu{}, cpu_change{};
    bool process{};
    double off_process{}, on_process{}, process_change{};
};
Result summarize(std::span<const Window> w);

// What the report says about the PC and the run. Plain text, no paths.
struct Machine {
    std::string when;      // local date and time
    std::string version;   // cpuopt's
    std::string game;      // "421810, the tested build"
    std::string cpu;       // "AMD Ryzen 9 9950X3D 16-Core Processor, 32 threads"
    std::string gpu;
    std::string windows;   // "10.0.26200"
    std::string display;   // the game's own settings: resolution, screen mode, frame rate limit, V-Sync
    std::string switches;  // the optimizations of the "on" windows
    std::string modules;   // other DLLs loaded from outside Windows
    bool fps_cap_removed{};
    unsigned pauses{};     // windows measured again (menu open, game in the background)
    double window_s{};
};
// The report's section for cpuopt-benchmark.txt (lines end in '\n').
std::string report(const Machine& m, std::span<const Window> w, const Result& r);
// The Mods page's two lines (its value boxes fit about 30 characters):
// "+58% fps, 172 to 270" and "-32%, 5.03 to 3.44 ms" (or "not measured").
std::string fps_line(const Result& r);
std::string cpu_line(const Result& r);
}  // namespace cpuopt::bench
