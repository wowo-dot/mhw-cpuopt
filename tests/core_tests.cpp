// Pure logic: signatures, frame-interval percentiles, control.ini, JSON text,
// cpuopt.ini, the benchmark's summary and report.
#include "../src/core/bench.hpp"
#include "../src/core/control.hpp"
#include "../src/core/histogram.hpp"
#include "../src/core/json.hpp"
#include "../src/core/settings.hpp"
#include "../src/core/signature.hpp"
#include <cmath>
#include <cstdio>
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

void signatures() {
    using cpuopt::Pattern;
    CHECK(Pattern::parse("48 8B 05 ?? ?? ?? ??").has_value());
    CHECK(Pattern::parse("48 8B 05 ?? ?? ?? ??")->size() == 7);
    CHECK(!Pattern::parse("?? ??"));       // wildcards only
    CHECK(!Pattern::parse("4"));           // half a byte
    CHECK(!Pattern::parse("48 GG"));       // not hex
    CHECK(!Pattern::parse("488B"));        // bytes must be separated
    CHECK(!Pattern::parse(""));
    std::string long_text;
    for (std::size_t i = 0; i <= Pattern::capacity; ++i) long_text += "90 ";
    CHECK(!Pattern::parse(long_text));     // longer than capacity

    const auto p = *Pattern::parse("?? 8B 05 ?? AA");
    const std::vector<std::uint8_t> data{0x48, 0x8B, 0x05, 0x01, 0xAA, 0x8B, 0x05, 0x8B, 0x05, 0x00, 0xAA, 0x8B, 0x05, 0x00};
    std::size_t hits[4]{};
    const auto found = p.find(data.data(), data.size(), hits, 4);
    CHECK(found == 2);
    CHECK(hits[0] == 0 && hits[1] == 6);  // the last 8B 05 has no room for the rest
    CHECK(p.find(data.data(), 3, hits, 4) == 0);
    std::size_t one[1]{};
    CHECK(p.find(data.data(), data.size(), one, 1) == 2 && one[0] == 0);  // counts past capacity

    // The tested build's frame-parity read: mov rax,[rip+disp] at 0x23922C2.
    const std::uint8_t site[] = {0x48, 0x8B, 0x05, 0xB7, 0x21, 0xE3, 0x02};
    CHECK(cpuopt::rip_target(0x23922C2, 7, cpuopt::read_i32(site + 3)) == 0x51C4480);
    const std::uint8_t negative[] = {0xF0, 0xFF, 0xFF, 0xFF};
    CHECK(cpuopt::read_i32(negative) == -16);
    CHECK(cpuopt::rip_target(0x1000, 7, -16) == 0xFF7);
}

void histogram() {
    cpuopt::IntervalHistogram h;
    for (int i = 0; i < 990; ++i) h.add(5000);
    for (int i = 0; i < 9; ++i) h.add(30000);
    h.add(150000);  // overflow bucket
    const auto s = h.take();
    CHECK(s.count == 1000);
    CHECK(s.p50_us == 5050);  // upper edge of the 5.00-5.05 ms bucket
    CHECK(s.p99_us == 5050);
    CHECK(s.p999_us == 30050);
    CHECK(s.max_us == 150000);
    CHECK(s.over_25ms == 10);
    CHECK(s.over_50ms == 1);
    CHECK(s.total_us == 990ULL * 5000 + 9 * 30000 + 150000);
    const auto empty = h.take();  // take() resets
    CHECK(empty.count == 0 && empty.p50_us == 0 && empty.max_us == 0);
    cpuopt::IntervalHistogram top;
    top.add(200000);
    CHECK(top.take().p50_us == 200000);  // rank in the overflow bucket reports the maximum
}

void control() {
    using cpuopt::parse_control;
    const auto c = parse_control("; comment\n[control]\ntoggles=0x3\r\nmarker=hub-start_1.a\nwindow_ms=2000\n\n");
    CHECK(c.has_value());
    CHECK(c->toggles == 3u);
    CHECK(c->marker == std::string("hub-start_1.a"));
    CHECK(c->window_ms == 2000u);
    CHECK(parse_control("toggles=1")->toggles == 1u);
    CHECK(parse_control("")->toggles == std::nullopt);
    CHECK(parse_control("marker=")->marker == std::string());
    CHECK(!parse_control("toggles=x"));
    CHECK(!parse_control("toggles=-1"));
    CHECK(!parse_control("marker=has space"));
    CHECK(!parse_control("marker=quote\""));
    CHECK(!parse_control("window_ms=999"));
    CHECK(!parse_control("window_ms=60001"));
    CHECK(parse_control("job_threads=16")->job_threads == 16u);
    CHECK(parse_control("job_threads=1")->job_threads == 1u);
    CHECK(!parse_control("job_threads=0"));
    CHECK(!parse_control("job_threads=65"));
    CHECK(parse_control("job_order=2\norder_us=20")->job_order == 2u);
    CHECK(parse_control("order_us=20")->order_us == 20u);
    CHECK(parse_control("job_order=3")->job_order == 3u);
    CHECK(!parse_control("job_order=4"));
    CHECK(!parse_control("order_us=0"));
    CHECK(parse_control("inline_us=25")->inline_us == 25u);
    CHECK(!parse_control("inline_us=0"));
    CHECK(!parse_control("inline_us=1001"));
    CHECK(parse_control("time_ms=1000")->time_ms == 1000u);
    CHECK(!parse_control("time_ms=9"));
    CHECK(!parse_control("time_ms=60001"));
    CHECK(parse_control("toggles=16383")->toggles == 16383u);
    CHECK(parse_control("max_fps=1000")->max_fps == 1000u);
    CHECK(!parse_control("max_fps=59"));
    CHECK(!parse_control("max_fps=1001"));
    CHECK(parse_control("toggles=32767")->toggles == 32767u);
    CHECK(parse_control("margin_us=500")->margin_us == 500u);
    CHECK(!parse_control("margin_us=49"));
    CHECK(!parse_control("margin_us=2001"));
    CHECK(parse_control("follow=0x34a6d18")->follow == 0x34a6d18u);
    CHECK(parse_control("follow=0")->follow == 0u);
    CHECK(parse_control("follow_us=20")->follow_us == 20u);
    CHECK(!parse_control("follow_us=4"));
    CHECK(parse_control("toggles=65535")->toggles == 65535u);
    CHECK(parse_control("contacts=3")->contacts == 3u);
    CHECK(!parse_control("contacts=0"));
    CHECK(!parse_control("contacts=4"));
    CHECK(parse_control("cpu_count=12")->cpu_count == 12u);
    CHECK(!parse_control("cpu_count=1"));
    CHECK(!parse_control("cpu_count=65"));
    CHECK(parse_control("sample_jobs=0")->sample_jobs == 0u);
    CHECK(parse_control("sample_jobs=16")->sample_jobs == 16u);
    CHECK(!parse_control("sample_jobs=33"));
    CHECK(parse_control("unit_us=20")->unit_us == 20u);
    CHECK(!parse_control("unit_us=0"));
    CHECK(!parse_control("unit_us=100001"));
    CHECK(parse_control("spin_us=300")->spin_us == 300u);
    CHECK(!parse_control("spin_us=9"));
    CHECK(!parse_control("spin_us=5001"));
    CHECK(!parse_control("unknown=1"));
    CHECK(!parse_control("toggles"));
    CHECK(parse_control("\xEF\xBB\xBFtoggles=2")->toggles == 2u);  // UTF-8 BOM
}

void json_text() {
    std::string out = "{";
    cpuopt::json::key(out, "a");
    cpuopt::json::string(out, "q\"b\\c\n\x01");
    cpuopt::json::key(out, "n");
    cpuopt::json::number(out, std::uint64_t(42));
    cpuopt::json::key(out, "f");
    cpuopt::json::fixed(out, 1.23456, 2);
    out += '}';
    CHECK(out == "{\"a\":\"q\\\"b\\\\c\\u000a\\u0001\",\"n\":42,\"f\":1.23}");
}

void settings() {
    using cpuopt::Settings;
    const Settings defaults;
    CHECK(cpuopt::parse_settings("") == defaults);
    // The default file reads back as the defaults, with every key once.
    const auto file = cpuopt::write_settings("", defaults);
    CHECK(cpuopt::parse_settings(file) == defaults);
    CHECK(file.find("enabled = on") != std::string::npos && file.find("remove_fps_cap = off") != std::string::npos);
    CHECK(file.find("toggle_key = None") != std::string::npos && file.find("# Experimental") != std::string::npos);
    // Changed values go into the lines they came from; comments and other lines stay.
    Settings changed = defaults;
    changed.enabled = false;
    changed.toggle_key = 0x100 | 0x78;  // Ctrl+F9
    changed.parallel_contacts = false;
    changed.unit_us = 50;
    const auto text = "# mine\nenabled = on    ; keep this comment\nunit_us=40\nfoo = bar\n";
    const auto written = cpuopt::write_settings(text, changed);
    CHECK(written.find("# mine\nenabled = off   ; keep this comment\nunit_us=50\nfoo = bar\n") == 0);
    // A comment keeps its column while there is room, then sits one space after the value.
    Settings keyed = defaults;
    keyed.toggle_key = 0x100 | 'P';
    CHECK(cpuopt::write_settings("toggle_key = None   ; c\n", keyed).find("toggle_key = Ctrl+P ; c\n") == 0);
    keyed.toggle_key = 0x300 | 0x7B;
    CHECK(cpuopt::write_settings("toggle_key = None   ; c\n", keyed).find("toggle_key = Ctrl+Shift+F12 ; c\n") == 0);
    CHECK(cpuopt::write_settings("toggle_key = None   ; c\n", defaults).find("toggle_key = None   ; c\n") == 0);
    CHECK(written.find("toggle_key = Ctrl+F9") != std::string::npos);  // missing keys appended
    CHECK(cpuopt::parse_settings(written) == changed);
    // CRLF files stay CRLF.
    const auto crlf = cpuopt::write_settings("enabled = on\r\n", changed);
    CHECK(crlf.find("enabled = off\r\n") == 0 && crlf.find("\nunit_us") != std::string::npos &&
          crlf.find("\r\n", crlf.find("unit_us")) != std::string::npos);
    // Bad lines are named and skipped.
    std::string problems;
    const auto odd = cpuopt::parse_settings("enabled = maybe\nunit_us = 5\nnope = on\ncache_ccd = OFF\n", &problems);
    CHECK(odd.enabled && odd.unit_us == 40 && !odd.cache_ccd);
    CHECK(problems == "line 1: bad value for enabled\nline 2: bad value for unit_us\nline 3: unknown key nope\n");
    // The switches.
    CHECK(cpuopt::toggles_of(defaults) == (16u | 8u | 32u | 256u | 2048u | 32768u | 16384u));
    Settings off = defaults;
    off.enabled = false;
    CHECK(cpuopt::toggles_of(off) == 0u);
    off.remove_fps_cap = true;
    CHECK(cpuopt::toggles_of(off) == 8192u);  // the cap switch is not under the master
    // Key names.
    CHECK(cpuopt::key_name(0) == "None" && cpuopt::key_name(0x100 | 0x78) == "Ctrl+F9");
    CHECK(cpuopt::key_name(0x600 | 'K') == "Shift+Alt+K" && cpuopt::key_name(0x05) == "Mouse4");
    CHECK(cpuopt::key_name(0x65) == "Numpad5" && cpuopt::key_name(0xBA) == "VK 0xBA");
    for (const std::uint32_t k : {0u, 0x178u, 0x64Bu, 0x05u, 0x65u, 0xBAu, 0x2Du, 0x187u, 0x330u})
        CHECK(cpuopt::parse_key(cpuopt::key_name(k)) == k);
    CHECK(cpuopt::parse_key("ctrl + shift + k") == 0x300u + 'K');
    CHECK(!cpuopt::parse_key("Hyper+F1") && !cpuopt::parse_key("F25") && !cpuopt::parse_key("Ctrl+"));
    // The benchmark's window length.
    CHECK(defaults.benchmark_s == 15 && file.find("benchmark_s = 15") != std::string::npos);
    CHECK(cpuopt::parse_settings("benchmark_s = 1\n").benchmark_s == 1 && cpuopt::parse_settings("benchmark_s = 61\n").benchmark_s == 15);
}

bool near(double a, double b) { return std::fabs(a - b) < 1e-6 * (std::fabs(b) + 1.0); }

void benchmark() {
    namespace b = cpuopt::bench;
    // Off-on, on-off, ...: a drift in the scene falls on both states.
    const bool order[] = {false, true, true, false, false, true, true, false, false, true, true, false, false, true, true, false};
    static_assert(std::size(order) == b::windows);
    for (unsigned i = 0; i < b::windows; ++i) CHECK(b::window_on(i) == order[i]);
    CHECK(b::settle_ms(15000) == 3000 && b::settle_ms(1000) == 200);
    CHECK(b::countdown_ms(15000) == 10000 && b::countdown_ms(1000) == 1000);
    // Off at 180 fps, on at 270 (the first pair's on window at 252).
    std::vector<b::Window> w;
    for (unsigned i = 0; i < b::windows; ++i) {
        b::Window x;
        x.on = b::window_on(i);
        x.seconds = 12;
        const double fps = !x.on ? 180.0 : i == 1 ? 252.0 : 270.0;
        x.frames = static_cast<std::uint64_t>(fps * 12 + 0.5);
        x.p50_ms = 1000.0 / fps;
        x.p99_ms = x.on ? 4.4 : 16.7;
        x.max_ms = 20;
        x.cpu_frames = x.frames;
        x.cpu_ms = x.on ? 3.6 : 5.2;
        x.process_ms = x.on ? 22.0 : 46.0;
        w.push_back(x);
    }
    const auto r = b::summarize(w);
    CHECK(r.pairs == 8 && near(r.off_fps, 180) && near(r.on_fps, 270) && near(r.gain, 0.5));
    CHECK(near(r.gain_low, 0.4) && near(r.gain_high, 0.5) && near(r.off_p99, 16.7) && near(r.on_p99, 4.4));
    CHECK(r.cpu && near(r.off_cpu, 5.2) && near(r.on_cpu, 3.6) && near(r.cpu_change, 3.6 / 5.2 - 1.0));
    CHECK(r.process && near(r.off_process, 46) && near(r.on_process, 22));
    CHECK(b::fps_line(r) == "+50% fps, 180 to 270" && b::cpu_line(r) == "-31%, 5.20 to 3.60 ms");
    b::Machine m;
    m.when = "2026-10-16 22:10";
    m.version = "0.14.0";
    m.game = "421810, the build cpuopt was made on";
    m.cpu = "AMD Ryzen 9 9950X3D 16-Core Processor, 32 threads";
    m.gpu = "NVIDIA GeForce RTX 5090";
    m.windows = "10.0.26200";
    m.display = "2560x1440, Windowed, frame rate No Limit, V-Sync Off, the game's DLSS Off";
    m.switches = "effect drawing, draw job order";
    m.modules = "cmfm.dll, mhw_dlss.dll";
    m.window_s = 15;
    m.pauses = 1;
    const auto text = b::report(m, w, r);
    std::printf("%s", text.c_str());
    CHECK(text.find("== cpuopt benchmark, 2026-10-16 22:10 ==\n") == 0);
    CHECK(text.find("Frame rate: 180.0 -> 270.0 fps, +50% (the pairs: +40% to +50%)\n") != std::string::npos);
    CHECK(text.find("Game thread CPU time per frame: 5.20 -> 3.60 ms, -31% (the CPU alone could reach 278 fps)\n") != std::string::npos);
    CHECK(text.find("All threads' CPU time per frame: 46.0 -> 22.0 ms, -52%\n") != std::string::npos);
    CHECK(text.find("8 pairs of 15 s windows, the first 3.0 s of each not counted; 1 window(s) measured again") != std::string::npos);
    CHECK(text.find("\n16  off ") != std::string::npos && text.find("\n17 ") == std::string::npos);
    CHECK(text.find("something besides the CPU") == std::string::npos && text.find("differ a lot") == std::string::npos);
    CHECK(text.find("Also loaded: cmfm.dll, mhw_dlss.dll\n") != std::string::npos && text.find("Remove 300 fps limit") == std::string::npos);
    // Held by the GPU with the optimizations on: the CPU could do 500 fps.
    auto held = w;
    for (auto& x : held)
        if (x.on) x.cpu_ms = 2.0;
    CHECK(b::report(m, held, b::summarize(held)).find("the CPU alone could reach 500 fps, the game ran at 270.") != std::string::npos);
    // A scene that changed: the pairs from +10% to +80%.
    auto drift = w;
    drift[1].frames = static_cast<std::uint64_t>(198 * 12);
    drift[2].frames = static_cast<std::uint64_t>(324 * 12);
    CHECK(b::report(m, drift, b::summarize(drift)).find("The pairs differ a lot (+10% to +80%)") != std::string::npos);
    // Without frame_budget: fps only.
    for (auto& x : w) x.cpu_frames = 0;
    const auto r2 = b::summarize(w);
    CHECK(!r2.cpu && r2.process && b::fps_line(r2) == "+50% fps, 180 to 270" && b::cpu_line(r2) == "not measured");
    CHECK(b::report(m, w, r2).find("Game thread CPU time per frame: not measured") != std::string::npos);
    CHECK(b::fps_line(r).size() <= 30 && b::cpu_line(r).size() <= 30);
}
}  // namespace

int main() {
    signatures();
    histogram();
    control();
    json_text();
    settings();
    benchmark();
    std::printf(failures ? "core_tests: %d failure(s)\n" : "core_tests: ok\n", failures);
    return failures ? 1 : 0;
}
