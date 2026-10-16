#!/usr/bin/env python3
import subprocess
import sys
from pathlib import Path

build = Path(sys.argv[1])
app = build / "forkmesh"


def defined(path):
    out = subprocess.run(["nm", "-D", "--defined-only", str(path)], capture_output=True, text=True).stdout
    return {line.split()[-1].split("@")[0] for line in out.splitlines() if line.strip()}


def needed(path):
    out = subprocess.run(["ldd", str(path)], capture_output=True, text=True).stdout
    libs = []
    for line in out.splitlines():
        if "=>" in line:
            target = line.split("=>")[1].split("(")[0].strip()
            if target and target != "not":
                libs.append(target)
    return libs


cache = {}
host = defined(app)
for lib in needed(app):
    cache[lib] = defined(lib)
    host |= cache[lib]
bad = 0
for module in sorted((build / "packages").glob("*.so")):
    available = set(host) | defined(module)
    for lib in needed(module):
        if lib not in cache:
            cache[lib] = defined(lib)
        available |= cache[lib]
    out = subprocess.run(["nm", "-D", "--undefined-only", str(module)], capture_output=True, text=True).stdout
    missing = sorted({line.split()[-1].split("@")[0] for line in out.splitlines() if line.strip()} - available)
    missing = [name for name in missing if not name.startswith(("_ITM_", "__gmon", "_Jv_", "__cxa_finalize", "__tls_get_addr"))]
    print(f"{module.name}: {'ok' if not missing else 'MISSING ' + str(len(missing))}")
    for name in missing[:5]:
        print("   ", subprocess.run(["c++filt", name], capture_output=True, text=True).stdout.strip())
    bad += bool(missing)
sys.exit(1 if bad else 0)
