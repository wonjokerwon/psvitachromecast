#!/usr/bin/env python3
"""Checks the mirroring viewer page (the HTML the plugin serves to the TV) in
a real Chromium: frames appear, the page recovers when the Vita goes away and
comes back, and it shows when mirroring is paused.

A small Python server stands in for the kernel plugin's HTTP endpoints, using
the exact HTML compiled into the plugin.

usage: viewer_test.py path/to/mirror_logic.c
"""

import io
import os
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

try:
    from PIL import Image
    from playwright.sync_api import sync_playwright
except ImportError:
    print("SKIP: pip install playwright pillow")
    sys.exit(0)

FAILURES = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        FAILURES.append(what)


def viewer_html(logic_c):
    """Compiles a tiny program that prints mirror_viewer_html."""
    tmp = tempfile.mkdtemp(prefix="vitacast-viewer-")
    exe = os.path.join(tmp, "print_html")
    main = os.path.join(tmp, "main.c")
    with open(main, "w") as f:
        f.write('#include <stdio.h>\nextern const char mirror_viewer_html[];\n'
                'int main(void){fputs(mirror_viewer_html, stdout);return 0;}\n')
    subprocess.run(["cc", "-o", exe, main, logic_c], check=True)
    return subprocess.run([exe], capture_output=True, check=True).stdout


def jpeg(color):
    buf = io.BytesIO()
    Image.new("RGB", (480, 272), color).save(buf, "JPEG")
    return buf.getvalue()


class FakePlugin:
    def __init__(self, html):
        self.html = html
        self.online = True
        self.paused = False
        self.frame = jpeg((200, 30, 30))
        state = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *a):
                pass

            def do_GET(self):
                if not state.online:
                    self.connection.close()
                    return
                if self.path == "/":
                    self.reply(200, "text/html", state.html)
                elif self.path.startswith("/status"):
                    body = ('{"paused":%s}' % ("true" if state.paused else "false")).encode()
                    self.reply(200, "application/json", body)
                elif self.path.startswith("/stream.mjpg"):
                    self.send_response(200)
                    self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=vitacastframe")
                    self.send_header("Connection", "close")
                    self.end_headers()
                    try:
                        while state.online:
                            f = state.frame
                            self.wfile.write(b"--vitacastframe\r\nContent-Type: image/jpeg\r\n"
                                             b"Content-Length: %d\r\n\r\n" % len(f) + f + b"\r\n")
                            self.wfile.flush()
                            time.sleep(0.05)
                    except OSError:
                        pass
                    self.connection.close()
                else:
                    self.reply(404, "text/plain", b"")

            def reply(self, code, ctype, body):
                self.send_response(code)
                self.send_header("Content-Type", ctype)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        self.httpd = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.port = self.httpd.server_address[1]
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()


def center_color(page):
    return page.evaluate("""() => {
        const v = document.getElementById('v');
        if (!v.naturalWidth) return null;
        const c = document.createElement('canvas');
        c.width = v.naturalWidth; c.height = v.naturalHeight;
        const g = c.getContext('2d'); g.drawImage(v, 0, 0);
        return Array.from(g.getImageData(c.width / 2, c.height / 2, 1, 1).data.slice(0, 3));
    }""")


def wait_until(fn, timeout):
    end = time.time() + timeout
    while time.time() < end:
        if fn():
            return True
        time.sleep(0.2)
    return fn()


def main():
    os.environ["NO_PROXY"] = os.environ["no_proxy"] = "*"
    plugin = FakePlugin(viewer_html(os.path.abspath(sys.argv[1])))
    url = f"http://127.0.0.1:{plugin.port}/"
    with sync_playwright() as p:
        # Reuse a preinstalled Chromium when one is provided (CHROMIUM=path).
        exe = os.environ.get("CHROMIUM")
        browser = p.chromium.launch(executable_path=exe) if exe else p.chromium.launch()
        page = browser.new_page(viewport={"width": 1280, "height": 720})
        page.goto(url)
        status = lambda: page.inner_text("#s")
        red = lambda: (center_color(page) or [0, 0, 0])[0] > 150

        print("Viewer page")
        check(wait_until(red, 10), "shows frames from the MJPEG stream")
        check(status() == "", "no status text while live")
        plugin.frame = jpeg((30, 30, 200))
        check(wait_until(lambda: (center_color(page) or [0, 0, 0])[2] > 150, 5), "updates as new frames arrive")

        plugin.paused = True
        check(wait_until(lambda: "Paused" in status(), 10), "shows when mirroring is paused")
        plugin.paused = False
        check(wait_until(lambda: status() == "", 10), "clears the paused notice")

        plugin.online = False
        check(wait_until(lambda: "Vita" in status() or "Reconnecting" in status(), 15),
              "notices when the Vita goes away")
        plugin.frame = jpeg((30, 200, 30))
        plugin.online = True
        check(wait_until(lambda: (center_color(page) or [0, 0, 0])[1] > 150, 20),
              "reconnects by itself when the Vita comes back")

        print("Viewer page on a Chromecast (CrKey user agent)")
        errors = []
        cc = browser.new_page(
            viewport={"width": 1280, "height": 720},
            user_agent="Mozilla/5.0 (X11; Linux aarch64) AppleWebKit/537.36 (KHTML, like Gecko) "
                       "Chrome/114.0.0.0 Safari/537.36 CrKey/1.56.500000",
        )
        cc.on("pageerror", lambda e: errors.append(str(e)))
        cc.goto(url)
        check(wait_until(lambda: (center_color(cc) or [0, 0, 0])[1] > 150, 10),
              "streams even where the Cast receiver SDK cannot start")
        check(cc.evaluate("document.querySelectorAll('script[src*=caf_receiver]').length") == 1,
              "loads the Cast receiver SDK to keep the TV app alive")
        check(not errors, f"no uncaught page errors ({errors[:1]})")
        browser.close()
    plugin.httpd.shutdown()

    if FAILURES:
        print(f"{len(FAILURES)} failure(s)")
        sys.exit(1)
    print("all viewer checks passed")


if __name__ == "__main__":
    main()
