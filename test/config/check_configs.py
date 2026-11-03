#!/usr/bin/env python3
"""Checks configurations without building the firmware: every CI configuration and a set of sensor setups must
pass main/check_config.cmake, and a set of broken ones must fail with the right message.

Needs `pip install esp-idf-kconfig` (the Kconfig tool ESP-IDF uses) and cmake. Run from anywhere:

    python test/config/check_configs.py
"""

import glob
import os
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

# Sensor setups that must pass. Each is added to sdkconfig.defaults.
GOOD = {
    "defaults": "",
    "every I2C type": """
CONFIG_GROWBOX_F1_CLIMATE_SHT3X=y
CONFIG_GROWBOX_F1_CLIMATE_ADDR=0x45
CONFIG_GROWBOX_F1_PRESSURE_BMP280=y
CONFIG_GROWBOX_F1_LIGHT_BH1750=y
CONFIG_GROWBOX_F1_LIGHT_ADDR=0x5C
CONFIG_GROWBOX_F2_CLIMATE_HTU21D=y
CONFIG_GROWBOX_F2_LIGHT_BH1750=y
CONFIG_GROWBOX_F3_CLIMATE_BME280=y
CONFIG_GROWBOX_F3_CLIMATE_ADDR=0x77
CONFIG_GROWBOX_F3_PRESSURE_NONE=y
CONFIG_GROWBOX_ROOM_SHT4X=y
""",
    "four DHTs": """
CONFIG_GROWBOX_F2_CLIMATE_DHT22=y
CONFIG_GROWBOX_F3_CLIMATE_DHT22=y
CONFIG_GROWBOX_F3_PRESSURE_NONE=y
CONFIG_GROWBOX_ROOM_DHT22=y
""",
    "no sensors at all": """
CONFIG_GROWBOX_F1_CLIMATE_NONE=y
CONFIG_GROWBOX_F1_SOIL_NONE=y
CONFIG_GROWBOX_F2_CLIMATE_NONE=y
CONFIG_GROWBOX_F2_SOIL_NONE=y
CONFIG_GROWBOX_F3_CLIMATE_NONE=y
CONFIG_GROWBOX_F3_PRESSURE_NONE=y
CONFIG_GROWBOX_F3_LIGHT_NONE=y
""",
    "AHT room sensor when floor 3 has an SHT3x": """
CONFIG_GROWBOX_F3_CLIMATE_SHT3X=y
CONFIG_GROWBOX_ROOM_AHT=y
""",
    "soil and light sensors on every floor": """
CONFIG_GROWBOX_F1_LIGHT_TEMT6000=y
CONFIG_GROWBOX_F2_LIGHT_TEMT6000=y
CONFIG_GROWBOX_F3_SOIL_CAPACITIVE=y
CONFIG_GROWBOX_F3_SOIL_PWR_GPIO=11
""",
}

# Broken setups: (fragment, text the error must contain).
BAD = {
    "two AHTs on one bus": ("CONFIG_GROWBOX_F2_BUS_MAIN=y\n", "both use I2C address 0x38 on the main bus"),
    "two DHTs on one pin": ("CONFIG_GROWBOX_F2_CLIMATE_DHT11=y\nCONFIG_GROWBOX_F2_CLIMATE_GPIO=18\n",
                            "GPIO18 is used by both floor 1 DHT and floor 2 DHT"),
    "analog sensor on an LP I2C pin": ("CONFIG_GROWBOX_F1_LIGHT_TEMT6000=y\nCONFIG_GROWBOX_F1_LIGHT_GPIO=6\n",
                                       "GPIO6 is used by both LP I2C SDA and floor 1 TEMT6000"),
    "BME280 room sensor at the BMP180's address": (
        "CONFIG_GROWBOX_ROOM_BME280=y\nCONFIG_GROWBOX_ROOM_I2C_ADDR=0x77\n",
        "floor 3 pressure sensor and room sensor both use I2C address 0x77"),
    "flash pin": ("CONFIG_GROWBOX_F1_LIGHT_PWM_GPIO=25\n", "GPIO25 (floor 1 LED strip) is a flash pin"),
    "BH1750 next to an LCD at the same address": (
        "CONFIG_GROWBOX_F3_LIGHT_BH1750=y\nCONFIG_GROWBOX_DISPLAY_HD44780_2004=y\nCONFIG_GROWBOX_DISPLAY_I2C_ADDR=0x23\n",
        "floor 3 BH1750 and display both use I2C address 0x23"),
    "camera on the console pins": (
        "CONFIG_ZB_ZED=y\nCONFIG_GROWBOX_CAMERA_COUNT=3\n", "GPIO16 is used by both the UART0 console TX"),
}


def check(name, fragment, workdir):
    defaults = os.path.join(workdir, "defaults")
    with open(os.path.join(ROOT, "sdkconfig.defaults")) as f:
        base = f.read()
    with open(defaults, "w") as f:
        f.write(base + "\n" + fragment)
    out = os.path.join(workdir, "sdkconfig.cmake")
    gen = subprocess.run([sys.executable, "-m", "kconfgen", "--kconfig", "test/config/Kconfig", "--defaults",
                          defaults, "--output", "cmake", out, "--output", "config",
                          os.path.join(workdir, "sdkconfig")],
                         cwd=ROOT, capture_output=True, text=True)
    if gen.returncode != 0:
        raise SystemExit(f"{name}: kconfgen failed:\n{gen.stdout}{gen.stderr}")
    run = subprocess.run(["cmake", f"-DSDKCONFIG_CMAKE={out}", "-P", "main/check_config.cmake"], cwd=ROOT,
                         capture_output=True, text=True)
    return run.returncode == 0, run.stdout + run.stderr


def main():
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        cases = dict(GOOD)
        for path in sorted(glob.glob(os.path.join(ROOT, "ci", "sdkconfig.*"))):
            with open(path) as f:
                cases[os.path.relpath(path, ROOT)] = f.read()
        for name, fragment in cases.items():
            ok, output = check(name, fragment, tmp)
            print(f"{'ok  ' if ok else 'FAIL'} pass: {name}")
            if not ok:
                print(output)
                failures += 1
        for name, (fragment, expected) in BAD.items():
            ok, output = check(name, fragment, tmp)
            good = not ok and expected in output
            print(f"{'ok  ' if good else 'FAIL'} fail: {name}")
            if not good:
                print(f"expected an error containing: {expected}\n{output}")
                failures += 1
    if failures:
        raise SystemExit(f"{failures} configuration check(s) went wrong")
    print("all configuration checks passed")


if __name__ == "__main__":
    main()
