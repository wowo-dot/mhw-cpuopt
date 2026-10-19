#include "runtime.hpp"
#include "../core/bench.hpp"
#include "../core/control.hpp"
#include "../core/json.hpp"
#include "../core/limit.hpp"
#include "../core/settings.hpp"
#include "../core/toggles.hpp"
#include "cpu_limit.hpp"
#include "frame_cap.hpp"
#include "limiter.hpp"
#include "frame_events.hpp"
#include "job_timing.hpp"
#include "log.hpp"
#include "sampler.hpp"
#include "system_info.hpp"
#include "thread_census.hpp"
#include <intrin.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <vector>

#ifndef CPUOPT_VERSION
#define CPUOPT_VERSION "0.0.0"
#endif

namespace cpuopt::runtime {
namespace {
HMODULE self_module{};
EventLog event_log;
Host host;
HANDLE stop_event{}, worker_thread{};
std::atomic<bool> started{}, stopped{};
std::atomic<std::uint64_t> windows_written{};
// Player mode: the settings (under settings_lock), the event that wakes the
// runtime's thread for a change, and switches that could not be applied.
std::atomic<bool> player_mode{};
SRWLOCK settings_lock = SRWLOCK_INIT;
Settings current_settings;
bool settings_dirty{};  // changed through `change`, not yet saved
HANDLE change_event{};
std::atomic<unsigned> failed{};
void mark(unsigned toggle, bool failed_now) noexcept {
    if (failed_now) failed.fetch_or(toggle);
    else failed.fetch_and(~toggle);
}
constexpr unsigned tracked_max = 6;
// The benchmark: requests from CMFM's thread, run by the runtime's thread;
// its line for the Mods page under bench_lock.
std::atomic<bool> bench_requested{}, menu_open{}, bench_running{};
SRWLOCK bench_lock = SRWLOCK_INIT;
std::string bench_lines[2] = {"Not run yet", "-"};
std::string limit_line = "measuring";  // the Mods page's "Limited by", under bench_lock
void set_bench_line(std::string first, std::string second) {
    AcquireSRWLockExclusive(&bench_lock);
    bench_lines[0] = std::move(first);
    bench_lines[1] = std::move(second);
    ReleaseSRWLockExclusive(&bench_lock);
}

std::wstring trim(std::wstring s) {
    while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n' || s.back() == L' ' || s.back() == L'\t')) s.pop_back();
    std::size_t start = 0;
    while (start < s.size() && (s[start] == L' ' || s[start] == L'\t' || s[start] == 0xFEFF)) ++start;
    return s.substr(start);
}

// Whole small file as bytes (less than `limit`), or nothing.
bool read_small(const std::wstring& path, std::string& out, DWORD limit = 8192) {
    const auto file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    std::string buffer(limit, '\0');
    DWORD n{};
    const bool ok = ReadFile(file, buffer.data(), limit, &n, nullptr) && n < limit;
    CloseHandle(file);
    if (ok) out.assign(buffer.data(), n);
    return ok;
}

std::wstring widen(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::uint64_t write_time(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
    return std::uint64_t(data.ftLastWriteTime.dwHighDateTime) << 32 | data.ftLastWriteTime.dwLowDateTime;
}

struct State {
    bool player{};                 // player mode: cpuopt.ini, no windows
    std::wstring settings_path;    // player mode's cpuopt.ini
    std::uint64_t settings_stamp{};
    std::wstring control_path;
    std::uint64_t control_stamp{};
    std::string marker;
    unsigned job_threads{16};  // participants while toggle job_thread_cap is on
    bool hook{};               // the executeJob detour is in the chain
    unsigned job_order{2}, order_us{20};  // toggle order_jobs: mode and threshold
    unsigned inline_us{25};               // toggle inline_rounds: threshold
    unsigned time_ms{1000};               // toggle reuse_server_time: how long a read is reused
    bool prim{}, server{};                // the prim_stores change / the server_time detours are in
    unsigned spin_us{300};                // toggle spin_workers: spin window
    bool spin{};                          // the worker_spin change is in
    unsigned unit_us{40};                 // toggle unit_threshold: the threshold to set (al-15: 40-50 best)
    unsigned cpu_count{12};               // toggle smaller_cpu: logical processors kept
    cpu_limit::Ccd ccd;                   // toggle cache_ccd: the largest L3's processors
    bool budget{};                        // the frame_budget detours are in
    unsigned max_fps{1000};               // toggle uncap: the cap to set
    unsigned margin_us{1000};             // toggle sleep_limiter: the spin left before the deadline (al-22)
    unsigned contacts_mode{1};            // toggle parallel_contacts: bit 0 contact pass, bit 1 setup pass (al-23: 1)
    unsigned follow{};                    // follow mode: the followed class's vtable RVA (0 off)
    unsigned follow_us{20};               // follow mode: the gap between samples
    bool wc_scan{};                       // toggle sample_profile: the write-combined scan
    bool running{};                       // the steam_running change is in
    bool prim_buffer{};                   // the prim ring's memory has been reported
    unsigned window_ms{5000};
    std::uint64_t seq{}, updates{}, presents{};
    double window_start{};
    std::vector<DWORD> own;
    ThreadCensus census;
    bool poller{};
};

void ms_field(std::string& out, const char* key, std::uint64_t us) {
    json::key(out, key);
    json::fixed(out, double(us) / 1000.0, 3);
}

// The poller's figures (only when al.frame cannot follow the game's frames).
void append_poller(std::string& out, const frame_clock::Window& f, double ms) {
    json::key(out, "poller");
    out += '{';
    json::key(out, "renderer");
    out += f.renderer ? "true" : "false";
    json::key(out, "counter");
    json::number(out, f.counter);
    json::key(out, "frames");
    json::number(out, f.frames);
    json::key(out, "fps");
    json::fixed(out, ms > 0 ? double(f.frames) * 1000.0 / ms : 0.0, 2);
    json::key(out, "intervals");
    out += '{';
    json::key(out, "count");
    json::number(out, f.intervals.count);
    ms_field(out, "p50", f.intervals.p50_us);
    ms_field(out, "p95", f.intervals.p95_us);
    ms_field(out, "p99", f.intervals.p99_us);
    ms_field(out, "max", f.intervals.max_us);
    out += '}';
    json::key(out, "polls");
    json::number(out, f.polls);
    json::key(out, "poll_gap_avg_us");
    json::number(out, f.polls ? f.poll_gap_total_us / f.polls : 0);
    json::key(out, "multi_step");
    json::number(out, f.multi_step);
    out += '}';
}

void write_window(State& s) {
    const double t = now_ms();
    const double ms = t - s.window_start;
    s.window_start = t;
    const auto frames = frame_events::take();
    s.updates += frames.updates;
    s.presents += frames.presents;
    std::string out = "{";
    json::key(out, "event");
    json::string(out, "window");
    json::key(out, "t_ms");
    json::fixed(out, t, 3);
    json::key(out, "ms");
    json::fixed(out, ms, 1);
    json::key(out, "seq");
    json::number(out, s.seq++);
    json::key(out, "toggles");
    json::number(out, std::uint64_t(toggles.load()));
    json::key(out, "marker");
    json::string(out, s.marker);
    json::key(out, "frame_state");
    json::number(out, std::int64_t(host.frame_state ? host.frame_state() : -1));
    frame_events::append(out, frames, ms);
    job_cap::append(out);
    job_timing::append_order(out);
    job_timing::append_inline(out, reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)));
    server_time::append(out);
    worker_spin::append(out);
    unit_lines::append(out);
    steam_running::append(out);
    cpu_limit::append(out);
    frame_budget::append(out);
    frame_cap::append(out);
    limiter::append(out);
    contacts::append(out);
    json::key(out, "prim_stores");
    out += s.prim ? "true" : "false";
    if (s.poller) append_poller(out, frame_clock::take(), ms);
    const auto busiest = s.census.append_window(out, s.own);
    out += '}';
    event_log.write(out);
    unit_lines::apply();  // a request made before sMhUnit existed
    // The prim ring's memory, read-only, once it is mapped.
    if (!s.prim_buffer && host.prim.found) {
        const auto b = prim_stores::probe(reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)) + host.prim.global_rva);
        if (b.pointer) {
            event_log.write(prim_stores::buffer_event(b, t));
            s.prim_buffer = true;
        }
    }
    const auto profile = sampler::take_event(t, toggles.load(), s.marker);
    if (!profile.empty()) event_log.write(profile);
    const auto timing = job_timing::take_event(t, reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)));
    if (!timing.empty()) event_log.write(timing);
    // Traced rounds, one line each (take_traces ends every line with a newline).
    const auto traced = job_timing::take_traces(reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)));
    for (std::size_t at = 0; at < traced.size();) {
        const auto end = traced.find('\n', at);
        event_log.write(std::string_view(traced).substr(at, end - at));
        at = end + 1;
    }
    // Sampled every tick: the game thread, the present thread, then the busiest others.
    std::vector<DWORD> tracked;
    for (const DWORD tid : {DWORD(frames.game_thread), DWORD(frames.present_thread)})
        if (tid && std::find(tracked.begin(), tracked.end(), tid) == tracked.end()) tracked.push_back(tid);
    for (const DWORD tid : busiest)
        if (tracked.size() < tracked_max && std::find(tracked.begin(), tracked.end(), tid) == tracked.end()) tracked.push_back(tid);
    sampler::set_tracked(tracked);
    windows_written.fetch_add(1);
}

void switched(const char* event, bool on, const char* why) {
    std::string note = "{";
    json::key(note, "event");
    json::string(note, event);
    json::key(note, "on");
    note += on ? "true" : "false";
    json::key(note, "reason");
    json::string(note, why);
    json::key(note, "t_ms");
    json::fixed(note, now_ms(), 3);
    note += '}';
    event_log.write(note);
}

// Applies one set of switches and parameters. `out` is the "control" event
// begun by the caller; it is finished and written here.
void apply_control(State& s, const Control& c, bool initial, std::string out) {
    if (!initial && !s.player) write_window(s);
    if (c.toggles) toggles.store(*c.toggles & toggle_mask);
    if (c.marker) s.marker = *c.marker;
    if (c.window_ms) s.window_ms = *c.window_ms;
    if (c.job_threads) s.job_threads = *c.job_threads;
    if (c.cpu_count) s.cpu_count = *c.cpu_count;
    if (c.sample_jobs) sampler::set_focus(*c.sample_jobs);
    if (c.wc_scan) s.wc_scan = *c.wc_scan != 0;
    sampler::set_wc_scan(s.wc_scan);
    if (c.max_fps) s.max_fps = *c.max_fps;
    if (c.margin_us) s.margin_us = *c.margin_us;
    if (c.contacts) s.contacts_mode = *c.contacts;
    contacts::request(on(parallel_contacts) ? s.contacts_mode : 0);  // applied on the game thread at the next frame
    if (c.follow) s.follow = *c.follow;
    if (c.follow_us) s.follow_us = *c.follow_us;
    sampler::set_follow_gap(s.follow_us);
    job_timing::set_follow(s.follow ? reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)) + s.follow : 0);
    frame_cap::request(on(uncap) ? s.max_fps : 0, s.player);  // applied on the game thread at the next frame
    // The frame budget: its two detours, and the round clock in the executeJob
    // detour. The timing detour also carries the sleep limiter's wait.
    const bool timing_hooks = on(budget) || on(sleep_limiter);
    if (!on(budget)) frame_budget::set_active(false);
    if (!on(sleep_limiter)) limiter::set_active(false, s.margin_us);
    if (timing_hooks != s.budget && host.budget_hooks) {
        const char* why = "";
        const bool ok = host.budget_hooks(timing_hooks, &why);
        s.budget = timing_hooks ? ok : !ok;
        switched("budget_hooks", s.budget, why);
    }
    if (on(sleep_limiter) && s.budget) limiter::set_active(true, s.margin_us);
    mark(sleep_limiter, on(sleep_limiter) && !s.budget);
    // The processors the game keeps: the largest L3's (cache_ccd), the first
    // cpu_count of those or of all (smaller_cpu); job rounds sized to match.
    DWORD_PTR mask = on(cache_ccd) && s.ccd.found ? s.ccd.mask : 0;
    if (on(smaller_cpu)) mask = cpu_limit::first_bits(mask ? mask : cpu_limit::original(), s.cpu_count);
    cpu_limit::set_mask(mask, on(smaller_cpu) ? s.cpu_count : 0);
    unsigned kept = 0;
    for (auto m = mask; m; m &= m - 1) ++kept;
    job_cap::request(on(job_thread_cap) ? s.job_threads : kept);  // applied on the game thread at the next frame
    if (c.job_order) s.job_order = *c.job_order;
    if (c.order_us) s.order_us = *c.order_us;
    if (c.inline_us) s.inline_us = *c.inline_us;
    if (c.time_ms) s.time_ms = *c.time_ms;
    if (c.spin_us) s.spin_us = *c.spin_us;
    if (c.unit_us) s.unit_us = *c.unit_us;
    unit_lines::request(on(unit_threshold) ? s.unit_us : 0);
    // One detour serves job timing, job order, inline rounds and the spinning
    // job threads' round announcements.
    const bool need = on(time_jobs) || on(order_jobs) || on(inline_rounds) || on(spin_workers) || on(budget);
    if (need != s.hook && host.job_hook) {
        const char* why = "";
        const bool ok = host.job_hook(need, &why);
        s.hook = need && ok;
        switched("job_hook", s.hook, why);
    }
    job_timing::set_measuring(on(time_jobs) && s.hook);
    mark(order_jobs, on(order_jobs) && !s.hook);
    job_timing::set_order(on(order_jobs) && s.hook ? static_cast<int>(s.job_order) : 0, s.order_us);
    job_timing::set_inline(on(inline_rounds) && s.hook, s.inline_us);
    // The frame budget measures once both its detours and the executeJob one
    // are in; here, so a change that brings both in at once measures at once
    // (al-34: the first benchmark window had no frame_budget numbers).
    if (on(budget) && s.budget && s.hook) frame_budget::set_active(true);
    // sPrimitive's record loop: a code change through al.patch.
    if (on(plain_prim_stores) != s.prim && host.prim_patch) {
        const char* why = "";
        const bool ok = host.prim_patch(on(plain_prim_stores), &why);
        s.prim = on(plain_prim_stores) ? ok : !ok;
        switched("prim_patch", s.prim, why);
    }
    mark(plain_prim_stores, on(plain_prim_stores) && !s.prim);
    // Server time: the detours go in before the cache is used, and the cache
    // stops before they go out.
    if (!on(reuse_server_time)) server_time::set_active(false, s.time_ms);
    if (on(reuse_server_time) != s.server && host.server_hooks) {
        const char* why = "";
        const bool ok = host.server_hooks(on(reuse_server_time), &why);
        s.server = on(reuse_server_time) ? ok : !ok;
        switched("server_hooks", s.server, why);
    }
    if (on(reuse_server_time) && s.server) server_time::set_active(true, s.time_ms);
    mark(reuse_server_time, on(reuse_server_time) && !s.server);
    // Spinning job threads: the wait is ready before the change sends threads
    // to it, and the change is out before the wait stops spinning.
    const bool spin = on(spin_workers) && s.hook;
    if (spin) worker_spin::set_active(true, s.spin_us);
    if (spin != s.spin && host.spin_patch) {
        const char* why = "";
        const bool ok = host.spin_patch(spin, &why);
        s.spin = spin ? ok : !ok;
        switched("spin_patch", s.spin, why);
    }
    if (!s.spin) worker_spin::set_active(false, s.spin_us);
    // IsSteamRunning: the cache is ready before the change sends calls to it
    // (it passes every call through while inactive).
    if (!on(cache_steam_running)) steam_running::set_active(false, s.time_ms);
    if (on(cache_steam_running) != s.running && host.running_patch) {
        const char* why = "";
        const bool ok = host.running_patch(on(cache_steam_running), &why);
        s.running = on(cache_steam_running) ? ok : !ok;
        switched("running_patch", s.running, why);
    }
    if (on(cache_steam_running) && s.running) steam_running::set_active(true, s.time_ms);
    json::key(out, "t_ms");
    json::fixed(out, now_ms(), 3);
    json::key(out, "toggles");
    json::number(out, std::uint64_t(toggles.load()));
    json::key(out, "requested_toggles");
    json::number(out, std::uint64_t(c.toggles.value_or(toggles.load())));
    json::key(out, "marker");
    json::string(out, s.marker);
    json::key(out, "window_ms");
    json::number(out, std::uint64_t(s.window_ms));
    json::key(out, "job_threads");
    json::number(out, std::uint64_t(s.job_threads));
    json::key(out, "job_order");
    json::number(out, std::uint64_t(s.job_order));
    json::key(out, "order_us");
    json::number(out, std::uint64_t(s.order_us));
    json::key(out, "inline_us");
    json::number(out, std::uint64_t(s.inline_us));
    json::key(out, "time_ms");
    json::number(out, std::uint64_t(s.time_ms));
    json::key(out, "spin_us");
    json::number(out, std::uint64_t(s.spin_us));
    json::key(out, "unit_us");
    json::number(out, std::uint64_t(s.unit_us));
    json::key(out, "cpu_count");
    json::number(out, std::uint64_t(s.cpu_count));
    json::key(out, "max_fps");
    json::number(out, std::uint64_t(s.max_fps));
    json::key(out, "margin_us");
    json::number(out, std::uint64_t(s.margin_us));
    json::key(out, "contacts");
    json::number(out, std::uint64_t(s.contacts_mode));
    json::key(out, "follow");
    json::number(out, std::uint64_t(s.follow));
    json::key(out, "follow_us");
    json::number(out, std::uint64_t(s.follow_us));
    json::key(out, "wc_scan");
    out += s.wc_scan ? "true" : "false";
    out += '}';
    event_log.write(out);
}

// Applies control.ini if it changed. The window so far is closed first, so
// no window mixes two switch states or markers.
void poll_control(State& s, bool initial) {
    const auto stamp = write_time(s.control_path);
    if (stamp == s.control_stamp) return;
    s.control_stamp = stamp;
    std::string text;
    if (!stamp || !read_small(s.control_path, text)) return;
    const auto parsed = parse_control(text);
    std::string out = "{";
    json::key(out, "event");
    json::string(out, "control");
    if (!parsed) {
        json::key(out, "t_ms");
        json::fixed(out, now_ms(), 3);
        json::key(out, "rejected");
        out += "true}";
        event_log.write(out);
        return;
    }
    apply_control(s, *parsed, initial, std::move(out));
}

void own_threads(State& s) {
    s.own = {GetCurrentThreadId(), sampler::thread_id()};
    if (frame_clock::poller_thread()) s.own.push_back(frame_clock::poller_thread());
    sampler::set_excluded(s.own);
}

// What was found at PRE_MAIN, one event each.
void log_discoveries(const State& s) {
        event_log.write(frame_clock::discovery_event(host.discovery, now_ms(), host.scanner));
        event_log.write(job_cap::discovery_event(host.job, now_ms(), host.scanner));
        {
            std::string site = "{";
            json::key(site, "event");
            json::string(site, "job_order_site");
            json::key(site, "reason");
            json::string(site, host.order_site);
            site += '}';
            event_log.write(site);
        }
        event_log.write(prim_stores::discovery_event(host.prim, now_ms(), host.scanner));
        event_log.write(server_time::discovery_event(host.server, now_ms(), host.scanner));
        event_log.write(worker_spin::discovery_event(host.spin, now_ms(), host.scanner));
        event_log.write(cpu_limit::ccd_event(s.ccd, now_ms()));
        event_log.write(frame_budget::discovery_event(host.budget, reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)), now_ms(),
                                                      host.scanner));
        event_log.write(unit_lines::discovery_event(host.units, now_ms(), host.scanner));
        event_log.write(contacts::discovery_event(host.contacts, reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)), now_ms(),
                                                  host.scanner));
        event_log.write(steam_running::discovery_event(host.running, reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)), now_ms(),
                                                       host.scanner));
}

DWORD WINAPI worker(void* parameter) noexcept {
    std::unique_ptr<State> owned(static_cast<State*>(parameter));
    auto& s = *owned;
    try {
        s.ccd = cpu_limit::find_cache_ccd();
        system_info::LoaderInfo loader{"Astera Loader", host.api_version, host.game_version, host.exe_sha256};
        event_log.write(system_info::start_event(self_module, s.control_path, loader));
        poll_control(s, true);  // a run may start with switches already set
        system_info::ModuleCensus modules;
        if (auto e = modules.changed_event(now_ms()); !e.empty()) event_log.write(e);
        log_discoveries(s);

        sampler::start();
        for (int i = 0; i < 100 && !sampler::thread_id(); ++i) Sleep(10);
        own_threads(s);
        std::string baseline;
        s.census.append_window(baseline, s.own);  // first figures need a starting point
        frame_events::take();
        s.window_start = now_ms();
        for (unsigned tick = 0;; ++tick) {
            if (WaitForSingleObject(stop_event, 100) == WAIT_OBJECT_0) break;
            // Without al.frame, or when it cannot follow this game's frames,
            // the poller counts frames from sRender's counter instead.
            const int state = host.frame_state ? host.frame_state() : -1;
            if (!s.poller && frame_clock::has_target() && (state < 0 || state == 3 /* AL_FRAME_UNAVAILABLE */)) {
                frame_clock::start_poller();
                s.poller = true;
                own_threads(s);
                event_log.write("{\"event\":\"poller_started\",\"reason\":\"al.frame unavailable\"}");
            }
            if (tick % 3 == 0) poll_control(s, false);
            if (now_ms() - s.window_start >= s.window_ms) {
                write_window(s);
                if (auto e = modules.changed_event(now_ms()); !e.empty()) event_log.write(e);
            }
        }
        // EXITING: the partial window, then everything of ours stopped.
        write_window(s);
        const bool sampler_stopped = sampler::stop(1000);
        frame_clock::stop_poller();
        char line[320];
        const int n = std::snprintf(line, sizeof(line),
                                    "{\"event\":\"stopped\",\"t_ms\":%.3f,\"windows\":%llu,\"updates\":%llu,\"presents\":%llu,"
                                    "\"sampler_stopped\":%s}",
                                    now_ms(), static_cast<unsigned long long>(windows_written.load()),
                                    static_cast<unsigned long long>(s.updates), static_cast<unsigned long long>(s.presents),
                                    sampler_stopped ? "true" : "false");
        if (n > 0) event_log.write(std::string_view(line, static_cast<std::size_t>(n)));
    } catch (...) {
        event_log.write("{\"event\":\"stopped\",\"reason\":\"exception\"}");
    }
    return 0;
}

// Player mode ---------------------------------------------------------------
Control control_of(const Settings& st) {
    Control c;
    c.toggles = toggles_of(st);
    c.marker = std::string("player");
    c.unit_us = st.unit_us;
    c.margin_us = st.limiter_margin_us;
    c.max_fps = 1000;  // remove_fps_cap: raised from the engine's 300 only
    c.contacts = 1;    // the contact pass (al-23)
    c.job_order = 2;
    c.order_us = 20;
    c.time_ms = 1000;
    return c;
}
void settings_event(const char* what, const std::string& detail) {
    std::string out = "{";
    json::key(out, "event");
    json::string(out, what);
    json::key(out, "t_ms");
    json::fixed(out, now_ms(), 3);
    json::key(out, "detail");
    json::string(out, detail);
    out += '}';
    event_log.write(out);
}
// Research: a GPU capture through PIX's layer, when al_entry.cpp loaded it at
// start (load-pix-capturer.txt). <folder>\pix-capture.txt asks for one: its
// first number is how many frames (1 when none, at most 30). The layer's
// CaptureNextFrame (what PIX's PIXGpuCaptureNextFrames calls) writes
// <folder>\pix-<local time>.wpix at the next Present; the request file goes.
void poll_pix_capture() {
    const auto request = host.folder + L"\\pix-capture.txt";
    if (GetFileAttributesW(request.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    std::string text;
    read_small(request, text);
    DeleteFileW(request.c_str());
    unsigned frames = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            if (frames) break;
            continue;
        }
        frames = frames * 10 + unsigned(c - '0');
        if (frames > 30) break;
    }
    frames = std::clamp(frames, 1U, 30U);
    SYSTEMTIME t{};
    GetLocalTime(&t);
    wchar_t name[64];
    swprintf_s(name, L"\\pix-%04u%02u%02u-%02u%02u%02u.wpix", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    const auto path = host.folder + name;
    using CaptureNextFrame = HRESULT(WINAPI*)(PCWSTR file, UINT32 frames);
    const auto layer = GetModuleHandleW(L"WinPixGpuCapturer.dll");
    const auto capture = layer ? reinterpret_cast<CaptureNextFrame>(GetProcAddress(layer, "CaptureNextFrame")) : nullptr;
    const HRESULT result = capture ? capture(path.c_str(), frames) : E_NOINTERFACE;
    std::string out = "{";
    json::key(out, "event");
    json::string(out, "pix_capture");
    json::key(out, "t_ms");
    json::fixed(out, now_ms(), 3);
    json::key(out, "frames");
    json::number(out, std::uint64_t(frames));
    json::key(out, "layer");
    out += layer ? "true" : "false";
    json::key(out, "hresult");
    char hr[16];
    std::snprintf(hr, sizeof(hr), "0x%08lX", static_cast<unsigned long>(result));
    json::string(out, hr);
    json::key(out, "file");
    json::string(out, utf8(path));
    out += '}';
    event_log.write(out);
}

// cpuopt.ini edited by hand while the game runs: read again.
void reload_if_edited(State& s) {
    const auto stamp = write_time(s.settings_path);
    if (!stamp || stamp == s.settings_stamp) return;
    s.settings_stamp = stamp;
    std::string text, problems;
    if (!read_small(s.settings_path, text)) return;
    const auto read = parse_settings(text, &problems);
    AcquireSRWLockExclusive(&settings_lock);
    if (!settings_dirty) current_settings = read;  // an unsaved change in game wins
    ReleaseSRWLockExclusive(&settings_lock);
    settings_event("settings_read", problems);
}
// Changes from the Mods page or the hotkey into cpuopt.ini, comments kept.
void save_if_dirty(State& s) {
    AcquireSRWLockExclusive(&settings_lock);
    const bool dirty = settings_dirty;
    const auto now = current_settings;
    settings_dirty = false;
    ReleaseSRWLockExclusive(&settings_lock);
    if (!dirty) return;
    std::string text;
    read_small(s.settings_path, text);
    const auto out = write_settings(text, now);
    const auto temp = s.settings_path + L".tmp";
    const auto file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool ok = file != INVALID_HANDLE_VALUE;
    if (ok) {
        DWORD n{};
        ok = WriteFile(file, out.data(), static_cast<DWORD>(out.size()), &n, nullptr) && n == out.size();
        CloseHandle(file);
        ok = ok && MoveFileExW(temp.c_str(), s.settings_path.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    if (ok) s.settings_stamp = write_time(s.settings_path);
    settings_event(ok ? "settings_saved" : "settings_not_saved", ok ? "" : "cpuopt.ini could not be written");
}
// The switches' states as the modules see them, for the log.
void status_event() {
    std::string out = "{";
    json::key(out, "event");
    json::string(out, "status");
    json::key(out, "t_ms");
    json::fixed(out, now_ms(), 3);
    json::key(out, "toggles");
    json::number(out, std::uint64_t(toggles.load()));
    json::key(out, "failed");
    json::number(out, std::uint64_t(failed.load()));
    unit_lines::append(out);
    contacts::append(out);
    limiter::append(out);
    frame_cap::append(out);
    cpu_limit::append(out);
    out += '}';
    event_log.write(out);
}

// The benchmark (bench.hpp) ---------------------------------------------------
struct HandleCloser {
    void operator()(void* h) const noexcept {
        if (h) CloseHandle(h);
    }
};
struct Bench {
    enum class Phase { idle, armed, countdown, running, paused } phase{Phase::idle};
    Settings settings;        // as they were when it was asked for
    bool ccd_found{};         // "Stay on the cache die" can run on this CPU
    unsigned failed{};        // switches that could not start in an "on" window
    std::unique_ptr<void, HandleCloser> game_thread;  // for its cycle count
    ULONG64 cycles_from{};    // the game thread's cycles when counting began
    std::uint64_t tsc_from{};
    double window_ms{15000};
    unsigned window{};        // the window running
    double started{}, due{};  // the window's start and end (running), the first window (countdown)
    bool counting{};          // past the window's settle time
    double counting_from{};
    ULONGLONG cpu_from{};     // the process's CPU time then, 100 ns
    unsigned pauses{};
    bool check_front{};       // the game has a window: count only while it is in front
    std::vector<bench::Window> done;
};

ULONGLONG process_cpu() noexcept {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
    return (ULONGLONG(kernel.dwHighDateTime) << 32 | kernel.dwLowDateTime) + (ULONGLONG(user.dwHighDateTime) << 32 | user.dwLowDateTime);
}
// A visible window of this process that is not a console's.
BOOL CALLBACK find_window(HWND window, LPARAM found) noexcept {
    DWORD pid{};
    GetWindowThreadProcessId(window, &pid);
    wchar_t name[64]{};
    GetClassNameW(window, name, 64);
    if (pid == GetCurrentProcessId() && IsWindowVisible(window) && std::wstring_view(name) != L"ConsoleWindowClass" &&
        std::wstring_view(name) != L"PseudoConsoleWindow") {
        *reinterpret_cast<bool*>(found) = true;
        return FALSE;
    }
    return TRUE;
}
bool has_window() noexcept {
    bool found = false;
    EnumWindows(find_window, reinterpret_cast<LPARAM>(&found));
    return found;
}
bool in_front() noexcept {
    DWORD pid{};
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

void bench_event(const char* what, const Bench& b) {
    char line[200];
    const int n = std::snprintf(line, sizeof(line), "{\"event\":\"bench\",\"t_ms\":%.3f,\"what\":\"%s\",\"window\":%u,\"pauses\":%u}", now_ms(), what,
                                b.window, b.pauses);
    if (n > 0) event_log.write(std::string_view(line, static_cast<std::size_t>(n)));
}

ULONG64 thread_cycles(const Bench& b) noexcept {
    ULONG64 c{};
    return b.game_thread && QueryThreadCycleTime(b.game_thread.get(), &c) ? c : 0;
}

// The Mods page's "Limited by" (limit.hpp): every second, the game thread's
// cycles and the frames since the last sample. Reads only.
struct LimitSampler {
    std::unique_ptr<void, HandleCloser> thread;
    DWORD tid{};
    ULONG64 cycles{};
    std::uint64_t tsc{}, frames{};
    double t{}, vsync_checked{};
    bool vsync{};
};
void limit_tick(LimitSampler& l) {
    const double t = now_ms();
    if (t - l.t < 1000.0) return;
    const auto id = static_cast<DWORD>(frame_events::game_thread());
    if (!id) return;
    if (id != l.tid) {  // the first frame, or another thread runs the frames now
        l.thread.reset(OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, id));
        l.tid = id;
        l.cycles = 0;
    }
    if (t - l.vsync_checked > 5000.0) {  // the game's own option; it can change in game
        const auto ini = system_info::folder_of(system_info::module_path(nullptr)) + L"\\graphics_option.ini";
        l.vsync = system_info::ini_value(ini, L"GraphicsOption", L"V-Sync") == "On";
        l.vsync_checked = t;
    }
    ULONG64 cycles{};
    const bool ok = l.thread && QueryThreadCycleTime(l.thread.get(), &cycles);
    const auto tsc = __rdtsc();
    const auto frames = frame_events::frames_total();
    if (ok && l.cycles && cycles > l.cycles && tsc > l.tsc && frames > l.frames) {
        const double ms = t - l.t;
        const double tsc_per_ms = double(tsc - l.tsc) / ms;
        limit::Input in;
        in.fps = double(frames - l.frames) * 1000.0 / ms;
        in.busy_ms = double(cycles - l.cycles) / tsc_per_ms / double(frames - l.frames);
        in.cap_fps = frame_cap::limit();
        in.vsync = l.vsync;
        auto line = limit::text(limit::classify(in));
        AcquireSRWLockExclusive(&bench_lock);
        limit_line = std::move(line);
        ReleaseSRWLockExclusive(&bench_lock);
    }
    l.cycles = ok ? cycles : 0;
    l.tsc = tsc;
    l.frames = frames;
    l.t = t;
}

// The settings with the optimizations on or off, and frame_budget in both.
Control bench_control(const Settings& st, bool on) {
    auto copy = st;
    copy.enabled = on;
    auto c = control_of(copy);
    c.toggles = *c.toggles | unsigned(budget);
    c.marker = std::string(on ? "benchmark-on" : "benchmark-off");
    return c;
}

// The optimizations the "on" windows ran: chosen, and usable on this game and CPU.
std::string switch_names(const Bench& b) {
    const auto& st = b.settings;
    struct Item {
        bool on, usable;
        unsigned toggle;
        const char* name;
    };
    const Item items[] = {
        {st.effect_stores, host.prim.found, cpuopt::plain_prim_stores, "effect drawing"},
        {st.job_order, host.job.found && host.order_site == "ok", cpuopt::order_jobs, "draw job order"},
        {st.server_time, host.server.found, cpuopt::reuse_server_time, "Steam server time"},
        {st.unit_threshold, host.job.found && host.units.found, cpuopt::unit_threshold, "update threads"},
        {st.cache_ccd, b.ccd_found, cpuopt::cache_ccd, "stay on the cache die"},
        {st.parallel_contacts, host.contacts.found, cpuopt::parallel_contacts, "parallel hit detection"},
        {st.sleep_limiter, host.budget.found, cpuopt::sleep_limiter, "sleeping frame limiter"},
    };
    std::string out;
    for (const auto& i : items) {
        if (!i.on || !i.usable) continue;
        if (!out.empty()) out += ", ";
        out += i.name;
        if (b.failed & i.toggle) out += " (could not start: cpuopt-session.log)";
    }
    return out.empty() ? "none" : out;
}

// The processors and priority MHW runs with as set outside cpuopt, when not all or not normal.
std::string processors_of() {
    unsigned allowed = 0;
    for (auto m = cpu_limit::outside(); m; m &= m - 1) ++allowed;
    const unsigned total = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    const auto priority = GetPriorityClass(GetCurrentProcess());
    const char* name = priority == HIGH_PRIORITY_CLASS           ? "High"
                       : priority == REALTIME_PRIORITY_CLASS     ? "Realtime"
                       : priority == ABOVE_NORMAL_PRIORITY_CLASS ? "Above normal"
                       : priority == BELOW_NORMAL_PRIORITY_CLASS ? "Below normal"
                       : priority == IDLE_PRIORITY_CLASS         ? "Low"
                                                                 : nullptr;
    const bool limited = allowed && allowed < total;
    if (!limited && !name) return {};
    std::string out = limited ? std::to_string(allowed) + " of " + std::to_string(total) + ", set outside cpuopt" : "all " + std::to_string(total);
    if (name) out += std::string("; priority ") + name;
    return out;
}

// The DLSS mod's mode, frame generation and Reflex, from its file, when it is loaded.
std::string dlss_of(bool& reflex) {
    reflex = false;
    if (!GetModuleHandleW(L"mhw_dlss.dll")) return {};
    std::string text;
    const auto path = system_info::folder_of(system_info::module_path(nullptr)) + L"\\AsteraLoader\\plugins\\mhw_dlss\\mhw_dlss.ini";
    if (!read_small(path, text, 1U << 16)) return "loaded, its settings file not found";
    const auto value = [&](const char* key) { return ini_lookup(text, key).value_or("?"); };
    const auto r = value("reflex");
    reflex = r == "on" || r == "boost";
    return value("dlss_mode") + ", frame generation " + value("frame_generation") + ", Reflex " + r;
}

// The effect vertex ring's memory: write-combined is what makes the game's reads slow.
std::string effect_memory_of() {
    if (!host.prim.found) return {};
    const auto b = prim_stores::probe(reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)) + host.prim.global_rva);
    if (!b.pointer) return "not mapped yet";
    return b.write_combined() ? "write-combined (slow to read back, as the effect drawing fix expects)"
                              : "not write-combined (cheap to read back here: the effect drawing fix has less to save)";
}

bench::Machine machine_of(const Bench& b) {
    bench::Machine m;
    SYSTEMTIME t{};
    GetLocalTime(&t);
    char when[32];
    std::snprintf(when, sizeof(when), "%04u-%02u-%02u %02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
    m.when = when;
    m.version = CPUOPT_VERSION;
    m.game = host.game_version.empty() ? "version unknown" : host.game_version;
    if (!host.exe_sha256.empty()) m.game += system_info::tested_exe(host.exe_sha256) ? ", the build cpuopt was made on" : ", not the build cpuopt was made on";
    m.cpu = system_info::cpu_name() + ", " + std::to_string(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)) + " threads";
    m.gpu = system_info::gpu_name();
    m.windows = system_info::windows_version();
    const auto ini = system_info::folder_of(system_info::module_path(nullptr)) + L"\\graphics_option.ini";
    const auto value = [&](const wchar_t* key) { return system_info::ini_value(ini, L"GraphicsOption", key); };
    m.display = value(L"Resolution").empty() ? std::string("graphics_option.ini not found")
                                             : value(L"Resolution") + ", " + value(L"ScreenMode") + ", frame rate " + value(L"FrameRate") +
                                                   ", V-Sync " + value(L"V-Sync") + ", the in-game DLSS option " + value(L"NVIDIA DLSS");
    m.switches = switch_names(b);
    m.processors = processors_of();
    m.dlss = dlss_of(m.reflex);
    m.effect_memory = effect_memory_of();
    m.modules = system_info::other_modules(self_module);
    m.fps_cap_removed = b.settings.remove_fps_cap;
    m.pauses = b.pauses;
    m.window_s = b.window_ms / 1000.0;
    return m;
}

// Appends the report to <folder>\cpuopt-benchmark.txt (CRLF; a new file past 1 MB).
bool append_report(const std::string& text) {
    std::string crlf;
    for (const char c : text) {
        if (c == '\n') crlf += '\r';
        crlf += c;
    }
    const auto path = host.folder + L"\\cpuopt-benchmark.txt";
    WIN32_FILE_ATTRIBUTE_DATA data{};
    const bool big = GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) && (data.nFileSizeHigh || data.nFileSizeLow > (1U << 20));
    const auto file = CreateFileW(path.c_str(), big ? GENERIC_WRITE : FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, big ? CREATE_ALWAYS : OPEN_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD n{};
    const bool ok = WriteFile(file, crlf.data(), static_cast<DWORD>(crlf.size()), &n, nullptr) && n == crlf.size();
    CloseHandle(file);
    return ok;
}

// One window's numbers, as the "windows" rows and the bench_window event hold them.
void append_window(std::string& out, const bench::Window& w) {
    out += w.on ? "[1," : "[0,";
    json::fixed(out, w.seconds, 3);
    out += ',';
    json::number(out, w.frames);
    for (const double v : {w.p50_ms, w.p99_ms, w.max_ms, w.busy_ms, w.busy_share}) {
        out += ',';
        json::fixed(out, v, 4);
    }
    out += ',';
    json::number(out, w.cpu_frames);
    for (const double v : {w.cpu_ms, w.update_ms, w.limiter_ms, w.rounds_ms, w.present_ms, w.serial_ms, w.process_ms}) {
        out += ',';
        json::fixed(out, v, 4);
    }
    out += ']';
}
constexpr const char* window_columns =
    "[\"on\",\"seconds\",\"frames\",\"p50\",\"p99\",\"max\",\"busy_ms\",\"busy_share\",\"cpu_frames\",\"cpu_ms\",\"update_ms\","
    "\"limiter_ms\",\"rounds_ms\",\"present_ms\",\"serial_ms\",\"process_ms\"]";

// Every window as it ends: written at once, so even a killed game keeps them.
void window_event(std::size_t index, const bench::Window& w) {
    std::string out = "{";
    json::key(out, "event");
    json::string(out, "bench_window");
    json::key(out, "t_ms");
    json::fixed(out, now_ms(), 3);
    json::key(out, "index");
    json::number(out, std::uint64_t(index));
    json::key(out, "columns");
    out += window_columns;
    json::key(out, "values");
    append_window(out, w);
    out += '}';
    event_log.write(out);
}

void result_event(const bench::Result& r, const Bench& b, std::span<const bench::Window> windows, const char* ended_early) {
    std::string out = "{";
    json::key(out, "event");
    json::string(out, "benchmark");
    json::key(out, "t_ms");
    json::fixed(out, now_ms(), 3);
    json::key(out, "stopped");
    json::string(out, ended_early ? ended_early : "");
    json::key(out, "window_ms");
    json::number(out, std::uint64_t(b.window_ms));
    json::key(out, "pauses");
    json::number(out, std::uint64_t(b.pauses));
    const std::pair<const char*, double> fields[] = {{"off_fps", r.off_fps}, {"on_fps", r.on_fps},       {"gain", r.gain},
                                                     {"gain_low", r.gain_low}, {"gain_high", r.gain_high}, {"off_p99", r.off_p99},
                                                     {"on_p99", r.on_p99},   {"off_cpu", r.off_cpu},     {"on_cpu", r.on_cpu},
                                                     {"off_process", r.off_process}, {"on_process", r.on_process}};
    for (const auto& [name, v] : fields) {
        json::key(out, name);
        json::fixed(out, v, 4);
    }
    json::key(out, "columns");
    out += window_columns;
    json::key(out, "windows");
    out += '[';
    for (std::size_t i = 0; i < windows.size(); ++i) {
        if (i) out += ',';
        append_window(out, windows[i]);
    }
    out += "]}";
    event_log.write(out);
}

void bench_start_window(State& s, Bench& b, double t) {
    apply_control(s, bench_control(b.settings, bench::window_on(b.window)), false, "{\"event\":\"control\"");
    if (bench::window_on(b.window)) b.failed |= failed.load();
    b.phase = Bench::Phase::running;
    b.started = t;
    b.due = t + b.window_ms;
    b.counting = false;
}

void bench_line_running(const Bench& b, double t) {
    const double left_s = (double(bench::windows - b.window - 1) * b.window_ms + (b.due - t)) / 1000.0;
    char line[64];
    std::snprintf(line, sizeof(line), "%u of %u, about %u:%02u left", b.window + 1, bench::windows, unsigned(left_s) / 60, unsigned(left_s) % 60);
    set_bench_line(line, bench::window_on(b.window) ? "now with them on" : "now with them off");
}

// One step, every 50 ms while a benchmark is on.
void bench_tick(State& s, Bench& b) {
    const double t = now_ms();
    const bool away = menu_open.load() || (b.check_front && !in_front());
    switch (b.phase) {
    case Bench::Phase::idle:
        return;
    case Bench::Phase::armed:
        if (!menu_open.load()) {
            b.phase = Bench::Phase::countdown;
            b.due = t + bench::countdown_ms(b.window_ms);
            set_bench_line("Starting in 10 s", "stand still, camera too");
        }
        return;
    case Bench::Phase::countdown:
        if (menu_open.load()) {
            b.phase = Bench::Phase::armed;
            set_bench_line("Close the menu to start", "then stand still, camera too");
        } else if (t >= b.due) {
            if (const auto id = frame_events::game_thread()) b.game_thread.reset(OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, id));
            bench_event("start", b);
            bench_start_window(s, b, t);
        }
        return;
    case Bench::Phase::paused:
        if (!away) {  // the window again, from its start
            b.phase = Bench::Phase::running;
            b.started = t;
            b.due = t + b.window_ms;
            b.counting = false;
            bench_event("resumed", b);
        }
        return;
    case Bench::Phase::running:
        break;
    }
    if (away) {
        b.phase = Bench::Phase::paused;
        ++b.pauses;
        bench_event("paused", b);
        set_bench_line(menu_open.load() ? "Paused: the menu is open" : "Paused: game in background",
                       "window " + std::to_string(b.window + 1) + " runs again");
        return;
    }
    if (!b.counting) {
        if (t < b.started + bench::settle_ms(b.window_ms)) return bench_line_running(b, t);
        frame_events::take();
        frame_budget::take_totals();
        b.cpu_from = process_cpu();
        b.cycles_from = thread_cycles(b);
        b.tsc_from = __rdtsc();
        b.counting_from = t;
        b.counting = true;
        return bench_line_running(b, t);
    }
    if (t < b.due) return bench_line_running(b, t);
    const auto frames = frame_events::take();
    const auto budget_totals = frame_budget::take_totals();
    const auto cpu = process_cpu();
    const auto cycles = thread_cycles(b);
    const auto tsc = __rdtsc();
    bench::Window w;
    w.on = bench::window_on(b.window);
    w.seconds = (t - b.counting_from) / 1000.0;
    w.frames = frames.update_interval.count;
    w.max_ms = double(frames.update_interval.max_us) / 1000.0;
    w.p50_ms = std::min(double(frames.update_interval.p50_us) / 1000.0, w.max_ms);  // bucket edges, up to 50 us high
    w.p99_ms = std::min(double(frames.update_interval.p99_us) / 1000.0, w.max_ms);
    // The game thread's cycles count at the invariant TSC rate (thread_census.cpp).
    if (b.cycles_from && cycles > b.cycles_from && tsc > b.tsc_from && w.frames && t > b.counting_from) {
        const double tsc_per_ms = double(tsc - b.tsc_from) / (t - b.counting_from);
        w.busy_share = std::min(1.0, double(cycles - b.cycles_from) / double(tsc - b.tsc_from));
        w.busy_ms = double(cycles - b.cycles_from) / tsc_per_ms / double(w.frames);
    }
    w.cpu_frames = budget_totals.frames;
    w.cpu_ms = budget_totals.cpu_frame_ms;
    w.update_ms = budget_totals.update_ms;
    w.limiter_ms = budget_totals.limiter_ms;
    w.rounds_ms = budget_totals.rounds_ms;
    w.present_ms = budget_totals.present_ms;
    w.serial_ms = budget_totals.serial_ms;
    w.process_ms = w.frames && cpu > b.cpu_from ? double(cpu - b.cpu_from) / 10000.0 / double(w.frames) : 0.0;
    b.done.push_back(w);
    window_event(b.done.size() - 1, w);
    if (++b.window < bench::windows) return bench_start_window(s, b, t);
    // Done: the report, the player's own settings back, a sound.
    const auto r = bench::summarize(b.done);
    const bool written = append_report(bench::report(machine_of(b), b.done, r));
    result_event(r, b, b.done, nullptr);
    apply_control(s, control_of(settings()), false, "{\"event\":\"control\"");
    set_bench_line(bench::fps_line(r), written ? bench::cpu_line(r) : "report not written: see log");
    if (b.check_front) MessageBeep(MB_ICONASTERISK);  // someone at the game to hear it
    b = Bench{};
    bench_running = false;
}

void bench_arm(const State& s, Bench& b, const Settings& st) {
    b = Bench{};
    b.settings = st;
    b.ccd_found = s.ccd.found;
    b.window_ms = double(st.benchmark_s) * 1000.0;
    b.check_front = has_window();
    b.phase = Bench::Phase::armed;
    bench_running = true;
    bench_event("armed", b);
    set_bench_line("Close the menu to start", "then stand still, camera too");
}

// A benchmark that ends early: its complete pairs, from two on, as a report
// and on the page. False when there are fewer.
bool bench_partial(const Bench& b, const char* why) {
    const std::span<const bench::Window> windows(b.done.data(), b.done.size() / 2 * 2);
    if (windows.size() < 4) return false;
    const auto r = bench::summarize(windows);
    auto m = machine_of(b);
    m.stopped = why;
    const bool written = append_report(bench::report(m, windows, r));
    result_event(r, b, windows, why);
    char first[64];
    std::snprintf(first, sizeof(first), "%s (%zu/%u)", bench::fps_line(r).c_str(), windows.size() / 2, bench::pairs);
    set_bench_line(first, written ? bench::cpu_line(r) : "report not written: see log");
    return true;
}

void bench_stop(Bench& b, const char* line, const char* why) {
    bench_event("stopped", b);
    if (!bench_partial(b, why)) set_bench_line(line, "run it again");
    b = Bench{};
    bench_running = false;
}

DWORD WINAPI player_worker(void* parameter) noexcept {
    std::unique_ptr<State> owned(static_cast<State*>(parameter));
    auto& s = *owned;
    try {
        s.ccd = cpu_limit::find_cache_ccd();
        system_info::LoaderInfo loader{"Astera Loader", host.api_version, host.game_version, host.exe_sha256};
        event_log.write(system_info::start_event(self_module, s.settings_path, loader));
        log_discoveries(s);
        auto applied = settings();
        auto key = applied.toggle_key;
        apply_control(s, control_of(applied), true, "{\"event\":\"control\"");
        if (host.hotkey) host.hotkey(key);
        double status_due = now_ms() + 10000.0;  // the switches settle within a few frames
        system_info::ModuleCensus modules;       // other mods: with each status line, when they changed
        Bench b;
        LimitSampler limiter_sampler;
        for (;;) {
            const HANDLE waits[2] = {stop_event, change_event};
            if (WaitForMultipleObjects(2, waits, FALSE, b.phase == Bench::Phase::idle ? 1000 : 50) == WAIT_OBJECT_0) break;
            reload_if_edited(s);
            poll_pix_capture();
            save_if_dirty(s);
            const auto now = settings();
            if (!(now == applied)) {
                if (b.phase != Bench::Phase::idle) bench_stop(b, "Stopped: a setting changed", "a setting changed");
                apply_control(s, control_of(now), false, "{\"event\":\"control\"");
                if (now.toggle_key != key && host.hotkey) host.hotkey(key = now.toggle_key);
                applied = now;
                status_due = now_ms() + 10000.0;
            }
            if (bench_requested.exchange(false) && b.phase == Bench::Phase::idle) bench_arm(s, b, now);
            bench_tick(s, b);
            limit_tick(limiter_sampler);
            // unit_threshold's request from the title screen, before sMhUnit
            // exists, and its drift check (research mode does this with each
            // window; player mode has none, so it waited until a setting changed).
            unit_lines::apply();
            if (now_ms() >= status_due) {
                status_event();
                if (auto e = modules.changed_event(now_ms()); !e.empty()) event_log.write(e);
                status_due = now_ms() + 300000.0;
            }
        }
        if (b.phase == Bench::Phase::running || b.phase == Bench::Phase::paused) {  // the game closing mid-run
            bench_event("stopped", b);
            bench_partial(b, "the game closed");
        }
        save_if_dirty(s);
        status_event();
        event_log.write("{\"event\":\"stopped\"}");
    } catch (...) {
        event_log.write("{\"event\":\"stopped\",\"reason\":\"exception\"}");
    }
    return 0;
}
}  // namespace

bool player() noexcept { return player_mode.load(); }
Settings settings() noexcept {
    AcquireSRWLockShared(&settings_lock);
    const auto copy = current_settings;
    ReleaseSRWLockShared(&settings_lock);
    return copy;
}
void change(const Settings& s) noexcept {
    AcquireSRWLockExclusive(&settings_lock);
    current_settings = s;
    settings_dirty = true;
    ReleaseSRWLockExclusive(&settings_lock);
    if (change_event) SetEvent(change_event);
}
void flip_enabled() noexcept {
    AcquireSRWLockExclusive(&settings_lock);
    current_settings.enabled = !current_settings.enabled;
    settings_dirty = true;
    ReleaseSRWLockExclusive(&settings_lock);
    if (change_event) SetEvent(change_event);
}
unsigned failed_toggles() noexcept { return failed.load(); }
void bench_press() noexcept {
    if (bench_running.load()) return;
    menu_open = true;  // pressed on the page, so it is open (CMFM's thread: before its CLOSED)
    bench_requested = true;
    if (change_event) SetEvent(change_event);
}
void menu_shown(bool open) noexcept {
    menu_open = open;
    if (change_event) SetEvent(change_event);
}
bool bench_busy() noexcept { return bench_running.load() || bench_requested.load(); }
std::string limit_text() {
    AcquireSRWLockShared(&bench_lock);
    auto copy = limit_line;
    ReleaseSRWLockShared(&bench_lock);
    return copy;
}
std::string bench_text(unsigned line) {
    AcquireSRWLockShared(&bench_lock);
    auto copy = bench_lines[line ? 1 : 0];
    ReleaseSRWLockShared(&bench_lock);
    return copy;
}

bool start(HMODULE self, const Host& h, std::string& reason) noexcept {
    try {
        if (started.exchange(true)) {
            reason = "already started";
            return false;
        }
        self_module = self;
        host = h;
        std::string sidecar;
        const auto sidecar_path = host.folder + L"\\cpuopt.dll.output-dir";
        if (!read_small(sidecar_path, sidecar)) {
            // Player mode.
            // Not "cpuopt.log": Astera Loader's own log of the plugin has that name (logs\plugins).
            const auto log_path = host.folder + L"\\cpuopt-session.log";
            if (!event_log.open_file(log_path, 1ULL << 20)) {
                reason = "cpuopt-session.log cannot be written: idle";
                return false;
            }
            auto state = std::make_unique<State>();
            state->player = true;
            state->settings_path = host.folder + L"\\cpuopt.ini";
            std::string text, problems;
            if (read_small(state->settings_path, text)) {
                current_settings = parse_settings(text, &problems);
            } else {
                current_settings = Settings{};
                settings_dirty = true;  // the default file, written by the runtime's thread
            }
            if (!problems.empty()) settings_event("settings_read", problems);
            state->settings_stamp = write_time(state->settings_path);
            player_mode = true;
            stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            change_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            worker_thread = CreateThread(nullptr, 0, player_worker, state.get(), 0, nullptr);
            if (!worker_thread) {
                reason = "could not start its thread";
                return false;
            }
            state.release();
            reason = "player mode, log " + utf8(log_path);
            return true;
        }
        const auto folder = trim(widen(sidecar));
        if (folder.empty() || !event_log.open(folder)) {
            reason = "run folder not writable: idle";
            return false;
        }
        auto state = std::make_unique<State>();
        state->control_path = folder + L"\\control.ini";
        stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        worker_thread = CreateThread(nullptr, 0, worker, state.get(), 0, nullptr);
        if (!worker_thread) {
            reason = "could not start its thread";
            return false;
        }
        state.release();  // the worker owns it now
        reason = utf8(event_log.path());
        return true;
    } catch (...) {
        reason = "start failed";
        return false;
    }
}

void stop(const char* why) noexcept {
    if (!worker_thread || stopped.exchange(true)) return;
    char line[160];
    const int n = std::snprintf(line, sizeof(line), "{\"event\":\"stopping\",\"t_ms\":%.3f,\"why\":\"%s\"}", now_ms(), why);
    if (n > 0) event_log.write(std::string_view(line, static_cast<std::size_t>(n)));
    SetEvent(stop_event);
    if (WaitForSingleObject(worker_thread, 2000) != WAIT_OBJECT_0) event_log.write("{\"event\":\"stop_timeout\"}");
}

void note(const char* event, const char* key, long long value) noexcept {
    char line[200];
    const int n = std::snprintf(line, sizeof(line), "{\"event\":\"%s\",\"t_ms\":%.3f,\"%s\":%lld}", event, now_ms(), key, value);
    if (n > 0) event_log.write(std::string_view(line, static_cast<std::size_t>(n)));
}

void at_exit() noexcept {
    char line[160];
    const int n = std::snprintf(line, sizeof(line), "{\"event\":\"exit\",\"t_ms\":%.3f,\"windows\":%llu}", now_ms(),
                                static_cast<unsigned long long>(windows_written.load()));
    if (n > 0) event_log.write_at_exit(std::string_view(line, static_cast<std::size_t>(n)));
}
}  // namespace cpuopt::runtime
