#!/usr/bin/env python3
"""Move data between a PC and the feeder over plain TCP, using the BusyBox `nc -l -p PORT` already on
the device. The feeder always listens and this script always connects, so no inbound firewall rule is
needed on the PC. Python 3 standard library only.

  send FILE                 feeder: nc -l -p 9000 > /path/on/device
  backup DIR                feeder: for p in 0 1 2 3 4 5 6; do nc -l -p 9000 < /dev/mtd$p; done
  receive SIZE FILE         feeder: <something> | nc -l -p 9000          (exactly SIZE bytes)
  capture SECONDS FILE      reads the hook's raw video stream (port 8556) for a few seconds

Every command takes --host (the feeder's IP address) and optionally --port.
"""
import argparse
import hashlib
import socket
import sys
import time

# Flash layout of this firmware (/proc/mtd): name, size in bytes
MTD = [("mtd0_UBOOT", 0x32000), ("mtd1_ENV", 0x1000), ("mtd2_ENVBK", 0x1000), ("mtd3_DTB", 0xC000),
       ("mtd4_KERNEL", 0x190000), ("mtd5_ROOTFS", 0x570000), ("mtd6_CONFIG", 0xC0000)]


def connect(host, port, wait_s=600):
    deadline = time.time() + wait_s
    while True:
        try:
            return socket.create_connection((host, port), timeout=10)
        except OSError:
            if time.time() > deadline:
                sys.exit(f"nothing listening on {host}:{port}")
            time.sleep(0.7)


def read_exact(sock, size):
    buf = bytearray()
    sock.settimeout(30)
    while len(buf) < size:
        chunk = sock.recv(min(65536, size - len(buf)))
        if not chunk:
            break
        buf += chunk
    return bytes(buf)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", required=True, help="feeder IP address")
    ap.add_argument("--port", type=int, default=9000)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("send"); s.add_argument("file")
    b = sub.add_parser("backup"); b.add_argument("dir")
    r = sub.add_parser("receive"); r.add_argument("size", type=lambda x: int(x, 0)); r.add_argument("file")
    c = sub.add_parser("capture"); c.add_argument("seconds", type=float); c.add_argument("file")
    a = ap.parse_args()

    if a.cmd == "send":
        data = open(a.file, "rb").read()
        with connect(a.host, a.port) as sk:
            sk.sendall(data)
            sk.shutdown(socket.SHUT_WR)
            time.sleep(0.5)
        print(f"sent {len(data)} bytes, md5 {hashlib.md5(data).hexdigest()}")

    elif a.cmd == "backup":
        import os
        os.makedirs(a.dir, exist_ok=True)
        for name, size in MTD:
            with connect(a.host, a.port) as sk:
                data = read_exact(sk, size)
            path = os.path.join(a.dir, name + ".bin")
            open(path, "wb").write(data)
            status = "OK" if len(data) == size else "SHORT"
            print(f"{name:<12} {len(data):>8} of {size:>8} bytes  {status}  sha256 {hashlib.sha256(data).hexdigest()[:16]}")
            time.sleep(1.5)

    elif a.cmd == "receive":
        with connect(a.host, a.port) as sk:
            data = read_exact(sk, a.size)
        open(a.file, "wb").write(data)
        print(f"received {len(data)} of {a.size} bytes -> {a.file}")

    elif a.cmd == "capture":
        port = a.port if a.port != 9000 else 8556
        total = 0
        with connect(a.host, port) as sk, open(a.file, "wb") as f:
            sk.settimeout(5)
            end = time.time() + a.seconds
            while time.time() < end:
                try:
                    chunk = sk.recv(65536)
                except socket.timeout:
                    continue
                if not chunk:
                    break
                f.write(chunk)
                total += len(chunk)
        print(f"captured {total} bytes in {a.seconds:g} s -> {a.file}  (check: ffprobe -f hevc {a.file})")


if __name__ == "__main__":
    main()
