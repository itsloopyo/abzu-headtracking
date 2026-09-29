#include "UnrealCamera.hpp"

#include "Framework.hpp"
#include "HeadTracking.hpp"
#include "LeanOffset.hpp"
#include "UEEngine.hpp"
#include "utility/Logging.hpp"
#include "utility/SafeMemory.hpp"

#include "cameraunlock/hooks/hook_manager.h"
#include "cameraunlock/math/quat4.h"

#include <windows.h>
#include <psapi.h>

#include <cmath>

namespace ueht {

namespace {
// --- Decoupled (UpdateCamera) hook -----------------------------------------
//
// UE4 APlayerCameraManager::UpdateCamera(float DeltaTime) is the per-frame,
// game-thread function that recomputes the camera POV from ControlRotation and
// fills the camera cache the renderer reads. We hook its tail: after the engine
// has written the clean POV, we add the head delta to the rendered rotation(s).
// ControlRotation is never touched, so aim/movement/game-logic stay clean - the
// head moves only what the player sees. Because the engine recomputes a clean
// POV every frame, our addition does not accumulate (no read-modify-subtract).
//
// The signature is fixed by UE: (this=PCM in RCX, float DeltaTime in XMM1). The
// hook only ever arms on a vtable slot the operator has confirmed is
// UpdateCamera via the DumpVtable diagnostic, so the prototype matches by
// construction. Statics here mirror the D3D11 Present hook pattern - MinHook
// detours must be free functions.
using UpdateCameraFn = void(__fastcall*)(void* pcm, float dt);

UpdateCameraFn        g_origUpdateCamera = nullptr;
HeadTracking*         g_hookTracking     = nullptr;
std::atomic<uint32_t> g_povOffset{0};
std::atomic<uint32_t> g_cacheOffset{0};
std::atomic<uint32_t> g_locationOffset{0};

/// FVector as Unreal lays it out (UE4.12: 3 floats, world units = cm).
#pragma pack(push, 4)
struct FVector { float X, Y, Z; };
#pragma pack(pop)

// World-space (horizon-locked) yaw: add the head delta straight onto the FRotator.
// A UE FRotator's Yaw is intrinsically a rotation about the world up-axis, so this
// pans the view around vertical regardless of camera pitch. Pitch/roll fold onto
// the engine's pitch/roll (camera-local). The engine rewrites a clean POV each
// frame, so the addition never accumulates.
bool TryAddDelta(uintptr_t rotator_addr, float dPitch, float dYaw, float dRoll) {
    __try {
        auto* r = reinterpret_cast<FRotator*>(rotator_addr);
        r->Pitch = r->Pitch + dPitch;
        r->Yaw   = r->Yaw   + dYaw;
        r->Roll  = r->Roll  + dRoll;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Camera-local yaw: compose the head rotation in the camera's current frame
// (Q_engine * Q_head), so head-yaw rotates about the camera's own up-axis and
// leans/rolls the view at extreme pitch. Reads the clean rotation the engine just
// wrote, composes, and writes the result back absolutely. Zero head input round-
// trips to the clean value, so toggling with a still head does not jump the view.
bool TryApplyLocal(uintptr_t rotator_addr, float dPitch, float dYaw, float dRoll) {
    using cameraunlock::math::Quat4;
    __try {
        auto* r = reinterpret_cast<FRotator*>(rotator_addr);
        const Quat4 qEngine = Quat4::FromYawPitchRoll(r->Yaw, r->Pitch, r->Roll);
        const Quat4 qHead   = Quat4::FromYawPitchRoll(dYaw, dPitch, dRoll);
        const Quat4 qResult = (qEngine * qHead).Normalized();
        float yaw{}, pitch{}, roll{};
        qResult.ToEulerYXZ(yaw, pitch, roll);
        r->Pitch = pitch;
        r->Yaw   = yaw;
        r->Roll  = roll;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Add the processed head-position offset to the rendered camera-cache Location.
// LeanWorldOffset owns the axis mapping; this only performs the guarded write.
// The engine rewrites a clean Location each frame, so this never accumulates.
bool TryAddPosition(uintptr_t loc_addr, const LeanOffsetUU& delta) {
    __try {
        auto* v = reinterpret_cast<FVector*>(loc_addr);
        v->X = v->X + delta.x;
        v->Y = v->Y + delta.y;
        v->Z = v->Z + delta.z;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __fastcall UpdateCameraDetour(void* pcm, float dt) {
    g_origUpdateCamera(pcm, dt);

    // Set before the hook is enabled and never cleared: the mod is never
    // unloaded (DllMain pins it).
    HeadTracking* const tracking = g_hookTracking;
    tracking->Update();
    if (!tracking->Enabled()) return;

    const auto base    = reinterpret_cast<uintptr_t>(pcm);
    const uint32_t rotOff = g_povOffset.load(std::memory_order_relaxed);

    // Position first: it needs the engine's clean (pre-injection) camera yaw so
    // the lean basis tracks body orientation, not the head-tracked view.
    if (tracking->PositionEnabled()) {
        const uint32_t locOff = g_locationOffset.load(std::memory_order_relaxed);
        if (locOff != 0) {
            const HeadPosition& posn = tracking->CurrentPosition();
            if (posn.valid) {
                float cleanYaw = 0.0f;
                if (rotOff != 0) {
                    FRotator clean{};
                    if (SafeRead(base + rotOff, clean)) cleanYaw = clean.Yaw;
                }
                TryAddPosition(base + locOff,
                               LeanWorldOffset(cleanYaw, posn.x, posn.y, posn.z));
            }
        }
    }

    const cameraunlock::TrackingPose& pose = tracking->CurrentPose();
    if (!pose.IsValid()) return;

    // ABZU's roll runs opposite the tracker's. Every build shipped
    // InvertRoll=true to correct it, and the correction lives here now.
    const float roll = -pose.roll;
    const bool worldYaw = tracking->WorldSpaceYaw();
    const auto injectAt = [&](uint32_t off) {
        if (off == 0) return;
        if (worldYaw) TryAddDelta (base + off, pose.pitch, pose.yaw, roll);
        else          TryApplyLocal(base + off, pose.pitch, pose.yaw, roll);
    };
    injectAt(rotOff);
    injectAt(g_cacheOffset.load(std::memory_order_relaxed));
}

uintptr_t HostModuleBase() {
    return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
}

}  // namespace

std::optional<std::string> UnrealCamera::OnInitialize() {
    UEHT_LOG(Info, "UnrealCamera: deferring resolution until the engine is alive.");
    return std::nullopt;
}

void UnrealCamera::OnFrame() {
    TickDecoupled();
}

void UnrealCamera::TickDecoupled() {
    uintptr_t pcm = m_pcm.load(std::memory_order_acquire);
    if (pcm == 0) {
        if ((m_framesSinceResolve++ % 120) == 0) {
            pcm = ResolveCameraManager();
        }
        if (pcm == 0) return;
    }

    const auto& cfg = Framework::Get().Cfg();
    if (cfg.dump_vtable && !m_vtableDumped) {
        DumpVtable(pcm);
        m_vtableDumped = true;
    }
    if (cfg.watch_pov) {
        WatchPov(pcm);
    }
    if (!m_hookInstalled && !m_hookAbandoned && cfg.update_camera_slot >= 0) {
        if (InstallDecoupledHook(pcm)) m_hookInstalled = true;
    }
    // Injection itself happens inside UpdateCameraDetour on the game thread.
}

uintptr_t UnrealCamera::WalkToCameraManager(uintptr_t gengine) {
    // The whole walk is unresolvable until the level is up, and the caller
    // retries every ~120 frames, so a warn per stage per attempt is thousands of
    // duplicate lines across a splash and a level load. Report a stage only when
    // the walk stops somewhere new, which is the only thing that carries
    // information: the stage it reaches tells us how far the chain is built.
    const auto stall = [this](Stage stage, const std::string& msg) {
        if (m_walkStall != stage) {
            m_walkStall = stage;
            log::Warn(msg);
        }
        return uintptr_t{0};
    };
    const builds::EngineOffsets& o = m_offsets;

    uintptr_t viewport = 0;
    if (!SafeRead(gengine + o.engine_to_game_viewport, viewport) || viewport == 0) {
        return stall(Stage::Viewport,
                     log::Format("Walk: GameViewport null at GEngine+0x%zX", o.engine_to_game_viewport));
    }
    uintptr_t game_instance = 0;
    if (!SafeRead(viewport + o.game_viewport_to_game_instance, game_instance) || game_instance == 0) {
        return stall(Stage::GameInstance,
                     log::Format("Walk: GameInstance null at Viewport+0x%zX", o.game_viewport_to_game_instance));
    }
    uintptr_t local_players_data = 0;
    if (!SafeRead(game_instance + o.game_instance_to_local_players, local_players_data) ||
        local_players_data == 0) {
        return stall(Stage::LocalPlayers,
                     log::Format("Walk: LocalPlayers data null at GI+0x%zX", o.game_instance_to_local_players));
    }
    uintptr_t local_player = 0;
    if (!SafeRead(local_players_data, local_player) || local_player == 0) {
        return stall(Stage::LocalPlayer, "Walk: LocalPlayers[0] null");
    }
    uintptr_t player_controller = 0;
    if (!SafeRead(local_player + o.local_player_to_player_controller, player_controller) ||
        player_controller == 0) {
        return stall(Stage::PlayerController,
                     log::Format("Walk: PlayerController null at LP+0x%zX", o.local_player_to_player_controller));
    }
    uintptr_t pcm = 0;
    if (!SafeRead(player_controller + o.player_controller_to_camera_manager, pcm) || pcm == 0) {
        return stall(Stage::CameraManager,
                     log::Format("Walk: PlayerCameraManager null at PC+0x%zX (PC=0x%llX)",
                                 o.player_controller_to_camera_manager,
                                 (unsigned long long)player_controller));
    }
    m_walkStall = Stage::None;
    return pcm;
}

// ---------------------------------------------------------------------------
// Decoupled (UpdateCamera) path
// ---------------------------------------------------------------------------

uintptr_t UnrealCamera::ResolveCameraManager() {
    const auto gengine = ue::LocateGEngine(m_offsets.uengine_class_rva);
    if (gengine == 0) return 0;

    const uintptr_t pcm = WalkToCameraManager(gengine);
    if (pcm == 0) return 0;

    m_pcm.store(pcm, std::memory_order_release);
    UEHT_LOG(Info, "UnrealCamera: PlayerCameraManager @ 0x%llX", (unsigned long long)pcm);
    return pcm;
}

void UnrealCamera::DumpVtable(uintptr_t pcm) {
    uintptr_t vtable = 0;
    if (!SafeRead(pcm, vtable) || vtable == 0) {
        UEHT_LOG(Warn, "DumpVtable: PCM vtable ptr unreadable");
        return;
    }
    const uintptr_t base = HostModuleBase();
    MODULEINFO mi{};
    if (!GetModuleInformation(GetCurrentProcess(), reinterpret_cast<HMODULE>(base), &mi, sizeof(mi)) ||
        mi.SizeOfImage == 0) {
        UEHT_LOG(Warn, "DumpVtable: GetModuleInformation failed; cannot bound vtable scan.");
        return;
    }
    const uintptr_t mod_end = base + mi.SizeOfImage;

    UEHT_LOG(Info, "DumpVtable: live PCM vtable @ 0x%llX  RVA=0x%llX  (module base 0x%llX)",
             (unsigned long long)vtable, (unsigned long long)(vtable - base),
             (unsigned long long)base);

    // Walk slots until a target leaves .text (a non-code pointer / string ends
    // the vtable). 256 is a generous upper bound for a UE AActor-derived vtable.
    for (int i = 0; i < 256; ++i) {
        uintptr_t fn = 0;
        if (!SafeRead(vtable + static_cast<uintptr_t>(i) * 8, fn)) break;
        if (fn < base || fn >= mod_end) {
            UEHT_LOG(Info, "DumpVtable: slot %d -> 0x%llX (non-module; end of vtable)",
                     i, (unsigned long long)fn);
            break;
        }
        UEHT_LOG(Info, "DumpVtable: slot %3d -> RVA 0x%llX", i, (unsigned long long)(fn - base));
    }
}

void UnrealCamera::WatchPov(uintptr_t pcm) {
    // Scan a window around the known camera region and report offsets whose
    // float value changed since the previous frame. Read-only. Single-threaded
    // (render thread), so function-local state is fine.
    constexpr uint32_t kLo = 0xB00, kHi = 0xC20, kStep = 4;
    constexpr int kCount = (kHi - kLo) / kStep;
    static float s_prev[kCount];
    static bool  s_have = false;

    if (!s_have) {
        for (int i = 0; i < kCount; ++i) {
            SafeRead(pcm + kLo + static_cast<uint32_t>(i) * kStep, s_prev[i]);
        }
        s_have = true;
        UEHT_LOG(Info, "WatchPov: baseline captured for PCM+0x%X..0x%X; move the camera to surface offsets.",
                 kLo, kHi);
        return;
    }

    // One pass emits up to 72 lines, so a frame-count throttle scales the log
    // with refresh rate: 1-in-30 at 144fps is ~62 MB/hour. Space the passes on
    // the wall clock instead and cap them per session. The offsets an RE
    // session is after surface within the first couple of minutes of wiggling
    // the camera, which is what kWatchPovPasses buys at kWatchPovIntervalMs.
    constexpr uint64_t kWatchPovIntervalMs = 500;
    constexpr int kWatchPovPasses = 240;
    static uint64_t s_nextEmitMs = 0;
    static int s_passes = 0;
    const uint64_t nowMs = GetTickCount64();
    bool emit = false;
    if (s_passes < kWatchPovPasses && nowMs >= s_nextEmitMs) {
        emit = true;
        ++s_passes;
        s_nextEmitMs = nowMs + kWatchPovIntervalMs;
        if (s_passes == kWatchPovPasses) {
            UEHT_LOG(Info, "WatchPov: pass cap reached; no further offset changes will be logged.");
        }
    }
    for (int i = 0; i < kCount; ++i) {
        float cur = 0.0f;
        if (!SafeRead(pcm + kLo + static_cast<uint32_t>(i) * kStep, cur)) continue;
        if (cur != s_prev[i]) {
            if (emit && (cur > -1.0e6f && cur < 1.0e6f)) {
                UEHT_LOG(Info, "WatchPov: PCM+0x%03X changed %.3f -> %.3f",
                         kLo + i * kStep, s_prev[i], cur);
            }
            s_prev[i] = cur;
        }
    }
}

bool UnrealCamera::InstallDecoupledHook(uintptr_t pcm) {
    const auto& cfg = Framework::Get().Cfg();
    const int slot = cfg.update_camera_slot;

    // This runs every frame until it succeeds, so anything that cannot become
    // true later must stop the retry rather than log again next frame. A bad
    // config value and a MinHook rejection are both permanent; abandoning them
    // leaves the mod dormant, which is what an unhookable camera means anyway.
    const auto abandon = [this](const std::string& msg) {
        m_hookAbandoned = true;
        log::Error(msg);
        return false;
    };

    if (slot < 0 || slot >= 256) {
        return abandon(log::Format(
            "InstallDecoupledHook: update_camera_slot=%d out of range; staying dormant.", slot));
    }

    uintptr_t vtable = 0;
    if (!SafeRead(pcm, vtable) || vtable == 0) return false;

    // Transient: the PCM can be mid-construction on the frame we first see it.
    uintptr_t target = 0;
    if (!SafeRead(vtable + static_cast<uintptr_t>(slot) * 8, target) || target == 0) {
        if (!m_hookSlotWarned) {
            m_hookSlotWarned = true;
            UEHT_LOG(Warn, "InstallDecoupledHook: vtable slot %d unreadable/null; retrying", slot);
        }
        return false;
    }

    g_hookTracking = &m_tracking;
    g_povOffset.store(cfg.pov_offset, std::memory_order_release);
    g_cacheOffset.store(cfg.cache_offset, std::memory_order_release);
    g_locationOffset.store(cfg.location_offset, std::memory_order_release);

    using cameraunlock::hooks::HookManager;
    using cameraunlock::hooks::HookStatus;
    auto& mh = HookManager::Instance();

    if (mh.CreateHook(reinterpret_cast<void*>(target),
                      reinterpret_cast<void*>(&UpdateCameraDetour),
                      reinterpret_cast<void**>(&g_origUpdateCamera)) != HookStatus::Ok) {
        return abandon(log::Format(
            "InstallDecoupledHook: CreateHook failed for slot %d (target 0x%llX); staying dormant.",
            slot, (unsigned long long)target));
    }
    if (mh.EnableHook(reinterpret_cast<void*>(target)) != HookStatus::Ok) {
        return abandon(log::Format(
            "InstallDecoupledHook: EnableHook failed for slot %d; staying dormant.", slot));
    }

    UEHT_LOG(Info,
        "InstallDecoupledHook: hooked UpdateCamera slot %d @ 0x%llX (RVA 0x%llX); "
        "injecting rot POV+0x%X cache+0x%X, pos Location+0x%X. "
        "ControlRotation left clean (decoupled).",
        slot, (unsigned long long)target, (unsigned long long)(target - HostModuleBase()),
        cfg.pov_offset, cfg.cache_offset, cfg.location_offset);
    return true;
}

}  // namespace ueht
