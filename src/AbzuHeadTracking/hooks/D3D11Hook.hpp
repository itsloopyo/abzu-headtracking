#pragma once

#include <functional>

namespace ueht::hooks {

/// Hooks IDXGISwapChain::Present by spinning up a throwaway swapchain on a
/// hidden window, reading the vtable, and routing the Present slot through
/// MinHook (via cameraunlock's HookManager). The hook stays installed for the
/// life of the process.
class D3D11Hook {
public:
    using PresentCallback = std::function<void()>;

    bool Hook(PresentCallback on_present);
};

}  // namespace ueht::hooks
