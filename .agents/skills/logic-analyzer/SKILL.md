---
name: logic-analyzer
description: >-
  Capture and decode 3.3V logic signals on headers H1 (K-Line) and H2 (CAN/TWAI)
  using a 24 MHz 8-channel FX2LP USB logic analyzer (sigrok-cli / fx2lafw),
  plus offline synthetic self-tests without hardware.
compatibility: sigrok-cli (fx2lafw), uv (Python >= 3.11), Windows / Linux / macOS
metadata:
  author: OBD2-Scanner-Dev
  version: "1.0.0"
---

# Skill: 8-Channel USB Logic Analyzer (`logic-analyzer`)

Workflow for capturing and decoding `3.3V` logic signals from the OBD2-firmware board using a **24 MHz 8-channel FX2LP (`fx2lafw`) USB logic analyzer** and `sigrok-cli`.

> [!CAUTION]
> **3.3V Logic Only:** Connect analyzer probes **only** to `3.3V` debug headers `H1` (`KLINE_RX`, `KLINE_TX`, `GND`), `H2` (`TWAI_RX`, `TWAI_TX`, `GND`), or `IO2` (`LED_STATUS`). Never connect probes directly to `K_LINE` (`12V`) or `CANH`/`CANL`.

---

## 1. Recommended 8-Channel Probe Mapping

| Analyzer Channel | Board Signal | Header / Pin | Protocol / Role |
| :--- | :--- | :--- | :--- |
| `D0` | `KLINE_TX` (`IO5`) | `H1` | K-Line UART `10,400 baud` & `25 ms` Fast-Init |
| `D1` | `KLINE_RX` (`IO4`) | `H1` | K-Line local echo / ECU response |
| `D2` | `TWAI_TX` (`IO15`) | `H2` | CAN/TWAI `500 kbps` / `250 kbps` TX |
| `D3` | `TWAI_RX` (`IO16`) | `H2` | CAN/TWAI RX |
| `D4` | `LED_STATUS` (`IO2`) | `LED1` | Optional PWM / state verification |
| `GND` | `GND` | `H1` / `H2` | Mandatory common ground reference |

---

## 2. Tool Discovery & Windows Driver Setup

1. Check if `sigrok-cli` is installed (`Get-Command sigrok-cli` or `C:\Program Files\sigrok\sigrok-cli\sigrok-cli.exe`).
2. If not installed on Windows, install `sigrok-cli` / PulseView (`sigrok.org`), and bind the FX2LP analyzer (`VID:PID 0925:3881` or `1d50:608c`) to the **WinUSB** driver using **Zadig** (required once on Windows for `fx2lafw`).
3. Verify device detection:
   ```powershell
   sigrok-cli --scan
   ```

---

## 3. Capture & Decode Recipes (`fx2lafw`)

* **K-Line `10,400 baud` TX + Echo (`H1`):**
  ```powershell
  sigrok-cli -d fx2lafw --config samplerate=1M --time 1s -c D0=KLINE_TX,D1=KLINE_RX -o kline_echo.sr
  sigrok-cli -i kline_echo.sr -P uart:rx=KLINE_TX:baudrate=10400 -P uart:rx=KLINE_RX:baudrate=10400
  ```
* **K-Line `25 ms` Fast-Init Pulse (`H1`):**
  ```powershell
  sigrok-cli -d fx2lafw --config samplerate=1M --time 2s -c D0=KLINE_TX,D1=KLINE_RX --triggers KLINE_TX=f -O csv -o kline_pulse.csv
  uv run .agents/skills/logic-analyzer/scripts/analyze_capture.py --mode kline-pulse --file kline_pulse.csv --rate 1000000
  ```
* **CAN `500 kbps` (`H2`):**
  ```powershell
  sigrok-cli -d fx2lafw --config samplerate=8M --time 2s -c D2=TWAI_TX,D3=TWAI_RX --triggers TWAI_TX=f -o can_500k.sr
  sigrok-cli -i can_500k.sr -P can:rx=TWAI_TX:bitrate=500000 -P can:rx=TWAI_RX:bitrate=500000
  ```

---

## 4. Testing Without Hardware

1. **Offline decoder/timing self-test (no analyzer or board required):**
   ```powershell
   uv run .agents/skills/logic-analyzer/scripts/analyze_capture.py --self-test
   ```
2. **Virtual `sigrok-cli` demo device (when `sigrok-cli` is installed):**
   ```powershell
   sigrok-cli -d demo --samples 1000 -O csv
   ```
