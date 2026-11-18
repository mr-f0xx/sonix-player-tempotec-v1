#!/usr/bin/env python3
"""Real TCP/HTTP and filesystem integration for the V1 transfer session."""
import contextlib
import http.client
import json
import os
from pathlib import Path
from queue import Queue, Empty
import socket
import subprocess
import sys
import tempfile
from threading import Thread
import time
import unittest
from urllib.parse import urlencode, urlsplit

DRIVER = Path(sys.argv.pop(1)).resolve()


class TransferTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name) / "sd"
        self.root.mkdir()
        self.secret = self.root.parent / "outside.flac"
        self.secret.write_bytes(b"outside the card")
        self.proc = subprocess.Popen([DRIVER, self.root], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        self.lines = Queue()
        self.reader = Thread(target=self.read_lines, daemon=True)
        self.reader.start()
        self.addCleanup(self.close_driver)
        self.command("no-wifi", "NO WIFI")
        self.command("no-card", "NO CARD")
        self.start_session()

    def close_driver(self):
        self.proc.stdin.close()
        try:
            self.proc.wait(timeout=4)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        self.reader.join()
        self.proc.stdout.close()
        self.assertEqual(self.proc.returncode, 0)

    def read_lines(self):
        for line in self.proc.stdout:
            self.lines.put(line.strip())
        self.lines.put(None)

    def command(self, value, prefix):
        self.proc.stdin.write(value + "\n")
        self.proc.stdin.flush()
        while True:
            try:
                line = self.lines.get(timeout=4)
            except Empty:
                self.fail(f"server did not respond to {value}")
            self.assertIsNotNone(line, f"server exited while waiting for {prefix}")
            if line.startswith(prefix):
                return line

    def start_session(self):
        _, addr, alias, self.code = self.command("start", "READY ").split()
        self.authority = urlsplit(addr).netloc
        self.alias = urlsplit(alias).netloc
        self.port = urlsplit(addr).port
        self.assertRegex(self.code, r"^[2-9A-HJ-NP-Z]{8}$")
        self.assertEqual(self.alias, f"sonix-transfer.local:{self.port}")

    def request(self, method, url, body=None, **headers):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=4)
        with contextlib.closing(conn):
            conn.request(method, url, body=body, headers=headers)
            res = conn.getresponse()
            return res.status, dict(res.headers), res.read()

    def pair(self):
        status, headers, _ = self.request("POST", "/pair", **{"X-Sonix-Pair": self.code})
        self.assertEqual(status, 204)
        cookie = headers["Set-Cookie"]
        self.assertIn("; HttpOnly; SameSite=Strict", cookie)
        return cookie.split(";", 1)[0]

    def action(self, method, route, cookie, status, body=None, **fields):
        actual, headers, data = self.request(method, f"/{route}?{urlencode(fields)}", body,
                                             **{"Cookie": cookie, "X-Sonix-Transfer": "1"})
        self.assertEqual(actual, status, (method, route, fields))
        return headers, data

    def raw_head(self, data):
        with socket.create_connection(("127.0.0.1", self.port), timeout=4) as conn:
            conn.sendall(data)
            response = conn.makefile("rb")
            with response:
                return int(response.readline().split()[1])

    def partial_upload(self, cookie, dest):
        conn = socket.create_connection(("127.0.0.1", self.port), timeout=3)
        self.addCleanup(conn.close)
        head = (f"PUT /upload?{urlencode({'path': dest})} HTTP/1.1\r\nHost: {self.authority}\r\n"
                f"Cookie: {cookie}\r\nX-Sonix-Transfer: 1\r\nContent-Length: 999999\r\n"
                "Expect: 100-continue\r\n\r\n")
        conn.sendall(head.encode())
        with conn.makefile("rb") as res:
            self.assertEqual(res.readline(), b"HTTP/1.1 100 Continue\r\n")
            self.assertEqual(res.readline(), b"\r\n")
        self.assertEqual(len(list(self.root.glob(".sonix-upload-*"))), 1)
        conn.sendall(b"fLaC" * 128)
        return conn

    def test_page_pairing_and_session_scope(self):
        self.assertEqual(self.request("GET", "/list?path=/")[0], 401)
        self.assertEqual(self.request("POST", "/pair", **{"X-Sonix-Pair": "AAAAAAAA"})[0], 401)
        status, headers, page = self.request("GET", "/", Host=self.alias)
        self.assertEqual(status, 200)
        self.assertIn("no-store", headers["Cache-Control"])
        self.assertEqual(len(page), int(headers["Content-Length"]))
        self.assertIn(b"Wi-Fi transfer", page)
        self.assertIn(b"#12b3aa", page)
        self.assertNotIn(b"{{", page)  # actual render, not just the generated template
        self.assertIn(b"Test\\'s code \\\"bad\\\": \\u003c/script>\\\\end", page)
        self.assertNotIn(self.code.encode(), page)
        self.assertEqual(self.request("GET", "/", Host="unrelated.example")[0], 403)
        cookie = self.pair()
        self.assertEqual(self.request("POST", "/create?path=/no-header", Cookie=cookie)[0], 403)
        self.assertEqual(self.request("OPTIONS", "/upload?path=/foreign", Cookie=cookie)[0], 405)
        self.assertEqual(self.request("GET", "/list?path=/", Cookie=cookie)[0], 200)
        old_port = self.port
        self.command("stop", "STOPPED")
        with self.assertRaises(ConnectionRefusedError):
            socket.create_connection(("127.0.0.1", old_port), timeout=3)
        self.start_session()
        self.assertNotEqual(self.pair(), cookie)
        self.assertEqual(self.request("GET", "/list?path=/", Cookie=cookie)[0], 401)

    def test_nested_albums_upload_list_download_rename_move_and_delete(self):
        cookie = self.pair()
        self.action("POST", "create", cookie, 201, path="/Música/")
        self.action("POST", "create", cookie, 204, path="/Música")
        self.action("POST", "create", cookie, 201, path="/Música/Album & B/")
        dest = "/Música/Album & B/01 + %.flac"
        data = b"fLaC" + os.urandom(1024 * 1024)
        self.action("PUT", "upload", cookie, 201, data, path=dest)
        self.assertEqual((self.root / dest[1:]).read_bytes(), data)
        self.assertFalse(list(self.root.rglob(".sonix-upload-*")))
        self.action("PUT", "upload", cookie, 409, b"not a replacement", path=dest)
        self.action("POST", "create", cookie, 409, path=dest)
        (self.root / "Música" / ".hidden").touch()
        self.assertEqual(len(json.loads(self.action("GET", "list", cookie, 200, path="/Música/")[1])), 1)
        rows = json.loads(self.action("GET", "list", cookie, 200, path="/Música/", hidden=1)[1])
        self.assertIn(".hidden", [r["name"] for r in rows])
        self.assertIn("/Música/Album & B/", [r["path"] for r in rows])
        rows = json.loads(self.action("GET", "list", cookie, 200, path="/Música/Album & B/")[1])
        self.assertEqual((rows[0]["path"], rows[0]["size"]), (dest, len(data)))
        headers, received = self.action("GET", "download", cookie, 200, path=dest)
        self.assertEqual(data, received)
        self.assertIn("attachment; filename*=UTF-8''01%20%2B%20%25.flac", headers["Content-Disposition"])
        self.action("POST", "move", cookie, 204, oldPath=dest, newPath="/Über.flac")
        self.assertEqual((self.root / "Über.flac").read_bytes(), data)
        self.action("POST", "delete", cookie, 204, path="/Música/")
        self.assertFalse((self.root / "Música").exists())

    def test_requires_card_paths_and_bounded_framing(self):
        cookie = self.pair()
        (self.root / "link").symlink_to(self.root.parent, target_is_directory=True)
        (self.root / "file-link").symlink_to(self.secret)
        os.mkfifo(self.root / "fifo")
        for path in ["../outside.flac", "/../outside.flac", "/..", "/../", "//x", "/x\\y", "/x\x00y"]:
            self.action("GET", "list", cookie, 400, path=path)
        self.action("GET", "list", cookie, 403, path="/link/")
        self.action("GET", "download", cookie, 403, path="/file-link")
        self.action("GET", "download", cookie, 403, path="/fifo")
        self.action("DELETE", "delete", cookie, 405, path="/file-link")
        self.action("POST", "delete", cookie, 403, path="/")
        self.action("PUT", "upload", cookie, 409, b"cannot follow", path="/file-link")
        self.assertEqual(self.secret.read_bytes(), b"outside the card")
        self.assertEqual(json.loads(self.action("GET", "list", cookie, 200, path="/", hidden=1)[1]), [])
        self.assertEqual(self.request("GET", "/list?path=%2Fbad%FF", Cookie=cookie)[0], 400)
        self.assertEqual(self.request("GET", "/list?path=%2Fbad%", Cookie=cookie)[0], 400)
        self.assertEqual(self.request("GET", "/list?path=/&path=/other", Cookie=cookie)[0], 400)
        head = (f"PUT /upload?path=/huge.flac HTTP/1.1\r\nHost: {self.authority}\r\n"
                f"Cookie: {cookie}\r\nX-Sonix-Transfer: 1\r\n").encode()
        self.assertEqual(self.raw_head(head + b"\r\n"), 411)
        self.assertEqual(self.raw_head(head + b"Content-Length: 4294967296\r\n\r\n"), 413)
        self.assertEqual(self.raw_head(head + b"Content-Length: 9999999999999999999999\r\n\r\n"), 400)
        self.assertEqual(self.raw_head(head + b"Content-Length: 1\r\nContent-Length: 1\r\n\r\n"), 400)
        self.assertEqual(self.raw_head(head + b"Transfer-Encoding: chunked\r\n\r\n"), 400)
        self.assertEqual(self.raw_head(head + b"X-Large: " + b"a" * 8300 + b"\r\n\r\n"), 431)
        self.assertFalse((self.root / "huge.flac").exists())

    def test_aborted_and_stopped_uploads_are_removed(self):
        cookie = self.pair()
        aborted = self.partial_upload(cookie, "/abort.flac")
        aborted.close()
        # A subsequent completed request also establishes that the first call
        # has left the serial worker and run its cleanup.
        self.action("GET", "list", cookie, 200, path="/")
        self.assertEqual(list(self.root.iterdir()), [])

        pending = self.partial_upload(cookie, "/stop.flac")
        now = time.monotonic()
        self.command("stop", "STOPPED")
        self.assertLess(time.monotonic() - now, 2, "stop must not wait for the socket timeout")
        with contextlib.suppress(ConnectionResetError):
            self.assertEqual(pending.recv(1), b"")
        self.assertEqual(list(self.root.iterdir()), [])

    def test_upload_completion_is_coalesced_and_consumed_once(self):
        cookie = self.pair()

        def completed():
            return self.command("completion", "COMPLETION ") == "COMPLETION 1"

        self.assertFalse(completed())  # serving/pairing is not a completion
        self.action("PUT", "upload", cookie, 201, b"track 1", path="/01.flac")
        self.assertFalse(completed())
        self.action("PUT", "upload", cookie, 201, b"track 2", path="/02.flac")
        self.assertFalse(completed())
        time.sleep(1.1)
        self.assertTrue(completed())
        self.assertFalse(completed())

        # No confirmation for a conflict, and a new in-progress upload
        # replaces the pending event instead of flashing mid-transfer.
        self.action("PUT", "upload", cookie, 409, b"conflict", path="/02.flac")
        self.action("PUT", "upload", cookie, 201, b"next", path="/03.flac")
        pending = self.partial_upload(cookie, "/incomplete.flac")
        time.sleep(1.1)
        self.assertFalse(completed())
        pending.close()
        self.action("GET", "list", cookie, 200, path="/")
        time.sleep(1.1)
        self.assertFalse(completed())

        self.action("PUT", "upload", cookie, 201, b"last", path="/04.flac")
        self.command("stop", "STOPPED")
        self.start_session()
        self.assertFalse(completed())

    def test_card_fd_is_pinned_and_idle_browser_does_not_block(self):
        original = self.root.parent / "original-card"
        self.root.rename(original)
        self.root.mkdir()
        (self.root / "not-on-card.flac").touch()
        cookie = self.pair()
        with socket.create_connection(("127.0.0.1", self.port), timeout=3):
            self.assertEqual(self.request("GET", "/", Cookie=cookie)[0], 200)
            self.action("PUT", "upload", cookie, 201, b"track", path="/on-card.flac")
        self.assertEqual((original / "on-card.flac").read_bytes(), b"track")
        self.assertFalse((self.root / "on-card.flac").exists())


if __name__ == "__main__":
    unittest.main()
