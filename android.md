# Companion Android Application Specification (`android.md`)

> [!NOTE]
> This document defines the **firmware-to-Android interface contract** and the **functional requirements** for the companion Android application repository. Keep it synchronized with [`README.md`](README.md) whenever wireless transports, `AT` commands, or protocol behaviors change in `OBD2-firmware`.

---

## 1. Purpose & Target Use Case

The companion Android app connects to the ESP32-S3 OBD-II dongle over **BLE 5.0** or **Wi-Fi TCP** to:
1. Provide a reliable ELM327 v1.5 + KWP2000 diagnostic client (including older `10,400 baud` K-Line vehicles such as the **2003 Daewoo Kalos**).
2. **Capture configurable pre-fault and post-fault telemetry context** around intermittent check-engine light (MIL) / DTC events, exporting a structured report ready for analysis by an AI assistant or a mechanic.
3. Expose extended dongle diagnostics (`AT+EXT`, `AT+STATS`, `AT+SNIFF`) and `VIRTUAL_BENCH` simulator controls (`AT+SIM:...`) for bench testing.

---

## 2. Wireless Transport & Connection Contract

### 2.1 Single-Active-Transport Arbitration (`transport_stream`)
- The firmware enforces a **single active transport lock** (`BLE`, `Wi-Fi`, or `USB`). Whichever transport sends data first acquires the lock.
- The lock is released **on disconnect** or after **`5000 ms` of inactivity**.
- **Android App Requirement:** Always close the active BLE GATT connection or Wi-Fi TCP socket cleanly before switching transports, and handle temporary `Timeout` / no-response cleanly if another client holds the lock.

### 2.2 BLE 5.0 GATT (`OBD2-ELM327`)
- **Advertised Device Name:** `OBD2-ELM327` (configurable via Web UI or NVS).
- **Advertised 16-bit Service UUIDs:** `0xFFF0` and `0xFFE0` (present in the BLE advertising packet for fast Android scan filtering).
- **Primary UART Service (`0000fff0-0000-1000-8000-00805f9b34fb`):**
  - `0xFFF1` (`0000fff1-0000-1000-8000-00805f9b34fb`): **Notify** (Dongle -> App). Enable CCCD notifications before sending commands.
  - `0xFFF2` (`0000fff2-0000-1000-8000-00805f9b34fb`): **Write / Write Without Response** (App -> Dongle).
- **Alias UART Service (`0000ffe0-0000-1000-8000-00805f9b34fb`):**
  - `0xFFE1` (`0000ffe1-0000-1000-8000-00805f9b34fb`): **Notify + Write / Write Without Response** (fallback if `0xFFF0` is not selected).
- **MTU & Chunking:** Request ATT MTU `>= 247` bytes (`requestMtu(256)`) immediately after service discovery. Measured throughput on `VIRTUAL_BENCH`: **~8.1 req/s**.
- **Android Permissions:**
  - Android 12+ (API 31+): `BLUETOOTH_SCAN` (with `neverForLocation` if applicable) and `BLUETOOTH_CONNECT`.
  - Android <= 11 (API <= 30): `ACCESS_FINE_LOCATION` + Location services enabled for BLE scanning.

### 2.3 Wi-Fi SoftAP & ELM327 TCP (`OBD2-Scanner`)
- **Default SoftAP SSID:** `OBD2-Scanner` (WPA2-PSK, default password `12345678`; optional `AP+STA` mode configurable via Web UI).
- **Static IP & Port:** **`192.168.42.1`**, TCP port **`35000`** (`TCP_NODELAY` enabled).
- **Throughput:** Measured on `VIRTUAL_BENCH`: **~136.9 req/s** (recommended for high-rate live dashboards and fast log transfers).
- **Android Network Binding:** Because `OBD2-Scanner` has no internet access, bind the app's `Socket` to the Wi-Fi `Network` via `ConnectivityManager.bindProcessToNetwork()` / `network.socketFactory` so Android does not route `192.168.42.1` over cellular data.
- **Embedded Web Config & OTA:** `http://192.168.42.1/` (HTTP port `80`). `POST /config`, `POST /ota`, and `POST /factory-reset` require the current AP password (`admin_pass` form field or `X-OTA-Auth` header); `POST /ota` rejects flashing if `V_BAT < 11.8 V` or `> 15.5 V`.

---

## 3. ELM327 v1.5 & Diagnostic Protocol Contract

### 3.1 Framing & Initialization Sequence
- Commands end with `\r` (`0x0D`). Responses end with `\r` (or `\r\n` if `ATL1`) and terminate with the ELM327 prompt character **`>`**. Accumulate incoming BLE notifications / TCP bytes until `>` is received.
- **Recommended Init Sequence:**
  1. `ATZ` -> `ELM327 v1.5`
  2. `ATE0` (disable echo), `ATL0` (CR only), `ATS0` (compact hex, or `ATS1` for spaces), `ATH1` (or `ATH0`), `ATAL`, `ATD0`, `ATAT1`, `ATST64`
  3. `ATSP0` (auto-detect) or explicit protocol:
     - `ATSP3`: ISO 9141-2 (`10,400 baud`)
     - `ATSP4`: KWP2000 5-baud init (`10,400 baud`)
     - `ATSP5`: KWP2000 Fast-Init (`10,400 baud`)
     - `ATSP6`–`ATSP9`: ISO 15765-4 CAN (11-bit/29-bit, `500 kbps` / `250 kbps`)
  4. `0100` (probe ECU & supported PIDs `01..20`), then `0120` / `0140` if bit 0 (`0x01`) is set.
  5. `ATRV` (battery voltage from `power_mgmt`, e.g. `12.6V`).
- **CAN Header Framing (`ATH1`):**
  - When `ATH1` is enabled on CAN (`ATSP0` / `ATSP6..9`), single-frame responses include the CAN ID and ISO-TP PCI length byte (e.g., `7E8064100181A8001` with `ATS0`, or `7E8 06 41 00 18 1A 80 01` with `ATS1`).
  - Multi-frame ISO-TP responses use `014\r0:490201...\r1:...\r2:...` formatting.

### 3.2 Extended Mode (`AT+EXT`) & Security Gating
- By default (`extended_unlocked == false`), state-modifying and actuator/memory SIDs (`0x11`, `0x27`, `0x28`, `0x2E`, `0x2F`, `0x30`, `0x31`, `0x34`–`0x37`, `0x3B`, `0x3D`, `0x85`) are blocked (`NRC 0x33`).
- Unlock with `AT+EXT=1` (or `AT+EXT=1,<passkey>` when a passkey is configured) and relock with `AT+EXT=0`.
- Additional extended commands:
  - `AT+STATS`: returns `RX` / `TX` / `ERR` frame counters.
  - `AT+SNIFF=1` / `AT+SNIFF=0`: raw CAN SLCAN monitor stream (`t<III><L><DATA>\r` / `T<IIIIIIII><L><DATA>\r`).
  - `AT+SIM:<KEY>=<VAL>`: remote control of `ecu_simulator` (`RPM=`, `SPD=`, `CLT=`, `VBAT=`, `DTC=P0301|CLEAR`, `START`, `STOP`).

### 3.3 K-Line Bandwidth Considerations (e.g., 2003 Daewoo Kalos)
- K-Line (`10,400 baud`) supports roughly **5 to 10 PID requests/sec** total.
- Avoid polling 20+ PIDs simultaneously during fault capture; keep active capture profiles focused (**4–8 PIDs**) so each sensor is sampled at `>= 1 Hz`.

---

## 4. Core Feature: Configurable Pre/Post-Fault Context Capture Mode

This mode is the primary differentiator over generic OBD-II apps: it continuously buffers targeted telemetry and freezes **before + after** context whenever a fault occurs.

### 4.1 Configurable DTC-to-PID Mapping (Rule Engine)
- Maintain an editable JSON/local rule table mapping **DTC codes or prefixes** to:
  - **Target PIDs** (Mode `01` PIDs, `ATRV`, and optional KWP2000 OEM local identifiers).
  - **Sampling priority / polling cadence**.
  - **Optional software trigger thresholds** (to catch intermittent glitches even before the ECU sets a confirmed MIL).
- **Default Reference Profile (Example — `P0131` O2 Sensor Circuit Low Voltage, Bank 1 Sensor 1):**
  - `0x03` (Fuel System Status: Open Loop vs Closed Loop)
  - `0x14` (O2 B1S1 Voltage & STFT)
  - `0x15` (O2 B1S2 Voltage, if supported — isolates sensor/wiring vs true lean condition)
  - `0x06` / `0x07` (STFT / LTFT Bank 1)
  - `0x0B` (Intake MAP)
  - `0x0C` (Engine RPM)
  - `0x0D` (Vehicle Speed)
  - `0x05` (Coolant Temp)
  - `ATRV` (Battery / Alternator Voltage)
  - *Optional glitch trigger:* `B1S1 < 0.08 V` for `> 3 s` while `Closed Loop` and `Coolant > 60 °C`.
- Users must be able to **create, edit, and select** PID profiles and adjust:
  - **Pre-fault buffer duration (`T_pre`):** configurable (e.g., `15 s` – `300 s`, default `60 s`).
  - **Post-fault capture duration (`T_post`):** configurable (e.g., `10 s` – `120 s`, default `30 s`).
  - **Trigger sources:**
    1. New confirmed or pending DTC (`Mode 03` / `Mode 07` / MIL bit in `0101` polled periodically, e.g. every `2–5 s`).
    2. User-defined PID threshold rule.
    3. Manual "Capture Now / Mark Event" button.

### 4.2 Rolling Ring Buffer & Capture State Machine
1. **ARMED (Rolling Buffer):** App polls the active PID list in a loop, timestamping each sample (`timestamp_ms`, `t_rel_ms`) into an in-memory circular buffer sized to `T_pre`.
2. **TRIGGERED (Post-Fault Window):** On trigger, snapshot the `T_pre` buffer, query `Mode 03` (confirmed DTCs), `Mode 07` (pending DTCs), `Mode 02` (Freeze Frame), and `ATRV`, then continue recording for `T_post` seconds.
3. **SAVED / EXPORTABLE:** Persist the completed capture locally so it is never lost on app backgrounding or phone disconnect.

### 4.3 Export Format (AI & Mechanic Ready)
Export each captured incident as a self-contained **JSON** (and optional **CSV** for spreadsheet/plotting):
- **Metadata:** capture timestamp (ISO 8601), vehicle VIN (`0902`), active protocol (`ATDP`), firmware/app version.
- **Fault Summary:** trigger reason, confirmed/pending DTC list (`Mode 03`/`07`), ECU Freeze Frame (`Mode 02`).
- **Time-Series Samples:** `t_rel_ms` (from `-T_pre * 1000` to `+T_post * 1000`), `vbat_v`, and decoded engineering values for every configured PID.

---

## 5. Testing Against `VIRTUAL_BENCH` (Without a Car)

1. Flash a **LilyGO TTGO T-Display S3** with `sdkconfig.virtual_bench` (`idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.virtual_bench" -B build_vbench -p COMx flash`).
2. Connect via BLE (`OBD2-ELM327`) or Wi-Fi (`OBD2-Scanner` -> `192.168.42.1:35000`).
3. Simulate driving & fault triggers in real time:
   - **Physical buttons on LilyGO:** `GPIO 0` (bottom-right) cycles `IDLE` -> `CRUISE` -> `ACCEL` -> `OVERHEAT`; `GPIO 14` (top-right) cycles `CLEAN` -> `DTC P0301` -> `LOW 11.2V`.
   - **Remote AT commands:** send `AT+SIM:RPM=2800`, `AT+SIM:VBAT=11400`, `AT+SIM:DTC=P0131` (or `P0301`), and `AT+SIM:DTC=CLEAR` directly from the app or test harness to verify the pre/post-fault capture pipeline end-to-end.
