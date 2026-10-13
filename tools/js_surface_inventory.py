#!/usr/bin/env python3
"""Inventory the Worker-runtime surface the Python app uses.

The CPython host (app/host/forkmesh_host) stands in for Pyodide's ``js``,
``pyodide.ffi`` and ``workers`` modules and for the Worker bindings on
``env``/``ctx``. This tool AST-scans every module the Worker ships
(app/src, world/src and marketplace package ``content/worker`` code) and
writes the attribute chains used on those names as JSON, so a test can fail
when app code starts using something the host does not implement.

Chains are written with ``()`` for a call, e.g. ``js.crypto.subtle.digest()``
or ``js.Uint8Array.new().to_py()``. Values assigned from a JS call inside a
function are followed one step (``resp = await js_fetch(...)`` then
``resp.status`` becomes ``js.fetch().status``).

    tools/js_surface_inventory.py [--output PATH] [--summary]
"""

from __future__ import annotations

import argparse
import ast
import json
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
MODULE_PREFIXES = ("js", "pyodide", "workers")

# Python wrappers in entry.py that return exactly what js.fetch returns.
FETCH_WRAPPERS = {
    "js_fetch", "js_fetch_with_timeout", "js_fetch_until_response",
}
ENV_ROOTS = {"env", "self.env", "worker_env", "runtime_env"}
CTX_ROOTS = {"ctx", "self.ctx", "self.state"}


def source_files(repo: Path = REPO) -> list[Path]:
    files = sorted((repo / "app" / "src").glob("*.py"))
    files += sorted((repo / "world" / "src").glob("*.py"))
    files += sorted((repo / "marketplace" / "packages").glob(
        "*/content/worker/**/*.py"))
    return files


def _import_aliases(tree: ast.Module) -> dict[str, str]:
    aliases: dict[str, str] = {}
    for node in ast.walk(tree):
        if isinstance(node, ast.ImportFrom) and node.module:
            top = node.module.split(".")[0]
            if top not in MODULE_PREFIXES or node.level:
                continue
            for name in node.names:
                aliases[name.asname or name.name] = (
                    node.module + "." + name.name)
        elif isinstance(node, ast.Import):
            for name in node.names:
                top = name.name.split(".")[0]
                if top in MODULE_PREFIXES:
                    aliases[name.asname or top] = (
                        name.name if name.asname else top)
    return aliases


def _module_defs(tree: ast.Module) -> set[str]:
    names = set()
    for node in tree.body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef,
                             ast.ClassDef)):
            names.add(node.name)
        elif isinstance(node, ast.Assign):
            for target in node.targets:
                if isinstance(target, ast.Name):
                    names.add(target.id)
    return names


def _dotted(node) -> str | None:
    if isinstance(node, ast.Name):
        return node.id
    if isinstance(node, ast.Attribute):
        base = _dotted(node.value)
        return None if base is None else base + "." + node.attr
    return None


def _chain(node, roots: dict[str, str]) -> list[str] | None:
    """Return [root, seg, ...] for a Name-rooted attribute/call chain."""
    if isinstance(node, ast.Await):
        return _chain(node.value, roots)
    if isinstance(node, ast.Name):
        if node.id in roots:
            return [roots[node.id]]
        return None
    if isinstance(node, ast.Attribute):
        # Allow multi-part roots such as ``self.env``.
        dotted = _dotted(node)
        if dotted in roots:
            return [roots[dotted]]
        base = _chain(node.value, roots)
        return None if base is None else base + ["." + node.attr]
    if isinstance(node, ast.Call):
        if (isinstance(node.func, ast.Name) and node.func.id == "__import__"
                and node.args and isinstance(node.args[0], ast.Constant)
                and node.args[0].value in MODULE_PREFIXES
                and "__import__" in roots):
            return [node.args[0].value]
        base = _chain(node.func, roots)
        return None if base is None else base + ["()"]
    return None


class _Scanner(ast.NodeVisitor):
    def __init__(self, roots: dict[str, str], found: dict[str, set],
                 follow: bool = True):
        self.follow = follow
        self.roots = [dict(roots)]
        self.found = found
        self.where = ""

    def _record(self, chain: list[str]):
        self.found.setdefault("".join(chain), set()).add(self.where)

    def visit_FunctionDef(self, node):
        self.roots.append(dict(self.roots[-1]))
        if not self.follow:
            self.generic_visit(node)
            self.roots.pop()
            return
        # A parameter named like an alias shadows the module binding.
        for arg in node.args.args + node.args.kwonlyargs:
            if arg.arg in self.roots[-1] and arg.arg not in (
                    "env", "ctx", "state"):
                self.roots[-1].pop(arg.arg)
        self.generic_visit(node)
        self.roots.pop()

    visit_AsyncFunctionDef = visit_FunctionDef

    def visit_Assign(self, node):
        self.generic_visit(node)
        if not self.follow:
            return
        chain = _chain(node.value, self.roots[-1])
        if chain is None and isinstance(node.value, ast.Await):
            inner = node.value.value
            if (isinstance(inner, ast.Call) and isinstance(inner.func, ast.Name)
                    and inner.func.id in FETCH_WRAPPERS):
                chain = ["js.fetch", "()"]
        if chain is None or len(chain) < 2:
            return
        if not chain[0].startswith(MODULE_PREFIXES):
            return
        for target in node.targets:
            if isinstance(target, ast.Name):
                self.roots[-1][target.id] = "".join(chain)

    def generic_visit(self, node):
        if isinstance(node, (ast.Attribute, ast.Call, ast.Name)):
            chain = _chain(node, self.roots[-1])
            if chain is not None:
                self._record(chain)
        if (isinstance(node, ast.Call) and isinstance(node.func, ast.Name)
                and node.func.id in ("getattr", "hasattr")
                and len(node.args) >= 2
                and isinstance(node.args[1], ast.Constant)
                and isinstance(node.args[1].value, str)):
            chain = _chain(node.args[0], self.roots[-1])
            if chain is not None:
                self._record(chain + ["." + node.args[1].value])
        super().generic_visit(node)


def scan(repo: Path = REPO) -> dict:
    files = source_files(repo)
    trees = {}
    for path in files:
        try:
            trees[path] = ast.parse(path.read_text("utf-8"), str(path))
        except SyntaxError:
            continue
    entry = repo / "app" / "src" / "entry.py"
    entry_aliases = _import_aliases(trees[entry]) if entry in trees else {}
    js_found: dict[str, set] = {}
    env_found: dict[str, set] = {}
    ctx_found: dict[str, set] = {}
    request_found: dict[str, set] = {}
    for path, tree in trees.items():
        rel = path.relative_to(repo).as_posix()
        aliases = _import_aliases(tree)
        source = path.read_text("utf-8")
        if path != entry and "_bind_runtime" in source:
            local = _module_defs(tree)
            for name, target in entry_aliases.items():
                if name not in aliases and name not in local:
                    aliases[name] = target
        aliases["__import__"] = "__import__"
        scanner = _Scanner(aliases, js_found)
        scanner.where = rel
        scanner.visit(tree)
        for roots, found in (
            ({r: "env" for r in ENV_ROOTS}, env_found),
            ({r: "ctx" for r in CTX_ROOTS}, ctx_found),
            ({"request": "request"}, request_found),
        ):
            sub = _Scanner(roots, found, follow=False)
            sub.where = rel
            sub.visit(tree)
    js_found = {k: v for k, v in js_found.items()
                if k.startswith(MODULE_PREFIXES) and k != "__import__"}
    env_vars = sorted({
        chain.split(".")[1].split("(")[0]
        for chain in env_found if chain.count(".") >= 1
    })
    binding_chains = sorted(
        chain for chain in env_found
        if chain.count(".") >= 2 or chain.endswith("()")
    )
    return {
        "files": len(trees),
        "js": {k: sorted(v) for k, v in sorted(js_found.items())},
        "env_names": env_vars,
        "env_chains": binding_chains,
        "ctx_chains": sorted(ctx_found),
        "request_attrs": sorted({
            chain.split(".")[1].split("(")[0]
            for chain in request_found if "." in chain
        }),
    }


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--output", "-o", help="write JSON here")
    parser.add_argument("--summary", action="store_true")
    args = parser.parse_args(argv)
    data = scan()
    if args.summary:
        print("files=%d js_chains=%d env_names=%d env_chains=%d ctx=%d" % (
            data["files"], len(data["js"]), len(data["env_names"]),
            len(data["env_chains"]), len(data["ctx_chains"])))
        return 0
    text = json.dumps(data, indent=1, sort_keys=True)
    if args.output:
        Path(args.output).write_text(text + "\n", "utf-8")
    else:
        print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
