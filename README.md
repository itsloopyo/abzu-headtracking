# Abzu Head Tracking

![ABZU running with this mod](https://raw.githubusercontent.com/itsloopyo/abzu-headtracking/main/assets/readme-clip.gif)

An unofficial head tracking mod for ABZU that moves the camera with your head while your mouse or controller keeps control of movement, driven by a webcam, phone, or any OpenTrack compatible tracker, with no VR headset required.

> [!CAUTION]
> ## Experimental prototype - expect missing core features
>
> This is **not** a finished mod.
>
> Current builds may only test whether head tracking can drive the camera. Bug fixes and core features like decoupled look/aim, independent reticle behavior, correct shot direction, off-screen reticle support, movement handling, and comfort tuning may be missing at this early stage of development.

## Features

- **Decoupled look** - head tracking moves only the rendered camera. Your swim direction and every game control stay untouched, so the diver keeps heading where you steer no matter where you look.
- **6DOF positional tracking** - lean and peek with head position, also injected into the rendered view only.
- **Rotation + position DOF modes** - cycle between full 6DOF, rotation-only, and position-only in-game.
- **Frame-rate-independent smoothing** and sample-rate-agnostic interpolation.

## Requirements

- A legitimately purchased copy of [ABZU on Steam](https://store.steampowered.com/app/384190/ABZU/).
- An OpenTrack-compatible tracking source. Get [OpenTrack](https://github.com/opentrack/opentrack/releases) for webcam, VR, or phone input.
- Windows 10 or 11 (64-bit).

## Installation

1. Download the latest `AbzuHeadTracking-vX.Y.Z-installer.zip` from the [Releases](https://github.com/itsloopyo/abzu-headtracking/releases) page.
2. Extract the ZIP anywhere.
3. Double-click `install.cmd`.
4. Configure OpenTrack to output UDP to `127.0.0.1` port `4242` (see below).
5. Launch the game.

If the installer cannot find your ABZU install, point it at the game directly with either an environment variable or a positional argument:

```powershell
# Environment variable
$env:ABZU_PATH = "D:\Games\ABZU"
.\install.cmd

# Or pass the path directly
.\install.cmd "D:\Games\ABZU"
```

### Manual Installation

If you would rather place the files by hand (for example, using the Nexus ZIP, which contains only the mod files):

1. Install the [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases) by placing its `dinput8.dll` in `ABZU\AbzuGame\Binaries\Win64\` next to `AbzuGame-Win64-Shipping.exe`.
2. Copy `AbzuHeadTracking.asi` and `HeadTracking.ini` into the same `Win64` folder.

## Setting Up OpenTrack

In OpenTrack, set the **Output** to `UDP over network` and configure it to send to address `127.0.0.1`, port `4242`. Pick an input source below.

### VR Headset Setup

1. Connect your headset to the PC with Air Link or Virtual Desktop.
2. Launch SteamVR.
3. In OpenTrack, set the **Input** to `SteamVR` (the OpenTrack SteamVR input plugin), then start tracking.

### Webcam Setup

1. In OpenTrack, set the **Input** to `neuralnet tracker` (works with any standard webcam, no IR markers needed).
2. Position yourself in frame, center your head, and start tracking.

### Phone App Setup

- If your phone app smooths its own output (SmoothTrack, etc.), point it directly at your PC's LAN IP on UDP port `4242`.
- If you want OpenTrack's curve mapping and filtering, have the app send to OpenTrack instead and let OpenTrack relay to `127.0.0.1:4242`.

## Controls

Two equivalent binding sets - use whichever your keyboard has. Both fire the same action.

| Action | Nav-cluster | Chord |
|--------|-------------|-------|
| Toggle tracking | `End` | `Ctrl+Shift+Y` |
| Cycle DOF mode (6DOF / rotation-only / position-only) | `Page Up` | `Ctrl+Shift+G` |
| Toggle yaw mode (horizon-locked / camera-local) | `Page Down` | `Ctrl+Shift+H` |

## Configuration

`HeadTracking.ini` is shipped next to `AbzuHeadTracking.asi` in `ABZU\AbzuGame\Binaries\Win64\` and read on startup. Edit it with any text editor. Section and key names are case-insensitive. The most useful settings:

```ini
[Network]
; UDP port the OpenTrack-compatible tracker sends to. Default 4242.
UdpPort = 4242

[Tracking]
; Per-axis sensitivity. 1.0 = 1:1 with tracker.
YawSensitivity   = 1.0
PitchSensitivity = 1.0
RollSensitivity  = 1.0

; Per-axis inversion.
InvertYaw   = false
InvertPitch = false
InvertRoll  = true

; Deadzone in degrees, applied to all axes. 0 = off.
Deadzone = 0.0

; Smoothing is chosen per connection and covers rotation and position. A tracker
; on this machine (loopback) uses LocalSmoothing; a phone or other device on the
; network uses RemoteSmoothing. 0.0 = none .. 1.0 = heavy.
LocalSmoothing  = 0.0
RemoteSmoothing = 0.15

[Hotkeys]
; Win32 VK names (or a numeric VK like 0x22). Ctrl+Shift+Y/G/H chord
; alternatives are baked in for keyboards without a nav cluster.
ToggleKey   = End
PositionKey = PageUp
YawModeKey  = PageDown

[Camera]
; How head tracking reaches the view:
;   updatecamera    : decoupled (default). Adds the head delta to the rendered
;                     camera only, leaving the game's control rotation clean.
;   controlrotation : couples head movement to the swim/control basis.
Mode = updatecamera

; Yaw axis. true (default) = horizon-locked (head-yaw rotates around the world
; up-axis, so "up" stays constant); false = camera-local (leans at extreme
; pitch). Toggle live with Page Down.
WorldSpaceYaw = true

[Position]
; 6DOF positional tracking, decoupled exactly like rotation. Enabled = startup
; state (true = start in 6DOF). Page Up cycles 6DOF -> rotation-only ->
; position-only.
Enabled      = true
SensitivityX = 1.0
SensitivityY = 1.0
SensitivityZ = 1.0
InvertX      = true
InvertY      = false
InvertZ      = true
; Travel limits in meters. Z is asymmetric (more forward than back).
LimitX     = 0.30
LimitY     = 0.20
LimitZ     = 0.40
LimitZBack = 0.10

[Logging]
LogToFile = true
LogPath   = HeadTracking.log
```

The `[Camera]` and `[Position]` sections also carry preset engine offsets (`UpdateCameraSlot`, `PovOffset`, `LocationOffset`) confirmed for the shipping ABZU build. Leave them as shipped unless a game patch moves them.

## Troubleshooting

**Mod not loading**
- Confirm `dinput8.dll`, `AbzuHeadTracking.asi`, and `HeadTracking.ini` are all in `AbzuGame\Binaries\Win64\`.
- Check `HeadTracking.log` in that folder for startup errors. It is rewritten on every launch and the previous run is kept as `HeadTracking.prev.log`, so send both when reporting a problem.

**No tracking response**
- Verify OpenTrack output is set to UDP, address `127.0.0.1`, port `4242`, and that tracking is started.
- Make sure `UdpPort` in `HeadTracking.ini` matches OpenTrack's port.
- Confirm tracking is toggled on (`End` or `Ctrl+Shift+Y`).

**Jittery or unstable tracking**
- Raise `LocalSmoothing` (tracker on this PC) or `RemoteSmoothing` (phone or other network device) toward `1.0` in `HeadTracking.ini`.
- For wireless or phone trackers, increase smoothing in OpenTrack as well.

**Wrong rotation axis or inverted axis**
- Lower the offending axis sensitivity, or raise `Deadzone` above `0.0` to ignore small movements.
- Flip `InvertYaw` / `InvertPitch` / `InvertRoll` if an axis moves the wrong way.
- If the view sits off-straight, centre it in your tracker app (OpenTrack's Center bind, or the CENTER button in your phone app) while looking straight ahead. The mod applies whatever the tracker sends, so the tracker owns the centre.

**View drifts or leans with head position**
- Tune the `[Position]` sensitivities and limits, or flip `InvertX` / `InvertY` / `InvertZ`.
- To disable positional tracking, cycle DOF mode with `Page Up` (or `Ctrl+Shift+G`) to rotation-only, or set `[Position] Enabled = false`.

**Yaw feels wrong when looking up or down at extreme angles**
- Try toggling between world-locked and camera-local yaw with `Page Down` (or `Ctrl+Shift+H`). World-locked (default) is horizon-stable - yaw always turns around vertical; camera-local follows the camera's current up-axis and leans/rolls the view at steep pitch.

## Updating

Download the new release and run `install.cmd` again. Your config is preserved.

## Uninstalling

Run `uninstall.cmd`. This removes the mod files. The ASI loader is only removed if the installer put it there. Use `uninstall.cmd /force` to remove it anyway.

## Building from Source

Prerequisites: [pixi](https://pixi.sh) and the Visual Studio 2022 C++ toolchain.

```powershell
git clone --recurse-submodules https://github.com/itsloopyo/abzu-headtracking.git
cd abzu-headtracking
pixi run build
```

## Community & Support

- Discord: [Loop's Head Tracking Hangout](https://discord.com/invite/dxyZdyFNT9) - setup help, bug reports, and new-release announcements
- [Lopari](https://lopari.app) - free Windows launcher with one-click install and launch for the released head-tracking mods
- [Headcam](https://headcam.app) - free app that turns your iPhone or Android phone into the head tracker

## License

MIT License - see [LICENSE](LICENSE) for details.

## Credits

- [Giant Squid Studios](https://www.giantsquid.com/) and 505 Games for ABZU.
- [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) by ThirteenAG.
- [OpenTrack](https://github.com/opentrack/opentrack) for the UDP tracking protocol.
- [MinHook](https://github.com/TsudaKageyu/minhook) by Tsuda Kageyu.

Third-party license texts are reproduced in
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md), which ships in every release.

## Disclaimer

This is an unofficial, fan-made modification. It is not affiliated with,
endorsed by, or sponsored by Giant Squid Studios, 505 Games, Epic Games, or any
other rights holder. ABZU and all related names, logos, and marks are
trademarks of their respective owners and are used here only to identify the
game the mod applies to.

The mod ships no game code and no game assets, modifies no game files, and
requires a legitimately purchased copy of ABZU. It changes only what the
rendered camera shows while the game is running, and uninstalling it returns
the game to stock.
