#!/usr/bin/env python3
# Copyright (c) 2024 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: Apache-2.0

"""
This module provides functionality for Device Firmware Update (DFU) operations
on Thingy:91 X devices, supporting both nRF53 and nRF91 chips.
"""

import os
import re
import sys
import time
import json
import subprocess
import argparse
import usb.core
import usb.util
import serial
import serial.tools.list_ports
import imgtool.image
from tempfile import TemporaryDirectory
from zipfile import ZipFile

sys.path.append(os.getcwd())
from utils.logger import get_logger

logger = get_logger()

# Constants for DFU commands
DFU_COMMANDS = {
    'RECOVER_NRF53': b"\x8e",
    'RECOVER_NRF91': b"\x8f",
    'RESET_NRF53': b"\x90",
    'RESET_NRF91': b"\x91",
    'READ_NRF53_VERSION': b"\x92"
}

class Thingy91XDFU:
    """
    A class to handle Device Firmware Update operations for Thingy:91 X devices.
    """
    def __init__(self, vid, pid, chip, serial_number):
        self.vid = vid
        self.pid = pid
        self.chip = chip
        self.serial_number = serial_number
        self.device = None
        self.out_endpoint = None
        self.in_endpoint = None

        match = re.search(r"(THINGY91X_)?([A-F0-9]+)", self.serial_number)
        if match is None:
            logger.error("Serial number doesn't match expected format")
            sys.exit(1)
        self.serial_number_digits = match.group(2)

    def release_device(self):
        if self.device:
            usb.util.dispose_resources(self.device)
            self.device = None
            self.out_endpoint = None
            self.in_endpoint = None

    def find_usb_device_with_retry(self, max_attempts=5, delay=10):
        for attempt in range(max_attempts):
            device = self.find_usb_device()
            if device:
                return device
            logger.debug(f"Device not found, retrying (attempt {attempt + 1}/{max_attempts})...")
            time.sleep(delay)
        raise Exception(f"Device not found after {max_attempts} attempts.")

    def find_usb_device(self):
        finder = self._find_usb_device_windows if sys.platform == "win32" else self._find_usb_device_posix
        return finder()

    # def _find_usb_device_posix(self):
    #     devices = list(usb.core.find(find_all=True, idVendor=self.vid, idProduct=self.pid))
    #     if self.serial_number:
    #         device = next((d for d in devices if usb.util.get_string(d, d.iSerialNumber) == self.serial_number), None)
    #         return (device, self.serial_number) if device else (None, None)

    #     if len(devices) == 1:
    #         device = devices[0]
    #         return device, usb.util.get_string(device, device.iSerialNumber)

    #     self._log_device_status(devices)
    #     return None, None

    def _find_usb_device_posix(self):
        # TODO: this function can be refactored a bit
        if self.serial_number is not None:
            device = usb.core.find(idVendor=self.vid, idProduct=self.pid, serial_number=self.serial_number)
            if device is None:
                logger.error(f"Device with serial number {self.serial_number} not found")
                # devices = list(usb.core.find(find_all=True, idVendor=self.vid, idProduct=self.pid))
                # logger.error(f"{len(devices)} available devices:")
                # for device in devices:
                #     snr = usb.util.get_string(device, device.iSerialNumber)
                #     logger.error(f"Serial Number: {snr}")
                return None
            logger.info(f"Device with serial number {self.serial_number} found")
            return device
        devices = list(usb.core.find(find_all=True, idVendor=self.vid, idProduct=self.pid))
        if len(devices) == 1:
            device = devices[0]
            snr = usb.util.get_string(device, device.iSerialNumber)
            return device, snr
        if len(devices) == 0:
            logger.error("No devices found.")
        else:
            logger.info("Multiple devices found.")
            for device in devices:
                snr = usb.util.get_string(device, device.iSerialNumber)
                logger.info(f"Serial Number: {snr}")
            logger.warning(
                "Please specify the serial number with the --serial option."
            )
        return None, None

    def _find_usb_device_windows(self):
        devices = list(usb.core.find(find_all=True, idVendor=self.vid, idProduct=self.pid))
        if len(devices) == 1:
            for com in serial.tools.list_ports.comports():
                if com.vid == self.vid and com.pid == self.pid:
                    if self.serial_number and self.serial_number != com.serial_number:
                        logger.error(f"Device with serial number {self.serial_number} not found")
                        return None, None
                    return devices[0], com.serial_number

        self._log_device_status(devices)
        return None, None

    def _log_device_status(self, devices):
        if not devices:
            logger.error("No devices found.")
        else:
            logger.info("Multiple devices found. Please specify the serial number.")
            for device in devices:
                logger.info(f"Serial Number: {usb.util.get_string(device, device.iSerialNumber)}")

    def prepare_bulk_endpoints(self):
        self.release_device()  # Ensure previous resources are released
        self.device = self.find_usb_device_with_retry()
        if not self.device:
            return False

        if "THINGY91X" not in self.serial_number:
            logger.warning("Device is already in bootloader mode")
            if self.chip != "nrf53":
                logger.error("The device is in nRF53 bootloader mode, but you are trying to flash an nRF91 image. Please program the connectivity_bridge firmware first.")
                return False
            return True

        bulk_interface = self._find_bulk_interface("CMSIS-DAP v2")
        if not bulk_interface:
            logger.error("Bulk interface not found")
            return False

        self.out_endpoint = self._find_endpoint(bulk_interface, usb.util.ENDPOINT_OUT)
        self.in_endpoint = self._find_endpoint(bulk_interface, usb.util.ENDPOINT_IN)

        if not self.out_endpoint or not self.in_endpoint:
            logger.error("Endpoints not found")
            return False

        return True

    def _find_bulk_interface(self, descriptor_string):
        for cfg in self.device:
            for intf in cfg:
                if sys.platform == "linux" and self.device.is_kernel_driver_active(intf.bInterfaceNumber):
                    try:
                        self.device.detach_kernel_driver(intf.bInterfaceNumber)
                    except usb.core.USBError as e:
                        logger.error(f"Could not detach kernel driver: {str(e)}")
                if usb.util.get_string(self.device, intf.iInterface) == descriptor_string:
                    return intf
        return None

    def _find_endpoint(self, interface, direction):
        return next((ep for ep in interface if usb.util.endpoint_direction(ep.bEndpointAddress) == direction), None)

    def read_nrf53_version(self):
        logger.info(f"Reading nRF53 version...")
        if not self.prepare_bulk_endpoints():
            return None

        try:
            self.out_endpoint.write(DFU_COMMANDS['READ_NRF53_VERSION'])
            data = bytes(self.in_endpoint.read(self.in_endpoint.wMaxPacketSize))
            if len(data) < 2 or data[0] != DFU_COMMANDS['READ_NRF53_VERSION'][0] or data[1] == 0xFF:
                logger.error("Failed to read nRF53 version")
                return None
            version_string = data[2:data[2]+2].decode("ascii")
            logger.info(f"nRF53 version: {version_string}")
            return version_string
        except usb.core.USBError as e:
            logger.error(f"Failed to send data: {e}")
            return None
        finally:
            self.release_device()

    def reset_device(self):
        logger.info(f"Resetting {self.chip.upper()}...")
        if not self.prepare_bulk_endpoints():
            return self.serial_number

        command = DFU_COMMANDS[f'RESET_{self.chip.upper()}']
        try:
            self.out_endpoint.write(command)
            logger.debug("Reset command sent successfully.")
            if self.chip == "nrf91":
                data = self.in_endpoint.read(self.in_endpoint.wMaxPacketSize)
                logger.debug(f"Response: {data}")
        except usb.core.USBError as e:
            logger.error(f"Failed to reset device: {e}")
            return None
        finally:
            self.release_device()

        time.sleep(2)  # Add a short delay after reset
        return self.serial_number

    def enter_bootloader_mode(self):
        logger.info(f"Entering bootloader mode on {self.chip.upper()}")
        if not self.prepare_bulk_endpoints():
            return self.serial_number

        if self.chip == "nrf91" and not self._check_nrf91_serial_port():
            return None

        command = DFU_COMMANDS[f'RECOVER_{self.chip.upper()}']
        try:
            self.out_endpoint.write(command)
            logger.debug("Bootloader command sent successfully.")
            if self.chip == "nrf91":
                data = self.in_endpoint.read(self.in_endpoint.wMaxPacketSize)
                logger.debug(f"Response: {data}")
        except usb.core.USBError as e:
            logger.error(f"Failed to enter bootloader mode: {e}")
            return None
        finally:
            self.release_device()

        return self.serial_number

    def _check_nrf91_serial_port(self):
        found_ports = [port.device for port in serial.tools.list_ports.comports()
                       if port.serial_number == self.serial_number]
        if len(found_ports) != 2:
            logger.error(f"Expected two serial ports, but found {len(found_ports)}")
            return False

        try:
            with serial.Serial(min(found_ports), 1000000, timeout=1):
                logger.debug("Serial port opened successfully")
            return True
        except serial.SerialException as e:
            logger.error(f"Failed to open serial port. Is a terminal open? Error: {e}")
            return False

    def wait_for_nrf53_recovery_mode(self, enter_mcuboot=True):
        logger.info(f"Waiting for {self.chip.upper()} mcuboot recovery mode, enter_mcuboot: {enter_mcuboot}")
        logger.debug(f"Serial number digits: {self.serial_number_digits}")

        port_info = None
        logger.debug("Waiting for device to enumerate...")
        mcuboot_serial_number = None
        for _ in range(30):
            time.sleep(5)
            try:
                com_ports = serial.tools.list_ports.comports()
            except TypeError:
                continue
            for port_info in com_ports:
                if port_info.serial_number is None:
                    continue
                logger.debug(f"Serial port: {port_info.device} has serial number: {port_info.serial_number}")
                if self.serial_number_digits in port_info.serial_number:
                    logger.debug(f"Serial port: {port_info.device}")
                    mcuboot_serial_number = port_info.serial_number
                    break
            if mcuboot_serial_number is None:
                continue
            if enter_mcuboot and ("THINGY91X" not in mcuboot_serial_number):
                logger.info(f"nrf53 has entered mcuboot mode")
                break
            if not enter_mcuboot and ("THINGY91X" in mcuboot_serial_number):
                logger.info(f"nrf53 has exited mcuboot mode")
                break
        if mcuboot_serial_number is None:
            logger.error("Serial port not found, unable to detrmine nrf53 recovery mode")
            sys.exit(1)
        return mcuboot_serial_number, port_info.device

    def perform_dfu(self, image):
        logger.info(f"Performing DFU on {self.chip.upper()} with firmware: {image}")
        if self.chip == "nrf53":
            mcuboot_serial_number, port = self.wait_for_nrf53_recovery_mode()
            serial_number = mcuboot_serial_number
        else:
            serial_number = self.serial_number

        command = f"nrfutil device program --serial-number {serial_number} --firmware {image}"
        logger.info(f"Executing command: {command}")
        result = subprocess.run(command, shell=True, capture_output=True, text=True)
        logger.info(f"Command output: {result.stdout}")
        if result.stderr:
            logger.error(f"Command error: {result.stderr}")

        if self.chip == "nrf53":
            mcuboot_serial_number, port = self.wait_for_nrf53_recovery_mode(enter_mcuboot=False)

        if self.chip == "nrf91":
            self.reset_device()

    def perform_bootloader_dfu(self, image, slot=1):
        logger.info(f"Performing MCUBoot DFU on {self.chip.upper()} with firmware: {image}")
        port = None
        if self.chip == "nrf53":
            _ , port = self.wait_for_nrf53_recovery_mode()
        else:
            ports = [port_info.device for port_info in serial.tools.list_ports.comports()
                     if port_info.serial_number == self.serial_number]
            ports.sort()
            port = ports[0]

        with TemporaryDirectory() as tmpdir:
            with ZipFile(image, "r") as zip_ref:
                zip_ref.extractall(tmpdir)
            with open(os.path.join(tmpdir, "manifest.json")) as manifest:
                manifest = json.load(manifest)
            imgfile = os.path.join(tmpdir, manifest["files"][slot]["file"])

            ret, version, digest = imgtool.image.Image.verify(imgfile, None)
            if ret != imgtool.image.VerifyResult.OK:
                logger.error(f"Image verification failed: {ret}")
                sys.exit(1)
            digest = digest.hex()

            if self.chip == "nrf91":
                os.system(f"mcumgr --conntype serial --connstring dev={port},baud=1000000 image list -t 1 || true")

            commands = [
                f"mcumgr --conntype serial --connstring dev={port},baud=1000000 image list",
                f"mcumgr --conntype serial --connstring dev={port},baud=1000000 image upload {imgfile} -n 2",
                f"mcumgr --conntype serial --connstring dev={port},baud=1000000 image confirm {digest}",
                f"mcumgr --conntype serial --connstring dev={port},baud=1000000 reset"
            ]

            for cmd in commands:
                ret = os.system(cmd)
                if ret != 0:
                    logger.error(f"Command failed: {cmd}")
                    sys.exit(1)

            if self.chip == "nrf53":
                _ , port = self.wait_for_nrf53_recovery_mode(enter_mcuboot=False)
            if self.chip == "nrf91":
                time.sleep(5)

            if self.chip == "nrf91":
                self.reset_device()

def detect_family_from_zip(zip_file):
    is_mcuboot = False
    with TemporaryDirectory() as tmpdir:
        with ZipFile(zip_file, "r") as zip_ref:
            zip_ref.extractall(tmpdir)
        try:
            with open(os.path.join(tmpdir, "manifest.json")) as manifest:
                manifest_content = manifest.read()
        except FileNotFoundError:
            logger.error("Manifest file not found")
            return None, is_mcuboot
    if '"type": "mcuboot"' in manifest_content:
        is_mcuboot = True
    if "nrf53" in manifest_content:
        return "nrf53", is_mcuboot
    if "nrf91" in manifest_content:
        return "nrf91", is_mcuboot
    return None, is_mcuboot


def main():
    parser = argparse.ArgumentParser(description="Thingy91X DFU", allow_abbrev=False)
    parser.add_argument("--image", type=str, help="application update file")
    parser.add_argument("--check-nrf53-version", action="store_true", help="check nrf53 version")
    parser.add_argument("--reset-only", action="store_true", help="only reset the device without performing DFU")
    parser.add_argument("--chip", type=str, help="bootloader mode to trigger: nrf53 or nrf91", default="")
    parser.add_argument("--vid", type=int, help="vendor id", default=0x1915)
    parser.add_argument("--pid", type=int, help="product id", default=0x910A)
    parser.add_argument("--serial", type=str, help="serial number", default=None)
    parser.add_argument("--bootloader-slot", type=int, help="bootloader slot", default=1)
    args = parser.parse_args()

    chip = args.chip
    if args.image:
        chip, is_mcuboot = detect_family_from_zip(args.image)
        if chip is None:
            logger.error("Could not determine chip family from image")
            return
    if args.chip and args.chip != chip:
        logger.error("Chip family does not match image")
        return
    if chip not in ["nrf53", "nrf91"]:
        logger.error("Invalid chip")
        return

    dfu = Thingy91XDFU(args.vid, args.pid, chip, args.serial)

    try:
        if args.check_nrf53_version:
            dfu.read_nrf53_version()
        elif args.reset_only:
            serial_number = dfu.reset_device()
            logger.info(f"{chip} on {serial_number} has been reset")
        else:
            serial_number = dfu.enter_bootloader_mode()
            logger.info(f"{chip} on {serial_number} is in bootloader mode")
            if is_mcuboot:
                dfu.perform_bootloader_dfu(args.image, args.bootloader_slot)
            else:
                dfu.perform_dfu(args.image)
    finally:
        dfu.release_device()


if __name__ == "__main__":
    main()
