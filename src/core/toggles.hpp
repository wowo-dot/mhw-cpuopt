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
    // 128: spin_workers, rejected (al-13) and removed in v0.16.0
    unit_threshold = 256U,    // sMhUnit's line threshold (run alone or on the job threads) set to
                              // control.ini unit_us instead of the game's 60 us (unit_lines.hpp)
    // 512: cache_steam_running, no measurable gain (al-15, al-22), removed in v0.16.0
    smaller_cpu = 1024U,      // research: the process confined to control.ini cpu_count logical
                              // processors and job rounds to as many threads (cpu_limit.hpp)
    cache_ccd = 2048U,        // the process confined to the processors sharing the largest L3 (the
                              // 3D V-Cache die), job rounds sized to match (cpu_limit.hpp)
    budget = 4096U,           // research: each frame of the game thread split into limiter, job
                              // rounds, Present path, GPU wait and serial work (frame_budget.hpp)
    uncap = 8192U,            // research: the engine's frame cap (sMain mMaxFps) raised to
                              // control.ini max_fps, for measuring capped scenes (frame_cap.hpp)
    sleep_limiter = 16384U,   // the engine's frame limiter waits on a high-resolution timer until
                              // control.ini margin_us before its deadline instead of spinning (limiter.hpp)
    parallel_contacts = 32768U,  // sCollision's contact and setup passes as job rounds, the flags the
                                 // engine sets with more than one job thread (contacts.hpp)
};
inline constexpr unsigned toggle_mask = 65535U & ~(128U | 512U);  // every defined bit

inline std::atomic<unsigned> toggles{};
inline bool on(Toggle t) noexcept { return (toggles.load(std::memory_order_relaxed) & t) != 0; }
}  // namespace cpuopt
