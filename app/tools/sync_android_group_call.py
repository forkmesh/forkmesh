#!/usr/bin/env python3

from __future__ import annotations

import sys
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[2]
PUBLIC = REPOSITORY / "app" / "public"
TARGET = REPOSITORY / "native" / "android" / "app" / "src" / "main" / "assets" / "group-call"
MODULES = (
    "chat-group-calls.js",
    "chat-group-call-crypto.js",
    "chat-call-e2ee.js",
    "chat-mlkem.js",
    "call-quality-meter.js",
    "fmm/fmm-call.js",
    "fmm/fmm-e2ee.js",
    "fmm/fmm-worker.js",
    "fmm/fmm-playout-worklet.js",
    "fmm/fmm-seal.js",
    "fmm/client.js",
    "fmm/wire.js",
    "fmm/cc.js",
    "fmm/fec.js",
    "fmm/frag.js",
    "fmm/hop.js",
    "fmm/jitter.js",
    "fmm/seqnum.js",
    "chat-call-facestream.js",
    "chat-call-face-template.js",
    "chat-call-face-geometry.js",
    "chat-call-face-expression.js",
    "chat-call-face-custom.js",
    "chat-call-faces.js",
    "chat-call-face-render.js",
    "chat-call-face-mesh.js",
    "chat-call-face-tracker.js",
    "chat-call-segment.js",
    "chat-call-outfits.js",
    "chat-call-backgrounds.js",
)
HAND_WRITTEN = ("index.html", "group-call-host.js")


def main(argv: list[str]) -> int:
    check = "--check" in argv
    stale = []
    for name in MODULES:
        source = (PUBLIC / name).read_bytes()
        target = TARGET / name
        if target.exists() and target.read_bytes() == source:
            continue
        stale.append(target)
        if not check:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(source)
    known = set(MODULES) | set(HAND_WRITTEN)
    extra = sorted(
        path for path in TARGET.rglob("*")
        if path.is_file() and path.relative_to(TARGET).as_posix() not in known
    ) if TARGET.exists() else []
    for path in stale:
        print(("stale: " if check else "updated: ") + str(path.relative_to(REPOSITORY)))
    for path in extra:
        print("not a group call module: " + str(path.relative_to(REPOSITORY)))
    return 1 if (check and stale) or extra else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
