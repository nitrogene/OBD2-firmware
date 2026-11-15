"""Offline & CSV analyzer for 8-channel logic analyzer captures (K-Line & Fast-Init)."""

from __future__ import annotations

import argparse
import csv
import tempfile
from pathlib import Path


def load_csv_channels(path: Path) -> tuple[list[str], list[list[int]]]:
    """Load sigrok-cli CSV export (skipping ';' metadata lines)."""
    headers: list[str] = []
    rows: list[list[int]] = []
    with path.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.reader(line for line in handle if not line.startswith(";"))
        for row in reader:
            if not row:
                continue
            if not headers:
                headers = [col.strip() for col in row]
                continue
            rows.append([int(val.strip()) for val in row])
    return headers, rows


def measure_first_low_pulse_ms(samples: list[int], sample_rate_hz: int) -> float | None:
    """Measure the duration in milliseconds of the first falling-to-rising LOW pulse."""
    start_idx: int | None = None
    for idx in range(1, len(samples)):
        if samples[idx - 1] == 1 and samples[idx] == 0 and start_idx is None:
            start_idx = idx
        elif samples[idx - 1] == 0 and samples[idx] == 1 and start_idx is not None:
            return (idx - start_idx) * 1000.0 / sample_rate_hz
    return None


def decode_uart_8n1(samples: list[int], sample_rate_hz: int, baud: int = 10400) -> list[int]:
    """Decode 8N1 UART bytes from a 1-channel logic sample list."""
    bit_samples = sample_rate_hz / baud
    decoded: list[int] = []
    idx = 1
    n = len(samples)
    while idx < n:
        if samples[idx - 1] == 1 and samples[idx] == 0:
            # Verify start bit center is still LOW
            start_center = int(idx + 0.5 * bit_samples)
            if start_center >= n or samples[start_center] != 0:
                idx += 1
                continue
            byte_val = 0
            for bit_pos in range(8):
                sample_pos = int(idx + (1.5 + bit_pos) * bit_samples)
                if sample_pos >= n:
                    return decoded
                byte_val |= (samples[sample_pos] & 1) << bit_pos
            decoded.append(byte_val)
            idx = int(idx + 9.5 * bit_samples)
        else:
            idx += 1
    return decoded


def run_self_test() -> int:
    """Generate synthetic 1 MHz captures and verify pulse + UART echo decoding."""
    rate = 1_000_000  # 1 MHz (1 us/sample)
    with tempfile.TemporaryDirectory() as tmpdir:
        tmp = Path(tmpdir)

        # 1. Synthetic 25.0 ms Fast-Init pulse
        pulse_csv = tmp / "pulse.csv"
        with pulse_csv.open("w", encoding="utf-8", newline="") as handle:
            handle.write("; Synthetic 25ms Fast-Init pulse\nKLINE_TX,KLINE_RX\n")
            for _ in range(5_000):
                handle.write("1,1\n")
            for _ in range(25_000):
                handle.write("0,0\n")
            for _ in range(5_000):
                handle.write("1,1\n")

        headers, rows = load_csv_channels(pulse_csv)
        tx_col = [r[0] for r in rows]
        pulse_ms = measure_first_low_pulse_ms(tx_col, rate)
        assert pulse_ms is not None and 24.0 <= pulse_ms <= 26.0, f"Bad pulse: {pulse_ms}"

        # 2. Synthetic 10,400 baud UART TX + RX echo (0x81 0x10 0xF1 0x81)
        expected = [0x81, 0x10, 0xF1, 0x81]
        uart_csv = tmp / "uart.csv"
        bit_us = int(round(rate / 10400))  # 96 us
        with uart_csv.open("w", encoding="utf-8", newline="") as handle:
            handle.write("; Synthetic 10400 baud 8N1\nKLINE_TX,KLINE_RX\n")
            for _ in range(200):
                handle.write("1,1\n")
            for val in expected:
                bits = [0] + [(val >> b) & 1 for b in range(8)] + [1]
                for bit in bits:
                    for _ in range(bit_us):
                        handle.write(f"{bit},{bit}\n")
                for _ in range(100):
                    handle.write("1,1\n")

        _, uart_rows = load_csv_channels(uart_csv)
        tx_bytes = decode_uart_8n1([r[0] for r in uart_rows], rate, 10400)
        rx_bytes = decode_uart_8n1([r[1] for r in uart_rows], rate, 10400)
        assert tx_bytes == expected and rx_bytes == expected, f"UART mismatch: {tx_bytes} vs {rx_bytes}"

    print(f"SELF-TEST OK: pulse={pulse_ms:.2f} ms, uart={[hex(b) for b in tx_bytes]}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Analyze sigrok-cli CSV captures for OBD2 bring-up.")
    parser.add_argument("--self-test", action="store_true", help="Run offline synthetic self-test.")
    parser.add_argument("--mode", choices=["kline-pulse", "kline-uart"], help="Analysis mode.")
    parser.add_argument("--file", type=Path, help="Path to sigrok-cli CSV file.")
    parser.add_argument("--rate", type=int, default=1_000_000, help="Sample rate in Hz.")
    args = parser.parse_args()

    if args.self_test:
        return run_self_test()

    if not args.mode or not args.file:
        parser.error("--mode and --file are required unless --self-test is set.")

    headers, rows = load_csv_channels(args.file)
    if args.mode == "kline-pulse":
        pulse_ms = measure_first_low_pulse_ms([r[0] for r in rows], args.rate)
        if pulse_ms is None:
            print("No LOW pulse detected.")
            return 1
        ok = 24.0 <= pulse_ms <= 26.0
        print(f"Measured pulse ({headers[0]}): {pulse_ms:.2f} ms -> {'OK (25+-1 ms)' if ok else 'OUT OF SPEC'}")
        return 0 if ok else 1

    tx_bytes = decode_uart_8n1([r[0] for r in rows], args.rate, 10400)
    rx_bytes = decode_uart_8n1([r[1] for r in rows], args.rate, 10400) if len(headers) > 1 else []
    print("TX:", " ".join(f"{b:02X}" for b in tx_bytes))
    if rx_bytes:
        print("RX:", " ".join(f"{b:02X}" for b in rx_bytes))
        print("ECHO:", "OK" if tx_bytes == rx_bytes else "MISMATCH")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
