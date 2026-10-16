#include "steam_running.hpp"
#include "../core/json.hpp"
#include "../core/signature.hpp"
#include "log.hpp"
#include <windows.h>
#include <atomic>
#include <cstring>

namespace cpuopt::steam_running {
namespace {
using Real = bool (*)();
std::atomic<Real> real_fn{};
std::atomic<bool> active{};
std::atomic<std::uint32_t> reuse_for_ms{};
// The kept answer: 1 << 33 (kept) | 1 << 32 if running | the low 32 bits of
// the time it was asked, in ms; 0 = none.
std::atomic<std::uint64_t> kept{};
std::atomic<std::uint64_t> calls{}, reused{};

std::uint32_t ms32() noexcept {
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(qpc_now()) * 1000ULL / static_cast<std::uint64_t>(qpc_frequency()));
}
std::uintptr_t slot_of(std::uintptr_t call) noexcept {
    return static_cast<std::uintptr_t>(static_cast<std::int64_t>(call + call_length) +
                                       read_i32(reinterpret_cast<const std::uint8_t*>(call + 2)));
}
}  // namespace

Discovery discover(const std::uintptr_t* poll, unsigned poll_count, const std::uintptr_t* lobby, unsigned lobby_count) noexcept {
    Discovery d;
    d.counts[0] = poll_count;
    d.counts[1] = lobby_count;
    if (!poll_count || !lobby_count) {
        d.reason = "signature not found";
        return d;
    }
    if (poll_count != 1 || lobby_count != 1) {
        d.reason = "signature not unique";
        return d;
    }
    d.sites[0] = poll[0] + poll_call;
    d.sites[1] = lobby[0] + lobby_call;
    const auto slot = slot_of(d.sites[0]);
    if (slot != slot_of(d.sites[1])) {
        d.reason = "different imports";
        return d;
    }
    d.found = true;
    d.reason = "ok";
    d.slot = slot;
    return d;
}

bool resolve(Discovery& d, std::uintptr_t steam_function, const char** reason) noexcept {
    std::uintptr_t value{};
    __try {
        value = *reinterpret_cast<volatile const std::uintptr_t*>(d.slot);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        value = 0;
    }
    if (!d.found || !steam_function || value != steam_function) {
        *reason = !steam_function ? "steam_api64 not loaded" : "import is not SteamAPI_IsSteamRunning";
        return false;
    }
    d.real = value;
    set_real(value);
    return true;
}

std::string discovery_event(const Discovery& d, std::uintptr_t base, double t_ms, std::string_view scanner) {
    std::string out = "{";
    json::key(out, "event");
    json::string(out, "steam_running");
    json::key(out, "t_ms");
    json::fixed(out, t_ms, 3);
    json::key(out, "found");
    out += d.found ? "true" : "false";
    json::key(out, "reason");
    json::string(out, d.reason);
    json::key(out, "scanner");
    json::string(out, scanner);
    json::key(out, "counts");
    out += '[';
    json::number(out, std::uint64_t(d.counts[0]));
    out += ',';
    json::number(out, std::uint64_t(d.counts[1]));
    out += ']';
    json::key(out, "site_rvas");
    out += '[';
    json::number(out, std::uint64_t(d.sites[0] ? d.sites[0] - base : 0));
    out += ',';
    json::number(out, std::uint64_t(d.sites[1] ? d.sites[1] - base : 0));
    out += ']';
    json::key(out, "stub");
    out += d.stub ? "true" : "false";
    out += '}';
    return out;
}

bool build_replacement(std::uintptr_t site, std::uintptr_t stub, std::array<std::uint8_t, call_length>& out) noexcept {
    const auto delta = static_cast<std::int64_t>(stub) - static_cast<std::int64_t>(site + call_length);
    if (!stub || delta < INT32_MIN || delta > INT32_MAX) return false;
    const auto rel = static_cast<std::int32_t>(delta);
    out[0] = 0x90;  // nop
    out[1] = 0xE8;  // call rel32, ending where the old call ended
    std::memcpy(out.data() + 2, &rel, sizeof(rel));
    return true;
}

bool __fastcall is_running() {
    const auto real = real_fn.load(std::memory_order_relaxed);
    if (!real) return false;  // never sent here without a real function
    if (active.load(std::memory_order_relaxed)) {
        const auto k = kept.load(std::memory_order_relaxed);
        if (k && ms32() - static_cast<std::uint32_t>(k) < reuse_for_ms.load(std::memory_order_relaxed)) {
            reused.fetch_add(1, std::memory_order_relaxed);
            return ((k >> 32) & 1) != 0;  // bit 33 only marks a kept answer
        }
    }
    const bool running = real();
    calls.fetch_add(1, std::memory_order_relaxed);
    if (active.load(std::memory_order_relaxed)) kept.store((running ? 1ULL << 32 : 0ULL) | ms32() | (1ULL << 33), std::memory_order_relaxed);
    return running;
}

void set_real(std::uintptr_t real) noexcept { real_fn.store(reinterpret_cast<Real>(real)); }

void set_active(bool on, unsigned reuse_ms) noexcept {
    reuse_for_ms.store(reuse_ms);
    if (!on) kept.store(0);
    active.store(on);
}

void append(std::string& out) {
    json::key(out, "steam_running");
    out += '{';
    json::key(out, "active");
    out += active.load(std::memory_order_relaxed) ? "true" : "false";
    json::key(out, "calls");
    json::number(out, calls.exchange(0, std::memory_order_relaxed));
    json::key(out, "reused");
    json::number(out, reused.exchange(0, std::memory_order_relaxed));
    out += '}';
}
}  // namespace cpuopt::steam_running
