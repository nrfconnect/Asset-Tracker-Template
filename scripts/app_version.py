#!/usr/bin/env python3
#
# Copyright (c) 2024 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause

import sys

def alfanum(s):
    return ''.join(c for c in s if c.isalnum())

def parse_ref_name(ref_name):
    if not ref_name.startswith('v') or not '.' in ref_name:
        return 0, 0, 0, alfanum(ref_name)

    ref_name = ref_name[1:]
    parts = ref_name.split('.', 2)
    if len(parts) < 3:
        raise ValueError('Version string must contain at least three parts separated by dots')

    major = int(parts[0])
    minor = int(parts[1])
    extra = ''

    if '-' in parts[2]:
        patchlevel, extra = parts[2].split('-')
        patchlevel = int(patchlevel)
        extra = alfanum(extra)
    else:
        patchlevel = int(parts[2])

    return major, minor, patchlevel, extra

if __name__ == '__main__':
    if len(sys.argv) != 2:
        print('Usage: python app_version.py <ref_name>')
        sys.exit(1)

    ref_name = sys.argv[1]
    try:
        major, minor, patchlevel, extra = parse_ref_name(ref_name)
        print(f'VERSION_MAJOR = {major}')
        print(f'VERSION_MINOR = {minor}')
        print(f'PATCHLEVEL = {patchlevel}')
        print(f'VERSION_TWEAK = 0')
        if extra:
            print(f'EXTRAVERSION = {extra}')
    except ValueError as e:
        print(str(e))
        sys.exit(1)
