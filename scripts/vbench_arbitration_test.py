"""Multi-transport lock arbitration test (simultaneous BLE + Wi-Fi TCP) for LilyGO VIRTUAL_BENCH."""

import argparse
import asyncio
import socket
import sys

from bleak import BleakClient, BleakScanner

UUID_FFF1 = "0000fff1-0000-1000-8000-00805f9b34fb"
UUID_FFF2 = "0000fff2-0000-1000-8000-00805f9b34fb"


async def run_arbitration(ble_name: str, wifi_host: str, wifi_port: int) -> None:
    print(f"[1/3] Scanning for BLE '{ble_name}'...")
    dev = await BleakScanner.find_device_by_name(ble_name, timeout=10.0)
    if dev is None:
        raise RuntimeError(f"BLE device '{ble_name}' not found")

    rx_buf = bytearray()
    prompt_evt = asyncio.Event()

    def on_notify(_sender: object, data: bytearray) -> None:
        rx_buf.extend(data)
        if b">" in rx_buf:
            prompt_evt.set()

    async def send_ble(client: BleakClient, cmd: str, timeout: float = 2.0) -> str:
        rx_buf.clear()
        prompt_evt.clear()
        await client.write_gatt_char(UUID_FFF2, (cmd + "\r").encode("ascii"), response=False)
        await asyncio.wait_for(prompt_evt.wait(), timeout=timeout)
        return rx_buf.decode("ascii", errors="replace")

    print("[2/3] BLE acquires lock -> Wi-Fi must be blocked until BLE disconnects...")
    async with BleakClient(dev) as ble:
        await ble.start_notify(UUID_FFF1, on_notify)
        assert "ELM327" in await send_ble(ble, "ATI")

        with socket.create_connection((wifi_host, wifi_port), timeout=3.0) as tcp:
            tcp.settimeout(0.8)
            tcp.sendall(b"ATI\r")
            try:
                blocked = tcp.recv(256)
                assert blocked == b"", f"Expected Wi-Fi blocked while BLE active, got {blocked!r}"
            except TimeoutError:
                pass
            print("  Wi-Fi blocked while BLE holds lock: OK")
        await ble.stop_notify(UUID_FFF1)

    # Windows BLE stack may take 1-3s after BleakClient exits to close the ACL link.
    # Retry Wi-Fi ATI until BLE_GAP_EVENT_DISCONNECT releases TRANSPORT_ID_BLE.
    resp = b""
    tcp = None
    for attempt in range(10):
        await asyncio.sleep(0.5)
        sock = socket.create_connection((wifi_host, wifi_port), timeout=3.0)
        sock.settimeout(0.8)
        sock.sendall(b"ATI\r")
        try:
            resp = sock.recv(256)
            if b"ELM327" in resp:
                tcp = sock
                break
        except TimeoutError:
            pass
        sock.close()

    assert tcp is not None and b"ELM327" in resp, f"Wi-Fi failed after BLE release: {resp!r}"
    try:
        print(f"  Wi-Fi acquired lock after BLE disconnect (attempt {attempt + 1}): OK")

        print("[3/3] Wi-Fi holds lock -> BLE must be blocked until Wi-Fi disconnects...")
        async with BleakClient(dev) as ble:
            await ble.start_notify(UUID_FFF1, on_notify)
            try:
                await send_ble(ble, "ATI", timeout=0.8)
                raise AssertionError("BLE should have been blocked while Wi-Fi holds lock")
            except TimeoutError:
                print("  BLE blocked while Wi-Fi holds lock: OK")
            await ble.stop_notify(UUID_FFF1)
    finally:
        tcp.close()

    await asyncio.sleep(0.2)
    async with BleakClient(dev) as ble:
        await ble.start_notify(UUID_FFF1, on_notify)
        assert "ELM327" in await send_ble(ble, "ATI")
        await ble.stop_notify(UUID_FFF1)
    print("  BLE acquired lock after Wi-Fi disconnect: OK — ARBITRATION PASSED!")


def main() -> int:
    parser = argparse.ArgumentParser(description="OBD2 multi-transport arbitration test")
    parser.add_argument("--ble-name", default="OBD2-ELM327")
    parser.add_argument("--host", default="192.168.0.10")
    parser.add_argument("--port", type=int, default=35000)
    args = parser.parse_args()
    asyncio.run(run_arbitration(args.ble_name, args.host, args.port))
    return 0


if __name__ == "__main__":
    sys.exit(main())
