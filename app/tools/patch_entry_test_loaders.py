#!/usr/bin/env python3
"""Teach entry.py's test loaders about the lazily-split domain modules.

Most worker suites read entry.py as text and AST-load the defs they
exercise. Moving a domain into its own lazy module (see
extract_lazy_module.py) therefore breaks every suite that names a moved
symbol — which is what made these splits expensive enough to skip, even
though startup memory is what fails deploys with 10021.

This rewrites the single expression those suites share so the "entry
source" they compile is entry.py plus the split modules, which is what the
Worker actually runs. Idempotent.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TESTS = ROOT / "tests"
NEEDLE = 'ENTRY.read_text(encoding="utf-8")'
MARKER = "# entry.py + its lazily-split domain modules"


def replacement(modules: list[str]) -> str:
    # Split modules come FIRST so every def they contain is followed by
    # entry.py's remaining defs: suites slice with
    # `text[text.index(name):]` and cut at the next "\n\n\nasync def",
    # which must still find one for a def at a module's end.
    parts = [
        '(ENTRY.parent / "%s.py").read_text(encoding="utf-8")' % name
        for name in modules
    ] + [NEEDLE]
    # Separate with a blank-line block and end on one: suites slice these
    # sources with `text[text.index(name):]` then cut at the next "\n\n\n",
    # which must still terminate for a def sitting at a file boundary.
    joined = '\n    + "\\n\\n\\n"\n    + '.join(parts)
    return '(\n    %s\n    %s\n    + "\\n\\n\\n"\n)' % (MARKER, joined)


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    modules = argv[1:]
    for name in modules:
        if not (ROOT / "src" / ("%s.py" % name)).is_file():
            print("no such module: src/%s.py" % name, file=sys.stderr)
            return 1

    patched = []
    for path in sorted(TESTS.glob("test_*.py")):
        text = path.read_text(encoding="utf-8")
        if NEEDLE not in text or MARKER in text:
            continue
        # Only suites that actually reference a moved symbol need widening;
        # leaving the rest untouched keeps the diff honest.
        moved_names = set()
        for name in modules:
            source = (ROOT / "src" / ("%s.py" % name)).read_text(
                encoding="utf-8")
            for line in source.splitlines():
                if line.startswith(("def ", "async def ", "class ")):
                    moved_names.add(
                        line.split("(")[0].split()[-1].rstrip(":"))
        if not any(symbol in text for symbol in moved_names):
            continue
        path.write_text(
            text.replace(NEEDLE, replacement(modules), 1), encoding="utf-8")
        patched.append(path.name)

    print("patched %d loader(s): %s" % (len(patched), ", ".join(patched)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
