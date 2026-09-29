#include <windows.h>

#include "Framework.hpp"

namespace {

DWORD WINAPI InitThread(LPVOID) {
    // We run off the loader lock; sleep briefly so the host has a chance to
    // finish its own module init before we start probing D3D.
    Sleep(100);
    ueht::Framework::Get().Initialize();
    return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        // The game's code jumps into ours through the hooks, so an unload would
        // crash it. Pinning means detach only ever comes at process exit, where
        // there is nothing to tear down that the OS does not reclaim itself.
        HMODULE pinned = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&DllMain), &pinned);
        CloseHandle(CreateThread(nullptr, 0, &InitThread, hModule, 0, nullptr));
    }
    return TRUE;
}
