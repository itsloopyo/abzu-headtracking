#pragma once

#include <cstdint>
#include <string>

#include "cameraunlock/data/tracking_pose.h"
#include "cameraunlock/data/position_settings.h"

namespace ueht {

/// Runtime configuration. Loaded from `HeadTracking.ini` next to the game
/// executable at DLL attach time. All fields have sane defaults so a missing
/// INI is fine.
struct Config {
    // [tracking]
    uint16_t udp_port      = 4242;
    float    yaw_sens      = 1.0f;
    float    pitch_sens    = 1.0f;
    float    roll_sens     = 0.5f;
    bool     invert_yaw    = false;
    bool     invert_pitch  = false;
    bool     invert_roll   = false;
    float    smoothing     = 0.2f;
    float    deadzone      = 0.5f;

    // [hotkeys] - nav-cluster virtual-key names matched by hotkey_poller.
    // Ctrl+Shift+T/Y/H chord equivalents are registered unconditionally in code.
    std::string recenter_key  = "Home";
    std::string toggle_key    = "End";
    std::string yaw_mode_key  = "PageDown";  // toggle world-space vs camera-local yaw
    std::string position_key  = "PageUp";    // toggle 6DOF positional tracking

    // [camera] - how head tracking reaches the rendered view.
    //   "controlrotation": write AController::ControlRotation (the engine's
    //       source rotation). Works today, but COUPLES head movement to the
    //       game's control/movement basis (the diver steers with your head).
    //   "updatecamera": decoupled path. Hook the PlayerCameraManager's per-frame
    //       camera-update virtual and add the head delta to the rendered POV
    //       only, leaving ControlRotation clean. Dormant until update_camera_slot
    //       is set to the confirmed vtable index (see NOTES.md discovery plan).
    std::string camera_mode = "controlrotation";
    // Yaw axis. true (default) = horizon-locked: head-yaw rotates around the
    //   world up-axis regardless of camera pitch ("up is a constant"). false =
    //   camera-local: head-yaw rotates around the camera's current up-axis,
    //   which leans/rolls the view at extreme pitch. Runtime-toggled by
    //   yaw_mode_key; this is just the startup value.
    bool world_space_yaw = true;
    // Diagnostics for the static-then-confirm discovery flow (read-only, safe):
    bool dump_vtable = false;  // on PCM resolve, log the live PCM vtable + slot RVAs
    bool watch_pov   = false;  // per-frame, log which PCM byte offsets change
    // Decoupled-hook wiring. Defaults leave it dormant.
    int      update_camera_slot = -1;     // PCM vtable index of UpdateCamera; -1 = off
    uint32_t pov_offset         = 0xBD4;  // PCM-relative FRotator the renderer reads
    uint32_t cache_offset       = 0;      // optional second FRotator (camera cache); 0 = off

    // [position] - 6DOF positional tracking. Decoupled like rotation: the head
    // offset is added to the rendered camera-cache FVector Location only, never
    // to anything game logic reads. OpenTrack position arrives in meters; it is
    // scaled to UE world units (cm) at the injection site. location_offset is
    // the cache POV Location (FMinimalViewInfo.Location), immediately before the
    // rotation at pov_offset; for ABZU it is pov_offset - 0xC = 0x3F8.
    bool     position_enabled = true;
    float    pos_sens_x       = 1.0f;
    float    pos_sens_y       = 1.0f;
    float    pos_sens_z       = 1.0f;
    bool     invert_pos_x     = false;
    bool     invert_pos_y     = false;
    bool     invert_pos_z     = false;
    float    pos_limit_x      = 0.30f;
    float    pos_limit_y      = 0.20f;
    float    pos_limit_z      = 0.40f;   // forward lean (generous)
    float    pos_limit_z_back = 0.10f;   // backward lean (restricted, avoids clipping)
    float    pos_smoothing    = 0.15f;
    uint32_t location_offset  = 0x3F8;   // PCM-relative FVector the renderer reads; 0 = off

    // [debug]
    bool log_to_file = true;
    std::string log_path;  // resolved at runtime if empty

    cameraunlock::SensitivitySettings AsSensitivity() const {
        cameraunlock::SensitivitySettings s;
        s.yaw          = yaw_sens;
        s.pitch        = pitch_sens;
        s.roll         = roll_sens;
        s.invert_yaw   = invert_yaw;
        s.invert_pitch = invert_pitch;
        s.invert_roll  = invert_roll;
        return s;
    }

    cameraunlock::DeadzoneSettings AsDeadzone() const {
        cameraunlock::DeadzoneSettings d;
        d.yaw = d.pitch = d.roll = deadzone;
        return d;
    }

    cameraunlock::PositionSettings AsPositionSettings() const {
        cameraunlock::PositionSettings p;
        p.sensitivity_x = pos_sens_x;
        p.sensitivity_y = pos_sens_y;
        p.sensitivity_z = pos_sens_z;
        p.invert_x      = invert_pos_x;
        p.invert_y      = invert_pos_y;
        p.invert_z      = invert_pos_z;
        p.limit_x       = pos_limit_x;
        p.limit_y       = pos_limit_y;
        p.limit_z       = pos_limit_z;
        p.limit_z_back  = pos_limit_z_back;
        p.smoothing     = pos_smoothing;
        return p;
    }

    /// Load from an INI file. Missing fields keep their defaults.
    /// Returns false only if the file exists but cannot be parsed.
    static bool LoadFromFile(const std::string& path, Config& out);

    /// Locate the INI next to the host process's executable.
    static std::string DefaultIniPathNextToHostExe();
};

}  // namespace ueht
