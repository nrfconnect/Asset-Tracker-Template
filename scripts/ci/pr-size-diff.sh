#!/usr/bin/env bash
# Generate a sticky-comment-ready markdown body comparing the PR HEAD app
# image against its merge base
#
# Required env vars: PR_HEAD_ELF BASE_SHA BOARD SIZE_COMMENT_PATH
#   PR_HEAD_ELF is relative to the west workspace dir (parent of this checkout).
# Optional env var: CI_RUN_URL

set -euo pipefail
: "${PR_HEAD_ELF:?}" "${BASE_SHA:?}" "${BOARD:?}" "${SIZE_COMMENT_PATH:?}"

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"    # asset-tracker-template/
WORKSPACE_DIR="$(cd "$REPO_ROOT/.." && pwd)"        # west workspace
SIZE_DIR="$WORKSPACE_DIR/artifacts/size"
mkdir -p "$SIZE_DIR" "$(dirname "$SIZE_COMMENT_PATH")"

PR_SHA=$(git -C "$REPO_ROOT" rev-parse HEAD)

(
  # Generate the PR image size report
  arm-zephyr-eabi-size -d "$WORKSPACE_DIR/$PR_HEAD_ELF" > "$SIZE_DIR/pr.size"

  # Generate the baseline image size report
  git -C "$REPO_ROOT" fetch --depth=1 origin "$BASE_SHA"
  git -C "$REPO_ROOT" checkout --detach "$BASE_SHA"
  (cd "$WORKSPACE_DIR" && west update -o=--depth=1 -n)
  (cd "$REPO_ROOT/app" && rm -rf build-baseline \
    && west build -p --sysbuild -b "$BOARD" -d build-baseline)
  arm-zephyr-eabi-size -d "$REPO_ROOT/app/build-baseline/app/zephyr/zephyr.elf" \
    > "$SIZE_DIR/baseline.size"
  git -C "$REPO_ROOT" checkout --detach "$PR_SHA"

  # If there's any difference from the baseline, include it in the comment
  diff -u0 "$SIZE_DIR/baseline.size" "$SIZE_DIR/pr.size" \
    > "$SIZE_DIR/size_change.diff" || true
  if [ -s "$SIZE_DIR/size_change.diff" ]; then
    echo "App size changed. See [CI run](${CI_RUN_URL:-})" > "$SIZE_COMMENT_PATH"
    echo '```diff' >> "$SIZE_COMMENT_PATH"
    echo -e '  ' >> "$SIZE_COMMENT_PATH"
    head -n 1 "$SIZE_DIR/baseline.size" >> "$SIZE_COMMENT_PATH"
    cat "$SIZE_DIR/size_change.diff" >> "$SIZE_COMMENT_PATH"
    echo '```' >> "$SIZE_COMMENT_PATH"
  else
    echo "App size did not change. See [CI run](${CI_RUN_URL:-})" > "$SIZE_COMMENT_PATH"
  fi
) || {
  # Something failed, so post a comment to look at the CI run for details
  echo "App size report failed. See [CI run](${CI_RUN_URL:-})" > "$SIZE_COMMENT_PATH"
}
