#pragma once

// The oracle: the HeadTracking.ini reader of the newest published build (the rolling `dev`
// pre-release, 10eb679), with ParseVk and ParseMode, the two functions that turned what it read
// into startup state, compiled from oracle/ with the core sources they included at its pin
// (3465659). The library that builds it renames `ueht` and `cameraunlock` at compile time so it
// links beside the current core and the current mod. This header names no core or mod type, so
// the test includes it without the renaming.

#include <cstdint>
#include <string>

namespace abzu_oracle_view {

struct OracleConfig {
    uint16_t udp_port;
    float yaw_sens, pitch_sens, roll_sens;
    bool invert_yaw, invert_pitch, invert_roll;
    float local_smoothing, remote_smoothing;
    float deadzone;
    std::string toggle_key, yaw_mode_key, position_key;
    std::string camera_mode;
    bool world_space_yaw;
    bool dump_vtable, watch_pov;
    int update_camera_slot;
    uint32_t pov_offset, cache_offset;
    bool position_enabled;
    float pos_sens_x, pos_sens_y, pos_sens_z;
    bool invert_pos_x, invert_pos_y, invert_pos_z;
    float pos_limit_x, pos_limit_y, pos_limit_z, pos_limit_z_back;
    uint32_t location_offset;
    bool log_to_file;
    std::string log_path;
};

// What the published build's Framework ran: Config::LoadFromFile on a default Config, with the
// INI at `ini_path`.
OracleConfig RunOracle(const std::string& ini_path);

// The published build's ParseVk: the virtual-key code a [Hotkeys] name bound, 0 for none.
int OracleParseVk(const std::string& name);

// The published build's ParseMode: whether a [Camera] Mode value ran the UpdateCamera path.
bool OracleIsUpdateCamera(const std::string& mode);

}  // namespace abzu_oracle_view
