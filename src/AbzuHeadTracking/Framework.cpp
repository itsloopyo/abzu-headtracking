#include "Framework.hpp"

#include "Mods.hpp"
#include "hooks/D3D11Hook.hpp"
#include "hooks/D3D12Hook.hpp"
#include "utility/Logging.hpp"

#include "ueht/Version.hpp"
#include "cameraunlock/hooks/hook_manager.h"

namespace ueht {

Framework& Framework::Get() {
    static Framework s;
    return s;
}

bool Framework::Initialize() {
    if (m_ready.load(std::memory_order_acquire))    return true;
    if (m_initFailed.load(std::memory_order_acquire)) return false;

    bool ok = false;
    std::call_once(m_initOnce, [&]{ ok = DoInitialize(); });

    if (!ok) {
        m_initFailed.store(true, std::memory_order_release);
        return false;
    }
    m_ready.store(true, std::memory_order_release);
    return true;
}

bool Framework::DoInitialize() {
    // Config + logging come up first so everything else has a place to talk.
    Config::LoadFromFile(Config::DefaultIniPathNextToHostExe(), m_config);
    if (m_config.log_to_file) {
        log::Init(m_config.log_path.empty() ? "ueht.log" : m_config.log_path);
    }
    UEHT_LOG(Info, "%s %s starting up", kProductName, kVersion);

    // MinHook is shared between cameraunlock_hooks and our D3D hooks.
    using cameraunlock::hooks::HookManager;
    auto& mh = HookManager::Instance();
    const auto status = mh.Initialize();
    if (status != cameraunlock::hooks::HookStatus::Ok &&
        status != cameraunlock::hooks::HookStatus::ErrorAlreadyInitialized) {
        UEHT_LOG(Error, "MinHook initialize failed: %s",
                 cameraunlock::hooks::HookStatusToString(status));
        return false;
    }

    m_mods = std::make_unique<Mods>();
    if (auto err = m_mods->Initialize(); err.has_value()) {
        UEHT_LOG(Error, "Mod initialization failed: %s", err->c_str());
        return false;
    }

    m_d3d11 = std::make_unique<hooks::D3D11Hook>();
    m_d3d12 = std::make_unique<hooks::D3D12Hook>();

    // D3D12 first — many UE5 games run on it. If D3D12 isn't loaded in the
    // process we fall through to D3D11.
    if (!m_d3d12->Hook([this]{ OnFrame(); })) {
        if (!m_d3d11->Hook([this]{ OnFrame(); })) {
            UEHT_LOG(Warn, "Neither D3D11 nor D3D12 Present could be hooked yet. "
                           "Will retry on first device creation.");
        }
    }

    UEHT_LOG(Info, "UEHT initialized.");
    return true;
}

void Framework::Shutdown() {
    UEHT_LOG(Info, "UEHT shutting down");
    if (m_d3d11) m_d3d11->Unhook();
    if (m_d3d12) m_d3d12->Unhook();
    if (m_mods)  m_mods->Shutdown();
    cameraunlock::hooks::HookManager::Instance().Shutdown();
    log::Shutdown();
}

void Framework::OnFrame() {
    if (!m_ready.load(std::memory_order_acquire)) return;
    m_mods->OnFrame();
}

}  // namespace ueht
