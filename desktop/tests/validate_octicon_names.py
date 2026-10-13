#!/usr/bin/env python3
"""Fail when the UI asks for an icon the app does not ship.

`tintedOcticonPixmap` silently returns an empty pixmap for an unknown name, so
a typo — or a GitHub octicon that was never vendored into resources/icons —
costs a button its icon with no warning at build or run time. That is how the
Tasks detail pane ended up with unlabelled "Assign Bot", "Complete" and "Send
to QA" buttons next to iconed neighbours.

The checker reads the names the source passes to the octicon helpers, plus the
names it passes through local helpers that take an `icon` parameter (the Tasks
pane builds its action buttons that way), and asserts each one is listed in
resources/icons.qrc.
"""

from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
QRC = ROOT / "resources" / "icons.qrc"

# helper name -> index of the argument that carries the icon name
DIRECT_HELPERS = {
    "setOcticon": 1,
    "setLabelOcticon": 1,
    "setTabOcticon": 2,
    "themedOcticon": 0,
    "tintedOcticonPixmap": 0,
    "rotatedTintedOcticonPixmap": 0,
    "octiconMarkup": 0,
}

# Helpers are declared with an icon-shaped parameter too; checking their own
# bodies would only re-report their parameter names.
HELPER_DEFINITION_NAMES = set(DIRECT_HELPERS) | {
    "applyStoredOcticon",
    "applyStoredLabelOcticon",
    "refreshTabOcticons",
    # Takes a freedesktop tray icon name, not one of ours.
    "postNotification",
}

# An icon argument is often a ternary that switches on some other string. Those
# comparison operands are not icon names, and they are written as QLatin1String
# or sit next to an equality operator.
COMPARISON = re.compile(r'QLatin1String\s*\(\s*"[^"]*"\s*\)|[=!]=\s*"[^"]*"'
                        r'|"[^"]*"\s*[=!]=')

ICON_NAME = re.compile(r"^[a-z][a-z0-9]*(?:-[a-z0-9]+)*$")
STRING_LITERAL = re.compile(r'"([^"\\]*)"')


def shipped_icons() -> set[str]:
    text = QRC.read_text(encoding="utf-8")
    return {name for name in re.findall(r'alias="([^"]+)\.svg"', text)}


def split_arguments(args: str) -> list[str]:
    """Split a call's argument text on the commas that separate arguments."""
    parts: list[str] = []
    depth = 0
    current = ""
    in_string = False
    escaped = False
    for ch in args:
        if in_string:
            current += ch
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == '"':
                in_string = False
            continue
        if ch == '"':
            in_string = True
            current += ch
        elif ch in "([{<":
            depth += 1
            current += ch
        elif ch in ")]}>":
            depth -= 1
            current += ch
        elif ch == "," and depth == 0:
            parts.append(current)
            current = ""
        else:
            current += ch
    parts.append(current)
    return parts


def call_arguments(text: str, open_paren: int) -> tuple[str, int] | None:
    """Return the argument text of the call whose '(' is at `open_paren`."""
    depth = 0
    in_string = False
    escaped = False
    for index in range(open_paren, len(text)):
        ch = text[index]
        if in_string:
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == '"':
                in_string = False
            continue
        if ch == '"':
            in_string = True
        elif ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                return text[open_paren + 1:index], index
        elif ch == ";" and depth == 0:
            return None
    return None


def find_calls(text: str, name: str):
    """Yield the argument text of every `name(...)` call in `text`."""
    for match in re.finditer(r"\b" + re.escape(name) + r"\s*\(", text):
        # A definition or declaration, not a call.
        head = text[max(0, match.start() - 40):match.start()]
        if re.search(r"\b(?:inline|static|void|QIcon|QPixmap|QString|auto)\s+$", head):
            continue
        found = call_arguments(text, match.end() - 1)
        if found:
            yield found[0]


def indirect_helpers(text: str) -> dict[str, int]:
    """Locals such as the Tasks pane's `addAction` that forward an icon name."""
    helpers: dict[str, int] = {}
    for match in re.finditer(r"(\w+)\s*(?:=\s*\[[^\]\n]*\]\s*)?\(", text):
        name = match.group(1)
        if name in HELPER_DEFINITION_NAMES or name in helpers:
            continue
        found = call_arguments(text, match.end() - 1)
        if not found:
            continue
        params, close = found
        if not re.match(r"\s*(?:const\s*)?\{|\s*\{", text[close + 1:close + 12]):
            continue
        for index, param in enumerate(split_arguments(params)):
            if re.search(r"QString\s*&\s*(?:icon|iconName)\b", param):
                helpers[name] = index
                break
    return helpers


def literals(argument: str) -> list[str]:
    argument = COMPARISON.sub(" ", argument)
    return [value for value in STRING_LITERAL.findall(argument)
            if ICON_NAME.match(value)]


def main() -> int:
    have = shipped_icons()
    problems: list[str] = []
    for path in sorted((ROOT / "src").glob("*.cpp")) + sorted((ROOT / "src").glob("*.h")):
        text = path.read_text(encoding="utf-8", errors="replace")
        wanted = dict(DIRECT_HELPERS)
        wanted.update(indirect_helpers(text))
        for helper, position in wanted.items():
            for args in find_calls(text, helper):
                arguments = split_arguments(args)
                if position >= len(arguments):
                    continue
                for name in literals(arguments[position]):
                    if name not in have:
                        problems.append(
                            f"{path.relative_to(ROOT)}: {helper}(...) asks for "
                            f'the icon "{name}", which resources/icons.qrc does '
                            "not ship")
    if problems:
        print("Missing icons — these draw as blank space in the UI:")
        for problem in sorted(set(problems)):
            print("  " + problem)
        return 1
    print(f"octicon names OK ({len(have)} icons shipped)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
