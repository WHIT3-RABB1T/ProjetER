#!/usr/bin/env python3
"""capture_crypto_sample.py -- one-off capture of a real (ciphertext,
plaintext) pair from an actual board connection, for the dashboard's
Cryptography tab (GET /api/crypto-sample in http_server.py reads the
JSON file this produces).

Why this exists as a separate script, and why it's not live/continuous:
ssl.SSLContext.wrap_socket() -- what http_server.py actually uses for
every real connection -- does its handshake and all reads/writes
directly against the OS file descriptor inside OpenSSL's C code. The
raw encrypted bytes never pass through any Python-level socket method,
so there's no way to sniff them from the outside (confirmed empirically:
subclassing socket.socket and overriding recv()/send() is simply never
called). The only way to see the actual ciphertext in Python is
ssl.MemoryBIO -- manually pumping encrypted bytes between the socket and
the TLS state machine yourself, which this script does. That's a much
more invasive way to run a TLS server than the tidy, hard-won-stable
wrap_socket()-based HTTPSServer in http_server.py, so rather than risk
that stability for a dashboard visualization, this captures ONE real
sample as a standalone, temporary process and saves it to disk -- the
live server goes back to its normal, unmodified code path immediately
after.

Usage (stop the running http_server.py first -- they can't both bind
:8443 at once):
    python3 tools/capture_crypto_sample.py
    # waits up to 60s for the board's next POST /sensors, saves
    # tools/crypto_sample.json, then exits -- restart http_server.py
    # once it prints "SAMPLE CAPTURED".

Needs the same tools/certs/server.{crt,key} http_server.py uses (see
tools/gen_https_cert.sh) -- this presents the exact same certificate the
board already trusts, so no firmware changes or reflash are needed.
"""
import ssl
import socket
import os
import json
import time
import binascii
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CERT = os.path.join(HERE, "certs", "server.crt")
KEY = os.path.join(HERE, "certs", "server.key")
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "crypto_sample.json")
PORT = 8443
ACCEPT_TIMEOUT_SECONDS = 60


def main():
    if not (os.path.isfile(CERT) and os.path.isfile(KEY)):
        sys.exit(f"Missing {CERT} / {KEY} -- run tools/gen_https_cert.sh first.")

    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(CERT, KEY)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    ctx.maximum_version = ssl.TLSVersion.TLSv1_2
    # Same four suites as http_server.py, so whichever real client shows
    # up (the board, or a browser looking at the dashboard) negotiates
    # exactly what it would against the real server.
    ctx.set_ciphers("AES128-SHA256:AES256-SHA256:ECDHE-RSA-AES128-GCM-SHA256:ECDHE-RSA-AES256-GCM-SHA384")

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", PORT))
    srv.listen(5)
    srv.settimeout(ACCEPT_TIMEOUT_SECONDS)
    print(f"waiting for a real POST /sensors from the board (up to {ACCEPT_TIMEOUT_SECONDS}s)...", flush=True)

    while True:
        conn, addr = srv.accept()
        conn.settimeout(15)
        print(f"connection from {addr}", flush=True)

        incoming = ssl.MemoryBIO()
        outgoing = ssl.MemoryBIO()
        sslobj = ctx.wrap_bio(incoming, outgoing, server_side=True)
        hs_in = hs_out = b""

        def pump_out():
            nonlocal hs_out
            d = outgoing.read()
            if d:
                hs_out += d
                conn.sendall(d)

        def pump_in():
            nonlocal hs_in
            d = conn.recv(65536)
            if not d:
                raise ConnectionError("peer closed during handshake")
            hs_in += d
            incoming.write(d)

        try:
            while True:
                try:
                    sslobj.do_handshake()
                    pump_out()
                    break
                except ssl.SSLWantReadError:
                    pump_out()
                    pump_in()
        except Exception as e:
            print(f"  handshake failed/aborted: {e} -- listening for next connection", flush=True)
            conn.close()
            continue

        print(f"  handshake OK: {sslobj.version()} / {sslobj.cipher()[0]} ({sslobj.cipher()[2]}-bit)", flush=True)

        app_ciphertext = b""

        def app_pump_in():
            nonlocal app_ciphertext
            d = conn.recv(65536)
            if not d:
                raise ConnectionError("peer closed mid-request")
            app_ciphertext += d
            incoming.write(d)

        try:
            plaintext_buf = b""
            while b"\r\n\r\n" not in plaintext_buf:
                try:
                    chunk = sslobj.read(65536)
                    if not chunk:
                        break
                    plaintext_buf += chunk
                except ssl.SSLWantReadError:
                    app_pump_in()

            header_part, _, rest = plaintext_buf.partition(b"\r\n\r\n")
            headers_text = header_part.decode(errors="replace")
            request_line = headers_text.split("\r\n", 1)[0] if headers_text else ""

            if not request_line.startswith("POST /sensors"):
                print(f"  not a board POST ({request_line!r}) -- listening for next connection", flush=True)
                conn.close()
                continue

            content_length = 0
            for line in headers_text.split("\r\n"):
                if line.lower().startswith("content-length:"):
                    content_length = int(line.split(":", 1)[1].strip())

            body = rest
            while len(body) < content_length:
                try:
                    chunk = sslobj.read(65536)
                    if not chunk:
                        break
                    body += chunk
                except ssl.SSLWantReadError:
                    app_pump_in()

            print(f"  captured real POST /sensors body ({len(body)} bytes)", flush=True)

            resp_plain = b"ack\n"
            resp = (b"HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nContent-Length: " +
                    str(len(resp_plain)).encode() + b"\r\n\r\n" + resp_plain)
            sslobj.write(resp)
            pump_out()

            sample = {
                "captured_at": time.strftime("%Y-%m-%d %H:%M:%S %Z"),
                "client_ip": addr[0],
                "tls_version": sslobj.version(),
                "cipher": sslobj.cipher()[0],
                "bits": sslobj.cipher()[2],
                "handshake_ciphertext_hex": binascii.hexlify(hs_in + hs_out).decode(),
                "handshake_ciphertext_len": len(hs_in) + len(hs_out),
                "handshake_bytes_client_to_server": len(hs_in),
                "handshake_bytes_server_to_client": len(hs_out),
                "request_headers_plaintext": headers_text,
                "request_body_plaintext": body.decode(errors="replace"),
                "app_record_ciphertext_hex": binascii.hexlify(app_ciphertext).decode(),
                "app_record_ciphertext_len": len(app_ciphertext),
            }
            with open(OUT, "w") as f:
                json.dump(sample, f, indent=2)
            print(f"SAMPLE CAPTURED -> {OUT}", flush=True)
            conn.close()
            break
        except Exception as e:
            print(f"  request read failed: {e} -- listening for next connection", flush=True)
            try:
                conn.close()
            except OSError:
                pass
            continue

    srv.close()
    print("done -- restart tools/http_server.py now.", flush=True)


if __name__ == "__main__":
    main()
