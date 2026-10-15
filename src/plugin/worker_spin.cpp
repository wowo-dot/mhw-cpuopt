#include "worker_spin.hpp"
#include "../core/json.hpp"
#include "../core/signature.hpp"
#include "log.hpp"
#include <intrin.h>
#include <atomic>
#include <cstring>

namespace cpuopt::worker_spin {
namespace {
std::atomic<bool> on{};
std::atomic<unsigned> window_us{};
std::atomic<std::uint64_t> window_cycles{}, claim_cycles{}, yield_cycles{};
// Published by the game thread before a round: places left, and a
// generation spinning threads watch. Kept on separate cache lines.
alignas(64) std::atomic<std::int32_t> places{};
alignas(64) std::atomic<std::uint64_t> generation{};
alignas(64) std::atomic<std::uint64_t> rounds{}, claimed{}, blocked{}, gave_up{};
const std::uint64_t calibration_tsc = __rdtsc();
const std::int64_t calibration_qpc = qpc_now();

double tsc_per_us() noexcept {
    const auto seconds = double(qpc_now() - calibration_qpc) / double(qpc_frequency());
    return seconds > 0.01 ? double(__rdtsc() - calibration_tsc) / seconds / 1e6 : 4000.0;
}
bool read_u32(std::uintptr_t at, std::uint32_t& value) noexcept {
    __try {
        value = *reinterpret_cast<volatile const std::uint32_t*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
}  // namespace

Discovery discover(std::uintptr_t base, const std::uintptr_t* matches, unsigned count) noexcept {
    Discovery d;
    d.matches = count;
    if (count != 1) {
        d.reason = count ? "signature not unique" : "signature not found";
        return d;
    }
    const auto site = matches[0];
    const auto* code = reinterpret_cast<const std::uint8_t*>(site);
    const auto slot = static_cast<std::uintptr_t>(
        static_cast<std::int64_t>(site + import_operand + 6) + read_i32(code + import_operand + 2));
    std::uintptr_t function{};
    __try {
        function = *reinterpret_cast<volatile const std::uintptr_t*>(slot);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        function = 0;
    }
    if (function != reinterpret_cast<std::uintptr_t>(&WaitForSingleObject)) {
        d.reason = "call is not WaitForSingleObject";
        return d;
    }
    d.found = true;
    d.reason = "ok";
    d.site_rva = static_cast<std::uint32_t>(site - base);
    return d;
}

std::string discovery_event(const Discovery& d, double t_ms, std::string_view scanner) {
    std::string out = "{";
    json::key(out, "event");
    json::string(out, "worker_spin");
    json::key(out, "t_ms");
    json::fixed(out, t_ms, 3);
    json::key(out, "found");
    out += d.found ? "true" : "false";
    json::key(out, "reason");
    json::string(out, d.reason);
    json::key(out, "scanner");
    json::string(out, scanner);
    json::key(out, "matches");
    json::number(out, std::uint64_t(d.matches));
    json::key(out, "site_rva");
    json::number(out, std::uint64_t(d.site_rva));
    out += '}';
    return out;
}

std::uintptr_t make_stub(std::uintptr_t site, const void* function) noexcept {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const std::uintptr_t granularity = si.dwAllocationGranularity;
    constexpr std::uintptr_t reach = 0x7FF00000;  // under 2 GB, with room for the page
    const std::uintptr_t lo = site > reach + granularity ? site - reach : granularity;
    const std::uintptr_t hi = site + reach;
    std::uint8_t* page = nullptr;
    for (std::uintptr_t at = lo; at < hi && !page;) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<const void*>(at), &mbi, sizeof(mbi))) break;
        const auto region = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        const auto end = region + mbi.RegionSize;
        if (mbi.State == MEM_FREE) {
            const auto candidate = (region + granularity - 1) & ~(granularity - 1);
            if (candidate + si.dwPageSize <= end && candidate + si.dwPageSize <= hi && candidate >= lo)
                page = static_cast<std::uint8_t*>(
                    VirtualAlloc(reinterpret_cast<void*>(candidate), si.dwPageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        }
        at = end > at ? end : at + granularity;
    }
    if (!page) return 0;
    const std::uint8_t jump[6] = {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};  // jmp [rip+0]
    std::memcpy(page, jump, sizeof(jump));
    const auto target = reinterpret_cast<std::uintptr_t>(function);
    std::memcpy(page + sizeof(jump), &target, sizeof(target));
    DWORD old{};
    if (!VirtualProtect(page, si.dwPageSize, PAGE_EXECUTE_READ, &old)) {
        VirtualFree(page, 0, MEM_RELEASE);
        return 0;
    }
    FlushInstructionCache(GetCurrentProcess(), page, 14);
    return reinterpret_cast<std::uintptr_t>(page);
}

bool build_replacement(std::uintptr_t target, std::uintptr_t stub, std::array<std::uint8_t, patch_length>& out) noexcept {
    const auto next = static_cast<std::int64_t>(target + patch_length);  // after the call: where the old call returned
    const auto delta = static_cast<std::int64_t>(stub) - next;
    if (!stub || delta < INT32_MIN || delta > INT32_MAX) return false;
    const auto rel = static_cast<std::int32_t>(delta);
    const std::uint8_t head[7] = {0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00, 0xE8};  // nop word [rax+rax+0]; call rel32
    std::memcpy(out.data(), head, sizeof(head));
    std::memcpy(out.data() + sizeof(head), &rel, sizeof(rel));
    return true;
}

DWORD __fastcall wait(HANDLE semaphore) {
    if (on.load(std::memory_order_relaxed)) {
        const auto budget = window_cycles.load(std::memory_order_relaxed);
        const auto yield = yield_cycles.load(std::memory_order_relaxed);
        auto seen = generation.load(std::memory_order_acquire);
        auto start = __rdtsc(), yielded = start;
        while (on.load(std::memory_order_relaxed)) {
            const auto g = generation.load(std::memory_order_acquire);
            if (g != seen) {
                seen = g;
                start = __rdtsc();
                auto left = places.load(std::memory_order_relaxed);
                while (left > 0 && !places.compare_exchange_weak(left, left - 1, std::memory_order_acq_rel)) {}
                if (left > 0) {
                    // A place in this round: its permit comes as soon as executeJob releases.
                    const auto until = __rdtsc() + claim_cycles.load(std::memory_order_relaxed);
                    do {
                        if (WaitForSingleObject(semaphore, 0) == WAIT_OBJECT_0) {
                            claimed.fetch_add(1, std::memory_order_relaxed);
                            return WAIT_OBJECT_0;
                        }
                        _mm_pause();
                    } while (__rdtsc() < until);
                    gave_up.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
                continue;
            }
            const auto now = __rdtsc();
            if (now - start > budget) break;
            if (now - yielded > yield) {
                SwitchToThread();
                yielded = now;
            }
            for (int i = 0; i < 16; ++i) _mm_pause();
        }
    }
    blocked.fetch_add(1, std::memory_order_relaxed);
    return WaitForSingleObject(semaphore, INFINITE);
}

void before_round(std::uintptr_t smain, std::uintptr_t jobs, std::uint32_t count) noexcept {
    if (!on.load(std::memory_order_relaxed)) return;
    std::uint32_t n = count, participants{};
    if (!jobs && !read_u32(smain + count_offset, n)) return;
    if (!read_u32(smain + participants_offset, participants)) return;
    if (n < 2 || participants < 2) return;  // executeJob runs these on the calling thread
    const auto released = (n >= participants ? participants : n) - 1;
    places.store(static_cast<std::int32_t>(released), std::memory_order_relaxed);
    generation.fetch_add(1, std::memory_order_release);
    rounds.fetch_add(1, std::memory_order_relaxed);
}

void set_active(bool enable, unsigned spin_us) noexcept {
    const auto per_us = tsc_per_us();
    window_us.store(spin_us);
    window_cycles.store(static_cast<std::uint64_t>(spin_us * per_us));
    claim_cycles.store(static_cast<std::uint64_t>(200 * per_us));
    yield_cycles.store(static_cast<std::uint64_t>(20 * per_us));
    on.store(enable);
}

bool active() noexcept { return on.load(std::memory_order_relaxed); }

void append(std::string& out) {
    json::key(out, "spin");
    out += '{';
    json::key(out, "active");
    out += on.load(std::memory_order_relaxed) ? "true" : "false";
    json::key(out, "spin_us");
    json::number(out, std::uint64_t(window_us.load(std::memory_order_relaxed)));
    json::key(out, "rounds");
    json::number(out, rounds.exchange(0, std::memory_order_relaxed));
    json::key(out, "claimed");
    json::number(out, claimed.exchange(0, std::memory_order_relaxed));
    json::key(out, "blocked");
    json::number(out, blocked.exchange(0, std::memory_order_relaxed));
    json::key(out, "gave_up");
    json::number(out, gave_up.exchange(0, std::memory_order_relaxed));
    out += '}';
}
}  // namespace cpuopt::worker_spin
