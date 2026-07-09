# Proton 11.0-1 — arm64ec (bionic)

Stock **Valve Proton 11.0-1** (final), recompiled from source against **Android bionic** for **arm64ec** — a drop-in Wine/Proton runtime for Winlator-bionic emulators.

**What it is**
- Base: Valve `proton-11.0-1` (latest 11.0); Wine unix side built with **Android NDK r27d**.
- DLL trees per wcp: `aarch64-windows` (arm64ec) + `i386-windows` + `x86_64-windows`.
- Two flavors: **sdk28** (API 28, 4KB pages) and **sdk35** (API 35 + 16KB-page support).

**Included**
- **esync + fsync + ntsync** (inproc-sync) — modern Wine synchronization.
- **Fast-yield gate** — `WINE_FAST_YIELD=1` (opt-in) fixes the `NtYieldExecution` busy-wait that pins one core at 100%.
- **FEX unixlib loader** (`MemoryWineLoadUnixLibByName`) — lets a unixlib-aware FEXCore load its native `.so` companion. **FEX-version-agnostic**: works with any FEXCore; dormant unless a matched `-unix` FEX is installed.
- winedmo media path (FMV via ffmpeg-8 when present in the imagefs).

**Compatible with**
- Apps: **Bannerlator, WinNative**, and other Winlator-bionic / Cmod-lineage emulators (bionic ABI, `/system/bin/linker64`).
- Translators: **FEXCore** (any version) or **wowbox64** for x86/x86_64.
- Graphics: **DXVK / VKD3D-Proton** → Vulkan (Turnip/Adreno).
- ⚠️ arm64ec requires a **fresh container**.

**What it does** — runs Windows x86/x86_64 games on Android arm64ec via Wine + FEX/wowbox64, D3D → DXVK/VKD3D → Vulkan.

**Which file** — newer device / Android 15+ / 16KB pages → **sdk35**; otherwise → **sdk28**.

**Notes** — this is **stock Valve, not GE-Proton** (no GE game-fixes / FSR / OptiScaler tier). The FEX unixlib loader is build-ahead: games run identically today (FEX DLLs still self-contained); it future-proofs you for when FEX ships thin DLLs.

## Using the FEX unixlib (optional)
This Proton includes the FEX **unixlib loader** — it loads FEX's native `.so` companion automatically, with no app change. To actually use it, install a matching **FEX `-unix`** component:

➜ **Get it from the Nightlies:** https://github.com/The412Banner/Nightlies/releases — grab **`FEX-*-unix.wcp`** (or **`FEX-*-PPA-unix.wcp`** for the DWARF/PPA-flavor DLLs).

⚠️ Use the **The412Banner/Nightlies** `-unix` builds specifically — they carry the `shm_open`/`shm_unlink` fix required to load on Android bionic. Other/older FEX `-unix` builds leave those symbols unresolved, so the `.so` fails to `dlopen` and silently falls back to the DLL (the unixlib never engages). Games run fine on any FEX regardless — the unixlib is build-ahead for when FEX makes it mandatory.
