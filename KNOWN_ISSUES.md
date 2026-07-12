# Known Issues

## x86_64 Proton 11 under box64 — GUI apps don't display (use arm64ec)

**Affected:** the **`proton-11.0-1-x86_64-*.wcp`** assets, run through **box64** on ARM Android devices.
The **arm64ec** assets (native ARM Wine + FEX) are **not** affected and are the recommended runtime.

### Symptom
Booting an x86_64 container starts, but the Winlator launcher chain
(`explorer → winhandler.exe → wfm.exe`) never shows a window — the file manager doesn't open,
automatically or manually. Games launched by shortcut are affected the same way (the launcher can't
bring up the guest UI).

### Diagnosis
Captured with Wine debug (`warn,err,fixme,module,seh`) + box64 logs on a 39-bit-VA Android device,
box64 **v0.4.3** (also reproduced on 0.4.1):

- `err:seh:install_bpf … not installing seccomp` — Proton 11's Wine installs a **seccomp BPF syscall
  filter** for its PE→unix syscall dispatch. Under box64 it can't ("native libs loaded at low
  addresses"; the device exposes only **39-bit** address space).
- box64 `Warning, cannot pre-load libandroid-sysvshm.so` / `libfakeinput.so` — Winlator's shared-memory
  (X display) and input-injection helper libs aren't preloaded into the emulated process.
- `wfm.exe` **loads** (`build_module loaded C:\windows\wfm.exe`), then throws
  **`RPC_S_SERVER_UNAVAILABLE` (0x6ba) ×13** and immediately `LdrUnloadDll`s → the process exits with
  no window.

### Root cause
Proton 11's seccomp-based syscall/service machinery is **incompatible with box64's own syscall
emulation** on a 39-bit Android VA layout. Wine's service/RPC layer half-starts, so GUI apps that talk
to it (`wfm.exe`, shortcut launches) die with "RPC server unavailable." This is **not**:
- a `wfm.exe` problem — it's a valid x86_64 binary and it loads;
- a build/strip/packaging problem — the arm64ec build boots and runs from the same source tree, and the
  x86_64 `.wcp` is byte-valid (installs and Wine boots).

### Workaround
Use the **arm64ec** `.wcp` (`proton-11.0-1-arm64ec-*.wcp`). It's the native, FEX-based runtime and the
one Bannerlator/Winlator-bionic apps are built around.

### Status
Open. Getting the x86_64/box64 path working is a separate task — see the fix directions in the tracking
notes (box64 version/env parity with a known-good box64 Proton 11 setup, preloading the Winlator
sysvshm/fakeinput libs, and/or a Wine build that doesn't require seccomp under box64).
