#!/usr/bin/env bash
# Generate a sticky-comment-ready markdown body reporting the PR HEAD app
# image size (text/data/bss/dec/hex + flash%/ram%).
#
# This intentionally does NOT compare against a baseline build: rebuilding
# the merge target on every PR doubles CI time and is fragile when the base
# branch's tree no longer builds cleanly with the current toolchain. The
# absolute numbers below, plus the previous main-branch sticky comment that
# a reviewer can look at, are enough to spot a meaningful regression.
#
# Required env vars: PR_HEAD_ELF PR_HEAD_BUILD_LOG SIZE_COMMENT_PATH
#   PR_HEAD_ELF and PR_HEAD_BUILD_LOG are relative to the west workspace dir
#   (parent of this checkout).
# Optional env var: CI_RUN_URL

set -euo pipefail
: "${PR_HEAD_ELF:?}" "${PR_HEAD_BUILD_LOG:?}" "${SIZE_COMMENT_PATH:?}"

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
WORKSPACE_DIR="$(cd "$REPO_ROOT/.." && pwd)"
mkdir -p "$(dirname "$SIZE_COMMENT_PATH")"

# Extract `flash%` and `ram%` for the app image from a Zephyr build log.
# The log contains a "Memory region" report per linked image (MCUboot, app,
# ...). We keep overwriting the percentages and print the last pair seen
# right before the app's `Generating files from .../app/zephyr/zephyr.elf`
# marker, so the values are guaranteed to belong to the app image.
extract_app_pct() {
  awk '
    /Memory region[[:space:]]+Used Size/ { flash=""; ram="" }
    $1 == "FLASH:" { flash=$NF }
    $1 == "RAM:"   { ram=$NF }
    /Generating files from.*\/app\/zephyr\/zephyr\.elf/ {
      print flash, ram
      exit
    }
  ' "$1"
}

# Use the host's `size` from binutils. It's host-arch-agnostic (parses any
# ELF, output is identical to `arm-zephyr-eabi-size`), always present in the
# Zephyr CI container via build-essential, and immune to Zephyr SDK layout
# changes (the 0.x -> 1.x bump moved toolchain binaries around).
if ! command -v size >/dev/null 2>&1; then
  echo "App size report unavailable: 'size' (binutils) not found on PATH. See [CI run](${CI_RUN_URL:-})" \
    > "$SIZE_COMMENT_PATH"
  exit 0
fi

# `read` returns non-zero on EOF without a newline; an empty extraction is
# fine (the percentages just render as `?`), so don't let it abort the script.
flash_pct=""; ram_pct=""
read -r flash_pct ram_pct < <(extract_app_pct "$WORKSPACE_DIR/$PR_HEAD_BUILD_LOG") || true

{
  echo "App size. See [CI run](${CI_RUN_URL:-})"
  echo '```'
  size -d "$WORKSPACE_DIR/$PR_HEAD_ELF" \
    | awk -v fp="${flash_pct:-?}" -v rp="${ram_pct:-?}" '
        NR == 1 { printf "%10s %10s %10s %10s %10s %10s %10s\n", \
          "text", "data", "bss", "dec", "hex", "flash%", "ram%" }
        NR == 2 { printf "%10s %10s %10s %10s %10s %10s %10s\n", \
          $1, $2, $3, $4, $5, fp, rp }
      '
  echo '```'
} > "$SIZE_COMMENT_PATH"
