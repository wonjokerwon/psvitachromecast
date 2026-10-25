#!/usr/bin/env python3
"""Tests the Cast sender (used by VitaCast Mirror) against a mock Chromecast.

The mock speaks CASTV2 over TLS like a real device: it reports a running
app, handles STOP/LAUNCH/GET_STATUS, and records every message so the test
can check the exact sequence the sender produced.

usage: sender_test.py path/to/vitacast-host
"""

import datetime
import json
import os
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import threading

try:
    from pychromecast.generated import cast_channel_pb2
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    from cryptography.x509.oid import NameOID
except ImportError:
    print("SKIP: pip install pychromecast cryptography")
    sys.exit(0)

RECEIVER_NS = "urn:x-cast:com.google.cast.receiver"
DASHCAST_NS = "urn:x-cast:com.madmod.dashcast"
FAILURES = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        FAILURES.append(what)


def make_cert(directory):
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "mock-chromecast")])
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (
        x509.CertificateBuilder()
        .subject_name(name)
        .issuer_name(name)
        .public_key(key.public_key())
        .serial_number(1)
        .not_valid_before(now - datetime.timedelta(days=1))
        .not_valid_after(now + datetime.timedelta(days=1))
        .sign(key, hashes.SHA256())
    )
    cert_path = os.path.join(directory, "cert.pem")
    key_path = os.path.join(directory, "key.pem")
    with open(cert_path, "wb") as f:
        f.write(cert.public_bytes(serialization.Encoding.PEM))
    with open(key_path, "wb") as f:
        f.write(
            key.private_bytes(
                serialization.Encoding.PEM,
                serialization.PrivateFormat.TraditionalOpenSSL,
                serialization.NoEncryption(),
            )
        )
    return cert_path, key_path


class MockChromecast:
    def __init__(self, directory, reject_launch=False):
        cert, key = make_cert(directory)
        self.ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.ctx.load_cert_chain(cert, key)
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(4)
        self.port = self.sock.getsockname()[1]
        self.reject_launch = reject_launch
        self.app = {"appId": "CC1AD845", "sessionId": "old-session", "transportId": "web-1"}
        self.messages = []
        threading.Thread(target=self.serve, daemon=True).start()

    def status(self, request_id):
        apps = [dict(self.app, displayName="App", namespaces=[], isIdleScreen=False)] if self.app else []
        return {
            "type": "RECEIVER_STATUS",
            "requestId": request_id,
            "status": {"applications": apps, "volume": {"level": 1.0, "muted": False}},
        }

    def serve(self):
        while True:
            try:
                raw, _ = self.sock.accept()
            except OSError:
                return
            threading.Thread(target=self.handle, args=(raw,), daemon=True).start()

    def send(self, conn, src, dst, ns, payload):
        msg = cast_channel_pb2.CastMessage()
        msg.protocol_version = msg.CASTV2_1_0
        msg.source_id = src
        msg.destination_id = dst
        msg.namespace = ns
        msg.payload_type = msg.STRING
        msg.payload_utf8 = json.dumps(payload)
        data = msg.SerializeToString()
        conn.sendall(struct.pack(">I", len(data)) + data)

    def handle(self, raw):
        try:
            conn = self.ctx.wrap_socket(raw, server_side=True)
        except (ssl.SSLError, OSError):
            return
        buf = b""
        try:
            while True:
                chunk = conn.recv(65536)
                if not chunk:
                    return
                buf += chunk
                while len(buf) >= 4:
                    (n,) = struct.unpack(">I", buf[:4])
                    if len(buf) < 4 + n:
                        break
                    msg = cast_channel_pb2.CastMessage()
                    msg.ParseFromString(buf[4 : 4 + n])
                    buf = buf[4 + n :]
                    self.on_message(conn, msg)
        except (OSError, ssl.SSLError):
            return

    def on_message(self, conn, msg):
        data = json.loads(msg.payload_utf8) if msg.payload_utf8 else {}
        self.messages.append((msg.destination_id, msg.namespace, data))
        if msg.namespace != RECEIVER_NS:
            return
        t, rid = data.get("type"), data.get("requestId", 0)
        if t == "GET_STATUS":
            self.send(conn, "receiver-0", msg.source_id, RECEIVER_NS, self.status(rid))
        elif t == "STOP":
            if self.app and data.get("sessionId") == self.app["sessionId"]:
                self.app = None
            self.send(conn, "receiver-0", msg.source_id, RECEIVER_NS, self.status(rid))
        elif t == "LAUNCH":
            if self.reject_launch:
                self.send(conn, "receiver-0", msg.source_id, RECEIVER_NS,
                          {"type": "LAUNCH_ERROR", "requestId": rid, "reason": "NOT_FOUND"})
                return
            # Real devices first broadcast an unrelated status, then the launched app.
            self.send(conn, "receiver-0", "*", RECEIVER_NS, self.status(0))
            self.app = {"appId": data["appId"], "sessionId": "dash-session", "transportId": "web-42"}
            self.send(conn, "receiver-0", msg.source_id, RECEIVER_NS, self.status(rid))


def run(binary, data_dir, *args):
    return subprocess.run([binary, "--quiet", "--data", data_dir, *args],
                          capture_output=True, text=True, timeout=60)


def main():
    binary = os.path.abspath(sys.argv[1])
    work = tempfile.mkdtemp(prefix="vitacast-sender-")
    url = "http://192.168.1.50:8080/"

    print("Cast sender: open URL through DashCast")
    tv = MockChromecast(work)
    r = run(binary, work, "--cast-url", f"127.0.0.1:{tv.port}", url)
    check(r.returncode == 0, f"cast-url succeeds ({r.stderr.strip()})")
    seq = [(d, ns.rsplit(".", 1)[-1], m.get("type")) for d, ns, m in tv.messages]
    stops = [m for d, ns, m in tv.messages if m.get("type") == "STOP"]
    check(bool(stops) and stops[0].get("sessionId") == "old-session", "stops the running app first")
    launches = [m for d, ns, m in tv.messages if m.get("type") == "LAUNCH"]
    check(bool(launches) and launches[0].get("appId") == "84912283", "launches DashCast (84912283)")
    check(("web-42", "connection", "CONNECT") in seq, "connects to the DashCast transport")
    loads = [(d, m) for d, ns, m in tv.messages if ns == DASHCAST_NS]
    check(len(loads) == 1, "sends one DashCast message")
    if loads:
        d, m = loads[0]
        check(d == "web-42", "DashCast message goes to the app transport")
        check(m.get("url") == url and m.get("force") is True, "asks DashCast to force-load the mirror URL")
        check(m.get("sessionId") == "dash-session", "includes the DashCast session id")
    order = [m.get("type") for d, ns, m in tv.messages if ns == RECEIVER_NS]
    check(order.index("STOP") < order.index("LAUNCH"), "STOP happens before LAUNCH")

    print("Cast sender: stop")
    r = run(binary, work, "--stop-cast", f"127.0.0.1:{tv.port}")
    check(r.returncode == 0, "stop-cast succeeds")
    check(tv.app is None, "the TV is back to no running app")

    print("Cast sender: errors")
    bad = MockChromecast(work, reject_launch=True)
    r = run(binary, work, "--cast-url", f"127.0.0.1:{bad.port}", url)
    check(r.returncode != 0 and "refused" in r.stderr, "LAUNCH_ERROR is reported")
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    closed_port = s.getsockname()[1]
    s.close()
    r = run(binary, work, "--cast-url", f"127.0.0.1:{closed_port}", url)
    check(r.returncode != 0 and "cannot connect" in r.stderr, "unreachable TV is reported")

    if FAILURES:
        print(f"{len(FAILURES)} failure(s)")
        sys.exit(1)
    print("all sender checks passed")


if __name__ == "__main__":
    main()
