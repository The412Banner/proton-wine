# DirectAudio — Progress Log / Checkpoint

## 2026-08-14 — v1.3.1: live in-game config ("mailbox")

`BANNER_AUDIO_DIRECT_RUNTIME=<file>` opts into live control: a flat KEY=VALUE mailbox (MS/MAXMS/PERF) the host
rewrites while the game runs. A 1 s watcher thread (off the audio path) stats it and, on change, re-reads +
rebuilds the stream via the existing reopen worker -> a setting lands WITHOUT relaunch (~77 ms, inaudible).
Overrides applied at the top of mixer_open_stream so they win over launch config and re-apply on every reopen;
values <=0 / absent keys revert to launch config. Unset = feature off (byte-identical to v1.3.0).

Root-caused + fixed a mailbox PERF bug found on device: da_apply_runtime_overrides set mx->perf = raw 0/1/2, but
the stream needs the AAudio enum (NONE=10/POWER_SAVING=11/LOW_LATENCY=12). A PERF=0 write opened an invalid perf
mode -> data callback stalled -> watchdog reopen loop -> dead audio. Now mapped like read_config_from_env; also
reset overrides on each read so an absent key reverts. Device stress-proven: PERF toggles + bg/fg cycles, 0 stalls.
(POWER_SAVING is unstable for DirectAudio under box64/FEX and is handled host-side, not in the driver.)

---

# DirectAudio — Progress Log / Checkpoint

## 2026-08-14 — feat/live-runtime-config: live in-game "mailbox" (built, device test pending)

Config is env-read once at attach, so cog changes need a relaunch. This branch adds a live channel:
`BANNER_AUDIO_DIRECT_RUNTIME=<file>` names a flat KEY=VALUE mailbox (MS/MAXMS/PERF). A 1 s watcher thread
(off the audio path) stats it; on change it re-reads and calls mixer_request_reopen, so a setting lands
WITHOUT relaunch (the reopen worker rebuilds ~77 ms, inaudible). Overrides applied at the top of
mixer_open_stream, so they win over launch config and re-apply on every reopen. Unset = feature off
(byte-identical to v1.3.0). Keys<=0 revert to launch config. Build: run 31842661853 (sdk28/35).

TEST (device): hot-swap the build, set RUNTIME=<path> + LOG=1 on a shortcut, launch, then
`printf 'MS=8\n' > <path>` (chown to guest uid) and watch logcat for `runtime: reload` + a new `open:` at
8 ms — no relaunch. NEXT: app-side writeDirectAudioRuntime() (sibling of applyAlsaAudioConfig, writes the file
instead of a JNI call) hooked into the in-game cog.

---

# DirectAudio — Progress Log / Checkpoint

## 2026-08-14 — CHECKPOINT: **v1.3.0 shipped** (resume here)

`main` = `31f42aba4`, tag `directaudio-v1.3.0` (Wine 11.0). GoW is long solved; the driver is
a stable, low-latency, self-healing shared-mixer backend.

### Shipped since the 2026-08-12 checkpoint below
- **v1.2** (`directaudio-v1.2`) — `mmdevapi_midi_n` spin fix: `unix_midi_get_driver` returns
  `L"alsa"` so mmdevapi delegates MIDI to winealsa instead of DirectAudio becoming its own
  MIDI driver and spinning a whole CPU core. GoW renders. Returned a core to **every** DA title.
- **v1.2.1** (`directaudio-v1.2.1`) — dead-AAudio-callback watchdog: a background/foreground
  cycle could starve the stream so AudioTrack self-disabled and the data callback never
  resumed (no error raised) → permanent silence. Per-period `mixer_watchdog()` rebuilds after
  1 s of silence-with-voices; `DA_EVENT` reopen lines are on in the release build.
- **v1.2.2** (`directaudio-v1.2.2`) — adaptive **decay** (a grown buffer comes back down when
  calm), **millisecond latency** knobs (`_MS`/`_MAXMS`, `_PERIOD_MS`), the rest of the tuning
  exposed to the environment, and the new **12 ms / 33 ms shipped default** (was 62.5 ms/83 ms).
- **v1.3.0** (`directaudio-v1.3.0`) — the four post-v1.2.2 wins:
  1. **Soft-knee limiter** on the 5.1→stereo downmix (was clipping at ~2.41× full scale) —
     roadmap item "downmix headroom", **done**.
  2. **Exclusive-mode honesty** — stopped claiming EXCLUSIVE support the driver never provided;
     the open log now reports granted vs requested sharing mode (`sharing req=/got=`).
  3. **`daprobe`** — a WASAPI capability-probe exe on its own workflow (accepted/native/rendered).
  4. Sharing `req=/got=` logging.

### Device-proven at the new default
7-game sweep: 5/7 titles hold the 4 ms floor (**25 ms total**); GoW 384 fr / 29 ms, DiRT 3
576 fr / 33 ms, zero underruns, no MIDI spin anywhere. New-default (12 ms) validated on GoW at
light load (voices≈1–2, xruns 0, decay idle); **high-voice combat stress not yet captured**.

### Three-engine probe baseline (`daprobe`)
DA: 10 ms period, renders native **48 kHz** (no resample). winealsa: honest stereo-only, ≤48 kHz,
10 ms period. winepulse: 20 ms period, **44.1 kHz → resamples** 48 kHz audio. See README + the
`reference_directaudio_probe_three_engine_baseline` note.

### Roadmap — where it stands
Done: downmix headroom (v1.3.0). Next: **route-change format handling** → **real surround**
(Android Spatializer 5.1) → **mic capture**. Plus: Mali verification, a lower host preset rung.

### Branches
`main`/tags = ship code. `diagnostics` = main + 1 instrumentation commit (23 logcat probes),
kept rebased on main — `git rebase main` after each release, then dispatch its build.

---

## 2026-08-12 — CHECKPOINT (resume here)

Native Wine mmdevapi backend → Android AAudio (one shared AAudio output + in-process
mixer; each guest WASAPI stream = a "voice"; a per-stream timer thread signals the
client's event handle each period).

### Shipped
- **v1.1** (tag `directaudio-v1.1`): audio switching (broadened AAudio error-callback
  recovery + stream-identity guard + no-lost-wakeup reopen gate) and guarded
  `AAudioStreamBuilder_setUsage` (weak symbol, safe on minSdk-26). Merged upstream into
  GameNative via joshuatam/GameNative#7 -> utkarshdalal/GameNative#1806.
- **v1.1.1** (tag `directaudio-v1.1.1`, main): fix `release_stream` teardown deadlock.
  A game that creates+starts+RELEASES a transient event-driven stream during init
  (God of War via FAudio/XAudio2) deadlocked its main thread on the unbounded
  `NtWaitForSingleObject(timer_thread)` join -> black-screen hang while audio played.
  Fix: atomic `please_quit` + 500 ms bounded join; on timeout unblock + leak rather than
  deadlock/UAF. Device-proven to clear THIS hang.

### Branches
- `main` / tags = ship code (clean, no logging).
- `feat/diagnostics` = latest + all logcat probes (tag `DirectAudio`, no WINEDEBUG). Not
  for release; hot-swap onto a container to trace on-device. Keep rebased on main.

### Open: God of War second stall (unsolved)
GoW gets past the release deadlock, then its MAIN thread hangs again in
`NtWaitForSingleObject` — waiting on a GoW-INTERNAL object (it makes zero driver calls
after FAudio's device-details format sweep; invisible under FEX/arm64ec). Audio plays
fine. Boots fine on winepulse/winealsa. Ruled out: latency, endpoint properties,
`get_position` (unused by GoW), `is_format_supported` PCM-vs-float (winealsa also S_OK's
it). Remaining difference is architectural (shared mixer vs per-stream backends).

### Next steps (both, later)
1. Instrument winealsa, run GoW on it (boots), capture its exact mmdevapi call sequence,
   diff vs ours -> the divergence = the specific trigger. Decisive.
2. Fix `is_format_supported` shared-mode closest-match: return S_FALSE for non-exact so
   mmdevapi's PE side (dlls/mmdevapi/client.c) hands the client the real float mix format
   (winepulse/winealsa behavior). Correctness toward making DirectAudio a full
   replacement for winepulse/winealsa. Don't regress DiRT 3 (which needed S_OK).

Long-term goal: DirectAudio becomes a drop-in REPLACEMENT for winepulse/winealsa.

---

## 2026-08-13 — v1.2 SHIPPED. GoW second stall SOLVED: `mmdevapi_midi_n` spin loop.

The "GoW second stall" above is **closed**, and the cause was not architectural, not
`is_format_supported`, and not AAudio. It was our own unimplemented MIDI vtable slot.

### Root cause
`mmdevapi` picks a MIDI driver in `init_driver()` (`dlls/mmdevapi/main.c`):

```c
midi_drvname[0] = 0;
wine_unix_call( midi_get_driver, midi_drvname );
if (midi_drvname[0]) load_driver( midi_drvname, &midi_driver );
else                 midi_driver = drvs;      // <-- us
```

We stubbed `midi_get_driver`, so DirectAudio became its OWN MIDI driver.
`DriverProc(DRV_LOAD)` then spawned `notify_thread()`:

```c
while (1) { MIDI_CALL( midi_notify_wait, &params );
            if (quit) break;                 // <-- `quit` is UNINITIALISED stack
            if (notify.send_notify) notify_client(&notify); }
```

`midi_notify_wait` is contractually BLOCKING (winealsa's waits on a real event and sets
`*quit`). Our stub returned `STATUS_SUCCESS` instantly and never wrote `*quit` -> tight
infinite spin, one core pegged for process lifetime, hammering the PE->Unix boundary.

On device: thread `mmdevapi_midi_n`, state `R`, holding **34079 of 34314** utime jiffies.

### Why every earlier theory missed it
- **"CPU 17%" was misread.** One pegged core of eight is ~12.5%; it read as "idle,
  therefore deadlocked" when it was actually "spinning".
- **Audio played fine** because the audio path was never involved.
- **The relay PoC and PERFORMANCE_MODE_NONE never helped** because both were about
  AAudio, and AAudio was never the problem.
- **All thread-state diagnostics were CONTAMINATED**: inspecting via the bridge means the
  user is in Termux, so the game is backgrounded and frozen (`State: T`). The fix was
  foreground-automated capture — that is what found the spinning thread in one look.

### Fix (`a350c9d`, on main, tagged `directaudio-v1.2`)
Mirror `winepulse.drv`: implement `midi_get_driver` -> `L"alsa"` so mmdevapi loads
winealsa for MIDI. Plus a defensive `midi_notify_wait` that sets `*quit` on first call.
Both native and wow64 vtables. **Audio path untouched.**

### Validation — 7 games, all zero underruns, all zero spin threads
| game | graphics | audio API | buffer | track latency | was |
|---|---|---|---|---|---|
| Insane 2 | D3D9 | FAudio | 4 ms | **25.00 ms** | - |
| GTA V Enhanced | D3D12/VKD3D | WASAPI | 4 ms | **25.00 ms** | - |
| DiRT Showdown | D3D11 | WASAPI | 4 ms | **25.00 ms** | 83.50 |
| Hades | D3D11 | FAudio | 4 ms | **25.00 ms** | 81.00 |
| GTA IV | D3D9 | **DirectSound** | 4 ms | **25.00 ms** | 81.00 |
| God of War | D3D11 | FAudio/XAudio2 | 8 ms | 29.00 ms | 45.00 |
| DiRT 3 | D3D11 | WASAPI | 12 ms | 33.00 ms | 87.50 |

GoW before/after: black screen -> renders; 0 -> 26.9 fps; GPU 0% -> 70%; CPU 17% -> 97%.
Human listening test across all titles: no crackling. Ninja Gaiden: untested (skipped).

### Latency framing — IMPORTANT
The historical "4.00 ms" figure is a **buffer size**, not end-to-end delay. AudioFlinger
adds a fixed **21.00 ms** (mixer + HAL) that no app can remove.
`4 ms buffer + 21 ms floor = 25.00 ms track latency` — verified exact on 3 games, and
verified additive within a single run (buffer 1152->1248 fr moved latency 45.00->47.00).
**Quote 25 ms.** Pulse's default `PULSE_LATENCY_MSEC=100` ~= 121 ms, so ~5x better.

### winealsa-absent test (post-release, PASSED)
Hid BOTH halves (`winealsa.drv` + `winealsa.so`) in the 11.0-5 layer, launched Hades:
DirectAudio still loaded, in-process AAudio present, **no MIDI thread**. Confirms
`load_driver` failing leaves `midi_driver` zeroed and `DriverProc` returns early.
Files restored, hashes verified against baseline.

### Release
`directaudio-v1.2` — 4 assets (release + diagnostics, sdk28/sdk35). Diagnostics branch is
`feat/diagnostics-v1.2` (`d4e13b8`) = v1.2 + 23 `DA_LOG` probes.

### Open / next
1. **Preset rework** (biggest user-visible win): default `stable`->`auto` (62.5->26 ms);
   add a "Minimum" rung at `bf=192, adaptive=true`. **NEVER put bf=192 on `low`** — `low`
   has `adaptive=false`, and every 25 ms result depended on adaptive (GoW and DiRT 3 could
   not hold 4 ms and grew to 8/12 ms). Reaching the floor currently requires hand-written
   `PRESET=custom ... BF=192` in the shortcut's envVars.
2. Downmix headroom (5.1 fold peaks ~2.41x full scale -> clips), route rate/burst
   re-derive, Spatializer/multichannel, mic capture. See the feature roadmap.
3. Mali GPUs still untested. Everything above is one device (Adreno 750 / Android 14).

---

## 2026-08-13 (later) — v1.2.1 SHIPPED. Dead AAudio data callback -> permanent audio loss.

Downstream bug report (JT, GameNative): "rapidly pause/resume the game and audio is
lost forever until you reopen it." Reproduced here on Bannerlator with v1.2, so it is
**driver-side, not app-specific**.

### Correction to the report
The trigger is **rapid BACKGROUND/FOREGROUND of the app**, not pause/resume. The
in-drawer Pause/Resume (`SIGSTOP`/`SIGCONT` on the guest) does NOT reproduce it. Rapid
bg/fg does, every time. (GameNative's "pause" may simply be backgrounding.)

### Root cause
Backgrounding freezes the guest, so it cannot feed the mixer and the in-process AAudio
stream starves. AudioTrack eventually disables itself and auto-restarts, but the data
callback never resumes:

```
19:50:20  hb: cb=19000 buf=5568(116ms) xruns=28    <-- last heartbeat ever
19:50:23  W AudioTrack: restartIfDisabled(263): releaseBuffer() track
          disabled due to previous underrun, restarting
          -- data callback never fires again --
19:50:36+ pad=1536 held=1536 playing=1             guest ring full, frozen
```

AudioFlinger agrees independently: track frames climb 576 -> 5760, underruns hit
442176, then the track goes ABSENT. This raises **no error**, so `mixer_error_cb` --
the only path that rebuilt the stream -- never runs.

### Three defects, all fixed (`de24081`, `7b9afff`)
1. **No stall detection.** `mixer_watchdog()`, called once per period from
   `unix_timer_loop` (which keeps ticking when audio is dead). Callback silent 1 s with
   voices playing -> rebuild via the existing reopen worker. A disabled AAudio stream
   can never be restarted; recreate is the only recovery, same as a route change.
2. **`cb_count` never advanced.** It was incremented INSIDE the `TRACE_ON()` test, so
   short-circuit meant it never ran with tracing off -- a dead callback looked identical
   to a live one. Now unconditional; this is what makes the watchdog work in RELEASE.
3. **Adaptive inflated on suspension.** Frozen guest -> xruns climb anyway -> monotonic
   growth -> permanent inflation (192 -> 5568 fr, 4 ms -> 116 ms). Now re-baselines on a
   gap > 500 ms instead of growing.

The watchdog **re-baselines rather than trips** when its OWN cadence jumps, so a normal
bg/fg cycle does not force a needless rebuild.

### Also: rebuilds are now visible in the RELEASE build
`mixer_request_reopen` only logged via Wine `WARN()` (needs WINEDEBUG, goes to stderr),
so a rebuild was invisible in logcat and unattributable. Added `DA_EVENT` (always on,
event-level only -- a few lines per session, not telemetry):
`I DirectAudio: reopen: data callback stalled` / `reopen: stream error`.
Needs `-llog` in `UNIX_LIBS`. Heartbeats/per-call tracing stay in the diagnostics build.

### Verified BOTH directions on device (DiRT 3)
**Recovers** (abuse run): heartbeats unbroken cb=1000->21000 (was: stop at 19000
forever); ring draining 199/1344/1/177 (was: pinned 1536); buffer resets to 192 fr
(4 ms) each rebuild (was: 5568 fr / 116 ms, never shrinks); 5 rebuilds, every old stream
closed 4->9->11->12 (no leak); **user heard NO dropout at all**. Inaudible because the
swap is ~77 ms and the new stream is promoted BEFORE the old is destroyed, so the guest
ring covers it.

**Does not misfire** (5 min clean gameplay, no backgrounding): `reopen:` events = **0**;
stream opens = **1**; buffer **384 fr / 8.00 ms flat** the whole session; xruns = **1**
(at startup); 77,000 callbacks. This also confirms defect 3 retroactively -- the growth
in the abuse run came from freezing, not from the game being hard to feed.

### Release
`directaudio-v1.2.1` -- 4 assets: `-sdk28/35` (release) and `-diagnostics-sdk28/35`.
Device restored to the shipped release driver (`9817a941`).

### Open
1. **SHIPPING GAP (biggest):** `proton_11.0` does not carry `dlls/winedirectaudio.drv`
   at all, its `mmdevapi` `default_list` has no `directaudio`, and the submodule pin on
   `feat/directaudio-submodule` is STALE at `cd6b4a3`. A Proton rebuild today ships
   neither v1.2 nor v1.2.1 -- everything on device is hot-swapped.
2. Preset rework: default `stable`->`auto`; add "Minimum" (`bf=192, adaptive=true`);
   **NEVER `bf=192` on `low`** (it has `adaptive=false`).
3. Downmix headroom, route rate/burst re-derive, Spatializer, mic capture.
4. Mali untested. Ninja Gaiden untested. Upstream to GameNative needs a NEW PR (#7 merged).

---

## 2026-08-13 (end of day) — branch cleanup + where to pick up

### Repo hygiene
Branches 18 -> 13. Deleted only what was merged into main or superseded:
`feat/directaudio-switching-usage`, `feat/fix-release-deadlock`,
`fix/midi-notify-spin`, `fix/aaudio-callback-watchdog` (all merged);
`feat/diagnostics`, `feat/diagnostics-v1.2`, `fix/aaudio-callback-watchdog-diag`
(instrumentation carried forward). All 9 GoW investigation branches KEPT --
they are the record of what was ruled out.

**New permanent `diagnostics` branch** = main + one instrumentation commit.
Refresh it with `git rebase main` after each release. This replaces the
per-release naming that produced a new stale branch every time.

Deleted SHAs (recoverable ~90d via `git push origin <sha>:refs/heads/<name>`):
8d47e2d9 2acab88c a350c9d1 7b9affff 4a9ba787 d4e13b8e 774344aa

### Shipped today
- **v1.2** — `mmdevapi_midi_n` spin fix. Root cause of the long-running GoW
  black screen; every DirectAudio title had been losing a CPU core since v1.
- **v1.2.1** — dead AAudio data-callback watchdog. Fixes JT's permanent audio
  loss on rapid background/foreground.
Both shipped with release + diagnostics builds, sdk28 and sdk35.

### PICK UP HERE (nothing is blocked)
1. **Shipping gap (biggest).** `proton_11.0` carries no `dlls/winedirectaudio.drv`
   at all, its mmdevapi `default_list` has no `directaudio`, and the submodule
   pin on `feat/directaudio-submodule` is stale at `cd6b4a3`. A Proton rebuild
   today ships NEITHER release -- everything on device is hot-swapped.
2. **GameNative PR** joshuatam/GameNative#8 open (both tzst, JT chooses via the
   `DIRECTAUDIO_ASSET` constant). When JT merges, utkarshdalal#1806 updates
   itself -- it tracks his branch, no upstream action needed. Open question for
   him: SDK 28 vs 35 for the bundled asset.
3. **Preset rework** -- default `stable`->`auto` (62.5 -> 26 ms), add a
   "Minimum" rung (`bf=192, adaptive=true`). NEVER `bf=192` on `low` -- it has
   `adaptive=false` and every 25 ms result depended on adaptive.
4. **Proton gate + runtime fallback.** Bannerlator offers DirectAudio on Proton
   10 containers where the ABI mismatches. A version gate alone is NOT enough
   (11.0-3 IS Proton 11 and still fails to load) -- needs a runtime fallback so
   a failed init lands on pulse instead of leaving the container with no audio.
5. Parked: cross-Proton-layer compat (works 11.0-5, `STATUS_DLL_NOT_FOUND` on
   11.0-3, cause unresolved). Downmix headroom, route rate/burst re-derive,
   Spatializer, mic capture. Mali untested. Ninja Gaiden untested.

---

## 2026-08-14 — adaptive DECAY: the buffer can come back down (branch, unmerged)

Branch `feat/adaptive-decay` (`e55375b`, off `main` `3d78ffc`). Answers a direct
question: can a target latency be set at launch and *adapted to* during play?
Before this, no -- `BF` was only a starting point and `MBF` a ceiling, and the
loop moved one way. Growth is now reversible.

### What it does
After 10 s (`DA_DECAY_QUIET_NS`) with no xrun climb, hand one burst back.

Two guards, because naive shrinking oscillates (shrink -> underrun -> grow ->
shrink), each cycle an audible click -- the reason Oboe's `LatencyTuner` refuses
to shrink at all:

1. **Floor = the size the stream OPENED at** (`base_buf_frames`, read back
   post-open so it is a size AAudio actually rounds to). Decay can only undo
   growth; it can never probe below what the launch config asked for. A rebuild
   re-runs open, so a recovered stream starts low rather than inheriting growth.
2. **Punishment.** An xrun within 5 s (`DA_DECAY_PUNISH_NS`) of a step down
   means that step went too far: the level we climbed back to becomes the new
   floor (`decay_floor`) and the quiet period doubles, capped at 32x (~5 min).
   Once the floor is reached `cur > floor` stops holding and the loop quiesces,
   so a title that needs headroom settles after a probe or two.

Suspension is not calm: the freeze re-baseline path also restarts both decay
timers, so backgrounded wall-clock cannot buy a step down on resume.

`BANNER_AUDIO_DIRECT_DECAY=0` restores grow-only. Defaults ON -- the floor
guarantee bounds the worst case to handing back latency that growth took.
Floor discovery logs via `DA_EVENT`; individual steps are TRACE only.

### NOT YET DONE
- **Device test.** Needs a title that actually grows -- DiRT 3 loading is the
  known repro (2568 underruns during load, then hours of smooth play holding the
  inflated buffer). Confirm BOTH paths: reclaim after a transient, AND the
  punished floor on a title that cannot hold the lower level. Per standing rule,
  test the OFF state (`DECAY=0`) too.
- **UI.** Deliberately left for a follow-up discussion on where it belongs in
  the app (preset rung vs. its own control, and whether a ms-valued "target
  latency" box is honest given ~21 ms of the measured 25 ms is AudioFlinger's
  output latency, which the driver does not control).
- Not merged, not tagged, not in any Proton layer.

## 2026-08-14 (later) — full env control: ms latency + 9 more knobs (same branch)

`dfa2cb7` (ms) + `401bf36` (the nine). Same branch `feat/adaptive-decay`, still
unmerged and still untested on device.

### Millisecond latency, with no UI
`_MS` / `_MAXMS` mirror `_BF` / `_MBF` in milliseconds and are read LAST so they
win. That precedence is load-bearing: `applyDirectAudioConfig` writes `_BF` into
the env from the selected preset on EVERY launch, so a hand-typed frame count
loses to the preset every time. Nothing but a person sets `_MS`.

ms->frames conversion happens AFTER open, not at config read, because it needs
the device burst. Rounds UP to a burst multiple (never below one burst): asking
5 ms on a 4 ms-burst device gives 8 ms. Rounding down under-serves a burst and
underruns; not rounding leaves decay chasing a size the stream cannot hold.

`_MS` is also the decay floor, so "target a latency and adapt around it" is now
literally true rather than aspirational.

### The nine
| knob | was | why it matters |
|---|---|---|
| `_PERIOD_MS` / `_MINPERIOD_MS` | `def_period` 10 ms / `min_period` 5 ms, compiled in | **the guest half of latency** - `get_latency` = buffer + period, and games size their buffers from the reported period. Every other knob steered only the AAudio half. Read at process attach: `get_device_period` is asked before any stream exists. min clamped to def (WASAPI contract). |
| `_EXCLUSIVE` | sharing mode hardcoded SHARED | never-tested lever; EXCLUSIVE can go lower where granted, AAudio falls back silently - hence the open log now reports GRANTED perf + sharing, not requested |
| `_WATCHDOG` / `_STALL_MS` | watchdog always on, 1 s | could not be A/B'd or its off state tested |
| `_DECAY_QUIET_MS` / `_DECAY_PUNISH_MS` / `_DECAY_MAXBACKOFF` | 10 s / 5 s / 32x | all three were reasoned guesses; now tunable in one device session instead of five build cycles |
| `_LOG` | diagnostics build only | heartbeats + growth + decay steps on a RELEASE build (`DA_LOG` = `DA_EVENT` behind the flag). Field diagnosis with no special binary. |

Parsing rule: `<= 0` means "not set" and falls back to the built-in, so a
malformed value can never request a 0 ms timeout or a zero-length buffer.
`_WATCHDOG` is the exception (boolean, 0 is an answer).

### Deliberately NOT exposed
Mix format, output rate, channel count, AAudio usage tag, device pinning
(`setDeviceId` is still never called), downmix gain, adaptive step size. Each
changes what the driver DOES rather than how it is tuned, and each belongs to a
roadmap item with its own device test.

### Still open
Device test for everything on this branch. The app also needs a plumbing fix:
`persistAudioToShortcut` drops EVERY `BANNER_AUDIO_DIRECT_*` token and re-emits
only the six keys it knows, so hand-typed `_MS`/`_MAXMS`/`_DECAY` survive
launches but are silently deleted the first time the in-game audio cog is
applied. Fix = drop only the keys being rewritten.

## 2026-08-14 (later still) — shipping defaults changed: 83 ms -> 33 ms total

Compiled-in defaults, used whenever a host sets no buffer config at all:

| | was | now |
|---|---|---|
| start buffer | 3000 fr / 62.5 ms (`RATE/16`) | **576 fr / 12 ms** (`DA_DEFAULT_MS`) |
| **total latency** | **83 ms** | **33 ms** |
| capacity / ceiling | 12000 fr / 250 ms (`RATE/4`) | **4800 fr / 100 ms** (`DA_DEFAULT_MAX_MS`) |

12 ms is the highest sustained floor in the 7-game sweep - the smallest buffer no
tested title had to grow away from, so a fresh launch neither crackles nor runs
adaptive on its first loading screen. NOT one burst (25 ms total): five of seven
titles held it but GoW needed 8 ms and DiRT 3 needed 12 ms, and DiRT 3 took 2568
underruns during loading before settling. n=1 device, and hardware with a
256-frame burst cannot do 4 ms at all. One burst stays the right opt-in.

The default goes through the same ms->burst-rounding path as `_MS`, so it lands
on a burst boundary on hardware whose burst is not 192.

Precedence is now: `_MS` > `_BF` > `DA_DEFAULT_MS`.

**This matters most for hosts that bundle the driver and wire no env at all** -
GameNative gets the new default directly. Bannerlator does NOT: every non-custom
preset writes `_BF`, so it keeps its 62.5 ms "stable" default until the app-side
preset rework (roadmap item 0) lands.
