#include "Framework.hpp"

#include <windows.h>

#include <filesystem>
#include <string>
#include <vector>

#include "Mods.hpp"
#include "hooks/D3D11Hook.hpp"
#include "hooks/D3D12Hook.hpp"
#include "utility/Logging.hpp"

#include "ueht/Version.hpp"
#include "cameraunlock/config/ini_reader.h"
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
// instead puts it wherever the launcher happened to start the game. Returns
// empty when the EXE path cannot be resolved; the caller says so rather than
// dropping a log somewhere unrelated.
std::string ResolveLogPath(const std::string& configured) {
    const std::string name = configured.empty() ? "AbzuHeadTracking.log" : configured;
    std::filesystem::path p(name);
    if (p.is_absolute()) return name;
    const std::wstring exe = HostExePath();
    if (exe.empty()) return {};
    return (std::filesystem::path(exe).parent_path() / p).string();
}

}  // namespace

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
    // The log opens BEFORE the config is parsed. LoadFromFile emits the
    // smoothing validation and retired-key warnings, and the logger drops any
    // line written while the file is still closed, so loading first threw all
    // of that away. The two [Logging] keys are therefore read on their own
    // first; LoadFromFile re-reads them into m_config a moment later.
    const std::string iniPath = Config::DefaultIniPathNextToHostExe();
    std::error_code iniEc;
    const bool iniFound = std::filesystem::exists(iniPath, iniEc);

    bool logToFile = m_config.log_to_file;
    std::string logPath = m_config.log_path;
    if (iniFound) {
        cameraunlock::IniReader logIni;
        if (logIni.Open(iniPath)) {
            logToFile = logIni.ReadBool("Logging", "LogToFile", logToFile);
            logPath   = logIni.ReadString("Logging", "LogPath", logPath.c_str());
        }
    }
    if (logToFile) {
        const std::string resolved = ResolveLogPath(logPath);
        if (resolved.empty()) {
            UEHT_LOG(Error, "Could not resolve the host EXE path; no log file will be written");
        } else {
            log::Init(resolved);
        }
    }

    UEHT_LOG(Info, "%s %s starting up", kProductName, kVersion);
    if (iniFound) {
        UEHT_LOG(Info, "Loading config from %s", iniPath.c_str());
    } else {
        UEHT_LOG(Info, "No config at %s; using defaults", iniPath.c_str());
    }
    Config::LoadFromFile(iniPath, m_config);

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

    // D3D12 first - many UE5 games run on it. If D3D12 isn't loaded in the
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
