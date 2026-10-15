#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace cpuopt::job_cap {
// The job_cap switch (toggles.hpp): how many threads take part in each of
// sMain's job rounds.
//
// sMain::executeJob (Fexty's name, 0x225E840) wakes `participants - 1` job
// threads through one semaphore, runs jobs itself from a shared atomic job
// counter, then spins (pause) until every woken thread has reported done, even
// a thread that woke too late to find a job. The game sets participants to
// min(logical processors, 32) - 1 from GetSystemInfo at start-up, whatever the
// process affinity: 31 on a 32-thread CPU. Because jobs come from a shared
// counter, fewer participants still run every job. The value lives in sMain at
// +0xD0 and is copied into +0xCC every frame; nothing else writes +0xD0 after
// start-up (checked in the clean database, 2026-10-15).
//
// Signatures (tested exe c2ebbbd2..., clean image a78ea69b...), each must match
// exactly once; together they prove the fields used:
// - instance: the job runner's prologue (0x225F620), mov r8,[rip+sMain] at +21;
//   the global is 0x51C4148;
// - release: executeJob reads participants [+0CCh], the job count [+0B0h] and
//   the semaphore [+9F1F0h] and zeroes its counters [+9F1F8h]/[+9F1FCh] (0x225EA7D);
// - spin: the busy wait on the done counter [+9F1FCh] (0x225EAC7);
// - copy: the per-frame copy of [+0D0h] into [+0CCh] (0x225FA05).
inline constexpr std::string_view instance_signature =
    "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 20 4C 8B 05 ?? ?? ?? ?? 48 8B F1 4C 8B 31";
inline constexpr std::string_view release_signature =
    "8B 83 CC 00 00 00 8B BB B0 00 00 00 48 8B 8B F0 F1 09 00 44 89 A3 F8 F1 09 00 44 89 A3 FC F1 09 00 3B F8";
inline constexpr std::string_view spin_signature = "8B 83 FC F1 09 00 3B C7 74 0C F3 90 8B 83 FC F1 09 00 3B C7";
inline constexpr std::string_view copy_signature = "80 B9 8C 00 00 00 00 48 8B D9 8B 81 D0 00 00 00 89 81 CC 00 00 00";
inline constexpr unsigned instance_operand = 21;  // offset of the 7-byte RIP-relative load in the instance match

inline constexpr std::uint32_t threads_created_offset = 0x90;  // byte: executeJob created its threads
inline constexpr std::uint32_t participants_offset = 0xCC;     // threads per round, the game thread included
inline constexpr std::uint32_t configured_offset = 0xD0;       // copied into participants every frame
inline constexpr std::uint32_t detected_offset = 0xAF680;      // GetSystemInfo's dwNumberOfProcessors

struct Discovery {
    bool found{};
    const char* reason{"not searched"};
    unsigned counts[4]{};  // instance, release, spin, copy
    std::uint32_t instance_rva{}, global_rva{};
    bool expected_values{};  // the tested build's sites
};
// `counts` from the scanner for the four signatures, in the order above;
// `instance_site` is the first instance match.
Discovery discover(std::uintptr_t image_base, const unsigned (&counts)[4], std::uintptr_t instance_site) noexcept;
std::string discovery_event(const Discovery&, double t_ms, std::string_view scanner);
void set_target(std::uintptr_t image_base, const Discovery&) noexcept;

// Participants wanted (game thread included); 0 = the game's own count. Any thread.
void request(unsigned participants) noexcept;
// al.frame BEGIN, on the game thread (no job round runs then): learns the
// game's count once its job threads exist, checks it against the start-up
// formula, writes a changed request, and stops for good if anything else
// changed the field. One relaxed load and two guarded reads per frame.
void on_begin() noexcept;
// EXITING: puts the game's own count back.
void restore() noexcept;

// "job": {state, native, requested, applied, applies} into an open JSON object.
void append(std::string& out);
}  // namespace cpuopt::job_cap
