// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo
#include "build_profile.hpp"
#include "runtime_discovery.hpp"
#include "utility/Logging.hpp"
#include <windows.h>
#include <vector>

namespace ueht::builds {
const BuildProfile* MatchRunningBuild(void* module) {
    ResetRuntimeDiscovery();
    auto host=module ? static_cast<HMODULE>(module) : GetModuleHandleW(nullptr);
    cameraunlock::memory::PeFingerprint running{};
    if(!cameraunlock::memory::ReadPeFingerprint(host,running)) {
        UEHT_LOG(Error,"Could not read executable header; tracking remains off.");return nullptr;
    }
    const bool exact=running.Matches(kSteamProfile_20201114.fingerprint);
    std::vector<std::uint8_t> bytes(running.SizeOfImage);SIZE_T copied=0;
    if(!ReadProcessMemory(GetCurrentProcess(),host,bytes.data(),bytes.size(),&copied) || copied!=bytes.size()){
        UEHT_LOG(Error,"Executable snapshot failed: Win32 error %lu",GetLastError());return nullptr;
    }
    Bootstrap found{};std::string reason;
    const ImageView view{bytes.data(),bytes.size(),reinterpret_cast<std::uintptr_t>(host)};
    if(!DiscoverBootstrap(view,found,reason)){
        UEHT_LOG(Warn,"discovery: %s",reason.c_str());
        if(exact){UEHT_LOG(Info,"Using exact historical profile %s",kSteamProfile_20201114.name);return &kSteamProfile_20201114;}
        return nullptr;
    }
    if(exact && found.engineClass!=kSteamProfile_20201114.offsets.uengine_class_rva){
        UEHT_LOG(Error,"Discovery disagrees with historical engine class; tracking remains off.");return nullptr;
    }
    StartRuntimeDiscovery(view,found,exact);
    static BuildProfile discovered{};
    discovered={"runtime-discovered",running,{found.engineClass,0,0,0,0,0}};
    UEHT_LOG(Info,"discovery: names=0x%X EngineClass=0x%X; waiting for live camera validation",found.names,found.engineClass);
    return &discovered;
}
}
