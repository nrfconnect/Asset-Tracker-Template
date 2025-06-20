#!/bin/bash

# Define paths
BUILD_LOG=$1
ROM_REPORT=$2
RAM_REPORT=$3

FLASH_BADGE_FILE_DEST=docs/flash_badge.json
RAM_BADGE_FILE_DEST=docs/ram_badge.json
FLASH_HISTORY_CSV_DEST=docs/flash_history.csv
RAM_HISTORY_CSV_DEST=docs/ram_history.csv
FLASH_PLOT_HTML_DEST=docs/flash_history_plot.html
RAM_PLOT_HTML_DEST=docs/ram_history_plot.html
ROM_REPORT_DEST=docs/rom_report_thingy91x.html
RAM_REPORT_DEST=docs/ram_report_thingy91x.html


if [ -z "$BUILD_LOG" ] || [ -z "$ROM_REPORT" ] || [ -z "$RAM_REPORT" ]; then
    echo "Error: Missing required arguments"
    echo "Usage: $0 <build_log> <rom_report> <ram_report>"
    exit 0
fi

# Function to handle errors
handle_error() {
    echo "Error: $1"
    exit 0
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

# Configure Git
git config --global --add safe.directory "$(pwd)"
git config --global user.email "github-actions@github.com"
git config --global user.name "GitHub Actions"

# Create a temporary directory for processing files
TEMP_DIR=$(mktemp -d)

# Fetch the latest gh-pages branch
git fetch origin gh-pages

# Create a worktree for gh-pages branch
git worktree add "$WORKTREE_DIR" gh-pages || handle_error "Failed to create worktree for gh-pages"

echo "Fetching existing CSV files from gh-pages..."
if [ -f "$WORKTREE_DIR/docs/ram_history.csv" ]; then
    echo "Found existing RAM history:"
    cat "$WORKTREE_DIR/docs/ram_history.csv"
    cp "$WORKTREE_DIR/docs/ram_history.csv" "$TEMP_DIR/ram_history.csv"
else
    echo "No existing RAM history, creating new file"
    touch "$TEMP_DIR/ram_history.csv"
fi

if [ -f "$WORKTREE_DIR/docs/flash_history.csv" ]; then
    echo "Found existing Flash history:"
    cat "$WORKTREE_DIR/docs/flash_history.csv"
    cp "$WORKTREE_DIR/docs/flash_history.csv" "$TEMP_DIR/flash_history.csv"
else
    echo "No existing Flash history, creating new file"
    touch "$TEMP_DIR/flash_history.csv"
fi

# Generate badge files and append to existing CSV files
echo "Parsing build log and generating badge files, updating csv history files and html plots..."
./tests/on_target/scripts/parse_memory_stats.py "$BUILD_LOG" "$TEMP_DIR" || handle_error "Failed to update badge, csv and html files"

# Ensure docs directory exists in worktree
mkdir -p "$WORKTREE_DIR/docs"

# Copy all files to worktree docs/
cp "$TEMP_DIR"/*.json "$WORKTREE_DIR/docs/" || handle_error "Failed to copy JSON files"
cp "$TEMP_DIR"/*.csv "$WORKTREE_DIR/docs/" || handle_error "Failed to copy CSV files"
cp "$TEMP_DIR"/*.html "$WORKTREE_DIR/docs/" || handle_error "Failed to copy HTML files"
cp "$ROM_REPORT" "$WORKTREE_DIR/$ROM_REPORT_DEST" || handle_error "Failed to copy ROM report"
cp "$RAM_REPORT" "$WORKTREE_DIR/$RAM_REPORT_DEST" || handle_error "Failed to copy RAM report"

# Navigate to worktree, commit and push
cd "$WORKTREE_DIR"
git add $FLASH_BADGE_FILE_DEST $RAM_BADGE_FILE_DEST \
    $FLASH_HISTORY_CSV_DEST $RAM_HISTORY_CSV_DEST \
    $FLASH_PLOT_HTML_DEST $RAM_PLOT_HTML_DEST \
    $ROM_REPORT_DEST $RAM_REPORT_DEST
git commit -m "Update memory usage badges"
git push origin gh-pages

# Clean up temp directory
rm -rf "$TEMP_DIR"

echo "Successfully updated gh-pages branch"
