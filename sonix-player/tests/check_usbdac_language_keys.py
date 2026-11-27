#!/usr/bin/env python3
"""Keep the DAC page's labels in all shipped language files."""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "sonix-player/src/gui/audio/dacpage.c"
LANG_DIR = ROOT / "sonix-packer/assets/V1/usr/resource/sonix/language"
MAX_ENTRIES = 2304  # sonix-player/src/system/core/lang.c

# Every `dac...` tag the page asks for: the mode and low-latency rows, the
# pass-through charging row and its note, the status lines, the confirmation and
# the two popups. A key missing from one language shows up on the device as the
# bare tag, and a key left behind by a rename is a line no reader ever sees.
used = set(re.findall(r'"(dac(?:_[A-Za-z0-9_]+)?)"', SOURCE.read_text(encoding="utf-8")))
if not used:
    sys.exit("no DAC page translation keys found in dacpage.c")

files = sorted(LANG_DIR.glob("*.ini"))
if len(files) != 11:
    sys.exit(f"expected 11 language files, found {len(files)} in {LANG_DIR}")

for path in files:
    entries = []
    found = set()
    for lineno, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip() or line.lstrip().startswith(("#", ";")) or "=" not in line:
            continue
        key, value = line.split("=", 1)
        key, value = key.strip(), value.strip()
        entries.append(key)
        if re.fullmatch(r"dac(?:_[A-Za-z0-9_]+)?", key):
            if key in found:
                sys.exit(f"{path.name}:{lineno}: duplicate key {key}")
            if not value:
                sys.exit(f"{path.name}:{lineno}: empty value for {key}")
            found.add(key)
    if len(entries) > MAX_ENTRIES:
        sys.exit(f"{path.name}: {len(entries)} entries exceeds MAX_ENTRIES {MAX_ENTRIES}")
    missing = sorted(used - found)
    extra = sorted(found - used)
    if missing or extra:
        sys.exit(f"{path.name}: missing={missing} stale={extra}")
    print(f"{path.name}: {len(entries)} entries, {len(found)} DAC page keys")

print(f"PASS DAC page translations ({len(used)} keys, {len(files)} languages)")
