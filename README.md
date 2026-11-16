# ESP32-S3 OBD-II Scanner & ECU Simulator Firmware

> [!NOTE]
> **Status (`v0.3.0+`):** Core firmware stacks (`bsp`, `power_mgmt`, `twai_driver`, `isotp`, `kline_driver`, `obd2_stack`, `kwp2000`, `transport_stream`, `elm327_emu`, `ecu_simulator`, `comm_ble`, `comm_wifi`, `virtual_bench_ui`) are implemented, compiled on ESP-IDF `v5.5.6`, tested via 24 host `pytest` cases, and validated in wireless black-box mode on a LilyGO TTGO T-Display S3 (`VIRTUAL_BENCH`).

Embedded firmware repository (ESP-IDF `v5.5.6` / FreeRTOS) for the universal **ESP32-S3 OBD-II Diagnostic Scanner & Automotive ECU Simulator** hardware platform, including a **Virtual Bench** profile for black-box BLE/Wi-Fi testing on a LilyGO TTGO T-Display S3.

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

## 6. Implemented Firmware Features, Connectivity & Operating Modes

### 6.1 Firmware Build Profiles
Two build configurations share the same codebase:

| Profile | Target Hardware | Command | Key Behavior |
| :--- | :--- | :--- | :--- |
| **Standard Hardware** | Custom `ESP32-S3-WROOM-1` OBD2 PCB | `idf.py set-target esp32s3 && idf.py build` | Full physical CAN (`IO15`/`IO16`), K-Line (`IO4`/`IO5`), `VBAT_SENSE` (`IO1`), `LED1` (`IO2`), and 10-min Deep Sleep governance. |
| **Virtual Bench (`CONFIG_OBD2_VIRTUAL_BENCH`)** | **LilyGO TTGO T-Display S3** (`170×320` ST7789) | `idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.virtual_bench" -B build_vbench build` | Disables conflicting physical CAN/K-Line/ADC/LED GPIOs & auto-sleep, drives the ST7789 TFT status UI + `GPIO 0`/`GPIO 14` buttons, simulates `V_BAT` (`12.6V` default), and enables dual-OTA (`partitions_ota.csv`, 2 MB `ota_0`/`ota_1`). |

### 6.2 Status LED `LED1` State Machine (`IO2`)
| Pattern | Timing | System State |
| :--- | :--- | :--- |
| **Off** | 0% | Unpowered or Deep Sleep |
| **Slow Breathing** | 0.5 Hz (PWM ramp) | Ready / idle, awaiting connection |
| **Slow Blink** | 1 Hz (500 ms ON / 500 ms OFF) | Protocol scanning / baud negotiation |
| **Solid On** | 100% | Vehicle bus connected and active |
| **Rapid Bursts** | 10 Hz (50 ms bursts) | Active frame transmission (PID streaming) |
| **SOS Morse** | 3 short, 3 long, 3 short | Critical fault (overvoltage `> 15.5 V` or CAN `Bus-Off`) |

### 6.3 Wireless Interfaces & Unified `transport_stream` Arbitration
Incoming byte streams from BLE and Wi-Fi pass through `components/transport_stream/` (static FreeRTOS `esp_ringbuf` buffers, zero dynamic allocation on hot paths, single-active-transport lock with automatic release on disconnect or `5000 ms` inactivity timeout):

- **BLE 5.0 GATT (`components/comm_ble`, NimBLE):**
  - Advertised name: `OBD2-ELM327` (NVS-configurable via Web UI or `comm_ble_set_name`).
  - Primary UART Service `0xFFF0`: `0xFFF1` (Notify TX) + `0xFFF2` (Write / Write-No-Rsp RX).
  - Alias UART Service `0xFFE0`: `0xFFE1` (Notify + Write / Write-No-Rsp) for broad Android/iOS ELM327 app compatibility.
  - Supports secure bonding and ATT MTU negotiation.
  - Measured black-box throughput on T-Display S3: **~8.1 req/s** (`200` PIDs in `24.66 s`).
- **Wi-Fi SoftAP / AP+STA, TCP Server & Web/OTA (`components/comm_wifi`):**
  - Default SoftAP SSID `OBD2-Scanner` (password `12345678`, static IP **`192.168.0.10/24`**), optional `AP+STA` mode persisted in NVS.
  - ELM327 TCP Server on **port `35000`** (`TCP_NODELAY` enabled).
  - Measured black-box throughput on T-Display S3: **~136.9 req/s** (`200` PIDs in `1.46 s`).
  - Embedded HTTP Server on **port `80`** (`http://192.168.0.10/`):
    - `GET /` & `POST /config`: configure SoftAP/STA SSID & password and BLE device name in NVS.
    - `POST /ota`: wireless `.bin` firmware upload (`esp_ota_ops`, verified with `1.18 MB` `VIRTUAL_BENCH` image on `partitions_ota.csv`).
    - `POST /factory-reset`: erases NVS and reboots.

### 6.4 ELM327 v1.5 Emulator & Extended UDS/SLCAN Mode (`components/elm327_emu`)
- **Standard ELM327 v1.5 AT Commands:** `ATZ`, `ATI`, `AT@1`, `ATWS`, `ATD`, `ATPC`, `ATE0`/`1`, `ATL0`/`1`, `ATS0`/`1`, `ATH0`/`1`, `ATRV` (via `power_mgmt`), `ATSP0..9`, `ATDP`, `ATDPN`, `ATAT0..2`, `ATST`, `ATSH`, `ATCRA`, `ATCF`, `ATCM`, `ATCAF0`/`1`, `ATFCSH`, `ATFCSD`, `ATFCSM`, `ATMA`, `ATSI`, `ATFI`, bare `\r` command repeat, and ISO-TP `0:`/`1:` multi-line formatting.
- **Extended Custom Mode (`AT+EXT=1` / `AT+EXT=0`):**
  - Unlocks UDS services in `ecu_simulator` / `obd2_stack`: `0x10` (DiagnosticSessionControl), `0x22` (ReadDataByIdentifier: `0xF190` VIN, `0xF187` Part Number, `0x010C` RPM, `0x010D` Speed), `0x2E` (WriteDataByIdentifier `0xF190` VIN), `0x19` (ReadDTCInformation `0x01`/`0x02`), and `0x14` (ClearDiagnosticInformation).
  - Extended AT commands: `AT+SNIFF=1/0`, `AT+STATS` (`RX`/`TX`/`ERR` counters), `AT+FACTORY` (NVS erase & reboot), plus `elm327_format_slcan_frame()` (`t<III><L><DATA>\r` / `T<IIIIIIII><L><DATA>\r`).

### 6.5 ECU Simulator & USB-C CLI (`components/ecu_simulator`, `main/cli_commands.c`)
- **In-Memory OBD/UDS Engine (`ecu_sim_core.c`):** Responds to Modes `01` (PIDs `00`, `04`, `05`, `0C`, `0D`, `0F`, `11`, `42`), `02`, `03`, `04`, `09` (`02` VIN), and UDS `0x10`/`0x22`/`0x2E`/`0x19`/`0x14` with DTC injection/clearing.
- **Physical Bus Responder (`MODE_ECU_SIMULATOR`):** `ecu_simulator_start_bus(can, kline)` broadcasts cyclic CAN frames (`0x280`, `0x316`) + `0x7E8` ISO-TP responses on TWAI, and answers KWP2000/ISO-9141 frames on K-Line (disabled in `CONFIG_OBD2_VIRTUAL_BENCH`).
- **USB-C CDC Console (`obd2>` prompt, 115200):**
  - Hardware/Diagnostics: `info`, `led <off|breath|blink|solid|burst|sos|duty <0-100>>`, `vbat`, `power [sleep|selftest]`, `can <ping|pong|rx>`, `kline <echo|pulse>`, `proto` (`ATSP0` auto-detect).
  - Simulator Control: `sim status`, `sim rpm <0-16383>`, `sim speed <0-255>`, `sim coolant <-40..215>`, `sim dtc <P0100>`, `sim clear`, `sim vbat <0-24000>`, `sim start [can|kline|both]`, `sim stop`.

### 6.6 LilyGO TTGO T-Display S3 Virtual Bench UI (`components/virtual_bench_ui`)
Active only when `CONFIG_OBD2_VIRTUAL_BENCH=y`:
- Drives the `170×320` ST7789 8-bit i8080 TFT (`GPIO 38/15/9/6/7/8/5`, `D0..D7` on `GPIO 39..42, 45..48`) in `320×170` landscape, showing live transport (`BLE`/`WIFI`/`USB`/`IDLE`), `RPM`, `SPD`, `CLT`, `VBAT`, `DTC` count, and last `AT`/OBD command.
- **Physical Buttons:**
  - **`GPIO 0`**: cycles engine scenarios (`IDLE` `850 RPM / 12.6V` → `CRUISE` `2400 RPM / 90 km/h / 14.1V` → `ACCEL` `4200 RPM / 130 km/h / 14.2V` → `OVERHEAT` `112 °C`).
  - **`GPIO 14`**: cycles fault/voltage states (`CLEAN` → `DTC P0301` → `LOW 11.2V`).

---

## 7. Hardware Safety Rules (Mandatory for Testing)

> [!CAUTION]
> **1. Strict Bench Wiring Order (Anti-Backfeeding):** Always connect `GND` and `+12V` power (via `H3` or `J1`) **before** connecting `CANH`, `CANL`, or `K_LINE`. Connecting an active 12V `K_LINE` while `GND`/`+12V` are unpowered backfeeds `+12V_PROT` through `R16`/`R18` and `U3` clamp diodes, risking transceiver damage.

> [!CAUTION]
> **2. In-Vehicle USB-C Ground Isolation:** When debugging via USB-C (`J2`) in a live vehicle, the laptop **must run strictly on its internal battery**. Never connect an AC-grounded charger while plugged into the vehicle OBD-II port (ground loop hazard).

---

## 8. Software Stack, Repository Layout, Builds & Testing

- **Framework:** **ESP-IDF `v5.5.6`** (C/C++, FreeRTOS, `idf.py` with CMake / Ninja).
- **Repository Layout:**
  ```text
  OBD2-firmware/
  ├── CMakeLists.txt
  ├── sdkconfig.defaults        # ESP32-S3, 240 MHz, 16 MB Flash, 2 MB PSRAM, USB CDC, NimBLE
  ├── sdkconfig.virtual_bench   # LilyGO TTGO T-Display S3 profile (VIRTUAL_BENCH + dual-OTA)
  ├── partitions_ota.csv        # Dual-OTA partition table (ota_0 + ota_1 = 2 MB each)
  ├── CHANGELOG.md              # Release history (v0.1.0 .. v0.3.0+)
  ├── bringup.md                # Hardware bring-up & dual-board bench validation guide
  ├── components/
  │   ├── bsp/                  # Board Support Package (GPIO pinout, LED1 PWM, VBAT ADC1)
  │   ├── twai_driver/          # Low-level TWAI driver, 500k/250k probe & Bus-Off recovery
  │   ├── isotp/                # Pure-C ISO 15765-2 transport layer (11-bit & 29-bit)
  │   ├── kline_driver/         # UART1 K-Line, echo verification, 5-baud & Fast-Init
  │   ├── obd2_stack/           # ATSP0 auto-detect, OBD-II PID/DTC codecs, security gating
  │   ├── kwp2000/              # GM-Daewoo KWP2000 OEM services
  │   ├── power_mgmt/           # VBAT moving avg, power state machine, TWDT & Deep Sleep
  │   ├── transport_stream/     # Static ring-buffered RX/TX stream & single-transport lock
  │   ├── elm327_emu/           # Pure-C ELM327 v1.5 parser, multi-line ISO-TP & SLCAN
  │   ├── ecu_simulator/        # Pure-C OBD/UDS responder + physical CAN/K-Line bus tasks
  │   ├── comm_ble/             # NimBLE GATT server (FFF0/FFF1/FFF2 + FFE0/FFE1)
  │   ├── comm_wifi/            # SoftAP/STA, ELM327 TCP :35000, HTTP Config & OTA :80
  │   └── virtual_bench_ui/     # ST7789 TFT status UI & GPIO0/GPIO14 buttons (VIRTUAL_BENCH)
  ├── main/
  │   ├── Kconfig.projbuild     # CONFIG_OBD2_VIRTUAL_BENCH option
  │   ├── app_main.c            # NVS, BSP, power_mgmt, transports, ELM327 host task
  │   └── cli_commands.c        # USB-C CDC CLI console (`obd2>`)
  ├── tests/                    # Host C+Python unit tests (24 pytest cases via gcc/ziglang)
  │   ├── test_isotp.py
  │   ├── test_phase4_protocols.py
  │   └── test_phase5_elm327_ecusim.py
  ├── scripts/                  # Black-box LilyGO bench & logic analyzer scripts
  │   ├── vbench_blackbox_test.py    # Wi-Fi TCP 192.168.0.10:35000 suite + 200-PID benchmark
  │   ├── vbench_ble_test.py         # BLE GATT FFF0/FFE0 suite + 200-PID benchmark (bleak)
  │   ├── vbench_arbitration_test.py # Simultaneous BLE + Wi-Fi transport lock test
  │   └── vbench_http_test.py        # Web Config GET / + wireless POST /ota uploader
  └── .github/workflows/
      ├── ci.yml                # ESP-IDF esp32s3 build, pytest (24 tests), ruff & clang-format
      └── release.yml           # Automated binary release on v* tags
  ```

### Prerequisites & Build Commands
- **ESP-IDF `v5.2+` (recommended `v5.5.6`):**
  - **Windows:** [ESP-IDF Windows Installer](https://dl.espressif.com/dl/esp-idf/) or the [VS Code ESP-IDF Extension](https://marketplace.visualstudio.com/items?itemName=espressif.esp-idf-extension).
  - **Linux / macOS:** [ESP-IDF Standard Setup (`v5.5.6` / `esp32s3`)](https://docs.espressif.com/projects/esp-idf/en/v5.5.6/esp32s3/get-started/linux-macos-setup.html).
- **Python Host Tooling (`uv`, Python `>= 3.11`):**
  - Install [`uv`](https://docs.astral.sh/uv/getting-started/installation/), then run `uv sync`.
- **Build & Flash Commands:**
  - Standard hardware build: `idf.py set-target esp32s3 && idf.py build`
  - LilyGO `VIRTUAL_BENCH` build & flash:
    ```powershell
    idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.virtual_bench" -B build_vbench -p COM8 flash
    ```

### Test Suites & Validation Scripts
- **Host Unit Tests (pure-C compiled via `gcc`/`ziglang` + `ctypes`, 24 tests):**
  ```powershell
  uv run pytest -v
  uvx ruff check tests/ scripts/
  ```
- **LilyGO Black-Box Wireless Validation (`VIRTUAL_BENCH`):**
  - BLE (`OBD2-ELM327`, no Wi-Fi switch needed): `uvx --with bleak python scripts/vbench_ble_test.py`
  - Wi-Fi (`OBD2-Scanner` / `192.168.0.10:35000`): `uv run python scripts/vbench_blackbox_test.py`
  - Multi-transport lock arbitration: `uvx --with bleak python scripts/vbench_arbitration_test.py`
  - Web Config & wireless OTA: `uv run python scripts/vbench_http_test.py --ota build_vbench/obd2_firmware.bin`
- **Hardware Bring-Up Checklist:** see [`bringup.md`](bringup.md).

---

## 9. Technical Reviews

AI agents and human reviewers auditing this repository must follow [`review/guidelines.md`](review/guidelines.md) and submit findings under `review/reviewXXX.md`.
