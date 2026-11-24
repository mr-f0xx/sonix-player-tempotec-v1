#!/usr/bin/env python3
"""Keep the SD card health page's translation keys in step with the UI.

The rest of the language files have a handful of historical keys that only
English or only Italiano carries, so a global "every file has every key" check
would fail for reasons unrelated to this page. Here the source of truth is the
set of `sd_health_*` literals used by the page and the System row that opens
it: every shipped language must have exactly those keys, with a non-empty
value, and the whole file must still fit lang.c's table.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCES = (
    ROOT / "sonix-player/src/gui/settings/sdhealthpage.c",
    ROOT / "sonix-player/src/gui/settings/systempage.c",
)
LANG_DIR = ROOT / "sonix-packer/assets/V1/usr/resource/sonix/language"
MAX_ENTRIES = 2304  # sonix-player/src/system/core/lang.c

USED = set()
for source in SOURCES:
    USED.update(re.findall(r'"(sd_health_[A-Za-z0-9_]+)"', source.read_text(encoding="utf-8")))
if not USED:
    sys.exit("no sd_health_* keys found in the page sources")

files = sorted(LANG_DIR.glob("*.ini"))
if len(files) != 11:
    sys.exit(f"expected 11 language files, found {len(files)} in {LANG_DIR}")

for path in files:
    entries = []
    keys = set()
    for lineno, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip() or line.lstrip().startswith(("#", ";")) or "=" not in line:
            continue
        key, value = line.split("=", 1)
        key, value = key.strip(), value.strip()
        entries.append(key)
        if key.startswith("sd_health_"):
            if key in keys:
                sys.exit(f"{path.name}:{lineno}: duplicate key {key}")
            if not value:
                sys.exit(f"{path.name}:{lineno}: empty value for {key}")
            keys.add(key)
    if len(entries) > MAX_ENTRIES:
        sys.exit(f"{path.name}: {len(entries)} entries exceeds MAX_ENTRIES {MAX_ENTRIES}")
    missing = sorted(USED - keys)
    extra = sorted(keys - USED)
    if missing or extra:
        sys.exit(f"{path.name}: missing={missing} stale={extra}")
    print(f"{path.name}: {len(entries)} entries, {len(keys)} SD health keys")

print(f"PASS SD card health translations ({len(USED)} keys, {len(files)} languages, cap {MAX_ENTRIES})")
