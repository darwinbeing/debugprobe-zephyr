#!/usr/bin/env bash
# Re-apply the out-of-tree patches in patches/ to the west workspace.
# `west update` re-fetches the trees clean, so this must run after every update
# (CI does it automatically; see patches/README.md). Idempotent: a patch that is
# already applied is skipped, so it is safe to re-run.
#
# Workspace root defaults to the local dev workspace; override for CI:
#   ZEPHYR_WORKSPACE="$GITHUB_WORKSPACE" scripts/apply_patches.sh
set -euo pipefail

APP_DIR="$(cd "$(dirname "$0")/.." && pwd)"
WS="${ZEPHYR_WORKSPACE:-$HOME/Developer/zephyrproject}"

apply_one() {
	local tree="$1" patch="$2"
	local dir="$WS/$tree" p="$APP_DIR/patches/$patch"
	if git -C "$dir" apply --reverse --check "$p" >/dev/null 2>&1; then
		echo "  = $patch already applied to $tree — skip"
	elif git -C "$dir" apply --check "$p" >/dev/null 2>&1; then
		git -C "$dir" apply "$p"
		echo "  + $patch -> $tree"
	else
		echo "  ! $patch does NOT apply to $tree (tree drifted from pinned rev?)" >&2
		return 1
	fi
}

echo "Applying out-of-tree patches (workspace: $WS)"
apply_one "zephyr"               "zephyr_sysconf_newlib_abi.patch"
apply_one "modules/hal/rpi_pico" "rp2040_flash_ramfunc.patch"
echo "Done."
