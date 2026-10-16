#pragma once
#include <string>

namespace cpuopt::cpu_limit {
// Research switch smaller_cpu (toggles.hpp): the game as on a smaller CPU.
//
// On the test PC (32 logical processors) the job threads' work is mostly
// hidden: in the Ancient Forest the jobs hold about 13 ms of work a frame,
// spread over 30 threads. On a 6-core/12-thread or 4-core/8-thread CPU that
// work decides the frame rate. The switch confines the whole process to the
// first `cpu_count` logical processors of its affinity (SMT siblings are
// adjacent, so 12 = six cores with their siblings) and the runtime asks
// job_cap for the same number of participants per round, as the game would
// set on such a CPU (its job threads stay created; rounds use count - 1).
// Off and at EXITING the process gets its own affinity back.
//
// Window thread only.
void set(unsigned cpu_count) noexcept;  // 0 = the process's own affinity
void restore() noexcept;                // EXITING; safe more than once
// "cpus": {limit, mask, applied} into an open JSON object.
void append(std::string& out);
}  // namespace cpuopt::cpu_limit
