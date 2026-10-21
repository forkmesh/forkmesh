#!/usr/bin/env python3
"""Write brotli and gzip copies of a built asset tree's text files.

The self-hosted app serves app/dist itself (forkmesh_host, behind a reverse
proxy that would otherwise gzip every response on the fly at a low level).
For each script, stylesheet, SVG, JSON or WebAssembly file of at least
``MIN_BYTES``, this writes ``<file>.br`` (brotli quality 11) and
``<file>.gz`` (gzip level 9) beside it; ``bindings.StaticAssets`` answers
with one of them when the request's Accept-Encoding allows it
(app/host/forkmesh_host/bindings.py ``precompressed_variant``).

A copy is written only when it is smaller than the file, and is skipped when
it is already newer than the file, so running this again after a later build
step (the World stage, the instance brand) only compresses what changed. A
file rewritten after its copies were made is served uncompressed by the host
until this runs again: the host never serves a copy older than its file.

Brotli comes from Node's zlib (Node is already a build dependency); without
Node only the gzip copies are written.

    python3 tools/precompress_assets.py [dist ...]
"""

from __future__ import annotations

import gzip
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

APP = Path(__file__).resolve().parents[1]
SUFFIXES = frozenset({".js", ".mjs", ".css", ".svg", ".json", ".wasm"})
MIN_BYTES = 1024
# The dashboard bundle's source fragments ride along with app/public but no
# page loads them (dashboard.js is their concatenation).
SKIPPED_PREFIXES = ("dashboard/js/",)

# Node's zlib does brotli without a Python dependency. Reads a JSON list of
# [source, target] pairs on stdin; quality 11.
BROTLI_SCRIPT = r"""
const fs = require("fs");
const zlib = require("zlib");
const jobs = JSON.parse(fs.readFileSync(0, "utf8"));
for (const [source, target] of jobs) {
  const data = fs.readFileSync(source);
  const packed = zlib.brotliCompressSync(data, { params: {
    [zlib.constants.BROTLI_PARAM_QUALITY]: 11,
    [zlib.constants.BROTLI_PARAM_SIZE_HINT]: data.length,
  } });
  if (packed.length < data.length) {
    fs.writeFileSync(target + ".tmp", packed);
    fs.renameSync(target + ".tmp", target);
  }
}
"""


def candidates(root: Path):
    for path in sorted(root.rglob("*")):
        if (path.suffix in SUFFIXES and path.is_file()
                and not path.is_symlink()
                and not path.relative_to(root).as_posix().startswith(
                    SKIPPED_PREFIXES)
                and path.stat().st_size >= MIN_BYTES):
            yield path


def fresh(source: Path, copy: Path) -> bool:
    try:
        return copy.stat().st_mtime_ns >= source.stat().st_mtime_ns
    except OSError:
        return False


def write_gzip(source: Path) -> bool:
    data = source.read_bytes()
    packed = gzip.compress(data, compresslevel=9, mtime=0)
    target = source.with_name(source.name + ".gz")
    if len(packed) >= len(data):
        target.unlink(missing_ok=True)
        return False
    temporary = target.with_name(target.name + ".tmp")
    temporary.write_bytes(packed)
    os.replace(temporary, target)
    return True


def node_binary() -> str | None:
    configured = os.environ.get("FORKMESH_NODE", "")
    return configured or shutil.which("node")


def write_brotli(sources: list[Path]) -> int:
    if not sources:
        return 0
    node = node_binary()
    if node is None:
        print("precompress: node is missing; writing gzip copies only",
              file=sys.stderr)
        return 0
    jobs = [[str(path), str(path.with_name(path.name + ".br"))]
            for path in sources]
    subprocess.run([node, "-e", BROTLI_SCRIPT], input=json.dumps(jobs),
                   text=True, check=True)
    return sum(path.with_name(path.name + ".br").is_file() for path in sources)


def precompress(root: Path) -> dict[str, int]:
    root = Path(root)
    gz = []
    br = []
    for path in candidates(root):
        if not fresh(path, path.with_name(path.name + ".gz")):
            gz.append(path)
        if not fresh(path, path.with_name(path.name + ".br")):
            br.append(path)
    written = sum(write_gzip(path) for path in gz)
    return {"gzip": written, "brotli": write_brotli(br)}


def main(argv: list[str]) -> int:
    roots = [Path(arg) for arg in argv[1:]] or [APP / "dist"]
    for root in roots:
        if not root.is_dir():
            print("precompress: no such directory: %s" % root, file=sys.stderr)
            return 2
        counts = precompress(root)
        print("precompressed %s: %d brotli, %d gzip"
              % (root, counts["brotli"], counts["gzip"]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
