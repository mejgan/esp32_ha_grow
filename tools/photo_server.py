#!/usr/bin/env python3
"""GrowBox photo server: receives the cameras' photos and turns them into time-lapse videos.

Only Python 3.8+ is needed (no packages); `video` also needs ffmpeg.

    python tools/photo_server.py serve --dir photos --token MYSECRET
    python tools/photo_server.py video --dir photos             # photos/floor1.mp4, floor2.mp4, ...

The GrowBox uploads to http://<this PC>:8088/upload (menuconfig: Grow Box -> Cameras -> Upload URL) with the same
token. Photos are saved by this PC's clock as photos/floorN/YYYY-MM-DD_HH-MM-SS.jpg. Open http://<this PC>:8088/
?token=MYSECRET in a browser to see the latest photo of every floor.
"""

import argparse
import datetime
import hmac
import html
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

MAX_PHOTO_BYTES = 16 * 1024 * 1024
JPEG_SOI = b"\xff\xd8"
JPEG_EOI = b"\xff\xd9"
FLOOR_RE = re.compile(r"^[1-9]$")
NAME_RE = re.compile(r"^\d{4}-\d{2}-\d{2}_\d{2}-\d{2}-\d{2}(_\d+)?\.jpg$")


def trim_jpeg(data: bytes) -> bytes:
    """The camera's buffer may hold padding after the image; cut everything after the last end-of-image marker."""
    end = data.rfind(JPEG_EOI)
    return data[: end + 2] if end > 0 else data


def photo_files(folder):
    try:
        return sorted(f for f in os.listdir(folder) if NAME_RE.match(f))
    except FileNotFoundError:
        return []


class PhotoStore:
    def __init__(self, root):
        self.root = os.path.abspath(root)
        self.lock = threading.Lock()
        os.makedirs(self.root, exist_ok=True)

    def floors(self):
        out = []
        for name in sorted(os.listdir(self.root)):
            m = re.match(r"^floor([1-9])$", name)
            if m and os.path.isdir(os.path.join(self.root, name)):
                out.append(int(m.group(1)))
        return out

    def save(self, floor: int, data: bytes, now=None) -> str:
        now = now or datetime.datetime.now()
        folder = os.path.join(self.root, f"floor{floor}")
        os.makedirs(folder, exist_ok=True)
        base = now.strftime("%Y-%m-%d_%H-%M-%S")
        with self.lock:
            name, n = f"{base}.jpg", 1
            while os.path.exists(os.path.join(folder, name)):
                name, n = f"{base}_{n}.jpg", n + 1
            fd, tmp = tempfile.mkstemp(dir=folder, suffix=".part")
            with os.fdopen(fd, "wb") as f:
                f.write(data)
            path = os.path.join(folder, name)
            os.replace(tmp, path)
        return path

    def latest(self, floor: int):
        folder = os.path.join(self.root, f"floor{floor}")
        files = photo_files(folder)
        return (os.path.join(folder, files[-1]), len(files)) if files else (None, 0)


def make_handler(store: PhotoStore, token: str):
    class Handler(BaseHTTPRequestHandler):
        server_version = "GrowBoxPhotos/1"

        def log_message(self, fmt, *args):
            sys.stderr.write("%s %s\n" % (datetime.datetime.now().strftime("%H:%M:%S"), fmt % args))

        def _send(self, code, body=b"", ctype="text/plain; charset=utf-8"):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(body)

        def _authorized(self):
            if not token:
                return True
            header = self.headers.get("Authorization", "")
            given = header[7:] if header.startswith("Bearer ") else ""
            if not given:
                given = parse_qs(urlparse(self.path).query).get("token", [""])[0]
            return hmac.compare_digest(given.encode(), token.encode())

        def do_POST(self):
            if urlparse(self.path).path != "/upload":
                return self._send(404, b"not found\n")
            if not self._authorized():
                return self._send(401, b"bad token\n")
            floor = self.headers.get("X-GrowBox-Floor", "")
            if not FLOOR_RE.match(floor):
                return self._send(400, b"missing or bad X-GrowBox-Floor\n")
            try:
                length = int(self.headers.get("Content-Length", ""))
            except ValueError:
                return self._send(411, b"Content-Length required\n")
            if length <= 0 or length > MAX_PHOTO_BYTES:
                return self._send(413, b"bad size\n")
            data = self.rfile.read(length)
            if len(data) != length or not data.startswith(JPEG_SOI):
                return self._send(400, b"not a complete JPEG\n")
            path = store.save(int(floor), trim_jpeg(data))
            device_time = self.headers.get("X-GrowBox-Time", "unknown")
            self.log_message("floor %s: saved %s (%d bytes, box clock %s)", floor, os.path.relpath(path, store.root),
                             length, device_time)
            body = json.dumps({"saved": os.path.relpath(path, store.root)}).encode()
            self._send(201, body + b"\n", "application/json")

        def do_GET(self):
            url = urlparse(self.path)
            if not self._authorized():
                return self._send(401, b"add ?token=... to the address\n")
            m = re.match(r"^/latest/floor([1-9])\.jpg$", url.path)
            if m:
                path, _ = store.latest(int(m.group(1)))
                if not path:
                    return self._send(404, b"no photo yet\n")
                with open(path, "rb") as f:
                    return self._send(200, f.read(), "image/jpeg")
            if url.path != "/":
                return self._send(404, b"not found\n")
            q = f"?token={html.escape(parse_qs(url.query).get('token', [''])[0], quote=True)}" if token else ""
            rows = []
            for floor in store.floors():
                path, count = store.latest(floor)
                when = os.path.basename(path)[:-4].replace("_", " ") if path else "-"
                rows.append(f"<figure><img src='/latest/floor{floor}.jpg{q}' alt='floor {floor}'>"
                            f"<figcaption>Floor {floor} &middot; {count} photos &middot; latest {when}"
                            f"</figcaption></figure>")
            page = ("<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'>"
                    "<title>GrowBox photos</title><meta http-equiv=refresh content=300>"
                    "<style>body{font-family:sans-serif;margin:16px;background:#111;color:#eee}"
                    "img{max-width:100%;border-radius:6px}figure{margin:0 0 24px}</style>"
                    "<h1>GrowBox photos</h1>" + ("".join(rows) or "<p>No photos yet.</p>"))
            self._send(200, page.encode(), "text/html; charset=utf-8")

        do_HEAD = do_GET

    return Handler


def make_server(directory, token, bind="0.0.0.0", port=8088):
    store = PhotoStore(directory)
    return ThreadingHTTPServer((bind, port), make_handler(store, token)), store


def make_videos(directory, fps=30, crf=20, width=1600):
    if shutil.which("ffmpeg") is None:
        print("ffmpeg not found: install it and make sure it is on PATH", file=sys.stderr)
        return 1
    store = PhotoStore(directory)
    floors = store.floors()
    if not floors:
        print(f"no photos in {store.root}", file=sys.stderr)
        return 1
    for floor in floors:
        folder = os.path.join(store.root, f"floor{floor}")
        files = photo_files(folder)
        if len(files) < 2:
            print(f"floor {floor}: only {len(files)} photo(s), skipped")
            continue
        # A concat list works on every OS (ffmpeg's glob pattern does not on Windows).
        listing = os.path.join(folder, "frames.txt")
        with open(listing, "w") as f:
            for name in files:
                f.write(f"file '{name}'\nduration {1 / fps:.6f}\n")
            f.write(f"file '{files[-1]}'\n")
        out = os.path.join(store.root, f"floor{floor}.mp4")
        cmd = ["ffmpeg", "-y", "-loglevel", "error", "-f", "concat", "-safe", "0", "-i", listing,
               "-vf", f"deflicker,scale={width}:-2", "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", str(crf),
               "-r", str(fps), out]
        print(f"floor {floor}: {len(files)} photos -> {out}")
        result = subprocess.run(cmd)
        os.remove(listing)
        if result.returncode != 0:
            return result.returncode
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("serve", help="receive photos")
    s.add_argument("--dir", default="photos")
    s.add_argument("--token", default=os.environ.get("GROWBOX_PHOTO_TOKEN", ""),
                   help="shared secret, same as the GrowBox's upload token (or env GROWBOX_PHOTO_TOKEN)")
    s.add_argument("--bind", default="0.0.0.0")
    s.add_argument("--port", type=int, default=8088)
    v = sub.add_parser("video", help="make floorN.mp4 time-lapse videos with ffmpeg")
    v.add_argument("--dir", default="photos")
    v.add_argument("--fps", type=int, default=30, help="photos per second of video (default 30)")
    v.add_argument("--crf", type=int, default=20, help="x264 quality, lower is better (default 20)")
    v.add_argument("--width", type=int, default=1600)
    args = ap.parse_args()

    if args.cmd == "video":
        return make_videos(args.dir, args.fps, args.crf, args.width)
    if not args.token:
        print("warning: no --token, anyone on the network can upload photos", file=sys.stderr)
    server, store = make_server(args.dir, args.token, args.bind, args.port)
    print(f"saving photos to {store.root}; upload URL http://<this PC>:{args.port}/upload")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
