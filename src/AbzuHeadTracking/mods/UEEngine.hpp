#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace ueht::ue {

/// Unreal Engine major/minor — detected from the host exe's file version info.
struct EngineVersion {
    uint16_t major = 0;
    uint16_t minor = 0;
    uint16_t patch = 0;
    bool valid() const { return major != 0; }
    std::string ToString() const;

    bool AtLeast(uint16_t M, uint16_t m) const {
        return major > M || (major == M && minor >= m);
    }
};

/// Detect UE version from the host executable's VERSIONINFO resource.
/// Returns empty if not a UE shipping build.
EngineVersion DetectEngineVersion();

/// Offsets we need to walk from GEngine to the active player's camera rotation.
/// These are *byte* offsets inside the corresponding UObject layouts. They
/// shift across UE versions; the table below holds known-good values dumped
/// from public UE source builds.
struct EngineOffsets {
    // UEngine::GameInstance / UEngine::GameViewport
    size_t engine_to_game_instance      = 0;
    // UGameInstance::LocalPlayers (TArray<ULocalPlayer*>)
    size_t game_instance_to_local_players = 0;
    // ULocalPlayer::PlayerController
    size_t local_player_to_player_controller = 0;
    // AController::ControlRotation (FRotator). The source UE reads each tick
    // to recompute APlayerCameraManager::ViewTarget.POV.Rotation. Writing POV
    // directly is futile because UpdateCamera clobbers it every frame.
    size_t controller_to_control_rotation = 0;
    // APlayerController::PlayerCameraManager (APlayerCameraManager*). Anchor for
    // the decoupled path: hook the PCM's per-frame camera update and inject the
    // head delta into the rendered POV only, leaving ControlRotation clean.
    size_t player_controller_to_camera_manager = 0;

    bool valid() const {
        return controller_to_control_rotation != 0;
    }
};

/// Look up offsets for a detected engine version. Falls back to the closest
/// known version. Returns std::nullopt only if the engine is too old/new
/// for any known table.
std::optional<EngineOffsets> OffsetsFor(EngineVersion v);

/// One-time discovery of the GEngine global pointer in the host process.
/// Returns 0 if discovery fails. Result is cached.
uintptr_t LocateGEngine();

}  // namespace ueht::ue
