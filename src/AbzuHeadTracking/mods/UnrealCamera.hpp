#pragma once

#include <atomic>
#include <cstdint>

#include "Mod.hpp"
#include "builds/build_profile.hpp"

namespace ueht {

class HeadTracking;

/// FRotator as Unreal lays it out in memory: { Pitch, Yaw, Roll } floats (deg).
/// We don't depend on Unreal headers - this matches UE4.x and UE5.x.
#pragma pack(push, 4)
struct FRotator {
    float Pitch = 0.0f;
    float Yaw   = 0.0f;
    float Roll  = 0.0f;
};
#pragma pack(pop)

/// Drives the active player's camera each frame from the processed OpenTrack
/// pose in `HeadTracking`: resolve the live PlayerCameraManager, hook its
/// per-frame camera-update virtual, and add the head delta to the rendered POV
/// only, leaving ControlRotation, and so the diver's steering, clean.
///
/// Resolution waits until the engine is live and retries until it succeeds.
/// Until then the mod is a no-op.
class UnrealCamera final : public Mod {
public:
    UnrealCamera(HeadTracking& tracking, const builds::EngineOffsets& offsets)
        : m_tracking(tracking), m_offsets(offsets) {}

    std::string_view Name() const override { return "UnrealCamera"; }

    std::optional<std::string> OnInitialize() override;
    void OnFrame() override;

private:
    /// Per-frame tick: resolve the PCM and install the UpdateCamera hook.
    void TickDecoupled();

    /// How far WalkToPlayerController got on its last run. Only a change is
    /// worth a log line - the walk is retried until the level is up.
    enum class Stage { None, Viewport, GameInstance, LocalPlayers, LocalPlayer, PlayerController, CameraManager };

    /// Walk GEngine -> GameViewport -> GameInstance -> LocalPlayer[0] ->
    /// PlayerController -> PlayerCameraManager. Returns the PCM pointer or 0.
    /// SEH-guarded.
    uintptr_t WalkToCameraManager(uintptr_t gengine);

    // --- Decoupled (UpdateCamera) path -------------------------------------
    /// Resolve the live APlayerCameraManager instance. Returns 0 until ready.
    uintptr_t ResolveCameraManager();
    /// One-shot: log the live PCM vtable address, its RVA, and each slot's
    /// target RVA. Read-only. Used to map slots to Ghidra.
    void DumpVtable(uintptr_t pcm);
    /// Per-frame read-only diff: log PCM byte offsets whose float value changed
    /// since the last frame. Reveals the POV / camera-cache rotation offsets.
    void WatchPov(uintptr_t pcm);
    /// Install the MinHook on the configured UpdateCamera vtable slot. One-shot.
    /// No-op (and logs) if the slot is unset or out of range.
    bool InstallDecoupledHook(uintptr_t pcm);

    HeadTracking&             m_tracking;
    const builds::EngineOffsets m_offsets;
    Stage                     m_walkStall = Stage::None;    // last stage the GEngine walk stopped at
    uint64_t                  m_framesSinceResolve = 0;

    std::atomic<uintptr_t>    m_pcm{0};                // live PlayerCameraManager
    bool                      m_vtableDumped = false;
    bool                      m_hookInstalled = false;
    bool                      m_hookAbandoned = false; // permanent failure; stop retrying
    bool                      m_hookSlotWarned = false;
};

}  // namespace ueht
