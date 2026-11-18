"""Web Config (GET /) and wireless OTA (POST /ota) validator for LilyGO VIRTUAL_BENCH."""

import argparse
import sys
import urllib.request
from pathlib import Path


def check_web_config(base_url: str) -> None:
    print(f"[1/2] Checking Web Config page at {base_url}/ ...")
    with urllib.request.urlopen(f"{base_url}/", timeout=5.0) as resp:
        assert resp.status == 200, f"Unexpected HTTP status: {resp.status}"
        html = resp.read().decode("utf-8", errors="replace")
        assert "OBD2-Scanner (ELM327)" in html, "Missing expected title in HTML"
        assert "action='/ota'" in html, "Missing OTA form in HTML"
        assert "action='/config'" in html, "Missing config form in HTML"
    print("  Web Config HTML OK")


def upload_ota(base_url: str, bin_path: Path) -> None:
    data = bin_path.read_bytes()
    print(f"[2/2] Uploading OTA firmware {bin_path} ({len(data)} bytes) to {base_url}/ota ...")
    req = urllib.request.Request(
        f"{base_url}/ota",
        data=data,
        headers={"Content-Type": "application/octet-stream"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=60.0) as resp:
        body = resp.read().decode("utf-8", errors="replace")
        assert resp.status == 200 and "OTA OK" in body, f"OTA failed: {resp.status} {body!r}"
        print(f"  OTA response: {body.strip()}")


def main() -> int:
    parser = argparse.ArgumentParser(description="OBD2 Web Config & OTA validator")
    parser.add_argument("--url", default="http://192.168.42.1", help="Base URL (default: http://192.168.42.1)")
    parser.add_argument("--ota", type=Path, default=None, help="Optional .bin file to flash over OTA")
    args = parser.parse_args()

    check_web_config(args.url)
    if args.ota is not None:
        upload_ota(args.url, args.ota)
    else:
        print("[2/2] Skipping /ota upload (pass --ota build_vbench/obd2_firmware.bin to flash wirelessly)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
