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
#include "cameraunlock/tracking/tracking_mode.h"

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
/// poller. `Update` runs the pipeline on the game thread, from the UpdateCamera
/// detour, so the pose is sampled for the frame the engine is simulating and is
/// read back on the same thread that wrote it.
class HeadTracking final : public Mod {
public:
    std::string_view Name() const override { return "HeadTracking"; }

    std::optional<std::string> OnInitialize() override;

    /// Game thread only. Samples the receiver and publishes the processed pose
    /// and position read by CurrentPose / CurrentPosition.
    void Update();

    /// Latest processed pose (yaw/pitch/roll in degrees). Invalid (timestamp 0)
    /// until the first sample; zero while tracking is toggled off; held at the
    /// last value while the tracker is not sending. Game thread only.
    const cameraunlock::TrackingPose& CurrentPose() const { return m_outPose; }

    /// Latest processed positional offset (meters, tracker axes). `valid` is
    /// false when position tracking is off or no sample has arrived. Game thread only.
    const HeadPosition& CurrentPosition() const { return m_outPos; }

    bool Enabled() const { return m_enabled.load(std::memory_order_acquire); }
    void SetEnabled(bool e) { m_enabled.store(e, std::memory_order_release); }

    /// Which degrees of freedom are live. The mode key cycles 6DOF ->
    /// rotation-only -> position-only -> 6DOF and saves the mode.
    cameraunlock::TrackingMode GetTrackingMode() const { return m_mode.load(std::memory_order_acquire); }
    bool PositionEnabled() const { return GetTrackingMode() != cameraunlock::TrackingMode::RotationOnly; }
    bool RotationEnabled() const { return GetTrackingMode() != cameraunlock::TrackingMode::PositionOnly; }
    void CycleTrackingMode();

    /// true = horizon-locked (world up) yaw; false = camera-local yaw.
    bool WorldSpaceYaw() const { return m_worldSpaceYaw.load(std::memory_order_acquire); }
    /// Flips the yaw mode and saves it.
    void ToggleYawMode();

private:
    /// Re-reads the receiver's source-address locality and, on a change, points
    /// both processors at the matching smoothing parameter.
    void SyncConnectionLocality();

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
    bool    m_loggedFirstSample = false;
    // Last locality pushed to the processors; drives LocalSmoothing vs RemoteSmoothing.
    bool    m_isRemoteConnection = false;

    // Written by the hotkey poller thread, read on the game thread.
    std::atomic<bool> m_enabled{true};
    std::atomic<bool> m_worldSpaceYaw{true};
    std::atomic<cameraunlock::TrackingMode> m_mode{cameraunlock::TrackingMode::RotationAndPosition};

    cameraunlock::TrackingPose m_outPose;
    HeadPosition               m_outPos;

    std::chrono::steady_clock::time_point m_lastFrame{};
};

}  // namespace ueht
