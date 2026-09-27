#include "ueht/Config.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "legacy_config/legacy_config.h"
#include "utility/Logging.hpp"

#include "cameraunlock/config/head_tracking_config_table.h"
#include "cameraunlock/config/value_codecs.h"
#include "cameraunlock/input/key_bindings.h"
#include "cameraunlock/tracking/tracking_mode.h"

namespace ueht::config {

namespace {

namespace cfg = ::cameraunlock::config;
using C = cfg::schema::Concept;
using ::cameraunlock::input::KeyModifiers;

// What the published build's HeadTracking.ini set where it differs from the
// frozen reader's code defaults: the installer ZIP, the launcher seed and the
// committed file all carried these, so a value equal to one of them is no
// player's choice.
legacy::Config Shipped() {
    legacy::Config s;
    s.roll_sens = 1.0f;
    s.invert_roll = true;
    s.deadzone = 0.0f;
    s.camera_mode = "updatecamera";
    s.update_camera_slot = 196;
    s.pov_offset = 0x404;
    s.invert_pos_x = true;
    s.log_path = "AbzuHeadTracking.log";
    return s;
}

// A hotkey the published build bound: the key its name gave through ParseVk,
// then the Ctrl+Shift chord it always registered beside it.
std::string Keys(const std::string& name, const char* key, int chord_vk,
                 std::vector<cfg::DroppedValue>& dropped) {
    const std::string chord =
        ::cameraunlock::input::FormatKeyBindings({{KeyModifiers::kCtrl | KeyModifiers::kShift, chord_vk}});
    const std::string bound = cfg::LegacyVirtualKeyToBindings(legacy::ParseVk(name), "Hotkeys", key, dropped);
    return bound.empty() ? chord : bound + ", " + chord;
}

cfg::ImportResult RunImport(const cfg::LegacyInput& input, Config& out) {
    std::vector<cfg::DroppedValue> dropped;
    std::vector<cfg::PoseShapingValue> pose_shaping;
    const legacy::Config shipped = Shipped();

    // The published build read the file through the game folder's ANSI form.
    // Where the folder had none it read nothing and ran on the defaults, and the
    // conversion's moved defaults take their place.
    legacy::Config read;
    if (input.ansi_lossy || legacy::Read(input.ansi_path, read) == legacy::ReadStatus::Absent) {
        cfg::LegacyFollowsDefaultsIni follows;
        for (const C id : {C::UdpPort, C::EnableOnStartup, C::WorldSpaceYaw, C::LocalSmoothing, C::RemoteSmoothing,
                           C::PositionLimitX, C::PositionLimitY, C::PositionLimitYDown, C::PositionLimitZ,
                           C::PositionLimitZBack, C::ToggleKey, C::CycleTrackingModeKey, C::YawModeKey}) {
            follows.NotInLegacy(id);
        }
        follows.TrackingMode(true);
        return cfg::ImportResult::Absent({}, {}, follows.Concepts());
    }

    out.udp_port = read.udp_port;
    out.local_smoothing = read.local_smoothing;
    out.position.local_smoothing = read.local_smoothing;
    out.remote_smoothing = read.remote_smoothing;
    out.position.remote_smoothing = read.remote_smoothing;
    out.world_space_yaw = read.world_space_yaw;

    // [Position] Enabled chose the startup mode and nothing else: the cycle
    // reached every mode either way.
    const ::cameraunlock::TrackingModeChannels mode = ::cameraunlock::EncodeTrackingMode(
        read.position_enabled ? ::cameraunlock::TrackingMode::RotationAndPosition
                              : ::cameraunlock::TrackingMode::RotationOnly);
    out.rotation_enabled = mode.rotation_enabled;
    out.position_enabled = mode.position_enabled;

    // The build held one vertical limit and used it for up and down alike.
    const Config defaults;
    out.position.limit_x = cfg::LegacyFiniteOrDefault(read.pos_limit_x, defaults.position.limit_x, "Position",
                                                      "LimitX", dropped);
    out.position.limit_y = cfg::LegacyFiniteOrDefault(read.pos_limit_y, defaults.position.limit_y, "Position",
                                                      "LimitY", dropped);
    out.position.limit_y_down = out.position.limit_y;
    out.position.limit_z = cfg::LegacyFiniteOrDefault(read.pos_limit_z, defaults.position.limit_z, "Position",
                                                      "LimitZ", dropped);
    out.position.limit_z_back = cfg::LegacyFiniteOrDefault(read.pos_limit_z_back, defaults.position.limit_z_back,
                                                           "Position", "LimitZBack", dropped);

    // The Ctrl+Shift chords were registered in code beside each named key.
    out.toggle_key_name = Keys(read.toggle_key, "ToggleKey", 'Y', dropped);
    out.cycle_tracking_mode_key_name = Keys(read.position_key, "PositionKey", 'G', dropped);
    out.yaw_mode_key_name = Keys(read.yaw_mode_key, "YawModeKey", 'H', dropped);

    // Pose shaping is the tracker's (approved change pose_shaping). The shipped
    // InvertRoll=true and [Position] InvertX=true corrected the axis conversion
    // and are folded into it: UnrealCamera negates roll and LeanWorldOffset
    // negates sway. A value the player changed is dropped.
    const auto shaping = [&](auto value, auto ship, const char* section, const char* key) {
        cfg::LegacyPoseShaping(value, ship, section, key, pose_shaping, dropped);
    };
    shaping(read.yaw_sens, shipped.yaw_sens, "Tracking", "YawSensitivity");
    shaping(read.pitch_sens, shipped.pitch_sens, "Tracking", "PitchSensitivity");
    shaping(read.roll_sens, shipped.roll_sens, "Tracking", "RollSensitivity");
    shaping(read.invert_yaw, shipped.invert_yaw, "Tracking", "InvertYaw");
    shaping(read.invert_pitch, shipped.invert_pitch, "Tracking", "InvertPitch");
    shaping(read.invert_roll, shipped.invert_roll, "Tracking", "InvertRoll");
    shaping(read.deadzone, shipped.deadzone, "Tracking", "Deadzone");
    shaping(read.pos_sens_x, shipped.pos_sens_x, "Position", "SensitivityX");
    shaping(read.pos_sens_y, shipped.pos_sens_y, "Position", "SensitivityY");
    shaping(read.pos_sens_z, shipped.pos_sens_z, "Position", "SensitivityZ");
    shaping(read.invert_pos_x, shipped.invert_pos_x, "Position", "InvertX");
    shaping(read.invert_pos_y, shipped.invert_pos_y, "Position", "InvertY");
    shaping(read.invert_tracker_z, shipped.invert_tracker_z, "Position", "InvertTrackerZ");

    // [Camera] Mode chose between writing the head into ControlRotation, which
    // steered the diver with the head, and the decoupled UpdateCamera hook. Aim
    // is always decoupled now (approved change coupled_aim).
    if (!legacy::IsUpdateCameraMode(read.camera_mode)) {
        dropped.push_back({cfg::DropRule::CoupledAim, "Camera", "Mode", read.camera_mode});
    }

    out.update_camera_slot = read.update_camera_slot;
    out.pov_offset = read.pov_offset;
    out.cache_offset = read.cache_offset;
    out.dump_vtable = read.dump_vtable;
    out.watch_pov = read.watch_pov;
    out.location_offset = read.location_offset;
    out.log_to_file = read.log_to_file;
    // An empty LogPath wrote HeadTracking.log.
    out.log_path = read.log_path.empty() ? defaults.log_path : read.log_path;

    // A setting still at what the published build shipped is no player's choice,
    // so it follows Defaults.ini. EnableOnStartup was not a setting: the build
    // always started with tracking on.
    cfg::LegacyFollowsDefaultsIni follows;
    follows.Setting(C::UdpPort, read.udp_port, shipped.udp_port);
    follows.NotInLegacy(C::EnableOnStartup);
    follows.Setting(C::WorldSpaceYaw, read.world_space_yaw, shipped.world_space_yaw);
    follows.TrackingMode(read.position_enabled, shipped.position_enabled);
    follows.Setting(C::LocalSmoothing, read.local_smoothing, shipped.local_smoothing);
    follows.Setting(C::RemoteSmoothing, read.remote_smoothing, shipped.remote_smoothing);
    follows.Setting(C::PositionLimitX, read.pos_limit_x, shipped.pos_limit_x);
    follows.Setting(C::PositionLimitY, read.pos_limit_y, shipped.pos_limit_y);
    follows.Setting(C::PositionLimitYDown, read.pos_limit_y, shipped.pos_limit_y);
    follows.Setting(C::PositionLimitZ, read.pos_limit_z, shipped.pos_limit_z);
    follows.Setting(C::PositionLimitZBack, read.pos_limit_z_back, shipped.pos_limit_z_back);
    follows.Setting(C::ToggleKey, legacy::ParseVk(read.toggle_key), legacy::ParseVk(shipped.toggle_key));
    follows.Setting(C::CycleTrackingModeKey, legacy::ParseVk(read.position_key), legacy::ParseVk(shipped.position_key));
    follows.Setting(C::YawModeKey, legacy::ParseVk(read.yaw_mode_key), legacy::ParseVk(shipped.yaw_mode_key));

    return cfg::ImportResult::Imported(std::move(dropped), std::move(pose_shaping), follows.Concepts());
}

std::optional<cfg::ConfigOwner<Config>> g_owner;

void WriteLines(const std::vector<std::string>& lines) {
    for (const std::string& line : lines) UEHT_LOG(Info, "config: %s", line.c_str());
}

}  // namespace

cfg::ConfigTable<Config> MakeTable() {
    cfg::ConfigTable<Config> table = cfg::HeadTrackingConfigTable<Config>(
        {C::UdpPort, C::EnableOnStartup, C::WorldSpaceYaw, C::RotationEnabled, C::LocalSmoothing, C::RemoteSmoothing,
         C::PositionEnabled, C::PositionLimitX, C::PositionLimitY, C::PositionLimitYDown, C::PositionLimitZ,
         C::PositionLimitZBack, C::ToggleKey, C::CycleTrackingModeKey, C::YawModeKey});
    table.Select(C::WorldSpaceYaw).Writable()
        .Select(C::RotationEnabled).Writable()
        .Select(C::PositionEnabled).Writable();
    table.Local("Camera", "UpdateCameraSlot", &Config::update_camera_slot, cfg::IntCodec<int>(),
                "The PlayerCameraManager vtable slot of UpdateCamera, which the mod hooks to move the\n"
                "rendered view. Outside 0 to 255 the hook is not installed and the mod does nothing.")
        .Engine();
    table.Local("Camera", "PovOffset", &Config::pov_offset, cfg::Hex32Codec(),
                "Where in the PlayerCameraManager the rendered camera rotation sits. 0x0 = off.")
        .Engine();
    table.Local("Camera", "CacheOffset", &Config::cache_offset, cfg::Hex32Codec(),
                "A second camera rotation to move with the first. 0x0 = off.")
        .Engine();
    table.Local("Camera", "DumpVtable", &Config::dump_vtable, cfg::BoolCodec(),
                "Diagnostics: true logs the PlayerCameraManager vtable once. Leave off for play.");
    table.Local("Camera", "WatchPov", &Config::watch_pov, cfg::BoolCodec(),
                "Diagnostics: true logs which camera offsets change as you look around. Leave off for play.");
    table.Local("Position", "LocationOffset", &Config::location_offset, cfg::Hex32Codec(),
                "Where in the PlayerCameraManager the rendered camera position sits. 0x0 = no lean.")
        .Engine();
    table.Local("Logging", "LogToFile", &Config::log_to_file, cfg::BoolCodec(),
                "true: write a log file for bug reports.");
    table.Local("Logging", "LogPath", &Config::log_path, cfg::StringCodec(),
                "The log file. A bare file name is next to the game's executable.");
    return table;
}

cfg::LegacyImport<Config> MakeLegacyImport() {
    cfg::LegacyImport<Config> import;
    import.run = &RunImport;
    import.keys = legacy::ReadKeys();
    return import;
}

cfg::ConfigOwnerOptions<Config> MakeOwnerOptions(const std::wstring& exe_dir, cfg::DefaultsFile defaults) {
    cfg::ConfigOwnerOptions<Config> options;
    options.path = exe_dir + L"\\" + kConfigFileName;
    options.table = MakeTable();
    options.import = MakeLegacyImport();
    options.legacy_path = exe_dir + L"\\" + kLegacyFileName;
    options.header.display_name = kDisplayName;
    options.defaults = std::move(defaults);
    return options;
}

Config Load(const std::wstring& exe_dir) {
    g_owner.emplace(MakeOwnerOptions(exe_dir, cfg::DefaultsFile::PerUser()));
    cfg::ConfigLoadResult<Config> loaded = g_owner->Load();
    WriteLines(loaded.log);
    const Config& c = loaded.config;
    UEHT_LOG(Info,
             "config: %s (udpPort=%d, onStartup=%d, mode rotation=%d position=%d, yaw=%s, "
             "smoothing local=%.2f remote=%.2f, limits x=%.2f y=%.2f/%.2f z=%.2f/%.2f, "
             "slot=%d pov=0x%X cache=0x%X location=0x%X)",
             cfg::ConfigLoadStatusName(loaded.status), c.udp_port, c.enable_on_startup ? 1 : 0,
             c.rotation_enabled ? 1 : 0, c.position_enabled ? 1 : 0, c.world_space_yaw ? "world" : "local",
             c.local_smoothing, c.remote_smoothing, c.position.limit_x, c.position.limit_y,
             c.position.limit_y_down, c.position.limit_z, c.position.limit_z_back, c.update_camera_slot,
             c.pov_offset, c.cache_offset, c.location_offset);
    return loaded.config;
}

void Save(const std::function<void(Config&)>& change) {
    const cfg::ConfigSaveResult saved = g_owner->Save(change);
    WriteLines(saved.log);
    if (saved.status != cfg::ConfigSaveStatus::Saved) {
        UEHT_LOG(Warn, "config: %s: %s", cfg::ConfigSaveStatusName(saved.status), saved.reason.c_str());
    }
}

}  // namespace ueht::config
