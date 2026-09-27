// Compiled into the oracle library only, with `ueht` and `cameraunlock` renamed, so
// "ueht/Config.hpp" here is the published build's (oracle/include/ueht/Config.hpp).
#include "ueht/Config.hpp"
#include "oracle_adapter.h"

#include <cctype>
#include <cstdlib>
#include <string>

#include "cameraunlock/input/hotkey_poller.h"

namespace ueht {
namespace {
#include "parse_vk.inc"

struct UnrealCamera {
    enum class Mode { ControlRotation, UpdateCamera };
};
#include "parse_mode.inc"
}  // namespace
}  // namespace ueht

namespace abzu_oracle_view {

OracleConfig RunOracle(const std::string& ini_path) {
    ueht::Config c;
    ueht::Config::LoadFromFile(ini_path, c);
    OracleConfig o{};
    o.udp_port = c.udp_port;
    o.yaw_sens = c.yaw_sens;
    o.pitch_sens = c.pitch_sens;
    o.roll_sens = c.roll_sens;
    o.invert_yaw = c.invert_yaw;
    o.invert_pitch = c.invert_pitch;
    o.invert_roll = c.invert_roll;
    o.local_smoothing = c.local_smoothing;
    o.remote_smoothing = c.remote_smoothing;
    o.deadzone = c.deadzone;
    o.toggle_key = c.toggle_key;
    o.yaw_mode_key = c.yaw_mode_key;
    o.position_key = c.position_key;
    o.camera_mode = c.camera_mode;
    o.world_space_yaw = c.world_space_yaw;
    o.dump_vtable = c.dump_vtable;
    o.watch_pov = c.watch_pov;
    o.update_camera_slot = c.update_camera_slot;
    o.pov_offset = c.pov_offset;
    o.cache_offset = c.cache_offset;
    o.position_enabled = c.position_enabled;
    o.pos_sens_x = c.pos_sens_x;
    o.pos_sens_y = c.pos_sens_y;
    o.pos_sens_z = c.pos_sens_z;
    o.invert_pos_x = c.invert_pos_x;
    o.invert_pos_y = c.invert_pos_y;
    o.invert_pos_z = c.invert_pos_z;
    o.pos_limit_x = c.pos_limit_x;
    o.pos_limit_y = c.pos_limit_y;
    o.pos_limit_z = c.pos_limit_z;
    o.pos_limit_z_back = c.pos_limit_z_back;
    o.location_offset = c.location_offset;
    o.log_to_file = c.log_to_file;
    o.log_path = c.log_path;
    return o;
}

int OracleParseVk(const std::string& name) { return ueht::ParseVk(name); }

bool OracleIsUpdateCamera(const std::string& mode) {
    return ueht::ParseMode(mode) == ueht::UnrealCamera::Mode::UpdateCamera;
}

}  // namespace abzu_oracle_view
