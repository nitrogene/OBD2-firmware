---
name: vbench-scenario
description: >-
  Play timed ECU simulator scenarios (cold start, O2 sensor drop, DTC injection,
  post-fault recovery) against a LilyGO T-Display S3 VIRTUAL_BENCH over USB-C CDC
  without interfering with the Android app's BLE or Wi-Fi connection.
compatibility: Python 3.11+, uv, pyserial, LilyGO T-Display S3 (VIRTUAL_BENCH)
---

# Skill: Virtual Bench Scenario Player (`vbench-scenario`)

Drives `ecu_simulator` in real time via USB-C CDC (`cmd_sim` in `main/cli_commands.c`), bypassing `transport_stream` so the companion Android app (`OBD2-android`) or Car Scanner can stay connected over BLE or Wi-Fi TCP simultaneously.

## 1. Usage

Always execute via `uv run`:

```powershell
# Auto-detect ESP32-S3 USB-C CDC port (VID=303A, PID=1001)
uv run --with pyserial python scripts/run_vbench_scenario.py --scenario scripts/scenarios/p0131_60s_fault_60s.json

# Explicit serial port
uv run --with pyserial python scripts/run_vbench_scenario.py --port COM8 --scenario scripts/scenarios/p0131_60s_fault_60s.json

# Dry-run (inspect expanded timeline without hardware)
uv run --with pyserial python scripts/run_vbench_scenario.py --scenario scripts/scenarios/p0131_60s_fault_60s.json --dry-run
```

> [!NOTE]
> The script opens the serial port with `DTR=False, RTS=False` so it does **not** reset the ESP32-S3 or drop an active BLE/Wi-Fi session.

## 2. Scenario JSON Format

```json
{
  "name": "Scenario name",
  "tick_ms": 500,
  "steps": [
    {
      "t_s": 0,
      "clear_dtc": true,
      "rpm": 850,
      "speed": 0,
      "coolant": 45,
      "vbat_mv": 14100,
      "o2_mv": 450,
      "ramp": true,
      "o2_osc_min_mv": 150,
      "o2_osc_max_mv": 750
    },
    {
      "t_s": 64,
      "dtc": "P0131"
    }
  ]
}
```

Supported fields per step:
- `t_s` (required): timestamp in seconds from scenario start.
- `rpm` (`0..16383`), `speed` (`0..255`), `coolant` (`-40..215`), `vbat_mv` (`0..24000`), `o2_mv` (`0..1275`).
- `ramp` (`true`/`false`): linearly interpolates `rpm`, `speed`, `coolant`, and `o2_mv` toward the next step every `tick_ms`.
- `o2_osc_min_mv` / `o2_osc_max_mv`: oscillates `o2_mv` sinusoidally between min and max during the step interval.
- `clear_dtc` (`true`) / `dtc` (`"P0131"`): clears or injects a DTC at `t_s`.

## 3. Reference Scenario (`p0131_60s_fault_60s.json`)

- **`t = 0..60 s`**: Cold start (`coolant` `45 -> 85 °C`, crossing the `60 °C` Open-Loop -> Closed-Loop threshold on PID `0x03`), `rpm` `850 -> 2600`, `speed` `0 -> 80 km/h`, `vbat_mv = 14100`, `o2_mv` oscillating `150..750 mV`.
- **`t = 60..64 s`**: `o2_mv` drops to `40 mV` (`< 80 mV` for `> 3 s` in Closed Loop), then `P0131` is injected at `t = 64 s`.
- **`t = 65..125 s`**: Post-fault recovery and deceleration (`o2_mv = 450 mV`, `rpm` `2400 -> 900`, `speed` `80 -> 0 km/h`) covering a 60 s post-fault capture window.
