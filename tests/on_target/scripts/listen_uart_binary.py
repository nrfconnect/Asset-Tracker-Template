#!/usr/bin/env python3

import argparse
import datetime
import pathlib
import string
import sys
import time


SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
ON_TARGET_DIR = SCRIPT_DIR.parent
if str(ON_TARGET_DIR) not in sys.path:
    sys.path.insert(0, str(ON_TARGET_DIR))

from utils.uart import UartBinary


def format_ascii(data: bytes) -> str:
    printable = set(string.printable.encode("ascii"))
    return "".join(chr(byte) if byte in printable and byte not in b"\r\n\t\x0b\x0c" else "." for byte in data)


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
        description="Listen to one or more Linux serial devices and dump binary data."
    )
    parser.add_argument(
        "devices",
        nargs="+",
        help="One or more serial device paths, for example /dev/serial/by-id/usb-SEGGER_J-Link_001052082975-if02",
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
    uarts = {}
    offsets = {}
    output_dir = args.output_dir.resolve()
    capture_date = datetime.date.today()
    raw_outputs = {}

    try:
        for device in args.devices:
            uarts[device] = UartBinary(
                uart=device,
                timeout=args.timeout,
                serial_timeout=args.serial_timeout,
                baudrate=args.baudrate,
            )
            offsets[device] = 0

        output_dir.mkdir(parents=True, exist_ok=True)
        raw_outputs = open_daily_outputs(output_dir, args.devices, capture_date)

        print(f"Listening on {len(args.devices)} device(s) at {args.baudrate} baud. Press Ctrl+C to stop.")
        print(f"Daily capture directory: {output_dir}")
        for device in args.devices:
            print(f"  {device} -> {capture_output_path(output_dir, device, capture_date)}")

        while True:
            capture_date, raw_outputs = rotate_daily_outputs(
                output_dir=output_dir,
                devices=args.devices,
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

                print(f"[{device_label(device)}] {chunk.hex(' ')}  |{format_ascii(chunk)}|")

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
