#pragma once
#include <optional>
#include <string>
#include <string_view>

namespace cpuopt {
// The run folder's control.ini, written by tools/run.py while the game runs:
//   toggles=<decimal or 0x hex>   runtime switches (toggles.hpp)
//   marker=<label>                scene label, [A-Za-z0-9_.-], at most 64 characters
//   window_ms=<1000..60000>       statistics window length
//   job_threads=<1..64>           threads per sMain job round while toggle job_cap
//                                 is on, the game thread included
//   job_order=<1|2>               toggle order_jobs: 1 reversed, 2 learned
//   order_us=<1..10000>           learned order: job types at least this slow go first
// Blank lines, [sections] and lines starting with ';' or '#' are ignored. A
// malformed line rejects the whole file, so a half-written file changes nothing.
struct Control {
    std::optional<unsigned> toggles;
    std::optional<std::string> marker;
    std::optional<unsigned> window_ms;
    std::optional<unsigned> job_threads;
    std::optional<unsigned> job_order, order_us;
    bool operator==(const Control&) const = default;
};
std::optional<Control> parse_control(std::string_view text);
}  // namespace cpuopt
