#!/usr/bin/env python3
from __future__ import annotations

import argparse
import io
import shlex
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parents[1]
HOOK_MARKER = "# forkmesh-www-push-check"
PROTECTED_REFS = ("refs/heads/main",)
ARCHIVED = (
    "app/tools/site_templates.py",
    "app/tools/build_market_catalog.py",
    "tools/changelog.py",
    "changelog",
    "www/templates",
    "www/public",
    "marketplace",
)
CHECKS = (
    ("app/tools/site_templates.py", "www/public is out of date with www/templates"),
    ("app/tools/build_market_catalog.py", "www/templates/pages/market.html is out of date with marketplace/catalog.json"),
)


def git(*arguments: str, worktree: Path = REPOSITORY, **options) -> subprocess.CompletedProcess:
    return subprocess.run(["git", "-C", str(worktree), *arguments], check=False, **options)


def pushed_commits(stdin_text: str) -> list[str]:
    commits = []
    for line in stdin_text.splitlines():
        fields = line.split()
        if len(fields) != 4:
            continue
        _local_ref, local_sha, remote_ref, _remote_sha = fields
        if remote_ref in PROTECTED_REFS and set(local_sha) != {"0"}:
            commits.append(local_sha)
    return commits


def check_commit(commit: str, worktree: Path = REPOSITORY) -> list[str]:
    present = [path for path in ARCHIVED
               if git("cat-file", "-e", "%s:%s" % (commit, path), worktree=worktree,
                      capture_output=True).returncode == 0]
    archive = git("archive", "--format=tar", commit, "--", *(present or ARCHIVED),
                  worktree=worktree, capture_output=True)
    if archive.returncode != 0:
        return ["could not read %s: %s" % (commit[:12], archive.stderr.decode(errors="replace").strip())]
    problems = []
    with tempfile.TemporaryDirectory(prefix="www-push-check-") as scratch:
        with tarfile.open(fileobj=io.BytesIO(archive.stdout)) as bundle:
            bundle.extractall(scratch, filter="data")
        for script, problem in CHECKS:
            if not (Path(scratch) / script).exists():
                continue
            result = subprocess.run([sys.executable, "-E", "-s", str(Path(scratch) / script), "--check"],
                                    capture_output=True, text=True, check=False)
            if result.returncode != 0:
                problems.append(problem)
                problems += ["  " + line for line in result.stderr.splitlines() if line.strip()]
    return problems


def run_hook(stdin_text: str, worktree: Path = REPOSITORY, stderr=sys.stderr) -> int:
    failed = False
    for commit in pushed_commits(stdin_text):
        problems = check_commit(commit, worktree)
        if not problems:
            continue
        failed = True
        print("www push check: %s would fail the website deploy:" % commit[:12], file=stderr)
        for line in problems:
            print("  " + line, file=stderr)
    if failed:
        print("Nothing was pushed. Put the edit in www/templates/pages (or rerun the generator that "
              "owns the page, such as app/tools/build_market_catalog.py), check that "
              "python3 app/tools/site_templates.py rewrites no page you did not mean to change, "
              "commit, and push again. See docs/www/templates.md.", file=stderr)
        return 1
    return 0


def hook_script() -> str:
    return (
        "#!/bin/sh\n%s\n"
        'script="$(git rev-parse --show-toplevel)/tools/www_push_check.py"\n'
        '[ -f "$script" ] || script=%s\n'
        '[ -f "$script" ] || exit 0\n'
        'exec python3 "$script" hook "$@"\n'
    ) % (HOOK_MARKER, shlex.quote(str(REPOSITORY / "tools" / "www_push_check.py")))


def install(worktree: Path = REPOSITORY, stdout=sys.stdout) -> int:
    located = git("rev-parse", "--git-common-dir", worktree=worktree, capture_output=True, text=True)
    if located.returncode != 0:
        print("%s is not a Git repository" % worktree, file=sys.stderr)
        return 2
    common = Path(located.stdout.strip())
    if not common.is_absolute():
        common = worktree / common
    configured = git("config", "--get", "core.hooksPath", worktree=worktree, capture_output=True, text=True)
    hooks = Path(configured.stdout.strip()) if configured.returncode == 0 and configured.stdout.strip() else common / "hooks"
    if not hooks.is_absolute():
        hooks = worktree / hooks
    target = hooks / "pre-push"
    if target.exists() and HOOK_MARKER not in target.read_text(encoding="utf-8", errors="replace"):
        print("left %s alone: it is not ours; call tools/www_push_check.py hook from it" % target, file=stdout)
        return 1
    hooks.mkdir(parents=True, exist_ok=True)
    target.write_text(hook_script(), encoding="utf-8")
    target.chmod(0o755)
    print("installed %s" % target, file=stdout)
    return 0


def main(arguments: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Refuse a push to main whose www/public pages are out of date with their templates.")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("install", help="install the pre-push hook in this checkout (shared by its worktrees)")
    hook = sub.add_parser("hook", help="run as the pre-push hook; reads the pushed refs on stdin")
    hook.add_argument("remote", nargs="?")
    hook.add_argument("url", nargs="?")
    check = sub.add_parser("check", help="check one commit")
    check.add_argument("commit", nargs="?", default="HEAD")
    options = parser.parse_args(arguments)
    if options.command == "install":
        return install()
    if options.command == "hook":
        return run_hook(sys.stdin.read())
    problems = check_commit(options.commit)
    for line in problems:
        print(line, file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
