#include "ueht/Config.hpp"

#include <windows.h>

#include <filesystem>

#include "legacy_config/legacy_config.h"

namespace ueht {

// The frozen reader in src/legacy_config reads the file; this copies what it
// read into the runtime Config.
bool Config::LoadFromFile(const std::string& path, Config& out) {
    legacy::Config read;
    if (legacy::Read(path, read) == legacy::ReadStatus::Absent) return true;  // defaults are fine

    out.udp_port           = read.udp_port;
    out.yaw_sens           = read.yaw_sens;
    out.pitch_sens         = read.pitch_sens;
    out.roll_sens          = read.roll_sens;
    out.invert_yaw         = read.invert_yaw;
    out.invert_pitch       = read.invert_pitch;
    out.invert_roll        = read.invert_roll;
    out.local_smoothing    = read.local_smoothing;
    out.remote_smoothing   = read.remote_smoothing;
    out.deadzone           = read.deadzone;
    out.toggle_key         = read.toggle_key;
    out.yaw_mode_key       = read.yaw_mode_key;
    out.position_key       = read.position_key;
    out.camera_mode        = read.camera_mode;
    out.world_space_yaw    = read.world_space_yaw;
    out.dump_vtable        = read.dump_vtable;
    out.watch_pov          = read.watch_pov;
    out.update_camera_slot = read.update_camera_slot;
    out.pov_offset         = read.pov_offset;
    out.cache_offset       = read.cache_offset;
    out.position_enabled   = read.position_enabled;
    out.pos_sens_x         = read.pos_sens_x;
    out.pos_sens_y         = read.pos_sens_y;
    out.pos_sens_z         = read.pos_sens_z;
    out.invert_pos_x       = read.invert_pos_x;
    out.invert_pos_y       = read.invert_pos_y;
    out.invert_tracker_z   = read.invert_tracker_z;
    out.pos_limit_x        = read.pos_limit_x;
    out.pos_limit_y        = read.pos_limit_y;
    out.pos_limit_z        = read.pos_limit_z;
    out.pos_limit_z_back   = read.pos_limit_z_back;
    out.location_offset    = read.location_offset;
    out.log_to_file        = read.log_to_file;
    out.log_path           = read.log_path;
    return true;
}

std::string Config::DefaultIniPathNextToHostExe() {
    char buf[MAX_PATH] = {};
    const auto n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0 || n == MAX_PATH) return "HeadTracking.ini";
    std::filesystem::path p(buf);
    return (p.parent_path() / "HeadTracking.ini").string();
}

}  // namespace ueht
