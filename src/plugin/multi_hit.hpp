#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace cpuopt::multi_hit {
// Switch multi_hit_timing (toggles.hpp): multi-hit attacks tick on the
// 60 fps schedule at any frame rate.
//
// An attack that hits an object goes into the hit object's multi-hit cache
// (cpObjCollision +0x9F0, 32 cCache entries of 0x70 bytes: mUID +0x50, the
// attacking node's UID; mTimer +0x54; mAge +0x58; mIsUseTimer +0x6C). While
// its UID is in the cache, the hit callback (sub_1412D6D20) drops further
// hits of that attack on that object. A new entry's mTimer is the attack's
// cAttackParam mMultiHitFrame (+0xE4, a float in 60 fps frames).
// cpObjCollision::move (sub_1412DD520) ages the cache once a frame: step =
// the object's factor (+0x9E8, 1.0 unless slowed this frame) x the frames
// elapsed at 60 fps (a global, +0x94); mAge += step; with mIsUseTimer,
// mTimer -= step, and at mTimer <= 0 the entry is cleared, so the attack may
// hit again. The time past zero is thrown away: an attack that keeps
// touching re-hits after ceil(M / step) rendered frames instead of M frames
// at 60 fps (Pierce Ammo Lv3, M = 3: 20 ticks a second at 60 fps, 18 at
// 144, 15.25 at 61). Moonbunnie's research (the "Effects of FPS"
// spreadsheet) measured this first; the code is checked against the clean
// database (2026-10-21).
//
// The switch carries that overshoot: a detour on move reads the cache before
// the game's own code runs and finds the entries it then cleared by timer
// (overshoot = step - mTimer before); on the object's next move, the next
// frame, a fresh entry (mAge 0) with the same UID, the same attack hitting
// again at once, starts with mTimer less the overshoot. Only the
// immediately following hit gets it, so an attack that stopped touching
// gains nothing; the object's slow-down factor stays in the step.
//
// The counters (always in research mode) give the measure: fresh entries,
// expiries by timer, continuations (a fresh entry right after an expiry of
// the same UID) and their interval against the nominal one: the previous
// entry's age at expiry over the new entry's mTimer (1.0 at 60 fps; 1.11
// for M = 3 at 144 fps without the switch; 1.0 on average with it).
//
// Signature (tested exe c2ebbbd2..., clean image a78ea69b...), matched once:
// move from its first byte through the global's load and its +0x94 read.
inline constexpr std::string_view move_signature =
    "48 89 7C 24 20 41 56 48 83 EC 40 48 8B 01 48 8B F9 48 89 5C 24 50 48 89 6C 24 58 48 89 74 24 60 0F 29 74 24 30 "
    "0F 29 7C 24 20 FF 50 78 8B 87 94 01 00 00 45 33 F6 21 87 90 01 00 00 0F 57 FF 44 89 B7 94 01 00 00 44 89 B7 9C "
    "01 00 00 48 8B 87 A8 01 00 00 48 85 C0 0F 84 9D 00 00 00 F6 40 0C 0E 0F 84 93 00 00 00 48 8D 9F 48 0A 00 00 41 "
    "8D 76 20 44 39 73 F8 74 74 80 7B 14 01 F3 0F 10 B7 E8 09 00 00 48 8B 05 ?? ?? ?? ?? F3 0F 59 B0 94 00 00 00";
inline constexpr unsigned global_rel = 0x87, global_next = 0x8B;  // mov rax,[rip+global] at +0x84
inline constexpr std::uint32_t cache_offset = 0x9F0, cache_stride = 0x70, cache_count = 32;
inline constexpr std::uint32_t uid_offset = 0x50, timer_offset = 0x54, age_offset = 0x58, use_timer_offset = 0x6C;
inline constexpr std::uint32_t factor_offset = 0x9E8, delta_offset = 0x94;

struct Discovery {
    bool found{};
    const char* reason{"not searched"};
    unsigned count{};
    std::uintptr_t site{}, global{};  // move, and the global holding the frame delta's object
};
Discovery discover(std::uintptr_t site, unsigned count) noexcept;
std::string discovery_event(const Discovery&, std::uintptr_t base, double t_ms, std::string_view scanner);
void set_target(const Discovery&) noexcept;
// The detour and where al.hooks stores its continuation.
void* detour() noexcept;
void** next() noexcept;
void set_carry(bool on) noexcept;     // the switch: carry the overshoot
void set_counting(bool on) noexcept;  // the counters
void on_begin() noexcept;             // al.frame BEGIN: the frame number carries are kept for
// "multi_hit": {carry, counting, calls, fresh, expired, continued, interval_ratio, carried} into an
// open JSON object; resets the window's counts.
void append(std::string& out);
}  // namespace cpuopt::multi_hit
