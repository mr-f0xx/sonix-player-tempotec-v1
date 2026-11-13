#!/usr/bin/env python3
"""A Sonix relay for YouTube Music, small enough to run on any machine.

This is the third way for the player to get a track, after the device itself
and a Piped or Invidious instance, and it is only needed when both of those are
refused. Google demands a browser-grade attestation (a PO token its JavaScript
computes) before it hands out some media, and the player has no JavaScript
engine to run it; yt-dlp, here, has everything it needs. It answers two
endpoints:

    GET /streams/<video-id>   the Piped shape the player already speaks: the
                              title, the artist, the duration, and the one
                              audio stream it should fetch.
    GET /audio/<video-id>?f=<format-id>
                              those audio bytes, fetched here and passed on.

The resolve is done with yt-dlp, which handles the attestation. The audio is
served *through this machine* rather than by handing the player the
googlevideo URL yt-dlp resolved, for two reasons:

  - YouTube's media URLs are bound to the address that resolved them. Fetched
    from the player they are a different client on a different address, and
    Google answers 403 -- which on the player reads "Could not reach the
    stream", with nothing to say what went wrong.
  - They are also very long (a signed query of well over two thousand
    characters), past what the player's own URL buffer will hold.

A short URL on the LAN has neither problem, and the audio-only filter stays
where it was: this script offers m4a/AAC streams only, and the video is never
fetched.

Run it on any machine the player can reach -- the PC on the same network is
the natural place:

    pip install yt-dlp
    python ytmusic-relay.py            # listens on 0.0.0.0:8010

Then point the player at it. On the device, /usr/data/device_config.ini
gains:

    [ytmusic]
    relay_base = http://<this-machine's-ip>:8010

(The player takes the setting without a restart of the machine, but a
track started while the relay was down must be started again.)
"""

import json
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = 8010
YTDLP = "yt-dlp"

# One yt-dlp run per track, not two: the player asks /streams/<id> and then
# fetches /audio/<id> a moment later, and resolving twice would double the
# wait before the music starts. The URLs inside are what is cached, and
# YouTube's own expire after hours, so a few minutes is far inside their life.
CACHE_TTL = 300

_cache = {}
_cache_lock = threading.Lock()


def ytdlp_info(video_id):
    """The full yt-dlp record for one video, resolved once and kept briefly."""
    now = time.time()
    with _cache_lock:
        hit = _cache.get(video_id)
        if hit and hit[0] > now:
            return hit[1]

    url = f"https://www.youtube.com/watch?v={video_id}"
    raw = subprocess.run(
        [YTDLP, "-J", "--no-playlist", "--no-warnings", url],
        capture_output=True, text=True, timeout=120,
    )
    if raw.returncode != 0:
        raise RuntimeError((raw.stderr or "yt-dlp failed").strip()[:300])
    info = json.loads(raw.stdout)

    with _cache_lock:
        _cache[video_id] = (now + CACHE_TTL, info)
    return info


def audio_formats(info):
    """The audio-only streams this player can decode, best first.

    m4a/AAC only: the player reads AAC in an MP4 container, and its Opus
    decoder takes Ogg rather than the WebM YouTube wraps Opus in. Nothing
    carrying a video track is ever offered.
    """
    out = []
    for f in info.get("formats") or []:
        if f.get("vcodec") not in (None, "none"):
            continue
        if f.get("acodec") in (None, "none"):
            continue
        if not f.get("url"):
            continue
        ext = (f.get("ext") or "").lower()
        if ext != "m4a" and "mp4a" not in (f.get("acodec") or ""):
            continue
        bitrate = f.get("abr") or f.get("tbr") or 0
        out.append({
            "id": str(f.get("format_id") or ""),
            "url": f["url"],
            "headers": dict(f.get("http_headers") or {}),
            "bitrate": int(bitrate * 1000),
        })
    out.sort(key=lambda s: s["bitrate"], reverse=True)
    return out


def pick_format(info, wanted):
    """The named format, or the best one when it is gone."""
    streams = audio_formats(info)
    if not streams:
        return None
    if wanted:
        for stream in streams:
            if stream["id"] == wanted:
                return stream
    return streams[0]


def check_video_id(video_id):
    return bool(video_id) and video_id.isalnum() and len(video_id) == 11


class Relay(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):  # quieter default logging
        sys.stderr.write("relay: %s\n" % (fmt % args))

    def _send(self, code, body, ctype):
        data = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(data)

    def _base_url(self):
        # Where this machine is, as the player addressed it: the relay hands
        # out its own address rather than one it would have to be told.
        host = self.headers.get("Host")
        if not host:
            address = self.server.server_address
            host = "%s:%d" % (address[0], address[1])
        return "http://%s" % host

    def do_HEAD(self):
        if self.path.startswith("/streams/"):
            self._send(200, "{}", "application/json")
            return
        self._send(404, '{"error": "only /streams/<video-id> and /audio/<video-id> are served"}',
                   "application/json")

    def do_GET(self):
        if self.path.startswith("/streams/"):
            self.streams()
            return
        if self.path.startswith("/audio/"):
            self.audio()
            return
        self._send(404, '{"error": "only /streams/<video-id> and /audio/<video-id> are served"}',
                   "application/json")

    def streams(self):
        target = self.path[len("/streams/"):].split("?")[0].strip("/")
        video_id = target.split("/")[0]
        if not check_video_id(video_id):
            self._send(400, '{"error": "bad id"}', "application/json")
            return
        try:
            info = ytdlp_info(video_id)
        except Exception as e:  # yt-dlp missing, timed out, died oddly
            self._send(502, json.dumps({"error": f"relay: {e}"}), "application/json")
            return

        streams = audio_formats(info)
        if not streams:
            self._send(502, json.dumps({"error": "no audio-only AAC stream for this video"}),
                       "application/json")
            return

        best = streams[0]
        base = self._base_url()
        answer = {
            "title": info.get("title") or "",
            "uploader": info.get("uploader") or info.get("channel") or "",
            "duration": int(info.get("duration") or 0),
            "audioStreams": [{
                "url": f"{base}/audio/{video_id}?f={best['id']}",
                "bitrate": best["bitrate"],
                "mimeType": "audio/mp4; codecs=mp4a.40.2",
                "codec": "mp4a",
                "itag": best["id"],
            }],
            "videoStreams": [],
        }
        self._send(200, json.dumps(answer), "application/json")

    def audio(self):
        query = self.path.split("?", 1)
        video_id = query[0][len("/audio/"):].strip("/").split("/")[0]
        wanted = ""
        if len(query) > 1:
            for part in query[1].split("&"):
                if part.startswith("f="):
                    wanted = part[2:]
        if not check_video_id(video_id):
            self._send(400, '{"error": "bad id"}', "application/json")
            return
        try:
            info = ytdlp_info(video_id)
        except Exception as e:
            self._send(502, json.dumps({"error": f"relay: {e}"}), "application/json")
            return
        stream = pick_format(info, wanted)
        if not stream:
            self._send(502, '{"error": "no audio stream for this video"}', "application/json")
            return

        headers = dict(stream["headers"])
        headers.setdefault("User-Agent", "Mozilla/5.0")
        range_header = self.headers.get("Range")
        if range_header:
            headers["Range"] = range_header
        request = urllib.request.Request(stream["url"], headers=headers)
        try:
            upstream = urllib.request.urlopen(request, timeout=30)
        except urllib.error.HTTPError as e:
            # Google's refusal, passed on as it is: without the address that
            # resolved the URL this is the 403 the player used to read as a
            # broken stream.
            self._send(e.code, json.dumps({"error": f"youtube: {e.code}"}), "application/json")
            return
        except Exception as e:
            self._send(502, json.dumps({"error": f"relay: {e}"}), "application/json")
            return

        with upstream:
            length = upstream.headers.get("Content-Length")
            self.send_response(upstream.status)
            self.send_header("Content-Type", upstream.headers.get("Content-Type", "audio/mp4"))
            if length:
                self.send_header("Content-Length", length)
            else:
                # No length to forward: the body ends with the connection.
                self.close_connection = True
                self.send_header("Connection", "close")
            content_range = upstream.headers.get("Content-Range")
            if content_range:
                self.send_header("Content-Range", content_range)
            accept_ranges = upstream.headers.get("Accept-Ranges")
            if accept_ranges:
                self.send_header("Accept-Ranges", accept_ranges)
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            while True:
                chunk = upstream.read(65536)
                if not chunk:
                    break
                self.wfile.write(chunk)


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else PORT
    server = ThreadingHTTPServer(("0.0.0.0", port), Relay)
    print(f"ytmusic-relay: listening on 0.0.0.0:{port}, resolving and serving audio with {YTDLP}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
