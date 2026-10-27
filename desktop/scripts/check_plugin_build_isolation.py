#!/usr/bin/env python3
import re
import subprocess
import sys
from pathlib import Path

build = Path(sys.argv[1]).resolve()
packages = Path(sys.argv[2]).resolve()
ninja = sys.argv[3] if len(sys.argv) > 3 else "ninja"

if not (build / "build.ninja").is_file():
    print(f"SKIP: {build} is not a Ninja build; the check reads Ninja's recorded header dependencies")
    sys.exit(0)

WINDOW_HEADER = re.compile(r"^MainWindow\w*\.h$")
PER_COMMIT = {"ForkMeshVersion.h", "ForkMeshBuildCommit.h"}
MODULE_OBJECT = re.compile(r"^CMakeFiles/forkmesh-package-([^/]+)\.dir/.*\.o$")
CORE_OBJECT = re.compile(r"^CMakeFiles/forkmesh\.dir/.*\.o$")


def run(*arguments):
    return subprocess.run([ninja, "-C", str(build), *arguments],
                          capture_output=True, text=True, check=True).stdout


objects = sorted({line.split(":")[0] for line in run("-t", "targets", "all").splitlines()
                  if line.split(":")[0].endswith(".o")})
modules = {}
for target in objects:
    match = MODULE_OBJECT.match(target)
    if match:
        modules[target] = match.group(1)
core = [target for target in objects if CORE_OBJECT.match(target)]
if not modules:
    print("SKIP: no package modules in this build")
    sys.exit(0)


def dependencies(targets):
    found = {}
    current = None
    for line in run("-t", "deps", *targets).splitlines():
        if line and not line.startswith(" "):
            current = line.split(":")[0]
            found[current] = None if "deps not found" in line else []
        elif current is not None and line.strip() and found[current] is not None:
            found[current].append((build / line.strip()).resolve())
    return found


def owning_plugin(path):
    try:
        relative = path.relative_to(packages)
    except ValueError:
        return None
    return relative.parts[0] if relative.parts else None


problems = []
unbuilt = []
module_ids = set(modules.values())
module_deps = dependencies(sorted(modules))
for target, headers in module_deps.items():
    plugin = modules[target]
    if headers is None:
        unbuilt.append(target)
        continue
    for header in headers:
        name = header.name
        if WINDOW_HEADER.match(name):
            problems.append(f"{plugin}: {target} depends on the window header {name}")
        elif name in PER_COMMIT:
            problems.append(f"{plugin}: {target} depends on {name}, rewritten on every commit")
        elif "cmake_pch" in name or "/forkmesh.dir/" in str(header) or "/forkmesh_autogen/" in str(header):
            problems.append(f"{plugin}: {target} depends on the app's own build output {header}")
        else:
            owner = owning_plugin(header)
            if owner and owner != plugin:
                problems.append(f"{plugin}: {target} depends on {owner}'s file {header}")

core_deps = dependencies(core) if core else {}
for target, headers in core_deps.items():
    if headers is None:
        unbuilt.append(target)
        continue
    for header in headers:
        owner = owning_plugin(header)
        if owner in module_ids:
            problems.append(f"core: {target} depends on the {owner} module's file {header}")

if unbuilt and len(unbuilt) == len(module_deps) + len(core_deps):
    print("FAIL: nothing has been built yet, so Ninja has no header dependencies to check")
    sys.exit(1)

for problem in problems:
    print(problem)
print(f"checked {len(module_deps)} plugin objects and {len(core_deps)} core objects"
      f" ({len(unbuilt)} not built yet), {len(problems)} problems")
sys.exit(1 if problems else 0)
