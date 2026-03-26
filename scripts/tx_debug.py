#!/usr/bin/env python3

from __future__ import annotations

import argparse
import http.client
import os
import re
import socket
import subprocess
import sys
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_HOST = os.environ.get("PHYTALLY_TX_HOST", "192.168.7.1")
DEFAULT_INTERFACE = os.environ.get("PHYTALLY_TX_INTERFACE", "en22")
DEFAULT_INTERFACE_IP = os.environ.get("PHYTALLY_TX_INTERFACE_IP", "192.168.7.2")
DEFAULT_BAUD = int(os.environ.get("PHYTALLY_TX_BAUD", "115200"))
DEFAULT_PIO = os.environ.get("PHYTALLY_PIO", "pio")
DEFAULT_RESET_WAIT = float(os.environ.get("PHYTALLY_TX_RESET_WAIT", "12"))
DEFAULT_INTERFACE_WAIT = float(os.environ.get("PHYTALLY_TX_INTERFACE_WAIT", "25"))
DEFAULT_PYTHON = os.environ.get("PHYTALLY_TX_PYTHON", "")
BOOT_MARKERS = (
    "ESP-ROM",
    "rst:",
    "waiting for download",
    "USB-NCM init:",
    "HTTP server starting on USB-NCM",
    "SETUP checkpoint:",
)


def _python_candidates() -> list[str]:
    candidates: list[str] = []

    def add(path: str | None) -> None:
        if path and path not in candidates:
            candidates.append(path)

    add(DEFAULT_PYTHON or None)
    add(sys.executable)
    add(str(Path.home() / ".platformio" / "penv" / "bin" / "python"))
    add("/tmp/pio-core-usbncm-fix/penv/bin/python")
    add("/usr/bin/python3")
    add("python3")
    return candidates


def python_with_pyserial() -> str | None:
    for candidate in _python_candidates():
        try:
            result = subprocess.run(
                [candidate, "-c", "import serial"],
                capture_output=True,
                text=True,
            )
        except OSError:
            continue
        if result.returncode == 0:
            return candidate
    return None


def ensure_pyserial_runtime(command: str) -> None:
    if command not in {"reset", "retest"}:
        return
    try:
        import serial  # type: ignore  # noqa: F401
        return
    except ImportError:
        python = python_with_pyserial()
        if not python:
            raise SystemExit("pyserial is required for reset support.")
        current = Path(sys.executable).resolve()
        replacement = Path(python).resolve() if Path(python).exists() else None
        if replacement is not None and replacement == current:
            raise SystemExit("pyserial is required for reset support.")
        os.execvp(python, [python, str(Path(__file__).resolve()), *sys.argv[1:]])


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="ESP32-S3 TX USB-NCM debug helper."
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    subparsers.add_parser("ports", help="List likely TX serial ports and interface state.")

    reset_parser = subparsers.add_parser("reset", help="Reset the TX and verify that it rebooted.")
    reset_parser.add_argument("--uart-port", help="UART/debug serial port, e.g. /dev/cu.wchusbserial...")
    reset_parser.add_argument(
        "--monitor-port",
        help="Deprecated native USB serial path hint. Reset verification now watches the UART/debug port.",
    )
    reset_parser.add_argument("--interface", default=DEFAULT_INTERFACE,
                              help=f"USB-NCM interface to wait for. Default: {DEFAULT_INTERFACE}")
    reset_parser.add_argument("--interface-ip", default=DEFAULT_INTERFACE_IP,
                              help=f"Expected host IPv4 on the USB-NCM interface. Default: {DEFAULT_INTERFACE_IP}")
    reset_parser.add_argument("--pulse-ms", type=int, default=100, help="RTS high pulse width. Default: 100")
    reset_parser.add_argument("--settle-ms", type=int, default=400, help="Wait after reset. Default: 400")
    reset_parser.add_argument("--wait-seconds", type=float, default=DEFAULT_RESET_WAIT,
                              help=f"Maximum time to confirm reboot. Default: {DEFAULT_RESET_WAIT}")
    reset_parser.add_argument("--interface-wait-seconds", type=float, default=DEFAULT_INTERFACE_WAIT,
                              help=f"Maximum time to wait for the USB-NCM interface. Default: {DEFAULT_INTERFACE_WAIT}")
    reset_parser.add_argument("--attempts", type=int, default=2,
                              help="Maximum reset attempts before failing. Default: 2")

    monitor_parser = subparsers.add_parser("monitor", help="Open PlatformIO serial monitor on the TX modem port.")
    monitor_parser.add_argument("--monitor-port", help="USB modem port, e.g. /dev/cu.usbmodem...")
    monitor_parser.add_argument("--baud", type=int, default=DEFAULT_BAUD, help=f"Monitor baud. Default: {DEFAULT_BAUD}")
    monitor_parser.add_argument("--pio", default=DEFAULT_PIO, help=f"PlatformIO executable. Default: {DEFAULT_PIO}")

    smoke_parser = subparsers.add_parser("smoke", help="Run ping plus sequential HTTP checks.")
    smoke_parser.add_argument("--host", default=DEFAULT_HOST, help=f"Target host. Default: {DEFAULT_HOST}")
    smoke_parser.add_argument("--interface", default=DEFAULT_INTERFACE, help=f"USB-NCM interface. Default: {DEFAULT_INTERFACE}")
    smoke_parser.add_argument("--interface-ip", default=DEFAULT_INTERFACE_IP,
                              help=f"Expected host IPv4 on the USB-NCM interface. Default: {DEFAULT_INTERFACE_IP}")
    smoke_parser.add_argument("--count", type=int, default=10, help="Number of sequential path rounds. Default: 10")
    smoke_parser.add_argument("--timeout", type=float, default=5.0, help="Per-request timeout in seconds. Default: 5")
    smoke_parser.add_argument("--ping-count", type=int, default=2, help="ICMP echo count. Default: 2")
    smoke_parser.add_argument(
        "--path",
        action="append",
        dest="paths",
        default=[],
        help="HTTP path to request. Repeatable. Default: / then /api/v1/status",
    )

    retest_parser = subparsers.add_parser(
        "retest",
        help="Reset the TX, wait for the interface to come back, then run smoke checks.",
    )
    retest_parser.add_argument("--uart-port", help="UART/debug serial port, e.g. /dev/cu.wchusbserial...")
    retest_parser.add_argument("--interface", default=DEFAULT_INTERFACE, help=f"USB-NCM interface. Default: {DEFAULT_INTERFACE}")
    retest_parser.add_argument("--interface-ip", default=DEFAULT_INTERFACE_IP,
                               help=f"Expected host IPv4 on the USB-NCM interface. Default: {DEFAULT_INTERFACE_IP}")
    retest_parser.add_argument("--host", default=DEFAULT_HOST, help=f"Target host. Default: {DEFAULT_HOST}")
    retest_parser.add_argument("--wait-seconds", type=float, default=12.0,
                               help="Maximum time to confirm ESP reboot markers. Default: 12")
    retest_parser.add_argument("--interface-wait-seconds", type=float, default=DEFAULT_INTERFACE_WAIT,
                               help=f"Maximum time to wait for the USB-NCM interface. Default: {DEFAULT_INTERFACE_WAIT}")
    retest_parser.add_argument("--attempts", type=int, default=2,
                               help="Maximum reset attempts before failing. Default: 2")
    retest_parser.add_argument("--count", type=int, default=10, help="Number of sequential path rounds. Default: 10")
    retest_parser.add_argument("--timeout", type=float, default=5.0, help="Per-request timeout in seconds. Default: 5")
    retest_parser.add_argument("--ping-count", type=int, default=2, help="ICMP echo count. Default: 2")
    retest_parser.add_argument(
        "--path",
        action="append",
        dest="paths",
        default=[],
        help="HTTP path to request. Repeatable. Default: / then /api/v1/status",
    )

    return parser.parse_args()


def likely_ports() -> tuple[list[str], list[str]]:
    dev_dir = Path("/dev")
    modems = sorted(str(path) for path in dev_dir.glob("cu.usbmodem*"))
    uarts = sorted(str(path) for path in dev_dir.glob("cu.wchusbserial*"))
    return modems, uarts


def preferred_port_candidates(explicit: str | None = None, *, prefer: str = "uart") -> list[str]:
    modems, uarts = likely_ports()
    ordered: list[str] = []

    def add(port: str | None) -> None:
        if port and port not in ordered:
            ordered.append(port)

    add(explicit)
    primary = reversed(uarts) if prefer == "uart" else reversed(modems)
    secondary = reversed(modems) if prefer == "uart" else reversed(uarts)
    for port in primary:
        add(port)
    for port in secondary:
        add(port)
    return ordered


def default_monitor_port() -> str:
    candidates = preferred_port_candidates(prefer="modem")
    if not candidates:
        raise SystemExit("No /dev/cu.usbmodem* or /dev/cu.wchusbserial* port found.")
    return candidates[0]


def default_uart_port() -> str:
    candidates = preferred_port_candidates(prefer="uart")
    if not candidates:
        raise SystemExit("No /dev/cu.wchusbserial* or /dev/cu.usbmodem* port found.")
    return candidates[0]


def port_exists(port: str) -> bool:
    return Path(port).exists()


def run_command(cmd: list[str], *, check: bool = True) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(cmd, capture_output=True, text=True)
    if check and result.returncode != 0:
        message = result.stderr.strip() or result.stdout.strip() or f"command failed: {' '.join(cmd)}"
        raise RuntimeError(message)
    return result


def print_interface_status(interface: str) -> bool:
    result = subprocess.run(["ifconfig", interface], capture_output=True, text=True)
    if result.returncode != 0:
        print(f"{interface}: unavailable")
        return False
    print(result.stdout.strip())
    return "status: active" in result.stdout


def print_arp_state(host: str) -> None:
    result = subprocess.run(["arp", "-an"], capture_output=True, text=True)
    if result.returncode != 0:
        return
    host_re = re.escape(host)
    for line in result.stdout.splitlines():
        if re.search(rf"\({host_re}\)", line):
            print(line)
            return


def cmd_ports(args: argparse.Namespace) -> int:
    modems, uarts = likely_ports()
    print("Monitor ports:")
    for item in modems:
        print(f"  {item}")
    print("UART ports:")
    for item in uarts:
        print(f"  {item}")
    print("Interface:")
    print_interface_status(DEFAULT_INTERFACE)
    ok, detail = check_interface_ipv4(DEFAULT_INTERFACE, DEFAULT_INTERFACE_IP)
    print(detail)
    return 0


def cmd_reset(args: argparse.Namespace) -> int:
    try:
        import serial  # type: ignore
    except ImportError as exc:
        raise SystemExit("pyserial is required for reset support.") from exc

    if args.monitor_port:
        print("note: --monitor-port is deprecated; reset now auto-selects from wchusbserial and usbmodem candidates")
    strategies = [
        ("rts-pulse", [
            (False, False, 100),
            (False, True, args.pulse_ms),
            (False, False, args.settle_ms),
        ]),
        ("rts-pulse-with-dtr-release", [
            (False, False, 100),
            (False, True, args.pulse_ms),
            (True, False, 120),
            (False, False, args.settle_ms),
        ]),
    ]

    ports = preferred_port_candidates(args.uart_port or args.monitor_port, prefer="uart")
    if not ports:
        raise RuntimeError("No reset-capable serial ports found.")

    for attempt in range(1, max(args.attempts, 1) + 1):
        for port in ports:
            if not port_exists(port):
                continue
            for name, steps in strategies:
                try:
                    apply_reset_sequence(serial, port, steps)
                    rebooted, detail = verify_reset(serial, port, args.wait_seconds)
                except serial.SerialException as exc:
                    print(
                        f"Reset attempt {attempt}/{args.attempts} ({name}) could not use {port}: {exc}",
                        file=sys.stderr,
                    )
                    break
                if not rebooted:
                    print(
                        f"Reset attempt {attempt}/{args.attempts} ({name}) on {port} did not verify: {detail}",
                        file=sys.stderr,
                    )
                    continue

                interface_ok, interface_detail = wait_for_interface_transition(
                    args.interface,
                    args.interface_ip,
                    args.interface_wait_seconds,
                )
                if interface_ok:
                    print(
                        f"Reset verified on {port} using {name} "
                        f"(attempt {attempt}/{args.attempts}); {detail}; "
                        f"{args.interface} {interface_detail}"
                    )
                    return 0
                print(
                    f"Reset attempt {attempt}/{args.attempts} ({name}) on {port} rebooted the ESP but "
                    f"{args.interface} {interface_detail}",
                    file=sys.stderr,
                )

    raise RuntimeError(f"reset did not restore {args.interface} to active state")


def apply_reset_sequence(serial_module, port: str, steps: list[tuple[bool, bool, int]]) -> None:
    with serial_module.Serial(port, DEFAULT_BAUD) as ser:
        ser.reset_input_buffer()
        ser.reset_output_buffer()
        for dtr, rts, delay_ms in steps:
            ser.dtr = dtr
            ser.rts = rts
            time.sleep(delay_ms / 1000.0)
    return 0


def verify_reset(serial_module, monitor_port: str, wait_seconds: float) -> tuple[bool, str]:
    deadline = time.monotonic() + wait_seconds
    monitor_existed_before = port_exists(monitor_port)
    saw_disconnect = not monitor_existed_before
    saw_reconnect = False
    captured = ""

    while time.monotonic() < deadline:
        exists = port_exists(monitor_port)
        if monitor_existed_before and not exists:
            saw_disconnect = True
        if exists and saw_disconnect:
            saw_reconnect = True
        if exists:
            try:
                with serial_module.Serial(monitor_port, DEFAULT_BAUD, timeout=0.25, write_timeout=0.25) as ser:
                    chunk = ser.read(512)
                    if chunk:
                        captured += chunk.decode(errors="ignore")
                        if any(marker in captured for marker in BOOT_MARKERS):
                            if saw_reconnect:
                                return True, f"{monitor_port} bounced and emitted boot markers"
                            return True, f"{monitor_port} emitted boot markers"
            except serial_module.SerialException:
                pass
        time.sleep(0.1)

    if saw_reconnect:
        return True, f"{monitor_port} disappeared and reappeared"
    if captured:
        snippet = captured.strip().replace("\r", " ").replace("\n", " ")
        return False, f"captured serial output without boot markers: {snippet[:120]}"
    return False, f"no reconnect and no boot markers on {monitor_port}"


def extract_ipv4(output: str) -> str | None:
    match = re.search(r"^\s*inet\s+(\d+\.\d+\.\d+\.\d+)\b", output, re.MULTILINE)
    if not match:
        return None
    return match.group(1)


def interface_state(interface: str) -> tuple[str, str]:
    result = subprocess.run(["ifconfig", interface], capture_output=True, text=True)
    if result.returncode != 0:
        return "unavailable", ""
    output = result.stdout
    if "status: active" in output:
        return "active", output
    if "status: inactive" in output:
        return "inactive", output
    return "present", output


def check_interface_ipv4(interface: str, expected_ip: str) -> tuple[bool, str]:
    state, output = interface_state(interface)
    if state == "unavailable":
        return False, f"{interface}: unavailable"
    if state != "active":
        return False, f"{interface}: present but not active"

    actual_ip = extract_ipv4(output)
    if not actual_ip:
        return False, f"{interface}: active but missing IPv4; expected {expected_ip}"
    if actual_ip != expected_ip:
        return False, f"{interface}: active with IPv4 {actual_ip}; expected {expected_ip}"
    return True, f"{interface}: active with IPv4 {actual_ip}"


def wait_for_interface_transition(interface: str, expected_ip: str, timeout_seconds: float) -> tuple[bool, str]:
    deadline = time.monotonic() + timeout_seconds
    saw_absent = False
    saw_inactive = False
    saw_missing_ipv4 = False
    saw_wrong_ipv4 = False

    while time.monotonic() < deadline:
        state, output = interface_state(interface)
        actual_ip = extract_ipv4(output) if output else None
        if state == "active" and actual_ip == expected_ip:
            if saw_absent or saw_inactive or saw_missing_ipv4 or saw_wrong_ipv4:
                return True, f"disappeared/recovered and is active with IPv4 {actual_ip}"
            return True, f"is active with IPv4 {actual_ip}"
        if state == "unavailable":
            saw_absent = True
        elif state == "inactive":
            saw_inactive = True
        elif state == "active" and actual_ip is None:
            saw_missing_ipv4 = True
        elif state == "active" and actual_ip != expected_ip:
            saw_wrong_ipv4 = True
        if output:
            print(output.strip())
        else:
            print(f"{interface}: unavailable")
        time.sleep(0.5)

    ok, detail = check_interface_ipv4(interface, expected_ip)
    if ok:
        return True, detail
    if saw_missing_ipv4 or saw_wrong_ipv4:
        return False, detail
    if saw_absent or saw_inactive:
        return False, f"did not recover to active state with IPv4 {expected_ip} before timeout"
    return False, f"never became active with IPv4 {expected_ip}"


def cmd_monitor(args: argparse.Namespace) -> int:
    last_error: str | None = None
    for port in preferred_port_candidates(args.monitor_port, prefer="modem"):
        cmd = [args.pio, "device", "monitor", "-d", str(ROOT), "-p", port, "-b", str(args.baud)]
        result = subprocess.run(cmd, cwd=ROOT)
        if result.returncode == 0:
            return 0
        last_error = f"monitor failed on {port} with exit code {result.returncode}"
    raise RuntimeError(last_error or "No monitor-capable serial ports found.")


def wait_for_interface(interface: str, timeout_seconds: float) -> bool:
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        if interface_state(interface)[0] == "active":
            print_interface_status(interface)
            return True
        print_interface_status(interface)
        time.sleep(0.5)
    return False


def run_ping(host: str, count: int) -> None:
    print(f"$ ping -c {count} {host}")
    result = subprocess.run(["ping", "-c", str(count), host], text=True)
    if result.returncode != 0:
        raise RuntimeError(f"ping failed for {host}")


def http_get(host: str, path: str, timeout: float) -> tuple[int, int]:
    conn = http.client.HTTPConnection(host, 80, timeout=timeout)
    try:
        conn.request("GET", path, headers={"Connection": "close"})
        response = conn.getresponse()
        body = response.read()
        return response.status, len(body)
    finally:
        conn.close()


def smoke_paths(args: argparse.Namespace) -> list[str]:
    return args.paths or ["/", "/api/v1/status"]


def run_smoke(
    host: str,
    interface: str,
    interface_ip: str,
    count: int,
    timeout: float,
    ping_count: int,
    paths: list[str],
) -> None:
    print_interface_status(interface)
    ok, detail = check_interface_ipv4(interface, interface_ip)
    print(detail)
    if not ok:
        raise RuntimeError(detail)
    print_arp_state(host)
    run_ping(host, ping_count)

    total = 0
    for round_index in range(1, count + 1):
        for path in paths:
            total += 1
            started = time.monotonic()
            status, size = http_get(host, path, timeout)
            elapsed = time.monotonic() - started
            print(f"{total:03d} round={round_index:02d} path={path} status={status} bytes={size} dt={elapsed:.3f}s")


def cmd_smoke(args: argparse.Namespace) -> int:
    run_smoke(args.host, args.interface, args.interface_ip, args.count, args.timeout, args.ping_count, smoke_paths(args))
    return 0


def cmd_retest(args: argparse.Namespace) -> int:
    reset_args = argparse.Namespace(
        uart_port=args.uart_port,
        monitor_port=None,
        interface=args.interface,
        interface_ip=args.interface_ip,
        pulse_ms=100,
        settle_ms=400,
        wait_seconds=args.wait_seconds,
        interface_wait_seconds=args.interface_wait_seconds,
        attempts=args.attempts,
    )
    cmd_reset(reset_args)
    run_smoke(args.host, args.interface, args.interface_ip, args.count, args.timeout, args.ping_count, smoke_paths(args))
    return 0


def main() -> int:
    args = parse_args()
    ensure_pyserial_runtime(args.command)
    commands = {
        "ports": cmd_ports,
        "reset": cmd_reset,
        "monitor": cmd_monitor,
        "smoke": cmd_smoke,
        "retest": cmd_retest,
    }
    try:
        return commands[args.command](args)
    except KeyboardInterrupt:
        return 130
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
