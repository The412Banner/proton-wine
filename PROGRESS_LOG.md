# PROGRESS LOG — staging/proton_10.34-GE/v17

Newest entry first. The full proton-wine log is PROGRESS_LOG.md on proton_11.0-2.

## 2026-11-24 — v17 layer, GE-Proton 10.0-34 arm64ec (staging/proton_10.34-GE/v17, building)

**Why:** user go 2026-11-23 ~23:20 EST: build v17 layers overnight and stage them for morning tests. No release,
no catalog change, no install.

**Base:** proton_10.34-GE b76055cc (v16, arm64ec only) = the Proton 10.0-4 tree + the GE-Proton game-fixes tier.

**Added:** the same seven source/patch commits as staging/proton_10.0/v17 (cherry-picked unchanged: ARM64EC code
map Wine 10 port of d4c9ffa7, unwind fixes fecf7376 + a188791d, their MARKERS rows, DiRT telemetry, offscreen
check cache on the Vulkan present path, WMA 8956410a), the Azure apt mirror pin (d48e30a1), versionCode 17.
Patch step simulated locally (make_requests + make_specfiles, PATCHES, MARKERS, GE tier 10/10): PASS.

**CI / staged files / device test:** pending; NOT device-tested.
