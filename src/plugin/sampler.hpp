#pragma once
#include <windows.h>
#include <cstdint>
#include <span>
#include <string>

namespace cpuopt::sampler {
// Wall-clock stack sampler, active only while toggle sample_profile is on.
// About 1,000 times a second it suspends each tracked thread (the game and
// present threads and the busiest others, at most six) and two other threads in
// rotation, copies the stack with RtlVirtualUnwind against exception tables
// read up front, and resumes the thread. Nothing allocates or takes a lock
// while a thread is suspended. Whole stacks are kept (function starts, innermost
// first) with a count, per thread, so the report can rebuild self and
// inclusive time per function and call paths. Waiting threads are sampled too.
// The innermost frame's exact instruction is also counted when it is in the
// game exe ("leaves"), so a report can tell which loop of a function is hot.
void start() noexcept;
// Ends the sampler thread (EXITING); true if it ended within the timeout.
bool stop(unsigned timeout_ms) noexcept;
DWORD thread_id() noexcept;
// Window thread only. Threads are tracked from the next sampling tick.
void set_tracked(std::span<const DWORD> threads) noexcept;
void set_excluded(std::span<const DWORD> threads) noexcept;
// When a sampled thread is in [begin, end) (executeJob's spin), also read
// sMain's job slots (spin_probe.hpp) through the sMain pointer at `global`.
void set_spin_probe(std::uintptr_t begin, std::uintptr_t end, std::uintptr_t global) noexcept;
// Job-thread focus: besides the tracked threads and the rotation, sample
// `per_tick` threads each tick from the largest group of threads sharing a
// start address (the game's 30 job threads), in turn; 0 = off. Any thread.
void set_focus(unsigned per_tick) noexcept;
// Follow mode: job_timing's thunk calls follow_begin when a job of the
// followed class starts (job_timing::set_follow) and follow_end when it
// returns. The sampler thread wakes and samples that thread every
// `gap_us` until the job ends; those samples carry flag 4, and a sample
// whose thread had left the job by the time it stopped is dropped.
void follow_begin(DWORD tid) noexcept;  // any thread
void follow_end(DWORD tid) noexcept;
void set_follow_gap(unsigned gap_us) noexcept;
// The "profile" event with the samples since the last call, or empty.
std::string take_event(double t_ms, unsigned toggles, const std::string& marker);
}  // namespace cpuopt::sampler
