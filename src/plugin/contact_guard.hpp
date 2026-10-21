#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace cpuopt::contact_guard {
// The lock that makes switch parallel_contacts safe (contacts.hpp).
//
// With the collider's contact flag set, executeTargetActiveNodeContact runs
// one job per active node on all job threads. Its geometry tests (BVH
// queries, nCollision::cCollisionNodeObject::hitCheck) take a stack per job
// thread and are built for that, but the callbacks they reach are not:
// - hitCheckCallback (sub_14248A080) calls, for every contact, the active
//   node's callback (code 0) and then the hit node's own callback (code 1,
//   through the node's +128/+208 by sub_142462F00), which can be any
//   object's reaction;
// - the job itself calls the active node's callback with code 4 for a node
//   that hit and code 2 at its end.
// The four callbacks enumContact registers (sCollision::Collider::
// enumContact, 2026-10-21 audit of the clean database) all write shared
// state without a lock for code 0:
// - sub_1412D6D20, cpObjCollision's hit callback: the hit object's record
//   list through sub_1412D5370, which takes a slot with lock xadd but fills
//   it without a lock and compacts the list under its critical section only
//   on overflow (>= 128 records), copying records another thread is still
//   writing: a record with a null partner (+0x40) that the reader
//   (sub_1412D5E90) dereferences at +0x240 (the crash at exe+0x12D61E3 on
//   hybrid Intel CPUs, 2026-10-20); also its attack history (target +0xA40);
// - sub_142396960: a check-then-write "nearest contact" in its context;
// - sub_1425109C0: array[count++] with count clamped from 9 to 7 after the
//   store; slot 8 is the count itself, so a second thread can read a pointer
//   as the count and write out of bounds;
// - sub_1426A16C0 (uGeometry2Collider): an allocation appended to a shared
//   MtArray (Reserve and ++ without a lock).
//
// So every callback runs under one recursive lock while the parallel pass
// is on: hitCheckCallback whole (codes 0 and 1), the four callbacks (codes
// 4 and 2), and any job whose callback is none of the four whole (counted:
// enumContact_0 is also reached through a table, so other callbacks may
// exist). The geometry stays parallel. The lock is taken only while
// contacts.cpp has the flag set (set_locking, on the game thread at
// al.frame BEGIN, when no collision runs); otherwise the detours pass
// through. The record writer's detour only counts: calls, and calls that
// found the list full (the overflow path the race needed).
//
// With detail on (research mode): the time spent waiting for and holding the
// lock, and same-object overlaps: two threads in the guarded code for the
// same object at once (one holding, one waiting), the races the lock
// prevents; keyed by a 4096-slot table, so a rare hash collision counts too.
//
// Signatures (tested exe c2ebbbd2..., clean image a78ea69b...), each from
// the function's first byte, matched once, each reaching an instruction of
// its own body (a field offset or the float negation).
enum Site : unsigned { hit_check, job, hit, nearest, list, geometry, records, site_count };
inline constexpr std::string_view signatures[site_count] = {
    // sCollision::NodeList::hitCheckCallback (0x14248A080): through the negation of the contact's +0x20
    "4C 8B DC 49 89 5B 08 49 89 6B 10 49 89 73 18 57 41 56 41 57 48 83 EC 50 48 8B BC 24 98 00 00 00 41 8B F1 48 8B 84 24 A0 00 00 00 "
    "49 8B E8 44 8B B4 24 90 00 00 00 4C 8B FA F3 0F 10 47 20 48 8B 18 0F 57 05 ?? ?? ?? ?? F3 0F 11 47 20 48 8B 4B 10",
    // sCollision::NodeList::executeTargetActiveNodeContact (0x142489810)
    "40 55 53 57 41 55 41 56 48 8D 6C 24 E0 48 81 EC 20 01 00 00 48 8B DA 4C 8B E9 E8 ?? ?? ?? ?? 48 8B 7B 08 44 8B F0 83 7F 20 00",
    // sub_1412D6D20, cpObjCollision's hit callback
    "4C 8B DC 41 57 48 81 EC 80 00 00 00 48 8B 81 A8 01 00 00 44 8B D2 4C 8B F9 48 85 C0 0F 84 02 05 00 00 F6 40 0C 0E 0F 84 F8 04 00 00",
    // sub_142396960, the nearest-contact callback
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 50 0F B6 84 24 A8 00 00 00 48 8B D9 48 8B 0D ?? ?? ?? ?? 49 8B E9",
    // sub_1425109C0, the contact list callback
    "40 53 48 83 EC 20 80 B9 C9 01 00 00 00 48 8B D9 75 3B 85 D2 75 37 49 8B 01 49 8B C9",
    // sub_1426A16C0, uGeometry2Collider's contact callback
    "48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 40 49 8B F1 49 8B E8 48 8B F9 85 D2 74 42 83 FA 03 0F 85 4B 01 00 00 48 8B 89 38 01 00 00",
    // sub_1412D5370, cpObjCollision's hit record writer
    "4C 8B DC 4D 89 4B 20 4D 89 43 18 49 89 53 10 56 57 41 54 41 55 48 83 EC 78 48 8B 81 A8 01 00 00 49 8B F9 4D 8B E8 4C 8B E2 48 8B F1 "
    "48 85 C0 0F 84 E3 03 00 00",
};
inline constexpr const char* site_names[site_count] = {
    "sCollision hitCheckCallback (cpuopt contact guard)",
    "sCollision executeTargetActiveNodeContact (cpuopt contact guard)",
    "cpObjCollision hit callback (cpuopt contact guard)",
    "nearest-contact callback (cpuopt contact guard)",
    "contact list callback (cpuopt contact guard)",
    "uGeometry2Collider contact callback (cpuopt contact guard)",
    "cpObjCollision hit record writer (cpuopt contact guard, counting)",
};
inline constexpr const char* short_names[site_count] = {"hit_check", "job", "hit", "nearest", "list", "geometry", "records"};
// ActiveNodeInfo: the context at +0x10, the callback at +0x18. A node's own
// context at +0x80 (also the hit object's cpObjCollision for the hit
// callback). The record writer's count at +0x4FF0, full at 128.
inline constexpr std::uint32_t info_context = 0x10, info_callback = 0x18, node_owner = 0x80;
inline constexpr std::uint32_t record_count = 0x4FF0, record_capacity = 128;

struct Discovery {
    bool found{};
    const char* reason{"not searched"};
    unsigned counts[site_count]{};
    std::uintptr_t sites[site_count]{};
};
Discovery discover(const std::uintptr_t (&sites)[site_count], const unsigned (&counts)[site_count]) noexcept;
std::string discovery_event(const Discovery&, std::uintptr_t base, double t_ms, std::string_view scanner);
// The four known callbacks, for the job detour's check.
void set_target(const Discovery&) noexcept;

// The detours, with their targets' signatures, and where al.hooks stores
// each one's continuation.
void* detour(Site s) noexcept;
void** next(Site s) noexcept;

// al_entry: true once every hook is registered and active.
void set_ready_probe(bool (*probe)() noexcept) noexcept;
bool ready() noexcept;
// contacts.cpp, game thread at BEGIN: on before it sets the flag, off after
// it cleared it.
void set_locking(bool on) noexcept;
bool locking() noexcept;
void set_detail(bool on) noexcept;  // research mode: timing and overlaps
// "contact_guard": {...} into an open JSON object; resets the window's counts.
void append(std::string& out);
}  // namespace cpuopt::contact_guard
