#pragma once
#include <windows.h>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace cpuopt::worker_spin {
// Switch spin_workers (toggles.hpp): job threads spin for a while after a
// round instead of going straight back to sleep.
//
// sMain's job threads (sub_14225F2F0) block in WaitForSingleObject on
// sMain's semaphore (+0x9F1F0) between rounds, and executeJob wakes
// min(jobs, participants) - 1 of them with one ReleaseSemaphore. Waking a
// sleeping thread is slow: on the test PC, waking 30 costs the caller 70 us
// and the last one is up about 75-110 us later. In the Ancient Forest
// (al-11, every switch on) only 14 of 31 threads took part in a round on
// average: the rest woke after the round's jobs were all taken, and rounds
// lost about 600 us a frame to the stagger (job_report's "gain" for the
// camera and unit move rounds).
//
// The change: the job threads' wait call becomes a call to `wait` (an
// al.patch CODE change of 11 bytes in the worker loop, through a 14-byte
// jump stub near the exe). `wait` spins for spin_us after a round:
// - before each round that will release threads, the executeJob detour
//   publishes how many it will release (`before_round`) and bumps a
//   generation;
// - a spinning thread that sees a new generation claims one of those places
//   and takes its permit from the semaphore by polling (no sleep, no wake);
// - a thread that finds no place keeps spinning; when spin_us has passed
//   without a round, or a claimed permit does not come, it blocks in
//   WaitForSingleObject as before.
// The semaphore still decides who runs: every permit is taken exactly once,
// by a spinning or a sleeping thread, so executeJob's count of threads done
// is unchanged. Spinning threads yield their CPU (SwitchToThread) every 20 us
// so ready threads on it still run.
//
// The patch (worker loop 0x225F330, tested exe c2ebbbd2..., clean image
// a78ea69b...):
//   0x225F330  mov rcx,[r15+9F1F0h]        kept
//   0x225F337  mov edx,0FFFFFFFFh          \ 11 bytes become a 6-byte NOP and
//   0x225F33C  call [WaitForSingleObject]  / call rel32 to the stub
//   0x225F342  test eax,eax                return point, unchanged
// The new call returns where the old one did, so a thread inside either one
// when the change goes in or out comes back to an instruction boundary.
inline constexpr std::string_view signature =
    "49 8B 8F F0 F1 09 00 BA FF FF FF FF FF 15 ?? ?? ?? ?? 85 C0 75 EA B8 01 00 00 00 F0 41 0F C1 87 F8 F1 09 00";
inline constexpr unsigned patch_offset = 7, patch_length = 11, import_operand = 12;  // call [rip+x] at +12, 6 bytes
inline constexpr std::uint32_t count_offset = 0xB0, participants_offset = 0xCC;

struct Discovery {
    bool found{};
    const char* reason{"not searched"};
    unsigned matches{};
    std::uint32_t site_rva{};
};
// From the scanner's matches; the call must go through the exe's import of
// WaitForSingleObject (the slot holds the same function this DLL imports).
Discovery discover(std::uintptr_t image_base, const std::uintptr_t* matches, unsigned count) noexcept;
std::string discovery_event(const Discovery&, double t_ms, std::string_view scanner);

// A 14-byte `jmp [rip+0]` to `function` in a page within 2 GB of `site`
// (read-execute once written); 0 if there is no room.
std::uintptr_t make_stub(std::uintptr_t site, const void* function) noexcept;
// The 11 replacement bytes for the change at `target` (the match + 7),
// calling `stub`; false if the stub is out of a rel32's reach.
bool build_replacement(std::uintptr_t target, std::uintptr_t stub, std::array<std::uint8_t, patch_length>& out) noexcept;

// The job threads' wait (rcx = the semaphore); always returns 0 or what
// WaitForSingleObject(sem, INFINITE) returns.
DWORD __fastcall wait(HANDLE semaphore);
// From the executeJob detour, just before executeJob runs on the game
// thread's behalf (not for rounds it runs alone).
void before_round(std::uintptr_t smain, std::uintptr_t jobs, std::uint32_t count) noexcept;
// On with a spin window, or off (threads block as before). Any thread.
void set_active(bool on, unsigned spin_us) noexcept;
bool active() noexcept;
// "spin": {active, spin_us, rounds, claimed, blocked, gave_up} since the last
// call, into an open JSON object.
void append(std::string& out);
}  // namespace cpuopt::worker_spin
