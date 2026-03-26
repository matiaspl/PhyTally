#!/usr/bin/env python3

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

import tx_debug


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_ENV = os.environ.get("PHYTALLY_TX_ENV", "tally_tx_esp32s3")
DEFAULT_PIO = os.environ.get("PHYTALLY_PIO", "pio")
DEFAULT_PIO_CORE_DIR = os.environ.get("PLATFORMIO_CORE_DIR", "/tmp/pio-core-usbncm-fix")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build and flash the ESP32-S3 TX firmware.")
    subparsers = parser.add_subparsers(dest="command", required=True)

    build_parser = subparsers.add_parser("build", help="Build the TX firmware image.")
    build_parser.add_argument("--env", default=DEFAULT_ENV, help=f"PlatformIO environment. Default: {DEFAULT_ENV}")

    fs_parser = subparsers.add_parser("uploadfs", help="Upload LittleFS assets to the TX.")
    fs_parser.add_argument("--env", default=DEFAULT_ENV, help=f"PlatformIO environment. Default: {DEFAULT_ENV}")
    fs_parser.add_argument("--uart-port", help="UART upload port. Defaults to the detected WCH serial port.")

    flash_parser = subparsers.add_parser("flash", help="Build and flash the TX firmware.")
    flash_parser.add_argument("--env", default=DEFAULT_ENV, help=f"PlatformIO environment. Default: {DEFAULT_ENV}")
    flash_parser.add_argument("--uart-port", help="UART upload port. Defaults to the detected WCH serial port.")
    flash_parser.add_argument("--fs", action="store_true", help="Upload LittleFS assets before flashing firmware.")
    flash_parser.add_argument("--no-build", action="store_true", help="Skip the build step and upload the existing image.")
    flash_parser.add_argument("--retest", action="store_true", help="Run tx_debug.py retest after flashing.")
    flash_parser.add_argument("--interface", default=tx_debug.DEFAULT_INTERFACE, help=f"USB-NCM interface. Default: {tx_debug.DEFAULT_INTERFACE}")
    flash_parser.add_argument("--interface-ip", default=tx_debug.DEFAULT_INTERFACE_IP, help=f"Expected host IPv4. Default: {tx_debug.DEFAULT_INTERFACE_IP}")
    flash_parser.add_argument("--host", default=tx_debug.DEFAULT_HOST, help=f"Target host. Default: {tx_debug.DEFAULT_HOST}")
    flash_parser.add_argument("--attempts", type=int, default=2, help="Reset attempts for retest. Default: 2")
    flash_parser.add_argument("--wait-seconds", type=float, default=tx_debug.DEFAULT_RESET_WAIT, help=f"Boot marker wait. Default: {tx_debug.DEFAULT_RESET_WAIT}")
    flash_parser.add_argument("--interface-wait-seconds", type=float, default=tx_debug.DEFAULT_INTERFACE_WAIT, help=f"USB-NCM wait. Default: {tx_debug.DEFAULT_INTERFACE_WAIT}")
    flash_parser.add_argument("--count", type=int, default=1, help="HTTP smoke rounds during retest. Default: 1")
    flash_parser.add_argument("--timeout", type=float, default=5.0, help="Per-request timeout during retest. Default: 5")
    flash_parser.add_argument("--ping-count", type=int, default=1, help="Ping count during retest. Default: 1")
    flash_parser.add_argument(
        "--path",
        action="append",
        dest="paths",
        default=[],
        help="HTTP path for retest. Repeatable. Default: / then /api/v1/status",
    )

    return parser.parse_args()


def run_command(cmd: list[str], *, check: bool = True) -> int:
    env = os.environ.copy()
    if DEFAULT_PIO_CORE_DIR:
        env["PLATFORMIO_CORE_DIR"] = DEFAULT_PIO_CORE_DIR
    print("$", " ".join(cmd))
    result = subprocess.run(cmd, cwd=ROOT, env=env)
    if check and result.returncode != 0:
        raise SystemExit(result.returncode)
    return result.returncode


def pio_run_args(environment: str) -> list[str]:
    return [DEFAULT_PIO, "run", "-d", str(ROOT), "-e", environment]


def build(environment: str) -> None:
    run_command(pio_run_args(environment))


def candidate_upload_ports(preferred_port: str | None) -> list[str]:
    if preferred_port:
        return [preferred_port]
    modems, uarts = tx_debug.likely_ports()
    if not uarts:
        return modems

    def suffix(path: str, marker: str) -> str:
        name = Path(path).name
        index = name.find(marker)
        return name[index + len(marker):] if index >= 0 else name

    uart_suffixes = {suffix(port, "wchusbserial") for port in uarts}
    matched_modems = [
        port for port in modems
        if suffix(port, "usbmodem") in uart_suffixes
    ]
    return uarts + matched_modems


def run_upload_target(environment: str, target: str, preferred_port: str | None) -> str:
    candidates = candidate_upload_ports(preferred_port)
    if not candidates:
        raise SystemExit("No candidate upload ports detected.")

    errors: list[str] = []
    for port in candidates:
        rc = run_command(
            pio_run_args(environment) + ["-t", target, "--upload-port", port],
            check=False,
        )
        if rc == 0:
            return port
        errors.append(f"{port}: exit {rc}")

    raise SystemExit(f"{target} failed on all candidate ports: {'; '.join(errors)}")


def uploadfs(environment: str, preferred_port: str | None) -> str:
    return run_upload_target(environment, "uploadfs", preferred_port)


def upload(environment: str, preferred_port: str | None) -> str:
    return run_upload_target(environment, "upload", preferred_port)


def run_retest(args: argparse.Namespace, uart_port: str) -> None:
    reset_port = uart_port
    if "wchusbserial" not in reset_port:
        try:
            reset_port = tx_debug.default_uart_port()
        except SystemExit:
            reset_port = uart_port
    python = tx_debug.python_with_pyserial() or sys.executable
    cmd = [
        python,
        str(ROOT / "scripts" / "tx_debug.py"),
        "retest",
        "--uart-port",
        reset_port,
        "--interface",
        args.interface,
        "--interface-ip",
        args.interface_ip,
        "--host",
        args.host,
        "--attempts",
        str(args.attempts),
        "--wait-seconds",
        str(args.wait_seconds),
        "--interface-wait-seconds",
        str(args.interface_wait_seconds),
        "--count",
        str(args.count),
        "--timeout",
        str(args.timeout),
        "--ping-count",
        str(args.ping_count),
    ]
    for path in args.paths or ["/", "/api/v1/status"]:
        cmd.extend(["--path", path])
    run_command(cmd)


def main() -> int:
    args = parse_args()

    if args.command == "build":
        build(args.env)
        return 0

    if args.command == "uploadfs":
        uploadfs(args.env, args.uart_port)
        return 0

    if args.command == "flash":
        uart_port = args.uart_port
        if not args.no_build:
            build(args.env)
        if args.fs:
            uart_port = uploadfs(args.env, uart_port)
        uart_port = upload(args.env, uart_port)
        if args.retest:
            run_retest(args, uart_port)
        return 0

    raise SystemExit(f"unsupported command: {args.command}")


if __name__ == "__main__":
    raise SystemExit(main())
