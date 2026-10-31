#!/usr/bin/env python3
"""Decide the official TempoTec release version an image is based on.

Two sources, in order:

1. The SHA-256 of the stock v1.upt. An update package is named by its
   release, but the package's own product metadata does not always carry it
   -- the supplied V1 1.3 archive still says "version":"1.2" in
   usr/resource/config.json -- so the known official packages are listed
   here by hash, each hash recorded next to its archive in
   firmware-input/README.md.

2. The product section of the stock rootfs's usr/resource/config.json (see
   extract_base_build.py), for any stock image that is not in the table.

Unknown or ambiguous metadata yields an empty value, which the About page
shows as a dash -- never a guessed version, never a date.
"""
import hashlib
import re
import sys
from pathlib import Path

from extract_base_build import extract_base_build

# Stock v1.upt SHA-256 -> official release label.
KNOWN_RELEASES = {
    # The supplied TempoTec V1 1.3 update (sonix-packer/firmware-input,
    # v1_upload.7z.001/.002). Its config.json product section still says 1.2.
    "9f929a92319a05a81bbd16cbf0fb8a886a9bb2def192f5a6786b6ea7032fa05d": "1.3",
}

VERSION_RE = re.compile(r"[0-9]+(?:\.[0-9]+)+")


def stock_release(upt_sha256, config_data):
    """Release label for a stock image: package hash first, product metadata
    as fallback. "" when neither source is unambiguous."""
    label = KNOWN_RELEASES.get(upt_sha256.lower())
    if label is not None and VERSION_RE.fullmatch(label) and len(label) < 64:
        return label
    return extract_base_build(config_data)


def main(argv):
    if len(argv) != 3:
        sys.exit("Usage: stock_release.py STOCK_UPT STOCK_CONFIG_JSON")
    upt, config = Path(argv[1]), Path(argv[2])
    digest = hashlib.sha256(upt.read_bytes()).hexdigest() if upt.is_file() else ""
    config_data = config.read_bytes() if config.is_file() else b""
    print(stock_release(digest, config_data))


if __name__ == "__main__":
    main(sys.argv)
