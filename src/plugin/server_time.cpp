#include "server_time.hpp"
#include "../core/json.hpp"
#include "../core/signature.hpp"
#include "log.hpp"
#include <atomic>
#include <ctime>

namespace cpuopt::server_time {
namespace {
Query query_fn{};
Date date_fn{};
std::atomic<std::uintptr_t> flag_global{};
std::atomic<bool> active{};
std::atomic<std::uint32_t> reuse_for_ms{};
// The kept read: server seconds << 32 | the low 32 bits of the read time in
// ms since start; 0 = none.
std::atomic<std::uint64_t> kept{};
std::atomic<std::uint64_t> reads{}, reused{}, other{};

std::uint32_t ms32() noexcept {
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(qpc_now()) * 1000ULL / static_cast<std::uint64_t>(qpc_frequency()));
}
bool fresh(std::uint32_t& value) noexcept {
    const auto k = kept.load(std::memory_order_relaxed);
    if (!k) return false;
    const auto age = ms32() - static_cast<std::uint32_t>(k);
    if (age >= reuse_for_ms.load(std::memory_order_relaxed)) return false;
    value = static_cast<std::uint32_t>(k >> 32) + age / 1000;
    return true;
}
void keep(std::uint32_t value) noexcept { kept.store(std::uint64_t(value) << 32 | ms32(), std::memory_order_relaxed); }

std::int64_t operand(std::uintptr_t base, std::uintptr_t match, unsigned at, unsigned length, unsigned displacement_at) noexcept {
    const auto rva = static_cast<std::uint32_t>(match + at - base);
    return rip_target(rva, length, read_i32(reinterpret_cast<const std::uint8_t*>(match + at + displacement_at)));
}
// date's flag: byte [[[global]+0x260]+0x10]+0xBFF0, bit 0; 0 where a pointer is null.
bool read_flag(std::uint8_t& flag) noexcept {
    const auto global = flag_global.load(std::memory_order_relaxed);
    if (!global) return false;
    __try {
        auto p = *reinterpret_cast<volatile const std::uintptr_t*>(global);
        if (!p) return false;  // the game's code would fault here: let it run instead
        flag = 0;
        p = *reinterpret_cast<volatile const std::uintptr_t*>(p + flag_offsets[0]);
        if (p) p = *reinterpret_cast<volatile const std::uintptr_t*>(p + flag_offsets[1]);
        if (p) flag = *reinterpret_cast<volatile const std::uint8_t*>(p + flag_offsets[2]) & 1;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
}  // namespace

Discovery discover(std::uintptr_t base, const std::uintptr_t* query, unsigned query_count, const std::uintptr_t* date,
                   unsigned date_count) noexcept {
    Discovery d;
    d.counts[0] = query_count;
    d.counts[1] = date_count;
    if (!query_count || !date_count) {
        d.reason = "signature not found";
        return d;
    }
    if (query_count != 1 || date_count != 1) {
        d.reason = "signature not unique";
        return d;
    }
    const auto q = query[0], t = date[0];
    const auto context = operand(base, t, date_context, 7, 3), import = operand(base, t, date_import, 6, 2);
    for (unsigned i = 0; i < 2; ++i)
        if (operand(base, q, query_context[i], 7, 3) != context || operand(base, q, query_import[i], 6, 2) != import) {
            d.reason = "different Steam calls";
            return d;
        }
    const auto global = operand(base, t, date_global, 7, 3);
    if (global <= 0 || context <= 0 || import <= 0) {
        d.reason = "operand changed";
        return d;
    }
    d.found = true;
    d.reason = "ok";
    d.query_rva = static_cast<std::uint32_t>(q - base);
    d.date_rva = static_cast<std::uint32_t>(t - base);
    d.global_rva = static_cast<std::uint32_t>(global);
    return d;
}

std::string discovery_event(const Discovery& d, double t_ms, std::string_view scanner) {
    std::string out = "{";
    json::key(out, "event");
    json::string(out, "server_time");
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
    json::key(out, "query_rva");
    json::number(out, std::uint64_t(d.query_rva));
    json::key(out, "date_rva");
    json::number(out, std::uint64_t(d.date_rva));
    json::key(out, "global_rva");
    json::number(out, std::uint64_t(d.global_rva));
    out += '}';
    return out;
}

void set_target(std::uintptr_t base, const Discovery& d) noexcept {
    if (d.found) flag_global.store(base + d.global_rva);
}

Query* query_next() noexcept { return &query_fn; }
Date* date_next() noexcept { return &date_fn; }

char __fastcall query_detour(void* session, std::uint64_t* out) {
    const auto next = query_fn;
    std::uint32_t value{};
    const bool on = active.load(std::memory_order_relaxed);
    if (!out || !on || !fresh(value)) {
        const char r = next(session, out);
        if (on && out && r == 1) {  // Steam's path: *out is the server time
            keep(static_cast<std::uint32_t>(*out));
            reads.fetch_add(1, std::memory_order_relaxed);
        }
        return r;
    }
    // The game's code takes Steam's path only in session state 2.
    using State = std::uint32_t(__fastcall*)(void*);
    const auto state = (*reinterpret_cast<State* const*>(session))[state_slot / sizeof(void*)](session);
    if (state != 2) {
        other.fetch_add(1, std::memory_order_relaxed);
        return next(session, out);
    }
    *out = value;
    reused.fetch_add(1, std::memory_order_relaxed);
    return 1;
}

std::uintptr_t __fastcall date_detour(std::uintptr_t self, std::uintptr_t out) {
    const auto next = date_fn;
    std::uint32_t value{};
    std::uint8_t flag{};
    const bool on = active.load(std::memory_order_relaxed);
    if (!on || !fresh(value) || !read_flag(flag)) {
        const auto r = next(self, out);
        if (on) {
            keep(*reinterpret_cast<const std::uint32_t*>(out + 0x18));
            reads.fetch_add(1, std::memory_order_relaxed);
        }
        return r;
    }
    std::tm parts{};
    const __time64_t time = value;
    if (_gmtime64_s(&parts, &time) != 0) return next(self, out);
    auto* d = reinterpret_cast<std::uint32_t*>(out);
    d[0] = static_cast<std::uint32_t>(parts.tm_year + 1900);
    d[1] = static_cast<std::uint32_t>(parts.tm_mon + 1);
    d[2] = static_cast<std::uint32_t>(parts.tm_mday);
    d[3] = static_cast<std::uint32_t>(parts.tm_hour);
    d[4] = static_cast<std::uint32_t>(parts.tm_min);
    d[5] = static_cast<std::uint32_t>(parts.tm_sec);
    *reinterpret_cast<std::uint64_t*>(out + 0x18) = value;
    *reinterpret_cast<std::uint8_t*>(out + 0x20) = flag;
    reused.fetch_add(1, std::memory_order_relaxed);
    return out;
}

void set_active(bool on, unsigned reuse_ms) noexcept {
    reuse_for_ms.store(reuse_ms);
    if (!on) kept.store(0);
    active.store(on);
}

void append(std::string& out) {
    json::key(out, "server_time");
    out += '{';
    json::key(out, "active");
    out += active.load(std::memory_order_relaxed) ? "true" : "false";
    json::key(out, "time_ms");
    json::number(out, std::uint64_t(reuse_for_ms.load(std::memory_order_relaxed)));
    json::key(out, "reads");
    json::number(out, reads.exchange(0, std::memory_order_relaxed));
    json::key(out, "reused");
    json::number(out, reused.exchange(0, std::memory_order_relaxed));
    json::key(out, "other");
    json::number(out, other.exchange(0, std::memory_order_relaxed));
    out += '}';
}
}  // namespace cpuopt::server_time
