# Changelog

All notable changes to `OBD2-firmware` are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.4.1] - 2026-11-18

### Fixed
- **Physical Bus Routing (`main/app_main.c`)**: Guarded the in-memory `ecu_sim_handle_request()` shortcut behind `#ifdef CONFIG_OBD2_VIRTUAL_BENCH` so Standard Hardware builds route ELM327 requests over physical CAN (`twai_driver` / `isotp`) and K-Line (`kline_driver` / `kwp2000`) via `obd2_autodetect_run()`.
- **K-Line ECU Simulator & Driver (`kline_driver`, `ecu_simulator`)**: Split `kline_drv_send_recv()` into dedicated `kline_drv_send()` and `kline_drv_recv()` primitives (eliminating `NULL` argument errors and TWDT starvation), and added 5-baud/Fast-Init wakeup plus ISO 9141-2/KWP2000 responder framing.
- **Wi-Fi, HTTP & OTA Hardening (`comm_wifi`, `sdkconfig.*`)**: Added constant-time authentication on `POST /config`, `/ota`, and `/factory-reset`, removed plaintext passwords from `GET /`, added HTML escaping against Stored XSS, blocked `/ota` when `V_BAT < 11.8 V`, bound TCP `:35000` exclusively to `192.168.0.10`, removed `WIFI_AUTH_OPEN` fallback, enabled `partitions_ota.csv` + app rollback in both build profiles, and heap-allocated the `GET /` HTML buffer (`3072 B`).
- **BLE & Diagnostic Security Gating (`comm_ble`, `elm327_emu`, `obd2_stack`, `kwp2000`)**: Enforced encrypted GATT writes when required, added `AT+EXT` passkey / `AT+FACTORY` confirmation, and blocked state-modifying SIDs (`0x11`, `0x28`, `0x2E`, `0x2F`, `0x30`, `0x3B`, `0x3D`, `0x85`) when `extended_unlocked == false`.
- **Concurrency, Power Management & Runtime Hooks**: Protected `s_sim`, `s_client_sock`, and 64-bit `s_last_activity_us` with FreeRTOS mutexes; wired `power_mgmt_notify_activity()` on wireless/OTA traffic, `twai_drv_check_and_recover()` + `LED1` state patterns, periodic Deep Sleep `VBAT_SENSE > 13.2 V` wake-up, `TRANSPORT_ID_USB` passthrough, and active `AT+SNIFF`/`ATMA`/CAN filter handling.
- **CLI & Codec Validation**: Fixed `PID 0x42` scaling/docstring in `obd2_codec` and added `strtol`/`strtoul` `endptr` validation in `main/cli_commands.c`.

### Added
- **Host Unit & Fuzzing Tests**: Added `tests/test_autodetect.py`, 4-byte KWP framing tests, ELM327 parser fuzzing, and ECU simulator DTC/PID boundary tests (`30` tests total).
- **CI & Code Formatting**: Added `.clang-format` and updated `.github/workflows/ci.yml` to run `clang-format --dry-run --Werror`, `uvx ruff check tests/ scripts/`, and build both `sdkconfig.defaults` and `sdkconfig.virtual_bench`.

### Changed
- **Documentation Standardization**: Enforced English-only documentation in `AGENTS.md` (Section 7) and translated `bringup.md` and `todo.md` completely into English.

## [0.4.0] - 2026-11-17

### Added
- **Dual-OTA Partition Table & T-Display S3 Scenarios (`partitions_ota.csv`, `virtual_bench_ui`)**: Added dual-OTA partition layout, T-Display S3 physical button scenarios, bench-safe `power_mgmt`, and PID benchmarks.
- **Phase 6 Validation Harnesses**: Added `bleak` BLE black-box test, BLE/Wi-Fi arbitration, HTTP/OTA validator, and Car Scanner sequence tests.
- **CI/Test Discovery**: Restricted `pytest` discovery to `tests/` via `pyproject.toml` and expanded `README.md` documentation.

## [0.3.0] - 2026-11-16

### Added
- **LilyGO TTGO T-Display S3 Virtual Bench (`CONFIG_OBD2_VIRTUAL_BENCH`)**: Kconfig option (`main/Kconfig.projbuild`) and build profile (`sdkconfig.virtual_bench`) that disable conflicting physical CAN/K-Line/ADC/LED GPIOs, simulate `V_BAT` (`bsp_vbat_set_sim_mv` + CLI `sim vbat <mv>`), and route `elm327_emu` to the in-memory `ecu_simulator`.
- **ST7789 TFT Status Display (`components/virtual_bench_ui`)**: `esp_lcd` 8-bit i8080 (`320x170`) live dashboard displaying active transport (`BLE`/`WIFI`/`USB`/`IDLE`), simulated `RPM`, speed, coolant temperature, `V_BAT`, DTC count, and last `AT`/OBD command.
- **Black-Box Wireless Test Script (`scripts/vbench_blackbox_test.py`)**: automated TCP/BLE validation harness for `VIRTUAL_BENCH`.

## [0.2.0] - 2026-11-15

### Added
- **Unified Transport Stream (`components/transport_stream`)**: static FreeRTOS ring buffers (`esp_ringbuf`) with zero dynamic allocation on hot paths and single-active-transport lock (`USB`, `BLE`, `Wi-Fi`).
- **ELM327 v1.5 Emulator (`components/elm327_emu`)**: pure-C99 AT & OBD parser (`ATZ`, `ATI`, `ATWS`, `ATD`, `ATPC`, `ATE/L/S/H`, `ATRV`, `ATSP/DP/DPN`, `ATAT/ST`, `ATSH/CRA/CF/CM`, `ATCAF/FCSH/FCSD/FCSM`, `ATMA`, `ATSI/FI`), ISO-TP `0:`/`1:` multi-line formatting, and extended mode (`AT+EXT=1/0`, `AT+SNIFF=1/0`, `AT+STATS`, `AT+FACTORY`, SLCAN frame formatter).
- **ECU Simulator (`components/ecu_simulator`)**: pure-C99 OBD-II/UDS responder (`Mode 01/02/03/04/09`, UDS `0x10`, `0x22`, `0x2E`, `0x19`, `0x14`), physical CAN (`500 kbps` cyclic `0x280`/`0x316` + `0x7E8`) and K-Line (`10,400 baud`) bus responder (`MODE_ECU_SIMULATOR`), plus `sim` CLI commands (`status`, `rpm`, `speed`, `coolant`, `dtc`, `clear`, `start`, `stop`).
- **BLE 5.0 GATT Server (`components/comm_ble`)**: NimBLE UART service `0xFFF0` (`0xFFF1` Notify / `0xFFF2` Write) + alias `0xFFE0`/`0xFFE1`, NVS-configurable name (`OBD2-ELM327`), bonding, and MTU exchange.
- **Wi-Fi SoftAP/STA, TCP Server & OTA (`components/comm_wifi`)**: SoftAP `192.168.0.10` + ELM327 TCP server on port `35000`, optional `AP+STA` mode via NVS, HTTP web configuration page (port `80`), OTA firmware upload (`/ota`), and NVS factory reset (`/factory-reset`).
- **CAN/TWAI, ISO-TP, K-Line & OBD-II/KWP2000 Stacks**: `twai_driver`, pure-C `isotp` (11-bit/29-bit), `kline_driver` (5-baud & Fast-Init), `obd2_stack` (`ATSP0` auto-detect & codecs), `kwp2000`, and `power_mgmt` (`VBAT` ADC telemetry, Deep Sleep, TWDT).
- **Host Tests & CI/Release**: 23 `pytest` unit/loopback tests (`tests/test_isotp.py`, `tests/test_phase4_protocols.py`, `tests/test_phase5_elm327_ecusim.py`), `ruff` lint job in `.github/workflows/ci.yml`, and `.github/workflows/release.yml` for `v*` tag binary releases.

## [0.1.0] - 2026-11-08

### Added
- Initial ESP-IDF `v5.5.6` project skeleton for `ESP32-S3-WROOM-1`, BSP (`components/bsp`), `MODE_BRINGUP` USB-C CLI, and GitHub Actions CI.
