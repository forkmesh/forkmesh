#!/usr/bin/env python3
"""Reject UTF-8 byte escapes inside Qt's UTF-16 QStringLiteral macro."""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
SOURCE_SUFFIXES = {".cpp", ".h", ".mm"}
SOURCE_ROOTS = (ROOT / "src", ROOT.parent / "marketplace" / "packages")
UTF8_LEAD_ESCAPE = re.compile(r"\\x(?:[c-f][0-9a-f])", re.IGNORECASE)


def _skip_quoted(source: str, offset: int, quote: str) -> int:
    offset += 1
    while offset < len(source):
        if source[offset] == "\\":
            offset += 2
        elif source[offset] == quote:
            return offset + 1
        else:
            offset += 1
    return offset


def malformed_qstring_literals(source: str):
    """Yield (line, call) for bad QStringLiteral calls outside comments."""
    offset = 0
    token = "QStringLiteral"
    while offset < len(source):
        if source.startswith("//", offset):
            newline = source.find("\n", offset + 2)
            offset = len(source) if newline < 0 else newline + 1
            continue
        if source.startswith("/*", offset):
            end = source.find("*/", offset + 2)
            offset = len(source) if end < 0 else end + 2
            continue
        if source[offset] in {'"', "'"}:
            offset = _skip_quoted(source, offset, source[offset])
            continue
        if not source.startswith(token, offset):
            offset += 1
            continue

        before = source[offset - 1] if offset else ""
        after_offset = offset + len(token)
        after = source[after_offset] if after_offset < len(source) else ""
        if (before.isalnum() or before == "_") or (after.isalnum() or after == "_"):
            offset += len(token)
            continue
        open_paren = after_offset
        while open_paren < len(source) and source[open_paren].isspace():
            open_paren += 1
        if open_paren >= len(source) or source[open_paren] != "(":
            offset += len(token)
            continue

        cursor = open_paren
        depth = 0
        while cursor < len(source):
            char = source[cursor]
            if char in {'"', "'"}:
                cursor = _skip_quoted(source, cursor, char)
                continue
            if source.startswith("//", cursor):
                newline = source.find("\n", cursor + 2)
                cursor = len(source) if newline < 0 else newline + 1
                continue
            if source.startswith("/*", cursor):
                end = source.find("*/", cursor + 2)
                cursor = len(source) if end < 0 else end + 2
                continue
            if char == "(":
                depth += 1
            elif char == ")":
                depth -= 1
                if depth == 0:
                    cursor += 1
                    break
            cursor += 1

        call = source[offset:cursor]
        if UTF8_LEAD_ESCAPE.search(call):
            yield source.count("\n", 0, offset) + 1, call.splitlines()[0]
        offset = max(cursor, offset + len(token))


def main() -> int:
    failures = []
    for source_root in SOURCE_ROOTS:
        for path in sorted(source_root.rglob("*")):
            if path.suffix not in SOURCE_SUFFIXES:
                continue
            source = path.read_text(encoding="utf-8")
            for line, call in malformed_qstring_literals(source):
                failures.append(
                    f"{path.relative_to(ROOT.parent)}:{line}: {call.strip()}"
                )
    if failures:
        print(
            "UTF-8 byte escapes are invalid inside QStringLiteral; use "
            "QString::fromUtf8 or a Unicode escape:",
            file=sys.stderr,
        )
        print("\n".join(failures), file=sys.stderr)
        return 1
    print("QString Unicode literal validation passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
