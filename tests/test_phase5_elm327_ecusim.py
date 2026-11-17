"""Host unit tests for pure-C Phase 5 components: elm327_emu.c + ecu_sim_core.c."""

from __future__ import annotations

import ctypes
import platform
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent
ELM_C = REPO_ROOT / "components" / "elm327_emu" / "elm327_emu.c"
ELM_INC = REPO_ROOT / "components" / "elm327_emu" / "include"
ECU_C = REPO_ROOT / "components" / "ecu_simulator" / "ecu_sim_core.c"
ECU_INC = REPO_ROOT / "components" / "ecu_simulator" / "include"
OBD2_C = REPO_ROOT / "components" / "obd2_stack" / "obd2_codec.c"
OBD2_INC = REPO_ROOT / "components" / "obd2_stack" / "include"

WRITE_CB = ctypes.CFUNCTYPE(None, ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t, ctypes.c_void_p)
VBAT_CB = ctypes.CFUNCTYPE(ctypes.c_uint32, ctypes.c_void_p)
OBD_CB = ctypes.CFUNCTYPE(
    ctypes.c_int,
    ctypes.c_uint32,
    ctypes.c_uint8,
    ctypes.c_bool,
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.c_size_t,
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.c_size_t,
    ctypes.POINTER(ctypes.c_size_t),
    ctypes.POINTER(ctypes.c_uint32),
    ctypes.c_void_p,
)
PROTO_CB = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_uint8, ctypes.c_void_p)
FACTORY_CB = ctypes.CFUNCTYPE(None, ctypes.c_void_p)


class ElmCallbacks(ctypes.Structure):
    _fields_ = [
        ("write_bytes", WRITE_CB),
        ("get_vbat_mv", VBAT_CB),
        ("send_obd_req", OBD_CB),
        ("set_protocol", PROTO_CB),
        ("factory_reset", FACTORY_CB),
        ("user_ctx", ctypes.c_void_p),
    ]


class ElmCtx(ctypes.Structure):
    _fields_ = [
        ("cb", ElmCallbacks),
        ("line_buf", ctypes.c_char * 128),
        ("line_len", ctypes.c_size_t),
        ("last_cmd", ctypes.c_char * 128),
        ("echo", ctypes.c_bool),
        ("linefeeds", ctypes.c_bool),
        ("spaces", ctypes.c_bool),
        ("headers", ctypes.c_bool),
        ("auto_format", ctypes.c_bool),
        ("extended_mode", ctypes.c_bool),
        ("sniff_enabled", ctypes.c_bool),
        ("protocol", ctypes.c_uint8),
        ("adaptive_timing", ctypes.c_uint8),
        ("timeout_st", ctypes.c_uint8),
        ("tx_header", ctypes.c_uint32),
        ("rx_filter_id", ctypes.c_uint32),
        ("rx_filter_mask", ctypes.c_uint32),
        ("fc_header", ctypes.c_uint32),
        ("fc_data", ctypes.c_uint8 * 5),
        ("fc_data_len", ctypes.c_uint8),
        ("fc_mode", ctypes.c_uint8),
        ("rx_frames", ctypes.c_uint32),
        ("tx_frames", ctypes.c_uint32),
        ("err_frames", ctypes.c_uint32),
    ]


class EcuSimState(ctypes.Structure):
    _fields_ = [
        ("rpm", ctypes.c_uint16),
        ("speed_kmh", ctypes.c_uint8),
        ("coolant_c", ctypes.c_int16),
        ("load_pct", ctypes.c_uint8),
        ("throttle_pct", ctypes.c_uint8),
        ("vbat_mv", ctypes.c_uint32),
        ("vin", ctypes.c_char * 18),
        ("dtcs", (ctypes.c_uint8 * 2) * 16),
        ("dtc_count", ctypes.c_uint8),
    ]


@pytest.fixture(scope="session")
def phase5_lib(tmp_path_factory: pytest.TempPathFactory) -> ctypes.CDLL:
    build_dir = tmp_path_factory.mktemp("phase5_build")
    ext = ".dll" if platform.system() == "Windows" else ".so"
    out_lib = build_dir / f"libphase5{ext}"

    compiler = [shutil.which("gcc")] if shutil.which("gcc") else [sys.executable, "-m", "ziglang", "cc"]
    cmd = [
        *compiler,
        "-shared",
        "-fPIC",
        "-O2",
        "-Wall",
        "-Wextra",
        f"-I{ELM_INC}",
        f"-I{ECU_INC}",
        f"-I{OBD2_INC}",
        str(ELM_C),
        str(ECU_C),
        str(OBD2_C),
        "-o",
        str(out_lib),
    ]
    subprocess.run(cmd, check=True)
    lib = ctypes.CDLL(str(out_lib))

    lib.elm327_init.argtypes = [ctypes.POINTER(ElmCtx), ctypes.POINTER(ElmCallbacks)]
    lib.elm327_feed_bytes.argtypes = [ctypes.POINTER(ElmCtx), ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t]
    lib.elm327_format_slcan_frame.argtypes = [
        ctypes.c_uint32,
        ctypes.c_bool,
        ctypes.POINTER(ctypes.c_uint8),
        ctypes.c_uint8,
        ctypes.c_char_p,
        ctypes.c_size_t,
    ]
    lib.elm327_format_slcan_frame.restype = ctypes.c_int

    lib.ecu_sim_init.argtypes = [ctypes.POINTER(EcuSimState)]
    lib.ecu_sim_set_rpm.argtypes = [ctypes.POINTER(EcuSimState), ctypes.c_uint16]
    lib.ecu_sim_set_speed.argtypes = [ctypes.POINTER(EcuSimState), ctypes.c_uint8]
    lib.ecu_sim_set_coolant.argtypes = [ctypes.POINTER(EcuSimState), ctypes.c_int16]
    lib.ecu_sim_inject_dtc.argtypes = [ctypes.POINTER(EcuSimState), ctypes.c_char_p]
    lib.ecu_sim_inject_dtc.restype = ctypes.c_int
    lib.ecu_sim_clear_dtcs.argtypes = [ctypes.POINTER(EcuSimState)]
    lib.ecu_sim_handle_request.argtypes = [
        ctypes.POINTER(EcuSimState),
        ctypes.c_bool,
        ctypes.POINTER(ctypes.c_uint8),
        ctypes.c_size_t,
        ctypes.POINTER(ctypes.c_uint8),
        ctypes.c_size_t,
        ctypes.POINTER(ctypes.c_size_t),
    ]
    lib.ecu_sim_handle_request.restype = ctypes.c_int
    return lib


class ElmHarness:
    def __init__(self, lib: ctypes.CDLL) -> None:
        self.lib = lib
        self.out = bytearray()
        self.sim = EcuSimState()
        lib.ecu_sim_init(ctypes.byref(self.sim))

        @WRITE_CB
        def _write(ptr, n: int, _ctx) -> None:
            self.out.extend(ptr[i] for i in range(n))

        @VBAT_CB
        def _vbat(_ctx) -> int:
            return self.sim.vbat_mv

        @OBD_CB
        def _obd(_tx_hdr, _proto, ext_mode, req_ptr, req_len, resp_ptr, max_resp, out_len_ptr, rx_id_ptr, _ctx) -> int:
            rc = lib.ecu_sim_handle_request(
                ctypes.byref(self.sim),
                ext_mode,
                req_ptr,
                req_len,
                resp_ptr,
                max_resp,
                out_len_ptr,
            )
            rx_id_ptr[0] = 0x7E8
            return 0 if rc == 0 else 1  # ELM_STATUS_OK vs ELM_STATUS_NO_DATA

        @PROTO_CB
        def _proto(_p: int, _ctx) -> int:
            return 0

        self.factory_called = False

        @FACTORY_CB
        def _factory(_user: ctypes.c_void_p) -> None:
            self.factory_called = True

        self._cb_refs = (_write, _vbat, _obd, _proto, _factory)
        self.cbs = ElmCallbacks(
            write_bytes=_write,
            get_vbat_mv=_vbat,
            send_obd_req=_obd,
            set_protocol=_proto,
            factory_reset=_factory,
            user_ctx=None,
        )
        self.ctx = ElmCtx()
        lib.elm327_init(ctypes.byref(self.ctx), ctypes.byref(self.cbs))

    def cmd(self, text: str) -> str:
        self.out.clear()
        raw = text.encode("ascii")
        arr = (ctypes.c_uint8 * len(raw))(*raw)
        self.lib.elm327_feed_bytes(ctypes.byref(self.ctx), arr, len(raw))
        return self.out.decode("ascii")


def test_elm327_at_basics_and_formatting(phase5_lib: ctypes.CDLL) -> None:
    h = ElmHarness(phase5_lib)
    assert h.cmd("ATZ\r") == "ATZ\rELM327 v1.5\r\r>"
    assert h.cmd("ATE0\r") == "ATE0\rOK\r\r>"
    assert h.cmd("ATI\r") == "ELM327 v1.5\r\r>"
    assert h.cmd("ATRV\r") == "12.6V\r\r>"
    assert h.cmd("ATSP6\r") == "OK\r\r>"
    assert h.cmd("ATDP\r") == "ISO 15765-4 (CAN 11/500)\r\r>"
    assert h.cmd("ATDPN\r") == "6\r\r>"
    assert h.cmd("ATUNKNOWN\r") == "?\r\r>"


def test_elm327_obd_mode01_and_headers(phase5_lib: ctypes.CDLL) -> None:
    h = ElmHarness(phase5_lib)
    h.cmd("ATE0\r")
    phase5_lib.ecu_sim_set_rpm(ctypes.byref(h.sim), 1500)
    phase5_lib.ecu_sim_set_speed(ctypes.byref(h.sim), 65)
    phase5_lib.ecu_sim_set_coolant(ctypes.byref(h.sim), 90)

    assert h.cmd("010C\r") == "41 0C 17 70\r\r>"
    assert h.cmd("010D\r") == "41 0D 41\r\r>"
    assert h.cmd("0105\r") == "41 05 82\r\r>"
    # Repeat last command with bare CR
    assert h.cmd("\r") == "41 05 82\r\r>"

    # Disable spaces, enable headers
    assert h.cmd("ATS0\r") == "OK\r\r>"
    assert h.cmd("ATH1\r") == "OK\r\r>"
    assert h.cmd("010C\r") == "7E8 410C1770\r\r>"

    # Unsupported PID -> NO DATA
    assert h.cmd("0199\r") == "NO DATA\r\r>"


def test_elm327_multiline_vin_and_dtc_cycle(phase5_lib: ctypes.CDLL) -> None:
    h = ElmHarness(phase5_lib)
    h.cmd("ATE0\r")

    vin_resp = h.cmd("0902\r")
    assert vin_resp.startswith("014\r0:49 02 01 31 47 31 4A\r1:")
    assert vin_resp.endswith("\r\r>")

    assert phase5_lib.ecu_sim_inject_dtc(ctypes.byref(h.sim), b"P0100") == 0
    assert phase5_lib.ecu_sim_inject_dtc(ctypes.byref(h.sim), b"C0300") == 0
    assert h.cmd("03\r") == "43 02 01 00 43 00\r\r>"
    assert h.cmd("04\r") == "44\r\r>"
    assert h.cmd("03\r") == "43 00\r\r>"


def test_extended_mode_and_security_gating(phase5_lib: ctypes.CDLL) -> None:
    h = ElmHarness(phase5_lib)
    h.cmd("ATE0\r")

    # 0x27 blocked by default -> NRC 0x33
    assert h.cmd("2701\r") == "7F 27 33\r\r>"
    # Enter extended mode
    assert h.cmd("AT+EXT=1\r") == "EXT OK\r\r>"
    assert h.cmd("1003\r") == "50 03\r\r>"

    # UDS 0x22 ReadDataByIdentifier (0x010C RPM, 0xF187 part number, unknown 0xFFFF)
    phase5_lib.ecu_sim_set_rpm(ctypes.byref(h.sim), 2000)
    assert h.cmd("22010C\r") == "62 01 0C 1F 40\r\r>"
    part_resp = h.cmd("22F187\r")
    assert "62 F1 87" in part_resp
    assert h.cmd("22FFFF\r") == "7F 22 31\r\r>"

    # UDS 0x2E WriteDataByIdentifier (VIN 0xF190) -> verify via 0902
    new_vin = b"WVWZZZ1KZBW000001"
    vin_hex = new_vin.hex().upper()
    assert h.cmd(f"2EF190{vin_hex}\r") == "6E F1 90\r\r>"
    assert bytes(h.sim.vin) == new_vin

    # UDS 0x19 ReadDTCInformation & 0x14 ClearDiagnosticInformation
    assert phase5_lib.ecu_sim_inject_dtc(ctypes.byref(h.sim), b"P0301") == 0
    assert h.cmd("1901FF\r") == "59 01 FF 01 00 01\r\r>"
    assert h.cmd("1902FF\r") == "59 02 FF 03 01 00 09\r\r>"
    assert h.cmd("14FFFFFF\r") == "54\r\r>"
    assert h.cmd("1901FF\r") == "59 01 FF 01 00 00\r\r>"

    # KWP2000 services (0x81 StartCommunication, 0x3E TesterPresent, 0x21 ReadDataByLocalIdentifier, 0x18 ReadDTCByStatus, 0x82 StopCommunication)
    assert h.cmd("81\r") == "C1 EF 8F\r\r>"
    assert h.cmd("3E\r") == "7E\r\r>"
    assert h.cmd("2101\r").startswith("61 01 ")
    assert phase5_lib.ecu_sim_inject_dtc(ctypes.byref(h.sim), b"P0100") == 0
    assert h.cmd("1802FF00\r") == "58 01 01 00 60\r\r>"
    assert h.cmd("14FF00\r") == "54\r\r>"
    assert h.cmd("82\r") == "C2\r\r>"

    # Extended AT commands: AT+SNIFF, AT+STATS, AT+FACTORY, AT+SIM, and hex validation
    assert h.cmd("AT+SNIFF=1\r") == "OK\r\r>"
    assert h.ctx.sniff_enabled is True
    stats = h.cmd("AT+STATS\r")
    assert stats.startswith("RX:") and "TX:" in stats and "ERR:" in stats
    assert h.cmd("AT+FACTORY\r") == "FACTORY RESET\r\r>"
    assert h.factory_called is True

    assert h.cmd("AT+SIM:RPM=2500\r") == "OK\r\r>"
    assert h.cmd("AT+SIM:DTC=P0100\r") == "OK\r\r>"
    assert h.cmd("AT+SIM:DTC=CLEAR\r") == "OK\r\r>"
    assert h.cmd("AT+SIM:RPM=XYZ\r") == "?\r\r>"

    # Strict hex validation on ATSH / ATCRA / ATCF / ATCM / ATFCSH
    assert h.cmd("ATSH7E0\r") == "OK\r\r>"
    assert h.ctx.tx_header == 0x7E0
    assert h.cmd("ATSHXYZ\r") == "?\r\r>"
    assert h.ctx.tx_header == 0x7E0
    assert h.cmd("ATCRA7E8\r") == "OK\r\r>"
    assert h.ctx.rx_filter_id == 0x7E8 and h.ctx.rx_filter_mask == 0x7FF
    assert h.cmd("ATCAR\r") == "OK\r\r>"
    assert h.ctx.rx_filter_id == 0 and h.ctx.rx_filter_mask == 0

    assert h.cmd("AT+EXT=0\r") == "OK\r\r>"
    assert h.ctx.sniff_enabled is False

    # Passkey enforcement when configured
    phase5_lib.elm327_set_ext_passkey(b"SECRET123")
    try:
        assert h.cmd("AT+EXT=1\r") == "AUTH ERR\r\r>"
        assert h.ctx.extended_mode is False
        assert h.cmd("AT+EXT=1,SECRET123\r") == "EXT OK\r\r>"
        assert h.ctx.extended_mode is True
    finally:
        phase5_lib.elm327_set_ext_passkey(None)


def test_slcan_formatter_and_tcp_loopback_e2e(phase5_lib: ctypes.CDLL) -> None:
    import socket
    import threading

    # 1. SLCAN 11-bit and 29-bit formatting
    buf = ctypes.create_string_buffer(64)
    payload = (ctypes.c_uint8 * 3)(0x02, 0x01, 0x0C)
    n11 = phase5_lib.elm327_format_slcan_frame(0x7DF, False, payload, 3, buf, 64)
    assert n11 > 0
    assert buf.value == b"t7DF302010C\r"

    n29 = phase5_lib.elm327_format_slcan_frame(0x18DB33F1, True, payload, 3, buf, 64)
    assert n29 > 0
    assert buf.value == b"T18DB33F1302010C\r"

    # 2. Loopback TCP ELM327 server simulating port 35000 Wi-Fi transport
    h = ElmHarness(phase5_lib)
    phase5_lib.ecu_sim_set_rpm(ctypes.byref(h.sim), 3000)

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.bind(("127.0.0.1", 0))
    srv.listen(1)
    port = srv.getsockname()[1]

    def server_worker() -> None:
        conn, _ = srv.accept()
        with conn:
            while True:
                data = conn.recv(256)
                if not data:
                    break
                reply = h.cmd(data.decode("ascii"))
                conn.sendall(reply.encode("ascii"))

    t = threading.Thread(target=server_worker, daemon=True)
    t.start()

    with socket.create_connection(("127.0.0.1", port), timeout=2.0) as cli:
        for req, expected in [
            (b"ATZ\r", b"ATZ\rELM327 v1.5\r\r>"),
            (b"ATE0\r", b"ATE0\rOK\r\r>"),
            (b"ATSP6\r", b"OK\r\r>"),
            (b"010C\r", b"41 0C 2E E0\r\r>"),
        ]:
            cli.sendall(req)
            resp = cli.recv(256)
            assert resp == expected

    srv.close()
    t.join(timeout=1.0)


def test_car_scanner_and_python_obd_init_sequence(phase5_lib: ctypes.CDLL) -> None:
    """Replay the exact init + polling sequence used by python-OBD and Car Scanner."""
    h = ElmHarness(phase5_lib)
    phase5_lib.ecu_sim_set_rpm(ctypes.byref(h.sim), 1800)
    phase5_lib.ecu_sim_set_speed(ctypes.byref(h.sim), 55)
    phase5_lib.ecu_sim_set_coolant(ctypes.byref(h.sim), 85)

    assert "ELM327 v1.5" in h.cmd("ATZ\r")
    assert h.cmd("ATE0\r") == "ATE0\rOK\r\r>"
    assert h.cmd("ATL0\r") == "OK\r\r>"
    assert h.cmd("ATS0\r") == "OK\r\r>"
    assert h.cmd("ATH1\r") == "OK\r\r>"
    assert h.cmd("ATSP0\r") == "OK\r\r>"

    # Supported PIDs 01..20 (advertises 04, 05, 0C, 0D, 0F, 11)
    assert h.cmd("0100\r") == "7E8 4100181A8001\r\r>"
    # Poll every advertised PID with headers on, spaces off
    assert h.cmd("0104\r").startswith("7E8 4104")
    assert h.cmd("0105\r") == "7E8 41057D\r\r>"
    assert h.cmd("010C\r") == "7E8 410C1C20\r\r>"
    assert h.cmd("010D\r") == "7E8 410D37\r\r>"
    assert h.cmd("010F\r") == "7E8 410F7D\r\r>"
    assert h.cmd("0111\r").startswith("7E8 4111")
    assert h.cmd("ATRV\r") == "12.6V\r\r>"
    assert h.cmd("03\r") == "7E8 4300\r\r>"


def test_elm327_parser_fuzzing_and_line_overflow(phase5_lib: ctypes.CDLL) -> None:
    """Verify ELM327 parser survives line buffer overflow, odd hex, and malformed AT commands."""
    h = ElmHarness(phase5_lib)
    h.cmd("ATE0\r")

    # 1. Line buffer overflow (> 64 chars without CR) emits BUFFER FULL and does not corrupt state
    overlong = "A" * 200 + "\r"
    assert "BUFFER FULL" in h.cmd(overlong)
    # Subsequent valid command must work cleanly
    assert h.cmd("ATI\r") == "ELM327 v1.5\r\r>"

    # 2. Odd-length hex and non-hex characters in OBD requests
    assert h.cmd("010\r") == "?\r\r>"
    assert h.cmd("01GG\r") == "?\r\r>"

    # 3. Malformed AT headers/filters/timing and out-of-range AT+SIM parameters
    for bad_cmd in (
        "ATSH\r",
        "ATSH123456789\r",
        "ATCRA\r",
        "ATCF\r",
        "ATCM\r",
        "ATFCSH\r",
        "ATSTGG\r",
        "AT+EXT=1\r",
    ):
        resp = h.cmd(bad_cmd)
        assert resp.endswith("\r\r>")

    # In extended mode, test AT+SIM boundary validation
    for bad_sim in (
        "AT+SIM:RPM=-1\r",
        "AT+SIM:RPM=20000\r",
        "AT+SIM:SPEED=300\r",
        "AT+SIM:COOLANT=-50\r",
        "AT+SIM:COOLANT=250\r",
        "AT+SIM:VBAT=1000\r",
        "AT+SIM:DTC=BAD\r",
        "AT+SIM:UNKNOWN=1\r",
    ):
        assert h.cmd(bad_sim) == "?\r\r>"


def test_ecu_sim_boundary_dtcs_and_modes(phase5_lib: ctypes.CDLL) -> None:
    """Verify DTC table saturation (max 16), Mode 02 Freeze Frame, and UDS 0x22/0x2E boundaries."""
    h = ElmHarness(phase5_lib)
    h.cmd("ATE0\r")

    # 1. Saturate DTC table (ECU_SIM_MAX_DTCS = 16)
    for i in range(16):
        dtc_code = f"P01{i:02X}".encode("ascii")
        assert phase5_lib.ecu_sim_inject_dtc(ctypes.byref(h.sim), dtc_code) == 0
    assert h.sim.dtc_count == 16

    # 17th DTC and invalid DTC strings must be rejected (-1) without overflowing
    assert phase5_lib.ecu_sim_inject_dtc(ctypes.byref(h.sim), b"P0200") == -1
    assert phase5_lib.ecu_sim_inject_dtc(ctypes.byref(h.sim), b"Z9999") == -1
    assert h.sim.dtc_count == 16

    # Clear and verify Mode 02 Freeze Frame
    phase5_lib.ecu_sim_clear_dtcs(ctypes.byref(h.sim))
    assert h.sim.dtc_count == 0
    phase5_lib.ecu_sim_set_rpm(ctypes.byref(h.sim), 1500)
    assert h.cmd("020C\r") == "42 0C 17 70\r\r>"

    # 2. UDS 0x22 (0xF190 VIN, 0x010D speed) & 0x2E invalid VIN length -> NRC 0x31
    assert h.cmd("AT+EXT=1\r") == "EXT OK\r\r>"
    phase5_lib.ecu_sim_set_speed(ctypes.byref(h.sim), 88)
    assert h.cmd("22010D\r") == "62 01 0D 58\r\r>"
    assert "62 F1 90" in h.cmd("22F190\r")
    assert h.cmd("2EF190313233\r") == "7F 2E 31\r\r>"

