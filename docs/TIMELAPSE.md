# Plant time-lapse with a camera

There are two ways:
- **Cameras plugged into the GrowBox** (Arducam Mega on SPI, photos uploaded over Wi-Fi to your PC). The box then
  runs as a Zigbee end device. See [CAMERAS.md](CAMERAS.md).
- **Separate ESP32-CAM boards** on Wi-Fi with ESPHome and Home Assistant, described below. The GrowBox stays a
  Zigbee router.

## Why a camera can't simply hang off the ESP32-C6 like on an ESP32-CAM

- **No camera interface.** Cameras like the OV2640 need a parallel camera port (about 11 pins) and several
  megabytes of frame memory (PSRAM). The ESP32-C6 has neither: no camera port, no PSRAM, 512 KB RAM.
- **Zigbee can't carry photos.** In practice it moves a few kB per second, and Zigbee2MQTT / Home Assistant have
  no way to receive images over Zigbee. One 200 KB photo would take a minute and slow the network for everything
  else.
- **The C6's Wi-Fi shares the radio with Zigbee**, so sending photos over Wi-Fi from the same chip makes the Zigbee
  side less reliable.
- SPI cameras with their own frame buffer (Arducam Mega) do connect. Getting the pictures out over Wi-Fi is stable
  only with the GrowBox as a Zigbee end device, which is what [CAMERAS.md](CAMERAS.md) does.

## What works well: a separate camera board

Use a cheap **ESP32 camera board on Wi-Fi** running ESPHome, one per floor. It shows up in Home Assistant as a
camera, Home Assistant saves a photo every N minutes, and ffmpeg turns the photos into a video.

| Board | Notes |
|---|---|
| **ESP32-CAM (AI-Thinker)**, about $6 | OV2640 2 MP, PSRAM. Needs a USB-serial adapter or the "ESP32-CAM-MB" base for the first flash. |
| Seeed **XIAO ESP32S3 Sense** | tiny, USB-C, OV2640 / OV3660 |
| Freenove ESP32-S3-WROOM CAM | USB-C, easy to flash |

Tips:
- **Lens:** a wide-angle (120–160°) OV2640 lens shows a whole floor from close up.
- **Power:** from the GrowBox 5 V buck (ESP32-CAM peaks around 300 mA), or a separate USB charger.
- **Mounting:** keep the camera in a fixed position. Even 1 mm of movement makes a time-lapse jump.
- **Flash LED:** leave the board's flash LED off. Take photos only while the grow lights are on, so every frame has
  the same light.

## ESPHome config (ESP32-CAM AI-Thinker)

```yaml
esphome:
  name: grow-cam-floor1

esp32:
  board: esp32cam
  framework:
    type: esp-idf

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

logger:
api:
ota:
  - platform: esphome

esp32_camera:
  name: Grow camera floor 1
  external_clock:
    pin: GPIO0
    frequency: 20MHz
  i2c_pins:
    sda: GPIO26
    scl: GPIO27
  data_pins: [GPIO5, GPIO18, GPIO19, GPIO21, GPIO36, GPIO39, GPIO34, GPIO35]
  vsync_pin: GPIO25
  href_pin: GPIO23
  pixel_clock_pin: GPIO22
  power_down_pin: GPIO32
  resolution: 1600x1200
  jpeg_quality: 10
  max_framerate: 1 fps
  idle_framerate: 0.05 fps
```

For other boards, take the camera pins from that board's ESPHome page. Every ESP32 camera board has its own
pinout.

## Home Assistant: save a photo every 30 minutes while the light is on

In `configuration.yaml`, allow writing to the media folder:

```yaml
homeassistant:
  allowlist_external_dirs:
    - /media
```

Automation (one per floor). Use your own light entity: the floor's `state_floor1` light from Zigbee2MQTT shows up
in Home Assistant as a light entity.

```yaml
alias: Grow time-lapse floor 1
triggers:
  - trigger: time_pattern
    minutes: "/30"
conditions:
  - condition: state
    entity_id: light.growbox_floor1
    state: "on"
actions:
  - action: camera.snapshot
    target:
      entity_id: camera.grow_camera_floor_1
    data:
      filename: "/media/growbox/floor1/{{ now().strftime('%Y%m%d_%H%M%S') }}.jpg"
```

Disk use, per camera:
- each 1600x1200 photo is about 150–250 KB;
- every 30 minutes over an 18 h light period gives about 7 MB per day;
- a 90-day grow is about 0.6 GB.

## Make the video

Copy the folder to a PC (or run this on the Home Assistant host with the ffmpeg add-on):

```bash
ffmpeg -framerate 30 -pattern_type glob -i 'floor1/*.jpg' -vf "deflicker,scale=1600:-2" -c:v libx264 -pix_fmt yuv420p -crf 20 floor1.mp4
```

- `-framerate 30` with one photo every 30 minutes makes one day last about 1 second.
- `deflicker` evens out small brightness changes between photos.
- For a slower video use `-framerate 15`. For one photo per day, change the automation to a daily time trigger.
