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

    python3 tools/extract_streamkeys.py path/to/hiby_player
    python3 tools/extract_streamkeys.py unpacked/rootfs        # finds usr/bin/hiby_player
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
import os
import re
import struct
import sys

PLAYER_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_INI = os.path.join(PLAYER_ROOT, "streaming-keys.ini")
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
        description="Extract Qobuz and Tidal credentials from a stock HiByOS player binary.")
    parser.add_argument("path", help="hiby_player binary, or an unpacked rootfs directory")
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
        binary = find_binary(args.path)
        with open(binary, "rb") as handle:
            data = handle.read()

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
