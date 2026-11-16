"""Black-box BLE GATT (FFF0/FFE0) validation script for LilyGO VIRTUAL_BENCH."""

import argparse
import asyncio
import sys

from bleak import BleakClient, BleakScanner

UUID_FFF1 = "0000fff1-0000-1000-8000-00805f9b34fb"
UUID_FFF2 = "0000fff2-0000-1000-8000-00805f9b34fb"
UUID_FFE1 = "0000ffe1-0000-1000-8000-00805f9b34fb"


async def run_ble_suite(device_name: str, timeout: float) -> None:
    print(f"[1/4] Scanning for BLE device '{device_name}' ({timeout:.0f}s)...")
    dev = await BleakScanner.find_device_by_name(device_name, timeout=timeout)
    if dev is None:
        raise RuntimeError(f"BLE device '{device_name}' not found")
    print(f"  Found: {dev.name} ({dev.address})")

    rx_buf = bytearray()
    prompt_evt = asyncio.Event()

    def on_notify(_sender: object, data: bytearray) -> None:
        rx_buf.extend(data)
        if b">" in rx_buf:
            prompt_evt.set()

    async def send_elm(client: BleakClient, cmd: str) -> str:
        rx_buf.clear()
        prompt_evt.clear()
        await client.write_gatt_char(UUID_FFF2, (cmd + "\r").encode("ascii"), response=False)
        await asyncio.wait_for(prompt_evt.wait(), timeout=3.0)
        return rx_buf.decode("ascii", errors="replace")

    print("[2/4] Connecting & verifying FFF0 + FFE0 GATT characteristics...")
    async with BleakClient(dev) as client:
        char_uuids = {c.uuid.lower() for s in client.services for c in s.characteristics}
        assert UUID_FFF1 in char_uuids and UUID_FFF2 in char_uuids, "Missing FFF1/FFF2"
        assert UUID_FFE1 in char_uuids, "Missing FFE1 alias"

        await client.start_notify(UUID_FFF1, on_notify)
        assert "ELM327 v1.5" in await send_elm(client, "ATZ")
        assert "OK" in await send_elm(client, "ATE0")
        assert "V" in await send_elm(client, "ATRV")
        assert "OK" in await send_elm(client, "ATSP6")
        print("  AT basics OK")

        print("[3/4] Testing Mode 01, 0902 VIN, and Extended UDS over BLE...")
        assert (await send_elm(client, "010C")).startswith("41 0C")
        assert (await send_elm(client, "010D")).startswith("41 0D")
        vin_resp = await send_elm(client, "0902")
        assert "014\r0:49 02 01" in vin_resp, f"0902 unexpected: {vin_resp!r}"

        assert "EXT OK" in await send_elm(client, "AT+EXT=1")
        assert "62 01 0C" in await send_elm(client, "22010C")
        assert "OK" in await send_elm(client, "AT+EXT=0")
        await client.stop_notify(UUID_FFF1)
        print("  OBD + UDS over BLE OK")

    print("[4/4] Verifying clean disconnect & reconnect...")
    await asyncio.sleep(0.3)
    async with BleakClient(dev) as client:
        await client.start_notify(UUID_FFF1, on_notify)
        assert "ELM327 v1.5" in await send_elm(client, "ATI")
        await client.stop_notify(UUID_FFF1)
    print("  Reconnect OK — ALL BLE CHECKS PASSED!")


def main() -> int:
    parser = argparse.ArgumentParser(description="OBD2 VIRTUAL_BENCH BLE black-box validator")
    parser.add_argument("--name", default="OBD2-ELM327", help="BLE device name")
    parser.add_argument("--timeout", type=float, default=10.0, help="Scan timeout (s)")
    args = parser.parse_args()
    asyncio.run(run_ble_suite(args.name, args.timeout))
    return 0


if __name__ == "__main__":
    sys.exit(main())
