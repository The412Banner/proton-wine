# proton-wine PROGRESS_LOG

## 2026-09-01 — Proton 10.0-2c arm64ec AIO refresh (branch `p10-2c-aio-refresh`)

Refresh the July-10 Proton **10.0-2c** arm64ec layer (controllerfix base,
branch `p10-2c` @ `276f94fc`) to full feature parity with the 10.0-4 AIO layer
(`proton_10.0` @ `9be51275`), as a single modern **true-sdk28 + 16KB-page**
wcp. Keeps the 10.0-2c Wine source + controllerfix; replays the AIO feature
stack on top. **ARM only; x86_64 deferred.**

### Key finding
`p10-2c` and `proton_10.0` share **no common git ancestor** (disjoint
histories; `proton_10.0` root = `147d88ee0c4`). Their tooling is the same
GameNative lineage but the underlying Wine source trees differ (patches were
regenerated against different bases). So features were replayed **surgically**
onto the 2c source, not by rebase/wholesale-swap.

### What landed (commits on `p10-2c-aio-refresh`)
1. `ntdll/win32u: replay AIO in-tree features` — WINE_FAST_YIELD (sync.c),
   WINEVMEMMAXSIZE (virtual.c), MAX_FONT_HANDLES 256->32768 (font.c).
2. `android/build: replay AIO 10.0-4 build tooling` — build-step-arm64ec.sh
   from AIO (true-sdk28+16KB decouple: TARGET stays android28 +
   `-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES` + `-Wl,-z,max-page-size=16384`;
   XRandR/XRender; `-g0 -O2` + llvm-strip; ccache; `set -eo pipefail` +
   `|| exit`; hard post-apply verification), + arihany patches (shell32
   drive-root guard, C.UTF-8 env, split 2c-anchored force_anon virtual.c),
   + configure.ac DirectAudio/AAudio hunks, build-step0.sh fail-hard.
3. `directaudio: add winedirectaudio.drv submodule` pinned `6f95a98e5e1e`
   (v1.3.2, Wine-10 ABI — the exact commit 9be51275 pins).
4. `ci: single 16KB/true-sdk28 build-proton.yml` — cloned from 9be51275,
   retargeted (name, push branch `p10-2c-aio-refresh`, wcp
   `proton-10.0-2c-arm64ec.wcp`, verName `10.0-2c`, Proton versionCode 1,
   ccache `-p10-2c-`, release job kept `if: false`); split sdk28/sdk35
   workflows removed.

### Reconciliations / deviations (documented)
- **FEX load-by-name: nothing to reconcile.** 2c's baked loader is already the
  identical load-by-name loader (`android/patches/.../dlls_ntdll_unix_loader_c.patch`
  byte-identical to AIO). One coherent loader; no double-apply.
- **winepulse patch stays DROPPED** (2c source override): AIO re-enabled it but
  2c's pulse.c predates the period-timer rewrite it anchors to.
- **force_anon split into its own 2c-anchored patch**: 2c's `virtual_map_image`
  differs from the 10.0-4 tree AIO's combined patch was authored against.
- **configure.ac: only the DirectAudio/AAudio hunks ported** (AIO's Vosk +
  gameinput/amdxc64/vccorlib140/windows.storage makefiles skipped — those dirs
  don't exist in 2c).
- **build-proton.yml is aarch64/sdk28-only** (proton_10.0 HEAD already dropped
  x86_64/sdk35); task's "matrix + x86_64 name" parenthetical described the
  earlier `d4fb7fee405` state. x86_64 build-step left as 2c (not exercised).

### Local pre-flight (not a build; `git apply` only)
All 69 active arm64ec patches apply sequentially onto the 2c source; the four
build-step verification tokens land (`force_anon`, `dir_len`, `"C.UTF-8"`,
`BANNER_AUDIO_DIRECT_RUNTIME`); font-cap 32768 + load-by-name present.

### STOP gate
After CI-green: verify arm64ec `.so` LOAD align 0x4000 + `.note.android.ident`
SDK=28 + single wcp + DA v1.3.2 marker + font-cap. **No release, no
make_latest, no catalog repoint** (gated on a real 16KB-device boot-proof).
