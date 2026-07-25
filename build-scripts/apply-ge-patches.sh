#!/bin/bash
# Apply GloriousEggroll (GE-Proton) wine patches on top of the Valve proton_11.0
# source. This runs AFTER the GameNative bionic/android patches have been applied
# in the build-step scripts — the leaner GE-Proton11-3 game-fixes tier was verified
# to apply cleanly on the bionic-patched tree in that order.
#
#   Tier: game-fixes  -- GE per-game compat fixes (GE-Proton11-3). Low conflict.
#   (ge-video-rework is NOT included in this tier; it is a separate port.)
#
# GE applies its patches with `patch -Np1` (fuzz tolerated), so we match that.
# Any patch that fails to apply is a HARD error so CI surfaces the conflict.
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$SCRIPT_DIR")"
GE_DIR="$ROOT/android/ge-patches"

apply_dir() {
  local dir="$1" tier="$2"
  [ -d "$dir" ] || { echo "GE: tier '$tier' dir missing ($dir) — skipping"; return 0; }
  local n=0 fail=0
  for p in "$dir"/*.patch; do
    [ -e "$p" ] || continue
    n=$((n+1))
    echo "GE[$tier]: applying $(basename "$p")"
    if patch -Np1 --fuzz=3 --no-backup-if-mismatch < "$p"; then
      :
    else
      echo "GE[$tier]: FAILED to apply $(basename "$p")"
      fail=$((fail+1))
    fi
  done
  echo "GE[$tier]: applied $((n-fail))/$n patches"
  return $fail
}

echo "=== Applying GE-Proton patch tiers ==="
rc=0
apply_dir "$GE_DIR/game-fixes" "game-fixes" || rc=$?

if [ "$rc" -ne 0 ]; then
  echo "=== GE patch application had $rc failure(s) ==="
  exit 1
fi
echo "=== GE patches applied cleanly ==="
