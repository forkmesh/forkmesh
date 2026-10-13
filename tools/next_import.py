from __future__ import annotations

import argparse
import os
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
DEFAULT_NEXT_REPO = next(
    (p for p in (Path.home() / "projects" / "forkmesh-next" / "src",
                 Path.home() / "projects" / "forkmesh-backup" / "src") if p.is_dir()),
    Path.home() / "projects" / "forkmesh-backup" / "src")
DEFAULT_SEED = "01450c5a48d874aba4e44c69c1860136647078ab"
DEFAULT_BASE = "c48046db27132039419e888c95dcf5b311db0995"
DEFAULT_BRANCH = "next-import"
SOURCE_REF = "refs/next-import/source"


class ImportFailure(RuntimeError):
    pass


def git(repo, *args, data=None, env=None):
    proc = subprocess.run(
        ["git", "-C", str(repo), *args],
        input=data,
        capture_output=True,
        env=env,
    )
    if proc.returncode != 0:
        raise ImportFailure(f"git {' '.join(args)}: {proc.stderr.decode(errors='replace').strip()}")
    return proc.stdout


def git_text(repo, *args):
    return git(repo, *args).decode().strip()


def resolve(repo, rev):
    proc = subprocess.run(
        ["git", "-C", str(repo), "rev-parse", "--verify", "--quiet", f"{rev}^{{commit}}"],
        capture_output=True,
    )
    return proc.stdout.decode().strip() if proc.returncode == 0 else None


def is_ancestor(repo, older, newer):
    proc = subprocess.run(["git", "-C", str(repo), "merge-base", "--is-ancestor", older, newer])
    return proc.returncode == 0


def tree_entries(repo, tree):
    out = git(repo, "ls-tree", "-z", tree)
    entries = {}
    for record in out.split(b"\0"):
        if not record:
            continue
        meta, name = record.split(b"\t", 1)
        entries[name] = meta
    return entries


def make_tree(repo, entries):
    data = b"".join(meta + b"\t" + name + b"\0" for name, meta in sorted(entries.items()))
    return git(repo, "mktree", "-z", data=data).decode().strip()


@dataclass
class Commit:
    tree: str
    parents: list
    author: tuple
    committer: tuple
    message: bytes


def split_ident(value):
    head, _, date = value.rpartition(b">")
    name, _, email = head.partition(b" <")
    return name.decode(), email.decode(), date.strip().decode()


def read_commit(repo, sha):
    raw = git(repo, "cat-file", "commit", sha)
    header, _, message = raw.partition(b"\n\n")
    tree, parents, author, committer = None, [], None, None
    for line in header.split(b"\n"):
        if line.startswith(b" "):
            continue
        key, _, value = line.partition(b" ")
        if key == b"tree":
            tree = value.decode()
        elif key == b"parent":
            parents.append(value.decode())
        elif key == b"author":
            author = split_ident(value)
        elif key == b"committer":
            committer = split_ident(value)
    return Commit(tree, parents, author, committer, message)


def write_commit(repo, tree, parents, author, committer, message):
    env = dict(os.environ)
    env.update({
        "GIT_AUTHOR_NAME": author[0],
        "GIT_AUTHOR_EMAIL": author[1],
        "GIT_AUTHOR_DATE": author[2],
        "GIT_COMMITTER_NAME": committer[0],
        "GIT_COMMITTER_EMAIL": committer[1],
        "GIT_COMMITTER_DATE": committer[2],
    })
    args = ["commit-tree", "--no-gpg-sign", tree]
    for parent in parents:
        args += ["-p", parent]
    return git(repo, *args, data=message, env=env).decode().strip()


@dataclass
class Overlay:
    base_entries: dict
    names: frozenset
    cache: dict = field(default_factory=dict)

    def apply(self, repo, tree):
        if tree in self.cache:
            return self.cache[tree]
        entries = tree_entries(repo, tree)
        missing = {name: self.base_entries[name] for name in self.names if name not in entries}
        mapped = make_tree(repo, {**entries, **missing}) if missing else tree
        self.cache[tree] = mapped
        return mapped


def build_overlay(repo, base, seed):
    base_entries = tree_entries(repo, f"{base}^{{tree}}")
    seed_entries = tree_entries(repo, f"{seed}^{{tree}}")
    names = frozenset(set(base_entries) - set(seed_entries))
    deleted = git(repo, "diff", "-z", "--no-renames", "--name-only", "--diff-filter=D", base, seed)
    for path in deleted.split(b"\0"):
        if path and path.split(b"/", 1)[0] not in names:
            raise ImportFailure(
                f"seed dropped {path.decode(errors='replace')} inside a top-level entry it kept; "
                "only whole top-level omissions can be restored"
            )
    return Overlay(base_entries, names)


def fetch_source(repo, next_repo, next_ref):
    git(repo, "fetch", "--no-tags", "--quiet", str(next_repo), f"+{next_ref}:{SOURCE_REF}")
    return git_text(repo, "rev-parse", SOURCE_REF)


def rewrite(repo, source, seed, base):
    if not is_ancestor(repo, seed, source):
        raise ImportFailure(f"seed {seed} is not an ancestor of {source}")
    overlay = build_overlay(repo, base, seed)
    seed_commit = read_commit(repo, seed)
    mapping = {
        seed: write_commit(
            repo,
            overlay.apply(repo, seed_commit.tree),
            [base],
            seed_commit.author,
            seed_commit.committer,
            seed_commit.message,
        )
    }
    order = git_text(repo, "rev-list", "--topo-order", "--reverse", f"{seed}..{source}").split()
    for sha in order:
        commit = read_commit(repo, sha)
        parents = []
        for parent in commit.parents:
            if parent not in mapping:
                raise ImportFailure(f"{sha} has parent {parent} outside the seed's history")
            parents.append(mapping[parent])
        mapping[sha] = write_commit(
            repo,
            overlay.apply(repo, commit.tree),
            parents,
            commit.author,
            commit.committer,
            commit.message,
        )
    return mapping, order


def checked_out_branches(repo):
    out = git_text(repo, "worktree", "list", "--porcelain")
    return {line.split(" ", 1)[1] for line in out.splitlines() if line.startswith("branch ")}


def update_branch(repo, branch, tip, force=False):
    ref = f"refs/heads/{branch}"
    current = resolve(repo, ref)
    if current == tip:
        return current, False
    if current and not force and not is_ancestor(repo, current, tip):
        raise ImportFailure(f"{branch} at {current} is not an ancestor of {tip}; rerun with --force")
    if ref in checked_out_branches(repo):
        raise ImportFailure(f"{branch} is checked out in a worktree; detach it first")
    git(repo, "update-ref", "-m", "next-import", ref, tip, current or "0" * 40)
    return current, True


def run(repo, next_repo, next_ref, seed, base, branch, force=False, dry_run=False):
    source = fetch_source(repo, next_repo, next_ref)
    mapping, order = rewrite(repo, source, seed, base)
    tip = mapping[source] if order else mapping[seed]
    previous = resolve(repo, f"refs/heads/{branch}")
    if dry_run:
        return {"source": source, "tip": tip, "previous": previous, "commits": len(order), "updated": False}
    previous, updated = update_branch(repo, branch, tip, force)
    return {"source": source, "tip": tip, "previous": previous, "commits": len(order), "updated": updated}


def parse_args(argv):
    parser = argparse.ArgumentParser(prog="next_import")
    parser.add_argument("--repo", default=str(REPO))
    parser.add_argument("--next-repo", default=str(DEFAULT_NEXT_REPO))
    parser.add_argument("--next-ref", default="refs/heads/main")
    parser.add_argument("--seed", default=DEFAULT_SEED)
    parser.add_argument("--base", default=DEFAULT_BASE)
    parser.add_argument("--branch", default=DEFAULT_BRANCH)
    parser.add_argument("--force", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)
    try:
        result = run(
            args.repo, args.next_repo, args.next_ref, args.seed, args.base,
            args.branch, force=args.force, dry_run=args.dry_run,
        )
    except ImportFailure as exc:
        print(f"next_import: {exc}", file=sys.stderr)
        return 1
    new = "all" if not result["previous"] else (
        git_text(args.repo, "rev-list", "--count", f"{result['previous']}..{result['tip']}")
    )
    print(f"source   {result['source']}")
    print(f"tip      {result['tip']}")
    print(f"previous {result['previous'] or '-'}")
    print(f"commits  {result['commits']} after the seed, new on {args.branch}: {new}")
    print("updated" if result["updated"] else ("dry run" if args.dry_run else "unchanged"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
