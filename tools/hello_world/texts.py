#!/usr/bin/env python3
"""Write the hello-world mission text file (a modified `Texts/Convoy.fli`).

    texts.py <donor Convoy.fli> <new .fli>

A `.fli` is UTF-16 LE text with a BOM and CRLF lines: an id line, a quoted
string, a blank line. The donor's entries are kept (kept scripts may still
name them) and the hello-world strings are appended with ids from 0900.
"""
import sys

# Replaced donor entries (the objective label reuses a donor id) and new ones.
REPLACE: dict[str, str] = {}
STRINGS = {
    "0900": "Reach the building.",
    "0901": "Kill the officer.",
    "0902": "Building reached.",
    "0903": "The officer is dead.",
    "0904": "Sabotage the camp radio.",
    "0905": "Radio sabotaged.",
    "0906": "Sabotage radio",
}


def main() -> int:
    donor, out = sys.argv[1], sys.argv[2]
    text = open(donor, encoding="utf-16").read().replace("\r\n", "\n")
    lines = text.split("\n")
    for i, line in enumerate(lines[:-1]):
        if line.strip() in REPLACE:
            lines[i + 1] = '"' + REPLACE[line.strip()] + '"'
    text = "\n".join(lines)
    ids = {line.strip() for line in text.split("\n") if line.strip().isdigit()}
    clash = ids & STRINGS.keys()
    if clash:
        raise SystemExit(f"donor already uses ids {sorted(clash)}")
    text = text.rstrip("\n") + "\n\n" + "".join(f'{key}\n"{value}"\n\n' for key, value in STRINGS.items())
    with open(out, "w", encoding="utf-16-le", newline="") as handle:
        handle.write("﻿" + text.replace("\n", "\r\n"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
