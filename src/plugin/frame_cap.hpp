#pragma once
#include <cstdint>
#include <string>

namespace cpuopt::frame_cap {
// Research switch uncap (toggles.hpp): the engine's frame cap raised, so
// scenes that sit at it measure again. Not meant for play: the engine never
// ran above its cap.
//
// sMain's frame timing (sub_14225F9F0) holds each frame to sMain +0x5C, the
// registered property mMaxFps (sMain::populatePropertyList): it Sleeps when
// 2 ms or more remain and otherwise spins on QueryPerformanceCounter. The
// graphics option FrameRate sets it; "no limit" gives the 300 fps cap. The
// switch writes control.ini's max_fps there at al.frame BEGIN on the game
// thread (no frame timing runs then), keeps the game's value, and writes it
// back when off and at EXITING. A value outside 10-1000 fps is never
// touched.
//
// Player mode (only_unlimited): the cap is raised only from the engine's 300,
// what the in-game frame rate setting "no limit" gives; a lower limit the
// player picked stays, and when the options change it the switch follows
// them instead of stopping.
//
// The sMain global comes from job_cap's discovery.
inline constexpr std::uint32_t max_fps_offset = 0x5C;
void set_target(std::uintptr_t smain_global) noexcept;
// 0 = the game's own cap. Any thread; applied at the next BEGIN.
void request(unsigned max_fps, bool only_unlimited = false) noexcept;
inline constexpr float unlimited_fps = 300.0f;  // the engine's mMaxFps with the frame rate at no limit
void on_begin() noexcept;  // al.frame BEGIN, game thread
void restore() noexcept;   // EXITING
// "cap": {state, game, requested, applied} into an open JSON object.
void append(std::string& out);
}  // namespace cpuopt::frame_cap
