#!/usr/bin/env python3
"""
Extracts the Qobuz (app_id, app_secret) and Tidal (client_id, client_secret)
application credentials from a stock HiByOS player binary and writes them into
a streaming-keys.ini the player reads.

Why this exists
---------------
Qobuz does not issue application credentials for personal or open-source
integrations, and Tidal's self-serve developer.tidal.com portal only issues
OpenAPI v2 keys that lack the v1 Limited Input Device (device_code) grant and
unencrypted v1 stream entitlements ("Client is not a Limited Input Device
client.").

The stock TempoTec V1 firmware talks to both services anyway: its player,
usr/bin/hiby_player, carries HiBy's Qobuz app_id/app_secret and Tidal 44-byte
base64 client_secret in .rodata, while assembling Tidal's 16-character OAuth2
client_id byte by byte via MIPS32 immediate stores right before referencing the
"client_id" form parameter.

This tool pulls those credentials out of a binary the user already possesses --
their own device's stock firmware -- and turns them into the same
streaming-keys.ini the rest of the workflow uses. Nothing here is downloaded,
and nothing the tool writes belongs in git: streaming-keys.ini and
streaming-keys.bin are both ignored. Do not publish an image that carries the
result; the credentials belong to HiBy, not this project.

    python3 tools/extract_streamkeys.py                         # uses bundled stock 1.3 archive
    python3 tools/extract_streamkeys.py path/to/v1.upt          # unpacks ISO + SquashFS + LZO in pure Python
    python3 tools/extract_streamkeys.py path/to/v1.zip          # or v1.zip.part1
    python3 tools/extract_streamkeys.py path/to/hiby_player
    python3 tools/extract_streamkeys.py unpacked/rootfs         # finds usr/bin/hiby_player
    python3 tools/extract_streamkeys.py --check unpacked/rootfs
    python3 tools/extract_streamkeys.py --list-candidates hiby_player

With no --out the ini is written next to the sealing tool's default, at
sonix-player/streaming-keys.ini. An existing ini is merged: its other sections
(podcasts, Last.fm, etc.) are kept and only extracted sections are replaced.

The heuristic never guesses silently. When more than one string could be an id
or a secret, it stops and --list-candidates shows what was found so values can
be forced explicitly.
"""

import argparse
import hashlib
import io
import lzma
import os
import re
import struct
import sys
import zipfile
import zlib

PLAYER_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO_ROOT = os.path.dirname(PLAYER_ROOT)
DEFAULT_INI = os.path.join(PLAYER_ROOT, "streaming-keys.ini")
DEFAULT_STOCK_INPUT = os.path.join(
    REPO_ROOT, "sonix-packer", "firmware-input", "v1.zip.part1"
)
STOCK_PLAYER_REL = os.path.join("usr", "bin", "hiby_player")

# A string this short is noise in a five-megabyte binary; every value we look
# for is longer.
MIN_STRING_LEN = 4

# Qobuz application ids are numeric: five digits in the oldest partner apps,
# nine in current ones. Standalone NUL-terminated strings, not substrings.
APP_ID_RE = re.compile(rb"^\d{5,12}$")
# Every Qobuz app secret seen in the wild is 32 lowercase hex characters.
SECRET_HEX_RE = re.compile(rb"^[0-9a-f]{32}$")
# Fallback for a partner secret that is alphanumeric rather than hex: the same
# length, a weaker score so a hex string always wins.
SECRET_WORD_RE = re.compile(rb"^[0-9A-Za-z]{32}$")

# Strings that prove the binary is the right one and locate its Qobuz code.
# The stock V1 binary speaks cleartext http://www.qobuz.com/api.json/0.2; the
# match is on the host part so either scheme works.
ANCHOR_RE = re.compile(rb"qobuz\.com/api\.json|device_manufacturer_id|"
                       rb"trackgetfileurl|request_sig|x-app-id|qobuz", re.IGNORECASE)
# A match on the bare word could occur in an unrelated binary; one of these
# has to be present for the file to count as a Qobuz client at all.
STRONG_ANCHOR_RE = re.compile(rb"qobuz\.com/api\.json|device_manufacturer_id|"
                              rb"trackgetfileurl|request_sig|x-app-id", re.IGNORECASE)

# Tidal v1 device-authorization strings present in stock HiByOS binaries.
TIDAL_STRONG_ANCHOR_RE = re.compile(
    rb"auth\.tidal\.com/v1/oauth2/device_authorization|"
    rb"auth\.tidal\.com/v1/oauth2/token|"
    rb"urn:ietf:params:oauth:grant-type:device_code|"
    rb"playbackinfopostpaywall",
    re.IGNORECASE,
)
TIDAL_ANCHOR_RE = re.compile(
    rb"auth\.tidal\.com/v1/oauth2|urn:ietf:params:oauth:grant-type:device_code|"
    rb"playbackinfopostpaywall|x-tidal-token",
    re.IGNORECASE,
)
# Partner Tidal v1 client_secrets are 44-char base64 strings ending in '='.
TIDAL_SECRET_B64_RE = re.compile(rb"^[A-Za-z0-9+/]{43}=$")
# Fallback if a build ever stores Tidal's 16-char client_id in .rodata.
TIDAL_RODATA_ID_RE = re.compile(rb"^[A-Za-z0-9_-]{16}$")
TIDAL_ID_NOISE = {
    b"device_authorize",
    b"application/json",
    b"www-authenticate",
    b"content-encoding",
    b"transfer-encodin",
}
TIDAL_MAX_SECRET_DISTANCE = 16384


class ExtractError(Exception):
    """The credentials could not be identified with enough certainty."""


def _is_printable(byte):
    return 0x20 <= byte <= 0x7E


def iter_strings(data):
    """Yields (offset, text) for every run of printable ASCII bytes.

    The split character is anything non-printable, which covers the NULs and
    alignment padding between C strings in .rodata as well as MIPS code bytes
    around them. The scan is deliberately format-agnostic: it works on an ELF,
    a squashfs member pulled out on its own, or a raw blob.
    """
    start = None
    for i, byte in enumerate(data):
        if _is_printable(byte):
            if start is None:
                start = i
        elif start is not None:
            if i - start >= MIN_STRING_LEN:
                yield start, data[start:i]
            start = None
    if start is not None and len(data) - start >= MIN_STRING_LEN:
        yield start, data[start:]


def _nearest(offset, anchors):
    return min(abs(offset - anchor) for anchor in anchors)


def _scan(data):
    """All strings first, so every distance is measured against all anchors."""
    strings = list(iter_strings(data))
    anchors = [offset for offset, text in strings if ANCHOR_RE.search(text)]
    strong = [offset for offset, text in strings if STRONG_ANCHOR_RE.search(text)]
    return strings, anchors, strong


def _scan_tidal(data, strings=None):
    if strings is None:
        strings = list(iter_strings(data))
    anchors = [offset for offset, text in strings if TIDAL_ANCHOR_RE.search(text)]
    strong = [offset for offset, text in strings if TIDAL_STRONG_ANCHOR_RE.search(text)]
    return strings, anchors, strong


def _record(hits, text, score):
    # The same literal can appear more than once; its closest occurrence wins.
    if text not in hits or score < hits[text]:
        hits[text] = score


def _choose(hits, label, hint_flags):
    best_score = min(hits.values())
    winners = sorted(value for value, score in hits.items() if score == best_score)
    if len(winners) > 1:
        shown = ", ".join(mask(w.decode("ascii")) for w in winners)
        raise ExtractError(
            f"more than one candidate for the {label} sits at the same "
            f"distance: {shown}. Re-run with --list-candidates and force "
            f"the right one with {hint_flags}.")
    return winners[0].decode("ascii")


def _looks_like_tidal_b64_secret(text):
    if not TIDAL_SECRET_B64_RE.match(text):
        return False
    # Reject degenerate all-one-character runs such as 'AAAA...='.
    has_upper = any(0x41 <= b <= 0x5A for b in text[:-1])
    has_lower = any(0x61 <= b <= 0x7A for b in text[:-1])
    has_digit_or_sym = any((0x30 <= b <= 0x39) or b in (0x2B, 0x2F) for b in text[:-1])
    return (has_upper and has_lower) or (has_lower and has_digit_or_sym) or (has_upper and has_digit_or_sym)


def _elf32_le_segments(buf):
    """Returns [(vaddr, file_off, filesz), ...] for PT_LOAD segments of a 32-bit LE ELF."""
    if len(buf) < 52 or buf[:4] != b"\x7fELF" or buf[4] != 1 or buf[5] != 1:
        return []
    e_phoff, = struct.unpack_from("<I", buf, 28)
    e_phentsize, e_phnum = struct.unpack_from("<HH", buf, 42)
    if e_phentsize < 32:
        return []
    segs = []
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        if off + 32 > len(buf):
            break
        p_type, p_off, p_vaddr, _, p_filesz, _, _, _ = struct.unpack_from("<8I", buf, off)
        if p_type == 1 and p_filesz > 0 and p_off + p_filesz <= len(buf):
            segs.append((p_vaddr, p_off, p_filesz))
    return segs


def _va_cstring(buf, segs, va, max_len=64):
    for vaddr, foff, filesz in segs:
        if vaddr <= va < vaddr + filesz:
            start = foff + (va - vaddr)
            end = buf.find(b"\x00", start, min(len(buf), start + max_len))
            if end > start and all(_is_printable(b) for b in buf[start:end]):
                return buf[start:end]
    return None


def scan_mips_assembled_strings(buf, min_len=12, max_len=24):
    """Finds NUL-terminated ASCII tokens built byte-by-byte via MIPS32 LE sb stores.

    Stock HiByOS builds Tidal's 16-character OAuth2 client_id at run time using
    immediate loads (addiu/ori $reg, $zero, ch) followed by contiguous byte
    stores (sb $reg, off($base)) and a terminating sb $zero, len($base), right
    before loading the "client_id" .rodata parameter string.

    Returns a list of (file_offset, token_bytes, nearby_rodata_labels).
    """
    segs = _elf32_le_segments(buf)
    limit = len(buf) & ~3
    regs = {0: ("val", 0)}
    spaces = {}
    results = []

    for pos in range(0, limit, 4):
        w, = struct.unpack_from("<I", buf, pos)
        op = (w >> 26) & 0x3F
        rs = (w >> 21) & 0x1F
        rt = (w >> 16) & 0x1F
        imm = w & 0xFFFF
        simm = imm - 0x10000 if (imm & 0x8000) else imm

        if op == 0x0F:  # lui rt, imm
            if rt != 0:
                regs[rt] = ("val", (imm << 16) & 0xFFFFFFFF)
        elif op == 0x09:  # addiu rt, rs, simm
            if rt != 0:
                src = regs.get(rs)
                if rs == 0:
                    regs[rt] = ("val", simm & 0xFFFFFFFF)
                elif src and src[0] == "val":
                    regs[rt] = ("val", (src[1] + simm) & 0xFFFFFFFF)
                elif src and src[0] == "sym":
                    regs[rt] = ("sym", src[1], src[2] + simm)
                else:
                    regs[rt] = ("sym", rs, simm)
        elif op == 0x0D:  # ori rt, rs, imm
            if rt != 0:
                src = regs.get(rs)
                if rs == 0:
                    regs[rt] = ("val", imm)
                elif src and src[0] == "val":
                    regs[rt] = ("val", src[1] | imm)
                else:
                    regs.pop(rt, None)
        elif op == 0x28:  # sb rt, simm(rs)
            val_entry = ("val", 0) if rt == 0 else regs.get(rt)
            if val_entry and val_entry[0] == "val" and 0 <= val_entry[1] <= 0x7F:
                bval = val_entry[1]
                base = regs.get(rs)
                if base and base[0] == "val":
                    space_id, addr = "abs", (base[1] + simm) & 0xFFFFFFFF
                elif base and base[0] == "sym":
                    space_id, addr = f"r{base[1]}", base[2] + simm
                else:
                    space_id, addr = f"r{rs}", simm
                space = spaces.setdefault(space_id, {})
                if space and any(pos - prev_pos > 256 for _, prev_pos in space.values()):
                    space.clear()
                space[addr] = (bval, pos)
                if bval == 0:
                    end_a = addr
                    start_a = end_a
                    while (start_a - 1) in space and 0x21 <= space[start_a - 1][0] <= 0x7E:
                        start_a -= 1
                    length = end_a - start_a
                    if min_len <= length <= max_len:
                        token = bytes(space[a][0] for a in range(start_a, end_a))
                        if re.fullmatch(rb"[A-Za-z0-9_-]+", token):
                            labels = set()
                            if segs:
                                tmp_vals = {k: v[1] for k, v in regs.items() if v[0] == "val"}
                                for la in range(pos + 4, min(limit, pos + 96), 4):
                                    lw, = struct.unpack_from("<I", buf, la)
                                    lop = (lw >> 26) & 0x3F
                                    lrs = (lw >> 21) & 0x1F
                                    lrt = (lw >> 16) & 0x1F
                                    limm = lw & 0xFFFF
                                    lsimm = limm - 0x10000 if (limm & 0x8000) else limm
                                    if lop == 0x0F and lrt != 0:
                                        tmp_vals[lrt] = (limm << 16) & 0xFFFFFFFF
                                    elif lop == 0x09 and lrs != 0 and lrs in tmp_vals:
                                        va = (tmp_vals[lrs] + lsimm) & 0xFFFFFFFF
                                        cstr = _va_cstring(buf, segs, va)
                                        if cstr:
                                            labels.add(cstr)
                                        if lrt != 0:
                                            tmp_vals[lrt] = va
                            first_pos = min(space[a][1] for a in range(start_a, end_a + 1))
                            results.append((first_pos, token, labels))
                    for a in range(start_a, end_a + 1):
                        space.pop(a, None)
        elif op == 0 and (w & 0x3F) in (0x21, 0x25) and rt == 0:
            rd = (w >> 11) & 0x1F
            if rd != 0:
                if rs in regs:
                    regs[rd] = regs[rs]
                else:
                    regs.pop(rd, None)
        else:
            if op in (0x08, 0x0A, 0x0B, 0x0C, 0x0E, 0x20, 0x21, 0x23, 0x24, 0x25):
                regs.pop(rt, None)
            elif op == 0 and w != 0:
                rd = (w >> 11) & 0x1F
                if rd != 0:
                    regs.pop(rd, None)

    return results


def has_tidal_anchors(data):
    """True when the binary carries Tidal v1 OAuth2 device-login strings."""
    _, anchors, strong = _scan_tidal(data)
    return bool(anchors and len(strong) >= 2)


def extract_tidal(data):
    """Returns (client_id, client_secret) as str, or raises ExtractError."""
    strings, anchors, strong_anchors = _scan_tidal(data)
    if not anchors or len(strong_anchors) < 2:
        raise ExtractError(
            "no Tidal device-login strings found (looked for "
            "auth.tidal.com/v1/oauth2/device_authorization, "
            "urn:ietf:params:oauth:grant-type:device_code). "
            "Is this the stock hiby_player binary?")

    secret_hits = {}
    for offset, text in strings:
        if _looks_like_tidal_b64_secret(text):
            dist = _nearest(offset, anchors)
            if dist <= TIDAL_MAX_SECRET_DISTANCE:
                _record(secret_hits, text, dist)

    if not secret_hits:
        raise ExtractError("no 44-character base64 Tidal client_secret found near Tidal strings")

    client_secret = _choose(
        secret_hits, "Tidal client_secret", "--tidal-client-id/--tidal-client-secret"
    )

    # First preference: MIPS32 byte-assembled client_id (the stock V1 layout).
    assembled = scan_mips_assembled_strings(data)
    tagged = {tok for _, tok, labels in assembled if b"client_id" in labels}
    if len(tagged) == 1:
        client_id = next(iter(tagged)).decode("ascii")
        return client_id, client_secret
    if len(tagged) > 1:
        shown = ", ".join(mask(t.decode("ascii")) for t in sorted(tagged))
        raise ExtractError(
            f"more than one byte-assembled Tidal client_id candidate: {shown}. "
            "Force the right one with --tidal-client-id/--tidal-client-secret."
        )

    untagged = {tok for _, tok, _ in assembled}
    if len(untagged) == 1:
        client_id = next(iter(untagged)).decode("ascii")
        return client_id, client_secret
    if len(untagged) > 1:
        shown = ", ".join(mask(t.decode("ascii")) for t in sorted(untagged))
        raise ExtractError(
            f"multiple byte-assembled tokens found ({shown}) without an ELF "
            "client_id label to disambiguate them. Force the right one with "
            "--tidal-client-id/--tidal-client-secret."
        )

    # Fallback if a HiByOS build keeps Tidal's 16-char client_id in .rodata.
    id_hits = {}
    for offset, text in strings:
        if TIDAL_RODATA_ID_RE.match(text) and text.lower() not in TIDAL_ID_NOISE:
            has_letter = any((0x41 <= b <= 0x5A) or (0x61 <= b <= 0x7A) for b in text)
            has_digit = any(0x30 <= b <= 0x39 for b in text)
            if has_letter and has_digit:
                dist = _nearest(offset, anchors)
                if dist <= TIDAL_MAX_SECRET_DISTANCE:
                    _record(id_hits, text, dist)

    if not id_hits:
        raise ExtractError("no Tidal client_id found (neither MIPS byte-assembled nor .rodata)")

    client_id = _choose(id_hits, "Tidal client_id", "--tidal-client-id/--tidal-client-secret")
    return client_id, client_secret


def extract(data):
    """Returns (app_id, app_secret) for Qobuz as str, or raises ExtractError."""
    strings, anchors, strong_anchors = _scan(data)
    if not anchors or not strong_anchors:
        raise ExtractError(
            "no Qobuz client strings found (looked for qobuz.com/api.json, "
            "device_manufacturer_id, trackgetFileUrl, request_sig, X-App-Id). "
            "Is this the stock hiby_player binary?")

    id_hits = {}      # value text -> nearest anchor distance
    secret_hits = {}
    for offset, text in strings:
        if APP_ID_RE.match(text):
            _record(id_hits, text, _nearest(offset, anchors))
        if SECRET_HEX_RE.match(text) or SECRET_WORD_RE.match(text):
            score = _nearest(offset, anchors)
            # A hex string is the known shape; an all-words 32-char string is
            # only a fallback, so push its score back.
            if not SECRET_HEX_RE.match(text):
                score += 1
            _record(secret_hits, text, score)

    if not id_hits:
        raise ExtractError("no numeric app_id string found")
    if not secret_hits:
        raise ExtractError("no 32-character app_secret string found")

    app_id = _choose(
        id_hits,
        "app_id",
        "--app-id/--app-secret",
    )
    app_secret = _choose(
        secret_hits,
        "app_secret",
        "--app-id/--app-secret",
    )
    if app_id == app_secret:
        raise ExtractError("the app_id and app_secret resolved to the same string")
    return app_id, app_secret


def list_candidates(data):
    """Human-readable diagnostic: every plausible string, masked, with offset."""
    strings, anchors, _ = _scan(data)
    _, tidal_anchors, _ = _scan_tidal(data, strings)
    rows = []
    for offset, text in strings:
        if APP_ID_RE.match(text) or SECRET_HEX_RE.match(text) or SECRET_WORD_RE.match(text):
            kind = "id?         " if APP_ID_RE.match(text) else "secret?     "
            distance = _nearest(offset, anchors) if anchors else -1
            rows.append((distance, offset, kind, mask(text.decode("ascii"))))
        if _looks_like_tidal_b64_secret(text):
            distance = _nearest(offset, tidal_anchors) if tidal_anchors else -1
            rows.append((distance, offset, "tidal-sec?  ", mask(text.decode("ascii"))))
    for offset, tok, labels in scan_mips_assembled_strings(data):
        tag = "tidal-id!   " if b"client_id" in labels else "mips-str?   "
        rows.append((0, offset, tag, mask(tok.decode("ascii"))))
    rows.sort()
    lines = ["distance  offset      kind          value"]
    lines += [f"{dist:>8}  0x{offset:08x}  {kind}  {value}" for dist, offset, kind, value in rows]
    if not anchors:
        lines.append("(no Qobuz anchor strings were found in this file)")
    if not tidal_anchors:
        lines.append("(no Tidal anchor strings were found in this file)")
    return "\n".join(lines)


def mask(value):
    """Keeps a credential out of a log that may be shared. First four, length."""
    if len(value) <= 4:
        return "*" * len(value)
    return f"{value[:4]}... ({len(value)} chars)"


def find_binary(path):
    """A file is used directly; a directory is searched for the stock player."""
    if os.path.isfile(path):
        return path
    if not os.path.isdir(path):
        raise ExtractError(f"{path} is neither a file nor a directory")
    direct = os.path.join(path, STOCK_PLAYER_REL)
    if os.path.isfile(direct):
        return direct
    # Fallback for an unusual unpack layout. An extracted rootfs is only a few
    # thousand files, so an exhaustive walk stays cheap; pruning directories
    # here could skip the very tree the binary lives in.
    for root, dirs, files in os.walk(path):
        if "hiby_player" in files:
            candidate = os.path.join(root, "hiby_player")
            if os.path.isfile(candidate):
                return candidate
    raise ExtractError(
        f"no usr/bin/hiby_player under {path}. Point this at an unpacked "
        "stock rootfs or at the binary itself.")


# ---------------------------------------------------------------------------
# Direct stock-archive unpacking (no 7z or unsquashfs needed)
# ---------------------------------------------------------------------------

_SPLIT_PART_RE = re.compile(r"^(?P<stem>.+)\.(?:(?P<part>part)(?P<pnum>\d+)|(?P<vnum>\d{3,}))$", re.IGNORECASE)


def lzo1x_decompress(src, max_out=131072):
    """Pure-Python LZO1X safe block decompressor (for SquashFS compression=3)."""
    out = bytearray()
    ip = 0
    end = len(src)
    if end == 0:
        return bytes(out)

    def need(n):
        if ip + n > end:
            raise ExtractError("corrupt LZO1X stream (truncated input)")

    def copy_lits(cnt):
        nonlocal ip
        need(cnt)
        if len(out) + cnt > max_out:
            raise ExtractError("corrupt LZO1X stream (output overflow)")
        out.extend(src[ip:ip + cnt])
        ip += cnt

    def copy_match(m_pos, m_len):
        if m_pos < 0 or m_pos >= len(out):
            raise ExtractError("corrupt LZO1X stream (invalid lookbehind)")
        if len(out) + m_len > max_out:
            raise ExtractError("corrupt LZO1X stream (output overflow)")
        dist = len(out) - m_pos
        if dist >= m_len:
            out.extend(out[m_pos:m_pos + m_len])
        else:
            for _ in range(m_len):
                out.append(out[m_pos])
                m_pos += 1

    enter_match = False
    need(1)
    t = src[ip]
    ip += 1
    if t > 17:
        t -= 17
        copy_lits(t)
        need(1)
        t = src[ip]
        ip += 1
        if t < 4 or t >= 16:
            enter_match = True
        else:
            t_next = t & 3
            need(1)
            m_pos = len(out) - 0x0801 - (t >> 2) - (src[ip] << 2)
            ip += 1
            copy_match(m_pos, 3)
            if t_next:
                copy_lits(t_next)
                need(1)
                t = src[ip]
                ip += 1
                enter_match = True
            else:
                need(1)
                t = src[ip]
                ip += 1

    while True:
        if not enter_match and t < 16:
            if t == 0:
                while True:
                    need(1)
                    b = src[ip]
                    ip += 1
                    if b != 0:
                        t += 15 + b
                        break
                    t += 255
            copy_lits(t + 3)
            need(1)
            t = src[ip]
            ip += 1
            if t < 16:
                t_next = t & 3
                need(1)
                m_pos = len(out) - 0x0801 - (t >> 2) - (src[ip] << 2)
                ip += 1
                copy_match(m_pos, 3)
                if t_next == 0:
                    need(1)
                    t = src[ip]
                    ip += 1
                    continue
                copy_lits(t_next)
                need(1)
                t = src[ip]
                ip += 1
        enter_match = False

        while True:
            if t >= 64:
                t_next = t & 3
                need(1)
                m_pos = len(out) - 1 - ((t >> 2) & 7) - (src[ip] << 3)
                ip += 1
                m_len = (t >> 5) + 1
            elif t >= 32:
                m_len = t & 31
                if m_len == 0:
                    while True:
                        need(1)
                        b = src[ip]
                        ip += 1
                        if b != 0:
                            m_len += 31 + b
                            break
                        m_len += 255
                m_len += 2
                need(2)
                v = src[ip] | (src[ip + 1] << 8)
                ip += 2
                m_pos = len(out) - 1 - (v >> 2)
                t_next = v & 3
            elif t >= 16:
                m_off = (t & 8) << 11
                m_len = t & 7
                if m_len == 0:
                    while True:
                        need(1)
                        b = src[ip]
                        ip += 1
                        if b != 0:
                            m_len += 7 + b
                            break
                        m_len += 255
                m_len += 2
                need(2)
                v = src[ip] | (src[ip + 1] << 8)
                ip += 2
                m_off += v >> 2
                if m_off == 0:
                    return bytes(out)
                m_pos = len(out) - m_off - 0x4000
                t_next = v & 3
            else:
                t_next = t & 3
                need(1)
                m_pos = len(out) - 1 - (t >> 2) - (src[ip] << 2)
                ip += 1
                m_len = 2

            copy_match(m_pos, m_len)
            if t_next == 0:
                need(1)
                t = src[ip]
                ip += 1
                break
            copy_lits(t_next)
            need(1)
            t = src[ip]
            ip += 1


def _sqfs_decompress_block(raw, compression, max_out):
    if compression == 1:  # gzip / zlib
        return zlib.decompress(raw, 15, max_out)
    if compression == 2 or compression == 4:  # lzma / xz
        return lzma.decompress(raw)
    if compression == 3:  # lzo
        return lzo1x_decompress(raw, max_out)
    raise ExtractError(f"unsupported SquashFS compression id {compression}")


def extract_from_squashfs_bytes(sq, rel_path="usr/bin/hiby_player"):
    """Extracts `rel_path` out of a SquashFS 4.0 image held in `sq`."""
    if len(sq) < 96 or sq[:4] != b"hsqs":
        raise ExtractError("not a SquashFS 4.0 little-endian image")
    (
        _magic, _inodes, _mtime, block_size, _frags, compression, _blog, _flags, _nids,
        s_major, s_minor, root_inode, _bytes_used, _id_tbl, _xattr_tbl,
        inode_tbl, dir_tbl, frag_tbl, _lookup_tbl,
    ) = struct.unpack_from("<4sIIIIHHHHHHQQQQQQQQ", sq, 0)
    if (s_major, s_minor) != (4, 0) or block_size < 4096 or block_size > (1 << 20):
        raise ExtractError(f"unsupported SquashFS version {s_major}.{s_minor}")

    meta_cache = {}

    def read_meta(base, block_off, inner_off, length):
        out = bytearray()
        cur = base + block_off
        off = inner_off
        while len(out) < length:
            if cur not in meta_cache:
                if cur + 2 > len(sq):
                    raise ExtractError("truncated SquashFS metadata block")
                hdr, = struct.unpack_from("<H", sq, cur)
                clen = hdr & 0x7FFF
                if clen == 0 or cur + 2 + clen > len(sq):
                    raise ExtractError("corrupt SquashFS metadata length")
                raw = sq[cur + 2:cur + 2 + clen]
                blk = raw if (hdr & 0x8000) else _sqfs_decompress_block(raw, compression, 8192)
                meta_cache[cur] = (blk, cur + 2 + clen)
            blk, nxt = meta_cache[cur]
            if off >= len(blk):
                off -= len(blk)
                cur = nxt
                continue
            take = min(length - len(out), len(blk) - off)
            out.extend(blk[off:off + take])
            off = 0
            cur = nxt
        return bytes(out)

    def lookup_entry(dir_ref, name):
        boff = (dir_ref >> 16) & 0xFFFFFFFF
        ioff = dir_ref & 0xFFFF
        itype, = struct.unpack_from("<H", read_meta(inode_tbl, boff, ioff, 16), 0)
        if itype == 1:
            ibuf = read_meta(inode_tbl, boff, ioff, 32)
            dblk, _nlink, fsize, doff, _parent = struct.unpack_from("<IIHHI", ibuf, 16)
        elif itype == 8:
            ibuf = read_meta(inode_tbl, boff, ioff, 40)
            _nlink, fsize, dblk, _parent, _icnt, doff, _xattr = struct.unpack_from("<IIIIHHI", ibuf, 16)
        else:
            raise ExtractError(f"expected directory inode while walking {rel_path}, got type {itype}")
        total = max(0, fsize - 3)
        dbuf = read_meta(dir_tbl, dblk, doff, total)
        pos = 0
        while pos < len(dbuf):
            if pos + 12 > len(dbuf):
                raise ExtractError("truncated SquashFS directory header")
            count, start_block, _inum = struct.unpack_from("<III", dbuf, pos)
            pos += 12
            for _ in range(count + 1):
                if pos + 8 > len(dbuf):
                    raise ExtractError("truncated SquashFS directory entry")
                eoff, _ediff, _etype, esize = struct.unpack_from("<HhHH", dbuf, pos)
                pos += 8
                ename = dbuf[pos:pos + esize + 1].decode("utf-8", "replace")
                pos += esize + 1
                if ename == name:
                    return (start_block << 16) | eoff
        return None

    parts = [p for p in rel_path.replace("\\", "/").split("/") if p]
    ref = root_inode
    for part in parts:
        nxt = lookup_entry(ref, part)
        if nxt is None:
            raise ExtractError(
                f"no {rel_path} in SquashFS rootfs (is this a Sonix image rather than stock firmware?)"
            )
        ref = nxt

    boff = (ref >> 16) & 0xFFFFFFFF
    ioff = ref & 0xFFFF
    itype, = struct.unpack_from("<H", read_meta(inode_tbl, boff, ioff, 16), 0)
    if itype == 2:
        ibuf = read_meta(inode_tbl, boff, ioff, 32)
        blocks_start, frag_idx, block_off, file_size = struct.unpack_from("<IIII", ibuf, 16)
        hdr_sz = 32
    elif itype == 9:
        ibuf = read_meta(inode_tbl, boff, ioff, 56)
        blocks_start, file_size, _sparse, _nlink, frag_idx, block_off, _xattr = struct.unpack_from(
            "<QQIIIII", ibuf, 16
        )
        hdr_sz = 56
    else:
        raise ExtractError(f"{rel_path} is not a regular file in SquashFS (inode type {itype})")

    if file_size > 32 * 1024 * 1024:
        raise ExtractError(f"{rel_path} in SquashFS is unexpectedly large ({file_size} bytes)")

    nfull = file_size // block_size if frag_idx != 0xFFFFFFFF else (file_size + block_size - 1) // block_size
    bsizes = struct.unpack_from(f"<{nfull}I", read_meta(inode_tbl, boff, ioff + hdr_sz, nfull * 4), 0) if nfull else ()

    out = bytearray()
    cur_d = blocks_start
    for bsz in bsizes:
        if bsz == 0:
            out.extend(b"\x00" * block_size)
        else:
            uncomp = bool(bsz & (1 << 24))
            clen = bsz & 0xFFFFFF
            if cur_d + clen > len(sq):
                raise ExtractError("truncated SquashFS file block")
            raw = sq[cur_d:cur_d + clen]
            cur_d += clen
            blk = raw if uncomp else _sqfs_decompress_block(raw, compression, block_size)
            out.extend(blk)

    if frag_idx != 0xFFFFFFFF:
        rem = file_size - len(out)
        frag_ptr_off = frag_tbl + (frag_idx // 512) * 8
        if frag_ptr_off + 8 > len(sq):
            raise ExtractError("truncated SquashFS fragment table")
        frag_blk_pos, = struct.unpack_from("<Q", sq, frag_ptr_off)
        fentry = read_meta(0, frag_blk_pos, (frag_idx % 512) * 16, 16)
        fstart, fsize, _ = struct.unpack_from("<QII", fentry, 0)
        uncomp = bool(fsize & (1 << 24))
        clen = fsize & 0xFFFFFF
        if fstart + clen > len(sq):
            raise ExtractError("truncated SquashFS fragment data")
        raw = sq[fstart:fstart + clen]
        fblk = raw if uncomp else _sqfs_decompress_block(raw, compression, block_size)
        if block_off + rem > len(fblk):
            raise ExtractError("corrupt SquashFS fragment slice")
        out.extend(fblk[block_off:block_off + rem])

    return bytes(out[:file_size])


def _looks_like_iso9660(buf):
    return len(buf) >= 17 * 2048 and buf[16 * 2048 + 1:16 * 2048 + 6] == b"CD001"


def extract_squashfs_from_upt_bytes(upt):
    """Reconstructs ota_v0/rootfs.squashfs from a HiByOS .upt ISO 9660 image."""
    if not _looks_like_iso9660(upt):
        raise ExtractError("not a valid .upt ISO 9660 firmware image")

    # Prefer Joliet SVD (type 2, UCS-2BE long names) so rootfs.squashfs.0000.<md5>
    # keeps its full name; fall back to Primary Volume Descriptor (type 1).
    chosen_vd = None
    joliet = False
    for sec in range(16, 32):
        off = sec * 2048
        if off + 2048 > len(upt) or upt[off + 1:off + 6] != b"CD001":
            break
        vtype = upt[off]
        if vtype == 255:
            break
        if vtype == 2:
            chosen_vd = upt[off:off + 2048]
            joliet = True
            break
        if vtype == 1 and chosen_vd is None:
            chosen_vd = upt[off:off + 2048]

    if chosen_vd is None:
        raise ExtractError("ISO 9660 image has no usable volume descriptor")

    def read_dir(lba, length):
        start = lba * 2048
        if start + length > len(upt):
            raise ExtractError("truncated ISO 9660 directory extent")
        buf = upt[start:start + length]
        pos = 0
        entries = []
        while pos < len(buf):
            rlen = buf[pos]
            if rlen == 0:
                pos = ((pos // 2048) + 1) * 2048
                continue
            if pos + rlen > len(buf) or rlen < 34:
                raise ExtractError("corrupt ISO 9660 directory record")
            rec = buf[pos:pos + rlen]
            elba, = struct.unpack_from("<I", rec, 2)
            elen, = struct.unpack_from("<I", rec, 10)
            flags = rec[25]
            nlen = rec[32]
            raw_name = rec[33:33 + nlen]
            if raw_name not in (b"\x00", b"\x01"):
                if joliet:
                    name = raw_name.decode("utf-16-be", "replace")
                else:
                    name = raw_name.decode("ascii", "replace")
                name = name.split(";", 1)[0]
                entries.append((name, elba, elen, bool(flags & 2)))
            pos += rlen
        return entries

    rlba, = struct.unpack_from("<I", chosen_vd, 158)
    rlen, = struct.unpack_from("<I", chosen_vd, 166)
    ota_dir = None
    for name, lba, ln, is_dir in read_dir(rlba, rlen):
        if is_dir and name.lower() == "ota_v0":
            ota_dir = (lba, ln)
            break
    if ota_dir is None:
        raise ExtractError("ISO 9660 image has no ota_v0 directory (not a HiByOS .upt)")

    sub = read_dir(*ota_dir)
    chunks = []
    expected_md5 = None
    expected_size = None
    for name, lba, ln, is_dir in sub:
        if is_dir:
            continue
        low = name.lower()
        if low == "ota_update.in":
            raw = upt[lba * 2048:lba * 2048 + ln].decode("utf-8", "replace")
            in_rootfs = False
            for line in raw.splitlines():
                line = line.strip()
                if line.startswith("img_name="):
                    in_rootfs = line.split("=", 1)[1].strip() == "rootfs.squashfs"
                elif in_rootfs and line.startswith("img_size="):
                    try:
                        expected_size = int(line.split("=", 1)[1].strip())
                    except ValueError:
                        pass
                elif in_rootfs and line.startswith("img_md5="):
                    expected_md5 = line.split("=", 1)[1].strip().lower()
        elif low.startswith("rootfs.squashfs."):
            rest = name[len("rootfs.squashfs."):]
            num_str = rest.split(".", 1)[0]
            if num_str.isdigit():
                chunks.append((int(num_str), name, lba, ln))

    if not chunks:
        raise ExtractError("ota_v0 has no rootfs.squashfs.* chunks")
    chunks.sort(key=lambda item: item[0])
    for idx, (num, _cname, lba, ln) in enumerate(chunks):
        if num != idx:
            raise ExtractError(f"missing rootfs.squashfs chunk {idx:04d} (saw {num:04d})")
        if lba * 2048 + ln > len(upt):
            raise ExtractError(f"truncated chunk {_cname} in .upt")

    sq = b"".join(upt[lba * 2048:lba * 2048 + ln] for _num, _cname, lba, ln in chunks)
    if expected_size is not None and len(sq) != expected_size:
        raise ExtractError(
            f"reconstructed rootfs.squashfs is {len(sq)} bytes, ota_update.in says {expected_size}"
        )
    if expected_md5:
        got_md5 = hashlib.md5(sq).hexdigest().lower()
        if got_md5 != expected_md5:
            raise ExtractError(
                f"reconstructed rootfs.squashfs MD5 {got_md5} != ota_update.in {expected_md5}"
            )
    return sq


def _join_split_pieces(path):
    """If `path` is NAME.part1 / NAME.001, joins all sibling pieces in order."""
    base = os.path.basename(path)
    m = _SPLIT_PART_RE.match(base)
    if not m:
        return None
    stem = m.group("stem")
    is_part = m.group("part") is not None
    parent = os.path.dirname(os.path.abspath(path))
    found = []
    for entry in os.listdir(parent):
        full = os.path.join(parent, entry)
        if not os.path.isfile(full):
            continue
        em = _SPLIT_PART_RE.match(entry)
        if not em or em.group("stem") != stem:
            continue
        if (em.group("part") is not None) != is_part:
            continue
        num = int(em.group("pnum") if is_part else em.group("vnum"))
        found.append((num, full))
    if not found:
        return None
    found.sort(key=lambda item: item[0])
    expected = found[0][0]
    pieces = []
    for num, full in found:
        if num != expected:
            raise ExtractError(
                f"split archive {stem} is missing piece {expected} before {os.path.basename(full)}"
            )
        with open(full, "rb") as handle:
            pieces.append(handle.read())
        expected += 1
    return stem, b"".join(pieces)


def _unpack_blob_to_player(blob, label):
    """Unwraps ZIP -> .upt -> SquashFS -> hiby_player bytes from an in-memory blob."""
    if blob[:4] == b"PK\x03\x04":
        try:
            with zipfile.ZipFile(io.BytesIO(blob)) as zf:
                names = [n for n in zf.namelist() if not n.endswith("/")]
                for name in names:
                    if os.path.basename(name) == "hiby_player":
                        return f"{label}:{name}", zf.read(name)
                for name in names:
                    if name.lower().endswith(".upt"):
                        return _unpack_blob_to_player(zf.read(name), f"{label}:{name}")
                for name in names:
                    if "squashfs" in name.lower():
                        return _unpack_blob_to_player(zf.read(name), f"{label}:{name}")
        except zipfile.BadZipFile as exc:
            raise ExtractError(f"{label}: invalid ZIP archive ({exc})") from exc
        raise ExtractError(f"{label}: ZIP archive contains neither v1.upt nor hiby_player")

    if _looks_like_iso9660(blob):
        sq = extract_squashfs_from_upt_bytes(blob)
        return f"{label}:usr/bin/hiby_player", extract_from_squashfs_bytes(sq)

    if blob[:4] == b"hsqs":
        return f"{label}:usr/bin/hiby_player", extract_from_squashfs_bytes(blob)

    return label, blob


def load_player_bytes(path=None):
    """Resolves `path` (ELF, unpacked rootfs, .upt, .squashfs, .zip, or .part1) to (source_label, bytes)."""
    if path is None:
        if os.path.isfile(DEFAULT_STOCK_INPUT):
            path = DEFAULT_STOCK_INPUT
        else:
            raise ExtractError(
                "no input path given and bundled stock firmware was not found at "
                f"{DEFAULT_STOCK_INPUT}"
            )

    if os.path.isdir(path):
        # First try an already-unpacked rootfs directory.
        try:
            binary = find_binary(path)
            with open(binary, "rb") as handle:
                return binary, handle.read()
        except ExtractError:
            pass
        # Otherwise look for a stock archive (.upt, .zip, .part1) inside the folder.
        for entry in sorted(os.listdir(path)):
            low = entry.lower()
            full = os.path.join(path, entry)
            if os.path.isfile(full) and (
                low.endswith(".upt") or low.endswith(".zip") or low.endswith(".part1") or low.endswith(".001")
            ):
                return load_player_bytes(full)
        raise ExtractError(
            f"no usr/bin/hiby_player or stock firmware archive (.upt/.zip) under {path}."
        )

    if not os.path.isfile(path):
        raise ExtractError(f"{path} is neither a file nor a directory")

    joined = _join_split_pieces(path)
    if joined is not None:
        stem, raw = joined
        return _unpack_blob_to_player(raw, os.path.join(os.path.dirname(path), stem))

    with open(path, "rb") as handle:
        raw = handle.read()
    return _unpack_blob_to_player(raw, path)


def _parse_ini(text):
    sections = {}
    order = []
    section = None
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line[0] in "#;":
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1].strip()
            if section not in sections:
                sections[section] = {}
                order.append(section)
            continue
        if section and "=" in line:
            key, value = line.split("=", 1)
            sections[section][key.strip()] = value.strip()
    return sections, order


def merge_ini(existing, app_id=None, app_secret=None, tidal=None):
    """Replaces extracted sections ([qobuz] and/or [tidal]), keeping others."""
    sections, order = _parse_ini(existing or "")
    if app_id is not None and app_secret is not None:
        if "qobuz" not in sections:
            order.insert(0, "qobuz")
        sections["qobuz"] = {"app_id": app_id, "app_secret": app_secret}
    if tidal is not None:
        client_id, client_secret = tidal
        if "tidal" not in sections:
            insert_at = order.index("qobuz") + 1 if "qobuz" in order else 0
            order.insert(insert_at, "tidal")
        sections["tidal"] = {"client_id": client_id, "client_secret": client_secret}

    out = [
        "# Streaming service credentials, extracted from the stock firmware of",
        "# this device. Do not commit or share this file, and do not publish an",
        "# image that carries it: these pairs belong to HiBy, not to Sonix.",
        "",
    ]
    for name in order:
        out.append(f"[{name}]")
        for key, value in sections[name].items():
            out.append(f"{key} = {value}")
        out.append("")
    return "\n".join(out)


def _seal(plain, key_path, bin_out):
    # Same directory as this file: the sealing format it implements is the one
    # streamkeys.c reads. Imported lazily so extraction never needs it.
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import seal_streamkeys

    key = seal_streamkeys.read_key(key_path)
    if key is None:
        raise ExtractError(
            f"no sealing key at {key_path}. Build the player once to create "
            "streaming-keys.key, or leave --seal off for a plain ini that a "
            "keyless build reads anyway.")
    blob = seal_streamkeys.seal(key, plain.encode("utf-8"))
    if len(blob) > seal_streamkeys.MAX_FILE:
        raise ExtractError("the sealed file would be too large")
    os.makedirs(os.path.dirname(os.path.abspath(bin_out)), exist_ok=True)
    with open(bin_out, "wb") as handle:
        handle.write(blob)


def main(argv=None):
    parser = argparse.ArgumentParser(
        description=(
            "Extract Qobuz and Tidal credentials from a stock HiByOS player binary, "
            "unpacked rootfs, .upt image, .squashfs, or .zip/.part1 archive."
        ))
    parser.add_argument(
        "path",
        nargs="?",
        default=None,
        help=(
            "hiby_player binary, unpacked rootfs, .upt, .squashfs, or .zip/.part1 "
            "(default: bundled sonix-packer/firmware-input/v1.zip.part1)"
        ),
    )
    parser.add_argument("-o", "--out", default=DEFAULT_INI,
                        help="streaming-keys.ini to write (default: %(default)s)")
    parser.add_argument("--service", choices=("all", "qobuz", "tidal"), default="all",
                        help="which credentials to extract (default: %(default)s)")
    parser.add_argument("--seal", action="store_true",
                        help="also write a sealed streaming-keys.bin")
    parser.add_argument("--key", default=os.path.join(PLAYER_ROOT, "streaming-keys.key"),
                        help="sealing key from the player build (default: %(default)s)")
    parser.add_argument("--bin-out", help="destination of the sealed .bin (with --seal)")
    parser.add_argument("--check", action="store_true",
                        help="report whether credentials can be extracted, write nothing")
    parser.add_argument("--list-candidates", action="store_true",
                        help="print every plausible string and exit")
    parser.add_argument("--reveal", action="store_true",
                        help="print full values instead of masking them")
    parser.add_argument("--app-id", help="force the Qobuz app_id (manual fallback)")
    parser.add_argument("--app-secret", help="force the Qobuz app_secret (manual fallback)")
    parser.add_argument("--tidal-client-id", help="force the Tidal client_id (manual fallback)")
    parser.add_argument("--tidal-client-secret", help="force the Tidal client_secret (manual fallback)")
    args = parser.parse_args(argv)

    try:
        binary, data = load_player_bytes(args.path)

        if args.list_candidates:
            print(list_candidates(data))
            return 0

        qobuz_pair = None
        tidal_pair = None

        if args.service in ("all", "qobuz"):
            if args.app_id and args.app_secret:
                qobuz_pair = (args.app_id, args.app_secret)
            elif args.app_id or args.app_secret:
                raise ExtractError("--app-id and --app-secret must be given together")
            else:
                qobuz_pair = extract(data)

        if args.service in ("all", "tidal"):
            if args.tidal_client_id and args.tidal_client_secret:
                tidal_pair = (args.tidal_client_id, args.tidal_client_secret)
            elif args.tidal_client_id or args.tidal_client_secret:
                raise ExtractError("--tidal-client-id and --tidal-client-secret must be given together")
            elif args.service == "tidal" or has_tidal_anchors(data):
                tidal_pair = extract_tidal(data)

        print(f"source:            {binary}")
        if qobuz_pair is not None:
            shown_id = qobuz_pair[0] if args.reveal else mask(qobuz_pair[0])
            shown_secret = qobuz_pair[1] if args.reveal else mask(qobuz_pair[1])
            print(f"qobuz app_id:      {shown_id}")
            print(f"qobuz app_secret:  {shown_secret}")
        if tidal_pair is not None:
            shown_tid = tidal_pair[0] if args.reveal else mask(tidal_pair[0])
            shown_tsec = tidal_pair[1] if args.reveal else mask(tidal_pair[1])
            print(f"tidal client_id:   {shown_tid}")
            print(f"tidal client_secret: {shown_tsec}")

        if args.check:
            print("check only: nothing written")
            return 0

        existing = ""
        if os.path.exists(args.out):
            with open(args.out, "r", encoding="utf-8", errors="replace") as handle:
                existing = handle.read()
        text = merge_ini(
            existing,
            app_id=qobuz_pair[0] if qobuz_pair else None,
            app_secret=qobuz_pair[1] if qobuz_pair else None,
            tidal=tidal_pair,
        )
        os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
        with open(args.out, "w", encoding="utf-8") as handle:
            handle.write(text)
        try:
            os.chmod(args.out, 0o600)
        except OSError:
            pass
        print(f"written:     {args.out}")

        if args.seal:
            if not args.bin_out:
                raise ExtractError("--seal needs --bin-out")
            _seal(text, args.key, args.bin_out)
            print(f"sealed:      {args.bin_out}")
        return 0
    except ExtractError as err:
        print(f"extract_streamkeys: {err}", file=sys.stderr)
        return 1
    except OSError as err:
        print(f"extract_streamkeys: {err}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
