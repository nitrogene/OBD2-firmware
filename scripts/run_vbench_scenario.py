"""Timed ECU simulator scenario player for LilyGO VIRTUAL_BENCH via USB-C CDC CLI."""

from __future__ import annotations

import argparse
import json
import math
import sys
import time
from pathlib import Path
from typing import Any

import serial
import serial.tools.list_ports

INTERP_FIELDS = ("rpm", "speed", "coolant", "o2_mv")
DIRECT_CMD_MAP = {
    "rpm": "sim rpm {val}",
    "speed": "sim speed {val}",
    "coolant": "sim coolant {val}",
    "vbat_mv": "sim vbat {val}",
    "o2_mv": "sim o2 {val}",
}


def find_esp32s3_port() -> str | None:
    """Return the first Espressif ESP32-S3 USB-Serial/JTAG port if connected."""
    for port in serial.tools.list_ports.comports():
        if port.vid == 0x303A and port.pid == 0x1001:
            return port.device
    return None


def send_cli_cmd(ser: serial.Serial | None, cmd: str, dry_run: bool) -> str:
    """Send a single CLI line to the LilyGO and return its response."""
    if dry_run or ser is None:
        return "(dry-run)"
    ser.reset_input_buffer()
    ser.write((cmd + "\r\n").encode("ascii"))
    time.sleep(0.06)
    raw = ser.read_all().decode("ascii", errors="replace")
    lines = [
        ln.strip()
        for ln in raw.splitlines()
        if ln.strip() and ln.strip() != cmd and not ln.strip().startswith("obd2>")
    ]
    return " | ".join(lines) if lines else "OK"


def build_timeline(scenario: dict[str, Any], default_tick_ms: int) -> list[tuple[float, list[str]]]:
    """Expand scenario steps (with optional linear ramps and O2 oscillation) into timed commands."""
    steps = sorted(scenario.get("steps", []), key=lambda s: float(s["t_s"]))
    if not steps:
        raise ValueError("Scenario contains no 'steps'")

    tick_s = max(0.1, float(scenario.get("tick_ms", default_tick_ms)) / 1000.0)
    timeline: list[tuple[float, list[str]]] = []
    last_vals: dict[str, int] = {}

    def emit_if_changed(t: float, key: str, val: int, cmds: list[str]) -> None:
        if last_vals.get(key) != val:
            last_vals[key] = val
            cmds.append(DIRECT_CMD_MAP[key].format(val=val))

    for idx, step in enumerate(steps):
        t_cur = float(step["t_s"])
        cmds: list[str] = []
        if step.get("clear_dtc"):
            cmds.append("sim clear")
        for key in ("vbat_mv", *INTERP_FIELDS):
            if key in step and step[key] is not None:
                emit_if_changed(t_cur, key, round(float(step[key])), cmds)
        if step.get("dtc"):
            cmds.append(f"sim dtc {step['dtc']}")
        if cmds:
            timeline.append((t_cur, cmds))

        if idx + 1 >= len(steps):
            continue

        nxt = steps[idx + 1]
        t_next = float(nxt["t_s"])
        dt = t_next - t_cur
        has_ramp = bool(step.get("ramp"))
        has_osc = "o2_osc_min_mv" in step and "o2_osc_max_mv" in step
        if dt <= tick_s or (not has_ramp and not has_osc):
            continue

        n_ticks = int(dt / tick_s)
        for k in range(1, n_ticks):
            t_sample = t_cur + k * tick_s
            if t_sample >= t_next - 0.02:
                break
            frac = (t_sample - t_cur) / dt
            tick_cmds: list[str] = []
            if has_ramp:
                for key in INTERP_FIELDS:
                    if key == "o2_mv" and has_osc:
                        continue
                    if key in step and key in nxt and step[key] is not None and nxt[key] is not None:
                        v0 = float(step[key])
                        v1 = float(nxt[key])
                        emit_if_changed(t_sample, key, round(v0 + (v1 - v0) * frac), tick_cmds)
            if has_osc and "o2_mv" not in tick_cmds:
                lo = float(step["o2_osc_min_mv"])
                hi = float(step["o2_osc_max_mv"])
                mid = (lo + hi) / 2.0
                amp = (hi - lo) / 2.0
                osc_val = round(mid + amp * math.sin(2.0 * math.pi * 0.8 * (t_sample - t_cur)))
                emit_if_changed(t_sample, "o2_mv", osc_val, tick_cmds)
            if tick_cmds:
                timeline.append((t_sample, tick_cmds))

    return timeline


def run_scenario(
    port: str | None,
    baud: int,
    scenario_path: Path,
    tick_ms: int,
    dry_run: bool,
) -> int:
    """Execute a JSON scenario against the LilyGO USB-C CDC console."""
    scenario = json.loads(scenario_path.read_text(encoding="utf-8"))
    timeline = build_timeline(scenario, tick_ms)

    ser: serial.Serial | None = None
    if not dry_run:
        target_port = port or find_esp32s3_port()
        if target_port is None:
            print("Error: no ESP32-S3 USB-C CDC port found (pass --port COMx)", file=sys.stderr)
            return 1
        ser = serial.Serial()
        ser.port = target_port
        ser.baudrate = baud
        ser.timeout = 0.2
        ser.dtr = False
        ser.rts = False
        ser.open()
        time.sleep(0.15)
        print(f"Connected to {target_port} @ {baud} baud (no-reset DTR/RTS=False)")

    name = scenario.get("name", scenario_path.name)
    print(f"Starting scenario '{name}' ({len(timeline)} events)...")
    t0 = time.monotonic()

    try:
        for t_target, cmds in timeline:
            if not dry_run:
                delay = (t0 + t_target) - time.monotonic()
                if delay > 0.0:
                    time.sleep(delay)
            for cmd in cmds:
                resp = send_cli_cmd(ser, cmd, dry_run)
                print(f"[t = {t_target:6.1f}s] {cmd:<24} -> {resp}", flush=True)
    finally:
        if ser is not None and ser.is_open:
            ser.close()

    print("Scenario complete.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Play timed ECU simulator scenario on LilyGO VIRTUAL_BENCH")
    parser.add_argument("--port", default=None, help="Serial port (e.g. COM8). Auto-detects ESP32-S3 if omitted.")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate (default: 115200)")
    parser.add_argument("--scenario", type=Path, required=True, help="Path to JSON scenario file")
    parser.add_argument("--tick-ms", type=int, default=500, help="Ramp interpolation step in ms (default: 500)")
    parser.add_argument("--dry-run", action="store_true", help="Print expanded timeline without opening serial port")
    args = parser.parse_args()
    return run_scenario(args.port, args.baud, args.scenario, args.tick_ms, args.dry_run)


if __name__ == "__main__":
    sys.exit(main())
