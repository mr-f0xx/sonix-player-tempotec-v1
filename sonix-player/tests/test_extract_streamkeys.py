#!/usr/bin/env python3
"""
Tests for tools/extract_streamkeys.py: the extraction of the Qobuz
application credentials from a stock HiByOS player binary.

No stock binary is available to the tests, so the fixtures reproduce the
documented shape of hiby_player's .rodata: NUL-separated C strings, the Qobuz
endpoint and header strings the Sonix client cites from the real binary, and
the known Qobuz credential formats (numeric app_id, 32-hex app_secret).
Plain Python only; run with `make extractkeys` or directly.
"""

import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.join(os.path.dirname(HERE), "tools")
sys.path.insert(0, TOOLS)

import extract_streamkeys as ex  # noqa: E402
import seal_streamkeys  # noqa: E402

APP_ID = b"742789304"
APP_SECRET = b"0a1b2c3d4e5f60718293a4b5c6d7e8f9"
WORD_SECRET = b"aZ39bQ72xW11kM48pV63nD27sT05hJ94"

# Strings qobuz.c traces to the stock V1 binary, so they are the anchors the
# real file is known to contain.
ANCHORS = [
    b"http://www.qobuz.com/api.json/0.2",
    b"X-App-Id: %s",
    b"device_manufacturer_id: %s",
    b"trackgetFileUrl",
    b"request_sig",
    b"%s/user/login?app_id=%s",
]


def blob(parts):
    """Joins string fragments the way .rodata stores them: NUL-separated."""
    return b"\x00".join(parts) + b"\x00"


def placed(items):
    """Places strings at explicit offsets, for exact distance assertions."""
    size = max(offset + len(text) for offset, text in items) + 1
    buf = bytearray(b"\x00" * size)
    for offset, text in items:
        buf[offset:offset + len(text)] = text
    return bytes(buf)


def realistic_binary():
    # Credentials grouped with the Qobuz strings, as C strings in .rodata;
    # distractors of the same shapes sit far away, among unrelated data.
    near = [
        ANCHORS[0], ANCHORS[5], APP_ID,
        ANCHORS[1], ANCHORS[2], APP_SECRET,
        ANCHORS[3], ANCHORS[4],
    ]
    padding = [b"unrelated string %d" % i for i in range(40)]
    far = [
        b"1234567890",                                  # numeric distractor
        b"fedcba9876543210fedcba9876543210",            # 32-hex distractor
        b"libcurl/7.68.0", b"OpenSSL 1.1.1f", b"443",
    ]
    return blob(near + padding + far)


class ExtractTests(unittest.TestCase):
    def test_finds_the_pair_beside_the_qobuz_strings(self):
        app_id, secret = ex.extract(realistic_binary())
        self.assertEqual(app_id, APP_ID.decode())
        self.assertEqual(secret, APP_SECRET.decode())

    def test_format_agnostic_an_elf_header_changes_nothing(self):
        # The real file is a MIPS32 ELF; scanning bytes, not parsing sections.
        elf = b"\x7fELF\x01\x01\x01\x00" + b"\x00" * 24 + realistic_binary()
        app_id, secret = ex.extract(elf)
        self.assertEqual((app_id, secret), (APP_ID.decode(), APP_SECRET.decode()))

    def test_no_qobuz_anchors_is_an_error_not_a_guess(self):
        data = blob([b"123456789", b"0a1b2c3d4e5f60718293a4b5c6d7e8f9",
                     b"libasound.so.2", b"totally unrelated"])
        with self.assertRaises(ex.ExtractError):
            ex.extract(data)

    def test_bare_word_qobuz_without_api_strings_is_rejected(self):
        data = blob([b"qobuz", b"123456789", b"0a1b2c3d4e5f60718293a4b5c6d7e8f9"])
        with self.assertRaises(ex.ExtractError):
            ex.extract(data)

    def test_missing_either_value_is_an_error(self):
        no_secret = blob([ANCHORS[0], APP_ID, ANCHORS[2]])
        with self.assertRaises(ex.ExtractError):
            ex.extract(no_secret)
        no_id = blob([ANCHORS[0], APP_SECRET, ANCHORS[2]])
        with self.assertRaises(ex.ExtractError):
            ex.extract(no_id)

    def test_equidistant_secrets_are_refused_not_guessed(self):
        # Two 32-hex strings at the exact same distance from the only anchor.
        other = b"11223344556677889900aabbccddeeff"
        data = placed([(100, APP_SECRET), (200, ANCHORS[0]), (300, other),
                       (500, APP_ID)])
        with self.assertRaises(ex.ExtractError) as ctx:
            ex.extract(data)
        self.assertIn("app_secret", str(ctx.exception))

    def test_word_character_secret_fallback(self):
        data = blob([ANCHORS[0], APP_ID, ANCHORS[2], WORD_SECRET])
        _, secret = ex.extract(data)
        self.assertEqual(secret, WORD_SECRET.decode())

    def test_hex_secret_beats_an_equidistant_word_secret(self):
        data = blob([ANCHORS[0], APP_ID, ANCHORS[2], APP_SECRET, WORD_SECRET])
        _, secret = ex.extract(data)
        self.assertEqual(secret, APP_SECRET.decode())

    def test_list_candidates_is_safe_and_sorted(self):
        report = ex.list_candidates(realistic_binary())
        self.assertIn("value", report)
        self.assertNotIn(APP_SECRET.decode(), report)
        self.assertIn(APP_SECRET[:4].decode(), report)

    def test_mask_never_shows_a_full_secret(self):
        masked = ex.mask(APP_SECRET.decode())
        self.assertNotIn(APP_SECRET.decode(), masked)
        self.assertTrue(masked.startswith(APP_SECRET[:4].decode()))


class FindBinaryTests(unittest.TestCase):
    def test_file_is_used_directly(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = os.path.join(tmp, "hiby_player")
            with open(binary, "wb") as handle:
                handle.write(realistic_binary())
            self.assertEqual(ex.find_binary(binary), binary)

    def test_rootfs_directory_is_walked_to_the_stock_path(self):
        with tempfile.TemporaryDirectory() as tmp:
            target = os.path.join(tmp, "usr", "bin", "hiby_player")
            os.makedirs(os.path.dirname(target))
            with open(target, "wb") as handle:
                handle.write(realistic_binary())
            self.assertEqual(ex.find_binary(tmp), target)

    def test_missing_binary_is_a_clear_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(ex.ExtractError):
                ex.find_binary(tmp)


class IniTests(unittest.TestCase):
    def test_merge_replaces_qobuz_and_keeps_the_other_sections(self):
        existing = "[tidal]\nclient_id = tid\nclient_secret = tsec\n\n" \
                   "[podcast]\napi_key = pk\napi_secret = ps\n"
        out = ex.merge_ini(existing, "123456789", "0a1b2c3d4e5f60718293a4b5c6d7e8f9")
        sections, order = ex._parse_ini(out)
        self.assertEqual(sections["qobuz"]["app_id"], "123456789")
        self.assertEqual(sections["qobuz"]["app_secret"], "0a1b2c3d4e5f60718293a4b5c6d7e8f9")
        self.assertEqual(sections["tidal"]["client_id"], "tid")
        self.assertEqual(sections["podcast"]["api_key"], "pk")
        self.assertIn("qobuz", order)

    def test_merge_into_an_empty_file(self):
        out = ex.merge_ini("", "123456789", "0a1b2c3d4e5f60718293a4b5c6d7e8f9")
        sections, _ = ex._parse_ini(out)
        self.assertEqual(set(sections), {"qobuz"})


class MainTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.binary = os.path.join(self.tmp.name, "hiby_player")
        with open(self.binary, "wb") as handle:
            handle.write(realistic_binary())
        self.ini = os.path.join(self.tmp.name, "streaming-keys.ini")

    def tearDown(self):
        self.tmp.cleanup()

    def test_check_writes_nothing(self):
        code = ex.main([self.binary, "--check", "-o", self.ini])
        self.assertEqual(code, 0)
        self.assertFalse(os.path.exists(self.ini))

    def test_end_to_end_writes_a_owner_only_ini_without_leaking_to_stdout(self):
        import io
        import contextlib
        captured = io.StringIO()
        with contextlib.redirect_stdout(captured):
            code = ex.main([self.binary, "-o", self.ini])
        self.assertEqual(code, 0)
        self.assertTrue(os.path.exists(self.ini))
        self.assertEqual(os.stat(self.ini).st_mode & 0o777, 0o600)
        # The full secret belongs in the file, not the console output.
        self.assertNotIn(APP_SECRET.decode(), captured.getvalue())
        with open(self.ini, encoding="utf-8") as handle:
            self.assertIn(APP_SECRET.decode(), handle.read())

    def test_force_overrides_an_ambiguous_binary(self):
        other = b"11223344556677889900aabbccddeeff"
        with open(self.binary, "wb") as handle:
            handle.write(blob([APP_SECRET, ANCHORS[0], ANCHORS[2], other, APP_ID]))
        code = ex.main([self.binary, "-o", self.ini,
                        "--app-id", "999111222", "--app-secret", other.decode()])
        self.assertEqual(code, 0)
        sections, _ = ex._parse_ini(open(self.ini, encoding="utf-8").read())
        self.assertEqual(sections["qobuz"]["app_id"], "999111222")
        self.assertEqual(sections["qobuz"]["app_secret"], other.decode())

    def test_force_requires_both_halves(self):
        code = ex.main([self.binary, "-o", self.ini, "--app-id", "999111222"])
        self.assertEqual(code, 1)

    def test_missing_file_is_reported_without_a_traceback(self):
        code = ex.main([os.path.join(self.tmp.name, "nope"), "-o", self.ini])
        self.assertEqual(code, 1)

    def test_an_unwritable_output_is_exit_code_2(self):
        blocker = os.path.join(self.tmp.name, "afile")
        open(blocker, "w").close()
        code = ex.main([self.binary, "-o", os.path.join(blocker, "x.ini")])
        self.assertEqual(code, 2)

    def test_list_candidates_through_main(self):
        import io
        import contextlib
        captured = io.StringIO()
        with contextlib.redirect_stdout(captured):
            code = ex.main([self.binary, "--list-candidates"])
        self.assertEqual(code, 0)
        self.assertIn("secret?", captured.getvalue())
        self.assertFalse(os.path.exists(self.ini))

    def test_sealed_output_round_trips_through_the_player_format(self):
        key_path = os.path.join(self.tmp.name, "streaming-keys.key")
        bin_out = os.path.join(self.tmp.name, "components", "streaming-keys.bin")
        key = os.urandom(seal_streamkeys.KEY_LEN)
        with open(key_path, "w") as handle:
            handle.write(key.hex())
        code = ex.main([self.binary, "-o", self.ini, "--seal",
                        "--key", key_path, "--bin-out", bin_out])
        self.assertEqual(code, 0)
        with open(bin_out, "rb") as handle:
            plain = seal_streamkeys.unseal(key, handle.read()).decode()
        sections, _ = ex._parse_ini(plain)
        self.assertEqual(sections["qobuz"]["app_id"], APP_ID.decode())
        self.assertEqual(sections["qobuz"]["app_secret"], APP_SECRET.decode())

    def test_seal_without_a_key_fails_cleanly(self):
        bin_out = os.path.join(self.tmp.name, "streaming-keys.bin")
        code = ex.main([self.binary, "-o", self.ini, "--seal",
                        "--key", os.path.join(self.tmp.name, "no.key"),
                        "--bin-out", bin_out])
        self.assertEqual(code, 1)
        self.assertFalse(os.path.exists(bin_out))


TIDAL_CLIENT_ID = b"zX9yV8wU7tS6rQ5p"
TIDAL_TOKEN_DECOY = b"mN4bV3cX2zL1kJ0h"
TIDAL_CLIENT_SECRET = b"AbCdEfGhIjKlMnOpQrStUvWxYz0123456789+/AbCdE="


def _mips_lui(rt, imm):
    import struct
    return struct.pack("<I", (0x0F << 26) | (rt << 16) | (imm & 0xFFFF))


def _mips_addiu(rt, rs, simm):
    import struct
    return struct.pack("<I", (0x09 << 26) | (rs << 21) | (rt << 16) | (simm & 0xFFFF))


def _mips_sb(rt, simm, rs):
    import struct
    return struct.pack("<I", (0x28 << 26) | (rs << 21) | (rt << 16) | (simm & 0xFFFF))


def _emit_assembled_token(token, bss_hi, bss_lo, trailing_va=None):
    """Builds the exact GCC MIPS32 LE sequence seen in hiby_player:
    lui $a2, bss_hi; addiu $v0, $a2, bss_lo; sb ch0, bss_lo($a2); sb ch1..N, 1..N($v0)
    followed by optional `lui $a1, hi; addiu $a1, $a1, lo` loading a .rodata string.
    """
    code = bytearray()
    code += _mips_addiu(7, 0, token[0])
    code += _mips_lui(6, bss_hi)
    code += _mips_addiu(2, 6, bss_lo)
    code += _mips_sb(7, bss_lo, 6)
    for idx, ch in enumerate(token[1:], start=1):
        code += _mips_addiu(3, 0, ch)
        code += _mips_sb(3, idx, 2)
    code += _mips_sb(0, len(token), 2)
    if trailing_va is not None:
        hi = (trailing_va + 0x8000) >> 16
        lo = trailing_va - (hi << 16)
        code += _mips_lui(5, hi)
        code += _mips_addiu(5, 5, lo)
    return bytes(code)


def realistic_mips_elf_with_tidal():
    """Minimal 32-bit LE ELF with one PT_LOAD mapping code + .rodata at vaddr 0x400000."""
    import struct

    vbase = 0x00400000
    hdr_size = 52 + 32  # ELF32_Ehdr + 1 * ELF32_Phdr
    rodata_off = 0x400

    rodata = bytearray()

    def add_cstr(s):
        off = len(rodata)
        rodata.extend(s + b"\x00")
        while len(rodata) % 4:
            rodata.append(0)
        return vbase + rodata_off + off

    va_client_id_lbl = add_cstr(b"client_id")
    va_tidal_db_lbl = add_cstr(b"%s/tidal.db")
    add_cstr(b"urn:ietf:params:oauth:grant-type:device_code")
    add_cstr(TIDAL_CLIENT_SECRET)
    add_cstr(b"https://auth.tidal.com/v1/oauth2/device_authorization")
    add_cstr(b"https://auth.tidal.com/v1/oauth2/token")
    for s in [ANCHORS[0], ANCHORS[5], APP_ID, ANCHORS[1], ANCHORS[2], APP_SECRET]:
        add_cstr(s)

    code = bytearray()
    # Decoy legacy x-tidal-token assembled first (references %s/tidal.db, NOT client_id)
    code += _emit_assembled_token(TIDAL_TOKEN_DECOY, 0x009F, 0x7DF0, va_tidal_db_lbl)
    code += b"\x00" * 64
    # Real OAuth2 client_id assembled right before loading "client_id"
    code += _emit_assembled_token(TIDAL_CLIENT_ID, 0x009F, 0x7E08, va_client_id_lbl)

    assert hdr_size + len(code) <= rodata_off
    payload = bytearray(b"\x00" * rodata_off)
    payload[hdr_size:hdr_size + len(code)] = code
    payload.extend(rodata)

    ehdr = bytearray(52)
    ehdr[0:4] = b"\x7fELF"
    ehdr[4] = 1  # ELFCLASS32
    ehdr[5] = 1  # ELFDATA2LSB
    ehdr[6] = 1
    struct.pack_into("<HHIIIIIHHHHHH", ehdr, 16,
                     2, 8, 1, vbase + hdr_size, 52, 0, 0, 52, 32, 1, 0, 0, 0)
    phdr = struct.pack("<8I", 1, 0, vbase, vbase, len(payload), len(payload), 5, 0x1000)
    payload[0:52] = ehdr
    payload[52:84] = phdr
    return bytes(payload)


class TidalExtractTests(unittest.TestCase):
    def test_extracts_mips_assembled_client_id_and_ignores_decoy_token(self):
        buf = realistic_mips_elf_with_tidal()
        cid, csec = ex.extract_tidal(buf)
        self.assertEqual(cid, TIDAL_CLIENT_ID.decode())
        self.assertEqual(csec, TIDAL_CLIENT_SECRET.decode())

    def test_default_all_writes_both_qobuz_and_tidal_masked_on_stdout(self):
        import io
        import contextlib
        with tempfile.TemporaryDirectory() as tmp:
            bpath = os.path.join(tmp, "hiby_player")
            ipath = os.path.join(tmp, "streaming-keys.ini")
            with open(bpath, "wb") as f:
                f.write(realistic_mips_elf_with_tidal())
            captured = io.StringIO()
            with contextlib.redirect_stdout(captured):
                rc = ex.main([bpath, "-o", ipath])
            self.assertEqual(rc, 0)
            out_log = captured.getvalue()
            self.assertNotIn(TIDAL_CLIENT_ID.decode(), out_log)
            self.assertNotIn(TIDAL_CLIENT_SECRET.decode(), out_log)
            with open(ipath, encoding="utf-8") as f:
                sections, _ = ex._parse_ini(f.read())
            self.assertEqual(sections["qobuz"]["app_id"], APP_ID.decode())
            self.assertEqual(sections["tidal"]["client_id"], TIDAL_CLIENT_ID.decode())
            self.assertEqual(sections["tidal"]["client_secret"], TIDAL_CLIENT_SECRET.decode())

    def test_service_tidal_preserves_existing_qobuz_and_other_sections(self):
        with tempfile.TemporaryDirectory() as tmp:
            bpath = os.path.join(tmp, "hiby_player")
            ipath = os.path.join(tmp, "streaming-keys.ini")
            with open(bpath, "wb") as f:
                f.write(realistic_mips_elf_with_tidal())
            with open(ipath, "w", encoding="utf-8") as f:
                f.write("[qobuz]\napp_id = keep_id\napp_secret = keep_sec\n")
            rc = ex.main([bpath, "--service", "tidal", "-o", ipath])
            self.assertEqual(rc, 0)
            with open(ipath, encoding="utf-8") as f:
                sections, _ = ex._parse_ini(f.read())
            self.assertEqual(sections["qobuz"]["app_id"], "keep_id")
            self.assertEqual(sections["tidal"]["client_id"], TIDAL_CLIENT_ID.decode())


if __name__ == "__main__":
    unittest.main(verbosity=2)
