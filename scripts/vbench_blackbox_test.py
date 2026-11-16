"""Black-box Wi-Fi TCP (192.168.0.10:35000) validation script for LilyGO VIRTUAL_BENCH."""

import argparse
import socket
import sys
import time


def send_elm(sock: socket.socket, cmd: str, timeout: float = 2.0) -> str:
    sock.settimeout(timeout)
    sock.sendall((cmd + "\r").encode("ascii"))
    buf = bytearray()
    while True:
        chunk = sock.recv(512)
        if not chunk:
            break
        buf.extend(chunk)
        if b">" in buf:
            break
    return buf.decode("ascii", errors="replace")


def run_suite(host: str, port: int, stress_cycles: int, bench_count: int) -> None:
    print(f"[1/4] Connecting to {host}:{port}...")
    with socket.create_connection((host, port), timeout=5.0) as s:
        assert "ELM327 v1.5" in send_elm(s, "ATZ"), "ATZ failed"
        assert "OK" in send_elm(s, "ATE0"), "ATE0 failed"
        assert "ELM327 v1.5" in send_elm(s, "ATI"), "ATI failed"
        vbat = send_elm(s, "ATRV")
        assert "V" in vbat, f"ATRV unexpected: {vbat!r}"
        assert "OK" in send_elm(s, "ATSP6"), "ATSP6 failed"
        print(f"  AT basics OK (ATRV={vbat.strip()})")

        print("[2/4] Mode 01 PIDs & Mode 09 VIN...")
        rpm_resp = send_elm(s, "010C")
        assert rpm_resp.startswith("41 0C"), f"010C unexpected: {rpm_resp!r}"
        spd_resp = send_elm(s, "010D")
        assert spd_resp.startswith("41 0D"), f"010D unexpected: {spd_resp!r}"
        vin_resp = send_elm(s, "0902")
        assert "014\r0:49 02 01" in vin_resp, f"0902 unexpected: {vin_resp!r}"
        print("  OBD Mode 01 & 0902 OK")

        print("[3/4] Extended mode UDS (0x22 / 0x19 / 0x14)...")
        assert "EXT OK" in send_elm(s, "AT+EXT=1"), "AT+EXT=1 failed"
        assert "62 01 0C" in send_elm(s, "22010C"), "UDS 0x22 010C failed"
        assert "59 01" in send_elm(s, "1901FF"), "UDS 0x19 01 failed"
        assert "OK" in send_elm(s, "AT+EXT=0"), "AT+EXT=0 failed"
        print("  Extended mode UDS OK")

        if bench_count > 0:
            t0 = time.perf_counter()
            for i in range(bench_count):
                pid = "010C" if (i % 2 == 0) else "010D"
                resp = send_elm(s, pid)
                assert resp.startswith("41 "), f"Bench req {i} failed: {resp!r}"
            dt = time.perf_counter() - t0
            print(f"  Wi-Fi Benchmark: {bench_count} PIDs in {dt:.2f}s ({bench_count / dt:.1f} req/s)")

    print(f"[4/4] Disconnect/reconnect stress ({stress_cycles} cycles)...")
    for i in range(stress_cycles):
        with socket.create_connection((host, port), timeout=5.0) as s:
            resp = send_elm(s, "ATI")
            assert "ELM327 v1.5" in resp, f"Cycle {i} failed: {resp!r}"
        time.sleep(0.05)
    print("  Stress test OK — ALL BLACK-BOX CHECKS PASSED!")


def main() -> int:
    parser = argparse.ArgumentParser(description="OBD2 VIRTUAL_BENCH Wi-Fi black-box validator")
    parser.add_argument("--host", default="192.168.0.10", help="Target IP (default: 192.168.0.10)")
    parser.add_argument("--port", type=int, default=35000, help="ELM327 TCP port (default: 35000)")
    parser.add_argument("--cycles", type=int, default=5, help="Reconnect stress cycles (default: 5)")
    parser.add_argument("--bench", type=int, default=200, help="Benchmark PID count (0=skip)")
    args = parser.parse_args()
    run_suite(args.host, args.port, args.cycles, args.bench)
    return 0


if __name__ == "__main__":
    sys.exit(main())
