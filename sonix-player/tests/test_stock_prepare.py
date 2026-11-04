#!/usr/bin/env python3
"""Host checks for prepare_v1_stock.sh: finding v1.upt in split archives.

The bundled TempoTec V1 1.3 OTA is committed as v1.zip.part1 + v1.zip.part2,
plain byte splits of one zip that neither 7-Zip nor unzip opens piece by piece.
These tests build a stand-in update (a zip that lists ota_v0/ota_update.in,
which is all the script's format check asks 7-Zip for), wrap it the way the
firmware arrives, and check that the script hands back exactly that file.
Needs 7z on PATH, as the script itself does; skipped without it.
"""
import io
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
PREPARE = ROOT / "sonix-packer" / "prepare_v1_stock.sh"


def fake_upt():
    data = io.BytesIO()
    with zipfile.ZipFile(data, "w", zipfile.ZIP_STORED) as archive:
        archive.writestr("ota_config.in", "current_version=0\n")
        archive.writestr("ota_v0/ota_update.in", "ota_version=0\n\n")
        archive.writestr("ota_v0/ota_compatibility.in", "compatibility=tempotec_v1_x1600_auto\n")
        # Incompressible filler, so the outer archive is big enough to split.
        archive.writestr("ota_v0/xImage.0000." + "0" * 32, os.urandom(96 * 1024))
    return data.getvalue()


def wrapped(upt):
    data = io.BytesIO()
    with zipfile.ZipFile(data, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("v1.upt", upt)
    return data.getvalue()


def split(data, pieces):
    size = -(-len(data) // pieces)
    return [data[i:i + size] for i in range(0, len(data), size)]


@unittest.skipUnless(shutil.which("7z"), "7z is not installed")
class StockPrepareTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="sonix-stock-prepare-"))
        self.upt = fake_upt()
        self.archive = wrapped(self.upt)

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def pieces(self, names, folder="input"):
        directory = self.tmp / folder
        directory.mkdir(exist_ok=True)
        (directory / "README.md").write_text("not firmware\n")
        for name, chunk in zip(names, split(self.archive, len(names))):
            (directory / name).write_bytes(chunk)
        return directory

    def prepare(self, source, expect_ok=True):
        out = self.tmp / "packer"
        shutil.rmtree(out, ignore_errors=True)
        result = subprocess.run(["bash", str(PREPARE), str(source), str(out)],
                                capture_output=True, text=True)
        if expect_ok:
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual((out / "v1_original.upt").read_bytes(), self.upt)
        else:
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse((out / "v1_original.upt").exists())
        return result

    def test_part_pieces_from_the_first_piece_any_piece_or_the_folder(self):
        folder = self.pieces(["v1.zip.part1", "v1.zip.part2"])
        result = self.prepare(folder / "v1.zip.part1")
        self.assertIn("Joined v1.zip from 2 piece(s)", result.stderr)
        self.prepare(folder / "v1.zip.part2")
        result = self.prepare(folder)
        # A folder walk opens the archive once, from its first piece.
        self.assertEqual(result.stderr.count("Joined v1.zip"), 1)

    def test_numbered_volumes_and_numeric_order(self):
        names = ["v1_upload.zip.%03d" % i for i in range(1, 12)]
        folder = self.pieces(names)
        result = self.prepare(folder)
        self.assertIn("Joined v1_upload.zip from 11 piece(s)", result.stderr)
        # part10 must come after part9, not after part1.
        folder = self.pieces(["v1.zip.part%d" % i for i in range(1, 12)], "parts")
        self.prepare(folder / "v1.zip.part1")

    def test_a_missing_piece_is_an_error(self):
        folder = self.pieces(["v1.zip.part1", "v1.zip.part2", "v1.zip.part3"])
        (folder / "v1.zip.part2").unlink()
        result = self.prepare(folder, expect_ok=False)
        self.assertIn("has no piece 2", result.stderr)

    def test_a_missing_last_piece_or_mixed_styles_do_not_pass(self):
        folder = self.pieces(["v1.zip.part1", "v1.zip.002"])
        result = self.prepare(folder / "v1.zip.part1", expect_ok=False)
        self.assertIn("is a piece missing or truncated", result.stderr)

    def test_unsplit_inputs_still_work(self):
        direct = self.tmp / "direct"
        direct.mkdir()
        (direct / "v1.upt").write_bytes(self.upt)
        self.prepare(direct / "v1.upt")
        self.prepare(direct)
        archive = self.tmp / "archive"
        archive.mkdir()
        (archive / "V1.1.zip").write_bytes(self.archive)
        self.prepare(archive / "V1.1.zip")
        self.prepare(archive)


if __name__ == "__main__":
    unittest.main()
