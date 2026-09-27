// The config differential test (convert-a-mod-to-the-canonical-config, section 5).
//
// Oracle: the reader of the newest published build, the rolling `dev` pre-release at 10eb679,
// with ParseVk and ParseMode and the core sources it compiled at its pin 3465659
// (oracle_adapter.h).
// Import: the frozen reader in src/legacy_config/.
//
// Comparison 1, oracle against import, on every input: whether a file was read, every field
// both read (floats bit for bit), and the startup state the build derived from them: the key
// each hotkey name bound, whether the camera ran the UpdateCamera path, the tracking mode and
// the yaw mode. It finds one difference, made by 425d354 after the published build: [Position]
// InvertZ is no longer read, and InvertTrackerZ (default false) is read in its place. Both are
// depth inversions, which the conversion drops as pose shaping.
//
// Also asserted: the import leaves its folder as it found it, and a read-only copy imports as a
// writable one does.

#include "legacy_config/legacy_config.h"
#include "oracle_adapter.h"

#include "cameraunlock/config/ini_reader.h"
#include "cameraunlock/config/testing/ini_mutations.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace ueht;

namespace {

constexpr const char* kFileName = "HeadTracking.ini";

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
        fs::create_directories(root_);
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
    try {
        Scratch scratch;

        // The dev build's installer ZIP, its launcher seed and 10eb679's HeadTracking.ini are
        // the same bytes (data/dev-shipped.ini). 4ced432 committed the one earlier version.
        const std::string shipped = ReadBytes(fs::path(ABZU_DIFFERENTIAL_DATA) / "dev-shipped.ini");
        const std::string earlier = ReadBytes(fs::path(ABZU_DIFFERENTIAL_DATA) / "4ced432-shipped.ini");

        const std::vector<Input> inputs = Inputs(shipped, earlier);
        std::printf("comparison 1 (oracle dev 10eb679 against the import) on %zu inputs\n", inputs.size());
        for (const Input& input : inputs) {
            Comparison1(scratch, input);
            scratch.Clear();
        }
    } catch (const std::exception& e) {
        std::printf("  FAIL: threw: %s\n", e.what());
        ++g_failures;
    }

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
