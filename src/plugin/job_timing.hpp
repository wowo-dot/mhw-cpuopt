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

// Switch order_jobs: the camera draw round's job list reordered for the
// round, and put back when it returns.
//
// The camera draw round is sMain::executeJob called from sub_14221A050's
// prepared-list path (0x221A39D; the caller passes its own list and count).
// Its call site is found by `camera_signature` (exactly once, and its call
// must go to executeJob) and recognised by the return address.
// - mode 1: the list reversed. al-07's replay predicted 254 us per frame back.
// - mode 2: learned: one camera round in 16 is timed (as job_timing does)
//   and each object class keeps a running mean of its job's duration; job
//   types whose mean is at least the threshold go first, slowest first, the
//   rest in the game's order. Predicted 239-256 us per frame back at 20 us.
// Jobs are independent, and which thread draws which object already varies
// from round to round, so the order changes nothing but timing.
inline constexpr std::string_view camera_signature =
    "46 8B 8C B6 B0 DE 3D 00 4E 8B 04 C6 48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 45 8B C7 48 8B D3 48 8B CD E8";
inline constexpr unsigned camera_call = 19, camera_return = 24;  // offsets of the call and the return address
inline constexpr unsigned learn_every = 16;
// The camera round's return address; 0 leaves ordering unavailable.
void set_camera_return(std::uintptr_t address) noexcept;
// 0 off, 1 reversed, 2 learned with this threshold. False if unavailable.
bool set_order(int mode, unsigned threshold_us) noexcept;
// "order": the switch's state and counts into an open JSON object.
void append_order(std::string& out);

// Switch inline_rounds: rounds whose jobs take little time in total run on
// the game thread alone.
//
// executeJob wakes min(jobs, participants) - 1 job threads through a
// semaphore and spins until every one of them has reported back, however
// little work the round holds. In al-07, four rounds a frame held 11-20 us of
// work but took 21-31 us (sub_1422E248F: 314 jobs, 19.5 us of work; also
// sub_1426D9328, sub_1411DA84C, sub_1423D29E1). With participants at 1,
// executeJob's own single-thread path runs the list on the calling thread
// (the path the game takes on a one-core machine); the detour sets +0xCC to 1
// for the call and puts the value back when it returns. Only after the job
// threads exist (+0x90), so their creation never sees 1.
//
// Learned per call site (the return address): the first `inline_learn`
// rounds and then one in `learn_every` are timed, and the site keeps a
// running mean of its rounds' work (weight 1/8). A site runs alone while its
// mean is below the threshold, and goes back to the job threads above 1.5x
// the threshold. Sites far above it are timed one round in 256. The camera
// round, while job order is on, stays with the order switch.
inline constexpr unsigned site_capacity = 64, inline_learn = 8, threads_created_offset = 0x90;
void set_inline(bool on, unsigned threshold_us) noexcept;
// "inline": state, counts and the sites, into an open JSON object.
void append_inline(std::string& out, std::uintptr_t image_base);
}  // namespace cpuopt::job_timing
