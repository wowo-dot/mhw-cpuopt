#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace cpuopt::server_time {
// Switch server_time (toggles.hpp): Steam's server time, read once and
// reused for control.ini's time_ms.
//
// Two game functions call ISteamUtils::GetServerRealTime (the interface from
// SteamInternal_ContextInit, vtable slot 3), and between them they run several
// times a frame from the per-frame checks sub_141976900/A60/D20/DE0:
// - query, sub_142215230 (a session object's vtable slot 0xC0): if the
//   interface exists and the session's state (slot 0x60, a flag read) is 2,
//   *out = the server time and it returns 1; otherwise *out = local time and
//   it returns 0;
// - date, sub_141B008E0: fills a 40-byte date (year, month, day, hour,
//   minute, second at +0..+0x14, the time at +0x18, a flag byte at +0x20 from
//   [[[global]+0x260]+0x10]+0xBFF0 bit 0) from the server time through
//   _gmtime64, and returns the date.
// Inside steamclient each call takes a lock (CThreadFullMutex::TryLock); in
// the al-05 profile those calls were 4.3% of the game thread (query 2.7%,
// date 1.6%).
//
// The switch: al.hooks detours on both. The first call after time_ms has
// passed goes to the game's own code and its server time is kept; until then
// the kept time is used, advanced by the whole seconds since it was read.
// query still asks the session's state and calls the game's code for any
// state but 2; date writes exactly what the game's code would. The server
// time is a whole-second clock, so a reused value can lag by at most a
// second. Nothing in Steam's own code is touched.
//
// Signatures (tested exe c2ebbbd2..., clean image a78ea69b...), each matched
// once from the function's entry; both must load the same Steam context and
// call the same import.
inline constexpr std::string_view query_signature =
    "48 89 5C 24 08 57 48 83 EC 20 48 8B DA 48 8B F9 48 85 D2 75 0D 32 C0 48 8B 5C 24 30 48 83 C4 20 5F C3 48 8D 0D ?? ?? ?? ?? "
    "FF 15 ?? ?? ?? ?? 48 83 38 00 74 3B 48 8B 07 48 8B CF FF 50 60 83 F8 02 75 2D 48 8D 0D ?? ?? ?? ?? 40 B7 01 FF 15 ?? ?? ?? ?? "
    "48 8B 08 48 8B 01 FF 50 18 8B C0 48 89 03";
inline constexpr std::string_view date_signature =
    "48 89 5C 24 08 57 48 83 EC 20 33 C0 48 8D 0D ?? ?? ?? ?? 48 89 02 48 8B DA 48 89 42 08 48 89 42 10 48 89 42 18 88 42 20 "
    "FF 15 ?? ?? ?? ?? 48 8B 08 48 8B 01 FF 50 18 48 8B 0D ?? ?? ?? ?? 8B C0 48 8B 89 60 02 00 00 48 85 C9 74 15 48 8B 49 10 "
    "48 85 C9 74 0C 0F B6 89 F0 BF 00 00 80 E1 01 EB 02 32 C9 88 4B 20 48 8D 4B 18 48 89 43 18 E8 ?? ?? ?? ?? 8B 48 14 81 C1 "
    "6C 07 00 00 89 0B";
// RIP-relative operands in the matches: lea rcx,[context] (7 bytes) and
// call [import] (6 bytes); date also loads the flag's global (7 bytes).
inline constexpr unsigned query_context[2] = {0x22, 0x43}, query_import[2] = {0x29, 0x4D};
inline constexpr unsigned date_context = 0x0C, date_import = 0x28, date_global = 0x37;
inline constexpr unsigned state_slot = 0x60;  // query's session: state, 2 = the Steam path
inline constexpr std::uint32_t flag_offsets[3] = {0x260, 0x10, 0xBFF0};

struct Discovery {
    bool found{};
    const char* reason{"not searched"};
    unsigned counts[2]{};  // query, date
    std::uint32_t query_rva{}, date_rva{}, global_rva{};
};
Discovery discover(std::uintptr_t image_base, const std::uintptr_t* query, unsigned query_count, const std::uintptr_t* date,
                   unsigned date_count) noexcept;
std::string discovery_event(const Discovery&, double t_ms, std::string_view scanner);
// The flag's global (date), from the discovery.
void set_target(std::uintptr_t image_base, const Discovery&) noexcept;

using Query = char(__fastcall*)(void* session, std::uint64_t* out);
using Date = std::uintptr_t(__fastcall*)(std::uintptr_t self, std::uintptr_t out);
char __fastcall query_detour(void* session, std::uint64_t* out);
std::uintptr_t __fastcall date_detour(std::uintptr_t self, std::uintptr_t out);
// Where al.hooks writes the functions to continue with.
Query* query_next() noexcept;
Date* date_next() noexcept;

// On: reuse a read for `reuse_ms`. Off: every call goes to the game's code
// and the kept time is dropped. Any thread.
void set_active(bool on, unsigned reuse_ms) noexcept;
// "server_time": {active, time_ms, reads, reused, other} since the last call,
// into an open JSON object. reads: calls the game's code answered while on;
// other: query calls in another session state.
void append(std::string& out);
}  // namespace cpuopt::server_time
