#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import os
import re
import signal
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

import fcntl
import serial


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PIO = os.environ.get("PHYTALLY_PIO", "pio")
DEFAULT_ESPTOOL = os.environ.get("PHYTALLY_ESPTOOL", "esptool")
DEFAULT_CORE_BASE = Path(os.environ.get("PHYTALLY_PIO_CORE_BASE", "/tmp/phytally-pio-cores"))
DEVICE_LIST_CORE = DEFAULT_CORE_BASE / "device-list"
GLOBAL_LOCK_PATH = Path(tempfile.gettempdir()) / "phytally-rx-deploy.lock"


ENV_BY_CHIP = {
    "esp32": "tally_rx",
    "esp32-s3": "tally_rx_esp32s3",
    "esp32-c6": "tally_rx_esp32c6",
    "esp8266": "tally_rx_esp8266",
}


@dataclass(frozen=True)
class PortInfo:
    port: str
    description: str
    hwid: str


@dataclass(frozen=True)
class DeviceTarget:
    port: str
    env: str
    chip: str
    mac: str | None
    description: str


class CommandError(RuntimeError):
    pass


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Safely detect connected ESP-based PhyTally receivers and deploy "
            "the matching PlatformIO firmware sequentially."
        )
    )
    parser.add_argument("--port", action="append", dest="ports", default=[],
                        help="Restrict probing to this serial port. Repeatable.")
    parser.add_argument("--env", action="append", dest="envs", default=[],
                        help="Restrict deployment to this PlatformIO environment. Repeatable.")
    parser.add_argument("--port-env", action="append", dest="port_envs", default=[],
                        help="Manually map a port to an env, e.g. /dev/cu.usbmodem1301=tally_rx_esp32c6")
    parser.add_argument("--list-only", action="store_true",
                        help="List detected supported devices and exit without building or flashing.")
    parser.add_argument("--dry-run", action="store_true",
                        help="Print what would be built and flashed without doing it.")
    parser.add_argument("--probe-timeout", type=int, default=6,
                        help="Per-port esptool probe timeout in seconds. Default: 6")
    parser.add_argument("--build-timeout", type=int, default=900,
                        help="Per-env build timeout in seconds. Default: 900")
    parser.add_argument("--upload-timeout", type=int, default=240,
                        help="Per-device upload timeout in seconds. Default: 240")
    parser.add_argument("--verbose", action="store_true", help="Stream subprocess output live.")
    return parser.parse_args()


def run_command(
    cmd: list[str],
    *,
    timeout: int,
    env: dict[str, str] | None = None,
    verbose: bool = False,
) -> subprocess.CompletedProcess[str]:
    proc = subprocess.Popen(
        cmd,
        cwd=ROOT,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        start_new_session=True,
    )
    try:
        output, _ = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired as exc:
        try:
            os.killpg(proc.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            output, _ = proc.communicate(timeout=3)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            output, _ = proc.communicate()
        raise CommandError(f"timed out after {timeout}s: {' '.join(cmd)}\n{output}") from exc

    if verbose and output:
        print(output, end="" if output.endswith("\n") else "\n")

    return subprocess.CompletedProcess(cmd, proc.returncode, output, "")


def pio_env(core_dir: Path) -> dict[str, str]:
    env = os.environ.copy()
    env["PLATFORMIO_CORE_DIR"] = str(core_dir)
    return env


def list_serial_ports(verbose: bool) -> list[PortInfo]:
    result = run_command(
        [DEFAULT_PIO, "device", "list", "--json-output"],
        timeout=20,
        env=pio_env(DEVICE_LIST_CORE),
        verbose=verbose,
    )
    if result.returncode != 0:
        raise CommandError(result.stdout.strip() or "pio device list failed")

    raw = json.loads(result.stdout)
    ports = []
    for item in raw:
        port = item.get("port")
        if not isinstance(port, str):
            continue
        ports.append(PortInfo(
            port=port,
            description=str(item.get("description", "")),
            hwid=str(item.get("hwid", "")),
        ))
    return ports


def parse_hwid_value(hwid: str, key: str) -> str | None:
    match = re.search(rf"{re.escape(key)}=([^ ]+)", hwid)
    return match.group(1) if match else None


def port_group_key(port: PortInfo) -> str:
    serial = parse_hwid_value(port.hwid, "SER")
    location = parse_hwid_value(port.hwid, "LOCATION")
    if serial or location:
        return f"{serial or '-'}@{location or '-'}"
    return port.port


def port_rank(port: PortInfo) -> tuple[int, str]:
    name = port.port.lower()
    if "usbmodem" in name:
        return (0, name)
    if "wchusbserial" in name:
        return (1, name)
    if "usbserial-" in name:
        return (2, name)
    if "slab_usbtouart" in name:
        return (3, name)
    return (9, name)


def canonical_ports(ports: Iterable[PortInfo]) -> list[PortInfo]:
    grouped: dict[str, list[PortInfo]] = {}
    for port in ports:
        grouped.setdefault(port_group_key(port), []).append(port)

    selected = []
    for items in grouped.values():
        selected.append(sorted(items, key=port_rank)[0])
    return sorted(selected, key=lambda p: p.port)


def classify_chip(output: str) -> tuple[str | None, str | None]:
    chip_match = re.search(r"Chip is ([^\r\n]+)", output)
    mac_match = re.search(r"MAC:\s*([0-9A-Fa-f:]{17})", output)
    chip_text = chip_match.group(1).strip().lower() if chip_match else None
    mac = mac_match.group(1).upper() if mac_match else None

    if not chip_text:
        return None, mac
    if "esp32-s3" in chip_text:
        return "esp32-s3", mac
    if "esp32-c6" in chip_text:
        return "esp32-c6", mac
    if "esp8266" in chip_text:
        return "esp8266", mac
    if "esp32" in chip_text:
        return "esp32", mac
    return None, mac


def heuristic_chip(port: PortInfo) -> str | None:
    hwid = port.hwid.upper()
    name = port.port.lower()
    if "VID:PID=303A:1001" in hwid:
        return "esp32-c6"
    if "VID:PID=1A86:55D3" in hwid:
        return "esp32-s3"
    if "VID:PID=10C4:EA60" in hwid and ("usbserial-" in name or "slab_usbtouart" in name):
        return None
    return None


def serial_probe_chip(port: PortInfo) -> str | None:
    try:
        with serial.Serial(port.port, 115200, timeout=0.25, write_timeout=0.25) as ser:
            ser.reset_input_buffer()
            ser.reset_output_buffer()
            ser.write(b"GET_INFO\n")
            ser.flush()
            deadline = time.monotonic() + 1.5
            chunks: list[str] = []
            while time.monotonic() < deadline:
                line = ser.readline()
                if not line:
                    continue
                text = line.decode("utf-8", errors="ignore").strip()
                chunks.append(text)
                if "INFO:" not in text:
                    continue
                if "VER=8266-" in text:
                    return "esp8266"
                if "VER=C6-" in text:
                    return "esp32-c6"
                if "VER=1.2" in text:
                    heuristic = heuristic_chip(port)
                    return heuristic or "esp32"
            combined = "\n".join(chunks)
            if "VER=8266-" in combined:
                return "esp8266"
            if "VER=C6-" in combined:
                return "esp32-c6"
            if "VER=1.2" in combined:
                heuristic = heuristic_chip(port)
                return heuristic or "esp32"
    except (serial.SerialException, OSError):
        return None
    return None


def probe_port(port: PortInfo, timeout: int, verbose: bool) -> DeviceTarget | None:
    serial_chip = serial_probe_chip(port)
    if serial_chip:
        return DeviceTarget(
            port=port.port,
            env=ENV_BY_CHIP[serial_chip],
            chip=serial_chip,
            mac=None,
            description=port.description,
        )

    cmd = [DEFAULT_ESPTOOL, "--port", port.port, "chip-id"]
    try:
        result = run_command(cmd, timeout=timeout, verbose=verbose)
    except CommandError:
        fallback = heuristic_chip(port)
        if not fallback:
            return None
        return DeviceTarget(
            port=port.port,
            env=ENV_BY_CHIP[fallback],
            chip=fallback,
            mac=None,
            description=port.description,
        )

    if result.returncode != 0:
        fallback = heuristic_chip(port)
        if not fallback:
            return None
        return DeviceTarget(
            port=port.port,
            env=ENV_BY_CHIP[fallback],
            chip=fallback,
            mac=None,
            description=port.description,
        )

    chip, mac = classify_chip(result.stdout)
    if not chip:
        return None

    return DeviceTarget(
        port=port.port,
        env=ENV_BY_CHIP[chip],
        chip=chip,
        mac=mac,
        description=port.description,
    )


def parse_manual_mappings(raw_items: list[str]) -> dict[str, str]:
    mappings: dict[str, str] = {}
    for item in raw_items:
        if "=" not in item:
            raise SystemExit(f"invalid --port-env value: {item!r}")
        port, env = item.split("=", 1)
        env = env.strip()
        if env not in set(ENV_BY_CHIP.values()):
            raise SystemExit(f"unsupported env in --port-env: {env}")
        mappings[port.strip()] = env
    return mappings


def detect_targets(args: argparse.Namespace) -> list[DeviceTarget]:
    manual = parse_manual_mappings(args.port_envs)
    requested_ports = set(args.ports)
    requested_envs = set(args.envs)

    ports = list_serial_ports(args.verbose)
    ports = [p for p in ports if p.port.startswith("/dev/cu.") or p.port.startswith("/dev/tty.")]
    if requested_ports:
        ports = [p for p in ports if p.port in requested_ports]
    ports = canonical_ports(ports)

    targets: list[DeviceTarget] = []
    seen_mac_or_port: set[str] = set()
    for port in ports:
        env = manual.get(port.port)
        if env:
            target = DeviceTarget(
                port=port.port,
                env=env,
                chip=env.replace("tally_rx_", "").replace("tally_rx", "esp32"),
                mac=None,
                description=port.description,
            )
        else:
            target = probe_port(port, timeout=args.probe_timeout, verbose=args.verbose)
        if not target:
            continue
        if requested_envs and target.env not in requested_envs:
            continue
        dedupe_key = target.mac or target.port
        if dedupe_key in seen_mac_or_port:
            continue
        seen_mac_or_port.add(dedupe_key)
        targets.append(target)
    return targets


def build_env(env_name: str, timeout: int, verbose: bool) -> None:
    core_dir = DEFAULT_CORE_BASE / env_name
    result = run_command(
        [DEFAULT_PIO, "run", "-e", env_name],
        timeout=timeout,
        env=pio_env(core_dir),
        verbose=verbose,
    )
    if result.returncode != 0:
        raise CommandError(result.stdout.strip() or f"build failed for {env_name}")


def upload_target(target: DeviceTarget, timeout: int, verbose: bool) -> None:
    core_dir = DEFAULT_CORE_BASE / target.env
    result = run_command(
        [DEFAULT_PIO, "run", "-e", target.env, "-t", "nobuild", "-t", "upload", "--upload-port", target.port],
        timeout=timeout,
        env=pio_env(core_dir),
        verbose=verbose,
    )
    if result.returncode != 0:
        raise CommandError(result.stdout.strip() or f"upload failed for {target.port}")


def main() -> int:
    args = parse_args()

    GLOBAL_LOCK_PATH.parent.mkdir(parents=True, exist_ok=True)
    with GLOBAL_LOCK_PATH.open("w") as lock_file:
        try:
            fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            print("another receiver deployment is already running", file=sys.stderr)
            return 2

        targets = detect_targets(args)
        if not targets:
            print("no supported receivers detected")
            return 0

        print("Detected supported receivers:")
        for target in targets:
            mac_suffix = f" mac={target.mac}" if target.mac else ""
            print(f"  {target.port}: {target.chip} -> {target.env}{mac_suffix}")

        if args.list_only or args.dry_run:
            return 0

        built_envs: set[str] = set()
        for target in targets:
            if target.env in built_envs:
                continue
            print(f"\nBuilding {target.env} ...")
            build_env(target.env, timeout=args.build_timeout, verbose=args.verbose)
            built_envs.add(target.env)

        for target in targets:
            print(f"\nFlashing {target.port} with {target.env} ...")
            upload_target(target, timeout=args.upload_timeout, verbose=args.verbose)

        print("\nDeployment complete.")
        return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except CommandError as exc:
        print(str(exc), file=sys.stderr)
        raise SystemExit(1)
