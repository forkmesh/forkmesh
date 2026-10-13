#!/usr/bin/env python3
"""Run mw_refs over every forkmesh app source and collect the records.

    run_refs.py <build-dir> <mw_refs binary> <out-dir> [-j N] [--only substring]
                [--define NAME]... [--extra-source path]...

--define FORKMESH_WINDOW_TESTS with --extra-source tests/test_window_resize.cpp
reproduces the window-test target, whose test hooks use members the app
build never sees. Extra sources borrow the flags of an app source.

The build dir must be a configured desktop tree (ninja -t compdb works on a
configured tree without building it). The GCC precompiled header cannot be
read by clang, so the PCH's source header is force-included instead.
"""
import argparse
import concurrent.futures
import json
import os
import shlex
import subprocess
import sys


def clang_args(entry, pch_header):
    argv = shlex.split(entry["command"])
    # Drop the launcher (ccache and its key=value options) and the compiler.
    while argv and (os.path.basename(argv[0]) == "ccache" or "=" in argv[0]):
        argv.pop(0)
    argv.pop(0)
    out = []
    skip = 0
    for i, arg in enumerate(argv):
        if skip:
            skip -= 1
            continue
        if arg in ("-o", "-MT", "-MF"):
            skip = 1
            continue
        if arg in ("-c", "-MD", "-Winvalid-pch") or arg == entry["file"]:
            continue
        if arg == "-include" and argv[i + 1].endswith("cmake_pch.hxx"):
            out += ["-include", pch_header]
            skip = 1
            continue
        out.append(arg)
    return ["-Wno-unknown-warning-option", "-Wno-everything"] + out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("build")
    ap.add_argument("tool")
    ap.add_argument("out")
    ap.add_argument("-j", type=int, default=8)
    ap.add_argument("--only", default="")
    ap.add_argument("--define", action="append", default=[])
    ap.add_argument("--extra-source", action="append", default=[])
    a = ap.parse_args()
    entries = json.load(open(os.path.join(a.build, "compile_commands.json")))
    app = [
        e for e in entries
        if "/forkmesh.dir/" in e.get("output", "")
        and e["file"].endswith(".cpp")
        and "/.qt/" not in e["file"]
        and "/moc_" not in e["file"]
        and a.only in e["file"]
    ]
    src = next(
        os.path.dirname(e["file"]) for e in app if "/desktop/src/" in e["file"]
    )
    pch_header = os.path.join(src, "MainWindowInternal.h")
    template = next(e for e in app if e["file"].endswith("/MainWindow.cpp"))
    for extra in a.extra_source:
        path = os.path.realpath(extra)
        app.append(dict(template, file=path,
                        command=template["command"].replace(template["file"], path)))
    os.makedirs(a.out, exist_ok=True)

    def run(e):
        name = os.path.basename(e["file"])
        rec = os.path.join(a.out, name + ".tsv")
        err = os.path.join(a.out, name + ".err")
        with open(rec, "w") as fo, open(err, "w") as fe:
            p = subprocess.run(
                [a.tool, e["file"]] + clang_args(e, pch_header)
                + ["-D" + d for d in a.define],
                cwd=e["directory"], stdout=fo, stderr=fe,
            )
        return name, p.returncode

    failed = []
    with concurrent.futures.ThreadPoolExecutor(a.j) as pool:
        for name, rc in pool.map(run, app):
            if rc not in (0,):
                failed.append((name, rc))
    print(f"{len(app)} sources, {len(failed)} with parse errors")
    for name, rc in failed[:20]:
        print(" ", rc, name)
    return 0


if __name__ == "__main__":
    sys.exit(main())
