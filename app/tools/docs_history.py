#!/usr/bin/env python3
"""Git history for the documentation tree, read once per site build.

``docs_site.py`` renders every page in ``www/docs`` plus the hand-written pages
under ``www/public/docs``; this module answers "when did each of those change,
and what changed recently?" from a single ``git log`` pass over both trees.

The site stages on whatever machine runs ``wrangler deploy`` and on clean CI
sandboxes, so a missing ``git``, a missing repository or a shallow clone must
degrade rather than fail: :func:`load_history` then reports
``available = False`` and callers fall back to file modification times.
"""

from __future__ import annotations

import subprocess
from dataclasses import dataclass
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[2]
# Both halves of the hosted /docs/ tree: the rendered Markdown library and the
# hand-authored HTML pages it links to.
HISTORY_PATHS: tuple[str, ...] = ("www/docs", "www/public/docs")
GIT_TIMEOUT_SECONDS = 120
RECORD = "\x1e"
FIELD = "\x1f"
STATUS_LABELS = {
    "A": "added",
    "M": "updated",
    "D": "removed",
    "R": "renamed",
    "C": "added",
    "T": "updated",
}


@dataclass(frozen=True)
class Change:
    """One repository path touched by one commit."""

    status: str  # added | updated | removed | renamed
    path: str  # repository-relative path after the commit
    previous: str = ""  # only set for renames


@dataclass(frozen=True)
class Commit:
    sha: str
    timestamp: int  # author time, seconds since the epoch
    author: str
    subject: str
    changes: tuple[Change, ...]
    parent: str = ""

    @property
    def short_sha(self) -> str:
        return self.sha[:9]


@dataclass(frozen=True)
class FileHistory:
    path: str
    added: int  # oldest commit in range that touched the file
    updated: int  # newest commit that touched the file
    revisions: int
    # False when the oldest record is an edit rather than the addition itself:
    # the file predates the history that was read (a shallow or grafted clone).
    added_known: bool = True


@dataclass(frozen=True)
class History:
    commits: tuple[Commit, ...]
    files: dict[str, FileHistory]
    available: bool

    def file(self, path: str) -> FileHistory | None:
        return self.files.get(path)


EMPTY = History(commits=(), files={}, available=False)


def _git_log(repository: Path, paths: tuple[str, ...]) -> str:
    command = [
        "git",
        "-C",
        str(repository),
        "log",
        # Merges repeat what their branch commits already recorded; one row per
        # authored change keeps the recent-updates list readable.
        "--no-merges",
        f"--format={RECORD}%H{FIELD}%ct{FIELD}%an{FIELD}%s{FIELD}%P",
        "--name-status",
        # Detect renames so a moved document keeps its age and revision count.
        "-M",
        "--",
        *paths,
    ]
    result = subprocess.run(
        command,
        capture_output=True,
        text=True,
        timeout=GIT_TIMEOUT_SECONDS,
        check=True,
    )
    return result.stdout


def parse_log(output: str) -> tuple[tuple[Commit, ...], dict[str, FileHistory]]:
    """Parse ``git log --name-status`` output, newest commit first."""
    commits: list[Commit] = []
    # Renames are read newest-first, so an older commit naming ``previous``
    # means the same document as the current path it was renamed into.
    aliases: dict[str, str] = {}
    stats: dict[str, dict[str, int]] = {}

    def current_path(path: str) -> str:
        seen: set[str] = set()
        while path in aliases and path not in seen:
            seen.add(path)
            path = aliases[path]
        return path

    sha = ""
    timestamp = 0
    author = ""
    subject = ""
    parent = ""
    changes: list[Change] = []

    def flush() -> None:
        if sha:
            commits.append(
                Commit(
                    sha=sha,
                    timestamp=timestamp,
                    author=author,
                    subject=subject,
                    changes=tuple(changes),
                    parent=parent,
                )
            )

    # str.splitlines() treats the record separator as a line break; split on
    # newlines only so the commit marker survives into the parser.
    for line in output.split("\n"):
        line = line.rstrip("\r")
        if line.startswith(RECORD):
            flush()
            fields = line[1:].split(FIELD)
            fields += [""] * (5 - len(fields))
            sha, raw_timestamp, author, subject = fields[:4]
            parent = (fields[4].split() or [""])[0]
            timestamp = int(raw_timestamp) if raw_timestamp.isdigit() else 0
            changes = []
            continue
        if not line.strip() or not sha:
            continue
        parts = line.split("\t")
        code = parts[0]
        label = STATUS_LABELS.get(code[:1])
        if label is None or len(parts) < 2:
            continue
        previous = ""
        if code[:1] in {"R", "C"} and len(parts) >= 3:
            previous, path = parts[1], parts[2]
        else:
            path = parts[1]
        path = current_path(path)
        if previous:
            aliases[previous] = path
        changes.append(Change(status=label, path=path, previous=previous))
        entry = stats.setdefault(path, {"added": timestamp, "updated": timestamp, "revisions": 0})
        entry["revisions"] += 1
        entry["added"] = timestamp  # the log walks backwards; the last wins
        entry["added_known"] = int(label == "added")
        entry["updated"] = max(entry["updated"], timestamp)

    flush()
    files = {
        path: FileHistory(
            path=path,
            added=values["added"],
            updated=values["updated"],
            revisions=values["revisions"],
            added_known=bool(values.get("added_known")),
        )
        for path, values in stats.items()
    }
    return tuple(commits), files


_CACHE: dict[tuple[str, tuple[str, ...]], History] = {}


def load_history(
    repository: Path = REPOSITORY, paths: tuple[str, ...] = HISTORY_PATHS
) -> History:
    """Read the documentation history, or :data:`EMPTY` when git cannot."""
    key = (str(repository), paths)
    cached = _CACHE.get(key)
    if cached is not None:
        return cached
    try:
        output = _git_log(repository, paths)
    except (OSError, ValueError, subprocess.SubprocessError):
        history = EMPTY
    else:
        commits, files = parse_log(output)
        history = History(commits=commits, files=files, available=bool(commits))
    _CACHE[key] = history
    return history


def file_history(path: str, repository: Path = REPOSITORY) -> FileHistory | None:
    """Dates for one path the ``--no-merges`` pass never saw.

    A file that entered history only through a merge (a conflict resolution
    or a merge that carried it in) has no authored commit of its own. Git's
    default history simplification keeps that merge, so it dates the file.
    """
    try:
        result = subprocess.run(
            ["git", "-C", str(repository), "log", "--format=%ct", "--", path],
            capture_output=True,
            text=True,
            timeout=GIT_TIMEOUT_SECONDS,
            check=True,
        )
    except (OSError, ValueError, subprocess.SubprocessError):
        return None
    stamps = [int(line) for line in result.stdout.split() if line.isdigit()]
    if not stamps:
        return None
    return FileHistory(
        path=path,
        added=stamps[-1],
        updated=stamps[0],
        revisions=len(stamps),
    )


def main() -> int:
    history = load_history()
    if not history.available:
        print("no documentation history available")
        return 1
    print(f"{len(history.commits)} commits, {len(history.files)} files")
    for commit in history.commits[:10]:
        print(f"{commit.short_sha} {commit.timestamp} {commit.subject}")
        for change in commit.changes:
            print(f"    {change.status:8} {change.path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
