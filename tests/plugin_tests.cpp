// The plugin's in-process parts in a stand-in process:
// - frame_clock finds a planted copy of the frame-parity signature in this
//   executable, follows its operands to a fake renderer and times a counter
//   advanced every 4 ms;
// - the census names a busy thread as the busiest;
// - the sampler records whole stacks that contain the busy thread's functions.
#include "../src/core/toggles.hpp"
#include "../src/plugin/frame_clock.hpp"
#include "../src/plugin/image_scan.hpp"
#include "../src/plugin/frame_events.hpp"
#include "../src/plugin/job_cap.hpp"
#include "../src/plugin/spin_probe.hpp"
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
// A second copy whose displacement points elsewhere: matches that disagree.
__declspec(allocate(".cpotest")) unsigned char planted_other[] = {
    0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00,
    0x49, 0xB9, 0xAB, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
    0x4C, 0x8B, 0x80, 0x10, 0xF2, 0x01, 0x00,
    0xC3};
// The job runner's prologue: mov r8,[rip+fake_smain] at +21.
__declspec(allocate(".cpotest")) unsigned char planted_runner[] = {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x41, 0x56,
    0x48, 0x83, 0xEC, 0x20, 0x4C, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xF1, 0x4C, 0x8B, 0x31, 0xC3};
std::uintptr_t fake_smain;
std::uintptr_t fake_renderer;  // writable data, like the game's singleton
std::uintptr_t other_renderer;

void plant(unsigned char* site, const void* target) {
    const auto displacement =
        static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(target) - static_cast<std::intptr_t>(reinterpret_cast<std::uintptr_t>(site) + 7));
    DWORD old{};
    VirtualProtect(site, 24, PAGE_EXECUTE_READWRITE, &old);
    std::memcpy(site + 3, &displacement, 4);
    VirtualProtect(site, 24, old, &old);
}

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
    using namespace cpuopt::frame_clock;
    plant(planted, &fake_renderer);
    plant(planted_other, &other_renderer);
    std::uintptr_t matches[match_capacity]{};
    CHECK(scan(base, matches, match_capacity) == 2);  // both planted copies, in address order
    const std::uintptr_t first[] = {reinterpret_cast<std::uintptr_t>(planted)};
    const std::uintptr_t both[] = {reinterpret_cast<std::uintptr_t>(planted), reinterpret_cast<std::uintptr_t>(planted_other)};
    const std::uintptr_t same[] = {first[0], first[0]};
    CHECK(std::string(discover(base, both, 2).reason) == "matches disagree");
    CHECK(std::string(discover(base, first, 0).reason) == "signature not found");
    CHECK(std::string(discover(base, first, match_capacity + 1).reason) == "too many matches");
    const std::uintptr_t outside[] = {1};
    CHECK(std::string(discover(base, outside, 1).reason) == "match unreadable");
    CHECK(discover(base, same, 2).found);  // repeated sites that agree

    const auto d = discover(base, first, 1);
    CHECK(d.found);
    CHECK(d.matches == 1);
    CHECK(d.site_rva == rva_of(planted));
    CHECK(d.singleton_rva == rva_of(&fake_renderer));
    CHECK(d.counter_offset == 0x1F210);
    CHECK(!d.expected_values);  // not the game

    auto renderer = std::make_unique<std::uint64_t[]>(0x20000 / 8);
    auto& counter = renderer[0x1F210 / 8];
    std::uintptr_t validated{};
    std::uint64_t value{};
    CHECK(!read(validated, value));  // no target yet
    set_target(base, d);
    CHECK(!read(validated, value));  // no renderer yet
    start_poller();
    CHECK(poller_thread() != 0);
    Sleep(50);
    CHECK(!cpuopt::frame_clock::take().renderer);  // no renderer yet
    fake_renderer = reinterpret_cast<std::uintptr_t>(renderer.get());
    CHECK(read(validated, value) && value == 0 && validated == fake_renderer);
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
    CHECK(read(validated, value) && value == 250);
    stop_poller();
    fake_renderer = 0;
}

// Synthetic callbacks with known time stamps: 4 ms frames with 1.5 ms of
// update, Presents 4 ms apart taking 0.2 ms, the render counter one ahead.
void frame_events_test() {
    namespace fe = cpuopt::frame_events;
    fe::take();
    const auto f = cpuopt::qpc_frequency();
    const auto at = [f](double ms) { return static_cast<std::int64_t>(1000 + ms * double(f) / 1000.0); };
    for (int i = 0; i < 100; ++i) {
        fe::on_begin(at(i * 4.0), 77);
        fe::on_render_counter(1000 + static_cast<std::uint64_t>(i));
        fe::on_end(at(i * 4.0 + 1.5));
        fe::on_present(at(i * 4.0 + 2.0), i ? at((i - 1) * 4.0 + 2.0) : 0, 88);
        fe::on_present_returned(at(i * 4.0 + 2.0), at(i * 4.0 + 2.2));
    }
    const auto w = fe::take();
    CHECK(w.updates == 100 && w.presents == 100);
    // Upper bucket edges: 50 us buckets for intervals, 10 us for durations.
    CHECK(w.update_interval.count == 99 && w.update_interval.p50_us >= 4000 && w.update_interval.p50_us <= 4050);
    CHECK(w.update_interval.max_us >= 3999 && w.update_interval.max_us <= 4001);
    CHECK(w.update_work.count == 100 && w.update_work.p50_us >= 1500 && w.update_work.p50_us <= 1510);
    CHECK(w.update_work.total_us >= 149900 && w.update_work.total_us <= 150100);
    CHECK(w.present_interval.count == 99 && w.present_interval.p99_us >= 4000 && w.present_interval.p99_us <= 4050);
    CHECK(w.present_call.count == 100 && w.present_call.p50_us >= 200 && w.present_call.p50_us <= 210);
    CHECK(w.render_seen && w.render_frames == 99 && w.render_counter == 1099);
    CHECK(w.game_thread == 77 && w.present_thread == 88);
    std::string out = "{";
    fe::append(out, w, 400.0);
    out += '}';
    CHECK(out.find("\"update\":{\"frames\":100,\"fps\":250.00,\"thread\":77,\"interval\":{\"count\":99,") != std::string::npos);
    CHECK(out.find("\"render\":{\"seen\":true,\"frames\":99,\"counter\":1099}") != std::string::npos);
    CHECK(fe::take().updates == 0);  // take() resets
}

// job_cap against a stand-in sMain: the game's own count learned once the job
// threads exist, caps written to +0xD0 and +0xCC, the native count back when
// off, clamping, and the cases where it must keep its hands off.
// spin_probe against a stand-in sMain: busy slots and their job functions.
void spin_probe_test() {
    namespace sp = cpuopt::spin_probe;
    auto object = std::make_unique<std::uint8_t[]>(sp::slots_offset + sp::max_slots * sp::slot_stride);
    std::memset(object.get(), 0, sp::slots_offset + sp::max_slots * sp::slot_stride);
    const auto smain = reinterpret_cast<std::uintptr_t>(object.get());
    const auto slot_job = [&](unsigned i) -> std::uintptr_t& {
        return *reinterpret_cast<std::uintptr_t*>(object.get() + sp::slots_offset + i * sp::slot_stride + sp::slot_job);
    };
    std::uintptr_t job_a[4] = {0, 0x1111, 0, 0}, job_b[4] = {0, 0x2222, 0, 0};  // context, function, ...
    sp::Reading r;
    CHECK(sp::read(smain, r) && r.busy == 0);
    slot_job(3) = reinterpret_cast<std::uintptr_t>(job_a);
    slot_job(31) = reinterpret_cast<std::uintptr_t>(job_b);
    CHECK(sp::read(smain, r) && r.busy == 2 && r.functions[0] == 0x1111 && r.functions[1] == 0x2222);
    slot_job(7) = 0x10;  // an unreadable entry still counts as busy, function unknown
    CHECK(sp::read(smain, r) && r.busy == 3 && r.functions[1] == 0);
    CHECK(!sp::read(0, r));
}

void job_cap_test() {
    namespace jc = cpuopt::job_cap;
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    {
        const auto site = reinterpret_cast<std::uintptr_t>(planted_runner) + jc::instance_operand;
        const auto displacement = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(&fake_smain) - static_cast<std::intptr_t>(site + 7));
        DWORD old{};
        VirtualProtect(planted_runner, sizeof(planted_runner), PAGE_EXECUTE_READWRITE, &old);
        std::memcpy(planted_runner + jc::instance_operand + 3, &displacement, 4);
        VirtualProtect(planted_runner, sizeof(planted_runner), old, &old);
    }
    std::uintptr_t found[2]{};
    CHECK(cpuopt::image_scan::find(base, jc::instance_signature, found, 2) == 1 && found[0] == reinterpret_cast<std::uintptr_t>(planted_runner));
    const unsigned missing[4] = {1, 0, 1, 1}, twice[4] = {1, 1, 2, 1}, all[4] = {1, 1, 1, 1};
    const auto site = reinterpret_cast<std::uintptr_t>(planted_runner);
    CHECK(std::string(jc::discover(base, missing, site).reason) == "signature not found");
    CHECK(std::string(jc::discover(base, twice, site).reason) == "signature not unique");
    CHECK(std::string(jc::discover(base, all, reinterpret_cast<std::uintptr_t>(planted)).reason) == "operand changed");
    const auto d = jc::discover(base, all, site);
    CHECK(d.found && d.instance_rva == rva_of(planted_runner) && d.global_rva == rva_of(&fake_smain) && !d.expected_values);

    auto object = std::make_unique<std::uint8_t[]>(jc::detected_offset + 64);
    std::memset(object.get(), 0, jc::detected_offset + 64);
    const auto field = [&](std::uint32_t offset) -> std::uint32_t& { return *reinterpret_cast<std::uint32_t*>(object.get() + offset); };
    const auto state = [] {
        std::string out = "{";
        jc::append(out);
        const auto at = out.find("\"state\":\"") + 9;
        return out.substr(at, out.find('"', at) - at);
    };
    const auto game = [&](std::uint32_t detected, std::uint32_t configured, std::uint8_t created) {
        field(jc::detected_offset) = detected;
        field(jc::configured_offset) = field(jc::participants_offset) = configured;
        object[jc::threads_created_offset] = created;
    };

    jc::set_target(base, d);
    jc::request(16);
    jc::on_begin();
    CHECK(state() == "waiting");  // no sMain yet
    fake_smain = reinterpret_cast<std::uintptr_t>(object.get());
    game(32, 31, 0);
    jc::on_begin();
    CHECK(state() == "waiting" && field(jc::configured_offset) == 31);  // job threads not created yet
    object[jc::threads_created_offset] = 1;
    jc::on_begin();
    CHECK(state() == "capped" && field(jc::configured_offset) == 16 && field(jc::participants_offset) == 16);
    jc::request(8);
    jc::on_begin();
    CHECK(field(jc::configured_offset) == 8 && field(jc::participants_offset) == 8);
    jc::request(0);
    jc::on_begin();
    CHECK(state() == "native" && field(jc::configured_offset) == 31 && field(jc::participants_offset) == 31);
    jc::request(40);  // more than the game's own count: the game's own count
    jc::on_begin();
    CHECK(state() == "native" && field(jc::configured_offset) == 31);
    jc::request(12);
    jc::on_begin();
    CHECK(field(jc::configured_offset) == 12);
    jc::restore();
    CHECK(state() == "restored" && field(jc::configured_offset) == 31 && field(jc::participants_offset) == 31);
    jc::request(5);
    jc::on_begin();
    CHECK(field(jc::configured_offset) == 31);  // restored is final

    // Something else changed the count: hands off for good.
    jc::set_target(base, d);
    game(32, 31, 1);
    jc::request(16);
    jc::on_begin();
    CHECK(field(jc::configured_offset) == 16);
    field(jc::configured_offset) = 20;
    jc::on_begin();
    CHECK(state() == "drift");
    jc::request(8);
    jc::on_begin();
    CHECK(field(jc::configured_offset) == 20);

    // Fields that do not follow the start-up formula: never written.
    jc::set_target(base, d);
    game(32, 30, 1);
    jc::request(16);
    jc::on_begin();
    CHECK(state() == "layout mismatch" && field(jc::configured_offset) == 30);

    // A 3-thread CPU keeps all three (no minus one below 4).
    jc::set_target(base, d);
    game(3, 3, 1);
    jc::request(2);
    jc::on_begin();
    CHECK(state() == "capped" && field(jc::configured_offset) == 2);
    jc::restore();
    CHECK(field(jc::configured_offset) == 3);
    fake_smain = 0;
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
    const DWORD excluded[] = {GetCurrentThreadId(), cpuopt::frame_clock::poller_thread(), cpuopt::sampler::thread_id()};
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
    const cpuopt::system_info::LoaderInfo loader{"Test Loader", 0x10008, "421810", "ab"};
    const auto e = cpuopt::system_info::start_event(GetModuleHandleW(nullptr), L"x", loader);
    CHECK(e.find("\"loader\":{\"name\":\"Test Loader\",\"api\":\"1.8\",\"game_version\":\"421810\",\"exe_sha256\":\"ab\"}") != std::string::npos);
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
    frame_events_test();
    job_cap_test();
    spin_probe_test();
    census_and_sampler_test();
    start_event_test();
    std::printf(failures ? "plugin_tests: %d failure(s)\n" : "plugin_tests: ok\n", failures);
    return failures ? 1 : 0;
}
