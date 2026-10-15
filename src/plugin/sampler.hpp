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
// The "profile" event with the samples since the last call, or empty.
std::string take_event(double t_ms, unsigned toggles, const std::string& marker);
}  // namespace cpuopt::sampler
