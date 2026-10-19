#!/usr/bin/env python3
"""Stage compact, semantics-equivalent Python for the Worker runtime.

Cloudflare validates a Python Worker inside a fixed memory allowance that
holds entry.py's executed global scope AND the source of every attached
module. That allowance is what fails deploys with error 10021. Moving route
code behind a request-lazy module reduces the entrypoint's executed startup
AST, but does not reduce the other half of the bill: every module's source
remains attached. 16% of src/ is comments and docstrings — text the runtime
never needs but the validator still pays for.

This writes src_build/, a byte-reduced copy that the Worker actually ships.
app/src and world/src stay the source of truth for review and tests. The staged
modules are round-tripped through Python's AST after docstrings are removed,
which drops comments and formatting without changing executable structure.
Function names remain present in production tracebacks; line numbers refer to
the compact staged module.

    tools/build_worker_python.py [--check]

--check reports the savings without writing.
"""

from __future__ import annotations

import ast
import hashlib
import os
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))
import build_app_plugins

ROOT = Path(__file__).resolve().parent.parent
APP_SOURCE = ROOT / "src"
SOURCE_ROOTS = (APP_SOURCE, ROOT.parent / "world" / "src")
STAGED = ROOT / "src_build"
# These modules assemble static dashboard assets during the preceding build
# step. The Worker never imports them at runtime, so attaching them spends
# scarce Python startup memory without adding a route or Durable Object class.
BUILD_ONLY_MODULES = {
    Path("dashboard_bundle.py"),
    Path("dashboard_shell.py"),
}
# These modules are repository-side tools/libraries, not Worker modules.
# mirror_gateway.py and the collaboration verifier import them from app/src,
# but no deployed Worker path imports either one. Attaching them made every
# upload and startup validation pay for code the isolate can never execute.
NON_WORKER_MODULES = {
    Path("collaboration.py"),
    Path("mirror_replica.py"),
}

def _strip_docstrings(source: str) -> str:
    """Remove docstrings, replacing each with as many blank lines."""
    tree = ast.parse(source)
    lines = source.splitlines(keepends=True)
    removals: list[tuple[int, int, bool]] = []
    for node in ast.walk(tree):
        if not isinstance(
            node, (ast.Module, ast.FunctionDef, ast.AsyncFunctionDef,
                   ast.ClassDef)
        ):
            continue
        body = getattr(node, "body", [])
        if not body:
            continue
        first = body[0]
        if not (
            isinstance(first, ast.Expr)
            and isinstance(first.value, ast.Constant)
            and isinstance(first.value.value, str)
        ):
            continue
        # A body that is ONLY a docstring still needs a statement.
        removals.append((first.lineno, first.end_lineno, len(body) == 1))
    for start, end, needs_pass in sorted(removals, reverse=True):
        indent = len(lines[start - 1]) - len(lines[start - 1].lstrip())
        replacement = (" " * indent + "pass\n") if needs_pass else "\n"
        lines[start - 1:end] = [replacement] + ["\n"] * (end - start)
    return "".join(lines)


def _compact_python(source: str) -> str:
    """Return the smallest stdlib-generated source with the same AST."""
    without_docstrings = _strip_docstrings(source)
    expected = ast.parse(without_docstrings)
    compact = ast.unparse(expected) + "\n"
    if ast.dump(ast.parse(compact)) != ast.dump(expected):
        raise ValueError("AST round-trip changed executable structure")
    return compact


def _compact_cache_dir() -> Path | None:
    configured = os.environ.get("FORKMESH_BUILD_CACHE")
    if configured:
        base = Path(configured)
    else:
        base = Path(os.environ.get("XDG_CACHE_HOME") or Path.home() / ".cache") / "forkmesh"
    directory = base / "worker-python"
    try:
        directory.mkdir(parents=True, exist_ok=True)
    except OSError:
        return None
    return directory


_COMPACT_VERSION = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()[:16]


def _compact_cached(source: str) -> str:
    directory = _compact_cache_dir()
    if directory is None:
        return _compact_python(source)
    key = hashlib.sha256(
        (_COMPACT_VERSION + "\0" + source).encode("utf-8")).hexdigest()
    path = directory / (key + ".py")
    try:
        return path.read_text("utf-8")
    except (OSError, UnicodeDecodeError):
        pass
    compact = _compact_python(source)
    try:
        tmp = path.with_name("%s.%d.tmp" % (key, os.getpid()))
        tmp.write_text(compact, "utf-8")
        os.replace(tmp, path)
    except OSError:
        pass
    return compact


def worker_python_payloads() -> list[tuple[Path, int, bytes]]:
    """Return the exact relative modules and bytes staged for Wrangler."""
    staged_files: list[tuple[Path, int, bytes]] = []
    staged_names: set[Path] = set()
    for source_root in SOURCE_ROOTS:
        for path in sorted(source_root.rglob("*")):
            if path.is_dir() or path.name.startswith("."):
                continue
            relative = path.relative_to(source_root)
            if "__pycache__" in relative.parts:
                continue
            if (
                source_root == APP_SOURCE
                and relative in BUILD_ONLY_MODULES | NON_WORKER_MODULES
            ):
                continue
            if relative in staged_names:
                raise ValueError(
                    "duplicate worker module %s in source roots" % relative
                )
            staged_names.add(relative)
            raw = path.read_bytes()
            if path.suffix != ".py":
                staged_files.append((relative, len(raw), raw))
                continue
            source = raw.decode("utf-8")
            try:
                reduced = _compact_cached(source)
            except (SyntaxError, ValueError) as error:
                raise ValueError(
                    "%s could not be compacted: %s" % (relative, error)
                ) from error
            staged_files.append(
                (relative, len(raw), reduced.encode("utf-8"))
            )
    generated = build_app_plugins.worker_payloads()
    generated_names = {relative for relative, _, _ in generated}
    staged_files = [
        payload for payload in staged_files if payload[0] not in generated_names
    ]
    for relative, raw_size, raw in generated:
        if relative.suffix == ".py":
            reduced = _compact_cached(raw.decode("utf-8")).encode("utf-8")
            staged_files.append((relative, raw_size, reduced))
        else:
            staged_files.append((relative, raw_size, raw))
    return sorted(staged_files, key=lambda payload: payload[0].as_posix())


def main(argv: list[str]) -> int:
    check_only = "--check" in argv[1:]
    try:
        staged_payloads = worker_python_payloads()
    except ValueError as error:
        print(
            "ERROR: %s" % error,
            file=sys.stderr,
        )
        return 1
    before = sum(raw_size for _, raw_size, _ in staged_payloads)
    after = sum(len(payload) for _, _, payload in staged_payloads)
    staged_files = [
        (relative, payload)
        for relative, _, payload in staged_payloads
    ]

    saved = before - after
    print("worker python: %d -> %d bytes (saved %d, %.1f%%)"
          % (before, after, saved, 100.0 * saved / max(1, before)))
    if check_only:
        return 0

    STAGED.mkdir(parents=True, exist_ok=True)
    expected_paths = {STAGED / relative for relative, _ in staged_files}
    for existing in sorted(STAGED.rglob("*"), reverse=True):
        if existing.is_file() and existing not in expected_paths:
            existing.unlink()
        elif existing.is_dir():
            try:
                existing.rmdir()
            except OSError:
                pass
    changed = 0
    for relative, payload in staged_files:
        target = STAGED / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        if target.exists() and target.read_bytes() == payload:
            continue
        target.write_bytes(payload)
        changed += 1
    if changed:
        print("staged %d files in %s (%d changed)" % (
            len(staged_files), STAGED.relative_to(ROOT), changed))
    else:
        print("worker python stage is up to date (%d files)" % len(staged_files))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
