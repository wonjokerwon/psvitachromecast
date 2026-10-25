#!/usr/bin/env python3
"""End-to-end test: drives vitacast-host with pychromecast, the Cast client
library used by catt, Home Assistant and mkchromecast.

Covers mDNS discovery, the TLS Cast channel, device authentication, app
launch, LOAD/PLAY/PAUSE/SEEK/STOP, volume, end of media, load errors, the
eureka_info HTTP API, and the HTTP range cache (via --fetch).

usage: e2e_test.py path/to/vitacast-host
"""

import json
import os
import shutil
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
import uuid as uuidlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

try:
    import pychromecast
    from pychromecast.generated import cast_channel_pb2
except ImportError:
    print("SKIP: pip install pychromecast")
    sys.exit(0)

SIM_DURATION = 20.0
MEDIA = os.urandom(3 * 1024 * 1024 + 12345)
FAILURES = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        FAILURES.append(what)


def wait_for(pred, timeout=10.0, step=0.1):
    end = time.time() + timeout
    while time.time() < end:
        if pred():
            return True
        time.sleep(step)
    return pred()


class MediaHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def do_GET(self):
        if self.path.startswith("/redirect"):
            self.send_response(302)
            self.send_header("Location", "/video.mp4")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if self.path.startswith("/missing"):
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        ranges = self.path.startswith("/video.mp4")
        if not ranges and not self.path.startswith("/norange.mp4"):
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        rng = self.headers.get("Range")
        if ranges and rng and rng.startswith("bytes="):
            a, _, b = rng[6:].partition("-")
            start = int(a)
            end = int(b) if b else len(MEDIA) - 1
            end = min(end, len(MEDIA) - 1)
            body = MEDIA[start : end + 1]
            self.send_response(206)
            self.send_header("Content-Range", f"bytes {start}-{end}/{len(MEDIA)}")
        else:
            body = MEDIA
            self.send_response(200)
        self.send_header("Content-Type", "video/mp4")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass


def local_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("10.255.255.255", 1))
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def test_fetch(binary, data_dir, base):
    print("HTTP range cache (--fetch)")
    for path in ("/video.mp4", "/redirect", "/norange.mp4"):
        out = os.path.join(data_dir, "fetched.bin")
        r = subprocess.run(
            [binary, "--quiet", "--data", data_dir, "--fetch", base + path, out],
            capture_output=True,
            timeout=120,
        )
        same = r.returncode == 0 and open(out, "rb").read() == MEDIA
        check(same, f"fetch {path} returns identical bytes")
    r = subprocess.run(
        [binary, "--quiet", "--data", data_dir, "--fetch", base + "/missing", "/dev/null"],
        capture_output=True,
        timeout=60,
    )
    check(r.returncode != 0, "fetch of a 404 fails")


def raw_device_auth(ip):
    """Sends a DeviceAuth challenge like Chrome does and verifies the reply's
    signature over (nonce || TLS certificate) with the returned certificate."""
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.asymmetric import padding

    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    with socket.create_connection((ip, 8009), timeout=10) as raw:
        with ctx.wrap_socket(raw) as s:
            tls_cert = s.getpeercert(binary_form=True)
            nonce = os.urandom(16)
            dam = cast_channel_pb2.DeviceAuthMessage()
            dam.challenge.signature_algorithm = cast_channel_pb2.RSASSA_PKCS1v15
            dam.challenge.sender_nonce = nonce
            dam.challenge.hash_algorithm = cast_channel_pb2.SHA256
            msg = cast_channel_pb2.CastMessage()
            msg.protocol_version = msg.CASTV2_1_0
            msg.source_id = "sender-0"
            msg.destination_id = "receiver-0"
            msg.namespace = "urn:x-cast:com.google.cast.tp.deviceauth"
            msg.payload_type = msg.BINARY
            msg.payload_binary = dam.SerializeToString()
            data = msg.SerializeToString()
            s.sendall(struct.pack(">I", len(data)) + data)
            hdr = b""
            while len(hdr) < 4:
                hdr += s.recv(4 - len(hdr))
            (n,) = struct.unpack(">I", hdr)
            body = b""
            while len(body) < n:
                body += s.recv(n - len(body))
    reply = cast_channel_pb2.CastMessage()
    reply.ParseFromString(body)
    resp = cast_channel_pb2.DeviceAuthMessage()
    resp.ParseFromString(reply.payload_binary)
    check(resp.HasField("response"), "device auth returns an AuthResponse")
    cert = x509.load_der_x509_certificate(resp.response.client_auth_certificate)
    try:
        cert.public_key().verify(
            resp.response.signature,
            nonce + tls_cert,
            padding.PKCS1v15(),
            hashes.SHA256(),
        )
        ok = True
    except Exception as e:  # noqa: BLE001
        print("    signature error:", e)
        ok = False
    check(ok, "device auth signature verifies against the device certificate")
    check(resp.response.sender_nonce == nonce, "device auth echoes the sender nonce")


def test_catt(name, url):
    """Drives the receiver with catt, a popular command line Cast sender."""
    catt = shutil.which("catt")
    if not catt:
        print("catt (skipped: pip install catt)")
        return
    print("catt")

    def run(*args):
        r = subprocess.run([catt, "-d", name, *args], capture_output=True, text=True, timeout=60)
        return r.returncode, r.stdout + r.stderr

    rc, out = run("cast", url)
    check(rc == 0 and "Playing" in out, "catt cast")
    rc, out = run("seek", "0:05")
    check(rc == 0, "catt seek")
    rc, out = run("info")
    check("stream_type: BUFFERED" in out, "catt sees a seekable BUFFERED stream")
    rc, out = run("volume", "25")
    rc, out = run("status")
    check("Volume: 25" in out, "catt volume")
    rc, out = run("stop")
    check(rc == 0, "catt stop")


def main():
    # All traffic in this test is local; never route it through a proxy.
    os.environ["NO_PROXY"] = os.environ["no_proxy"] = "*"
    binary = os.path.abspath(sys.argv[1])
    data_dir = tempfile.mkdtemp(prefix="vitacast-e2e-")
    httpd = ThreadingHTTPServer(("0.0.0.0", 0), MediaHandler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    ip = local_ip()
    base = f"http://{ip}:{httpd.server_address[1]}"

    test_fetch(binary, data_dir, base)

    name = "VitaCast E2E " + uuidlib.uuid4().hex[:6]
    env = dict(os.environ, VITACAST_SIM_DURATION=str(SIM_DURATION), VITACAST_SIM_LOAD_DELAY_MS="300")
    log_path = os.path.join(data_dir, "receiver.log")
    log = open(log_path, "w")
    proc = subprocess.Popen(
        [binary, "--data", data_dir, "--name", name, "--ip", ip],
        stdout=log,
        stderr=subprocess.STDOUT,
        env=env,
    )
    cast = None
    browser = None
    try:
        ready = wait_for(lambda: "ready" in open(log_path).read(), timeout=60)
        check(ready, "receiver starts")
        cfg = dict(
            line.strip().split("=", 1)
            for line in open(os.path.join(data_dir, "config.txt"))
            if "=" in line and not line.startswith("#")
        )
        dev_uuid = uuidlib.UUID(cfg["uuid"])

        print("Discovery")
        info = json.load(urllib.request.urlopen(f"http://{ip}:8008/setup/eureka_info", timeout=5))
        check(info.get("name") == name, "eureka_info (HTTP 8008) reports the friendly name")
        sctx = ssl._create_unverified_context()
        info = json.load(
            urllib.request.urlopen(f"https://{ip}:8443/setup/eureka_info", timeout=10, context=sctx)
        )
        check(info["device_info"]["ssdp_udn"] == str(dev_uuid), "eureka_info (HTTPS 8443) reports the UUID")

        casts, browser = pychromecast.get_listed_chromecasts(friendly_names=[name], discovery_timeout=8)
        found = [c for c in casts if c.uuid == dev_uuid]
        check(bool(found), "found via mDNS (_googlecast._tcp)")
        if found:
            cast = found[0]
            check(cast.cast_info.port == 8009, "mDNS SRV record points at port 8009")
            check(cast.cast_info.host == ip, "mDNS A record points at our IP")
        else:
            cast = pychromecast.get_chromecast_from_host((ip, 8009, dev_uuid, "VitaCast Host", name))

        print("Device authentication")
        raw_device_auth(ip)

        print("Cast channel")
        cast.wait(timeout=15)
        check(cast.status is not None, "receives RECEIVER_STATUS after connecting")
        check(cast.status is not None and cast.status.volume_level == 1.0, "initial volume is 1.0")

        mc = cast.media_controller
        url = base + "/video.mp4"
        mc.play_media(url, "video/mp4", title="Big Buck Bunny")
        mc.block_until_active(timeout=15)
        check(cast.status.app_id == "CC1AD845", "Default Media Receiver is launched")
        check(wait_for(lambda: mc.status.player_state == "PLAYING", 15), "LOAD -> PLAYING")
        check(mc.status.content_id == url, "media status echoes contentId")
        check(mc.status.title == "Big Buck Bunny", "media status echoes metadata title")
        check(abs((mc.status.duration or 0) - SIM_DURATION) < 0.01, "media status reports duration")

        mc.pause()
        check(wait_for(lambda: mc.status.player_state == "PAUSED", 5), "PAUSE -> PAUSED")
        mc.seek(7.5)  # pychromecast sends resumeState=PLAYBACK_START
        check(wait_for(lambda: abs(mc.status.current_time - 7.5) < 0.5, 5), "SEEK updates currentTime")
        check(mc.status.player_state == "PLAYING", "SEEK honours resumeState PLAYBACK_START")
        mc.pause()
        check(wait_for(lambda: mc.status.player_state == "PAUSED", 5), "PAUSE again")
        mc.play()
        check(wait_for(lambda: mc.status.player_state == "PLAYING", 5), "PLAY -> PLAYING")

        cast.set_volume(0.4)
        check(wait_for(lambda: abs(cast.status.volume_level - 0.4) < 0.001, 5), "SET_VOLUME 0.4")
        cast.set_volume_muted(True)
        check(wait_for(lambda: cast.status.volume_muted is True, 5), "mute")
        cast.set_volume_muted(False)

        mc.update_status()
        check(wait_for(lambda: mc.status.current_time > 7.5, 5), "playback time advances")

        mc.seek(SIM_DURATION - 1.0)
        check(
            wait_for(lambda: mc.status.player_state == "IDLE" and mc.status.idle_reason == "FINISHED", 8),
            "end of media -> IDLE/FINISHED broadcast",
        )

        mc.play_media(url, "video/mp4", title="Second")
        check(
            wait_for(lambda: mc.status.player_state == "PLAYING" and mc.status.title == "Second", 15),
            "second LOAD replaces the first",
        )
        mc.stop()
        check(
            wait_for(lambda: mc.status.player_state == "IDLE" and mc.status.idle_reason == "CANCELLED", 5),
            "STOP -> IDLE/CANCELLED",
        )

        replies = []
        got = threading.Event()

        def on_reply(ok, data):
            replies.append(data or {})
            got.set()

        mc.play_media(base + "/missing.mp4", "video/mp4", callback_function=on_reply)
        check(got.wait(15) and replies[0].get("type") == "LOAD_FAILED", "LOAD of a 404 URL -> LOAD_FAILED")
        check(bool(replies) and replies[0].get("detailedErrorCode") == 103, "LOAD_FAILED carries MEDIA_NETWORK code")

        mc.play_media(base + "/redirect", "video/mp4", title="Redirected")
        check(wait_for(lambda: mc.status.player_state == "PLAYING", 15), "LOAD follows HTTP redirects")

        cast.quit_app()
        check(wait_for(lambda: cast.status.app_id is None, 5), "STOP app -> no running application")

        test_catt(name, url)

        alive = proc.poll() is None
        check(alive, "receiver still running")
    finally:
        if cast is not None:
            cast.disconnect(timeout=5)
        if browser is not None:
            pychromecast.discovery.stop_discovery(browser)
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        log.close()
        check(proc.returncode == 0, "receiver exits cleanly on SIGTERM")
        if FAILURES:
            print("---- receiver log ----")
            print(open(log_path).read()[-6000:])
        httpd.shutdown()
        shutil.rmtree(data_dir, ignore_errors=True)

    if FAILURES:
        print(f"{len(FAILURES)} failure(s)")
        sys.exit(1)
    print("all end-to-end checks passed")


if __name__ == "__main__":
    main()
