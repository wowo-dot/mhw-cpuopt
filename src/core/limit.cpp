#include "limit.hpp"
#include <cmath>
#include <cstdio>

namespace cpuopt::limit {
Verdict classify(const Input& in) noexcept {
    Verdict v;
    if (!(in.fps > 0) || !(in.busy_ms > 0)) return v;
    v.cpu_fps = 1000.0 / in.busy_ms;
    if (in.cap_fps > 0 && in.fps >= 0.97 * in.cap_fps) {
        v.kind = Kind::frame_cap;
        return v;
    }
    const double ratio = in.fps / v.cpu_fps;
    if (ratio >= 0.93) v.kind = Kind::cpu;
    else if (ratio >= 0.85) v.kind = Kind::even;
    else v.kind = in.vsync ? Kind::vsync : Kind::gpu;
    return v;
}

std::string text(const Verdict& v) {
    char line[48];
    // The CPU's figure to the nearest 5 fps: it moves less from second to second.
    const unsigned cpu = static_cast<unsigned>(std::lround(v.cpu_fps / 5.0) * 5);
    switch (v.kind) {
    case Kind::cpu: return "CPU";
    case Kind::even: return "CPU and GPU about even";
    case Kind::gpu: std::snprintf(line, sizeof(line), "graphics card (CPU: %u fps)", cpu); return line;
    case Kind::vsync: std::snprintf(line, sizeof(line), "V-Sync (CPU: %u fps)", cpu); return line;
    case Kind::frame_cap: return "frame rate limit";
    case Kind::unknown: break;
    }
    return "measuring";
}
}  // namespace cpuopt::limit
