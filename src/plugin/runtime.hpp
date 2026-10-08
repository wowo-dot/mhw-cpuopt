#pragma once
#include <windows.h>

namespace cpuopt::runtime {
// DllMain, process attach: starts the window thread. Without the sidecar
// <dll>.output-dir naming a run folder the plugin does nothing at all.
void start(HMODULE self) noexcept;
// DllMain, process exit: one last line if the log is free.
void at_exit() noexcept;
}  // namespace cpuopt::runtime
