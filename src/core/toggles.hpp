#pragma once
#include <atomic>

namespace cpuopt {
// Runtime switches, set live from the run folder's control.ini (control.hpp).
// Every bit starts off. Research switches observe; optimization switches,
// when they exist, change one engine path each and are measured on and off
// in the same scene.
enum Toggle : unsigned {
    sample_profile = 1U,  // research: stack sampler (sampler.hpp); suspends sampled threads briefly
};
inline constexpr unsigned toggle_mask = 1U;  // every defined bit

inline std::atomic<unsigned> toggles{};
inline bool on(Toggle t) noexcept { return (toggles.load(std::memory_order_relaxed) & t) != 0; }
}  // namespace cpuopt
