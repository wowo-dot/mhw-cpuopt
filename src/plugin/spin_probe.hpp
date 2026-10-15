#pragma once
#include <cstdint>

namespace cpuopt::spin_probe {
// Read-only look at a job round while the game thread spins in
// sMain::executeJob (job_cap.hpp). In the spin loop (0x225EAD1) rbx holds
// sMain and edi the number of job threads released for this round. Each
// released thread increments sMain's started counter [+9F1F8h] when it wakes
// (its slot number) and the done counter [+9F1FCh] when it has run out of
// jobs; executeJob zeroes both before releasing (0x225EA90, 0x225EA97). So at
// a spin sample:
//   released - started = threads not yet running (wake-up and scheduling),
//   started - done     = threads still running jobs.
// (Each thread's slot keeps its last job after it finishes, so slot contents
// cannot tell running from finished; v0.3.1 read them and was misleading.)
// The other threads keep running while this reads: a snapshot, not a
// coherent state. Checked on the clean database, 2026-10-15.
inline constexpr std::uint32_t started_offset = 0x9F1F8, done_offset = 0x9F1FC;
inline constexpr unsigned max_threads = 32;

struct Reading {
    std::uint32_t released{}, started{}, done{};
    unsigned not_started() const noexcept { return released > started ? released - started : 0; }
    unsigned running() const noexcept { return started > done ? started - done : 0; }
};
// `smain` from the spinning thread's rbx, `released` from its edi. False if
// sMain's counters are not readable or the values are not plausible.
bool read(std::uintptr_t smain, std::uint32_t released, Reading& out) noexcept;
}  // namespace cpuopt::spin_probe
