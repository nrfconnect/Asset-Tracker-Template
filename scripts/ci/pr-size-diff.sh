#!/usr/bin/env bash
# Generate a sticky-comment-ready markdown body comparing the PR HEAD app
# image against its merge base
#
# Required env vars: PR_HEAD_ELF PR_HEAD_BUILD_LOG BASE_SHA BOARD SIZE_COMMENT_PATH
#   PR_HEAD_ELF and PR_HEAD_BUILD_LOG are relative to the west workspace dir
#   (parent of this checkout).
# Optional env var: CI_RUN_URL

set -euo pipefail
: "${PR_HEAD_ELF:?}" "${PR_HEAD_BUILD_LOG:?}" "${BASE_SHA:?}" "${BOARD:?}" "${SIZE_COMMENT_PATH:?}"

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"    # asset-tracker-template/
WORKSPACE_DIR="$(cd "$REPO_ROOT/.." && pwd)"        # west workspace
SIZE_DIR="$WORKSPACE_DIR/artifacts/size"
mkdir -p "$SIZE_DIR" "$(dirname "$SIZE_COMMENT_PATH")"

PR_SHA=$(git -C "$REPO_ROOT" rev-parse HEAD)

# Extract `flash%` and `ram%` for the app image from a Zephyr build log
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

# Render an ELF's size as a tab-free, fixed-width 7-column table
# (text/data/bss/dec/hex/flash%/ram%). The filename column is dropped so the
# diff isn't dominated by long absolute paths. flash%/ram% come from the
# Memory region report Zephyr prints at link time and are read out of the
# corresponding build log.
size_report() {
  local elf="$1" build_log="$2"
  local flash_pct ram_pct
  read -r flash_pct ram_pct < <(extract_app_pct "$build_log")
  arm-zephyr-eabi-size -d "$elf" \
    | awk -v fp="${flash_pct:-?}" -v rp="${ram_pct:-?}" '
        NR == 1 {
          printf "%10s %10s %10s %10s %10s %10s %10s\n", \
            "text", "data", "bss", "dec", "hex", "flash%", "ram%"
        }
        NR == 2 {
          printf "%10s %10s %10s %10s %10s %10s %10s\n", \
            $1, $2, $3, $4, $5, fp, rp
        }
      '
}

(
  # Generate the PR image size report
  size_report "$WORKSPACE_DIR/$PR_HEAD_ELF" "$WORKSPACE_DIR/$PR_HEAD_BUILD_LOG" \
    > "$SIZE_DIR/pr.size"

  # Generate the baseline image size report
  git -C "$REPO_ROOT" fetch --depth=1 origin "$BASE_SHA"
  git -C "$REPO_ROOT" checkout --detach "$BASE_SHA"
  (cd "$WORKSPACE_DIR" && west update -o=--depth=1 -n)
  (cd "$REPO_ROOT/app" && rm -rf build-baseline \
    && west build -p --sysbuild -b "$BOARD" -d build-baseline 2>&1 \
    | tee "$SIZE_DIR/baseline_build.log")
  size_report "$REPO_ROOT/app/build-baseline/app/zephyr/zephyr.elf" \
    "$SIZE_DIR/baseline_build.log" > "$SIZE_DIR/baseline.size"
  git -C "$REPO_ROOT" checkout --detach "$PR_SHA"

  # If there's any difference from the baseline, include it in the comment.
  # `--label` keeps the `---`/`+++` lines short so they don't dwarf the data.
  diff -u0 --label baseline --label pr \
    "$SIZE_DIR/baseline.size" "$SIZE_DIR/pr.size" \
    > "$SIZE_DIR/size_change.diff" || true
  if [ -s "$SIZE_DIR/size_change.diff" ]; then
    {
      echo "App size changed. See [CI run](${CI_RUN_URL:-})"
      echo '```diff'
      # Prepend one space to the column header so it lines up with the
      # `-`/`+` prefix that diff adds to the data rows.
      printf ' '; head -n 1 "$SIZE_DIR/baseline.size"
      cat "$SIZE_DIR/size_change.diff"
      echo '```'
    } > "$SIZE_COMMENT_PATH"
  else
    echo "App size did not change. See [CI run](${CI_RUN_URL:-})" > "$SIZE_COMMENT_PATH"
  fi
) || {
  # Something failed, so post a comment to look at the CI run for details
  echo "App size report failed. See [CI run](${CI_RUN_URL:-})" > "$SIZE_COMMENT_PATH"
}
