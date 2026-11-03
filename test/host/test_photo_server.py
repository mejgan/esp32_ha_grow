"""Tests for tools/photo_server.py: run with  python -m unittest test/host/test_photo_server.py"""

import http.client
import os
import sys
import tempfile
import threading
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
import photo_server  # noqa: E402

JPEG = b"\xff\xd8\xff\xe0" + bytes(range(256)) * 10 + b"\xff\xd9"


class PhotoServerTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.server, self.store = photo_server.make_server(self.tmp.name, "s3cret", "127.0.0.1", 0)
        self.port = self.server.server_address[1]
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.tmp.cleanup()

    def request(self, method, path, body=None, headers=None):
        c = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
        c.request(method, path, body=body, headers=headers or {})
        r = c.getresponse()
        data = r.read()
        c.close()
        return r.status, data

    def upload(self, data=JPEG, floor="2", token="s3cret"):
        headers = {"Content-Type": "image/jpeg", "X-GrowBox-Floor": floor, "X-GrowBox-Time": "2026-11-03T13:30:00"}
        if token is not None:
            headers["Authorization"] = f"Bearer {token}"
        return self.request("POST", "/upload", data, headers)

    def test_upload_saves_and_trims(self):
        padded = JPEG + b"\x00" * 37  # the camera's buffer can hold padding after the image
        status, body = self.upload(padded)
        self.assertEqual(status, 201, body)
        path, count = self.store.latest(2)
        self.assertEqual(count, 1)
        with open(path, "rb") as f:
            self.assertEqual(f.read(), JPEG)
        self.assertRegex(os.path.basename(path), photo_server.NAME_RE)

    def test_same_second_does_not_overwrite(self):
        for _ in range(3):
            self.assertEqual(self.upload()[0], 201)
        self.assertEqual(self.store.latest(2)[1], 3)

    def test_rejects(self):
        self.assertEqual(self.upload(token="wrong")[0], 401)
        self.assertEqual(self.upload(token=None)[0], 401)
        self.assertEqual(self.upload(floor="x")[0], 400)
        self.assertEqual(self.upload(floor="../../etc")[0], 400)
        self.assertEqual(self.upload(data=b"not a jpeg")[0], 400)
        self.assertEqual(self.request("POST", "/other", JPEG, {"Authorization": "Bearer s3cret"})[0], 404)
        self.assertEqual(self.store.floors(), [])

    def test_index_and_latest(self):
        self.upload()
        self.assertEqual(self.request("GET", "/")[0], 401)
        status, page = self.request("GET", "/?token=s3cret")
        self.assertEqual(status, 200)
        self.assertIn(b"Floor 2", page)
        status, img = self.request("GET", "/latest/floor2.jpg?token=s3cret")
        self.assertEqual(status, 200)
        self.assertEqual(img, JPEG)
        self.assertEqual(self.request("GET", "/latest/floor3.jpg?token=s3cret")[0], 404)


if __name__ == "__main__":
    unittest.main()
