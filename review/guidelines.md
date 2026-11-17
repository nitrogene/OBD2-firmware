# Reviewer Guidelines — ESP32-S3 OBD-II Firmware

Welcome! This document outlines instructions, technical requirements, and reporting standards for any **reviewer** (AI agent or human engineer) auditing the **ESP32-S3 OBD-II Firmware** project.

The goal of a technical review is to provide rigorous, evidence-based critical scrutiny to ensure reliability, safety, maintainability, and adherence to project requirements.

---

## 1. Format & Naming Conventions

### File Naming
Submit your review inside the `review/` directory using the following format:
`review/reviewXXX.md` *(e.g., `review001.md`, `review_architecture_01.md`)*.

> [!NOTE]
> **Lifecycle:** Once your review is placed in `review/`, the project team (user and AI assistant) will evaluate your observations, arbitrate each item, integrate accepted action points into `todo.md`, and then **delete** your `reviewXXX.md` file. Deletion signifies full resolution and keeps the intake clean for subsequent reviews.

### Mandatory Metadata Header
Every review **must begin** with this metadata block to guarantee full audit traceability:

```markdown
# Technical Review: [Short and descriptive title]

- **Reviewer / Agent Name:** [E.g. Claude, Gemini, Domain Expert, etc.]
- **Date:** [YYYY-MM-DD]
- **Audited Scope:** [E.g. Module / Subsystem / Architecture / PR / Release]
- **Examined Reference / Version:** [Git commit hash, tag, or document version]
- **Overall Verdict:** [🟢 Approved / 🟠 Approved with Reservations / 🔴 Rejected (Critical fixes required)]
```

---

## 2. Project Context, Audit Axes & Pitfalls to Avoid (Domain-Specific)

> [!IMPORTANT]
> **Customize this section for your project's domain** (e.g., Embedded Firmware, Hardware/PCB, Web/Backend, Mechanical CAD).

### 2.0 Universal Auditor Mindset: Tabula Rasa & Evidence-Based Scrutiny
- Disregard any prior conversational memory or unverified assumptions. Adopt the stance of an impartial third-party auditor discovering the project fresh.
- Ground every observation strictly in versioned repository artifacts (source files, specs, schematics, tests, datasheets, logs).
- Cross-check documentation claims against actual implementation and authoritative references before raising an alert.

### 2.1 Project Context & Known Constraints
- **Target Environment & Scope:** Automotive 12V passenger vehicles only (SAE J1962 Type A). Never target 24V commercial systems. Target MCU is `ESP32-S3-WROOM-1-N16R2` (`16 MB` Quad SPI Flash, `2 MB` Octal SPI PSRAM, `3.3V` logic — GPIOs are **not** 5V tolerant, `Vmax = 3.6 V`).
- **Core Stack & Architecture:** ESP-IDF `v5.5.6` / FreeRTOS modular firmware under `components/` (`bsp`, `power_mgmt`, `twai_driver`, `isotp`, `kline_driver`, `obd2_stack`, `kwp2000`, `transport_stream`, `elm327_emu`, `ecu_simulator`, `comm_ble`, `comm_wifi`, `virtual_bench_ui`). Dual build profiles: **Standard Hardware** (`sdkconfig.defaults`) and **LilyGO TTGO T-Display S3 Virtual Bench** (`sdkconfig.virtual_bench`, `CONFIG_OBD2_VIRTUAL_BENCH=y`).
- **Current Progress State:** Check `todo.md`, `README.md`, and `AGENTS.md` before raising findings to distinguish intentional roadmap items from regressions or unfulfilled claims.

### 2.2 Domain-Specific Audit Axes
- **Axis 1 — Safety, Security & Failure Modes:**
  - Verify wireless transport security (`comm_wifi` HTTP/OTA/TCP, `comm_ble` GATT permissions, no plaintext credential leaks or Stored XSS on `GET /`, authentication on `/config`, `/ota`, `/factory-reset`, and `AT+EXT` / `AT+FACTORY`).
  - Verify diagnostic service gating (`obd2_is_service_allowed()` must block `0x11`, `0x27`, `0x28`, `0x2E`, `0x2F`, `0x30`, `0x31`, `0x34`–`0x37`, `0x3B`, `0x3D`, `0x85` when `extended_unlocked == false`).
  - Verify battery voltage protection (`V_BAT < 11.8 V` blocks OTA/heavy operations; `> 15.5 V` triggers `BSP_LED_SOS`; Deep Sleep wakes on both CAN/K-Line edge and `VBAT_SENSE > 13.2 V` for quiet K-Line vehicles).
- **Axis 2 — Correctness, Timing & Concurrency:**
  - Check FreeRTOS multi-task thread safety across Dual-Core LX7 (mutex protection on `ecu_sim_state_t`, socket descriptors, and non-atomic 64-bit `int64_t` timestamps).
  - Check that high-priority tasks (`prio >= 3`) never spin without yielding on driver error paths (preventing `5000 ms` TWDT panics) and respect K-Line timings (`P1`–`P4`, `25 ms` Fast-Init, `200 ms/bit` 5-baud).
- **Axis 3 — Interfaces, Protocols & README Compliance:**
  - Verify that `main/app_main.c` routes ELM327 requests to physical CAN (`twai_driver`/`isotp`) and K-Line (`kline_driver`/`kwp2000`) when `!CONFIG_OBD2_VIRTUAL_BENCH`.
  - Verify that every `AT` command, CLI command, `LED1` pattern, and `transport_id_t` (`USB`, `BLE`, `WIFI`) documented in `README.md` is wired end-to-end (no state-only no-ops or dead functions).
- **Axis 4 — Performance, Resources & Partitioning:**
  - Verify zero dynamic allocation on hot packet paths (`transport_stream` static ring buffers, `isotp_link_t`), bounded stack usage, and valid dual-OTA partition tables (`partitions_ota.csv`) with rollback support.
- **Axis 5 — Maintainability, Testing & CI:**
  - Verify that `.github/workflows/ci.yml` executes `clang-format --dry-run --Werror`, `uvx ruff check tests/ scripts/`, `uv run pytest`, and builds both `sdkconfig.defaults` and `sdkconfig.virtual_bench`.

### 2.3 Known Design Decisions (Do Not Flag)
- **`CONFIG_OBD2_VIRTUAL_BENCH` Pin & Bus Isolation:** Disabling physical CAN/K-Line/ADC/LED GPIOs and routing `elm327_emu` to the in-memory `ecu_simulator` when `CONFIG_OBD2_VIRTUAL_BENCH=y` is intentional to avoid GPIO conflicts with the LilyGO T-Display S3 8-bit parallel ST7789 LCD.
- **Pure-C Host Unit Testing via `ctypes`:** Compiling pure-C protocol components (`isotp`, `kline_proto`, `obd2_codec`, `kwp2000`, `elm327_emu`, `ecu_sim_core`) on the host via `gcc`/`ziglang` + `pytest` without ESP-IDF mocks is an intentional fast-feedback architecture.

---

## 3. Mandatory Criticality Classification

Observations must be strictly categorized into 3 severity levels:

### 🔴 1. Blocking / Critical (*High / Critical*)
*Issues that prevent operation, break core invariants, or introduce severe safety/security/data-loss risks.*

### 🟠 2. Important (*Medium*)
*Issues that compromise robustness, reliability, edge-case handling, performance, or compliance.*

### 🟢 3. Minor / Suggestion (*Low / Info*)
*Opportunities to improve clarity, cost, ergonomics, testability, or maintainability.*

---

## 4. Expectations for High-Quality Feedback

For observations to be directly actionable, each remark must provide:

1. **Pinpoint Precision:** Cite exact file paths, line ranges, symbols, modules, or component/net identifiers.
2. **Technical Demonstration (Why?):** Support analysis with concrete evidence, calculations, traces, specs, or standards.
3. **Turnkey Actionable Solution (How to fix?):** Propose a clear, concrete remediation step without speculative placeholders.

---

## 5. Review Template (`reviewXXX.md`)

Copy and complete the template below in your `review/reviewXXX.md` file:

```markdown
# Technical Review: [Short title of your audit]

- **Reviewer / Agent Name:** [Your name / AI model]
- **Date:** [YYYY-MM-DD]
- **Audited Scope:** [Module / Subsystem / Architecture]
- **Examined Reference / Version:** [Git commit hash or document version]
- **Overall Verdict:** [🟢 Approved / 🟠 Approved with Reservations / 🔴 Rejected]

---

## Executive Summary
[3 to 5 sentence summary on overall health, compliance, and key priorities.]

---

## Observations Summary

| Ref | Severity | Target / Scope | Summary Description |
| :--- | :--- | :--- | :--- |
| **B1** | 🔴 Blocking | `...` | ... |
| **I1** | 🟠 Important | `...` | ... |
| **M1** | 🟢 Minor | `...` | ... |

---

## Detailed Observations & Recommendations

### 🔴 Blocking Observations (Critical)

#### B1. [Descriptive Title]
- **Affected Targets / Scope:** `...`
- **Technical Observation:** [Detailed problem description]
- **Justification & Risk:** [Evidence, calculation, spec, or standard demonstrating hazard]
- **Recommended Solution:** [Concrete actionable fix]

---

### 🟠 Important Observations (Robustness & Reliability)

#### I1. [Recommendation Title]
- **Affected Targets / Scope:** `...`
- **Technical Observation:** [Description]
- **Justification:** [Impact on reliability, edge cases, or compliance]
- **Recommended Solution:** [Recommended change]

---

### 🟢 Minor Observations (Optimization & Polish)

#### M1. [Optimization Title]
- **Affected Targets / Scope:** `...`
- **Observation & Proposal:** [Concrete improvement suggestion]
- **Expected Benefit:** [Maintainability, clarity, cost, or testability gain]
```
