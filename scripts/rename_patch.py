#!/usr/bin/env python3
import os
import sys
import subprocess

def main():
    print("\n===== Patch Module Renamer =====\n")

    new_name = input("Enter the new module name (e.g. new-name): ").strip()
    if not new_name:
        print("You must provide a module name.")
        sys.exit(1)

    patch_file = input("Enter the full path to the patch file: ").strip()
    if not os.path.isfile(patch_file):
        print("Patch file does not exist.")
        sys.exit(1)

    # Transformations
    new_name_cap = new_name[:1].upper() + new_name[1:]
    new_name_up = new_name.upper()

    output_patch = f"/tmp/{new_name}-module.patch"

    with open(patch_file, "rt") as fin, open(output_patch, "wt") as fout:
        for line in fin:
            line = line.replace('DUMMY', new_name_up)
            line = line.replace('Dummy', new_name_cap)
            line = line.replace('dummy', new_name)
            fout.write(line)

    print(f"\nPatched file written to: {output_patch}")

    apply_now = input("Do you want to apply the changes now with 'git apply'? (y/N): ")
    if apply_now.lower().startswith('y'):
        try:
            subprocess.check_call(['git', 'apply', output_patch])
            print("Patch applied successfully.")
        except subprocess.CalledProcessError as e:
            print("git apply failed:")
            print(e)
    else:
        print("Done. Patch not applied.")

if __name__ == "__main__":
    main()
