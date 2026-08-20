#include "ueht/Config.hpp"

#include <windows.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>

#include "cameraunlock/config/ini_reader.h"
#include "utility/Logging.hpp"

namespace ueht {
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

bool Config::LoadFromFile(const std::string& path, Config& out) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (path.empty() || !fs::exists(path, ec)) return true;  // defaults are fine

    cameraunlock::IniReader ini;
    if (!ini.Open(path)) return false;

    // Section/key names match the shipped HeadTracking.ini exactly. The reader
    // is case-insensitive (Win32 GetPrivateProfile*), but we mirror the file's
    // CamelCase so the mapping is obvious.
    out.udp_port     = static_cast<uint16_t>(ini.ReadInt("Network", "UdpPort", out.udp_port));

    out.yaw_sens     = ini.ReadFloat ("Tracking", "YawSensitivity",   out.yaw_sens);
    out.pitch_sens   = ini.ReadFloat ("Tracking", "PitchSensitivity", out.pitch_sens);
    out.roll_sens    = ini.ReadFloat ("Tracking", "RollSensitivity",  out.roll_sens);
    out.invert_yaw   = ini.ReadBool  ("Tracking", "InvertYaw",        out.invert_yaw);
    out.invert_pitch = ini.ReadBool  ("Tracking", "InvertPitch",      out.invert_pitch);
    out.invert_roll  = ini.ReadBool  ("Tracking", "InvertRoll",       out.invert_roll);
    // Each key falls back to its own default (local 0.0, remote 0.15), never to
    // a shared one: a bad RemoteSmoothing dropping to the local default would
    // leave a phone's network jitter entirely unsmoothed.
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
    // Offsets are entered as hex (e.g. 0xBD4); strtoul base 0 honours the prefix.
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
    out.invert_pos_z     = ini.ReadBool ("Position", "InvertZ",      out.invert_pos_z);
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
