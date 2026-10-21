#include "multi_hit.hpp"
#include "../core/json.hpp"
#include <windows.h>
#include <atomic>
#include <cstring>
#include <unordered_map>

namespace cpuopt::multi_hit {
namespace {
using Move = std::uintptr_t (*)(std::uintptr_t);  // whatever it returns is passed on
void* next_fn{};
std::atomic<std::uintptr_t> delta_global{};
std::atomic<bool> carry_on{}, counting_on{};
std::atomic<std::uint64_t> frame{};

// One cache slot as read before the game's own aging.
struct Slot {
    std::uint32_t uid{};
    float timer{}, age{};
    bool use{};
};
// An entry the game cleared by timer this frame.
struct Carry {
    std::uint32_t uid{};
    float overshoot{}, age{};  // 60 fps frames past zero; the entry's age at expiry
};
struct Pending {
    std::uint64_t frame{};
    unsigned count{};
    Carry carries[cache_count];
};
// Carries by hit object, kept from one move of the object to its next. The
// objects move on the job threads (unit lines), each once a frame: stripes
// keep them apart.
struct Stripe {
    SRWLOCK lock = SRWLOCK_INIT;
    std::unordered_map<std::uintptr_t, Pending> map;
};
constexpr unsigned stripe_count = 64;
Stripe stripes[stripe_count];
Stripe& stripe_of(std::uintptr_t object) noexcept { return stripes[(object >> 6) * 0x9E3779B97F4A7C15ULL >> 58]; }

struct Counter {  // one per 64 bytes
    std::atomic<std::uint64_t> v{};
    char pad[56]{};
};
Counter calls, fresh, expired, continued, carried, ratio_milli;

bool read_slots(std::uintptr_t object, Slot (&slots)[cache_count], float& step) noexcept {
    __try {
        for (unsigned i = 0; i < cache_count; ++i) {
            const auto at = object + cache_offset + i * cache_stride;
            slots[i].uid = *reinterpret_cast<volatile const std::uint32_t*>(at + uid_offset);
            slots[i].timer = *reinterpret_cast<volatile const float*>(at + timer_offset);
            slots[i].age = *reinterpret_cast<volatile const float*>(at + age_offset);
            slots[i].use = *reinterpret_cast<volatile const std::uint8_t*>(at + use_timer_offset) == 1;
        }
        const auto holder = *reinterpret_cast<volatile const std::uintptr_t*>(delta_global.load(std::memory_order_relaxed));
        const auto factor = *reinterpret_cast<volatile const float*>(object + factor_offset);
        step = holder ? factor * *reinterpret_cast<volatile const float*>(holder + delta_offset) : 0.0f;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool read_uid(std::uintptr_t object, unsigned i, std::uint32_t& uid) noexcept {
    __try {
        uid = *reinterpret_cast<volatile const std::uint32_t*>(object + cache_offset + i * cache_stride + uid_offset);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool write_timer(std::uintptr_t object, unsigned i, float value) noexcept {
    __try {
        *reinterpret_cast<volatile float*>(object + cache_offset + i * cache_stride + timer_offset) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The carries the object's previous move left, if that was the last frame.
bool take(std::uintptr_t object, std::uint64_t now, Pending& out) noexcept {
    auto& s = stripe_of(object);
    bool got = false;
    AcquireSRWLockExclusive(&s.lock);
    if (const auto it = s.map.find(object); it != s.map.end()) {
        if (it->second.frame + 1 == now) {
            out = it->second;
            got = true;
        }
        s.map.erase(it);
    }
    ReleaseSRWLockExclusive(&s.lock);
    return got;
}
void keep(std::uintptr_t object, const Pending& p) noexcept {
    auto& s = stripe_of(object);
    AcquireSRWLockExclusive(&s.lock);
    try {
        if (s.map.size() > 256)  // objects that stopped moving (destroyed): drop old ones
            for (auto it = s.map.begin(); it != s.map.end();) it = it->second.frame + 2 < p.frame ? s.map.erase(it) : std::next(it);
        s.map[object] = p;
    } catch (...) {
    }
    ReleaseSRWLockExclusive(&s.lock);
}

std::uintptr_t move_detour(std::uintptr_t object) {
    const auto original = reinterpret_cast<Move>(next_fn);
    const bool carry = carry_on.load(std::memory_order_relaxed);
    if ((!carry && !counting_on.load(std::memory_order_relaxed)) || !object) return original(object);
    Slot slots[cache_count];
    float step{};
    if (!read_slots(object, slots, step)) return original(object);
    calls.v.fetch_add(1, std::memory_order_relaxed);
    const auto now = frame.load(std::memory_order_relaxed);
    // Fresh entries (made since the last move): an attack hitting again right
    // after its entry expired gets the overshoot back.
    Pending last;
    const bool have = take(object, now, last);
    for (unsigned i = 0; i < cache_count; ++i) {
        const auto& s = slots[i];
        if (!s.uid || !s.use || s.age != 0.0f) continue;
        fresh.v.fetch_add(1, std::memory_order_relaxed);
        if (!have) continue;
        for (unsigned c = 0; c < last.count; ++c) {
            auto& k = last.carries[c];
            if (!k.uid || k.uid != s.uid) continue;
            continued.v.fetch_add(1, std::memory_order_relaxed);
            if (s.timer > 0.0f) ratio_milli.v.fetch_add(static_cast<std::uint64_t>(k.age / s.timer * 1000.0f + 0.5f), std::memory_order_relaxed);
            if (carry && k.overshoot > 0.0f && write_timer(object, i, s.timer - k.overshoot)) {
                slots[i].timer -= k.overshoot;
                carried.v.fetch_add(1, std::memory_order_relaxed);
            }
            k.uid = 0;  // one carry, one entry
            break;
        }
    }
    const auto result = original(object);
    // Entries the game cleared by timer: the time past zero.
    Pending p;
    p.frame = now;
    for (unsigned i = 0; i < cache_count && step > 0.0f; ++i) {
        const auto& s = slots[i];
        if (!s.uid || !s.use || s.timer <= 0.0f || s.timer - step > 0.0f) continue;
        std::uint32_t uid_now{};
        if (!read_uid(object, i, uid_now) || uid_now != 0) continue;  // not cleared (the attacker's timer did not run)
        expired.v.fetch_add(1, std::memory_order_relaxed);
        p.carries[p.count++] = {s.uid, step - s.timer, s.age + step};
    }
    if (p.count) keep(object, p);
    return result;
}

std::uint64_t take_count(Counter& c) noexcept { return c.v.exchange(0, std::memory_order_relaxed); }
}  // namespace

Discovery discover(std::uintptr_t site, unsigned count) noexcept {
    Discovery d;
    d.count = count;
    if (count != 1) {
        d.reason = count ? "signature not unique" : "signature not found";
        return d;
    }
    std::int32_t rel{};
    std::memcpy(&rel, reinterpret_cast<const void*>(site + global_rel), 4);
    d.site = site;
    d.global = site + global_next + static_cast<std::intptr_t>(rel);
    d.found = true;
    d.reason = "ok";
    return d;
}

std::string discovery_event(const Discovery& d, std::uintptr_t base, double t_ms, std::string_view scanner) {
    std::string out = "{";
    json::key(out, "event");
    json::string(out, "multi_hit");
    json::key(out, "t_ms");
    json::fixed(out, t_ms, 3);
    json::key(out, "found");
    out += d.found ? "true" : "false";
    json::key(out, "reason");
    json::string(out, d.reason);
    json::key(out, "scanner");
    json::string(out, scanner);
    json::key(out, "count");
    json::number(out, std::uint64_t(d.count));
    json::key(out, "move_rva");
    json::number(out, std::uint64_t(d.found && d.site > base ? d.site - base : 0));
    json::key(out, "global_rva");
    json::number(out, std::uint64_t(d.found && d.global > base ? d.global - base : 0));
    out += '}';
    return out;
}

void set_target(const Discovery& d) noexcept { delta_global.store(d.found ? d.global : 0); }
void* detour() noexcept { return reinterpret_cast<void*>(&move_detour); }
void** next() noexcept { return &next_fn; }
void set_carry(bool on) noexcept { carry_on.store(on); }
void set_counting(bool on) noexcept { counting_on.store(on); }
void on_begin() noexcept { frame.fetch_add(1, std::memory_order_relaxed); }

void append(std::string& out) {
    json::key(out, "multi_hit");
    out += '{';
    json::key(out, "carry");
    out += carry_on.load() ? "true" : "false";
    json::key(out, "counting");
    out += counting_on.load() ? "true" : "false";
    json::key(out, "calls");
    json::number(out, take_count(calls));
    json::key(out, "fresh");
    json::number(out, take_count(fresh));
    json::key(out, "expired");
    json::number(out, take_count(expired));
    const auto c = take_count(continued);
    json::key(out, "continued");
    json::number(out, c);
    json::key(out, "interval_ratio");
    json::fixed(out, c ? double(take_count(ratio_milli)) / 1000.0 / double(c) : (take_count(ratio_milli), 0.0), 3);
    json::key(out, "carried");
    json::number(out, take_count(carried));
    out += '}';
}
}  // namespace cpuopt::multi_hit
