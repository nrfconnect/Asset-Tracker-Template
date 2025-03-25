#!/bin/bash

# Exit on error
set -e

# Define paths to files to be committed
BADGE_FILE=tests/on_target/power_badge.json
HTML_FILE=tests/on_target/power_measurements_plot.html
CSV_FILE=tests/on_target/power_measurements.csv

BADGE_FILE_DEST=docs/power_badge.json
HTML_FILE_DEST=docs/power_measurements_plot.html
CSV_FILE_DEST=docs/power_measurements.csv

# Check if files exist
if [ ! -f $BADGE_FILE ]; then
  echo "Badge file not found: $BADGE_FILE"
  exit 0
fi
if [ ! -f $HTML_FILE ]; then
  echo "HTML file not found: $HTML_FILE"
  exit 0
fi
if [ ! -f $CSV_FILE ]; then
  echo "CSV file not found: $CSV_FILE"
  exit 0
fi

# Configure Git
git config --global --add safe.directory "$(pwd)"
git config --global user.email "github-actions@github.com"
git config --global user.name "GitHub Actions"

# Ensure the gh-pages branch exists and switch to it
git fetch origin gh-pages || git checkout -b gh-pages
git checkout gh-pages

# Stage, commit, and push changes to the branch
cp $BADGE_FILE $BADGE_FILE_DEST
cp $HTML_FILE $HTML_FILE_DEST
cp $CSV_FILE $CSV_FILE_DEST
git add $BADGE_FILE_DEST $HTML_FILE_DEST $CSV_FILE_DEST
git commit -m "Update power badge, html and csv to docs folder"
git push origin gh-pages
