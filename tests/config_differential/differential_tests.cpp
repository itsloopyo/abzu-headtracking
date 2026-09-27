// The config differential test (convert-a-mod-to-the-canonical-config, section 5).
//
// Oracle: the reader of the newest published build, the rolling `dev` pre-release at 10eb679,
// with ParseVk and ParseMode and the core sources it compiled at its pin 3465659
// (oracle_adapter.h).
// Import: the frozen reader in src/legacy_config/.
// Migration: the conversion, run by the config owner in a folder holding only a copy of the
// input as HeadTracking.ini, which imports it into a new CameraUnlock.ini, then the canonical
// reader and table on that file.
//
// Comparison 1, oracle against import, on every input: whether a file was read, every field
// both read (floats bit for bit), and the startup state the build derived from them: the key
// each hotkey name bound, whether the camera ran the UpdateCamera path, the tracking mode and
// the yaw mode. It finds one difference, made by 425d354 after the published build: [Position]
// InvertZ is no longer read, and InvertTrackerZ (default false) is read in its place. Both are
// depth inversions, which the conversion drops as pose shaping.
//
// Comparison 2, import against migration, on every input: every setting that remains one, the
// startup state (tracking on, the mode, the yaw mode) and the bindings of each hotkey, where the
// only differences allowed are core's approved ones, each asserted as a recorded drop: a
// sensitivity, deadzone or inversion away from what the build shipped (pose_shaping), a [Camera]
// Mode that ran the coupled ControlRotation path (coupled_aim), a non-finite position limit
// (N2), and a hotkey name that bound 0xFF (N1) or a Ctrl, Shift or Alt key (N3). The shipped
// InvertRoll=true and [Position] InvertX=true are folded into the axis code
// (tests/lean_budget_tests.cpp holds the sway fold to the processor's inversion).
//
// A setting the player never changed from what the published build shipped follows Defaults.ini
// (owner rule of 2026-09-26): the import lists exactly those rows in follows_defaults_ini, and a
// third migration of every input, over a Defaults.ini that differs from the built-in value on
// every global row the table binds, writes each of them `default` and runs on Defaults.ini's
// value, while every row the player changed runs on the value the first migration carried.
//
// The hotkeys are compared as bindings: the key each name bound, then the Ctrl+Shift chord the
// build registered beside it. Which held modifiers let a binding fire is core's rule now
// (RegisterKeyBindings): a bare key does not fire while Ctrl and Shift are both held, where the
// published build's poller fired it anyway.
//
// A file holding a value the published build ran on that no canonical row can hold is not
// converted: UdpPort 0, which that build read from any UdpPort that is not a number, and a
// negative or oversized position limit. The owner defers, as core's docs/canonical-config.md
// says, and the test asserts that it does, writes nothing and runs the session on the import.
//
// Also asserted after every load: HeadTracking.ini keeps its bytes, last write time and
// attributes, the folder holds it and CameraUnlock.ini and nothing else, a read-only copy
// migrates as a writable one does, the migrated file draws no diagnostic, and a second load
// over the same Defaults.ini reads CameraUnlock.ini, gives the same settings and changes
// neither file. The import leaves its folder as it found it.

#include "ueht/Config.hpp"
#include "legacy_config/legacy_config.h"
#include "oracle_adapter.h"
#include "utility/Logging.hpp"

#include "cameraunlock/config/canonical_ini.h"
#include "cameraunlock/config/config_owner.h"
#include "cameraunlock/config/ini_editor.h"
#include "cameraunlock/config/ini_reader.h"
#include "cameraunlock/config/testing/ini_mutations.h"
#include "cameraunlock/input/key_bindings.h"
#include "cameraunlock/tracking/tracking_mode.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace ueht;

namespace {

constexpr const char* kFileName = "HeadTracking.ini";
constexpr const char* kConfigName = "CameraUnlock.ini";

int g_failures = 0;
int g_checks = 0;

void Check(bool cond, const std::string& what) {
    ++g_checks;
    if (!cond) {
        if (g_failures < 200) std::printf("  FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

bool SameBits(float a, float b) { return std::memcmp(&a, &b, sizeof a) == 0; }

std::string ReadBytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("could not read " + path.string());
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void WriteBytes(const fs::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) throw std::runtime_error("could not write " + path.string());
}

// Every file in a folder with its bytes, its last write time and its attributes.
struct Entry {
    std::string name;
    std::string bytes;
    FILETIME written;
    DWORD attributes;
    bool operator==(const Entry& o) const {
        return name == o.name && bytes == o.bytes && CompareFileTime(&written, &o.written) == 0 &&
               attributes == o.attributes;
    }
    bool operator<(const Entry& o) const { return name < o.name; }
};

std::vector<Entry> List(const fs::path& dir) {
    std::vector<Entry> entries;
    for (const auto& e : fs::directory_iterator(dir)) {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExW(e.path().c_str(), GetFileExInfoStandard, &data)) {
            throw std::runtime_error("no attributes for " + e.path().string());
        }
        entries.push_back({e.path().filename().string(), ReadBytes(e.path()), data.ftLastWriteTime,
                           data.dwFileAttributes});
    }
    std::sort(entries.begin(), entries.end());
    return entries;
}

void SetReadOnly(const fs::path& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) throw std::runtime_error("no attributes for " + path.string());
    if (!SetFileAttributesW(path.c_str(), attrs | FILE_ATTRIBUTE_READONLY)) {
        throw std::runtime_error("could not set attributes on " + path.string());
    }
}

struct Input {
    std::string name;
    std::optional<std::string> bytes;  // nullopt: no file
};

std::string Join(const std::vector<std::string>& v) {
    std::string s;
    for (const std::string& x : v) s += (s.empty() ? "" : ", ") + x;
    return s;
}

// Every field both builds read, and the startup state each derived from them. [Position]
// InvertZ (oracle) and InvertTrackerZ (import) are the one pair compared apart, below.
std::vector<std::string> FieldDifferences(const abzu_oracle_view::OracleConfig& o, const legacy::Config& i) {
    std::vector<std::string> d;
    auto b = [&d](const char* n, bool x, bool y) { if (x != y) d.push_back(n); };
    auto f = [&d](const char* n, float x, float y) { if (!SameBits(x, y)) d.push_back(n); };
    auto n = [&d](const char* name, long long x, long long y) { if (x != y) d.push_back(name); };
    auto s = [&d](const char* name, const std::string& x, const std::string& y) { if (x != y) d.push_back(name); };
    n("udp_port", o.udp_port, i.udp_port);
    f("yaw_sens", o.yaw_sens, i.yaw_sens);
    f("pitch_sens", o.pitch_sens, i.pitch_sens);
    f("roll_sens", o.roll_sens, i.roll_sens);
    b("invert_yaw", o.invert_yaw, i.invert_yaw);
    b("invert_pitch", o.invert_pitch, i.invert_pitch);
    b("invert_roll", o.invert_roll, i.invert_roll);
    f("local_smoothing", o.local_smoothing, i.local_smoothing);
    f("remote_smoothing", o.remote_smoothing, i.remote_smoothing);
    f("deadzone", o.deadzone, i.deadzone);
    s("toggle_key", o.toggle_key, i.toggle_key);
    s("yaw_mode_key", o.yaw_mode_key, i.yaw_mode_key);
    s("position_key", o.position_key, i.position_key);
    s("camera_mode", o.camera_mode, i.camera_mode);
    b("world_space_yaw", o.world_space_yaw, i.world_space_yaw);
    b("dump_vtable", o.dump_vtable, i.dump_vtable);
    b("watch_pov", o.watch_pov, i.watch_pov);
    n("update_camera_slot", o.update_camera_slot, i.update_camera_slot);
    n("pov_offset", o.pov_offset, i.pov_offset);
    n("cache_offset", o.cache_offset, i.cache_offset);
    b("position_enabled", o.position_enabled, i.position_enabled);
    f("pos_sens_x", o.pos_sens_x, i.pos_sens_x);
    f("pos_sens_y", o.pos_sens_y, i.pos_sens_y);
    f("pos_sens_z", o.pos_sens_z, i.pos_sens_z);
    b("invert_pos_x", o.invert_pos_x, i.invert_pos_x);
    b("invert_pos_y", o.invert_pos_y, i.invert_pos_y);
    f("pos_limit_x", o.pos_limit_x, i.pos_limit_x);
    f("pos_limit_y", o.pos_limit_y, i.pos_limit_y);
    f("pos_limit_z", o.pos_limit_z, i.pos_limit_z);
    f("pos_limit_z_back", o.pos_limit_z_back, i.pos_limit_z_back);
    n("location_offset", o.location_offset, i.location_offset);
    b("log_to_file", o.log_to_file, i.log_to_file);
    s("log_path", o.log_path, i.log_path);

    n("toggle key bound", abzu_oracle_view::OracleParseVk(o.toggle_key), legacy::ParseVk(i.toggle_key));
    n("yaw mode key bound", abzu_oracle_view::OracleParseVk(o.yaw_mode_key), legacy::ParseVk(i.yaw_mode_key));
    n("position key bound", abzu_oracle_view::OracleParseVk(o.position_key), legacy::ParseVk(i.position_key));
    b("UpdateCamera path", abzu_oracle_view::OracleIsUpdateCamera(o.camera_mode),
      legacy::IsUpdateCameraMode(i.camera_mode));
    return d;
}

// The corpus descriptor of every key the frozen reader reads.
std::vector<cameraunlock::config::testing::MutationKey> MutationKeys() {
    using cameraunlock::config::testing::MutationKey;
    auto plain = [](const char* s, const char* k, const char* alt) {
        MutationKey m;
        m.section = s;
        m.key = k;
        m.alternate = alt;
        return m;
    };
    auto smoothing = [&plain](const char* s, const char* k) {
        MutationKey m = plain(s, k, "0.3");
        m.out_of_range = {"-0.5", "1.5"};
        return m;
    };
    auto hotkey = [&plain](const char* k, const char* alt) {
        MutationKey m = plain("Hotkeys", k, alt);
        m.hotkey = true;
        return m;
    };
    return {
        plain("Network", "UdpPort", "4243"),
        plain("Tracking", "YawSensitivity", "1.5"),
        plain("Tracking", "PitchSensitivity", "1.5"),
        plain("Tracking", "RollSensitivity", "0.5"),
        plain("Tracking", "InvertYaw", "true"),
        plain("Tracking", "InvertPitch", "true"),
        plain("Tracking", "InvertRoll", "false"),
        smoothing("Tracking", "LocalSmoothing"),
        smoothing("Tracking", "RemoteSmoothing"),
        plain("Tracking", "Smoothing", "0.5"),
        plain("Position", "Smoothing", "0.5"),
        plain("Tracking", "Deadzone", "1.0"),
        hotkey("ToggleKey", "F7"),
        hotkey("YawModeKey", "F8"),
        hotkey("PositionKey", "F9"),
        plain("Camera", "Mode", "controlrotation"),
        plain("Camera", "WorldSpaceYaw", "false"),
        plain("Camera", "DumpVtable", "true"),
        plain("Camera", "WatchPov", "true"),
        plain("Camera", "UpdateCameraSlot", "197"),
        plain("Camera", "PovOffset", "0x408"),
        plain("Camera", "CacheOffset", "0x410"),
        plain("Position", "Enabled", "false"),
        plain("Position", "SensitivityX", "1.5"),
        plain("Position", "SensitivityY", "1.5"),
        plain("Position", "SensitivityZ", "1.5"),
        plain("Position", "InvertX", "false"),
        plain("Position", "InvertY", "true"),
        plain("Position", "InvertTrackerZ", "true"),
        plain("Position", "LimitX", "0.25"),
        plain("Position", "LimitY", "0.15"),
        plain("Position", "LimitZ", "0.35"),
        plain("Position", "LimitZBack", "0.05"),
        plain("Position", "LocationOffset", "0x3FC"),
        plain("Logging", "LogToFile", "false"),
        plain("Logging", "LogPath", "Other.log"),
    };
}

class Scratch {
public:
    Scratch() {
        root_ = fs::temp_directory_path() / ("abzu-config-differential-" + std::to_string(GetCurrentProcessId()));
        fs::remove_all(root_);
        fs::create_directories(root_ / "global");
    }
    ~Scratch() {
        std::error_code ec;
        for (const auto& e : fs::recursive_directory_iterator(root_, ec)) {
            if (e.is_regular_file()) SetFileAttributesW(e.path().c_str(), FILE_ATTRIBUTE_NORMAL);
        }
        fs::remove_all(root_, ec);
    }
    // Every folder, once an input is done with them, so the run holds a few folders on disk at a
    // time rather than thousands.
    void Clear() {
        for (const auto& dir : fs::directory_iterator(root_)) {
            if (dir.path().filename() == "global" || dir.path().filename() == "skewed") continue;
            for (const auto& e : fs::recursive_directory_iterator(dir.path())) {
                if (e.is_regular_file()) SetFileAttributesW(e.path().c_str(), FILE_ATTRIBUTE_NORMAL);
            }
            fs::remove_all(dir.path());
        }
    }
    fs::path Fresh(const std::string& leaf) {
        const fs::path dir = root_ / (leaf + std::to_string(next_++));
        fs::create_directories(dir);
        return dir;
    }
    // One Defaults.ini for every owner, created by the first at the built-in values.
    fs::path DefaultsPath() const { return root_ / "global" / "Defaults.ini"; }
    // A Defaults.ini that differs from the built-in values on every row that follows it.
    fs::path SkewedPath() const { return root_ / "skewed" / "Defaults.ini"; }

private:
    fs::path root_;
    int next_ = 0;
};

fs::path Place(const fs::path& dir, const Input& input) {
    const fs::path file = dir / kFileName;
    if (input.bytes) WriteBytes(file, *input.bytes);
    return file;
}

struct ImportRun {
    legacy::Config config;
    legacy::ReadStatus status = legacy::ReadStatus::Read;
};

abzu_oracle_view::OracleConfig View(const legacy::Config& c) {
    abzu_oracle_view::OracleConfig o{};
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
    o.invert_pos_z = c.invert_tracker_z;
    o.pos_limit_x = c.pos_limit_x;
    o.pos_limit_y = c.pos_limit_y;
    o.pos_limit_z = c.pos_limit_z;
    o.pos_limit_z_back = c.pos_limit_z_back;
    o.location_offset = c.location_offset;
    o.log_to_file = c.log_to_file;
    o.log_path = c.log_path;
    return o;
}

bool SameImport(const ImportRun& a, const ImportRun& b) {
    return a.status == b.status && FieldDifferences(View(a.config), b.config).empty() &&
           a.config.invert_tracker_z == b.config.invert_tracker_z;
}

// The import on one copy of the input, which it must leave as it found it.
ImportRun RunImport(Scratch& scratch, const Input& input, bool readOnly) {
    const fs::path dir = scratch.Fresh(readOnly ? "import-ro" : "import");
    const fs::path file = Place(dir, input);
    if (input.bytes && readOnly) SetReadOnly(file);
    const std::vector<Entry> before = List(dir);
    ImportRun run;
    run.status = legacy::Read(file.string(), run.config);
    Check(List(dir) == before, input.name + ": the import changed its folder");
    return run;
}

ImportRun Comparison1(Scratch& scratch, const Input& input) {
    const fs::path odir = scratch.Fresh("oracle");
    const fs::path ofile = Place(odir, input);
    const abzu_oracle_view::OracleConfig oracle = abzu_oracle_view::RunOracle(ofile.string());
    const ImportRun import = RunImport(scratch, input, false);
    const ImportRun readOnly = RunImport(scratch, input, true);
    Check(SameImport(import, readOnly), input.name + ": a read-only copy imports differently");

    Check((import.status == legacy::ReadStatus::Absent) == !input.bytes.has_value(),
          input.name + ": the import's status");
    const std::vector<std::string> fields = FieldDifferences(oracle, import.config);
    Check(fields.empty(), input.name + ": fields differ: " + Join(fields));

    // 425d354: the published build read the depth inversion from [Position] InvertZ, and the
    // frozen reader reads it from InvertTrackerZ, each through IniReader::ReadBool with a default
    // of false. That key swap is the whole of the difference.
    bool expectedInvertZ = false;
    bool expectedInvertTrackerZ = false;
    cameraunlock::IniReader ini;
    if (input.bytes && ini.Open(ofile.string())) {
        expectedInvertZ = ini.ReadBool("Position", "InvertZ", false);
        expectedInvertTrackerZ = ini.ReadBool("Position", "InvertTrackerZ", false);
    }
    Check(oracle.invert_pos_z == expectedInvertZ, input.name + ": the oracle's InvertZ");
    Check(import.config.invert_tracker_z == expectedInvertTrackerZ, input.name + ": the import's InvertTrackerZ");
    return import;
}

using cameraunlock::config::ConfigLoadStatus;
using cameraunlock::config::DropRule;
using cameraunlock::config::DroppedValue;
using cameraunlock::config::ImportResult;
using cameraunlock::config::ImportStatus;
using cameraunlock::input::KeyBinding;
using cameraunlock::input::KeyModifiers;
using Concept = cameraunlock::config::schema::Concept;

cameraunlock::config::ConfigLoadResult<Config> LoadOwner(const fs::path& defaults, const fs::path& dir) {
    cameraunlock::config::ConfigOwner<Config> owner(
        config::MakeOwnerOptions(dir.wstring(), cameraunlock::config::DefaultsFile::At(defaults.wstring())));
    return owner.Load();
}

// The import with its map, on its own copy, for the values it drops.
ImportResult RunMappedImport(Scratch& scratch, const Input& input) {
    const fs::path file = Place(scratch.Fresh("mapped"), input);
    cameraunlock::config::LegacyInput legacyInput;
    legacyInput.path = file.wstring();
    legacyInput.ansi_path = file.string();
    Config out;
    return config::MakeLegacyImport().run(legacyInput, out);
}

// What the published build shipped where it differs from the frozen code defaults, restated
// here from data/dev-shipped.ini rather than taken from the map under test.
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

bool IsModifierKey(int vk) { return (vk >= 0x10 && vk <= 0x12) || (vk >= 0xA0 && vk <= 0xA5); }

// The bindings the published build registered for one action: the key its name bound, unless
// N1 or N3 unbinds it, then the Ctrl+Shift chord.
std::vector<KeyBinding> ExpectedBindings(const std::string& name, int chord) {
    std::vector<KeyBinding> b;
    const int vk = legacy::ParseVk(name);
    if (vk >= 0x01 && vk <= 0xFE && !IsModifierKey(vk)) b.push_back({KeyModifiers::kNone, vk});
    b.push_back({KeyModifiers::kCtrl | KeyModifiers::kShift, chord});
    return b;
}

bool SameBindings(const std::string& list, const std::vector<KeyBinding>& expected) {
    const cameraunlock::input::KeyBindingsParseResult parsed = cameraunlock::input::ParseKeyBindings(list);
    if (!parsed.ok() || parsed.bindings.size() != expected.size()) return false;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (parsed.bindings[i].vk != expected[i].vk || parsed.bindings[i].modifiers != expected[i].modifiers) {
            return false;
        }
    }
    return true;
}

float FiniteOr(float value, float fallback) { return std::isfinite(value) ? value : fallback; }

// Every setting the migration carries, against the import's, and the startup state.
std::vector<std::string> MigrationDifferences(const legacy::Config& l, const Config& m) {
    const Config d;
    std::vector<std::string> out;
    auto x = [&out](const char* n, bool same) { if (!same) out.push_back(n); };
    x("UdpPort", m.udp_port == l.udp_port);
    x("LocalSmoothing", SameBits(m.local_smoothing, l.local_smoothing) &&
                            SameBits(m.position.local_smoothing, l.local_smoothing));
    x("RemoteSmoothing", SameBits(m.remote_smoothing, l.remote_smoothing) &&
                             SameBits(m.position.remote_smoothing, l.remote_smoothing));
    x("WorldSpaceYaw", m.world_space_yaw == l.world_space_yaw);
    const auto mode = cameraunlock::DecodeTrackingMode(m.rotation_enabled, m.position_enabled);
    x("tracking mode", mode && *mode == (l.position_enabled ? cameraunlock::TrackingMode::RotationAndPosition
                                                             : cameraunlock::TrackingMode::RotationOnly));
    // The published build always started with tracking on.
    x("EnableOnStartup", m.enable_on_startup);
    x("PositionLimitX", SameBits(m.position.limit_x, FiniteOr(l.pos_limit_x, d.position.limit_x)));
    x("PositionLimitY", SameBits(m.position.limit_y, FiniteOr(l.pos_limit_y, d.position.limit_y)));
    x("PositionLimitYDown", SameBits(m.position.limit_y_down, FiniteOr(l.pos_limit_y, d.position.limit_y)));
    x("PositionLimitZ", SameBits(m.position.limit_z, FiniteOr(l.pos_limit_z, d.position.limit_z)));
    x("PositionLimitZBack",
      SameBits(m.position.limit_z_back, FiniteOr(l.pos_limit_z_back, d.position.limit_z_back)));
    x("processor shaping", m.position.sensitivity_x == 1.0f && m.position.sensitivity_y == 1.0f &&
                               m.position.sensitivity_z == 1.0f && !m.position.invert_x && !m.position.invert_y &&
                               !m.position.invert_z);
    x("UpdateCameraSlot", m.update_camera_slot == l.update_camera_slot);
    x("PovOffset", m.pov_offset == l.pov_offset);
    x("CacheOffset", m.cache_offset == l.cache_offset);
    x("LocationOffset", m.location_offset == l.location_offset);
    x("DumpVtable", m.dump_vtable == l.dump_vtable);
    x("WatchPov", m.watch_pov == l.watch_pov);
    x("LogToFile", m.log_to_file == l.log_to_file);
    x("LogPath", m.log_path == (l.log_path.empty() ? std::string("HeadTracking.log") : l.log_path));
    x("ToggleKey", SameBindings(m.toggle_key_name, ExpectedBindings(l.toggle_key, 'Y')));
    x("CycleTrackingModeKey", SameBindings(m.cycle_tracking_mode_key_name, ExpectedBindings(l.position_key, 'G')));
    x("YawModeKey", SameBindings(m.yaw_mode_key_name, ExpectedBindings(l.yaw_mode_key, 'H')));
    return out;
}

// Every setting the session runs on that differs between two loads.
std::vector<std::string> SettingsDifferences(const Config& a, const Config& b) {
    std::vector<std::string> d;
    auto x = [&d](const char* n, bool same) { if (!same) d.push_back(n); };
    x("UdpPort", a.udp_port == b.udp_port);
    x("EnableOnStartup", a.enable_on_startup == b.enable_on_startup);
    x("WorldSpaceYaw", a.world_space_yaw == b.world_space_yaw);
    x("RotationEnabled", a.rotation_enabled == b.rotation_enabled);
    x("PositionEnabled", a.position_enabled == b.position_enabled);
    x("LocalSmoothing", SameBits(a.local_smoothing, b.local_smoothing));
    x("RemoteSmoothing", SameBits(a.remote_smoothing, b.remote_smoothing));
    x("PositionLimitX", SameBits(a.position.limit_x, b.position.limit_x));
    x("PositionLimitY", SameBits(a.position.limit_y, b.position.limit_y));
    x("PositionLimitYDown", SameBits(a.position.limit_y_down, b.position.limit_y_down));
    x("PositionLimitZ", SameBits(a.position.limit_z, b.position.limit_z));
    x("PositionLimitZBack", SameBits(a.position.limit_z_back, b.position.limit_z_back));
    x("ToggleKey", a.toggle_key_name == b.toggle_key_name);
    x("CycleTrackingModeKey", a.cycle_tracking_mode_key_name == b.cycle_tracking_mode_key_name);
    x("YawModeKey", a.yaw_mode_key_name == b.yaw_mode_key_name);
    x("UpdateCameraSlot", a.update_camera_slot == b.update_camera_slot);
    x("PovOffset", a.pov_offset == b.pov_offset);
    x("CacheOffset", a.cache_offset == b.cache_offset);
    x("LocationOffset", a.location_offset == b.location_offset);
    x("DumpVtable", a.dump_vtable == b.dump_vtable);
    x("WatchPov", a.watch_pov == b.watch_pov);
    x("LogToFile", a.log_to_file == b.log_to_file);
    x("LogPath", a.log_path == b.log_path);
    return d;
}

// ASCII but for the value of LogPath, the one text row: StringCodec keeps a value as its bytes,
// so a log path the published build read in the ANSI code page is written back as it was.
bool AsciiButLogPath(const std::string& bytes) {
    std::size_t start = 0;
    while (start < bytes.size()) {
        std::size_t end = bytes.find('\n', start);
        if (end == std::string::npos) end = bytes.size();
        const std::string line = bytes.substr(start, end - start);
        if (line.rfind("LogPath=", 0) != 0) {
            for (const char c : line) {
                if (static_cast<unsigned char>(c) > 0x7F) return false;
            }
        }
        start = end + 1;
    }
    return true;
}

bool CrlfOnly(const std::string& bytes) {
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (bytes[i] == '\n' && (i == 0 || bytes[i - 1] != '\r')) return false;
        if (bytes[i] == '\r' && (i + 1 == bytes.size() || bytes[i + 1] != '\n')) return false;
    }
    return !bytes.empty() && bytes.back() == '\n';
}

struct Migrated {
    Config config;
    std::string bytes;
};

// The migration on one copy of the input over the Defaults.ini at `defaults`. Returns what it ran
// on and the file it wrote.
std::optional<Migrated> Migrate(Scratch& scratch, const Input& input, bool readOnly, const fs::path& defaults) {
    const fs::path dir = scratch.Fresh(readOnly ? "migration-ro" : "migration");
    const fs::path file = Place(dir, input);
    if (input.bytes && readOnly) SetReadOnly(file);
    const std::vector<Entry> before = List(dir);
    const std::string defaultsBefore = fs::exists(defaults) ? ReadBytes(defaults) : "";

    const cameraunlock::config::ConfigLoadResult<Config> loaded = LoadOwner(defaults, dir);
    const ConfigLoadStatus expected = input.bytes ? ConfigLoadStatus::Migrated : ConfigLoadStatus::Created;
    Check(loaded.status == expected, input.name + ": not " + cameraunlock::config::ConfigLoadStatusName(expected) +
                                         " but " + cameraunlock::config::ConfigLoadStatusName(loaded.status) +
                                         ": " + loaded.reason);
    if (loaded.status != expected) return std::nullopt;

    // HeadTracking.ini as it was, and CameraUnlock.ini beside it, and nothing else.
    std::vector<Entry> after = List(dir);
    const auto created = std::find_if(after.begin(), after.end(), [](const Entry& e) { return e.name == kConfigName; });
    Check(created != after.end(), input.name + ": no CameraUnlock.ini");
    if (created == after.end()) return std::nullopt;
    const std::string bytes = created->bytes;
    after.erase(created);
    Check(after == before, input.name + ": HeadTracking.ini or its folder changed");

    // What the owner wrote reads back as it is, with nothing to report.
    const cameraunlock::config::CanonicalIni doc = cameraunlock::config::ParseCanonicalIni(bytes);
    Check(doc.IsReadable() && cameraunlock::config::HasCanonicalStamp(bytes) && doc.diagnostics.empty(),
          input.name + ": the migrated file draws reader diagnostics");
    Check(AsciiButLogPath(bytes) && CrlfOnly(bytes), input.name + ": the migrated file is not ASCII with CRLF endings");
    Check(loaded.diagnostics.empty(), input.name + ": the migrated file draws table diagnostics");

    // The next start reads CameraUnlock.ini, runs on the same settings and changes nothing.
    const std::vector<Entry> settled = List(dir);
    const cameraunlock::config::ConfigLoadResult<Config> again = LoadOwner(defaults, dir);
    Check(again.status == ConfigLoadStatus::Canonical, input.name + ": the second start did not read CameraUnlock.ini");
    Check(SettingsDifferences(again.config, loaded.config).empty(),
          input.name + ": the second start runs on other settings: " +
              Join(SettingsDifferences(again.config, loaded.config)));
    Check(List(dir) == settled, input.name + ": the second start changed a file");
    if (!defaultsBefore.empty()) {
        Check(ReadBytes(defaults) == defaultsBefore, input.name + ": Defaults.ini changed");
    }
    return Migrated{loaded.config, bytes};
}

// Every row the table binds that follows Defaults.ini: its section and key, its value in the
// skewed Defaults.ini, and whether two loads agree on it.
struct FollowingRow {
    Concept concept;
    const char* section;
    const char* key;
    const char* skewed;
    std::function<bool(const Config&, const Config&)> same;
};

const std::vector<FollowingRow>& FollowingRows() {
    static const std::vector<FollowingRow> rows = {
        {Concept::UdpPort, "Network", "UdpPort", "5555",
         [](const Config& a, const Config& b) { return a.udp_port == b.udp_port; }},
        {Concept::EnableOnStartup, "General", "EnableOnStartup", "false",
         [](const Config& a, const Config& b) { return a.enable_on_startup == b.enable_on_startup; }},
        {Concept::WorldSpaceYaw, "General", "WorldSpaceYaw", "false",
         [](const Config& a, const Config& b) { return a.world_space_yaw == b.world_space_yaw; }},
        {Concept::RotationEnabled, "General", "RotationEnabled", "false",
         [](const Config& a, const Config& b) { return a.rotation_enabled == b.rotation_enabled; }},
        {Concept::LocalSmoothing, "Smoothing", "LocalSmoothing", "0.25",
         [](const Config& a, const Config& b) {
             return SameBits(a.local_smoothing, b.local_smoothing) &&
                    SameBits(a.position.local_smoothing, b.position.local_smoothing);
         }},
        {Concept::RemoteSmoothing, "Smoothing", "RemoteSmoothing", "0.45",
         [](const Config& a, const Config& b) {
             return SameBits(a.remote_smoothing, b.remote_smoothing) &&
                    SameBits(a.position.remote_smoothing, b.position.remote_smoothing);
         }},
        {Concept::PositionEnabled, "Position", "PositionEnabled", "true",
         [](const Config& a, const Config& b) { return a.position_enabled == b.position_enabled; }},
        {Concept::PositionLimitX, "Position", "PositionLimitX", "0.25",
         [](const Config& a, const Config& b) { return SameBits(a.position.limit_x, b.position.limit_x); }},
        {Concept::PositionLimitY, "Position", "PositionLimitY", "0.15",
         [](const Config& a, const Config& b) { return SameBits(a.position.limit_y, b.position.limit_y); }},
        {Concept::PositionLimitYDown, "Position", "PositionLimitYDown", "0.12",
         [](const Config& a, const Config& b) { return SameBits(a.position.limit_y_down, b.position.limit_y_down); }},
        {Concept::PositionLimitZ, "Position", "PositionLimitZ", "0.35",
         [](const Config& a, const Config& b) { return SameBits(a.position.limit_z, b.position.limit_z); }},
        {Concept::PositionLimitZBack, "Position", "PositionLimitZBack", "0.05",
         [](const Config& a, const Config& b) { return SameBits(a.position.limit_z_back, b.position.limit_z_back); }},
        {Concept::ToggleKey, "Hotkeys", "ToggleKey", "F7",
         [](const Config& a, const Config& b) { return a.toggle_key_name == b.toggle_key_name; }},
        {Concept::CycleTrackingModeKey, "Hotkeys", "CycleTrackingModeKey", "F8",
         [](const Config& a, const Config& b) {
             return a.cycle_tracking_mode_key_name == b.cycle_tracking_mode_key_name;
         }},
        {Concept::YawModeKey, "Hotkeys", "YawModeKey", "F9",
         [](const Config& a, const Config& b) { return a.yaw_mode_key_name == b.yaw_mode_key_name; }},
    };
    return rows;
}

// The rows the player never changed, worked out here from what the published build shipped:
// every setting HeadTracking.ini held at its shipped value, and every one it did not hold.
std::set<Concept> Untouched(const legacy::Config& l) {
    const legacy::Config shipped = Shipped();
    std::set<Concept> u = {Concept::EnableOnStartup};
    if (l.udp_port == shipped.udp_port) u.insert(Concept::UdpPort);
    if (l.world_space_yaw == shipped.world_space_yaw) u.insert(Concept::WorldSpaceYaw);
    if (l.position_enabled == shipped.position_enabled) {
        u.insert(Concept::RotationEnabled);
        u.insert(Concept::PositionEnabled);
    }
    if (SameBits(l.local_smoothing, shipped.local_smoothing)) u.insert(Concept::LocalSmoothing);
    if (SameBits(l.remote_smoothing, shipped.remote_smoothing)) u.insert(Concept::RemoteSmoothing);
    if (l.pos_limit_x == shipped.pos_limit_x) u.insert(Concept::PositionLimitX);
    if (l.pos_limit_y == shipped.pos_limit_y) {
        u.insert(Concept::PositionLimitY);
        u.insert(Concept::PositionLimitYDown);
    }
    if (l.pos_limit_z == shipped.pos_limit_z) u.insert(Concept::PositionLimitZ);
    if (l.pos_limit_z_back == shipped.pos_limit_z_back) u.insert(Concept::PositionLimitZBack);
    if (legacy::ParseVk(l.toggle_key) == legacy::ParseVk(shipped.toggle_key)) u.insert(Concept::ToggleKey);
    if (legacy::ParseVk(l.position_key) == legacy::ParseVk(shipped.position_key)) {
        u.insert(Concept::CycleTrackingModeKey);
    }
    if (legacy::ParseVk(l.yaw_mode_key) == legacy::ParseVk(shipped.yaw_mode_key)) u.insert(Concept::YawModeKey);
    return u;
}

std::string Names(const std::set<Concept>& concepts) {
    std::vector<std::string> names;
    for (const Concept c : concepts) {
        names.push_back(cameraunlock::config::schema::kConcepts[static_cast<std::size_t>(c)].name);
    }
    return Join(names);
}

// The drops the map must record for an imported file, as (rule, section, key).
using DropKey = std::tuple<DropRule, std::string, std::string>;

std::set<DropKey> ExpectedDrops(const legacy::Config& l) {
    const legacy::Config s = Shipped();
    std::set<DropKey> d;
    auto shaping = [&d](bool changed, const char* section, const char* key) {
        if (changed) d.insert({DropRule::PoseShaping, section, key});
    };
    auto differs = [](float a, float b) { return !(a == b); };
    shaping(differs(l.yaw_sens, s.yaw_sens), "Tracking", "YawSensitivity");
    shaping(differs(l.pitch_sens, s.pitch_sens), "Tracking", "PitchSensitivity");
    shaping(differs(l.roll_sens, s.roll_sens), "Tracking", "RollSensitivity");
    shaping(l.invert_yaw != s.invert_yaw, "Tracking", "InvertYaw");
    shaping(l.invert_pitch != s.invert_pitch, "Tracking", "InvertPitch");
    shaping(l.invert_roll != s.invert_roll, "Tracking", "InvertRoll");
    shaping(differs(l.deadzone, s.deadzone), "Tracking", "Deadzone");
    shaping(differs(l.pos_sens_x, s.pos_sens_x), "Position", "SensitivityX");
    shaping(differs(l.pos_sens_y, s.pos_sens_y), "Position", "SensitivityY");
    shaping(differs(l.pos_sens_z, s.pos_sens_z), "Position", "SensitivityZ");
    shaping(l.invert_pos_x != s.invert_pos_x, "Position", "InvertX");
    shaping(l.invert_pos_y != s.invert_pos_y, "Position", "InvertY");
    shaping(l.invert_tracker_z != s.invert_tracker_z, "Position", "InvertTrackerZ");
    if (!legacy::IsUpdateCameraMode(l.camera_mode)) d.insert({DropRule::CoupledAim, "Camera", "Mode"});
    auto finite = [&d](float v, const char* key) {
        if (!std::isfinite(v)) d.insert({DropRule::NonFiniteNumber, "Position", key});
    };
    finite(l.pos_limit_x, "LimitX");
    finite(l.pos_limit_y, "LimitY");
    finite(l.pos_limit_z, "LimitZ");
    finite(l.pos_limit_z_back, "LimitZBack");
    auto key = [&d](const std::string& name, const char* k) {
        const int vk = legacy::ParseVk(name);
        if (vk == 0xFF) d.insert({DropRule::KeyCodeOutOfRange, "Hotkeys", k});
        if (IsModifierKey(vk)) d.insert({DropRule::ModifierKey, "Hotkeys", k});
    };
    key(l.toggle_key, "ToggleKey");
    key(l.position_key, "PositionKey");
    key(l.yaw_mode_key, "YawModeKey");
    return d;
}

// The skewed Defaults.ini: the built-in one the first owner created, with every row that follows
// it set to a value that differs from the built-in one and from what the published build shipped.
void WriteSkewedDefaults(const Scratch& scratch) {
    std::vector<cameraunlock::IniEdit> edits;
    for (const FollowingRow& row : FollowingRows()) edits.push_back({row.section, row.key, row.skewed});
    const cameraunlock::IniEditResult edited = cameraunlock::EditIni(ReadBytes(scratch.DefaultsPath()), edits);
    if (edited.refusal != cameraunlock::IniEditRefusal::None) {
        throw std::runtime_error(std::string("could not skew Defaults.ini: ") +
                                 cameraunlock::IniEditRefusalName(edited.refusal) + " " + edited.section + " " +
                                 edited.key);
    }
    fs::create_directories(scratch.SkewedPath().parent_path());
    WriteBytes(scratch.SkewedPath(), edited.bytes);
}

// A value the published build ran on that no canonical row can hold: UdpPort 0, which that build
// read from any UdpPort that is not a number, and a finite position limit outside 0 to 10.
bool OutsideARange(const legacy::Config& l) {
    auto outside = [](float v) { return std::isfinite(v) && (v < 0.0f || v > 10.0f); };
    return l.udp_port == 0 || outside(l.pos_limit_x) || outside(l.pos_limit_y) || outside(l.pos_limit_z) ||
           outside(l.pos_limit_z_back);
}

// Such a file is not converted: the owner defers, runs the session on what the import gave,
// writes nothing, and tries again at the next launch.
void CheckDeferred(Scratch& scratch, const Input& input, const legacy::Config& l) {
    for (const bool readOnly : {false, true}) {
        const fs::path dir = scratch.Fresh(readOnly ? "deferred-ro" : "deferred");
        const fs::path file = Place(dir, input);
        if (readOnly) SetReadOnly(file);
        const std::vector<Entry> before = List(dir);
        const std::string defaultsBefore = ReadBytes(scratch.DefaultsPath());
        const cameraunlock::config::ConfigLoadResult<Config> loaded = LoadOwner(scratch.DefaultsPath(), dir);
        Check(loaded.status == ConfigLoadStatus::Deferred,
              input.name + ": a value outside a range is not deferred but " +
                  cameraunlock::config::ConfigLoadStatusName(loaded.status));
        Check(List(dir) == before, input.name + ": a deferred import changed its folder");
        Check(ReadBytes(scratch.DefaultsPath()) == defaultsBefore, input.name + ": Defaults.ini changed");
        const std::vector<std::string> d = MigrationDifferences(l, loaded.config);
        Check(d.empty(), input.name + ": the deferred session differs from the import: " + Join(d));
    }
}

void Comparison2(Scratch& scratch, const Input& input, const ImportRun& import, const Config& skewed) {
    if (input.bytes && OutsideARange(import.config)) {
        CheckDeferred(scratch, input, import.config);
        return;
    }
    const std::optional<Migrated> migrated = Migrate(scratch, input, false, scratch.DefaultsPath());
    const std::optional<Migrated> readOnly = Migrate(scratch, input, true, scratch.DefaultsPath());
    if (!migrated || !readOnly) return;
    Check(SettingsDifferences(migrated->config, readOnly->config).empty(),
          input.name + ": a read-only copy migrates differently: " +
              Join(SettingsDifferences(migrated->config, readOnly->config)));

    // No file: the owner creates the fresh file and imports nothing, so the moved defaults apply
    // (tests/canonical_config_tests.cpp holds that file to the committed one).
    if (!input.bytes) return;

    const legacy::Config& l = import.config;
    const Config& m = migrated->config;
    const std::vector<std::string> d = MigrationDifferences(l, m);
    Check(d.empty(), input.name + ": migration differs from the import: " + Join(d));

    const ImportResult imported = RunMappedImport(scratch, input);
    Check(imported.status == ImportStatus::Imported, input.name + ": the mapped import's status");
    std::set<DropKey> drops;
    for (const DroppedValue& drop : imported.dropped) drops.insert({drop.rule, drop.section, drop.key});
    Check(drops.size() == imported.dropped.size(), input.name + ": a value is dropped twice");
    if (drops != ExpectedDrops(l)) {
        std::vector<std::string> got;
        for (const DroppedValue& drop : imported.dropped) {
            got.push_back(cameraunlock::config::DescribeDroppedValue(drop));
        }
        Check(false, input.name + ": the drops are not the approved ones: " + Join(got));
    }

    const std::set<Concept> follows(imported.follows_defaults_ini.begin(), imported.follows_defaults_ini.end());
    Check(follows.size() == imported.follows_defaults_ini.size(), input.name + ": follows_defaults_ini repeats a row");
    const std::set<Concept> untouched = Untouched(l);
    Check(follows == untouched, input.name + ": the rows left to Defaults.ini are " + Names(follows) +
                                    ", not the untouched " + Names(untouched));

    const std::optional<Migrated> over = Migrate(scratch, input, false, scratch.SkewedPath());
    if (!over) return;
    const cameraunlock::config::CanonicalIni doc = cameraunlock::config::ParseCanonicalIni(over->bytes);
    for (const FollowingRow& row : FollowingRows()) {
        const std::string what = input.name + ": over the skewed Defaults.ini, " + row.section + " " + row.key;
        if (untouched.count(row.concept) != 0) {
            const cameraunlock::config::CanonicalValue* written = doc.Find(row.section, row.key);
            Check(written != nullptr && written->value == "default", what + " is not written default");
            Check(row.same(over->config, skewed), what + " does not take Defaults.ini's value");
        } else {
            Check(row.same(over->config, m), what + " does not keep the value the player changed");
        }
    }
}

std::vector<Input> Inputs(const std::string& shipped, const std::string& earlier) {
    using cameraunlock::config::testing::GenerateIniMutations;
    std::vector<Input> inputs;
    inputs.push_back({"no file", std::nullopt});
    inputs.push_back({"empty file", std::string()});
    inputs.push_back({"dev shipped HeadTracking.ini", shipped});
    inputs.push_back({"4ced432 HeadTracking.ini", earlier});
    for (auto& m : GenerateIniMutations(shipped, legacy::ReadKeys(), MutationKeys())) {
        inputs.push_back({"corpus: " + m.name, std::move(m.bytes)});
    }
    return inputs;
}

}  // namespace

int main() {
    ueht::log::Init({});
    try {
        Scratch scratch;

        // The built-in Defaults.ini, created by the first owner, then the skewed copy of it and
        // what a start with no legacy file runs on over it.
        LoadOwner(scratch.DefaultsPath(), scratch.Fresh("created"));
        WriteSkewedDefaults(scratch);
        const std::optional<Migrated> skewed =
            Migrate(scratch, {"no file, skewed Defaults.ini", std::nullopt}, false, scratch.SkewedPath());
        if (!skewed) throw std::runtime_error("no start over the skewed Defaults.ini");
        const std::optional<Migrated> builtIn =
            Migrate(scratch, {"no file, built-in Defaults.ini", std::nullopt}, false, scratch.DefaultsPath());
        if (!builtIn) throw std::runtime_error("no start over the built-in Defaults.ini");
        for (const FollowingRow& row : FollowingRows()) {
            // Position only: the mode differs from rotation and position, though PositionEnabled
            // alone does not.
            if (row.concept == Concept::PositionEnabled) continue;
            Check(!row.same(skewed->config, builtIn->config),
                  std::string("the skewed Defaults.ini holds the built-in ") + row.key);
        }
        scratch.Clear();

        // The dev build's installer ZIP, its launcher seed and 10eb679's HeadTracking.ini are
        // the same bytes (data/dev-shipped.ini). 4ced432 committed the one earlier version.
        const std::string shipped = ReadBytes(fs::path(ABZU_DIFFERENTIAL_DATA) / "dev-shipped.ini");
        const std::string earlier = ReadBytes(fs::path(ABZU_DIFFERENTIAL_DATA) / "4ced432-shipped.ini");

        const std::vector<Input> inputs = Inputs(shipped, earlier);
        std::printf("comparison 1 (oracle dev 10eb679 against the import) on %zu inputs\n", inputs.size());
        std::printf("comparison 2 (the import against the migration)\n");
        for (const Input& input : inputs) {
            Comparison2(scratch, input, Comparison1(scratch, input), skewed->config);
            scratch.Clear();
        }
    } catch (const std::exception& e) {
        std::printf("  FAIL: threw: %s\n", e.what());
        ++g_failures;
    }

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
