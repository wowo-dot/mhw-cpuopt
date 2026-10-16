#include "cpu_limit.hpp"
#include "../core/json.hpp"
#include <atomic>
#include <cstdio>
#include <memory>

namespace cpuopt::cpu_limit {
namespace {
SRWLOCK lock = SRWLOCK_INIT;
DWORD_PTR own{};     // the process's affinity when first read; 0 = not read yet
DWORD_PTR ours{};    // the affinity this module set, while it stands; 0 = none
DWORD_PTR before{};  // what was in effect right before that change
std::atomic<unsigned> limit{};
std::atomic<std::uint64_t> applied_mask{};
std::atomic<bool> applied{};

unsigned bits(DWORD_PTR mask) noexcept {
    unsigned n = 0;
    for (; mask; mask &= mask - 1) ++n;
    return n;
}
DWORD_PTR own_locked() noexcept {
    if (!own) {
        DWORD_PTR process{}, system{};
        if (GetProcessAffinityMask(GetCurrentProcess(), &process, &system)) own = process;
    }
    return own;
}
}  // namespace

DWORD_PTR first_bits(DWORD_PTR mask, unsigned count) noexcept {
    DWORD_PTR out = 0;
    for (unsigned bit = 0; bit < sizeof(DWORD_PTR) * 8 && count; ++bit)
        if (mask & (DWORD_PTR(1) << bit)) {
            out |= DWORD_PTR(1) << bit;
            --count;
        }
    return out;
}

DWORD_PTR original() noexcept {
    AcquireSRWLockExclusive(&lock);
    const auto m = own_locked();
    ReleaseSRWLockExclusive(&lock);
    return m;
}

DWORD_PTR outside() noexcept {
    AcquireSRWLockExclusive(&lock);
    DWORD_PTR process{}, system{};
    GetProcessAffinityMask(GetCurrentProcess(), &process, &system);
    const auto m = ours && process == ours ? before : process;
    ReleaseSRWLockExclusive(&lock);
    return m;
}

Ccd find_cache_ccd() noexcept {
    Ccd c;
    DWORD bytes = 0;
    GetLogicalProcessorInformationEx(RelationCache, nullptr, &bytes);
    if (!bytes) {
        c.reason = "no cache information";
        return c;
    }
    auto buffer = std::make_unique<std::uint8_t[]>(bytes);
    auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.get());
    if (!GetLogicalProcessorInformationEx(RelationCache, info, &bytes)) {
        c.reason = "no cache information";
        return c;
    }
    DWORD_PTR largest_mask = 0;
    for (DWORD at = 0; at < bytes;) {
        const auto* e = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.get() + at);
        if (e->Relationship == RelationCache && e->Cache.Level == 3 && e->Cache.GroupMask.Group == 0) {
            ++c.caches;
            const auto kb = static_cast<std::uint32_t>(e->Cache.CacheSize / 1024);
            if (kb > c.largest_kb) {
                c.next_kb = c.largest_kb;
                c.largest_kb = kb;
                largest_mask = e->Cache.GroupMask.Mask;
            } else if (kb > c.next_kb) {
                c.next_kb = kb;
            }
        }
        if (!e->Size) break;
        at += e->Size;
    }
    if (c.caches < 2) {
        c.reason = "one L3 cache";
        return c;
    }
    if (c.largest_kb <= c.next_kb) {
        c.reason = "L3 caches of equal size";
        return c;
    }
    c.mask = largest_mask & original();
    c.count = bits(c.mask);
    if (c.count < 2) {
        c.reason = "the largest L3's processors are outside the process's affinity";
        return c;
    }
    c.found = true;
    c.reason = "ok";
    return c;
}

std::string ccd_event(const Ccd& c, double t_ms) {
    std::string out = "{";
    json::key(out, "event");
    json::string(out, "cache_ccd");
    json::key(out, "t_ms");
    json::fixed(out, t_ms, 3);
    json::key(out, "found");
    out += c.found ? "true" : "false";
    json::key(out, "reason");
    json::string(out, c.reason);
    json::key(out, "caches");
    json::number(out, std::uint64_t(c.caches));
    json::key(out, "largest_kb");
    json::number(out, std::uint64_t(c.largest_kb));
    json::key(out, "next_kb");
    json::number(out, std::uint64_t(c.next_kb));
    json::key(out, "count");
    json::number(out, std::uint64_t(c.count));
    char hex[24];
    std::snprintf(hex, sizeof(hex), "0x%llx", static_cast<unsigned long long>(c.mask));
    json::key(out, "mask");
    json::string(out, hex);
    out += '}';
    return out;
}

void set_mask(DWORD_PTR mask, unsigned cpu_limit) noexcept {
    AcquireSRWLockExclusive(&lock);
    const auto base = own_locked();
    DWORD_PTR process{}, system{};
    if (base && GetProcessAffinityMask(GetCurrentProcess(), &process, &system)) {
        if (ours && process != ours) ours = before = 0;  // changed by someone else since: theirs now
        if (mask) {
            const auto from = ours ? before : process;  // ours replaced, theirs kept as the bound
            const auto target = mask & from;
            if (target && target != process && SetProcessAffinityMask(GetCurrentProcess(), target)) {
                if (!ours) before = process;
                ours = process = target;
            }
        } else if (ours) {
            if (SetProcessAffinityMask(GetCurrentProcess(), before)) process = before;
            ours = before = 0;
        }
        applied_mask.store(process);
        applied.store(mask && (process & ~mask) == 0);  // on the wanted processors, by us or by a rule
    }
    limit.store(cpu_limit);
    ReleaseSRWLockExclusive(&lock);
}

void restore() noexcept {
    AcquireSRWLockExclusive(&lock);
    DWORD_PTR process{}, system{};
    if (ours && GetProcessAffinityMask(GetCurrentProcess(), &process, &system) && process == ours)
        SetProcessAffinityMask(GetCurrentProcess(), before);
    ours = before = 0;
    applied.store(false);
    limit.store(0);
    ReleaseSRWLockExclusive(&lock);
}

void append(std::string& out) {
    json::key(out, "cpus");
    out += '{';
    json::key(out, "limit");
    json::number(out, std::uint64_t(limit.load()));
    json::key(out, "applied");
    out += applied.load() ? "true" : "false";
    json::key(out, "mask");
    char hex[24];
    std::snprintf(hex, sizeof(hex), "0x%llx", static_cast<unsigned long long>(applied_mask.load()));
    json::string(out, hex);
    out += '}';
}
}  // namespace cpuopt::cpu_limit
