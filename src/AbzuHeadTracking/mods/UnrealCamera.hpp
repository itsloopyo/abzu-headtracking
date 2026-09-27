#pragma once

#include <atomic>
#include <cstdint>

#include "Mod.hpp"
#include "UEEngine.hpp"

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
/// Offsets shift across UE 4.x/5.x and per-game build, so resolution waits until
/// the engine is live and retries until it succeeds. Until then the mod is a
/// no-op.
class UnrealCamera final : public Mod {
public:
    explicit UnrealCamera(HeadTracking& tracking) : m_tracking(tracking) {}

    std::string_view Name() const override { return "UnrealCamera"; }

    std::optional<std::string> OnInitialize() override;
    void OnFrame() override;
    void OnShutdown() override;

private:
    /// Per-frame tick: resolve the PCM and install the UpdateCamera hook.
    void TickDecoupled();

    /// How far WalkToPlayerController got on its last run. Only a change is
    /// worth a log line - the walk is retried until the level is up.
    enum class Stage { None, Viewport, GameInstance, LocalPlayers, LocalPlayer, PlayerController };

    /// Walk GEngine -> GameViewport -> GameInstance -> LocalPlayer[0] ->
    /// PlayerController. Returns the PlayerController pointer or 0. SEH-guarded.
    uintptr_t WalkToPlayerController(uintptr_t gengine, const ue::EngineOffsets& offsets);

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
    bool                      m_resolveLogged = false; // engine/offset-table warns, once each
    Stage                     m_walkStall = Stage::None;    // last stage the GEngine walk stopped at
    uint64_t                  m_framesSinceResolve = 0;

    std::atomic<uintptr_t>    m_pcm{0};                // live PlayerCameraManager
    bool                      m_vtableDumped = false;
    bool                      m_hookInstalled = false;
    bool                      m_hookAbandoned = false; // permanent failure; stop retrying
    bool                      m_hookSlotWarned = false;
};

}  // namespace ueht
