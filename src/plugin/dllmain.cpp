#include "runtime.hpp"

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        cpuopt::runtime::start(module);
    } else if (reason == DLL_PROCESS_DETACH && reserved) {  // process exit, not FreeLibrary
        cpuopt::runtime::at_exit();
    }
    return TRUE;
}
