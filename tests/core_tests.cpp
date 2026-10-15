// Pure logic: signatures, frame-interval percentiles, control.ini, JSON text.
#include "../src/core/control.hpp"
#include "../src/core/histogram.hpp"
#include "../src/core/json.hpp"
#include "../src/core/signature.hpp"
#include <cstdio>
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
    for (int i = 0; i < 65; ++i) long_text += "90 ";
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
    CHECK(!parse_control("job_order=3"));
    CHECK(!parse_control("order_us=0"));
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
}  // namespace

int main() {
    signatures();
    histogram();
    control();
    json_text();
    std::printf(failures ? "core_tests: %d failure(s)\n" : "core_tests: ok\n", failures);
    return failures ? 1 : 0;
}
