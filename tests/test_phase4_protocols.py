"""Host unit tests for pure-C Phase 4 components: kline_proto, obd2_codec, kwp2000."""

from __future__ import annotations

import ctypes
import platform
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent
KLINE_C = REPO_ROOT / "components" / "kline_driver" / "kline_proto.c"
KLINE_INC = REPO_ROOT / "components" / "kline_driver" / "include"
OBD2_C = REPO_ROOT / "components" / "obd2_stack" / "obd2_codec.c"
OBD2_INC = REPO_ROOT / "components" / "obd2_stack" / "include"
KWP_C = REPO_ROOT / "components" / "kwp2000" / "kwp2000.c"
KWP_INC = REPO_ROOT / "components" / "kwp2000" / "include"


class KlineFrame(ctypes.Structure):
    _fields_ = [
        ("fmt", ctypes.c_uint8),
        ("target", ctypes.c_uint8),
        ("source", ctypes.c_uint8),
        ("payload", ctypes.c_uint8 * 255),
        ("payload_len", ctypes.c_uint8),
        ("checksum", ctypes.c_uint8),
    ]


@pytest.fixture(scope="session")
def phase4_lib(tmp_path_factory: pytest.TempPathFactory) -> ctypes.CDLL:
    """Compile kline_proto.c, obd2_codec.c, and kwp2000.c into a shared library."""
    build_dir = tmp_path_factory.mktemp("phase4_build")
    ext = ".dll" if platform.system() == "Windows" else ".so"
    out_lib = build_dir / f"libphase4{ext}"

    compiler = [shutil.which("gcc")] if shutil.which("gcc") else [sys.executable, "-m", "ziglang", "cc"]
    cmd = [
        *compiler,
        "-shared",
        "-fPIC",
        "-O2",
        "-Wall",
        "-Wextra",
        f"-I{KLINE_INC}",
        f"-I{OBD2_INC}",
        f"-I{KWP_INC}",
        str(KLINE_C),
        str(OBD2_C),
        str(KWP_C),
        "-o",
        str(out_lib),
    ]
    subprocess.run(cmd, check=True)
    lib = ctypes.CDLL(str(out_lib))

    lib.kline_checksum.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t]
    lib.kline_checksum.restype = ctypes.c_uint8

    lib.kline_build_kwp_frame.argtypes = [
        ctypes.c_uint8,
        ctypes.c_uint8,
        ctypes.c_uint8,
        ctypes.POINTER(ctypes.c_uint8),
        ctypes.c_uint8,
        ctypes.POINTER(ctypes.c_uint8),
        ctypes.c_size_t,
    ]
    lib.kline_build_kwp_frame.restype = ctypes.c_int

    lib.kline_parse_frame.argtypes = [
        ctypes.POINTER(ctypes.c_uint8),
        ctypes.c_size_t,
        ctypes.POINTER(KlineFrame),
    ]
    lib.kline_parse_frame.restype = ctypes.c_int

    lib.kline_verify_echo.argtypes = [
        ctypes.POINTER(ctypes.c_uint8),
        ctypes.POINTER(ctypes.c_uint8),
        ctypes.c_size_t,
    ]
    lib.kline_verify_echo.restype = ctypes.c_bool

    lib.kline_classify_keybytes.argtypes = [ctypes.c_uint8, ctypes.c_uint8]
    lib.kline_classify_keybytes.restype = ctypes.c_int

    lib.obd2_is_service_allowed.argtypes = [ctypes.c_uint8, ctypes.c_bool]
    lib.obd2_is_service_allowed.restype = ctypes.c_bool

    lib.obd2_decode_dtc.argtypes = [ctypes.c_uint8, ctypes.c_uint8, ctypes.c_char_p]
    lib.obd2_decode_dtc.restype = ctypes.c_int

    lib.obd2_encode_dtc.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint8)]
    lib.obd2_encode_dtc.restype = ctypes.c_int

    lib.obd2_scale_mode01_pid.argtypes = [
        ctypes.c_uint8,
        ctypes.POINTER(ctypes.c_uint8),
        ctypes.c_uint8,
        ctypes.POINTER(ctypes.c_int32),
    ]
    lib.obd2_scale_mode01_pid.restype = ctypes.c_int

    lib.kwp2000_build_read_data.argtypes = [
        ctypes.c_uint8,
        ctypes.c_uint8,
        ctypes.c_uint16,
        ctypes.POINTER(ctypes.c_uint8),
        ctypes.c_size_t,
    ]
    lib.kwp2000_build_read_data.restype = ctypes.c_int

    lib.kwp2000_build_io_control.argtypes = [
        ctypes.c_uint8,
        ctypes.c_uint8,
        ctypes.c_uint8,
        ctypes.POINTER(ctypes.c_uint8),
        ctypes.c_uint8,
        ctypes.POINTER(ctypes.c_uint8),
        ctypes.c_size_t,
    ]
    lib.kwp2000_build_io_control.restype = ctypes.c_int

    lib.kwp2000_build_read_dtc.argtypes = [
        ctypes.c_uint8,
        ctypes.c_uint8,
        ctypes.c_uint16,
        ctypes.POINTER(ctypes.c_uint8),
        ctypes.c_size_t,
    ]
    lib.kwp2000_build_read_dtc.restype = ctypes.c_int

    lib.kwp2000_build_clear_dtc.argtypes = [
        ctypes.c_uint8,
        ctypes.c_uint16,
        ctypes.POINTER(ctypes.c_uint8),
        ctypes.c_size_t,
    ]
    lib.kwp2000_build_clear_dtc.restype = ctypes.c_int

    lib.kwp2000_build_service.argtypes = [
        ctypes.c_uint8,
        ctypes.c_uint8,
        ctypes.POINTER(ctypes.c_uint8),
        ctypes.c_uint8,
        ctypes.c_bool,
        ctypes.POINTER(ctypes.c_uint8),
        ctypes.c_size_t,
    ]
    lib.kwp2000_build_service.restype = ctypes.c_int
    return lib


def test_kline_framing_checksum_and_echo(phase4_lib: ctypes.CDLL) -> None:
    payload = (ctypes.c_uint8 * 1)(0x81)
    out = (ctypes.c_uint8 * 16)()
    total = phase4_lib.kline_build_kwp_frame(0x80, 0x10, 0xF1, payload, 1, out, 16)
    assert total == 5
    # 0x81 + 0x10 + 0xF1 + 0x81 = 0x203 -> 0x03
    assert bytes(out[:total]) == b"\x81\x10\xF1\x81\x03"

    parsed = KlineFrame()
    assert phase4_lib.kline_parse_frame(out, total, ctypes.byref(parsed)) == 0
    assert parsed.target == 0x10 and parsed.source == 0xF1
    assert bytes(parsed.payload[: parsed.payload_len]) == b"\x81"

    # Corrupt checksum -> error
    out[4] ^= 0x01
    assert phase4_lib.kline_parse_frame(out, total, ctypes.byref(parsed)) < 0

    # Echo verification
    tx = (ctypes.c_uint8 * 4)(0x81, 0x10, 0xF1, 0x81)
    rx_ok = (ctypes.c_uint8 * 4)(0x81, 0x10, 0xF1, 0x81)
    rx_col = (ctypes.c_uint8 * 4)(0x81, 0x10, 0x00, 0x81)
    assert phase4_lib.kline_verify_echo(tx, rx_ok, 4) is True
    assert phase4_lib.kline_verify_echo(tx, rx_col, 4) is False


def test_kline_keybyte_classification(phase4_lib: ctypes.CDLL) -> None:
    # 1 = ISO9141-2, 2 = KWP2000, 0 = UNKNOWN
    assert phase4_lib.kline_classify_keybytes(0x08, 0x08) == 1
    assert phase4_lib.kline_classify_keybytes(0x94, 0x94) == 1
    assert phase4_lib.kline_classify_keybytes(0xE9, 0x8F) == 2
    assert phase4_lib.kline_classify_keybytes(0xEA, 0x8F) == 2


@pytest.mark.parametrize("dtc", ["P0100", "C0300", "B1234", "U0100"])
def test_obd2_dtc_roundtrip(phase4_lib: ctypes.CDLL, dtc: str) -> None:
    raw = (ctypes.c_uint8 * 2)()
    assert phase4_lib.obd2_encode_dtc(dtc.encode("ascii"), raw) == 0
    out_str = ctypes.create_string_buffer(6)
    assert phase4_lib.obd2_decode_dtc(raw[0], raw[1], out_str) == 0
    assert out_str.value.decode("ascii") == dtc


def test_obd2_mode01_pid_scaling(phase4_lib: ctypes.CDLL) -> None:
    val = ctypes.c_int32(0)
    # Coolant: 130 - 40 = 90.00 C -> 9000
    d1 = (ctypes.c_uint8 * 1)(130)
    assert phase4_lib.obd2_scale_mode01_pid(0x05, d1, 1, ctypes.byref(val)) == 0
    assert val.value == 9000

    # RPM: (0x0B * 256 + 0xB8) / 4 = 3000 / 4 = 750.00 -> 75000
    d2 = (ctypes.c_uint8 * 2)(0x0B, 0xB8)
    assert phase4_lib.obd2_scale_mode01_pid(0x0C, d2, 2, ctypes.byref(val)) == 0
    assert val.value == 75000


def test_kwp2000_services_and_security_gating(phase4_lib: ctypes.CDLL) -> None:
    out = (ctypes.c_uint8 * 32)()

    # GM-Daewoo ECM ReadDataByLocalIdentifier (0x21 0x01)
    n = phase4_lib.kwp2000_build_read_data(0x10, 0x21, 0x01, out, 32)
    assert n == 6
    parsed = KlineFrame()
    assert phase4_lib.kline_parse_frame(out, n, ctypes.byref(parsed)) == 0
    assert bytes(parsed.payload[: parsed.payload_len]) == b"\x21\x01"

    # InputOutputControlByLocalIdentifier (0x30) on ABS (0x58)
    n_io = phase4_lib.kwp2000_build_io_control(0x58, 0x05, 0x07, None, 0, out, 32)
    assert n_io == 7

    # ReadDTC (0x18) & ClearDTC (0x14)
    assert phase4_lib.kwp2000_build_read_dtc(0x10, 0x02, 0xFF00, out, 32) == 8
    assert phase4_lib.kwp2000_build_clear_dtc(0x10, 0xFF00, out, 32) == 7

    # Sensitive services MUST be blocked when extended_unlocked=False
    for blocked_sid in (0x27, 0x31, 0x34, 0x35, 0x36, 0x37):
        assert phase4_lib.obd2_is_service_allowed(blocked_sid, False) is False
        rc = phase4_lib.kwp2000_build_service(0x10, blocked_sid, None, 0, False, out, 32)
        assert rc == -2  # KWP_ERR_SECURITY_BLOCKED

        # Allowed when explicitly unlocked in extended mode
        assert phase4_lib.obd2_is_service_allowed(blocked_sid, True) is True
        rc_ok = phase4_lib.kwp2000_build_service(0x10, blocked_sid, None, 0, True, out, 32)
        assert rc_ok == 5
