#!/usr/bin/env python3
"""
http_server.py — minimal HTTPS server for the board's periodic HTTP client.

The board (NetXDuo/App/app_netxduo.c, App_HTTP_Thread_Entry) opens a fresh
TLS connection every HTTP_POLL_PERIOD_MS milliseconds and POSTs one
combined JSON reading of the whole onboard sensor suite (Core/Src/sensors.c,
Sensors_ReadAllJSON() -- one key per category: temperature, humidity,
pressure, accelerometer, gyroscope, magnetometer, light) to HTTP_RESOURCE
("/sensors") at HTTP_SERVER_ADDRESS:HTTP_SERVER_HTTPS_PORT. This answers
POSTs by looking up each top-level key in CATEGORY_FORMATTERS below and
logging the reading in that category's own shape (falling back to a plain
key=value dump for an unrecognized category, or printing the raw body
verbatim if it isn't valid JSON at all -- e.g. an older firmware build
still sending one-object-per-resource, or the even older plain decimal
counter, this used to send), and still answers plain GETs (from a browser
or curl) with a small greeting — either way it logs the request, enough to
confirm the board is actually reaching this machine, now over an
encrypted connection.

The TLS side is deliberately pinned narrow, to match exactly what the
board's NetX Secure TLS stack can do: its ciphersuite table
(nx_crypto_tls_ciphers -- see NetXDuo/App/nx_secure_user.h, no ECC/AEAD
ciphers enabled) offers only TLS 1.2 with static-RSA key exchange, AES-CBC,
HMAC-SHA256 -- i.e. TLS_RSA_WITH_AES_{128,256}_CBC_SHA256, OpenSSL names
AES128-SHA256 / AES256-SHA256. Modern openssl/Python defaults would happily
negotiate TLS 1.3 or an ECDHE suite instead, which the board can't do at
all, so both the version and the cipher list are pinned explicitly below.

Needs a cert/key pair first -- run tools/gen_https_cert.sh once (or again,
if HTTP_SERVER_ADDRESS/HOST in app_netxduo.h ever changes: the board's
trusted root is the exact DER bytes of tools/certs/server.crt, baked into
NetXDuo/App/https_ca_cert.h at that same generation step).

Usage:
    python3 tools/gen_https_cert.sh                # once, or after the IP changes
    python3 tools/http_server.py                    # listen on 0.0.0.0:8443
    python3 tools/http_server.py --port 8443

Stop with Ctrl+C.
"""

import argparse
import json
import os
import socket
import ssl
import sys
import time
from http.server import BaseHTTPRequestHandler, HTTPServer


def guess_local_ip() -> str:
    """Best-effort guess at this machine's LAN-facing IP (the one
    HTTP_SERVER_ADDRESS in app_netxduo.h should point at)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except OSError:
        return "unknown (no network route — are you connected to the hotspot?)"
    finally:
        s.close()


def format_generic(reading: dict) -> str:
    """Fallback formatter: just key=value for every field, in whatever
    shape the reading actually has -- used for any category that doesn't
    need special-cased formatting (temperature, humidity, pressure, light),
    and for any category this server doesn't recognize at all."""
    if not reading:
        return "(empty reading)"
    return ", ".join(f"{k}={v}" for k, v in reading.items())


def format_axes(unit_label: str):
    """Builds a formatter for one of the three ISM330DHCX/IIS2MDC axis
    readings (accelerometer/gyroscope/magnetometer) -- each is just
    {"x":N,"y":N,"z":N} in a different unit."""
    def fmt(reading: dict) -> str:
        return f"x={reading.get('x')}, y={reading.get('y')}, z={reading.get('z')} ({unit_label})"
    return fmt


def format_ranging(reading: dict) -> str:
    zones = reading.get("zones")
    if zones is None:
        return format_generic(reading)
    return f"{len(zones)} zone(s): {zones}"


# One formatter per top-level key Sensors_ReadAllJSON() (Core/Src/sensors.c)
# can put in the combined reading -- an unrecognized key (e.g. a future
# sensor category) just falls back to format_generic via do_POST's
# dict.get() below rather than failing.
CATEGORY_FORMATTERS = {
    "temperature": format_generic,
    "humidity": format_generic,
    "pressure": format_generic,
    "accelerometer": format_axes("mg"),
    "gyroscope": format_axes("mdps"),
    "magnetometer": format_axes("mgauss"),
    "light": format_generic,
    "ranging": format_ranging,
}


class Handler(BaseHTTPRequestHandler):
    # BaseHTTPRequestHandler already logs to stderr via log_message; keep
    # that default (it includes client address and timestamp) and just
    # tack on which resource was requested.
    def do_GET(self):
        body = f"Hello from {socket.gethostname()}, you asked for {self.path}\n".encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        # The board (App_HTTP_Thread_Entry / sensors.c) POSTs one combined
        # JSON object per request -- {"temperature":{...},
        # "accelerometer":{...}, ...}, one key per sensor category that
        # had data this round -- no other encoding, so just read exactly
        # Content-Length bytes back off the socket, then print each
        # top-level key using CATEGORY_FORMATTERS to know its shape.
        length = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(length)

        try:
            reading = json.loads(raw.decode())
        except (ValueError, UnicodeDecodeError) as e:
            # Not JSON -- print it verbatim rather than fail the request;
            # this is also what an even older firmware build's plain
            # decimal counter looks like.
            print(f"    non-JSON body on {self.path} ({e}): {raw!r}", flush=True)
            reply = b"ack (unparsed)\n"
        else:
            if not isinstance(reading, dict):
                print(f"    {self.path}: {reading!r}", flush=True)
            elif not reading:
                print(f"    {self.path}: (empty reading)", flush=True)
            else:
                for category, value in reading.items():
                    formatter = CATEGORY_FORMATTERS.get(category, format_generic)
                    try:
                        line = formatter(value) if isinstance(value, dict) else repr(value)
                    except Exception as e:
                        line = f"(couldn't format: {e}) {value!r}"
                    print(f"    {category}: {line}", flush=True)
            reply = b"ack\n"

        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(reply)))
        self.end_headers()
        self.wfile.write(reply)

    def log_message(self, format, *args):
        ts = time.strftime("%H:%M:%S")
        print(f"[{ts}] {self.address_string()} -> {format % args}", flush=True)


class HTTPSServer(HTTPServer):
    """HTTPServer that wraps each *accepted connection* in TLS individually,
    rather than wrapping the listening socket itself.

    Wrapping the listening socket (ctx.wrap_socket(server.socket, ...)) makes
    accept() perform the TLS handshake automatically, inline -- convenient,
    but fragile: this server is single-threaded, so if any one client sends
    a malformed or incompatible ClientHello, that accept() call itself
    raises (or worse, hangs) *before* socketserver's own request loop gets a
    chance to isolate the failure, silently wedging the server for every
    later connection too, with nothing printed to explain why. Wrapping
    per-connection here means a bad handshake from one client only ever
    fails that one get_request() call -- socketserver's own loop (which
    already treats OSError, the parent class of ssl.SSLError, as "drop this
    one and keep serving") handles the rest, and we get a log line either
    way.
    """

    def __init__(self, server_address, handler_cls, ssl_context):
        self.ssl_context = ssl_context
        super().__init__(server_address, handler_cls)

    def get_request(self):
        conn, addr = super().get_request()
        try:
            tls_conn = self.ssl_context.wrap_socket(conn, server_side=True)
            # Proof this is a real, negotiated TLS channel (not merely "on
            # port 8443") -- exactly what a browser's padlock/certificate
            # details show: the actual protocol version and cipher suite
            # this specific connection settled on, straight from OpenSSL
            # itself, not from anything we assumed or configured.
            cipher_name, tls_version, secret_bits = tls_conn.cipher()
            print(f"[{time.strftime('%H:%M:%S')}] {addr[0]} -> TLS established: "
                  f"{tls_conn.version()} / {cipher_name} ({secret_bits}-bit)", flush=True)
            return tls_conn, addr
        except (ssl.SSLError, OSError) as e:
            print(f"[{time.strftime('%H:%M:%S')}] {addr[0]} -> TLS handshake failed: {e}", flush=True)
            conn.close()
            raise


def main() -> None:
    here = os.path.dirname(os.path.abspath(__file__))
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--host", default="0.0.0.0",
                         help="local address to bind (default: 0.0.0.0, all interfaces)")
    parser.add_argument("--port", type=int, default=8443,
                         help="TCP port to listen on (default: 8443, matches app_netxduo.h HTTP_SERVER_HTTPS_PORT)")
    parser.add_argument("--certfile", default=os.path.join(here, "certs", "server.crt"),
                         help="server certificate (default: tools/certs/server.crt)")
    parser.add_argument("--keyfile", default=os.path.join(here, "certs", "server.key"),
                         help="server private key (default: tools/certs/server.key)")
    args = parser.parse_args()

    if not (os.path.isfile(args.certfile) and os.path.isfile(args.keyfile)):
        print(f"Missing {args.certfile} / {args.keyfile} -- run tools/gen_https_cert.sh first.",
              file=sys.stderr)
        sys.exit(1)

    # Pinned to exactly what the board's NetX Secure TLS stack can
    # negotiate -- see the module docstring above.
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(certfile=args.certfile, keyfile=args.keyfile)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    ctx.maximum_version = ssl.TLSVersion.TLSv1_2
    ctx.set_ciphers("AES128-SHA256:AES256-SHA256")

    server = HTTPSServer((args.host, args.port), Handler, ctx)

    print(f"Listening on TLS 1.2 (AES128-SHA256:AES256-SHA256) {args.host}:{args.port}", flush=True)
    print(f"Certificate: {args.certfile}", flush=True)
    print(f"This machine's LAN-facing IP looks like: {guess_local_ip()}", flush=True)
    print("  -> HTTP_SERVER_ADDRESS in NetXDuo/App/app_netxduo.h must match this, on the same network as the board,", flush=True)
    print("     and must match what tools/gen_https_cert.sh signed the certificate for.", flush=True)
    print("Waiting for requests from the board... Ctrl+C to stop.\n", flush=True)

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
