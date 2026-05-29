#pragma once

#include <functional>

namespace ueht::hooks {

/// Placeholder D3D12 Present hook. D3D12 needs IDXGISwapChain3 + the command
/// queue captured at swapchain creation time; this scaffold is non-functional
/// today and `Hook()` always returns false, causing the framework to fall
/// back to the D3D11 path.
class D3D12Hook {
public:
    using PresentCallback = std::function<void()>;

    D3D12Hook() = default;
    ~D3D12Hook() = default;

    bool Hook(PresentCallback) { return false; }
    void Unhook() {}
    bool IsHooked() const { return false; }
};

}  // namespace ueht::hooks
