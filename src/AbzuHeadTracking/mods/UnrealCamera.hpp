#pragma once

#include <atomic>
#include <cstdint>

#include "Mod.hpp"
#include "UEEngine.hpp"

namespace ueht {

class HeadTracking;

/// FRotator as Unreal lays it out in memory: { Pitch, Yaw, Roll } floats (deg).
/// We don't depend on Unreal headers — this matches UE4.x and UE5.x.
#pragma pack(push, 4)
struct FRotator {
    float Pitch = 0.0f;
    float Yaw   = 0.0f;
    float Roll  = 0.0f;
};
#pragma pack(pop)

/// Drives the active player's camera each frame from the processed OpenTrack
/// pose in `HeadTracking`. Two modes (see `Mode` / `Config::camera_mode`):
///
///   ControlRotation: walk GEngine -> GameInstance -> LocalPlayers[0] ->
///     PlayerController -> AController::ControlRotation, and write
///     engine_intent + tracking_delta into that FRotator. Couples head movement
///     to the game's control/movement basis; kept as a stopgap.
///   UpdateCamera: resolve the live PlayerCameraManager, hook its per-frame
///     camera-update virtual, and add the head delta to the rendered POV only,
///     leaving ControlRotation clean (decoupled).
///
/// Offsets shift across UE 4.x/5.x and per-game build, so both paths defer all
/// resolution until the engine is live and retry until it succeeds. Until then
/// the mod is a no-op.
class UnrealCamera final : public Mod {
public:
    explicit UnrealCamera(HeadTracking& tracking) : m_tracking(tracking) {}

    std::string_view Name() const override { return "UnrealCamera"; }

    std::optional<std::string> OnInitialize() override;
    void OnFrame() override;
    void OnShutdown() override;

    /// How the head delta reaches the rendered view. See Config::camera_mode.
    enum class Mode {
        ControlRotation,  // write AController::ControlRotation (coupled; default)
        UpdateCamera,     // hook the PCM camera-update virtual, write POV only (decoupled)
    };

private:
    /// Per-frame tick for each mode. OnFrame dispatches by m_mode.
    void TickDecoupled();        // Mode::UpdateCamera
    void TickControlRotation();  // Mode::ControlRotation

    /// ControlRotation path: locate the active camera's FRotator. Returns true
    /// and populates `m_rotationSlot` on success.
    bool Resolve();

    /// Walk GEngine -> GameViewport -> GameInstance -> LocalPlayer[0] ->
    /// PlayerController. Returns the PlayerController pointer or 0. SEH-guarded.
    uintptr_t WalkToPlayerController(uintptr_t gengine, const ue::EngineOffsets& offsets);

    /// ControlRotation path: PlayerController -> ControlRotation FRotator.
    FRotator* WalkToRotation(uintptr_t gengine, const ue::EngineOffsets& offsets);

    // --- Decoupled (UpdateCamera) path -------------------------------------
    /// Resolve the live APlayerCameraManager instance. Returns 0 until ready.
    uintptr_t ResolveCameraManager();
    /// One-shot: log the live PCM vtable address, its RVA, and each slot's
    /// target RVA. Read-only. Used to map slots to Ghidra (NOTES.md plan).
    void DumpVtable(uintptr_t pcm);
    /// Per-frame read-only diff: log PCM byte offsets whose float value changed
    /// since the last frame. Reveals the POV / camera-cache rotation offsets.
    void WatchPov(uintptr_t pcm);
    /// Install the MinHook on the configured UpdateCamera vtable slot. One-shot.
    /// No-op (and logs) if the slot is unset or out of range.
    bool InstallDecoupledHook(uintptr_t pcm);

    HeadTracking&             m_tracking;
    Mode                      m_mode = Mode::ControlRotation;
    std::atomic<FRotator*>    m_rotationSlot{nullptr};
    bool                      m_resolveLogged = false; // throttle resolve logs
    uint64_t                  m_framesSinceResolve = 0;

    std::atomic<uintptr_t>    m_pcm{0};                // live PlayerCameraManager
    bool                      m_vtableDumped = false;
    bool                      m_hookInstalled = false;
};

}  // namespace ueht
