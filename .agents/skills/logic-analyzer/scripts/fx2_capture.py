"""100% Python USB capture driver for FX2LP (fx2lafw) 8-channel logic analyzers."""

from __future__ import annotations

import argparse
import io
import struct
import tarfile
import tempfile
import time
import urllib.request
from pathlib import Path

import libusb
import usb.backend.libusb1 as usb_libusb1
import usb.core

# Common VID:PID pairs for 24 MHz 8-ch FX2LP analyzers (Saleae clones / Cypress / sigrok)
KNOWN_FX2_IDS: list[tuple[int, int, str]] = [
    (0x0925, 0x3881, "Saleae Logic / FX2LP 8-ch Clone"),
    (0x04B4, 0x8613, "Cypress CY7C68013A FX2LP (blank)"),
    (0x1D50, 0x608C, "sigrok fx2lafw"),
    (0x21A9, 0x1001, "Saleae Logic 8"),
]

FX2LAFW_TAR_URL = (
    "https://sigrok.org/download/binary/sigrok-firmware-fx2lafw/"
    "sigrok-firmware-fx2lafw-bin-0.1.7.tar.gz"
)
DEFAULT_FW_NAME = "fx2lafw-saleae-logic.fw"
DEFAULT_FW_DIR = Path(__file__).resolve().parent.parent / "firmware"

# fx2lafw vendor commands
CMD_GET_FW_VERSION = 0xB0
CMD_START = 0xB1

# Cypress FX2LP RAM upload command & CPUCS register
FX2_ANCHOR_LOAD = 0xA0
FX2_CPUCS_ADDR = 0xE600
EP_DATA_IN = 0x82


def get_usb_backend():
    """Return a pyusb libusb1 backend using the bundled libusb-1.0 DLL."""
    return usb_libusb1.get_backend(find_library=lambda _: libusb.dll._name)


def compute_fx2_rate_config(sample_rate_hz: int) -> tuple[int, int]:
    """Compute fx2lafw (flags, delay) for CMD_START.

    fx2lafw uses either 48 MHz (flag bit 6 = 0x40) or 30 MHz (flag = 0x00) base clock.
    """
    if sample_rate_hz <= 0:
        raise ValueError("Sample rate must be positive")
    if 48_000_000 % sample_rate_hz == 0:
        div = 48_000_000 // sample_rate_hz
        if 2 <= div <= 256:
            return 0x40, div - 1
    if 30_000_000 % sample_rate_hz == 0:
        div = 30_000_000 // sample_rate_hz
        if 2 <= div <= 256:
            return 0x00, div - 1
    raise ValueError(f"Unsupported sample rate {sample_rate_hz} Hz for fx2lafw")


def parse_intel_hex(ihx_text: str) -> list[tuple[int, bytes]]:
    """Parse Intel HEX firmware into (address, data) chunks."""
    chunks: list[tuple[int, bytes]] = []
    for raw_line in ihx_text.splitlines():
        line = raw_line.strip()
        if not line or not line.startswith(":"):
            continue
        byte_count = int(line[1:3], 16)
        addr = int(line[3:7], 16)
        rec_type = int(line[7:9], 16)
        if rec_type == 0x00:
            payload = bytes.fromhex(line[9 : 9 + byte_count * 2])
            chunks.append((addr, payload))
        elif rec_type == 0x01:
            break
    return chunks


def ensure_fx2lafw_firmware(fw_dir: Path = DEFAULT_FW_DIR, fw_name: str = DEFAULT_FW_NAME) -> Path:
    """Return cached fx2lafw .fw path, downloading from sigrok.org if missing."""
    fw_path = fw_dir / fw_name
    if fw_path.is_file() and fw_path.stat().st_size > 0:
        return fw_path

    fw_dir.mkdir(parents=True, exist_ok=True)
    print(f"Downloading {fw_name} from {FX2LAFW_TAR_URL}...")
    with urllib.request.urlopen(FX2LAFW_TAR_URL, timeout=20) as resp:
        tar_bytes = resp.read()

    with tarfile.open(fileobj=io.BytesIO(tar_bytes), mode="r:gz") as tar:
        for member in tar.getmembers():
            if member.isfile() and member.name.endswith(fw_name):
                extracted = tar.extractfile(member)
                if extracted is None:
                    break
                fw_path.write_bytes(extracted.read())
                print(f"Cached firmware at {fw_path} ({fw_path.stat().st_size} bytes)")
                return fw_path

    raise FileNotFoundError(f"{fw_name} not found inside {FX2LAFW_TAR_URL}")


def upload_fx2_firmware(dev: usb.core.Device, fw_path: Path) -> None:
    """Upload Intel HEX (.ihx) or raw binary (.fw) firmware to FX2LP RAM via 0xA0."""
    raw_bytes = fw_path.read_bytes()
    if fw_path.suffix.lower() == ".ihx" or raw_bytes.lstrip().startswith(b":"):
        chunks = parse_intel_hex(raw_bytes.decode("utf-8"))
    else:
        # Flat binary image loaded starting at RAM address 0x0000 in <=64-byte chunks
        chunks = [(offset, raw_bytes[offset : offset + 64]) for offset in range(0, len(raw_bytes), 64)]

    if not chunks:
        raise ValueError(f"Empty firmware image: {fw_path}")

    # Hold 8051 CPU in reset, load RAM, then release reset
    dev.ctrl_transfer(0x40, FX2_ANCHOR_LOAD, FX2_CPUCS_ADDR, 0, b"\x01")
    for addr, data in chunks:
        dev.ctrl_transfer(0x40, FX2_ANCHOR_LOAD, addr, 0, data)
    dev.ctrl_transfer(0x40, FX2_ANCHOR_LOAD, FX2_CPUCS_ADDR, 0, b"\x00")
    time.sleep(1.0)


def scan_fx2_devices() -> list[tuple[usb.core.Device, str]]:
    """Find connected FX2LP devices matching KNOWN_FX2_IDS."""
    backend = get_usb_backend()
    found: list[tuple[usb.core.Device, str]] = []
    for vid, pid, desc in KNOWN_FX2_IDS:
        devs = list(usb.core.find(find_all=True, idVendor=vid, idProduct=pid, backend=backend) or [])
        for dev in devs:
            found.append((dev, desc))
    return found


def export_raw_to_csv(raw_bytes: bytes, out_csv: Path, channel_names: list[str]) -> int:
    """Write raw 8-channel FX2LP samples (1 byte/sample) to CSV."""
    if len(channel_names) > 8:
        raise ValueError("At most 8 channels supported")
    with out_csv.open("w", encoding="utf-8", newline="") as handle:
        handle.write(",".join(channel_names) + "\n")
        num_ch = len(channel_names)
        for sample in raw_bytes:
            bits = [str((sample >> ch) & 1) for ch in range(num_ch)]
            handle.write(",".join(bits) + "\n")
    return len(raw_bytes)


def _query_fx2lafw_version(dev: usb.core.Device) -> tuple[int, int] | None:
    """Return (major, minor) if fx2lafw is active, else None."""
    try:
        ver = dev.ctrl_transfer(0xC0, CMD_GET_FW_VERSION, 0, 0, 2, timeout=400)
        return int(ver[0]), int(ver[1])
    except usb.core.USBError:
        return None


def capture_fx2_csv(
    out_csv: Path,
    sample_rate_hz: int,
    duration_s: float,
    channel_names: list[str],
    fw_path: Path | None = None,
) -> int:
    """Capture digital samples from the first detected fx2lafw device into CSV."""
    devices = scan_fx2_devices()
    if not devices:
        raise RuntimeError("No FX2LP logic analyzer found. Check USB cable and WinUSB driver.")

    dev, desc = devices[0]
    print(f"Using USB device: {desc} ({dev.idVendor:04x}:{dev.idProduct:04x})")
    dev.set_configuration()

    ver = _query_fx2lafw_version(dev)
    if ver is None:
        resolved_fw = fw_path if fw_path is not None else ensure_fx2lafw_firmware()
        print(f"Uploading {resolved_fw.name} to FX2LP RAM...")
        upload_fx2_firmware(dev, resolved_fw)

        # Wait for USB re-enumeration after CPUCS reset release
        for _ in range(10):
            devices = scan_fx2_devices()
            if devices:
                dev, _ = devices[0]
                try:
                    dev.set_configuration()
                    ver = _query_fx2lafw_version(dev)
                    if ver is not None:
                        break
                except usb.core.USBError:
                    pass
            time.sleep(0.3)

    if ver is None:
        raise RuntimeError("fx2lafw did not respond after firmware upload.")
    print(f"fx2lafw active: v{ver[0]}.{ver[1]}")

    flags, delay = compute_fx2_rate_config(sample_rate_hz)
    total_samples = int(sample_rate_hz * duration_s)
    read_len = ((total_samples + 511) // 512) * 512

    cmd_payload = struct.pack("<BBB", flags, (delay >> 8) & 0xFF, delay & 0xFF)
    dev.ctrl_transfer(0x40, CMD_START, 0, 0, cmd_payload, timeout=1000)

    chunks: list[bytes] = []
    remaining = read_len
    while remaining > 0:
        chunk_size = min(remaining, 16384)
        data = dev.read(EP_DATA_IN, chunk_size, timeout=max(2000, int(duration_s * 2000)))
        chunks.append(bytes(data))
        remaining -= len(data)

    raw = b"".join(chunks)[:total_samples]
    return export_raw_to_csv(raw, out_csv, channel_names)


def run_self_test() -> int:
    """Offline self-test for USB backend, rate math, Intel HEX parser, and CSV export."""
    backend = get_usb_backend()
    assert backend is not None, "libusb backend unavailable"

    assert compute_fx2_rate_config(1_000_000) == (0x40, 47)
    assert compute_fx2_rate_config(8_000_000) == (0x40, 5)
    assert compute_fx2_rate_config(12_000_000) == (0x40, 3)

    ihx = ":03000000020010EB\n:00000001FF\n"
    parsed = parse_intel_hex(ihx)
    assert parsed == [(0x0000, b"\x02\x00\x10")]

    with tempfile.TemporaryDirectory() as tmpdir:
        out = Path(tmpdir) / "test.csv"
        count = export_raw_to_csv(bytes([0b00000101, 0b00000010]), out, ["D0", "D1", "D2"])
        assert count == 2
        lines = out.read_text(encoding="utf-8").splitlines()
        assert lines == ["D0,D1,D2", "1,0,1", "0,1,0"]

    print("FX2 CAPTURE SELF-TEST OK (libusb backend ready, rate/ihx/csv verified)")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="100% Python FX2LP USB logic analyzer capture.")
    parser.add_argument("--self-test", action="store_true", help="Run offline self-test.")
    parser.add_argument("--scan", action="store_true", help="Scan USB for FX2LP devices.")
    parser.add_argument("--out", type=Path, help="Output CSV path.")
    parser.add_argument("--rate", type=int, default=1_000_000, help="Sample rate in Hz.")
    parser.add_argument("--time", type=float, default=0.5, help="Capture duration in seconds.")
    parser.add_argument("--channels", default="D0,D1,D2,D3", help="Comma-separated channel names.")
    parser.add_argument("--fw", type=Path, help="Optional fx2lafw .fw/.ihx firmware to upload.")
    args = parser.parse_args()

    if args.self_test:
        return run_self_test()

    if args.scan:
        found = scan_fx2_devices()
        if not found:
            print("No FX2LP logic analyzer detected.")
            return 1
        for dev, desc in found:
            print(f"Found: {desc} (VID=0x{dev.idVendor:04x}, PID=0x{dev.idProduct:04x})")
        return 0

    if not args.out:
        parser.error("--out is required for capture.")

    ch_names = [c.strip() for c in args.channels.split(",") if c.strip()]
    count = capture_fx2_csv(args.out, args.rate, args.time, ch_names, args.fw)
    print(f"Captured {count} samples at {args.rate} Hz -> {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
