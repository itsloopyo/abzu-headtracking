// Behaviour lock for the 6DOF lean boundary: which way the camera moves when
// the player leans, and how much of the processor's asymmetric travel budget
// each direction gets.
//
// The bug this guards against shipped once. HeadTracking.ini carried
// InvertZ = true, and PositionProcessor applies inversion BEFORE its
// [-LimitZ, +LimitZBack] clamp, so a forward lean was cut off at the 0.10m
// backward allowance while pulling back got the 0.40m forward one. The
// direction still looked right, so nothing in the render path noticed - the
// camera just refused to lean in.
//
// The whole shipped chain runs here: the real HeadTracking.ini, the real
// Config -> PositionSettings mapping, the real PositionProcessor, and the real
// engine-boundary mapping. A default in Config.hpp that disagrees with the
// shipped INI fails this test, because the INI is what users actually get.

#include <cmath>
#include <cstdio>
#include <string>

#include "ueht/Config.hpp"
#include "mods/LeanOffset.hpp"

#include "cameraunlock/data/position_data.h"
#include "cameraunlock/math/quat4.h"
#include "cameraunlock/processing/position_processor.h"

#ifndef ABZUHT_SHIPPED_INI
#error "ABZUHT_SHIPPED_INI must name the shipped HeadTracking.ini"
#endif

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    if (ok) return;
    std::printf("FAIL: %s\n", what);
    ++g_failures;
}

void CheckNear(float actual, float expected, const char* what) {
    if (std::fabs(actual - expected) <= 1e-3f) return;
    std::printf("FAIL: %s (expected %.4f, got %.4f)\n", what, expected, actual);
    ++g_failures;
}

/// The config a user actually runs with, not the struct defaults.
ueht::Config ShippedConfig() {
    ueht::Config cfg;
    const std::string path = ABZUHT_SHIPPED_INI;
    if (!ueht::Config::LoadFromFile(path, cfg)) {
        std::printf("FAIL: could not parse %s\n", path.c_str());
        ++g_failures;
    }
    // LoadFromFile treats a missing file as "defaults are fine", which would
    // quietly test something other than what ships.
    if (std::FILE* f = std::fopen(path.c_str(), "r")) {
        std::fclose(f);
    } else {
        std::printf("FAIL: shipped INI not found at %s\n", path.c_str());
        ++g_failures;
    }
    return cfg;
}

/// Forward (+X) component of the camera-location delta, in UE world units, for
/// a sustained head offset of `raw_z` meters on the tracker's depth axis.
float ForwardUU(const ueht::Config& cfg, float raw_z) {
    cameraunlock::PositionProcessor processor;
    processor.SetSettings(cfg.AsPositionSettings());
    const cameraunlock::PositionData raw(0.0f, 0.0f, raw_z);
    // Two ticks so the exponential smoothing has settled on the clamped value.
    cameraunlock::math::Vec3 out =
        processor.Process(raw, cameraunlock::math::Quat4::Identity(), 1.0f);
    out = processor.Process(raw, cameraunlock::math::Quat4::Identity(), 1.0f);
    return ueht::LeanWorldOffset(0.0f, out.x, out.y, out.z).x;
}

/// Negative depth is a forward lean throughout cameraunlock-core, and UE's +X
/// is forward.
void LeanDirectionIsNotReversed(const ueht::Config& cfg) {
    Check(ForwardUU(cfg, -0.05f) > 0.0f, "forward lean moves the camera forward (+X)");
    Check(ForwardUU(cfg, 0.05f) < 0.0f, "backward lean moves the camera backward (-X)");
}

/// A metre of lean either way is far past both limits, so what comes out is
/// whichever half of the asymmetric budget that direction actually got.
void LeanBudgetsAreNotSwapped(const ueht::Config& cfg) {
    CheckNear(ForwardUU(cfg, -1.0f), 0.40f * ueht::kMetersToUU,
              "forward lean saturates on the 0.40m budget");
    CheckNear(ForwardUU(cfg, 1.0f), -0.10f * ueht::kMetersToUU,
              "backward lean saturates on the 0.10m budget");
}

/// The lean basis is the camera's yaw with world up, so heave stays vertical
/// and sway stays perpendicular to the facing direction.
void LateralAndVerticalAxesHold() {
    const ueht::LeanOffsetUU ahead = ueht::LeanWorldOffset(0.0f, 0.1f, 0.2f, 0.0f);
    CheckNear(ahead.y, 0.1f * ueht::kMetersToUU, "sway maps to UE right (+Y) at yaw 0");
    CheckNear(ahead.z, 0.2f * ueht::kMetersToUU, "heave maps to UE up (+Z)");

    const ueht::LeanOffsetUU turned = ueht::LeanWorldOffset(90.0f, 0.0f, 0.0f, -0.1f);
    CheckNear(turned.y, 0.1f * ueht::kMetersToUU, "forward lean follows a 90 degree yaw onto +Y");
}

}  // namespace

int main() {
    const ueht::Config cfg = ShippedConfig();

    LeanDirectionIsNotReversed(cfg);
    LeanBudgetsAreNotSwapped(cfg);
    LateralAndVerticalAxesHold();

    if (g_failures != 0) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all lean-budget checks passed\n");
    return 0;
}
