#include "frame_budget.hpp"
#include "../core/histogram.hpp"
#include "../core/json.hpp"
#include "limiter.hpp"
#include "log.hpp"
#include <intrin.h>
#include <windows.h>
#include <atomic>

namespace cpuopt::frame_budget {
namespace {
Timing timing_fn{};
Present present_fn{};
std::atomic<bool> on{};

// The frame in progress (game thread only): cycles, and the swapchain's QPC.
std::uint64_t limiter_cycles{}, rounds_cycles{}, present_cycles{};
std::int64_t swapchain_qpc{}, begin_qpc{}, end_qpc{}, last_begin_qpc{};
bool open{};  // a frame began while measuring

// Per frame, in us.
enum Part : unsigned { interval, update, limiter, rounds, present_path, swapchain, gpu_wait, serial, cpu_frame, parts };
constexpr const char* part_names[parts] = {"interval", "update", "limiter", "rounds", "present_path", "swapchain", "gpu_wait", "serial", "cpu_frame"};
std::array<DurationHistogram, parts> histograms;

const std::uint64_t calibration_tsc = __rdtsc();
const std::int64_t calibration_qpc = qpc_now();
double tsc_per_us() noexcept {
    const auto seconds = double(qpc_now() - calibration_qpc) / double(qpc_frequency());
    return seconds > 0.01 ? double(__rdtsc() - calibration_tsc) / seconds / 1e6 : 4000.0;
}
double qpc_per_us() noexcept { return double(qpc_frequency()) / 1e6; }
std::uint64_t clamp_us(double us) noexcept { return us > 0 ? static_cast<std::uint64_t>(us + 0.5) : 0; }

// The finished frame (BEGIN of the previous one to this BEGIN) into the histograms.
void close_frame(std::int64_t next_begin) noexcept {
    const double per_us = tsc_per_us(), q = qpc_per_us();
    const double interval_us = double(next_begin - last_begin_qpc) / q;
    const double update_us = end_qpc > last_begin_qpc ? double(end_qpc - last_begin_qpc) / q : 0;
    const double limiter_us = double(limiter_cycles) / per_us, rounds_us = double(rounds_cycles) / per_us;
    const double present_us = double(present_cycles) / per_us, swapchain_us = double(swapchain_qpc) / q;
    const double wait_us = present_us > swapchain_us ? present_us - swapchain_us : 0;
    const double serial_us = update_us - limiter_us - rounds_us - present_us;
    const double cpu_us = interval_us - limiter_us - wait_us;
    const double values[parts] = {interval_us, update_us, limiter_us, rounds_us, present_us, swapchain_us, wait_us, serial_us, cpu_us};
    for (unsigned i = 0; i < parts; ++i) histograms[i].add(clamp_us(values[i]));
}
}  // namespace

Discovery discover(const std::uintptr_t* timing, unsigned timing_count, const std::uintptr_t* present, unsigned present_count) noexcept {
    Discovery d;
    d.counts[0] = timing_count;
    d.counts[1] = present_count;
    if (!timing_count || !present_count) {
        d.reason = "signature not found";
        return d;
    }
    if (timing_count != 1 || present_count != 1) {
        d.reason = "signature not unique";
        return d;
    }
    d.found = true;
    d.reason = "ok";
    d.timing = timing[0];
    d.present = present[0];
    return d;
}

std::string discovery_event(const Discovery& d, std::uintptr_t base, double t_ms, std::string_view scanner) {
    std::string out = "{";
    json::key(out, "event");
    json::string(out, "frame_budget");
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
    json::number(out, std::uint64_t(d.counts[0]));
    out += ',';
    json::number(out, std::uint64_t(d.counts[1]));
    out += ']';
    json::key(out, "timing_rva");
    json::number(out, std::uint64_t(d.timing ? d.timing - base : 0));
    json::key(out, "present_rva");
    json::number(out, std::uint64_t(d.present ? d.present - base : 0));
    out += '}';
    return out;
}

Timing* timing_next() noexcept { return &timing_fn; }
Present* present_next() noexcept { return &present_fn; }

std::uint64_t __fastcall timing_detour(std::uintptr_t smain) {
    const bool measure = on.load(std::memory_order_relaxed);
    const auto t0 = measure ? __rdtsc() : 0;
    limiter::before(smain);  // toggle sleep_limiter's wait: part of the limiter's time
    const auto r = timing_fn(smain);
    if (measure) limiter_cycles += __rdtsc() - t0;
    return r;
}

std::uint64_t __fastcall present_detour(std::uintptr_t render, std::uint32_t a2, std::uint8_t a3) {
    if (!on.load(std::memory_order_relaxed)) return present_fn(render, a2, a3);
    const auto t0 = __rdtsc();
    const auto r = present_fn(render, a2, a3);
    present_cycles += __rdtsc() - t0;
    return r;
}

void add_round(std::uint64_t cycles) noexcept {
    if (on.load(std::memory_order_relaxed)) rounds_cycles += cycles;
}
void add_swapchain(std::int64_t ticks) noexcept {
    if (on.load(std::memory_order_relaxed)) swapchain_qpc += ticks;
}

void on_begin(std::int64_t qpc) noexcept {
    if (!on.load(std::memory_order_relaxed)) {
        open = false;
        return;
    }
    if (open && end_qpc) close_frame(qpc);
    open = true;
    last_begin_qpc = qpc;
    end_qpc = 0;
    limiter_cycles = rounds_cycles = present_cycles = 0;
    swapchain_qpc = 0;
}
void on_end(std::int64_t qpc) noexcept {
    if (open) end_qpc = qpc;
}

void set_active(bool enable) noexcept { on.store(enable); }
bool active() noexcept { return on.load(std::memory_order_relaxed); }

Totals take_totals() noexcept {
    Totals t;
    for (unsigned i = 0; i < parts; ++i) {
        const auto s = histograms[i].take();
        if (i != cpu_frame) continue;
        t.frames = s.count;
        t.cpu_frame_ms = s.count ? double(s.total_us) / double(s.count) / 1000.0 : 0.0;
    }
    return t;
}

void append(std::string& out) {
    json::key(out, "budget");
    out += '{';
    json::key(out, "active");
    out += on.load(std::memory_order_relaxed) ? "true" : "false";
    bool first_count = true;
    for (unsigned i = 0; i < parts; ++i) {
        const auto s = histograms[i].take();
        if (first_count) {
            json::key(out, "frames");
            json::number(out, s.count);
            first_count = false;
        }
        json::key(out, part_names[i]);
        out += '{';
        json::key(out, "mean");
        json::fixed(out, s.count ? double(s.total_us) / double(s.count) / 1000.0 : 0.0, 4);
        json::key(out, "p50");
        json::fixed(out, double(s.p50_us) / 1000.0, 3);
        json::key(out, "p95");
        json::fixed(out, double(s.p95_us) / 1000.0, 3);
        json::key(out, "p99");
        json::fixed(out, double(s.p99_us) / 1000.0, 3);
        out += '}';
    }
    out += '}';
}
}  // namespace cpuopt::frame_budget
