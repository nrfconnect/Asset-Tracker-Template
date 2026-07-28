#!/bin/bash

set -euo pipefail

# Define paths to files to be committed
BADGE_FILE=tests/on_target/power_badge.json
HTML_FILE=tests/on_target/power_measurements_plot.html
CSV_FILE=tests/on_target/power_measurements.csv

BADGE_FILE_DEST=docs/power_badge.json
HTML_FILE_DEST=docs/power_measurements_plot.html
CSV_FILE_DEST=docs/power_measurements.csv

# Function to handle errors
handle_error() {
    echo "Error: $1" >&2
    exit 1
}

# Temporary worktree directory
WORKTREE_DIR=$(mktemp -d)

# Function to cleanup on exit
cleanup() {
    if [ -d "$WORKTREE_DIR" ]; then
        git worktree remove "$WORKTREE_DIR" --force 2>/dev/null || true
    fi
}
trap cleanup EXIT

# Check if measurement artifacts exist (generated before pytest asserts on thresholds).
# Missing files means the test failed early; skip publish without masking the test failure.
if [ ! -f "$BADGE_FILE" ] || [ ! -f "$HTML_FILE" ] || [ ! -f "$CSV_FILE" ]; then
  echo "Power measurement artifacts not found; skipping gh-pages publish" >&2
  exit 0
fi

# Configure Git
git config --global --add safe.directory "$(pwd)"
git config --global user.email "github-actions@github.com"
git config --global user.name "GitHub Actions"

# Fetch the latest gh-pages branch
git fetch origin gh-pages

# Create a worktree for gh-pages branch
git worktree add "$WORKTREE_DIR" gh-pages || handle_error "Failed to create worktree for gh-pages"

# Copy files to the worktree
cp "$BADGE_FILE" "$WORKTREE_DIR/$BADGE_FILE_DEST" || handle_error "Failed to copy badge file"
cp "$HTML_FILE" "$WORKTREE_DIR/$HTML_FILE_DEST" || handle_error "Failed to copy HTML file"
cp "$CSV_FILE" "$WORKTREE_DIR/$CSV_FILE_DEST" || handle_error "Failed to copy CSV file"

# Navigate to worktree, commit and push
cd "$WORKTREE_DIR"
git add "$BADGE_FILE_DEST" "$HTML_FILE_DEST" "$CSV_FILE_DEST"
git commit -m "Update power badge, html and csv to docs folder"
git push origin gh-pages

echo "Successfully updated gh-pages branch"
