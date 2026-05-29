#include "UnrealCamera.hpp"

#include "Framework.hpp"
#include "HeadTracking.hpp"
#include "UEEngine.hpp"
#include "utility/Logging.hpp"
#include "utility/SafeMemory.hpp"

#include "cameraunlock/hooks/hook_manager.h"
#include "cameraunlock/math/angle_utils.h"
#include "cameraunlock/math/quat4.h"

#include <windows.h>
#include <psapi.h>

#include <cmath>

namespace ueht {

namespace {
/// Normalize angle to (-180, 180].
float Wrap180(float deg) {
    return cameraunlock::math::NormalizeAngle(deg);
}

/// SEH-guarded slot write. Free function so the calling method can hold C++
/// objects with destructors (MSVC C2712: __try can't coexist with object
/// unwinding in the same function, even with /EHa).
bool TryWriteRotation(FRotator* slot, float pitch, float yaw, float roll) {
    __try {
        slot->Pitch = pitch;
        slot->Yaw   = yaw;
        slot->Roll  = roll;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

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
// UpdateCamera (see NOTES.md discovery flow), so the prototype matches by
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

/// OpenTrack -> UE world unit scale. Position arrives processed to meters; UE
/// world space is centimeters.
constexpr float kMetersToUU = 100.0f;

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
// The offset (sway/heave/surge, meters) is mapped into world space through the
// camera's clean horizon-locked yaw so leaning follows body orientation, not the
// head-rotated view (CameraUnlock 6DOF doctrine). Yaw-only basis + world up keeps
// the lean roll-independent. Engine rewrites a clean Location each frame, so this
// never accumulates.
bool TryAddPosition(uintptr_t loc_addr, float cleanYawDeg,
                    float sway, float heave, float surge) {
    __try {
        const float yr = cleanYawDeg * 0.01745329252f;  // deg -> rad
        const float cy = std::cos(yr), sy = std::sin(yr);
        const float fwd = surge * kMetersToUU;
        const float rgt = sway  * kMetersToUU;
        const float up  = heave * kMetersToUU;
        auto* v = reinterpret_cast<FVector*>(loc_addr);
        v->X = v->X + (cy * fwd - sy * rgt);  // UE forward (+X)
        v->Y = v->Y + (sy * fwd + cy * rgt);  // UE right   (+Y)
        v->Z = v->Z + up;                     // UE up      (+Z)
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __fastcall UpdateCameraDetour(void* pcm, float dt) {
    g_origUpdateCamera(pcm, dt);

    auto* tracking = g_hookTracking;
    if (tracking == nullptr || pcm == nullptr) return;
    if (!tracking->Enabled()) return;

    const auto base    = reinterpret_cast<uintptr_t>(pcm);
    const uint32_t rotOff = g_povOffset.load(std::memory_order_relaxed);

    // Position first: it needs the engine's clean (pre-injection) camera yaw so
    // the lean basis tracks body orientation, not the head-tracked view.
    if (tracking->PositionEnabled()) {
        const uint32_t locOff = g_locationOffset.load(std::memory_order_relaxed);
        if (locOff != 0) {
            const auto posn = tracking->CurrentPosition();
            if (posn.valid) {
                float cleanYaw = 0.0f;
                if (rotOff != 0) {
                    FRotator clean{};
                    if (SafeRead(base + rotOff, clean)) cleanYaw = clean.Yaw;
                }
                TryAddPosition(base + locOff, cleanYaw, posn.x, posn.y, posn.z);
            }
        }
    }

    const auto pose = tracking->CurrentPose();
    if (!pose.IsValid()) return;

    const bool worldYaw = tracking->WorldSpaceYaw();
    const auto injectAt = [&](uint32_t off) {
        if (off == 0) return;
        if (worldYaw) TryAddDelta (base + off, pose.pitch, pose.yaw, pose.roll);
        else          TryApplyLocal(base + off, pose.pitch, pose.yaw, pose.roll);
    };
    injectAt(rotOff);
    injectAt(g_cacheOffset.load(std::memory_order_relaxed));
}

UnrealCamera::Mode ParseMode(const std::string& s) {
    if (s == "updatecamera" || s == "UpdateCamera") return UnrealCamera::Mode::UpdateCamera;
    return UnrealCamera::Mode::ControlRotation;
}

uintptr_t HostModuleBase() {
    return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
}

}  // namespace

std::optional<std::string> UnrealCamera::OnInitialize() {
    m_mode = ParseMode(Framework::Get().Cfg().camera_mode);
    UEHT_LOG(Info, "UnrealCamera: camera_mode=%s; deferring resolution until engine is alive.",
             m_mode == Mode::UpdateCamera ? "updatecamera (decoupled)" : "controlrotation (coupled)");
    return std::nullopt;
}

void UnrealCamera::OnFrame() {
    if (m_mode == Mode::UpdateCamera) {
        TickDecoupled();
    } else {
        TickControlRotation();
    }
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
    if (!m_hookInstalled && cfg.update_camera_slot >= 0) {
        if (InstallDecoupledHook(pcm)) m_hookInstalled = true;
    }
    // Injection itself happens inside UpdateCameraDetour on the game thread.
}

void UnrealCamera::TickControlRotation() {
    auto* slot = m_rotationSlot.load(std::memory_order_acquire);
    if (slot == nullptr) {
        // Try to resolve every ~120 frames to avoid hammering the scanner.
        if ((m_framesSinceResolve++ % 120) == 0) {
            if (Resolve()) {
                slot = m_rotationSlot.load(std::memory_order_acquire);
                UEHT_LOG(Info, "UnrealCamera: camera rotation slot at %p", slot);
            }
        }
        if (slot == nullptr) return;
    }

    if (!m_tracking.Enabled()) return;

    const auto pose = m_tracking.CurrentPose();
    if (!pose.IsValid()) return;

    // Each frame we read the slot, subtract our last-applied delta to recover
    // the engine's intended rotation, then write engine_intent + new delta.
    static thread_local FRotator s_lastDelta{};

    // Read through an SEH guard: a level transition can free the
    // PlayerController, leaving this slot dangling. An unguarded read would
    // fault here and crash the host before the write guard below could catch
    // it and re-resolve.
    FRotator current{};
    if (!SafeRead(reinterpret_cast<uintptr_t>(slot), current)) {
        UEHT_LOG(Warn, "UnrealCamera: rotation slot faulted on read; re-resolving.");
        m_rotationSlot.store(nullptr, std::memory_order_release);
        s_lastDelta = {};
        return;
    }

    // Diagnostic: every ~300 frames, log the slot value BEFORE our write to see
    // whether UE has been overwriting our previous write (architectural test).
    static thread_local int s_diagCounter = 0;
    const bool diag = ((s_diagCounter++ % 300) == 0);

    FRotator engineIntent;
    engineIntent.Pitch = Wrap180(current.Pitch - s_lastDelta.Pitch);
    engineIntent.Yaw   = Wrap180(current.Yaw   - s_lastDelta.Yaw);
    engineIntent.Roll  = Wrap180(current.Roll  - s_lastDelta.Roll);

    FRotator delta;
    delta.Pitch = pose.pitch;
    delta.Yaw   = pose.yaw;
    delta.Roll  = pose.roll;

    const FRotator wrote{engineIntent.Pitch + delta.Pitch,
                         engineIntent.Yaw   + delta.Yaw,
                         engineIntent.Roll  + delta.Roll};

    if (!TryWriteRotation(slot, wrote.Pitch, wrote.Yaw, wrote.Roll)) {
        UEHT_LOG(Warn, "UnrealCamera: rotation slot faulted; re-resolving.");
        m_rotationSlot.store(nullptr, std::memory_order_release);
        s_lastDelta = {};
        return;
    }
    if (diag) {
        UEHT_LOG(Info,
            "UnrealCamera diag: pre=(P=%.2f Y=%.2f R=%.2f) lastDelta=(P=%.2f Y=%.2f R=%.2f) "
            "engineIntent=(P=%.2f Y=%.2f R=%.2f) newDelta=(P=%.2f Y=%.2f R=%.2f) wrote=(P=%.2f Y=%.2f R=%.2f)",
            current.Pitch, current.Yaw, current.Roll,
            s_lastDelta.Pitch, s_lastDelta.Yaw, s_lastDelta.Roll,
            engineIntent.Pitch, engineIntent.Yaw, engineIntent.Roll,
            delta.Pitch, delta.Yaw, delta.Roll,
            wrote.Pitch, wrote.Yaw, wrote.Roll);
    }

    s_lastDelta = delta;
}

void UnrealCamera::OnShutdown() {
    g_hookTracking = nullptr;  // stop the detour touching tracking after teardown
}

// ---------------------------------------------------------------------------
// ControlRotation resolution (default path)
// ---------------------------------------------------------------------------

bool UnrealCamera::Resolve() {
    const auto version = ue::DetectEngineVersion();
    if (!version.valid()) {
        if (!m_resolveLogged) {
            UEHT_LOG(Warn, "UnrealCamera: host EXE doesn't report a UE major version.");
            m_resolveLogged = true;
        }
        return false;
    }

    const auto offsets = ue::OffsetsFor(version);
    if (!offsets) {
        if (!m_resolveLogged) {
            UEHT_LOG(Warn, "UnrealCamera: no offset table for UE %s.", version.ToString().c_str());
            m_resolveLogged = true;
        }
        return false;
    }

    const auto gengine = ue::LocateGEngine();
    if (gengine == 0) {
        if (!m_resolveLogged) {
            UEHT_LOG(Warn, "UnrealCamera: GEngine not located yet for UE %s.",
                     version.ToString().c_str());
            m_resolveLogged = true;
        }
        return false;
    }

    FRotator* rot = WalkToRotation(gengine, *offsets);
    if (!rot) return false;
    m_rotationSlot.store(rot, std::memory_order_release);
    m_resolveLogged = false;
    return true;
}

uintptr_t UnrealCamera::WalkToPlayerController(uintptr_t gengine, const ue::EngineOffsets& o) {
    uintptr_t viewport = 0;
    if (!SafeRead(gengine + o.engine_to_game_instance, viewport) || viewport == 0) {
        UEHT_LOG(Warn, "Walk: GameViewport ptr null at GEngine+0x%zX", o.engine_to_game_instance);
        return 0;
    }
    uintptr_t game_instance = 0;  // UGameViewportClient::GameInstance at +0x88 (UE 4.12 ABZU)
    if (!SafeRead(viewport + 0x88, game_instance) || game_instance == 0) {
        UEHT_LOG(Warn, "Walk: GameInstance ptr null at Viewport+0x88");
        return 0;
    }
    uintptr_t local_players_data = 0;
    if (!SafeRead(game_instance + o.game_instance_to_local_players, local_players_data) ||
        local_players_data == 0) {
        UEHT_LOG(Warn, "Walk: LocalPlayers data null at GI+0x%zX", o.game_instance_to_local_players);
        return 0;
    }
    uintptr_t local_player = 0;
    if (!SafeRead(local_players_data, local_player) || local_player == 0) {
        UEHT_LOG(Warn, "Walk: LocalPlayers[0] null");
        return 0;
    }
    uintptr_t player_controller = 0;
    if (!SafeRead(local_player + o.local_player_to_player_controller, player_controller) ||
        player_controller == 0) {
        UEHT_LOG(Warn, "Walk: PlayerController null at LP+0x%zX", o.local_player_to_player_controller);
        return 0;
    }
    return player_controller;
}

FRotator* UnrealCamera::WalkToRotation(uintptr_t gengine, const ue::EngineOffsets& o) {
    const uintptr_t pc = WalkToPlayerController(gengine, o);
    if (pc == 0) return nullptr;
    const uintptr_t rot_addr = pc + o.controller_to_control_rotation;
    UEHT_LOG(Info, "WalkToRotation: PC=0x%llX -> ControlRotation @ 0x%llX",
             (unsigned long long)pc, (unsigned long long)rot_addr);
    return reinterpret_cast<FRotator*>(rot_addr);
}

// ---------------------------------------------------------------------------
// Decoupled (UpdateCamera) path
// ---------------------------------------------------------------------------

uintptr_t UnrealCamera::ResolveCameraManager() {
    const auto version = ue::DetectEngineVersion();
    if (!version.valid()) return 0;
    const auto offsets = ue::OffsetsFor(version);
    if (!offsets || offsets->player_controller_to_camera_manager == 0) {
        if (!m_resolveLogged) {
            UEHT_LOG(Warn, "UnrealCamera: no PlayerCameraManager offset for UE %s.",
                     version.ToString().c_str());
            m_resolveLogged = true;
        }
        return 0;
    }
    const auto gengine = ue::LocateGEngine();
    if (gengine == 0) return 0;

    const uintptr_t pc = WalkToPlayerController(gengine, *offsets);
    if (pc == 0) return 0;

    uintptr_t pcm = 0;
    if (!SafeRead(pc + offsets->player_controller_to_camera_manager, pcm) || pcm == 0) {
        UEHT_LOG(Warn, "UnrealCamera: PlayerCameraManager null at PC+0x%zX",
                 offsets->player_controller_to_camera_manager);
        return 0;
    }
    m_pcm.store(pcm, std::memory_order_release);
    m_resolveLogged = false;
    UEHT_LOG(Info, "UnrealCamera: PlayerCameraManager @ 0x%llX (PC=0x%llX)",
             (unsigned long long)pcm, (unsigned long long)pc);
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
    static int   s_frame = 0;

    if (!s_have) {
        for (int i = 0; i < kCount; ++i) {
            SafeRead(pcm + kLo + static_cast<uint32_t>(i) * kStep, s_prev[i]);
        }
        s_have = true;
        UEHT_LOG(Info, "WatchPov: baseline captured for PCM+0x%X..0x%X; move the camera to surface offsets.",
                 kLo, kHi);
        return;
    }

    // Throttle so a moving camera doesn't flood the log every frame.
    const bool emit = ((s_frame++ % 30) == 0);
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
    if (slot < 0 || slot >= 256) {
        UEHT_LOG(Warn, "InstallDecoupledHook: update_camera_slot=%d out of range; staying dormant.", slot);
        return false;
    }

    uintptr_t vtable = 0;
    if (!SafeRead(pcm, vtable) || vtable == 0) return false;

    uintptr_t target = 0;
    if (!SafeRead(vtable + static_cast<uintptr_t>(slot) * 8, target) || target == 0) {
        UEHT_LOG(Warn, "InstallDecoupledHook: vtable slot %d unreadable/null", slot);
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
        UEHT_LOG(Error, "InstallDecoupledHook: CreateHook failed for slot %d (target 0x%llX)",
                 slot, (unsigned long long)target);
        g_hookTracking = nullptr;
        return false;
    }
    if (mh.EnableHook(reinterpret_cast<void*>(target)) != HookStatus::Ok) {
        UEHT_LOG(Error, "InstallDecoupledHook: EnableHook failed for slot %d", slot);
        g_hookTracking = nullptr;
        return false;
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
