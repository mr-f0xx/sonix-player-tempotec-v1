#!/usr/bin/env python3
"""Host checks for stock build extraction and packer metadata stamping."""
import importlib.util
import json
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
PACKER = ROOT / "sonix-packer"
sys.path.insert(0, str(PACKER))
spec = importlib.util.spec_from_file_location("base_build", PACKER / "extract_base_build.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
spec = importlib.util.spec_from_file_location("stock_release", PACKER / "stock_release.py")
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


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


class StockReleaseTest(unittest.TestCase):
    # The hash of the supplied V1 1.3 archive, as recorded in
    # sonix-packer/firmware-input/README.md.
    V13_SHA256 = "9f929a92319a05a81bbd16cbf0fb8a886a9bb2def192f5a6786b6ea7032fa05d"

    @staticmethod
    def config(version="1.2"):
        return json.dumps([{"type": "slef", "version": 1},
                           {"type": "product", "company": "TempoTec",
                            "device": "V1", "version": version}]).encode()

    def test_known_package_wins_over_product_metadata(self):
        # The 1.3 package's own config.json still says 1.2; the package hash
        # is what names the release.
        self.assertEqual(release.stock_release(self.V13_SHA256, self.config("1.2")), "1.3")
        self.assertEqual(release.stock_release(self.V13_SHA256.upper(), self.config("1.2")), "1.3")
        self.assertEqual(release.stock_release(self.V13_SHA256, b""), "1.3")

    def test_unknown_package_falls_back_to_product_version(self):
        other = "0" * 64
        for version in ("1.2", "1.4.2"):
            self.assertEqual(release.stock_release(other, self.config(version)), version)
        for data in (b"", b"not JSON", b"[]"):
            self.assertEqual(release.stock_release(other, data), "")
        self.assertEqual(release.stock_release("", self.config("1.2")), "1.2")

    def test_documented_hash_is_known(self):
        readme = (PACKER / "firmware-input/README.md").read_text()
        documented = re.findall(r"`([0-9a-f]{64})`", readme)
        self.assertIn(self.V13_SHA256, documented)
        self.assertIn(self.V13_SHA256, release.KNOWN_RELEASES)
        for label in release.KNOWN_RELEASES.values():
            self.assertRegex(label, r"^[0-9]+(\.[0-9]+)+$")


if __name__ == "__main__":
    unittest.main()
