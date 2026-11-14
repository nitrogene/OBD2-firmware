# Project Rules: ESP32-S3 OBD-II Scanner & ECU Simulator Firmware

## 1. Hardware Contract & Single Source of Truth
- **Hardware Reference:** The physical board is defined in the sibling hardware repository ([nitrogene/OBD2](https://github.com/nitrogene/OBD2)), specifically `documents/software.md` and `documents/hardware.md`.
- **No Invented Hardware Specs:** Never guess or alter GPIO assignments, voltage divider ratios (`VBAT_SENSE` `100 kΩ` / `12 kΩ`), transceiver constraints (`U2` TJA1051T, `U3` L9637D), or jumper roles (`JP1`, `JP2`). Cross-check against `README.md` or `OBD2/documents/software.md`.
- **3.3V Logic Boundary:** `ESP32-S3-WROOM-1` I/Os are not 5V tolerant (`Vmax = 3.6 V`).

## 2. Build, Tooling & Code Modularity
- **Firmware Stack:** ESP-IDF v5.2+ (C/C++, FreeRTOS, `idf.py` with CMake/Ninja).
- **Python Execution via `uv`:** All Python tooling, host scripts, and tests must be executed via `uv run` (e.g., `uv run pytest`). Direct calls to `python` are prohibited.
- **Modular Components:** Organize firmware under `components/` (`bsp`, `twai_driver`, `isotp`, `kline_driver`, `obd2_stack`, `kwp2000`, `power_mgmt`, `comm_ble`, `comm_wifi`, `ecu_simulator`). Keep hardware pin mappings isolated in `components/bsp/`.
- **Error Handling & Concurrency:** Check `esp_err_t` return values explicitly, avoid blocking delays in high-priority FreeRTOS tasks (`twai_rx_tx`, `kline_engine`), and handle K-Line local TX-to-RX echo filtering.

## 3. Safety & Bench Testing Rules
- **Anti-Backfeeding Wiring Order:** Always connect `GND` and `+12V` (via `H3` or `J1`) **before** connecting `CANH`, `CANL`, or `K_LINE`.
- **In-Vehicle Ground Isolation:** When debugging via USB-C (`J2`) in a live vehicle, the host laptop must run strictly on battery (never on AC mains earth).
- **12V Scope Only:** Firmware targets 12V passenger vehicles (SAE J1962 Type A). Never target 24V commercial systems.

## 4. Verification & Documentation Sequencing
- **No Anticipated Checkoffs:** Only check tasks in `todo.md` (`- [x]`) or update protocol/hardware status docs **after** the code compiles (`idf.py build`) and passes relevant unit/bench tests.
- **Autonomy Limits:** Maximum 3 consecutive attempts to fix a recurring build, linker, or protocol failure. If blocked, stop, summarize the root cause, and yield control to the user.

## 5. Technical Review Gateway (`review/`)
- Scan `review/review*.md`, sort findings by criticality (Blocking / Important / Minor) per `review/guidelines.md`, arbitrate with the user, integrate validated items into `todo.md`, and delete the processed review file.

## 6. Post-Validation & Commit Policy
- Commit and push **only** after explicit textual approval from the user on the commit message.

## 7. Typography & Formulas
- **No LaTeX for pins, components, or simple values:** Use plain text with Markdown inline code: `IO1`, `LED1`, `100 kΩ`, `3.3V`, `500 kbps`.
- Dollar markup `$ ... $` is reserved strictly for complex mathematical equations.
