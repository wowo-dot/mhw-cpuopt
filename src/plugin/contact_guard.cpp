#include "contact_guard.hpp"
#include "../core/json.hpp"
#include "log.hpp"
#include <windows.h>
#include <atomic>
#include <cstdio>

namespace cpuopt::contact_guard {
namespace {
// Every target takes integer or pointer arguments in its first four places,
// so one 10-argument forwarder fits all of them (the job: 2; the hit check:
// 7). Arguments past the fourth are copied from the caller's stack as they
// are, whatever the caller passed.
using Fn10 = std::uintptr_t (*)(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t,
                                std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t);
using Job = std::uintptr_t (*)(std::uintptr_t, std::uintptr_t);
void* next_fn[site_count]{};

std::atomic<bool> locking_on{}, detail_on{}, ready_seen{}, bypass_on{};
std::atomic<bool (*)() noexcept> probe{};
std::uintptr_t known[4]{};  // the four callbacks' addresses: hit, nearest, list, geometry

// The lock: a critical section that spins before it sleeps. The guarded
// callbacks hold it about 70 ns each, some 700 times a frame in a fight; an
// SRW lock's waiters went to sleep and were woken for each (al-46: 1.2 ms of
// waiting a frame summed over the job threads for 48 us held), while a
// spinning waiter usually gets it within a few hundred ns. After the spin it
// sleeps as before, so a holder that Windows interrupted costs a short spin,
// not a core. 4000 spins, as the Windows heap's lock.
constexpr DWORD spin_count = 4000;
struct Lock {
    CRITICAL_SECTION cs;
    Lock() noexcept { InitializeCriticalSectionEx(&cs, spin_count, CRITICAL_SECTION_NO_DEBUG_INFO); }
    ~Lock() { DeleteCriticalSection(&cs); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
};
Lock lock;
thread_local unsigned depth{};

// Window counters.
struct Counter {  // one per 64 bytes, so job threads counting apart do not share a line
    std::atomic<std::uint64_t> v{};
    char pad[56]{};
};
Counter calls[site_count], overlaps[site_count];
Counter acquired, contended, wait_qpc, hold_qpc, unknown_jobs, overflows, fullest;
std::atomic<std::uint64_t> unknown_last{};  // the last unknown callback (address)

// Same-object overlaps (detail): threads in the guarded code per object.
constexpr unsigned slots = 4096;
std::atomic<std::uint32_t> inflight[slots]{};
unsigned slot_of(std::uintptr_t key) noexcept { return static_cast<unsigned>((key >> 4) * 0x9E3779B97F4A7C15ULL >> 52) & (slots - 1); }

std::uintptr_t read_ptr(std::uintptr_t at) noexcept {
    __try {
        return *reinterpret_cast<volatile const std::uintptr_t*>(at);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
std::uint32_t read_u32(std::uintptr_t at) noexcept {
    __try {
        return *reinterpret_cast<volatile const std::uint32_t*>(at);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// The lock for one guarded call: taken by the outermost guarded call on
// this thread, nested ones pass.
class Hold {
public:
    Hold(Site s, std::uintptr_t key1, std::uintptr_t key2) noexcept {
        if (depth++ > 0) return;
        if (detail_on.load(std::memory_order_relaxed)) {
            detail_ = true;
            if (key2 == key1) key2 = 0;  // one object once: not an overlap with itself
            for (auto k : {key1, key2})
                if (k) {
                    auto& slot = inflight[slot_of(k)];
                    if (slot.fetch_add(1, std::memory_order_acq_rel) > 0) overlaps[s].v.fetch_add(1, std::memory_order_relaxed);
                    keys_[count_++] = k;
                }
        }
        if (!TryEnterCriticalSection(&lock.cs)) {
            contended.v.fetch_add(1, std::memory_order_relaxed);
            const auto t = detail_ ? qpc_now() : 0;
            EnterCriticalSection(&lock.cs);
            if (detail_) wait_qpc.v.fetch_add(static_cast<std::uint64_t>(qpc_now() - t), std::memory_order_relaxed);
        }
        acquired.v.fetch_add(1, std::memory_order_relaxed);
        took_ = true;
        if (detail_) from_ = qpc_now();
    }
    ~Hold() {
        if (took_) {
            if (detail_) hold_qpc.v.fetch_add(static_cast<std::uint64_t>(qpc_now() - from_), std::memory_order_relaxed);
            LeaveCriticalSection(&lock.cs);
        }
        for (unsigned i = 0; i < count_; ++i) inflight[slot_of(keys_[i])].fetch_sub(1, std::memory_order_acq_rel);
        --depth;
    }
    Hold(const Hold&) = delete;
    Hold& operator=(const Hold&) = delete;

private:
    bool took_{}, detail_{};
    std::int64_t from_{};
    std::uintptr_t keys_[2]{};
    unsigned count_{};
};

// The four callbacks: codes 4 and 2 from the job, 0 through hitCheckCallback
// (already held then). Key: the callback's context; for the hit callback
// also the hit object (the hit node's +0x80).
template <Site S>
std::uintptr_t callback_detour(std::uintptr_t a1, std::uintptr_t a2, std::uintptr_t a3, std::uintptr_t a4, std::uintptr_t a5,
                               std::uintptr_t a6, std::uintptr_t a7, std::uintptr_t a8, std::uintptr_t a9, std::uintptr_t a10) {
    const auto original = reinterpret_cast<Fn10>(next_fn[S]);
    if (!locking_on.load(std::memory_order_acquire)) return original(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10);
    calls[S].v.fetch_add(1, std::memory_order_relaxed);
    if (bypass_on.load(std::memory_order_relaxed)) return original(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10);
    // The hit node is the fourth argument only for codes 0 and 4 (code 2,
    // the job's end, passes three).
    const bool has_node = (a2 & 0xFFFFFFFFu) == 0 || (a2 & 0xFFFFFFFFu) == 4;
    const auto target = S == hit && has_node && a4 && detail_on.load(std::memory_order_relaxed) ? read_ptr(a4 + node_owner) : 0;
    Hold h(S, a1, target);
    return original(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10);
}

// hitCheckCallback(this, ?, hit node, ?, ?, contact, &{info, ...}): both
// callbacks of one contact. Keys: the active node's context and the hit
// node's owner.
std::uintptr_t hit_check_detour(std::uintptr_t a1, std::uintptr_t a2, std::uintptr_t a3, std::uintptr_t a4, std::uintptr_t a5,
                                std::uintptr_t a6, std::uintptr_t a7, std::uintptr_t a8, std::uintptr_t a9, std::uintptr_t a10) {
    const auto original = reinterpret_cast<Fn10>(next_fn[hit_check]);
    if (!locking_on.load(std::memory_order_acquire)) return original(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10);
    calls[hit_check].v.fetch_add(1, std::memory_order_relaxed);
    if (bypass_on.load(std::memory_order_relaxed)) return original(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10);
    std::uintptr_t context{}, owner{};
    if (detail_on.load(std::memory_order_relaxed)) {
        const auto info = a7 ? read_ptr(a7) : 0;
        context = info ? read_ptr(info + info_context) : 0;
        owner = a3 ? read_ptr(a3 + node_owner) : 0;
    }
    Hold h(hit_check, context, owner);
    return original(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10);
}

bool known_callback(std::uintptr_t fn) noexcept {
    for (const auto k : known)
        if (k && fn == k) return true;
    return false;
}

// executeTargetActiveNodeContact(node list, info): a job whose callback is
// none of the four runs whole under the lock.
std::uintptr_t job_detour(std::uintptr_t list, std::uintptr_t info) {
    const auto original = reinterpret_cast<Job>(next_fn[job]);
    if (!locking_on.load(std::memory_order_acquire)) return original(list, info);
    calls[job].v.fetch_add(1, std::memory_order_relaxed);
    if (bypass_on.load(std::memory_order_relaxed)) return original(list, info);
    const auto callback = info ? read_ptr(info + info_callback) : 0;
    if (!callback || known_callback(callback)) return original(list, info);
    unknown_jobs.v.fetch_add(1, std::memory_order_relaxed);
    unknown_last.store(callback, std::memory_order_relaxed);
    Hold h(job, info ? read_ptr(info + info_context) : 0, 0);
    return original(list, info);
}

// The record writer: counts only. a1 is the hit object.
std::uintptr_t records_detour(std::uintptr_t a1, std::uintptr_t a2, std::uintptr_t a3, std::uintptr_t a4, std::uintptr_t a5,
                              std::uintptr_t a6, std::uintptr_t a7, std::uintptr_t a8, std::uintptr_t a9, std::uintptr_t a10) {
    const auto original = reinterpret_cast<Fn10>(next_fn[records]);
    if (locking_on.load(std::memory_order_acquire)) {
        calls[records].v.fetch_add(1, std::memory_order_relaxed);
        const auto count = a1 ? read_u32(a1 + record_count) : 0;
        if (count >= record_capacity) overflows.v.fetch_add(1, std::memory_order_relaxed);
        auto seen = fullest.v.load(std::memory_order_relaxed);
        while (count > seen && !fullest.v.compare_exchange_weak(seen, count, std::memory_order_relaxed)) {}
    }
    return original(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10);
}

void* const detours[site_count] = {
    reinterpret_cast<void*>(&hit_check_detour),       reinterpret_cast<void*>(&job_detour),
    reinterpret_cast<void*>(&callback_detour<hit>),   reinterpret_cast<void*>(&callback_detour<nearest>),
    reinterpret_cast<void*>(&callback_detour<list>),  reinterpret_cast<void*>(&callback_detour<geometry>),
    reinterpret_cast<void*>(&records_detour),
};

std::uint64_t take(Counter& c) noexcept { return c.v.exchange(0, std::memory_order_relaxed); }
}  // namespace

Discovery discover(const std::uintptr_t (&sites)[site_count], const unsigned (&counts)[site_count]) noexcept {
    Discovery d;
    for (unsigned i = 0; i < site_count; ++i) {
        d.counts[i] = counts[i];
        d.sites[i] = sites[i];
    }
    for (const auto c : counts)
        if (c != 1) {
            d.reason = c ? "signature not unique" : "signature not found";
            return d;
        }
    d.found = true;
    d.reason = "ok";
    return d;
}

std::string discovery_event(const Discovery& d, std::uintptr_t base, double t_ms, std::string_view scanner) {
    std::string out = "{";
    json::key(out, "event");
    json::string(out, "contact_guard");
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
    for (unsigned i = 0; i < site_count; ++i) {
        if (i) out += ',';
        json::number(out, std::uint64_t(d.counts[i]));
    }
    out += ']';
    json::key(out, "site_rvas");
    out += '[';
    for (unsigned i = 0; i < site_count; ++i) {
        if (i) out += ',';
        json::number(out, std::uint64_t(d.found && d.sites[i] > base ? d.sites[i] - base : 0));
    }
    out += "]}";
    return out;
}

void set_target(const Discovery& d) noexcept {
    for (unsigned i = 0; i < 4; ++i) known[i] = d.found ? d.sites[hit + i] : 0;
}

void* detour(Site s) noexcept { return s < site_count ? detours[s] : nullptr; }
void** next(Site s) noexcept { return s < site_count ? &next_fn[s] : nullptr; }

void set_ready_probe(bool (*p)() noexcept) noexcept { probe.store(p); }
bool ready() noexcept {
    if (ready_seen.load(std::memory_order_acquire)) return true;
    const auto p = probe.load();
    if (!p || !p()) return false;
    ready_seen.store(true, std::memory_order_release);  // the hooks are never taken out once active
    return true;
}
void set_locking(bool on) noexcept { locking_on.store(on, std::memory_order_release); }
bool locking() noexcept { return locking_on.load(std::memory_order_acquire); }
void set_detail(bool on) noexcept { detail_on.store(on); }
void set_bypass(bool on) noexcept { bypass_on.store(on); }

void append(std::string& out) {
    json::key(out, "contact_guard");
    out += '{';
    json::key(out, "ready");
    out += ready_seen.load() ? "true" : "false";
    json::key(out, "locking");
    out += locking_on.load() ? "true" : "false";
    if (bypass_on.load()) {
        json::key(out, "bypass");
        out += "true";
    }
    json::key(out, "calls");
    out += '{';
    for (unsigned i = 0; i < site_count; ++i) {
        json::key(out, short_names[i]);
        json::number(out, take(calls[i]));
    }
    out += '}';
    json::key(out, "acquired");
    json::number(out, take(acquired));
    json::key(out, "contended");
    json::number(out, take(contended));
    json::key(out, "unknown_jobs");
    json::number(out, take(unknown_jobs));
    if (const auto u = unknown_last.exchange(0)) {
        json::key(out, "unknown_rva");
        json::number(out, std::uint64_t(u - reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr))));
    }
    json::key(out, "overflows");
    json::number(out, take(overflows));
    json::key(out, "fullest");
    json::number(out, take(fullest));
    if (detail_on.load()) {
        const double f = double(qpc_frequency());
        json::key(out, "wait_us");
        json::fixed(out, double(take(wait_qpc)) * 1e6 / f, 1);
        json::key(out, "hold_us");
        json::fixed(out, double(take(hold_qpc)) * 1e6 / f, 1);
        json::key(out, "overlaps");
        out += '{';
        for (unsigned i = 0; i < site_count; ++i) {
            json::key(out, short_names[i]);
            json::number(out, take(overlaps[i]));
        }
        out += '}';
    }
    out += '}';
}
}  // namespace cpuopt::contact_guard
