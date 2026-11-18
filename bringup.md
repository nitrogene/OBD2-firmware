# Hardware Bring-Up & Bench Validation Guide (`bringup.md`)

Step-by-step hardware validation procedure for **a single board** and **two boards in a mirror bench setup (ECU <-> Scanner)**.

> [!CAUTION]
> **Mandatory Safety Rules (see [`AGENTS.md`](AGENTS.md) & [`README.md`](README.md)):**
> 1. **Anti-Backfeeding Wiring Order:** Always connect `GND` and `+12V` (via `H3` or `J1`) **before** connecting `CANH`, `CANL`, or `K_LINE`.
> 2. **USB-C Isolation on Live Vehicles:** When the board is connected to a live vehicle, the host laptop plugged into `J2` must run strictly **on battery only** (never on AC mains earth).
> 3. **12V Scope Only:** Never supply beyond automotive 12V specifications (`U4` buck converter / `D1` TVS rated up to `26.0 V`).
> 4. **3.3V Logic Limits:** `H1`, `H2`, `IO1..IO20` are **not 5V tolerant** (`Vmax = 3.6 V`). Never connect a logic analyzer probe directly to `K_LINE` (`12V`) or `CANH`/`CANL`.

---

## 1. Pre-Flash Checks (Blank Board — Multimeter Only)

Perform these checks before the first flash on every new board.

### 1.1 Unpowered Continuity & Resistance Checks (Ohmmeter)
1. **No power rail shorts:**
   * `+12V` <-> `GND` (`H3` or `J1`): no short circuit (high impedance / capacitive charging).
   * `3.3V` <-> `GND`: no short circuit.
2. **CAN bus termination (`JP1`):**
   * `CANH` <-> `CANL` (`J1`) with `JP1` **closed**: `~120 Ω`.
   * `CANH` <-> `CANL` (`J1`) with `JP1` **open**: high impedance (`> kΩ`).
   * Two-board bench connected with `JP1` closed at both ends: `~60 Ω`.

### 1.2 Powered `+12V` Checks (Blank Board, Before USB)
1. Connect `GND` then `+12V` (`12.0 V` - `12.6 V`) to `H3`.
2. **Power rails & regulation:**
   * Verify `3.3V` is present (powers `ESP32-S3-WROOM-1` and `U2` TJA1051T `VIO`).
   * Verify UVLO threshold if tested with a variable supply: `U4` (TPS54331) shuts down cleanly below `<= 8.01 V`.
3. **`VBAT_SENSE` voltage divider (`R12` `100 kΩ` / `R13` `12 kΩ`, `k = 12 / 112 ≈ 0.10714`):**
   * Measure voltage between `IO1` and `GND` with a multimeter:
     * At `V_BAT = 12.00 V` -> `V_ADC ≈ 1.286 V`
     * At `V_BAT = 12.60 V` -> `V_ADC ≈ 1.350 V`
     * On USB only (without `+12V`) -> `V_ADC < 0.300 V` (`+12V_PROT` isolated by `Q1`).

### 1.3 USB-C Enumeration (Blank Board, `J2` / `IO19`, `IO20`)
1. Connect `J2` via USB-C to the PC (without `+12V` or with `+12V` already established).
2. Verify a virtual serial port (`COMx` on Windows, `/dev/ttyACM*` on Linux) enumerates.
3. If no port appears: hold `IO0` (`TP10`) to `GND` while pulsing `EN` (`SW1` / `TP11`) to force ROM download mode.

---

## 2. Flash & Single-Board Tests (USB-C Only)

1. Flash and open the serial console:
   ```powershell
   idf.py -p COMx flash monitor
   ```
2. Verify the `obd2> ` prompt and run:
   * `info` -> must display `ESP32-S3`, `Flash : 16 MB`, `PSRAM : 2 MB`, and the GPIO mapping.
   * `led breath`, `led blink`, `led solid`, `led burst`, `led sos`, `led duty 50` -> visually verify `LED1` (`IO2`).
   * `vbat` -> on USB only, must report `V_ADC < 300 mV` (`V_BAT < 2.8 V`).

---

## 3. Single-Board Tests Under `+12V` (Multimeter + 8-Channel USB Logic Analyzer)

### 3.1 Probe Connections (`3.3V` Only)
Connect the 8-channel USB logic analyzer (or oscilloscope) to the `3.3V` debug headers:
* **Header `H1`:** `KLINE_RX` (`IO4`), `KLINE_TX` (`IO5`), `GND`
* **Header `H2`:** `TWAI_RX` (`IO16`), `TWAI_TX` (`IO15`), `GND`

### 3.2 `VBAT_SENSE` Calibration
1. Apply `GND` then `+12V` on `H3` (e.g., `12.60 V` measured by multimeter at `H3`).
2. Run `vbat` in the `obd2> ` console and compare `V_ADC` / `V_BAT` against the multimeter reading.

### 3.3 K-Line `10,400 baud` & Fast-Init (`U3` L9637D)
> [!NOTE]
> `kline echo` requires `+12V` on `H3`/`J1` to power the 12V side of transceiver `U3` (L9637D).

1. **`kline echo`:**
   * Expected CLI output: `K-Line TX 4 bytes, RX echo 4 bytes: 81 10 F1 81 -> ECHO OK`.
   * Logic analyzer (`H1`): `10,400 baud` UART frame (8N1, bit = `96.15 µs`) visible on `KLINE_TX` (`IO5`) and echoed nearly instantaneously on `KLINE_RX` (`IO4`).
2. **`kline pulse`:**
   * Expected CLI output: `K-Line 25 ms dominant / 25 ms recessive Fast-Init pulse sent on IO5`.
   * Logic analyzer (`H1`): measure a low pulse of **`25 ± 1 ms`** followed by a high return of **`25 ± 1 ms`** on `KLINE_TX` and `KLINE_RX`.

### 3.4 Single-Board TWAI / CAN `500 kbps` (`U2` TJA1051T)
1. Run `can ping 500`.
2. Logic analyzer (`H2`): observe `500 kbps` activity (bit = `2 µs`) on `TWAI_TX` (`IO15`). In single-board mode, the CLI should report an RX timeout (no other node on the bus to ACK the frame).

---

## 4. Two-Board Tests (Mirror Bench ECU <-> Scanner)

### 4.1 Bench Wiring
1. Flash **the same `obd2_firmware.bin` image** onto both boards (**Board A = ECU**, **Board B = Scanner/Bench**).
2. Verify `JP1` is **closed** on both boards (`~60 Ω` between `CANH` and `CANL`).
3. **Power-up order:**
   * Turn off `+12V`, connect the `J1` harness (or `H3` + `CANH`, `CANL`, `K_LINE`, `GND` bus), then turn on `+12V`.
   * Connect both boards via USB-C to the PC (e.g., `COMx` for ECU, `COMy` for Scanner) and open two monitors `idf.py -p COMx monitor` / `idf.py -p COMy monitor`.

### 4.2 CAN `500 kbps` Ping / Pong Test
1. On **Board A (ECU)**:
   ```text
   obd2> can pong 10000
   ```
2. Within 10 seconds, on **Board B (Scanner)**:
   ```text
   obd2> can ping
   ```
3. **Pass criteria:**
   * Board A prints `CAN RX ID=0x7DF, replying 0x7E8...` then `CAN TX 0x7E8: ESP_OK`.
   * Board B prints `CAN RX ID=0x7E8 DLC=8 data=06 41 00 BF ...`.
   * On the logic analyzer (`H2`), `0x7DF` and `0x7E8` frames at `500 kbps` are clean with a valid ACK bit.
