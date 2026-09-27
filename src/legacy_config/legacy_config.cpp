#include "legacy_config/legacy_config.h"

#include <windows.h>

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>

#include "cameraunlock/config/ini_reader.h"
#include "utility/Logging.hpp"

namespace ueht::legacy {
namespace {

// Smoothing reaches cameraunlock::math::CalculateSmoothingFactor, which runs
// exp() on it. strtod parses "nan" and "inf" without complaint, and every
// comparison-based clamp downstream is skipped by NaN because each comparison
// against it is false, so "LocalSmoothing=nan" would otherwise poison the
// smoothed pose for the rest of the session with nothing in the log. Reject it
// here instead. Validation only, never a floor: a configured 0.0 stays 0.0.
float SanitizeSmoothing(const char* key, float value, float fallback) {
    if (!std::isfinite(value)) {
        UEHT_LOG(Warn, "config: %s is not a finite number; using %.2f", key, fallback);
        return fallback;
    }
    if (value < 0.0f || value > 1.0f) {
        const float clamped = (value < 0.0f) ? 0.0f : 1.0f;
        UEHT_LOG(Warn, "config: %s=%g is outside [0,1]; clamped to %.2f", key,
                 static_cast<double>(value), clamped);
        return clamped;
    }
    return value;
}

// Warned once per process rather than once per load: config is reloadable, and
// repeating this on every reload buries it.
//
// The old value is deliberately NOT migrated into the new keys. The single
// Smoothing value carried a hidden 0.15 floor, so the number in an existing
// config does not mean what it used to: copying it across would hand a local
// user smoothing they never chose under the new semantics, and copying it into
// only one of the two keys would be a guess about which connection they were on.
void WarnRetiredSmoothingKey(const cameraunlock::IniReader& reader,
                             const char* section, const char* key) {
    static bool warned = false;
    if (warned) return;
    if (reader.ReadString(section, key, "").empty()) return;
    warned = true;
    UEHT_LOG(Warn,
        "Config key [%s] %s has been retired and is IGNORED. Smoothing is now two "
        "keys: LocalSmoothing (default 0, applies to a tracker on this machine) and "
        "RemoteSmoothing (default 0.15, applies to a tracker on the network). The "
        "old value is not migrated because the semantics changed - it carried a "
        "hidden 0.15 floor that no longer exists. Set the two new keys.",
        section, key);
}

}  // namespace

ReadStatus Read(const std::string& path, Config& out) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (path.empty() || !fs::exists(path, ec)) return ReadStatus::Absent;

    cameraunlock::IniReader ini;
    if (!ini.Open(path)) return ReadStatus::Absent;

    out.udp_port     = static_cast<uint16_t>(ini.ReadInt("Network", "UdpPort", out.udp_port));

    out.yaw_sens     = ini.ReadFloat ("Tracking", "YawSensitivity",   out.yaw_sens);
    out.pitch_sens   = ini.ReadFloat ("Tracking", "PitchSensitivity", out.pitch_sens);
    out.roll_sens    = ini.ReadFloat ("Tracking", "RollSensitivity",  out.roll_sens);
    out.invert_yaw   = ini.ReadBool  ("Tracking", "InvertYaw",        out.invert_yaw);
    out.invert_pitch = ini.ReadBool  ("Tracking", "InvertPitch",      out.invert_pitch);
    out.invert_roll  = ini.ReadBool  ("Tracking", "InvertRoll",       out.invert_roll);
    {
        const float local_default  = out.local_smoothing;
        const float remote_default = out.remote_smoothing;
        out.local_smoothing  = SanitizeSmoothing(
            "LocalSmoothing",  ini.ReadFloat("Tracking", "LocalSmoothing",  local_default),
            local_default);
        out.remote_smoothing = SanitizeSmoothing(
            "RemoteSmoothing", ini.ReadFloat("Tracking", "RemoteSmoothing", remote_default),
            remote_default);
    }
    WarnRetiredSmoothingKey(ini, "Tracking", "Smoothing");
    WarnRetiredSmoothingKey(ini, "Position", "Smoothing");
    out.deadzone     = ini.ReadFloat ("Tracking", "Deadzone",         out.deadzone);

    out.toggle_key   = ini.ReadString("Hotkeys", "ToggleKey",   out.toggle_key.c_str());
    out.yaw_mode_key = ini.ReadString("Hotkeys", "YawModeKey",  out.yaw_mode_key.c_str());
    out.position_key = ini.ReadString("Hotkeys", "PositionKey", out.position_key.c_str());

    out.camera_mode        = ini.ReadString("Camera", "Mode",            out.camera_mode.c_str());
    out.world_space_yaw    = ini.ReadBool  ("Camera", "WorldSpaceYaw",   out.world_space_yaw);
    out.dump_vtable        = ini.ReadBool  ("Camera", "DumpVtable",      out.dump_vtable);
    out.watch_pov          = ini.ReadBool  ("Camera", "WatchPov",        out.watch_pov);
    out.update_camera_slot = ini.ReadInt   ("Camera", "UpdateCameraSlot", out.update_camera_slot);
    {
        const std::string pov   = ini.ReadString("Camera", "PovOffset",   "");
        const std::string cache = ini.ReadString("Camera", "CacheOffset", "");
        if (!pov.empty())   out.pov_offset   = static_cast<uint32_t>(std::strtoul(pov.c_str(),   nullptr, 0));
        if (!cache.empty()) out.cache_offset = static_cast<uint32_t>(std::strtoul(cache.c_str(), nullptr, 0));
    }

    out.position_enabled = ini.ReadBool ("Position", "Enabled",      out.position_enabled);
    out.pos_sens_x       = ini.ReadFloat("Position", "SensitivityX", out.pos_sens_x);
    out.pos_sens_y       = ini.ReadFloat("Position", "SensitivityY", out.pos_sens_y);
    out.pos_sens_z       = ini.ReadFloat("Position", "SensitivityZ", out.pos_sens_z);
    out.invert_pos_x     = ini.ReadBool ("Position", "InvertX",      out.invert_pos_x);
    out.invert_pos_y     = ini.ReadBool ("Position", "InvertY",      out.invert_pos_y);
    out.invert_tracker_z = ini.ReadBool ("Position", "InvertTrackerZ", out.invert_tracker_z);
    out.pos_limit_x      = ini.ReadFloat("Position", "LimitX",       out.pos_limit_x);
    out.pos_limit_y      = ini.ReadFloat("Position", "LimitY",       out.pos_limit_y);
    out.pos_limit_z      = ini.ReadFloat("Position", "LimitZ",       out.pos_limit_z);
    out.pos_limit_z_back = ini.ReadFloat("Position", "LimitZBack",   out.pos_limit_z_back);
    {
        const std::string loc = ini.ReadString("Position", "LocationOffset", "");
        if (!loc.empty()) out.location_offset = static_cast<uint32_t>(std::strtoul(loc.c_str(), nullptr, 0));
    }

    out.log_to_file  = ini.ReadBool  ("Logging", "LogToFile", out.log_to_file);
    out.log_path     = ini.ReadString("Logging", "LogPath",   out.log_path.c_str());
    return ReadStatus::Read;
}

std::vector<cameraunlock::config::LegacyKey> ReadKeys() {
    return {
        {"Network", "UdpPort"},
        {"Tracking", "YawSensitivity"},
        {"Tracking", "PitchSensitivity"},
        {"Tracking", "RollSensitivity"},
        {"Tracking", "InvertYaw"},
        {"Tracking", "InvertPitch"},
        {"Tracking", "InvertRoll"},
        {"Tracking", "LocalSmoothing"},
        {"Tracking", "RemoteSmoothing"},
        {"Tracking", "Smoothing"},
        {"Position", "Smoothing"},
        {"Tracking", "Deadzone"},
        {"Hotkeys", "ToggleKey"},
        {"Hotkeys", "YawModeKey"},
        {"Hotkeys", "PositionKey"},
        {"Camera", "Mode"},
        {"Camera", "WorldSpaceYaw"},
        {"Camera", "DumpVtable"},
        {"Camera", "WatchPov"},
        {"Camera", "UpdateCameraSlot"},
        {"Camera", "PovOffset"},
        {"Camera", "CacheOffset"},
        {"Position", "Enabled"},
        {"Position", "SensitivityX"},
        {"Position", "SensitivityY"},
        {"Position", "SensitivityZ"},
        {"Position", "InvertX"},
        {"Position", "InvertY"},
        {"Position", "InvertTrackerZ"},
        {"Position", "LimitX"},
        {"Position", "LimitY"},
        {"Position", "LimitZ"},
        {"Position", "LimitZBack"},
        {"Position", "LocationOffset"},
        {"Logging", "LogToFile"},
        {"Logging", "LogPath"},
    };
}

int ParseVk(const std::string& name) {
    if (name.empty()) return 0;

    std::string n;
    n.reserve(name.size());
    for (char c : name) n.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));

    // Numeric VK literal, e.g. "0x22" or "34" (base 0 honours the 0x prefix).
    if (std::isdigit(static_cast<unsigned char>(n[0]))) {
        int v = static_cast<int>(std::strtol(n.c_str(), nullptr, 0));
        if (v > 0 && v <= 0xFF) return v;
    }
    if (n.size() >= 2 && n[0] == 'F' && std::isdigit(static_cast<unsigned char>(n[1]))) {
        int idx = std::atoi(n.c_str() + 1);
        if (idx >= 1 && idx <= 24) return 0x6F + idx;  // VK_F1 = 0x70
    }
    if (n.size() == 1) {
        char c = n[0];
        if (c >= 'A' && c <= 'Z') return c;
        if (c >= '0' && c <= '9') return c;
    }
    if (n == "HOME")     return 0x24;  // VK::Home
    if (n == "END")      return 0x23;  // VK::End
    if (n == "INSERT")   return 0x2D;  // VK::Insert
    if (n == "DELETE")   return 0x2E;  // VK::Delete
    if (n == "SPACE")    return 0x20;  // VK::Space
    if (n == "PAGEUP")   return 0x21;  // VK_PRIOR
    if (n == "PAGEDOWN") return 0x22;  // VK_NEXT
    if (n == "ESCAPE" || n == "ESC") return 0x1B;  // VK::Escape
    return 0;
}

bool IsUpdateCameraMode(const std::string& s) {
    return s == "updatecamera" || s == "UpdateCamera";
}

}  // namespace ueht::legacy
