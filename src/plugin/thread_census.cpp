#include "thread_census.hpp"
#include "../core/json.hpp"
#include "log.hpp"
#include <intrin.h>
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace cpuopt {
namespace {
// x64 layouts of the NtQuerySystemInformation records used here.
struct ThreadRecord {  // SYSTEM_THREAD_INFORMATION
    LARGE_INTEGER kernel, user, create;
    ULONG wait_time;
    void* start;
    HANDLE process, thread;
    LONG priority, base_priority;
    ULONG switches, state, wait_reason;
};
static_assert(sizeof(ThreadRecord) == 0x50);
constexpr std::size_t process_record_bytes = 0x100;  // SYSTEM_PROCESS_INFORMATION; threads follow
constexpr std::size_t process_next = 0x0, process_thread_count = 0x4, process_id = 0x50;
struct ProcessorRecord {  // SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION
    LARGE_INTEGER idle, kernel, user, reserved[2];
    ULONG interrupts;
};
static_assert(sizeof(ProcessorRecord) == 48);
constexpr int system_process_information = 5, system_processor_performance_information = 8, thread_win32_start_address = 9;

using QuerySystem = LONG(NTAPI*)(int, void*, ULONG, ULONG*);
using QueryThread = LONG(NTAPI*)(HANDLE, int, void*, ULONG, ULONG*);
const auto ntdll = GetModuleHandleW(L"ntdll.dll");
const auto query_system = reinterpret_cast<QuerySystem>(GetProcAddress(ntdll, "NtQuerySystemInformation"));
const auto query_thread = reinterpret_cast<QueryThread>(GetProcAddress(ntdll, "NtQueryInformationThread"));

template <typename T>
T field(const std::byte* p, std::size_t offset) {
    T v;
    std::memcpy(&v, p + offset, sizeof(T));
    return v;
}

std::string address_label(std::uintptr_t address) {
    if (!address) return {};
    HMODULE module{};
    char text[320];
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &module)) {
        wchar_t path[MAX_PATH]{};
        const auto n = GetModuleFileNameW(module, path, MAX_PATH);
        std::wstring_view name(path, n);
        if (const auto slash = name.find_last_of(L"\\/"); slash != std::wstring_view::npos) name.remove_prefix(slash + 1);
        std::snprintf(text, sizeof(text), "%s+0x%llx", utf8(name).c_str(),
                      static_cast<unsigned long long>(address - reinterpret_cast<std::uintptr_t>(module)));
    } else {
        std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(address));
    }
    return text;
}
}  // namespace

ThreadCensus::ThreadCensus() {
    tsc_ = __rdtsc();
    qpc_ = qpc_now();
    QueryProcessCycleTime(GetCurrentProcess(), &process_cycles_);
}

void ThreadCensus::open_thread(DWORD tid, Thread& t) {
    t.handle = OpenThread(THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
    if (!t.handle) return;
    void* start{};
    if (query_thread && query_thread(t.handle, thread_win32_start_address, &start, sizeof(start), nullptr) >= 0)
        t.start = address_label(reinterpret_cast<std::uintptr_t>(start));
    PWSTR description{};
    if (SUCCEEDED(GetThreadDescription(t.handle, &description)) && description) {
        t.name = utf8(description);
        LocalFree(description);
    }
    QueryThreadCycleTime(t.handle, &t.cycles);  // the baseline for its first window
}

std::vector<DWORD> ThreadCensus::append_window(std::string& out, std::span<const DWORD> own) {
    const auto tsc = __rdtsc();
    const auto qpc = qpc_now();
    const double seconds = double(qpc - qpc_) / double(qpc_frequency());
    // QueryThreadCycleTime counts at the invariant TSC rate.
    const double tsc_hz = seconds > 0 ? double(tsc - tsc_) / seconds : 0;
    tsc_ = tsc;
    qpc_ = qpc;
    const auto cycles_ms = [&](std::uint64_t c) { return tsc_hz > 0 ? double(c) * 1000.0 / tsc_hz : 0.0; };

    // One process snapshot: the thread list, switches and priorities.
    ULONG needed{};
    LONG status = -1;
    for (int attempt = 0; attempt < 4 && query_system; ++attempt) {
        if (buffer_.size() < needed + 65536) buffer_.resize(needed + 65536);
        status = query_system(system_process_information, buffer_.data(), static_cast<ULONG>(buffer_.size()), &needed);
        if (status >= 0) break;
    }
    struct Row {
        DWORD tid;
        double ms;
        std::uint32_t switches;
        LONG priority;
        bool own;
        const Thread* thread;
    };
    std::vector<Row> rows;
    unsigned created{}, total{};
    for (auto& [tid, t] : threads_) t.seen = false;
    const auto pid = static_cast<std::uintptr_t>(GetCurrentProcessId());
    if (status >= 0) {
        for (std::size_t at = 0;;) {
            const auto* p = buffer_.data() + at;
            if (field<std::uintptr_t>(p, process_id) == pid) {
                const auto count = field<ULONG>(p, process_thread_count);
                for (ULONG i = 0; i < count; ++i) {
                    ThreadRecord r;
                    std::memcpy(&r, p + process_record_bytes + i * sizeof(ThreadRecord), sizeof(r));
                    const auto tid = static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(r.thread));
                    auto [it, inserted] = threads_.try_emplace(tid);
                    auto& t = it->second;
                    t.seen = true;
                    ++total;
                    if (inserted) {
                        ++created;
                        open_thread(tid, t);
                        t.switches = r.switches;
                        t.fresh = true;
                    }
                    std::uint64_t cycles{};
                    if (t.handle) QueryThreadCycleTime(t.handle, &cycles);
                    const auto spent = cycles >= t.cycles ? cycles - t.cycles : 0;
                    const auto switched = r.switches - t.switches;
                    t.cycles = cycles;
                    t.switches = r.switches;
                    const bool mine = std::find(own.begin(), own.end(), tid) != own.end();
                    rows.push_back({tid, cycles_ms(spent), switched, r.priority, mine, &t});
                }
                break;
            }
            const auto next = field<ULONG>(p, process_next);
            if (!next) break;
            at += next;
        }
    }
    unsigned exited{};
    for (auto it = threads_.begin(); it != threads_.end();) {
        if (it->second.seen) {
            ++it;
            continue;
        }
        if (it->second.handle) CloseHandle(it->second.handle);
        it = threads_.erase(it);
        ++exited;
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.ms > b.ms; });

    std::uint64_t process_cycles{};
    QueryProcessCycleTime(GetCurrentProcess(), &process_cycles);
    json::key(out, "tsc_mhz");
    json::fixed(out, tsc_hz / 1e6, 1);
    json::key(out, "process_cpu_ms");
    json::fixed(out, cycles_ms(process_cycles - process_cycles_), 2);
    process_cycles_ = process_cycles;
    json::key(out, "thread_count");
    json::number(out, std::uint64_t(total));
    json::key(out, "threads_created");
    json::number(out, std::uint64_t(created));
    json::key(out, "threads_exited");
    json::number(out, std::uint64_t(exited));
    // Threads that used at least 0.2% of one CPU, ours always. "new": first window.
    json::key(out, "thread_columns");
    out += "[\"tid\",\"cpu_ms\",\"switches\",\"priority\",\"start\",\"name\",\"flags\"]";
    json::key(out, "threads");
    out += '[';
    bool first = true;
    const double floor_ms = seconds * 1000.0 * 0.002;
    std::vector<DWORD> busiest;
    for (const auto& r : rows) {
        if (!r.own && !r.thread->fresh && busiest.size() < 16) busiest.push_back(r.tid);
        if (r.ms < floor_ms && !r.own) continue;
        out += first ? "[" : ",[";
        first = false;
        json::number(out, std::uint64_t(r.tid));
        out += ',';
        json::fixed(out, r.ms, 3);
        out += ',';
        json::number(out, std::uint64_t(r.switches));
        out += ',';
        json::number(out, std::int64_t(r.priority));
        out += ',';
        json::string(out, r.thread->start);
        out += ',';
        json::string(out, r.thread->name);
        out += ',';
        json::string(out, r.own ? (r.thread->fresh ? "own,new" : "own") : (r.thread->fresh ? "new" : ""));
        out += ']';
    }
    out += ']';
    for (auto& [tid, t] : threads_) t.fresh = false;

    // Busy share per logical processor in group 0, all processes.
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    std::vector<ProcessorRecord> cpu(si.dwNumberOfProcessors);
    json::key(out, "cpu_busy");
    out += '[';
    if (query_system && query_system(system_processor_performance_information, cpu.data(),
                                     static_cast<ULONG>(cpu.size() * sizeof(ProcessorRecord)), nullptr) >= 0) {
        if (processors_.size() != cpu.size()) processors_.assign(cpu.size(), {});
        for (std::size_t i = 0; i < cpu.size(); ++i) {
            const auto idle = static_cast<std::uint64_t>(cpu[i].idle.QuadPart);
            const auto all = static_cast<std::uint64_t>(cpu[i].kernel.QuadPart + cpu[i].user.QuadPart);  // kernel includes idle
            const auto d_idle = idle - processors_[i].idle, d_all = all - processors_[i].total;
            processors_[i] = {idle, all};
            if (i) out += ',';
            json::fixed(out, d_all ? 100.0 * (1.0 - double(d_idle) / double(d_all)) : 0.0, 1);
        }
    }
    out += ']';
    return busiest;
}
}  // namespace cpuopt
