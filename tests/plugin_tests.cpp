// The plugin's in-process parts in a stand-in process:
// - frame_clock finds a planted copy of the frame-parity signature in this
//   executable, follows its operands to a fake renderer and times a counter
//   advanced every 4 ms;
// - the census names a busy thread as the busiest;
// - the sampler records whole stacks that contain the busy thread's functions;
// - prim_stores' replacement loop writes what the game's loop writes (both run
//   here as machine code on the same inputs);
// - server_time answers from a kept read as the game's two functions would;
// - inline rounds run small rounds with participants 1 once learned;
// - spinning job threads (worker_spin) keep executeJob's round accounting on
//   a stand-in job system, and its jump stub and patch bytes are sound.
#include "../src/core/toggles.hpp"
#include "../src/plugin/frame_clock.hpp"
#include "../src/plugin/image_scan.hpp"
#include "../src/plugin/frame_events.hpp"
#include "../src/plugin/job_cap.hpp"
#include "../src/plugin/spin_probe.hpp"
#include "../src/plugin/job_timing.hpp"
#include "../src/plugin/log.hpp"
#include "../src/plugin/prim_stores.hpp"
#include "../src/plugin/sampler.hpp"
#include "../src/plugin/server_time.hpp"
#include "../src/plugin/system_info.hpp"
#include "../src/plugin/thread_census.hpp"
#include "../src/plugin/worker_spin.hpp"
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <random>
#include <sstream>
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

// spin_probe against a stand-in sMain's started and done counters.
// job_timing: the round analysis on hand-made records, then the detour on a
// stand-in executeJob that runs a list's jobs in order on this thread.
std::uintptr_t timing_seen_context[8], timing_seen_rdx[8];
__declspec(noinline) std::uintptr_t timing_job(std::uintptr_t context, std::uintptr_t rdx, std::uintptr_t, std::uintptr_t) {
    const auto i = *reinterpret_cast<const std::uintptr_t*>(context + 8);  // object: vtable, index
    timing_seen_context[i] = context;
    timing_seen_rdx[i] = rdx;
    const auto due = cpuopt::qpc_now() + cpuopt::qpc_frequency() * static_cast<std::int64_t>(i + 1) / 10000;  // (i+1) * 0.1 ms
    while (cpuopt::qpc_now() < due) {}
    return i;
}
void __fastcall stand_in_execute_job(std::uintptr_t smain, int, std::uintptr_t jobs, std::uint32_t count, std::uintptr_t) {
    const auto list = jobs ? jobs : smain + cpuopt::job_timing::default_jobs_offset;
    const auto n = jobs ? count : *reinterpret_cast<const std::uint32_t*>(smain + cpuopt::job_timing::job_count_offset);
    for (std::uint32_t i = 0; i < n; ++i) {
        const auto* entry = reinterpret_cast<const std::uintptr_t*>(list + 32ULL * i);
        reinterpret_cast<std::uintptr_t (*)(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t)>(entry[1])(entry[0], 0xAA, 0, 0);
    }
}
const std::uintptr_t timing_vtable[2] = {};  // in this exe's data: stands in for a game class
__declspec(noinline) std::uintptr_t timing_noop(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t) { return 0; }
__declspec(noinline) void timing_round(std::uintptr_t smain, std::uintptr_t list, std::uint32_t n) {
    cpuopt::job_timing::detour(smain, 1, list, n, 0);
}

void job_timing_test() {
    namespace jt = cpuopt::job_timing;
    // Analysis: three jobs on two lanes; the last to end is job 2.
    const jt::Record recs[3] = {{0, 0, 100, 400, 1}, {0, 0, 110, 210, 2}, {0, 0, 220, 900, 2}};
    const auto r = jt::analyse(recs, 3, 90, 950, 2);
    CHECK(r.jobs == 3 && r.duration == 860 && r.work == 300 + 100 + 680 && r.longest == 680);
    CHECK(r.bound == 680);  // the longest job beats work / 2 = 540
    CHECK(r.last == 2 && r.last_start == 130 && r.last_duration == 680 && r.threads_used == 2);
    const jt::Record not_run[1] = {};
    CHECK(jt::analyse(not_run, 1, 0, 10, 4).work == 0);

    // The detour on a stand-in executeJob.
    *jt::next_slot() = &stand_in_execute_job;
    std::uintptr_t objects[4][2];
    std::uintptr_t list[4][4];
    for (std::uintptr_t i = 0; i < 4; ++i) {
        objects[i][0] = reinterpret_cast<std::uintptr_t>(timing_vtable);
        objects[i][1] = i;
        list[i][0] = reinterpret_cast<std::uintptr_t>(objects[i]);
        list[i][1] = reinterpret_cast<std::uintptr_t>(&timing_job);
        list[i][2] = 0x5000 + i;
        list[i][3] = 0x6000 + i;
    }
    std::uintptr_t before[4][4];
    std::memcpy(before, list, sizeof(list));
    auto smain = std::make_unique<std::uint8_t[]>(0x200);
    std::memset(smain.get(), 0, 0x200);
    *reinterpret_cast<std::uint32_t*>(smain.get() + jt::participants_offset) = 2;
    const auto smain_address = reinterpret_cast<std::uintptr_t>(smain.get());

    jt::take_event(0, reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)));
    jt::set_measuring(false);
    timing_round(smain_address, reinterpret_cast<std::uintptr_t>(list), 4);  // passes straight through
    CHECK(jt::take_event(0, reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr))).empty());
    jt::set_trace_every(1);  // every timed round traced
    jt::set_measuring(true);
    std::memset(timing_seen_context, 0, sizeof(timing_seen_context));
    timing_round(smain_address, reinterpret_cast<std::uintptr_t>(list), 4);
    CHECK(std::memcmp(before, list, sizeof(list)) == 0);  // every entry back as it was
    bool forwarded = true;
    for (unsigned i = 0; i < 4; ++i)
        forwarded = forwarded && timing_seen_context[i] == reinterpret_cast<std::uintptr_t>(objects[i]) && timing_seen_rdx[i] == 0xAA;
    CHECK(forwarded);
    // sMain's own list (no list passed): count from +0xB0, list at +0xF0.
    auto own = std::make_unique<std::uint8_t[]>(jt::default_jobs_offset + 32 * 4);
    std::memset(own.get(), 0, jt::default_jobs_offset + 32 * 4);
    std::memcpy(own.get() + jt::default_jobs_offset, list, sizeof(list));
    *reinterpret_cast<std::uint32_t*>(own.get() + jt::job_count_offset) = 4;
    *reinterpret_cast<std::uint32_t*>(own.get() + jt::participants_offset) = 2;
    timing_round(reinterpret_cast<std::uintptr_t>(own.get()), 0, 0);
    CHECK(std::memcmp(own.get() + jt::default_jobs_offset, list, sizeof(list)) == 0);
    // Too many jobs, or a list that cannot be written: untouched, counted as skipped.
    std::vector<std::uintptr_t> big(4 * (jt::capacity + 1));
    for (std::size_t i = 0; i < jt::capacity + 1; ++i) {
        big[4 * i] = reinterpret_cast<std::uintptr_t>(objects[0]);
        big[4 * i + 1] = reinterpret_cast<std::uintptr_t>(&timing_noop);
    }
    const auto big_before = big;
    timing_round(smain_address, reinterpret_cast<std::uintptr_t>(big.data()), jt::capacity + 1);
    CHECK(big == big_before);
    auto* page = static_cast<std::uintptr_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    page[0] = reinterpret_cast<std::uintptr_t>(objects[0]);
    page[1] = reinterpret_cast<std::uintptr_t>(&timing_noop);
    DWORD old{};
    VirtualProtect(page, 4096, PAGE_READONLY, &old);
    timing_round(smain_address, reinterpret_cast<std::uintptr_t>(page), 1);
    VirtualFree(page, 0, MEM_RELEASE);
    jt::set_measuring(false);

    const auto e = jt::take_event(1, reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)));
    const auto exe = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    char job[96];
    const auto at = e.find("\"callers\":[[");
    const auto caller_rva = std::strtoull(e.c_str() + at + 12, nullptr, 10);
    const auto caller_fn = rva_of(reinterpret_cast<const void*>(&timing_round));
    std::snprintf(job, sizeof(job), ",%u,%llu,8,", rva_of(reinterpret_cast<const void*>(&timing_job)),
                  static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(timing_vtable) - exe));
    std::printf("job_timing: %s\n", e.substr(0, 300).c_str());
    CHECK(e.find("\"rounds\":2,\"skipped\":2,") != std::string::npos);
    CHECK(caller_rva > caller_fn && caller_rva < caller_fn + 64);  // the return address inside timing_round
    CHECK(e.find(job) != std::string::npos);  // (timing_job, timing_vtable): 4 jobs in each of 2 rounds
    // Both timed rounds traced: 4 jobs in list order, all on this thread
    // (thread 0, the game thread here), each about (i+1) * 0.1 ms.
    const auto traces = jt::take_traces(exe);
    std::size_t lines = 0;
    for (auto line = traces.find("{\"event\":\"job_trace\""); line != std::string::npos; line = traces.find("{\"event\":\"job_trace\"", line + 1)) ++lines;
    std::printf("job_trace: %zu rounds, first: %s\n", lines, traces.substr(0, traces.find('\n')).c_str());
    CHECK(lines == 2);
    char first_job[64];
    std::snprintf(first_job, sizeof(first_job), "\"jobs\":[[%llu,%u,", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(timing_vtable) - exe),
                  rva_of(reinterpret_cast<const void*>(&timing_job)));
    CHECK(traces.find(first_job) != std::string::npos);
    CHECK(traces.find("\"game_thread\":0,\"threads\":1,\"dropped\":0}") != std::string::npos);
    CHECK(jt::take_traces(exe).empty());  // drained
    jt::set_trace_every(499);
}

// Job order on a stand-in executeJob that runs the list in order on this
// thread: reversed, learned (the slow class first), restored after each round,
// and only for the camera round's caller.
const std::uintptr_t slow_class[2] = {}, fast_class[2] = {};
std::uintptr_t ran_order[16];
unsigned ran_count;
__declspec(noinline) std::uintptr_t order_job(std::uintptr_t context, std::uintptr_t, std::uintptr_t, std::uintptr_t) {
    const auto* object = reinterpret_cast<const std::uintptr_t*>(context);  // vtable, id
    if (ran_count < 16) ran_order[ran_count++] = object[1];
    if (object[0] == reinterpret_cast<std::uintptr_t>(slow_class)) {
        const auto due = cpuopt::qpc_now() + cpuopt::qpc_frequency() * 3 / 10000;  // 0.3 ms
        while (cpuopt::qpc_now() < due) {}
    }
    return 0;
}
__declspec(noinline) void camera_round(std::uintptr_t smain, std::uintptr_t list, std::uint32_t n) {
    cpuopt::job_timing::detour(smain, 1, list, n, 0);
}
volatile unsigned other_rounds;  // keeps the linker from folding other_round into camera_round
__declspec(noinline) void other_round(std::uintptr_t smain, std::uintptr_t list, std::uint32_t n) {
    other_rounds = other_rounds + 1;
    cpuopt::job_timing::detour(smain, 1, list, n, 0);
}

void job_order_test() {
    namespace jt = cpuopt::job_timing;
    const auto exe = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    *jt::next_slot() = &stand_in_execute_job;
    auto smain = std::make_unique<std::uint8_t[]>(0x200);
    std::memset(smain.get(), 0, 0x200);
    *reinterpret_cast<std::uint32_t*>(smain.get() + jt::participants_offset) = 2;
    const auto s = reinterpret_cast<std::uintptr_t>(smain.get());
    // Five jobs: ids 0..4, job 3 of the slow class.
    std::uintptr_t objects[5][2], list[5][4];
    for (std::uintptr_t i = 0; i < 5; ++i) {
        objects[i][0] = reinterpret_cast<std::uintptr_t>(i == 3 ? slow_class : fast_class);
        objects[i][1] = i;
        list[i][0] = reinterpret_cast<std::uintptr_t>(objects[i]);
        list[i][1] = reinterpret_cast<std::uintptr_t>(&order_job);
        list[i][2] = list[i][3] = 0;
    }
    std::uintptr_t before[5][4];
    std::memcpy(before, list, sizeof(list));
    const auto run = [&](auto round) {
        ran_count = 0;
        round(s, reinterpret_cast<std::uintptr_t>(list), 5);
        return std::to_string(ran_order[0]) + std::to_string(ran_order[1]) + std::to_string(ran_order[2]) + std::to_string(ran_order[3]) +
               std::to_string(ran_order[4]);
    };
    // The camera round's return address: from one timed round's caller.
    jt::take_event(0, exe);
    jt::set_trace_every(0);
    jt::set_measuring(true);
    run(camera_round);
    jt::set_measuring(false);
    const auto e = jt::take_event(0, exe);
    const auto caller_rva = std::strtoull(e.c_str() + e.find("\"callers\":[[") + 12, nullptr, 10);
    CHECK(caller_rva > rva_of(reinterpret_cast<const void*>(&camera_round)));
    CHECK(!jt::set_order(1, 20));  // no camera site yet: unavailable
    jt::set_camera_return(exe + caller_rva);

    CHECK(run(camera_round) == "01234");  // switch off
    CHECK(jt::set_order(1, 20));
    CHECK(run(camera_round) == "43210");  // reversed
    CHECK(std::memcmp(before, list, sizeof(list)) == 0);  // the game's order back
    CHECK(run(other_round) == "01234");  // not the camera round
    CHECK(jt::set_order(2, 100));
    CHECK(run(camera_round) == "01234");  // nothing learned yet
    for (unsigned i = 0; i < jt::learn_every * 5; ++i) run(camera_round);  // five learning rounds
    CHECK(run(camera_round) == "30124");  // the slow class first, the rest in order
    CHECK(std::memcmp(before, list, sizeof(list)) == 0);
    CHECK(run(other_round) == "01234");
    std::string out = "{";
    jt::append_order(out);
    std::printf("job_order: %s}\n", out.c_str());
    CHECK(out.find("\"mode\":2,\"available\":true,\"threshold_us\":100,") != std::string::npos);
    CHECK(out.find("\"timed\":5,") != std::string::npos && out.find("\"classes\":2") != std::string::npos);
    jt::set_order(0, 20);
    CHECK(run(camera_round) == "01234");
    jt::set_trace_every(499);
}

// The signature's bytes as numbers; -1 for a wildcard.
std::vector<int> signature_bytes(std::string_view text) {
    std::vector<int> out;
    std::istringstream in{std::string(text)};
    for (std::string token; in >> token;) out.push_back(token == "??" ? -1 : std::stoi(token, nullptr, 16));
    return out;
}

// prim_stores: the game's preheader and loop (from the signature) and the
// replacement, each wrapped in a function that loads the registers from a
// struct, runs the loop and stores the registers that live on.
struct PrimRegs {
    std::uint64_t rbp, rbx, r12, r13, r14, r8, rdx;  // inputs
    std::uint64_t out_rdx, out_rcx, out_rax, out_r14, out_r11, out_r8;
};
using PrimFn = void (*)(PrimRegs*);
PrimFn make_prim(const std::vector<std::uint8_t>& snippet) {
    std::vector<std::uint8_t> code = {
        0x53, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,  // push rbx, rbp, r12, r13, r14, r15
        0x49, 0x89, 0xCF,                                            // mov r15,rcx
        0x49, 0x8B, 0x2F,                                            // mov rbp,[r15]
        0x49, 0x8B, 0x5F, 0x08,                                      // mov rbx,[r15+8]
        0x4D, 0x8B, 0x67, 0x10,                                      // mov r12,[r15+10h]
        0x4D, 0x8B, 0x6F, 0x18,                                      // mov r13,[r15+18h]
        0x4D, 0x8B, 0x77, 0x20,                                      // mov r14,[r15+20h]
        0x4D, 0x8B, 0x47, 0x28,                                      // mov r8,[r15+28h]
        0x49, 0x8B, 0x57, 0x30,                                      // mov rdx,[r15+30h]
        0x33, 0xC9};                                                 // xor ecx,ecx (0x2570BE7)
    code.insert(code.end(), snippet.begin(), snippet.end());
    const std::uint8_t tail[] = {
        0x49, 0x89, 0x57, 0x38,  // mov [r15+38h],rdx
        0x49, 0x89, 0x4F, 0x40,  // mov [r15+40h],rcx
        0x49, 0x89, 0x47, 0x48,  // mov [r15+48h],rax
        0x4D, 0x89, 0x77, 0x50,  // mov [r15+50h],r14
        0x4D, 0x89, 0x5F, 0x58,  // mov [r15+58h],r11
        0x4D, 0x89, 0x47, 0x60,  // mov [r15+60h],r8
        0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C, 0x5D, 0x5B, 0xC3};
    code.insert(code.end(), std::begin(tail), std::end(tail));
    auto* memory = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    std::memcpy(memory, code.data(), code.size());
    DWORD old{};
    VirtualProtect(memory, 4096, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), memory, 4096);
    return reinterpret_cast<PrimFn>(memory);
}
std::uintptr_t fake_primitive_global;

void prim_stores_test() {
    namespace ps = cpuopt::prim_stores;
    // The signature holds the bytes the change replaces, and no wildcard from
    // the preheader on.
    const auto sig = signature_bytes(ps::signature);
    CHECK(sig.size() == ps::patch_offset + ps::patch_length);
    bool same = true, fixed = true;
    for (unsigned i = 0; i < ps::patch_length; ++i) same = same && sig[ps::patch_offset + i] == ps::expected[i];
    for (unsigned i = ps::preheader; i < sig.size(); ++i) fixed = fixed && sig[i] >= 0;
    CHECK(same && fixed);
    std::vector<std::uint8_t> original, replaced;
    for (unsigned i = ps::preheader; i < sig.size(); ++i) original.push_back(static_cast<std::uint8_t>(sig[i]));
    replaced.assign(original.begin(), original.begin() + (ps::patch_offset - ps::preheader));
    replaced.insert(replaced.end(), ps::replacement.begin(), ps::replacement.end());
    CHECK(replaced.size() == original.size());
    const auto game = make_prim(original), ours = make_prim(replaced);

    std::mt19937_64 rng(20261015);
    bool equal = true, independent = true;
    unsigned cases = 0;
    for (const unsigned count : {1u, 2u, 3u, 17u, 300u, 4096u}) {
        for (int round = 0; round < 8; ++round, ++cases) {
            const std::size_t bytes = 16ULL * count + 64;
            std::vector<std::uint8_t> a(bytes), b(bytes);
            for (auto& x : a) x = static_cast<std::uint8_t>(rng());
            for (auto& x : b) x = static_cast<std::uint8_t>(rng());
            const auto a_tail = std::vector<std::uint8_t>(a.end() - 64, a.end()), b_tail = std::vector<std::uint8_t>(b.end() - 64, b.end());
            PrimRegs in{};
            in.rbx = rng();
            in.r12 = rng() & 0x0FFFFFFF;  // a vertex offset
            in.r13 = rng() & 0x0FFFFFFF;
            in.r14 = rng();
            in.r8 = count;
            in.rdx = rng() % 3;
            PrimRegs x = in, y = in;
            x.rbp = reinterpret_cast<std::uint64_t>(a.data());
            y.rbp = reinterpret_cast<std::uint64_t>(b.data());
            game(&x);
            ours(&y);
            equal = equal && std::memcmp(a.data(), b.data(), 16ULL * count) == 0 && x.out_rdx == y.out_rdx && x.out_rcx == y.out_rcx &&
                    x.out_r14 == y.out_r14 && x.out_r11 == y.out_r11 && x.out_r8 == y.out_r8 &&
                    x.out_rax - x.rbp == y.out_rax - y.rbp;
            independent = independent && std::equal(a_tail.begin(), a_tail.end(), a.end() - 64) && std::equal(b_tail.begin(), b_tail.end(), b.end() - 64);
            // The first record as the header says.
            std::uint32_t d0{}, d1{};
            std::memcpy(&d0, a.data(), 4);
            std::memcpy(&d1, a.data() + 4, 4);
            equal = equal && d0 == (static_cast<std::uint32_t>(in.rdx) | static_cast<std::uint32_t>(in.r12) << 4) &&
                    d1 == ((static_cast<std::uint32_t>(in.r14) & 0xF) | static_cast<std::uint32_t>(in.r13) << 4);
        }
    }
    std::printf("prim_stores: %u cases, same records and registers: %s\n", cases, equal ? "yes" : "NO");
    CHECK(equal && independent);

    // discover and probe on a planted copy: the global points at a fake
    // sPrimitive whose ring is write-combined memory.
    static unsigned char planted_prim[256];
    for (std::size_t i = 0; i < sig.size(); ++i) planted_prim[i] = static_cast<unsigned char>(sig[i] < 0 ? 0 : sig[i]);
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto site = reinterpret_cast<std::uintptr_t>(planted_prim);
    const auto displacement = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(&fake_primitive_global) -
                                                        static_cast<std::intptr_t>(site + ps::global_operand + 7));
    std::memcpy(planted_prim + ps::global_operand + 3, &displacement, 4);
    const std::uintptr_t one[1] = {site}, two[2] = {site, site};
    CHECK(std::string(ps::discover(base, one, 0).reason) == "signature not found");
    CHECK(std::string(ps::discover(base, two, 2).reason) == "signature not unique");
    const auto d = ps::discover(base, one, 1);
    CHECK(d.found && d.site_rva == rva_of(planted_prim) && d.global_rva == rva_of(&fake_primitive_global));
    planted_prim[ps::patch_offset + 3] ^= 1;
    CHECK(std::string(ps::discover(base, one, 1).reason) == "changed bytes differ");
    planted_prim[ps::patch_offset + 3] ^= 1;
    CHECK(!ps::probe(base + d.global_rva).object);  // no sPrimitive yet
    auto object = std::make_unique<std::uint8_t[]>(0x200);
    std::memset(object.get(), 0, 0x200);
    fake_primitive_global = reinterpret_cast<std::uintptr_t>(object.get());
    CHECK(ps::probe(base + d.global_rva).object && !ps::probe(base + d.global_rva).pointer);  // not mapped
    auto* ring = VirtualAlloc(nullptr, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE | PAGE_WRITECOMBINE);
    CHECK(ring != nullptr);
    *reinterpret_cast<void**>(object.get() + ps::ring_offset) = ring;
    const auto b = ps::probe(base + d.global_rva);
    CHECK(b.pointer && b.write_combined());
    std::printf("prim_stores: %s\n", ps::buffer_event(b, 0).c_str());
    CHECK(ps::buffer_event(b, 0).find("\"write_combined\":true") != std::string::npos);
    VirtualFree(ring, 0, MEM_RELEASE);
    fake_primitive_global = 0;
}

// server_time: stand-ins for the game's two functions, a session object and
// the date flag's pointer chain.
std::uint32_t steam_now = 1760000000;  // 2025-10-09 08:53:20 UTC
std::uint32_t session_state = 2;
unsigned native_queries, native_dates;
__declspec(noinline) std::uint32_t __fastcall stand_in_state(void*) { return session_state; }
void* session_vtable[16];
struct StandInSession {
    void** vtable;
} stand_in_session{session_vtable};
std::uintptr_t fake_flag_global;
char __fastcall stand_in_query(void* session, std::uint64_t* out) {
    ++native_queries;
    if (!out) return 0;
    if (stand_in_state(session) != 2) {
        *out = 12345;  // local time
        return 0;
    }
    *out = steam_now;
    return 1;
}
std::uintptr_t __fastcall stand_in_date(std::uintptr_t, std::uintptr_t out) {
    ++native_dates;
    std::memset(reinterpret_cast<void*>(out), 0, 0x21);
    auto p = *reinterpret_cast<const std::uintptr_t*>(fake_flag_global + 0x260);  // the global holds the object
    std::uint8_t flag = 0;
    if (p && (p = *reinterpret_cast<const std::uintptr_t*>(p + 0x10)) != 0) flag = *reinterpret_cast<const std::uint8_t*>(p + 0xBFF0) & 1;
    *reinterpret_cast<std::uint8_t*>(out + 0x20) = flag;
    *reinterpret_cast<std::uint64_t*>(out + 0x18) = steam_now;
    std::tm t{};
    const __time64_t time = steam_now;
    _gmtime64_s(&t, &time);
    const std::uint32_t fields[6] = {std::uint32_t(t.tm_year + 1900), std::uint32_t(t.tm_mon + 1), std::uint32_t(t.tm_mday),
                                     std::uint32_t(t.tm_hour), std::uint32_t(t.tm_min), std::uint32_t(t.tm_sec)};
    std::memcpy(reinterpret_cast<void*>(out), fields, sizeof(fields));
    return out;
}
// Planted copies of both signatures for discover: wildcards become operands
// that point at these.
std::uintptr_t fake_steam_context, fake_steam_import, other_steam_context;
unsigned char planted_query[256], planted_date[256];
void plant_operand(unsigned char* code, unsigned at, unsigned length, unsigned displacement_at, const void* target) {
    const auto d = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(target) -
                                             static_cast<std::intptr_t>(reinterpret_cast<std::uintptr_t>(code) + at + length));
    std::memcpy(code + at + displacement_at, &d, 4);
}

void server_time_test() {
    namespace st = cpuopt::server_time;
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto q = signature_bytes(st::query_signature), t = signature_bytes(st::date_signature);
    for (std::size_t i = 0; i < q.size(); ++i) planted_query[i] = static_cast<unsigned char>(q[i] < 0 ? 0 : q[i]);
    for (std::size_t i = 0; i < t.size(); ++i) planted_date[i] = static_cast<unsigned char>(t[i] < 0 ? 0 : t[i]);
    for (unsigned i = 0; i < 2; ++i) {
        plant_operand(planted_query, st::query_context[i], 7, 3, &fake_steam_context);
        plant_operand(planted_query, st::query_import[i], 6, 2, &fake_steam_import);
    }
    plant_operand(planted_date, st::date_context, 7, 3, &fake_steam_context);
    plant_operand(planted_date, st::date_import, 6, 2, &fake_steam_import);
    plant_operand(planted_date, st::date_global, 7, 3, &fake_flag_global);
    const std::uintptr_t qs[1] = {reinterpret_cast<std::uintptr_t>(planted_query)}, ds[1] = {reinterpret_cast<std::uintptr_t>(planted_date)};
    const std::uintptr_t qs2[2] = {qs[0], qs[0]};
    CHECK(std::string(st::discover(base, qs, 0, ds, 1).reason) == "signature not found");
    CHECK(std::string(st::discover(base, qs2, 2, ds, 1).reason) == "signature not unique");
    auto d = st::discover(base, qs, 1, ds, 1);
    CHECK(d.found && d.query_rva == rva_of(planted_query) && d.date_rva == rva_of(planted_date) && d.global_rva == rva_of(&fake_flag_global));
    plant_operand(planted_query, st::query_context[1], 7, 3, &other_steam_context);
    CHECK(std::string(st::discover(base, qs, 1, ds, 1).reason) == "different Steam calls");
    st::set_target(base, d);

    // The flag's chain: global -> +0x260 -> +0x10 -> byte +0xBFF0 = 3 (bit 0 set).
    auto a = std::make_unique<std::uint8_t[]>(0x268), b = std::make_unique<std::uint8_t[]>(0x18), c = std::make_unique<std::uint8_t[]>(0xBFF8);
    std::memset(a.get(), 0, 0x268);
    std::memset(b.get(), 0, 0x18);
    std::memset(c.get(), 0, 0xBFF8);
    c[0xBFF0] = 3;
    *reinterpret_cast<std::uintptr_t*>(b.get() + 0x10) = reinterpret_cast<std::uintptr_t>(c.get());
    *reinterpret_cast<std::uintptr_t*>(a.get() + 0x260) = reinterpret_cast<std::uintptr_t>(b.get());
    fake_flag_global = reinterpret_cast<std::uintptr_t>(a.get());

    session_vtable[st::state_slot / sizeof(void*)] = reinterpret_cast<void*>(&stand_in_state);
    *st::query_next() = &stand_in_query;
    *st::date_next() = &stand_in_date;
    std::uint64_t value{};
    // Off: every call is the game's.
    st::set_active(false, 1000);
    CHECK(st::query_detour(&stand_in_session, &value) == 1 && value == steam_now && native_queries == 1);
    CHECK(st::query_detour(&stand_in_session, &value) == 1 && native_queries == 2);
    // On: one read, then reuse.
    st::set_active(true, 5000);
    CHECK(st::query_detour(&stand_in_session, &value) == 1 && native_queries == 3);
    steam_now += 7;  // Steam moves on; the kept read is still within time_ms
    value = 0;
    CHECK(st::query_detour(&stand_in_session, &value) == 1 && value == steam_now - 7 && native_queries == 3);
    CHECK(st::query_detour(&stand_in_session, nullptr) == 0 && native_queries == 4);  // no out: the game's code
    session_state = 1;  // another session state: the game's code (local time)
    CHECK(st::query_detour(&stand_in_session, &value) == 0 && value == 12345 && native_queries == 5);
    session_state = 2;
    // date from the kept read, byte for byte what the game's code writes for that time.
    std::uint8_t ours[40], game_date[40];
    std::memset(ours, 0xEE, sizeof(ours));
    CHECK(st::date_detour(0, reinterpret_cast<std::uintptr_t>(ours)) == reinterpret_cast<std::uintptr_t>(ours) && native_dates == 0);
    steam_now -= 7;
    std::memset(game_date, 0xEE, sizeof(game_date));
    stand_in_date(0, reinterpret_cast<std::uintptr_t>(game_date));
    CHECK(std::memcmp(ours, game_date, 0x21) == 0);
    const std::uint32_t expected_fields[6] = {2025, 10, 9, 8, 53, 20};
    CHECK(std::memcmp(ours, expected_fields, sizeof(expected_fields)) == 0 && ours[0x20] == 1);
    // A kept read older than time_ms: the game's code again, and kept anew.
    st::set_active(true, 20);
    Sleep(40);
    steam_now += 100;
    native_dates = 0;
    st::date_detour(0, reinterpret_cast<std::uintptr_t>(ours));
    CHECK(native_dates == 1 && *reinterpret_cast<std::uint64_t*>(ours + 0x18) == steam_now);
    // Whole seconds since the read are added.
    st::set_active(true, 60000);
    st::query_detour(&stand_in_session, &value);  // a fresh read? no: kept within 60 s
    Sleep(1100);
    CHECK(st::query_detour(&stand_in_session, &value) == 1 && value == steam_now + 1);
    std::string out = "{";
    st::append(out);
    std::printf("server_time: %s}\n", out.c_str());
    CHECK(out.find("\"active\":true,\"time_ms\":60000,") != std::string::npos);
    st::set_active(false, 0);
    native_queries = 0;
    st::query_detour(&stand_in_session, &value);
    CHECK(native_queries == 1);
    fake_flag_global = 0;
}

// Inline rounds: a small site runs alone once learned (participants 1 during
// the call, the old value back after), not before its job threads exist,
// and a site with more work keeps its job threads.
std::uint32_t participants_seen[4096];
unsigned rounds_seen;
void __fastcall recording_execute_job(std::uintptr_t smain, int mode, std::uintptr_t jobs, std::uint32_t count, std::uintptr_t extra) {
    if (rounds_seen < 4096) participants_seen[rounds_seen++] = *reinterpret_cast<const std::uint32_t*>(smain + cpuopt::job_timing::participants_offset);
    stand_in_execute_job(smain, mode, jobs, count, extra);
}
__declspec(noinline) std::uintptr_t slow_job(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t) {
    const auto due = cpuopt::qpc_now() + cpuopt::qpc_frequency() / 10000;  // 0.1 ms
    while (cpuopt::qpc_now() < due) {}
    return 0;
}
volatile unsigned small_rounds, big_rounds;  // keep the linker from folding the two sites
__declspec(noinline) void small_site(std::uintptr_t smain, std::uintptr_t list, std::uint32_t n) {
    small_rounds = small_rounds + 1;
    cpuopt::job_timing::detour(smain, 1, list, n, 0);
}
__declspec(noinline) void big_site(std::uintptr_t smain, std::uintptr_t list, std::uint32_t n) {
    big_rounds = big_rounds + 2;
    cpuopt::job_timing::detour(smain, 1, list, n, 0);
}

void inline_test() {
    namespace jt = cpuopt::job_timing;
    const auto exe = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    *jt::next_slot() = &recording_execute_job;
    auto smain = std::make_unique<std::uint8_t[]>(0x200);
    std::memset(smain.get(), 0, 0x200);
    auto& participants = *reinterpret_cast<std::uint32_t*>(smain.get() + jt::participants_offset);
    participants = 31;
    const auto s = reinterpret_cast<std::uintptr_t>(smain.get());
    std::uintptr_t object[2] = {reinterpret_cast<std::uintptr_t>(timing_vtable), 0};
    std::uintptr_t small[3][4], big[5][4];
    for (auto& e : small) {
        e[0] = reinterpret_cast<std::uintptr_t>(object);
        e[1] = reinterpret_cast<std::uintptr_t>(&timing_noop);
        e[2] = e[3] = 0;
    }
    for (auto& e : big) {
        e[0] = reinterpret_cast<std::uintptr_t>(object);
        e[1] = reinterpret_cast<std::uintptr_t>(&slow_job);
        e[2] = e[3] = 0;
    }
    std::uintptr_t small_before[3][4];
    std::memcpy(small_before, small, sizeof(small));
    jt::set_measuring(false);
    jt::set_order(0, 20);
    jt::set_inline(true, 100);  // 100 us: the small site's 3 empty jobs are under it, the big site's 0.5 ms is not
    const auto run = [&](auto site, auto& list, unsigned times) {
        rounds_seen = 0;
        for (unsigned i = 0; i < times; ++i) site(s, reinterpret_cast<std::uintptr_t>(list), static_cast<std::uint32_t>(std::size(list)));
        unsigned alone = 0;
        for (unsigned i = 0; i < rounds_seen; ++i) alone += participants_seen[i] == 1 ? 1 : 0;
        return alone;
    };
    // Job threads not created yet: never alone, whatever was learned.
    CHECK(run(small_site, small, 40) == 0);
    smain[jt::threads_created_offset] = 1;
    CHECK(run(small_site, small, 40) == 40);  // learned in the first rounds: all alone now
    CHECK(participants == 31);                // put back after each round
    CHECK(std::memcmp(small_before, small, sizeof(small)) == 0);
    CHECK(run(big_site, big, 24) == 0);
    std::string out = "{";
    jt::append_inline(out, exe);
    out += '}';
    std::printf("inline: %s\n", out.c_str());
    CHECK(out.find("\"on\":true,\"threshold_us\":100,\"rounds\":40,") != std::string::npos);
    CHECK(out.find(",1]") != std::string::npos && out.find(",0]") != std::string::npos);  // one site alone, one not
    jt::set_inline(false, 100);
    CHECK(run(small_site, small, 10) == 0);  // off: the game's own count
    *jt::next_slot() = &stand_in_execute_job;
}

// worker_spin on a stand-in job system with the game's protocol: the main
// thread zeroes started/done, releases min(jobs, participants) - 1 permits,
// takes jobs from a shared counter itself, then spins until done equals the
// number released; job threads wait (worker_spin::wait), count started, take
// jobs, count done.
struct StandInJobs {
    std::uint8_t smain[0x100]{};
    HANDLE semaphore{};
    std::atomic<std::int32_t> next{}, started{}, done{};
    std::atomic<std::int64_t> ran{};
    std::int32_t count{};
    std::atomic<bool> quit{};
};
DWORD WINAPI stand_in_job_thread(void* p) {
    auto& j = *static_cast<StandInJobs*>(p);
    while (!j.quit.load()) {
        while (cpuopt::worker_spin::wait(j.semaphore) != 0) {}
        if (j.quit.load()) break;
        j.started.fetch_add(1);
        for (std::int32_t i; (i = j.next.fetch_add(1)) < j.count;) j.ran.fetch_add(1);
        j.done.fetch_add(1);
    }
    return 0;
}
// One round; false if the threads did not all report back within 2 s.
bool stand_in_round(StandInJobs& j, std::int32_t jobs) {
    auto& participants = *reinterpret_cast<std::uint32_t*>(j.smain + cpuopt::worker_spin::participants_offset);
    j.count = jobs;
    j.next = 0;
    j.started = 0;
    j.done = 0;
    cpuopt::worker_spin::before_round(reinterpret_cast<std::uintptr_t>(j.smain), 1, static_cast<std::uint32_t>(jobs));
    const auto released = static_cast<std::int32_t>(std::min<std::uint32_t>(static_cast<std::uint32_t>(jobs), participants)) - 1;
    if (released > 0) ReleaseSemaphore(j.semaphore, released, nullptr);
    for (std::int32_t i; (i = j.next.fetch_add(1)) < jobs;) j.ran.fetch_add(1);
    const auto due = cpuopt::qpc_now() + 2 * cpuopt::qpc_frequency();
    while (j.done.load() != std::max(released, 0))
        if (cpuopt::qpc_now() > due) return false;
    return true;
}
__declspec(noinline) std::uintptr_t stub_target(std::uintptr_t a) { return a * 3 + 1; }
unsigned char planted_wait[64];
std::uintptr_t fake_wait_import, other_import;

void worker_spin_test() {
    namespace ws = cpuopt::worker_spin;
    // Patch bytes: a 6-byte NOP, then call rel32 ending where the old call ended.
    std::array<std::uint8_t, ws::patch_length> bytes{};
    const std::uintptr_t target = 0x14225F337;
    CHECK(ws::build_replacement(target, 0x140001000, bytes));
    std::int32_t rel{};
    std::memcpy(&rel, bytes.data() + 7, 4);
    CHECK(bytes[0] == 0x66 && bytes[5] == 0x00 && bytes[6] == 0xE8 && target + ws::patch_length + rel == 0x140001000);
    CHECK(!ws::build_replacement(target, 0x7FF000000000ULL, bytes));  // out of reach
    // A stub within 2 GB of this exe that jumps to a function.
    const auto exe = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto stub = ws::make_stub(exe, reinterpret_cast<const void*>(&stub_target));
    const auto distance = stub > exe ? stub - exe : exe - stub;
    CHECK(stub && distance < 0x80000000ULL);
    CHECK(stub && reinterpret_cast<std::uintptr_t (*)(std::uintptr_t)>(stub)(5) == 16);
    MEMORY_BASIC_INFORMATION mbi{};
    VirtualQuery(reinterpret_cast<const void*>(stub), &mbi, sizeof(mbi));
    CHECK(mbi.Protect == PAGE_EXECUTE_READ);  // never writable and executable at once
    // discover: the planted wait must call through a slot holding WaitForSingleObject.
    const auto sig = signature_bytes(ws::signature);
    for (std::size_t i = 0; i < sig.size(); ++i) planted_wait[i] = static_cast<unsigned char>(sig[i] < 0 ? 0 : sig[i]);
    const auto plant_slot = [&](const std::uintptr_t* slot) {
        const auto at = reinterpret_cast<std::uintptr_t>(planted_wait) + ws::import_operand;
        const auto d = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(slot) - static_cast<std::intptr_t>(at + 6));
        std::memcpy(planted_wait + ws::import_operand + 2, &d, 4);
    };
    fake_wait_import = reinterpret_cast<std::uintptr_t>(&WaitForSingleObject);
    other_import = reinterpret_cast<std::uintptr_t>(&SetEvent);
    const std::uintptr_t site[1] = {reinterpret_cast<std::uintptr_t>(planted_wait)};
    plant_slot(&fake_wait_import);
    const auto d = ws::discover(exe, site, 1);
    CHECK(d.found && d.site_rva == rva_of(planted_wait));
    plant_slot(&other_import);
    CHECK(std::string(ws::discover(exe, site, 1).reason) == "call is not WaitForSingleObject");
    CHECK(std::string(ws::discover(exe, site, 0).reason) == "signature not found");

    // The stand-in job system: 8 job threads, participants 9.
    StandInJobs j;
    *reinterpret_cast<std::uint32_t*>(j.smain + ws::participants_offset) = 9;
    j.semaphore = CreateSemaphoreW(nullptr, 0, 64, nullptr);
    HANDLE threads[8];
    for (auto& t : threads) t = CreateThread(nullptr, 0, stand_in_job_thread, &j, 0, nullptr);
    std::string out;
    ws::append(out);  // reset counters
    // Off: every wait blocks; every round completes.
    ws::set_active(false, 300);
    bool all = true;
    for (int r = 0; r < 200; ++r) all = stand_in_round(j, 1 + r % 40) && all;
    CHECK(all);
    std::string off = "{";
    ws::append(off);
    // On: rounds back to back; threads spin between them and claim places.
    ws::set_active(true, 2000);
    all = true;
    std::int64_t jobs_expected = 0;
    j.ran = 0;
    for (int r = 0; r < 3000; ++r) {
        const auto n = 1 + r % 40;
        all = stand_in_round(j, n) && all;
        jobs_expected += n;
        if (r % 500 == 499) Sleep(5);  // a gap longer than the spin window: threads block, then wake
    }
    CHECK(all && j.ran.load() == jobs_expected);
    std::string on = "{";
    ws::append(on);
    std::printf("worker_spin: off %s}\n             on  %s}\n", off.c_str(), on.c_str());
    const auto claimed = number_after(on, "claimed");
    CHECK(claimed > 1000);  // most permits taken by spinning threads
    // Off again while threads spin: they block and rounds still complete.
    ws::set_active(false, 300);
    all = true;
    for (int r = 0; r < 100; ++r) all = stand_in_round(j, 1 + r % 40) && all;
    CHECK(all);
    j.quit = true;
    ReleaseSemaphore(j.semaphore, 8, nullptr);
    WaitForMultipleObjects(8, threads, TRUE, 2000);
    for (auto t : threads) CloseHandle(t);
    CloseHandle(j.semaphore);
}

void spin_probe_test() {
    namespace sp = cpuopt::spin_probe;
    auto object = std::make_unique<std::uint8_t[]>(sp::done_offset + 8);
    std::memset(object.get(), 0, sp::done_offset + 8);
    const auto smain = reinterpret_cast<std::uintptr_t>(object.get());
    const auto counter = [&](std::uint32_t offset) -> std::uint32_t& { return *reinterpret_cast<std::uint32_t*>(object.get() + offset); };
    sp::Reading r;
    counter(sp::started_offset) = 3;
    counter(sp::done_offset) = 1;
    CHECK(sp::read(smain, 5, r) && r.released == 5 && r.not_started() == 2 && r.running() == 2);
    counter(sp::started_offset) = 5;
    counter(sp::done_offset) = 5;
    CHECK(sp::read(smain, 5, r) && r.not_started() == 0 && r.running() == 0);
    counter(sp::done_offset) = 6;  // a snapshot can see done ahead of started: never negative
    CHECK(sp::read(smain, 5, r) && r.running() == 0);
    CHECK(!sp::read(smain, 33, r));  // more threads than a round can release
    counter(sp::started_offset) = 1000;
    CHECK(!sp::read(smain, 5, r));  // not a counter
    CHECK(!sp::read(0, 5, r));
}

// job_cap against a stand-in sMain: the game's own count learned once the job
// threads exist, caps written to +0xD0 and +0xCC, the native count back when
// off, clamping, and the cases where it must keep its hands off.

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
    {
        const auto at = e.find("\"leaves\":[[");
        CHECK(at != std::string::npos);
        const char* p = e.c_str() + at + 11;
        char* end{};
        const auto rva = std::strtoull(p, &end, 10);
        const auto game = std::strtoull(end + 1, &end, 10);
        const auto all = std::strtoull(end + 1, &end, 10);
        const auto leaf_fn = rva_of(reinterpret_cast<const void*>(&busy_leaf));
        std::printf("sampler: hottest leaf rva %llu (busy_leaf %u), game %llu all %llu\n", rva, leaf_fn, game, all);
        CHECK(rva >= leaf_fn && rva < leaf_fn + 256 && game == all && all > 10);
    }
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
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // output survives a crash
    frame_clock_test();
    frame_events_test();
    job_cap_test();
    spin_probe_test();
    job_timing_test();
    job_order_test();
    inline_test();
    prim_stores_test();
    server_time_test();
    worker_spin_test();
    census_and_sampler_test();
    start_event_test();
    std::printf(failures ? "plugin_tests: %d failure(s)\n" : "plugin_tests: ok\n", failures);
    return failures ? 1 : 0;
}
