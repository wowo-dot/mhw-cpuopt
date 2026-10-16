#pragma once
#include <atomic>

namespace cpuopt {
// Runtime switches, set live from the run folder's control.ini (control.hpp).
// Every bit starts off. Research switches observe; optimization switches,
// when they exist, change one engine path each and are measured on and off
// in the same scene.
enum Toggle : unsigned {
    sample_profile = 1U,      // research: stack sampler (sampler.hpp); suspends sampled threads briefly
    job_thread_cap = 2U,      // sMain's job rounds use control.ini's job_threads threads, not the game's
                              // count from all logical processors (job_cap.hpp)
    time_jobs = 4U,           // research: times every job of sMain's job rounds through a detour on
                              // executeJob, installed the first time this is on (job_timing.hpp)
    order_jobs = 8U,          // the camera draw round's job list reordered (control.ini job_order:
                              // 1 reversed, 2 learned expensive types first; job_timing.hpp)
    plain_prim_stores = 16U,  // sPrimitive's record loop writes its records without reading them back
                              // (an al.patch code change; prim_stores.hpp)
    reuse_server_time = 32U,  // Steam's server time reused for control.ini time_ms (detours on the
                              // game's two callers of GetServerRealTime; server_time.hpp)
    inline_rounds = 64U,      // job rounds whose jobs take less than control.ini inline_us in total
                              // run on the game thread alone, learned per call site (job_timing.hpp)
    spin_workers = 128U,      // job threads spin for control.ini spin_us after a round and take the
                              // next round's places without being woken (worker_spin.hpp)
    unit_threshold = 256U,    // sMhUnit's line threshold (run alone or on the job threads) set to
                              // control.ini unit_us instead of the game's 60 us (unit_lines.hpp)
    cache_steam_running = 512U,  // SteamAPI_IsSteamRunning answered once per time_ms at its two
                                 // per-frame call sites (al.patch; steam_running.hpp)
    smaller_cpu = 1024U,      // research: the process confined to control.ini cpu_count logical
                              // processors and job rounds to as many threads (cpu_limit.hpp)
    cache_ccd = 2048U,        // the process confined to the processors sharing the largest L3 (the
                              // 3D V-Cache die), job rounds sized to match (cpu_limit.hpp)
    budget = 4096U,           // research: each frame of the game thread split into limiter, job
                              // rounds, Present path, GPU wait and serial work (frame_budget.hpp)
    uncap = 8192U,            // research: the engine's frame cap (sMain mMaxFps) raised to
                              // control.ini max_fps, for measuring capped scenes (frame_cap.hpp)
};
inline constexpr unsigned toggle_mask = 16383U;  // every defined bit

inline std::atomic<unsigned> toggles{};
inline bool on(Toggle t) noexcept { return (toggles.load(std::memory_order_relaxed) & t) != 0; }
}  // namespace cpuopt
