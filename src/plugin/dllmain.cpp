#include "runtime.hpp"

// Astera Loader calls al_plugin_info and al_plugin_init (al_entry.cpp); a
// refused plugin is unloaded again, so DllMain does no work.
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
    } else if (reason == DLL_PROCESS_DETACH && reserved) {  // process exit, not FreeLibrary
        cpuopt::runtime::at_exit();
    }
    return TRUE;
}
