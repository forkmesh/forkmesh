from __future__ import annotations

import asyncio
import json
import os
import re
from pathlib import Path

from .instance_repo import (
    HOSTED_ROOT, ID_RE, NAME_RE, OID_RE, REPO_RE, Git, RepoError, _forked,
    _users)

PULL_DIR_RE = re.compile(r"^\.forkmesh/pulls/([1-9][0-9]{0,8})/pull\.md$")
BRANCH_RE = re.compile(r"^(?!forkmesh/)(?!-)[A-Za-z0-9._/-]{1,200}$")
SUBJECT_RE = re.compile(
    r"(?m)^Subject:[ \t]*(?:\[PATCH[^\]]*\][ \t]*)?(.+(?:\n[ \t].*)*)$")
MAX_PULLS = 200


def front_matter(text):
    lines = str(text or "").split("\n")
    if not lines or lines[0].strip() != "---":
        return {}
    values = {}
    for line in lines[1:]:
        if line.strip() == "---":
            return values
        key, _, value = line.partition(":")
        if key.strip() and key.strip() not in values:
            values[key.strip()] = value.strip()
    return {}


def _value(git, git_dir, *args):
    completed = git.run(git_dir, *args, check=False)
    if completed.returncode != 0:
        return ""
    return completed.stdout.decode("utf-8", "replace").strip()


def _blob(git, git_dir, spec):
    completed = git.run(git_dir, "cat-file", "blob", spec, check=False)
    if completed.returncode != 0:
        return ""
    return completed.stdout.decode("utf-8", "replace")


def _ancestor(git, git_dir, older, newer):
    return git.run(git_dir, "merge-base", "--is-ancestor", older, newer,
                   check=False).returncode == 0


def _pull_numbers(git, git_dir, ledger):
    numbers = set()
    for path in _value(git, git_dir, "ls-tree", "-r", "--name-only", ledger,
                       "--", ".forkmesh/pulls").splitlines():
        match = PULL_DIR_RE.fullmatch(path)
        if match:
            numbers.add(int(match.group(1)))
    return sorted(numbers)


def _record_ref(git, git_dir, number, ledger):
    own = _value(git, git_dir, "rev-parse", "--verify", "-q",
                 "refs/heads/forkmesh/pull/%d^{commit}" % number)
    return own if OID_RE.fullmatch(own) else ledger


def _subjects(text):
    return {" ".join(subject.split()) for subject in SUBJECT_RE.findall(
        text or "") if subject.strip()}


def plan(git, git_dir, numbers=None, force=False):
    ledger = _value(git, git_dir, "rev-parse", "--verify", "-q",
                    "refs/heads/forkmesh/pulls^{commit}")
    if not OID_RE.fullmatch(ledger):
        return []
    wanted = set(numbers or [])
    found = []
    for number in _pull_numbers(git, git_dir, ledger)[-MAX_PULLS:]:
        if wanted and number not in wanted:
            continue
        ref = _record_ref(git, git_dir, number, ledger)
        values = front_matter(_blob(
            git, git_dir, "%s:.forkmesh/pulls/%d/pull.md" % (ref, number)))
        item = {"number": number, "head": values.get("head", "")}
        if values.get("status", "open").lower() != "open":
            continue
        branch = item["head"]
        if values.get("derive") == "branch":
            item.update(action="skip", reason="follows its branch already")
        elif ":" in branch or not BRANCH_RE.fullmatch(branch) or (
                branch == values.get("base")):
            item.update(action="skip", reason="not a branch of this repository")
        else:
            item.update(_check(git, git_dir, number, ref, values, branch,
                               force or number in wanted))
        found.append(item)
    return found


def _check(git, git_dir, number, ref, values, branch, force):
    tip = _value(git, git_dir, "rev-parse", "--verify", "-q",
                 "refs/heads/%s^{commit}" % branch)
    if not OID_RE.fullmatch(tip):
        return {"action": "skip", "reason": "the head branch is gone"}
    head = values.get("headOid", "").lower()
    result = {"tip": tip, "headOid": head}
    if tip == values.get("syncedHeadOid", "").lower():
        return dict(result, action="skip", reason="in sync")
    onto = ""
    for key in ("headBaseOid", "creationBaseOid"):
        candidate = values.get(key, "").lower()
        if OID_RE.fullmatch(candidate) and _ancestor(
                git, git_dir, candidate, tip):
            onto = candidate
            break
    if not onto:
        return dict(result, action="skip",
                    reason="the branch does not contain the pull's base")
    series = _subjects(_blob(
        git, git_dir, "%s:.forkmesh/pulls/%d/commits.mbox" % (ref, number)))
    branch_subjects = set(_value(
        git, git_dir, "log", "--no-merges", "--format=%s", "-n", "500",
        "%s..%s" % (onto, tip)).splitlines())
    if series and not series & branch_subjects and not force:
        return dict(result, action="skip",
                    reason="the branch's commits do not match the pull's")
    if not series and not force:
        return dict(result, action="skip",
                    reason="no commit series to match; pass --number")
    before = head if OID_RE.fullmatch(head) else onto
    base = values.get("base", "")
    count = _value(git, git_dir, "rev-list", "--count", "--no-merges", tip,
                   "^refs/heads/" + base) if BRANCH_RE.fullmatch(base) else ""
    return dict(result, action="sync", before=before,
                commits=int(count) if count.isdigit() else 0)


async def locate(runtime, spec):
    entry, env = runtime.entry, runtime.env
    hosted_git = entry._hosted_git._load()
    replica = hosted_git._ReplicaRuntime(env)
    key_bi = await entry.blind_index(env, spec["owner"] + "/" + spec["name"])
    row = await hosted_git._hosted_repos.find_by_key(replica, key_bi)
    if not row:
        raise RepoError("%s/%s is not a hosted repository"
                        % (spec["owner"], spec["name"]))
    return {"id": row["id"]}


async def queue(runtime, spec, items):
    entry, env = runtime.entry, runtime.env
    core_push = entry._core_push._load()
    work = []
    for item in items:
        update = {"ref": "refs/heads/" + item["head"], "old": item["before"],
                  "new": item["tip"], "commits": item["commits"]}
        work.append((item["number"], update,
                     "updated the head to `%s` at %s" % (
                         item["head"], item["tip"][:7])))
    return await core_push.queue_pull_synchronize(
        env, spec["owner"], spec["name"], spec["actor"], work)


def read_spec(args):
    owner = str(args.owner or "").strip().lower()
    name = str(args.name or "").strip()
    actor = str(args.actor or owner).strip().lower()
    if not NAME_RE.fullmatch(owner) or not NAME_RE.fullmatch(actor):
        raise RepoError("invalid --owner or --actor")
    if not REPO_RE.fullmatch(name) or name.startswith("."):
        raise RepoError("invalid --name")
    root = Path(args.hosted_root)
    if not root.is_absolute():
        raise RepoError("--hosted-root must be absolute")
    numbers = sorted({int(number) for number in args.number or []
                      if 0 < int(number) < 10 ** 9})
    return {"owner": owner, "name": name, "actor": actor, "root": str(root),
            "numbers": numbers, "dryRun": bool(args.dry_run)}


def run(args, spec):
    from .ops import _runtime
    app_user, git_user = _users(args)

    def in_app(phase, *extra):
        if app_user:
            return _forked(app_user, lambda: asyncio.run(
                phase(_runtime(args), spec, *extra)))
        return asyncio.run(phase(_runtime(args), spec, *extra))

    info = in_app(locate)
    if not ID_RE.fullmatch(str(info.get("id") or "")):
        raise RepoError("invalid repository id")
    git_dir = Path(spec["root"]) / (info["id"] + ".git")
    if not git_dir.is_dir():
        raise RepoError("%s is missing" % git_dir)
    pulls = plan(Git(git_user), git_dir, spec["numbers"])
    chosen = [item for item in pulls if item.get("action") == "sync"]
    queued = 0
    if chosen and not spec["dryRun"]:
        queued = in_app(queue, chosen)
    return {"ok": True, "owner": spec["owner"], "name": spec["name"],
            "dryRun": spec["dryRun"], "pulls": pulls, "queued": queued}


def cmd_pull_resync(args):
    from . import vault
    try:
        if vault.enabled(os.environ):
            raise RepoError("the app vault is on; this command needs the "
                            "data key in the app settings")
        result = run(args, read_spec(args))
    except RepoError as error:
        result = {"ok": False, "error": str(error)}
    print(json.dumps(result, sort_keys=True, indent=1))
    return 0 if result.get("ok") else 1


def add_parser(sub, common):
    command = sub.add_parser(
        "pull-resync",
        help="move open pull requests whose head branch moved to the "
             "branch's tip (records a synchronize event per pull)")
    common(command)
    command.add_argument("--owner", required=True,
                         help="account that stores the repository")
    command.add_argument("--name", required=True)
    command.add_argument("--number", type=int, action="append",
                         help="only this pull request (repeatable); also "
                              "allows a pull without a commit series")
    command.add_argument("--actor", default="",
                         help="account named in the timeline (default "
                              "--owner)")
    command.add_argument("--dry-run", action="store_true",
                         help="print what would change and queue nothing")
    command.add_argument("--hosted-root", default=HOSTED_ROOT)
    command.add_argument("--app-user", default=None)
    command.add_argument("--git-user", default=None)
    command.set_defaults(func=cmd_pull_resync)
    return command

