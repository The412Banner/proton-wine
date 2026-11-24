# PROGRESS LOG — staging/proton_10.0/v17

Newest entry first. The full proton-wine log is PROGRESS_LOG.md on proton_11.0-2.

## 2026-11-23 — v17 layer, Proton 10.0-4 arm64ec (staging/proton_10.0/v17, building)

**Why:** user go 2026-11-23 ~23:20 EST: build v17 layers overnight and stage them for morning tests. No release,
no catalog change, no install.

**Base:** proton_10.0 cc815410 (v16, arm64ec only).

**Added (one commit each):** ARM64EC code map sized for 47-bit addresses (Wine 10 hand port of d4c9ffa7: same
alloc_arm64ec_map bug, only the ROUND_SIZE / map_view arguments differ); unwind fixes fecf7376 + a188791d
(cherry-picked, applied unchanged) + their MARKERS rows; DiRT telemetry patch (applies unchanged); offscreen
check cache opt-in WINE_OFFSCREEN_CHECK_MS (hand port: in Wine 10 the per-present check is
vulkan_surface_update_offscreen in vulkan.c); WMA cutscene audio 8956410a (applies unchanged); Azure apt mirror
pin (d48e30a1); versionCode 17.
Not for Wine 10 (user): the GameNative #119 / #128 / #133 / #67 and DroidDeck-Proton #34 items.
Patch step simulated locally (make_requests + make_specfiles, PATCHES, MARKERS): PASS.

**CI / staged files / device test:** pending; NOT device-tested.
