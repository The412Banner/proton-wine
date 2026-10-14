# Layer build workflow

Each layer branch carries exactly one build workflow, named for the layer it builds:

| branch | workflow file | layer |
|---|---|---|
| `proton_10.0` | `build-proton-10.0-4.yml` | Proton 10.0-4 |
| `proton_10.34-GE` | `build-ge-proton-10.0-34.yml` | GE-Proton 10.0-34 |
| `proton_11.0` | `build-proton-11.0-1.yml` | Proton 11.0-1 |
| `proton_11.0-2` | `build-proton-11.0-2.yml` | Proton 11.0-2 (arm64ec + x86_64) |
| `proton_11.3-GE` | `build-ge-proton-11.0-3.yml` | GE-Proton 11.0-3 |
| `proton_11.5-GE` | `build-ge-proton-11.0-5.yml` | GE-Proton 11.0-5 |
| `proton_11.6-GE` | `build-ge-proton-11.0-6.yml` | GE-Proton 11.0-6 |
| `proton_11.7-GE` | `build-ge-proton-11.0-7.yml` | GE-Proton 11.0-7 (arm64ec + x86_64) |
| `proton_11.7.1-GE` | `build-ge-proton-11.0-7-valve.yml` | GE-Proton 11.0-7.1 (Valve wine 46b29104, the GE-Proton11-7 base, + the 11.0-2 layer stack; arm64ec + x86_64) |
| `proton_11.9-GE` | `build-ge-proton-11.0-9.yml` | GE-Proton 11.0-9 (Valve wine 981e2f0b9007, the GE-Proton11-9 base, merged onto `proton_11.7.1-GE`; arm64ec + x86_64) |
| `proton_11.9.1-DroidDeck` | `build-droiddeck-proton-11.0-9.1.yml` | DroidDeck-Proton 11.0-9.1 (`proton_11.9-GE` r2 + the DroidDeck-Proton CachyOS game-fixes tier; arm64ec + x86_64) |

The file name is distinct per branch on purpose: GitHub labels a run with the
workflow name taken from the default branch's copy of the same path, so a shared
`build-proton.yml` made every layer's run read as the default branch's layer.

## How a build is triggered

`workflow_dispatch` only works for workflows on the default branch, so layer
builds are push-triggered. Each workflow fires on a push to its own parent
branch and to `staging/<parent>/**`. Work on a layer therefore goes:

1. branch `staging/<parent>/<topic>` off the parent;
2. push — the run appears under the layer's own workflow name with `run-name`
   `<Layer> — staging/<parent>/<topic> (push)`;
3. once green, fast-forward the parent to the staging tip.

Releases are cut separately (`gh release create` with the verified artifacts);
the `release:` job in each workflow is hard-disabled (`if: false`).

## What a run does

1. termuxfs sysroot, NDK r27d, bylaws llvm-mingw toolchain, cached wine-tools
   and ccache;
2. `build-step-<arch>.sh --configure` — fail-hard: configure error, missing or
   non-applying Android patch, or a missing source marker for any shipped
   feature stops the build (see the `MARKERS` table in the script);
   on GE branches `apply-ge-patches.sh` then applies the game-fixes tier with
   one registered marker per patch;
3. `--build`, `--install` — fail-hard;
4. `verify-layer.py` on the installed tree — tree shape, 16 KB page alignment,
   Android API level, the compiled feature strings, RtlIsEcCode guard bytes,
   font-handle cap, XP desktop markers in both PE arches, DirectAudio 3-file
   set, GE strings;
5. prefixPack.txz downloaded from a pinned GameNative/bionic-prefix-files
   commit and sha256-checked;
6. profile.json generated, `.wcp` (zstd) and `.wcp.xz` packaged;
7. the packaged archives checked (required entries, profile.json byte-identical
   to the generated one);
8. artifacts uploaded (30-day retention).

## Matrix

`arch` × `api_level`. Every leg compiles against the android28 NDK target;
`api_level: 35` only adds 16 KB linker alignment (`--enable-16kb-pages`).
arm64ec is always built 16 KB-aligned on api 28. `proton_11.0-2` also builds
x86_64 (4 KB and 16 KB legs), and `proton_11.7-GE`, `proton_11.7.1-GE` and
`proton_11.9-GE` build x86_64 at api_level 28 only (4 KB, the leg 11.0-2 ships); the other branches are
arm64ec-only.

## versionCode

Stamped in the `Generate profile.json` step of each workflow. Every layer
release bumps it in all seven files; the app installs a higher versionCode as a
new layer slot alongside the old one.
