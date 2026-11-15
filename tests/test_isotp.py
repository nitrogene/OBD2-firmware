"""Host unit tests for the real C implementation of components/isotp/isotp.c."""

from __future__ import annotations

import ctypes
import platform
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent
ISOTP_C = REPO_ROOT / "components" / "isotp" / "isotp.c"
ISOTP_INC = REPO_ROOT / "components" / "isotp" / "include"

ISOTP_MAX_PAYLOAD = 4095
ISOTP_OK = 0
ISOTP_ERR_INVALID_ARG = -1
ISOTP_ERR_BUSY = -2
ISOTP_ERR_OVERFLOW = -3
ISOTP_ERR_TIMEOUT = -4
ISOTP_ERR_SEQ = -5
ISOTP_ERR_NO_DATA = -6

SEND_CAN_CB = ctypes.CFUNCTYPE(
    ctypes.c_int,
    ctypes.c_uint32,
    ctypes.c_bool,
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.c_uint8,
    ctypes.c_void_p,
)
GET_MS_CB = ctypes.CFUNCTYPE(ctypes.c_uint32, ctypes.c_void_p)


class IsotopConfig(ctypes.Structure):
    _fields_ = [
        ("tx_id", ctypes.c_uint32),
        ("rx_id", ctypes.c_uint32),
        ("extd", ctypes.c_bool),
        ("block_size", ctypes.c_uint8),
        ("stmin_ms", ctypes.c_uint8),
        ("pad_to_8", ctypes.c_bool),
        ("padding_byte", ctypes.c_uint8),
        ("n_bs_ms", ctypes.c_uint32),
        ("n_cr_ms", ctypes.c_uint32),
        ("send_can", SEND_CAN_CB),
        ("get_ms", GET_MS_CB),
        ("user_ctx", ctypes.c_void_p),
    ]


class IsotopLink(ctypes.Structure):
    _fields_ = [
        ("cfg", IsotopConfig),
        ("tx_state", ctypes.c_int),
        ("tx_buf", ctypes.c_uint8 * ISOTP_MAX_PAYLOAD),
        ("tx_len", ctypes.c_uint16),
        ("tx_offset", ctypes.c_uint16),
        ("tx_sn", ctypes.c_uint8),
        ("tx_bs_rem", ctypes.c_uint8),
        ("tx_peer_bs", ctypes.c_uint8),
        ("tx_peer_stmin_ms", ctypes.c_uint32),
        ("tx_last_ms", ctypes.c_uint32),
        ("tx_last_err", ctypes.c_int),
        ("rx_state", ctypes.c_int),
        ("rx_buf", ctypes.c_uint8 * ISOTP_MAX_PAYLOAD),
        ("rx_len", ctypes.c_uint16),
        ("rx_offset", ctypes.c_uint16),
        ("rx_sn", ctypes.c_uint8),
        ("rx_bs_rem", ctypes.c_uint8),
        ("rx_last_ms", ctypes.c_uint32),
        ("rx_last_err", ctypes.c_int),
    ]


@pytest.fixture(scope="session")
def isotp_lib(tmp_path_factory: pytest.TempPathFactory) -> ctypes.CDLL:
    """Compile components/isotp/isotp.c into a shared library for host testing."""
    build_dir = tmp_path_factory.mktemp("isotp_build")
    ext = ".dll" if platform.system() == "Windows" else ".so"
    out_lib = build_dir / f"libisotp{ext}"

    gcc = shutil.which("gcc")
    if gcc:
        cmd = [gcc, "-shared", "-fPIC", "-O2", "-Wall", "-Wextra", f"-I{ISOTP_INC}", str(ISOTP_C), "-o", str(out_lib)]
    else:
        cmd = [
            sys.executable,
            "-m",
            "ziglang",
            "cc",
            "-shared",
            "-fPIC",
            "-O2",
            "-Wall",
            "-Wextra",
            f"-I{ISOTP_INC}",
            str(ISOTP_C),
            "-o",
            str(out_lib),
        ]

    subprocess.run(cmd, check=True)
    lib = ctypes.CDLL(str(out_lib))

    lib.isotp_init.argtypes = [ctypes.POINTER(IsotopLink), ctypes.POINTER(IsotopConfig)]
    lib.isotp_init.restype = ctypes.c_int

    lib.isotp_send.argtypes = [
        ctypes.POINTER(IsotopLink),
        ctypes.POINTER(ctypes.c_uint8),
        ctypes.c_uint16,
    ]
    lib.isotp_send.restype = ctypes.c_int

    lib.isotp_on_can_rx.argtypes = [
        ctypes.POINTER(IsotopLink),
        ctypes.c_uint32,
        ctypes.c_bool,
        ctypes.POINTER(ctypes.c_uint8),
        ctypes.c_uint8,
    ]
    lib.isotp_on_can_rx.restype = ctypes.c_int

    lib.isotp_poll.argtypes = [ctypes.POINTER(IsotopLink)]
    lib.isotp_poll.restype = ctypes.c_int

    lib.isotp_receive.argtypes = [
        ctypes.POINTER(IsotopLink),
        ctypes.POINTER(ctypes.c_uint8),
        ctypes.c_uint16,
        ctypes.POINTER(ctypes.c_uint16),
    ]
    lib.isotp_receive.restype = ctypes.c_int
    return lib


class VirtualEndpoint:
    """Helper wrapping one isotp_link_t with captured CAN frames and virtual clock."""

    def __init__(
        self,
        lib: ctypes.CDLL,
        tx_id: int,
        rx_id: int,
        *,
        extd: bool = False,
        block_size: int = 0,
        stmin_ms: int = 0,
        pad_to_8: bool = True,
        padding_byte: int = 0xAA,
        n_bs_ms: int = 1000,
        n_cr_ms: int = 1000,
    ) -> None:
        self.lib = lib
        self.now_ms = 0
        self.tx_frames: list[tuple[int, bool, bytes]] = []
        self.link = IsotopLink()

        @SEND_CAN_CB
        def _send_can(can_id: int, is_extd: bool, data_ptr, dlc: int, _ctx) -> int:
            payload = bytes(data_ptr[i] for i in range(dlc))
            self.tx_frames.append((can_id, bool(is_extd), payload))
            return 0

        @GET_MS_CB
        def _get_ms(_ctx) -> int:
            return self.now_ms

        self._send_cb = _send_can
        self._ms_cb = _get_ms
        self.cfg = IsotopConfig(
            tx_id=tx_id,
            rx_id=rx_id,
            extd=extd,
            block_size=block_size,
            stmin_ms=stmin_ms,
            pad_to_8=pad_to_8,
            padding_byte=padding_byte,
            n_bs_ms=n_bs_ms,
            n_cr_ms=n_cr_ms,
            send_can=self._send_cb,
            get_ms=self._ms_cb,
            user_ctx=None,
        )
        assert lib.isotp_init(ctypes.byref(self.link), ctypes.byref(self.cfg)) == ISOTP_OK

    def send(self, payload: bytes) -> int:
        arr = (ctypes.c_uint8 * len(payload))(*payload)
        return self.lib.isotp_send(ctypes.byref(self.link), arr, len(payload))

    def feed_can(self, can_id: int, extd: bool, data: bytes) -> int:
        arr = (ctypes.c_uint8 * len(data))(*data)
        return self.lib.isotp_on_can_rx(ctypes.byref(self.link), can_id, extd, arr, len(data))

    def poll(self) -> int:
        return self.lib.isotp_poll(ctypes.byref(self.link))

    def receive(self, max_len: int = ISOTP_MAX_PAYLOAD) -> bytes | None:
        buf = (ctypes.c_uint8 * max_len)()
        out_len = ctypes.c_uint16(0)
        rc = self.lib.isotp_receive(ctypes.byref(self.link), buf, max_len, ctypes.byref(out_len))
        if rc == ISOTP_ERR_NO_DATA:
            return None
        assert rc == ISOTP_OK, f"Unexpected receive rc={rc}"
        return bytes(buf[: out_len.value])


def pump_bus(scanner: VirtualEndpoint, ecu: VirtualEndpoint, step_ms: int = 1, max_steps: int = 2000) -> None:
    """Exchange all pending CAN frames between two VirtualEndpoints."""
    for _ in range(max_steps):
        progressed = False
        while scanner.tx_frames:
            cid, extd, data = scanner.tx_frames.pop(0)
            assert ecu.feed_can(cid, extd, data) == ISOTP_OK
            progressed = True
        while ecu.tx_frames:
            cid, extd, data = ecu.tx_frames.pop(0)
            assert scanner.feed_can(cid, extd, data) == ISOTP_OK
            progressed = True
        scanner.now_ms += step_ms
        ecu.now_ms += step_ms
        assert scanner.poll() == ISOTP_OK
        assert ecu.poll() == ISOTP_OK
        if (
            not progressed
            and not scanner.tx_frames
            and not ecu.tx_frames
            and scanner.link.tx_state == 0
            and ecu.link.tx_state == 0
        ):
            # Stop once both sides are idle
            break


def test_single_frame_padded_and_unpadded(isotp_lib: ctypes.CDLL) -> None:
    scanner = VirtualEndpoint(isotp_lib, 0x7E0, 0x7E8, pad_to_8=True, padding_byte=0x55)
    ecu = VirtualEndpoint(isotp_lib, 0x7E8, 0x7E0, pad_to_8=False)

    assert scanner.send(b"\x01\x00") == ISOTP_OK
    assert scanner.tx_frames == [(0x7E0, False, b"\x02\x01\x00\x55\x55\x55\x55\x55")]

    pump_bus(scanner, ecu)
    assert ecu.receive() == b"\x01\x00"
    assert ecu.receive() is None

    assert ecu.send(b"\x41\x00\xBF\x9F\xA8\x93") == ISOTP_OK
    assert ecu.tx_frames == [(0x7E8, False, b"\x06\x41\x00\xBF\x9F\xA8\x93")]
    pump_bus(scanner, ecu)
    assert scanner.receive() == b"\x41\x00\xBF\x9F\xA8\x93"


def test_29bit_extended_ids(isotp_lib: ctypes.CDLL) -> None:
    scanner = VirtualEndpoint(isotp_lib, 0x18DA10F1, 0x18DAF110, extd=True)
    ecu = VirtualEndpoint(isotp_lib, 0x18DAF110, 0x18DA10F1, extd=True)

    # Wrong extd flag should be ignored
    assert ecu.feed_can(0x18DA10F1, False, b"\x02\x01\x00\xAA\xAA\xAA\xAA\xAA") == ISOTP_OK
    assert ecu.receive() is None

    payload = b"\x09\x02"
    assert scanner.send(payload) == ISOTP_OK
    pump_bus(scanner, ecu)
    assert ecu.receive() == payload


@pytest.mark.parametrize("length", [8, 17, 64, 200, 4095])
def test_multiframe_end_to_end(isotp_lib: ctypes.CDLL, length: int) -> None:
    scanner = VirtualEndpoint(isotp_lib, 0x7E0, 0x7E8, block_size=3, stmin_ms=2)
    ecu = VirtualEndpoint(isotp_lib, 0x7E8, 0x7E0, block_size=3, stmin_ms=2)

    payload = bytes((i * 37 + 11) & 0xFF for i in range(length))
    assert scanner.send(payload) == ISOTP_OK
    pump_bus(scanner, ecu, step_ms=1, max_steps=10000)
    assert ecu.receive() == payload


def test_sequence_error_and_fc_overflow(isotp_lib: ctypes.CDLL) -> None:
    rx = VirtualEndpoint(isotp_lib, 0x7E8, 0x7E0)
    # First Frame claiming 20 bytes
    assert rx.feed_can(0x7E0, False, b"\x10\x14\x01\x02\x03\x04\x05\x06") == ISOTP_OK
    assert rx.tx_frames[0][2][:3] == b"\x30\x00\x00"  # FC CTS
    # Wrong SN (0x22 instead of 0x21)
    assert rx.feed_can(0x7E0, False, b"\x22\x07\x08\x09\x0A\x0B\x0C\x0D") == ISOTP_ERR_SEQ

    tx = VirtualEndpoint(isotp_lib, 0x7E0, 0x7E8)
    assert tx.send(bytes(20)) == ISOTP_OK
    assert tx.feed_can(0x7E8, False, b"\x32\x00\x00\xAA\xAA\xAA\xAA\xAA") == ISOTP_ERR_OVERFLOW


def test_timeouts_n_bs_and_n_cr(isotp_lib: ctypes.CDLL) -> None:
    tx = VirtualEndpoint(isotp_lib, 0x7E0, 0x7E8, n_bs_ms=100)
    assert tx.send(bytes(20)) == ISOTP_OK
    tx.now_ms = 50
    assert tx.poll() == ISOTP_OK
    # FC.WAIT refreshes timer
    assert tx.feed_can(0x7E8, False, b"\x31\x00\x00\xAA\xAA\xAA\xAA\xAA") == ISOTP_OK
    tx.now_ms = 120
    assert tx.poll() == ISOTP_OK
    tx.now_ms = 150
    assert tx.poll() == ISOTP_ERR_TIMEOUT

    rx = VirtualEndpoint(isotp_lib, 0x7E8, 0x7E0, n_cr_ms=100)
    assert rx.feed_can(0x7E0, False, b"\x10\x14\x01\x02\x03\x04\x05\x06") == ISOTP_OK
    rx.now_ms = 100
    assert rx.poll() == ISOTP_ERR_TIMEOUT


def test_invalid_args_and_busy(isotp_lib: ctypes.CDLL) -> None:
    ep = VirtualEndpoint(isotp_lib, 0x7E0, 0x7E8)
    assert ep.send(b"") == ISOTP_ERR_INVALID_ARG
    assert ep.send(bytes(20)) == ISOTP_OK
    assert ep.send(b"\x01\x00") == ISOTP_ERR_BUSY
