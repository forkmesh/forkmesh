#!/usr/bin/env python3
from __future__ import annotations

import argparse
import bisect
import gzip
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

TEXT_BASE = 0x100000000
APP_FRAME = re.compile(r"ForkMesh \+0x([0-9a-fA-F]+)")


def read_symbols(path: Path) -> tuple[list[int], list[str]]:
    opener = gzip.open if path.suffix == ".gz" else open
    rows = []
    with opener(path, "rt", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            parts = line.split(None, 2)
            if len(parts) != 3 or parts[1] not in ("t", "T"):
                continue
            try:
                rows.append((int(parts[0], 16), parts[2].strip()))
            except ValueError:
                continue
    rows.sort()
    return [address for address, _ in rows], [name for _, name in rows]


def locate(addresses: list[int], names: list[str], offset: int, base: int = TEXT_BASE) -> tuple[str, int] | None:
    target = base + offset
    index = bisect.bisect_right(addresses, target) - 1
    if index < 0:
        return None
    return names[index], target - addresses[index]


def demangler() -> list[str] | None:
    found = shutil.which("swift-demangle")
    if found:
        return [found, "--simplified"]
    bundled = Path.home() / ".forkmesh" / "toolchain" / "swift" / "usr" / "bin" / "swift-demangle"
    if bundled.exists():
        return [str(bundled), "--simplified"]
    found = shutil.which("swift")
    return [found, "demangle", "--simplified"] if found else None


def demangle(names: list[str]) -> list[str]:
    command = demangler()
    if not command or not names:
        return names
    stripped = [name[1:] if name.startswith("_$s") else name for name in names]
    result = subprocess.run(command, input="\n".join(stripped) + "\n", capture_output=True, text=True)
    lines = result.stdout.splitlines()
    return lines if result.returncode == 0 and len(lines) == len(names) else names


def offsets_from(values: list[str], text: str) -> list[int]:
    found = [int(value.lstrip("+"), 16) for value in values]
    found += [int(match, 16) for match in APP_FRAME.findall(text)]
    return list(dict.fromkeys(found))


def describe(symbols: Path, offsets: list[int], base: int = TEXT_BASE, readable: bool = True) -> list[str]:
    addresses, names = read_symbols(symbols)
    located = [locate(addresses, names, offset, base) for offset in offsets]
    shown = demangle([hit[0] for hit in located if hit]) if readable else [hit[0] for hit in located if hit]
    lines = []
    for offset, hit in zip(offsets, located):
        if hit is None:
            lines.append("+0x%x  (before the first symbol)" % offset)
            continue
        name = shown.pop(0)
        lines.append("+0x%x  %s + 0x%x" % (offset, name, hit[1]))
    return lines


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Name the ForkMesh +0x<offset> frames of an iOS performance report "
        "from the build's -symbols.txt.gz (nm -n of its dSYM).")
    parser.add_argument("symbols", type=Path, help="ForkMesh-native-<version>-<build>-symbols.txt.gz")
    parser.add_argument("offsets", nargs="*", help="offsets such as +0x2c240; with none, read a report on stdin")
    parser.add_argument("--base", type=lambda value: int(value, 16), default=TEXT_BASE,
                        help="__TEXT load address in the symbol file (default 0x100000000)")
    parser.add_argument("--mangled", action="store_true", help="print Swift names without demangling")
    args = parser.parse_args(argv)
    text = "" if args.offsets or sys.stdin.isatty() else sys.stdin.read()
    offsets = offsets_from(args.offsets, text)
    if not offsets:
        parser.error("no offsets given and no 'ForkMesh +0x...' frames on stdin")
    for line in describe(args.symbols, offsets, args.base, not args.mangled):
        print(line)
    return 0


if __name__ == "__main__":
    os.environ.setdefault("PYTHONIOENCODING", "utf-8")
    sys.exit(main())
