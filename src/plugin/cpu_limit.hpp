#pragma once
#include <windows.h>
#include <cstdint>
#include <string>

namespace cpuopt::cpu_limit {
// The process's processor affinity, for two switches (toggles.hpp):
//
// - research switch smaller_cpu: the game as on a smaller CPU. On the test PC
//   (32 logical processors) the job threads' work is mostly hidden; the
//   switch keeps the first control.ini cpu_count logical processors (SMT
//   siblings are adjacent, so 12 = six cores with their siblings) and the
//   runtime asks job_cap for as many participants per round.
// - cache_ccd: the game on the processors that share the largest L3 cache
//   (the 3D V-Cache die of a 7950X3D or 9950X3D), job rounds sized to match.
//   al-17 (Ancient Forest, best set): 241.4 fps on CCD0's 16 against 230.2
//   on all 32, three rounds of three; Windows' own X3D handling did not do
//   it for MHW. On a CPU whose L3 caches are one or equal, nothing changes.
//
// Off and at EXITING the process gets its own affinity back.
struct Ccd {
    bool found{};
    const char* reason{"not searched"};
    DWORD_PTR mask{};                   // the largest L3's processors, within the process's affinity
    unsigned count{}, caches{};         // processors in `mask`; L3 caches seen
    std::uint32_t largest_kb{}, next_kb{};  // the largest L3 and the next one
};
// GetLogicalProcessorInformationEx's L3 caches (processor group 0).
Ccd find_cache_ccd() noexcept;
std::string ccd_event(const Ccd&, double t_ms);
// The lowest `count` bits set in `mask`.
DWORD_PTR first_bits(DWORD_PTR mask, unsigned count) noexcept;
// The process's own affinity, before any change.
DWORD_PTR original() noexcept;
// Window thread only. 0 = the process's own affinity.
void set_mask(DWORD_PTR mask, unsigned limit) noexcept;  // `limit`: cpu_count for the log, 0 if none
void restore() noexcept;                                 // EXITING; safe more than once
// "cpus": {limit, applied, mask} into an open JSON object.
void append(std::string& out);
}  // namespace cpuopt::cpu_limit
