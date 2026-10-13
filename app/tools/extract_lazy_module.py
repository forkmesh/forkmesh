#!/usr/bin/env python3
"""Move a family of top-level defs out of entry.py into a lazy module.

Cloudflare validates a Python Worker by executing entry.py's global scope
under a fixed memory budget; crossing it fails EVERY deploy with error
10021, and the newer Pyodide snapshots (which fix the "Cannot enter into
task" isolate wedge) need more headroom than the current tree leaves. The
established remedy is to keep route-specific domains out of that path and
reach them through a _LazyModule proxy — see the class docstring in
entry.py and the existing status_monitoring/admin_console splits.

    tools/extract_lazy_module.py <module-name> <def-name> [<def-name> ...]

The moved defs keep their source verbatim. Names they resolve from the
entrypoint (shared helpers, constants, runtime primitives) arrive through
_bind_runtime on first use, exactly like the other lazy modules. entry.py
keeps one proxy assignment per moved public name, at the position the defs
used to occupy, so import-time ordering is unchanged.
"""

from __future__ import annotations

import ast
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ENTRY = ROOT / "src" / "entry.py"

HEADER = '''"""{summary}

Split out of entry.py so Cloudflare's startup-memory validation (error
10021) does not pay for this domain on every isolate; entry.py loads it
through a _LazyModule proxy on the first request that needs it.
"""


def _bind_runtime(runtime):
    """Supply the entrypoint primitives used by this domain."""
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value
'''


def main(argv: list[str]) -> int:
    if len(argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    module_name = argv[1]
    wanted = list(dict.fromkeys(argv[2:]))

    source = ENTRY.read_text(encoding="utf-8")
    lines = source.splitlines(keepends=True)
    offsets = [0]
    for line in lines:
        offsets.append(offsets[-1] + len(line))

    tree = ast.parse(source)
    picked: dict[str, tuple[int, int]] = {}
    for node in tree.body:
        if not isinstance(
            node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)
        ):
            continue
        if node.name not in wanted:
            continue
        start_line = min(
            [node.lineno] + [d.lineno for d in node.decorator_list]
        )
        picked[node.name] = (
            offsets[start_line - 1], offsets[node.end_lineno]
        )

    missing = [name for name in wanted if name not in picked]
    if missing:
        print("not found at top level: %s" % ", ".join(missing),
              file=sys.stderr)
        return 1

    spans = sorted(picked.values())
    moved = "".join(source[start:end] for start, end in spans)

    # A default argument is evaluated at def time, so a moved function whose
    # signature references an entrypoint name would fail before
    # _bind_runtime ever runs. Refuse rather than ship that landmine.
    moved_tree = ast.parse(moved)
    local_names = {
        node.name for node in moved_tree.body
        if isinstance(
            node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)
        )
    }
    for node in moved_tree.body:
        args = getattr(node, "args", None)
        if args is None:
            continue
        for default in list(args.defaults) + [
            d for d in args.kw_defaults if d is not None
        ]:
            for ref in ast.walk(default):
                if isinstance(ref, ast.Name) and ref.id not in local_names:
                    if ref.id in dir(__builtins__) or ref.id in local_names:
                        continue
                    print(
                        "refusing: %s has a default argument referencing %s, "
                        "which is evaluated at import time" % (node.name,
                                                               ref.id),
                        file=sys.stderr)
                    return 1

    summary = "%s routes, loaded on demand." % module_name.replace("_", " ")
    target = ROOT / "src" / ("%s.py" % module_name)
    target.write_text(
        HEADER.format(summary=summary) + "\n\n" + moved.strip("\n") + "\n",
        encoding="utf-8",
    )

    # Replace the first span with the proxy block; blank the rest.
    proxy = ['_%s = _LazyModule("%s")' % (module_name, module_name)]
    proxy += ['%s = _%s.export("%s")' % (name, module_name, name)
              for name in wanted]
    replacement = "\n".join(proxy) + "\n"
    rebuilt = []
    cursor = 0
    for index, (start, end) in enumerate(spans):
        rebuilt.append(source[cursor:start])
        if index == 0:
            rebuilt.append(replacement)
        cursor = end
    rebuilt.append(source[cursor:])
    ENTRY.write_text("".join(rebuilt), encoding="utf-8")

    print("moved %d defs (%d bytes) to src/%s.py"
          % (len(wanted), len(moved), module_name))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
