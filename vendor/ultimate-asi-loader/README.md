# Ultimate ASI Loader (vendored)

Bundled copy of Ultimate ASI Loader, the install-time source of truth.
Refresh manually with `pixi run update-deps`, then commit.

## Snapshot

- Upstream: https://github.com/ThirteenAG/Ultimate-ASI-Loader
- Tag: `v9.7.1`
- Commit: `2155f2177733d673a3eb783141ceedd564a0a0e2`
- Asset: `Ultimate-ASI-Loader_x64.zip`
- dinput8.dll SHA-256: `810111a7f6a6cef892877c9f7c4582ccde2d621d119891f700c5309c370508bf`
- Fetched at: 2026-05-17T14:33:10.1216037+01:00

`dinput8.dll` is extracted from the upstream asset untouched. install.cmd copies it to
the ABZU exe dir as `dinput8.dll` (the hook slot UE4 4.12 loads ASI plugins through).
