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
// The whole chain runs here: the defaults CameraUnlock.ini is created with,
// the real PositionProcessor, and the real engine-boundary mapping.

#include <cmath>
#include <cstdio>
#include <string>

#include "ueht/Config.hpp"
#include "mods/LeanOffset.hpp"

#include "cameraunlock/data/position_data.h"
#include "cameraunlock/math/quat4.h"
#include "cameraunlock/processing/position_processor.h"

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

/// The settings a fresh CameraUnlock.ini runs on.
ueht::Config FreshConfig() { return ueht::config::MakeTable().defaults(); }

/// Forward (+X) component of the camera-location delta, in UE world units, for
/// a sustained head offset of `raw_z` meters on the tracker's depth axis.
float ForwardUU(const ueht::Config& cfg, float raw_z) {
    cameraunlock::PositionProcessor processor;
    processor.SetSettings(cfg.position);
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
    CheckNear(ahead.y, -0.1f * ueht::kMetersToUU, "tracker +x sway maps to UE left (-Y) at yaw 0");
    CheckNear(ahead.z, 0.2f * ueht::kMetersToUU, "heave maps to UE up (+Z)");

    const ueht::LeanOffsetUU turned = ueht::LeanWorldOffset(90.0f, 0.0f, 0.0f, -0.1f);
    CheckNear(turned.y, 0.1f * ueht::kMetersToUU, "forward lean follows a 90 degree yaw onto +Y");
}

/// Every build before CameraUnlock.ini shipped [Position] InvertX=true, which
/// PositionProcessor applied before its clamp, and mapped x to UE +Y as it
/// came. LeanWorldOffset now negates sway itself and the processor inverts
/// nothing, and the camera has to land in the same place.
void SwayMatchesTheShippedInversion(const ueht::Config& cfg) {
    for (const float raw_x : {-1.0f, -0.2f, -0.05f, 0.0f, 0.05f, 0.2f, 1.0f}) {
        cameraunlock::PositionSettings old_settings = cfg.position;
        old_settings.invert_x = true;
        cameraunlock::PositionProcessor old_processor;
        old_processor.SetSettings(old_settings);
        cameraunlock::PositionProcessor processor;
        processor.SetSettings(cfg.position);
        const cameraunlock::PositionData raw(raw_x, 0.0f, 0.0f);
        cameraunlock::math::Vec3 old_out, out;
        for (int i = 0; i < 3; ++i) {
            old_out = old_processor.Process(raw, cameraunlock::math::Quat4::Identity(), 0.016f);
            out = processor.Process(raw, cameraunlock::math::Quat4::Identity(), 0.016f);
        }
        const float old_right = old_out.x * ueht::kMetersToUU;
        const float right = ueht::LeanWorldOffset(0.0f, out.x, out.y, out.z).y;
        Check(old_right == right, "sway lands where the shipped InvertX=true put it");
    }
}

}  // namespace

int main() {
    const ueht::Config cfg = FreshConfig();
    Check(!cfg.position.invert_x && !cfg.position.invert_y && !cfg.position.invert_z,
          "the processor inverts no axis");

    SwayMatchesTheShippedInversion(cfg);

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
