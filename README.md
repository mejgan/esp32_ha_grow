# GrowBox-C6

ESP32-C6 firmware for a three-floor grow box. It runs the LED strip of each floor on a schedule, cuts the light
when a floor gets too hot, reads temperature, humidity, soil moisture, pressure and light, and shows everything on
an optional display. It joins Zigbee as a router and is controlled from Home Assistant through Zigbee2MQTT.

## Features

- **Per floor:** dimmable LED strip (PWM), auto schedule (on/off time to the minute, brightness) or manual
  control, over-temperature cutoff with 2 °C hysteresis, enable/disable.
- **Sensors:**
  - Floor 1: DHT11 and a capacitive soil sensor.
  - Floor 2: AHT10 and a YL-69 soil sensor.
  - Floor 3: AHT10, BMP180 for pressure and a backup temperature, and a TEMT6000 light sensor.
- **Fail-safe:**
  - A floor without its own temperature reading uses the hottest other floor.
  - If no temperature sensor gives a reading for `GROWBOX_TEMP_FAILSAFE_MIN` minutes (default 10), the lights go
    off.
  - A `sensor_fault` flag tells Home Assistant when a sensor gives no reading.
- **Clock:**
  - Set from the Zigbee coordinator (Zigbee2MQTT).
  - Survives crashes, watchdog resets and OTA restarts, so the schedule keeps running without the network.
  - Lost only on power loss; until the next sync, auto mode keeps the lights off.
- **Display (optional):**
  - SSD1306 (128x64 and 128x32) and SH1106 OLEDs, 16x2 and 20x4 HD44780 LCDs on a PCF8574 backpack, ST7789 and
    ILI9341 TFTs.
  - Pages rotate automatically.
- **BOOT button:**
  - Short press: next display page.
  - Hold 10 s: Zigbee factory reset (pair again).
- **Firmware updates over Zigbee** from Zigbee2MQTT, with automatic rollback if the new firmware never comes up.

## Hardware

A 3D-printable case with a lid for every supported display is in [`case/`](case/README.md). Its USB-C socket is a
breakout board wired to the ESP32-C6's built-in USB: GPIO13 = D+, GPIO12 = D-.

Default pins (all changeable in `idf.py menuconfig` → *Grow Box*):

| Function | GPIO | Notes |
|---|---|---|
| Floor 1 DHT11 data | 18 | 10 kΩ pull-up recommended |
| Floor 1 capacitive soil AOUT | 0 | ADC1 |
| Floor 2 YL-69 AO | 1 | ADC1 |
| Floor 2 YL-69 VCC | 10 | powered only while reading (less corrosion) |
| Floor 2 AHT10 SDA / SCL | 6 / 7 | LP I2C, fixed pins |
| Floor 3 AHT10 + BMP180 SDA / SCL | 22 / 23 | |
| Floor 3 TEMT6000 SIG | 2 | ADC1 |
| LED strip PWM floor 1 / 2 / 3 | 19 / 20 / 21 | MOSFET gate |
| Button | 9 | BOOT button of the dev board |

**MOSFETs:** use logic-level MOSFETs that fully switch on at 3.3 V, such as IRLZ44N or IRLB8721, or AO3400 for
small strips. The common "IRF520 module" needs about 10 V on the gate. At 3.3 V it only half switches on, gets
hot and dims the strip.

**Light sensor range:** the TEMT6000 tops out at about 600 lx with this circuit, so under grow lights it sits at
the maximum.

### Displays

Pick the type in *Grow Box → Display*. I2C displays share a sensor I2C bus and need no extra pins; the floor 3 bus
(GPIO22/23) is the default. The address is auto-detected unless set:
- OLEDs: 0x3C / 0x3D.
- PCF8574 backpacks: 0x27 / 0x3F and the others in their range, except 0x38, which is the AHT10's address.

| Display | Wiring | Grid |
|---|---|---|
| SSD1306 / SH1106 128x64 | VCC 3V3, GND, SDA, SCL | 21 x 8 characters |
| SSD1306 128x32 | VCC 3V3, GND, SDA, SCL | 21 x 4 |
| HD44780 16x2 / 20x4 + PCF8574 | VCC 5V, GND, SDA, SCL | 16 x 2 / 20 x 4 |
| ST7789 / ILI9341 (SPI) | SCLK 4, MOSI 5, CS 11, DC 3, RST → 3V3, BL → 3V3 | 20 x 20 at 240x320, font x2 |

Notes:
- **5 V LCD backpacks:** the backpack's pull-ups pull SDA/SCL up to 5 V, and the ESP32-C6 pins are not 5 V
  tolerant. Remove the backpack's pull-up resistors (the bus has pull-ups on the 3.3 V side) or use a level
  shifter. Turn the contrast trimmer if the screen stays blank.
- **TFTs:** set the panel size, rotation, offsets and color inversion in menuconfig.
  - Typical ST7789 boards: 240x240 or 240x320, color inversion on.
  - ST7789 boards without a CS pin: set CS to -1 and SPI mode 3.
  - ILI9341: color inversion off, BGR on.
- **Font size:** the TFT font scale changes how much fits on one page. Everything fits on one page from
  240x320 at scale 2.
- **OLED burn-in:** to save the OLED, set *Turn the display off after …* and wake it with the button.

## Build and flash

Needs ESP-IDF 6.0 or newer.

```bash
idf.py set-target esp32c6
idf.py menuconfig   # Grow Box: pins, display, calibration
idf.py build flash monitor
```

In the case, flash through its USB-C socket: `idf.py -p /dev/ttyACM0 flash monitor`.

The build also writes `build/growbox.ota` (the Zigbee OTA file) and `build/growbox_ota_index.json`.

**Updating from the old single-app firmware:**
- The partition table now has two app slots for OTA. Flash once over USB with `idf.py flash`; after that,
  updates can go over Zigbee.
- `nvs` keeps its place, so the settings and the Zigbee network survive the switch, and the old settings format
  is migrated.
- `CONFIG_FREERTOS_HZ=1000` and rollback are set in `sdkconfig.defaults`. Defaults only apply to a new
  `sdkconfig`, so delete your old `sdkconfig` (or set both options in menuconfig) before building.

## Zigbee2MQTT

1. Copy `z2m/growbox.mjs` to `<z2m data>/external_converters/` and restart Zigbee2MQTT.
2. Enable *Permit join*, power the box (or hold BOOT for 10 s to re-pair) and wait for "GrowBox-C6".

Properties use the floor as a suffix:
- Light: `state_floor1`, `brightness_floor1`.
- Readings: `temperature_floor2`, `soil_moisture_floor2`.
- Settings: `auto_mode_floor3`, `light_on_hour_floor1` / `light_on_minute_floor1`, `max_temperature_floor1`.
- Status: `heat_lock_floor1`, `sensor_fault_floor1`.

**Behavior:**
- Any manual light command switches the floor to manual mode.
- While a floor is disabled or heat-locked, ON is refused and the light shows OFF again.
- Turning `auto_mode` back on hands the light to the schedule again.
- Out-of-range setting writes are rejected.

### Firmware updates over Zigbee

1. Bump `PROJECT_VER` in `CMakeLists.txt` (e.g. `2.0.0` → `2.0.1`) and build.
2. Copy `build/growbox.ota` and `build/growbox_ota_index.json` into the Zigbee2MQTT data directory.
3. In `configuration.yaml`:
   ```yaml
   ota:
     zigbee_ota_override_index_location: growbox_ota_index.json
   ```
4. Restart Zigbee2MQTT, then *OTA → Check for update → Update* on the device page.

During the update the light keeps running. After the update the new firmware has to join the network (or run for
10 minutes); otherwise the next reset rolls back to the previous version.

## Calibration

The log prints the raw soil voltage on every read:

```
sensors: floor 1: temp ok 24.0C | hum ok 55.0% | soil ok 1980mV (51.4%)
```

Note the mV in dry air and in water and set them in *Grow Box → Soil sensor calibration*.

## Code layout

- `main/`: firmware.
  - `main.c`: control loop.
  - `zb_device.c`: Zigbee data model.
  - `zb_ota.c`: updates.
  - `display.c` and `disp_*.c`: display task and drivers.
  - Sensor drivers.
- `components/growbox_logic`: schedule, cutoff, sensor math, DHT11 and OTA file decoding. No ESP-IDF
  dependencies.
- `components/growbox_ui`: display pages as text grids, and the 5x7 font. No ESP-IDF dependencies.
- `test/host`: unit tests for both components, plus `ui_preview`, which renders every page of every display type
  to images.
- `tools/make_ota_image.py`: wraps `growbox.bin` into a Zigbee OTA file.
- `case/`: enclosure generator, STLs and previews.

Run the host tests:

```bash
cmake -S test/host -B build-host && cmake --build build-host && ctest --test-dir build-host --output-on-failure
```

CI (`.github/workflows/build.yml`) runs the host tests and builds the firmware once per display type.
