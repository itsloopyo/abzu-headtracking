#pragma once

#include <atomic>
#include <chrono>
#include <memory>

#include "Mod.hpp"

#include "cameraunlock/data/tracking_pose.h"
#include "cameraunlock/input/hotkey_poller.h"
#include "cameraunlock/processing/pose_interpolator.h"
#include "cameraunlock/processing/position_interpolator.h"
#include "cameraunlock/processing/position_processor.h"
#include "cameraunlock/processing/tracking_processor.h"
#include "cameraunlock/protocol/udp_receiver.h"

namespace ueht {

/// Processed head position offset, in meters, in tracker axes (x = sway/right,
/// y = heave/up, z = surge/forward). `valid` is false until the receiver has
/// delivered a position sample.
struct HeadPosition {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    bool  valid = false;
};

/// Owns the OpenTrack UDP receiver, the processing pipeline, and the hotkey
/// poller. Produces a processed `TrackingPose` consumed by `UnrealCamera`.
class HeadTracking final : public Mod {
public:
    std::string_view Name() const override { return "HeadTracking"; }

    std::optional<std::string> OnInitialize() override;
    void OnFrame() override;
    void OnShutdown() override;

    /// Latest processed pose (yaw/pitch/roll in degrees). Returns Zero() if the
    /// receiver hasn't bound yet or tracking is toggled off.
    cameraunlock::TrackingPose CurrentPose() const;

    /// Latest processed positional offset (meters, tracker axes). `valid` is
    /// false when position tracking is off or no sample has arrived.
    HeadPosition CurrentPosition() const;

    bool Enabled() const { return m_enabled.load(std::memory_order_acquire); }
    void SetEnabled(bool e) { m_enabled.store(e, std::memory_order_release); }
    void Recenter();

    /// Which degrees of freedom are live. PageUp / Ctrl+Shift+G cycles these.
    enum class DofMode {
        SixDof,        // rotation + position
        RotationOnly,  // 3DOF: head rotation only
        PositionOnly,  // 3DOF: head position only
    };
    DofMode GetDofMode() const { return m_dofMode.load(std::memory_order_acquire); }
    bool PositionEnabled() const { return GetDofMode() != DofMode::RotationOnly; }
    bool RotationEnabled() const { return GetDofMode() != DofMode::PositionOnly; }
    /// Cycle 6DOF -> rotation-only -> position-only -> 6DOF.
    void CycleDofMode();

    /// true = horizon-locked (world up) yaw; false = camera-local yaw.
    /// Read by UnrealCamera's hook each frame to pick the rotation-application path.
    bool WorldSpaceYaw() const { return m_worldSpaceYaw.load(std::memory_order_acquire); }
    void ToggleYawMode();

private:
    std::unique_ptr<cameraunlock::UdpReceiver>       m_receiver;
    // Pipeline order (doctrine): receiver -> interpolator -> processor. The
    // interpolator bridges tracker sample rate to frame rate (velocity-continuous
    // extrapolation past the last sample) so the processor's smoothing is not just
    // softening a held-sample staircase at high refresh.
    cameraunlock::PoseInterpolator                   m_poseInterp;
    cameraunlock::PositionInterpolator               m_posInterp;
    cameraunlock::TrackingProcessor                  m_processor;
    cameraunlock::PositionProcessor                  m_posProcessor;
    std::unique_ptr<cameraunlock::input::HotkeyPoller> m_hotkeys;

    // New-sample detection for the interpolators: the receiver stamps each parsed
    // packet; an unchanged stamp across frames means "no new data, keep interpolating".
    int64_t m_lastSampleTs = 0;
    bool    m_wasReceiving = false;

    std::atomic<bool> m_enabled{true};
    std::atomic<bool> m_worldSpaceYaw{true};   // initialized from config at startup
    std::atomic<DofMode> m_dofMode{DofMode::SixDof};  // initialized from config at startup
    std::atomic<bool> m_recenterPending{false};

    // Latest processed pose, published from OnFrame.
    mutable std::atomic<float> m_outYaw  {0.0f};
    mutable std::atomic<float> m_outPitch{0.0f};
    mutable std::atomic<float> m_outRoll {0.0f};
    mutable std::atomic<int64_t> m_outTs {0};

    // Latest processed position offset (meters, tracker axes).
    mutable std::atomic<float> m_outPosX{0.0f};
    mutable std::atomic<float> m_outPosY{0.0f};
    mutable std::atomic<float> m_outPosZ{0.0f};
    mutable std::atomic<bool>  m_outPosValid{false};

    std::chrono::steady_clock::time_point m_lastFrame{};
};

}  // namespace ueht
