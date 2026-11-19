"""Hardware-In-the-Loop (HIL) regression suite for LilyGO T-Display S3 VIRTUAL_BENCH.

Run manually on a developer PC with the LilyGO attached (never executed in CI):
    uv run --with pyserial --with bleak pytest tests_hil/ -v
"""

import asyncio
import socket
import time
import urllib.error
import urllib.request

import pytest
import serial
import serial.tools.list_ports
from bleak import BleakClient, BleakScanner

UUID_FFF0 = "0000fff0-0000-1000-8000-00805f9b34fb"
UUID_FFE0 = "0000ffe0-0000-1000-8000-00805f9b34fb"
UUID_FFF1 = "0000fff1-0000-1000-8000-00805f9b34fb"
UUID_FFF2 = "0000fff2-0000-1000-8000-00805f9b34fb"
UUID_FFE1 = "0000ffe1-0000-1000-8000-00805f9b34fb"


def _find_esp32s3_port() -> str | None:
    for port in serial.tools.list_ports.comports():
        if port.vid == 0x303A and port.pid == 0x1001:
            return port.device
    return None


def _usb_cmd(ser: serial.Serial, cmd: str, wait_s: float = 0.35) -> str:
    ser.reset_input_buffer()
    ser.write((cmd + "\r\n").encode("ascii"))
    time.sleep(wait_s)
    return ser.read_all().decode("ascii", errors="replace")


def test_hil_usb_cdc_cli() -> None:
    """Verify USB-C CDC CLI commands and AT passthrough on the attached LilyGO."""
    port = _find_esp32s3_port()
    if port is None:
        pytest.skip("No ESP32-S3 USB-C CDC port (VID=303A PID=1001) attached")

    ser = serial.Serial()
    ser.port = port
    ser.baudrate = 115200
    ser.timeout = 1.0
    ser.dtr = False
    ser.rts = False
    ser.open()
    try:
        time.sleep(0.2)
        assert "ESP32-S3" in _usb_cmd(ser, "info")
        assert "App Version :" in _usb_cmd(ser, "info")
        assert "V_BAT" in _usb_cmd(ser, "vbat")
        assert "ECU Sim:" in _usb_cmd(ser, "sim status")
        assert "ELM327 v1.5" in _usb_cmd(ser, "at ATI")
        assert "OBD2-fw" in _usb_cmd(ser, "at AT+VER")
        assert "OK" in _usb_cmd(ser, "at ATD")
        assert "OK" in _usb_cmd(ser, "at ATE0")
        assert "V" in _usb_cmd(ser, "at ATRV")
        assert "41 0C" in _usb_cmd(ser, "at 010C")
        assert "41 20" in _usb_cmd(ser, "at 0120")
        assert "41 40" in _usb_cmd(ser, "at 0140")
    finally:
        ser.close()


def test_hil_ble_advertising_and_car_scanner_sequence() -> None:
    """Verify BLE service UUID advertising and replay the Car Scanner init/polling sequence."""

    async def _run() -> None:
        devs = await BleakScanner.discover(timeout=6.0, return_adv=True)
        match = next(
            ((d, adv) for d, adv in devs.values() if d.name == "OBD2-ELM327"),
            None,
        )
        if match is None:
            pytest.skip("BLE device 'OBD2-ELM327' not found")

        dev, adv = match
        adv_uuids = {u.lower() for u in adv.service_uuids}
        assert UUID_FFF0 in adv_uuids, f"Missing 0xFFF0 in BLE adv: {adv.service_uuids}"
        assert UUID_FFE0 in adv_uuids, f"Missing 0xFFE0 in BLE adv: {adv.service_uuids}"

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

        async with BleakClient(dev) as client:
            char_uuids = {c.uuid.lower() for s in client.services for c in s.characteristics}
            assert {UUID_FFF1, UUID_FFF2, UUID_FFE1}.issubset(char_uuids)

            await client.start_notify(UUID_FFF1, on_notify)
            assert "ELM327 v1.5" in await send_elm(client, "ATZ")
            for at_cmd in ("ATE0", "ATD0", "ATH1", "ATSP0", "ATM0", "ATS0", "ATAT1", "ATAL", "ATST64"):
                assert "OK" in await send_elm(client, at_cmd), f"{at_cmd} failed"

            assert "7E8064100381A9001" in await send_elm(client, "0100")
            assert "7E806412000000001" in await send_elm(client, "0120")
            assert "7E806414040000000" in await send_elm(client, "0140")
            assert "7E804410C" in await send_elm(client, "010C")
            assert "7E803410D" in await send_elm(client, "010D")
            assert "014\r0:490201" in await send_elm(client, "0902")

            assert "OK" in await send_elm(client, "AT+SIM:DTC=P0301")
            assert "0301" in await send_elm(client, "03")
            assert "7E80144" in await send_elm(client, "04")
            assert "7E8024300" in await send_elm(client, "03")

            assert "EXT OK" in await send_elm(client, "AT+EXT=1")
            assert "62010C" in await send_elm(client, "22010C")
            assert "OK" in await send_elm(client, "AT+EXT=0")
            await client.stop_notify(UUID_FFF1)

    asyncio.run(_run())


def test_hil_wifi_tcp_and_http_if_connected() -> None:
    """Verify Wi-Fi TCP :35000 and HTTP auth/XSS protections when connected to OBD2-Scanner."""
    host = "192.168.42.1"
    try:
        sock = socket.create_connection((host, 35000), timeout=1.0)
    except OSError:
        pytest.skip("Host PC is not connected to OBD2-Scanner SoftAP (192.168.42.1)")

    try:
        sock.settimeout(2.0)
        sock.sendall(b"ATZ\rATE0\r0100\r")
        time.sleep(0.2)
        resp = sock.recv(512).decode("ascii", errors="replace")
        assert "ELM327 v1.5" in resp
        assert "41 00" in resp
    finally:
        sock.close()

    with urllib.request.urlopen(f"http://{host}/", timeout=3.0) as r:
        html = r.read().decode("utf-8", errors="replace")
        assert r.status == 200
        assert "OBD2-Scanner (ELM327)" in html
        assert "12345678" not in html

    req = urllib.request.Request(
        f"http://{host}/config",
        data=b"ble_name=Test",
        method="POST",
    )
    with pytest.raises(urllib.error.HTTPError) as exc_info:
        urllib.request.urlopen(req, timeout=3.0)
    assert exc_info.value.code == 401
