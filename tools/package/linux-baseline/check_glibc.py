#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

NEED_RE = re.compile(r"Name:\s+(GLIBC|GLIBCXX|CXXABI)_([0-9][0-9.]*)\b")
ELF_MAGIC = b"\x7fELF"


def parse_version(text: str) -> tuple[int, ...]:
    return tuple(int(part) for part in text.split(".") if part.isdigit())


def needed_versions(readelf_output: str) -> dict[str, tuple[int, ...]]:
    highest: dict[str, tuple[int, ...]] = {}
    section = False
    for line in readelf_output.splitlines():
        if line.startswith("Version needs section"):
            section = True
            continue
        if section and line.startswith(("Version definition section", "Version symbols section")):
            section = False
        if not section:
            continue
        match = NEED_RE.search(line)
        if match:
            family, version = match.group(1), parse_version(match.group(2))
            if version > highest.get(family, ()):
                highest[family] = version
    return highest


def is_elf(path: Path) -> bool:
    try:
        with path.open("rb") as handle:
            return handle.read(4) == ELF_MAGIC
    except OSError:
        return False


def is_appimage(path: Path) -> bool:
    try:
        with path.open("rb") as handle:
            head = handle.read(11)
    except OSError:
        return False
    return head[:4] == ELF_MAGIC and head[8:11] == b"AI\x02"


def readelf(path: Path) -> str:
    result = subprocess.run(["readelf", "-V", "-W", str(path)], capture_output=True,
                            text=True, check=False)
    return result.stdout


def elf_files(root: Path):
    for dirpath, _dirs, files in os.walk(root):
        for name in files:
            candidate = Path(dirpath) / name
            if not candidate.is_symlink() and is_elf(candidate):
                yield candidate


def scan(paths: list[Path]) -> dict[str, dict[str, tuple[int, ...]]]:
    report: dict[str, dict[str, tuple[int, ...]]] = {}
    for path in paths:
        report[str(path)] = needed_versions(readelf(path))
        if is_appimage(path):
            with tempfile.TemporaryDirectory(prefix="forkmesh-appimage-scan-") as scratch:
                env = dict(os.environ, APPIMAGE_EXTRACT_AND_RUN="1")
                subprocess.run([str(path.resolve()), "--appimage-extract"], cwd=scratch, env=env,
                               capture_output=True, check=True)
                root = Path(scratch) / "squashfs-root"
                for inner in elf_files(root):
                    report[f"{path}!/{inner.relative_to(root)}"] = needed_versions(readelf(inner))
    return report


def violations(report: dict[str, dict[str, tuple[int, ...]]],
               limits: dict[str, tuple[int, ...]]) -> list[str]:
    found = []
    for name, versions in sorted(report.items()):
        for family, limit in limits.items():
            version = versions.get(family)
            if version and version > limit:
                found.append(f"{name} needs {family}_{'.'.join(map(str, version))} "
                             f"(baseline {family}_{'.'.join(map(str, limit))})")
    return found


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Fail when a Linux release binary needs a newer glibc or libstdc++ "
                    "than the supported baseline.")
    parser.add_argument("--glibc-max", required=True)
    parser.add_argument("--glibcxx-max", required=True)
    parser.add_argument("files", nargs="+")
    args = parser.parse_args(argv)
    if not shutil.which("readelf"):
        print("readelf (binutils) is required", file=sys.stderr)
        return 2
    paths = [Path(name) for name in args.files]
    for path in paths:
        if not is_elf(path):
            print(f"{path} is not an ELF file", file=sys.stderr)
            return 2
    report = scan(paths)
    limits = {"GLIBC": parse_version(args.glibc_max), "GLIBCXX": parse_version(args.glibcxx_max)}
    for path in paths:
        highest = report[str(path)]
        inner = [v for k, v in report.items() if k.startswith(f"{path}!/")]
        glibc = max([highest.get("GLIBC", ())] + [v.get("GLIBC", ()) for v in inner])
        print(f"{path}: highest GLIBC_{'.'.join(map(str, glibc)) or 'none'} "
              f"across {1 + len(inner)} ELF file(s)")
    problems = violations(report, limits)
    for problem in problems:
        print(f"FAIL {problem}", file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
