# 🍷 Proton 11.0-2 — FEX unixlib support · arm64ec + x86_64 (bionic)

Stock **Valve Proton 11.0-2** (final), recompiled from source against **Android bionic** for **arm64ec and x86_64** — a drop-in Wine/Proton runtime for Winlator-bionic emulators, now with the **FEX unixlib loader** built in.

> 🪶 **Smaller & faster to install:** these builds are compiled `-g0` (no debug info) and **stripped**, cutting the installed tree from ~2 GB to **~730 MB** and the download to **~80 MB**. The `.wcp` is now **zstd**-packed, so it decompresses far faster than the old xz (the app auto-detects, no change needed). Assets: `arm64ec` + `x86_64`, for **SDK 28** (Android 9+) and **SDK 35** (Android 15 / 16 KB pages).

---

## ✨ What it provides / does

- 🎮 **Runs Windows games on Android** (arm64ec) — via Wine + FEX or wowbox64
- 🆕 **Latest Valve Proton 11.0-2 base** — not an old snapshot
- ⚡ **Fast-yield gate** — opt-in (`WINE_FAST_YIELD=1`) fix for the bug that pins one CPU core at 100%; better perf/battery
- 🪝 **FEX unixlib loader** — can load FEX's native `.so` add-on (the new thing we built); future-proofs you for when FEX makes it required
- 📁 **Auto-finds the FEX `.so`** — searches the right folder on its own, so **no app change needed**
- 🧩 **Works with any FEXCore version** — not locked to one
- 🎞️ **Video / FMV** — ships `winedmo` (built against **ffmpeg-8**); actual decoding needs the emulator app's imagefs to supply the ffmpeg-8 libs (current Bannerlator / WinNative do), else it falls back to gstreamer.
- 🖼️ **Graphics via DXVK / VKD3D → Vulkan** (Turnip / Adreno)
- 🔄 **esync + fsync + ntsync** — modern threading for heavy games
- 🤖 **Bionic** — works on **Bannerlator**, **WinNative**, and other Winlator-bionic apps

> **In one sentence:** it's your current, sync-modern, fast-yield-capable Proton 11 that **also loads the FEX unixlib on its own with no app changes** — the build that keeps working when FEX makes the unixlib mandatory.

---

## 🌙 Matching FEX unixlib builds (Nightlies)

The unixlib loader stays **dormant** until you install a matched **`-unix` FEXCore**. Grab one from the Nightlies:

**➡️ https://github.com/The412Banner/Nightlies/releases**

Look for the `-unix` assets on the latest build, e.g. `FEX-…-Nightly-…-unix.wcp` (standard) or `FEX-…-PPA-unix.wcp` (PPA). Install that alongside this Proton and the `.so` companion is picked up automatically — no app change.

> ⚠️ **Use the Nightlies `-unix` builds specifically — not other ports.** Older/other FEX `-unix` wcps (e.g. [nicholasx417's](https://github.com/nicholasx417/WinNative-Components/releases)) leave `shm_open`/`shm_unlink` unresolved — Android's bionic doesn't provide those symbols, so the `.so` fails to `dlopen` and silently falls back to the DLL (the unixlib never actually loads). The Nightlies builds compile in stubs for those two functions, so the `.so` loads correctly. Games run fine either way; only whether the unixlib engages differs.

---

## 📦 Which file do I want?

| Your device | Pick |
|---|---|
| Newer device / Android 15+ / 16KB pages | **`…-sdk35.wcp`** |
| Everything else (API 28, 4KB pages) | **`…-sdk28.wcp`** |

`.wcp` = ready to install · `.wcp.xz` = smaller download, extract first. All assets are **arm64ec**.

## 🧩 Compatible with

- **Apps:** Bannerlator, WinNative, and other Winlator-bionic / Cmod-lineage emulators (bionic ABI, `/system/bin/linker64`)
- **Translators:** FEXCore (any version) or wowbox64 for x86 / x86_64
- **Graphics:** DXVK / VKD3D-Proton → Vulkan (Turnip / Adreno)

## ⚠️ Notes

- **arm64ec requires a fresh container.**
- This is **stock Valve Proton, not GE-Proton** — no GE game-fixes / FSR / OptiScaler tier.
- The FEX unixlib loader is **build-ahead**: games run identically today (FEX DLLs are still self-contained); it future-proofs you for when FEX ships thin DLLs that need the native `.so`.

---

## 🔗 Upstream

This is a from-source **bionic recompile** of Valve's official release — no game-behavior patches on top (stock, not GE). All credit for Proton itself goes to **Valve / the Proton team**.

- **Upstream release:** [Valve — Proton 11.0-2](https://github.com/ValveSoftware/Proton/releases/tag/proton-11.0-2)
- **Exact Wine source:** [`ValveSoftware/wine` @ `dc26e618`](https://github.com/ValveSoftware/wine/commit/dc26e61847081a1b5cb0733dc30feba6ee575482) (the commit Proton 11.0-2 pins) — recompiled against Android bionic (arm64ec + x86_64) with our FEX-unixlib / fast-yield / XRandR / DirectAudio additions.

---

*Base: [Valve `proton-11.0-2`](https://github.com/ValveSoftware/Proton/releases/tag/proton-11.0-2) (final, Wine source [`dc26e618`](https://github.com/ValveSoftware/wine/commit/dc26e61847081a1b5cb0733dc30feba6ee575482)) · Wine unix side built with Android NDK r27d · DLL trees: `aarch64-windows` (arm64ec) + `i386-windows` + `x86_64-windows`.*



