#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "cameraunlock/config/config_owner.h"
#include "cameraunlock/config/head_tracking_config.h"

namespace ueht {

/// Runtime configuration: CameraUnlock.ini next to the game executable, in
/// cameraunlock-core's canonical format. The owner imports HeadTracking.ini, the
/// file every earlier build read from the same folder, once while
/// CameraUnlock.ini is absent, and never writes it.
struct Config : cameraunlock::HeadTrackingConfig {
    // [Camera] Confirmed for ABZU (AbzuGame-Win64-Shipping.exe, UE 4.12):
    // UpdateCamera is PCM vtable slot 196, and the renderer reads the
    // camera-cache FRotator at PCM+0x404. cache_offset is an optional second
    // FRotator to nudge (0 = off).
    int      update_camera_slot = 196;
    uint32_t pov_offset         = 0x404;
    uint32_t cache_offset       = 0;
    // Diagnostics for the static-then-confirm discovery flow (read-only, safe):
    bool dump_vtable = false;  // on PCM resolve, log the live PCM vtable + slot RVAs
    bool watch_pov   = false;  // per-frame, log which PCM byte offsets change

    // [Position] The PCM-relative FVector the renderer reads
    // (FMinimalViewInfo.Location), immediately before the rotation at
    // pov_offset. 0 = off.
    uint32_t location_offset = 0x3F8;

    // [Logging] A bare file name lands next to the game executable.
    bool log_to_file = true;
    std::string log_path = "HeadTracking.log";
};

namespace config {

constexpr const char* kDisplayName = "ABZU";
constexpr const wchar_t* kConfigFileName = L"CameraUnlock.ini";
constexpr const wchar_t* kLegacyFileName = L"HeadTracking.ini";

cameraunlock::config::ConfigTable<Config> MakeTable();
cameraunlock::config::LegacyImport<Config> MakeLegacyImport();

/// The owner's options for CameraUnlock.ini in `exe_dir`, with HeadTracking.ini
/// beside it as the legacy file. The mod passes the player's own Defaults.ini,
/// a test one at a scratch path.
cameraunlock::config::ConfigOwnerOptions<Config> MakeOwnerOptions(
    const std::wstring& exe_dir, cameraunlock::config::DefaultsFile defaults);

/// Build the process's owner for CameraUnlock.ini in `exe_dir`, load it, and
/// write every line the load returned to the log. Once, from the init thread.
Config Load(const std::wstring& exe_dir);

/// Apply-then-save for a toggle: the caller has applied the new value; this
/// writes it through the owner and logs what happened. A failed save leaves the
/// session running on the new value. Never from a per-frame path.
void Save(const std::function<void(Config&)>& change);

}  // namespace config

}  // namespace ueht
