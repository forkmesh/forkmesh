#!/usr/bin/env bash
# name: Check ForkMesh files
# description: Read every special agent, shortcut, skill and action in .forkmesh and report the ones the desktop would reject.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
python3 - <<'PY'
import json
import pathlib
import re

problems = []
root = pathlib.Path(".forkmesh")
for path in sorted(root.glob("teammates/*.json")):
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except ValueError as error:
        problems.append(f"{path}: not valid JSON ({error})")
        continue
    if data.get("id") != path.stem:
        problems.append(f"{path}: id {data.get('id')!r} does not match the file name")
    for key in ("name", "specialty"):
        if not str(data.get(key) or "").strip():
            problems.append(f"{path}: {key} is empty")
for path in sorted(list(root.glob("shortcuts/*")) + list(root.glob("skills/*.skill"))):
    head = path.read_text(encoding="utf-8", errors="replace").splitlines()[:20]
    if not any(line.startswith("# name:") for line in head):
        problems.append(f"{path}: no '# name:' line in the first 20 lines")
for path in sorted(root.glob("*.yml")) + sorted(root.glob("*.yaml")):
    text = path.read_text(encoding="utf-8", errors="replace")
    if not re.search(r"(?m)^\s*-?\s*run:", text):
        problems.append(f"{path}: no step with a run command")
    if "\t" in text:
        problems.append(f"{path}: uses a tab for indentation")
for line in problems:
    print(line)
print(f"{len(problems)} problem(s) in .forkmesh")
raise SystemExit(1 if problems else 0)
PY
