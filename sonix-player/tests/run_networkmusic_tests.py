#!/usr/bin/env python3
"""Real loopback DAV/FTP servers and libcurl; no external accounts or servers."""
import base64
import contextlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import os
from pathlib import Path
import shutil
import socket
import socketserver
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from urllib.parse import quote, unquote
from xml.sax.saxutils import escape


DRIVER = str(Path(sys.argv.pop(1)).resolve())
AUDIO = b"fLaC" + bytes(range(256)) * 1536
TRACK = "Café #1 + 100%.flac"
SECOND = "02 - encore.mp3"
DAV_FOLDER = "Album & Friends"
FTP_FOLDER = "Album #1"
USER, PASSWORD = "demo", "pass @ :%"
AUTH = "Basic " + base64.b64encode(f"{USER}:{PASSWORD}".encode()).decode()


class Dav(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_):
        pass

    def reply(self, status, body=b"", content_type="application/octet-stream", **headers):
        self.send_response(status)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Content-Type", content_type)
        for key, value in headers.items():
            self.send_header(key, value)
        self.end_headers()
        self.wfile.write(body)

    def authenticated(self):
        if self.headers.get("Authorization") == AUTH:
            return True
        self.reply(401, **{"WWW-Authenticate": 'Basic realm="Music"'})
        return False

    def do_PROPFIND(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        if not self.authenticated():
            return
        if self.headers.get("Depth") != "1" or b"resourcetype" not in body:
            self.reply(400)
            return
        folder = unquote(self.path)
        origin = f"{self.server.scheme}://127.0.0.1:{self.server.server_port}"

        def entry(href, directory=False, status=200):
            prop = "<d:collection/>" if directory else ""
            return (f"<d:response><d:href>{escape(href)}</d:href><d:propstat>"
                    f"<d:prop><d:resourcetype>{prop}</d:resourcetype></d:prop>"
                    f"<d:status>HTTP/1.1 {status} OK</d:status></d:propstat></d:response>")

        if folder == "/dav/":
            entries = [entry("/dav/", True), entry(quote(f"/dav/{DAV_FOLDER}/"), True),
                       entry("/dav/Album%2FExternal/", True), entry("/dav/%2e%2e/Out/", True),
                       entry("/dav/deeper/sub.flac"), entry("/dav/unavailable.flac", status=404),
                       entry("http://outside.invalid/dav/secret.flac"), entry("/outside/song.flac"),
                       entry("/dav/cover.jpg")]
        elif folder == f"/dav/{DAV_FOLDER}/":
            entries = [entry(quote(folder), True), entry(origin + quote(folder + TRACK)),
                       entry(quote(folder + SECOND)), entry(quote(folder + TRACK))]
        elif folder == "/many/":
            entries = [entry(f"/many/{i:04}.flac") for i in range(401)]
        elif folder == "/huge/":
            self.reply(207, b" " * (1024 * 1024 + 1), "text/xml")
            return
        elif folder == "/invalid/":
            self.reply(207, b"<html>not a multistatus</html>", "text/html")
            return
        elif folder == "/truncated/":
            self.reply(207, b'<d:multistatus xmlns:d="DAV:"><d:response>', "text/xml")
            return
        else:
            self.reply(404)
            return
        xml = '<?xml version="1.0" encoding="utf-8"?><d:multistatus xmlns:d="DAV:">'
        self.reply(207, (xml + "".join(entries) + "</d:multistatus>").encode(), "text/xml")

    def do_GET(self):
        if not self.authenticated():
            return
        path = unquote(self.path)
        if path in (f"/dav/{DAV_FOLDER}/{TRACK}", f"/dav/{DAV_FOLDER}/{SECOND}"):
            self.reply(200, AUDIO)
        elif path == "/dav/stall.flac":
            self.send_response(200)
            self.send_header("Content-Length", str(len(AUDIO)))
            self.end_headers()
            self.wfile.write(AUDIO[:4096])
            self.wfile.flush()
            time.sleep(3)
        elif path == "/dav/short.flac":
            self.send_response(200)
            self.send_header("Content-Length", str(len(AUDIO)))
            self.end_headers()
            self.wfile.write(AUDIO[:4096])
            self.close_connection = True
        elif path == "/dav/too-big.flac":
            self.send_response(200)
            self.send_header("Content-Length", str(2**32))
            self.end_headers()
            self.close_connection = True
        elif path == "/dav/redirect.flac":
            self.reply(302, Location=quote(f"/dav/{DAV_FOLDER}/{TRACK}"))
        elif path == "/dav/login.flac":
            self.reply(200, b"<html>Sign in</html>", "text/html")
        else:
            self.reply(404)


class Ftp(socketserver.StreamRequestHandler):
    def handle(self):
        passive = None
        folder = "/"
        authed = False
        self.request.settimeout(4)

        def send(text):
            self.wfile.write((text + "\r\n").encode())

        send("220 Test music server")
        try:
            while line := self.rfile.readline(4096):
                cmd, _, arg = line.decode().rstrip("\r\n").partition(" ")
                if cmd == "USER":
                    user = arg
                    send("331 Password required")
                elif cmd == "PASS":
                    authed = user == USER and arg == PASSWORD
                    send("230 Logged in" if authed else "530 Login incorrect")
                elif cmd == "QUIT":
                    send("221 Bye")
                    return
                elif not authed:
                    send("530 Authenticate first")
                elif cmd == "PWD":
                    send(f'257 "{folder}" is the current directory')
                elif cmd == "CWD":
                    new = arg if arg.startswith("/") else folder.rstrip("/") + "/" + arg
                    if new in ("/", "/pub", f"/pub/{FTP_FOLDER}"):
                        folder = new
                        send("250 Directory changed")
                    else:
                        send("550 File not found")
                elif cmd in ("TYPE", "PBSZ", "PROT"):
                    send("200 OK")
                elif cmd == "EPSV":
                    send("502 EPSV not implemented")
                elif cmd == "PASV":
                    if passive:
                        passive.close()
                    passive = socket.create_server(("127.0.0.1", 0))
                    passive.settimeout(4)
                    hi, lo = divmod(passive.getsockname()[1], 256)
                    # This address must be ignored, even on old stock curl.
                    send(f"227 Entering Passive Mode (203,0,113,10,{hi},{lo})")
                elif cmd in ("SIZE", "RETR") and (folder, arg) != (f"/pub/{FTP_FOLDER}", TRACK):
                    send("550 File not found")
                elif cmd == "SIZE":
                    send(f"213 {len(AUDIO)}")
                elif cmd == "MLSD" and self.server.legacy:
                    send("502 MLSD not implemented")
                elif cmd in ("MLSD", "LIST", "RETR"):
                    names = ""
                    if cmd == "MLSD":
                        names = (f"type=dir; {FTP_FOLDER}\r\ntype=cdir; .\r\ntype=pdir; ..\r\n" if folder == "/pub"
                                 else f"type=file;size={len(AUDIO)}; {TRACK}\r\ntype=file; cover.png\r\n")
                    elif cmd == "LIST":
                        names = (f"04-21-24  02:12PM       <DIR> {FTP_FOLDER}\r\n" if folder == "/pub"
                                 else f"-rw-r--r-- 1 user group {len(AUDIO)} Sep 12 12:00 {TRACK}\r\n"
                                      "lrwxrwxrwx 1 user group 9 Sep 12 12:00 ignore -> directory\r\n")
                    send("150 Opening data connection")
                    data, _ = passive.accept()
                    passive.close()
                    passive = None
                    if self.server.context:
                        data = self.server.context.wrap_socket(data, server_side=True)
                    with data:
                        data.sendall(AUDIO if cmd == "RETR" else names.encode())
                        if self.server.context:
                            data.unwrap().close()
                    send("226 Transfer complete")
                else:
                    send("502 Not implemented")
        except (BrokenPipeError, ConnectionResetError, TimeoutError):
            pass
        finally:
            if passive:
                passive.close()


class FTPServer(socketserver.ThreadingTCPServer):
    daemon_threads = True


@contextlib.contextmanager
def serve(kind, context=None, legacy=False):
    if kind == "dav":
        server = ThreadingHTTPServer(("127.0.0.1", 0), Dav)
        server.scheme = "https" if context else "http"
    else:
        server = FTPServer(("127.0.0.1", 0), Ftp)
        server.context = context
        server.legacy = legacy
        server.scheme = "ftps" if context else "ftp"
    if context:
        server.socket = context.wrap_socket(server.socket, server_side=True)
    with server:
        thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.05}, daemon=True)
        thread.start()
        try:
            host, port = server.server_address
            yield f"{server.scheme}://{host}:{port}"
        finally:
            server.shutdown()
            thread.join(2)


class NetworkMusicTests(unittest.TestCase):
    def setUp(self):
        self.card = Path(self.enterContext(tempfile.TemporaryDirectory()))

    def driver(self, mode, kind, url, name="", password=PASSWORD, ca=None, card=None):
        env = os.environ.copy()
        if ca:
            env["SONIX_TEST_SHARE_CA"] = str(ca)
        p = subprocess.run([DRIVER, mode, kind, url, str(card or self.card), USER, password, name],
                           check=True, capture_output=True, text=True, env=env, timeout=8)
        return p.stdout.splitlines()

    def no_parts(self):
        self.assertEqual(list(self.card.rglob("*.part")), [])

    def test_address_validation(self):
        subprocess.run([DRIVER], check=True, timeout=5)

    def test_webdav_browse_and_album(self):
        with serve("dav") as url:
            self.assertEqual(self.driver("list", "dav", url + "/dav"), ["OK", "complete", f"d\t{DAV_FOLDER}"])
            folder = url + quote(f"/dav/{DAV_FOLDER}/")
            self.assertEqual(self.driver("list", "dav", folder), ["OK", "complete", f"f\t{SECOND}", f"f\t{TRACK}"])
            self.assertEqual(self.driver("album", "dav", folder)[:2], ["OK", "2 0"])
            saved = sorted(self.card.rglob("*.flac"))
            self.assertEqual(len(saved), 1)
            self.assertEqual(saved[0].parent.name, DAV_FOLDER)
            self.assertEqual(saved[0].read_bytes(), AUDIO)
            saved[0].write_bytes(b"Keep my original")
            self.assertEqual(self.driver("album", "dav", folder)[:2], ["OK", "0 2"])
            self.assertEqual(saved[0].read_bytes(), b"Keep my original")
            self.no_parts()

    def test_ftp_mlsd_and_legacy_list(self):
        for legacy in (False, True):
            with self.subTest(legacy=legacy), serve("ftp", legacy=legacy) as url:
                self.assertEqual(self.driver("list", "ftp", url + "/pub/"), ["OK", "complete", f"d\t{FTP_FOLDER}"])
                folder = url + quote(f"/pub/{FTP_FOLDER}/")
                self.assertEqual(self.driver("list", "ftp", folder), ["OK", "complete", f"f\t{TRACK}"])
                status, count, saved = self.driver("save", "ftp", folder, TRACK)
                self.assertEqual((status, count), ("OK", "1 0"))
                self.assertEqual(Path(saved).read_bytes(), AUDIO)
                self.no_parts()

    def test_authentication_and_missing_card(self):
        for kind in ("dav", "ftp"):
            with self.subTest(kind=kind), serve(kind) as url:
                folder = "/dav/" if kind == "dav" else "/pub/"
                self.assertEqual(self.driver("list", kind, url + folder, password="wrong")[0], "AUTH_ERROR")
                self.assertEqual(self.driver("list", kind, url + folder, card=self.card / "no-card")[0], "OK")
                self.assertEqual(self.driver("save", kind, url + folder, TRACK, card=self.card / "no-card")[0], "NO_CARD")

    def test_limits_and_invalid_responses(self):
        with serve("dav") as url:
            many = self.driver("list", "dav", url + "/many/")
            self.assertEqual(many[:2], ["OK", "truncated"])
            self.assertEqual(len(many[2:]), 384)
            for path, status in (("/huge/", "LIST_TOO_LARGE"), ("/invalid/", "BAD_LIST"), ("/truncated/", "BAD_LIST")):
                self.assertEqual(self.driver("list", "dav", url + path)[0], status)
            for name, status in (("redirect.flac", "SERVER_ERROR"), ("login.flac", "SERVER_ERROR"),
                                 ("too-big.flac", "FILE_TOO_LARGE"), ("short.flac", "NETWORK_ERROR")):
                self.assertEqual(self.driver("save", "dav", url + "/dav/", name)[0], status)
            self.assertEqual(list(self.card.rglob("*.flac")), [])
            self.no_parts()

    def test_cancel_and_release_before_handoff(self):
        with serve("dav") as url:
            for mode in ("cancel", "release"):
                start = time.monotonic()
                self.assertEqual(self.driver(mode, "dav", url + "/dav/", "stall.flac")[0], "CANCELLED")
                self.assertLess(time.monotonic() - start, 2)
                self.assertEqual(list(self.card.rglob("*.flac")), [])
                self.no_parts()

    def test_never_follow_local_symlinks(self):
        outside = Path(self.enterContext(tempfile.TemporaryDirectory()))
        (self.card / "Music").symlink_to(outside, target_is_directory=True)
        with serve("dav") as url:
            self.assertEqual(self.driver("save", "dav", url + "/dav/", TRACK)[0], "DISK_ERROR")
        self.assertEqual(list(outside.iterdir()), [])

    @unittest.skipUnless(shutil.which("openssl"), "openssl needed to make a temporary CA")
    def test_tls_certificate_verification(self):
        temp = Path(self.enterContext(tempfile.TemporaryDirectory()))
        cert, key = temp / "server.pem", temp / "server.key"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                        "-subj", "/CN=127.0.0.1", "-addext", "subjectAltName=IP:127.0.0.1",
                        "-keyout", str(key), "-out", str(cert)], check=True, capture_output=True, timeout=10)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        for kind in ("dav", "ftp"):
            with self.subTest(kind=kind), serve(kind, context) as url:
                path = "/dav/" if kind == "dav" else "/pub/"
                self.assertEqual(self.driver("list", kind, url + path)[0], "CERT_ERROR")
                self.assertEqual(self.driver("list", kind, url + path, ca=cert)[0], "OK")
                directory = DAV_FOLDER if kind == "dav" else FTP_FOLDER
                self.assertEqual(self.driver("save", kind, url + path + quote(directory) + "/", TRACK, ca=cert)[:2], ["OK", "1 0"])


if __name__ == "__main__":
    unittest.main()
