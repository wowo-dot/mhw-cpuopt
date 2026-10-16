#include "runtime.hpp"
#include "../core/control.hpp"
#include "../core/json.hpp"
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
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <vector>

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

std::wstring trim(std::wstring s) {
    while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n' || s.back() == L' ' || s.back() == L'\t')) s.pop_back();
    std::size_t start = 0;
    while (start < s.size() && (s[start] == L' ' || s[start] == L'\t' || s[start] == 0xFEFF)) ++start;
    return s.substr(start);
}

// Whole small file as bytes, or nothing.
bool read_small(const std::wstring& path, std::string& out) {
    const auto file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    char buffer[8192];
    DWORD n{};
    const bool ok = ReadFile(file, buffer, sizeof(buffer), &n, nullptr) && n < sizeof(buffer);
    CloseHandle(file);
    if (ok) out.assign(buffer, n);
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
    if (on(budget) && s.budget && s.hook) frame_budget::set_active(true);
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
        for (;;) {
            const HANDLE waits[2] = {stop_event, change_event};
            if (WaitForMultipleObjects(2, waits, FALSE, 1000) == WAIT_OBJECT_0) break;
            reload_if_edited(s);
            save_if_dirty(s);
            const auto now = settings();
            if (!(now == applied)) {
                apply_control(s, control_of(now), false, "{\"event\":\"control\"");
                if (now.toggle_key != key && host.hotkey) host.hotkey(key = now.toggle_key);
                applied = now;
                status_due = now_ms() + 10000.0;
            }
            if (now_ms() >= status_due) {
                status_event();
                status_due = now_ms() + 300000.0;
            }
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
            const auto log_path = host.folder + L"\\cpuopt.log";
            if (!event_log.open_file(log_path, 1ULL << 20)) {
                reason = "cpuopt.log cannot be written: idle";
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
