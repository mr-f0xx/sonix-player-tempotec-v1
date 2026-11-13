"""The YouTube Music relay, with the network taken out.

Only the relay's own logic is under test: the yt-dlp call is replaced by a
canned record, so the suite pins what the script does with one -- which stream
it offers, the URL it hands the player, and that the audio endpoint serves those
bytes with the length the player checks its download against.

That last one is the fix this suite exists for. The relay used to hand the
player the googlevideo URL yt-dlp had resolved; the player then fetched it from
another address, which Google answers with 403, and through a URL longer than
its own buffer. Both showed up as "could not reach the stream".

Run with: python3 tests/test_ytmusic_relay.py (from sonix-player/)
"""

import importlib.util
import json
import os
import sys
import threading
import urllib.error
import urllib.request
from http.server import ThreadingHTTPServer

RELAY = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools", "ytmusic-relay.py")

spec = importlib.util.spec_from_file_location("ytmusic_relay", RELAY)
relay = importlib.util.module_from_spec(spec)
spec.loader.exec_module(relay)

VID = "dQw4w9WgXcQ"

INFO = {
    "title": "Never Gonna Give You Up",
    "uploader": "Rick Astley",
    "duration": 213,
    "formats": [
        # No URL at all: nothing to serve.
        {"format_id": "251", "ext": "webm", "acodec": "opus", "vcodec": "none", "abr": 160},
        # Opus in WebM: the player's decoder reads Ogg, not this.
        {"format_id": "250", "ext": "webm", "acodec": "opus", "vcodec": "none", "abr": 70,
         "url": "https://media.example/opus"},
        # A progressive file: it carries a video track and must never be offered.
        {"format_id": "18", "ext": "mp4", "acodec": "mp4a.40.2", "vcodec": "avc1", "abr": 96,
         "url": "https://media.example/muxed"},
        # The two the relay may offer, best first.
        {"format_id": "140", "ext": "m4a", "acodec": "mp4a.40.2", "vcodec": "none", "abr": 128,
         "url": "https://media.example/140", "http_headers": {"User-Agent": "youtube-dl/1"}},
        {"format_id": "141", "ext": "m4a", "acodec": "mp4a.40.2", "vcodec": "none", "abr": 256,
         "url": "https://media.example/141", "http_headers": {"User-Agent": "youtube-dl/1"}},
    ],
}

failures = []


def check(cond, what):
    if cond:
        print("ok   %s" % what)
    else:
        failures.append(what)
        print("FAIL %s" % what)


# --- the picks -------------------------------------------------------------

relay.ytdlp_info = lambda video_id: INFO

streams = relay.audio_formats(INFO)
check([s["id"] for s in streams] == ["141", "140"],
      "only m4a/AAC is offered, best bitrate first")
check(all("video" not in s["url"] for s in streams), "no muxed media is offered")
check(relay.pick_format(INFO, "140")["id"] == "140", "the player's format is honoured")
check(relay.pick_format(INFO, "999")["id"] == "141", "a format that is gone falls back to the best")
check(relay.audio_formats({"formats": []}) == [], "a video with no audio offers nothing")

# --- the endpoints --------------------------------------------------------

server = ThreadingHTTPServer(("127.0.0.1", 0), relay.Relay)
port = server.server_address[1]
threading.Thread(target=server.serve_forever, daemon=True).start()


def get(path, host=None):
    request = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path))
    if host:
        request.add_header("Host", host)
    try:
        with urllib.request.urlopen(request, timeout=10) as answer:
            return answer.status, answer.headers, answer.read()
    except urllib.error.HTTPError as refusal:  # a refusal is an answer here
        return refusal.code, refusal.headers, refusal.read()


status, _, body = get("/streams/%s" % VID, host="relay.test:8010")
answer = json.loads(body)
check(status == 200, "the resolve answers 200")
check(answer["duration"] == 213 and answer["uploader"] == "Rick Astley",
      "the reply carries what the now-playing screen shows")
check(len(answer["audioStreams"]) == 1, "one stream is offered")
stream = answer["audioStreams"][0]
check(stream["itag"] == "141" and stream["bitrate"] == 256000, "the best AAC stream is offered")
check(stream["mimeType"].startswith("audio/mp4"), "and it says it is AAC in MP4")
# This is the fix: the player is pointed at the relay, never at googlevideo.
check(stream["url"] == "http://relay.test:8010/audio/%s?f=141" % VID,
      "the stream URL points at the relay that answered")
check(len(stream["url"]) < 120, "and it is short enough for the player's own URL buffer")

status, _, body = get("/streams/not-an-id")
check(status == 400, "a bad id is refused")

# The audio endpoint: the bytes are proxied, and the length the player checks
# its download against is the one the upstream gave.
#
# Only the relay's own fetch is replaced, and by URL: the suite's client calls
# go through the same urllib module, so patching urlopen outright would answer
# the check's own request as well as the relay's.
served = {}
real_urlopen = relay.urllib.request.urlopen


class FakeUpstream:
    status = 200
    headers = {"Content-Length": "11", "Content-Type": "audio/mp4"}

    def __init__(self):
        self.sent = False

    def read(self, size=-1):
        if self.sent:
            return b""
        self.sent = True
        return b"hello world"

    def __enter__(self):
        return self

    def __exit__(self, *args):
        return False


def fake_urlopen(request, timeout=None):
    if not request.full_url.startswith("https://media.example/"):
        return real_urlopen(request, timeout=timeout)  # the suite's own call
    served["url"] = request.full_url
    served["headers"] = {k.lower(): v for k, v in request.header_items()}
    return FakeUpstream()


relay.urllib.request.urlopen = fake_urlopen
try:
    status, headers, body = get("/audio/%s?f=140" % VID)
finally:
    relay.urllib.request.urlopen = real_urlopen

check(status == 200 and body == b"hello world", "the audio endpoint passes the bytes on")
check(headers.get("Content-Length") == "11",
      "with the upstream length, which the player's download check needs")
check(served.get("url") == "https://media.example/140", "the named format is the one fetched")
check(served.get("headers", {}).get("user-agent") == "youtube-dl/1",
      "and it is fetched with the headers yt-dlp resolved it with")

server.shutdown()

if failures:
    print("\n%d failure%s" % (len(failures), "" if len(failures) == 1 else "s"))
    sys.exit(1)
print("\nrelay check: ok")
