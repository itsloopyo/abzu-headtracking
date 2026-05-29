#pragma once

#include <atomic>
#include <memory>
#include <mutex>

#include "ueht/Config.hpp"

namespace ueht {

class Mods;
namespace hooks { class D3D11Hook; class D3D12Hook; }

/// Process-wide singleton. Owns config, mods, and graphics hooks.
/// Initialization happens off the loader-lock thread spawned from DllMain.
class Framework {
public:
    static Framework& Get();

    /// Idempotent. Safe to call repeatedly from the Present hook trampoline
    /// (one-shot initialization happens on the first call).
    bool Initialize();

    /// Tear down hooks and stop the OpenTrack receiver. Best-effort on detach.
    void Shutdown();

    bool IsReady() const { return m_ready.load(std::memory_order_acquire); }

    const Config& Cfg() const { return m_config; }
    Mods&         GetMods()   { return *m_mods; }

    /// Called from the Present detour each frame.
    void OnFrame();

private:
    Framework() = default;
    ~Framework() = default;

    Framework(const Framework&) = delete;
    Framework& operator=(const Framework&) = delete;

    bool DoInitialize();

    std::once_flag                       m_initOnce;
    std::atomic<bool>                    m_ready{false};
    std::atomic<bool>                    m_initFailed{false};

    Config                               m_config;
    std::unique_ptr<Mods>                m_mods;
    std::unique_ptr<hooks::D3D11Hook>    m_d3d11;
    std::unique_ptr<hooks::D3D12Hook>    m_d3d12;
};

}  // namespace ueht
