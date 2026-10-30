#!/usr/bin/env python3
"""Read the TempoTec V1 release version from stock usr/resource/config.json.

Use the product section, not the config schema version or bundled libraries.
Unknown, malformed or ambiguous metadata yields an empty value, never a date.
"""
import json
import re
import sys
from pathlib import Path


def extract_base_build(data):
    try:
        config = json.loads(data)
    except (ValueError, UnicodeDecodeError):
        return ""
    if not isinstance(config, list):
        return ""
    products = [entry for entry in config
                if isinstance(entry, dict) and entry.get("type") == "product"]
    if len(products) != 1:
        return ""
    product = products[0]
    if product.get("company") != "TempoTec" or product.get("device") != "V1":
        return ""
    version = product.get("version")
    if not isinstance(version, str) or not re.fullmatch(r"[0-9]+(?:\.[0-9]+)+", version):
        return ""
    return version if len(version) < 64 else ""


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("Usage: extract_base_build.py STOCK_CONFIG_JSON")
    path = Path(sys.argv[1])
    print(extract_base_build(path.read_bytes()) if path.is_file() else "")
