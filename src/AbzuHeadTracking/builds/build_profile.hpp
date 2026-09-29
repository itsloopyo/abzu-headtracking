#pragma once

#include <cstddef>
#include <cstdint>

#include "cameraunlock/memory/pe_fingerprint.h"

namespace ueht::builds {

// Everything pinned to one shipped AbzuGame-Win64-Shipping.exe. On any other
// build these point at unrelated memory, so nothing that consumes one runs until
// the running executable has matched a profile. A patched or other-store build
// gets a new profile appended beside the existing ones, never an edit to them.
struct EngineOffsets {
    // The UEngine UClass static (Z_Registration_Info_UClass_UEngine.OuterSingleton),
    // as an RVA. LocateGEngine matches live objects against the class it holds.
    uintptr_t uengine_class_rva;
    // Byte offsets of the GEngine -> PlayerCameraManager walk, read off the
    // FProperty registration immediates in the Z_Construct_UClass_* functions.
    size_t engine_to_game_viewport;              // UEngine::GameViewport
    size_t game_viewport_to_game_instance;       // UGameViewportClient::GameInstance
    size_t game_instance_to_local_players;       // UGameInstance::LocalPlayers (TArray data)
    size_t local_player_to_player_controller;    // UPlayer::PlayerController
    size_t player_controller_to_camera_manager;  // APlayerController::PlayerCameraManager
};

struct BuildProfile {
    const char* name;
    cameraunlock::memory::PeFingerprint fingerprint;
    EngineOffsets offsets;
};

extern const BuildProfile kSteamProfile_20201114;

/// Fingerprints the host EXE and returns its profile, or nullptr (and logs why)
/// when no known profile matches. A null result means the mod must stay fully
/// dormant: no hooks, no threads.
const BuildProfile* MatchRunningBuild();

}  // namespace ueht::builds
