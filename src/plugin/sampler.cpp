#include "sampler.hpp"
#include "../core/json.hpp"
#include "../core/toggles.hpp"
#include "log.hpp"
#include "spin_probe.hpp"
#include <psapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace cpuopt::sampler {
namespace {
constexpr unsigned module_capacity = 512, name_capacity = 512, thread_capacity = 1024;
constexpr unsigned depth_capacity = 48, stack_capacity = 1U << 14, tracked_capacity = 6, rotation = 2;
constexpr unsigned thread_stat_capacity = 256, unknown_module = 0xFFFF;
constexpr std::uint32_t raw_address = 0x80000000U;  // frame RVA without an unwind entry: instruction address

struct Module {
    std::uintptr_t base{}, end{};
    const RUNTIME_FUNCTION* table{};
    unsigned count{}, id{};
};
struct Thread {
    DWORD id{};
    HANDLE handle{};
    std::uintptr_t teb{};
};
struct Stack {
    std::uint64_t hash{};
    std::uint32_t tid{}, count{};
    std::uint16_t depth{}, truncated{};
    std::array<std::uint64_t, depth_capacity> frames;  // module id << 32 | rva
};
struct ThreadStat {
    DWORD tid{};
    std::uint32_t samples{}, failed{};
};
// Spin probe readings: which job functions were still running, per job round
// (the frame that called executeJob), while the game thread spun.
struct SpinJob {
    std::uint64_t caller{};  // frame id of executeJob's caller
    std::uint64_t function{};  // job function (absolute address)
    std::uint32_t count{};
};
constexpr unsigned spin_job_capacity = 512;
struct Table {
    std::array<Stack, stack_capacity> stacks;
    std::array<ThreadStat, thread_stat_capacity> threads;
    unsigned used{}, thread_count{};
    std::uint64_t samples{}, overflow{}, ticks{};
    std::int64_t opened{};  // qpc when the table started collecting
    std::uint64_t spin_samples{}, spin_unreadable{}, spin_overflow{};
    std::array<std::uint32_t, spin_probe::max_slots + 1> spin_busy{};  // samples by number of busy slots
    std::array<SpinJob, spin_job_capacity> spin_jobs{};
};

// The spin probe's code range and sMain's global (job_cap.hpp); set once.
std::atomic<std::uintptr_t> spin_begin{}, spin_end{}, smain_global{};

// Sampler thread only, apart from names (lock) and the tables (lock).
std::array<Module, module_capacity> modules;
unsigned module_count{};
std::array<std::array<char, 64>, name_capacity> names{};
unsigned name_count{};
std::array<Thread, thread_capacity> threads;
unsigned thread_count{}, next_thread{};
SRWLOCK lock = SRWLOCK_INIT;
std::unique_ptr<Table> tables[2];
unsigned active{};
std::atomic<DWORD> sampler_thread{};
std::atomic<bool> started{}, stopping{};
HANDLE run_thread{};
// Set by the window thread, read by the sampler at each tick.
std::array<std::atomic<DWORD>, tracked_capacity> tracked{};
std::array<std::atomic<DWORD>, 8> excluded{};

using QueryThread = LONG(NTAPI*)(HANDLE, int, void*, ULONG, ULONG*);
QueryThread query_thread{};
struct BasicInformation {
    LONG exit_status;
    void* teb;
    void* client[2];
    ULONG_PTR affinity;
    LONG priority, base_priority;
};

unsigned name_id(const wchar_t* name) noexcept {
    std::array<char, 64> narrow{};
    for (unsigned i = 0; name[i] && i + 1 < narrow.size(); ++i) narrow[i] = name[i] < 128 ? static_cast<char>(name[i]) : '?';
    for (unsigned i = 0; i < name_count; ++i)
        if (names[i] == narrow) return i;
    if (name_count == name_capacity) return unknown_module;
    AcquireSRWLockExclusive(&lock);
    names[name_count] = narrow;
    const auto id = name_count++;
    ReleaseSRWLockExclusive(&lock);
    return id;
}
bool exception_table(std::uintptr_t base, const RUNTIME_FUNCTION*& table, unsigned& count) noexcept {
    __try {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + static_cast<std::uintptr_t>(dos->e_lfanew));
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
        const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        table = dir.VirtualAddress ? reinterpret_cast<const RUNTIME_FUNCTION*>(base + dir.VirtualAddress) : nullptr;
        count = table ? dir.Size / sizeof(RUNTIME_FUNCTION) : 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void refresh_modules() noexcept {
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    unsigned n{};
    for (BOOL ok = Module32FirstW(snapshot, &entry); ok && n < module_capacity; ok = Module32NextW(snapshot, &entry)) {
        Module m;
        m.base = reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);
        m.end = m.base + entry.modBaseSize;
        if (!exception_table(m.base, m.table, m.count)) continue;
        m.id = name_id(entry.szModule);
        modules[n++] = m;
    }
    CloseHandle(snapshot);
    std::sort(modules.begin(), modules.begin() + n, [](const Module& a, const Module& b) { return a.base < b.base; });
    module_count = n;
}
bool skipped(DWORD id) noexcept {
    for (const auto& e : excluded)
        if (e.load(std::memory_order_relaxed) == id) return true;
    return id == sampler_thread.load(std::memory_order_relaxed);
}
void refresh_threads() noexcept {
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    std::array<DWORD, thread_capacity> live{};
    unsigned n{};
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    const auto pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snapshot, &entry); ok && n < thread_capacity; ok = Thread32Next(snapshot, &entry))
        if (entry.th32OwnerProcessID == pid && !skipped(entry.th32ThreadID)) live[n++] = entry.th32ThreadID;
    CloseHandle(snapshot);
    unsigned kept{};
    for (unsigned i = 0; i < thread_count; ++i) {
        if (std::find(live.begin(), live.begin() + n, threads[i].id) != live.begin() + n) threads[kept++] = threads[i];
        else CloseHandle(threads[i].handle);
    }
    thread_count = kept;
    for (unsigned i = 0; i < n && thread_count < thread_capacity; ++i) {
        bool known = false;
        for (unsigned j = 0; j < kept && !known; ++j) known = threads[j].id == live[i];
        if (known) continue;
        const auto handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, live[i]);
        if (!handle) continue;
        BasicInformation info{};
        if (!query_thread || query_thread(handle, 0, &info, sizeof(info), nullptr) < 0 || !info.teb) {
            CloseHandle(handle);
            continue;
        }
        threads[thread_count++] = {live[i], handle, reinterpret_cast<std::uintptr_t>(info.teb)};
    }
}
const Module* find(std::uintptr_t pc) noexcept {
    const auto end = modules.begin() + module_count;
    auto it = std::upper_bound(modules.begin(), end, pc, [](std::uintptr_t v, const Module& m) { return v < m.base; });
    if (it == modules.begin()) return nullptr;
    --it;
    return pc < it->end ? &*it : nullptr;
}
const RUNTIME_FUNCTION* lookup(const Module& m, std::uintptr_t pc) noexcept {
    if (!m.table) return nullptr;
    const auto rva = static_cast<DWORD>(pc - m.base);
    unsigned lo = 0, hi = m.count;
    while (lo < hi) {
        const auto mid = (lo + hi) / 2;
        const auto& f = m.table[mid];
        if (rva < f.BeginAddress) hi = mid;
        else if (rva >= f.EndAddress) lo = mid + 1;
        else return &f;
    }
    return nullptr;
}
// The function a frame belongs to: module id and function start, or the
// instruction RVA flagged raw_address when there is no unwind entry.
std::uint64_t frame_of(std::uintptr_t pc, bool leaf) noexcept {
    const auto* m = find(pc);
    if (!m) return std::uint64_t(unknown_module) << 32;
    const auto* f = lookup(*m, leaf ? pc : pc - 1);
    const auto rva = f ? f->BeginAddress : (static_cast<std::uint32_t>(pc - m->base) | raw_address);
    return std::uint64_t(m->id) << 32 | rva;
}
// Runs while the thread is suspended: reads its stack inside the TEB bounds,
// no allocation, no locks.
unsigned walk(CONTEXT& c, std::uintptr_t teb, std::array<std::uintptr_t, depth_capacity>& pcs, bool& cut) noexcept {
    unsigned n{};
    __try {
        const auto stack_base = *reinterpret_cast<const std::uintptr_t*>(teb + 8);
        const auto stack_limit = *reinterpret_cast<const std::uintptr_t*>(teb + 16);
        while (n < depth_capacity) {
            const auto pc = static_cast<std::uintptr_t>(c.Rip), sp = static_cast<std::uintptr_t>(c.Rsp);
            if (!pc || sp < stack_limit || sp + 8 > stack_base) break;
            pcs[n++] = pc;
            const auto* m = find(pc);
            if (!m) break;
            // A return address may follow a call that ends its function.
            const auto* f = lookup(*m, n == 1 ? pc : pc - 1);
            if (!f) {  // leaf function: the return address is on top of the stack
                c.Rip = *reinterpret_cast<const DWORD64*>(sp);
                c.Rsp += 8;
            } else {
                void* data{};
                DWORD64 frame{};
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, m->base, pc, const_cast<PRUNTIME_FUNCTION>(f), &c, &data, &frame, nullptr);
            }
            if (c.Rsp <= sp) break;
        }
        if (n == depth_capacity) cut = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        cut = true;
    }
    return n;
}
ThreadStat* stat_of(Table& t, DWORD tid) noexcept {
    for (unsigned i = 0; i < t.thread_count; ++i)
        if (t.threads[i].tid == tid) return &t.threads[i];
    if (t.thread_count == thread_stat_capacity) return nullptr;
    auto& s = t.threads[t.thread_count++];
    s = {tid, 0, 0};
    return &s;
}
void record(DWORD tid, const std::uint64_t* frames, unsigned depth, bool cut) noexcept {
    std::uint64_t hash = 0xcbf29ce484222325ULL ^ tid;
    for (unsigned i = 0; i < depth; ++i) hash = (hash ^ frames[i]) * 0x100000001b3ULL;
    AcquireSRWLockExclusive(&lock);
    auto& t = *tables[active];
    ++t.samples;
    if (auto* s = stat_of(t, tid)) ++s->samples;
    auto slot = static_cast<unsigned>(hash >> 32) & (stack_capacity - 1);
    bool stored = false;
    for (unsigned probe = 0; probe < 64 && !stored; ++probe, slot = (slot + 1) & (stack_capacity - 1)) {
        auto& s = t.stacks[slot];
        if (s.count == 0) {
            if (t.used >= stack_capacity * 3 / 4) break;  // keep probes short
            s.hash = hash;
            s.tid = tid;
            s.count = 1;
            s.depth = static_cast<std::uint16_t>(depth);
            s.truncated = cut ? 1 : 0;
            std::copy(frames, frames + depth, s.frames.begin());
            ++t.used;
            stored = true;
        } else if (s.hash == hash && s.tid == tid && s.depth == depth && std::equal(frames, frames + depth, s.frames.begin())) {
            ++s.count;
            stored = true;
        }
    }
    if (!stored) ++t.overflow;
    ReleaseSRWLockExclusive(&lock);
}
void failed(DWORD tid) noexcept {
    AcquireSRWLockExclusive(&lock);
    if (auto* s = stat_of(*tables[active], tid)) ++s->failed;
    ReleaseSRWLockExclusive(&lock);
}
void probe(std::uint64_t caller) noexcept {
    std::uintptr_t smain{};
    __try {
        smain = *reinterpret_cast<volatile const std::uintptr_t*>(smain_global.load(std::memory_order_relaxed));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        smain = 0;
    }
    spin_probe::Reading r;
    const bool ok = spin_probe::read(smain, r);
    AcquireSRWLockExclusive(&lock);
    auto& t = *tables[active];
    ++t.spin_samples;
    if (!ok) {
        ++t.spin_unreadable;
    } else {
        ++t.spin_busy[r.busy];
        for (unsigned i = 0; i < r.busy; ++i) {
            bool stored = false;
            for (auto& j : t.spin_jobs) {
                if (j.count && (j.caller != caller || j.function != r.functions[i])) continue;
                j.caller = caller;
                j.function = r.functions[i];
                ++j.count;
                stored = true;
                break;
            }
            if (!stored) ++t.spin_overflow;
        }
    }
    ReleaseSRWLockExclusive(&lock);
}
void sample(const Thread& t) noexcept {
    std::array<std::uintptr_t, depth_capacity> pcs;
    CONTEXT c{};
    c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (SuspendThread(t.handle) == static_cast<DWORD>(-1)) {
        failed(t.id);
        return;
    }
    bool cut = false;
    unsigned n{};
    if (GetThreadContext(t.handle, &c)) n = walk(c, t.teb, pcs, cut);
    ResumeThread(t.handle);
    if (!n) {
        failed(t.id);
        return;
    }
    std::array<std::uint64_t, depth_capacity> frames;
    for (unsigned i = 0; i < n; ++i) frames[i] = frame_of(pcs[i], i == 0);
    record(t.id, frames.data(), n, cut);
    // Caught spinning in executeJob: which jobs are the others still running?
    if (pcs[0] >= spin_begin.load(std::memory_order_relaxed) && pcs[0] < spin_end.load(std::memory_order_relaxed))
        probe(n > 1 ? frames[1] : 0);
}
bool is_tracked(DWORD id) noexcept {
    for (const auto& t : tracked)
        if (t.load(std::memory_order_relaxed) == id) return true;
    return false;
}
DWORD WINAPI run(void*) noexcept {
    sampler_thread = GetCurrentThreadId();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    query_thread = reinterpret_cast<QueryThread>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
    const auto timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    std::uint64_t refreshed{};
    bool was_on = false;
    while (!stopping.load(std::memory_order_relaxed)) {
        if (!on(sample_profile)) {
            was_on = false;
            Sleep(50);
            continue;
        }
        const auto now = GetTickCount64();
        if (!was_on || now - refreshed >= 2000) {
            refresh_modules();
            refresh_threads();
            refreshed = now;
            was_on = true;
        }
        for (unsigned i = 0; i < thread_count; ++i)
            if (is_tracked(threads[i].id)) sample(threads[i]);
        for (unsigned k = 0, tries = 0; k < rotation && thread_count && tries < thread_count; ++tries) {
            const auto& t = threads[next_thread++ % thread_count];
            if (is_tracked(t.id)) continue;
            sample(t);
            ++k;
        }
        AcquireSRWLockExclusive(&lock);
        ++tables[active]->ticks;
        ReleaseSRWLockExclusive(&lock);
        if (timer) {
            LARGE_INTEGER due{};
            due.QuadPart = -10000;  // 1 ms
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, 20);
        } else {
            Sleep(1);
        }
    }
    for (unsigned i = 0; i < thread_count; ++i) CloseHandle(threads[i].handle);
    thread_count = 0;
    if (timer) CloseHandle(timer);
    return 0;
}
}  // namespace

void start() noexcept {
    if (started.exchange(true)) return;
    try {
        tables[0] = std::make_unique<Table>();
        tables[1] = std::make_unique<Table>();
    } catch (...) {
        return;
    }
    tables[0]->opened = qpc_now();
    run_thread = CreateThread(nullptr, 0, run, nullptr, 0, nullptr);
}

bool stop(unsigned timeout_ms) noexcept {
    stopping.store(true);
    return !run_thread || WaitForSingleObject(run_thread, timeout_ms) == WAIT_OBJECT_0;
}

DWORD thread_id() noexcept { return sampler_thread.load(); }

void set_tracked(std::span<const DWORD> list) noexcept {
    for (unsigned i = 0; i < tracked_capacity; ++i) tracked[i].store(i < list.size() ? list[i] : 0, std::memory_order_relaxed);
}
void set_spin_probe(std::uintptr_t begin, std::uintptr_t end, std::uintptr_t global) noexcept {
    smain_global.store(global);
    spin_end.store(end);
    spin_begin.store(begin);
}

void set_excluded(std::span<const DWORD> list) noexcept {
    for (unsigned i = 0; i < excluded.size(); ++i) excluded[i].store(i < list.size() ? list[i] : 0, std::memory_order_relaxed);
}

std::string take_event(double t_ms, unsigned toggle_state, const std::string& marker) {
    if (!tables[0]) return {};
    AcquireSRWLockExclusive(&lock);
    auto& t = *tables[active];
    active ^= 1;
    tables[active]->opened = qpc_now();
    std::vector<std::array<char, 64>> known(names.begin(), names.begin() + name_count);
    ReleaseSRWLockExclusive(&lock);
    std::string out;
    if (t.samples) {
        const double ms = double(qpc_now() - t.opened) * 1000.0 / double(qpc_frequency());
        out = "{";
        json::key(out, "event");
        json::string(out, "profile");
        json::key(out, "t_ms");
        json::fixed(out, t_ms, 3);
        json::key(out, "ms");
        json::fixed(out, ms, 1);
        json::key(out, "toggles");
        json::number(out, std::uint64_t(toggle_state));
        json::key(out, "marker");
        json::string(out, marker);
        json::key(out, "ticks");
        json::number(out, t.ticks);
        json::key(out, "samples");
        json::number(out, t.samples);
        json::key(out, "overflow");
        json::number(out, t.overflow);
        // The spin probe: busy-slot counts, and running job functions per
        // executeJob caller ([caller frame index, job function RVA or absolute
        // address when outside the exe, samples]).
        const auto exe = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        json::key(out, "spin_probe");
        out += '{';
        json::key(out, "samples");
        json::number(out, t.spin_samples);
        json::key(out, "unreadable");
        json::number(out, t.spin_unreadable);
        json::key(out, "overflow");
        json::number(out, t.spin_overflow);
        json::key(out, "busy");
        out += '[';
        for (unsigned i = 0; i < t.spin_busy.size(); ++i) {
            if (i) out += ',';
            json::number(out, std::uint64_t(t.spin_busy[i]));
        }
        out += ']';
        out += '}';
        json::key(out, "tracked");
        out += '[';
        for (unsigned i = 0; i < tracked_capacity; ++i) {
            if (i) out += ',';
            json::number(out, std::uint64_t(tracked[i].load()));
        }
        out += ']';
        json::key(out, "modules");
        out += '[';
        for (std::size_t i = 0; i < known.size(); ++i) {
            if (i) out += ',';
            json::string(out, known[i].data());
        }
        out += ']';
        json::key(out, "threads");
        out += '[';
        for (unsigned i = 0; i < t.thread_count; ++i) {
            out += i ? ",[" : "[";
            json::number(out, std::uint64_t(t.threads[i].tid));
            out += ',';
            json::number(out, std::uint64_t(t.threads[i].samples));
            out += ',';
            json::number(out, std::uint64_t(t.threads[i].failed));
            out += ']';
        }
        out += ']';
        // Frames are listed once ([module, rva]); stacks refer to them by index.
        std::unordered_map<std::uint64_t, std::uint32_t> index;
        std::string stacks;
        std::string frames;
        bool first = true;
        for (const auto& s : t.stacks) {
            if (!s.count) continue;
            stacks += first ? "[" : ",[";
            first = false;
            json::number(stacks, std::uint64_t(s.tid));
            stacks += ',';
            json::number(stacks, std::uint64_t(s.count));
            stacks += ',';
            json::number(stacks, std::uint64_t(s.truncated));
            stacks += ",[";
            for (unsigned i = 0; i < s.depth; ++i) {
                const auto [it, inserted] = index.try_emplace(s.frames[i], static_cast<std::uint32_t>(index.size()));
                if (inserted) {
                    frames += index.size() == 1 ? "[" : ",[";
                    json::number(frames, s.frames[i] >> 32);
                    frames += ',';
                    json::number(frames, s.frames[i] & 0xFFFFFFFFULL);
                    frames += ']';
                }
                if (i) stacks += ',';
                json::number(stacks, std::uint64_t(it->second));
            }
            stacks += "]]";
        }
        // Spin probe jobs: their callers join the frame list.
        std::string spin_jobs;
        MODULEINFO image{};
        GetModuleInformation(GetCurrentProcess(), GetModuleHandleW(nullptr), &image, sizeof(image));
        const auto exe_end = exe + image.SizeOfImage;
        for (const auto& j : t.spin_jobs) {
            if (!j.count) continue;
            const auto [it, inserted] = index.try_emplace(j.caller, static_cast<std::uint32_t>(index.size()));
            if (inserted) {
                frames += index.size() == 1 ? "[" : ",[";
                json::number(frames, j.caller >> 32);
                frames += ',';
                json::number(frames, j.caller & 0xFFFFFFFFULL);
                frames += ']';
            }
            const bool in_exe = j.function >= exe && j.function < exe_end;
            spin_jobs += spin_jobs.empty() ? "[" : ",[";
            json::number(spin_jobs, std::uint64_t(it->second));
            spin_jobs += in_exe ? ",1," : ",0,";
            json::number(spin_jobs, std::uint64_t(in_exe ? j.function - exe : j.function));
            spin_jobs += ',';
            json::number(spin_jobs, std::uint64_t(j.count));
            spin_jobs += ']';
        }
        json::key(out, "spin_job_columns");
        out += "[\"caller_frame\",\"in_exe\",\"function\",\"samples\"]";
        json::key(out, "spin_jobs");
        out += '[';
        out += spin_jobs;
        out += ']';
        json::key(out, "frames");
        out += '[';
        out += frames;
        out += ']';
        json::key(out, "stack_columns");
        out += "[\"tid\",\"count\",\"truncated\",\"frames\"]";
        json::key(out, "stacks");
        out += '[';
        out += stacks;
        out += "]}";
    }
    // The drained table is reused; only the sampler fills the active one.
    for (auto& s : t.stacks) s.count = 0;
    t.used = t.thread_count = 0;
    t.samples = t.overflow = t.ticks = 0;
    t.spin_samples = t.spin_unreadable = t.spin_overflow = 0;
    t.spin_busy = {};
    for (auto& j : t.spin_jobs) j = {};
    return out;
}
}  // namespace cpuopt::sampler
