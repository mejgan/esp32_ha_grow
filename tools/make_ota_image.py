#!/usr/bin/env python3
"""Wrap an ESP-IDF app binary into a Zigbee OTA upgrade file (ZCL spec, OTA Upgrade cluster, file format).

The build runs this automatically and writes build/growbox.ota plus build/growbox_ota_index.json, an entry for a
Zigbee2MQTT local OTA index. Standalone use:

    python tools/make_ota_image.py --version 2.0.0 build/growbox.bin build/growbox.ota

The file version is derived from "major.minor.patch" exactly like main/CMakeLists.txt does (0xMMmmpp00), so the
firmware inside the file reports the same version once it runs.
"""

import argparse
import hashlib
import json
import os
import re
import struct
import sys

OTA_FILE_ID = 0x0BEEF11E
OTA_HEADER_VERSION = 0x0100
OTA_HEADER_LEN = 56
ZIGBEE_STACK_PRO = 0x0002
TAG_UPGRADE_IMAGE = 0x0000
MODEL_ID = "GrowBox-C6"


def file_version(version: str) -> int:
    m = re.match(r"^(\d+)\.(\d+)\.(\d+)", version)
    if not m:
        raise ValueError(f"version '{version}' must look like major.minor.patch")
    major, minor, patch = (int(g) for g in m.groups())
    if max(major, minor, patch) > 255:
        raise ValueError("version parts must be 0..255")
    return (major << 24) | (minor << 16) | (patch << 8)


def build_ota(image: bytes, manufacturer: int, image_type: int, version: int, header_string: str) -> bytes:
    hs = header_string.encode("ascii")[:32].ljust(32, b"\0")
    total = OTA_HEADER_LEN + 6 + len(image)
    header = struct.pack(
        "<IHHHHHIH32sI",
        OTA_FILE_ID,
        OTA_HEADER_VERSION,
        OTA_HEADER_LEN,
        0,  # field control: no optional fields
        manufacturer,
        image_type,
        version,
        ZIGBEE_STACK_PRO,
        hs,
        total,
    )
    assert len(header) == OTA_HEADER_LEN
    return header + struct.pack("<HI", TAG_UPGRADE_IMAGE, len(image)) + image


def int_auto(text: str) -> int:
    return int(text, 0)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", help="app binary (build/growbox.bin)")
    ap.add_argument("output", help="OTA file to write")
    ap.add_argument("--version", required=True, help="firmware version major.minor.patch (PROJECT_VER)")
    ap.add_argument("--manufacturer", type=int_auto, default=0x131B, help="manufacturer code (default 0x131B)")
    ap.add_argument("--image-type", type=int_auto, default=0x4742, help="image type (default 0x4742)")
    ap.add_argument("--header-string", default=None, help="32-char header string (default: model and version)")
    ap.add_argument("--index", help="also write a Zigbee2MQTT OTA index (JSON list with one entry) here")
    args = ap.parse_args()

    with open(args.input, "rb") as f:
        image = f.read()
    if len(image) < 32 or image[0] != 0xE9:
        print(f"error: {args.input} is not an ESP app image", file=sys.stderr)
        return 1

    version = file_version(args.version)
    header_string = args.header_string or f"{MODEL_ID} {args.version}"
    ota = build_ota(image, args.manufacturer, args.image_type, version, header_string)
    with open(args.output, "wb") as f:
        f.write(ota)

    if args.index:
        entry = {
            "fileName": os.path.basename(args.output),
            "fileVersion": version,
            "fileSize": len(ota),
            "url": os.path.basename(args.output),
            "imageType": args.image_type,
            "manufacturerCode": args.manufacturer,
            "sha512": hashlib.sha512(ota).hexdigest(),
            "otaHeaderString": header_string,
            "modelId": MODEL_ID,
        }
        with open(args.index, "w") as f:
            json.dump([entry], f, indent=2)
            f.write("\n")

    print(
        f"{args.output}: version {args.version} (0x{version:08X}), manufacturer 0x{args.manufacturer:04X}, "
        f"image type 0x{args.image_type:04X}, {len(ota)} bytes"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
