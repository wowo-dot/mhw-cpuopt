#pragma once
#include <cstdint>

namespace cpuopt::spin_probe {
// Read-only look at sMain's job slots while the game thread spins in
// sMain::executeJob, waiting for its job threads (job_cap.hpp). Each
// participant has a 0x48-byte slot from sMain+0x9E8F0 (the game thread is
// slot 0, `lea rcx,[rbx+9E8F0h]` at 0x225EABB; job thread i is slot i, as the
// worker loop computes it); the job runner stores the job a thread is running
// at slot+0x30 (`mov [rsi+30h], rax`, 0x225F6BB) and 0 once no job is left. A
// job entry is 32 bytes: context at +0, function at +8. Checked on the clean
// database, 2026-10-15. The other threads keep running while this reads, so
// a reading is a snapshot, not a coherent state.
inline constexpr std::uint32_t slots_offset = 0x9E8F0, slot_stride = 0x48, slot_job = 0x30, job_function = 8;
inline constexpr unsigned max_slots = 32;

struct Reading {
    unsigned busy{};                        // slots with a job, the game thread's included
    std::uintptr_t functions[max_slots]{};  // their job functions
};
// False if sMain or its slots are not readable. Any thread; no allocation.
bool read(std::uintptr_t smain, Reading& out) noexcept;
}  // namespace cpuopt::spin_probe
