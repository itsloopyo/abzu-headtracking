#include "HeadTracking.hpp"

#include "Framework.hpp"
#include "utility/Logging.hpp"

#include "cameraunlock/math/smoothing_utils.h"

#include "cameraunlock/input/key_binding_registration.h"
#include "cameraunlock/input/key_bindings.h"

#include <functional>
#include <stdexcept>
#include <string>
#include <utility>

namespace ueht {

namespace {
/// Puts one hotkey list from CameraUnlock.ini on the poller.
void Register(cameraunlock::input::HotkeyPoller& poller, const std::string& list, std::function<void()> action) {
    const auto parsed = cameraunlock::input::ParseKeyBindings(list);
    if (!parsed.ok()) throw std::logic_error("hotkey list '" + list + "' does not parse: " + parsed.error);
    cameraunlock::input::RegisterKeyBindings(poller, parsed.bindings, std::move(action));
}
}  // namespace

std::optional<std::string> HeadTracking::OnInitialize() {
    const auto& cfg = Framework::Get().Cfg();

    m_enabled.store(cfg.enable_on_startup, std::memory_order_release);
    m_worldSpaceYaw.store(cfg.world_space_yaw, std::memory_order_release);
    m_mode.store(cameraunlock::DecodeTrackingMode(cfg.rotation_enabled, cfg.position_enabled).value(),
                 std::memory_order_release);

    // The processor's sensitivity and deadzone stay at identity: the tracker
    // shapes the pose. Position carries the same two smoothing values as
    // rotation; the processor's connection flag picks which one applies.
    m_processor.SetLocalSmoothing(cfg.local_smoothing);
    m_processor.SetRemoteSmoothing(cfg.remote_smoothing);
    m_posProcessor.SetSettings(cfg.position);

    m_receiver = std::make_unique<cameraunlock::UdpReceiver>();
    m_receiver->SetLog([](const std::string& m){ UEHT_LOG(Info, "[udp] %s", m.c_str()); });
    if (!m_receiver->Start(static_cast<uint16_t>(cfg.udp_port))) {
        // Non-fatal - UdpReceiver schedules its own retry loop when the port
        // is held. We log and continue; pose simply stays zero until it binds.
        UEHT_LOG(Warn, "OpenTrack UDP %d not bound yet; receiver will retry.", cfg.udp_port);
    } else {
        UEHT_LOG(Info, "Listening for OpenTrack on UDP %d", cfg.udp_port);
    }

    m_hotkeys = std::make_unique<cameraunlock::input::HotkeyPoller>();
    Register(*m_hotkeys, cfg.toggle_key_name, [this]{ SetEnabled(!Enabled()); });
    Register(*m_hotkeys, cfg.cycle_tracking_mode_key_name, [this]{ CycleTrackingMode(); });
    Register(*m_hotkeys, cfg.yaw_mode_key_name, [this]{ ToggleYawMode(); });
    m_hotkeys->Start();

    // Seed the locality flag so the first frame already uses the right value.
    m_isRemoteConnection = !m_receiver->IsRemoteConnection();
    SyncConnectionLocality();

    m_lastFrame = std::chrono::steady_clock::now();
    return std::nullopt;
}

void HeadTracking::Update() {
    const auto now = std::chrono::steady_clock::now();
    const float dt = std::chrono::duration<float>(now - m_lastFrame).count();
    m_lastFrame = now;

    if (!Enabled()) {
        m_outPose.yaw = m_outPose.pitch = m_outPose.roll = 0.0f;
        m_outPos.valid = false;
        m_wasReceiving = false;
        return;
    }

    // Tracking loss holds the last published pose and position rather than
    // snapping the view to centre; the processors' smoothing then blends from
    // that held pose once packets resume.
    if (!m_receiver->IsReceiving()) {
        m_wasReceiving = false;
        return;
    }

    // A tracker swap (local OpenTrack <-> phone on WiFi) must pick up the other
    // smoothing parameter without a restart, so the flag is re-read every frame.
    SyncConnectionLocality();

    float yaw{}, pitch{}, roll{};
    if (!m_receiver->GetRotation(yaw, pitch, roll)) return;

    if (!m_loggedFirstSample) {
        UEHT_LOG(Info, "HeadTracking: first OpenTrack sample yaw=%.2f pitch=%.2f roll=%.2f", yaw, pitch, roll);
        m_loggedFirstSample = true;
    }

    // New-sample edge: the receiver's packet timestamp changes only when fresh
    // data arrives. Held frames between packets report the same stamp, which is
    // exactly what lets the interpolators bridge the gap instead of flat-spotting.
    const int64_t sampleTs = m_receiver->GetLastReceiveTimestamp();
    const bool isNew = (sampleTs != m_lastSampleTs);
    m_lastSampleTs = sampleTs;

    // A resume after loss or a re-enable drops the interpolators' history, so
    // they do not extrapolate from the segment before the gap.
    if (!m_wasReceiving) {
        m_poseInterp.Reset();
        m_posInterp.Reset();
        m_wasReceiving = true;
    }

    // Receiver -> interpolator -> processor.
    const auto interp = m_poseInterp.Update(yaw, pitch, roll, isNew, dt);
    const auto processed = m_processor.Process(interp.yaw, interp.pitch, interp.roll, dt);

    // In position-only mode the head must not rotate the view, so publish a zero
    // delta (the processor still runs to keep its smoothing state warm for the
    // next mode switch). A zero delta is a no-op in both the world-yaw add and
    // the camera-local compose paths.
    const bool rotOn = RotationEnabled();
    m_outPose.yaw          = rotOn ? processed.yaw   : 0.0f;
    m_outPose.pitch        = rotOn ? processed.pitch : 0.0f;
    m_outPose.roll         = rotOn ? processed.roll  : 0.0f;
    m_outPose.timestamp_us = processed.timestamp_us;

    float px{}, py{}, pz{};
    if (!PositionEnabled() || !m_receiver->GetPosition(px, py, pz)) {
        m_outPos.valid = false;
        return;
    }

    // Tag with the receiver stamp so the position interpolator shares the same
    // new-sample detection as the pose interpolator.
    const cameraunlock::PositionData raw(px, py, pz, sampleTs);
    const cameraunlock::PositionData interpPos = m_posInterp.Update(raw, dt);

    // Tracker-pivot compensation wants the physical head rotation, which is the
    // processor's smoothed pose rather than the mode-gated one published above.
    float physYaw{}, physPitch{}, physRoll{};
    m_processor.GetSmoothedRotation(physYaw, physPitch, physRoll);
    const auto rotQ = cameraunlock::math::Quat4::FromYawPitchRoll(physYaw, physPitch, physRoll);
    const cameraunlock::math::Vec3 offset = m_posProcessor.Process(interpPos, rotQ, dt);

    m_outPos.x = offset.x;
    m_outPos.y = offset.y;
    m_outPos.z = offset.z;
    m_outPos.valid = true;
}

void HeadTracking::SyncConnectionLocality() {
    const bool isRemote = m_receiver->IsRemoteConnection();
    if (isRemote == m_isRemoteConnection) return;
    m_isRemoteConnection = isRemote;

    m_processor.SetIsRemoteConnection(isRemote);
    m_posProcessor.SetIsRemoteConnection(isRemote);

    const auto& cfg = Framework::Get().Cfg();
    const double effective = cameraunlock::math::GetEffectiveSmoothing(
        cfg.local_smoothing, cfg.remote_smoothing, isRemote);
    UEHT_LOG(Info, "Tracker connection is %s; smoothing=%.2f",
             isRemote ? "remote" : "local", effective);
}

void HeadTracking::CycleTrackingMode() {
    using cameraunlock::TrackingMode;
    TrackingMode next;
    const char* label;
    switch (GetTrackingMode()) {
        case TrackingMode::RotationAndPosition: next = TrackingMode::RotationOnly;        label = "3DOF rotation only"; break;
        case TrackingMode::RotationOnly:        next = TrackingMode::PositionOnly;        label = "3DOF position only"; break;
        default:                                next = TrackingMode::RotationAndPosition; label = "6DOF (rotation + position)"; break;
    }
    m_mode.store(next, std::memory_order_release);
    UEHT_LOG(Info, "Tracking mode: %s", label);
    const cameraunlock::TrackingModeChannels channels = cameraunlock::EncodeTrackingMode(next);
    config::Save([channels](Config& c) {
        c.rotation_enabled = channels.rotation_enabled;
        c.position_enabled = channels.position_enabled;
    });
}

void HeadTracking::ToggleYawMode() {
    const bool world = !m_worldSpaceYaw.load(std::memory_order_acquire);
    m_worldSpaceYaw.store(world, std::memory_order_release);
    UEHT_LOG(Info, "Yaw mode: %s", world ? "world-space (horizon-locked)" : "camera-local");
    config::Save([world](Config& c) { c.world_space_yaw = world; });
}

}  // namespace ueht
