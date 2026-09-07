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
counter, this used to send) -- either way it logs the request, enough to
confirm the board is actually reaching this machine, now over an
encrypted connection.

GET / serves dashboard.html, a live browser dashboard with two tabs
(sidebar, left) -- open https://<this machine's IP>:<port>/ in a browser
(self-signed cert, so it'll warn once; proceed past it):
  - Dashboard: one card per sensor category, polling GET /api/latest
    every 500ms, showing whatever the board's most recent POST
    contained. Clicking a card opens a live-updating chart of that
    category's history (GET /api/history?category=<name>, polled while
    the chart is open) -- see HISTORY_WINDOW_SECONDS below for how much
    is kept.
  - Cryptography: the cipher suites offered (GET /api/latest's
    "board_tls" field, from _tls_by_ip below, shows the board's real
    last-negotiated version/cipher/bits), the server's actual certificate
    and key pair (GET /api/cert-info -- parses tools/certs/server.crt/
    .key fresh on every request via the `cryptography` package; the
    private key's raw bytes are deliberately never included, type/size
    only), and a real captured (ciphertext, plaintext) pair from an
    actual board connection (GET /api/crypto-sample, reading
    tools/crypto_sample.json -- see tools/capture_crypto_sample.py for
    how and why that has to be captured as a separate one-off script
    rather than something this server does continuously).
Any other GET path still gets the old plaintext greeting, e.g. for a
quick curl-reachability check.

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
import collections
import json
import os
import socket
import socketserver
import ssl
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import urlparse, parse_qs

# Only used by _serve_cert_info_json, to parse tools/certs/server.crt/.key
# for the Cryptography tab -- guarded because it's the one dependency
# this file needs beyond the standard library, and the rest of the
# server has no reason to fail to start without it.
try:
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    _HAVE_CRYPTOGRAPHY = True
except ImportError:
    _HAVE_CRYPTOGRAPHY = False

# Bounds every accepted connection's TLS handshake and body read (see
# HTTPSServer below) -- generous next to any real handshake+POST cycle
# (observed: well under 2s), but short enough that a connection that's
# genuinely never going to finish gets dropped in seconds, not never.
CONNECTION_TIMEOUT_SECONDS = 10

# How much reading history the chart view (GET /api/history) can show --
# a diagnostic tool's rolling window, not a long-term data log. At the
# board's observed ~1 reading/second cadence this is ~10 minutes;
# HISTORY_MAX_ENTRIES is a hard backstop on top (count, not time) in case
# the board's poll period is ever tightened well below 1s.
HISTORY_WINDOW_SECONDS = 600
HISTORY_MAX_ENTRIES = 2000

# The most recent successfully-parsed reading, and a rolling window of
# past ones, for the browser dashboard (GET / -- dashboard.html) to show:
# /api/latest for the live tiles, /api/history for a clicked card's chart.
# Written by do_POST, read by do_GET; HTTPSServer is threaded (one thread
# per connection), so every access goes through _state_lock rather than
# assuming only one request is ever in flight at a time.
_state_lock = threading.Lock()
_latest_reading = None    # dict, or None before the first POST ever arrives
_latest_at_ms = None      # int, time.time()*1000 when _latest_reading was set
_latest_source = None     # str, the board's IP at that time
_history = collections.deque(maxlen=HISTORY_MAX_ENTRIES)  # [(t_ms, reading_dict), ...], oldest first

# Set once in main() from --certfile/--keyfile; read by _serve_cert_info_json
# (parses the cert/key fresh on every request, same "always current, no
# caching" rule as dashboard.html) and _serve_crypto_sample_json (locates
# the sibling crypto_sample.json).
_cert_path = None
_key_path = None

# Actual negotiated TLS parameters (version/cipher/bits), per source IP,
# for the dashboard's Cryptography tab to show real live numbers instead
# of just a description of what's configured. Keyed by IP rather than one
# single "last connection" value because the browser's own polling
# (GET /api/latest every 500ms) would otherwise completely drown out the
# board's much rarer connections -- looking this up by _latest_source (the
# board's own IP, from do_POST) gets the board's negotiated parameters
# specifically, regardless of how many browser polls happened since.
# Unbounded but self-limiting in practice: one entry per distinct client
# IP that has ever connected, which on a private board<->laptop link is a
# small, fixed set (the board and whoever's browser).
_tls_by_ip = {}


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
        path = urlparse(self.path).path
        if path == "/":
            self._serve_dashboard()
        elif path == "/api/latest":
            self._serve_latest_json()
        elif path == "/api/history":
            self._serve_history_json()
        elif path == "/api/cert-info":
            self._serve_cert_info_json()
        elif path == "/api/crypto-sample":
            self._serve_crypto_sample_json()
        else:
            body = f"Hello from {socket.gethostname()}, you asked for {self.path}\n".encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    def _serve_dashboard(self):
        # Read dashboard.html fresh on every request (it's a handful of KB,
        # this isn't a hot path) rather than caching it in memory, so
        # editing the file takes effect on the next browser refresh with
        # no need to restart this server.
        path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "dashboard.html")
        try:
            with open(path, "rb") as f:
                body = f.read()
        except OSError as e:
            body = f"dashboard.html missing or unreadable: {e}\n".encode()
            self.send_response(500)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        # This page changes as this tool evolves and is re-read fresh off
        # disk on every request (see the comment above) specifically so
        # edits take effect immediately -- a browser caching an old copy
        # (nothing here sent Cache-Control before, so caching behavior was
        # left entirely up to each browser's own heuristics) would defeat
        # that and silently serve a stale page/script indefinitely.
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _serve_latest_json(self):
        # dashboard.html polls this every 500ms. Snapshot the shared state
        # under the lock rather than holding it while we serialize/write,
        # so a POST arriving mid-response never blocks on us (or us on it)
        # for longer than a dict copy.
        with _state_lock:
            reading, updated_at_ms, source = _latest_reading, _latest_at_ms, _latest_source
            board_tls = _tls_by_ip.get(source) if source else None
        payload = json.dumps({
            "reading": reading,
            "updated_at_ms": updated_at_ms,
            "source": source,
            "board_tls": board_tls,
        }).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(payload)

    def _serve_history_json(self):
        # Backs a clicked card's chart (dashboard.html polls this every
        # ~1s while a chart is open). ?category=<name> narrows each
        # history entry down to just that category's own sub-object
        # (e.g. {"x":...,"y":...,"z":...} for accelerometer) -- entries
        # from a round where that category had no data are skipped
        # entirely rather than sent as null, same "only real data" rule
        # everything else here follows. Without ?category, the full
        # combined reading is returned per entry instead (mainly useful
        # for poking at this endpoint directly with curl).
        category = (parse_qs(urlparse(self.path).query).get("category") or [None])[0]
        with _state_lock:
            snapshot = list(_history)
        if category:
            entries = [{"t": t, "v": r[category]} for t, r in snapshot if category in r]
        else:
            entries = [{"t": t, "reading": r} for t, r in snapshot]
        payload = json.dumps({"entries": entries}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(payload)

    def _serve_cert_info_json(self):
        # Backs the Cryptography tab's certificate/key panel. Parses
        # tools/certs/server.crt and .key fresh on every request (same
        # "always current" rule as dashboard.html -- if someone reruns
        # gen_https_cert.sh, the next page load just shows the new one)
        # rather than caching anything at startup.
        info = {}
        if not _HAVE_CRYPTOGRAPHY:
            info["error"] = "the 'cryptography' package isn't installed on this machine (pip install cryptography)"
        else:
            try:
                with open(_cert_path, "rb") as f:
                    cert_pem = f.read()
                cert = x509.load_pem_x509_certificate(cert_pem)
                pub = cert.public_key()
                numbers = pub.public_numbers()
                info["subject"] = cert.subject.rfc4514_string()
                info["issuer"] = cert.issuer.rfc4514_string()
                info["serial_number"] = format(cert.serial_number, "X")
                # .not_valid_before/.not_valid_after (naive, UTC) rather
                # than the _utc-suffixed properties: those were only added
                # in `cryptography` 42, and this machine has 41.x.
                info["not_valid_before"] = cert.not_valid_before.isoformat() + "Z"
                info["not_valid_after"] = cert.not_valid_after.isoformat() + "Z"
                info["sha256_fingerprint"] = ":".join(f"{b:02X}" for b in cert.fingerprint(hashes.SHA256()))
                info["public_key_algorithm"] = "RSA"
                info["public_key_bits"] = pub.key_size
                info["public_key_exponent"] = numbers.e
                modulus_hex = format(numbers.n, "X")
                info["public_key_modulus_hex"] = modulus_hex
                info["pem"] = cert_pem.decode()
            except (OSError, ValueError, AttributeError) as e:
                info["error"] = f"couldn't read/parse {_cert_path}: {e}"

            # Private key: type/size only, on purpose -- see dashboard.html's
            # Cryptography tab for why the raw key material itself is never
            # served, even locally. This isn't a hypothetical caution: the
            # board's own connection already has no forward secrecy (static
            # RSA key exchange), so this one file is the single point of
            # failure for every past and future session with the board --
            # putting its bytes on an HTTP response, on a dev laptop, on a
            # shared hotspot, for a dashboard visualization, isn't a
            # trade worth making.
            try:
                with open(_key_path, "rb") as f:
                    key_pem = f.read()
                key = serialization.load_pem_private_key(key_pem, password=None)
                info["private_key_algorithm"] = "RSA"
                info["private_key_bits"] = key.key_size
                info["private_key_location"] = os.path.relpath(_key_path, os.path.dirname(_cert_path or "."))
            except (OSError, ValueError) as e:
                info["private_key_error"] = f"couldn't read/parse {_key_path}: {e}"

        payload = json.dumps(info).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(payload)

    def _serve_crypto_sample_json(self):
        # Backs the Cryptography tab's ciphertext/plaintext panel -- a
        # real (not simulated) TLS record captured from an actual board
        # connection by tools/capture_crypto_sample.py (see that file for
        # why this has to be a separate one-off capture rather than
        # something this server does continuously). Just relays whatever
        # is currently in tools/crypto_sample.json; a 404-shaped response
        # if that capture has never been run yet.
        path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "crypto_sample.json")
        try:
            with open(path, "rb") as f:
                payload = f.read()
            status = 200
        except OSError:
            payload = json.dumps({
                "error": "no capture yet -- run tools/capture_crypto_sample.py (see its docstring)",
            }).encode()
            status = 200  # still 200: this is a normal, expected state before the first capture
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(payload)

    def do_POST(self):
        # The board (App_HTTP_Thread_Entry / sensors.c) POSTs one combined
        # JSON object per request -- {"temperature":{...},
        # "accelerometer":{...}, ...}, one key per sensor category that
        # had data this round -- no other encoding, so just read exactly
        # Content-Length bytes back off the socket, then print each
        # top-level key using CATEGORY_FORMATTERS to know its shape.
        length = int(self.headers.get("Content-Length", 0))
        try:
            raw = self.rfile.read(length)
        except OSError as e:
            # Covers socket.timeout (CONNECTION_TIMEOUT_SECONDS, set in
            # HTTPSServer.get_request()) among other things -- print a
            # clean line instead of letting socketserver's default
            # handle_error() dump a full traceback for what's really just
            # one dropped/incomplete connection.
            print(f"    {self.path}: read failed/timed out ({e}) -- dropping this connection", flush=True)
            return

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

            if isinstance(reading, dict) and reading:
                # Feed the browser dashboard (GET / -> dashboard.html,
                # polling GET /api/latest for the live tiles and
                # GET /api/history for a clicked card's chart) -- only for
                # an actual sensor reading, not an empty or non-dict body.
                global _latest_reading, _latest_at_ms, _latest_source
                now_ms = int(time.time() * 1000)
                with _state_lock:
                    _latest_reading = reading
                    _latest_at_ms = now_ms
                    _latest_source = self.client_address[0]
                    _history.append((now_ms, reading))
                    cutoff = now_ms - HISTORY_WINDOW_SECONDS * 1000
                    while _history and _history[0][0] < cutoff:
                        _history.popleft()

            reply = b"ack\n"

        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(reply)))
        self.end_headers()
        self.wfile.write(reply)

    def log_message(self, format, *args):
        ts = time.strftime("%H:%M:%S")
        print(f"[{ts}] {self.address_string()} -> {format % args}", flush=True)


class HTTPSServer(socketserver.ThreadingMixIn, HTTPServer):
    """HTTPServer that wraps each *accepted connection* in TLS individually,
    rather than wrapping the listening socket itself, and handles each
    connection on its own thread.

    Wrapping the listening socket (ctx.wrap_socket(server.socket, ...)) makes
    accept() perform the TLS handshake automatically, inline -- convenient,
    but fragile: if any one client sends a malformed or incompatible
    ClientHello, that accept() call itself raises (or worse, hangs) *before*
    socketserver's own request loop gets a chance to isolate the failure.
    Wrapping per-connection here means a bad handshake from one client only
    ever fails that one get_request() call -- socketserver's own loop
    (which already treats OSError, the parent class of ssl.SSLError, as
    "drop this one and keep serving") handles the rest, and we get a log
    line either way.

    That still isn't the whole story, though -- real-hardware testing
    found the board occasionally sitting with TCP fully established but
    the TLS handshake never progressing at all, for as long as it was left
    running, recovering only when this server process itself was killed
    and restarted. Nothing here was actually the board's fault: plain
    HTTPServer (even with the per-connection wrap above) is still
    single-threaded and serializes every connection through one
    accept-handle-repeat loop, and neither the TLS handshake
    (wrap_socket's do_handshake_on_connect) nor a handler's own
    self.rfile.read() in do_POST() had any timeout at all -- so if either
    one ever blocked on a single connection (a dropped byte, a partial
    body, any read that doesn't fully complete), this process's *only*
    thread never returned to accept() again, and every later connection
    from the board just sat fully established in the OS's own backlog,
    never reaching this code at all, forever. Killing the process was
    the only way out because that's what dropped the backlog too.
    ThreadingMixIn (each connection handled on its own thread, so one
    stuck connection can never block any other) plus a hard timeout on
    every connection (CONNECTION_TIMEOUT_SECONDS, set before the TLS
    handshake even starts, so it bounds that *and* every read/write in
    do_POST/do_GET) together close both ends of that gap.
    """

    daemon_threads = True  # so a still-stuck handler thread never blocks process exit (Ctrl+C)

    def __init__(self, server_address, handler_cls, ssl_context):
        self.ssl_context = ssl_context
        super().__init__(server_address, handler_cls)

    def get_request(self):
        conn, addr = super().get_request()
        conn.settimeout(CONNECTION_TIMEOUT_SECONDS)
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
            with _state_lock:
                _tls_by_ip[addr[0]] = {
                    "version": tls_conn.version(),
                    "cipher": cipher_name,
                    "bits": secret_bits,
                    "at_ms": int(time.time() * 1000),
                }
            return tls_conn, addr
        except (ssl.SSLError, OSError) as e:
            print(f"[{time.strftime('%H:%M:%S')}] {addr[0]} -> TLS handshake failed or timed out: {e}", flush=True)
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

    global _cert_path, _key_path
    _cert_path, _key_path = args.certfile, args.keyfile

    # Pinned to exactly what the board's NetX Secure TLS stack can
    # negotiate -- see the module docstring above.
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(certfile=args.certfile, keyfile=args.keyfile)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    ctx.maximum_version = ssl.TLSVersion.TLSv1_2
    # AES128-SHA256/AES256-SHA256 (static-RSA key exchange, no forward
    # secrecy) are the only two ciphers the board's NetX Secure stack can
    # do at all -- but modern browsers have deliberately stopped offering
    # non-forward-secret suites like these for years, so a real browser
    # hitting this same port for the dashboard (see do_GET's "/" and
    # "/api/latest" routes) gets a hard TLS-layer refusal
    # (SSL_ERROR_NO_CYPHER_OVERLAP in Firefox) with no cert-warning
    # click-through to bypass, since there's no shared cipher at all, not
    # just an untrusted cert. Adding two ECDHE suites alongside fixes both
    # sides on one port/one cert: the board still only ever offers (and
    # gets) the old RSA suites, since ECDHE isn't in its list, while a
    # browser negotiates one of the ECDHE ones instead -- the same 2048-bit
    # RSA cert signs both key exchange types, so no new cert is needed.
    ctx.set_ciphers("AES128-SHA256:AES256-SHA256:ECDHE-RSA-AES128-GCM-SHA256:ECDHE-RSA-AES256-GCM-SHA384")

    server = HTTPSServer((args.host, args.port), Handler, ctx)
    local_ip = guess_local_ip()

    print(f"Listening on TLS 1.2 (AES128-SHA256:AES256-SHA256) {args.host}:{args.port}", flush=True)
    print(f"Certificate: {args.certfile}", flush=True)
    print(f"This machine's LAN-facing IP looks like: {local_ip}", flush=True)
    print("  -> HTTP_SERVER_ADDRESS in NetXDuo/App/app_netxduo.h must match this, on the same network as the board,", flush=True)
    print("     and must match what tools/gen_https_cert.sh signed the certificate for.", flush=True)
    print(f"Dashboard: https://{local_ip}:{args.port}/ (browser will warn on the self-signed cert -- proceed past it)", flush=True)
    print("Waiting for requests from the board... Ctrl+C to stop.\n", flush=True)

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
