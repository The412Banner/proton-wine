#!/bin/bash
# Apply the GloriousEggroll (GE-Proton) game-fixes tier on top of the Valve proton
# source. This runs AFTER the GameNative bionic/android patches have been applied
# in the build-step scripts — the game-fixes tier was verified to apply cleanly on
# the bionic-patched tree in that order.
#
#   Tier: game-fixes  -- GE per-game compat fixes. Low conflict.
#   (ge-video-rework is NOT included in this tier; it is a separate port.)
#
# GE applies its patches with `patch -Np1`, i.e. GNU patch's default fuzz of 2, and
# so do we. (Fuzz 3 ignores every context line of a 3-line hunk: on this base it
# "applied" nascar25-protector into a comment block and a different function, and
# the per-patch marker still matched.)
#
# Tier = GE-Proton11-9 patches/game-patches (same 15 files as 11-7; only
# return-to-krondor and vgsoh changed). Not carried (all deliberate):
#   layered-overlay-wine, multi-process-launcher-x11-fallback -- skipped on every
#     one of our GE layers (desktop compositor / Wayland launcher paths).
#   nascar25-protector -- rewrites the x86_64 Linux SIGSYS handler that GE's
#     wine-wayland 0271 "Forward syscalls to linux when EAC EOS bootstrapper" hack
#     adds; Valve 46b29104 has no Linux SIGSYS handler for it to hook (macOS only).
#     It is unix signal_x86_64.c code, which an arm64ec build never compiles, and
#     under FEX the x86 syscall instruction never reaches a host SIGSYS anyway.
# dai_xinput is carried from our GE 11-6 layer: GE 11-7 dropped it for its Sony
# XInput hotfix stack, which this layer does not take.
#
# Fail-hard contract:
#   * a patch that does not apply is a FATAL error (CI surfaces the conflict);
#   * after EACH patch a source token unique to that fix must be present in the
#     file it targets (fuzz can land a hunk, but a rename/refactor can also make
#     `patch` "succeed" on the wrong function — the marker catches that);
#   * a patch file with no registered marker is FATAL too: adding a GE fix means
#     adding its marker below, so nothing can ride in unverified.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$SCRIPT_DIR")"
GE_DIR="$ROOT/android/ge-patches"

# One marker per patch: "<target file>|<literal token that only exists once applied>"
marker_for() {
  case "$1" in
    0001-win32u-Avoid-zero-WM_ACTIVATEAPP-lparam-on-first-for.patch)
      echo "dlls/win32u/input.c|get_activateapp_thread_id" ;;
    ai-limit-dx12-compute-shader-fallback.patch)
      # x86_64-only hook (#if __x86_64__ && !__arm64ec__): inert in the arm64ec build
      echo "dlls/ntdll/loader.c|patch_ai_limit_compute_shaders" ;;
    assettocorsa-hud.patch)
      # GE-Proton11-7 version (create_font_collection_from_set, the newer dwrite)
      echo "dlls/dwrite/font.c|244210" ;;
    black-desert-keep-fullscreen-on-focus-loss.patch)
      echo "dlls/win32u/input.c|WINE_BLACK_DESERT_KEEP_FULLSCREEN" ;;
    dai_xinput.patch)
      # dropped by GE in 11-7 for its Sony-XInput stack (not carried); ours from GE 11-6
      echo "dlls/win32u/input.c|GameLoop" ;;
    eac_60101_timeout.patch)
      echo "dlls/ntdll/unix/server.c|EAC_LAUNCHERDIR" ;;
    maplestory-kernelbase-charprev-null.patch)
      echo "dlls/kernelbase/string.c|if (!start) return NULL;" ;;
    maplestory-spi-stickykeys-filterkeys.patch)
      echo "dlls/win32u/sysparams.c|WINE_SPI_WARN(SPI_SETSTICKYKEYS)" ;;
    max-payne-cpu-detection.patch)
      # i386 ntdll only (#ifdef __i386__): active for 32-bit Max Payne under FEX WoW64
      echo "dlls/ntdll/loader.c|patch_max_payne_cpu_detection" ;;
    pso2_hack.patch)
      echo "dlls/ntdll/unix/file.c|WINE_NO_OPEN_FILE_SEARCH" ;;
    return-to-krondor-text-bitmap-readback.patch)
      # GE-Proton11-9 version (rebased by GE onto the newer bleeding-edge, 18ec6165)
      echo "dlls/win32u/dib.c|use_krondor_bitmap_readback" ;;
    silence-starcitizen-unsupported-os.patch)
      echo "dlls/user32/msgbox.c|Star Citizen" ;;
    vgsoh.patch)
      # GE-Proton11-9 version: matched on the executable name (610bc97b), not SteamGameId
      echo "dlls/kernelbase/file.c|VGOEmuLauncher.exe" ;;
    *) echo "" ;;
  esac
}

apply_dir() {
  local dir="$1" tier="$2"
  [ -d "$dir" ] || { echo "GE: tier '$tier' dir missing ($dir) — skipping"; return 0; }
  local n=0 fail=0
  for p in "$dir"/*.patch; do
    [ -e "$p" ] || continue
    n=$((n+1))
    local name; name="$(basename "$p")"
    local marker; marker="$(marker_for "$name")"
    if [ -z "$marker" ]; then
      echo "GE[$tier]: FATAL: no verification marker registered for $name (add it to marker_for in $0)"
      fail=$((fail+1))
      continue
    fi
    local file="${marker%%|*}" token="${marker#*|}"
    echo "GE[$tier]: applying $name"
    if ! patch -Np1 --fuzz=2 --no-backup-if-mismatch < "$p"; then
      echo "GE[$tier]: FATAL: failed to apply $name"
      fail=$((fail+1))
      continue
    fi
    if grep -qF -- "$token" "$ROOT/$file"; then
      echo "GE[$tier]:   ok    marker '$token' present in $file"
    else
      echo "GE[$tier]: FATAL: $name applied but marker '$token' is NOT in $file"
      fail=$((fail+1))
    fi
  done
  echo "GE[$tier]: applied+verified $((n-fail))/$n patches"
  return $fail
}

echo "=== Applying GE-Proton patch tiers ==="
rc=0
apply_dir "$GE_DIR/game-fixes" "game-fixes" || rc=$?

if [ "$rc" -ne 0 ]; then
  echo "=== GE patch application had $rc failure(s) — refusing to build a partial GE layer ==="
  exit 1
fi
echo "=== GE patches applied and verified ==="
