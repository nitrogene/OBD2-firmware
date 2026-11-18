# Project Rules: ESP32-S3 OBD-II Scanner & ECU Simulator Firmware

## 1. Hardware Contract & Single Source of Truth
- **Hardware Reference:** The physical board is defined in the sibling hardware repository ([nitrogene/OBD2](https://github.com/nitrogene/OBD2)), specifically `documents/software.md` and `documents/hardware.md`.
- **No Invented Hardware Specs:** Never guess or alter GPIO assignments, voltage divider ratios (`VBAT_SENSE` `100 kΩ` / `12 kΩ`), transceiver constraints (`U2` TJA1051T, `U3` L9637D), or jumper roles (`JP1`, `JP2`). Cross-check against `README.md` or `OBD2/documents/software.md`.
- **3.3V Logic Boundary:** `ESP32-S3-WROOM-1` I/Os are not 5V tolerant (`Vmax = 3.6 V`).

## 2. Build, Tooling & Code Modularity
- **Firmware Stack:** ESP-IDF v5.2+ (C/C++, FreeRTOS, `idf.py` with CMake/Ninja).
- **Python Execution via `uv`:** All Python tooling, host scripts, and tests must be executed via `uv run` (e.g., `uv run pytest`). Direct calls to `python` are prohibited.
- **Modular Components:** Organize firmware under `components/` (`bsp`, `twai_driver`, `isotp`, `kline_driver`, `obd2_stack`, `kwp2000`, `power_mgmt`, `transport_stream`, `elm327_emu`, `ecu_simulator`, `comm_ble`, `comm_wifi`, `virtual_bench_ui`). Keep hardware pin mappings isolated in `components/bsp/`.
- **Error Handling & Concurrency:** Check `esp_err_t` return values explicitly, avoid blocking delays in high-priority FreeRTOS tasks (`twai_rx_tx`, `kline_engine`), and handle K-Line local TX-to-RX echo filtering.

## 3. Safety & Bench Testing Rules
- **Anti-Backfeeding Wiring Order:** Always connect `GND` and `+12V` (via `H3` or `J1`) **before** connecting `CANH`, `CANL`, or `K_LINE`.
- **In-Vehicle Ground Isolation:** When debugging via USB-C (`J2`) in a live vehicle, the host laptop must run strictly on battery (never on AC mains earth).
- **12V Scope Only:** Firmware targets 12V passenger vehicles (SAE J1962 Type A). Never target 24V commercial systems.

## 4. Mandatory Pre-Completion & Anti-Regression Checks

Before marking any task as `[x]` in `todo.md` or claiming a feature in `README.md`, agents and developers **must** verify all 6 gates below:

### 4.1 End-to-End Integration & Profile Separation (`Standard` vs `CONFIG_OBD2_VIRTUAL_BENCH`)
- **No Unconditional Simulator Stubs in `app_main.c`:** In-memory simulator shortcuts (`ecu_sim_handle_request`) and simulated `V_BAT` overrides must be strictly guarded by `#ifdef CONFIG_OBD2_VIRTUAL_BENCH` (or an explicit runtime bench flag). When `CONFIG_OBD2_VIRTUAL_BENCH` is disabled (Standard Hardware), `app_main.c` must route ELM327 callbacks (`send_obd_req`, `set_protocol`) to the physical bus drivers (`obd2_autodetect`, `twai_driver` + `isotp`, `kline_driver` + `kwp2000`).
- **No Dead Runtime Hooks:** Every exported health, telemetry, or UI state function must have an active caller on the runtime path:
  - `power_mgmt_notify_activity()` must be called on **all** active transport traffic (`BLE`, `Wi-Fi TCP`, `USB`, and HTTP OTA), not solely in USB CLI commands.
  - `bsp_led_set_pattern()` must be driven by actual protocol states (`BLINK_SLOW` during scan, `SOLID` when locked, `BURST` during frame TX/RX, `SOS` on overvoltage or CAN `Bus-Off`).
  - `twai_drv_check_and_recover()` must be called periodically in the active CAN task.
  - Every `transport_id_t` enum value (`TRANSPORT_ID_USB`, `TRANSPORT_ID_BLE`, `TRANSPORT_ID_WIFI`) must be registered via `transport_stream_register_tx()` and fed via `transport_stream_feed_rx()`.

### 4.2 Driver Preconditions & Watchdog (`TWDT`) Starvation Prevention
- **Cross-Check Callee Preconditions:** Before passing `NULL` or `0` arguments to a driver helper (such as `kline_drv_send_recv()`), inspect the callee's guard clauses (`if (tx_frame == NULL || rx_buf == NULL) return ESP_ERR_INVALID_ARG`). Provide dedicated `*_send()` and `*_recv()` primitives instead of overloading combined helpers with `NULL` pointers.
- **No Unthrottled Error Loops in FreeRTOS Tasks:** Every `while (running)` loop in a FreeRTOS task at priority `>= 3` must block on an OS primitive (queue, ringbuf, semaphore, UART/TWAI timeout) **and** include a fallback `vTaskDelay()` when a driver call returns an immediate error (`!= ESP_OK`), preventing CPU starvation and `5000 ms` Task Watchdog (`TWDT`) panics.

### 4.3 Firmware Security & Automotive Safety Gating
- **HTTP Config & Wireless OTA Hardening (`comm_wifi`):**
  - Never interpolate plaintext passwords (`s_pass`, `s_sta_pass`) into `GET /` HTML responses (`value=''`, `placeholder='********'`), and always HTML-escape user-configurable strings (`SSID`, device name) to prevent Stored XSS.
  - Require authentication on state-mutating HTTP endpoints (`POST /config`, `POST /ota`, `POST /factory-reset`).
  - In `POST /ota`, query `power_mgmt_get_status()` and reject flashing when `V_BAT < 11.8 V` (`POWER_STATE_UNDERVOLTAGE_CRITICAL`) or `> 15.5 V` (`POWER_STATE_OVERVOLTAGE`).
  - Ensure both build profiles include an OTA-capable partition table (`partitions_ota.csv`) and `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` if wireless OTA is enabled.
  - Never silently fall back to `WIFI_AUTH_OPEN` on SoftAP, and bind diagnostic TCP sockets (`:35000`) to the SoftAP interface (`192.168.42.1`) unless STA access is explicitly authorized.
- **BLE & Diagnostic Service Gating (`comm_ble`, `elm327_emu`, `obd2_stack`, `kwp2000`):**
  - Enforce link encryption (`BLE_GATT_CHR_F_WRITE_ENC`) when secure bonding is enabled.
  - Require a passkey/secret or physical confirmation before unlocking `AT+EXT` or executing `AT+FACTORY`.
  - When `extended_unlocked == false`, `obd2_is_service_allowed()` must block **all** state-modifying, actuator, memory-write, and reset SIDs: `0x11` (`ECUReset`), `0x27` (`SecurityAccess`), `0x28` (`CommunicationControl`), `0x2E` (`WriteDataByIdentifier`), `0x2F` (`InputOutputControlByIdentifier`), `0x30` (`InputOutputControlByLocalIdentifier`), `0x31` (`RoutineControl`), `0x34`–`0x37` (Download/Upload/Transfer), `0x3B` (`WriteDataByLocalIdentifier`), `0x3D` (`WriteMemoryByAddress`), and `0x85` (`ControlDTCSetting`).

### 4.4 Dual-Core Concurrency & Thread Safety (ESP32-S3 LX7)
- **Mutex Protection on Shared State:** Any struct, buffer, socket descriptor (`s_client_sock`), or 64-bit timestamp (`int64_t s_last_activity_us`) read and written by more than one FreeRTOS task must be guarded by a FreeRTOS mutex (`SemaphoreHandle_t`) or critical section (`portMUX_TYPE`). Note that 64-bit loads/stores (`int64_t` / `uint64_t`) are **not atomic** on 32-bit Xtensa LX7 cores.

### 4.5 Documentation-to-Code Behavioral Parity
- **No State-Only No-Op Commands:** Do not list an `AT` command (`AT+SNIFF=1`, `ATMA`, `ATSH`, `ATCRA`, `ATCF`, `ATCM`, `ATFCSH`, `ATFCSD`, `ATFCSM`, `ATST`) or hardware wakeup mode (`VBAT_SENSE > 13.2 V` wake-up for K-Line vehicles in Deep Sleep) as implemented in `README.md` or `[x]` in `todo.md` if the parser only stores a variable that no downstream driver reads.
- **CLI Input Validation & Help Parity:** Validate numeric CLI arguments using `strtol`/`strtoul` `endptr` checks (rejecting non-numeric strings), and keep CLI `help`/usage strings synchronized with `README.md`.

### 4.6 CI & Static Verification Gate
- **CI Must Execute Every Claimed Check:** `.github/workflows/ci.yml` must execute `clang-format --dry-run --Werror` on all `.c`/`.h` files under `main/` and `components/`, run `uvx ruff check tests/ scripts/`, run `uv run pytest`, and compile both `sdkconfig.defaults` (Standard Hardware) and `sdkconfig.virtual_bench` (`VIRTUAL_BENCH`). Run `uvx clang-format --dry-run --Werror` and `uvx ruff check` locally before committing C/H or Python changes.
- **Autonomy Limits:** Maximum 3 consecutive attempts to fix a recurring build, linker, or protocol failure. If blocked, stop, summarize the root cause, and yield control to the user.

## 5. Technical Review Gateway (`review/`)
- Scan `review/review*.md`, validate format with `uv run .agents/skills/review/scripts/review_tool.py validate`, sort findings by criticality (Blocking / Important / Minor) per `review/guidelines.md`, arbitrate with the user, integrate validated items into `todo.md` via `uv run .agents/skills/review/scripts/review_tool.py triage --apply --delete-review`, and delete the processed review file.

## 6. Post-Validation & Commit Policy
- Commit and push **only** after explicit textual approval from the user on the commit message (or when explicitly asked by the user to open a Pull Request on a feature branch).

## 7. Documentation Language, Typography & Formulas
- **English-Only Documentation:** All project documentation (`README.md`, `todo.md`, `CHANGELOG.md`, `bringup.md`, `AGENTS.md`, `review/`, `.github/`, `.agents/skills/`, as well as code comments and docstrings) must be written exclusively in English.
- **No LaTeX for pins, components, or simple values:** Use plain text with Markdown inline code: `IO1`, `LED1`, `100 kΩ`, `3.3V`, `500 kbps`.
- Dollar markup `$ ... $` is reserved strictly for complex mathematical equations.

## 8. Documentation Governance (`README.md` vs `todo.md` vs `CHANGELOG.md`)
Each root documentation file has a single, non-overlapping role:
- **`README.md` (Current Functional Contract):** High-level description of what the firmware **currently does** (architecture, hardware/pinout contract, safety/security guarantees, user-facing interfaces, CLI/`AT` commands, build & test workflows). Must stay free of implementation minutiae (no internal variable names, mutex details, or patch-level explanations) and must never claim unimplemented or unverified behavior.
- **`todo.md` (Active Backlog Only):** Open action items (`- [ ]`) and pending hardware/bench validations. Completed items (`- [x]`) should be pruned regularly once reflected in `CHANGELOG.md` (and `README.md` if applicable) so `todo.md` remains a concise list of remaining work.
- **`CHANGELOG.md` (Historical Record):** Versioned log of all notable changes (new features, bug fixes, security patches, refactors, CI updates) following Keep a Changelog / SemVer.

### Routing Rule When Completing a `todo.md` Item
Completing a task in `todo.md` updates `CHANGELOG.md` (when user-visible or release-relevant), and updates `README.md` **only if** the change alters the external contract:
- **Update `README.md`** when adding, changing, or removing a user-facing feature, `AT`/CLI command, protocol, security/gating policy, GPIO/hardware spec, build profile, or test workflow.
- **Do NOT update `README.md`** for internal bug fixes, thread-safety/mutex fixes, compiler/lint fixes, internal refactors, or hardware validation of an already-documented feature (record these in `CHANGELOG.md` only, and remove/prune the completed item from `todo.md`).

## 9. Dev-PC Hardware-In-the-Loop (`tests_hil/`) Regression Gate
- **CI Isolation:** `tests_hil/` is strictly for local developer benches with a physical target attached (LilyGO T-Display S3 `VIRTUAL_BENCH`). GitHub Actions CI must only run hardware-free `tests/` (`uv run pytest`).
- **When to Run `tests_hil/`:** Run `tests_hil/` **only on demand** or **when changes may impact** `comm_ble`, `comm_wifi`, `transport_stream`, `elm327_emu`, `ecu_simulator`, `virtual_bench_ui`, `main/app_main.c`, `main/cli_commands.c`, or as a pre-release gate before tagging `v*`.
- **When to Skip:** Do **not** run `tests_hil/` for documentation-only (`.md`), CI (`.github/`), host-test (`tests/`), or isolated physical-bus-only driver (`twai_driver`, `kline_driver`, `kwp2000`) changes.
- **No Hardware Connected:** If no LilyGO is attached on USB/BLE, report `tests_hil/` as skipped/not run (never block or fabricate results).
