#!/bin/bash

# Define paths
BUILD_LOG=$1

# Define paths to files to be committed
FLASH_BADGE_FILE=tests/on_target/flash_badge.json
RAM_BADGE_FILE=tests/on_target/ram_badge.json

FLASH_BADGE_FILE_DEST=docs/flash_badge.json
RAM_BADGE_FILE_DEST=docs/ram_badge.json

if [ -z "$BUILD_LOG" ]; then
    echo "Error: Build log file path not provided"
    exit 0
fi

# Function to handle errors
handle_error() {
    echo "Error: $1"
    exit 0
}

# Generate badge files using Python script
echo "Generating badge files..."
./tests/on_target/scripts/parse_memory_stats.py "$BUILD_LOG" || handle_error "Failed to generate badge files"


# Configure Git
git config --global --add safe.directory "$(pwd)"
git config --global user.email "github-actions@github.com"
git config --global user.name "GitHub Actions"

# Ensure the gh-pages branch exists and switch to it
git fetch origin gh-pages
git checkout gh-pages || handle_error "Not able to checkout gh-pages"
git pull origin gh-pages || handle_error "Failed to pull latest gh-pages"

# Stage, commit, and push changes to the branch
cp $FLASH_BADGE_FILE $FLASH_BADGE_FILE_DEST
cp $RAM_BADGE_FILE $RAM_BADGE_FILE_DEST
git add $FLASH_BADGE_FILE_DEST $RAM_BADGE_FILE_DEST
git commit -m "Update memory usage badges"
git push origin gh-pages
