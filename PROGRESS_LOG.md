# proton-wine PROGRESS_LOG (branch `proton_11.0-2` — Wayland v8 base)

Newest entry at the top.

## 2026-11-24 07:30 EST — Proton 11.0-3 line (staging/proton_11.0-3/v17, CI-green, staged, NOT device-tested)
- **Why (user, 2026-11-24 ~05:45 EST):** "I need a new branch made and proton build for the latest 11-3 proton release" (Valve Proton 11.0-3, released 2026-11-24 01:22 EST). New line carrying everything the 11.0-2 v17 line has. No release, no tag, no install.
- **Base:** Valve Proton 11.0-3 wine submodule = ValveSoftware/wine 957ac7cfa4f5 (checked via the proton-11.0-3 tag's `wine` gitlink; 11.0-2 = dc26e618, an ancestor: 226 Valve commits, incl. 11.0-2e e60892cc).
- **Parent `proton_11.0-3` = 6af7d353:** origin/proton_11.0-2 a190d333 + merge of 957ac7cf. Conflicts (all kept both sides):
  - `dlls/ntdll/unix/signal_arm64.c` — Valve moved SP/PC writes into the new KiUserEmulationDispatcher if/else (restore_context, usr2_handler) and added RESTORE_FLAGS_EMULATION in NtSetContextThread; our Wine MR 12262 CPSR masks (8d45bbe9) kept on top; Valve's new save_syscall_entry_frame() gets `& cpsr_user_mask` as in upstream Wine master.
  - `dlls/rsaenh/rsaenh.c` — init_hash() CALG_HMAC: took Valve's upstream version (Nello De Gregoris, Wine-Bug 60345), which includes our 6be60001 NULL-descriptor check; our HP_HMAC_INFO return check stays.
  - `dlls/win32u/vulkan.c` — vkDestroyDevice: Valve's signaller-thread / fence-op cleanup first, then our order (remove handle-table entries, then destroy the host device; 87536120/51a96c46).
  - Auto-merged files touched by both sides: our delta re-checked line-for-line identical in all 20 (except the two intended changes above).
- **Staging `staging/proton_11.0-3/v17`:** parent + the 20 v17 commits cherry-picked from a190d333..67b031f2 (no conflicts; android/ build-scripts/ .github/ identical to 67b031f2) + f2266edc patch refresh + a7a295b0 workflow.
- **Patch refresh (f2266edc):** Valve 11.0-3 carries upstream Wine unixlib load-by-name (Julliard 5ffc0a61/083a4694/bbe2f325/39a9fa26) → our backport patches unix_private.h / wow64 virtual.c / unixlib.h / winternl.h removed; loader.c + virtual.c patches no longer add their own copies (loader.c applied "cleanly" but made a second load_unixlib_by_name: caught by a duplicate-definition scan, not by git apply); our WARN traces kept in Valve's function. signal_x86_64.c "jmp *%rcx" patch removed (Valve f545b276314). Context refreshes: sync.c (fsync line after Valve 957ac7cf), ntsync_userspace, alert_fd_close (fsync hunk dropped, Valve has it), winepulse (GN guard on Valve's new period code, + free(period->device)), offscreen check cache (TRACE moved by 820f4a1353a).
- **Checked locally (FIT):** CI-order patch sim (server_protocol.def.patch + make_requests + make_specfiles, then PATCHES, then MARKERS) on f2266edc: arm64ec 74/74 applied, 55/56 markers + DirectAudio marker verified in the pinned submodule 21010855 (sim tree has no submodules); x86_64 65/65, 39/40 + same. Duplicate scan vs 11.0-2 baseline: no new duplicates. Native aarch64 compile (gcc 15 / clang 21, configure + make on the patched arm64ec tree): all 171 objects of ntdll (unix + arm64ec PE), wineserver, win32u, winex11, ws2_32 + rsaenh arm64ec build; only host-config misses (no EGL → win32u/opengl.c, x86_64 mingw-gcc asm → ntdll signal_x86_64 arm64ec-x64, both build with EGL / clang).
- **Workflow (a7a295b0):** build-proton-11.0-2.yml → build-proton-11.0-3.yml: push triggers only proton_11.0-3 + staging/proton_11.0-3/**, no pull_request; versionName 11.0-3-<arch>, versionCode 17; description = Valve Proton 11.0-3 base (wine 957ac7cfa4f5) + v10–v17 list; release job if: false. Parent push (6af7d353 carries only the 11.0-2 workflow, filters don't match) started no run (checked).
- **CI:** push run **45388353315** (06:53–07:18 EST), headSha a7a295b0 == pushed. Jobs: build (aarch64, 28) success, build (x86_64, 28) success, release skipped (if: false). Logs read: arm64ec 74/74 "SUCCESS … applied", 56/56 source markers ok (incl. DirectAudio, both rpcrt4 import checks), verify-layer PASS; x86_64 65/65, 40/40, verify-layer PASS. Skipped steps are the arch-only ones (sysvshm, server-browser front, Wayland upload on x86_64; media registrations on aarch64). wcp profile: Proton 11.0-3-arm64ec / 11.0-3-x86_64, versionCode 17.
- **Staged (host == /sdcard sha256, rows in /root/src/dl/v17-layers/FILES.tsv):** /sdcard/Download/v17-layers/proton-11.0-3-arm64ec-v17-a7a295b0.wcp (117387295 B, sha256 25b83d11…78fd81f) and proton-11.0-3-x86_64-v17-a7a295b0.wcp (66054755 B, 3b7ffdd6…889040d7); host copies in /root/src/dl/v17-layers/.
- **Device test:** NOT device-tested. Risks to test: Valve's 9 ARM64EC dispatch commits (Jacek Caban: KiUserEmulationDispatcher / InSimulation timing, syscall dispatcher) with our FEX builds + GameNative #67/#133 patches; .NET apps (a7415c0da0a runs AnyCPU .NET as ARM64EC on ARM64, 97ac0f670af/ec3e6862737 mono arch selection); winepulse period rework; unixlib by-name (FEX companion) now upstream code.
- **Next:** user tests the two wcp; then decide merge/release. Resume: worktree /root/src/proton-wine-wt-p113 (staging branch), sims /root/src/proton-wine-wt-p113-sim-{arm64ec,x86_64}, baseline /root/src/proton-wine-wt-p113-base112 (67b031f2).

## 2026-11-23 — v17 layers round: 11.0-2 + P10.0-4 + GE10.0-34 + DroidDeck 11.0-12.8 (staging/<parent>/v17, building; NO release)
- **Why (user, 2026-11-23 ~23:20 EST):** build v17 layers overnight and stage them in /sdcard/Download/v17-layers/ for the morning tests. No release, no catalog change, no install. A separate session ports the new GE-Proton 11-14 line.
- **Branches (new, off the v16 commits):** `staging/proton_11.0-2/v17` off a190d333 (= release 2c0670d0 + 6 log commits, checked: only PROGRESS_LOG.md differs), `staging/proton_10.0/v17` off cc815410, `staging/proton_10.34-GE/v17` off b76055cc, `staging/proton_11.12.8-DroidDeck/v17` off d48e30a1 (fix-unwind-loop).
- **11.0-2 commits (one per item):** cherry-picks of d4c9ffa7 (ARM64EC code map 47-bit) + fecf7376 / a188791d (unwind stops) + MARKERS; DiRT telemetry patch (from the DD 12.x layers); offscreen check cache (hand-ported onto this init.c: TRACE line after the other_process check); WMA 8956410a; GameNative #119 (both files unchanged), #128 (separate patch after ntsync_userspace.patch), #133 with #131, #67 ntdll half (sechost part in-tree since v16), DroidDeck-Proton #34; Azure apt mirror pin (d48e30a1); versionCode 17.
- **Checked locally:** patch step simulated (tools/make_requests + make_specfiles as autogen does, PATCHES, MARKERS, GE tier where present) PASS on both arches; #119's generated protocol hunks equal make_requests output.
- **Skipped (user):** GameNative #122, #142, #102, ntsync-android v0.2.0, #124, #126, #135, #129, #118, #85, #86, #101, open PRs.
- **CI / staged files / device test:** pending; NOT device-tested.

## 2026-10-30 — DroidDeck-Proton 11.0-11.9 added to v16 (parent branch `proton_11.11.9-DroidDeck` @8e2653cb, no own release)
- **Why (user):** "build the new proton layer and make its own parent branch but don't create a release for it and add it as v16 to the v16 All in One release and update that release description to reflect it" + "add all of its new features fixes or patches since it is technically an update over DroidDeck 11-11.1". Asked about the in-app list → user chose **"Also add to the app list"** (mirror + catalog rows).
- **Branch:** `proton_11.11.9-DroidDeck` created at `8e2653cb` (the device-tested commit; new ref, nothing rewritten). Push run **41119590016** (headSha 8e2653cb): aarch64 + x86_64 success, release job skipped (`if: false`). Logs read: arm64ec 65/65 patches, x86_64 63/63, 0 failed, all 11-11.9 / Valve / GE / winewayland markers ok, both rpcrt4 import checks ok, verify-layer PASS ×2; only warning = Node 20 deprecation.
- **Assets:** temp branch `tmp/release-v16-dd119` (orphan, workflow only) run **41124121578** checked run/branch/head/conclusion, names and profile.json (`11.0-11.9-arm64ec` / `11.0-11.9-x86_64`, versionCode 16), refused existing names (no `--clobber`), attached `DroidDeck-proton-11.0-11.9-arm64ec.wcp` (117808904 B, sha256 `ab6f7fa3…7fc31731`) and `DroidDeck-proton-11.0-11.9-x86_64.wcp` (63948427 B, `2b65e648…dbc8ef2d`) to `build-bionic-layers-20261030-v16` → 10 assets, the 8 old ones unchanged. Tag still `2c0670d0`, still Latest, not draft.
- **Same as tested?** Rebuild of the same commit, so not byte-identical. Read-only compare runs 41124360255 + **41124560888** vs the device-tested run 41101080463 (wcp `973d84be…` / `7a977cb0…`): arm64ec 999 files identical, 1517 PE files differ only in TimeDateStamp/CheckSum/debug-dir stamps, 52 same-size `.a` link libraries (ar timestamps), prefixPack.txz 411/412 identical + 1 `.reg` differing only in key write times; x86_64 674 identical, 1510 PE stamps only, 54 `.a`, prefixPack 294/295 + 1 `.reg` times only. All unix `.so` identical. → equivalent to the tested build.
- **Notes:** title → "… · DroidDeck-Proton 11.0-11.9 · 6 layers · arm64ec + x86_64"; new section "DroidDeck-Proton 11.0-11.9 — added to v16" (every 11-11.2…11-11.9 Wine fix with PR + author, Valve 674d3cdd ×9, GE CHOICE /T + B8G8R8A8_UNORM, WineHQ ef6eccbd + 039ad9fe, not-included FEX-D2/FEX_DEBUGLOG), new layer block, DiRT Showdown device test, credits, branch list. Old notes backed up at /root/src/dl/v16/notes-backup-20261030-before-dd119.md (device).
- **Delivery (winlator-contents):** `tmp/mirror-v16-dd119` run 41124748490 copied both files into `bionic-layers-20261030-v16` (sha256 -c OK, 10 assets), mirror notes/title updated; catalog `contents.json` **1bbd65a8**: 2 rows `DroidDeck-Proton-11.0-11.9-{arm64ec,x86_64} (v16)` (202 → 204), URLs answer. Bannerlator's ContainerLayerUpdater counts DroidDeck-Proton layers of one arch as one line → containers on 11.0-11.1 are offered 11.0-11.9.
- **Proof level:** arm64ec device-tested (DiRT Showdown, esync + userspace ntsync, Direct XInput); x86_64 NOT device-tested.
- **Temp branches:** user "yes delete the temp branches" → backed up as `refs/backup/20261030/tmp-release-v16-dd119` (ec7afabc, proton-wine) and `refs/backup/20261030/tmp-mirror-v16-dd119` (200a3da1, winlator-contents), then both branches deleted; no `tmp/` branches left; both releases still 10 assets.
- **Open:** x86_64 device test. FEX-D2 FEXCore component = follow-up.

## 2026-10-30 — DroidDeck-Proton 11.0-11.9 picks build `8e2653cb` on device: DiRT Showdown OK (`staging/proton_11.11.9-DroidDeck/v16`, CI-green, partly device-tested)
- **Built:** on top of f3ea51e5: `c745ab46` merge Valve bleeding-edge 674d3cdd (9 commits), `c51f2d3a` GE cmd CHOICE /T console timeout (GloriousEggroll), `29f811d6` winewayland dummy buffer leak (Brian Carbone, ef6eccbd), `bb46077e` clipboard class name (Nikolay Sivov, 039ad9fe), `60213f0b` B8G8R8A8_UNORM reported first (GloriousEggroll, hand port), `8e2653cb` MARKERS ×7. versionName 11.0-11.9, versionCode 16 (user's choice).
- **CI:** push run 41101080463 (headSha 8e2653cb) aarch64 + x86_64 green; per-job logs read (`gh run view --log` gave 0 bytes): 65/63 patches, all 7 new markers ok, verify-layer PASS.
- **Device:** arm64ec layer root-swapped in place 14:27 EDT at the user's request (same name/versionCode, so Bannerlator would refuse a normal install): all 2569 files == build, app owner, modes and SELinux labels as installed; old f3ea51e5 layer kept in `files/bl-layer-backup/`. User ran DiRT Showdown twice on Bannerlator 4119eb6b, container 1, Direct XInput on: 15:37 esync (prefix refreshed from the new wine.inf), 15:39 ntsync (userspace: the kernel ntsync module has been disabled by its own boot guard since 10-24). Both normal exits, no crash or tombstone, same Wine `err:` set as the earlier build, DXVK no errors, controller log 6 pass / 0 fail, rumble ok. User: "launched fine", "controls were fine".
- **Note:** Bannerlator byte-patches `lib/wine/aarch64-unix/winebus.so` on every launch (WinebusRumblePatcher, TideGear #91: 2 sites, 8 bytes). A layer hash check after a launch must skip that file.
- **Proof level:** DiRT Showdown device-proven (esync + userspace ntsync, Direct XInput). NOT tested: Wine's own xinput path with Valve's rescan change + pad hotplug, kernel ntsync, other games, the x86_64 layer.
- **Open:** no parent branch `proton_11.11.9-DroidDeck` and no release until the user says so. FEX-D2 Blizzard patches = follow-up (FEXCore component). Container 2 is gone (user deleted it), so its stale-layer item is closed.

## 2026-10-30 — DroidDeck-Proton 11.0-11.9 layer (`staging/proton_11.11.9-DroidDeck/v16`, CI-green, user testing on device)
- **Why:** user: the 11-11.9 DroidDeck-Proton fixes in a Bannerlator layer, keeping every fix of the 11.0-11.1 layer.
- **Built:** off `proton_11.11.1-DroidDeck` v16 (068807e2). `bcde54f5`: android/patches from DroidDeck-Proton f7d00335 (shared-storage owner #4, i386 lost wake #5, Wine files not read-only #6, shared storage case-insensitive #7, maxjivi05; WoW Forever ARM64EC ntdll #3, arusiasotto, arm64ec list only: its start_thread hunk calls an `__aarch64__`-only function) + winex11 offscreen-check cache (ff9db6f6 + bab12909, hand port, off unless WINE_OFFSCREEN_CHECK_MS); MARKERS + verify-layer. `f3ea51e5`: workflow `build-droiddeck-proton-11.0-11.9.yml`, versionName 11.0-11.9, versionCode 16 (user choice).
- **CI:** push run 41090740654 (headSha f3ea51e5) aarch64 + x86_64 green, logs read: 65/63 patches, markers ok, GE ok, verify-layer PASS.
- **Device:** wcp staged /sdcard/Download/DroidDeck-Proton-11.0-11.9-{arm64ec,x86_64}-v16-f3ea51e5.wcp (sha256 90d19b9e… / 57c5588c…); user installed arm64ec 13:25 EDT, all 2569 files == wcp; container 1 on it. User tests it themselves. NOT device-proven yet.
- **Not here:** FEX-D2 Blizzard patches (follow-up, FEXCore component). User removed 11.0-2-arm64ec-15 from the device (container 2 still names it).
- **Next (user picked):** Valve bleeding-edge 674d3cdd merge (xinput rescan, winebus report length, critical-section, Radeon version), GE cmd CHOICE timeout, GE B8G8R8A8_UNORM-first (hand port), upstream winewayland dummy-buffer leak + clipboard class name. User: check the controller changes work with Bannerlator's input system.

## 2026-10-30 — v16 RELEASED — five layers (`build-bionic-layers-20261030-v16`, Latest + catalog default)
- **Why:** DOOM (2016) (GOG) crashed at start on v15: `Galaxy64.dll` → `sechost` → `rpcrt4` access violation (writing 0x10); the EA app's `EABackgroundService` + MSI 0x8007065b failed the same way. Cause: Valve's "HACK: Avoid delay importing rpcrt4" (be454c4d1d60, Billy Laws) was reverted upstream (8fb9c7f7154b, Jacek Caban, Valve now patches LLVM, CW #27308); our llvm-mingw lacks that LLVM patch → delay-loaded variadic `NdrClientCall` loses its stack arguments on arm64ec. Proton 10 delay-loaded rpcrt4 without the hack.
- **Fix:** re-apply the hack (Proton 11 layers) / same Makefile change (Proton 10): `rpcrt4` from DELAYIMPORTS to IMPORTS in sechost + advapi32; MARKERS check in build-step-arm64ec.sh (fatal on miss); versionCode 16. Branches `staging/<parent>/rpcrt4-exit-thunks` fast-forwarded on the user's go (backups `refs/backup/20261030/<parent>-before-rpcrt4-exit-thunks`): proton_11.11.1-DroidDeck 068807e2 · proton_11.0-2 2c0670d0 · proton_11.9-GE a01a7c58 · proton_10.0 cc815410 · proton_10.34-GE b76055cc. proton_11.0 (11.0-1) not affected, stays v15.
- **CI:** push runs 41081173822 / 41081176670 / 41081180748 / 41081184459 / 41081187566 all green (release jobs `if: false`); both "ok … imports rpcrt4 directly" lines in every arm64ec log.
- **Release (all on GitHub's side, user on slow Wi-Fi):** draft 429198399 → temp branch `tmp/release-v16` (orphan commit, workflow only) run 41087511008 pulled the `proton-*-sdk28` artifacts, renamed DroidDeck-Proton-* → DroidDeck-proton-*, attached 8 wcp; job sha256 == GitHub asset digests (all 8). Published 2026-10-30, tag == proton_11.0-2 2c0670d0, non-prerelease, Latest; notes in the v15 layout (per-layer details, Credits). Mirror winlator-contents `bionic-layers-20261030-v16` via temp branch `tmp/mirror-v16` run 41087915491 (target main acc3fe6a, not latest, 8 files == proton-wine digests). Catalog winlator-contents 93c6d0d8: 8 rows → v16 (11.0-1 row stays v15); all 8 URLs 200.
- **Device:** DroidDeck-Proton 11.0-11.1 arm64ec — DOOM (2016) GOG to menus 60 fps via DOOMx64vk.exe and DOOMx64.exe hand-off (Pocket FIT, Bannerlator zero-copy off). Other layers / x86_64 not device-tested.
- **Open:** temp branches tmp/release-v16 (proton-wine) and tmp/mirror-v16 (winlator-contents) — delete on the user's word.

## 2026-10-24 — CJK locale fix merged into six layer branches (fix/cjk-locale/*, MERGED, NOT device-tested)
- Why: GBK games got code page 1252 on our layers: bionic setlocale("") answers "C.UTF-8" and unix_to_win_locale() fell back to LC_ALL only for exactly "C".
- Fix (dlls/ntdll/unix/env.c): "C.*" treated like "C", then LC_ALL / LANG. Fast-forwarded on the user's go: proton_10.0 9877181615, proton_10.34-GE 0939168e7d, proton_11.0 dcc517e34e, proton_11.0-2 7b4dd1dfdb, proton_11.9-GE 6463949d61, proton_11.11.1-DroidDeck 4f45e18387. Backups refs/backup/20261024/<branch>-before-cjk-locale.
- CI: branch runs 40130106661…40130115307 all green (verify-layer PASS); push runs 40158648780…40158658587 started (release jobs if: false). No release; versionCode 15 kept.
- Open: CJK game test on a rebuilt layer; the Bannerlator app fix (main defb2b9f) covers old layers too.

## 2026-10-23: v15 RELEASED — six layers (`build-bionic-layers-20261023-v15`, Latest + catalog default)

- **Why:** on DroidDeck-Proton 11.0-9.1 the Bannerlator drawer showed FEX "N/A" and Xbox sign-in failed with
  `REGDB_E_CLASSNOTREG` (PropertySet only under `Wow6432Node`). User decisions 10-23: six layers (Proton 10.0-4,
  GE 10.0-34, 11.0-1, 11.0-2, GE 11.0-9, new DroidDeck-Proton 11.0-11.1 replacing 11.0-9.1); GE 11.0-3/5/6/7/7.1 and
  both CachyOS dropped (stay v14); P10 PropertySet backport skipped; GameNative #85/#86/#82 parts 3-4 held.
- **Built (staging/<parent>/v15, then parents fast-forwarded; v14 tips at `refs/backup/20261023/<parent>-before-v15`):**
  - FEX unixlib name fix re-ported onto Valve's `load_unixlib_by_name` (extension set one byte early → `libarm64ecfe.so`):
    GE 11.0-9 + DroidDeck 11.0-11.1; MARKERS source check.
  - `android/prefixpack-winrt-registry.py` + workflow step: classes from the layer's own WINE_REGISTRY scripts added to
    both prefix views (arm64ec +18/+18, 11.0-1 +17, x86_64 +30, Wine 10 +6; check uses PropertySet, Buffer on Wine 10).
  - GameNative patch files with PR # + author in the build steps: #58 #69 #73 #94 #46 #82(OPWR) #66 #42 (+#84 on
    11.0/11.0-2); Wine 10: #94 #46 #42 + re-ports of #58 #73.
  - DroidDeck 11.0-11.1 (`proton_11.11.1-DroidDeck`, new): GE 11.0-9 v15 + merge Valve `c53678ab22a8` (GE-Proton11-10/11-11
    base) + 17 CachyOS game fixes from 11.0-9.1 + CachyOS 20261015 setupapi/crypt32 + GE SRW lock (Sebastian Matkovich)
    + RtlWaitOnAddress alert (Jason Eggleston). Not taken: MF 0093/0094, EM/OpenXR, ACE x7, PS lsteamclient fallback.
    Every 11.0-9.1 change verified present (reverse-apply), patch lists a superset.
  - versionCode 15 everywhere.
- **CI (parents):** DD 40025401541 · 10.0 40025487521 · 10.34-GE 40025537577 · 11.0 40025585461 · 11.0-2 40025641710 ·
  11.9-GE 40025697141 — all green, headSha = branch tips (12edf94e, de3c3b7d, 63e0ca0f, 7ca691b0, 4fdfbe70, 0af00e72).
- **Release:** tag on 11.0-2 `4fdfbe70`, 9 wcp (remote sha256 == local); winlator-contents mirror
  `bionic-layers-20261023-v15` + `contents.json` `a727c40` (9 rows → v15, DD 9.1 rows → 11.0-11.1). Notes in the v12
  all-in-one layout (per-layer `<details>`, Credits).
- **Device-proven (Pocket FIT):** DroidDeck 11.0-11.1 arm64ec only — drawer unixlib, BALL x PIT, in-place update from
  11.0-9.1 (Bannerlator `301facfe`) kept saves, DiRT Showdown 143.7 fps (needs Bannerlator `ea07e522` EmulateModeset=N),
  95/95 WinRT classes both views, no FATAL. **Not device-tested:** GE 11.0-9, 11.0-2, 11.0-1, both Proton 10, all x86_64.
- **Open:** device tests above; Proton 10 still has no PropertySet; catalog `proton-10.0-arm64ec` entry left alone (user).

## 2026-09-30: v9 RELEASED — all ten layers (`build-bionic-layers-20260930-v9`, Latest + catalog default)

- **Every Wine 11 layer** (11.0-1, 11.0-2, GE 11.0-3/-5/-6/-7/-7.1, CachyOS): winewayland **touch** (`wl_touch` →
  `WM_POINTER*`, lparam normalised to 0..65535 over the virtual screen — first version sent pixels; CachyOS keeps its
  upstream touch), **rsaenh** fixes (bl4ckh4ck5 HMAC `b4fc579` as is; CPVerifySignature key pin rewritten: ref taken
  under the handle-table lock, `release_object_ref` never frees the slot), **opt-in userspace ntsync** (`WINENTSYNC=1`;
  Joshua Tam / GameNative `ntsync-android` @ `7ce6435`, one `ntsync_userspace.patch`; unset = exact old esync path),
  **dormant lsteamclient** (Valve 11.0-2 SDK 165 + our bionic fixes + GN `owned_dlcs`; redirect only with
  `WINE_LSTEAMCLIENT=1`; arm64ec only, no `steam.exe`). versionCode 9.
- **Proton 10.0-4 / GE 10.0-34:** v8 Wayland + HDR10 EDID backported to Wine 10 (GDI driver 102 → 103,
  `pClipClientSurfaces` hook), touch, rsaenh (HMAC adapted to BCrypt handles), desktop no longer locks the pointer in
  virtual-desktop mode, noexec shared-section retry without `PROT_EXEC` (Insane 2). No ntsync / lsteamclient.
- Release published server-side (`publish/v9` 8709ca5694a: 14 wcps pulled from their CI runs, sha256-gated against the
  device-tested files); winlator-contents copy + `contents.json` `c086862` (14 rows); parents fast-forwarded, v8 tips at
  `refs/backup/20260930/<parent>-v8`.
- Device-proven (Pocket FIT): 11.0-2 DiRT Showdown esync + ntsync live, CS:S SteamLite/Goldberg + Raw Brawlhalla with the
  bridge dormant; P10-4 Wayland + X11 and GE 10.0-34 Wayland (cursor, Insane 2); other layers launched by the user.
  Touch inside a Windows game not yet tested.

## 2026-09-17: x86_64 leg restored on proton_11.0-2 — the v8 set is complete (`d00527c9054`)

V7 shipped three x86_64 layer rows. Two of them (`GE-Proton-11.0-7-x86_64`, `GE-Proton-11.0-7.1-x86_64`)
rebuilt at vc 8 as part of the six-layer port, because those workflows keep both legs. **`Proton-11.0-2-x86_64`
had no v8 counterpart** — the Wayland work had narrowed its matrix to `arch: [aarch64]`. Restored:

- `arch: [aarch64, x86_64]` at `api_level: [28]` (what v7 shipped). Every x86_64 build-step branch was
  still present in the workflow; only the matrix had been cut.
- **Wayland upload guarded** `if: matrix.arch == 'aarch64'`. Its `if-no-files-found: error` would
  otherwise fail the box64 job on an empty `winewayland-files`. Confirmed in the log: the step ran on
  the aarch64 job only.
- **Per-arch `${DESC}`.** The single shared description would have told x86_64 users the box64 layer had
  Wayland and HDR10. The x86_64 leg now reads "(bionic, box64)", drops the arm64ec-only FEX-unixlib
  loader, and makes no Wayland claim; the arm64ec text is unchanged.

CI **35262398189** success on both legs, `verify-layer: PASS` on each.
`proton-11.0-2-arm64ec.wcp` 117,540,380 B vc 8 (2599 entries, 3/3 winewayland) ·
`proton-11.0-2-x86_64.wcp` 63,758,490 B vc 8 (2248 entries, 0 winewayland — correct).

### Control Panel fix on the x86_64 legs — verified, not assumed
The fix is `xp_add_control_panel()` in `programs/explorer/startmenu.c`, which came in with the v8
payload, so it is in all nine v8 trees and compiles for both legs. Verified in the built binaries: the
marker is the UTF-16 string `L"control.exe"`, and `lib/wine/x86_64-windows/explorer.exe` carries it with
the same signature as the known-good arm64ec build. Both `control.exe` and `explorer.exe` ship in the
x86_64 and i386 PE directories of every x86_64 wcp.

### Complete v8 set staged on the device (12 wcps, `/sdcard/Download/`)
arm64ec: `11.0-1` `c7618ce7` · `11.0-2` `df6cc696` · `GE 11.0-3` `8e7008b4` · `GE 11.0-5` `f32fdfe9` ·
`GE 11.0-6` `c7cf8513` · `GE 11.0-7` `f98a443d` · `GE 11.0-7.1` `638bd31c` · `CachyOS` `de9bcf78`
x86_64: `11.0-2` `c7379ead` · `GE 11.0-7` `9e22e70b` · `GE 11.0-7.1` `48dcfa74` · `CachyOS` `ade66e65`

## 2026-09-17: CachyOS ported to v8 (`proton_11.0-cachyos` @ `4fadabecc77`)

Same canonical payload as the six 11.x layers (366 files). CachyOS is Wine 11.0 at
**`WINE_GDI_DRIVER_VERSION` 109 — identical to the v8 base** — and shares a merge-base
(`79604fd6795b`), so the Wayland driver applies. Its `winewayland.drv` is ahead of the base on its own
upstream work, so five files needed a real merge rather than a clean apply:

- `Makefile.in`, `waylanddrv.h`, `wayland.c` — additive on both sides. CachyOS carries
  `alpha-modifier-v1`, `color-management-v1` and `content-type-v1`; v8 adds `banner-desktop-v1`. Kept all.
- `display.c` — `wayland_add_device_monitor` uses **both** CachyOS's `DXVK_HDR`/`DXVK_NO_HDR` env
  override and v8's HDR10 EDID handoff. Kept both; v8's assignment stays last, matching the six.
  Consequence: a real handoff overrides an explicit env setting. Same behaviour as the six layers.
- `window.c` — the one with a compile trap. CachyOS's `wayland_win_data_create_wayland_surface` takes
  **three** arguments (`data, toplevel_surface, owner_surface`); v8's code calls it with **two**. Kept
  CachyOS's newer toplevel/owner model and its 3-arg signature, folded in v8's desktop-surface early
  return, the `!desktop_mode` guard and the z-order reporting, and updated both v8 call sites
  (including `(data, NULL)` -> `(data, NULL, NULL)`). A naive merge would not have compiled.

Identity: `versionCode` 1 -> 8 on both profiles; `versionName 11.0-20260703-${ARCH_NAME}` keeps its own
slot. x86_64 (box64) leg retained, Wayland upload guarded `if: matrix.arch == 'aarch64'`.

CI **35258322497** success on both legs, `verify-layer: PASS` on each. arm64ec wcp 117,669,994 B,
sha256 `de9bcf78…8c16` (device copy matches CI exactly), 2600 entries, 3/3 winewayland files, 8 Wayland
Turnips. x86_64 wcp 63,879,915 B vc 8, not staged.

### Proton 10 — parked by the user
`proton_10.0` (10.0-4) and `proton_10.34-GE` (10.0-34) are Wine 10.0 at driver version **102 vs 109**,
with **no merge-base** against the 11.x line. Divergence on the v8 code files: `win32u/vulkan.c`
+1120/-3676, `sysparams.c` +269/-948, `winewayland.drv` +1720/-4485 over 24 files. That is a backport
against an older driver interface, not a port. Deferred on the user's instruction.

## 2026-09-17: Wayland v8 ported to the six remaining 11.x layers (staging/<layer>/v8-wayland)

The v8 base (`proton_11.0-2` @ `ac1fe84df3d`) was replayed onto all six other 11.x layer lines.
Each port is a single commit on `staging/<layer>/v8-wayland`, a clean +1 fast-forward over its parent.

| layer | staging tip | CI run | wcp identity |
| --- | --- | --- | --- |
| `proton_11.0` | `e5fa703ed7f` | 35248712888 | `11.0-1-arm64ec` vc 8 |
| `proton_11.3-GE` | `d46dfe15c4b` | 35248917771 | `11.0-3-arm64ec` vc 8 |
| `proton_11.5-GE` | `9ae118410b9` | 35249104592 | `11.0-5-arm64ec` vc 8 |
| `proton_11.6-GE` | `523d48bc7c9` | 35249230572 | `11.0-6-arm64ec` vc 8 |
| `proton_11.7-GE` | `807734e65c8` | 35249378639 | `11.0-7-arm64ec` vc 8 (+ x86_64 leg) |
| `proton_11.7.1-GE` | `f8d48977dce` | 35249628060 | `11.0-7.1-arm64ec` vc 8 (+ x86_64 leg) |

### Port shape
- Delta = **366 files**: ~340 Wayland/Turnip dependency files + 25 source files + the per-branch workflow.
- `dlls/winewayland.drv/` is **byte-identical** between the pre-Wayland `proton_11.0-2` base and every
  11.x layer, so the driver source applied unchanged; only `win32u`/`ntdll` differed (1 line).
- Per-layer workflow edits mirrored from the v8 base: apt `libwayland-bin`, `versionCode` -> 8 on both
  the Proton and Wine profile blocks, the v8 short description, the `winewayland-files` packaging loop,
  and a `winewayland-${ARCH_NAME}` upload step.

### Conflicts resolved (2, both real)
1. **`dlls/amd_ags_x64/amd_ags_x64_main.c`** — on `proton_11.0` and the GE layers, which had locally
   removed the vkd3d-ext `uavSlot` from the DX12 create-device TRACE. Resolution: keep the layer's
   TRACE wording, add v8's `report_call` diagnostics. (`proton_11.7.1-GE` keeps the vkd3d ext — its
   newer base never dropped it — and applied cleanly.)
2. **`dlls/win32u/vulkan.c`** on `proton_11.7.1-GE` only, which sits on Valve `46b29104` and has a
   signaller-thread path the `74e9e80fb0` base lacks. Resolution: keep 11.7.1's signaller-thread
   shutdown + fence-op free, then apply v8's reorder (drop bookkeeping **before** destroy, ERR tracing).

### x86_64 legs
`proton_11.7-GE` and `proton_11.7.1-GE` keep their box64 legs. winewayland is arm64ec-only, so the
Wayland upload step is guarded with `if: matrix.arch == 'aarch64'` (an unguarded
`if-no-files-found: error` would fail the x86_64 job on an empty folder). The x86_64 DESC keeps its
box64 text and does **not** claim Wayland; it still moves to vc 8.

### Verification
- All six runs **success**; `verify-layer: PASS` on every leg; `winewayland-arm64ec` uploaded on all six.
- wcps re-verified off-CI: zstd integrity, `profile.json` identity (`vc 8`, per-layer versionName),
  `winewayland.so` + `winewayland.drv` (aarch64-windows and i386-windows) present, 8 Wayland Turnip ICDs.
- Staged to the device under `/sdcard/Download/`; sha256s recorded alongside.

### Follow-up: Wine profile identity corrected (staging/<layer>/v8-wine-label)
The **Wine** profile block on `11.3-GE`, `11.5-GE`, `11.6-GE`, `11.7-GE` still carried the copy-pasted
`11.0-1` values — `versionName`, `description` and the `proton-wine-11.0-1-$ARCH_NAME.wcp.xz` filename —
so four layers emitted their Wine component under `proton_11.0`'s name. This port had moved them vc 0 -> 8,
which would have made four different trees collide on one identity. Corrected to each layer's own slot:

| layer | Wine versionName | Wine description | wcp filename |
| --- | --- | --- | --- |
| `proton_11.3-GE` `4eac68edde4` | `11.0-3-${ARCH_NAME}` | = its Proton description | `proton-wine-11.0-3-*` |
| `proton_11.5-GE` `132ac014f64` | `11.0-5-${ARCH_NAME}` | = its Proton description | `proton-wine-11.0-5-*` |
| `proton_11.6-GE` `f6031f6b00f` | `11.0-6-${ARCH_NAME}` | = its Proton description | `proton-wine-11.0-6-*` |
| `proton_11.7-GE` `340a1f0e9cb` | `11.0-7-${ARCH_NAME}` | `${DESC}` (per-arch, as its Proton block) | `proton-wine-11.0-7-*` |

`proton_11.0` (genuinely 11.0-1), `proton_11.0-2` and `proton_11.7.1-GE` were already correct.
The Proton profile and the layer wcp are untouched by this — the Wine block only feeds the separate
`proton-wine-*.wcp.xz`, so the six layer wcps staged above remain valid.

### Known, not fixed here
- **Not yet on v8:** `proton_10.0` (10.0-4, vc 7), `proton_10.34-GE` (10.0-34, vc 7),
  `proton_11.0-cachyos` (vc 1), and `proton_11.0-2`'s x86_64 leg (matrix is aarch64-only). The 10.x
  pair share no merge-base with the 11.x line — a separate port, not a repeat of this one.

## 2026-09-17: v8 identity applied (staging/proton_11.0-2/v8-rename)

- Workflow `build-proton-11.0-2.yml` now stamps `versionName 11.0-2-${ARCH_NAME}` (was `11.0-2.1-*`)
  and `versionCode 8` (was 16) in both the Proton and Wine profiles; description tagline shortened to
  "Own layer line 11.0-2 (vc 8 - supersedes stock 11.0-2 and the v16 Wayland line)".
- arm64ec-only for now (Wayland line is arm64ec-only; x86_64 leg deferred to the cross-layer v8 port step).

## 2026-09-16: Wayland v16 line merged into proton_11.0-2 (v8 base) — commit `234bdd0050b`

- `--no-ff` merge (parents: `777342a9618` proton_11.0-2 + `133433d2240` `feat/wayland-hdr-ags-v16`), pushed to
  `origin/proton_11.0-2`; CI run **35219499413** fired (in_progress at log time).
- One conflict in `build-scripts/build-step-arm64ec.sh`: the parent's fail-hard MARKERS array (20 rows) vs
  v16's AGS source checks. Resolved by keeping the array and folding v16's two AGS rows in
  (`dlls/amd_ags_x64/unixlib.c|__ANDROID__`, `...|STATUS_NOT_IMPLEMENTED`). `bash -n` clean.
- Audit: merged tree contains every v16 file; diff vs v16 == exactly the parent's 6-file CI-hardening set
  (`.github/workflows/README.md`, `build-proton.yml`→`build-proton-11.0-2.yml`, deleted `publish-p11-consolidated.yml`,
  `build-step-arm64ec.sh`/`build-step-x86_64.sh`, `verify-layer.py`).
- Identity NOT yet bumped: the merged workflow still stamps `versionName 11.0-2.1-*`, `versionCode 16`. The v8
  rename (vn `11.0-2-arm64ec`, vc 8) is a separate later step, alongside porting the stack to the other layers.

## 2026-09-16: the builtin AGS was unreachable, and why — the copy beside the .exe always won (versionCode 16)

**Device results of versionCode 15 (user, container 7, RE3):**
1. Game's `amd_ags_x64.dll` moved aside, `WINEDLLOVERRIDES=amd_ags_x64=b,n`: **RE3 did not start.**
   The Wayland log shows explorer.exe and tabtip.exe connecting and then nothing.
2. Layer's own `lib/wine/aarch64-windows/amd_ags_x64.dll` (524,288 B) copied into the game folder
   with `=n`: **also did not start.**
3. `Software\Wine\AmdAgs` never appeared, in any run, running or after `wineserver` exited.
4. Original 42,496-byte DLL restored, override cleared: RE3 launches and presents again.

### All three are now explained, and none of them the way the last round guessed
**The prefix's `C:\windows\system32` holds the real builtin PEs, not fake DLLs — and the copy list
does not include this module.** Measured against the installed layer's `lib/wine/aarch64-windows`:

| module | layer | prefix system32 | stamped |
| --- | --- | --- | --- |
| `atiadlxx.dll` | 851,968 | 851,968 | 2026-09-15 22:53 |
| `version.dll` | 589,824 | 589,824 | 22:53 |
| `xinput1_3.dll` | 589,824 | 589,824 | 22:53 |
| `winmm.dll` | 917,504 | 917,504 | 22:53 |
| `opengl32.dll` | 2,293,760 | 2,293,760 | 22:53 |
| `user32.dll` | 2,949,120 | 2,949,120 | 22:53 |
| **`amd_ags_x64.dll`** | **524,288** | **MISSING** | — |

Every one was restamped when the container moved to this layer, so the copy happens on layer
switch and works — from a **fixed list** that predates the module existing. So the only copy of
`amd_ags_x64.dll` the loader can ever see is the one `re3.exe` ships.

- **Why the default run never used the builtin.** That shipped copy is a plain x64 PE, so
  `load_builtin()` starts from its machine and `find_builtin_dll()` searches
  `get_pe_dir(IMAGE_FILE_MACHINE_AMD64)` = `/x86_64-windows` — a directory an arm64ec build does
  not have (the wcp has `aarch64-unix`, `aarch64-windows`, `i386-windows` and nothing else). Miss →
  `STATUS_IMAGE_ALREADY_LOADED` → the game's copy is used. Every time, whatever the load order.
- **Why test 1 failed, and why the versionCode 15 change was never reached.**
  `find_builtin_without_file()` returns `STATUS_DLL_NOT_FOUND` immediately unless the prefix is
  bootstrapping or the module is a 16-bit one — `dlls/ntdll/loader.c:3285`. **The v14/v15 notes
  claimed this path would find the builtin when no file was present; that was wrong.** With
  nothing in the app directory and nothing in system32 the import simply fails and the process
  never starts. `load_builtin()` only runs on a file that was found and mapped, so the v15 change
  was not exercised at all by that test.
- **Why test 2 failed, for a third and unrelated reason.** `load_builtin()` refuses a
  `wine_builtin` image under `LO_NATIVE` outright — `dlls/ntdll/unix/loader.c:1756`,
  `if (image_info->wine_builtin) { if (loadorder == LO_NATIVE) return STATUS_DLL_NOT_FOUND; … }`.
  Forcing `native` on a Wine builtin is the one combination Wine rejects. The very same copy under
  the default order, or `=b,n`, would have been found: its own machine is ARM64, so the search
  would have gone to `aarch64-windows`. Test 2 was one character from working.

**Mapping was never the problem.** `map_image_into_view()` switches an ARM64 image to its x64 view
through `update_arm64x_mapping()` when the caller asks for AMD64
(`dlls/ntdll/unix/virtual.c:3431-3441`), so the arm64ec builtin loads into an x64 process normally.
The built PE is also structurally identical to every other builtin in the layer — machine ARM64
with `.hexpthk` and `.a64xrm` sections, exactly like `user32.dll`, `atiadlxx.dll` and `vulkan-1.dll`
— and its imports (advapi32, kernel32, ntdll, ucrtbase, user32, version, vulkan-1) are all present.

### Fix (`8af606a1f66`, `dlls/ntdll/unix/loader.c`)
On arm64ec the builtin search follows the current machine for a **named short list** of modules
whose shipped copy cannot work here at all — `arm64ec_builtin_must_win()`, currently just
`amd_ags_x64.dll`. A game's AGS reaches the AMD driver through ADL2/atiadlxx and imports nothing
from DXGI, so on a non-AMD GPU it reports no display whatsoever, and a title that asks AGS about
its screen is told there is none. That is how the HDR option ends up greyed out with no way to
turn it on. **This is not new behaviour — it is what x86_64 Proton already does**, where
`/x86_64-windows` exists and the builtin is found with none of this. A named case in this file has
precedent: `get_load_order()` already special-cases `easyanticheat*`.

The default order is untouched for everything else, for the reason that still stands: `dxgi`,
`d3d9`, `d3d11`, `d3d12`, `opengl32`, `winmm`, `version`, `dsound` and `xinput1_3/1_4` have no
`--prefer-native` in this tree, and a ReShade proxy is usually one of those. The container's own
`DllOverrides` covers `dxgi`/`d3d9`/`d3d11` but not `opengl32` or `winmm`.

Rollback for a title that regresses: set the `amd_ags_x64` DLL override to **native**, which
returns above all of this.

### What versionCode 15's Stage marks should now say
They were never reached before, because the builtin never loaded. With the lookup fixed the first
run should show `Stage` 7 with `Adapter` = the Vulkan device name, `Displays` ≥ 1,
`ColorSpace` 12 and `HDR10` 1. Anything short of that now points at a specific step rather than at
silence — the versionCode 15 table still applies.

**Device status: not device-proven.** No release, tag, catalog change or staging.

### Also worth fixing outside the layer
`amd_ags_x64.dll` belongs in the app's system32 copy list. With it there, a container gets the
builtin the ordinary way and "move the game's copy aside" becomes a working workaround instead of
a failure to launch. That is a Bannerlator change, not a layer one, and it is worth doing anyway:
this loader entry is a per-module list, and the copy list is the general answer.

### Carry into v8
`8af606a1f66` replaces versionCode 15's `235b0fc17a0` hunk in the same place; carry the later one.
It is arm64ec-only by construction and inert on any parent that is not an arm64ec build. Do not
widen it to the default load order while carrying. The CI commit is not carried.

## 2026-09-16: say why the HDR option is greyed out, instead of leaving silence (versionCode 15)

**Device result of versionCode 14 (user, Pocket FIT, container 7, RE3, HDR10 external screen):**
layer install verified (`lib/wine/aarch64-windows/amd_ags_x64.dll` present, `profile.json`
versionCode 14, container on `Proton-11.0-2.1-arm64ec-14`). `Software\Wine\AmdAgs` **absent**, with
and without `WINEDLLOVERRIDES=amd_ags_x64=b` — confirmed in the game's own `/proc/<pid>/environ`
alongside `DXVK_HDR=1`. Ran to the title screen and through Options → Display, killed the game,
waited for `wineserver` to exit, force-stopped the app: `pgrep wineserver` = 0 and
`grep -a -c AmdAgs user.reg` = 0, so not a flush artefact. HDR Mode still greyed.

### The v14 diagnostic had one blind spot, and it is the interesting one
`init_ags_context()` returned at `if (ret != AGS_SUCCESS || !context->device_count) return ret;`
**before** `report_hdr_state()`. So an absent key means one of three things, and versionCode 14
cannot tell them apart:
1. the builtin never loaded and the process used the `amd_ags_x64.dll` beside `re3.exe`;
2. it loaded, was called, and bailed on that line — a context with no Vulkan device carries no
   display, `agsInitialize` still returns `AGS_SUCCESS` with `numDevices` 0, RE3's walk
   (`test eax,eax; jle`) exits immediately and greys the option out;
3. it ran all the way and honestly found no HDR.

Everything ruled out by inspection this round, so the next device run is decisive rather than
another round of the same:
- **Not a missing import.** The built `amd_ags_x64.dll` is machine `0xaa64` (ARM64X, correct dir)
  and imports only advapi32, kernel32, ntdll, ucrtbase, user32, version, vulkan-1 — all present in
  the layer, sizes checked.
- **Not a container DLL override.** `HKCU\Software\Wine\DllOverrides` in the prefix has 95 entries
  (`dxgi`, `d3d9`, `d3d11`, `ddraw`, `d3dcompiler_*`, `openal32`, `msvc*`, `ucrtbase`, `wined3d` =
  `native,builtin`; `atiadlxx`, `nvcuda` = `disabled`) and **no `amd_ags_x64`**, so the default
  order applied.
- **Not the app fighting the env var.** Bannerlator treats `WINEDLLOVERRIDES` as a user field it
  merges into `envVars` (`EpicGameFixes.mergeDllOverrides`), it does not set a competing one.

### A real arm64ec loader bug found on the way
`load_builtin()` starts from the machine of the file it just found
(`dlls/ntdll/unix/loader.c:1750`), and for a plain x64 DLL beside a game's `.exe` that is
`IMAGE_FILE_MACHINE_AMD64`, so `find_builtin_dll()` searches
`get_pe_dir(AMD64)` = **`/x86_64-windows`** — a directory an arm64ec build does not have at all
(the wcp has only `aarch64-unix`, `aarch64-windows`, `i386-windows`; confirmed on the installed
layer too). The identical builtin loads fine when *no* file is found anywhere, because
`find_builtin_without_file()` (`dlls/ntdll/loader.c:3352`) uses the running process's `pe_dir`
instead. The two paths disagree, and the consequence is that `WINEDLLOVERRIDES=<dll>=b` — the
override versionCode 14's own notes tell people to set — **cannot do what it says** for any DLL a
game ships beside its `.exe`: the lookup misses and `LO_BUILTIN` returns `STATUS_DLL_NOT_FOUND`.

`235b0fc17a0` follows the current machine on arm64ec when the load order names the builtin
explicitly (`LO_BUILTIN`, `LO_BUILTIN_NATIVE`), as well as for a hybrid image as before. **The
default order is left alone on purpose:** redirecting it too would let builtins shadow every x64
wrapper a game ships next to its exe, and the names that matter are unprotected — `dxgi`, `d3d9`,
`d3d11`, `d3d12`, `opengl32`, `winmm`, `version`, `dsound`, `xinput1_3/1_4` have no
`--prefer-native` in this tree, and a ReShade proxy is usually one of those. Widening it needs
evidence this round does not have yet.

### Fix (three commits, branch `feat/wayland-hdr-ags-v15` off v14's `670738f453e`)
- `27cdaf945b9` `dlls/amd_ags_x64/amd_ags_x64_main.c`: the whole path is marked in
  `HKEY_CURRENT_USER\Software\Wine\AmdAgs`, earliest first — `Stage` 1 DllMain attach (+`Process`,
  `Module`), 2 an init entry point (+`Entry`, `Calls` bitmask over agsInit / agsInitialize /
  agsGetGPUInfo / DX11_CreateDevice / DX12_CreateDevice / agsSetDisplayMode / agsDeInitialize),
  3 version settled (+`VersionRequested`, `PublicVersion`, `AgsVersionRow`), 4 about to ask Vulkan,
  5 Vulkan answered (+`VkCreateInstance`, `VkEnumerate`, `VkDevicesRaw`, `VkDeviceType0`,
  `VkDeviceName0`, `VkDevicesKept`, `Devices`), 6 displays walked (+`Displays`, `Adapter`,
  `ColorSpace`, `HDR10`, `MaxLuminance`), 7 context handed over (+`Result`). **Every exit path of
  `init_ags_context()` reports before returning**, including the no-device one. Two fixes found on
  that path: `if ((vr = vkCreateInstance(...) < 0))` put the comparison inside the assignment so
  `vr` was only ever 0 or 1 in the warning (control flow was already right), and the
  integrated/discrete device-type filter now falls back to keeping whatever Vulkan listed if it
  would otherwise keep nothing, saying so with `VkTypeFilterBypassed` — reporting no device at all
  is strictly worse, since only the AMD-specific fields care about the type and they are guarded
  separately.
- `235b0fc17a0` `dlls/ntdll/unix/loader.c`: the arm64ec builtin lookup above.
- `301101143507` ci versionCode 14 → 15 + one sentence per profile.

**X11 unaffected.** Neither commit touches win32u, winewayland or anything keyed on
`monitor->hdr_enabled`; the loader change is inside `is_arm64ec()` and only fires for an explicit
override.

### Reading it on device
`bridge "grep -a -A16 'Software..Wine..AmdAgs]' <prefix>/user.reg"`

| what you see | what it means |
| --- | --- |
| key absent entirely | the builtin never loaded, and `DllMain` is as early as it gets — a loader problem, not an AGS one |
| `Stage`=1 only | loaded, but the game never called an AGS init entry point. Check `Calls` |
| `Stage`=5, `Devices`=0 | the v14 blind spot: Vulkan gave AGS nothing. `VkCreateInstance`/`VkEnumerate` carry the VkResult, `VkDevicesRaw`/`VkDeviceType0` say whether it was the type filter |
| `Stage`=6, `Displays`=0 | the display walk found nothing: `EnumDisplayDevices`' DeviceString did not equal `Adapter` |
| `Stage`=7, `ColorSpace`=8 | DXVK reported sRGB — `DXVK_HDR` did not reach the process |
| `Stage`=7, `ColorSpace`=12, `HDR10`=1 | Wine's side is done; anything still greyed is the title's own gate |

**Zero-build discriminator, works on versionCode 14 already:** rename
`E:\Winlator\Games\Resident Evil 3\amd_ags_x64.dll` out of the way. With no file next to the exe
the loader takes `find_builtin_without_file()`, which looks in the running process's PE directory,
so the builtin is reachable by the path that never had the bug. If the key then appears, the
lookup was the problem; if it still does not, the DLL is loading and failing later.

**Device status: not device-proven.** No release, tag, catalog change or staging.

### Carry into v8
`27cdaf945b9` is self-contained and rides with the versionCode 14 AGS commits. `235b0fc17a0`
touches `dlls/ntdll/unix/loader.c` and is arm64ec-only by construction — inert on any parent that
is not an arm64ec build, and inert there too unless a DLL override names a builtin. Do not widen
it to the default load order while carrying. The CI commit is not carried.

## 2026-09-15: the layer ships Wine's own AGS, which is where RE Engine actually asks about HDR (versionCode 14)

**Ask (user, 2026-09-15, second round):** versionCode 13 is installed and partly working — the
compositor reports `qcom_compressed` buffers, `strings … winewayland.so | grep -c DXVK_HDR` = 3 on
the installed layer against 0 on v11, and **both** monitor devices in the prefix carry the advanced
colour device property as TRUE
(`…\Enum\DISPLAY\WAY0001\0000&0000\Properties\{233a9ef3-…}\0006 @=hex(ffff0011):01,00,00,00`, same
for `0001&0000`). Resident Evil 3 still greys "HDR Mode" out with *"This setting requires HDR
support."* Find the remaining gap. Wine's stderr is discarded by this app, so no diagnostic may
depend on it.

### v13's premise was wrong for this game. DisplayConfig is not where it asks.

Disassembled the installed `re3.exe` (167 MB, `E:\Winlator\Games\Resident Evil 3\`) and the
installed `dxgi.dll`, and read the prefix registry, rather than reasoning from the import list.

- **`re3.exe` does send the packets — and throws two of the answers away.** There are exactly 10
  call sites for the DisplayConfig triple, all in one display-enumeration function. Per active
  path it sends, in order, types **3, 4, 6, 7, 9, 11** (`.text:0x142eafec6`…`0x142eaffe6`):
  `…ffa4  mov dword [rbp-0x6c], 0x20` / `…ffaf  mov dword [rbp-0x70], 9` → GET_ADVANCED_COLOR_INFO,
  then `…ffd4  mov dword [rbp-0xc], 0x18` / `…ffdf  mov dword [rbp-0x10], 0xb` →
  GET_SDR_WHITE_LEVEL. Neither return value is tested, and **neither buffer is ever read back**:
  across the whole function the only later accesses to `[rbp-0x70 … -0x54]` and `[rbp-0x10 … +4]`
  are writes, from unrelated code reusing the slots. What it *does* consume right after is
  `[r13+0x1c]`/`[r13+0x20]` (the source mode's `position.x/y`) and `[rsi+0x28]`/`[rsi+0x2c]` (the
  target mode's `activeSize.cx/cy`) — geometry, not colour. Only the type-3 result is stored, and
  only on success. **So versionCode 13 could not have changed this title's mind.**
- **The id match (candidate a) is sound — ruled out, not assumed.** The game builds each header
  from `modes[i].id` / `modes[i].adapterId` (`mov ecx,[rsi+4]` / `mov rax,[rsi+8]` on a
  `DISPLAYCONFIG_MODE_INFO`), which for a target mode is exactly what `set_mode_target_info()`
  writes: `monitor->output_id` and `monitor->source->gpu->luid`
  (`dlls/win32u/sysparams.c:3862-3864`) — the same pair the handler matches on at `:8205-8209`.
  The handler compares `header.adapterId` too, contrary to the brief. Device registry confirms the
  ids line up: `DISPLAY\WAY0001\0000&0000` is the **virtual** source's monitor with output_id **1**
  (`{CA085853-…}\0002 = 01,00,00,00`), `0001&0000` is the detached physical one with output_id
  **0**; both carry the HDR property and both carry the 256-byte EDID.
- **The EDID is correct.** Decoded the one in the prefix: v1.4, digital, 10 bits per primary
  (`0xb5`), 1280x720@60 DTD, name "Wayland", 1 extension = CTA-861 rev 3 carrying a Colorimetry
  Data Block (BT.2020 RGB) and an HDR Static Metadata Data Block with ET bit 2 set (SMPTE ST 2084),
  max-luminance code 106 ≈ 497 nits, frame-average code 74 ≈ 248 nits.
- **DXVK never asks Wine about advanced colour.** Its `dxgi.dll` has 3 real
  `DisplayConfigGetDeviceInfo` call sites and the packet types are the two 8-byte constants at
  `0x2ee36fb30`/`0x2ee36fb38`: `type=1 size=84` (GET_SOURCE_NAME) and `type=2 size=420`
  (GET_TARGET_NAME) — monitor→device-path mapping only, for the SetupAPI EDID read. `DXVK_HDR` /
  `dxgi.enableHDR` is read at `0x2edf0c1ac` (`cmp qword [rsp+0xc8],1` / `cmp byte [rax],0x31`) into
  `options->enableHDR` at +0x3c, and the only thing that can clear it is the UE4 guard
  (`rfind("-Win64-Shipping")` then `GetModuleHandleA("d3d12")`), which cannot fire for `re3.exe`.
  **So `IDXGIOutput6::GetDesc1` was already returning `DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020`
  on this container**, and a DXGI-only gate would have worked before v13.

### Root cause: the layer has never shipped `amd_ags_x64.dll`

`re3.exe` imports **15** entry points from `amd_ags_x64.dll`, including `agsInitialize` and
`agsSetDisplayMode` — AMD's "put this display into HDR10" call. It imports **no** NVAPI symbol at
all (`grep -c nvapi64 re3.exe` = 0), so the NVAPI route the community writeups describe is not this
binary. Its HDR answer comes from `AGSDisplayInfo`.

Wine's builtin fills that in from DXGI and is **not** AMD-only:
`fill_chroma_info()` (`dlls/amd_ags_x64/amd_ags_x64_main.c:553-565`) walks every DXGI adapter and
output, matches on `HMONITOR`, and sets `info->HDR10 = 1` when
`output_desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020`. Only the extra AMD device
fields sit behind `vk_properties->vendorID == 0x1002` (`:793`); the display walk
(`init_device_displays_600()`, `:838`) runs for every Vulkan device. That is what the DLL is for.

The arm64ec build passed **`--disable-amd_ags_x64`** (`build-scripts/build-step-arm64ec.sh:139`),
so it was never built. Checked on the device: `lib/wine/aarch64-windows/` in versionCode 9, 10, 11
**and** 13 has `atiadlxx.dll` and `amdxc64.dll` and no `amd_ags_x64.dll`; the prefix has none
either. So every AGS title on this layer has always loaded the copy sitting next to its own .exe —
for RE3 a 42 KB `amd_ags_x64.dll` whose only USER32 import is `EnumDisplayDevicesA` and whose
display data comes from ADL2/`atiadlxx`. On a Turnip/Adreno device that finds no AMD display, so
`HDR10` stays 0 and the game says the setting requires HDR support. Exactly the observed sentence.

### Fix (four commits, branch `feat/wayland-hdr-ags-v14` off v13's `c7c25989efd`)
- `5b934b4ef4b` `build-scripts/build-step-arm64ec.sh` + `android/patches/…unixlib.c.patch`: drop
  `--disable-amd_ags_x64`. Nothing else was needed to build it — `WIN_ARCH` already has `arm64ec`
  and `#pragma makedep arm64ec_x64` is used by modules that build here today (ntdll, rpcrt4,
  oleaut32). The Android unixlib patch gained two stub entries: bionic has no `libdrm_amdgpu`, so
  the stock `unixlib.c` was `#ifdef`'d out, but it left `__wine_unix_call_funcs[] = {}` — the PE
  side indexes that table by `enum amd_ags_funcs` and the dispatcher does not bounds check. Only
  reachable behind `vendorID == 0x1002`, i.e. never on Adreno, but the table is now the right shape
  and answers `STATUS_NOT_IMPLEMENTED`, which `init_unix_lib()` already reads as "no unix side".
  Two new post-apply greps (`__ANDROID__`, `STATUS_NOT_IMPLEMENTED` in that file) join the
  hardening block, because the apply loop is fail-soft; and `--install` now aborts if
  `amd_ags_x64.dll` is missing from the built layer.
- `fa7607d7030` `dlls/amd_ags_x64/amd_ags_x64_main.c` + `Makefile.in`: the device diagnostic, see
  below. `advapi32` joins the imports.
- `a63e57ae66f` `dlls/win32u/sysparams.c`: `GET_SDR_WHITE_LEVEL` answers 1000 (the 80-nit SDR
  reference white, in the thousandths the field is defined in — Windows' own default) and
  `SET_ADVANCED_COLOR_STATE` agrees when the request matches `monitor->hdr_enabled` and returns
  `STATUS_NOT_SUPPORTED` when it does not, instead of both falling into the "Unimplemented packet
  type" arm. This is the user's candidate (b), closed on principle rather than on evidence: RE3
  discards the type-11 result, so it cannot be *this* title's cause, but an app that reads a
  failure there as "the HDR query failed" would behave identically to one never told about HDR.
  `GET_TARGET_BASE_TYPE` (6) and `GET_SUPPORT_VIRTUAL_RESOLUTION` (7) are left failing: nothing to
  do with colour, and RE3 discards those too.
- `b62aba315a1` ci versionCode 13 → 14 + one sentence per profile.

**Build.** CI run 35045330344 (workflow_dispatch on `feat/wayland-hdr-ags-v14`, headSha
`b62aba315a1e5d241d18c0d75a5f39ceb8fc5d01` verified): ✅ green, artifact `proton-arm64ec-sdk28` →
`proton-11.0-2-arm64ec.wcp`, sha256
`bd55b618140625a2520936da97741e5614c46d1fecb8a62f98c1f49a5269de26` (117,539,686 B, +86,194 B on
v13), profile `Proton 11.0-2.1-arm64ec` versionCode 14 → installs as
`Proton-11.0-2.1-arm64ec-14` next to -13. The install step's own check printed
`amd_ags_x64.dll present: …/lib/wine/aarch64-windows/amd_ags_x64.dll`.
Against the installed v13 layer the file list gains exactly five files and loses none:
`lib/wine/aarch64-windows/amd_ags_x64.dll` (524,288 B), `lib/wine/i386-windows/amd_ags_x64.dll`
(61,440 B), `lib/wine/aarch64-unix/amd_ags_x64.so` (3,664 B) and the two `libamd_ags_x64.a` import
libs. On-artifact markers: `Software\Wine\AmdAgs` plus `Process`/`Adapter`/`Displays`/`ColorSpace`/
`HDR10`/`MaxLuminance` and `amd_ags_x64: %s asked AGS about %u display(s) on %s; …` in
`amd_ags_x64.dll`; `DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL.`,
`DISPLAYCONFIG_DEVICE_INFO_SET_ADVANCED_COLOR_STATE.` and `Cannot turn advanced colour %s for
monitor %s.` in `win32u.so`; v13's `DXVK_HDR` still 3× in `winewayland.so`.
No release, tag, catalog change or staging. Stacked on `feat/wayland-hdr-advanced-color-v13`, which
is stacked on `feat/wayland-ubwc-v12`; fast-forward the chain once the device test has spoken.

### Proof the gate really is AGS (static, from the installed binaries)
`re3.exe` calls `agsInitialize(0x1800001 /* AGS 6.0.1 */, NULL, &ctx, &gpu_info)` at
`.text:0x142fa9560` and returns failure from its whole display-capability routine if that is not
`AGS_SUCCESS` (`test eax,eax; je …; xor al,al; jmp`). It then walks `gpu_info.numDevices` (+0x10)
and `gpu_info.devices` (+0x18) with device stride **0x78**, per device `numDisplays` (+0x48) and
`displays` (+0x50) with display stride **0x1d8**, matches a display by byte-comparing its own
display name against **+0x100** (`displayDeviceName`), and then:

```
000142fa94bf  41f6822001000012   test byte ptr [r10 + 0x120], 0x12
000142fa94c7  7522               jne  0x142fa94eb        ; found an HDR display
...
000142fa94f4  d1e8               shr  eax, 1             ; HDR10          -> its record +8
000142fa94f8  c1e904             shr  ecx, 4             ; freesyncHDR    -> its record +9
```

`0x12` is bit 1 | bit 4 = `AGSDisplayInfo::HDR10 | freesyncHDR`. With no display setting either
bit it keeps the values pre-set at function entry — index `0xffffffffffffffff`, both flags 0 — i.e.
"no HDR display", which is the greyed-out option. Wine's structs are byte-identical to what it
reads: `AGSDisplayInfo_600` is `name[256]` + `displayDeviceName[32]`, so its flags dword is at
0x120 with HDR10 = bit 1 and freesyncHDR = bit 4, and `sizeof(AGSDeviceInfo_600)` is 0x78 with
`numDisplays` at 0x48 and `displays` at 0x50. `amd_ags_info[]` maps 6.0.0–6.0.1 to
`AMD_AGS_VERSION_6_0_0`/`sizeof(AGSDeviceInfo_600)`, and `determine_ags_version()` returns
`get_version_number(*ags_version)` directly when the app names a version, so 0x1800001 selects
exactly that row and `init_device_displays_600()`. `re3.exe` also imports and calls
`agsSetDisplayMode` (`.text:0x142fa9ff5`), which the builtin implements
(`Mode_600_HDR10_PQ` → `DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020`, `:1190`).

The display walk matches on `strcmp(EnumDisplayDevices DeviceString, vk_properties->deviceName)`
(`:615`), and both sides are the Vulkan GPU name here — `re3_config.ini` recorded
`[Render/Adapter] Description=Turnip Adreno (TM) 750` and `[Render/Display]
DisplayName=\\.\DISPLAY1`, which is the virtual source (source id 0), the one carrying the HDR
monitor. The DXVK GPU spoof (`gpuName=NVIDIA GeForce GTX 480`) only rewrites DXGI's adapter
description, not Vulkan's, so it does not break that match.

**Risk.** `agsDriverExtensionsDX12_CreateDevice` in the builtin just forwards to
`D3D12CreateDevice` and reports whatever `ID3D12DeviceExt3` says; its deliberate AGS_DX_FAILURE
hack is gated on `SteamGameId == "3321460"` (`:…+41`), and RE3 is 952060, so it never fires.
`agsDriverExtensionsDX11_CreateDevice` likewise just forwards. Rollback if an AGS title regresses:
`WINEDLLOVERRIDES=amd_ags_x64=n` (native) or `=amd_ags_x64=` (disabled) in the shortcut's
`envVars`; `=b` forces the builtin.

**Load order.** `dlls/amd_ags_x64/Makefile.in` has no `--prefer-native`, so the builtin does not
carry `IMAGE_DLLCHARACTERISTICS_PREFER_NATIVE`, which is the only thing `prefer_native` is checked
against (`dlls/ntdll/unix/loader.c:1039`, reached from `load_builtin()`'s `LO_DEFAULT` arm at
`:1779`). So with the DLL present, Wine's default load order takes the builtin over the copy next
to the .exe. A container that wants the old behaviour sets the `amd_ags_x64` DLL override to
`native` or `disabled` — that is also the one-line rollback if this regresses an AGS title.

**X11 guarantee (v13's, kept).** Both new win32u cases read `monitor->hdr_enabled` and nothing
else, so an X11 session — no EDID, `edid_describes_hdr_screen()` false, virtual monitor's flag off
— answers for an SDR screen exactly as in v13. AGS's `HDR10` is a different flag on a different
path: it comes from DXVK's colour space, i.e. from `DXVK_HDR`, which is what it would be on real
Proton, and the app only exports it when the user turns HDR on.

### Diagnostic, readable with no Wine stderr
`init_ags_context()` writes, once per process and only in a process that uses AGS at all, to
**`HKEY_CURRENT_USER\Software\Wine\AmdAgs`** (the prefix's `user.reg`, the same class of marker as
the monitor device property the user could already read):

| value | meaning |
| --- | --- |
| `Process` | which .exe wrote it |
| `Adapter` | the Vulkan device name displays are matched against |
| `Displays` | how many displays AGS reported |
| `ColorSpace` | `DXGI_OUTPUT_DESC1.ColorSpace`, or -1 if no DXGI output matched the monitor |
| `HDR10` | what the game is told; 1 iff `ColorSpace` was 12 |
| `MaxLuminance` | the screen's peak in nits, as DXGI read it out of our EDID |

Reading it: **key absent** ⇒ the builtin never ran and the process is still on its own
`amd_ags_x64.dll` (add the DLL override). **`Displays` 0** ⇒ the display walk found nothing, which
means `EnumDisplayDevicesA`'s `DeviceString` did not equal `Adapter` — the walk requires an exact
`strcmp` (`:615`). **`ColorSpace` 8** ⇒ `DXVK_HDR` never reached the process. **`ColorSpace` 12 and
`HDR10` 1** ⇒ Wine's side is done and anything still greyed out is the title's own gate (RE Engine
is reported to want exclusive fullscreen, and `re3_config.ini` currently has `PCWindowMode=0`,
`FullScreenMode=false`, `PCColorSpace=0`). A `MESSAGE` line says the same thing for anyone whose
stderr does survive.

**Device status: not device-proven.** No release, tag, catalog change or staging.

### Carry into v8 (all seven parents)
- The AGS work travels with v13's `efec2d86a45` + `4fc5781cf58` as one HDR feature, but note it is
  **build-script, not Wine source**: `--disable-amd_ags_x64` exists in
  `build-scripts/build-step-x86_64.sh:116` too and is deliberately left there. Each parent that
  gets an arm64ec Wayland build needs the flag dropped in its own copy of the arm64ec script, plus
  `android/patches/dlls_amd_ags_x64_unixlib.c.patch` in its patch list and in its verification
  block. A parent whose `dlls/amd_ags_x64` is an older Proton import must be checked for the
  `vendorID == 0x1002` placement first: the display walk has to be **outside** it, or the builtin
  reports nothing on an Adreno.
- Cherry-pick `fa7607d7030` (diagnostic) and `a63e57ae66f` (win32u packets) freely; both are
  self-contained. `a63e57ae66f` touches only `dlls/win32u/sysparams.c` and applies to the X11-only
  parents, where it is inert while their virtual monitor's `hdr_enabled` stays off.
- The CI commit is NOT carried; each parent stamps its own versionCode.

## 2026-09-15: Windows tells games the screen is in HDR, so their own HDR option stops being greyed out (versionCode 13)

**Ask (user, 2026-09-15):** in-game HDR options are still greyed out on versionCode 11/12, whose
HDR10 EDID DXGI does read (`IDXGIOutput6::GetDesc1` returns the screen's real peak). Games do not
decide from the EDID: they ask Windows whether the display is *in* HDR.

**Root cause (source, and `re3.exe`'s import table):** the question is DisplayConfig's
`DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO` (`QueryDisplayConfig` +
`DisplayConfigGetDeviceInfo`, both imported by the titles that grey the option out). win32u answers
it at `dlls/win32u/sysparams.c:8150-8191` from `monitor->hdr_enabled`: advancedColorSupported and
advancedColorEnabled 1 with bitsPerColorChannel 10 when it is set, 0/0/8 when it is not. Only a
driver sets that flag. Proton's winex11 does — `dlls/winex11.drv/display.c:499`,
`(env = getenv("DXVK_HDR")) && *env == '1'` (Proton `d33f47c489f`, CW-Bug-Id #22912) — and
winewayland never did, so the flag was 0 on every monitor of every Wayland session. And it has to
reach the **virtual desktop's** monitor, the same place the EDID had to reach in versionCode 11:
`QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS)` returns only that monitor, and `add_virtual_source()`
builds it from an all-zero `struct gdi_monitor`.

### Fix (two commits)
- `efec2d86a45` `dlls/winewayland.drv/display.c`: `wayland_add_device_monitor()` sets
  `monitor.hdr_enabled` on the monitor that carries the EDID when **both** hold: `DXVK_HDR=1`,
  read exactly as winex11 reads it so the two drivers gate this on the same thing (the app exports
  it when HDR output is on for the session), and the EDID really describes HDR10 — the app gave a
  peak luminance, so the CTA HDR Static Metadata Data Block carries one. Both are read once per
  process in the existing `edid_hdr_init()` `pthread_once` (`hdr_output_enabled`,
  `edid_hdr_has_peak`), not per monitor. An SDR screen gets no EDID from us at all, so it can never
  claim advanced colour whatever `DXVK_HDR` says.
- `4fc5781cf58` `dlls/win32u/sysparams.c`, exactly parallel to v11's `48fb81bc902`: `struct
  device_manager_ctx` gets `primary_hdr_enabled`, `add_monitor()` captures it under the same
  `ctx->is_primary` condition that captures `primary_edid`, `add_virtual_source()` assigns it next
  to `monitor.edid`, `release_display_manager_ctx()` clears it. The carry is **narrower than the
  driver flag**: it is kept only when that EDID really describes an HDR screen — new
  `edid_describes_hdr_screen()` walks the CTA-861 extension blocks for an HDR Static Metadata Data
  Block (extended tag 0x06) that advertises SMPTE ST 2084. Reason (lead, 2026-09-15): win32u is
  shared code, and winex11 sets `hdr_enabled` from `DXVK_HDR=1` alone with no EDID and no check, so
  an unguarded carry would light advanced colour up for an X11 session whose user hand-set
  `DXVK_HDR=1`, where nothing can present HDR — a washed-out picture with nothing in the log to
  explain it. A driver that gives no EDID (every X11 session here) still changes nothing.
- Deliberately **not** in this build: `GET_SDR_WHITE_LEVEL`, `SET_ADVANCED_COLOR_STATE` and the
  `_2` variants — win32u still answers them `STATUS_INVALID_PARAMETER`. A second step only if
  (a)+(b) prove insufficient on device.

### Diagnostics (MESSAGE level, reach wine_debug.log; only when the driver gave an EDID)
- winewayland `report_edid_handoff()` now ends with the state, and with the reason when it is off:
  `… hands win32u 256 bytes, advanced colour on (DXVK_HDR=1, HDR10 EDID)` ·
  `… advanced colour off (DXVK_HDR is not 1)` · `… off (the EDID names no peak luminance)`.
- win32u `report_monitor_edids()` names it per active monitor, read back after the registry round
  trip: `active monitor DISPLAY\WAY0001\0000&0000: Device Parameters\EDID 256 bytes, advanced
  colour on;`. That line is the proof the flag survived `write_monitor_to_registry()` →
  `WINE_DEVPROPKEY_MONITOR_HDR_ENABLED` → `update_display_cache_from_registry()`.

**Verification off-device:** `edid_describes_hdr_screen()` compiled (gcc `-Wall -Wextra`, no
warning) and run against the EDID `wayland_edid_build()` actually produces: peak+avg+min and
peak-only accepted; base block alone (128 bytes), no EDID, a CTA block with only the Colorimetry
block, an HDR metadata block without the ST 2084 bit, a malformed data-block offset and a non-CTA
extension block all refused (8/8).

**Build:** branch `feat/wayland-hdr-advanced-color-v13` off v12 (`822c244556e`): `efec2d86a45`
(winewayland), `4fc5781cf58` (win32u), `899618e8a8c` (ci versionCode 12 → 13 + one sentence per
profile). CI run 35041126515 (workflow_dispatch on that branch, headSha `899618e8a8c` verified):
✅ green, artifact `proton-arm64ec-sdk28` → `proton-11.0-2-arm64ec.wcp`, sha256
`b8af5604354fd21dc275267d66c882a85242ae3cc8b6ac9804382ec87a519ec1` (117,453,492 B), profile
`Proton 11.0-2.1-arm64ec` versionCode 13 (installs as `Proton-11.0-2.1-arm64ec-13` next to -12).
Against the v12 wcp (`ef8df2d6…`): same 2,544 files; exactly three change size —
`lib/wine/aarch64-unix/win32u.so` (+320 B), `lib/wine/aarch64-unix/winewayland.so` (+272 B) and
`profile.json` (+521 B). The other 1,524 differing files are PEs, import archives and .sys/.cpl/.ocx
with identical sizes (COFF timestamps and checksums); every other unix library — the eight Turnips,
libEGL, libgallium, libwayland, libdrm — is byte-identical to v12. On-artifact markers: `DXVK_HDR`
appears 3× in v13's `winewayland.so` and 0× in v12's; `advanced colour on (DXVK_HDR=1, HDR10 EDID)`
and `off (DXVK_HDR is not 1)` are in `winewayland.so`, ` advanced colour %s;` in `win32u.so`.
No release, tag or catalog change; not staged anywhere. This branch is stacked on
`feat/wayland-ubwc-v12`, which is itself not merged into `feat/winewayland-desktop-11.0-2` yet —
fast-forward both once the device test has spoken.

**Device status: not device-proven.** What a device test should see, with HDR on for the session on
an HDR10 phone: the two log lines above saying `advanced colour on`, and a game whose HDR option is
now selectable (re3-class titles, God of War). On an SDR phone, and on any X11 session, both lines
must keep saying `off` / be absent, and nothing may change.

### Carry into v8 (all seven parents)
- Cherry-pick `efec2d86a45` (winewayland) **and** `4fc5781cf58` (win32u) on top of v10's
  `612401793ce` and v11's `48fb81bc902`. The four are one feature — the EDID, the EDID on the
  virtual monitor, the flag, the flag on the virtual monitor — and either half of this pair alone
  changes nothing a game can see.
- `efec2d86a45` touches only `dlls/winewayland.drv/display.c`, inside v10/v11's hunks, so it goes
  after them; Wayland-only, so it is inert on the parents that never load winewayland.drv.
- `4fc5781cf58` touches only `dlls/win32u/sysparams.c` and applies to the X11-only parents too.
  **X11 exposure to keep in mind there:** their winex11 sets `gdi_monitor.hdr_enabled` from
  `DXVK_HDR=1` alone, so the `edid_describes_hdr_screen()` test in `add_monitor()` is the only
  thing keeping an env-var-only claim off the virtual desktop's monitor. Do not drop it when
  resolving conflicts, and re-check that the parent's `add_virtual_source()` still builds its
  monitor from a zeroed `gdi_monitor`.
- The CI commit `899618e8a8c` is NOT carried.

## 2026-09-15: zero-copy game buffers become UBWC where gralloc allows it (versionCode 12)

**Why (Wayland performance audit, 2026-09-15, Phase 1 fix 2):** every zero-copy swapchain came
out linear. The zero-copy WSI asked gralloc for `GPU_SAMPLED_IMAGE | GPU_FRAMEBUFFER |
COMPOSER_OVERLAY` and nothing else, and QTI gralloc compresses only when the producer sets its
vendor bit `GRALLOC_USAGE_PRIVATE_ALLOC_UBWC` (= gralloc1 `PRODUCER_USAGE_PRIVATE_0` =
`AHARDWAREBUFFER_USAGE_VENDOR_0`, bit 28) with a GPU usage and no CPU bit (`IsUBwcEnabled`, AOSP
`hardware/qcom/sm8150/display` `gralloc/gr_utils.cpp:731-759`; msm8998 `gr_allocator.cpp:619-644`).
Qualcomm's own driver hands the Android loader that bit (WinNative-Emu/Drivers
`add_ubwc_swapchain_usage.py`, captured on an A840); Mesa never sets it. Logs: Pocket FIT
`banner-ahb: 1280x720 swapchain (5 images) on gralloc buffers: linear`; Fold `… linear` after
`gralloc handle layout unknown (2 fds, 34 ints)`. The copy path was already UBWC (the compositor
advertises `qcom_compressed`), so zero-copy traded the copy for uncompressed game buffers.

**Change (Banners-Turnip `wayland` `0121416`, `patches/wayland/banner_ahb_wsi.py`; no Wine source
change):** a chain asks gralloc, in order,
1. **UBWC**: `GPU_SAMPLED_IMAGE | GPU_FRAMEBUFFER | COMPOSER_OVERLAY | VENDOR_0` (usage
   `0x10000b00`), only when the chain's `drm_mod_list` holds `QCOM_COMPRESSED`. That list is the
   compositor's modifiers for the format, filtered by what the driver can create with this
   swapchain's usage, flags, format list and compression control (`wsi_configure_native_image`;
   Turnip's `tu_formats.cc:530-561` refuses QCOM_COMPRESSED for compression-disabled, incompatible
   mutable lists and any usage `ubwc_possible()` rejects). So it means both "the compositor imports
   UBWC for this format" (the app's `BANNER_WAYLAND_UBWC=0` takes it away) and "this image may be
   UBWC". Never for storage swapchains or with `BANNER_WSI_AHB_LINEAR=1`.
2. **plain**: the old request (gralloc's own choice, linear on QTI).
3. **CPU-linear**: plain + `CPU_READ_RARELY`, which gralloc can never compress.
A buffer is used only when its native handle is a QTI private handle (`'gmsm'` magic, flags next:
`PRIV_FLAGS_UBWC_ALIGNED` = UBWC, no UBWC flag = linear; `PRIV_FLAGS_UBWC_ALIGNED_PI`, which a
UBWC-PI buffer carries instead of UBWC_ALIGNED, counts as unreadable), `vkCreateImage` accepts
gralloc's pitch with that modifier, and, for UBWC, gralloc's buffer (`lseek`) is at least the
driver's UBWC image size (Turnip lays UBWC out itself from modifier + pitch, metadata first then
pixels, `fd6_layout.c:380-392`, the gralloc sharing layout). Anything else moves on to the next
request, so a wrong guess can only cost UBWC. An unreadable handle (no `'gmsm'`: newer grallocs,
the Fold) skips request 2 and ends on 3, linear, exactly as before; its ints are printed once per
request (`banner-ahb: unreadable gralloc handle from the UBWC request (2 fds, 34 ints); ints: …`)
so a reader for that handle can be written from a real dump. Later images of a chain reuse its
request; a UBWC buffer smaller than the image is refused rather than imported.

**What wine_debug.log prints (once per swapchain):**
- UBWC: `MESA: info: banner-ahb: 1280x720 swapchain (5 images) on gralloc buffers: UBWC (QCOM_COMPRESSED), stride 1280 px`
- linear: `… on gralloc buffers: linear, stride 1280 px (no UBWC: <why>)`, `<why>` one or more of
  `gralloc answered the UBWC request with a linear buffer` · `gralloc handle layout unknown (N fds,
  M ints)` · `qcom_compressed is not among the compositor's modifiers for this format and usage` ·
  `a storage swapchain` · `BANNER_WSI_AHB_LINEAR=1` · `gralloc refused the UBWC request` · `the
  driver refused gralloc's UBWC layout (pitch P px, <VkResult>)` · `gralloc's UBWC buffer is X
  bytes, the driver's UBWC image needs Y`.
- The compositor's own line already names it: `layer zero-copy: AHB swapchain from <exe> (…,
  UBWC (QCOM_COMPRESSED), stride … px)`.

**Compositor (Bannerlator): no change needed.** `sc_layer_present_ahb` hands the AHB to
SurfaceControl as is (SurfaceFlinger/HWC read gralloc's own metadata); `ahb_attach` only logs the
modifier; no game AHB is ever CPU-locked. The copy-path fallback (window not a layer candidate)
imports the same dma-buf into the compositor's Turnip with `QCOM_COMPRESSED` + gralloc's pitch,
the pitch the game's Turnip already accepted; a refusal is logged as `dmabuf: vkCreateImage(
qcom_compressed, …) -> …: the game's UBWC layout was refused` and the frame is then layer-only.

**Verification off-device:** the patch applies to all six Mesa pins (plain `7cda7850`, a7xx
`7631b525`, a8xx/a8xx-gen8 `12b7b819`, smxz `c501e1d1`, white `9c475fc3`, upstream `bbc7792f`);
the helper block compiles cleanly with gcc and clang (host and aarch64-android29) against stub
Mesa types; a mock-gralloc/mock-driver test of `banner_ahb_setup_chain` passes 10 scenarios
(Pocket FIT UBWC, gralloc ignores the bit, Fold unreadable handle, no qcom_compressed, storage,
driver refuses the UBWC pitch, UBWC buffer 4 KiB short, gralloc refuses the bit, UBWC_PI,
BANNER_WSI_AHB_LINEAR=1), checking request, modifier, the explicit layout in the pNext chain and
the log line.

**Build:** Banners-Turnip run 34999563098 ✅ (headSha `0121416` verified; the patch applied in all
seven trees; new string guard `gralloc answered the UBWC request with a linear buffer` passed for
all eight drivers). Vendored here: the eight `libvulkan_freedreno_wayland*.so` only (SONAME/NEEDED
and ICD manifests unchanged; that run's libEGL, libGLESv2 and libdrm are byte-identical to the
vendored ones, its libgallium differs only in the embedded tree hash - kept). Commits `7f4f351d010`
(wayland-deps + TURNIP.md), `9830f557ba6` (ci versionCode 11 -> 12, one sentence per profile).
Layer run 35001539591 (dispatch on `feat/wayland-ubwc-v12`, headSha `9830f557ba6` verified):
✅ green, artifact `proton-arm64ec-sdk28` -> `proton-11.0-2-arm64ec.wcp`, sha256
`ef8df2d6b7083df24a9b9d65d3ffd3e29e28c31d8e945b12b4453d8d7545132e` (117,452,924 B), profile
`Proton 11.0-2.1-arm64ec` versionCode 12 (installs as `Proton-11.0-2.1-arm64ec-12` next to -11).
Against the v11 wcp (`7f58c98d…`): same 2,544 files; content differs only in the eight
`lib/libvulkan_freedreno_wayland*.so` (each carries `gralloc answered the UBWC request with a linear
buffer`, `on gralloc buffers: UBWC (QCOM_COMPRESSED)` and `unreadable gralloc handle from the %s
request`) and `profile.json`; 1,502 PE files differ only in their COFF/export/debug timestamps and
checksum (checked byte by byte), 22 import archives by timestamps (same sizes); every unix library
(win32u, winewayland, libEGL, libgallium, libwayland …) is byte-identical to v11.
No release, tag or catalog change; not staged anywhere.

**Device status: not device-proven.** The Pocket FIT test (device agent) should see `UBWC
(QCOM_COMPRESSED)` in both logs, a correct picture, and in `dumpsys SurfaceFlinger` the
`banner_wayland_game` layer as a UBWC format with composition DEVICE (rotated). The Fold should be
unchanged (linear, reason `gralloc handle layout unknown (2 fds, 34 ints)`) plus the two ints
dumps. Not merged into `feat/winewayland-desktop-11.0-2` (fast-forward after the device test).
The Fold's handle reader (WinNative's MIT `u_gralloc_aimapper.c`: IMapper5 via
`android_load_sphal_library`, COMPRESSION / PLANE_LAYOUTS metadata, QTI's two-plane UBWC
description collapsed to one QCOM_COMPRESSED plane) is NOT ported: it needs the SP-HAL mapper
loadable from a Wine process (unverified linker namespace) and a Fold to prove it; the ints dump
is the first step either way.

### Carry into v8 (all seven parents)
- The eight `android/wayland-deps/usr/lib/libvulkan_freedreno_wayland*.so` from Banners-Turnip
  `wayland` at or after `0121416` (the patch is applied on every build there, so any later run
  carries it; the v8 port vendors one run's full set as before).
- `android/wayland-deps/TURNIP.md`: the "Since versionCode 12 those gralloc buffers can be
  UBWC-compressed" paragraph and the Turnip provenance sentence (reword if v8 vendors one full run).
- No Wine source change. The CI commit `9830f557ba6` (versionCode 11 -> 12, description
  sentences) is NOT carried.

## 2026-09-14: the HDR10 EDID never reached DXGI — the virtual desktop's monitor had none (versionCode 11)

**Bug (user's Galaxy Fold, not rooted, versionCode 10, AIO Graphics Test HDR + God of War):**
`wine_debug.log` has the `winewayland: HDR10 monitor description (EDID) for Windows: max 1351
(EDID 1345.4), max frame-average 1351 (EDID 1345.4), min 0.0005 (EDID 0.0008) nits` line, yet
DXVK (2.4.1-gplasync and 3.1) logs `readMonitorEdidFromKey: Failed to get EDID reg key size` +
`DXGI: Failed to parse display metadata + colorimetry info, using blank.` and
`IDXGIOutput6::GetDesc1` returns the stand-ins 1499 / 799 / 0.01 nits with P3 primaries.

### Root cause (source; the logs agree) — line numbers are the unfixed `sysparams.c` at `aa9b9c02372`
- The session is a **Windows virtual desktop**: the session log says `Windows virtual desktop
  created by explorer.exe`, `size 1280x960` (GoW: 1280x720), and wine_debug.log has
  `winewayland: virtual desktop 0x10020 (0,0)-(1280,960) on the compositor`. So
  `DF_WINE_VIRTUAL_DESKTOP` is set and win32u's virtual-desktop path runs.
- `update_display_devices()` (`dlls/win32u/sysparams.c:2976-2988`, call at :2982): after the driver's
  `UpdateDisplayDevices` it calls `add_virtual_source()` when `is_virtual_desktop()`.
- `add_source()` (`sysparams.c:1965-1967`): "in virtual desktop mode, report all physical sources as
  detached". winewayland's monitor, the one carrying the v10 EDID, sits on that detached source.
- `add_virtual_source()` (`sysparams.c:2907`) builds the only active monitor from
  `struct gdi_monitor monitor = {0};` (:2911) and calls `add_monitor( &monitor, ctx )` (:2966):
  `edid_len` 0, so the path is `DISPLAY\Default_Monitor\0000&0000` (`add_monitor`,
  `sysparams.c:2104-2107`) and
  `write_monitor_to_registry` puts `BAD_EDID`, not `EDID`, in its `Device Parameters`
  (`sysparams.c:2027-2030`).
- `NtUserQueryDisplayConfig` returns only active monitors (`sysparams.c:3890`, `is_monitor_active`
  is FALSE for a detached source). DXVK's `getMonitorDevicePath` uses `QDC_ONLY_ACTIVE_PATHS` +
  `DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME` (`wsi_monitor_win32.cpp:226-285` at 2.4.1), finds
  the interface `\\?\DISPLAY#Default_Monitor#0000&0000#{e6f07b5f-…}`, opens its `Device
  Parameters` fine and finds no `EDID` value (`:288-291`), which is exactly the error on the Fold.
- Ruled out: (a) the EDID is built in the process that writes the display cache (the one line
  comes from explorer, right before its virtual-desktop line; other processes read the registry);
  (b) stale registry from earlier sessions: `prepare_devices()` empties `Enum\DISPLAY` on every
  forced update (`sysparams.c:927`, reached from `add_gpu`), so a prefix's old
  `Default_Monitor`/`BAD_EDID` keys never survive into a session; (e) setupapi/DisplayConfig: the key opened, only the value was missing.
- Why v10's off-device check passed: it proved the EDID bytes and the parsers, not win32u's
  virtual-desktop path, which only runs with explorer's desktop.

### Fix (`48fb81bc902`, `dlls/win32u/sysparams.c`)
- `struct device_manager_ctx` gets `primary_edid` / `primary_edid_len` / `primary_monitor_path`.
- `add_monitor()`: after the monitor is written, if the source is the primary one and nothing is
  kept yet, keep a copy of its EDID for this update.
- `add_virtual_source()`: `monitor.edid` / `edid_len` = that copy. The virtual monitor becomes
  `DISPLAY\WAY0001\0000&0000` with `Device Parameters\EDID` (256 bytes); the physical one keeps
  its EDID at `DISPLAY\WAY0001\0001&0000`. DisplayConfig's TARGET_NAME now says "Wayland" with
  `edidIdsValid`, and the preferred mode is the one it already was (the EDID's DTD is the
  output's current mode, which is also the largest virtual mode).
- `release_display_manager_ctx()` frees the copy.
- No EDID from the driver (every X11 session, and Wayland without the HDR variables) = nothing
  changes. hdr_enabled is not carried here (winewayland never set it; Proton's winex11 does from
  DXVK_HDR=1, so an unguarded carry would change X11 virtual desktops - left out on purpose).
  **Done in versionCode 13** (`efec2d86a45` + `4fc5781cf58`): winewayland sets it, and the carry
  onto the virtual monitor is guarded by the EDID really describing HDR10, so X11 sessions still
  change nothing. Carry those two with this commit.

### Diagnostics (MESSAGE level, reach wine_debug.log; only when the driver gave an EDID)
- winewayland `report_edid_handoff()` (`display.c`), once per process and again if the size or
  mode changes: `winewayland: explorer.exe (pid 00xx) built the screen's EDID for output <name>
  (<width>x<height>) and hands win32u 256 bytes`.
- win32u `report_monitor_edids()`, after `update_display_cache_from_registry()` in
  `lock_display_devices()`, still under the display lock; one line per process, repeated only
  when its text changes: `win32u: display update in explorer.exe (pid 00xx) on a virtual desktop:
  the screen's EDID (256 bytes) is on DISPLAY\WAY0001\0001&0000; active monitor
  DISPLAY\WAY0001\0000&0000: Device Parameters\EDID 256 bytes;` — the size is read back from the
  registry. The broken case reads `active monitor DISPLAY\Default_Monitor\0000&0000: Device
  Parameters\EDID MISSING, 0 bytes;`. explorer may print one earlier line without "on a virtual
  desktop" (its first update can run before it switches to the new desktop).
- Expected on the Fold with the fix: those lines, no `readMonitorEdidFromKey` / `using blank`
  in the dxgi log, and GetDesc1 MaxLuminance = MaxFullFrameLuminance = 1345.43, MinLuminance
  0.0008 (the app now sends 0.0005 nits → CTA min code; v10's harness shows DXVK passes it through),
  primaries (0.6797, 0.3203) (0.2646, 0.6904) (0.1504, 0.0596).

### Build
- Branch `fix/wayland-hdr-edid-v11` off `fix/wayland-hdr-edid` (`aa9b9c02372`): `48fb81bc902`
  win32u fix + both diagnostics, `e61db998c5d` ci versionCode 10 → 11 + the HDR sentence now names
  the virtual desktop's monitor (both profiles). Syntax-checked (gcc; clang
  `--target=aarch64-linux-android28`, `-Wall -Wextra`): no error or new warning in either file.
- CI run 34925241464 (workflow_dispatch, headSha `e61db998c5d` verified), artifact
  `proton-arm64ec-sdk28` → `proton-11.0-2-arm64ec.wcp`. **Green.** wcp sha256
  `7f58c98d4482e54d6acbb588e65b93efade4a7cb3e152e19227dd2312620309e` (117,471,828 B), profile
  `Proton 11.0-2.1-arm64ec` versionCode 11 with the reworded HDR sentence. Against the v10 wcp:
  same file list; content differs only in `lib/wine/aarch64-unix/win32u.so` (carries
  `win32u: display update in %s (pid %04x)%s: …` and ` active monitor %s: Device
  Parameters\EDID %s%u bytes;`), `lib/wine/aarch64-unix/winewayland.so` (carries
  `… built the screen's EDID for output %s (%dx%d) and hands win32u %u bytes`) and
  `profile.json`; every PE ≤ 4 bytes and the import archives by timestamps only (same sizes).
  libwayland, the Turnips and every other unix library are byte-identical to v10.
- No release, tag or catalog change. No device test by this change's author (Fold = user; Pocket
  FIT off-limits this round).

### Carry into v8 (all seven parents)
- Cherry-pick `48fb81bc902` together with v10's `612401793ce`: the EDID is useless without it on
  any virtual-desktop session (every Bannerlator Wayland session). `dlls/win32u/sysparams.c` only
  for the fix; the `display.c` hunk only adds the report and sits inside v10's hunk, so it goes
  after `612401793ce`. The win32u part is driver-neutral and applies to the X11-only parents as
  well; check `add_virtual_source()` still builds its monitor from a zeroed `gdi_monitor` there.
- The CI commit `e61db998c5d` is NOT carried.

## 2026-09-14: HDR10 — the monitor's EDID for Windows (versionCode 10); VK_EXT_swapchain_colorspace was never hidden

**Ask:** make HDR10 (already proven on the user's Galaxy Fold with DXVK v3.1 on versionCode 9)
work with DXVK 2.x and vkd3d-proton, and tell Windows the screen's real brightness. Evidence
given: with DXVK 2.4.1-gplasync God of War shows no HDR option and `GoW_d3d11.log` prints
`VK_EXT_swapchain_colorspace — extension supported : 0`; `strings` finds the name twice in the v9
`winevulkan.dll` and never in `winevulkan.so`.

### 1. VK_EXT_swapchain_colorspace: not missing, and not what DXVK's HDR depends on (no code change)
- **Wine exposes it already, at the instance level where the spec puts it** (`vk.xml:21961`,
  `type="instance"`). The host→guest instance filter is in win32u (`vulkan.c:4144-4180`,
  `vulkan_init_once` → `client_extensions`) and the PE side (`winevulkan/loader.c:358-365, 473-516`),
  not in `winevulkan.so`, which has no extension-name table at all. v9 binaries from the device:
  `VK_EXT_swapchain_colorspace` ×1 in `win32u.so`, ×2 in `winevulkan.dll`, ×0 in `winevulkan.so` —
  exactly the counts of `VK_EXT_surface_maintenance1`, which DXVK's own "Enabled instance
  extensions" list shows reaching the game. Turnip reports both from the same
  `#ifdef TU_USE_WSI_PLATFORM` block (`tu_device.cc:185-187` at 7cda7850). make_vulkan skips neither.
- **DXVK looks for it in the DEVICE list** — since commit `4335eccae9` (2022-12-18): at `4c0cbbef`
  (= the user's v2.4.1-293-gplasync) `dxvk_extensions.h:315` (DxvkDeviceExtensions),
  `dxvk_adapter.cpp:892-893` (`m_deviceExtensions` = vkEnumerateDeviceExtensionProperties, :728),
  enabled in vkCreateDevice (:999, :1110-1111) and logged (:1286-1287). No Mesa driver can list an
  instance extension there, so that line says 0 on every Mesa system — including the Fold's
  **working** v3.1 HDR session (`extSwapchainColorSpace : 0` in `gow2/GoW_d3d11.log`).
- **Nothing in DXVK reads the flag.** Grep of `src/` in v2.1, 2.2, 2.3, 4c0cbbef, 2.5.3, 2.6.2,
  2.7.1, 3.1: only the adapter/device-info files that enable and log it. DXGI HDR is
  `dxgi.enableHDR` (DXVK_HDR=1) → `DXGI_OUTPUT_DESC1.ColorSpace` (`dxgi_output.cpp:234`,
  `dxgi_monitor.cpp:91-95`), plus `CheckColorSpaceSupport` → `Presenter::supportsColorSpace` →
  `vkGetPhysicalDeviceSurfaceFormatsKHR` (`dxvk_presenter.cpp:384-397, 429-467`), which Mesa's
  Wayland WSI fills from `wp_color_manager_v1` whatever the instance enabled
  (`wsi_wl_display_determine_colorspaces`). vkd3d-proton v3.0.1 never names the extension:
  `dxgi_vk_swap_chain_CheckColorSpaceSupport` (`swapchain.c:1266`) reads surface formats too, and
  D3D12 titles get their DXGI output from DXVK's `dxgi.dll`.
- So a winevulkan change could only flip DXVK's log line to 1 (by listing an instance extension
  among device extensions and stripping it again before the host's vkCreateDevice) — it cannot
  change what a game sees. Not done.
- **Why GoW showed no HDR option on 2.4.1 is NOT explained by DXVK's source:** GetDesc1, the
  monitor colour-space logic, ValidateColorSpaceSupport, D3D11 CheckColorSpaceSupport and
  `env::getEnvVar` are identical between 4c0cbbef and v3.1. Open; needs a controlled Fold A/B
  (same shortcut, DXVK 2.4.1 vs v3.1, `DXVK_HDR=1` confirmed in the session log's
  `session environment:` line, game restarted after the change).

### 2. The EDID (what versionCode 10 adds)
Before: `winewayland.drv` gives its monitor no EDID, win32u writes `BAD_EDID`
(`sysparams.c:2024-2030`), DXVK's `readMonitorEdidFromKey` fails ("Failed to get EDID reg key
size" in every Fold dxgi log) and `NormalizeDisplayMetadata` (`wsi_edid.h:38-56`) substitutes
1499 / 799 / 0.01 nits (HDR) or 270 / 270 / 0.5 (SDR).

Now (`dlls/winewayland.drv/wayland_edid.c` + `.h`, `display.c`): when any of
`BANNER_WAYLAND_HDR_MAX_NITS`, `_MAX_AVG_NITS`, `_MIN_NITS` (decimal nits, the app's per-session
contract) parses, `wayland_add_device_monitor` hands win32u a 256-byte EDID; none set = no EDID,
exactly as before; X11 untouched (win32u rebuilds the monitor keys on every device update, so an
X11 session never inherits it). One `winewayland: HDR10 monitor description (EDID) for Windows:
max … (EDID …), max frame-average …, min … nits` line per process; a malformed value is named
and ignored.
- Base block, EDID 1.4: vendor `WAY` (unassigned in hwdata's PnP list), product 1, 2026, digital
  10 bpc DisplayPort, gamma 2.2, Display P3 primaries + D65 (what DXVK assumes for HDR without an
  EDID), preferred DTD = the output's current mode (CVT-RB proportions; size clamped to 4095,
  pixel clock to 655.35 MHz), name "Wayland", two dummy descriptors, one extension.
- CTA-861 rev 3 block: Colorimetry (BT.2020 RGB) + HDR Static Metadata (traditional SDR + SMPTE
  ST 2084 EOTFs, type 1; desired content max, max frame-average, min luminance). CTA-861.3 coding:
  max = 50·2^(cv/32) (cv = round(32·log2(n/50)), 1..255, 0 = not given), min = max·(cv/255)²/100;
  trailing absent values left out, the min only with a max (libdisplay-info fails a min without
  one). 1351 nits → cv 152 = 1345.43 nits (the coding's ~2 % step); min 0 → "not given".
- win32u picks up id `WAY0001`, name "Wayland" and the preferred mode from it.

### Verified off-device against the real parsers (`scratchpad/hdr10/harness/roundtrip.cpp`)
The committed `wayland_edid.c`, linked with libdisplay-info at `275e6459` (the submodule both
DXVK 4c0cbbef and v3.1 build), DXVK's own `src/wsi/wsi_edid.cpp` (byte-identical in both) and
win32u's `get_monitor_info_from_edid` extracted verbatim from `sysparams.c`: 13 scenarios, 0
failures — no libdisplay-info conformance failure (this revision returns "" for none; its own
reference EDIDs print the same empty "FAIL" in `di-edid-decode`), `supportsST2084 = 1`, DXVK's
luminances equal the encoder's decode, all 255 max codes and 255×15 min codes round-trip, the
env parser takes "1351", " 0.05 ", "0", "1." and rejects "", "-1", "1e3", "12,5", "inf", "nan".
Fold contract (1351 / 1351 / 0) → `DXGI_OUTPUT_DESC1` with DXVK_HDR=1: MaxLuminance 1345.43,
MaxFullFrameLuminance 1345.43, MinLuminance 0.01 (DXVK's own stand-in for "not given"), primaries
(0.6797, 0.3203) (0.2646, 0.6904) (0.1504, 0.0596) white (0.3125, 0.3291). Both changed files
syntax-check clean with gcc and with clang `--target=aarch64-linux-android28`, `-Wall -Wextra`.

### Build
- Commits on `fix/wayland-hdr-edid` (off `459bf7a8a7c`): `612401793ce` winewayland EDID,
  `05569528f36` ci versionCode 9 → 10 + one description sentence (both profiles).
- CI run 34909438885 (workflow_dispatch, headSha `05569528f36` verified), artifact
  `proton-arm64ec-sdk28` → `proton-11.0-2-arm64ec.wcp`. **Green.** wcp sha256
  `31d165a58026de7d94f4e6a097c3b918bd72e1256793b70cbccd04c5f56f8a60` (117,477,213 B), profile
  `Proton 11.0-2.1-arm64ec` versionCode 10 with the HDR sentence. Against v9: `winewayland.so`
  differs (carries the three variable names and the `HDR10 monitor description (EDID)` line),
  `win32u.so` and `winevulkan.so` byte-identical, `winevulkan.dll` 4 bytes (PE timestamp and
  checksum, same size).
- No Pocket FIT test this round (lead, 2026-09-14): the user tests on the Fold from the lead's
  Gamehub-Components test release. Not merged into `feat/winewayland-desktop-11.0-2` yet
  (fast-forward once the Fold has spoken, as with versionCode 9).

### Carry into v8 (all seven parents)
- **Superseded in part by versionCode 11:** on a virtual desktop (every Bannerlator Wayland
  session) this commit alone never reaches DXGI; carry it together with `48fb81bc902`.
- Cherry-pick `612401793ce` (`dlls/winewayland.drv/wayland_edid.c` + `wayland_edid.h` new,
  `display.c` hunk, one `Makefile.in` SOURCES line). `git merge-tree` against the parents as they
  are today: clean on proton_11.0, proton_11.0-2, proton_11.3-GE, proton_11.5-GE, proton_11.6-GE;
  on proton_10.0 and proton_10.34-GE only the `Makefile.in` line conflicts (its context line
  `wayland_data_device.c` does not exist there — add `wayland_edid.c` to SOURCES by hand),
  `display.c` merges. It goes on top of the Wayland support itself, like everything on this list.
- The CI commit `05569528f36` is NOT carried.
- Optional, not in this build: Proton's winex11 sets `gdi_monitor.hdr_enabled` from DXVK_HDR=1
  (DisplayConfig ADVANCED_COLOR_INFO → advancedColorSupported/Enabled); winewayland never does.
  Parity would be one line in `wayland_add_device_monitor` (suggest: only when the EDID is given
  AND DXVK_HDR=1). Not needed by DXVK or vkd3d-proton. **Done in versionCode 13**
  (`efec2d86a45` + `4fc5781cf58`), with exactly that gate plus the EDID having HDR10 metadata, and
  with the flag carried onto the virtual desktop's monitor: it is what makes a game's *own* HDR
  option selectable. Carry those two together with this commit and `48fb81bc902`.

## 2026-09-14: native OpenGL black with sound on phones with no DRM node (versionCode 9)

**Bug (user's own Adreno 840, standard Bannerlator, `Proton-11.0-2.1-arm64ec-8`, Turnip
a8xx-white; also reported on Adreno 830):** Wizardry (native OpenGL) plays sound behind a
black window. Session log: `feedback ready: 8 format/modifier pairs, main device 0:0`, then
`0 GPU frames from games | ~293 window redraws` every 10 s. Vulkan/DXVK games are fine.

### Root cause (Mesa source at 7cda7850, the tree libEGL/libgallium come from)
1. The phone gives apps no `/dev/dri` node, so the compositor's dma-buf feedback names
   `main_device` 0:0 (`dmabuf_render_node()` finds nothing to `stat`).
2. `default_dmabuf_feedback_main_device()` → `loader_get_render_node(0:0)` → NULL →
   `fd_render_gpu` stays -1 (this build has no wl_drm fallback: `HAVE_BIND_WL_DISPLAY` off).
3. `dri2_initialize_wayland_drm()` builds the kopper screen anyway (fd -1 means "no DRM" in
   `kopper_init_screen` → `pipe_loader_vk_probe_dri` → `zink_create_screen`), then
   **`dri2_setup_device(disp, false)` (platform_wayland.c:2752) fails**:
   `loader_is_device_render_capable(-1)` is false and `dri_query_compatible_render_only_device_fd(-1)`
   gives -1 (egl_dri2.c:862-868).
4. `eglInitialize` retries with `Zink=FALSE, ForceSoftware=TRUE` → the wl_shm software path,
   which this build cannot draw (gallium = zink only, no LLVM) → black.
The Pocket FIT never hit it: its `/dev/dri/renderD128` (msm display node) opens and is
describable, so step 2 gets a real fd.

### Fix (Banners-Turnip `wayland`, `patches/wayland/egl_wayland_no_drm_node.py`)
In `dri2_initialize_wayland_drm()` only: when the display is kopper and there is no usable
render node (none from the feedback, or one `loader_is_device_render_capable()` /
`_eglFindDevice()` cannot place — exactly when the stock `dri2_setup_device` call would fail),
close the fd, build the kopper screen with fd -1 (zink on the one Vulkan device, as X11's
`LIBGL_KOPPER_DRI2` path does), and skip `dri2_setup_device` (no EGLDevice, as Android's
pure-swrast path does; the software EGLDevice would make win32u report the display as
unaccelerated). A node that works keeps the stock path. One warning line each on the no-node
path and on the software fallback. See `android/wayland-deps/TURNIP.md`.

### What changed in this repo
Only `android/wayland-deps/usr/lib/libEGL.so.1` (sha256 `a9b5f3ad…55519`, from Banners-Turnip
`wayland` `644f1a5c`, run 34877806759, artifact `banner-mesa-wayland`). Every other library -
the eight Turnips, libgallium (Zink), libGLESv2, libdrm, libwayland - stays the bytes of
versionCode 7's run 34804055227: same Mesa commit, same flags, and the new libEGL's imports and
the old libgallium's exports are identical symbol sets (checked with `nm -D`), so nothing on the
Vulkan side can differ from versionCode 8. Plus TURNIP.md and the versionCode/description.

### Build and device proof
- CI run 34879146480 (workflow_dispatch on `fix/wayland-gl-no-drm-node`, headSha `aeaff6e8ee3`),
  artifact `proton-arm64ec-sdk28` → `proton-11.0-2-arm64ec.wcp`, sha256
  `bce6e7cc0e8b4251e9cc1b58a11c3efe6a485857ec02b270a7bb9a61940e6961` (117,467,136 B), profile
  `Proton 11.0-2.1-arm64ec` versionCode 9. Full diff against the v8 wcp: `lib/libEGL.so.1`,
  `profile.json`, and build timestamps in PE/.a files (every PE diff ≤ 12 bytes, same size); every
  unix-side library, the Turnips and winevulkan included, is byte-identical.
- **User's Adreno 840 (standard app, pre-release 7): v9 renders Wizardry** ("it works on a840").
  The wcp is attached to pre-release 7 as `proton-11.0-2.1-arm64ec-wayland-v9.wcp` in place of v8.
- Pocket FIT (Adreno 750, node present), pre-release-7-equivalent app, throwaway container on -9:
  Wizardry `feedback ready: 8 format/modifier pairs, main device 226:128` → `is presenting GPU frames
  through Wayland: 1280x720, format XR24, qcom_compressed` → `300 GPU frames from games` per 10 s;
  Wine `+wgl`: `accelerated: 1`, `zink Vulkan 1.4(Turnip Adreno (TM) 750 (MESA_TURNIP))`, no
  `wayland-egl:` line (stock DRM path). Insane 2 (DXVK) `143.4 fps | ~16k GPU frames` per 10 s.
- Pocket FIT with the compositor forced to advertise `main device 0:0` (debug app switch
  `BANNER_WAYLAND_NO_RENDER_NODE=1`, the A840's condition): on -8, `0 GPU frames from games | 300
  window redraws`, Wine's display EGLDevice = Mesa's software one (`accelerated: 0`) = black; on -9,
  `MESA-EGL: warning: wayland-egl: the compositor names no DRM render node this process can open;
  running zink on the Vulkan device without one`, display device `0x0` (no EGLDevice), `accelerated:
  1`, `zink … Turnip Adreno (TM) 750`, `300 GPU frames from games` per 10 s, HUD "OpenGL 30.0 fps".

### Carry into v8 (all seven parents)
- `android/wayland-deps/usr/lib/libEGL.so.1` from Banners-Turnip `wayland` at or after
  `644f1a5c` (the patch is applied on every build there, so any later run carries it; the v8
  port vendors one run's full set as before).
- `android/wayland-deps/TURNIP.md`: the "Since versionCode 9 the render node is optional"
  paragraph and the libEGL provenance sentence (reword the latter if v8 vendors one full run).
- No Wine source change. The CI commit (versionCode 8 → 9, description sentence) is NOT carried.

## 2026-09-14: XP Start menu "Control Panel" did nothing (versionCode 8)

**Bug (user, 2026-09-14 08:31, container 7 on `Proton-11.0-2.1-arm64ec-7`):** in the
Wine XP desktop, Start menu → Control Panel opens nothing, and the entry has a plain
folder icon. People use Control Panel → Add/Remove Programs to uninstall software.

### Root cause (from source, and from the user's own session logs)
1. The XP Start menu entry was `xp_add_folder(CSIDL_CONTROLS)`, which runs
   `ShellExecuteEx(SEE_MASK_IDLIST)` on the Control Panel pidl. The classic Start menu
   does the same.
2. shell32 can't run a virtual folder in-process. `SHELL_translate_idlist` starts
   `explorer.exe ::{20D04FE0-…}\::{21EC2020-…}` instead.
3. In that child, `make_explorer_window` ran `GetFullPathNameW` on the name. ntdll
   treats any string whose second character is `:` as drive-relative, so the name
   became `::\{20d04fe0-…}\::{21ec2020-…}`. `ILCreateFromPathW` could not parse it,
   explorer logged `Failed to create PIDL`, and the process exited without opening
   a window. Upstream Wine master has the same code, so this is a stock Wine bug,
   not something the XP desktop introduced.
4. The icon: the prefix's `HKCR\CLSID\{21EC2020…}` has only a `LocalizedString`
   (from `shell32.rgs`) and no `DefaultIcon`. `folders.c` therefore falls back to
   `IDI_SHELL_FOLDER`.

Evidence from the user's 08:30:18 session:
- `Wayland-logs/wayland-2026-09-14_08-30-18.log`: the start menu closes at
  08:30:26.454. A second `explorer.exe (pid 19233)` connects at 08:30:26.532 and
  disconnects at 08:30:26.590. It never opens a window.
- `bannerlator/wayland/previous/2026-09-14_08-30-26/wine_debug.log`:
  `00f4:fixme:exec:SHELL_execute flags ignored: 0x00000004`,
  `01a8:fixme:shell:DllGetClassObject failed for CLSID={00000000-…}`,
  `01a8:err:explorer:make_explorer_window Failed to create PIDL for L"::\\{20d04fe0-3aea-1069-a2d8-08002b30309d}\\::{21ec2020-3aea-1069-a2dd-08002b30309d}".`

### Fix (commits on this branch)
- `2b01f10fcd7` explorer: open `::{CLSID}` shell namespace paths instead of mangling
  them (`programs/explorer/explorer.c`, `make_explorer_window`). Names that start
  with `::` are now passed through unchanged. File paths still go through
  `GetFullPathNameW`.
- `6b8452edae4` explorer: the XP Start menu Control Panel entry now starts
  `control.exe`, with shell32's Control Panel icon (`programs/explorer/startmenu.c`,
  new `xp_add_control_panel()`, icon id 36). The icon is the one control.exe's own
  window uses. The `CSIDL_CONTROLS` folder is kept as a fallback if control.exe is
  missing.
- `f71f4a2a583` ci: versionCode 7 → 8, plus one sentence added to the description.
  The layer installs as `Proton-11.0-2.1-arm64ec-8`, next to `-7`.

CI run 34846379023 (workflow_dispatch; headSha `f71f4a2a583` verified), green.
The wcp is `proton-11.0-2-arm64ec.wcp` from artifact `proton-arm64ec-sdk28`:
sha256 `de8dcad0227dbfb0f32fd766a6c34497bd69787b9a62459657a146895a4c5d09`,
117,465,780 bytes. profile.json reads Proton `11.0-2.1-arm64ec`, versionCode 8.
explorer.exe carries the UTF-16 string `control.exe` once in each of aarch64-windows
and i386-windows; v7 carries it zero times. The XP taskbar marker is unchanged.
No release, tag or catalog change.

### Device proof (AYANEO Pocket FIT, Bannerlator pubg `com.tencent.ig`, Wayland sessions)
The test layer was hand-installed into its own slot, `contents/Proton/11.0-2.1-arm64ec-8`,
with app ownership and label. `-7` was left untouched. Both tests ran on throwaway
containers, each created through the app UI (Wayland, Turnip r4, FEX 2609-stable).

**Before: `Proton-11.0-2.1-arm64ec-7`, container "ZZ CPL v7"**
- The Start menu shows Control Panel with a plain folder icon, same as the user's
  screenshot.
- Clicking it: at 09:04:58.667 an `explorer.exe (pid 11470)` connects. It disconnects
  at .720 and opens no window. Wine log:
  `01ac:err:explorer:make_explorer_window Failed to create PIDL for L"::\\{20d04fe0-…}\\::{21ec2020-…}"`.
- The same layer run from Start-menu `.bat` launchers (the Run box can't be typed into
  from the test harness):
  - `control` opens "Wine Control Panel" (Add/Remove Programs, Display Settings,
    Game Controllers, Internet Settings).
  - `control appwiz.cpl` opens "Add/Remove Programs".
  - `rundll32 shell32.dll,Control_RunDLL` opens "Wine Control Panel".
  - `explorer ::{20D04FE0-…}\::{21EC2020-…}` and `explorer ::{20D04FE0-…}`: explorer
    exits after about 56 ms with the same ERR. No window.
- The other XP Start menu entries all open: My Documents, My Pictures and My Music
  (Wine Explorer windows), My Computer (wfm), Task Manager, Wine Configuration, and
  the All Programs entries.

**After: `Proton-11.0-2.1-arm64ec-8`, container "ZZ CPL V8"**
- The Start menu shows Control Panel with the shell32 Control Panel icon (a grey panel
  with the Wine glass), in the same style as the Task Manager icon below it.
- Clicking it: at 09:38:50.001 `control.exe (pid 23917)` connects, and at 09:38:50.096
  the "Wine Control Panel" window opens (Add/Remove Programs, Display Settings, Game
  Controllers, Internet Settings). No ERR in the log.
- Double-clicking Add/Remove Programs in that window: `rundll32.exe (pid 24125)`
  opens the "Add/Remove Programs" window (Install…, list, Modify…/Remove).
- `explorer ::{20D04FE0-…}\::{21EC2020-…}` opens a "Control Panel" Explorer window
  listing the four applets with their comments. `explorer ::{20D04FE0-…}` opens
  "My Computer" (Control Panel, C:, D:, E:, F:, Z:). No PIDL ERR either time.
- Regression check: My Documents still opens its Explorer window.

The explorer fix covers every `ShellExecute` of a virtual folder: the XP My Computer
entry when wfm.exe is absent, the Control Panel fallback when control.exe is missing,
and any shortcut or program that opens a `::{CLSID}` folder. The classic Start menu
doesn't need it, because its Control Panel is a cascade (see below).

### Did it ever work before v7? Yes. v7 broke it.
Device check on the pre-XP layer `Proton-11.0-6-arm64ec-6` (X11 container "ZZ CPL v6",
stock Wine classic taskbar):
- Start → **"Control Panel ▶" is a submenu** listing Add/Remove Programs, Display
  Settings, Game Controllers and Internet Settings.
- Clicking Add/Remove Programs opens the "Add/Remove Programs" window. It runs through
  the Control Panel folder's own execute hook, so the only log line is
  `SHELL_execute flags ignored: 0x00000004` and no explorer child starts.

Stock `add_shell_item` turns a folder item into a cascade and never executes the
folder itself. v7's XP Start menu replaced that cascade with one flat item that
executes the Control Panel folder. That is the one path that reaches the stock
`GetFullPathNameW` bug in explorer.c, so the user's "since v7" is exact. The explorer
bug itself is old (the v6 source `349547afa45`, Proton 9 `feat/p9-combined` and
upstream master all have it), but the stock menu never hit it.

### Carry into v8 (all seven AIO layers)
All seven v7 parents have both halves of the bug: the XP entry via
`xp_add_folder(CSIDL_CONTROLS)` and the explorer `GetFullPathNameW` call. On the
five Wine-11 parents, `programs/explorer/{explorer,startmenu}.c` are byte-identical
to this branch before the fix. On the two Wine-10 parents, only unrelated
explorer.c hunks differ. A `git merge-tree` simulation cherry-picks both commits
cleanly onto all seven. When Wayland is folded into v8, carry:
`2b01f10fcd7` (explorer.c) and `6b8452edae4` (startmenu.c).
Do not carry the CI commit; each parent stamps its own versionCode.

Optional follow-up (not done): shell32 still has no `DefaultIcon` for the Control
Panel CLSID, so the classic menu and Explorer's My Computer view still draw it as a
folder. The fix would be a `folders.c` fallback to `IDI_SHELL_CONTROL_PANEL`.

## 2026-10-02 — v10: Media Foundation cutscene fixes (Ninja Gaiden Sigma)

Ninja Gaiden Sigma's opening has two movies read through the Media Foundation Source
Reader (sync reads, explicit stream indices, NV12 + PCM output). The first
(`ninja_vi.wmv`, WMV3) played; the second (`ninja_gaiden.wmv`, WMV2 1080p, two WMA
audio streams) played black with sound.

Cause: avdec_wmv2 gives every output frame a buffer duration of 2:09.59. Since video
samples take the decoder's output PTS (NelloKudo `924c786e74`), they took that duration
too. The game waits for time + duration before reading the next video sample, so it
read one frame and then only audio.

Fix: on video output, accept the decoder duration only when it is at most four frame
intervals (1 s without a frame rate); otherwise use the output frame interval, or the
preserved input duration. Device-proven: both movies play to the title screen (movie 2:
2942 of ~2944 frames delivered, natural EOS on video and audio).

Carried in v10:
- NelloKudo's Media Foundation series from the Cachy branches (13 commits: topology
  restore, mfsrcsnk absolute seek, decoded audio alignment, WMA decoder types/output
  sizing, native compressed reader streams + retry with decoded streams, WMA
  bits-per-sample, winedmo extension-less URLs, ANY_STREAM ready-stream preference,
  video PTS from decoder output).
- GE's media-converter env gate (`41b4951fd5`): converters register only when their
  `PROTON_*` env var is 1.
- mfreadwrite: record the delivered timestamp on async SAMPLE_READY responses.
- winegstreamer: ignore an implausible decoder duration on video output samples.

Tested on the v9-fmv staging branch (test layer versionCode 90). Not regression-tested
against other games' cutscenes yet.
- 2026-10-03 `staging/proton_11.0-2/v12-winedmo` 15c8b5ef8d3 (run 37145799734): `--with-ffmpeg` + FFMPEG_CFLAGS=-I$deps/include, versionCode 12. Device evidence (NGS, vc11): `warn:dmo:DllMain Failed to init unixlib, status 0xc0000139`, `winedmo_demuxer_check returning 0xc000007a` — winedmo.so had no `__wine_unix_call_funcs` because HAVE_FFMPEG was never defined (both build steps passed `--without-ffmpeg`); only the NEEDED libav* came from FFMPEG_LIBS. The CI termuxfs installs Termux ffmpeg (FFmpeg 8 = .so.62, same as the imagefs). Every other layer branch has the same two lines.
- 2026-10-03 15:41 vc12 (45001126, run 37147229878) device-proven: winedmo inits (`FFmpeg support: avutil 60.8.100 avformat 62.3.100`), demuxes both NGS Σ WMVs, intro plays at 60 fps with the app's FFmpeg row; default (key absent / DWORD 0) stays winegstreamer.
