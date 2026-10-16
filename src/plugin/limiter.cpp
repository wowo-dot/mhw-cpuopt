#include "limiter.hpp"
#include "../core/histogram.hpp"
#include "../core/json.hpp"
#include "log.hpp"  // qpc_now, qpc_frequency
#include <windows.h>
#include <atomic>
#include <cmath>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace cpuopt::limiter {
namespace {
enum State : int { off, ready, no_timer, odd_fields, fixed_step };
constexpr const char* state_names[] = {"off", "waiting", "no high-resolution timer", "fields out of range", "fixed-step branch"};
std::atomic<bool> on{};
std::atomic<unsigned> margin{500};
std::atomic<int> state{off};
std::atomic<std::uint64_t> frames{}, waits{}, past_margin{};
std::atomic<unsigned long> timer_error{};
DurationHistogram wait_us, late_us;
HANDLE timer{};      // game thread only; one handle for the process's life
bool timer_failed{};

template <typename T>
bool read(std::uintptr_t at, T& value) noexcept {
    __try {
        value = *reinterpret_cast<volatile const T*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
}  // namespace

bool remaining(std::uintptr_t smain, std::int64_t now, std::int64_t frequency, double& seconds) noexcept {
    float scale{}, max_fps{};
    std::uint8_t mode{};
    std::uint64_t scaled{}, reference{};
    std::int64_t last{};
    if (!read(smain + scale_offset, scale) || !read(smain + max_fps_offset, max_fps) || !read(smain + mode_offset, mode) ||
        !read(smain + scaled_offset, scaled) || !read(smain + reference_offset, reference) || !read(smain + last_qpc_offset, last))
        return false;
    if (mode || !(scale >= 1.0f && scale <= 1000.0f) || !(max_fps >= 10.0f && max_fps <= 1000.0f) || frequency <= 0) return false;
    if (last == 0 || last > now) last = now;  // as the game does
    // The game: sum += (int)((now - last) * rate); elapsed = (int)sum - (int)reference,
    // in ticks times the rate; it waits while elapsed / frequency < rate / max_fps.
    const auto sum = scaled + static_cast<std::uint32_t>(static_cast<std::int32_t>(double(static_cast<std::int32_t>(now - last)) * scale));
    const auto elapsed = static_cast<std::int32_t>(static_cast<std::uint32_t>(sum) - static_cast<std::uint32_t>(reference));
    seconds = (double(scale) / max_fps - double(elapsed) / double(frequency)) / scale;
    return std::isfinite(seconds);
}

void set_active(bool enable, unsigned margin_us) noexcept {
    margin.store(margin_us);
    on.store(enable);
    if (!enable) state.store(off);
}
bool active() noexcept { return on.load(std::memory_order_relaxed); }

void before(std::uintptr_t smain) noexcept {
    if (!on.load(std::memory_order_relaxed)) return;
    frames.fetch_add(1, std::memory_order_relaxed);
    if (!timer && !timer_failed) {
        timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        timer_failed = !timer;
        if (timer_failed) timer_error.store(GetLastError());  // the game's spin stays
    }
    if (!timer) {
        state.store(no_timer);
        return;
    }
    const auto frequency = qpc_frequency();
    const auto now = qpc_now();
    double left{};
    if (!remaining(smain, now, frequency, left)) {
        std::uint8_t mode{};
        state.store(read(smain + mode_offset, mode) && mode ? fixed_step : odd_fields);
        return;
    }
    state.store(ready);
    const double sleep = left - double(margin.load(std::memory_order_relaxed)) * 1e-6;
    if (sleep < 100e-6) return;  // too little to be worth a timer
    const auto planned = now + static_cast<std::int64_t>((sleep < 0.1 ? sleep : 0.1) * double(frequency));
    LARGE_INTEGER due{};
    due.QuadPart = -static_cast<LONGLONG>((sleep < 0.1 ? sleep : 0.1) * 1e7);  // relative, 100 ns units
    if (!SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) return;
    WaitForSingleObject(timer, 200);
    const auto woke = qpc_now();
    waits.fetch_add(1, std::memory_order_relaxed);
    wait_us.add(static_cast<std::uint64_t>(double(woke - now) * 1e6 / double(frequency)));
    const auto late = woke > planned ? static_cast<std::uint64_t>(double(woke - planned) * 1e6 / double(frequency)) : 0;
    late_us.add(late);
    if (late > margin.load(std::memory_order_relaxed)) past_margin.fetch_add(1, std::memory_order_relaxed);
}

void append(std::string& out) {
    json::key(out, "limiter");
    out += '{';
    json::key(out, "active");
    out += on.load(std::memory_order_relaxed) ? "true" : "false";
    json::key(out, "state");
    json::string(out, state_names[state.load()]);
    if (const auto error = timer_error.load()) {
        json::key(out, "timer_error");
        json::number(out, std::uint64_t(error));
    }
    json::key(out, "margin_us");
    json::number(out, std::uint64_t(margin.load()));
    json::key(out, "frames");
    json::number(out, frames.exchange(0));
    json::key(out, "waits");
    json::number(out, waits.exchange(0));
    json::key(out, "past_margin");
    json::number(out, past_margin.exchange(0));
    const auto summary = [&](const char* name, DurationHistogram& h) {
        const auto s = h.take();
        json::key(out, name);
        out += '{';
        json::key(out, "mean");
        json::fixed(out, s.count ? double(s.total_us) / double(s.count) / 1000.0 : 0.0, 4);
        json::key(out, "p50");
        json::fixed(out, double(s.p50_us) / 1000.0, 3);
        json::key(out, "p99");
        json::fixed(out, double(s.p99_us) / 1000.0, 3);
        json::key(out, "max");
        json::fixed(out, double(s.max_us) / 1000.0, 3);
        out += '}';
    };
    summary("wait", wait_us);
    summary("late", late_us);
    out += '}';
}
}  // namespace cpuopt::limiter
