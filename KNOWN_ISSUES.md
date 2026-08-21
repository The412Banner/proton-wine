# Known Issues

## x86_64 Proton 11 — file-manager desktop doesn't open under box64 (use arm64ec)

**Affected:** the **`proton-11.0-2-x86_64-*.wcp`** assets, run through **box64** on ARM Android devices.
The **arm64ec** assets (native ARM Wine + FEX) are **not** affected and are the recommended runtime.

### Symptom
Booting an x86_64 container reaches the Wine desktop (wallpaper + Start button render), but the
Winlator file manager (`wfm.exe`) never opens — automatically or manually. Games launched by shortcut
are affected the same way.

### Diagnosis (device-verified 2026-07-12)
This was investigated extensively with Wine debug (`+service`, `+rpc`, `+ole`, `+relay`) and box64 logs.
Key findings — **it is NOT a box64/seccomp problem** (that was ruled out):

- **box64 is fine.** The Service Control Manager RPC works (`OpenSCManagerW`/`OpenServiceW` succeed over
  `\\.\pipe\svcctl`), box64 spawns processes correctly, and box64 version/preset/dynarec settings make
  no difference (STABILITY = EXTREME = same result). The `install_bpf … not installing seccomp` message
  is non-fatal (Wine continues without seccomp) and is **not** the cause.
- **Real bug found:** the x86_64 container ships with essential Wine services **disabled** — `RpcSs`
  (`Start=dword:4`) and `PlugPlay` — a side-effect of the container's **ESSENTIAL** Startup Selection
  (`WineUtils.changeServicesStatus` sets `Start=4` for those in ESSENTIAL/AGGRESSIVE mode). This causes
  `StartServiceW(RpcSs)` → `ERROR_SERVICE_DISABLED (0x422)` and `RPC_S_SERVER_UNAVAILABLE`. Switching the
  container to **NORMAL** Startup Selection re-enables the services (verified: rpcss starts, PlugPlay
  connects, RPC errors drop to ~0).
- **But that did not fix the symptom** — even with every service running, `wfm.exe` still doesn't
  display its window, with no crash and no error. The remaining cause is `wfm.exe`-specific and was not
  resolved; the effort/payoff (x86_64-under-box64 is slower than arm64ec) did not justify a deeper dig.

### Workaround
Use the **arm64ec** `.wcp` (`proton-11.0-2-arm64ec-*.wcp`) — native, FEX-based, faster, and the runtime
these builds are designed around. If a container is stuck on the x86_64 desktop, setting its Startup
Selection to **NORMAL** at least restores the Wine services (RpcSs/PlugPlay), though the file manager
may still not auto-open.

### Status
**Parked.** box64 exonerated; a real service-config bug identified and worked around; the residual
wfm.exe display issue is unresolved and low-priority. arm64ec is the recommended runtime.
