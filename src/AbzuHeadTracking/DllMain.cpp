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
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hModule);
            CloseHandle(CreateThread(nullptr, 0, &InitThread, hModule, 0, nullptr));
            break;
        case DLL_PROCESS_DETACH:
            ueht::Framework::Get().Shutdown();
            break;
        default: break;
    }
    return TRUE;
}
