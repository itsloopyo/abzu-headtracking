#include "D3D11Hook.hpp"

#include "utility/Logging.hpp"

#include "cameraunlock/hooks/hook_manager.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <atomic>

namespace ueht::hooks {

namespace {

using Microsoft::WRL::ComPtr;

using PresentFn      = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeBuffersFn = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

PresentFn       g_origPresent       = nullptr;
ResizeBuffersFn g_origResizeBuffers = nullptr;
std::atomic<bool> g_inPresent{false};

D3D11Hook::PresentCallback g_callback;

HRESULT STDMETHODCALLTYPE PresentDetour(IDXGISwapChain* swap, UINT sync, UINT flags) {
    // Re-entrancy guard: many overlays call Present indirectly from their own
    // callbacks.
    if (!g_inPresent.exchange(true, std::memory_order_acq_rel)) {
        try {
            if (g_callback) g_callback();
        } catch (...) {
            // Swallow — never propagate into the engine's render thread.
        }
        g_inPresent.store(false, std::memory_order_release);
    }
    return g_origPresent(swap, sync, flags);
}

HRESULT STDMETHODCALLTYPE ResizeBuffersDetour(IDXGISwapChain* swap, UINT count, UINT w, UINT h,
                                              DXGI_FORMAT fmt, UINT flags) {
    return g_origResizeBuffers(swap, count, w, h, fmt, flags);
}

/// Creates a 1x1 windowless swapchain so we can read the IDXGISwapChain vtable.
bool CreateDummySwapchain(ComPtr<IDXGISwapChain>& outSwap, ComPtr<ID3D11Device>& outDev) {
    WNDCLASSEXA wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = DefWindowProcA;
    wc.hInstance     = GetModuleHandleA(nullptr);
    wc.lpszClassName = "UEHT_DummyWnd";
    RegisterClassExA(&wc);
    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "ueht", 0, 0, 0, 1, 1,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return false;

    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount       = 1;
    desc.BufferDesc.Width  = 1;
    desc.BufferDesc.Height = 1;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferDesc.RefreshRate.Numerator = 60;
    desc.BufferDesc.RefreshRate.Denominator = 1;
    desc.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow      = hwnd;
    desc.SampleDesc.Count  = 1;
    desc.Windowed          = TRUE;
    desc.SwapEffect        = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL fl;
    const auto hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &desc,
        outSwap.GetAddressOf(), outDev.GetAddressOf(), &fl, nullptr);

    DestroyWindow(hwnd);
    UnregisterClassA(wc.lpszClassName, wc.hInstance);
    return SUCCEEDED(hr);
}

}  // namespace

D3D11Hook::D3D11Hook() = default;
D3D11Hook::~D3D11Hook() { Unhook(); }

bool D3D11Hook::Hook(PresentCallback on_present) {
    if (m_hooked) return true;

    ComPtr<IDXGISwapChain> swap;
    ComPtr<ID3D11Device>   dev;
    if (!CreateDummySwapchain(swap, dev)) {
        UEHT_LOG(Warn, "D3D11Hook: failed to create dummy swapchain");
        return false;
    }

    auto** vtbl = *reinterpret_cast<void***>(swap.Get());
    void* present       = vtbl[8];   // IDXGISwapChain::Present
    void* resizeBuffers = vtbl[13];  // IDXGISwapChain::ResizeBuffers

    g_callback = std::move(on_present);

    using cameraunlock::hooks::HookManager;
    using cameraunlock::hooks::HookStatus;
    auto& mh = HookManager::Instance();

    if (mh.CreateHook(present, reinterpret_cast<void*>(&PresentDetour),
                      reinterpret_cast<void**>(&g_origPresent)) != HookStatus::Ok) {
        UEHT_LOG(Error, "D3D11Hook: failed to create Present hook");
        return false;
    }
    mh.CreateHook(resizeBuffers, reinterpret_cast<void*>(&ResizeBuffersDetour),
                  reinterpret_cast<void**>(&g_origResizeBuffers));

    if (mh.EnableHook(present) != HookStatus::Ok) {
        UEHT_LOG(Error, "D3D11Hook: failed to enable Present hook");
        return false;
    }
    mh.EnableHook(resizeBuffers);

    UEHT_LOG(Info, "D3D11Hook: Present @ %p hooked", present);
    m_hooked = true;
    return true;
}

void D3D11Hook::Unhook() {
    if (!m_hooked) return;
    // Cleanup is centralized in HookManager::Shutdown when the framework
    // tears down; per-hook removal here would race the render thread.
    m_hooked = false;
    g_callback = nullptr;
}

}  // namespace ueht::hooks
