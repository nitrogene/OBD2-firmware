---
name: logic-analyzer
description: >-
  100% Python capture and decode of 3.3V logic signals on headers H1 (K-Line) and
  H2 (CAN/TWAI) using a 24 MHz 8-channel FX2LP USB logic analyzer (pyusb + libusb),
  plus offline synthetic self-tests without hardware.
compatibility: uv (Python >= 3.11), pyusb, libusb, Windows / Linux / macOS
metadata:
  author: OBD2-Scanner-Dev
  version: "2.0.0"
---

# Skill: 100% Python 8-Channel USB Logic Analyzer (`logic-analyzer`)

Pure-Python workflow (`pyusb` + bundled `libusb`) for capturing and decoding `3.3V` logic signals from the OBD2-firmware board using a **24 MHz 8-channel FX2LP USB logic analyzer** — no `sigrok-cli` required.

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

## 2. Windows USB Driver Setup (One-Time)

1. Plug in the 24 MHz 8-channel FX2LP analyzer.
2. On Windows, bind the device (`VID:PID 0925:3881`, `04B4:8613`, or `1D50:608C`) to **WinUSB** using **Zadig**.
3. Verify USB detection in Python:
   ```powershell
   uv run .agents/skills/logic-analyzer/scripts/fx2_capture.py --scan
   ```

---

## 3. 100% Python Capture & Decode Recipes

* **K-Line `10,400 baud` TX + Echo (`H1`):**
  ```powershell
  uv run .agents/skills/logic-analyzer/scripts/fx2_capture.py --rate 1000000 --time 1.0 --channels KLINE_TX,KLINE_RX --out kline_echo.csv
  uv run .agents/skills/logic-analyzer/scripts/analyze_capture.py --mode kline-uart --file kline_echo.csv --rate 1000000
  ```
* **K-Line `25 ms` Fast-Init Pulse (`H1`):**
  ```powershell
  uv run .agents/skills/logic-analyzer/scripts/fx2_capture.py --rate 1000000 --time 1.0 --channels KLINE_TX,KLINE_RX --out kline_pulse.csv
  uv run .agents/skills/logic-analyzer/scripts/analyze_capture.py --mode kline-pulse --file kline_pulse.csv --rate 1000000
  ```
* **CAN `500 kbps` (`H2`):**
  ```powershell
  uv run .agents/skills/logic-analyzer/scripts/fx2_capture.py --rate 8000000 --time 1.0 --channels KLINE_TX,KLINE_RX,TWAI_TX,TWAI_RX --out can_500k.csv
  uv run .agents/skills/logic-analyzer/scripts/analyze_capture.py --mode can --file can_500k.csv --rate 8000000 --bitrate 500000
  ```

---

## 4. Testing Without Hardware

Run both offline self-tests (no analyzer or ESP32-S3 board required):
```powershell
uv run .agents/skills/logic-analyzer/scripts/fx2_capture.py --self-test
uv run .agents/skills/logic-analyzer/scripts/analyze_capture.py --self-test
```
