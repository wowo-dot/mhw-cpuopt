#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace cpuopt::job_timing {
// Research switch job_timing (toggles.hpp): times every job of every
// sMain::executeJob round, to see whether the jobs that finish last (the ones
// the game thread spins for, spin_probe.hpp) are long jobs that started late.
//
// An al.hooks detour on executeJob (0x225E840; the signature covers its use of
// the job count at +0xB0) points each entry of the round's job list (32 bytes:
// context at +0, function at +8) at a timing thunk for the round and puts the
// entries back when the round returns. Lists come from the caller, or are
// sMain's own (+0xF0) when the caller passes none. The thunk forwards the
// four argument registers, so it fits every job mode (0-5). Rounds with more
// jobs than `capacity`, or with a list that is not writable, run untouched.
inline constexpr std::string_view execute_job_signature =
    "40 53 55 41 57 48 83 EC 40 49 8B E8 44 8B FA 48 8B D9 4D 85 C0 74 09 44 89 89 B0 00 00 00 EB 07 44 8B 89 B0 00 00 00";
inline constexpr std::uint32_t default_jobs_offset = 0xF0, job_count_offset = 0xB0, participants_offset = 0xCC;
inline constexpr unsigned capacity = 8192;

using ExecuteJob = void(__fastcall*)(std::uintptr_t smain, int mode, std::uintptr_t jobs, std::uint32_t count, std::uintptr_t extra);
void __fastcall detour(std::uintptr_t smain, int mode, std::uintptr_t jobs, std::uint32_t count, std::uintptr_t extra);
// Where al.hooks writes the function to continue with.
ExecuteJob* next_slot() noexcept;
void set_measuring(bool on) noexcept;

// One timed job.
struct Record {
    std::uintptr_t function{}, context{};
    std::uint64_t start{}, end{};  // TSC
    std::uint32_t thread{};
};
// One round, from its records (index = position in the job list).
struct Round {
    std::uint32_t jobs{}, threads_used{};
    std::uint64_t duration{}, work{}, longest{};  // cycles
    std::uint64_t bound{};       // max(longest job, work / participants): no order of these jobs on this many threads beats it
    std::uint32_t last{};        // index of the job that ended last
    std::uint64_t last_start{};  // its start, cycles after the round began
    std::uint64_t last_duration{};
};
Round analyse(const Record* records, std::uint32_t count, std::uint64_t round_start, std::uint64_t round_end, std::uint32_t participants) noexcept;

// The "job_timing" event for the window so far, or empty. `image_base` turns
// addresses into RVAs.
std::string take_event(double t_ms, std::uintptr_t image_base);

// Whole rounds for offline replay (tools/job_sim.py): every `every`-th timed
// round (default 499, so all round types are traced in proportion to how often
// they run) is copied, up to trace_jobs jobs, into a ring of trace_slots
// rounds; the window thread drains it. A full ring drops rounds and counts them.
inline constexpr unsigned trace_jobs = 2048, trace_slots = 64;
void set_trace_every(std::uint32_t every) noexcept;
// One "job_trace" event per traced round since the last call: per job, in
// list order, [vtable RVA, function RVA, start, duration, thread], times in
// TSC cycles from the round's start, threads numbered in order of first job
// (the game thread's number in "game_thread").
std::string take_traces(std::uintptr_t image_base);
}  // namespace cpuopt::job_timing
