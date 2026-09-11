Consolidated **arm64ec (bionic)** build of all **seven** Proton / GE-Proton layers, rebuilt from the [`build-bionic-layers-20260908-xinput`](https://github.com/The412Banner/proton-wine/releases/tag/build-bionic-layers-20260908-xinput) sources — every layer is its v6 build plus the **Wine XP desktop**: a Windows XP (Luna) style taskbar, start menu, window frames and visual style for buttons and controls, drawn by Wine itself. Nothing else was added, removed or rebased. Every layer is a single `.wcp` that runs on both 4 KB- and 16 KB-page devices. **Proton 11.0-2 additionally ships an x86_64 build** alongside arm64ec.

> 🧪 **Pre-release — for testing.** v6 stays the current release and the in-app catalog default; this build is not offered by the app yet. The wcp install into a new coexisting `<version>-arm64ec-7` slot next to v6's `-6` — nothing is overwritten, and existing containers keep working.
>
> **Device-tested on one layer only.** The Wine XP desktop was built and proven on **GE-Proton 11.0-6** (Adreno 750): taskbar, start menu, Display Properties, window frames including game (Vulkan / OpenGL) windows, the XP visual style in light and dark mode, and a fresh container starting in full XP. The other six layers carry the same changes — confirmed present in each built layer — but have **not** been booted on a device. The two Wine 10 layers needed a small port (Wine 10 handles GPU windows and its single `comctl32` differently). The 16 KB-page boot of these wcp remains untested.

## What's new

- 🪟 **Windows XP style taskbar and start menu** (all layers) — Wine's own explorer shell redrawn in the Luna style: green **start** button with the Wine logo, task buttons, notification area and a clock with the day and date, and a taskbar you can drag to one, two or three rows. The start menu has the user tile, pinned and recent programs with the icons of their shortcut targets, My Documents / My Pictures / My Music / My Computer, Control Panel, Task Manager, Wine Configuration, Run and **All Programs** cascades. Right-click the desktop → **Display Properties** to choose the style (Windows XP or Classic), the colour scheme (Blue, Olive Green, Silver), XP title bars, XP buttons and controls, the Wine XP desktop background, taskbar size, lock and clock — every change applies live.
- 🖼️ **XP window frames** — title bars, caption buttons and window edges in the XP style, drawn by `win32u` for every program. Paint-only: frame and title-bar sizes stay the usual system metrics, so nothing a game can measure changes. Game windows (Vulkan / OpenGL) paint their frame through a bitmap, so it shows correctly on the app's X server instead of white or black.
- 🎛️ **XP visual style for controls** — a new `winexp.msstyles` theme: push buttons, check boxes, radio buttons, group boxes, scroll bars, combo boxes, edit fields, spinners, tabs, progress bars, trackbars, list headers, tree views, toolbars, status bars and rebars. It has no system metrics — it changes how controls are drawn, never their size or the system colours — and `uxtheme` picks up a theme switch from another process immediately.
- 🌓 **Dark mode** — with the app's dark theme every scheme turns into a dark twin: **Blue → navy, Olive Green → moss, Silver → graphite** (title bars, frames, taskbar, start menu, dark control artwork with light text). Wine readability fixes that also help its own theme: themed tab labels use the theme's text colour, and labels on themed tab pages keep the normal text colour.
- 🔌 **Turn Off really ends the session** — the start menu now ends every program and the desktop (programs still get the normal end-session messages and can cancel). A background helper without a window used to keep an empty desktop running.
- 📝 **Wordpad icon** — `write.exe` carries the Wordpad icon, as on Windows, so shortcuts to it show one.
- ⚙️ **Defaults** — a new container starts with the whole Wine XP desktop in **Blue**, every option on. Existing containers moved to v7 get it too (they have no XP settings yet); **Display Properties → Windows and buttons → Classic** switches back to the plain Wine look.
- 🔢 **versionCode → 7** on every layer — new coexisting install slot `<version>-arm64ec-7` next to v6's `-6`.
- ♻️ **Carried over from v6** — XInput update-thread fix (controllers no longer die mid-game) · `RtlIsEcCode` bounds check (Denuvo unwind loop, *NFS Heat*) · DirectAudio v1.3.2 · ws2_32 dual-stack DNS · nsiproxy default route (`WINE_ANDROID_GATEWAY`) · gdiplus span clamp (Wine 11 layers) · realized-font-handle cap `32768` · GE game-fix tiers · SD-card boot fix (`noexec` / `force_anon`) · drive-root copy fix · `C.UTF-8` locale · `WINEVMEMMAXSIZE` cap · fast-yield gate · FEX-unixlib loader · XRandR / XRender · one wcp per layer (4 KB + 16 KB pages).

**Scope of the changes versus v6:** PE-side `explorer.exe`, `uxtheme.dll`, `comctl32` (themed tab labels) and `write.exe`, the new `winexp.msstyles` (installed to `C:\windows\resources\themes\winexp`), the unix-side `win32u.so` (frame painting), and the `versionCode` stamp. No `ntdll`, FEX, DXVK, audio or input changes.

## Layers

<details>
<summary><b>GE-Proton 11.0-6</b> &nbsp;·&nbsp; arm64ec · Wine 11 · versionCode <code>7</code></summary>

<br>

| | |
|---|---|
| **Base** | GloriousEggroll **[GE-Proton11-6](https://github.com/GloriousEggroll/proton-ge-custom/releases/tag/GE-Proton11-6)** game-fix tier on Valve **[Proton 11.0-1](https://github.com/ValveSoftware/Proton/releases/tag/proton-11.0-1)** (Wine 11.0-1) |
| **Installs as** | `11.0-6-arm64ec-7` |
| **ntdll fix** | `RtlIsEcCode` bounds check (Denuvo unwind loop) |
| **EA fixes** | ws2_32 dual-stack DNS · nsiproxy default route · gdiplus span clamp |
| **DirectAudio** | v1.3.2 (vendored source) — opt-in via registry `Audio=directaudio`; mic capture opt-in via `BANNER_AUDIO_DIRECT_MIC=1` |
| **XInput fix** | update thread survives transient wait failures (controllers no longer die mid-game) |
| **Wine XP desktop** | Luna taskbar + start menu · XP window frames · `winexp.msstyles` visual style — Blue / Olive Green / Silver, navy / moss / graphite in dark mode |
| **Assets** | `GE-proton-11.0-6-arm64ec.wcp` (4 KB + 16 KB pages) |

**Android compatibility fixes** — SD-card boot (`noexec` / `force_anon`) · drive-root copy · `C.UTF-8` locale
**Runtime** — realized-font-handle cap `32768` · `WINEVMEMMAXSIZE` cap · fast-yield gate · FEX-unixlib loader · XRandR / XRender
**Build** — `-g0 -O2` release build, `llvm-strip` on both the PE DLLs/EXEs and the unix `.so` loaders · zstd-compressed `.wcp` · ccache in CI (build speed only, not in the layer)
**Inherited bionic base** — the Winlator-bionic / GameNative Android patch set every layer is built on: esync/fsync, winex11 driver (window/keyboard/mouse/OpenGL/bitblt), preloader, clipboard, winemenubuilder, MIDI, DNS resolver, wow64 syscall path
**GE game-fixes** — `maplestory` · `dai_xinput` · `eac` · `pso2` · `assettocorsa` · `silence-starcitizen` · `vgsoh` · `WM_ACTIVATEAPP`

> ℹ️ GE dropped its `battlenet` workaround upstream in GE-Proton11-6, so this layer's game-fix set is the 11.0-5 tier minus `battlenet`. This is the layer the *NFS Heat* / Denuvo fix was device-proven on.

</details>

<details>
<summary><b>GE-Proton 11.0-5</b> &nbsp;·&nbsp; arm64ec · Wine 11 · versionCode <code>7</code></summary>

<br>

| | |
|---|---|
| **Base** | GloriousEggroll **[GE-Proton11-5](https://github.com/GloriousEggroll/proton-ge-custom/releases/tag/GE-Proton11-5)** game-fix tier on Valve **[Proton 11.0-1](https://github.com/ValveSoftware/Proton/releases/tag/proton-11.0-1)** (Wine 11.0-1) |
| **Installs as** | `11.0-5-arm64ec-7` |
| **ntdll fix** | `RtlIsEcCode` bounds check (Denuvo unwind loop) |
| **EA fixes** | ws2_32 dual-stack DNS · nsiproxy default route · gdiplus span clamp |
| **DirectAudio** | v1.3.2 (vendored source) — opt-in via registry `Audio=directaudio`; mic capture opt-in via `BANNER_AUDIO_DIRECT_MIC=1` |
| **XInput fix** | update thread survives transient wait failures (controllers no longer die mid-game) |
| **Wine XP desktop** | Luna taskbar + start menu · XP window frames · `winexp.msstyles` visual style — Blue / Olive Green / Silver, navy / moss / graphite in dark mode |
| **Assets** | `GE-proton-11.0-5-arm64ec.wcp` (4 KB + 16 KB pages) |

**Android compatibility fixes** — SD-card boot (`noexec` / `force_anon`) · drive-root copy · `C.UTF-8` locale
**Runtime** — realized-font-handle cap `32768` · `WINEVMEMMAXSIZE` cap · fast-yield gate · FEX-unixlib loader · XRandR / XRender
**Build** — `-g0 -O2` release build, `llvm-strip` on both the PE DLLs/EXEs and the unix `.so` loaders · zstd-compressed `.wcp` · ccache in CI (build speed only, not in the layer)
**Inherited bionic base** — the Winlator-bionic / GameNative Android patch set every layer is built on: esync/fsync, winex11 driver (window/keyboard/mouse/OpenGL/bitblt), preloader, clipboard, winemenubuilder, MIDI, DNS resolver, wow64 syscall path
**GE game-fixes** — `battlenet` · `maplestory` · `dai_xinput` · `eac` · `pso2` · `assettocorsa` · `silence-starcitizen` · `vgsoh` · `WM_ACTIVATEAPP`

</details>

<details>
<summary><b>GE-Proton 11.0-3</b> &nbsp;·&nbsp; arm64ec · Wine 11 · versionCode <code>7</code></summary>

<br>

| | |
|---|---|
| **Base** | GloriousEggroll **[GE-Proton11-3](https://github.com/GloriousEggroll/proton-ge-custom/releases/tag/GE-Proton11-3)** game-fix tier on Valve **[Proton 11.0-1](https://github.com/ValveSoftware/Proton/releases/tag/proton-11.0-1)** (Wine 11.0-1) |
| **Installs as** | `11.0-3-arm64ec-7` |
| **ntdll fix** | `RtlIsEcCode` bounds check (Denuvo unwind loop) |
| **EA fixes** | ws2_32 dual-stack DNS · nsiproxy default route · gdiplus span clamp |
| **DirectAudio** | v1.3.2 — opt-in via registry `Audio=directaudio`; mic capture opt-in via `BANNER_AUDIO_DIRECT_MIC=1` |
| **XInput fix** | update thread survives transient wait failures (controllers no longer die mid-game) |
| **Wine XP desktop** | Luna taskbar + start menu · XP window frames · `winexp.msstyles` visual style — Blue / Olive Green / Silver, navy / moss / graphite in dark mode |
| **Assets** | `GE-proton-11.0-3-arm64ec.wcp` (4 KB + 16 KB pages) |

**Android compatibility fixes** — SD-card boot (`noexec` / `force_anon`) · drive-root copy · `C.UTF-8` locale
**Runtime** — realized-font-handle cap `32768` · `WINEVMEMMAXSIZE` cap · fast-yield gate · FEX-unixlib loader · XRandR / XRender
**Build** — `-g0 -O2` release build, `llvm-strip` on both the PE DLLs/EXEs and the unix `.so` loaders · zstd-compressed `.wcp` · ccache in CI (build speed only, not in the layer)
**Inherited bionic base** — the Winlator-bionic / GameNative Android patch set every layer is built on: esync/fsync, winex11 driver (window/keyboard/mouse/OpenGL/bitblt), preloader, clipboard, winemenubuilder, MIDI, DNS resolver, wow64 syscall path
**GE game-fixes** — `battlenet` · `maplestory` · `dai_xinput` · `eac` · `pso2` · `assettocorsa` · `silence-starcitizen` · `vgsoh` · `WM_ACTIVATEAPP`

</details>

<details>
<summary><b>Proton 11.0-1</b> &nbsp;·&nbsp; arm64ec · Wine 11 · versionCode <code>7</code></summary>

<br>

| | |
|---|---|
| **Base** | Stock Valve **[Proton 11.0-1](https://github.com/ValveSoftware/Proton/releases/tag/proton-11.0-1)** (Wine 11.0-1) — plain Proton, no GE game-fixes |
| **Installs as** | `11.0-1-arm64ec-7` |
| **ntdll fix** | `RtlIsEcCode` bounds check (Denuvo unwind loop) |
| **EA fixes** | ws2_32 dual-stack DNS · nsiproxy default route · gdiplus span clamp |
| **DirectAudio** | v1.3.2 — opt-in via registry `Audio=directaudio`; mic capture opt-in via `BANNER_AUDIO_DIRECT_MIC=1` |
| **XInput fix** | update thread survives transient wait failures (controllers no longer die mid-game) |
| **Wine XP desktop** | Luna taskbar + start menu · XP window frames · `winexp.msstyles` visual style — Blue / Olive Green / Silver, navy / moss / graphite in dark mode |
| **Assets** | `proton-11.0-1-arm64ec.wcp` (4 KB + 16 KB pages) |

**Android compatibility fixes** — SD-card boot (`noexec` / `force_anon`) · drive-root copy · `C.UTF-8` locale
**Runtime** — realized-font-handle cap `32768` · `WINEVMEMMAXSIZE` cap · fast-yield gate · FEX-unixlib loader · XRandR / XRender
**Build** — `-g0 -O2` release build, `llvm-strip` on both the PE DLLs/EXEs and the unix `.so` loaders · zstd-compressed `.wcp` · ccache in CI (build speed only, not in the layer)
**Inherited bionic base** — the Winlator-bionic / GameNative Android patch set every layer is built on: esync/fsync, winex11 driver (window/keyboard/mouse/OpenGL/bitblt), preloader, clipboard, winemenubuilder, MIDI, DNS resolver, wow64 syscall path
**GE game-fixes** — none (plain Proton)

</details>

<details>
<summary><b>Proton 11.0-2</b> &nbsp;·&nbsp; arm64ec + <b>x86_64</b> · Wine 11 · versionCode <code>7</code></summary>

<br>

| | |
|---|---|
| **Base** | Stock Valve **[Proton 11.0-2](https://github.com/ValveSoftware/Proton/releases/tag/proton-11.0-2)** (Wine 11) — plain Proton, no GE game-fixes |
| **Installs as** | `11.0-2-arm64ec-7` · `11.0-2-x86_64-7` |
| **ntdll fix** | `RtlIsEcCode` bounds check (Denuvo unwind loop) — arm64ec build only; the x86_64 build has no EC code path |
| **EA fixes** | ws2_32 dual-stack DNS · nsiproxy default route · gdiplus span clamp (both architectures) |
| **DirectAudio** | v1.3.2 — opt-in via registry `Audio=directaudio`; mic capture opt-in via `BANNER_AUDIO_DIRECT_MIC=1` (both architectures) |
| **XInput fix** | update thread survives transient wait failures (controllers no longer die mid-game) |
| **Wine XP desktop** | Luna taskbar + start menu · XP window frames · `winexp.msstyles` visual style — Blue / Olive Green / Silver, navy / moss / graphite in dark mode |
| **Assets** | **arm64ec:** `proton-11.0-2-arm64ec.wcp` (4 KB + 16 KB pages) &nbsp;·&nbsp; **x86_64:** `proton-11.0-2-x86_64.wcp` (4 KB-page build) |

**Android compatibility fixes** — SD-card boot (`noexec` / `force_anon`) · drive-root copy · `C.UTF-8` locale
**Runtime** — realized-font-handle cap `32768` · `WINEVMEMMAXSIZE` cap · fast-yield gate · FEX-unixlib loader · XRandR / XRender
**Build** — `-g0 -O2` release build, `llvm-strip` on both the PE DLLs/EXEs and the unix `.so` loaders · zstd-compressed `.wcp` · ccache in CI (build speed only, not in the layer)
**Inherited bionic base** — the Winlator-bionic / GameNative Android patch set every layer is built on: esync/fsync, winex11 driver (window/keyboard/mouse/OpenGL/bitblt), preloader, clipboard, winemenubuilder, MIDI, DNS resolver, wow64 syscall path
**GE game-fixes** — none (plain Proton)

> ⚠️ The **x86_64** wcp carries the same fixes, but Proton 11 under box64 currently does **not** render a window — use the **arm64ec** build, which is the proven runtime. The x86_64 asset is published for completeness / testing; it is the 4 KB-page (`sdk28`) build only.

</details>

<details>
<summary><b>Proton 10.0-4</b> &nbsp;·&nbsp; arm64ec · <b>Wine 10</b> · versionCode <code>7</code></summary>

<br>

| | |
|---|---|
| **Base** | Stock Valve **[Proton 10.0-4](https://github.com/ValveSoftware/Proton/releases/tag/proton-10.0-4)** (Wine 10) — plain Proton, no GE game-fixes |
| **Installs as** | `10.0-4-arm64ec-7` |
| **ntdll fix** | `RtlIsEcCode` bounds check (Denuvo unwind loop) |
| **EA fixes** | ws2_32 dual-stack DNS · nsiproxy default route (Wine 10's gdiplus has no span assertion — no clamp needed) |
| **DirectAudio** | v1.3.2 **Wine-10 ABI port** — opt-in via registry `Audio=directaudio`; mic capture opt-in via `BANNER_AUDIO_DIRECT_MIC=1` |
| **XInput fix** | update thread survives transient wait failures (controllers no longer die mid-game) |
| **Wine XP desktop** | Luna taskbar + start menu · XP window frames · `winexp.msstyles` visual style — Blue / Olive Green / Silver, navy / moss / graphite in dark mode |
| **Assets** | `proton-10.0-4-arm64ec.wcp` (4 KB + 16 KB pages) |

**Android compatibility fixes** — SD-card boot (`force_anon`, GameNative's original Wine-10 diff) · drive-root copy · `C.UTF-8` locale (fixes authored against the Wine-10 tree)
**Runtime** — realized-font-handle cap `32768` · `WINEVMEMMAXSIZE` cap · fast-yield gate · FEX-unixlib loader · XRandR / XRender
**Build** — `-g0 -O2` release build, `llvm-strip` on both the PE DLLs/EXEs and the unix `.so` loaders · zstd-compressed `.wcp` · ccache in CI (build speed only, not in the layer)
**Inherited bionic base** — the Winlator-bionic / GameNative Android patch set every layer is built on: esync/fsync, winex11 driver (window/keyboard/mouse/OpenGL/bitblt), preloader, clipboard, winemenubuilder, MIDI, DNS resolver, wow64 syscall path
**GE game-fixes** — none (plain Proton)

> ℹ️ The DirectAudio mainline is Wine-11-only (it drops the `midi_get_driver` unixlib guard); this layer ships the Wine-10-guarded 1.3.2 build so the same feature set works on Wine 10.

</details>

<details>
<summary><b>GE-Proton 10.0-34</b> &nbsp;·&nbsp; arm64ec · <b>Wine 10</b> · versionCode <code>7</code></summary>

<br>

| | |
|---|---|
| **Base** | Valve **[Proton 10.0-4](https://github.com/ValveSoftware/Proton/releases/tag/proton-10.0-4)** (Wine 10) with the **[GE-Proton10-34](https://github.com/GloriousEggroll/proton-ge-custom/releases/tag/GE-Proton10-34)** game-fix tier layered on (GE's game-patch files only, not GE's Wine tree) |
| **Installs as** | `10.0-34-arm64ec-7` |
| **ntdll fix** | `RtlIsEcCode` bounds check (Denuvo unwind loop) |
| **EA fixes** | ws2_32 dual-stack DNS · nsiproxy default route (Wine 10's gdiplus has no span assertion — no clamp needed) |
| **DirectAudio** | v1.3.2 **Wine-10 ABI port** — opt-in via registry `Audio=directaudio`; mic capture opt-in via `BANNER_AUDIO_DIRECT_MIC=1` |
| **XInput fix** | update thread survives transient wait failures (controllers no longer die mid-game) |
| **Wine XP desktop** | Luna taskbar + start menu · XP window frames · `winexp.msstyles` visual style — Blue / Olive Green / Silver, navy / moss / graphite in dark mode |
| **Assets** | `GE-proton-10.0-34-arm64ec.wcp` (4 KB + 16 KB pages) |

**Android compatibility fixes** — SD-card boot (`force_anon`, Wine-10 diff) · drive-root copy · `C.UTF-8` locale
**Runtime** — realized-font-handle cap `32768` · `WINEVMEMMAXSIZE` cap · fast-yield gate · FEX-unixlib loader · XRandR / XRender
**Build** — `-g0 -O2` release build, `llvm-strip` on both the PE DLLs/EXEs and the unix `.so` loaders · zstd-compressed `.wcp` · ccache in CI (build speed only, not in the layer)
**Inherited bionic base** — the Winlator-bionic / GameNative Android patch set every layer is built on: esync/fsync, winex11 driver (window/keyboard/mouse/OpenGL/bitblt), preloader, clipboard, winemenubuilder, MIDI, DNS resolver, wow64 syscall path
**GE game-fixes (10 — superset)**
- **GE-Proton10-34 tier:** `assettocorsa` · `dai_xinput` · `eac` · `pso2` · `silence-starcitizen` · `vgsoh`
- **plus our extras:** `battlenet` · `maplestory-charprev` · `maplestory-stickykeys` · `WM_ACTIVATEAPP`

> ℹ️ GE-Proton10-34's `lemansultimate-gameinput` patch is intentionally **omitted** — our Valve Proton 10.0 base already ships `dlls/gameinput`, so GE's stub is redundant/conflicting.

</details>

## Installation

Each layer is a single `.wcp` — install the one matching your Proton / GE version. There is no longer an `sdk28` / `sdk35` choice: the same file serves 4 KB- and 16 KB-page devices (4 KB boot device-proven; 16 KB boot untested).

**Proton 11.0-2** additionally offers an **x86_64** wcp — but Proton 11 under box64 does not currently render a window, so use the **arm64ec** build unless you are specifically testing x86_64.

This is a **pre-release**, so the app does not offer it yet: import the wcp by hand (container editor → install from file). Once it is installed, containers on the matching `<version>-6` layer are offered it as an **in-place update** (with a revert snapshot first), or create a **fresh container** on the `-7` slot to see the XP desktop exactly as a new user will. Existing v6 (`-6`) containers keep working unchanged. For EA / Denuvo titles pair the layer with FEXCore nightly 2608+45.

**Built from:** `aio-eanet/xp-proton_11.6-GE` `0b60d0d3` · `aio-eanet/xp-proton_11.5-GE` `b821bc10` · `aio-eanet/xp-proton_11.3-GE` `da092720` · `aio-eanet/xp-proton_11.0` `91af073d` · `aio-eanet/xp-proton_11.0-2` `31254bce` · `aio-eanet/xp-proton_10.34-GE` `3417bb5c` · `aio-eanet/xp-proton_10.0` `1e9d197c` (CI runs 34629988483 · 34630031659 · 34630044726 · 34630056346 · 34630060475 · 34630668116 · 34630678482, all green). Each is its v6 parent commit plus the Wine XP desktop series (25 commits; the two Wine 10 layers add one port commit) and the versionCode stamp.

**Not in the in-app catalog yet** — delivery copies for the catalog will be published when this is promoted from pre-release.
