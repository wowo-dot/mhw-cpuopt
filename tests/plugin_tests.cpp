// The plugin's in-process parts in a stand-in process:
// - frame_clock finds a planted copy of the frame-parity signature in this
//   executable, follows its operands to a fake renderer and times a counter
//   advanced every 4 ms;
// - the census names a busy thread as the busiest;
// - the sampler records whole stacks that contain the busy thread's functions.
#include "../src/core/toggles.hpp"
#include "../src/plugin/frame_clock.hpp"
#include "../src/plugin/log.hpp"
#include "../src/plugin/sampler.hpp"
#include "../src/plugin/system_info.hpp"
#include "../src/plugin/thread_census.hpp"
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {
int failures = 0;
#define CHECK(x)                                                         \
    do {                                                                 \
        if (!(x)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);     \
            ++failures;                                                  \
        }                                                                \
    } while (0)

// A planted copy of the signature in an executable section; the displacement
// is filled in at run time to point at fake_renderer.
#pragma section(".cpotest", read, execute)
__declspec(allocate(".cpotest")) unsigned char planted[] = {
    0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00,                    // mov rax,[rip+fake_renderer]
    0x49, 0xB9, 0xAB, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,  // mov r9,0AAAAAAAAAAAAAAABh
    0x4C, 0x8B, 0x80, 0x10, 0xF2, 0x01, 0x00,                    // mov r8,[rax+1F210h]
    0xC3};
std::uintptr_t fake_renderer;  // writable data, like the game's singleton

std::atomic<bool> stop{};
volatile std::uint64_t sink{};
__declspec(noinline) void busy_leaf() {
    volatile std::uint64_t local[16];
    for (unsigned i = 0; i < 4096; ++i) {
        local[i & 15] = sink;
        sink = sink * 6364136223846793005ULL + local[(i + 3) & 15];
    }
}
__declspec(noinline) void busy_outer() {
    while (!stop.load()) busy_leaf();
}
DWORD WINAPI busy_thread(void*) {
    busy_outer();
    return 0;
}

std::uint32_t rva_of(const void* p) {
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(p) - reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)));
}
// Value after "key": in a JSON line (numbers only).
long long number_after(const std::string& text, const std::string& key) {
    const auto at = text.find("\"" + key + "\":");
    return at == std::string::npos ? -1 : std::atoll(text.c_str() + at + key.size() + 3);
}

void frame_clock_test() {
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto site = reinterpret_cast<std::uintptr_t>(planted);
    const auto displacement = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(&fake_renderer) - static_cast<std::intptr_t>(site + 7));
    DWORD old{};
    VirtualProtect(planted, sizeof(planted), PAGE_EXECUTE_READWRITE, &old);
    std::memcpy(planted + 3, &displacement, 4);
    VirtualProtect(planted, sizeof(planted), old, &old);

    const auto d = cpuopt::frame_clock::discover(base);
    CHECK(d.found);
    CHECK(d.matches == 1);
    CHECK(d.site_rva == rva_of(planted));
    CHECK(d.singleton_rva == rva_of(&fake_renderer));
    CHECK(d.counter_offset == 0x1F210);
    CHECK(!d.expected_values);  // not the game

    auto renderer = std::make_unique<std::uint64_t[]>(0x20000 / 8);
    auto& counter = renderer[0x1F210 / 8];
    cpuopt::frame_clock::start(base, d);
    Sleep(50);
    CHECK(!cpuopt::frame_clock::take().renderer);  // no renderer yet
    fake_renderer = reinterpret_cast<std::uintptr_t>(renderer.get());
    Sleep(20);
    cpuopt::frame_clock::take();
    const auto t0 = cpuopt::qpc_now();
    for (int i = 0; i < 250; ++i) {
        // 4 ms frames, paced by QPC so Sleep granularity does not matter.
        const auto due = t0 + cpuopt::qpc_frequency() * (i + 1) * 4 / 1000;
        while (cpuopt::qpc_now() < due) Sleep(0);
        reinterpret_cast<std::atomic<std::uint64_t>&>(counter).fetch_add(1);
    }
    Sleep(5);
    const auto w = cpuopt::frame_clock::take();
    std::printf("frame_clock: frames %llu intervals %llu p50 %.2f ms p99 %.2f ms polls %llu gap avg %llu us max %llu us\n",
                static_cast<unsigned long long>(w.frames), static_cast<unsigned long long>(w.intervals.count), w.intervals.p50_us / 1000.0,
                w.intervals.p99_us / 1000.0, static_cast<unsigned long long>(w.polls),
                static_cast<unsigned long long>(w.polls ? w.poll_gap_total_us / w.polls : 0), static_cast<unsigned long long>(w.max_poll_gap_us));
    CHECK(w.renderer);
    CHECK(w.frames == 250);  // exact
    CHECK(w.intervals.count >= 240);
    CHECK(w.intervals.p50_us >= 3500 && w.intervals.p50_us <= 4600);
    fake_renderer = 0;
}

void census_and_sampler_test() {
    const auto busy = CreateThread(nullptr, 0, busy_thread, nullptr, 0, nullptr);
    const auto busy_id = GetThreadId(busy);
    cpuopt::ThreadCensus census;
    std::string first;
    const DWORD own[] = {GetCurrentThreadId()};
    census.append_window(first, own);
    cpuopt::sampler::start();
    for (int i = 0; i < 100 && !cpuopt::sampler::thread_id(); ++i) Sleep(10);
    const DWORD excluded[] = {GetCurrentThreadId(), cpuopt::frame_clock::thread_id(), cpuopt::sampler::thread_id()};
    cpuopt::sampler::set_excluded(excluded);
    Sleep(300);
    std::string window = "{";
    const auto busiest = census.append_window(window, own);
    window += '}';
    CHECK(!busiest.empty() && busiest[0] == busy_id);
    CHECK(window.find("\"cpu_busy\":[") != std::string::npos);

    cpuopt::sampler::set_tracked(busiest);
    cpuopt::sampler::take_event(0, 0, "");
    cpuopt::toggles.store(cpuopt::sample_profile);
    Sleep(500);
    cpuopt::toggles.store(0);
    Sleep(60);
    const auto e = cpuopt::sampler::take_event(1, 1, "test");
    stop.store(true);
    WaitForSingleObject(busy, INFINITE);
    CloseHandle(busy);
    const auto samples = number_after(e, "samples");
    std::printf("sampler: samples %lld ticks %lld bytes %zu\n", samples, number_after(e, "ticks"), e.size());
    CHECK(samples > 200);
    // The busy thread's stacks hold busy_leaf (innermost) and busy_outer.
    const auto modules = e.find("\"modules\":[");
    const auto exe = e.find("\"plugin_tests.exe\"", modules);
    CHECK(modules != std::string::npos && exe != std::string::npos);
    int id = 0;
    for (auto at = modules; (at = e.find("\",\"", at + 1)) < exe;) ++id;
    char leaf[48], outer[48];
    std::snprintf(leaf, sizeof(leaf), "[%d,%u]", id, rva_of(reinterpret_cast<const void*>(&busy_leaf)));
    std::snprintf(outer, sizeof(outer), "[%d,%u]", id, rva_of(reinterpret_cast<const void*>(&busy_outer)));
    CHECK(e.find(leaf) != std::string::npos);
    CHECK(e.find(outer) != std::string::npos);
    char thread[32];
    std::snprintf(thread, sizeof(thread), "[%lu,", static_cast<unsigned long>(busy_id));
    CHECK(e.find(thread, e.find("\"threads\":[")) != std::string::npos);
    CHECK(cpuopt::sampler::take_event(2, 0, "").empty());  // drained, nothing new while off
}

void start_event_test() {
    const auto e = cpuopt::system_info::start_event(GetModuleHandleW(nullptr), L"x");
    CHECK(e.front() == '{' && e.back() == '}');
    CHECK(e.find("\"brand\":\"") != std::string::npos);
    const auto at = e.find("\"sha256\":\"");
    CHECK(at != std::string::npos && e.find('"', at + 10) == at + 10 + 64);
    cpuopt::system_info::ModuleCensus modules;
    CHECK(modules.changed_event(0).find("plugin_tests.exe") != std::string::npos);
    CHECK(modules.changed_event(0).empty());  // unchanged
}
}  // namespace

int main() {
    frame_clock_test();
    census_and_sampler_test();
    start_event_test();
    std::printf(failures ? "plugin_tests: %d failure(s)\n" : "plugin_tests: ok\n", failures);
    return failures ? 1 : 0;
}
