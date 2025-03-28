#!/usr/bin/env python3

import re
import sys
import json
import os

def format_size(size_in_bytes):
    """Format size in bytes to a human-readable string with appropriate unit."""
    if size_in_bytes >= 1024 * 1024:
        return f"{size_in_bytes / (1024 * 1024):.0f} MB"
    elif size_in_bytes >= 1024:
        return f"{size_in_bytes / 1024:.0f} KB"
    else:
        return f"{size_in_bytes} B"

def create_badge_json(label, used, total, percent, output_file):
    """Create a badge JSON file with the given parameters."""
    badge_data = {
        "schemaVersion": 1,
        "label": label,
        "message": f"{format_size(used)}/{format_size(total)} ({percent:.1f}%)",
        "color": "blue"
    }

    with open(output_file, 'w') as f:
        json.dump(badge_data, f, indent=4)

def parse_memory_stats(log_file):
    with open(log_file, 'r') as f:
        content = f.read()

    # Find memory stats that are followed by the app build path
    pattern = r'Memory region\s+Used Size\s+Region Size\s+%age Used\s+' \
             r'FLASH:\s+(\d+)\s+B\s+(\d+)\s+KB\s+(\d+\.\d+)%\s+' \
             r'RAM:\s+(\d+)\s+B\s+(\d+)\s+B\s+(\d+\.\d+)%\s+' \
             r'IDT_LIST:.*?\s+' \
             r'Generating files from .*?/app/build/app/zephyr/zephyr\.elf for board: thingy91x'

    match = re.search(pattern, content)
    if not match:
        print("Error: Could not find memory stats in log file")
        sys.exit(1)

    flash_used = int(match.group(1))
    flash_total = int(match.group(2)) * 1024  # Convert KB to B
    flash_percent = float(match.group(3))

    ram_used = int(match.group(4))
    ram_total = int(match.group(5))
    ram_percent = float(match.group(6))

    # Create badge JSON files
    badge_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ram_badge_file = os.path.join(badge_dir, "ram_badge.json")
    flash_badge_file = os.path.join(badge_dir, "flash_badge.json")

    create_badge_json("RAM Usage", ram_used, ram_total, ram_percent, ram_badge_file)
    create_badge_json("Flash Usage", flash_used, flash_total, flash_percent, flash_badge_file)

    print("Memory Usage Statistics:")
    print(f"FLASH: {flash_used:,} B / {flash_total:,} B ({flash_percent:.2f}%)")
    print(f"RAM:   {ram_used:,} B / {ram_total:,} B ({ram_percent:.2f}%)")
    print(f"\nBadge files created:")
    print(f"RAM Badge: {ram_badge_file}")
    print(f"Flash Badge: {flash_badge_file}")

if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <build_output_log>")
        sys.exit(1)

    parse_memory_stats(sys.argv[1])
