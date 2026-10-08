#pragma once
#include <windows.h>
#include <span>
#include <string>

namespace cpuopt::sampler {
// Wall-clock stack sampler, active only while toggle sample_profile is on.
// About 1,000 times a second it suspends each tracked thread (the busiest
// threads of the last window, at most four) and two other threads in
// rotation, copies the stack with RtlVirtualUnwind against exception tables
// read up front, and resumes the thread. Nothing allocates or takes a lock
// while a thread is suspended. Whole stacks are kept (function starts, innermost
// first) with a count, per thread, so the report can rebuild self and
// inclusive time per function and call paths. Waiting threads are sampled too.
void start() noexcept;
DWORD thread_id() noexcept;
// Window thread only. Threads are tracked from the next sampling tick.
void set_tracked(std::span<const DWORD> threads) noexcept;
void set_excluded(std::span<const DWORD> threads) noexcept;
// The "profile" event with the samples since the last call, or empty.
std::string take_event(double t_ms, unsigned toggles, const std::string& marker);
}  // namespace cpuopt::sampler
