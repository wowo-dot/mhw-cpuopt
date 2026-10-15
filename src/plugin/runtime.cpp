#include "runtime.hpp"
#include "../core/control.hpp"
#include "../core/json.hpp"
#include "../core/toggles.hpp"
#include "frame_events.hpp"
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
    std::wstring control_path;
    std::uint64_t control_stamp{};
    std::string marker;
    unsigned job_threads{16};  // participants while toggle job_thread_cap is on
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
    if (s.poller) append_poller(out, frame_clock::take(), ms);
    const auto busiest = s.census.append_window(out, s.own);
    out += '}';
    event_log.write(out);
    const auto profile = sampler::take_event(t, toggles.load(), s.marker);
    if (!profile.empty()) event_log.write(profile);
    // Sampled every tick: the game thread, the present thread, then the busiest others.
    std::vector<DWORD> tracked;
    for (const DWORD tid : {DWORD(frames.game_thread), DWORD(frames.present_thread)})
        if (tid && std::find(tracked.begin(), tracked.end(), tid) == tracked.end()) tracked.push_back(tid);
    for (const DWORD tid : busiest)
        if (tracked.size() < tracked_max && std::find(tracked.begin(), tracked.end(), tid) == tracked.end()) tracked.push_back(tid);
    sampler::set_tracked(tracked);
    windows_written.fetch_add(1);
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
    if (!initial) write_window(s);
    if (parsed->toggles) toggles.store(*parsed->toggles & toggle_mask);
    if (parsed->marker) s.marker = *parsed->marker;
    if (parsed->window_ms) s.window_ms = *parsed->window_ms;
    if (parsed->job_threads) s.job_threads = *parsed->job_threads;
    job_cap::request(on(job_thread_cap) ? s.job_threads : 0);  // applied on the game thread at the next frame
    json::key(out, "t_ms");
    json::fixed(out, now_ms(), 3);
    json::key(out, "toggles");
    json::number(out, std::uint64_t(toggles.load()));
    json::key(out, "requested_toggles");
    json::number(out, std::uint64_t(parsed->toggles.value_or(toggles.load())));
    json::key(out, "marker");
    json::string(out, s.marker);
    json::key(out, "window_ms");
    json::number(out, std::uint64_t(s.window_ms));
    json::key(out, "job_threads");
    json::number(out, std::uint64_t(s.job_threads));
    out += '}';
    event_log.write(out);
}

void own_threads(State& s) {
    s.own = {GetCurrentThreadId(), sampler::thread_id()};
    if (frame_clock::poller_thread()) s.own.push_back(frame_clock::poller_thread());
    sampler::set_excluded(s.own);
}

DWORD WINAPI worker(void* parameter) noexcept {
    std::unique_ptr<State> owned(static_cast<State*>(parameter));
    auto& s = *owned;
    try {
        system_info::LoaderInfo loader{"Astera Loader", host.api_version, host.game_version, host.exe_sha256};
        event_log.write(system_info::start_event(self_module, s.control_path, loader));
        poll_control(s, true);  // a run may start with switches already set
        system_info::ModuleCensus modules;
        if (auto e = modules.changed_event(now_ms()); !e.empty()) event_log.write(e);
        event_log.write(frame_clock::discovery_event(host.discovery, now_ms(), host.scanner));
        event_log.write(job_cap::discovery_event(host.job, now_ms(), host.scanner));

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
}  // namespace

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
            reason = "no run folder (cpuopt.dll.output-dir): idle";
            return false;
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
