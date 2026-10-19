from __future__ import annotations

import os
import pwd
import shutil
import time
from pathlib import Path
from typing import Iterable, Mapping

PROJECT_ROOTS = ("src", "data", "config", "ssh", "state", "logs")
STRICT_ROOTS = ("src", "ssh")
SHARED_PROJECT_FOLDERS = frozenset({
    "bin", "builds", "cache", "release", "tmp", "toolchain", "toolchain-windows", "worktrees",
})
DISPOSABLE_NAMES = frozenset({
    ".cache", ".pytest_cache", ".tmp", ".worktrees", "__pycache__", "build", "builds", "cache",
    "caches", "dist", "node_modules", "out", "temp", "tmp", "worktrees",
})
DISPOSABLE_PREFIXES = ("build-", "build_", "tmp-", "tmp.")
FROZEN_PROJECTS = ("forkmesh-backup",)


class RefusedDelete(RuntimeError):
    pass


def _clean(path: str | os.PathLike[str]) -> str:
    text = os.fspath(path)
    if not text.strip():
        return ""
    absolute = os.path.abspath(os.path.expanduser(text))
    head, tail = os.path.split(absolute)
    if not tail:
        return os.path.realpath(absolute)
    return os.path.join(os.path.realpath(head), tail)


def _inside(child: str, parent: str) -> bool:
    return child != parent and child.startswith(parent.rstrip("/") + "/")


def _within(child: str, parent: str) -> bool:
    return child == parent or _inside(child, parent)


def _disposable(name: str) -> bool:
    return name in DISPOSABLE_NAMES or name.startswith(DISPOSABLE_PREFIXES)


def _homes(environ: Mapping[str, str]) -> list[str]:
    homes = []
    for candidate in (environ.get("HOME", ""), _account_home()):
        cleaned = _clean(candidate) if candidate else ""
        if cleaned and cleaned != "/" and cleaned not in homes:
            homes.append(cleaned)
    return homes


def _account_home() -> str:
    try:
        return pwd.getpwuid(os.getuid()).pw_dir
    except (KeyError, OSError):
        return ""


def protected_roots(environ: Mapping[str, str] | None = None) -> list[tuple[str, str]]:
    env = os.environ if environ is None else environ
    roots: list[tuple[str, str]] = []

    def add(path: str, kind: str) -> None:
        cleaned = _clean(path) if path else ""
        if cleaned and cleaned != "/" and (cleaned, kind) not in roots:
            roots.append((cleaned, kind))

    def add_project(project: str) -> None:
        add(project, "project")
        for sub in PROJECT_ROOTS:
            add(os.path.join(project, sub), "strict" if sub in STRICT_ROOTS else "owned")

    def add_store(store: str) -> None:
        cleaned = _clean(store)
        if not cleaned:
            return
        add_project(cleaned)
        try:
            slugs = set(os.listdir(cleaned))
        except OSError:
            slugs = set()
        for slug in slugs | {"forkmesh"}:
            add_project(os.path.join(cleaned, slug))

    projects_roots = []
    for home in _homes(env):
        projects_roots.append(os.path.join(home, "projects"))
        add(os.path.join(home, ".ssh"), "strict")
        add(os.path.join(home, ".forkmesh"), "owned")
        add(os.path.join(home, ".config", "ForkMesh"), "owned")
        add(os.path.join(home, ".local", "share", "ForkMesh"), "owned")
        add(os.path.join(home, ".local", "share", "forkmesh"), "owned")
    for variable in ("XDG_DATA_HOME", "XDG_CONFIG_HOME"):
        value = (env.get(variable) or "").strip()
        if value:
            add(os.path.join(value, "ForkMesh"), "owned")
    override = (env.get("FORKMESH_PROJECTS_DIR") or "").strip()
    if override:
        projects_roots.append(override)
    forkmesh_home = (env.get("FORKMESH_HOME") or "").strip()
    if forkmesh_home:
        add_project(forkmesh_home)
        add_store(os.path.join(os.path.dirname(os.path.normpath(forkmesh_home)), "forkmesh_data"))
    for variable in ("FORKMESH_DATA_DIR", "FORKMESH_CONFIG_DIR"):
        value = (env.get(variable) or "").strip()
        if value:
            add(value, "owned")
    project_dir = (env.get("FORKMESH_PROJECT_DIR") or "").strip()
    if project_dir:
        add(project_dir, "project")
    for projects in projects_roots:
        add(projects, "projects")
        cleaned = _clean(projects)
        try:
            children = sorted(os.listdir(cleaned))
        except OSError:
            children = []
        for name in set(children) | {"forkmesh", *FROZEN_PROJECTS}:
            project = os.path.join(cleaned, name)
            if name in FROZEN_PROJECTS:
                add(project, "frozen")
                continue
            add_project(project)
        add_store(os.path.join(cleaned, "forkmesh_data"))
    return roots


def _git_entry(path: str) -> str:
    marker = os.path.join(path, ".git")
    if os.path.islink(marker):
        return "file"
    if os.path.isdir(marker):
        return "dir"
    if os.path.isfile(marker):
        return "file"
    return ""


def refusal(target: str | os.PathLike[str], parent: str | os.PathLike[str], *,
            worktree: bool = False, environ: Mapping[str, str] | None = None) -> str:
    env = os.environ if environ is None else environ
    raw = os.fspath(target) if target is not None else ""
    if not raw.strip():
        return "the path is empty"
    path = _clean(raw)
    if path == "/":
        return "the path is /"
    for home in _homes(env):
        if _within(home, path):
            return f"{path} is HOME ({home}) or contains it"
    raw_parent = os.fspath(parent) if parent is not None else ""
    if not raw_parent.strip():
        return "no expected parent folder was given"
    base = _clean(raw_parent)
    if base == "/":
        return "the expected parent folder is /"
    if not _inside(path, base):
        return f"{path} is not inside the expected parent {base}"
    parts = path.split("/")
    if os.path.basename(path) == ".git" or (".git" in parts[:-1] and _git_history_part(parts)):
        return f"{path} is git history (.git)"
    entry = _git_entry(path)
    if entry == "dir":
        return f"{path} is a git checkout (it has a .git folder)"
    if entry == "file" and not worktree:
        return f"{path} is a git worktree; only the caller that created it may remove it"
    roots = protected_roots(env)
    for root, kind in roots:
        if _within(root, path):
            return f"{path} is or contains protected ForkMesh folder {root}"
    innermost = None
    for root, kind in roots:
        if _inside(path, root) and (innermost is None or len(root) > len(innermost[0])):
            innermost = (root, kind)
    if innermost is not None:
        root, kind = innermost
        rel = path[len(root.rstrip("/")) + 1:].split("/")
        disposable = any(_disposable(part) for part in rel) or ".forkmesh" in rel[:-1]
        if kind == "frozen":
            return f"{path} is inside {root}, which is never erased"
        if kind == "project" and not (rel[0] in SHARED_PROJECT_FOLDERS or rel[0].startswith("toolchain-")
                                      or disposable):
            return f"{path} is inside project folder {root} but not in a cache, build or tmp folder"
        if kind == "strict" and not disposable:
            return f"{path} is inside protected folder {root} but not in a cache, build or tmp folder"
        if kind == "owned" and len(rel) < 2 and not disposable:
            return f"{path} is a top-level folder of protected ForkMesh folder {root}"
        if kind == "projects" and not disposable:
            return f"{path} is inside the projects root {root}"
    return ""


def _git_history_part(parts: list[str]) -> bool:
    index = parts.index(".git")
    following = parts[index + 1:index + 2]
    return not following or following[0] in {"objects", "refs", "logs", "HEAD", "packed-refs"}


def check(target: str | os.PathLike[str], parent: str | os.PathLike[str], *,
          worktree: bool = False, environ: Mapping[str, str] | None = None) -> Path:
    reason = refusal(target, parent, worktree=worktree, environ=environ)
    if reason:
        raise RefusedDelete(f"Refusing to delete: {reason}")
    return Path(_clean(os.fspath(target)))


def remove_tree(target: str | os.PathLike[str], parent: str | os.PathLike[str], *,
                worktree: bool = False, ignore_errors: bool = False,
                environ: Mapping[str, str] | None = None) -> bool:
    path = check(target, parent, worktree=worktree, environ=environ)
    if os.path.islink(path) or os.path.isfile(path):
        try:
            os.unlink(path)
        except FileNotFoundError:
            return False
        except OSError:
            if not ignore_errors:
                raise
            return False
        return True
    if not os.path.exists(path):
        return False
    shutil.rmtree(path, ignore_errors=ignore_errors)
    return not os.path.exists(path)


def remove_trees(targets: Iterable[str | os.PathLike[str]], parent: str | os.PathLike[str], *,
                 ignore_errors: bool = False, environ: Mapping[str, str] | None = None) -> int:
    paths = [check(target, parent, environ=environ) for target in targets]
    return sum(1 for path in paths
               if remove_tree(path, parent, ignore_errors=ignore_errors, environ=environ))


def move_aside(target: str | os.PathLike[str], *, label: str = "replaced") -> Path:
    path = Path(_clean(os.fspath(target)))
    if not os.fspath(target).strip() or str(path) == "/":
        raise RefusedDelete("Refusing to move aside an empty path or /")
    stamp = time.strftime("%Y%m%d-%H%M%S")
    candidate = path.with_name(f"{path.name}.{label}-{stamp}")
    counter = 1
    while candidate.exists() or candidate.is_symlink():
        counter += 1
        candidate = path.with_name(f"{path.name}.{label}-{stamp}-{counter}")
    os.rename(path, candidate)
    return candidate
