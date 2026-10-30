#!/usr/bin/env python3
"""Host checks for stock build extraction and packer metadata stamping."""
import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
PACKER = ROOT / "sonix-packer"
spec = importlib.util.spec_from_file_location("base_build", PACKER / "extract_base_build.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class BaseBuildTest(unittest.TestCase):
    def test_vendor_stamp(self):
        binary = b"\x7fELF\x00Firmware Built : Mar 26 2026  18:49:34\x00other strings\x00"
        self.assertEqual(module.extract_base_build(binary), "2026-03-26 18:49:34")
        self.assertEqual(module.extract_base_build(binary * 2), "2026-03-26 18:49:34")

    def test_unknown_or_invalid(self):
        for binary in (b"", b"HiBy Engine Build Aug 15 2025\x00",
                       b"Firmware Built : Feb 30 2026 18:49:34\x00",
                       b"Firmware Built : Mar 26 2026 25:49:34\x00",
                       b"Firmware Built : Mar 26 2026 18:49:34"):
            self.assertEqual(module.extract_base_build(binary), "")

    def test_ambiguous(self):
        self.assertEqual(module.extract_base_build(
            b"Firmware Built : Mar 26 2026 18:49:34\x00\x00"
            b"Firmware Built : Mar 27 2026 18:49:34\x00"), "")

    def test_actual_packer_substitution(self):
        script = (PACKER / "sonix_firmware_packer.sh").read_text()
        line = next(line for line in script.splitlines()
                    if line.strip().startswith('sed_file ') and 'base_build_version' in line)
        with tempfile.TemporaryDirectory() as tmp:
            info = Path(tmp) / "system-info.json"
            for stamp in ("2026-03-26 18:49:34", ""):
                info.write_text((PACKER / "assets/V1/usr/resource/sonix/components/system-info.json").read_text())
                subprocess.run(["bash", "-eu", "-c", '''
                    sed_file() { sed -E "$1" "$2" > "$2.tmp"; mv "$2.tmp" "$2"; }
                    JSON="$1"; BASE_BUILD="$2"
                    ''' + line, "test", str(info), stamp], check=True)
                data = json.loads(info.read_text())
                self.assertEqual(data["base_build_version"], stamp)
                self.assertEqual(data["build_version"], "")
                self.assertEqual(data["OS_version"], "beta")


if __name__ == "__main__":
    unittest.main()
