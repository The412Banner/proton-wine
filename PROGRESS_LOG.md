# PROGRESS LOG — staging/proton_11.12.8-DroidDeck/v17

Newest entry first. The full proton-wine log is PROGRESS_LOG.md on proton_11.0-2.

## 2026-11-23 — v17 layer, DroidDeck-Proton 11.0-12.8 (staging/proton_11.12.8-DroidDeck/v17, building)

**Why:** user go 2026-11-23 ~23:20 EST: build v17 layers overnight and stage them for morning tests. No release,
no catalog change, no install.

**Base:** staging/proton_11.12.8-DroidDeck/fix-unwind-loop d48e30a1 = v16 layer 067c719d + ARM64EC code map
d4c9ffa7 + unwind fixes fecf7376 / a188791d + Azure apt mirror pin (already had the DiRT telemetry fix and the
offscreen check cache).

**Added (one commit each):** MARKERS rows for d4c9ffa7 / fecf7376 / a188791d; WMA cutscene audio 8956410a;
GameNative #119 (Sleep(0) throttle + UDP fast path; one context line for next_killed); #128 (userspace ntsync
wait-any duplicates); #133 with #131 (system call writes into FEX-protected code); #67 ntdll half (no sechost
part, in-tree since v16); DroidDeck-Proton #34 (Black Ops III ARM64EC); versionCode 17.
Patch step simulated locally (make_requests + make_specfiles, PATCHES, MARKERS, GE tier): PASS on both arches.

**CI / staged files / device test:** pending (see the v17 entry on proton_11.0-2 and the staging folder
/sdcard/Download/v17-layers/). NOT device-tested.
