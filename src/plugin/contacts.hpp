#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace cpuopt::contacts {
// Switch parallel_contacts (toggles.hpp): sCollision's collider runs its
// contact pass (and its setup pass) as job rounds, as the engine itself
// chooses when its job system has more than one thread.
//
// sMhMain's constructor builds sCollision (stored at sMain +0xAF8E8); its
// collider (sCollision +0x84E8, run each frame by sub_1422D1440 while the
// byte at +0x8528 is set) is set up there by sub_14248A160, which sets the
// collider's flags +0x3C (setup pass) and +0x3D (contact pass) to
// "sMain +0xCC != 1": more than one job participant. That runs during
// sMhMain's construction, before the job threads exist, so both flags stay 0
// for the whole session (read-only check in al-22: 0 and 0 with 16
// participants), and every contact (sCollision::NodeList::executeContact)
// runs on the game thread: about 4% of its time in the Velkhana fight
// (al-22). With the contact flag set, executeContact queues
// executeTargetActiveNodeContact per active node on sMain's own job list and
// runs one executeJob round; with the setup flag, the setup pass
// (movePhase1_runSetupObjectCollisionSystem) queues its node lists the same
// way. Both paths are the engine's own; the BVH queries they make take a
// stack per job thread (sub_1425B8CD0).
//
// The switch sets the flags chosen by control.ini contacts (bit 0 the
// contact pass, bit 1 the setup pass; 3, both, is what the engine would set)
// at al.frame BEGIN on the game thread, where no collision runs, after
// checking the object (sCollision's vtable, the flags 0 or 1), and puts back
// what it set when off and at EXITING. If the engine's own setup clears a
// flag again, it is set again (counted). +0x3E (the contact end pass, which
// the engine sets to 0) is left alone.
//
// Signatures (tested exe c2ebbbd2..., clean image a78ea69b...), each matched
// once: the setup's flag stores (with sMain's global), sub_1422D1440's
// collider call, sMhMain's constructor storing sCollision at +0xAF8E8, and
// sCollision's constructor setting its vtable.
inline constexpr std::string_view setup_signature =
    "48 8B 05 ?? ?? ?? ?? 83 B8 CC 00 00 00 01 0F 95 C0 88 46 3C 88 46 3D 48 8B 5C 24 70 44 88 76 3E";
inline constexpr std::string_view move_signature = "80 BB 28 85 00 00 00 74 0C 48 8D 8B E8 84 00 00 E8";
inline constexpr std::string_view owner_signature = "E8 ?? ?? ?? ?? EB 03 49 8B C4 BA 10 00 00 00 48 89 83 E8 F8 0A 00";
inline constexpr std::string_view vtable_signature = "E8 ?? ?? ?? ?? 33 D2 48 8D 05 ?? ?? ?? ?? 48 89 03 B9 00 01 00 00";
inline constexpr std::uint32_t collision_offset = 0xAF8E8, collider_offset = 0x84E8, active_offset = 0x8528;
inline constexpr std::uint32_t contact_flag = 0x3D, setup_flag = 0x3C;
// setup_signature: sMain's global, rel32 at +3 (next instruction at +7);
// vtable_signature: sCollision's vtable, rel32 at +10 (next at +14).
inline constexpr unsigned global_rel = 3, global_next = 7, vtable_rel = 10, vtable_next = 14;
inline constexpr unsigned contact_pass = 1, setup_pass = 2;

struct Discovery {
    bool found{};
    const char* reason{"not searched"};
    unsigned counts[4]{};          // setup, move, owner, vtable
    std::uintptr_t global{}, vtable{};
};
// `sites`: the first match of each signature; `smain_global`: job_cap's
// (the setup's own global must be the same one).
Discovery discover(const std::uintptr_t (&sites)[4], const unsigned (&counts)[4], std::uintptr_t smain_global) noexcept;
std::string discovery_event(const Discovery&, std::uintptr_t base, double t_ms, std::string_view scanner);
void set_target(const Discovery&) noexcept;
// 0 = the engine's flags; bit 0 the contact pass, bit 1 the setup pass. Any
// thread; applied at the next BEGIN.
void request(unsigned mode) noexcept;
void on_begin() noexcept;  // al.frame BEGIN, game thread
void restore() noexcept;   // EXITING
// "contacts": {state, mode, contact, setup, writes} into an open JSON object;
// resets the window's write count.
void append(std::string& out);
}  // namespace cpuopt::contacts
