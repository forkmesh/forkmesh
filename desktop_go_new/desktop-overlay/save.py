#!/usr/bin/env python3
import difflib
import os
import shutil
import sys
from pathlib import Path

OVERLAY = Path(__file__).resolve().parent
REPO = OVERLAY.parent.parent
ROOT = Path(os.environ.get("FORKMESH_GO_NEW_BUILD_ROOT", Path.home() / "projects/forkmesh/builds/desktop-go-new"))
STAGE = ROOT / "stage"
AREAS = {"10-desktop.patch": ["desktop"], "20-marketplace.patch": ["marketplace"]}
SKIP = {"build", "__pycache__"}


def files(base, area):
    top = base / area
    for dirpath, dirnames, filenames in os.walk(top):
        dirnames[:] = [d for d in dirnames if d not in SKIP and not d.startswith("build-")]
        for name in filenames:
            path = Path(dirpath) / name
            yield path.relative_to(base).as_posix()


def read(path):
    try:
        return path.read_text(encoding="utf-8").splitlines(keepends=True)
    except UnicodeDecodeError:
        return None


def main():
    if not STAGE.is_dir():
        sys.exit(f"no stage at {STAGE}; run stage.sh first")
    for patch_name, areas in AREAS.items():
        chunks = []
        for area in areas:
            for rel in sorted(files(STAGE, area)):
                staged = STAGE / rel
                original = REPO / rel
                if not original.exists():
                    target = OVERLAY / "files" / rel
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(staged, target)
                    continue
                if staged.read_bytes() == original.read_bytes():
                    continue
                old, new = read(original), read(staged)
                if old is None or new is None:
                    sys.exit(f"binary change in {rel}; overlay patches carry text only")
                chunks.append(f"diff --git a/{rel} b/{rel}\n")
                chunks.extend(difflib.unified_diff(old, new, f"a/{rel}", f"b/{rel}"))
        (OVERLAY / "patches" / patch_name).write_text("".join(chunks), encoding="utf-8")
        print(f"{patch_name}: {sum(1 for c in chunks if c.startswith('diff --git'))} files")


if __name__ == "__main__":
    main()
