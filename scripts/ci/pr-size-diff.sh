#!/usr/bin/env bash
# Generate a markdown body comparing the PR HEAD app image against its merge
# base, using the Zephyr SDK's `arm-zephyr-eabi-size`. Paired with a sticky-
# comment action (marocchino/sticky-pull-request-comment) in CI.
#
# Expected env vars (all required):
#   PR_HEAD_ELF        path (from the workspace dir) to the PR HEAD ELF
#                      (already produced by the main build step)
#   BASE_SHA           commit SHA the PR merges into
#   BOARD              board qualifier, e.g. nrf9151dk/nrf9151/ns
#   SIZE_COMMENT_PATH  output path for the rendered markdown body
#
# Optional:
#   CI_RUN_URL         link included in the comment footer
#   SIZE_TOOL          override (defaults to arm-zephyr-eabi-size on PATH,
#                      which the Zephyr SDK installs)

set -euo pipefail
: "${PR_HEAD_ELF:?}" "${BASE_SHA:?}" "${BOARD:?}" "${SIZE_COMMENT_PATH:?}"

SIZE_TOOL="${SIZE_TOOL:-arm-zephyr-eabi-size}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"    # asset-tracker-template/
WORKSPACE_DIR="$(cd "$REPO_ROOT/.." && pwd)"    # west workspace
SIZE_DIR="$WORKSPACE_DIR/artifacts/size"
mkdir -p "$SIZE_DIR" "$(dirname "$SIZE_COMMENT_PATH")"

snapshot_size() {
  local elf="$1" out="$2"
  {
    echo "# board: $BOARD"
    echo "# elf: $(basename "$elf")"
    "$SIZE_TOOL" -d "$elf" | awk 'NR == 2 {
      printf "text     %10d\n", $1
      printf "data     %10d\n", $2
      printf "bss      %10d\n", $3
      printf "total    %10d\n", $4
    }'
  } > "$out"
}

# On any failure, write a truthful fallback comment so the sticky doesn't
# linger with stale numbers. The step itself still exits non-zero.
write_failure_comment() {
  {
    echo "## App size report — \`$BOARD\`"
    echo
    echo "Failed to generate size report. See [CI run](${CI_RUN_URL:-})."
  } > "$SIZE_COMMENT_PATH"
}
trap 'rc=$?; [ "$rc" -ne 0 ] && write_failure_comment; exit "$rc"' EXIT

PR_SHA=$(git -C "$REPO_ROOT" rev-parse HEAD)

# 1) PR HEAD size — from the artifact the main build already produced.
if [ ! -f "$WORKSPACE_DIR/$PR_HEAD_ELF" ]; then
  echo "PR HEAD ELF not found at $WORKSPACE_DIR/$PR_HEAD_ELF" >&2
  exit 1
fi
snapshot_size "$WORKSPACE_DIR/$PR_HEAD_ELF" "$SIZE_DIR/pr.size"

# 2) Build baseline at BASE_SHA.
git -C "$REPO_ROOT" fetch --depth=1 origin "$BASE_SHA"
git -C "$REPO_ROOT" checkout --detach "$BASE_SHA"

manifest_changed=0
if ! git -C "$REPO_ROOT" diff --quiet "$PR_SHA" "$BASE_SHA" -- west.yml; then
  manifest_changed=1
  (cd "$WORKSPACE_DIR" && west update -o=--depth=1 -n)
fi

(
  cd "$REPO_ROOT/app"
  rm -rf build-baseline
  west build -p --sysbuild -b "$BOARD" -d build-baseline
)
snapshot_size "$REPO_ROOT/app/build-baseline/app/zephyr/zephyr.elf" \
  "$SIZE_DIR/baseline.size"

# 3) Restore PR HEAD so downstream steps see the intended tree.
git -C "$REPO_ROOT" checkout --detach "$PR_SHA"
if [ "$manifest_changed" = "1" ]; then
  (cd "$WORKSPACE_DIR" && west update -o=--depth=1 -n)
fi

# 4) Render markdown body.
parse_row() { awk -v k="$2" '$1 == k { print $2; exit }' "$1"; }
row() {
  local key="$1" b p d sign
  b=$(parse_row "$SIZE_DIR/baseline.size" "$key")
  p=$(parse_row "$SIZE_DIR/pr.size" "$key")
  if [ -z "$b" ] || [ -z "$p" ]; then
    return 0
  fi
  d=$((p - b))
  sign=""
  if [ "$d" -gt 0 ]; then sign="+"; fi
  printf "| %-7s | %10d | %10d | %s%d |\n" "$key" "$b" "$p" "$sign" "$d"
}

{
  echo "## App size report — \`$BOARD\`"
  echo
  if diff -q "$SIZE_DIR/baseline.size" "$SIZE_DIR/pr.size" > /dev/null; then
    echo "No change in app size."
  else
    echo "| Metric  | Baseline (B) |   PR (B)   |   Δ (B)    |"
    echo "|---------|--------------|------------|------------|"
    row text
    row data
    row bss
    row total
    echo
    echo "<details><summary>Raw diff</summary>"
    echo
    echo '```diff'
    diff -u0 "$SIZE_DIR/baseline.size" "$SIZE_DIR/pr.size" || true
    echo '```'
    echo "</details>"
  fi
  if [ -n "${CI_RUN_URL:-}" ]; then
    echo
    echo "[CI run]($CI_RUN_URL)"
  fi
} > "$SIZE_COMMENT_PATH"

trap - EXIT
