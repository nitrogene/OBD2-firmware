# (Work In Progress) ESP32-S3 OBD-II Scanner & ECU Simulator Firmware

> [!WARNING]
> **Work In Progress (WIP):** This repository is currently in its initialization phase. Firmware components are under active development based on the finalized hardware specifications.

Embedded firmware repository (ESP-IDF / FreeRTOS) for the universal **ESP32-S3 OBD-II Diagnostic Scanner & Automotive ECU Simulator** hardware platform.

---

## 1. Hardware Repository & Reference Documentation

This repository contains **only the embedded software**. All schematics, 4-layer PCB layout, BOM, enclosures, and electrical specifications live in the dedicated hardware repository:

- **Hardware Repository:** [nitrogene/OBD2](https://github.com/nitrogene/OBD2)
- **Software Handover & Specifications (Contractual Reference):** [`documents/software.md`](https://github.com/nitrogene/OBD2/blob/main/documents/software.md)
- **Hardware Architecture & Electronic Anatomy:** [`documents/hardware.md`](https://github.com/nitrogene/OBD2/blob/main/documents/hardware.md)
- **Machine-Readable Component & Net Semantics:** [`documents/schema/semantics.json`](https://github.com/nitrogene/OBD2/blob/main/documents/schema/semantics.json)
- **Component Datasheets Index:** [`documents/datasheets/datasheets.md`](https://github.com/nitrogene/OBD2/blob/main/documents/datasheets/datasheets.md)

---

## 2. AI Developer Context: Platform & Operating Roles

The physical PCBA is a **single universal hardware design** that assumes one of two roles depending on hardware jumper settings (`JP1`, `JP2`) and the flashed firmware configuration:

| Feature / Setting | Role 1: OBD-II Scanner (Client) | Role 2: ECU Simulator (Server / Testbench) |
| :--- | :--- | :--- |
| **Purpose** | In-vehicle diagnostic dongle (SAE J1962 12V) | Benchtop ECU emulator for dual-board mirror testing |
| **CAN Jumper `JP1` (120 Ω)** | **OPEN** in vehicle (`CLOSED` on dual-board bench) | **CLOSED** (provides 120 Ω bus termination) |
| **K-Line Jumper `JP2` (500 Ω)** | **CLOSED** (provides ISO 9141-2 500 Ω pull-up) | **OPEN** (passive high-impedance ECU, `Rin >= 100 kΩ`) |
| **Primary Interfaces** | BLE 5.0, Wi-Fi SoftAP/WebSocket, USB-C CDC | USB-C CDC / BLE CLI (`set rpm`, `fault inject`, etc.) |

### Target MCU & Electrical Boundaries
- **Module:** `ESP32-S3-WROOM-1-N16R2` (Xtensa Dual-Core 32-bit LX7 up to 240 MHz)
- **Memory:** 16 MB Quad SPI Flash, 2 MB Octal SPI PSRAM
- **Logic Voltage:** `3.3V` (regulated by `U5` LDL1117S33R + `FB1`). **ESP32-S3 GPIOs are NOT 5V tolerant (`Vmax = 3.6 V`).**
- **Vehicle Scope:** Strictly **12V passenger cars** (SAE J1962 Type A). Never connect to 24V commercial vehicles.

---

## 3. Complete GPIO Pinout & Hardware Mapping

| GPIO | Direction | Net Name | Peripheral / Function | Hardware & Electrical Notes |
| :--- | :---: | :--- | :--- | :--- |
| **`IO1`** | Analog In | `VBAT_SENSE` | **ADC1_CH0** (Battery Voltage) | Precision divider `R12` (100 kΩ) / `R13` (12 kΩ), `C10` (100 nF, `fc ≈ 148 Hz`), `D6` (BAV199) clamp. Safe with active Wi-Fi. |
| **`IO2`** | Digital Out | `LED_STATUS` | **Status LED `LED1`** (Red) | Active HIGH (`3.3V`), current limited by `R6` (100 Ω, ~13 mA). Supports PWM via LEDC. |
| **`IO4`** | Digital In | `KLINE_RX` | **UART RX (K-Line)** | From `U3` (L9637D pin 1) via `R1` (10 Ω). Internal 10 kΩ pull-up to `3.3V` in `U3`. |
| **`IO5`** | Digital Out | `KLINE_TX` | **UART TX (K-Line)** | To `U3` (L9637D pin 4) via `R2` (10 Ω). `3.3V` idle = `12V` recessive; `0V` = dominant. |
| **`IO15`** | Digital Out | `TWAI_TX` | **TWAI TX (CAN)** | To `U2` (TJA1051T/3/1J pin 1). `3.3V` logic level. |
| **`IO16`** | Digital In | `TWAI_RX` | **TWAI RX (CAN)** | From `U2` (TJA1051T/3/1J pin 4). `3.3V` logic level via `VIO` pin. |
| **`IO19`** | Diff I/O | `USB_D-` | **Native USB OTG D-** | Series `R22` (22 Ω), ESD TVS `U7`. Used for ROM flashing, JTAG, and CDC serial. |
| **`IO20`** | Diff I/O | `USB_D+` | **Native USB OTG D+** | Series `R23` (22 Ω), ESD TVS `U6`. Used for ROM flashing, JTAG, and CDC serial. |
| **`IO0`** | Strapping In | `IO0` | **ROM Bootloader** | External 10 kΩ pull-up `R21` to `3.3V`. Ground `TP10` at reset for recovery. |
| **`EN`** | Reset In | `ESP_EN` | **Hardware Reset (`CHIP_PU`)** | 10 ms RC (`R15` 10 kΩ + `C12` 1 µF), pushbutton `SW1`, test point `TP11`. |

- **Debug Logic Analyzer Headers:** `H1` (`KLINE_RX`, `KLINE_TX`, `GND`) and `H2` (`TWAI_RX`, `TWAI_TX`, `GND`) expose 3.3V logic signals.
- **Power & Bus Connectors:** `J1` (5-pin JST-XH OBD-II / mirror harness) and `H3` (1x2 lab `+12V` / `GND`).

---

## 4. Power Telemetry & Sleep Management (`IO1` / ADC1_CH0)

### Conversion Formula
Attenuation ratio of `R12` (`100 kΩ`) and `R13` (`12 kΩ`):
- `k = 12 / (100 + 12) = 12 / 112 ≈ 0.1071428`
- `V_BAT = V_ADC / k = V_ADC * 9.3333`

### ADC Implementation Guidelines
- Use ESP-IDF `adc_oneshot` + calibration (`esp_adc_cal` / `adc_cali`).
- Use `ADC_ATTEN_DB_12` (full-scale ~0 mV to 3100 mV).
- Apply a 32- or 64-sample moving average to filter ignition noise.

### Voltage Thresholds & Expected Firmware Behavior
| Condition | `V_BAT` | `V_ADC` | Expected Firmware Behavior |
| :--- | :---: | :---: | :--- |
| **USB Power Only (Bench Debug)** | `< 2.8 V` (~0 V) | `< 0.300 V` | `+12V_PROT` isolated by `Q1`. Suppress low-battery warnings. |
| **Buck UVLO Hardware Cutoff** | `<= 8.01 V` | `<= 0.858 V` | Buck `U4` (TPS54331) shuts down cleanly; NVS Flash protected. |
| **Critically Discharged** | `< 11.8 V` | `< 1.264 V` | Warn user; block heavy operations / flashing. |
| **Nominal Resting Battery** | `12.4 V – 12.8 V` | `1.328 V – 1.371 V` | Engine off; arm 10-minute inactivity auto-sleep timer. |
| **Engine Running (Alternator)** | `13.8 V – 14.6 V` | `1.478 V – 1.564 V` | Ignition ON detected; stay continuously active. |
| **Overvoltage Fault** | `> 15.5 V` | `> 1.660 V` | Trigger overvoltage alert & `LED1` SOS pattern. |
| **TVS `D1` Transient Clamp** | `26.0 V` | `2.785 V` | Within `ADC_ATTEN_DB_12` range (`< 3.1 V`) without saturation. |

### Low-Power Sleep Notes
- Always-on `U2`, `U3`, `U4`, and `U5` draw ~8–15 mA total when 12V is connected and ESP32-S3 is in Deep Sleep (`< 20 µA`).
- Configure wake-up via GPIO edge (`ext0`/`ext1`) on CAN/K-Line activity or ULP comparator on `VBAT_SENSE > 13.2 V`.

---

## 5. Communication Protocol Specifications

### 5.1 CAN Bus (ISO 15765-4 & ESP32-S3 TWAI)
- **Physical Layer:** `U2` (TJA1051T/3/1J), CAN 2.0B compliant.
- **Required Bitrates & ID Formats:**
  - `500 kbps` (11-bit standard & 29-bit extended)
  - `250 kbps` (11-bit standard & 29-bit extended)
- **Standard Arbitration IDs:**
  - 11-bit: Broadcast `0x7DF`, ECM `0x7E0` -> `0x7E8`, TCM `0x7E1` -> `0x7E9`.
  - 29-bit: Broadcast `0x18DB33F1`, ECM `0x18DA10F1` -> `0x18DAF110`.
- **ISO-TP (ISO 15765-2):** Implement Single Frame (`0x00–0x07`), First Frame (`0x10` + 12-bit length), Flow Control (`0x30`, BS, STmin), and Consecutive Frame (`0x20–0x2F`).

### 5.2 K-Line (ISO 9141-2 / ISO 14230 KWP2000)
- **Physical Layer:** `U3` (L9637D013TR), half-duplex single-wire 12V bus at **10,400 baud**.
- **Critical Driver Requirement:** Every byte transmitted on `IO5` (`KLINE_TX`) is **immediately echoed back** on `IO4` (`KLINE_RX`). The driver must read and verify/filter this local echo (useful for collision detection).
- **5-Baud Slow Init (ISO 9141-2):**
  1. Bus idle high (`12V`) >= `2600 ms`.
  2. Bit-bang target address (`0x33` generic OBD-II, or `0x11` GM/Daewoo ECM) at **5 baud** (`200 ms/bit`, `2.0 s` total) on `IO5`.
  3. Switch to `10,400 baud`, receive sync byte `0x55`, then key bytes `KB1`, `KB2`.
  4. Send inverted `~KB2` to lock session.
- **Fast-Init (ISO 14230-2):**
  1. Bus idle high >= `300 ms`.
  2. Drive `IO5` dominant (`0V` on bus) for `25 ± 1 ms` (`T_inil`), then recessive (`12V`) for `25 ± 1 ms` (`T_wup`).
  3. Send `StartCommunicationRequest` at `10,400 baud`: `0x81 0x10 0xF1 0x81 0x03`.
  4. Expect `StartCommunicationPositiveResponse` (`0xC1 0xEA 0x8F ...`).
- **OEM Extension (GM-Daewoo Kalos / Chevrolet Aveo):**
  - Multi-module addressing on Pin 7: ECM `0x10`/`0x11`, TCM `0x28`, ABS `0x58`, Airbag SDM `0x50`/`0x51`.
  - KWP2000 services: `0x21`/`0x22` (ReadDataByLocalIdentifier), `0x30` (InputOutputControlByLocalIdentifier), `0x18`/`0x14` (Read/Clear OEM DTCs).

---

## 6. User Interfaces, Connectivity & ECU Simulator

### 6.1 Status LED `LED1` State Machine (`IO2`)
| Pattern | Timing | System State |
| :--- | :--- | :--- |
| **Off** | 0% | Unpowered or Deep Sleep |
| **Slow Breathing** | 0.5 Hz (PWM ramp) | Ready / idle, awaiting connection |
| **Slow Blink** | 1 Hz (500 ms ON / 500 ms OFF) | Protocol scanning / baud negotiation |
| **Solid On** | 100% | Vehicle bus connected and active |
| **Rapid Bursts** | 10 Hz (50 ms bursts) | Active frame transmission (PID streaming) |
| **SOS Morse** | 3 short, 3 long, 3 short | Critical fault (overvoltage `> 16V` or CAN `Bus-Off`) |

### 6.2 Wireless & USB Interfaces
- **BLE 5.0 GATT:** `CHAR_VEHICLE_DATA` (Notify), `CHAR_COMMAND_IN` (Write Without Response), `CHAR_BATTERY_VOLTAGE` (Read / Notify).
- **Wi-Fi (802.11 b/g/n):** SoftAP (`OBD2-Scanner-XXXX`, `192.168.4.1`) or Station mode + WebSocket server with embedded HTML5 dashboard.
- **USB-C (`J2` / `IO19`, `IO20`):** Native USB CDC serial console and JTAG debug.

### 6.3 ECU Simulator Mode (`MODE_ECU_SIMULATOR`)
When compiled/configured as ECU Simulator:
- Broadcast periodic CAN frames (10 ms / 50 ms) and respond to `0x7DF`/`0x7E0` on `0x7E8` for OBD-II Modes `01`, `02`, `03`, `04`.
- Respond to K-Line 5-baud and Fast-Init wake-up sequences.
- Provide USB-C CDC / BLE CLI commands: `set rpm <val>`, `set speed <val>`, `set coolant <temp>`, `fault inject <dtc>`, `fault clear`.

---

## 7. Hardware Safety Rules (Mandatory for Testing)

> [!CAUTION]
> **1. Strict Bench Wiring Order (Anti-Backfeeding):** Always connect `GND` and `+12V` power (via `H3` or `J1`) **before** connecting `CANH`, `CANL`, or `K_LINE`. Connecting an active 12V `K_LINE` while `GND`/`+12V` are unpowered backfeeds `+12V_PROT` through `R16`/`R18` and `U3` clamp diodes, risking transceiver damage.

> [!CAUTION]
> **2. In-Vehicle USB-C Ground Isolation:** When debugging via USB-C (`J2`) in a live vehicle, the laptop **must run strictly on its internal battery**. Never connect an AC-grounded charger while plugged into the vehicle OBD-II port (ground loop hazard).

---

## 8. Recommended Software Stack & Architecture

- **Framework:** **ESP-IDF v5.2+** (C/C++, FreeRTOS, `idf.py` with CMake / Ninja).
- **Recommended FreeRTOS Tasks:**
  - `twai_rx_tx` (Prio 10): Reactive CAN/TWAI packet handling
  - `kline_engine` (Prio 10): Strict UART & 5-baud timings
  - `comm_host` (Prio 5): BLE GATT / Wi-Fi WebSocket / USB CDC
  - `ecu_sim` (Prio 4): ECU simulator engine (when enabled)
  - `power_telemetry` (Prio 3): `VBAT_SENSE` ADC sampling & sleep governance
  - `ui_led` (Prio 2): `LED1` PWM state machine
- **Recommended Repository Layout:**
  ```text
  OBD2-firmware/
  ├── CMakeLists.txt
  ├── sdkconfig.defaults        # TWAI, USB CDC (IO19/IO20), BLE 5.0, 240 MHz, 16MB Flash, 2MB PSRAM
  ├── bringup.md                # Hardware bring-up & dual-board bench validation guide
  ├── components/
  │   ├── bsp/                  # Board Support Package (GPIO pinout, LED1, ADC1)
  │   ├── twai_driver/          # Low-level TWAI driver & filters
  │   ├── isotp/                # ISO 15765-2 transport layer
  │   ├── kline_driver/         # UART K-Line, echo filtering & 5-baud/Fast-Init
  │   ├── obd2_stack/           # SAE J1979 Modes 01-09
  │   ├── kwp2000/              # GM-Daewoo KWP2000 OEM services
  │   ├── power_mgmt/           # VBAT_SENSE calibration & Deep Sleep
  │   ├── comm_ble/             # BLE 5.0 GATT server
  │   ├── comm_wifi/            # SoftAP / WebSocket dashboard
  │   └── ecu_simulator/        # ECU emulation & CLI
  ├── main/
  │   ├── CMakeLists.txt
  │   ├── app_main.c            # Boot, role selection & task setup
  │   └── cli_commands.c        # USB-C CDC CLI console
  └── review/
      └── guidelines.md         # AI/Human technical review protocol
  ```

### Prerequisites & Local Build Setup
- **ESP-IDF `v5.2+` (recommended `v5.5.6`, aligned with [`.github/workflows/ci.yml`](.github/workflows/ci.yml)):**
  - **Windows:** [ESP-IDF Windows Installer](https://dl.espressif.com/dl/esp-idf/) (includes ESP-IDF, `xtensa-esp-elf` toolchain for `esp32s3`, CMake/Ninja, OpenOCD, and USB JTAG/CDC drivers) or the [VS Code ESP-IDF Extension](https://marketplace.visualstudio.com/items?itemName=espressif.esp-idf-extension).
  - **Linux / macOS:** [ESP-IDF Standard Setup (`v5.5.6` / `esp32s3`)](https://docs.espressif.com/projects/esp-idf/en/v5.5.6/esp32s3/get-started/linux-macos-setup.html).
- **Python Host Tooling (`uv`, Python `>= 3.11`):**
  - Install [`uv`](https://docs.astral.sh/uv/getting-started/installation/) for host scripts and `pytest` (`uv sync`, then `uv run pytest`).
- **Quick Commands (once Phase 1 structure is initialized):**
  - Firmware build (in ESP-IDF PowerShell / sourced environment): `idf.py set-target esp32s3 && idf.py build`
  - Host unit tests: `uv run pytest`
  - Hardware bring-up checklist (multimeter, 8-ch logic analyzer, dual-board bench): see [`bringup.md`](bringup.md)

---

## 9. Technical Reviews

AI agents and human reviewers auditing this repository must follow [`review/guidelines.md`](review/guidelines.md) and submit findings under `review/reviewXXX.md`.
