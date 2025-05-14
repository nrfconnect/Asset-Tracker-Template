#!/usr/bin/env python3
import os
import sys
import subprocess
import argparse

def main():
    parser = argparse.ArgumentParser(description="Patch Module Renamer and Applier")
    parser.add_argument("--name", help="The new module name (e.g. new-name)")
    parser.add_argument("--patch_file", help="The full path to the patch file")
    parser.add_argument("--apply", action="store_true", help="Automatically apply the patch with 'git apply'")

    args = parser.parse_args()

    print("\n===== Patch Module Renamer and Applier =====\n")

    name = args.name.strip()
    if not name:
        print("You must provide a module name.")
        sys.exit(1)

    patch_file = args.patch_file.strip()
    if not os.path.isfile(patch_file):
        print(f"Patch file '{patch_file}' does not exist.")
        sys.exit(1)

    # Transformations
    name_cap = name[:1].upper() + name[1:]
    name_up = name.upper()

    output_patch = f"/tmp/{name}-module.patch"

    with open(patch_file, "rt") as fin, open(output_patch, "wt") as fout:
        for line in fin:
            line = line.replace('DUMMY', name_up)
            line = line.replace('Dummy', name_cap)
            line = line.replace('dummy', name)
            fout.write(line)

    print(f"\nPatched file written to: {output_patch}")

    if args.apply:
        try:
            subprocess.check_call(['git', 'apply', output_patch])
            print("Patch applied successfully.")
        except subprocess.CalledProcessError as e:
            print("git apply failed:")
            print(e)
    else:
        print(f"Done. To apply the patch, run: git apply {output_patch}")

if __name__ == "__main__":
    main()
