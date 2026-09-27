#pragma once

// The HeadTracking.ini reader of the last build that read the file in its
// pre-canonical layout, frozen so a player updating from any older build is
// converted exactly as that build read the file. Nothing in this folder is ever
// edited. Three things differ from the reader it was taken from
// (ueht::Config::LoadFromFile): it fills this frozen copy of that build's Config
// and defaults rather than the runtime type, it says whether it found a file,
// and it carries the two functions that turned what it read into startup state,
// ParseVk from mods/HeadTracking.cpp and ParseMode from mods/UnrealCamera.cpp.
// ParseVk names its keys by their virtual-key codes where the original took them
// from core's cameraunlock::input::VK, which is not frozen. It writes nothing, as
// the reader it was taken from wrote nothing.

#include <cstdint>
#include <string>
#include <vector>

#include "cameraunlock/config/legacy_import.h"

namespace ueht::legacy {

struct Config {
    // [Network]
    uint16_t udp_port = 4242;

    // [Tracking]
    float yaw_sens = 1.0f;
    float pitch_sens = 1.0f;
    float roll_sens = 0.5f;
    bool invert_yaw = false;
    bool invert_pitch = false;
    bool invert_roll = false;
    float local_smoothing = 0.0f;
    float remote_smoothing = 0.15f;
    float deadzone = 0.5f;

    // [Hotkeys], key names ParseVk turns into virtual-key codes.
    std::string toggle_key = "End";
    std::string yaw_mode_key = "PageDown";
    std::string position_key = "PageUp";

    // [Camera]
    std::string camera_mode = "controlrotation";
    bool world_space_yaw = true;
    bool dump_vtable = false;
    bool watch_pov = false;
    int update_camera_slot = -1;
    uint32_t pov_offset = 0xBD4;
    uint32_t cache_offset = 0;

    // [Position]
    bool position_enabled = true;
    float pos_sens_x = 1.0f;
    float pos_sens_y = 1.0f;
    float pos_sens_z = 1.0f;
    bool invert_pos_x = false;
    bool invert_pos_y = false;
    bool invert_tracker_z = false;
    float pos_limit_x = 0.30f;
    float pos_limit_y = 0.20f;
    float pos_limit_z = 0.40f;
    float pos_limit_z_back = 0.10f;
    uint32_t location_offset = 0x3F8;

    // [Logging]
    bool log_to_file = true;
    std::string log_path;
};

enum class ReadStatus {
    Read,
    // No file at the path. The Config holds the defaults.
    Absent,
};

// Fill `out`, a default-constructed Config, from the INI at `ini_path` (an ANSI
// path, as the published build opened it); absent keys keep their defaults.
ReadStatus Read(const std::string& ini_path, Config& out);

// Every section and key Read reads, in the order it reads them. The two retired
// Smoothing keys it only warns about are among them.
std::vector<cameraunlock::config::LegacyKey> ReadKeys();

// The virtual-key code a [Hotkeys] name bound, 0 for a name that bound nothing.
int ParseVk(const std::string& name);

// Whether a [Camera] Mode value ran the decoupled UpdateCamera path. Any other
// value ran the ControlRotation path.
bool IsUpdateCameraMode(const std::string& mode);

}  // namespace ueht::legacy
