# Abzu Head Tracking

![ABZU running with this mod](https://raw.githubusercontent.com/itsloopyo/abzu-headtracking/main/assets/readme-clip.gif)

An unofficial head tracking mod for ABZU that moves the camera with your head while your mouse or controller keeps control of movement, driven by a webcam, phone, or any OpenTrack compatible tracker, with no VR headset required.

## Features

- **Decoupled look** - head tracking moves only the rendered camera. Your swim direction and every game control stay untouched, so the diver keeps heading where you steer no matter where you look.
- **6DOF positional tracking** - lean and peek with head position, also injected into the rendered view only.
- **Works with any OpenTrack compatible tracker** - free options available for PC, iOS and Android

## Requirements

- A legitimately purchased copy of [ABZU on Steam](https://store.steampowered.com/app/384190/ABZU/).
- An OpenTrack-compatible tracking source. Get [OpenTrack](https://github.com/opentrack/opentrack/releases) for webcam, VR, or phone input.
- Windows 10 or 11 (64-bit).

## Installation

### Lopari

Download [Lopari](https://lopari.app), choose **ABZÛ**, and click
**Play with head tracking**.

### Standalone Installer

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
2. Copy `AbzuHeadTracking.asi` into the same `Win64` folder. The mod creates `CameraUnlock.ini` there when it first starts.

## Setting Up OpenTrack

The mod listens for OpenTrack pose data on UDP port `4242`, on every network
interface. One datagram is six little-endian 64-bit floats in the order
`x, y, z, yaw, pitch, roll`: position in centimetres, rotation in degrees, 48
bytes in total. Anything that sends that to that port drives the view.
OpenTrack's **UDP over network** output sends exactly this, and the steps below
set it up.

1. Install [OpenTrack](https://github.com/opentrack/opentrack/releases).
2. Pick a tracker under **Input**, using the notes below.
3. Set **Output** to **UDP over network**, host `127.0.0.1`, port `4242`.
4. Press **Start**. Tracking and the game can start in either order.

### Webcam

OpenTrack ships a `neuralnet tracker` input that reads a plain webcam. Select it
under **Input**, pick your camera in its settings, and use the output settings
above. How well it tracks depends on your camera and your lighting, so try it
before buying anything.

### Phone

A phone app can reach the mod directly, with no OpenTrack on the PC, if it sends
the datagram described above. Point it at this PC's IP address (run `ipconfig`
to find it) on port `4242`. Not every phone tracker speaks this protocol, so
check yours for an OpenTrack or UDP output option first. [Headcam](https://headcam.app)
sends it, and I wrote it so decent tracking is free for anyone who already owns
a phone.

Sending direct works when the app filters its own signal on the device. The
mod's smoothing is sized to take the edge off a clean signal rather than to
rescue a noisy one, so a raw feed sent direct will jitter. If it does, point the
app at OpenTrack's **UDP over network** *input* on some other port, say 5252,
and let OpenTrack's filters and curves clean it up before its output forwards to
`127.0.0.1:4242`.

Anything arriving from outside `127.0.0.0/8` counts as a remote connection and
is smoothed with `RemoteSmoothing` rather than `LocalSmoothing`. That includes a
tracker on this very PC that sends to the machine's own LAN address, because the
mod reads the source address and not the machine.

### Headset or other hardware

If your device has an OpenTrack input driver, select it under **Input** and use
the same output settings. OpenTrack's own **Input** list is the authority on
what it can read; the mod only ever sees what OpenTrack sends.

### Centring

Centring belongs to your tracker. The mod subtracts no centre of its own: it
applies the pose it receives exactly as it arrives, so a stream of zeros holds
the view where the game itself puts it. Press the centre control in your tracker
(OpenTrack's **Center** bind, or the CENTER button in Headcam) and the tracker
zeroes its own output, which leaves the view centred with the mod doing nothing.

That is why there is no centre hotkey here and nothing to re-centre in game. Two
centres in series would drift apart, because each side re-centres at moments the
other cannot see, and you would end up pressing twice to centre once. If the
view sits off to one side, centre it in the tracker.

## Controls

Two equivalent binding sets by default - use whichever your keyboard has:

| Action | Nav-cluster | Chord |
|--------|-------------|-------|
| Toggle tracking | `End` | `Ctrl+Shift+Y` |
| Cycle tracking mode (6DOF / rotation-only / position-only) | `Page Up` | `Ctrl+Shift+G` |
| Toggle yaw mode (horizon-locked / camera-local) | `Page Down` | `Ctrl+Shift+H` |

Each action's keys are a list in the `[Hotkeys]` section of `CameraUnlock.ini`, chords included, so any of them can be changed or removed there (see [Configuration](#configuration)).

The tracking mode and the yaw mode you pick are saved to `CameraUnlock.ini` and come back at the next start. **Toggle tracking** changes the current session only and is never saved. Whether head tracking is on when the game starts is `EnableOnStartup`.

## Configuration

Close the game before editing `CameraUnlock.ini`, then launch it again to load your changes. Set sensitivity, axis mapping, and centering in your tracker.

<!-- cameraunlock:config -->
The mod reads its settings from `AbzuGame\Binaries\Win64\CameraUnlock.ini` in the game folder, and creates the file when it starts and finds none. Edit it with any text editor.

A setting set to `default` takes its value from `Defaults.ini`, which every head tracking mod that keeps its settings in `CameraUnlock.ini` reads. Head tracking mods that keep their settings in another file do not read it. Writing a value in place of `default` changes that setting for this game only. When the mod saves a setting that a hotkey changed in game, it writes the new value in place of `default`, so that setting no longer follows `Defaults.ini` in this game until you set it to `default` again.

`Defaults.ini` is `%AppData%\CameraUnlock\Defaults.ini` on Windows; `$XDG_CONFIG_HOME/CameraUnlock/Defaults.ini` on Linux, or `~/.config/CameraUnlock/Defaults.ini` where `XDG_CONFIG_HOME` is not set, under Wine and Proton too; and `~/Library/Application Support/CameraUnlock/Defaults.ini` on macOS. The mod's log, where it writes one, names the file it read.

When the mod starts and finds no `Defaults.ini`, it creates one holding the built-in values, unless Windows runs the game as a packaged app. The mod never changes `Defaults.ini` after that. Edit it with any text editor.

The built-in value of each setting set to `default` below:

- `UdpPort=4242`
- `EnableOnStartup=true`
- `WorldSpaceYaw=true`
- `RotationEnabled=true`
- `LocalSmoothing=0.0`
- `RemoteSmoothing=0.15`
- `PositionEnabled=true`
- `PositionLimitX=0.3`
- `PositionLimitY=0.2`
- `PositionLimitYDown=0.2`
- `PositionLimitZ=0.4`
- `PositionLimitZBack=0.1`
- `ToggleKey=End, Ctrl+Shift+Y`
- `CycleTrackingModeKey=PageUp, Ctrl+Shift+G`
- `YawModeKey=PageDown, Ctrl+Shift+H`

With every setting at its default, the file reads:

```ini
; ABZU head tracking settings.
; Comments start with ; and go on their own line. Text after a value is part of the value.
; Hotkeys are key names such as End, PageUp or Ctrl+Shift+Y. Separate several with commas; leave empty for none.
; A setting set to default takes its value from Defaults.ini, which every head tracking mod
; that keeps its settings in CameraUnlock.ini reads: %AppData%\CameraUnlock\Defaults.ini on
; Windows, $XDG_CONFIG_HOME/CameraUnlock/Defaults.ini (normally ~/.config/CameraUnlock) on
; Linux, under Wine and Proton too, and ~/Library/Application Support/CameraUnlock/Defaults.ini
; on macOS. The log names the file it read. Write a value instead of default to change that
; setting for this game only.

[CameraUnlock]
; Written by the mod. Leave this section in place.
ConfigFormat=1

[Network]
; UDP port the mod receives tracker data on (OpenTrack protocol).
UdpPort=default

[General]
; true: head tracking is on when the game starts. ToggleKey turns it on and off.
EnableOnStartup=default
; true: yaw turns around the world's up axis. false: around the camera's own up axis.
WorldSpaceYaw=default
; true: turning your head turns the view.
; Tracking mode at startup, with PositionEnabled. The mode hotkey changes both.
RotationEnabled=default

[Smoothing]
; Smoothing when the tracker runs on this PC. 0 is the least, 1 the most.
LocalSmoothing=default
; Smoothing when the tracker is another device on the network, such as a phone.
; 0 is the least, 1 the most.
RemoteSmoothing=default

[Position]
; true: moving your head moves the view.
; Tracking mode at startup, with RotationEnabled. The mode hotkey changes both.
PositionEnabled=default
; How far, in metres, leaning left or right can move the view.
PositionLimitX=default
; How far, in metres, raising your head can move the view.
PositionLimitY=default
; How far, in metres, lowering your head can move the view.
PositionLimitYDown=default
; How far, in metres, leaning forward can move the view.
PositionLimitZ=default
; How far, in metres, leaning back can move the view.
PositionLimitZBack=default
; Where in the PlayerCameraManager the rendered camera position sits. 0x0 = no lean.
; LocationOffset=0x3F8

[Hotkeys]
; Turns head tracking on and off.
ToggleKey=default
; Changes the tracking mode: rotation and position, rotation only, position only.
CycleTrackingModeKey=default
; Switches yaw between the world's up axis and the camera's own (WorldSpaceYaw).
YawModeKey=default

[Camera]
; The PlayerCameraManager vtable slot of UpdateCamera, which the mod hooks to move the
; rendered view. Outside 0 to 255 the hook is not installed and the mod does nothing.
; UpdateCameraSlot=196
; Where in the PlayerCameraManager the rendered camera rotation sits. 0x0 = off.
; PovOffset=0x404
; A second camera rotation to move with the first. 0x0 = off.
; CacheOffset=0x0
; Diagnostics: true logs the PlayerCameraManager vtable once. Leave off for play.
DumpVtable=false
; Diagnostics: true logs which camera offsets change as you look around. Leave off for play.
WatchPov=false

[Logging]
; true: write a log file for bug reports.
LogToFile=true
; The log file. A bare file name is next to the game's executable.
LogPath=HeadTracking.log
```
<!-- /cameraunlock:config -->

The mod discovers the camera hook and its fields from the installed game and
validates their live layouts before applying tracking. The engine settings
`UpdateCameraSlot`, `PovOffset`, `CacheOffset` and `LocationOffset` supply values
only for the exact historical fallback. With runtime discovery, zero still
disables the corresponding camera write, and a negative slot disables the hook.
`CacheOffset=0` leaves the secondary rotation write off. Leave these settings
alone; `HeadTracking.log` reports discovery failures that may need a mod update.

## Troubleshooting

**Mod not loading**
- Confirm the ASI loader and `AbzuHeadTracking.asi` are both in `AbzuGame\Binaries\Win64\`, and that the mod created `CameraUnlock.ini` there.
- Check `HeadTracking.log` in that folder for startup errors. It is rewritten on every launch and the previous run is kept as `HeadTracking.prev.log`, so send both when reporting a problem.

**No tracking response**
- Verify OpenTrack output is set to UDP, address `127.0.0.1`, port `4242`, and that tracking is started.
- Make sure `UdpPort` in `CameraUnlock.ini` matches OpenTrack's port.
- Confirm tracking is toggled on (`End` or `Ctrl+Shift+Y`).

**Jittery or unstable tracking**
- Raise `LocalSmoothing` (tracker on this PC) or `RemoteSmoothing` (phone or other network device) toward `1.0` in `CameraUnlock.ini`.
- For wireless or phone trackers, increase smoothing in OpenTrack as well.

**Wrong rotation axis or inverted axis**
- Sensitivity, deadzone and axis inversion are set in your tracker app, not in the mod.
- If the view sits off-straight, centre it in your tracker app (OpenTrack's Center bind, or the CENTER button in your phone app) while looking straight ahead. The mod applies whatever the tracker sends, so the tracker owns the centre.

**View drifts or leans with head position**
- Tune the position limits (`PositionLimitX` and the rest) in `CameraUnlock.ini`. A sway, heave or depth axis that moves the wrong way is set in your tracker app.
- To disable positional tracking, cycle the tracking mode with `Page Up` (or `Ctrl+Shift+G`) to rotation-only, or set `PositionEnabled=false`.

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
