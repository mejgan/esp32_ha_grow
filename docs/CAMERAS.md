# Cameras: plant photos and time-lapse

Up to three **Arducam Mega** SPI cameras (3 MP or 5 MP), one per floor, plug straight into the GrowBox. While a
floor's light is on, its camera takes a photo every 30 minutes (configurable). The GrowBox switches Wi-Fi on,
uploads the photo to a small server on your PC, and switches Wi-Fi off again. Nothing is stored on the box. The PC
saves the photos and turns them into a time-lapse video.

```
camera ──SPI──> GrowBox (ESP32-C6) ──Wi-Fi──> PC: tools/photo_server.py ──ffmpeg──> floor1.mp4
```

## Why the GrowBox has to be a Zigbee end device

The ESP32-C6 has one radio for Zigbee and Wi-Fi. Espressif's
[coexistence table](https://docs.espressif.com/projects/esp-idf/en/v6.0/esp32c6/api-guides/coexist.html) rates
Wi-Fi plus a Zigbee **router** as "unstable" and Wi-Fi plus a Zigbee **end device** as "stable". So with cameras
the box joins as an end device:
- It still does everything else: lights, sensors, settings, OTA.
- It just stops relaying Zigbee traffic for other devices.
- Its receiver stays on, so light commands still arrive at once.

## Hardware

| Part | Notes |
|---|---|
| Arducam Mega 3MP or 5MP (SPI) | One per floor. The 5MP has autofocus. JPEG up to 2048x1536. |
| Cable per floor | 12-core, or a Cat5 for the camera plus a second one for the sensors. The case's upper hole per floor takes a PG9 gland for a cable up to 8 mm. |

### Wiring (default pins, no TFT display)

| Camera pin | ESP32-C6 |
|---|---|
| VCC | 3V3 (the Mega also accepts 5V) |
| GND | GND |
| SCK | GPIO4 |
| MOSI | GPIO5 |
| MISO | GPIO3 |
| CS | camera 1: GPIO11 · camera 2: GPIO16 · camera 3: GPIO17 |

**Pin notes:**
- **TFT display:** the cameras share its SCK (GPIO4) and MOSI (GPIO5). MISO moves to GPIO16 and the CS pins to
  GPIO17 and GPIO8, so at most two cameras fit.
  - GPIO8 is also the DevKit's RGB LED; it may flicker while camera 2 is read.
- **GPIO16/17** are normally the UART console. When a camera uses them, set *Component config → ESP System
  Settings → Channel for console output* to *USB Serial/JTAG*. Logs and flashing then go through the USB-C socket.
  The build stops with a message if you forget.
- **Pin conflicts:** at boot the firmware checks every camera pin against all other features, and disables the
  cameras with a log message if two share a pin.

**Cable length:**
- SPI is meant for short runs. Up to about 1 m works at the default 4 MHz when each signal (SCK, MOSI, MISO, CS) is
  twisted against a GND wire.
- For longer cables set *Camera SPI clock* to 1–2 MHz; the photo only takes a little longer to read.

## Firmware setup

In `idf.py menuconfig`:

1. *Component config → Zigbee → Zigbee Device Type → **End Device***.
2. *Grow Box → Cameras*:
   - number of cameras;
   - Wi-Fi name and password;
   - upload URL, e.g. `http://192.168.1.10:8088/upload` (your PC's address);
   - upload token (any secret; the same one goes to the server);
   - interval and resolution.
3. If a camera uses GPIO16/17: console on *USB Serial/JTAG* (see above).

**Switching from router to end device needs pairing again:**
1. Hold the button for 10 s (or remove the device in Zigbee2MQTT).
2. Enable *Permit join*.

**Trigger a photo from Home Assistant:** every floor with a camera gets a **take_photo** switch in Zigbee2MQTT. Turn
it on and that floor's camera takes and uploads a photo right away, light or not. The switch turns itself off
again.

## The PC server

Needs Python 3.8 or newer; nothing else to install. Making videos also needs [ffmpeg](https://ffmpeg.org/).

```bash
python tools/photo_server.py serve --dir photos --token MYSECRET
```

- Photos land in `photos/floor1/2026-11-03_13-30-00.jpg`, `photos/floor2/…`, named by the PC's clock.
- To see the latest photo of every floor, open `http://<PC address>:8088/?token=MYSECRET` in a browser. The page
  refreshes every 5 minutes.
- Allow port 8088 through the PC's firewall.
- To start it automatically, use a Task Scheduler task on Windows or a systemd service on Linux that runs the
  command above.

Make the videos (one per floor, `photos/floor1.mp4` …):

```bash
python tools/photo_server.py video --dir photos
python tools/photo_server.py video --dir photos --fps 15   # slower
```

- At the default 30 fps with a photo every 30 minutes, one day of growth (18 h of light, 36 photos) lasts about
  1.2 seconds.
- `deflicker` evens out small brightness changes between photos.

### Server on the internet instead of at home

Use an `https://` upload URL whose certificate comes from a public CA (Let's Encrypt is fine). The box checks it
against the built-in certificate list. Keep the token secret: anyone with it can upload photos.

## Without the end-device change

If you'd rather keep the GrowBox a Zigbee router, use separate ESP32-CAM boards on Wi-Fi instead. See
[TIMELAPSE.md](TIMELAPSE.md).
