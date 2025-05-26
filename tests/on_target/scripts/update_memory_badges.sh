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

# Configure Git
git config --global --add safe.directory "$(pwd)"
git config --global user.email "github-actions@github.com"
git config --global user.name "GitHub Actions"

# Create a temporary directory for gh-pages content
TEMP_DIR=$(mktemp -d)

# Fetch existing CSV files from gh-pages branch
git fetch origin gh-pages

echo "Fetching existing CSV files..."
if git show origin/gh-pages:docs/ram_history.csv > "$TEMP_DIR/ram_history.csv" 2>/dev/null; then
    echo "Found existing RAM history:"
    cat "$TEMP_DIR/ram_history.csv"
else
    echo "No existing RAM history, creating new file"
    touch "$TEMP_DIR/ram_history.csv"
fi

if git show origin/gh-pages:docs/flash_history.csv > "$TEMP_DIR/flash_history.csv" 2>/dev/null; then
    echo "Found existing Flash history:"
    cat "$TEMP_DIR/flash_history.csv"
else
    echo "No existing Flash history, creating new file"
    touch "$TEMP_DIR/flash_history.csv"
fi

# Generate badge files and append to existing CSV files
echo "Parsing build log and generating badge files, updating csv history files and html plots..."
./tests/on_target/scripts/parse_memory_stats.py "$BUILD_LOG" "$TEMP_DIR" || handle_error "Failed to update badge, csv and html files"

# Switch to gh-pages branch and update files
git checkout gh-pages || handle_error "Not able to checkout gh-pages"
git pull origin gh-pages || handle_error "Failed to pull latest gh-pages"

# Copy all files to docs/
mkdir -p docs
cp "$TEMP_DIR"/*.json docs/
cp "$TEMP_DIR"/*.csv docs/
cp "$TEMP_DIR"/*.html docs/
cp "$ROM_REPORT" "$ROM_REPORT_DEST"
cp "$RAM_REPORT" "$RAM_REPORT_DEST"

git add $FLASH_BADGE_FILE_DEST $RAM_BADGE_FILE_DEST \
    $FLASH_HISTORY_CSV_DEST $RAM_HISTORY_CSV_DEST \
    $FLASH_PLOT_HTML_DEST $RAM_PLOT_HTML_DEST \
    $ROM_REPORT_DEST $RAM_REPORT_DEST
git commit -m "Update memory usage badges"
git push origin gh-pages

# Clean up
rm -rf "$TEMP_DIR"
