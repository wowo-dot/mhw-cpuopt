#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace cpuopt::unit_lines {
// Switch unit_threshold (toggles.hpp): when sMhUnit runs a unit line on the
// game thread alone and when it hands it to the job threads.
//
// sMhUnit (sMhMain +0xAF8D0) keeps its units in lines (248 bytes each from
// +0x78). Three passes walk every line each frame: move (sub_14221ACE0,
// cUnit::genericMove), a second step (sub_14221B400, sub_142228F20 per
// unit) and a third (sub_14221C280). A line flagged parallel (bit 0 of its
// flags at +0x98) goes to executeJob only if the time it took last frame
// (+0xA8, +0xAC, ... in us, measured by the pass) is above sMhUnit +0x4A58;
// otherwise the game thread walks the line unit by unit. The constructor
// (sub_142217D00) sets that threshold to 60 us. In the Ancient Forest with
// v0.6's switches on (al-11), the game thread spent about 15% of its samples
// in these passes outside executeJob, walking lines alone.
//
// The switch writes control.ini's unit_us there instead (checked first: the
// dword must hold the constructor's 60; the dword after it is an atomic
// counter, sub_14221A890), and writes 60 back when off and at EXITING. If
// anything else changes the field, it stops for good.
//
// Signatures (tested exe c2ebbbd2..., clean image a78ea69b...), each matched
// once: the threshold read in the move pass and in the second pass, the
// constructor's 60, and sMhMain's constructor storing sMhUnit at +0xAF8D0.
inline constexpr std::string_view move_signature =
    "F6 C2 01 74 16 8B 87 58 4A 00 00 C6 85 90 00 00 00 01 39 84 3E A8 00 00 00 77 07";
inline constexpr std::string_view step_signature = "F6 C1 01 74 15 8B 87 58 4A 00 00 41 39 84 3E AC 00 00 00 76 05 40 B5 01";
inline constexpr std::string_view ctor_signature = "48 C7 83 58 4A 00 00 3C 00 00 00";
inline constexpr std::string_view owner_signature =
    "B9 C0 DE 3D 00 E8 ?? ?? ?? ?? 48 85 C0 74 12 BA 40 00 00 00 48 8B C8 44 8B C2 E8 ?? ?? ?? ?? EB 03 49 8B C4 48 89 83 D0 F8 0A 00";
inline constexpr std::uint32_t unit_offset = 0xAF8D0, threshold_offset = 0x4A58, native_threshold = 60;

struct Discovery {
    bool found{};
    const char* reason{"not searched"};
    unsigned counts[4]{};  // move, step, ctor, owner
};
Discovery discover(const unsigned (&counts)[4]) noexcept;
std::string discovery_event(const Discovery&, double t_ms, std::string_view scanner);
// The address of sMain's global (job_cap's discovery); 0 leaves it untouched.
void set_target(std::uintptr_t smain_global) noexcept;
// The threshold wanted, in us; 0 = the game's own 60. Applied at once if
// sMhUnit exists, else at the next `apply`. Window thread only.
void request(unsigned us) noexcept;
// Retries a pending request (each window). Window thread only.
void apply() noexcept;
// EXITING: the game's 60 back. Any thread; safe more than once.
void restore() noexcept;
// "units": {state, native, requested, applied} into an open JSON object.
void append(std::string& out);
}  // namespace cpuopt::unit_lines
