#!/usr/bin/env python3
#
# Copyright (c) 2024 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause


from intelhex import IntelHex

import hashlib
import argparse
import struct
import ecdsa

def parse_args():
    parser = argparse.ArgumentParser(
        description='Check NSIB signature of a given file',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        allow_abbrev=False)
    parser.add_argument('-i', '--input', required=True, type=argparse.FileType('r', encoding='UTF-8'),
                        help='Input hex file.')
    parser.add_argument('-p', '--public-key', required=True, type=argparse.FileType('r', encoding='UTF-8'),
                        help='Public key file (PEM).')
    parser.add_argument('-a', '--start-address', required=False, type=str, default=None, help='Start address of the image (hex).')
    parser.add_argument('-v', '--version', required=False, type=int, default=None, help='Expected firmware version.')

    args = parser.parse_args()
    return args

def get_fw_info_version(image):
    valid_expected = 0x9102FFFF
    offset = 0
    while True:
        offset = image.find(b'\xde\xe6\x1e\x28\x4c\xbb\xce\x8f', start=offset+1)
        if offset == -1:
            break
        struct_size = struct.unpack('<I', image.tobinstr(start=offset + 12, size=4))[0]
        firmware_size = struct.unpack('<I', image.tobinstr(start=offset + 16, size=4))[0]
        firmware_version = struct.unpack('<I', image.tobinstr(start=offset + 20, size=4))[0]
        image_start = struct.unpack('<I', image.tobinstr(start=offset + 24, size=4))[0]
        boot_address = struct.unpack('<I', image.tobinstr(start=offset + 28, size=4))[0]
        valid = struct.unpack('<I', image.tobinstr(start=offset + 32, size=4))[0]
        print(f"Firmware size: 0x{firmware_size:08x}")
        print(f"Firmware version: {firmware_version}")
        print(f"Image start: 0x{image_start:08x}")
        print(f"Boot address: 0x{boot_address:08x}")
        if valid == valid_expected:
            return firmware_version
        else:
            print(f"Invalid firmware info structure at offset 0x{offset:08x}, valid: 0x{valid:08x}")

def check_signature(image, public_key, start_address, version_expected):
    public_key_bytes_expected = public_key.to_string()

    version = get_fw_info_version(image)
    if version_expected is not None and version != version_expected:
        raise Exception(f"Version mismatch. Expected: {version_expected}, found: {version}")

    signature_locations = []
    offset = 0
    while True:
        offset = image.find(b'\xde\xe6\x1e\x28\x83\x84\x51\x86', start=offset+1)
        if offset == -1:
            break
        signature_locations.append(offset)

    for offset in signature_locations:
        validation_info_version = image[offset + 8]
        info_hardware_id = image[offset + 9]
        validation_info_crypto_id = image[offset + 10]
        info_magic_compatibility_id = image[offset + 11]
        starting_address = struct.unpack('<I', image.tobinstr(start=offset + 12, size=4))[0]
        hash_bytes = image.tobinstr(start=offset + 16, size=32)
        public_key_bytes = image.tobinstr(start=offset + 48, size=64)
        signature_bytes = image.tobinstr(start=offset + 112, size=64)

        print(f"Validation info version: {validation_info_version}")
        print(f"Info hardware ID: {info_hardware_id}")
        print(f"Validation info crypto ID: {validation_info_crypto_id}")
        print(f"Info magic compatibility ID: {info_magic_compatibility_id}")

        if start_address is not None and start_address != starting_address:
            raise Exception(f"Signature found at address 0x{starting_address:08x} but expected 0x{start_address:08x}")

        if public_key_bytes != public_key_bytes_expected:
            print(f"Image has signature for different public key: {public_key_bytes.hex()}")
            continue
        public_key.verify(signature_bytes, hash_bytes, hashfunc=hashlib.sha256)
        print(f"Signature verified for image starting at address 0x{starting_address:08x}")
        return
    raise Exception("No matching signature found in the image.")

def get_image_hex(file_name):
    file_name = str(file_name.name)
    if file_name.endswith('.hex'):
        return IntelHex(file_name)
    elif file_name.endswith('.bin'):
        with open(file_name, 'rb') as f:
            raw_bytes = f.read()
            image = IntelHex()
            image.frombytes(raw_bytes)
            return image

def main():

    args = parse_args()
    public_key = ecdsa.VerifyingKey.from_pem(args.public_key.read())

    if args.start_address is not None:
        start_address = int(args.start_address, 16)
    else:
        start_address = None

    check_signature(get_image_hex(args.input), public_key, start_address, args.version)

if __name__ == '__main__':
    main()
