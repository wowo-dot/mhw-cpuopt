#pragma once
#include <windows.h>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace cpuopt {
// Per-window CPU accounting without touching the threads: cycles per thread
// (QueryThreadCycleTime), context switches and priority (one
// NtQuerySystemInformation process snapshot), and how busy each logical
// processor was (all processes). Window-thread only.
class ThreadCensus {
public:
    ThreadCensus();
    // Appends "threads", "cpus" and related keys to an open JSON object and
    // returns the busiest game threads (not ours), most cycles first.
    std::vector<DWORD> append_window(std::string& out, std::span<const DWORD> own_threads);

private:
    struct Thread {
        HANDLE handle{};
        std::uint64_t cycles{};
        std::uint32_t switches{};
        std::string start, name;
        bool seen{};
        bool fresh{true};
    };
    struct Processor {
        std::uint64_t idle{}, total{};
    };
    void open_thread(DWORD tid, Thread& t);
    std::unordered_map<DWORD, Thread> threads_;
    std::vector<Processor> processors_;
    std::vector<std::byte> buffer_;
    std::uint64_t tsc_{}, process_cycles_{};
    std::int64_t qpc_{};
};
}  // namespace cpuopt
