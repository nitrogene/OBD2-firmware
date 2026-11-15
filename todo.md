# Firmware Development Roadmap (`todo.md`)

> [!NOTE]
> Tasks are checked off (`- [x]`) **only after** successful `idf.py build` / `uv run pytest` and hardware or bench validation. Hardware specifications are defined in [`nitrogene/OBD2` (`documents/software.md`)](https://github.com/nitrogene/OBD2/blob/main/documents/software.md).

---

## Phase 1 — ESP-IDF Bootstrap & Board Support Package (`bsp`)
- [x] Initialize ESP-IDF v5.2+ project structure (`CMakeLists.txt`, `main/CMakeLists.txt`, `main/app_main.c`).
- [x] Configure `sdkconfig.defaults` (ESP32-S3, 240 MHz CPU, 16 MB Flash, 2 MB Octal PSRAM, TWAI, Native USB CDC on `IO19`/`IO20`, BLE 5.0).
- [x] Implement `components/bsp/` with exact GPIO mapping (`IO1` `VBAT_SENSE`, `IO2` `LED_STATUS`, `IO4` `KLINE_RX`, `IO5` `KLINE_TX`, `IO15` `TWAI_TX`, `IO16` `TWAI_RX`).
- [x] Implement a minimal `MODE_BRINGUP` / USB-C CDC self-test CLI (LED1 PWM, `VBAT_SENSE` readout, raw CAN `500 kbps` ping/pong, K-Line `10,400 baud` local echo & `25 ms` pulse) for dual-board hardware validation.
- [x] Test `logic-analyzer` skill without a target board (`uv run .agents/skills/logic-analyzer/scripts/analyze_capture.py --self-test`, `sigrok-cli --scan` / WinUSB driver, and `fx2lafw` capture test).
- [ ] Execute hardware bring-up checklist ([`bringup.md`](bringup.md)) on physical board(s) (multimeter, `LED1` PWM & USB-C CDC console, 8-ch USB logic analyzer on `H1`/`H2`, dual-board CAN/K-Line bench).
- [x] Verify CI `esp-idf-build` job (`.github/workflows/ci.yml`) passes on `esp32s3` after initial bootstrap.

## Phase 2 — Power Telemetry & Sleep Governance (`power_mgmt`)
- [x] Implement `ADC1_CH0` (`IO1`) calibrated sampling (`adc_oneshot` + `ADC_ATTEN_DB_12`, 32/64-sample moving average, `k = 12/112`).
- [x] Implement vehicle power state detection (USB-only `< 2.8 V`, critical `< 11.8 V`, resting `12.4–12.8 V`, engine running `13.8–14.6 V`, overvoltage `> 15.5 V`).
- [x] Implement 10-minute inactivity Deep Sleep and wake-up (`ext0`/`ext1` on bus activity or ULP threshold `> 13.2 V`).
- [x] Enable hardware task watchdog (TWDT) and optional boot self-test diagnostics.
- [ ] Validate Phase 2 on hardware (`power` / `vbat` CLI across USB-only, `12V` resting, `14V` running, `> 15.5V` SOS, and `power sleep` / `ext1` wake-up).

## Phase 3 — CAN / TWAI & ISO-TP (`twai_driver`, `isotp`)
- [x] Implement `components/twai_driver/` supporting 500 kbps and 250 kbps (11-bit and 29-bit IDs), listen-only bitrate probe, hardware acceptance filters, bus-off recovery, and raw monitor sniffer.
- [x] Implement `components/isotp/` (ISO 15765-2: Single Frame, First Frame, Flow Control, Consecutive Frame) + host Python unit tests (`uv run pytest`).
- [x] Verify CI `python-tests` job (`.github/workflows/ci.yml`) runs ISO-TP `pytest` suite and add host C unit tests if applicable.
- [ ] Validate 500 kbps CAN echo on dual-board mirror bench (`JP1` CLOSED on both boards).

## Phase 4 — K-Line, Auto-Detection & Diagnostic Protocols (`kline_driver`, `obd2_stack`, `kwp2000`)
- [x] Implement `components/kline_driver/` at 10,400 baud 8N1 with local TX-to-RX echo collision filtering, checksums, P1–P4 timings, and periodic keep-alive.
- [x] Implement 5-Baud Slow Init (bit-bang `IO5`: `0x33`/`0x11` -> `0x55` -> `KB1`/`KB2` ISO 9141 `08 08` vs KWP `94 94` -> `~KB2`) and Fast-Init (`25 ms` `T_inil` / `T_wup` -> `StartCommunication`).
- [x] Implement `ATSP0` auto-protocol detection (listen-only CAN 11b/29b 500k/250k -> KWP Fast/Slow -> ISO 9141-2) with NVS persistence of last working protocol.
- [x] Implement `components/obd2_stack/` (SAE J1979 Modes `01`–`09`), `components/kwp2000/` (GM-Daewoo multi-module `0x21`/`0x22`, `0x30`, `0x18`/`0x14`), and default block on sensitive services (`0x27`, `0x34`/`0x36`) outside unlocked extended mode.
- [x] Add host Python unit tests (`uv run pytest` via `ctypes` on pure C) for K-Line checksum/framing, `obd2_stack` PID/DTC encoding, `kwp2000` services, and security gating (`0x27`/`0x34`/`0x36`), verified in CI `python-tests`.
- [ ] Validate Phase 4 on hardware (`logic-analyzer` on `H1` for 5-baud & `25 ms` Fast-Init timings, dual-board K-Line echo, and real GM-Daewoo Kalos test).

## Phase 5 — ELM327 Emulation, ECU Simulator, Wireless Transports & Extended Mode (`elm327_emu`, `ecu_simulator`, `comm_ble`, `comm_wifi`)
- [ ] Implement unified `transport_stream` abstraction (static ring buffers, zero dynamic allocation on hot paths, single-active-transport lock).
- [ ] Implement `components/elm327_emu/` v1.5 parser (`>`, `\r`, `ATZ`/`ATI`/`ATWS`/`ATD`/`ATPC`, `ATE`/`ATL`/`ATS`/`ATH`, `ATRV` via `power_mgmt`, `ATSP`/`ATDP`/`ATDPN`, `ATAT`/`ATST`, `ATSH`/`ATCRA`/`ATCF`/`ATCM`, `ATCAF`/`ATFCSH`/`ATFCSD`/`ATFCSM`, `ATMA`, `ATSI`/`ATFI`, exact ELM327 status strings & `0:`/`1:` multi-line format).
- [ ] Implement `components/ecu_simulator/` (`MODE_ECU_SIMULATOR`: cyclic CAN frames, Mode `01`–`04` responder on `0x7E8`, K-Line 5-baud & Fast-Init responder, CLI `set rpm`/`speed`/`coolant`/`fault inject`).
- [ ] Implement `components/comm_ble/` (BLE 5.0 GATT UART `FFF0` `FFF1`/`FFF2` + alias `FFE0`/`FFE1`, configurable name, secure pairing, MTU negotiation, clean disconnect).
- [ ] Implement `components/comm_wifi/` (SoftAP `192.168.0.10` + ELM327 TCP server port `35000`, NVS-configurable SSID/password, optional STA mode, OTA update & web config page).
- [ ] Implement extended "custom" mode (enter/exit command, binary/SLCAN raw CAN/K-Line sniffer, UDS `0x10`/`0x22`/`0x2E`/`0x19`/`0x14`, bus error/load stats, NVS factory reset).
- [ ] Run end-to-end validation (`python-OBD`, `ELM327-emulator`, Car Scanner / OBD Fusion, real GM-Daewoo Kalos K-Line test, disconnect/12V-drop robustness).
- [ ] Extend CI (`.github/workflows/ci.yml` / `release.yml`) with formatting/lint checks (`clang-format`, `ruff`) and binary release artifacts on `v*` tags.
