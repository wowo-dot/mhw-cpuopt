#include "job_timing.hpp"
#include "../core/json.hpp"
#include "log.hpp"
#include "worker_spin.hpp"
#include <windows.h>
#include <intrin.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace cpuopt::job_timing {
namespace {
ExecuteJob next_fn{};
std::atomic<bool> measuring{};

// Game thread only: executeJob runs one round at a time, on the game thread.
std::array<Record, capacity> records;

using Job = std::uintptr_t (*)(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t);
// Runs in place of every job of a timed round, on whichever thread claims it.
std::uintptr_t job_thunk(Record* r, std::uintptr_t a2, std::uintptr_t a3, std::uintptr_t a4) {
    r->thread = GetCurrentThreadId();
    r->start = __rdtsc();
    const auto result = reinterpret_cast<Job>(r->function)(r->context, a2, a3, a4);
    r->end = __rdtsc();
    return result;
}

struct CallerStats {
    std::uintptr_t caller{};
    std::uint64_t rounds{}, jobs{}, duration{}, work{}, longest{}, bound{};
    std::uint64_t last_frac_milli{}, last_in_tail{}, last_started_late{}, threads_used{};
};
struct JobStats {
    std::uintptr_t caller{}, function{}, vtable{};
    std::uint64_t count{}, total{}, max{}, last{};
};
constexpr unsigned caller_capacity = 64, job_stats_capacity = 4096;
SRWLOCK lock = SRWLOCK_INIT;
std::array<CallerStats, caller_capacity> callers{};
std::array<JobStats, job_stats_capacity> job_stats{};
std::uint64_t rounds_total{}, skipped{}, overflow{};

// Traced rounds: written by the game thread, drained by the window thread.
struct TraceJob {
    std::uintptr_t vtable{}, function{};
    std::uint64_t start{}, end{};
    std::uint32_t thread{};
};
struct Trace {
    std::uintptr_t caller{};
    std::uint32_t mode{}, participants{}, jobs{}, game_thread{};
    std::uint64_t start{}, end{};
    std::array<TraceJob, trace_jobs> list;
};
std::atomic<std::uint32_t> trace_every{499};
std::uint64_t timed_rounds{};  // game thread only
std::unique_ptr<std::array<Trace, trace_slots>> traces;
std::atomic<std::uint32_t> trace_head{}, trace_tail{};  // head: next to fill (game thread); tail: next to drain
std::atomic<std::uint64_t> traces_dropped{};

// Ordering (switch order_jobs): the camera draw round only.
struct Entry {
    std::uint64_t q[4];  // context, function, argument, spare
};
struct ClassCost {
    std::uintptr_t vtable{};
    std::uint64_t mean{};  // cycles, running mean
    std::uint32_t samples{};
};
constexpr unsigned class_capacity = 4096;
std::atomic<std::uintptr_t> camera_return_address{};
std::atomic<int> order_mode{};
std::atomic<std::uint64_t> order_threshold{};  // cycles
std::atomic<unsigned> order_threshold_us{};
// Game thread only.
std::array<Entry, capacity> backup;
std::array<std::uint8_t, capacity> moved_flag{};
std::array<std::pair<std::uint64_t, std::uint32_t>, capacity> front;  // (mean, index)
std::array<ClassCost, class_capacity> class_costs{};
std::uint64_t camera_rounds{};
std::atomic<std::uint32_t> classes_known{};
std::atomic<std::uint64_t> order_rounds{}, order_moved{}, order_timed{};
// TSC rate, from the TSC and QPC at start and when a threshold is set.
const std::uint64_t calibration_tsc = __rdtsc();
const std::int64_t calibration_qpc = qpc_now();
std::int64_t opened = qpc_now();
double tsc_per_us() noexcept {
    const auto seconds = double(qpc_now() - calibration_qpc) / double(qpc_frequency());
    return seconds > 0.01 ? double(__rdtsc() - calibration_tsc) / seconds / 1e6 : 4000.0;
}

// Inline rounds (switch inline_rounds). Sites are written by the game thread
// and read by the window thread: relaxed atomics, no lock per round.
struct Site {
    std::atomic<std::uintptr_t> caller{};
    std::atomic<std::uint64_t> mean{};  // cycles of work per round, running mean
    std::atomic<std::uint32_t> samples{};
    std::atomic<bool> alone{};
    std::uint64_t rounds{};  // game thread only
};
std::array<Site, site_capacity> sites{};
std::atomic<bool> inline_on{};
std::atomic<std::uint64_t> inline_threshold{};  // cycles
std::atomic<unsigned> inline_threshold_us{};
std::atomic<std::uint64_t> alone_rounds{}, inline_timed{}, inline_full{};
Site* site_of(std::uintptr_t caller) noexcept {
    for (auto& s : sites) {
        const auto c = s.caller.load(std::memory_order_relaxed);
        if (c == caller) return &s;
        if (!c) {
            s.caller.store(caller, std::memory_order_relaxed);
            return &s;
        }
    }
    inline_full.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}
bool threads_created(std::uintptr_t smain) noexcept {
    __try {
        return *reinterpret_cast<volatile const std::uint8_t*>(smain + threads_created_offset) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// executeJob, alone on this thread when `alone`: participants 1 for the call.
void execute(ExecuteJob original, std::uintptr_t smain, int mode, std::uintptr_t jobs, std::uint32_t count, std::uintptr_t extra,
             bool alone) {
    if (!alone) {
        worker_spin::before_round(smain, jobs, count);  // spinning job threads claim their places
        original(smain, mode, jobs, count, extra);
        return;
    }
    auto* participants = reinterpret_cast<volatile std::uint32_t*>(smain + participants_offset);
    const auto saved = *participants;
    *participants = 1;
    original(smain, mode, jobs, count, extra);
    *participants = saved;
    alone_rounds.fetch_add(1, std::memory_order_relaxed);
}
// One timed round's work into its site's mean, and the site's decision.
void learn_site(Site& site, std::uint64_t work) noexcept {
    const auto samples = site.samples.load(std::memory_order_relaxed);
    const auto mean = site.mean.load(std::memory_order_relaxed);
    const auto next = samples ? mean - mean / 8 + work / 8 : work;
    site.mean.store(next, std::memory_order_relaxed);
    site.samples.store(samples + 1, std::memory_order_relaxed);
    if (samples + 1 < inline_learn) return;
    const auto threshold = inline_threshold.load(std::memory_order_relaxed);
    const bool alone = site.alone.load(std::memory_order_relaxed);
    site.alone.store(alone ? next < threshold + threshold / 2 : next < threshold, std::memory_order_relaxed);
}

bool writable(std::uintptr_t address, std::size_t bytes) noexcept {
    const auto end = address + bytes;
    while (address < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
            (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
            !(mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
            return false;
        address = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    }
    return true;
}
bool read_u32(std::uintptr_t at, std::uint32_t& value) noexcept {
    __try {
        value = *reinterpret_cast<volatile const std::uint32_t*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// The object's vtable, when the context is an object (modes 0 and 1); 0 otherwise.
std::uintptr_t vtable_of(std::uintptr_t context) noexcept {
    if (context < 0x10000) return 0;
    __try {
        return *reinterpret_cast<volatile const std::uintptr_t*>(context);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
void wrap(std::uintptr_t list, std::uint32_t n) noexcept {
    for (std::uint32_t i = 0; i < n; ++i) {
        auto* entry = reinterpret_cast<std::uintptr_t*>(list + 32ULL * i);
        records[i] = {entry[1], entry[0], 0, 0, 0};
        entry[0] = reinterpret_cast<std::uintptr_t>(&records[i]);
        entry[1] = reinterpret_cast<std::uintptr_t>(&job_thunk);
    }
}
void unwrap(std::uintptr_t list, std::uint32_t n) noexcept {
    for (std::uint32_t i = 0; i < n; ++i) {
        auto* entry = reinterpret_cast<std::uintptr_t*>(list + 32ULL * i);
        entry[0] = records[i].context;
        entry[1] = records[i].function;
    }
}
std::uintptr_t vtable_of(std::uintptr_t context) noexcept;
void keep_trace(std::uintptr_t caller, int mode, std::uint32_t participants, std::uint32_t n, std::uint64_t t0, std::uint64_t t1) noexcept {
    const auto every = trace_every.load(std::memory_order_relaxed);
    if (!every || ++timed_rounds % every || n > trace_jobs || !traces) return;
    const auto head = trace_head.load(std::memory_order_relaxed);
    if (head - trace_tail.load(std::memory_order_acquire) >= trace_slots) {
        traces_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    auto& t = (*traces)[head % trace_slots];
    t.caller = caller;
    t.mode = static_cast<std::uint32_t>(mode);
    t.participants = participants;
    t.jobs = n;
    t.game_thread = GetCurrentThreadId();
    t.start = t0;
    t.end = t1;
    for (std::uint32_t i = 0; i < n; ++i) {
        const auto& r = records[i];
        t.list[i] = {mode <= 1 ? vtable_of(r.context) : 0, r.function, r.start, r.end, r.thread};
    }
    trace_head.store(head + 1, std::memory_order_release);
}
std::uintptr_t vtable_of(std::uintptr_t context) noexcept;
ClassCost* class_slot(std::uintptr_t vtable, bool insert) noexcept {
    auto slot = static_cast<unsigned>((vtable * 0x9E3779B97F4A7C15ULL) >> 52) & (class_capacity - 1);
    for (unsigned probe = 0; probe < 64; ++probe, slot = (slot + 1) & (class_capacity - 1)) {
        auto& c = class_costs[slot];
        if (c.vtable == vtable) return &c;
        if (!c.vtable) {
            if (!insert) return nullptr;
            c.vtable = vtable;
            classes_known.fetch_add(1, std::memory_order_relaxed);
            return &c;
        }
    }
    return nullptr;
}
// Each timed job's duration into its class's running mean (weight 1/8).
void learn(std::uint32_t n) noexcept {
    for (std::uint32_t i = 0; i < n; ++i) {
        const auto& rec = records[i];
        if (!rec.end || rec.end < rec.start) continue;
        const auto vtable = vtable_of(rec.context);
        auto* c = vtable ? class_slot(vtable, true) : nullptr;
        if (!c) continue;
        const auto d = rec.end - rec.start;
        c->mean = c->samples ? c->mean - c->mean / 8 + d / 8 : d;
        ++c->samples;
    }
}
// The list in its round order; the game's order stays in `backup`. Returns
// how many jobs were moved to the front (all of them when reversed).
std::uint32_t reorder(std::uintptr_t list, std::uint32_t n, int mode) noexcept {
    auto* entries = reinterpret_cast<Entry*>(list);
    std::copy(entries, entries + n, backup.begin());
    if (mode == 1) {
        for (std::uint32_t i = 0; i < n; ++i) entries[i] = backup[n - 1 - i];
        return n;
    }
    const auto threshold = order_threshold.load(std::memory_order_relaxed);
    std::uint32_t k = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
        const auto vtable = vtable_of(backup[i].q[0]);
        const auto* c = vtable ? class_slot(vtable, false) : nullptr;
        if (c && c->samples >= 4 && c->mean >= threshold) front[k++] = {c->mean, i};
    }
    if (!k) return 0;
    std::stable_sort(front.begin(), front.begin() + k, [](const auto& a, const auto& b) { return a.first > b.first; });
    std::uint32_t at = 0;
    for (std::uint32_t j = 0; j < k; ++j) {
        entries[at++] = backup[front[j].second];
        moved_flag[front[j].second] = 1;
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        if (moved_flag[i]) {
            moved_flag[i] = 0;
            continue;
        }
        entries[at++] = backup[i];
    }
    return k;
}
void restore_order(std::uintptr_t list, std::uint32_t n) noexcept {
    std::copy(backup.begin(), backup.begin() + n, reinterpret_cast<Entry*>(list));
}
void merge(std::uintptr_t caller, const Round& r, std::uint32_t n) noexcept {
    AcquireSRWLockExclusive(&lock);
    ++rounds_total;
    CallerStats* c = nullptr;
    for (auto& candidate : callers) {
        if (candidate.rounds && candidate.caller != caller) continue;
        c = &candidate;
        break;
    }
    if (c) {
        c->caller = caller;
        ++c->rounds;
        c->jobs += r.jobs;
        c->duration += r.duration;
        c->work += r.work;
        c->longest += r.longest;
        c->bound += r.bound;
        c->last_frac_milli += n ? std::uint64_t(r.last) * 1000 / n : 0;
        c->last_in_tail += r.last >= n - n / 10 ? 1 : 0;
        c->last_started_late += r.last_start * 2 > r.duration ? 1 : 0;
        c->threads_used += r.threads_used;
    } else {
        ++overflow;
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        const auto& rec = records[i];
        if (!rec.end || rec.end < rec.start) continue;
        const auto vtable = vtable_of(rec.context);
        auto slot = static_cast<unsigned>((caller ^ rec.function * 31 ^ vtable * 131) * 0x9E3779B97F4A7C15ULL >> 52) & (job_stats_capacity - 1);
        bool stored = false;
        for (unsigned probe = 0; probe < 32 && !stored; ++probe, slot = (slot + 1) & (job_stats_capacity - 1)) {
            auto& j = job_stats[slot];
            if (j.count && (j.caller != caller || j.function != rec.function || j.vtable != vtable)) continue;
            const auto d = rec.end - rec.start;
            j.caller = caller;
            j.function = rec.function;
            j.vtable = vtable;
            ++j.count;
            j.total += d;
            j.max = std::max(j.max, d);
            j.last += i == r.last ? 1 : 0;
            stored = true;
        }
        if (!stored) ++overflow;
    }
    ReleaseSRWLockExclusive(&lock);
}
}  // namespace

ExecuteJob* next_slot() noexcept { return &next_fn; }
void set_trace_every(std::uint32_t every) noexcept { trace_every.store(every); }
void set_measuring(bool on) noexcept {
    if (on && !traces) {
        try {
            traces = std::make_unique<std::array<Trace, trace_slots>>();
        } catch (...) {
        }
    }
    measuring.store(on);
}

Round analyse(const Record* recs, std::uint32_t count, std::uint64_t round_start, std::uint64_t round_end, std::uint32_t participants) noexcept {
    Round r;
    r.jobs = count;
    r.duration = round_end > round_start ? round_end - round_start : 0;
    std::uint64_t last_end{};
    std::array<std::uint32_t, 64> seen{};
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto& rec = recs[i];
        if (!rec.end || rec.end < rec.start) continue;  // did not run
        const auto d = rec.end - rec.start;
        r.work += d;
        r.longest = std::max(r.longest, d);
        if (rec.end > last_end) {
            last_end = rec.end;
            r.last = i;
            r.last_start = rec.start > round_start ? rec.start - round_start : 0;
            r.last_duration = d;
        }
        bool known = false;
        for (std::uint32_t k = 0; k < r.threads_used && !known; ++k) known = seen[k] == rec.thread;
        if (!known && r.threads_used < seen.size()) seen[r.threads_used++] = rec.thread;
    }
    const auto lanes = std::max<std::uint32_t>(1, std::min(participants, count));
    r.bound = std::max(r.longest, r.work / lanes);
    return r;
}

void __fastcall detour(std::uintptr_t smain, int mode, std::uintptr_t jobs, std::uint32_t count, std::uintptr_t extra) {
    const auto original = next_fn;
    const bool timing = measuring.load(std::memory_order_acquire);  // acquire: the trace ring exists
    const int order = order_mode.load(std::memory_order_relaxed);
    const bool inl = inline_on.load(std::memory_order_relaxed);
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const bool camera = order && caller == camera_return_address.load(std::memory_order_relaxed);
    if ((!timing && !camera && !inl) || mode < 0 || mode > 5) {
        execute(original, smain, mode, jobs, count, extra, false);
        return;
    }
    // Inline rounds: this site's decision, and whether this round is timed for it.
    Site* site = inl && !camera ? site_of(caller) : nullptr;
    bool alone = false, site_round = false;
    if (site) {
        const auto distant = site->mean.load(std::memory_order_relaxed) >= 4 * inline_threshold.load(std::memory_order_relaxed);
        site_round = site->samples.load(std::memory_order_relaxed) < inline_learn || ++site->rounds % (distant ? 256 : learn_every) == 0;
        alone = site->alone.load(std::memory_order_relaxed) && threads_created(smain);
    }
    if (!timing && !camera && !site_round) {
        execute(original, smain, mode, jobs, count, extra, alone);
        return;
    }
    std::uintptr_t list = jobs;
    std::uint32_t n = count, participants = 1;
    if (!list) {  // the caller filled sMain's own list and count
        list = smain + default_jobs_offset;
        if (!read_u32(smain + job_count_offset, n)) n = 0;
    }
    if (!read_u32(smain + participants_offset, participants)) participants = 1;
    if (n == 0 || n > capacity || !writable(list, 32ULL * n)) {
        AcquireSRWLockExclusive(&lock);
        ++skipped;
        ReleaseSRWLockExclusive(&lock);
        execute(original, smain, mode, jobs, count, extra, alone);
        return;
    }
    // Reorder first: timing then wraps the entries in their round order.
    std::uint32_t moved = 0;
    if (camera && n >= 2) {
        moved = reorder(list, n, order);
        order_rounds.fetch_add(1, std::memory_order_relaxed);
        order_moved.fetch_add(moved, std::memory_order_relaxed);
    }
    const bool learn_round = camera && order == 2 && (timing || ++camera_rounds % learn_every == 0);
    const bool wrapped = timing || learn_round || site_round;
    if (wrapped) wrap(list, n);
    const auto t0 = __rdtsc();
    execute(original, smain, mode, jobs, count, extra, alone);
    const auto t1 = __rdtsc();
    if (wrapped) unwrap(list, n);
    if (learn_round) {
        learn(n);
        order_timed.fetch_add(1, std::memory_order_relaxed);
    }
    if (moved) restore_order(list, n);
    if (site_round || timing) {
        const auto r = analyse(records.data(), n, t0, t1, alone ? 1 : participants);
        if (site_round) {
            learn_site(*site, r.work);
            inline_timed.fetch_add(1, std::memory_order_relaxed);
        }
        if (timing) {
            merge(caller, r, n);
            keep_trace(caller, mode, alone ? 1 : participants, n, t0, t1);
        }
    }
}

void set_camera_return(std::uintptr_t address) noexcept { camera_return_address.store(address); }

bool set_order(int mode, unsigned threshold_us) noexcept {
    if (mode && !camera_return_address.load()) {
        order_mode.store(0);
        return false;
    }
    order_threshold.store(static_cast<std::uint64_t>(threshold_us * tsc_per_us()));
    order_threshold_us.store(threshold_us);
    order_mode.store(mode);
    return true;
}

void append_order(std::string& out) {
    json::key(out, "order");
    out += '{';
    json::key(out, "mode");
    json::number(out, std::uint64_t(order_mode.load(std::memory_order_relaxed)));
    json::key(out, "available");
    out += camera_return_address.load(std::memory_order_relaxed) ? "true" : "false";
    json::key(out, "threshold_us");
    json::number(out, std::uint64_t(order_threshold_us.load(std::memory_order_relaxed)));
    json::key(out, "rounds");
    json::number(out, order_rounds.exchange(0, std::memory_order_relaxed));
    json::key(out, "moved");
    json::number(out, order_moved.exchange(0, std::memory_order_relaxed));
    json::key(out, "timed");
    json::number(out, order_timed.exchange(0, std::memory_order_relaxed));
    json::key(out, "classes");
    json::number(out, std::uint64_t(classes_known.load(std::memory_order_relaxed)));
    out += '}';
}

void set_inline(bool on, unsigned threshold_us) noexcept {
    inline_threshold.store(static_cast<std::uint64_t>(threshold_us * tsc_per_us()));
    inline_threshold_us.store(threshold_us);
    inline_on.store(on);
}

void append_inline(std::string& out, std::uintptr_t image_base) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image_base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image_base + static_cast<std::uintptr_t>(dos->e_lfanew));
    const auto image_end = image_base + nt->OptionalHeader.SizeOfImage;
    const auto per_us = tsc_per_us();
    json::key(out, "inline");
    out += '{';
    json::key(out, "on");
    out += inline_on.load(std::memory_order_relaxed) ? "true" : "false";
    json::key(out, "threshold_us");
    json::number(out, std::uint64_t(inline_threshold_us.load(std::memory_order_relaxed)));
    json::key(out, "rounds");
    json::number(out, alone_rounds.exchange(0, std::memory_order_relaxed));
    json::key(out, "timed");
    json::number(out, inline_timed.exchange(0, std::memory_order_relaxed));
    json::key(out, "full");
    json::number(out, inline_full.exchange(0, std::memory_order_relaxed));
    // Sites: [caller RVA, mean work in 0.1 us, timed rounds, alone].
    json::key(out, "sites");
    out += '[';
    bool first = true;
    for (const auto& site : sites) {
        const auto caller = site.caller.load(std::memory_order_relaxed);
        if (!caller) break;
        out += first ? "[" : ",[";
        first = false;
        json::number(out, std::uint64_t(caller >= image_base && caller < image_end ? caller - image_base : 0));
        out += ',';
        json::number(out, std::uint64_t(double(site.mean.load(std::memory_order_relaxed)) * 10.0 / per_us + 0.5));
        out += ',';
        json::number(out, std::uint64_t(site.samples.load(std::memory_order_relaxed)));
        out += site.alone.load(std::memory_order_relaxed) ? ",1]" : ",0]";
    }
    out += "]}";
}

std::string take_event(double t_ms, std::uintptr_t image_base) {
    std::vector<CallerStats> c;
    std::vector<JobStats> j;
    std::uint64_t rounds, skip, over;
    AcquireSRWLockExclusive(&lock);
    for (auto& x : callers)
        if (x.rounds) c.push_back(x), x = {};
    for (auto& x : job_stats)
        if (x.count) j.push_back(x), x = {};
    rounds = rounds_total;
    skip = skipped;
    over = overflow;
    rounds_total = skipped = overflow = 0;
    const auto start = opened;
    opened = qpc_now();
    ReleaseSRWLockExclusive(&lock);
    if (!rounds && !skip) return {};
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image_base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image_base + static_cast<std::uintptr_t>(dos->e_lfanew));
    const auto image_end = image_base + nt->OptionalHeader.SizeOfImage;
    const auto rva = [&](std::uintptr_t a) -> std::uint64_t { return a >= image_base && a < image_end ? a - image_base : 0; };
    std::sort(j.begin(), j.end(), [](const JobStats& a, const JobStats& b) { return a.total > b.total; });
    if (j.size() > 300) j.resize(300);
    std::string out = "{";
    json::key(out, "event");
    json::string(out, "job_timing");
    json::key(out, "t_ms");
    json::fixed(out, t_ms, 3);
    json::key(out, "ms");
    json::fixed(out, double(qpc_now() - start) * 1000.0 / double(qpc_frequency()), 1);
    json::key(out, "rounds");
    json::number(out, rounds);
    json::key(out, "skipped");
    json::number(out, skip);
    json::key(out, "overflow");
    json::number(out, over);
    json::key(out, "caller_columns");
    out += "[\"caller_rva\",\"rounds\",\"jobs\",\"duration\",\"work\",\"longest\",\"bound\",\"last_frac_milli\",\"last_in_tail\","
           "\"last_started_late\",\"threads_used\"]";
    json::key(out, "callers");
    out += '[';
    for (std::size_t i = 0; i < c.size(); ++i) {
        const auto& x = c[i];
        out += i ? ",[" : "[";
        for (const std::uint64_t v : {rva(x.caller), x.rounds, x.jobs, x.duration, x.work, x.longest, x.bound, x.last_frac_milli,
                                      x.last_in_tail, x.last_started_late, x.threads_used}) {
            if (out.back() != '[') out += ',';
            json::number(out, v);
        }
        out += ']';
    }
    out += ']';
    json::key(out, "job_columns");
    out += "[\"caller_rva\",\"function_rva\",\"vtable_rva\",\"count\",\"total\",\"max\",\"last\"]";
    json::key(out, "jobs");
    out += '[';
    for (std::size_t i = 0; i < j.size(); ++i) {
        const auto& x = j[i];
        out += i ? ",[" : "[";
        for (const std::uint64_t v : {rva(x.caller), rva(x.function), rva(x.vtable), x.count, x.total, x.max, x.last}) {
            if (out.back() != '[') out += ',';
            json::number(out, v);
        }
        out += ']';
    }
    out += "]}";
    return out;
}
std::string take_traces(std::uintptr_t image_base) {
    std::string out;
    if (!traces) return out;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image_base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image_base + static_cast<std::uintptr_t>(dos->e_lfanew));
    const auto image_end = image_base + nt->OptionalHeader.SizeOfImage;
    const auto rva = [&](std::uintptr_t a) -> std::uint64_t { return a >= image_base && a < image_end ? a - image_base : 0; };
    auto tail = trace_tail.load(std::memory_order_relaxed);
    const auto head = trace_head.load(std::memory_order_acquire);
    for (; tail != head; ++tail) {
        const auto& t = (*traces)[tail % trace_slots];
        std::array<std::uint32_t, 64> threads{};  // thread ids in order of first job
        unsigned thread_count{};
        const auto thread_number = [&](std::uint32_t id) -> unsigned {
            for (unsigned k = 0; k < thread_count; ++k)
                if (threads[k] == id) return k;
            if (thread_count == threads.size()) return 63;
            threads[thread_count] = id;
            return thread_count++;
        };
        std::string line = "{";
        json::key(line, "event");
        json::string(line, "job_trace");
        json::key(line, "caller_rva");
        json::number(line, rva(t.caller));
        json::key(line, "mode");
        json::number(line, std::uint64_t(t.mode));
        json::key(line, "participants");
        json::number(line, std::uint64_t(t.participants));
        json::key(line, "duration");
        json::number(line, t.end - t.start);
        json::key(line, "job_columns");
        line += "[\"vtable_rva\",\"function_rva\",\"start\",\"duration\",\"thread\"]";
        json::key(line, "jobs");
        line += '[';
        // Threads numbered in order of their first job start.
        std::vector<std::uint32_t> order(t.jobs);
        for (std::uint32_t i = 0; i < t.jobs; ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) { return t.list[a].start < t.list[b].start; });
        for (const auto i : order) thread_number(t.list[i].thread);
        for (std::uint32_t i = 0; i < t.jobs; ++i) {
            const auto& j = t.list[i];
            const bool ran = j.end && j.end >= j.start;
            line += i ? ",[" : "[";
            json::number(line, rva(j.vtable));
            line += ',';
            json::number(line, rva(j.function));
            line += ',';
            json::number(line, ran && j.start > t.start ? j.start - t.start : 0);
            line += ',';
            json::number(line, ran ? j.end - j.start : 0);
            line += ',';
            json::number(line, std::uint64_t(thread_number(j.thread)));
            line += ']';
        }
        line += ']';
        json::key(line, "game_thread");
        json::number(line, std::uint64_t(thread_number(t.game_thread)));
        json::key(line, "threads");
        json::number(line, std::uint64_t(thread_count));
        json::key(line, "dropped");
        json::number(line, traces_dropped.exchange(0, std::memory_order_relaxed));
        line += "}\n";
        out += line;
    }
    trace_tail.store(tail, std::memory_order_release);
    return out;
}
}  // namespace cpuopt::job_timing
