# Firmware Active Backlog (`todo.md`)

> [!NOTE]
> Per [`AGENTS.md`](AGENTS.md) Section 8, `todo.md` tracks **only open work** (`- [ ]`). Completed milestones (Phases 1–7, `v0.1.0`–`v0.4.1`) are recorded in [`CHANGELOG.md`](CHANGELOG.md), and the current functional contract is documented in [`README.md`](README.md).
> Hardware specifications are defined in [`nitrogene/OBD2` (`documents/software.md`)](https://github.com/nitrogene/OBD2/blob/main/documents/software.md).

---

## Hardware & End-to-End Validation

- [ ] **Execute Hardware Bring-Up Checklist ([`bringup.md`](bringup.md)):** Validate physical board(s) with multimeter, `LED1` PWM & USB-C CDC console, 8-channel USB logic analyzer on `H1`/`H2`, and dual-board CAN/K-Line bench.
- [ ] **Validate Power Telemetry & Sleep on Hardware:** Run `power` / `vbat` CLI across USB-only (`< 2.8 V`), `12V` resting, `14V` running, `> 15.5 V` SOS, and `power sleep` / `ext1` + `VBAT_SENSE > 13.2 V` wake-up.
- [ ] **Validate `500 kbps` CAN Echo on Dual-Board Mirror Bench:** Test `can ping` / `can pong` with `JP1` CLOSED on both boards (`~60 Ω`).
- [ ] **Validate K-Line & Auto-Detection on Hardware:** Verify `logic-analyzer` captures on `H1` for 5-baud & `25 ms` Fast-Init timings, dual-board K-Line echo, and live GM-Daewoo Kalos vehicle test.
- [ ] **Validate Remaining End-to-End Client Compatibility:** Test Car Scanner over Wi-Fi (`192.168.42.1:35000`), `python-OBD`, OBD Fusion, custom Android client app (ELM327 + extended mode), and 12V-drop robustness on hardware *(Car Scanner BLE validated)*.

---

## Cryptographic Hardening & Hardware Provisioning

- [ ] **Hardware Provisioning Module (`components/bsp/bsp_provisioning.c`) with Domain-Separated Key Derivation:** Implement `bsp_prov_get_default_ap_pass()`, `bsp_prov_get_ext_passkey()`, and `bsp_prov_get_ap_ssid()` using `esp_efuse_mac_get_default()` + `ESP_EFUSE_OPTIONAL_UNIQUE_ID` via `mbedtls_sha256` with distinct domain-separation tags (`"OBD2-AP-PASS-v1"`, `"OBD2-EXT-KEY-v1"`), formatted in an unambiguous alphanumeric alphabet (`0/O`, `1/I/L` excluded, `10-12` chars for WPA2-PSK, `8` chars for `AT+EXT`), with optional `esp_hmac` (`HMAC_UP` eFuse key block) priority for production boards.
- [ ] **Wire Derived Secrets into `comm_wifi`, `elm327_emu`, and SoftAP SSID:** Replace `COMM_WIFI_DEFAULT_PASS` (`"12345678"`) fallback in `components/comm_wifi/comm_wifi.c` with `bsp_prov_get_default_ap_pass()`, suffix the default SoftAP SSID (`OBD2-Scanner-XXXX`) with the last 2 MAC bytes, and call `elm327_set_ext_passkey()` from `main/app_main.c` at boot so `AT+FACTORY` / `/factory-reset` deterministically restores per-chip credentials.
- [ ] **Secure Pairing UX (USB-C CDC CLI & `VIRTUAL_BENCH` ST7789 Display):** Avoid printing derived secrets in permanent boot logs; add an explicit USB-C CDC CLI command (e.g., `prov`) to display the default `ap_pass` and `AT+EXT` passkey on physical connection, and display the initial pairing code on the ST7789 screen (`components/virtual_bench_ui`) on first boot (blank NVS) or button press.
- [ ] **Enforce BLE Link Encryption by Default (`components/comm_ble`):** Enable `s_require_enc = true` by default when bonding is active so unencrypted GATT writes are rejected (`BLE_ATT_ERR_INSUFFICIENT_ENC`).
- [ ] **Provisioning Host & Hardware Tests:** Add `pytest` host unit tests for deterministic derivation, domain separation (`ap_pass` != `ext_passkey`), WPA2 length (`>= 8`), and unambiguous alphabet; verify on two physical ESP32-S3 boards that secrets differ per chip and survive `AT+FACTORY`.
