# Third-Party Notices

AbzuHeadTracking bundles or links against the following third-party components.

## Ultimate ASI Loader

- **Version:** v9.7.1
- **License:** MIT
- **Upstream:** https://github.com/ThirteenAG/Ultimate-ASI-Loader
- **Usage:** Loads the mod's `.asi` plugin into the ABZU process via the `dinput8.dll` hook slot.
- **Bundled:** yes. Bundled in release ZIP and used as the install-time source.

---

## MinHook

- **Version:** 1.3.3
- **License:** BSD-2-Clause
- **Upstream:** https://github.com/TsudaKageyu/minhook
- **Usage:** Inline function hooking for the camera and D3D11 hooks.
- **Bundled:** no. Statically linked into the mod DLL.

---

## cameraunlock-core

- **Version:** submodule (see `.gitmodules`)
- **License:** MIT
- **Upstream:** https://github.com/itsloopyo/cameraunlock-core
- **Usage:** Shared head-tracking processing, protocol, and math used by the mod.
- **Bundled:** no. Compiled into the mod DLL.

---

## OpenTrack

- **Version:** N/A (UDP protocol only)
- **License:** ISC
- **Upstream:** https://github.com/opentrack/opentrack
- **Usage:** UDP pose protocol on port 4242. No OpenTrack code is bundled.
- **Bundled:** no.

---

ABZU is developed by Giant Squid and published by 505 Games. This mod is an
unofficial third-party modification; no game code, assets, or proprietary
DLLs are redistributed.
