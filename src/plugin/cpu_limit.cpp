#include "cpu_limit.hpp"
#include "../core/json.hpp"
#include <windows.h>
#include <atomic>
#include <cstdio>

namespace cpuopt::cpu_limit {
namespace {
SRWLOCK lock = SRWLOCK_INIT;
DWORD_PTR original{};  // the process's affinity before the first limit; 0 = never limited
std::atomic<unsigned> limit{};
std::atomic<std::uint64_t> applied_mask{};
std::atomic<bool> applied{};

// The lowest `count` bits set in `mask`.
DWORD_PTR first_bits(DWORD_PTR mask, unsigned count) noexcept {
    DWORD_PTR out = 0;
    for (unsigned bit = 0; bit < sizeof(DWORD_PTR) * 8 && count; ++bit)
        if (mask & (DWORD_PTR(1) << bit)) {
            out |= DWORD_PTR(1) << bit;
            --count;
        }
    return out;
}
}  // namespace

void set(unsigned cpu_count) noexcept {
    AcquireSRWLockExclusive(&lock);
    DWORD_PTR process{}, system{};
    if (GetProcessAffinityMask(GetCurrentProcess(), &process, &system)) {
        if (!original) original = process;
        const auto target = cpu_count ? first_bits(original, cpu_count) : original;
        if (target && target != process && SetProcessAffinityMask(GetCurrentProcess(), target)) process = target;
        applied_mask.store(process);
        applied.store(cpu_count && process != original);
    }
    limit.store(cpu_count);
    ReleaseSRWLockExclusive(&lock);
}

void restore() noexcept {
    AcquireSRWLockExclusive(&lock);
    if (original) SetProcessAffinityMask(GetCurrentProcess(), original);
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
