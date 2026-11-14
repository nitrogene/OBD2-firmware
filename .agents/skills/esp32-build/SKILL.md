---
name: esp32-build
description: >-
  Portable ESP-IDF v5.2+ build, host-test (uv), and flash/monitor workflow for
  ESP32-S3 firmware. Dynamically discovers ESP-IDF and toolchain paths across
  Windows, Linux, and macOS without hardcoded local paths.
compatibility: ESP-IDF v5.2+ (esp32s3), uv, PowerShell / bash
metadata:
  author: OBD2-Scanner-Dev
  version: "1.0.0"
---

# Skill: ESP32-S3 Build, Test & Flash (`esp32-build`)

Portable workflow for activating ESP-IDF, building `esp32s3` firmware, running host `uv` tests, and flashing physical hardware without relying on machine-specific paths.

---

## 1. Dynamic ESP-IDF Discovery (No Hardcoded Paths)

Never assume a fixed drive letter or version directory (e.g., `C:\Espressif` or `D:\Espressif`). Resolve the environment dynamically in this order:

### Windows (PowerShell)
1. **Already active?**
   Check `Get-Command idf.py -ErrorAction SilentlyContinue`. If found, proceed directly to Section 2.
2. **Resolve `IDF_TOOLS_PATH` and `IDF_PATH`:**
   - Check `$env:IDF_TOOLS_PATH` and `$env:IDF_PATH` if already set.
   - Otherwise, scan candidate roots (`$env:USERPROFILE\esp`, `C:\Espressif`, `D:\Espressif`, `E:\Espressif`, plus any drive root `\Espressif` via `Get-PSDrive -PSProvider FileSystem`) for `frameworks\esp-idf-*\export.ps1` (preferring `v5.2+`, newest version first).
3. **Avoid system-Python mismatch (critical on Windows):**
   If a newer system Python (e.g., 3.13) is in `PATH`, `export.ps1` may look for a nonexistent `python_env\idf*_py3.13_env`. Before sourcing `export.ps1`, check `<IDF_TOOLS_PATH>\tools\idf-python\*` and prepend that directory to `$env:PATH`.
4. **Activate:**
   ```powershell
   # Example pattern after resolving $toolsRoot and $idfRoot dynamically:
   $env:IDF_TOOLS_PATH = $toolsRoot
   if ($idfPythonDir) { $env:PATH = "$idfPythonDir;$env:PATH" }
   & "$idfRoot\export.ps1" | Out-Null
   idf.py --version
   ```

### Linux / macOS (`bash` / `zsh`)
1. Check `command -v idf.py`.
2. Otherwise, check `$IDF_PATH/export.sh`, `~/esp/esp-idf/export.sh`, or `/opt/esp/idf/export.sh` and `source` it.

### Fallback
If no ESP-IDF `v5.2+` installation is found, **stop and ask the user** for their `IDF_PATH` / `IDF_TOOLS_PATH`. Do not invent paths.

---

## 2. Build & Host Test Workflow

1. **Target configuration (only when unconfigured or switching target):**
   - Check `sdkconfig` (`CONFIG_IDF_TARGET="esp32s3"`). Run `idf.py set-target esp32s3` only if needed, as it triggers a full reconfigure.
2. **Firmware build:**
   ```powershell
   idf.py build
   ```
3. **Host Python tests (always via `uv`, never bare `python`):**
   ```powershell
   uv run pytest
   ```
4. **Autonomy & verification gates:**
   - Maximum **3 consecutive attempts** to fix a recurring build, CMake, or linker error; if still blocked, stop, summarize the root cause, and yield to the user.
   - Never mark tasks as `[x]` in `todo.md` before `idf.py build` and relevant tests pass.

---

## 3. Flash & Monitor (Physical Hardware)

Before flashing or connecting to hardware, enforce these mandatory safety rules:
- **Anti-backfeeding wiring order:** Always connect `GND` and `+12V` (`H3` or `J1`) **before** connecting `CANH`, `CANL`, or `K_LINE`.
- **In-vehicle USB-C (`J2`) isolation:** Host laptop must run strictly on internal battery (never on AC mains earth) when plugged into a live vehicle.

Commands:
1. List serial ports (e.g., `[System.IO.Ports.SerialPort]::GetPortNames()` on Windows, `/dev/ttyACM*` or `/dev/cu.usbmodem*` on Unix).
2. Flash (non-interactive) or flash + monitor (interactive):
   ```powershell
   idf.py -p <PORT> flash
   idf.py -p <PORT> flash monitor
   ```
3. **ROM Bootloader recovery:** If native USB CDC/JTAG (`IO19`/`IO20`) does not enumerate or fails to enter download mode, hold `IO0` (`TP10`) low while pulsing `EN` (`SW1` / `TP11`).
