#include "frame_events.hpp"
#include "../core/json.hpp"
#include "log.hpp"
#include <atomic>

namespace cpuopt::frame_events {
namespace {
struct State {
    // Game thread only, apart from the atomics.
    std::int64_t last_begin{}, begin{};
    std::uint64_t last_counter{};
    bool have_counter{};
    std::atomic<std::uint64_t> updates{}, presents{}, render_frames{}, render_counter{};
    std::atomic<bool> render_seen{};
    std::atomic<std::uint32_t> game_thread{}, present_thread{};
    IntervalHistogram update_interval, present_interval;
    DurationHistogram update_work, present_call;
} state;

std::uint64_t us(std::int64_t ticks) noexcept {
    const auto f = static_cast<std::uint64_t>(qpc_frequency());
    const auto t = static_cast<std::uint64_t>(ticks);
    return t / f * 1000000ULL + t % f * 1000000ULL / f;
}

void summary(std::string& out, const char* key, std::uint64_t count, std::uint64_t total, std::uint64_t p50, std::uint64_t p90,
             std::uint64_t p95, std::uint64_t p99, std::uint64_t p999, std::uint64_t max, std::uint64_t over_25ms) {
    json::key(out, key);
    out += '{';
    const auto ms = [&](const char* k, std::uint64_t v) {
        json::key(out, k);
        json::fixed(out, double(v) / 1000.0, 3);
    };
    json::key(out, "count");
    json::number(out, count);
    json::key(out, "mean");
    json::fixed(out, count ? double(total) / double(count) / 1000.0 : 0.0, 4);
    ms("p50", p50);
    ms("p90", p90);
    ms("p95", p95);
    ms("p99", p99);
    ms("p999", p999);
    ms("max", max);
    json::key(out, "over_25ms");
    json::number(out, over_25ms);
    out += '}';
}
template <typename S>
void summary(std::string& out, const char* key, const S& s) {
    summary(out, key, s.count, s.total_us, s.p50_us, s.p90_us, s.p95_us, s.p99_us, s.p999_us, s.max_us, s.over_25ms);
}
}  // namespace

void on_begin(std::int64_t qpc, std::uint32_t thread) noexcept {
    if (state.last_begin && qpc > state.last_begin) state.update_interval.add(us(qpc - state.last_begin));
    state.last_begin = state.begin = qpc;
    state.updates.fetch_add(1, std::memory_order_relaxed);
    if (state.game_thread.load(std::memory_order_relaxed) != thread) state.game_thread.store(thread, std::memory_order_relaxed);
}

void on_end(std::int64_t qpc) noexcept {
    if (state.begin && qpc >= state.begin) state.update_work.add(us(qpc - state.begin));
    state.begin = 0;
}

void on_render_counter(std::uint64_t value) noexcept {
    if (state.have_counter && value > state.last_counter)
        state.render_frames.fetch_add(value - state.last_counter, std::memory_order_relaxed);
    state.last_counter = value;
    state.have_counter = true;
    state.render_counter.store(value, std::memory_order_relaxed);
    state.render_seen.store(true, std::memory_order_relaxed);
}

void on_present(std::int64_t present_qpc, std::int64_t previous_qpc, std::uint32_t thread) noexcept {
    if (previous_qpc && present_qpc > previous_qpc) state.present_interval.add(us(present_qpc - previous_qpc));
    state.presents.fetch_add(1, std::memory_order_relaxed);
    if (state.present_thread.load(std::memory_order_relaxed) != thread) state.present_thread.store(thread, std::memory_order_relaxed);
}

void on_present_returned(std::int64_t present_qpc, std::int64_t now_qpc) noexcept {
    if (present_qpc && now_qpc >= present_qpc) state.present_call.add(us(now_qpc - present_qpc));
}

Window take() noexcept {
    Window w;
    w.updates = state.updates.exchange(0, std::memory_order_relaxed);
    w.presents = state.presents.exchange(0, std::memory_order_relaxed);
    w.update_interval = state.update_interval.take();
    w.present_interval = state.present_interval.take();
    w.update_work = state.update_work.take();
    w.present_call = state.present_call.take();
    w.render_frames = state.render_frames.exchange(0, std::memory_order_relaxed);
    w.render_counter = state.render_counter.load(std::memory_order_relaxed);
    w.render_seen = state.render_seen.load(std::memory_order_relaxed);
    w.game_thread = state.game_thread.load(std::memory_order_relaxed);
    w.present_thread = state.present_thread.load(std::memory_order_relaxed);
    return w;
}

std::uint32_t game_thread() noexcept { return state.game_thread.load(std::memory_order_relaxed); }
std::uint32_t present_thread() noexcept { return state.present_thread.load(std::memory_order_relaxed); }

void append(std::string& out, const Window& w, double window_ms) {
    const double seconds = window_ms / 1000.0;
    json::key(out, "update");
    out += '{';
    json::key(out, "frames");
    json::number(out, w.updates);
    json::key(out, "fps");
    json::fixed(out, seconds > 0 ? double(w.updates) / seconds : 0.0, 2);
    json::key(out, "thread");
    json::number(out, std::uint64_t(w.game_thread));
    summary(out, "interval", w.update_interval);
    summary(out, "work", w.update_work);
    out += '}';
    json::key(out, "present");
    out += '{';
    json::key(out, "count");
    json::number(out, w.presents);
    json::key(out, "fps");
    json::fixed(out, seconds > 0 ? double(w.presents) / seconds : 0.0, 2);
    json::key(out, "thread");
    json::number(out, std::uint64_t(w.present_thread));
    summary(out, "interval", w.present_interval);
    summary(out, "call", w.present_call);
    out += '}';
    json::key(out, "render");
    out += '{';
    json::key(out, "seen");
    out += w.render_seen ? "true" : "false";
    json::key(out, "frames");
    json::number(out, w.render_frames);
    json::key(out, "counter");
    json::number(out, w.render_counter);
    out += '}';
}
}  // namespace cpuopt::frame_events
