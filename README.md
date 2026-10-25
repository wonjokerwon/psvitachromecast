# VitaCast

Two homebrew apps that connect the **PlayStation Vita** to Chromecasts and
Google TVs:

- **VitaCast Mirror** shows the Vita's screen on a Chromecast or Google TV,
  whatever you're playing. See [Screen mirroring](#screen-mirroring).
- **VitaCast** turns the Vita into a Cast *receiver* that open-source senders
  can play videos on. See [Cast receiver](#cast-receiver).

## Screen mirroring

```
 PS Vita                                          Chromecast / Google TV
 ┌──────────────────────────────┐   1. "open this page"   ┌──────────────────┐
 │ VitaCast Mirror app ─────────┼────── Cast (TLS) ──────▶│ DashCast app     │
 │                              │                         │   loads page ──┐ │
 │ vitacast_mirror.skprx        │   2. live view          │                │ │
 │  framebuffer                 │◀──── HTTP :8080 ────────┼── viewer page ◀┘ │
 │   └ IFTU scale (hardware)    │      MJPEG stream       │                  │
 │      └ JPEG encode (hardware)│                         └──────────────────┘
 └──────────────────────────────┘
```

1. Install `vitacast_mirror.vpk` with VitaShell and open **VitaCast Mirror**.
   The Vita needs HENkaku/Ensō with taiHEN, as for any plugin, and
   *Settings → HENkaku Settings → Enable Unsafe Homebrew* turned on (VitaShell
   needs it too).
2. Pick your TV from the list and press ✕.
3. The TV shows the Vita's screen. Close the app (START) and play: mirroring
   keeps running until you reboot.

There's nothing to set up on the TV, and nothing to edit in taiHEN's config:
the app loads the mirroring plugin when you open it. After a reboot, open the
app again to start mirroring.

| Button | Where | Action |
| --- | --- | --- |
| ✕ / ○ | app | Mirror to the selected TV / stop |
| L / R | app | Quality: Smooth (480×272), Balanced (640×368), Sharp (960×544) |
| □ | app | Pause / resume (the TV shows "Paused") |
| △ | app | Search for TVs again |
| L + R + SELECT, hold 1 s | anywhere, even in a game | Pause / resume mirroring |

The live view also works in any browser on the same network:
`http://<vita-ip>:8080/` (the app shows the address).

### How it works

- **Capture.** A kernel plugin reads the framebuffer of whatever app is in
  front. The IFTU display hardware scales it and converts it to YCbCr, so
  almost no CPU time is taken from the game. This is the technique xerpi's
  [vita-udcd-uvc](https://github.com/xerpi/vita-udcd-uvc) uses for USB
  streaming.
- **Encoding.** The Vita's hardware JPEG encoder, as used by Rinnegatamante's
  Vita Recorder and VITA2PC.
- **Delivery.** The plugin serves an MJPEG stream and a small viewer page
  (`mirror/plugin/`). To show it on a TV, the app launches
  [DashCast](https://github.com/stestagg/dashcast), a public Cast app that
  loads any web page, the same way `catt cast_site` does. The page keeps the
  Cast session alive and reconnects by itself when the Vita sleeps or Wi-Fi
  drops.

### Status and limits

**This hasn't run on real hardware yet.** Everything builds against the
current vitasdk, and every kernel API it uses comes from stable
`*ForDriver` libraries. The parts that can run off-device are tested in CI:
- TV discovery and the DashCast launch sequence, against a mock Chromecast;
- the viewer page, in Chromium (live frames, pause notice, reconnecting);
- request handling and layout detection.

The JPEG encoder's input layout isn't documented. On the first frame the
plugin runs a self-test that detects it and checks the scaled output against
the encoder's own colour converter. The result is in
`ux0:data/VitaCast/mirror_log.txt`. If mirroring misbehaves, please share that
file.

Expected limits:
- **Video only for now.** Mirroring game audio needs a different hook.
- **Not lag-free.** Expect a fraction of a second of delay: fine for people
  watching, not for playing off the TV.
- **Wi-Fi bound.** The Vita's 2.4 GHz Wi-Fi limits frame rate; "Balanced"
  should manage roughly 20–30 fps, "Sharp" less.
- **DashCast is a third-party app** hosted outside Google. If it ever goes
  away, the fallback is a Cast app ID registered for VitaCast.
- If a kernel plugin ever misbehaves, reboot: VitaCast Mirror loads it only
  when you open the app, and never adds it to taiHEN's config.

## Cast receiver

VitaCast (`vitacast.vpk`) makes the Vita show up as a Chromecast-style device on
your Wi-Fi network. Senders can discover it, launch the media receiver, and
play, pause, seek, stop and change the volume, with the video decoded by the
Vita's hardware decoder.

```
 phone / PC (Cast sender)                       PS Vita running VitaCast
 ┌──────────────────────┐   mDNS _googlecast    ┌─────────────────────────┐
 │ catt, Home Assistant │ ────────────────────▶ │ "PS Vita"  192.168.1.23 │
 │ pychromecast, ...    │   TLS :8009 CASTV2    │                         │
 │                      │ ◀───────────────────▶ │  receiver + media       │
 └──────────────────────┘                       │  session state machine  │
            │                                   │         │               │
            │ hosts the video (HTTP)            │  sceAvPlayer (H.264/AAC)│
            └─────────────────────────────────▶ │  ◀── HTTP range cache   │
                                                └─────────────────────────┘
```

### What works, and what can't

VitaCast implements the Cast v2 protocol: mDNS discovery, the TLS channel on
port 8009, connection/heartbeat/receiver/media namespaces, the Default Media
Receiver (`CC1AD845`) and the `/setup/eureka_info` device API. It does **not**
make the Vita indistinguishable from a real Chromecast to Google's own software,
because nothing homebrew can:

| Sender | Works? |
| --- | --- |
| [catt](https://github.com/skorokithakis/catt) (command line) | **Yes**, tested in CI |
| [pychromecast](https://github.com/home-assistant-libs/pychromecast) (Python library) | **Yes**, tested in CI |
| Apps built on pychromecast (Home Assistant's Cast integration, mkchromecast, ...) | Should work, not yet tested |
| Other open-source senders that skip Google's device check (go-chromecast, castnow / node-castv2-client, ...) | Should work, not yet tested |
| Chrome's "Cast..." menu, Android/iOS apps using the Google Cast SDK (Google Photos, the Google Home app, most video apps) | **No** |
| YouTube, Netflix, Spotify | **No** (they need their own receiver apps, not the media receiver) |

**Why Chrome and phone apps don't see it as castable:** official Google Cast
senders run a *device authentication* challenge, and only accept a reply
signed with a key whose certificate chains up to Google's Cast root CA.
Google installs those keys in certified hardware at the factory. VitaCast
answers the challenge correctly, but with its own self-signed certificate, so
those senders reject it. The only ways around that use keys extracted from
certified devices or commercial apps, and this project does not do that.

YouTube, Netflix and Spotify launch their own receiver apps, which use
proprietary protocols rather than the standard media namespace. VitaCast
answers `LAUNCH_ERROR` for them right away, so the sender fails fast instead
of hanging.

#### Project status

The protocol core runs on every push in CI against pychromecast and catt
(discovery, TLS, device auth, launch, load, pause, seek, stop, volume, end of
media, load errors, redirects), under ASan/UBSan and ThreadSanitizer. The Vita
build compiles and packages in CI against the current vitasdk. The
Vita-specific parts (AvPlayer playback, audio output, on-screen UI) **have not
been tested on real hardware yet**. Reports and logs from people who try it are
very welcome (see [Troubleshooting](#troubleshooting)).

### Installing

1. You need a Vita or PS TV running HENkaku/Ensō (or any setup that installs
   homebrew VPKs).
2. Download `vitacast.vpk` from the latest [GitHub Actions](../../actions)
   build artifact, or [build it](#building).
3. Install it with VitaShell, then start **VitaCast** from the LiveArea.
4. On first start VitaCast creates its device key, which takes up to a minute.
   Then it shows **Ready to cast** with its IP address.

The Vita and the sender must be on the same Wi-Fi network, and the router must
let devices see each other: turn off "AP/client isolation" and make sure
multicast/mDNS isn't blocked.

### Casting to it

With [catt](https://github.com/skorokithakis/catt):

```sh
pip install catt
catt scan                                    # lists "PS Vita"
catt -d "PS Vita" cast https://example.com/video.mp4
catt -d "PS Vita" cast ./holiday.mp4         # catt serves local files itself
catt -d "PS Vita" pause | play | seek 1:30 | volume 40 | stop
```

If discovery is blocked on your network, use the IP shown on the Vita:
`catt -d 192.168.1.23 cast ...`.

**Home Assistant**'s Google Cast integration uses pychromecast, so the Vita
should appear there as a `media_player` that `media_player.play_media` can
send MP4 URLs to (not yet tested).

#### Controls on the Vita

| Button | Action |
| --- | --- |
| ✕ | Play / pause |
| ○ | Stop |
| ← / → | Seek -10 s / +10 s |
| L / R | Seek -60 s / +60 s |
| ↑ / ↓ | Volume |
| △ | Show the recent log on the idle screen |
| START + SELECT | Quit |

Senders are told about local changes, so their UI stays in sync, as with a
real Chromecast.

#### Supported media

The Vita decodes **MP4 with H.264 video and AAC audio** in hardware. 720p or
lower plays best. Media is fetched over HTTP or HTTPS. The server should
support `Range` requests (almost all do): without them VitaCast still plays,
but seeking is slow because it has to re-read the file from the start.

Not supported: HLS (`.m3u8`), DASH (`.mpd`), WebM/VP9, MKV and live streams.
Senders get a `LOAD_FAILED` reply for these.

HTTPS server certificates are **not verified**: the Vita has no current CA
store, and most media URLs would otherwise fail.

### Settings and files

Everything lives in `ux0:data/VitaCast/`:

| File | Purpose |
| --- | --- |
| `config.txt` | `name=` is the name senders show. Edit it and restart VitaCast. `uuid=` is the device ID. |
| `device_key.pem`, `device_cert.pem` | The device identity. Delete both to make a new one. |
| `log.txt` | Log of the current session |

## Building

### PS Vita

Install [vitasdk](https://vitasdk.org), which includes vita2d and mbedTLS, or use its Docker
image:

```sh
cmake -S . -B build-vita -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-vita
# -> build-vita/vitacast_mirror.vpk  (VitaCast Mirror, includes vitacast_mirror.skprx)
#    build-vita/vitacast.vpk         (VitaCast receiver)

# or
docker run --rm -v "$PWD":/workspace vitasdk/vitasdk:latest \
  sh -c 'cmake -S . -B build-vita -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake && cmake --build build-vita'
```

### Desktop build and tests

The shared Cast code also builds for Linux and macOS as `vitacast-host`: a
receiver with a simulated player, plus the sender commands VitaCast Mirror
uses. It's for developing and testing without a Vita:

```sh
sudo apt install libmbedtls-dev          # or: brew install mbedtls
pip install pychromecast catt cryptography playwright pillow
python -m playwright install chromium    # for the viewer page test
cmake -S . -B build -DVITACAST_HOST=ON [-DVITACAST_SANITIZE=ON | -DVITACAST_TSAN=ON]
cmake --build build
ctest --test-dir build --output-on-failure

./build/vitacast-host --name "Desk Receiver"   # receiver; then: catt -d "Desk Receiver" cast <url>
./build/vitacast-host --discover               # list Chromecasts / Google TVs
./build/vitacast-host --cast-url 192.168.1.40 https://example.com/   # show a page on a TV via DashCast
./build/vitacast-host --stop-cast 192.168.1.40
```

| Test | What it covers |
| --- | --- |
| `tests/test_core.c` | JSON, protobuf/Cast framing, mDNS responder and browser, URL parsing, mirroring plugin logic (YCbCr layout detection, HTTP routing) |
| `tests/e2e_test.py` | The receiver, driven by pychromecast and catt over real mDNS, TLS and HTTP |
| `tests/sender_test.py` | The DashCast launch sequence, against a mock Chromecast |
| `tests/viewer_test.py` | The mirroring viewer page in Chromium |

LiveArea images are generated by `tools/gen_assets.py`, which needs Pillow.

## Code layout

| Path | Role |
| --- | --- |
| `mirror/plugin/vitacast_mirror.c` | Kernel plugin: framebuffer capture, IFTU scaling, hardware JPEG, MJPEG/HTTP server, encoder self-test, L+R+SELECT toggle |
| `mirror/plugin/mirror_logic.c` | Plugin logic without kernel APIs (tested on desktop): viewer page, request routing, layout detection |
| `mirror/app/main.c` | VitaCast Mirror app: loads the plugin, finds TVs, starts/stops mirroring |
| `src/core/cast_sender.c` | Cast sender: stops the running app, launches DashCast, sends the URL |
| `src/core/receiver.c` | Cast session logic: virtual connections, heartbeat, device auth, receiver namespace (launch/stop/volume/app availability), media namespace (load/play/pause/seek/stop, status broadcasts), and the `eureka_info` HTTP endpoint |
| `src/core/castmsg.c` | CASTV2 framing, with a minimal protobuf codec for `CastMessage` and `DeviceAuthMessage` |
| `src/core/mdns.c` | mDNS/DNS-SD responder and browser for `_googlecast._tcp` (PTR/SRV/TXT/A) |
| `src/core/tls.c` | mbedTLS: device key/certificate, TLS server and client, auth signatures |
| `src/core/http.c` | HTTP(S) client and a random-access, read-ahead block cache over `Range` requests |
| `src/core/json.c`, `sb.c` | Small JSON DOM and string builder |
| `src/vita/player_vita.c` | `sceAvPlayer` playback. Reads media through AvPlayer's file callbacks from the HTTP cache. Audio via `sceAudioOut`, video frames as GPU textures. |
| `src/vita/ui.c`, `main.c` | vita2d UI, input, Wi-Fi/suspend handling, power management |
| `src/host/` | Desktop platform layer and simulated player |

Each sender connection runs on its own thread. Outgoing messages go through
a per-connection queue, so status broadcasts never touch another thread's TLS
session.

### Why is the receiver an app and not a plugin?

A Cast receiver has to keep sockets open, run TLS, decode video and own the
screen while it plays. On the Vita that means a foreground application:
plugins injected into other apps can't safely take over the display and the
hardware decoder, and the system suspends background apps. VitaCast is a
normal homebrew app. Leave it open, the way you leave a TV on the Chromecast
input.

## Troubleshooting

- **VitaCast Mirror finds no TVs.** The Vita and the TV must be on the same
  network, and the router must not isolate wireless clients or block
  multicast/mDNS.
- **The TV shows "Waiting for PS Vita..."** The Vita went to sleep or lost
  Wi-Fi. Wake it; the TV reconnects by itself.
- **Mirroring is black, garbled or has wrong colours.** Send
  `ux0:data/VitaCast/mirror_log.txt`: its self-test lines show what the
  hardware reported.
- **The sender can't find the Vita.** Check that both are on the same network
  and that the router doesn't isolate wireless clients. Try casting to the IP
  shown on screen (`catt -d <ip> ...`).
- **"Format not supported" / playback error.** Use MP4 with H.264 and AAC.
  Re-encode with
  `ffmpeg -i in.mkv -c:v libx264 -profile:v high -level 4.1 -vf scale=-2:720 -c:a aac -movflags +faststart out.mp4`.
- **Something else.** Press △ on the idle screen for the recent log, or copy
  `ux0:data/VitaCast/log.txt` and attach it to an issue.
