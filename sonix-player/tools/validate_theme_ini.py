#!/usr/bin/env python3
"""Mirror of load_custom() in sonix-player/src/gui/shell/theme.c, used to
verify the example palettes parse exactly as the firmware would."""

import sys

KEYS = ["screen_bg", "surface", "surface_pressed", "panel", "cover_bg",
        "text_primary", "text_secondary", "accent"]


def hex_color(s: str) -> bool:
    if len(s) != 7 or s[0] != "#":
        return False
    return all(c in "0123456789abcdefABCDEF" for c in s[1:])


def load_custom(path: str) -> bool:
    with open(path, "rb") as f:
        data = f.read()
    if len(data) > 4096:
        return False
    # Emulate fgets(line, 256): 255 bytes (or up to newline) per read.
    text = data.decode("utf-8", errors="replace")
    lines, i = [], 0
    while i < len(text):
        j = text.find("\n", i, i + 255)
        if j == -1:
            lines.append(text[i:i + 255])  # chunk with no newline in buffer
            i += 255
        else:
            lines.append(text[i:j + 1])
            i = j + 1

    seen = 0
    section = False
    for n, raw in enumerate(lines):
        line = raw
        # if (!strchr(line, '\n') && !feof(f)) -> invalid (chunk never terminated)
        if "\n" not in line and i < len(text):
            return False
        if "\n" not in line and not (n == len(lines) - 1):
            return False
        key = line.strip()
        if not key or key[0] in ";#":
            continue
        if key == "[theme]" and not section:
            section = True
            continue
        eq = key.find("=")
        if not section or eq == -1:
            return False
        name, value = key[:eq].strip(), key[eq + 1:].strip()
        try:
            idx = KEYS.index(name)
        except ValueError:
            return False
        if (seen & (1 << idx)) or not hex_color(value):
            return False
        seen |= 1 << idx
    return seen == 0xFF


def main() -> int:
    ok = True
    for path in sys.argv[1:]:
        good = load_custom(path)
        print(f"{'PASS' if good else 'FAIL'}  {path}")
        ok &= good
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
