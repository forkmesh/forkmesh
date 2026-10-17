#!/usr/bin/env python3

import argparse
import base64
import configparser
import hashlib
import os
import re
import subprocess
import sys
import tempfile
import time
import uuid
from pathlib import Path

from cryptography.hazmat.primitives import serialization

sys.path.insert(0, str(Path(__file__).resolve().parent))
import forkmesh_paths  # noqa: E402

REPO = Path(__file__).resolve().parent.parent
LEDGER_REF = "refs/heads/forkmesh/pulls"
PULLS_PREFIX = ".forkmesh/pulls"
EVENT_NAME = re.compile(r"^(\d{4})-([a-z-]+)\.md$")
ATTEMPTS = 5
PULL_BRANCH_SOURCES = ("refs/heads/forkmesh/pull/%d",
                       "refs/remotes/origin/forkmesh/pull/%d")


class LedgerError(Exception):
    pass


class LedgerMoved(LedgerError):
    pass


def git(repo, *args, input_bytes=None, env=None, check=True):
    result = subprocess.run(
        ["git", "-C", str(repo), *args], input=input_bytes,
        capture_output=True, env=env)
    if check and result.returncode != 0:
        raise LedgerError("git %s failed: %s" % (
            " ".join(args[:2]), result.stderr.decode(errors="replace").strip()))
    return result


def git_text(repo, *args, **kwargs):
    return git(repo, *args, **kwargs).stdout.decode(errors="replace").strip()


def ledger_tip(repo):
    result = git(repo, "rev-parse", "--verify", "-q", LEDGER_REF + "^{commit}",
                 check=False)
    tip = result.stdout.decode().strip()
    if result.returncode != 0 or not tip:
        raise LedgerError("%s does not exist in %s." % (LEDGER_REF, repo))
    return tip


def read_blob(repo, rev, path):
    result = git(repo, "cat-file", "blob", "%s:%s" % (rev, path), check=False)
    if result.returncode != 0:
        return None
    return result.stdout.decode("utf-8", "surrogateescape")


def list_pull_files(repo, rev, number):
    out = git_text(repo, "ls-tree", "--name-only", rev,
                   "%s/%d/" % (PULLS_PREFIX, number))
    return [Path(line).name for line in out.splitlines() if line]


def parse_front_matter(text):
    lines = text.split("\n")
    values = {}
    if not lines or lines[0].strip() != "---":
        return values, text
    i = 1
    while i < len(lines) and lines[i] != "---":
        line = lines[i]
        sep = line.find(": ")
        if sep >= 0:
            values[line[:sep]] = line[sep + 2:]
        elif line.endswith(":"):
            values[line[:-1]] = ""
        i += 1
    if i >= len(lines):
        return values, ""
    return values, strip_edge_newlines("\n".join(lines[i + 1:]))


def strip_edge_newlines(text):
    return text.strip("\n")


def load_pull(repo, rev, number):
    text = read_blob(repo, rev, "%s/%d/pull.md" % (PULLS_PREFIX, number))
    if text is None:
        raise LedgerError("Pull request #%d not found on %s." % (number, LEDGER_REF))
    values, body = parse_front_matter(text)
    names = sorted(n for n in list_pull_files(repo, rev, number)
                   if EVENT_NAME.match(n))
    events = []
    for name in names:
        ev_text = read_blob(repo, rev, "%s/%d/%s" % (PULLS_PREFIX, number, name))
        ev_values, ev_body = parse_front_matter(ev_text or "")
        ev_values["body"] = ev_body
        ev_values["_file"] = name
        events.append(ev_values)
    return {"number": number, "text": text, "values": values, "body": body,
            "status": values.get("status", "open"), "events": events,
            "event_names": names}


def next_event_index(names):
    highest = 1
    for name in names:
        match = EVENT_NAME.match(name)
        if match:
            highest = max(highest, int(match.group(1)))
    return highest + 1


def repo_key(owner, name):
    owner = (owner or "").strip().lower()
    name = (name or "").strip().lower()
    if not owner or not name:
        return ""
    for part in (owner, name):
        if "/" in part or "\n" in part or "\x00" in part:
            return ""
    return owner + "/" + name


def signing_prefix(kind, key):
    if not key:
        return "forkmesh-%s-v1\n" % kind
    return "forkmesh-%s-v2\n%s\n" % (kind, key)


def comment_canonical(number, ev, key):
    content_hash = hashlib.sha256(ev["body"].encode("utf-8")).hexdigest()
    return (signing_prefix("pull-comment", key) + ev["type"] + "\n" +
            str(int(number)) + "\n" + ev["author"] + "\n" + str(int(ev["ts"])) +
            "\n" + content_hash).encode("utf-8")


def b64url(raw):
    return base64.urlsafe_b64encode(raw).decode().rstrip("=")


class Identity:
    def __init__(self, private_key):
        self.key = private_key
        self.public = b64url(private_key.public_key().public_bytes(
            serialization.Encoding.Raw, serialization.PublicFormat.Raw))

    @classmethod
    def load(cls, path):
        path = Path(path)
        if not path.is_file():
            raise LedgerError("No identity key at %s." % path)
        return cls(serialization.load_pem_private_key(path.read_bytes(),
                                                      password=None))

    def sign(self, data):
        return b64url(self.key.sign(data))


def make_comment_event(number, body, identity, author_name, key, ts=None,
                       event_id=None):
    if "\n" in author_name or "\r" in author_name:
        raise LedgerError("The author name must be one line.")
    ev = {
        "type": "comment",
        "id": event_id or str(uuid.uuid4()),
        "author": identity.public,
        "authorName": author_name,
        "ts": int(ts if ts is not None else time.time() * 1000),
        "body": strip_edge_newlines(body),
    }
    if not ev["body"].strip():
        raise LedgerError("The comment body is empty.")
    ev["sig"] = identity.sign(comment_canonical(number, ev, key))
    return ev


def render_event(ev):
    lines = [
        "---",
        "type: " + ev["type"],
        "id: " + ev["id"],
        "author: " + ev["author"],
        "authorName: " + ev["authorName"],
        "ts: " + str(ev["ts"]),
        "sig: " + ev["sig"],
        "---",
        "",
    ]
    return "\n".join(lines) + "\n" + ev["body"] + "\n"


def with_status(text, status):
    lines = text.split("\n")
    if not lines or lines[0].strip() != "---":
        raise LedgerError("pull.md has no front matter.")
    end = None
    for i in range(1, len(lines)):
        if lines[i] == "---":
            end = i
            break
    if end is None:
        raise LedgerError("pull.md front matter is not terminated.")
    for i in range(1, end):
        if lines[i].startswith("status: ") or lines[i] == "status:":
            lines[i] = "status: " + status
            return "\n".join(lines)
    insert_at = end
    for i in range(1, end):
        if lines[i].startswith("head: "):
            insert_at = i + 1
            break
    lines.insert(insert_at, "status: " + status)
    return "\n".join(lines)


def with_merged(text, merged_at, merged_by):
    lines = with_status(text, "merged").split("\n")
    end = lines.index("---", 1)
    fields = {"mergedAt": str(int(merged_at)), "mergedBy": merged_by}
    values = {}
    for i in range(1, end):
        name, sep, value = lines[i].partition(": ")
        if sep:
            values[name] = (i, value)
    for name, value in fields.items():
        if name in values:
            lines[values[name][0]] = "%s: %s" % (name, value)
    missing = ["%s: %s" % (n, v) for n, v in fields.items() if n not in values]
    insert_at = values["ts"][0] if "ts" in values else end
    lines[insert_at:insert_at] = missing
    reward = values.get("rewardStatus")
    condition = values.get("rewardCondition", (None, ""))[1].strip()
    if reward and reward[1].strip() == "pending_merge" and condition in ("", "merged"):
        index = lines.index("rewardStatus: " + reward[1])
        lines[index] = "rewardStatus: pending_wallet"
    return "\n".join(lines)


def resolve_commit(repo, ref):
    result = git(repo, "rev-parse", "--verify", "-q", ref + "^{commit}", check=False)
    return result.stdout.decode().strip() if result.returncode == 0 else ""


def pull_branch_source(repo, number):
    for pattern in PULL_BRANCH_SOURCES:
        ref = pattern % number
        if resolve_commit(repo, ref) and read_blob(
                repo, ref, "%s/%d/pull.md" % (PULLS_PREFIX, number)) is not None:
            return ref
    return ""


def pull_folder_files(repo, rev, number):
    folder = "%s/%d/" % (PULLS_PREFIX, number)
    out = git_text(repo, "ls-tree", "-r", "--name-only", rev, folder)
    return {path: read_blob(repo, rev, path) for path in out.splitlines() if path}


def build_tree(repo, base_tree, files):
    scratch = tempfile.mkdtemp(prefix="pull-ledger-index-")
    index_path = os.path.join(scratch, "index")
    env = dict(os.environ, GIT_INDEX_FILE=index_path)
    try:
        git(repo, "read-tree", base_tree, env=env)
        for path, content in files.items():
            blob = git_text(repo, "hash-object", "-w", "--stdin",
                            input_bytes=content.encode("utf-8", "surrogateescape"))
            git(repo, "update-index", "--add", "--cacheinfo",
                "100644,%s,%s" % (blob, path), env=env)
        return git_text(repo, "write-tree", env=env)
    finally:
        for leftover in ("index", "index.lock"):
            try:
                os.unlink(os.path.join(scratch, leftover))
            except FileNotFoundError:
                pass
        os.rmdir(scratch)


def commit_tree(repo, tree, parent, message):
    return git_text(repo, "commit-tree", tree, "-p", parent, "-m", message)


def cas_update(repo, ref, new, old, message):
    result = git(repo, "update-ref", "-m", message, ref, new, old, check=False)
    if result.returncode != 0:
        raise LedgerMoved("%s moved while writing (%s)." % (
            ref, result.stderr.decode(errors="replace").strip()))


def plan_steps(repo, tip, action, number, args, identity, author_name, key):
    steps = []
    source = ""
    if action == "merged" and read_blob(
            repo, tip, "%s/%d/pull.md" % (PULLS_PREFIX, number)) is None:
        source = pull_branch_source(repo, number)
    if source:
        steps.append(("pull #%d: adopt" % number,
                      pull_folder_files(repo, source, number), None))
    pull = load_pull(repo, source or tip, number)
    status = pull["status"]
    if status == "merged":
        raise LedgerError("Pull request #%d is merged; refusing to %s it." % (
            number, action))
    if action == "close" and status == "closed":
        raise LedgerError("Pull request #%d is already closed." % number)
    if action == "reopen" and status != "closed":
        raise LedgerError("Pull request #%d is %s, not closed." % (number, status))
    if action == "merged" and status != "open":
        raise LedgerError("Pull request #%d is %s, not open." % (number, status))
    body = args.get("comment")
    if body is not None:
        ev = make_comment_event(number, body, identity, author_name, key)
        name = "%04d-%s.md" % (next_event_index(pull["event_names"]), ev["type"])
        steps.append(("pull #%d: comment" % number,
                      {"%s/%d/%s" % (PULLS_PREFIX, number, name): render_event(ev)},
                      ev))
    if action in ("close", "reopen"):
        new_status = "closed" if action == "close" else "open"
        steps.append(("pull #%d: %s" % (number, new_status),
                      {"%s/%d/pull.md" % (PULLS_PREFIX, number):
                       with_status(pull["text"], new_status)}, None))
    if action == "merged":
        merged_at = args.get("merged_at") or int(time.time() * 1000)
        steps.append(("pull #%d: merged" % number,
                      {"%s/%d/pull.md" % (PULLS_PREFIX, number):
                       with_merged(pull["text"], merged_at, identity.public)}, None))
    return steps


def write_steps(repo, tip, steps):
    parent = tip
    for message, files, _ in steps:
        tree = build_tree(repo, parent + "^{tree}", files)
        parent = commit_tree(repo, tree, parent, message)
    cas_update(repo, LEDGER_REF, parent, tip, steps[-1][0])
    return parent


def snapshot_pull_metadata(repo, ledger_commit, number, retire=False):
    pull_tree = git_text(repo, "rev-parse", "--verify",
                         "%s:%s/%d" % (ledger_commit, PULLS_PREFIX, number))
    branch_ref = "refs/heads/forkmesh/pull/%d" % number
    metadata_ref = "refs/pr/%d/metadata" % number
    old_tip = resolve_commit(repo, branch_ref)
    parent = old_tip or resolve_commit(repo, metadata_ref) or resolve_commit(
        repo, PULL_BRANCH_SOURCES[1] % number)
    if parent and not retire:
        result = git(repo, "rev-parse", "--verify", "-q",
                     "%s:%s/%d" % (parent, PULLS_PREFIX, number), check=False)
        if result.stdout.decode().strip() == pull_tree:
            return parent
    pulls = git_text(repo, "mktree", input_bytes=(
        "040000 tree %s\t%d\n" % (pull_tree, number)).encode())
    forkmesh = git_text(repo, "mktree", input_bytes=(
        "040000 tree %s\tpulls\n" % pulls).encode())
    root = git_text(repo, "mktree", input_bytes=(
        "040000 tree %s\t.forkmesh\n" % forkmesh).encode())
    commit_args = ["commit-tree", root]
    if parent:
        commit_args += ["-p", parent]
    commit_args += ["-m", "pull #%d: metadata snapshot" % number]
    new_tip = git_text(repo, *commit_args)
    message = "pull #%d: metadata snapshot" % number
    if retire:
        git(repo, "update-ref", "-m", message, metadata_ref, new_tip)
        if old_tip:
            result = git(repo, "update-ref", "-m", message, "-d", branch_ref,
                         old_tip, check=False)
            if result.returncode != 0:
                raise LedgerMoved("%s moved while retiring it (%s)." % (
                    branch_ref, result.stderr.decode(errors="replace").strip()))
        return new_tip
    if old_tip:
        cas_update(repo, branch_ref, new_tip, old_tip, message)
    else:
        cas_update(repo, branch_ref, new_tip, "0" * 40, message)
    git(repo, "update-ref", "-m", message, metadata_ref, new_tip)
    return new_tip


def ledger_checkouts(repo):
    out = git_text(repo, "worktree", "list", "--porcelain", check=False)
    paths, current = [], None
    for line in out.splitlines():
        if line.startswith("worktree "):
            current = line[len("worktree "):]
        elif line == "branch " + LEDGER_REF and current:
            paths.append(current)
    return paths


def sync_checkout(checkout, old, new, paths):
    for path in paths:
        if git(checkout, "diff", "--quiet", old, "--", path, check=False).returncode:
            return "it has its own changes to %s" % path
        if git(checkout, "diff", "--cached", "--quiet", old, "--", path,
               check=False).returncode:
            return "its index differs from %s for %s" % (old[:12], path)
        if (Path(checkout) / path).exists() and read_blob(checkout, old, path) is None:
            return "it already has an untracked %s" % path
    git(checkout, "checkout", new, "--", *paths)
    return ""


def default_repo_key(repo):
    url = git_text(repo, "config", "--get", "remote.origin.url", check=False)
    parts = [p for p in re.split(r"[/:]", url.rstrip("/")) if p]
    if len(parts) < 2:
        return ""
    name = parts[-1][:-4] if parts[-1].endswith(".git") else parts[-1]
    return repo_key(parts[-2], name)


def default_author_name(repo):
    settings = forkmesh_paths.resolve().settings_file
    if settings.is_file():
        parser = configparser.RawConfigParser(strict=False, interpolation=None)
        try:
            parser.read_string(settings.read_text(errors="replace"))
            name = parser.get("account", "nodeName", fallback="").strip()
            if name:
                return name.lower()
        except configparser.Error:
            pass
    return git_text(repo, "config", "--get", "user.name", check=False)


def show(repo, number, out):
    tip = ledger_tip(repo)
    pull = load_pull(repo, tip, number)
    values = pull["values"]
    out.write("#%d %s\n" % (number, values.get("title", "")))
    out.write("author: %s (%s)\n" % (values.get("authorName", ""),
                                     values.get("author", "")))
    out.write("status: %s\n" % pull["status"])
    out.write("base: %s  head: %s\n" % (values.get("base", ""),
                                       values.get("head", "")))
    out.write("ledger: %s\n\n" % tip)
    if pull["body"]:
        out.write(pull["body"] + "\n\n")
    for ev in pull["events"]:
        stamp = ev.get("ts", "")
        try:
            stamp = time.strftime("%Y-%m-%d %H:%M:%S",
                                  time.localtime(int(stamp) / 1000))
        except (TypeError, ValueError):
            pass
        extra = " " + ev["state"] if ev.get("state") else ""
        out.write("--- %s %s%s by %s at %s\n" % (
            ev["_file"], ev.get("type", ""), extra, ev.get("authorName", ""), stamp))
        if ev.get("body"):
            out.write(ev["body"] + "\n")
    return 0


def run_write(repo, action, number, args, identity, author_name, key, execute,
              out, sync_checkout_enabled=True):
    for attempt in range(ATTEMPTS):
        tip = ledger_tip(repo)
        steps = plan_steps(repo, tip, action, number, args, identity,
                           author_name, key)
        if not execute:
            out.write("Dry run against %s %s (repo key %s). Nothing written.\n" % (
                LEDGER_REF, tip, key or "(none, v1)"))
            for message, files, _ in steps:
                out.write("\ncommit: %s\n" % message)
                for path, content in files.items():
                    out.write("file: %s\n%s" % (path, content))
                    if not content.endswith("\n"):
                        out.write("\n")
            for checkout in ledger_checkouts(repo):
                out.write("\nwould refresh those files in the ledger checkout %s\n"
                          % checkout if sync_checkout_enabled else
                          "\nwould leave the ledger checkout %s stale\n" % checkout)
            out.write("\nRe-run with --execute to write.\n")
            return 0
        try:
            new = write_steps(repo, tip, steps)
        except LedgerMoved as moved:
            out.write("%s; retrying.\n" % moved)
            continue
        out.write("%s: %s -> %s\n" % (LEDGER_REF, tip, new))
        for message, _, _ in steps:
            out.write("  %s\n" % message)
        try:
            snapshot_pull_metadata(repo, new, number, retire=action == "merged")
        except LedgerError as err:
            out.write("warning: per-pull metadata ref not advanced (%s); the "
                      "desktop refreshes it on its next pull list.\n" % err)
        touched = sorted({p for _, files, _ in steps for p in files})
        for checkout in ledger_checkouts(repo):
            if not sync_checkout_enabled:
                out.write("warning: %s still shows the old files; the desktop's "
                          "next ledger commit there will undo this write.\n" % checkout)
                continue
            reason = sync_checkout(checkout, tip, new, touched)
            if reason:
                out.write("warning: did not refresh %s (%s); the desktop's next "
                          "ledger commit there can undo this write.\n" % (
                              checkout, reason))
            else:
                out.write("refreshed %s in the ledger checkout %s\n" % (
                    ", ".join(touched), checkout))
        return 0
    raise LedgerError("%s kept moving; gave up after %d attempts." % (
        LEDGER_REF, ATTEMPTS))


def read_body(text, path):
    if text is not None and path is not None:
        raise LedgerError("Give the text or a file, not both.")
    if path is not None:
        return Path(path).read_text(encoding="utf-8")
    return text


def build_parser():
    parser = argparse.ArgumentParser(
        prog="pull_ledger.py",
        description="Comment on, close, reopen and record merges of native "
                    "ForkMesh pull requests "
                    "on the forkmesh/pulls ledger branch. Writes are dry runs "
                    "unless --execute is given.")
    parser.add_argument("--repo-dir", default=str(REPO),
                        help="git repository holding refs/heads/forkmesh/pulls "
                             "(default: this checkout)")
    parser.add_argument("--repo", help="owner/name the signatures bind to "
                                       "(default: from remote.origin.url)")
    parser.add_argument("--identity-key",
                        help="Ed25519 PEM key (default: the node identity key)")
    parser.add_argument("--no-sync-checkout", action="store_true",
                        help="do not refresh the written files in a worktree that "
                             "has forkmesh/pulls checked out (the desktop's "
                             "pull-meta checkout)")
    parser.add_argument("--author-name",
                        help="authorName on new events (default: the desktop "
                             "account name, then git user.name)")
    sub = parser.add_subparsers(dest="command", required=True)
    p_show = sub.add_parser("show", help="print a pull and its conversation")
    p_show.add_argument("number", type=int)
    p_comment = sub.add_parser("comment", help="add a signed comment")
    p_comment.add_argument("number", type=int)
    p_comment.add_argument("--body", help="comment text")
    p_comment.add_argument("--body-file", help="read the comment from a file")
    p_comment.add_argument("--execute", action="store_true", help="write it")
    p_close = sub.add_parser(
        "close", help="set status: closed, keeping the pull and its conversation")
    p_close.add_argument("number", type=int)
    p_close.add_argument("--comment", help="comment posted before closing")
    p_close.add_argument("--comment-file", help="read that comment from a file")
    p_close.add_argument("--execute", action="store_true", help="write it")
    p_reopen = sub.add_parser("reopen", help="set a closed pull back to open")
    p_reopen.add_argument("number", type=int)
    p_reopen.add_argument("--execute", action="store_true", help="write it")
    p_merged = sub.add_parser(
        "merged", help="record an open pull whose change is already on its base "
                       "as merged and retire its branch")
    p_merged.add_argument("number", type=int)
    p_merged.add_argument("--comment", help="comment posted before the merge record")
    p_merged.add_argument("--comment-file", help="read that comment from a file")
    p_merged.add_argument("--execute", action="store_true", help="write it")
    return parser


def main(argv=None, out=None):
    out = out or sys.stdout
    args = build_parser().parse_args(argv)
    repo = Path(args.repo_dir)
    try:
        if args.command == "show":
            return show(repo, args.number, out)
        extra = {}
        if args.command == "comment":
            body = read_body(args.body, args.body_file)
            if body is None:
                raise LedgerError("comment needs --body or --body-file.")
            extra["comment"] = body
        elif args.command in ("close", "merged"):
            body = read_body(args.comment, args.comment_file)
            if body is not None:
                extra["comment"] = body
        if args.repo:
            owner, _, name = args.repo.partition("/")
            key = repo_key(owner, name)
            if not key:
                raise LedgerError("--repo must be owner/name.")
        else:
            key = default_repo_key(repo)
        if not key:
            raise LedgerError("Could not tell the repository from remote.origin.url; "
                              "pass --repo owner/name.")
        identity = Identity.load(args.identity_key or
                                 forkmesh_paths.resolve().identity_key)
        author_name = args.author_name or default_author_name(repo)
        return run_write(repo, args.command, args.number, extra, identity,
                         author_name, key, args.execute, out,
                         not args.no_sync_checkout)
    except LedgerError as err:
        sys.stderr.write("error: %s\n" % err)
        return 1


if __name__ == "__main__":
    sys.exit(main())
