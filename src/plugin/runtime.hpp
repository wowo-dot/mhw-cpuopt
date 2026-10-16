#pragma once
#include "frame_budget.hpp"
#include "frame_clock.hpp"
#include "job_cap.hpp"
#include "prim_stores.hpp"
#include "server_time.hpp"
#include "steam_running.hpp"
#include "unit_lines.hpp"
#include "contacts.hpp"
#include "worker_spin.hpp"
#include "../core/settings.hpp"
#include <windows.h>
#include <cstdint>
#include <string>

namespace cpuopt::runtime {
// What the plugin entry (al_entry.cpp) learned from Astera Loader.
struct Host {
    std::wstring folder;                  // the plugin folder (real path)
    std::uint32_t api_version{};          // Astera Loader's API version
    std::string game_version, exe_sha256; // from al.game, if it answered
    frame_clock::Discovery discovery;     // the frame counter, searched at PRE_MAIN
    job_cap::Discovery job;               // sMain's job thread count, searched at PRE_MAIN
    std::string scanner;                  // "al.scanner" or "own"
    bool frame_events{}, present_events{};  // subscribed to al.frame / al.d3d12
    // al.frame's state (AL_FRAME_*), or -1 without al.frame; any thread.
    int (*frame_state)() noexcept {};
    // Puts the executeJob detour (job_timing.hpp) in or out of the chain; the
    // first "on" registers it. False with the reason when it cannot. Any thread.
    bool (*job_hook)(bool on, const char** reason) noexcept {};
    std::string order_site;  // the camera round's call site: "ok" or why not
    prim_stores::Discovery prim;    // sPrimitive's record loop, searched at PRE_MAIN
    server_time::Discovery server;  // the game's two server time calls, searched at PRE_MAIN
    worker_spin::Discovery spin;    // the job threads' wait, searched at PRE_MAIN
    unit_lines::Discovery units;    // sMhUnit's line threshold, searched at PRE_MAIN
    steam_running::Discovery running;  // the two IsSteamRunning calls, searched at PRE_MAIN
    contacts::Discovery contacts;      // sCollision's parallel flags, searched at PRE_MAIN
    frame_budget::Discovery budget;    // the limiter's function and cD3DRender::present
    // Put the prim_stores change and the server_time detours in or out;
    // false with the reason when they cannot. Any thread.
    bool (*prim_patch)(bool on, const char** reason) noexcept {};
    bool (*server_hooks)(bool on, const char** reason) noexcept {};
    bool (*spin_patch)(bool on, const char** reason) noexcept {};
    bool (*running_patch)(bool on, const char** reason) noexcept {};
    bool (*budget_hooks)(bool on, const char** reason) noexcept {};
    // Player mode: the hotkey for the master switch changed (0: none). Any thread.
    void (*hotkey)(std::uint32_t key) noexcept {};
};

// al_plugin_init. With <folder>\cpuopt.dll.output-dir (a research run):
// opens the run log there and starts the window thread. Without it, player
// mode: settings from <folder>\cpuopt.ini (made with the defaults if
// missing), the log <folder>\cpuopt-session.log (new each launch, at most 1 MB), the
// switches applied on the runtime's own thread, no sampler and no window
// statistics. False, with the reason, when neither log can be opened: the
// plugin then stays idle.
bool start(HMODULE self, const Host& host, std::string& reason) noexcept;
// EXITING: the last window, the sampler and poller stopped, the window
// thread joined (at most about 2 s). Safe to call more than once.
void stop(const char* why) noexcept;
// Player mode.
bool player() noexcept;
Settings settings() noexcept;             // any thread
void change(const Settings& s) noexcept;  // any thread; applied and saved by the runtime's thread
void flip_enabled() noexcept;             // the master switch's hotkey; any thread
// Toggle bits of switches that are on but could not be applied.
unsigned failed_toggles() noexcept;
// The benchmark (bench.hpp): the page's Run button, from CMFM's thread. It
// starts 10 s after the Mods page closes, pauses while the page is open or
// the game is in the background (that window runs again), and stops at any
// settings change. The report goes to <folder>\cpuopt-benchmark.txt.
void bench_press() noexcept;
void menu_shown(bool open) noexcept;  // CMFM's Mods page opened or closed
bool bench_busy() noexcept;           // asked for, waiting or running
// The page's two lines: the frame rate's result or where the run is (0), and
// the CPU time's result or the run's state (1). About 30 characters each.
std::string bench_text(unsigned line);

// One line into the run log from a phase callback (GAME_READY, SHUTDOWN).
void note(const char* event, const char* key, long long value) noexcept;
// DllMain at process exit: a last line if the log is free.
void at_exit() noexcept;
}  // namespace cpuopt::runtime
