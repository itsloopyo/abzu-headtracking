# Changelog

All notable changes to this project will be documented in this file.

## [Unreleased]

### Fixed

- `AbzuHeadTracking.log` now starts fresh on every launch instead of appending
  forever, and the previous session is kept alongside it as
  `AbzuHeadTracking.prev.log`
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

### Added

- Startup line naming the config file that was loaded, or reporting that none
  was found and defaults are in use

### Changed

- Removed recentring from the mod entirely, along with the `Home` hotkey, the
  `Ctrl+Shift+T` chord and the `[Hotkeys] RecenterKey` setting. Every tracker app
  centres itself, so a mod-side centre was a second centre in series with the
  tracker's and the two drifted apart. The mod now applies the tracker pose as
  absolute; centre it in your tracker app.
- Replace the single `[Tracking] Smoothing` key with `LocalSmoothing` (default 0.0) and `RemoteSmoothing` (default 0.15), selected per connection from the packet source address
- Remove the `[Position] Smoothing` key: position now uses the same connection-selected value as rotation
- Remove the hidden 0.15 baseline smoothing floor, so local trackers get zero-latency tracking by default

## [0.0.0] - 2026-05-17

### Added
- Initial scaffold from cameraunlock-core templates. No working build yet.
