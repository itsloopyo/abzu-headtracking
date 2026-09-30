#include "Framework.hpp"

#include <windows.h>

#include <filesystem>
#include <string>
#include <vector>

#include "Mods.hpp"
#include "builds/build_profile.hpp"
#include "hooks/D3D11Hook.hpp"
#include "utility/Logging.hpp"

#include "ueht/Version.hpp"
#include "cameraunlock/hooks/hook_manager.h"

namespace ueht {

namespace {

// GetModuleFileNameW truncates silently at the buffer size and reports it by
// returning exactly that size, so grow until the call fits. A deep Steam
// library path is not far off MAX_PATH, and the old fixed buffer turned that
// into a CWD-relative log the user would never find.
std::wstring HostExePath() {
    std::vector<wchar_t> buf(MAX_PATH);
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size()) return std::wstring(buf.data(), n);
        if (buf.size() >= 32768) return {};
        buf.resize(buf.size() * 2);
    }
}

// A bare filename in [Logging] LogPath must land next to the host EXE, which is
// where the README tells users to look. Resolving it against the process CWD
// instead puts it wherever the launcher happened to start the game.
std::string ResolveLogPath(const std::string& configured) {
    const std::string name = configured.empty() ? "HeadTracking.log" : configured;
    std::filesystem::path p(name);
    if (p.is_absolute()) return name;
    return (std::filesystem::path(HostExePath()).parent_path() / p).string();
}

}  // namespace

Framework& Framework::Get() {
    // Never destroyed. A static's destructor runs in DLL_PROCESS_DETACH under the
    // loader lock, after the OS has already killed every other thread, and this
    // one would join the receiver and hotkey threads and take the log mutex that
    // a killed thread may have been holding.
    static Framework* const s = new Framework();
    return *s;
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
    // The log's own settings are in CameraUnlock.ini, so the config loads first.
    // The logger keeps every line written before Init and writes them to the file
    // Init opens, so nothing the load reports is lost.
    UEHT_LOG(Info, "%s %s starting up", kProductName, kVersion);
    const std::wstring exe = HostExePath();
    if (exe.empty()) {
        UEHT_LOG(Error, "Could not resolve the host EXE path; CameraUnlock.ini cannot be found");
        log::Init({});
        return false;
    }
    m_config = config::Load(std::filesystem::path(exe).parent_path().wstring());

    if (m_config.log_to_file) {
        log::Init(ResolveLogPath(m_config.log_path));
    } else {
        log::Init({});
    }

    const builds::BuildProfile* const build = builds::MatchRunningBuild();
    if (build == nullptr) return false;

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

    m_mods = std::make_unique<Mods>(*build);
    if (auto err = m_mods->Initialize(); err.has_value()) {
        UEHT_LOG(Error, "Mod initialization failed: %s", err->c_str());
        return false;
    }

    m_d3d11 = std::make_unique<hooks::D3D11Hook>();
    if (!m_d3d11->Hook([this]{ OnFrame(); })) {
        UEHT_LOG(Error, "D3D11 Present could not be hooked; head tracking stays off.");
        return false;
    }

    UEHT_LOG(Info, "UEHT initialized.");
    return true;
}

void Framework::OnFrame() {
    if (!m_ready.load(std::memory_order_acquire)) return;
    m_mods->OnFrame();
}

}  // namespace ueht
