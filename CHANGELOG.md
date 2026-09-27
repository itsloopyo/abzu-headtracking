# Changelog

All notable changes to this project will be documented in this file.

## [Unreleased]

### Fixed

- Leaning forward now gets its full 0.40m of camera travel instead of 0.10m,
  and leaning back gets 0.10m instead of 0.40m. `HeadTracking.ini` carried
  `InvertZ = true` to reach UE's +X-forward axes, but `PositionProcessor`
  applies inversion before its asymmetric `LimitZ` / `LimitZBack` clamp, so
  each lean direction was clamped on the other one's budget. The flip into
  engine axes now happens where the offset is written to the camera cache. The
  key is renamed `InvertTrackerZ` and defaults to false, because an existing
  `HeadTracking.ini` carrying `InvertZ = true` would otherwise reverse the lean
  outright; it now means only what its name says, a tracker whose depth axis
  runs backwards
- Third-party licence compliance. The `.asi` statically links MinHook, whose
  BSD-2-Clause requires its copyright notice and disclaimer to travel with the
  binary, and MinHook in turn carries the separately copyrighted Hacker
  Disassembler Engine 32/64. `THIRD-PARTY-NOTICES.md` now reproduces every
  required licence text in full, corrects the MinHook version to the pinned
  v1.3.4 commit, and states the ABZU trademark and non-affiliation position.
  The Nexus ZIP shipped the binary with no licence or notices at all and now
  carries both. Packaging fails instead of silently omitting a licence file
- `HeadTracking.log` now starts fresh on every launch instead of appending
  forever, and the previous session is kept alongside it as
  `HeadTracking.prev.log`
- A bare `[Logging] LogPath` now resolves next to the game EXE rather than the
  process working directory, so the log is where the README says it is
- Capped the per-frame camera-rotation diagnostic at 20 samples per session; it
  was writing about 180 KB per hour of play
- The log file now opens before the config is parsed. Every config warning
  (non-finite or out-of-range smoothing, the retired `Smoothing` key) was
  written while the file was still closed and was therefore discarded, so a
  misconfigured INI produced a silently corrected value and an empty log
- A `[Logging] LogPath` no longer falls back to a working-directory-relative
  file when the game EXE path is longer than 260 characters. The path lookup
  grows its buffer, and if the EXE still cannot be resolved the mod says so
  instead of dropping the log somewhere the user will not find it
- Bounded the `[Camera] WatchPov` RE diagnostic. Its one-in-30-frames gate
  scaled with refresh rate and each pass emits up to 72 lines, about 62 MB an
  hour at 144fps. Passes are now spaced on the wall clock and capped per
  session
- Log volume during startup and level loads. `LocateGEngine` retries every ~120
  frames until the engine is constructed and dumped the UEngine UClass header
  and up to eight scan candidates on every failed attempt, roughly 25 lines per
  retry for the whole splash-and-menu stretch. Those dumps were scaffolding for
  pinning `SuperStruct`, which is now a verified constant, and the remaining
  failure diagnostics report once per session. The GEngine walk likewise reports
  a stage only when it stops somewhere new
- `InstallDecoupledHook` retried every frame after the camera manager resolved,
  so an out-of-range `UpdateCameraSlot` or a MinHook rejection wrote a line per
  frame for the rest of the session. Neither can become true later, so the mod
  now reports once and stays dormant. Faults on the ControlRotation slot are
  capped at five, and the resolution lines report once instead of on every
  re-resolve
- The mod now says so on the debug channel when the log file cannot be opened,
  instead of silently discarding every line while telling the user to send a log

### Added

- A setting set to `default` in `CameraUnlock.ini` takes its value from `Defaults.ini`, which every head tracking mod that keeps its settings in `CameraUnlock.ini` reads. Head tracking mods that keep their settings in another file do not read it, and neither do earlier versions of this mod. Writing a value in place of `default` changes that setting for this game only. When the mod saves a setting that a hotkey changed in game, it writes the new value in place of `default`, so that setting no longer follows `Defaults.ini` in this game until you set it to `default` again.
- `Defaults.ini` is `%AppData%\CameraUnlock\Defaults.ini` on Windows; `$XDG_CONFIG_HOME/CameraUnlock/Defaults.ini` on Linux, or `~/.config/CameraUnlock/Defaults.ini` where `XDG_CONFIG_HOME` is not set, under Wine and Proton too; and `~/Library/Application Support/CameraUnlock/Defaults.ini` on macOS. The mod's log, where it writes one, names the file it read.
- When the mod starts and finds no `Defaults.ini`, it creates one holding the built-in values, unless Windows runs the game as a packaged app. The mod never changes `Defaults.ini` after that.
- `EnableOnStartup` says whether head tracking is on when the game starts. It defaults to on, as the mod always started.
- Startup line naming the config file that was loaded, or reporting that none
  was found and defaults are in use

### Changed

- Settings move to `AbzuGame\Binaries\Win64\CameraUnlock.ini`. Earlier versions of the mod kept these settings in `HeadTracking.ini`, in the same folder. The first time this version starts and finds no `CameraUnlock.ini`, it reads your settings from `HeadTracking.ini` and writes them into `CameraUnlock.ini`. It never changes `HeadTracking.ini`, and does not read it again while `CameraUnlock.ini` exists.
- A setting that the defaults the README shows set to `default` is written as `default` when you never changed it from the default earlier versions used, because `HeadTracking.ini` does not hold it or holds that default. It then follows `Defaults.ini`, so it takes the value `Defaults.ini` gives it, or the built-in value where `Defaults.ini` gives none, which can differ from the default earlier versions used. A setting you changed is written with the value imported for it, or as `default` where that value equals its default at that start.
- `RotationEnabled` and `PositionEnabled` are one setting here, the tracking mode, so both are written as `default` or neither is.
- Comments, and keys the mod never read, are not carried over. Nor are these, where your old file had them:
  - A sensitivity, scale, deadzone, response curve or axis inversion you changed from its default. Set these in your tracker instead.
  - The aim decoupling setting. Aim is always decoupled now, so your aim stays with the mouse or controller while your head moves the view, even if your old file had decoupling turned off. Here that is `[Camera] Mode`: any value but `updatecamera` wrote your head into the diver's control rotation, so the diver steered with your head.
  - A hotkey set to Ctrl, Shift or Alt on its own. That key goes down before the key of any chord made with it, so the hotkey is left unbound, and it keeps its Ctrl+Shift chord where it has one.
- An older version of the mod reads `HeadTracking.ini` and never reads `CameraUnlock.ini`, so a setting you change after updating is not in `HeadTracking.ini`.
- Deleting only `CameraUnlock.ini` makes the next start read `HeadTracking.ini` again. To go back to the defaults, replace everything in `CameraUnlock.ini` with the defaults the README shows. Every setting they set to `default` then follows `Defaults.ini`.
- Hotkeys are written as key names, and each hotkey lists every key that triggers it, the Ctrl+Shift chord included: `ToggleKey=End, Ctrl+Shift+Y`. `[Hotkeys] PositionKey` is now `CycleTrackingModeKey`.
- A hotkey bound to a plain key no longer fires while Ctrl and Shift are both held, so Ctrl+Shift with that key reaches only a binding that names the chord.
- The tracking mode cycle and the yaw mode toggle save the moment they change, and the game starts in the mode you left it in. Turning head tracking on or off with End still changes the current session only.
- A `HeadTracking.ini` from the dev build names its log `AbzuHeadTracking.log`, and `CameraUnlock.ini` keeps that name. A new install writes `HeadTracking.log`.
- A `HeadTracking.ini` whose `UdpPort` is not a number (which the dev build read as port 0), or whose `[Position]` limits are negative or above 10 metres, is not converted: the mod runs as the old file says, saves nothing, and tries again at the next start.
- Removed recentring from the mod entirely, along with the `Home` hotkey, the
  `Ctrl+Shift+T` chord and the `[Hotkeys] RecenterKey` setting. Every tracker app
  centres itself, so a mod-side centre was a second centre in series with the
  tracker's and the two drifted apart. The mod now applies the tracker pose as
  absolute; centre it in your tracker app.
- Replace the single `[Tracking] Smoothing` key with `LocalSmoothing` (default 0.0) and `RemoteSmoothing` (default 0.15), selected per connection from the packet source address
- Remove the `[Position] Smoothing` key: position now uses the same connection-selected value as rotation
- Remove the hidden 0.15 baseline smoothing floor, so local trackers get zero-latency tracking by default
- Renamed the log file to `HeadTracking.log` (previous session
  `HeadTracking.prev.log`), matching the mod's INI. Uninstall removes the old
  `AbzuHeadTracking.log` / `AbzuHeadTracking.prev.log` from existing installs

### Removed

- The aim decoupling setting, and the coupled aim it could turn on. Aim is always decoupled: your aim stays with the mouse or controller while your head moves the view. In this mod that was `[Camera] Mode=controlrotation`, which is gone along with the code path behind it.
- The sensitivity, scale, deadzone, response curve and axis inversion settings. Set these in your tracker app instead.
- With these settings at their shipped defaults the camera moves as it did before. The shipped `InvertRoll=true` and `[Position] InvertX=true` corrected the mod's own axis conversion and are now part of it.

## [0.0.0] - 2026-05-17

### Added
- Initial scaffold from cameraunlock-core templates. No working build yet.
