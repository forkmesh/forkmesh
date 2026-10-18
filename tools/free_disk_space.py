#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
import re
import shutil
import stat
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import forkmesh_safe_delete

NUMBER = re.compile(r"^(\d{3,5})(?:\D|$)")
AGENT_BUILD = re.compile(r"^agent-(\d{3,5})(?:\D|$)")
TMP_NUMBERED = re.compile(r"^(?:fm|cm)(\d{3,5})(?:\D|$)")
CACHE_NUMBERED = re.compile(r"^fm(\d{3,5})(?:\D|$)")
BACKUP_PATTERNS = (re.compile(r"^forkmesh-fleet-\d{8}-\d{6}\.tar\.gz\.age$"),
                   re.compile(r"^forkmesh-minimal-\d{8}-\d{6}\.tar\.gz$"))
BACKUP_KEEP = 3
CLAUDE_VERSION = re.compile(r"^\d+(?:\.\d+)+$")
STEPS = ("usage", "agents", "tests", "versions", "caches", "report")
DELETE_STEPS = ("agents", "tests", "versions")


@dataclass
class Candidate:
    path: Path
    parent: Path
    reason: str
    group: str = "agents"
    size: int = 0


def _parse_args(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Free disk space by removing ForkMesh build and agent debris that nothing uses. "
                    "Dry run unless --execute is given.")
    parser.add_argument("--execute", action="store_true", help="delete the listed items")
    parser.add_argument("--idle-minutes", type=int, default=60,
                        help="skip anything changed in the last N minutes (default 60)")
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1],
                        help="the primary checkout whose worktrees are kept")
    parser.add_argument("--home", type=Path, default=Path.home())
    parser.add_argument("--tmp", type=Path, default=Path("/tmp"))
    parser.add_argument("--no-caches", action="store_true", help="skip the pip and npm cache clean")
    parser.add_argument("--proc", type=Path, default=Path("/proc"))
    parser.add_argument("--step", choices=STEPS,
                        help="run one part only: usage, agents, tests, versions, caches or report")
    return parser.parse_args(argv)


def tree_size(path: Path) -> int:
    total = 0
    try:
        top = path.lstat()
    except OSError:
        return 0
    if not stat.S_ISDIR(top.st_mode):
        return top.st_blocks * 512
    total += top.st_blocks * 512
    for root, dirs, files in os.walk(path, onerror=lambda _e: None):
        for name in dirs + files:
            try:
                total += os.lstat(os.path.join(root, name)).st_blocks * 512
            except OSError:
                pass
    return total


def touched_since(path: Path, cutoff: float) -> bool:
    try:
        if path.lstat().st_mtime >= cutoff:
            return True
    except OSError:
        return False
    if not path.is_dir() or path.is_symlink():
        return False
    for root, dirs, files in os.walk(path, onerror=lambda _e: None):
        for name in dirs + files:
            try:
                if os.lstat(os.path.join(root, name)).st_mtime >= cutoff:
                    return True
            except OSError:
                pass
    return False


def paths_in_use(proc: Path) -> set[str]:
    used: set[str] = set()
    own = {str(os.getpid()), str(os.getppid())}
    try:
        pids = [entry for entry in os.listdir(proc) if entry.isdigit()]
    except OSError:
        return used
    for pid in pids:
        if pid in own:
            continue
        base = proc / pid
        for link in ("cwd", "root", "exe"):
            try:
                used.add(os.readlink(base / link))
            except OSError:
                pass
        try:
            for fd in os.listdir(base / "fd"):
                try:
                    used.add(os.readlink(base / "fd" / fd))
                except OSError:
                    pass
        except OSError:
            pass
        try:
            for arg in (base / "cmdline").read_bytes().split(b"\0"):
                text = arg.decode(errors="replace")
                if text.startswith("/"):
                    used.add(text)
                elif "=/" in text:
                    used.add(text.split("=", 1)[1])
        except OSError:
            pass
        try:
            for line in (base / "mountinfo").read_text(errors="replace").splitlines():
                fields = line.split()
                if len(fields) > 4:
                    used.add(fields[3])
                    used.add(fields[4])
        except OSError:
            pass
    return {path.removesuffix(" (deleted)") for path in used if path.startswith("/")}


def in_use(path: Path, used: set[str]) -> bool:
    text = str(path)
    prefix = text.rstrip("/") + "/"
    return any(entry == text or entry.startswith(prefix) for entry in used)


def worktrees(repo: Path) -> tuple[set[str], set[str], set[str]]:
    output = subprocess.run(["git", "-C", str(repo), "worktree", "list", "--porcelain"],
                            check=True, capture_output=True, text=True).stdout
    paths, names, numbers = set(), set(), set()
    for line in output.splitlines():
        if not line.startswith("worktree "):
            continue
        path = line.split(" ", 1)[1]
        paths.add(os.path.realpath(path))
        name = os.path.basename(path)
        names.add(name)
        match = NUMBER.match(name)
        if match:
            numbers.add(match.group(1))
    return paths, names, numbers


def _children(folder: Path) -> list[Path]:
    try:
        return sorted(folder.iterdir())
    except OSError:
        return []


def _registered(path: Path, worktree_paths: set[str]) -> bool:
    real = os.path.realpath(path)
    prefix = real.rstrip("/") + "/"
    return any(entry == real or entry.startswith(prefix) for entry in worktree_paths)


def find_candidates(args: argparse.Namespace, worktree_paths: set[str], names: set[str],
                    numbers: set[str]) -> list[Candidate]:
    found: list[Candidate] = []
    home = args.home
    scratch = args.repo / ".worktrees" / ".scratch"
    for path in _children(scratch):
        if path.name not in names:
            found.append(Candidate(path, scratch, "agent scratch with no worktree"))
    builds = home / "projects" / "forkmesh" / "builds"
    for path in _children(builds):
        match = AGENT_BUILD.match(path.name)
        if match and match.group(1) not in numbers:
            found.append(Candidate(path, builds, "agent build with no worktree"))
    for path in _children(args.tmp):
        match = TMP_NUMBERED.match(path.name)
        if match and match.group(1) not in numbers:
            found.append(Candidate(path, args.tmp, "agent /tmp scratch with no worktree"))
    pytest_root = args.tmp / f"pytest-of-{os.environ.get('USER') or Path.home().name}"
    for path in _children(pytest_root):
        if path.name.startswith("pytest-") and path.name != "pytest-current":
            found.append(Candidate(path, pytest_root, "old pytest temp dir", "tests"))
    cache = home / ".cache"
    for path in _children(cache):
        match = CACHE_NUMBERED.match(path.name)
        if match and match.group(1) not in numbers:
            found.append(Candidate(path, cache, "agent cache scratch with no worktree"))
    sandbox = home / ".local" / "share" / "ForkMesh" / "ForkMesh" / "actions" / "sandbox"
    for path in _children(sandbox):
        if path.name.startswith(("forkmesh-run-", "forkmesh-discard-")):
            found.append(Candidate(path, sandbox, "stranded Actions sandbox", "tests"))
    qttests = home / ".qttest" / "share" / "ForkMeshTests"
    for path in _children(qttests):
        if "qt_temp-" in path.name:
            found.append(Candidate(path, qttests, "leftover Qt test temp dir", "tests"))
    trash = home / ".local" / "share" / "Trash"
    for path in _children(trash / "files"):
        if path.name.startswith("forkmesh-cleanup-"):
            found.append(Candidate(path, trash / "files", "trashed by clean-forkmesh-temporary-files",
                                   "versions"))
            info = trash / "info" / f"{path.name}.trashinfo"
            if info.exists():
                found.append(Candidate(info, trash / "info", "its Trash record", "versions"))
    found.extend(_old_backups(home))
    found.extend(_old_claude_versions(home, args.proc))
    return [item for item in found if not _registered(item.path, worktree_paths)]


def _old_backups(home: Path) -> list[Candidate]:
    folder = home / "Backups" / "forkmesh"
    found = []
    for pattern in BACKUP_PATTERNS:
        matches = sorted(path for path in _children(folder) if pattern.match(path.name))
        found.extend(Candidate(path, folder, f"older than the {BACKUP_KEEP} newest backups", "versions")
                     for path in matches[:-BACKUP_KEEP])
    return found


def _old_claude_versions(home: Path, proc: Path) -> list[Candidate]:
    versions = home / ".local" / "share" / "claude" / "versions"
    entries = [path for path in _children(versions) if CLAUDE_VERSION.match(path.name)]
    if len(entries) < 2:
        return []
    entries.sort(key=lambda path: tuple(int(part) for part in path.name.split(".")))
    keep = {entries[-1].name}
    for pid in _children(proc):
        if not pid.name.isdigit():
            continue
        try:
            exe = os.readlink(pid / "exe").removesuffix(" (deleted)")
        except OSError:
            continue
        if exe.startswith(str(versions) + "/"):
            keep.add(exe[len(str(versions)) + 1:].split("/", 1)[0])
    return [Candidate(path, versions, "old Claude Code version", "versions")
            for path in entries if path.name not in keep]


def _make_writable(path: Path) -> None:
    for root, dirs, _files in os.walk(path, onerror=lambda _e: None):
        for name in dirs:
            target = os.path.join(root, name)
            try:
                mode = os.lstat(target).st_mode
                if stat.S_ISDIR(mode) and not mode & stat.S_IWUSR:
                    os.chmod(target, mode | stat.S_IWUSR | stat.S_IXUSR)
            except OSError:
                pass


def remove(candidate: Candidate) -> bool:
    if candidate.path.is_dir() and not candidate.path.is_symlink():
        try:
            mode = candidate.path.lstat().st_mode
            if not mode & stat.S_IWUSR:
                os.chmod(candidate.path, mode | stat.S_IWUSR | stat.S_IXUSR)
        except OSError:
            pass
        _make_writable(candidate.path)
    try:
        return forkmesh_safe_delete.remove_tree(candidate.path, candidate.parent, ignore_errors=True)
    except forkmesh_safe_delete.RefusedDelete as error:
        print(f"  refused: {error}")
        return False


def clean_caches(execute: bool) -> None:
    commands = []
    if shutil.which("pip3"):
        commands.append(["pip3", "cache", "purge"])
    if shutil.which("npm"):
        commands.append(["npm", "cache", "clean", "--force"])
    for command in commands:
        print(("run   " if execute else "would ") + " ".join(command))
        if execute:
            subprocess.run(command, check=False, capture_output=True)


def report_large(home: Path) -> None:
    folders = [
        home / "Android", home / ".android" / "avd", home / ".gradle", home / ".forkmesh" / "gradle-home",
        home / ".gradle-forkmesh-mobile", home / "Backups", home / "Downloads",
        home / "projects" / "forkmesh" / "data" / "backups", home / "projects" / "forkmesh" / "data" / "actions",
        home / "projects" / "forkmesh" / "release", home / ".npm" / "_npx", home / ".cache" / "go-build",
        home / ".cache" / "ccache", home / ".local" / "share" / "Trash",
    ]
    rows = [(tree_size(path), path) for path in folders if path.exists()]
    rows = [row for row in rows if row[0] >= 512 * 1024 * 1024]
    if not rows:
        return
    print("\nLeft for you to decide (not touched):")
    for size, path in sorted(rows, reverse=True):
        print(f"  {_human(size):>8}  {path}")


def _human(size: float) -> str:
    for unit in ("B", "KB", "MB", "GB"):
        if size < 1024:
            return f"{size:.1f} {unit}" if unit != "B" else f"{int(size)} B"
        size /= 1024
    return f"{size:.1f} TB"


def _free(path: Path) -> str:
    usage = shutil.disk_usage(path)
    return f"{_human(usage.free)} free of {_human(usage.total)} ({(usage.total - usage.free) * 100 // usage.total}% used)"


def main(argv: list[str] | None = None) -> int:
    args = _parse_args(argv)
    args.repo = args.repo.resolve()
    args.home = args.home.resolve()
    step = args.step
    if step in (None, "usage"):
        print(f"Before: / {_free(args.home)}; /tmp {_free(args.tmp)}")
    if step == "usage":
        worktree_paths, _names, _numbers = worktrees(args.repo)
        print(f"Keeping {len(worktree_paths)} registered worktrees of {args.repo}")
        return 0
    if step == "report":
        print(f"After: / {_free(args.home)}; /tmp {_free(args.tmp)}")
        report_large(args.home)
        return 0
    removable: list[Candidate] = []
    if step != "caches":
        groups = DELETE_STEPS if step is None else (step,)
        worktree_paths, names, numbers = worktrees(args.repo)
        used = paths_in_use(args.proc)
        cutoff = time.time() - args.idle_minutes * 60
        for candidate in find_candidates(args, worktree_paths, names, numbers):
            if candidate.group not in groups:
                continue
            if in_use(candidate.path, used):
                print(f"skip  {candidate.path}  (a running process uses it)")
                continue
            if args.idle_minutes > 0 and touched_since(candidate.path, cutoff):
                print(f"skip  {candidate.path}  (changed in the last {args.idle_minutes} min)")
                continue
            candidate.size = tree_size(candidate.path)
            removable.append(candidate)
    total = 0
    for candidate in removable:
        verb = "del  " if args.execute else "would"
        print(f"{verb} {_human(candidate.size):>8}  {candidate.path}  ({candidate.reason})")
        if args.execute and not remove(candidate):
            continue
        total += candidate.size
    if step in (None, "caches"):
        if args.no_caches:
            print("Left the pip and npm caches alone (--no-caches).")
        else:
            clean_caches(args.execute)
    if step != "caches":
        print(f"\n{'Freed' if args.execute else 'Would free'} {_human(total)} from {len(removable)} items.")
    if step is None:
        print(f"After: / {_free(args.home)}; /tmp {_free(args.tmp)}")
        report_large(args.home)
    if not args.execute:
        print("\nDry run. Re-run with --execute to delete.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
