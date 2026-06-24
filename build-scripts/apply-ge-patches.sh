#!/bin/bash
# Apply GloriousEggroll (GE-Proton) wine patches on top of the Valve proton_11.0
# source, BEFORE the GameNative bionic/android patches are applied at configure
# time. Tiered so the high-value / low-conflict patches land first.
#
#   Tier 1 (default)  : game-fixes  -- GE per-game compat fixes. Low conflict.
#   (future tiers: proton-custom [FSR/reflex], hotfixes, wine-staging)
#
# GE applies its patches with `patch -Np1` (fuzz tolerated), so we match that.
# Any patch that fails to apply is a hard error so CI surfaces the conflict.
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
      echo "GE[$tier]: ❌ FAILED to apply $(basename "$p")"
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
