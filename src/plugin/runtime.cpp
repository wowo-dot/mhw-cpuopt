#include "runtime.hpp"
#include "../core/control.hpp"
#include "../core/json.hpp"
#include "../core/toggles.hpp"
#include "frame_clock.hpp"
#include "log.hpp"
#include "sampler.hpp"
#include "system_info.hpp"
#include "thread_census.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

namespace cpuopt::runtime {
namespace {
HMODULE self_module{};
EventLog event_log;
std::atomic<std::uint64_t> windows_written{};

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
    unsigned window_ms{5000};
    std::uint64_t seq{};
    double window_start{};
    std::vector<DWORD> own;
    ThreadCensus census;
};

void ms_field(std::string& out, const char* key, std::uint64_t us) {
    json::key(out, key);
    json::fixed(out, double(us) / 1000.0, 3);
}

void write_window(State& s) {
    const double t = now_ms();
    const double ms = t - s.window_start;
    s.window_start = t;
    const auto f = frame_clock::take();
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
    json::key(out, "renderer");
    out += f.renderer ? "true" : "false";
    json::key(out, "counter");
    json::number(out, f.counter);
    json::key(out, "frames");
    json::number(out, f.frames);
    json::key(out, "fps");
    json::fixed(out, ms > 0 ? double(f.frames) * 1000.0 / ms : 0.0, 2);
    json::key(out, "frame_ms_avg");
    json::fixed(out, f.frames ? ms / double(f.frames) : 0.0, 3);
    json::key(out, "intervals");
    out += '{';
    json::key(out, "count");
    json::number(out, f.intervals.count);
    ms_field(out, "p50", f.intervals.p50_us);
    ms_field(out, "p90", f.intervals.p90_us);
    ms_field(out, "p95", f.intervals.p95_us);
    ms_field(out, "p99", f.intervals.p99_us);
    ms_field(out, "p999", f.intervals.p999_us);
    ms_field(out, "max", f.intervals.max_us);
    json::key(out, "over_25ms");
    json::number(out, f.intervals.over_25ms);
    json::key(out, "over_50ms");
    json::number(out, f.intervals.over_50ms);
    out += '}';
    json::key(out, "polls");
    json::number(out, f.polls);
    json::key(out, "poll_gap_avg_us");
    json::number(out, f.polls ? f.poll_gap_total_us / f.polls : 0);
    json::key(out, "poll_gap_max_us");
    json::number(out, f.max_poll_gap_us);
    json::key(out, "multi_step");
    json::number(out, f.multi_step);
    const auto busiest = s.census.append_window(out, s.own);
    out += '}';
    event_log.write(out);
    const auto profile = sampler::take_event(t, toggles.load(), s.marker);
    if (!profile.empty()) event_log.write(profile);
    sampler::set_tracked(std::span<const DWORD>(busiest.data(), std::min<std::size_t>(busiest.size(), 4)));
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
    out += '}';
    event_log.write(out);
}

DWORD WINAPI worker(void*) noexcept {
    try {
        const auto dll_path = system_info::module_path(self_module);
        std::string sidecar;
        if (!read_small(dll_path + L".output-dir", sidecar)) {
            OutputDebugStringW(L"mhw_cpuopt: no .output-dir sidecar; idle\n");
            return 0;
        }
        const auto folder = trim(widen(sidecar));
        if (folder.empty() || !event_log.open(folder)) {
            OutputDebugStringW(L"mhw_cpuopt: run folder not writable; idle\n");
            return 0;
        }
        State s;
        s.control_path = folder + L"\\control.ini";
        event_log.write(system_info::start_event(self_module, s.control_path));
        poll_control(s, true);  // a run may start with switches already set
        system_info::ModuleCensus modules;
        if (auto e = modules.changed_event(now_ms()); !e.empty()) event_log.write(e);

        // The loader may run before the game's code is decrypted in memory, so
        // the signature search repeats once a second for up to two minutes.
        // The first attempt and the final one are logged.
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const double give_up = now_ms() + 120000;
        unsigned attempts = 0;
        const auto discover = [&]() -> bool {  // true when settled
            const auto found = frame_clock::discover(base);
            const bool settled = found.found || now_ms() >= give_up;
            if (++attempts == 1 || settled) event_log.write(frame_clock::discovery_event(found, now_ms(), attempts, settled));
            if (!found.found) return settled;
            frame_clock::start(base, found);
            for (int i = 0; i < 100 && !frame_clock::thread_id(); ++i) Sleep(10);
            return true;
        };
        bool discovering = !discover();
        sampler::start();
        for (int i = 0; i < 100 && !sampler::thread_id(); ++i) Sleep(10);
        // Our own threads are reported as such and never sampled.
        const auto own = [&] {
            s.own = {GetCurrentThreadId(), sampler::thread_id(), frame_clock::thread_id()};
            sampler::set_excluded(s.own);
        };
        own();

        std::string baseline;
        s.census.append_window(baseline, s.own);  // first figures need a starting point
        frame_clock::take();
        s.window_start = now_ms();
        for (unsigned tick = 0;; ++tick) {
            Sleep(100);
            if (discovering && tick % 10 == 9) {
                discovering = !discover();
                if (!discovering) own();  // the poller thread is ours now
            }
            if (tick % 3 == 0) poll_control(s, false);
            if (now_ms() - s.window_start >= s.window_ms) {
                write_window(s);
                if (auto e = modules.changed_event(now_ms()); !e.empty()) event_log.write(e);
            }
        }
    } catch (...) {
        event_log.write("{\"event\":\"stopped\",\"reason\":\"exception\"}");
    }
    return 0;
}
}  // namespace

void start(HMODULE self) noexcept {
    self_module = self;
    // Pinned: the threads run until the process ends, so the DLL never unloads.
    HMODULE pinned{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&start), &pinned);
    if (const auto thread = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr)) CloseHandle(thread);
}

void at_exit() noexcept {
    char line[160];
    const int n = std::snprintf(line, sizeof(line), "{\"event\":\"exit\",\"t_ms\":%.3f,\"windows\":%llu}", now_ms(),
                                static_cast<unsigned long long>(windows_written.load()));
    if (n > 0) event_log.write_at_exit(std::string_view(line, static_cast<std::size_t>(n)));
}
}  // namespace cpuopt::runtime
