#!/usr/bin/env python3

import argparse
import datetime
import os
import pathlib
import re
import subprocess
import sys
import threading
import time

import serial


DEFAULT_UART_TIMEOUT = 60 * 15
DEFAULT_SEGGER_INTERFACE = "if02"
DEFAULT_DEVICE_BY_ID_DIR = pathlib.Path("/dev/serial/by-id")


def build_segger_device_path(
    serial_number: str, interface: str = DEFAULT_SEGGER_INTERFACE
) -> str:
    return str(
        DEFAULT_DEVICE_BY_ID_DIR / f"usb-SEGGER_J-Link_{serial_number}-{interface}"
    )


def match_segger_device_by_suffix(
    serial_number: str, interface: str = DEFAULT_SEGGER_INTERFACE
) -> str | None:
    matches = sorted(
        DEFAULT_DEVICE_BY_ID_DIR.glob(
            f"usb-SEGGER_J-Link_*{serial_number}-{interface}"
        )
    )
    if len(matches) == 1:
        return str(matches[0])
    return None


def resolve_device_path(device: str) -> str:
    device = device.strip()

    if device.startswith("/"):
        return device

    if device.startswith("usb-SEGGER_J-Link_"):
        return str(DEFAULT_DEVICE_BY_ID_DIR / device)

    resolved = match_segger_device_by_suffix(device)
    if resolved is not None:
        return resolved

    if device.isdigit():
        resolved = match_segger_device_by_suffix(device.zfill(12))
        if resolved is not None:
            return resolved

    return build_segger_device_path(device)


def check_uart_usage(uart: str) -> None:
    if not os.path.exists(uart):
        raise RuntimeError(f"Uart {uart} does not exist!")

    try:
        result = subprocess.run(["lsof", uart], capture_output=True, check=False)
        if result.stdout:
            for line in result.stdout.splitlines()[1:]:
                fields = re.split(r"\s+", line.decode("utf-8").strip())
                if len(fields) >= 4:
                    command, pid, user = fields[0], fields[1], fields[2]
                    raise RuntimeError(
                        f"Uart {uart} in use!\nCommand: {command}, PID: {pid}, User: {user}"
                    )
    except FileNotFoundError:
        print("Warning: 'lsof' not found, skipping UART usage check.", file=sys.stderr)


class UartBinary:
    def __init__(
        self,
        uart: str,
        timeout: int = DEFAULT_UART_TIMEOUT,
        serial_timeout: int = 5,
        baudrate: int = 1000000,
    ) -> None:
        check_uart_usage(uart)
        self.uart = uart
        self.timeout = timeout
        self.serial_timeout = serial_timeout
        self.baudrate = baudrate
        self.data = b""
        self._evt = threading.Event()
        self._thread = threading.Thread(target=self._uart, daemon=True)
        self._thread.start()
        self._selfdestruct = threading.Timer(timeout, self.selfdestruct)
        self._selfdestruct.start()

    def _open_serial(self) -> serial.Serial:
        serial_port = serial.Serial(
            self.uart, baudrate=self.baudrate, timeout=self.serial_timeout
        )

        if serial_port.in_waiting:
            print(
                f"Warning: UART {self.uart} has {serial_port.in_waiting} unread bytes, "
                "resetting input buffer.",
                file=sys.stderr,
            )
            serial_port.reset_input_buffer()

        if serial_port.out_waiting:
            print(
                f"Warning: UART {self.uart} has {serial_port.out_waiting} unwritten bytes, "
                "resetting output buffer.",
                file=sys.stderr,
            )
            serial_port.reset_output_buffer()

        return serial_port

    def _uart(self) -> None:
        serial_port = self._open_serial()

        while not self._evt.is_set():
            try:
                data = serial_port.read(8192)
            except serial.serialutil.SerialException:
                print(
                    f"Warning: Serial exception on {self.uart}, retrying.",
                    file=sys.stderr,
                )
                serial_port.close()
                time.sleep(1)
                if self._evt.is_set():
                    return
                serial_port = self._open_serial()
                continue

            if not data:
                continue

            self.data += data

        serial_port.close()

    def selfdestruct(self) -> None:
        print(f"UART SELFDESTRUCTED {self.uart}", file=sys.stderr)
        self.stop()

    def stop(self) -> None:
        self._selfdestruct.cancel()
        self._evt.set()
        self._thread.join()


def device_label(device: str) -> str:
    return pathlib.Path(device).name


def capture_output_path(output_dir: pathlib.Path, device: str, capture_date: datetime.date) -> pathlib.Path:
    return output_dir / f"{device_label(device)}_{capture_date.isoformat()}.bin"


def open_daily_outputs(output_dir: pathlib.Path, devices: list[str], capture_date: datetime.date) -> dict[str, object]:
    outputs = {}
    for device in devices:
        output_path = capture_output_path(output_dir, device, capture_date)
        outputs[device] = output_path.open("ab")
    return outputs


def rotate_daily_outputs(
    output_dir: pathlib.Path,
    devices: list[str],
    current_date: datetime.date,
    raw_outputs: dict[str, object],
) -> tuple[datetime.date, dict[str, object]]:
    new_date = datetime.date.today()
    if new_date == current_date:
        return current_date, raw_outputs

    for raw_output in raw_outputs.values():
        raw_output.close()

    print(f"Rotating capture files for {new_date.isoformat()}.")
    return new_date, open_daily_outputs(output_dir, devices, new_date)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Listen to a Linux serial device and dump binary data."
    )
    parser.add_argument(
        "--device",
        action="append",
        required=True,
        help=(
            "SEGGER serial number or full serial device path to capture; repeat "
            "--device to listen on multiple UARTs "
            f"(plain serial numbers resolve to /dev/serial/by-id/usb-SEGGER_J-Link_<serial>-{DEFAULT_SEGGER_INTERFACE})."
        ),
    )
    parser.add_argument(
        "--baudrate",
        type=int,
        default=1000000,
        help="Serial baudrate passed to UartBinary (default: 1000000).",
    )
    parser.add_argument(
        "--serial-timeout",
        type=int,
        default=5,
        help="Underlying pyserial read timeout in seconds (default: 5).",
    )
    parser.add_argument(
        "--timeout",
        type=int,
        default=7 * 24 * 60 * 60,
        help="How long the helper thread is allowed to run before self-destructing (default: 604800).",
    )
    parser.add_argument(
        "--poll-interval",
        type=float,
        default=0.1,
        help="How often to poll for newly received bytes (default: 0.1).",
    )
    parser.add_argument(
        "--output-dir",
        type=pathlib.Path,
        default=pathlib.Path.cwd(),
        help="Directory where daily raw .bin files are stored (default: current working directory).",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    devices = list(dict.fromkeys(resolve_device_path(device) for device in args.device))
    uarts = {}
    offsets = {}
    output_dir = args.output_dir.resolve()
    capture_date = datetime.date.today()
    raw_outputs = {}

    try:
        for device in devices:
            uarts[device] = UartBinary(
                uart=device,
                timeout=args.timeout,
                serial_timeout=args.serial_timeout,
                baudrate=args.baudrate,
            )
            offsets[device] = 0

        output_dir.mkdir(parents=True, exist_ok=True)
        raw_outputs = open_daily_outputs(output_dir, devices, capture_date)

        print(
            f"Listening on {len(devices)} device(s) at {args.baudrate} baud. Press Ctrl+C to stop."
        )
        print(f"Daily capture directory: {output_dir}")
        for device in devices:
            print(f"  {device} -> {capture_output_path(output_dir, device, capture_date)}")

        while True:
            capture_date, raw_outputs = rotate_daily_outputs(
                output_dir=output_dir,
                devices=devices,
                current_date=capture_date,
                raw_outputs=raw_outputs,
            )

            for device, uart in uarts.items():
                data = uart.data
                offset = offsets[device]
                if len(data) <= offset:
                    continue

                chunk = data[offset:]
                offsets[device] = len(data)

                raw_output = raw_outputs.get(device)
                if raw_output is not None:
                    raw_output.write(chunk)
                    raw_output.flush()

            time.sleep(args.poll_interval)
    except KeyboardInterrupt:
        print("\nStopping listener.")
        return 0
    finally:
        for raw_output in raw_outputs.values():
            raw_output.close()
        for uart in uarts.values():
            uart.stop()


if __name__ == "__main__":
    raise SystemExit(main())
