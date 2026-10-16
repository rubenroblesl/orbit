#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
#
# Checks that every English text in macos/Launcher has a Spanish entry in
# macos/Localization/es.lproj/Localizable.strings, and that the table has no stale entries.
# Interpolations (\(...)) and format specifiers (%@, %lld, %1$@...) compare as one placeholder.
#
#   macos/tools/check_localization.py

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
CALL = re.compile(r'\b(?:L|Text|Button|Toggle|Picker|LabeledContent|TableColumn|confirmationDialog|Label)\(\s*"')


def literal(source, start):
    """The string literal that starts after the quote at `start`, with nested interpolations."""
    out, i, depth = [], start, 0
    while i < len(source):
        ch = source[i]
        if depth == 0 and ch == "\\" and source[i + 1] == "(":
            out.append(source[i:i + 2])
            depth, i = 1, i + 2
            continue
        if depth == 0 and ch == "\\":
            out.append(source[i:i + 2])
            i += 2
            continue
        if depth == 0 and ch == '"':
            return "".join(out)
        if depth:
            depth += {"(": 1, ")": -1}.get(ch, 0)
        out.append(ch)
        i += 1
    return "".join(out)


def interpolations(text):
    out, depth, i = [], 0, 0
    while i < len(text):
        if text.startswith("\\(", i) and depth == 0:
            depth, i = 1, i + 2
            out.append("§")
            continue
        if depth:
            depth += {"(": 1, ")": -1}.get(text[i], 0)
        else:
            out.append(text[i])
        i += 1
    return "".join(out)


def normalize(text):
    text = interpolations(text).replace("\\n", "\n").replace('\\"', '"')
    return re.sub(r"%(\d+\$)?(lld|ld|d|@|lf|f)", "§", text)


def main():
    used = set()
    for path in sorted((ROOT / "Launcher").glob("*.swift")):
        if path.name == "Entry.swift":
            continue
        source = path.read_text()
        for match in CALL.finditer(source):
            used.add(normalize(literal(source, match.end())))
    spanish = parse_strings(ROOT / "Localization/es.lproj/Localizable.strings")
    translated = {normalize(key) for key in spanish}
    missing = sorted(used - translated)
    stale = sorted(translated - used)
    for text in missing:
        print(f"missing Spanish text: {text!r}")
    for text in stale:
        print(f"unused Spanish entry: {text!r}")
    return 1 if missing or stale else 0


def parse_strings(path):
    entries = {}
    pattern = re.compile(r'"((?:[^"\\]|\\.)*)"\s*=\s*"((?:[^"\\]|\\.)*)"\s*;')
    for key, value in pattern.findall(path.read_text()):
        entries[key] = value
    return entries


if __name__ == "__main__":
    sys.exit(main())
