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
- [x] Implement unified `transport_stream` abstraction (static ring buffers, zero dynamic allocation on hot paths, single-active-transport lock).
- [x] Implement `components/elm327_emu/` v1.5 parser (`>`, `\r`, `ATZ`/`ATI`/`ATWS`/`ATD`/`ATPC`, `ATE`/`ATL`/`ATS`/`ATH`, `ATRV` via `power_mgmt`, `ATSP`/`ATDP`/`ATDPN`, `ATAT`/`ATST`, `ATSH`/`ATCRA`/`ATCF`/`ATCM`, `ATCAF`/`ATFCSH`/`ATFCSD`/`ATFCSM`, `ATMA`, `ATSI`/`ATFI`, exact ELM327 status strings & `0:`/`1:` multi-line format).
- [x] Implement `components/ecu_simulator/` (`MODE_ECU_SIMULATOR`: cyclic CAN frames, Mode `01`–`04` responder on `0x7E8`, K-Line 5-baud & Fast-Init responder, CLI `set rpm`/`speed`/`coolant`/`fault inject`).
- [x] Implement `components/comm_ble/` (BLE 5.0 GATT UART `FFF0` `FFF1`/`FFF2` + alias `FFE0`/`FFE1`, configurable name, secure pairing, MTU negotiation, clean disconnect).
- [x] Implement `components/comm_wifi/` (SoftAP `192.168.0.10` + ELM327 TCP server port `35000`, NVS-configurable SSID/password, optional STA mode, OTA update & web config page).
- [x] Implement extended "custom" mode (enter/exit command, binary/SLCAN raw CAN/K-Line sniffer, UDS `0x10`/`0x22`/`0x2E`/`0x19`/`0x14`, bus error/load stats, NVS factory reset).
- [ ] Run end-to-end validation (`python-OBD`, `ELM327-emulator`, Car Scanner / OBD Fusion, real GM-Daewoo Kalos K-Line test, disconnect/12V-drop robustness). *(Host loopback TCP + C unit tests passing; hardware/app validation deferred to Phase 6 & bench tests.)*
- [x] Extend CI (`.github/workflows/ci.yml` / `release.yml`) with formatting/lint checks (`clang-format`, `ruff`) and binary release artifacts on `v*` tags.

## Phase 6 — Black-Box Bench on LilyGO TTGO T-Display S3 (`VIRTUAL_BENCH`)
- [x] Add optional `CONFIG_OBD2_VIRTUAL_BENCH` Kconfig/board profile for LilyGO T-Display S3 (disables physical CAN/K-Line pins shared with the TFT and routes `elm327_emu` / `obd2_stack` to the in-memory `ecu_simulator` + simulated `V_BAT`).
- [x] Add optional ST7789 TFT status screen (`esp_lcd`, active only under `CONFIG_OBD2_VIRTUAL_BENCH`) displaying active transport (`BLE`/`WiFi`/`USB`), simulated ECU metrics (`RPM`, speed, coolant, `V_BAT`, DTCs), and last `AT`/OBD RX-TX command.
- [x] Run black-box wireless validation on T-Display S3 (BLE `FFF0`/`FFE0` and Wi-Fi TCP `192.168.0.10:35000`): `ATZ`..`ATRV`, Mode `01` PIDs, multi-frame `0902` VIN, `03`/`04` DTC injection/clear, and disconnect/reconnect stress tests.
- [ ] Add LilyGO physical button controls (`GPIO 0` / `GPIO 14` under `CONFIG_OBD2_VIRTUAL_BENCH`) to cycle engine scenarios (`RPM`/`speed`/`coolant`), inject/clear DTCs, and simulate `V_BAT` drops without a host PC. *(Implemented & flashed to T-Display S3; ready for physical button press check.)*
- [ ] Validate multi-transport lock arbitration (`transport_stream`: simultaneous BLE + Wi-Fi TCP clients, lock exclusivity, timeout & clean release).
- [x] Benchmark BLE (`FFF1`/`FFF2`) vs Wi-Fi TCP (`35000`) PID polling throughput & latency (200+ `010C`/`010D` requests/s) for Android UI gauge sizing. *(BLE: 200 PIDs in 24.66s = 8.1 req/s; Wi-Fi TCP: 200 PIDs in 1.46s = 136.9 req/s.)*
- [x] Validate `power_mgmt` state transitions & `ATRV` accuracy via simulated `V_BAT` (`sim vbat 14200` / `12600` / `11400` -> `RUNNING` / `RESTING` / `CRITICAL_LOW` + ST7789 display).
- [x] Validate Web Config (`http://192.168.0.10/` BLE name & Wi-Fi NVS persistence) and add dual-OTA partition table (`partitions_ota.csv`) to test `/ota` wireless flashing. *(`GET /` HTML OK + `POST /ota` 1182608 bytes -> `OTA OK, rebooting...` verified on T-Display S3.)*
- [ ] Validate compatibility with `python-OBD`, Car Scanner / OBD Fusion, and the custom Android client app (ELM327 + extended mode).

## Phase 7 — Firmware Security & README Compliance Remediation

### Actions issues de la revue du 2026-11-17 (Antigravity (Gemini))
<!-- Source : review/review001.md - Verdict : 🔴 Rejected (Critical fixes required) -->

#### Correctifs Critiques (Bloquants)
- [x] **[BLOQUANT] Standard Hardware Profile Never Routes ELM327 Requests to Physical CAN or K-Line Buses :** Guard the direct in-memory `ecu_sim_handle_request()` shortcut with `#ifdef CONFIG_OBD2_VIRTUAL_BENCH`, wire `elm_cb.set_protocol` to `obd2_autodetect_run()` / `twai_drv_init()` / `kline_drv_fast_init()` / `kline_drv_slow_init()`, and dispatch `elm_send_obd_req_cb()` over `isotp` (`twai_driver`, protocols `6..9`) or `kline_drv_send_recv()` (protocols `3..5`) with `obd2_is_service_allowed()` enforcement (`main/app_main.c:46-75, 111-119`).
- [x] **[BLOQUANT] Broken K-Line ECU Simulator Task (`kline_drv_send_recv(NULL, 0, ...)` Always Fails & Triggers TWDT Panic) :** Split `kline_drv_send_recv()` into separate `kline_drv_send()` (with echo verification) and `kline_drv_recv()` functions in `components/kline_driver/`, use them in `ecu_sim_kline_task()` with a fallback `vTaskDelay()` on error, and implement 5-baud/Fast-Init wakeup + ISO 9141-2/KWP2000 responder framing (`components/ecu_simulator/ecu_simulator.c:138-187`, `components/kline_driver/kline_driver.c:110-141`).
- [x] **[BLOQUANT] Unauthenticated HTTP OTA & Config, Plaintext Wi-Fi Password Disclosure / Stored XSS, Missing `V_BAT` Check, and STA Network Exposure :** Add HTTP authentication on `/config`, `/ota`, and `/factory-reset`, remove plaintext passwords from `GET /` with HTML escaping on SSIDs, block `/ota` when `V_BAT < 11.8 V`, enable `partitions_ota.csv` + app rollback in `sdkconfig.defaults`, bind TCP `:35000` to `192.168.0.10`, and disallow `WIFI_AUTH_OPEN` fallback (`components/comm_wifi/comm_wifi.c:135-367, 422-427`, `sdkconfig.defaults:10`).
- [x] **[BLOQUANT] Unauthenticated BLE GATT Access, Trivial `AT+EXT=1` / `AT+FACTORY` Unlock, and Incomplete Service Security Gating :** Block `0x11`, `0x28`, `0x2E`, `0x2F`, `0x30`, `0x3B`, `0x3D`, and `0x85` in `obd2_is_service_allowed()` when `extended_unlocked == false`, pass `extended_unlocked` to `kwp2000_build_io_control()`, require a passkey/confirmation for `AT+EXT` and `AT+FACTORY`, and support encrypted BLE GATT writes (`BLE_GATT_CHR_F_WRITE_ENC`) (`components/comm_ble/comm_ble.c:83-120, 289-295`, `components/elm327_emu/elm327_emu.c:127-159`, `components/obd2_stack/obd2_codec.c:5-22`, `components/kwp2000/kwp2000.c:51-99`).

#### Améliorations de Robustesse & CEM (Importants)
- [x] **[IMPORTANT] Missing USB-C CDC Transport Integration with `transport_stream` & `elm327_emu` :** Register `TRANSPORT_ID_USB` in `main/cli_commands.c` (via an `at`/`elm` passthrough command or automatic `AT` forwarding) and add `AT+SIM=...` commands in `elm327_emu.c` for wireless simulator control (`main/app_main.c`, `main/cli_commands.c`, `components/transport_stream/include/transport_stream.h:15`).
- [x] **[IMPORTANT] Wireless Activity (`BLE` / `Wi-Fi` / `ELM327`) Never Resets the 10-Minute Auto-Sleep Timer :** Call `power_mgmt_notify_activity()` inside `elm327_host_task()` (`main/app_main.c:84`) whenever `n > 0U` and inside `http_post_ota_handler()` (`comm_wifi.c:228`) during OTA chunk reception (`main/app_main.c:77-91`, `components/transport_stream/transport_stream.c:59-84`, `components/power_mgmt/power_mgmt.c:105-148`).
- [x] **[IMPORTANT] Status LED (`LED1`) State Machine and TWAI `Bus-Off` Recovery Are Not Hooked into Runtime Operations :** Invoke `twai_drv_check_and_recover()` in the CAN polling/send loop (setting `BSP_LED_SOS` on `TWAI_ALERT_BUS_OFF`), and drive `bsp_led_set_pattern()` across `obd2_autodetect_run()` (`BLINK_SLOW`), protocol lock (`SOLID`), and frame TX/RX (`BURST`) (`main/app_main.c`, `components/obd2_stack/obd2_autodetect.c`, `components/twai_driver/twai_driver.c:152-179`).
- [x] **[IMPORTANT] `AT+SNIFF=1`, `ATMA`, and CAN Filter/Header/Timing `AT` Commands Are Parsed as No-Ops :** Pass `elm327_ctx_t` header/filter/timing parameters into `send_obd_req()`, separate `ATCRA` from `ATCF`/`ATCM` with strict hex validation, and stream incoming CAN/K-Line frames via `elm327_format_slcan_frame()` when `sniff_enabled` or `ATMA` is active (`components/elm327_emu/elm327_emu.c:139-143, 252-307, 322-325`).
- [x] **[IMPORTANT] Multi-Task Data Races on `s_sim`, `s_client_sock`, and 64-Bit `s_last_activity_us` :** Protect `s_sim` in `ecu_simulator`, `s_client_sock` in `comm_wifi.c`, and `s_last_activity_us` in `power_mgmt.c` with FreeRTOS mutexes (`components/ecu_simulator/ecu_simulator.c:14-18`, `components/comm_wifi/comm_wifi.c:33, 72-89, 346-365`, `components/power_mgmt/power_mgmt.c:20, 109, 112, 231-234`).
- [x] **[IMPORTANT] Deep Sleep Omits `VBAT_SENSE > 13.2 V` Wake-Up for K-Line Vehicles :** Add ULP RISC-V / ADC threshold wake-up or periodic Deep Sleep timer wake-up in `power_mgmt_enter_deep_sleep()` to sample `VBAT_SENSE` (`IO1`) and wake when `V_BAT > 13.2 V` (`components/power_mgmt/power_mgmt.c:236-258`).

#### Optimisations de Nomenclature & DFM (Mineurs)
- [x] **[MINEUR] CI Workflow (`ci.yml`) Does Not Execute `clang-format`, Omits `scripts/` from `ruff`, and Skips Building `VIRTUAL_BENCH` :** Update `.github/workflows/ci.yml` to run `clang-format --dry-run --Werror` on `main/` and `components/`, run `uvx ruff check tests/ scripts/`, and build both `sdkconfig.defaults` and `sdkconfig.virtual_bench` (`.github/workflows/ci.yml:25-32, 68-75`).
- [x] **[MINEUR] `obd2_scale_mode01_pid` Docstring Unit Mismatch for PID `0x42` & Unchecked `strtol` in CLI :** Update `obd2_codec.h:54` docstring to `(Control module V * 100)` with rounded division in `obd2_codec.c:160`, validate `strtol`/`strtoul` `endptr` in `main/cli_commands.c`, and list `vbat|start|stop` in `sim` usage strings (`components/obd2_stack/include/obd2_codec.h:54`, `components/obd2_stack/obd2_codec.c:160`, `main/cli_commands.c:83, 162, 367, 372-406, 465`).
- [x] **[MINEUR] Unfilled Template Placeholders in `review/guidelines.md` :** Populate Section 2 of `review/guidelines.md` with the ESP32-S3 OBD-II hardware boundaries, protocol/security audit axes, and `VIRTUAL_BENCH` design decisions (`review/guidelines.md:43-57`).

#### Durcissement Cryptographique & Provisioning Matériel (Prochaines Étapes)
- [ ] **Dériver les mots de passe par défaut (`ap_pass`, `admin_pass`, passkey `AT+EXT`) depuis les caractéristiques matérielles de l'ESP32-S3 :** Remplacer le secret statique par défaut (`"12345678"`) dans `components/comm_wifi/comm_wifi.c` par une dérivation déterministe unique par puce basée sur l'adresse MAC d'usine (`esp_efuse_mac_get_default()`) et l'identifiant unique eFuse (`ESP_EFUSE_OPTIONAL_UNIQUE_ID` / bloc eFuse HMAC via `mbedtls_sha256` ou périphérique `esp_hmac`), avec affichage du code d'appairage initial sur le port série USB-C CDC (et sur l'écran ST7789 en mode `VIRTUAL_BENCH`).
