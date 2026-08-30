#pragma once

#include <cmath>

namespace ueht {

/// Camera-location delta in UE world units (cm), in UE axes: +X forward,
/// +Y right, +Z up.
struct LeanOffsetUU {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

/// OpenTrack -> UE world unit scale. Position arrives processed to meters; UE
/// world space is centimeters.
constexpr float kMetersToUU = 100.0f;

/// Maps a processed head offset (meters, pipeline axes) into a world-space
/// camera-location delta through the camera's clean horizon-locked yaw, so
/// leaning follows body orientation rather than the head-rotated view
/// (CameraUnlock 6DOF doctrine). A yaw-only basis plus world up keeps the lean
/// roll-independent.
///
/// `pipeline_z` runs NEGATIVE for a forward lean, which is what earns the
/// forward direction the generous half of PositionProcessor's asymmetric
/// [-limit_z, +limit_z_back] clamp. UE's +X is forward, so the flip into engine
/// axes belongs here, downstream of that clamp. Doing it with
/// PositionSettings::invert_z instead runs it upstream and hands a forward lean
/// the 0.10m backward budget.
inline LeanOffsetUU LeanWorldOffset(float clean_yaw_deg, float pipeline_x,
                                    float pipeline_y, float pipeline_z) {
    const float yr = clean_yaw_deg * 0.01745329252f;  // deg -> rad
    const float cy = std::cos(yr), sy = std::sin(yr);
    const float fwd = -pipeline_z * kMetersToUU;
    const float rgt = pipeline_x * kMetersToUU;

    LeanOffsetUU o;
    o.x = cy * fwd - sy * rgt;
    o.y = sy * fwd + cy * rgt;
    o.z = pipeline_y * kMetersToUU;
    return o;
}

}  // namespace ueht
