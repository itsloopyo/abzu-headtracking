#include "HeadTracking.hpp"

#include "Framework.hpp"
#include "utility/Logging.hpp"

#include <cctype>
#include <cstring>

#include <windows.h>

namespace {
/// True while either Control and either Shift are held. The chord letter's own
/// key-down edge is tracked by the poller; this only gates the action so a bare
/// letter press never fires it.
bool ChordHeld() {
    constexpr int kDown = 0x8000;
    return (GetAsyncKeyState(VK_CONTROL) & kDown) && (GetAsyncKeyState(VK_SHIFT) & kDown);
}
}  // namespace

namespace ueht {

namespace {
/// Map a friendly key name from ueht.ini to a Win32 VK_ code.
/// Handles "F1".."F24", single A-Z/0-9, and a few common names.
int ParseVk(const std::string& name) {
    using namespace cameraunlock::input;
    if (name.empty()) return 0;

    std::string n;
    n.reserve(name.size());
    for (char c : name) n.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));

    // Numeric VK literal, e.g. "0x22" or "34" (base 0 honours the 0x prefix).
    if (std::isdigit(static_cast<unsigned char>(n[0]))) {
        int v = static_cast<int>(std::strtol(n.c_str(), nullptr, 0));
        if (v > 0 && v <= 0xFF) return v;
    }
    if (n.size() >= 2 && n[0] == 'F' && std::isdigit(static_cast<unsigned char>(n[1]))) {
        int idx = std::atoi(n.c_str() + 1);
        if (idx >= 1 && idx <= 24) return 0x6F + idx;  // VK_F1 = 0x70
    }
    if (n.size() == 1) {
        char c = n[0];
        if (c >= 'A' && c <= 'Z') return c;
        if (c >= '0' && c <= '9') return c;
    }
    if (n == "HOME")     return VK::Home;
    if (n == "END")      return VK::End;
    if (n == "INSERT")   return VK::Insert;
    if (n == "DELETE")   return VK::Delete;
    if (n == "SPACE")    return VK::Space;
    if (n == "PAGEUP")   return 0x21;  // VK_PRIOR
    if (n == "PAGEDOWN") return 0x22;  // VK_NEXT
    if (n == "ESCAPE" || n == "ESC") return VK::Escape;
    return 0;
}
}  // namespace

std::optional<std::string> HeadTracking::OnInitialize() {
    const auto& cfg = Framework::Get().Cfg();

    m_worldSpaceYaw.store(cfg.world_space_yaw, std::memory_order_release);
    m_dofMode.store(cfg.position_enabled ? DofMode::SixDof : DofMode::RotationOnly,
                    std::memory_order_release);

    m_processor.SetSensitivity(cfg.AsSensitivity());
    m_processor.SetDeadzone(cfg.AsDeadzone());
    m_processor.SetSmoothing(cfg.smoothing);

    m_posProcessor.SetSettings(cfg.AsPositionSettings());

    m_receiver = std::make_unique<cameraunlock::UdpReceiver>();
    m_receiver->SetLog([](const std::string& m){ UEHT_LOG(Info, "[udp] %s", m.c_str()); });
    if (!m_receiver->Start(cfg.udp_port)) {
        // Non-fatal — UdpReceiver schedules its own retry loop when the port
        // is held. We log and continue; pose simply stays zero until it binds.
        UEHT_LOG(Warn, "OpenTrack UDP %u not bound yet; receiver will retry.", cfg.udp_port);
    } else {
        UEHT_LOG(Info, "Listening for OpenTrack on UDP %u", cfg.udp_port);
    }

    m_hotkeys = std::make_unique<cameraunlock::input::HotkeyPoller>();
    // Nav-cluster bindings (Home / End) from config.
    if (int vk = ParseVk(cfg.recenter_key); vk != 0) {
        m_hotkeys->SetRecenterKey(vk, [this]{ Recenter(); });
    }
    if (int vk = ParseVk(cfg.toggle_key); vk != 0) {
        m_hotkeys->SetToggleKey(vk, [this]{ SetEnabled(!Enabled()); });
    }
    if (int vk = ParseVk(cfg.yaw_mode_key); vk != 0) {
        m_hotkeys->AddHotkey(vk, [this]{ ToggleYawMode(); });
    }
    if (int vk = ParseVk(cfg.position_key); vk != 0) {
        m_hotkeys->AddHotkey(vk, [this]{ CycleDofMode(); });
    }
    // Chord equivalents (Ctrl+Shift+T recenter, Ctrl+Shift+Y toggle, Ctrl+Shift+G
    // DOF-mode cycle, Ctrl+Shift+H yaw mode) for keyboards without a nav cluster.
    // The poller edge-detects the letter; ChordHeld gates it.
    m_hotkeys->AddHotkey('T', [this]{ if (ChordHeld()) Recenter(); });
    m_hotkeys->AddHotkey('Y', [this]{ if (ChordHeld()) SetEnabled(!Enabled()); });
    m_hotkeys->AddHotkey('G', [this]{ if (ChordHeld()) CycleDofMode(); });
    m_hotkeys->AddHotkey('H', [this]{ if (ChordHeld()) ToggleYawMode(); });
    m_hotkeys->Start();

    m_lastFrame = std::chrono::steady_clock::now();
    return std::nullopt;
}

void HeadTracking::OnFrame() {
    const auto now = std::chrono::steady_clock::now();
    const float dt = std::chrono::duration<float>(now - m_lastFrame).count();
    m_lastFrame = now;

    if (!Enabled() || !m_receiver || !m_receiver->IsReceiving()) {
        m_outYaw.store(0.0f, std::memory_order_relaxed);
        m_outPitch.store(0.0f, std::memory_order_relaxed);
        m_outRoll.store(0.0f, std::memory_order_relaxed);
        m_outPosValid.store(false, std::memory_order_release);
        m_wasReceiving = false;  // resume eases in from a clean interpolator segment
        return;
    }

    float yaw{}, pitch{}, roll{};
    if (!m_receiver->GetRotation(yaw, pitch, roll)) return;

    static bool s_loggedFirst = false;
    if (!s_loggedFirst) {
        UEHT_LOG(Info, "HeadTracking: first OpenTrack sample yaw=%.2f pitch=%.2f roll=%.2f", yaw, pitch, roll);
        s_loggedFirst = true;
    }

    // New-sample edge: the receiver's packet timestamp changes only when fresh
    // data arrives. Held frames between packets report the same stamp, which is
    // exactly what lets the interpolators bridge the gap instead of flat-spotting.
    const int64_t sampleTs = m_receiver->GetLastReceiveTimestamp();
    const bool isNew = (sampleTs != m_lastSampleTs);
    m_lastSampleTs = sampleTs;

    // Recenter (Home / Ctrl+Shift+T) or a fresh resume after data loss: clear the
    // interpolator history and smoothing so we ease in from the live sample rather
    // than from a stale segment. Done on the game thread so nothing races Process.
    const bool recenter = m_recenterPending.exchange(false, std::memory_order_acq_rel);
    const bool resume = !m_wasReceiving;
    m_wasReceiving = true;
    if (recenter || resume) {
        m_poseInterp.Reset();
        m_posInterp.Reset();
        m_posProcessor.ResetSmoothing();
    }

    // Receiver -> interpolator -> processor.
    const auto interp = m_poseInterp.Update(yaw, pitch, roll, isNew, dt);
    const auto processed = m_processor.Process(interp.yaw, interp.pitch, interp.roll, dt);

    // In position-only mode the head must not rotate the view, so publish a zero
    // delta (the processor still runs to keep its smoothing state warm for the
    // next mode switch). pose stays "valid" via the timestamp; a zero delta is a
    // no-op in both the world-yaw add and the camera-local compose paths.
    const bool rotOn = RotationEnabled();
    m_outYaw  .store(rotOn ? processed.yaw   : 0.0f, std::memory_order_relaxed);
    m_outPitch.store(rotOn ? processed.pitch : 0.0f, std::memory_order_relaxed);
    m_outRoll .store(rotOn ? processed.roll  : 0.0f, std::memory_order_relaxed);
    m_outTs   .store(processed.timestamp_us, std::memory_order_release);

    // --- Positional tracking (6DOF) ---------------------------------------
    float px{}, py{}, pz{};
    if (!PositionEnabled() || !m_receiver->GetPosition(px, py, pz)) {
        m_outPosValid.store(false, std::memory_order_release);
        return;
    }

    // Tag with the receiver stamp so the position interpolator shares the same
    // new-sample detection as the pose interpolator.
    const cameraunlock::PositionData raw(px, py, pz, sampleTs);

    if (recenter) m_posProcessor.SetCenter(raw);

    const cameraunlock::PositionData interpPos = m_posInterp.Update(raw, dt);

    // Tracker-pivot compensation wants the processed rotation as a quaternion.
    const auto rotQ = cameraunlock::math::Quat4::FromYawPitchRoll(
        processed.yaw, processed.pitch, processed.roll);
    const cameraunlock::math::Vec3 offset = m_posProcessor.Process(interpPos, rotQ, dt);

    m_outPosX.store(offset.x, std::memory_order_relaxed);
    m_outPosY.store(offset.y, std::memory_order_relaxed);
    m_outPosZ.store(offset.z, std::memory_order_relaxed);
    m_outPosValid.store(true, std::memory_order_release);
}

void HeadTracking::OnShutdown() {
    if (m_hotkeys)  { m_hotkeys->Stop();  m_hotkeys.reset(); }
    if (m_receiver) { m_receiver->Stop(); m_receiver.reset(); }
}

cameraunlock::TrackingPose HeadTracking::CurrentPose() const {
    cameraunlock::TrackingPose p;
    p.yaw          = m_outYaw  .load(std::memory_order_relaxed);
    p.pitch        = m_outPitch.load(std::memory_order_relaxed);
    p.roll         = m_outRoll .load(std::memory_order_relaxed);
    p.timestamp_us = m_outTs   .load(std::memory_order_acquire);
    return p;
}

HeadPosition HeadTracking::CurrentPosition() const {
    HeadPosition p;
    p.valid = m_outPosValid.load(std::memory_order_acquire);
    p.x = m_outPosX.load(std::memory_order_relaxed);
    p.y = m_outPosY.load(std::memory_order_relaxed);
    p.z = m_outPosZ.load(std::memory_order_relaxed);
    return p;
}

void HeadTracking::Recenter() {
    if (m_receiver) m_receiver->Recenter();
    m_processor.Recenter();
    m_recenterPending.store(true, std::memory_order_release);  // position center captured next frame
    UEHT_LOG(Info, "Recentered.");
}

void HeadTracking::CycleDofMode() {
    const bool wasPos = PositionEnabled();
    DofMode next;
    const char* label;
    switch (GetDofMode()) {
        case DofMode::SixDof:       next = DofMode::RotationOnly; label = "3DOF rotation only"; break;
        case DofMode::RotationOnly: next = DofMode::PositionOnly; label = "3DOF position only"; break;
        default:                    next = DofMode::SixDof;       label = "6DOF (rotation + position)"; break;
    }
    m_dofMode.store(next, std::memory_order_release);
    // Re-seat the position origin only on an off->on transition, so enabling
    // position starts from the current head pose instead of jumping by whatever
    // offset accrued while it was off. Leaving it on (position-only -> 6DOF) must
    // not recenter, or the view would snap back to neutral.
    const bool nowPos = (next != DofMode::RotationOnly);
    if (nowPos && !wasPos) m_recenterPending.store(true, std::memory_order_release);
    UEHT_LOG(Info, "DOF mode: %s", label);
}

void HeadTracking::ToggleYawMode() {
    const bool world = !m_worldSpaceYaw.load(std::memory_order_acquire);
    m_worldSpaceYaw.store(world, std::memory_order_release);
    UEHT_LOG(Info, "Yaw mode: %s", world ? "world-space (horizon-locked)" : "camera-local");
}

}  // namespace ueht
