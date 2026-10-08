#pragma once
#include "../core/histogram.hpp"
#include <windows.h>
#include <cstdint>
#include <string>

namespace cpuopt::frame_clock {
// The engine's 64-bit frame counter, found without hooks. The signature is
// the renderer's frame-parity read (mov rax,[rip+renderer]; mov r9,1/3
// magic; mov r8,[rax+counter]); the singleton RVA and the counter offset are
// taken from its operands. Tested build: site 0x23922C2, singleton 0x51C4480,
// counter +0x1F210 (incremented at 0x22932DB: inc qword [rbx+1F210h]).
struct Discovery {
    bool found{};
    unsigned matches{};
    std::uint32_t site_rva{}, singleton_rva{}, counter_offset{};
    bool expected_values{};  // equal to the tested build's values
    const char* reason{"not searched"};
};
Discovery discover(std::uintptr_t image_base) noexcept;
// `final`: found, or the last attempt.
std::string discovery_event(const Discovery&, double t_ms, unsigned attempt, bool final);

// A poller thread reads the counter every 500 us (high-resolution waitable
// timer) and times each change. Frame counts per window are exact; single
// intervals carry the poll latency (about 0.5 ms, reported per window).
void start(std::uintptr_t image_base, const Discovery&) noexcept;
DWORD thread_id() noexcept;

struct Window {
    bool renderer{};                       // the renderer existed at the end of the window
    std::uint64_t counter{}, frames{};     // counter at the end; counter advance in the window
    std::uint64_t polls{}, multi_step{};   // polls; changes of more than one frame between polls
    std::uint64_t max_poll_gap_us{}, poll_gap_total_us{};
    IntervalHistogram::Summary intervals;  // observed single-frame intervals
};
Window take() noexcept;
}  // namespace cpuopt::frame_clock
