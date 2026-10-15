#include "job_timing.hpp"
#include "../core/json.hpp"
#include "log.hpp"
#include <windows.h>
#include <intrin.h>
#include <algorithm>
#include <array>
#include <atomic>
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
std::int64_t opened = qpc_now();

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
void set_measuring(bool on) noexcept { measuring.store(on); }

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
    if (!measuring.load(std::memory_order_relaxed) || mode < 0 || mode > 5) {
        original(smain, mode, jobs, count, extra);
        return;
    }
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
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
        original(smain, mode, jobs, count, extra);
        return;
    }
    wrap(list, n);
    const auto t0 = __rdtsc();
    original(smain, mode, jobs, count, extra);
    const auto t1 = __rdtsc();
    unwrap(list, n);
    merge(caller, analyse(records.data(), n, t0, t1, participants), n);
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
}  // namespace cpuopt::job_timing
