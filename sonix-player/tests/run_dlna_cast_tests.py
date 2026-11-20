#!/usr/bin/env python3
"""The cast side of DLNA, end to end, over real sockets.

The driver is the player: it searches with SSDP, reads the renderer's
description, asks for its sink protocol info, sends the DIDL, and then serves
the track over HTTP -- all production code. This file is the other end: an
SSDP responder, a renderer description, the two SOAP services, and then the
television fetching the URL it was handed.

That split is the point. The bug behind issue #363 was in what the player
answered when a television said "Range: bytes=0-", and in what it told the
renderer the stream was: neither is visible without a renderer that asks.
"""
import http.client
import random
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import xml.etree.ElementTree as ET
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from queue import Empty, Queue
from urllib.parse import urlsplit

DRIVER = Path(sys.argv.pop(1)).resolve()

RENDERER_NAME = "Sonix Test Renderer"
AVT = "urn:schemas-upnp-org:service:AVTransport:1"
CM = "urn:schemas-upnp-org:service:ConnectionManager:1"
# What a television that follows DLNA asks for, and what it will not play
# without.
FEATURES = "DLNA.ORG_OP=01;DLNA.ORG_CI=0;DLNA.ORG_FLAGS=01700000000000000000000000000000"

DEVICE_XML = f"""<?xml version="1.0"?>
<root xmlns="urn:schemas-upnp-org:device-1-0">
  <specVersion><major>1</major><minor>0</minor></specVersion>
  <device>
    <deviceType>urn:schemas-upnp-org:device:MediaRenderer:1</deviceType>
    <friendlyName>{RENDERER_NAME}</friendlyName>
    <manufacturer>Sonix</manufacturer>
    <modelName>Test Television</modelName>
    <UDN>uuid:sonix-test-renderer</UDN>
    <serviceList>
      <service>
        <serviceType>{AVT}</serviceType>
        <serviceId>urn:upnp-org:serviceId:AVTransport</serviceId>
        <controlURL>/avt/control</controlURL>
        <eventSubURL>/avt/event</eventSubURL>
        <SCPDURL>/avt/scpd.xml</SCPDURL>
      </service>
      <service>
        <serviceType>{CM}</serviceType>
        <serviceId>urn:upnp-org:serviceId:ConnectionManager</serviceId>
        <controlURL>/cm/control</controlURL>
        <eventSubURL>/cm/event</eventSubURL>
        <SCPDURL>/cm/scpd.xml</SCPDURL>
      </service>
    </serviceList>
  </device>
</root>
"""


def envelope(body):
    return (
        '<?xml version="1.0"?>'
        '<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
        's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/">'
        f"<s:Body>{body}</s:Body></s:Envelope>"
    )


class FakeRenderer:
    """An SSDP responder, a description and the two services, on real ports."""

    def __init__(self, sink):
        self.sink = sink
        self.lock = threading.Lock()
        self.actions = []  # every SOAPAction the player sent, in order
        self.current_uri = None
        self.metadata = None
        self.protocol_info_asked = 0
        self.httpd = None
        self.ssdp = None
        self.closed = threading.Event()

    # -- HTTP -------------------------------------------------------------

    def start(self):
        renderer = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *args):
                pass

            def reply(self, body, status=200):
                data = body.encode()
                self.send_response(status)
                self.send_header("Content-Type", 'text/xml; charset="utf-8"')
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def do_GET(self):
                if self.path == "/device.xml":
                    self.reply(DEVICE_XML)
                else:
                    self.reply("", status=404)

            def do_POST(self):
                length = int(self.headers.get("Content-Length") or 0)
                payload = self.rfile.read(length).decode(errors="replace")
                action = (self.headers.get("SOAPAction") or "").strip('"').split("#")[-1]
                with renderer.lock:
                    renderer.actions.append(action)
                if self.path == "/cm/control" and action == "GetProtocolInfo":
                    with renderer.lock:
                        renderer.protocol_info_asked += 1
                        sink = renderer.sink
                    self.reply(envelope(
                        f'<u:GetProtocolInfoResponse xmlns:u="{CM}">'
                        f"<Source></Source><Sink>{sink}</Sink>"
                        "</u:GetProtocolInfoResponse>"))
                elif self.path == "/avt/control" and action == "SetAVTransportURI":
                    root = ET.fromstring(payload)
                    uri = root.find(".//CurrentURI")
                    meta = root.find(".//CurrentURIMetaData")
                    with renderer.lock:
                        renderer.current_uri = uri.text if uri is not None else None
                        renderer.metadata = meta.text if meta is not None else None
                    self.reply(envelope(
                        f'<u:SetAVTransportURIResponse xmlns:u="{AVT}"/>'))
                elif self.path == "/avt/control":
                    self.reply(envelope(f'<u:{action}Response xmlns:u="{AVT}"/>'))
                else:
                    self.reply("", status=404)

        self.httpd = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.httpd.daemon_threads = True
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

        self.ssdp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.ssdp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            self.ssdp.bind(("", 1900))
        except OSError as error:
            raise unittest.SkipTest(f"SSDP port 1900 is taken: {error}")
        group = socket.inet_aton("239.255.255.250")
        self.ssdp.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                             group + socket.inet_aton("0.0.0.0"))
        self.ssdp.settimeout(0.2)
        threading.Thread(target=self.answer_search, daemon=True).start()

    def answer_search(self):
        location = f"http://127.0.0.1:{self.httpd.server_address[1]}/device.xml"
        response = (
            "HTTP/1.1 200 OK\r\n"
            "CACHE-CONTROL: max-age=1800\r\n"
            "EXT:\r\n"
            f"LOCATION: {location}\r\n"
            "SERVER: Test/1.0 UPnP/1.0 SonixRenderer/1.0\r\n"
            "ST: urn:schemas-upnp-org:device:MediaRenderer:1\r\n"
            "USN: uuid:sonix-test-renderer::urn:schemas-upnp-org:device:MediaRenderer:1\r\n"
            "Content-Length: 0\r\n\r\n"
        )
        while not self.closed.is_set():
            try:
                packet, address = self.ssdp.recvfrom(4096)
            except socket.timeout:
                continue
            except OSError:
                return
            if b"M-SEARCH" in packet:
                try:
                    self.ssdp.sendto(response.encode(), address)
                except OSError:
                    return

    def set_sink(self, sink):
        with self.lock:
            self.sink = sink

    def stop(self):
        self.closed.set()
        if self.httpd:
            self.httpd.shutdown()
            self.httpd.server_close()
        if self.ssdp:
            self.ssdp.close()


class Driver:
    """The player, driven a line at a time."""

    def __init__(self):
        self.proc = subprocess.Popen([str(DRIVER)], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, text=True)
        self.lines = Queue()
        threading.Thread(target=self.read, daemon=True).start()

    def read(self):
        for line in self.proc.stdout:
            self.lines.put(line.rstrip("\n"))
        self.lines.put(None)

    def send(self, command):
        self.proc.stdin.write(command + "\n")
        self.proc.stdin.flush()

    def expect(self, prefix, timeout=10):
        deadline = time.time() + timeout
        while True:
            remaining = deadline - time.time()
            if remaining <= 0:
                raise AssertionError(f"no line starting with {prefix!r}")
            try:
                line = self.lines.get(timeout=remaining)
            except Empty:
                raise AssertionError(f"no line starting with {prefix!r}")
            if line is None:
                raise AssertionError("the driver exited")
            if line.startswith(prefix):
                return line[len(prefix):].strip()

    def status(self):
        self.send("status")
        return self.expect("STATUS")

    def wait_for_status(self, predicate, timeout=20):
        deadline = time.time() + timeout
        seen = ""
        while time.time() < deadline:
            seen = self.status()
            if predicate(seen):
                return seen
            time.sleep(0.1)
        raise AssertionError(f"status stayed {seen!r}")

    def close(self):
        try:
            self.send("quit")
            self.proc.stdin.close()
            self.proc.wait(timeout=5)
        except (BrokenPipeError, subprocess.TimeoutExpired):
            self.proc.kill()
            self.proc.wait()
        self.proc.stdout.close()
        assert self.proc.returncode == 0


class CastTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.tmp.name)
        rng = random.Random(363)
        cls.tracks = {}
        for name, size in (("track.mp3", 40_000), ("track.flac", 30_000),
                          ("track.ogg", 12_000), ("track.xyz", 4_000)):
            path = cls.root / name
            path.write_bytes(bytes(rng.getrandbits(8) for _ in range(size)))
            cls.tracks[path.suffix] = path
        # Big enough that an unread socket fills and the writer has to wait:
        # that is what tells one threaded stream from one at a time.
        cls.slow = cls.root / "long.mp3"
        cls.slow.write_bytes(b"\xff\xfb\x90\x00" * (3 * 1024 * 1024))

        cls.renderer = FakeRenderer("http-get:*:audio/mpeg:*,http-get:*:audio/flac:*,"
                                    "http-get:*:audio/ogg:*,http-get:*:audio/mp4:*")
        cls.renderer.start()
        cls.driver = Driver()
        cls.driver.send("scan")
        cls.driver.expect("SCAN")
        status = cls.driver.wait_for_status(lambda s: not s.startswith("Searching"), timeout=30)
        assert "Found" in status or "renderer" in status, status
        cls.driver.send("list")
        count = int(cls.driver.expect("TARGETS").split()[0])
        targets = [cls.driver.expect("TARGET").split(" ", 1) for _ in range(count)]
        cls.index = next(int(i) for i, name in targets if name == RENDERER_NAME)

    @classmethod
    def tearDownClass(cls):
        cls.driver.close()
        cls.renderer.stop()
        cls.tmp.cleanup()

    # -- helpers ----------------------------------------------------------

    def cast(self, path):
        self.driver.send(f"select {self.index}")
        self.driver.expect("SELECT")
        self.driver.send(f"cast {path}")
        self.assertEqual(self.driver.expect("CAST"), "1")
        return self.driver.wait_for_status(lambda s: s.startswith("Playing on "))

    def uri(self):
        with self.renderer.lock:
            return self.renderer.current_uri

    def fetch(self, url, headers=None, method="GET"):
        parts = urlsplit(url)
        connection = http.client.HTTPConnection(parts.hostname, parts.port, timeout=20)
        self.addCleanup(connection.close)
        connection.request(method, parts.path, headers=headers or {})
        response = connection.getresponse()
        body = response.read()
        return response, body

    def path(self, suffix):
        return self.tracks[suffix]

    # -- tests ------------------------------------------------------------

    def test_scan_finds_the_renderer(self):
        self.assertIsNotNone(self.index)

    def test_first_get_serves_the_whole_track(self):
        """The television's opening request: "Range: bytes=0-".

        Its second number is missing, which means "to the end". This used to
        come back as a one-byte body -- issue #363.
        """
        self.cast(self.path(".mp3"))
        url = self.uri()
        response, body = self.fetch(url, {"Range": "bytes=0-"})
        size = self.path(".mp3").stat().st_size
        self.assertEqual(response.status, 206)
        self.assertEqual(len(body), size)
        self.assertEqual(body, self.path(".mp3").read_bytes())
        self.assertEqual(response.getheader("Content-Range"),
                         f"bytes 0-{size - 1}/{size}")
        self.assertEqual(response.getheader("Content-Length"), str(size))

    def test_headers_say_what_the_stream_is(self):
        self.cast(self.path(".mp3"))
        url = self.uri()
        response, body = self.fetch(url, {"getcontentFeatures.dlna.org": "1",
                                         "transferMode.dlna.org": "Streaming"})
        self.assertEqual(response.status, 200)
        self.assertEqual(len(body), len(self.path(".mp3").read_bytes()))
        self.assertEqual(response.getheader("contentFeatures.dlna.org"),
                         f"DLNA.ORG_PN=MP3;{FEATURES}")
        self.assertEqual(response.getheader("transferMode.dlna.org"), "Streaming")
        self.assertEqual(response.getheader("Content-Type"), "audio/mpeg")
        self.assertEqual(response.getheader("Accept-Ranges"), "bytes")
        self.assertIn("UPnP/1.0", response.getheader("Server"))
        self.assertRegex(response.getheader("Date"),
                         r"^[A-Z][a-z]{2}, \d{2} [A-Z][a-z]{2} \d{4} "
                         r"\d{2}:\d{2}:\d{2} GMT$")

    def test_the_url_carries_the_track_format(self):
        """Some televisions decide what a stream is from its name."""
        self.cast(self.path(".flac"))
        self.assertTrue(urlsplit(self.uri()).path.endswith("/track.flac"),
                        self.uri())
        self.cast(self.path(".mp3"))
        self.assertTrue(urlsplit(self.uri()).path.endswith("/track.mp3"),
                        self.uri())

    def test_range_forms(self):
        self.cast(self.path(".mp3"))
        url = self.uri()
        data = self.path(".mp3").read_bytes()
        size = len(data)
        cases = {
            "bytes=0-9": (0, 9),
            "bytes=10-19": (10, 19),
            f"bytes={size - 100}-": (size - 100, size - 1),
            "bytes=-100": (size - 100, size - 1),
            f"bytes=0-{size + 1000}": (0, size - 1),
        }
        for header, (first, last) in cases.items():
            with self.subTest(range=header):
                response, body = self.fetch(url, {"Range": header})
                self.assertEqual(response.status, 206)
                self.assertEqual(body, data[first:last + 1])
                self.assertEqual(response.getheader("Content-Range"),
                                 f"bytes {first}-{last}/{size}")

    def test_a_range_past_the_end_is_refused(self):
        self.cast(self.path(".mp3"))
        size = self.path(".mp3").stat().st_size
        response, body = self.fetch(self.uri(), {"Range": f"bytes={size}-"})
        self.assertEqual(response.status, 416)
        self.assertEqual(response.getheader("Content-Range"), f"bytes */{size}")
        self.assertEqual(body, b"")

    def test_head_answers_without_a_body(self):
        self.cast(self.path(".mp3"))
        size = self.path(".mp3").stat().st_size
        response, body = self.fetch(self.uri(), method="HEAD")
        self.assertEqual(response.status, 200)
        self.assertEqual(response.getheader("Content-Length"), str(size))
        self.assertEqual(body, b"")

    def test_the_didl_describes_a_playable_track(self):
        self.cast(self.path(".mp3"))
        with self.renderer.lock:
            metadata = self.renderer.metadata
        self.assertTrue(metadata.startswith("<?xml") or "<DIDL-Lite" in metadata)
        didl = ET.fromstring(metadata)
        ns = {"didl": "urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/",
              "upnp": "urn:schemas-upnp-org:metadata-1-0/upnp/",
              "dc": "http://purl.org/dc/elements/1.1/"}
        item = didl.find("didl:item", ns)
        self.assertIsNotNone(item)
        self.assertEqual(item.find("dc:title", ns).text, "track.mp3")
        self.assertEqual(item.find("upnp:class", ns).text,
                         "object.item.audioItem.musicTrack")
        res = item.find("didl:res", ns)
        # A star here is exactly what a DLNA television refuses.
        self.assertEqual(res.get("protocolInfo"), f"http-get:*:audio/mpeg:DLNA.ORG_PN=MP3;{FEATURES}")
        self.assertEqual(int(res.get("size")), self.path(".mp3").stat().st_size)
        self.assertEqual(res.text, self.uri())

    def test_the_renderer_picks_the_spelling_of_the_format(self):
        """FLAC is "audio/x-flac" to some televisions, "audio/flac" to others."""
        self.renderer.set_sink("http-get:*:audio/x-flac:*,http-get:*:audio/mpeg:*")
        self.addCleanup(self.renderer.set_sink,
                        "http-get:*:audio/mpeg:*,http-get:*:audio/flac:*,"
                        "http-get:*:audio/ogg:*,http-get:*:audio/mp4:*")
        self.cast(self.path(".flac"))
        response, _ = self.fetch(self.uri())
        self.assertEqual(response.getheader("Content-Type"), "audio/x-flac")
        didl = ET.fromstring(self.renderer.metadata)
        res = didl.find(".//{urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/}res")
        self.assertTrue(res.get("protocolInfo").startswith("http-get:*:audio/x-flac:"),
                        res.get("protocolInfo"))

    def test_a_format_dlna_has_no_profile_for_is_not_named(self):
        self.cast(self.path(".ogg"))
        response, _ = self.fetch(self.uri())
        self.assertEqual(response.getheader("Content-Type"), "audio/ogg")
        self.assertNotIn("DLNA.ORG_PN", response.getheader("contentFeatures.dlna.org"))

    def test_an_unknown_extension_is_still_served(self):
        self.cast(self.path(".xyz"))
        response, body = self.fetch(self.uri(), {"Range": "bytes=0-"})
        self.assertEqual(response.status, 206)
        self.assertEqual(body, self.path(".xyz").read_bytes())
        self.assertEqual(response.getheader("Content-Type"), "application/octet-stream")

    def test_another_token_is_not_served(self):
        self.cast(self.path(".mp3"))
        parts = urlsplit(self.uri())
        response, _ = self.fetch(f"http://{parts.hostname}:{parts.port}/sonix/deadbeef/track.mp3")
        self.assertEqual(response.status, 404)

    def test_a_second_renderer_is_served_while_the_first_reads(self):
        """A seek opens a new connection before the old one is finished."""
        self.cast(self.slow)
        url = self.uri()
        parts = urlsplit(url)
        # A tiny receive window, so the writer really has to wait rather than
        # filling buffers a 12 MB track would fit in.
        stalled = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        stalled.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        stalled.settimeout(10)
        stalled.connect((parts.hostname, parts.port))
        self.addCleanup(stalled.close)
        # "bytes=1-" on purpose: the first byte is already there, so this asks
        # for the whole rest of a long track without depending on how the
        # opening "bytes=0-" is read.
        stalled.sendall(f"GET {parts.path} HTTP/1.1\r\nHost: tv\r\n"
                        f"Range: bytes=1-\r\n\r\n".encode())
        stalled.recv(64)  # and then stop reading, so the writer blocks
        started = time.time()
        response, body = self.fetch(url)
        self.assertEqual(response.status, 200)
        self.assertEqual(len(body), self.slow.stat().st_size)
        self.assertLess(time.time() - started, 5,
                        "the second request waited for the first to finish")

    def test_stop_withdraws_the_url(self):
        self.cast(self.path(".mp3"))
        url = self.uri()
        self.driver.send("stop")
        self.driver.expect("STOP")
        self.driver.wait_for_status(lambda s: s.startswith("Stopped"))
        response, _ = self.fetch(url)
        self.assertEqual(response.status, 404)

    def test_a_track_that_is_not_there_says_so(self):
        self.driver.send(f"select {self.index}")
        self.driver.expect("SELECT")
        self.driver.send(f"cast {self.root / 'missing.mp3'}")
        self.assertEqual(self.driver.expect("CAST"), "1")
        self.driver.wait_for_status(lambda s: s.startswith("This track cannot"))


if __name__ == "__main__":
    unittest.main(argv=sys.argv[:1], verbosity=2)
