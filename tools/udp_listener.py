#!/usr/bin/env python3
"""
udp_listener.py — quick sanity check for the "Démonstrateur with Claude" board's
Wi-Fi/UDP link.

The board (Core/Src/wifi.c) sends one UDP packet per sample to
SERVER_IP:SERVER_PORT (see Core/Inc/wifi.h), containing JSON like:
    {"ax":123,"ay":-45,"az":16000,"gx":0,"gy":1,"gz":-2}

This just listens on a UDP port, prints what arrives, and reports the
packet rate — enough to confirm the board actually joined the Wi-Fi network
and is reaching this machine.

Usage:
    python3 udp_listener.py                  # listen on 0.0.0.0:4210
    python3 udp_listener.py --port 4210
    python3 udp_listener.py --host 10.198.244.16 --port 4210

Stop with Ctrl+C.
"""

import argparse
import json
import socket
import time


def guess_local_ip() -> str:
    """Best-effort guess at this machine's LAN-facing IP (the one the board's
    SERVER_IP should point at). Opens a UDP socket "connected" to a public
    address to let the OS pick the outbound interface — no packet is actually
    sent (UDP connect() is just a local route lookup)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except OSError:
        return "unknown (no network route — are you connected to the hotspot?)"
    finally:
        s.close()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--host", default="0.0.0.0",
                         help="local address to bind (default: 0.0.0.0, all interfaces)")
    parser.add_argument("--port", type=int, default=4210,
                         help="UDP port to listen on (default: 4210, matches wifi.h SERVER_PORT)")
    parser.add_argument("--raw", action="store_true",
                         help="print raw bytes instead of trying to parse JSON")
    args = parser.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((args.host, args.port))

    print(f"Listening on UDP {args.host}:{args.port}")
    print(f"This machine's LAN-facing IP looks like: {guess_local_ip()}")
    print("  -> SERVER_IP in Core/Inc/wifi.h must match this, on the same network as the board.")
    print("Waiting for packets from the board... Ctrl+C to stop.\n")

    count = 0
    last_t = None

    try:
        while True:
            data, addr = sock.recvfrom(2048)
            now = time.monotonic()
            count += 1
            dt = (now - last_t) if last_t is not None else None
            last_t = now

            ts = time.strftime("%H:%M:%S")
            rate = f"{1.0 / dt:5.2f} Hz" if dt else "  --  "
            prefix = f"[{ts}] #{count:<5} from {addr[0]}:{addr[1]}  ({rate})  "

            if args.raw:
                print(prefix + repr(data))
                continue

            try:
                text = data.decode("utf-8").strip()
                payload = json.loads(text)
                fields = "  ".join(f"{k}={v}" for k, v in payload.items())
                print(prefix + fields)
            except (UnicodeDecodeError, json.JSONDecodeError):
                print(prefix + f"(not JSON) {data!r}")

    except KeyboardInterrupt:
        print(f"\nStopped. Received {count} packet(s) total.")
    finally:
        sock.close()


if __name__ == "__main__":
    main()
