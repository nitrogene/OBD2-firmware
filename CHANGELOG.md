# Changelog

All notable changes to `OBD2-firmware` are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
