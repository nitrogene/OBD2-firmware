"""100% Python analyzer for 8-channel logic captures (K-Line UART, Fast-Init pulse, CAN)."""

from __future__ import annotations

import argparse
import csv
import tempfile
from dataclasses import dataclass
from pathlib import Path


@dataclass
class CanFrame:
    """Decoded standard (11-bit) CAN 2.0A data frame."""

    can_id: int
    dlc: int
    data: list[int]
    ack_ok: bool


def load_csv_channels(path: Path) -> tuple[list[str], list[list[int]]]:
    """Load CSV export (skipping ';' metadata lines)."""
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


def _can_crc15(bits: list[int]) -> int:
    """Compute CAN 15-bit CRC over unstuffed SOF..data bits."""
    crc = 0
    for bit in bits:
        crc_nxt = bit ^ ((crc >> 14) & 1)
        crc = (crc << 1) & 0x7FFF
        if crc_nxt:
            crc ^= 0x4599
    return crc


def _can_stuff_bits(raw_bits: list[int]) -> list[int]:
    """Apply CAN bit-stuffing (insert opposite bit after 5 identical bits)."""
    stuffed: list[int] = []
    last_bit = -1
    run_len = 0
    for bit in raw_bits:
        stuffed.append(bit)
        if bit == last_bit:
            run_len += 1
        else:
            last_bit = bit
            run_len = 1
        if run_len == 5:
            stuffed.append(1 - bit)
            last_bit = 1 - bit
            run_len = 1
    return stuffed


def decode_can_frames(samples: list[int], sample_rate_hz: int, bitrate: int = 500_000) -> list[CanFrame]:
    """Decode standard 11-bit CAN 2.0A frames from TWAI_TX/TWAI_RX logic samples."""
    bit_samples = sample_rate_hz / bitrate
    frames: list[CanFrame] = []
    n = len(samples)
    idx = 1

    while idx < n:
        # Look for falling edge (SOF: 1 -> 0)
        if not (samples[idx - 1] == 1 and samples[idx] == 0):
            idx += 1
            continue

        # Sample bit stream with CAN bit-destuffing until CRC delimiter
        destuffed: list[int] = []
        raw_pos = 0
        last_bit = 0
        run_len = 1
        destuffed.append(0)  # SOF

        # Read enough bits for standard frame (up to 111 bits + stuffing)
        while len(destuffed) < 120:
            raw_pos += 1
            sample_idx = int(idx + (raw_pos + 0.5) * bit_samples)
            if sample_idx >= n:
                break
            bit = samples[sample_idx] & 1
            if run_len == 5:
                # Stuff bit expected to be inverse of last_bit
                if bit == last_bit:
                    break
                last_bit = bit
                run_len = 1
                continue
            destuffed.append(bit)
            if bit == last_bit:
                run_len += 1
            else:
                last_bit = bit
                run_len = 1

            # Check if we have reached DLC (index 15..18) and full payload + CRC (15) + CRC delim (1)
            if len(destuffed) >= 19:
                dlc = (destuffed[15] << 3) | (destuffed[16] << 2) | (destuffed[17] << 1) | destuffed[18]
                if dlc > 8:
                    break
                expected_len = 19 + dlc * 8 + 15 + 1  # up to CRC delimiter (unstuffed)
                if len(destuffed) == expected_len:
                    # Next two raw bits are ACK slot and ACK delimiter
                    ack_idx = int(idx + (raw_pos + 1.5) * bit_samples)
                    ack_ok = (ack_idx < n) and (samples[ack_idx] == 0)

                    can_id = 0
                    for b in destuffed[1:12]:
                        can_id = (can_id << 1) | b
                    data_bytes: list[int] = []
                    for byte_i in range(dlc):
                        val = 0
                        base = 19 + byte_i * 8
                        for b in destuffed[base : base + 8]:
                            val = (val << 1) | b
                        data_bytes.append(val)
                    frames.append(CanFrame(can_id=can_id, dlc=dlc, data=data_bytes, ack_ok=ack_ok))
                    idx = int(idx + (raw_pos + 10) * bit_samples)
                    break
        else:
            idx += 1
            continue
        idx += 1

    return frames


def _build_synthetic_can_bits(can_id: int, data: list[int], ack: bool = True) -> list[int]:
    """Build a valid stuffed CAN 2.0A bitstream (SOF..EOF) for self-test."""
    bits: list[int] = [0]  # SOF
    bits.extend((can_id >> shift) & 1 for shift in range(10, -1, -1))
    bits.extend([0, 0, 0])  # RTR=0, IDE=0, r0=0
    dlc = len(data)
    bits.extend((dlc >> shift) & 1 for shift in range(3, -1, -1))
    for byte_val in data:
        bits.extend((byte_val >> shift) & 1 for shift in range(7, -1, -1))
    crc = _can_crc15(bits)
    bits.extend((crc >> shift) & 1 for shift in range(14, -1, -1))
    stuffed = _can_stuff_bits(bits)
    stuffed.append(1)  # CRC delimiter
    stuffed.append(0 if ack else 1)  # ACK slot
    stuffed.append(1)  # ACK delimiter
    stuffed.extend([1] * 7)  # EOF
    return stuffed


def run_self_test() -> int:
    """Generate synthetic captures and verify K-Line pulse, UART echo, and CAN 500k."""
    with tempfile.TemporaryDirectory() as tmpdir:
        tmp = Path(tmpdir)

        # 1. Synthetic 25.0 ms Fast-Init pulse (1 MHz)
        pulse_csv = tmp / "pulse.csv"
        with pulse_csv.open("w", encoding="utf-8", newline="") as handle:
            handle.write("KLINE_TX,KLINE_RX\n")
            handle.writelines("1,1\n" for _ in range(5_000))
            handle.writelines("0,0\n" for _ in range(25_000))
            handle.writelines("1,1\n" for _ in range(5_000))

        _, rows = load_csv_channels(pulse_csv)
        pulse_ms = measure_first_low_pulse_ms([r[0] for r in rows], 1_000_000)
        assert pulse_ms is not None and 24.0 <= pulse_ms <= 26.0, f"Bad pulse: {pulse_ms}"

        # 2. Synthetic 10,400 baud UART TX + RX echo (1 MHz)
        expected_uart = [0x81, 0x10, 0xF1, 0x81]
        uart_csv = tmp / "uart.csv"
        bit_us = round(1_000_000 / 10400)
        with uart_csv.open("w", encoding="utf-8", newline="") as handle:
            handle.write("KLINE_TX,KLINE_RX\n")
            handle.writelines("1,1\n" for _ in range(200))
            for val in expected_uart:
                for bit in [0] + [(val >> b) & 1 for b in range(8)] + [1]:
                    handle.writelines(f"{bit},{bit}\n" for _ in range(bit_us))
                handle.writelines("1,1\n" for _ in range(100))

        _, uart_rows = load_csv_channels(uart_csv)
        tx_bytes = decode_uart_8n1([r[0] for r in uart_rows], 1_000_000, 10400)
        rx_bytes = decode_uart_8n1([r[1] for r in uart_rows], 1_000_000, 10400)
        assert tx_bytes == expected_uart and rx_bytes == expected_uart

        # 3. Synthetic 500 kbps CAN 0x7DF frame at 8 MHz (16 samples/bit)
        can_rate = 8_000_000
        spb = can_rate // 500_000
        can_payload = [0x02, 0x01, 0x00, 0x55, 0x55, 0x55, 0x55, 0x55]
        can_bits = [1] * 10 + _build_synthetic_can_bits(0x7DF, can_payload, ack=True)
        can_csv = tmp / "can.csv"
        with can_csv.open("w", encoding="utf-8", newline="") as handle:
            handle.write("TWAI_TX\n")
            for bit in can_bits:
                handle.writelines(f"{bit}\n" for _ in range(spb))

        _, can_rows = load_csv_channels(can_csv)
        frames = decode_can_frames([r[0] for r in can_rows], can_rate, 500_000)
        assert len(frames) == 1, f"Expected 1 CAN frame, got {len(frames)}"
        assert frames[0].can_id == 0x7DF and frames[0].data == can_payload and frames[0].ack_ok

    print(
        f"SELF-TEST OK: pulse={pulse_ms:.2f} ms | "
        f"uart={[hex(b) for b in tx_bytes]} | "
        f"can=0x{frames[0].can_id:03X} ack={frames[0].ack_ok}"
    )
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="100% Python analyzer for OBD2 logic captures.")
    parser.add_argument("--self-test", action="store_true", help="Run offline synthetic self-test.")
    parser.add_argument("--mode", choices=["kline-pulse", "kline-uart", "can"], help="Analysis mode.")
    parser.add_argument("--file", type=Path, help="Path to CSV capture file.")
    parser.add_argument("--rate", type=int, default=1_000_000, help="Sample rate in Hz.")
    parser.add_argument("--bitrate", type=int, default=500_000, help="CAN bitrate (500000 or 250000).")
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

    if args.mode == "kline-uart":
        tx_bytes = decode_uart_8n1([r[0] for r in rows], args.rate, 10400)
        rx_bytes = decode_uart_8n1([r[1] for r in rows], args.rate, 10400) if len(headers) > 1 else []
        print("TX:", " ".join(f"{b:02X}" for b in tx_bytes))
        if rx_bytes:
            print("RX:", " ".join(f"{b:02X}" for b in rx_bytes))
            print("ECHO:", "OK" if tx_bytes == rx_bytes else "MISMATCH")
        return 0

    frames = decode_can_frames([r[0] for r in rows], args.rate, args.bitrate)
    if not frames:
        print("No CAN frames decoded.")
        return 1
    for f in frames:
        data_hex = " ".join(f"{b:02X}" for b in f.data)
        print(f"CAN ID=0x{f.can_id:03X} DLC={f.dlc} ACK={'OK' if f.ack_ok else 'NACK'} DATA=[{data_hex}]")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
