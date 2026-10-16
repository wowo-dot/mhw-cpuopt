#include "bench.hpp"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <vector>

namespace cpuopt::bench {
namespace {
double median(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const auto n = v.size();
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}
double change(double before, double after) { return before > 0 ? after / before - 1.0 : 0.0; }

void add(std::string& out, const char* format, ...) {
    char line[512];
    va_list args;
    va_start(args, format);
    const int n = std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (n > 0) out.append(line, static_cast<std::size_t>(n < int(sizeof(line)) ? n : int(sizeof(line)) - 1));
}
// "+48%" / "-3%" / "+0.4%": one decimal below 10%.
std::string percent(double g) {
    char text[32];
    const double p = g * 100.0;
    std::snprintf(text, sizeof(text), p > -10.0 && p < 10.0 ? "%+.1f%%" : "%+.0f%%", p);
    return text;
}
}  // namespace

bool window_on(unsigned i) noexcept {
    const bool off_first = (i / 2) % 2 == 0;
    return (i % 2 == 0) != off_first;
}
double settle_ms(double window_ms) noexcept { return std::min(3000.0, window_ms / 5.0); }
double countdown_ms(double window_ms) noexcept { return std::min(10000.0, window_ms); }

Result summarize(std::span<const Window> w) {
    Result r;
    std::vector<double> fps[2], p99[2], cpu[2], process[2], gains;
    r.cpu = r.process = !w.empty();
    for (const auto& x : w) {
        if (!x.frames || x.seconds <= 0) continue;
        const int s = x.on ? 1 : 0;
        fps[s].push_back(x.fps());
        p99[s].push_back(x.p99_ms);
        if (x.cpu_frames) cpu[s].push_back(x.cpu_ms);
        else r.cpu = false;
        if (x.process_ms > 0) process[s].push_back(x.process_ms);
        else r.process = false;
    }
    for (std::size_t i = 0; i + 1 < w.size(); i += 2) {
        const auto& a = w[i];
        const auto& b = w[i + 1];
        if (a.on == b.on || !a.frames || !b.frames) continue;
        const auto& off = a.on ? b : a;
        const auto& on = a.on ? a : b;
        gains.push_back(change(off.fps(), on.fps()));
    }
    r.pairs = static_cast<unsigned>(gains.size());
    r.off_fps = median(fps[0]);
    r.on_fps = median(fps[1]);
    r.gain = change(r.off_fps, r.on_fps);
    if (!gains.empty()) {
        r.gain_low = *std::min_element(gains.begin(), gains.end());
        r.gain_high = *std::max_element(gains.begin(), gains.end());
    }
    r.off_p99 = median(p99[0]);
    r.on_p99 = median(p99[1]);
    r.cpu = r.cpu && !cpu[0].empty() && !cpu[1].empty();
    if (r.cpu) {
        r.off_cpu = median(cpu[0]);
        r.on_cpu = median(cpu[1]);
        r.cpu_change = change(r.off_cpu, r.on_cpu);
    }
    r.process = r.process && !process[0].empty() && !process[1].empty();
    if (r.process) {
        r.off_process = median(process[0]);
        r.on_process = median(process[1]);
        r.process_change = change(r.off_process, r.on_process);
    }
    return r;
}

std::string report(const Machine& m, std::span<const Window> w, const Result& r) {
    std::string out;
    add(out, "== cpuopt benchmark, %s ==\n", m.when.c_str());
    add(out, "cpuopt %s; game %s\n", m.version.c_str(), m.game.c_str());
    add(out, "CPU: %s\n", m.cpu.c_str());
    add(out, "GPU: %s\n", m.gpu.empty() ? "unknown" : m.gpu.c_str());
    add(out, "Windows %s\n", m.windows.c_str());
    add(out, "Game options: %s\n", m.display.c_str());
    add(out, "Optimizations in the \"on\" windows: %s\n", m.switches.c_str());
    if (m.fps_cap_removed) add(out, "Remove 300 fps limit: on in both halves\n");
    if (!m.modules.empty()) add(out, "Also loaded: %s\n", m.modules.c_str());
    add(out, "%u pairs of %.0f s windows, the first %.1f s of each not counted", pairs, m.window_s, settle_ms(m.window_s * 1000.0) / 1000.0);
    if (m.pauses) add(out, "; %u window(s) measured again after a pause", m.pauses);
    out += "\n\n";

    add(out, "Frame rate: %.1f -> %.1f fps, %s", r.off_fps, r.on_fps, percent(r.gain).c_str());
    if (r.pairs > 1) add(out, " (the pairs: %s to %s)", percent(r.gain_low).c_str(), percent(r.gain_high).c_str());
    out += '\n';
    add(out, "Frame time p99: %.2f -> %.2f ms\n", r.off_p99, r.on_p99);
    if (r.cpu) {
        add(out, "Game thread CPU time per frame: %.2f -> %.2f ms, %s", r.off_cpu, r.on_cpu, percent(r.cpu_change).c_str());
        if (r.on_cpu > 0) add(out, " (the CPU alone could reach %.0f fps)", 1000.0 / r.on_cpu);
        out += '\n';
    } else {
        out += "Game thread CPU time per frame: not measured on this game version\n";
    }
    if (r.process) add(out, "All threads' CPU time per frame: %.1f -> %.1f ms, %s\n", r.off_process, r.on_process, percent(r.process_change).c_str());
    out += '\n';

    out += " #  optimizations    fps   p50 ms   p99 ms   max ms   CPU ms  all threads ms\n";
    for (std::size_t i = 0; i < w.size(); ++i) {
        const auto& x = w[i];
        add(out, "%2zu  %-13s %6.1f %8.2f %8.2f %8.2f", i + 1, x.on ? "on" : "off", x.fps(), x.p50_ms, x.p99_ms, x.max_ms);
        if (x.cpu_frames) add(out, " %8.2f", x.cpu_ms);
        else out += "        -";
        if (x.process_ms > 0) add(out, " %15.1f", x.process_ms);
        else out += "               -";
        out += '\n';
    }
    out += '\n';

    // What the numbers mean here.
    if (r.pairs > 1 && r.gain_high - r.gain_low > 0.25)
        add(out, "- The pairs differ a lot (%s to %s): the scene changed while it ran. Stand still in one spot, camera too, and run it again.\n",
            percent(r.gain_low).c_str(), percent(r.gain_high).c_str());
    if (r.cpu && r.on_cpu > 0 && r.on_fps < 0.9 * 1000.0 / r.on_cpu) {
        add(out, "- With the optimizations on, something besides the CPU held the frame rate here (the graphics card, V-Sync or a frame rate "
                 "limit): the CPU alone could reach %.0f fps, the game ran at %.0f.",
            1000.0 / r.on_cpu, r.on_fps);
        if (r.off_cpu > 0 && r.off_fps < 0.9 * 1000.0 / r.off_cpu) out += " The same held the \"off\" windows, so the CPU time shows the gain better than fps.";
        out += '\n';
    }
    out += "- fps counts the game's own frames; frames that frame generation adds are not counted.\n\n";
    return out;
}

std::string fps_line(const Result& r) {
    char line[64];
    std::snprintf(line, sizeof(line), "%s fps, %.0f to %.0f", percent(r.gain).c_str(), r.off_fps, r.on_fps);
    return line;
}
std::string cpu_line(const Result& r) {
    if (!r.cpu) return "not measured";
    char line[64];
    std::snprintf(line, sizeof(line), "%s, %.2f to %.2f ms", percent(r.cpu_change).c_str(), r.off_cpu, r.on_cpu);
    return line;
}
}  // namespace cpuopt::bench
