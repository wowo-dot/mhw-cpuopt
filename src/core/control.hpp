#pragma once
#include <optional>
#include <string>
#include <string_view>

namespace cpuopt {
// The run folder's control.ini, written by tools/run.py while the game runs:
//   toggles=<decimal or 0x hex>   runtime switches (toggles.hpp)
//   marker=<label>                scene label, [A-Za-z0-9_.-], at most 64 characters
//   window_ms=<1000..60000>       statistics window length
// Blank lines, [sections] and lines starting with ';' or '#' are ignored. A
// malformed line rejects the whole file, so a half-written file changes nothing.
struct Control {
    std::optional<unsigned> toggles;
    std::optional<std::string> marker;
    std::optional<unsigned> window_ms;
    bool operator==(const Control&) const = default;
};
std::optional<Control> parse_control(std::string_view text);
}  // namespace cpuopt
