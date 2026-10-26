#!/usr/bin/env python3
"""Regenerate the MainWindowInternal.h split in one step.

    split_internal_header.py <configured-build-dir> [--parts 4]

Run from the repository root. It builds the two libclang helpers into the
build dir, lists every namespace-scope function MainWindowInternal.h still
defines inline, and moves the movable ones into
desktop/src/MainWindowInternal<N>.cpp, appending to whatever those files
already hold. Use it after a merge brought new inline helpers into the header,
or to redo the split from a header restored with `git checkout`.
Needs clang-19's libclang (/usr/lib/llvm-19).
"""
import argparse
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from run_refs import clang_args  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("build")
    ap.add_argument("--parts", type=int, default=4)
    a = ap.parse_args()
    repo = os.getcwd()
    src = os.path.join(repo, "desktop/src")
    header = os.path.join(src, "MainWindowInternal.h")
    tool = os.path.join(a.build, "inline_defs")
    subprocess.check_call([
        "gcc", "-O2", "-o", tool, os.path.join(HERE, "inline_defs.c"),
        "-I/usr/lib/llvm-19/include", "-L/usr/lib/llvm-19/lib", "-lclang-19",
        "-Wl,-rpath,/usr/lib/llvm-19/lib"])
    subprocess.check_call(["ninja", "-C", a.build, "-t", "compdb"],
                          stdout=open(os.path.join(a.build, "compile_commands.json"), "w"))
    entries = json.load(open(os.path.join(a.build, "compile_commands.json")))
    tmpl = next(e for e in entries if e["file"].endswith("/desktop/src/MainWindowPackageChrome.cpp")
                and "/forkmesh.dir/" in e.get("output", ""))
    args = [x for x in clang_args(tmpl, "/dev/null") if x not in ("-include", "/dev/null")]
    probe = os.path.join(a.build, "inline_defs_probe.cpp")
    open(probe, "w").write('#include "MainWindowInternal.h"\n')
    tsv = os.path.join(a.build, "inline_defs.tsv")
    with open(tsv, "w") as out:
        rc = subprocess.run([tool, header, probe] + args + ["-I" + src],
                            cwd=tmpl["directory"], stdout=out).returncode
    if rc != 0:
        sys.exit(f"inline_defs could not parse the header (exit {rc})")
    subprocess.check_call([sys.executable, os.path.join(HERE, "move_inline_defs.py"),
                           header, tsv, os.path.join(src, "MainWindowInternal"),
                           str(a.parts)])


if __name__ == "__main__":
    main()
