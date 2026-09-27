// CameraUnlock.ini: the committed file against the table that renders it, what
// the owner creates on a first start and on the first start after an update,
// and what a toggle's save changes.
//
// Every owner here reads and creates Defaults.ini at a scratch path
// (DefaultsFile::At), never the developer's own.
//
// `--render-config <path>` writes the table's fresh render to <path> and exits,
// which is how `pixi run render-config` rewrites the committed file.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include <windows.h>

#include "ueht/Config.hpp"
#include "utility/Logging.hpp"

#include "cameraunlock/config/config_table.h"

namespace {

namespace cfg = ::cameraunlock::config;
namespace fs = std::filesystem;
using ueht::Config;

int g_failures = 0;

void Check(bool ok, const std::string& what) {
    if (ok) return;
    std::printf("FAIL: %s\n", what.c_str());
    ++g_failures;
}

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

std::string Fresh() {
    return cfg::RenderCanonicalFresh(ueht::config::MakeTable(), cfg::RenderHeader{ueht::config::kDisplayName});
}

class Scratch {
public:
    Scratch() {
        root_ = fs::temp_directory_path() / ("abzu-canonical-config-" + std::to_string(GetCurrentProcessId()));
        fs::remove_all(root_);
        fs::create_directories(root_ / "global");
    }
    ~Scratch() {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }
    fs::path Fresh(const std::string& leaf) {
        const fs::path dir = root_ / (leaf + std::to_string(next_++));
        fs::create_directories(dir);
        return dir;
    }
    // One Defaults.ini for every owner, created by the first with the built-in values.
    cfg::DefaultsFile Defaults() const { return cfg::DefaultsFile::At((root_ / "global" / "Defaults.ini").wstring()); }
    fs::path DefaultsPath() const { return root_ / "global" / "Defaults.ini"; }

private:
    fs::path root_;
    int next_ = 0;
};

cfg::ConfigLoadResult<Config> LoadIn(const Scratch& scratch, const fs::path& dir) {
    cfg::ConfigOwner<Config> owner(ueht::config::MakeOwnerOptions(dir.wstring(), scratch.Defaults()));
    return owner.Load();
}

std::vector<std::string> Lines(const std::string& s) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\n') {
            lines.push_back(s.substr(start, i + 1 - start));
            start = i + 1;
        }
    }
    if (start < s.size()) lines.push_back(s.substr(start));
    return lines;
}

// The lines of `after` that differ from `before`, which must have as many.
std::vector<std::string> ChangedLines(const std::string& before, const std::string& after) {
    const std::vector<std::string> a = Lines(before);
    const std::vector<std::string> b = Lines(after);
    if (a.size() != b.size()) return {"the line count changed"};
    std::vector<std::string> changed;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) changed.push_back(b[i]);
    }
    return changed;
}

void TestTheCommittedFileIsTheFreshRender() {
    Check(ReadBytes(ABZU_COMMITTED_CONFIG) == Fresh(),
          "HeadTracking.ini differs from the table's fresh render; run pixi run render-config");
}

// A first start with no HeadTracking.ini creates the committed file and a
// Defaults.ini holding core's built-in values.
void TestAFirstStartCreatesTheCommittedFile(Scratch& scratch) {
    const fs::path dir = scratch.Fresh("created");
    const cfg::ConfigLoadResult<Config> loaded = LoadIn(scratch, dir);
    Check(loaded.status == cfg::ConfigLoadStatus::Created, "a first start creates CameraUnlock.ini");
    Check(ReadBytes(dir / "CameraUnlock.ini") == ReadBytes(ABZU_COMMITTED_CONFIG),
          "a first start writes the committed file");
    Check(!fs::exists(dir / "HeadTracking.ini"), "a first start writes no HeadTracking.ini");
    Check(ReadBytes(scratch.DefaultsPath()) == ReadBytes(ABZU_DEFAULTS_FIXTURE),
          "the created Defaults.ini is core's fixture");

    const Config& c = loaded.config;
    Check(c.udp_port == 4242, "UdpPort");
    Check(c.enable_on_startup, "EnableOnStartup");
    Check(c.rotation_enabled && c.position_enabled, "the tracking mode is 6DOF");
    Check(c.world_space_yaw, "WorldSpaceYaw");
    Check(c.local_smoothing == 0.0f && c.remote_smoothing == 0.15f, "smoothing");
    Check(c.position.limit_x == 0.30f && c.position.limit_y == 0.20f && c.position.limit_y_down == 0.20f &&
              c.position.limit_z == 0.40f && c.position.limit_z_back == 0.10f,
          "position limits");
    Check(c.update_camera_slot == 196 && c.pov_offset == 0x404 && c.cache_offset == 0 && c.location_offset == 0x3F8,
          "the camera offsets are the confirmed ABZU ones");
    Check(!c.dump_vtable && !c.watch_pov, "diagnostics are off");
    Check(c.log_to_file && c.log_path == "HeadTracking.log", "logging");
    Check(c.toggle_key_name == "End, Ctrl+Shift+Y", "ToggleKey");
    Check(c.cycle_tracking_mode_key_name == "PageUp, Ctrl+Shift+G", "CycleTrackingModeKey");
    Check(c.yaw_mode_key_name == "PageDown, Ctrl+Shift+H", "YawModeKey");
}

// The newest published build shipped one HeadTracking.ini, in its installer ZIP
// and its launcher seed alike. Imported with Defaults.ini at the built-in
// values it gives the bytes a new player gets but one line: it named its log
// AbzuHeadTracking.log, and the player keeps that name. HeadTracking.ini is
// left as it was.
void TestTheFirstUpdateGivesTheCommittedFile(Scratch& scratch) {
    const std::string shipped = ReadBytes(fs::path(ABZU_DIFFERENTIAL_DATA) / "dev-shipped.ini");
    const fs::path dir = scratch.Fresh("upgrade");
    WriteBytes(dir / "HeadTracking.ini", shipped);
    const cfg::ConfigLoadResult<Config> loaded = LoadIn(scratch, dir);
    Check(loaded.status == cfg::ConfigLoadStatus::Migrated, "the shipped file migrates");
    const std::vector<std::string> changed =
        ChangedLines(ReadBytes(ABZU_COMMITTED_CONFIG), ReadBytes(dir / "CameraUnlock.ini"));
    Check(changed.size() == 1 && changed[0] == "LogPath=AbzuHeadTracking.log\r\n",
          "the published file imports into the committed file but its log name");
    Check(ReadBytes(dir / "HeadTracking.ini") == shipped, "HeadTracking.ini is left as it was");

    const cfg::ConfigLoadResult<Config> again = LoadIn(scratch, dir);
    Check(again.status == cfg::ConfigLoadStatus::Canonical, "the second start reads CameraUnlock.ini");
    bool saysLegacyIsNotRead = false;
    for (const std::string& line : again.log) {
        if (line.find("is left as it was and is not read") != std::string::npos) saysLegacyIsNotRead = true;
    }
    Check(saysLegacyIsNotRead, "the second start says HeadTracking.ini is not read");
}

// The yaw toggle's save writes WorldSpaceYaw and no other byte; the mode
// cycle's writes the pair and no other byte; the next start reads both back.
void TestASaveChangesOnlyItsRows(Scratch& scratch) {
    const fs::path dir = scratch.Fresh("save");
    const fs::path file = dir / "CameraUnlock.ini";
    {
        cfg::ConfigOwner<Config> owner(ueht::config::MakeOwnerOptions(dir.wstring(), scratch.Defaults()));
        Check(owner.Load().status == cfg::ConfigLoadStatus::Created, "the save test starts from a created file");
        const std::string created = ReadBytes(file);

        const cfg::ConfigSaveResult yaw = owner.Save([](Config& c) { c.world_space_yaw = false; });
        Check(yaw.status == cfg::ConfigSaveStatus::Saved, "the yaw save is saved");
        const std::vector<std::string> yawLines = ChangedLines(created, ReadBytes(file));
        Check(yawLines.size() == 1 && yawLines[0] == "WorldSpaceYaw=false\r\n", "the yaw save changes one line");
        bool named = false;
        for (const std::string& line : yaw.log) {
            if (line.find("WorldSpaceYaw=false is now set for this game") != std::string::npos) named = true;
        }
        Check(named, "the save says WorldSpaceYaw no longer follows Defaults.ini");

        const std::string afterYaw = ReadBytes(file);
        const cfg::ConfigSaveResult mode = owner.Save([](Config& c) {
            c.rotation_enabled = true;
            c.position_enabled = false;
        });
        Check(mode.status == cfg::ConfigSaveStatus::Saved, "the mode save is saved");
        const std::vector<std::string> modeLines = ChangedLines(afterYaw, ReadBytes(file));
        Check(modeLines.size() == 2 && modeLines[0] == "RotationEnabled=true\r\n" &&
                  modeLines[1] == "PositionEnabled=false\r\n",
              "the mode save changes the pair and nothing else");
    }
    const cfg::ConfigLoadResult<Config> again = LoadIn(scratch, dir);
    Check(again.status == cfg::ConfigLoadStatus::Canonical, "the next start reads the saved file");
    Check(!again.config.world_space_yaw, "the yaw mode came back");
    Check(again.config.rotation_enabled && !again.config.position_enabled, "the tracking mode came back");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--render-config") == 0) {
        WriteBytes(argv[2], Fresh());
        std::printf("wrote %s\n", argv[2]);
        return 0;
    }
    ueht::log::Init({});
    try {
        Scratch scratch;
        TestTheCommittedFileIsTheFreshRender();
        TestAFirstStartCreatesTheCommittedFile(scratch);
        TestTheFirstUpdateGivesTheCommittedFile(scratch);
        TestASaveChangesOnlyItsRows(scratch);
    } catch (const std::exception& e) {
        std::printf("FAIL threw: %s\n", e.what());
        return 1;
    }
    if (g_failures != 0) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all canonical config checks passed\n");
    return 0;
}
