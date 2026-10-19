#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
PROTECTED = {"main", "master", "HEAD", "forkmesh/pulls"}
PROTECTED_PREFIXES = ("forkmesh/pull/",)
TOMBSTONE_PREFIX = "refs/forkmesh/trash/deleted/"
MESH_PREFIX = "refs/remotes/forkmesh-mesh/"
USAGE = """\
    prune_merged.py                      # dry run: print the plan
    prune_merged.py --execute            # local branches, worktrees, agent sessions
    prune_merged.py --remote --execute   # and the merged branches on every core

Kept: main, master, forkmesh/pulls, forkmesh/pull/*, --keep branches, the
branches of open tasks in --tasks (the Tasks page list as JSON), the
branches of agents that are running or have not finished, and worktrees with
uncommitted files, a process inside them, or a path outside the worktree
folders. Organization tasks are not touched (docs/tools/prune-merged.md).
"""
MERGE_SUBJECT = re.compile(r"^Merge (?:branch )?'?([^' ]+)'? into main$")


def git(repo, *args, timeout=600):
    return subprocess.run(["git", "-C", str(repo), *args], capture_output=True,
                          text=True, timeout=timeout)


def output(repo, *args):
    return git(repo, *args).stdout.strip()


def protected(branch):
    return branch in PROTECTED or branch.startswith(PROTECTED_PREFIXES)


class History:
    def __init__(self, repo):
        self.repo = repo
        self.first_parent = set(output(repo, "rev-list", "--first-parent", "main").split())
        self.merged_names = set()
        for subject in output(repo, "log", "--merges", "--format=%s", "main").splitlines():
            match = MERGE_SUBJECT.match(subject)
            if match:
                self.merged_names.add(match.group(1))
        self._contained = {}

    def contained(self, sha):
        if sha not in self._contained:
            self._contained[sha] = git(self.repo, "merge-base", "--is-ancestor",
                                       sha, "main").returncode == 0
        return self._contained[sha]


def refs(repo, prefix):
    found = {}
    for line in output(repo, "for-each-ref", "--format=%(refname) %(objectname)",
                       prefix).splitlines():
        name, sha = line.split()
        found[name[len(prefix):]] = sha
    return found


def worktrees(repo):
    by_branch = {}
    for block in output(repo, "worktree", "list", "--porcelain").split("\n\n"):
        fields = dict(line.split(" ", 1) for line in block.splitlines() if " " in line)
        branch = fields.get("branch", "")
        if branch.startswith("refs/heads/"):
            by_branch[branch[len("refs/heads/"):]] = fields["worktree"]
    return by_branch


def busy_paths():
    cwds = set()
    for pid in os.listdir("/proc") if os.path.isdir("/proc") else ():
        if pid.isdigit():
            try:
                cwds.add(os.readlink("/proc/%s/cwd" % pid))
            except OSError:
                pass
    return cwds


def worktree_problem(path, roots, cwds):
    if not any(path.startswith(str(root).rstrip("/") + "/") for root in roots):
        return "worktree outside the worktree folders: %s" % path
    if any(cwd == path or cwd.startswith(path + "/") for cwd in cwds):
        return "a process is running in %s" % path
    status = git(path, "status", "--porcelain")
    if status.returncode != 0:
        return "git status failed in %s" % path
    changes = len(status.stdout.splitlines())
    if changes:
        return "%d uncommitted file(s) in %s" % (changes, path)
    return ""


def load_sessions(sessions_dir):
    sessions = []
    if not sessions_dir.is_dir():
        return sessions
    for entry in sorted(sessions_dir.iterdir()):
        meta_path = entry / "meta.json"
        try:
            meta = json.loads(meta_path.read_text())
        except (OSError, ValueError):
            continue
        sessions.append((entry, meta))
    return sessions


def session_done(meta, branches, history):
    if meta.get("status") == "running":
        return False
    branch = meta.get("branchName") or ""
    if not branch:
        return False
    tip = branches.get(branch)
    if tip is None:
        return branch in history.merged_names
    if not history.contained(tip):
        return False
    return tip not in history.first_parent or meta.get("status") == "success"


def plan_local(repo, sessions_dir, worktree_roots, keep):
    history = History(repo)
    branches = refs(repo, "refs/heads/")
    trees = worktrees(repo)
    cwds = busy_paths()
    current = output(repo, "symbolic-ref", "--quiet", "--short", "HEAD")
    kept = {}
    done = []
    for entry, meta in load_sessions(sessions_dir):
        branch = meta.get("branchName") or ""
        if meta.get("teammateId"):
            if branch:
                kept.setdefault(branch, "standing agent %s (%s)" % (
                    meta.get("id", entry.name), meta.get("teammateName") or meta["teammateId"]))
            continue
        if session_done(meta, branches, history):
            done.append((entry, meta))
        elif branch:
            kept.setdefault(branch, "agent session %s is %s and not merged" % (
                meta.get("id", entry.name), meta.get("status") or "unfinished"))
    kept.update(keep)
    finished = {meta.get("branchName") for _entry, meta in done}
    delete = []
    for branch, sha in sorted(branches.items()):
        if protected(branch) or branch == current:
            continue
        if branch in kept or not history.contained(sha):
            continue
        if sha in history.first_parent and branch not in finished:
            kept[branch] = "no commits of its own yet (a new agent's branch)"
            continue
        tree = trees.get(branch, "")
        if tree == str(repo):
            continue
        problem = worktree_problem(tree, worktree_roots, cwds) if tree else ""
        if problem:
            kept[branch] = problem
            continue
        delete.append({"branch": branch, "sha": sha, "worktree": tree})
    deleting = {item["branch"] for item in delete}
    recycle = [(entry, meta) for entry, meta in done
               if meta["branchName"] in deleting or meta["branchName"] not in branches]
    held = {branch: reason for branch, reason in kept.items()
            if branch in branches and history.contained(branches[branch])}
    return {"history": history, "delete": delete, "recycle": recycle, "kept": held,
            "kept_all": kept, "deleting": deleting}


def plan_remote(repo, history, keep, local_left):
    fetched = git(repo, "fetch", "--prune", "origin", timeout=900)
    if fetched.returncode != 0:
        raise SystemExit("git fetch origin failed: %s" % fetched.stderr.strip()[-400:])
    pairs = []
    for branch, sha in sorted(refs(repo, "refs/remotes/origin/").items()):
        if protected(branch) or branch in keep or branch in local_left:
            continue
        if history.contained(sha):
            pairs.append(("refs/heads/" + branch, sha))
    return pairs


def task_keeps(path, repository):
    doc = json.loads(Path(path).read_text())
    keep = {}
    for task in doc.get("tasks", doc) if isinstance(doc, dict) else doc:
        if task.get("status") == "done":
            continue
        for item in task.get("workItems") or []:
            if item.get("branch") and item.get("repository", repository) == repository:
                keep.setdefault(item["branch"], "open task %s %s" % (
                    str(task.get("id", ""))[:8], (task.get("title") or "")[:50]))
    return keep


def recycle_session(entry, meta, recycle_bin, outcomes_path):
    target = recycle_bin / entry.name
    if target.exists():
        return "recycle-bin already has %s" % entry.name
    recycle_bin.mkdir(parents=True, exist_ok=True)
    os.rename(entry, target)
    (target / "deleted-at.json").write_text(json.dumps(
        {"deletedAtMs": int(time.time() * 1000)}, separators=(",", ":")))
    model = (meta.get("model") or "").strip().lower()
    if not model:
        return ""
    try:
        outcomes = json.loads(outcomes_path.read_text())
    except (OSError, ValueError):
        outcomes = {"models": []}
    merged = 1 if meta.get("merged") else 0
    for item in outcomes.setdefault("models", []):
        if (item.get("provider") == meta.get("provider")
                and (item.get("model") or "").strip().lower() == model):
            item["runs"] = max(0, item.get("runs", 0)) + 1
            item["merged"] = max(0, item.get("merged", 0)) + merged
            break
    else:
        outcomes["models"].append({"merged": merged, "model": model,
                                   "provider": meta.get("provider"), "runs": 1})
    outcomes_path.write_text(json.dumps(outcomes, indent=4))
    return ""


def apply_local(repo, plan, agents_dir, key):
    errors = []
    for entry, meta in plan["recycle"]:
        problem = recycle_session(entry, meta, agents_dir / "recycle-bin" / key,
                                  agents_dir / "model-outcomes.json")
        if problem:
            errors.append(problem)
    for item in plan["delete"]:
        if item["worktree"]:
            removed = git(repo, "worktree", "remove", item["worktree"])
            if removed.returncode != 0:
                errors.append("%s: %s" % (item["worktree"], removed.stderr.strip()))
                continue
        problem = retire_branch(repo, item["branch"], item["sha"])
        if problem:
            errors.append("%s: %s" % (item["branch"], problem))
    git(repo, "worktree", "prune")
    return errors


def retire_branch(repo, branch, sha):
    tombstone = git(repo, "update-ref", TOMBSTONE_PREFIX + branch, sha)
    if tombstone.returncode != 0:
        return tombstone.stderr.strip()
    deleted = git(repo, "update-ref", "-d", "refs/heads/" + branch, sha)
    if deleted.returncode != 0:
        return deleted.stderr.strip()
    mesh = MESH_PREFIX + branch
    mesh_tip = output(repo, "rev-parse", "-q", "--verify", mesh)
    if mesh_tip and git(repo, "merge-base", "--is-ancestor", mesh_tip, sha).returncode == 0:
        git(repo, "update-ref", "-d", mesh, mesh_tip)
    return ""


def delete_remote(pairs, repository, execute):
    sys.path.insert(0, str(TOOLS))
    import next_common as common
    import next_fleet
    runner = common.Runner(execute=execute)
    next_fleet.delete_refs(common.load_fleet(), [], common.Ssh(), runner,
                           repository, pairs)


def default_layout():
    sys.path.insert(0, str(TOOLS))
    import forkmesh_paths
    return forkmesh_paths.resolve()


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Prune branches, worktrees and agent sessions that main has merged.",
        epilog=USAGE, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo", default=str(TOOLS.parent),
                        help="source checkout (default: this one)")
    parser.add_argument("--repository", default="forkmesh/forkmesh",
                        help="owner/name on the cores and in the agent store")
    parser.add_argument("--agents", help="desktop agent store (default: <data>/agents)")
    parser.add_argument("--worktree-root", action="append", default=[],
                        help="extra folder whose worktrees may be removed")
    parser.add_argument("--keep", action="append", default=[], metavar="BRANCH",
                        help="never prune this branch")
    parser.add_argument("--tasks", metavar="FILE",
                        help="task list JSON; branches of open tasks are kept")
    parser.add_argument("--remote", action="store_true",
                        help="also delete the merged branches on every core")
    parser.add_argument("--execute", action="store_true",
                        help="actually do it (the default is a dry run)")
    args = parser.parse_args(argv)

    repo = Path(args.repo).resolve()
    layout = None if args.agents else default_layout()
    agents_dir = Path(args.agents) if args.agents else layout.data_path("agents")
    key = args.repository.replace("/", "-")
    roots = [repo / ".worktrees", repo / ".claude" / "worktrees",
             *(Path(root) for root in args.worktree_root)]
    if layout is not None:
        roots.append(layout.home_dir / "worktrees")

    keep = task_keeps(args.tasks, args.repository) if args.tasks else {}
    keep.update((branch, "--keep") for branch in args.keep)
    plan = plan_local(repo, agents_dir / "sessions" / key, roots, keep)
    print("Local: %d branch(es), %d worktree(s), %d agent session(s) to prune" % (
        len(plan["delete"]), sum(1 for item in plan["delete"] if item["worktree"]),
        len(plan["recycle"])))
    for item in plan["delete"]:
        print("  branch %s%s" % (item["branch"],
                                 "  (worktree %s)" % item["worktree"] if item["worktree"] else ""))
    for entry, meta in plan["recycle"]:
        print("  agent %s  %s" % (entry.name, (meta.get("issueTitle") or meta.get("prompt") or "")[:60]))
    if plan["kept"]:
        print("Kept although merged:")
        for branch, reason in sorted(plan["kept"].items()):
            print("  %s: %s" % (branch, reason))

    errors = apply_local(repo, plan, agents_dir, key) if args.execute else []
    for error in errors:
        print("error: %s" % error, file=sys.stderr)

    if args.remote:
        left = set(refs(repo, "refs/heads/")) if args.execute else (
            set(refs(repo, "refs/heads/")) - plan["deleting"])
        pairs = plan_remote(repo, plan["history"], set(plan["kept_all"]), left)
        print("Remote: %d merged branch(es) to delete on the cores" % len(pairs))
        for ref, sha in pairs:
            print("  %s %s" % (ref[len("refs/heads/"):], sha[:12]))
        if pairs:
            delete_remote(pairs, args.repository, args.execute)
    if not args.execute:
        print("(dry run: nothing was changed; pass --execute to apply)")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
