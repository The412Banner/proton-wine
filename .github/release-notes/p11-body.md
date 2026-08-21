# 🍷 Proton 11.0-2 — FEX unixlib support · arm64ec + x86_64 (bionic)

Stock **Valve Proton 11.0-2** (final), recompiled from source against **Android bionic** for **arm64ec and x86_64** — a drop-in Wine/Proton runtime for Winlator-bionic emulators, now with the **FEX unixlib loader** built in.

> 🪶 **Smaller & faster to install:** these builds are compiled `-g0` (no debug info) and **stripped**, cutting the installed tree from ~2 GB to **~730 MB** and the download to **~80 MB**. The `.wcp` is now **zstd**-packed, so it decompresses far faster than the old xz (the app auto-detects, no change needed). Assets: `arm64ec` + `x86_64`, for **SDK 28** (Android 9+) and **SDK 35** (Android 15 / 16 KB pages).

---

## 🆕 What changed in 11.0-2 (upstream Valve)

**Full upstream changelog → https://github.com/ValveSoftware/Proton/releases/tag/proton-11.0-2**

11.0-2 is a stabilization / game-fix point release over 11.0-1 (255 wine commits on top of the 11.0-1 base). Highlights:

- **Proton 11 regressions fixed (vs Proton 10):** Terraria performance, Source SDK 2007/2013 `gameinfo.txt` detection, Natural Selection 2 servers, C&C series Steam Workshop level detection, CEG-game startup delay, The Talos Principle login window, Exanima performance.
- **Recent-update / anti-cheat breakage fixed:** Helldivers 2, Marvel Rivals, Arc Raiders, Metal Gear Solid V: The Phantom Pain, Diablo IV, Trove PTS (several via EAC launcher locale handling).
- **Controller:** improved controller-hotplug reliability; Rocket League no longer mis-detects re-plugged DualShock 4 / DualSense as an extra device.
- **alt+tab / black-screen / fullscreen:** SHOGUN: Total War, Homeworld 2 Classic, Forza Horizon 4/5/6, Resident Evil 2 low-res fullscreen, DIRT 5 hybrid-core crash.
- **Locale-at-launch:** Rocket League, Tom Clancy's The Division 2 and Squad no longer fail to start under non-English locales.
- **Media / video:** MediaFoundation `topology_loader` rework; audio fixes (Portal 2 7.1 setups, Dakka Squadron intro video).
- **Newly playable:** Warhammer: Dark Omen (Classic), Plain Sight, Portal Worlds, SMILE GAME BUILDER, Heroes of the Three Kingdoms 7, and more.

### 🧵 ARM64EC / FEX — what 11.0-2 means for this build
- **Our FEXCore unixlib support is unchanged — still built our way.** Valve did **not** adopt an equivalent loader. In fact 11.0-2 *reverts* three of Valve's own FEX-specific desktop pieces (the FEX stats-shm interface, FEX kernel-side unaligned-atomic handling, and FEX Asahi TSO support) — desktop-ARM experiments unrelated to our Android arm64ec unixlib scheme, which we continue to apply via our own patches.
- **We do inherit real upstream ARM64EC fixes:** cooperative suspend, syscall-callback `NtContinue` support, `RtlRaiseException` caller-context synthesis, and a new WoW64 `BTCpuNotifyProcessExecuteFlagsChange` interface — all of which benefit the arm64ec runtime we ship.

---

## ✨ What it provides / does

- 🎮 **Runs Windows games on Android** (arm64ec) — via Wine + FEX or wowbox64
- 🆕 **Latest Valve Proton 11.0-2 base** — not an old snapshot
- ⚡ **Fast-yield gate** — opt-in (`WINE_FAST_YIELD=1`) fix for the bug that pins one CPU core at 100%; better perf/battery
- 🖥️ **In-game refresh unlock** — built with `--with-xrandr/--with-xrender` so games can select 144/120/90/60 Hz (needs the app's refresh-unlock setting)
- 🔊 **DirectAudio 1.3.1** driver — low-latency in-unixlib AAudio path (opt-in per container; app-injected on qualifying arm64ec layers)
- 🩹 **Bionic bug-fixes** — noexec/anti-tamper relaxation, drive-root file-copy crash fix, `LC_ALL=C.UTF-8` locale bring-up
- 🪝 **FEX unixlib loader** — can load FEX's native `.so` add-on; future-proofs you for when FEX makes it required
- 📁 **Auto-finds the FEX `.so`** — searches the right folder on its own, so **no app change needed**
- 🧩 **Works with any FEXCore version** — not locked to one
- 🎞️ **Video / FMV** — ships `winedmo` (built against **ffmpeg-8**); actual decoding needs the emulator app's imagefs to supply the ffmpeg-8 libs (current Bannerlator / WinNative do), else it falls back to gstreamer.
- 🖼️ **Graphics via DXVK / VKD3D → Vulkan** (Turnip / Adreno) — installed separately, not bundled in this layer
- 🔄 **esync + fsync + ntsync** — modern threading for heavy games
- 🤖 **Bionic** — works on **Bannerlator**, **WinNative**, and other Winlator-bionic apps

> **In one sentence:** it's the current Valve Proton 11.0-2, sync-modern and fast-yield-capable, that **also loads the FEX unixlib on its own with no app changes** — the build that keeps working when FEX makes the unixlib mandatory.

---

## 🌙 Matching FEX unixlib builds (Nightlies)

The unixlib loader stays **dormant** until you install a matched **`-unix` FEXCore**. Grab one from the Nightlies:

**➡️ https://github.com/The412Banner/Nightlies/releases**

Look for the `-unix` assets on the latest build, e.g. `FEX-…-Nightly-…-unix.wcp` (standard) or `FEX-…-PPA-unix.wcp` (PPA). Install that alongside this Proton and the `.so` companion is picked up automatically — no app change.

> ⚠️ **Use the Nightlies `-unix` builds specifically — not other ports.** Older/other FEX `-unix` wcps leave `shm_open`/`shm_unlink` unresolved — Android's bionic doesn't provide those symbols, so the `.so` fails to `dlopen` and silently falls back to the DLL (the unixlib never actually loads). The Nightlies builds compile in stubs for those two functions, so the `.so` loads correctly. Games run fine either way; only whether the unixlib engages differs.

---

## 📦 Which file do I want?

All assets are `.wcp` (zstd, ready to install — no extracting). Each SDK ships an **arm64ec** build (the native, recommended runtime) and an **x86_64** build (box64 double-emulation; arm64ec is preferred).

| Your device | Pick (arm64ec) |
|---|---|
| Newer device / Android 15+ / 16 KB pages | **`proton-11.0-2-arm64ec-sdk35.wcp`** |
| Everything else (API 28, 4 KB pages) | **`proton-11.0-2-arm64ec-sdk28.wcp`** |

x86_64 equivalents (`proton-11.0-2-x86_64-sdk28.wcp` / `-sdk35.wcp`) are provided for parity but arm64ec is the working runtime — see Notes.

## 🧩 Compatible with

- **Apps:** Bannerlator, WinNative, and other Winlator-bionic / Cmod-lineage emulators (bionic ABI, `/system/bin/linker64`)
- **Translators:** FEXCore (any version) or wowbox64 for x86 / x86_64
- **Graphics:** DXVK / VKD3D-Proton → Vulkan (Turnip / Adreno) — installed as separate components

## ⚠️ Notes

- **arm64ec requires a fresh container.**
- This is **stock Valve Proton, not GE-Proton** — no GE game-fixes / FSR / OptiScaler tier.
- **DXVK/VKD3D and the x86 translator (FEX/box64) are not bundled** in this layer — install them as separate components, as with every Proton layer.
- **x86_64 under box64 is not a working GUI runtime** on Proton 11 (disabled RpcSs/PlugPlay + an unresolved window-display issue) — use the arm64ec asset.
- The FEX unixlib loader is **build-ahead**: games run identically today (FEX DLLs are still self-contained); it future-proofs you for when FEX ships thin DLLs that need the native `.so`.

---

*Base: Valve [`proton-11.0-2`](https://github.com/ValveSoftware/Proton/releases/tag/proton-11.0-2) (final) · Wine unix side built with Android NDK r27d · DLL trees: `aarch64-windows` (arm64ec) + `i386-windows` + `x86_64-windows`.*
