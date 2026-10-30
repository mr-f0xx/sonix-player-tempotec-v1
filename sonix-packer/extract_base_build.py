#!/usr/bin/env python3
"""Read the official player's Firmware Built stamp without executing its ELF.

Only this timestamp is retained, never the vendor binary or its credentials.
Unknown/ambiguous stamps produce an empty value rather than a guessed version.
"""
import datetime
import re
import sys
from pathlib import Path


STAMP = re.compile(
    rb"(?:^|\x00)Firmware Built\s*:\s*"
    rb"([A-Z][a-z]{2}\s+[0-9]{1,2}\s+[0-9]{4}\s+[0-9]{2}:[0-9]{2}:[0-9]{2})\x00"
)
MONTHS = "Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec".split()


def extract_base_build(data):
    stamps = set()
    for match in STAMP.finditer(data):
        month, day, year, time = match[1].decode("ascii").split()
        try:
            hour, minute, second = map(int, time.split(":"))
            date = datetime.datetime(int(year), MONTHS.index(month) + 1,
                                     int(day), hour, minute, second)
        except ValueError:
            continue
        stamps.add(date.isoformat(sep=" "))
    return stamps.pop() if len(stamps) == 1 else ""


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("Usage: extract_base_build.py STOCK_HIBY_PLAYER")
    print(extract_base_build(Path(sys.argv[1]).read_bytes()))
