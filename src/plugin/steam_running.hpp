#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace cpuopt::steam_running {
// Switch cache_steam_running (toggles.hpp): SteamAPI_IsSteamRunning answered
// from one call per time_ms at its two per-frame call sites.
//
// SteamAPI_IsSteamRunning opens Steam's registry key (HKCU ... ActiveProcess)
// and reads it on every call; under MO2 the key's close also passes through
// usvfs's NtClose hook. The game calls it several times a frame from
// sub_142209BF0 (under the session poll sub_1422ADAE0) and sub_142215C00; in
// the Ancient Forest (al-11, v0.6's switches on) its registry calls were
// about 1.5% of the game thread. Its answer changes only when Steam quits.
//
// The change, at both sites: the 6-byte `call [rip+import]` becomes a 1-byte
// NOP and a call rel32 to a jump stub (made at PRE_MAIN, within 2 GB of the
// exe) to `is_running`, which calls the real function at most once per
// time_ms and otherwise returns its last answer. The new call returns where
// the old one did. The two other call sites (sub_1421E0E10, sub_1422153C0)
// are left alone.
//
// Signatures (tested exe c2ebbbd2..., clean image a78ea69b...), each matched
// once; both calls must go through the same import slot, which must hold
// steam_api64's SteamAPI_IsSteamRunning.
inline constexpr std::string_view poll_signature =
    "85 C0 74 06 FF 15 ?? ?? ?? ?? FF 15 ?? ?? ?? ?? 48 8B 5C 24 30 84 C0 0F 84 87 00 00 00 48 8D 0D ?? ?? ?? ?? FF 15";
inline constexpr std::string_view lobby_signature =
    "48 8B D9 E8 ?? ?? ?? ?? FF 15 ?? ?? ?? ?? 84 C0 0F 84 A1 01 00 00 48 8D 0D ?? ?? ?? ?? FF 15";
inline constexpr unsigned poll_call = 10, lobby_call = 8, call_length = 6;

struct Discovery {
    bool found{};
    const char* reason{"not searched"};
    unsigned counts[2]{};          // poll, lobby
    std::uintptr_t sites[2]{};     // the two calls
    std::uintptr_t real{};         // SteamAPI_IsSteamRunning
    std::uintptr_t stub{};         // made at PRE_MAIN by the plugin entry
};
// `steam_function`: the address the import slot must hold.
Discovery discover(const std::uintptr_t* poll, unsigned poll_count, const std::uintptr_t* lobby, unsigned lobby_count,
                   std::uintptr_t steam_function) noexcept;
std::string discovery_event(const Discovery&, std::uintptr_t image_base, double t_ms, std::string_view scanner);
// NOP, then call rel32 to `stub`, returning to `site` + 6; false out of reach.
bool build_replacement(std::uintptr_t site, std::uintptr_t stub, std::array<std::uint8_t, call_length>& out) noexcept;

// The cached call; `real` from the discovery.
bool __fastcall is_running();
void set_real(std::uintptr_t real) noexcept;
// On: reuse an answer for `reuse_ms`; off: every call is Steam's. Any thread.
void set_active(bool on, unsigned reuse_ms) noexcept;
// "steam_running": {active, calls, reused} since the last call.
void append(std::string& out);
}  // namespace cpuopt::steam_running
