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
    @staticmethod
    def product(version="1.2", **changes):
        product = {"type": "product", "company": "TempoTec", "device": "V1",
                   "version": version}
        product.update(changes)
        return product

    def test_release_version(self):
        for version in ("1.2", "1.3", "1.3.1"):
            config = [{"type": "slef", "version": 1}, self.product(version)]
            self.assertEqual(module.extract_base_build(json.dumps(config)), version)

    def test_unknown_or_invalid(self):
        for data in (b"", b"not JSON", b"{}", b"[]", b"[null,1]",
                     b"[{", b"\xff", b"Firmware Built : Mar 26 2026 18:49:34"):
            self.assertEqual(module.extract_base_build(data), "")
        for version in (None, 1.2, "", "2026-03-26 18:49:34", "1.2&", "1.2\n", "1." * 40):
            self.assertEqual(module.extract_base_build(json.dumps([self.product(version)])), "")

    def test_wrong_product_or_ambiguous(self):
        for products in ([self.product(company="HiBy")],
                         [self.product(device="V3")],
                         [self.product(), self.product("1.3")],
                         [self.product(), self.product()]):
            self.assertEqual(module.extract_base_build(json.dumps(products)), "")

    def test_actual_packer_substitution(self):
        script = (PACKER / "sonix_firmware_packer.sh").read_text()
        line = next(line for line in script.splitlines()
                    if line.strip().startswith('sed_file ') and 'base_build_version' in line)
        with tempfile.TemporaryDirectory() as tmp:
            info = Path(tmp) / "system-info.json"
            for stamp in ("1.2", "1.3", ""):
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
