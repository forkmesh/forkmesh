#!/usr/bin/env python3

from pathlib import Path
import sys


PACKAGE = Path(__file__).resolve().parents[1]
SOURCE = PACKAGE.parents[2] / "app" / "public" / "assets" / "codemap"
EXTENSION = PACKAGE / "content" / "extension"
SHARED = (
    "codemap.js",
    "vendor/three-0.184.0/three.module.min.js",
    "vendor/three-0.184.0/three.core.min.js",
    "vendor/three-0.184.0/LICENSE",
)


def main(argv):
    check = "--check" in argv
    drifted = []
    for name in SHARED:
        source = (SOURCE / name).read_bytes()
        target = EXTENSION / name
        if target.is_file() and target.read_bytes() == source:
            continue
        drifted.append(target)
        if not check:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(source)
    for path in drifted:
        print(("out of date: " if check else "wrote ") + str(path.relative_to(PACKAGE)))
    return 1 if check and drifted else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
