#!/usr/bin/env python3

import ast
import asyncio
import base64
import hashlib
import importlib.util
import io
import os
import subprocess
import sys
import time
from pathlib import Path

import pytest
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import (
    Ed25519PrivateKey,
    Ed25519PublicKey,
)

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
EVENTS = ROOT / "app" / "src" / "events.py"
TOOL = HERE / "pull_ledger.py"

_spec = importlib.util.spec_from_file_location("pull_ledger", TOOL)
pl = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(pl)

REPO_KEY = "alice/project"
OPEN_PULL = 7
MERGED_PULL = 8


def _b64url_decode(value):
    return base64.urlsafe_b64decode((value + "=" * (-len(value) % 4)).encode())


async def _ed25519_verify(pub, sig, data):
    try:
        Ed25519PublicKey.from_public_bytes(_b64url_decode(pub)).verify(
            _b64url_decode(sig), data)
        return True
    except Exception:
        return False


async def _sha256_hex(text):
    return hashlib.sha256(text.encode()).hexdigest()


def _load_app_events():
    tree = ast.parse(EVENTS.read_text(encoding="utf-8"), filename=str(EVENTS))
    js_bound = {"to_js", "_to_js_bytes", "ed25519_verify", "sha256_hex",
                "verify_release_manifest"}
    selected = [
        node for node in tree.body
        if (isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
            and node.name not in js_bound)
        or isinstance(node, ast.Assign)
    ]
    module = ast.fix_missing_locations(ast.Module(body=selected, type_ignores=[]))
    namespace = {
        "time": time,
        "ed25519_verify": _ed25519_verify,
        "sha256_hex": _sha256_hex,
        "clean_string": lambda value, limit: str(value or "")[:limit],
    }
    exec(compile(module, str(EVENTS), "exec"), namespace)
    return namespace


APP = _load_app_events()


def app_verifies(number, ev, repo_keys, allow_v1=False):
    return asyncio.run(APP["verify_pull_comment_event"](
        number, ev, repo_keys, allow_v1))


@pytest.fixture(autouse=True)
def sandbox(tmp_path, monkeypatch):
    home = tmp_path / "home"
    home.mkdir()
    monkeypatch.setenv("HOME", str(home))
    for name, sub in (("XDG_DATA_HOME", "data"), ("XDG_CONFIG_HOME", "config"),
                      ("XDG_CACHE_HOME", "cache")):
        monkeypatch.setenv(name, str(home / sub))
    for name in ("FORKMESH_HOME", "FORKMESH_PROJECTS_DIR", "FORKMESH_DATA_DIR",
                 "FORKMESH_CONFIG_DIR", "FORKMESH_PROJECT", "GIT_DIR",
                 "GIT_WORK_TREE", "GIT_INDEX_FILE"):
        monkeypatch.delenv(name, raising=False)
    monkeypatch.setenv("GIT_CONFIG_NOSYSTEM", "1")
    monkeypatch.setenv("GIT_CONFIG_GLOBAL", str(home / "gitconfig"))
    return home


def run_git(repo, *args, input_bytes=None):
    return subprocess.run(["git", "-C", str(repo), *args], check=True,
                          capture_output=True, input=input_bytes).stdout.decode().strip()


PULL_MD = """---
schema: forkmesh-pull-v1
number: {n}
title: Add the thing
base: main
head: feature/{n}
status: {status}
ts: 1790000000000
author: contributorkey
authorName: Contributor
sig: contributorsig
---

Please take this.
"""

REVIEW_MD = """---
type: review
id: r1
author: reviewerkey
authorName: reviewer
ts: 1790000001000
state: changes
sig: reviewsig
---

Needs work.
"""


@pytest.fixture
def repo(tmp_path):
    path = tmp_path / "repo"
    path.mkdir()
    run_git(path, "init", "-q", "-b", "main")
    run_git(path, "config", "user.name", "Ledger Tester")
    run_git(path, "config", "user.email", "ledger@example.invalid")
    run_git(path, "remote", "add", "origin", "https://app.example/alice/project")
    (path / "README").write_text("hi\n")
    run_git(path, "add", "README")
    run_git(path, "commit", "-q", "-m", "init")
    index = tmp_path / "seed-index"
    env = dict(os.environ, GIT_INDEX_FILE=str(index))
    files = {
        ".forkmesh/pulls/%d/pull.md" % OPEN_PULL: PULL_MD.format(n=OPEN_PULL, status="open"),
        ".forkmesh/pulls/%d/0002-review.md" % OPEN_PULL: REVIEW_MD,
        ".forkmesh/pulls/%d/pull.md" % MERGED_PULL: PULL_MD.format(n=MERGED_PULL, status="merged"),
    }
    for rel, content in files.items():
        blob = run_git(path, "hash-object", "-w", "--stdin", input_bytes=content.encode())
        subprocess.run(["git", "-C", str(path), "update-index", "--add", "--cacheinfo",
                        "100644,%s,%s" % (blob, rel)], check=True, env=env)
    tree = subprocess.run(["git", "-C", str(path), "write-tree"], check=True, env=env,
                          capture_output=True).stdout.decode().strip()
    commit = run_git(path, "commit-tree", tree, "-m", "pull #7: open")
    run_git(path, "update-ref", pl.LEDGER_REF, commit)
    return path


@pytest.fixture
def key_file(tmp_path):
    key = Ed25519PrivateKey.generate()
    path = tmp_path / "throwaway.pem"
    path.write_bytes(key.private_bytes(serialization.Encoding.PEM,
                                       serialization.PrivateFormat.PKCS8,
                                       serialization.NoEncryption()))
    return path


def cli(repo, key_file, *args):
    out = io.StringIO()
    code = pl.main(["--repo-dir", str(repo), "--identity-key", str(key_file),
                    "--author-name", "operator", *args], out=out)
    return code, out.getvalue()


def ledger_files(repo, rev=pl.LEDGER_REF):
    names = run_git(repo, "ls-tree", "-r", "--name-only", rev).splitlines()
    return {n: run_git(repo, "cat-file", "blob", "%s:%s" % (rev, n)) for n in names}


def all_refs(repo):
    return run_git(repo, "for-each-ref", "--format=%(refname) %(objectname)")


def test_dry_run_writes_nothing(repo, key_file):
    refs, objects = all_refs(repo), run_git(repo, "count-objects", "-v")
    code, out = cli(repo, key_file, "close", str(OPEN_PULL), "--comment", "Closing.")
    assert code == 0
    assert "Nothing written" in out
    assert "commit: pull #7: comment" in out and "commit: pull #7: closed" in out
    assert "file: .forkmesh/pulls/7/0003-comment.md" in out
    assert "status: closed" in out
    assert all_refs(repo) == refs
    assert run_git(repo, "count-objects", "-v") == objects


def test_dry_run_from_command_line_with_sandboxed_env(repo, key_file, sandbox):
    env = {k: v for k, v in os.environ.items()
           if k not in ("FORKMESH_HOME", "FORKMESH_PROJECTS_DIR")}
    refs = all_refs(repo)
    result = subprocess.run(
        [sys.executable, str(TOOL), "--repo-dir", str(repo), "--identity-key",
         str(key_file), "comment", str(OPEN_PULL), "--body", "hello"],
        capture_output=True, text=True, env=env)
    assert result.returncode == 0, result.stderr
    assert "Dry run" in result.stdout
    assert all_refs(repo) == refs


def test_comment_event_matches_desktop_format_and_app_verifier(repo, key_file):
    code, out = cli(repo, key_file, "comment", str(OPEN_PULL), "--body",
                    "\nFirst line\n\nsecond line\n", "--execute")
    assert code == 0, out
    files = ledger_files(repo)
    text = files[".forkmesh/pulls/7/0003-comment.md"] + "\n"
    lines = text.split("\n")
    assert lines[0] == "---"
    assert [line.split(": ")[0] for line in lines[1:7]] == [
        "type", "id", "author", "authorName", "ts", "sig"]
    assert lines[7] == "---" and lines[8] == ""
    assert text.endswith("---\n\nFirst line\n\nsecond line\n")
    values, body = pl.parse_front_matter(text)
    assert values["type"] == "comment" and values["authorName"] == "operator"
    ev = dict(values, body=body)
    assert app_verifies(OPEN_PULL, ev, (REPO_KEY,))
    assert not app_verifies(OPEN_PULL + 1, ev, (REPO_KEY,))
    assert not app_verifies(OPEN_PULL, ev, ("alice/other",))
    assert not app_verifies(OPEN_PULL, dict(ev, body="edited"), (REPO_KEY,))
    assert run_git(repo, "log", "-1", "--format=%s", pl.LEDGER_REF) == "pull #7: comment"
    assert run_git(repo, "log", "-1", "--format=%an", pl.LEDGER_REF) == "Ledger Tester"


def test_signature_matches_desktop_canonical_bytes():
    key = Ed25519PrivateKey.generate()
    identity = pl.Identity(key)
    ev = pl.make_comment_event(12, "body", identity, "op", REPO_KEY, ts=1790000000123,
                               event_id="fixed")
    expected = ("forkmesh-pull-comment-v2\nalice/project\ncomment\n12\n" +
                identity.public + "\n1790000000123\n" +
                hashlib.sha256(b"body").hexdigest()).encode()
    key.public_key().verify(_b64url_decode(ev["sig"]), expected)


def test_close_keeps_pull_and_flips_status(repo, key_file):
    before = ledger_files(repo)
    old_tip = run_git(repo, "rev-parse", pl.LEDGER_REF)
    code, out = cli(repo, key_file, "close", str(OPEN_PULL), "--comment",
                    "Closing: superseded by #9. Reply here to dispute.", "--execute")
    assert code == 0, out
    after = ledger_files(repo)
    assert set(before) <= set(after)
    added = set(after) - set(before)
    assert added == {".forkmesh/pulls/7/0003-comment.md"}
    pull = after[".forkmesh/pulls/7/pull.md"]
    assert "\nstatus: closed\n" in pull
    assert pull.replace("status: closed", "status: open") == before[
        ".forkmesh/pulls/7/pull.md"]
    for name in before:
        if name != ".forkmesh/pulls/7/pull.md":
            assert after[name] == before[name]
    log = run_git(repo, "log", "--format=%s", "%s..%s" % (old_tip, pl.LEDGER_REF))
    assert log.splitlines() == ["pull #7: closed", "pull #7: comment"]
    meta = run_git(repo, "rev-parse", "refs/heads/forkmesh/pull/7")
    assert run_git(repo, "rev-parse", "refs/pr/7/metadata") == meta
    assert "status: closed" in run_git(
        repo, "cat-file", "blob", meta + ":.forkmesh/pulls/7/pull.md")
    code, out = cli(repo, key_file, "show", str(OPEN_PULL))
    assert code == 0 and "status: closed" in out and "Reply here to dispute." in out
    assert "Needs work." in out


def test_reopen_restores_open(repo, key_file):
    assert cli(repo, key_file, "close", str(OPEN_PULL), "--execute")[0] == 0
    code, _ = cli(repo, key_file, "reopen", str(OPEN_PULL), "--execute")
    assert code == 0
    assert "\nstatus: open\n" in ledger_files(repo)[".forkmesh/pulls/7/pull.md"]
    assert run_git(repo, "log", "-1", "--format=%s", pl.LEDGER_REF) == "pull #7: open"


def test_refuses_unknown_and_merged_pulls(repo, key_file):
    refs = all_refs(repo)
    assert cli(repo, key_file, "close", "99", "--execute")[0] == 1
    assert cli(repo, key_file, "comment", "99", "--body", "x", "--execute")[0] == 1
    assert cli(repo, key_file, "close", str(MERGED_PULL), "--execute")[0] == 1
    assert cli(repo, key_file, "reopen", str(OPEN_PULL), "--execute")[0] == 1
    assert all_refs(repo) == refs


def test_compare_and_swap_refuses_moved_tip(repo, key_file):
    tip = run_git(repo, "rev-parse", pl.LEDGER_REF)
    identity = pl.Identity.load(key_file)
    steps = pl.plan_steps(repo, tip, "close", OPEN_PULL, {}, identity, "op", REPO_KEY)
    concurrent = run_git(repo, "commit-tree", tip + "^{tree}", "-p", tip, "-m",
                         "pull #8: comment")
    run_git(repo, "update-ref", pl.LEDGER_REF, concurrent)
    with pytest.raises(pl.LedgerMoved):
        pl.write_steps(repo, tip, steps)
    assert run_git(repo, "rev-parse", pl.LEDGER_REF) == concurrent


def test_execute_retries_on_top_of_concurrent_commit(repo, key_file, monkeypatch):
    real = pl.cas_update
    moved = {}

    def racing(repo_dir, ref, new, old, message):
        if not moved and ref == pl.LEDGER_REF:
            tip = run_git(repo_dir, "rev-parse", ref)
            moved["commit"] = run_git(repo_dir, "commit-tree", tip + "^{tree}", "-p",
                                      tip, "-m", "pull #8: comment")
            run_git(repo_dir, "update-ref", ref, moved["commit"])
        return real(repo_dir, ref, new, old, message)

    monkeypatch.setattr(pl, "cas_update", racing)
    code, out = cli(repo, key_file, "close", str(OPEN_PULL), "--execute")
    assert code == 0, out
    assert "retrying" in out
    assert run_git(repo, "rev-parse", pl.LEDGER_REF + "^") == moved["commit"]
    assert "\nstatus: closed\n" in ledger_files(repo)[".forkmesh/pulls/7/pull.md"]


def desktop_style_commit(checkout, number):
    pull = checkout / ".forkmesh" / "pulls" / str(number) / "pull.md"
    pull.write_text(pull.read_text() + "edited\n")
    run_git(checkout, "add", ".forkmesh/pulls")
    run_git(checkout, "commit", "-q", "-m", "pull #%d: comment" % number, "--",
            ".forkmesh/pulls")


def test_ledger_checkout_is_refreshed_so_desktop_commit_keeps_write(repo, key_file,
                                                                   tmp_path):
    checkout = tmp_path / "pull-meta"
    run_git(repo, "worktree", "add", "-q", str(checkout), "forkmesh/pulls")
    code, out = cli(repo, key_file, "close", str(OPEN_PULL), "--comment", "bye",
                    "--execute")
    assert code == 0, out
    assert "refreshed" in out
    assert "status: closed" in (checkout / ".forkmesh/pulls/7/pull.md").read_text()
    desktop_style_commit(checkout, MERGED_PULL)
    files = ledger_files(repo)
    assert "\nstatus: closed\n" in files[".forkmesh/pulls/7/pull.md"]
    assert ".forkmesh/pulls/7/0003-comment.md" in files


def test_dirty_ledger_checkout_is_left_alone(repo, key_file, tmp_path):
    checkout = tmp_path / "pull-meta"
    run_git(repo, "worktree", "add", "-q", str(checkout), "forkmesh/pulls")
    pull = checkout / ".forkmesh/pulls/7/pull.md"
    pull.write_text(pull.read_text() + "uncommitted desktop edit\n")
    code, out = cli(repo, key_file, "close", str(OPEN_PULL), "--execute")
    assert code == 0, out
    assert "did not refresh" in out
    assert "uncommitted desktop edit" in pull.read_text()


def test_merged_records_the_merge_and_retires_the_branch(repo, key_file):
    assert cli(repo, key_file, "comment", str(OPEN_PULL), "--body", "hi",
               "--execute")[0] == 0
    branch_tip = run_git(repo, "rev-parse", "refs/heads/forkmesh/pull/7")
    code, out = cli(repo, key_file, "merged", str(OPEN_PULL), "--comment",
                    "Merged into main as abc123.", "--execute")
    assert code == 0, out
    files = ledger_files(repo)
    pull = files[".forkmesh/pulls/7/pull.md"]
    assert "\nstatus: merged\n" in pull
    assert "\nmergedBy: %s\n" % pl.Identity.load(key_file).public in pull
    assert pull.index("mergedAt: ") < pull.index("\nts: ")
    assert "Merged into main as abc123." in files[".forkmesh/pulls/7/0004-comment.md"]
    assert run_git(repo, "log", "-2", "--format=%s", pl.LEDGER_REF).splitlines() == [
        "pull #7: merged", "pull #7: comment"]
    assert "refs/heads/forkmesh/pull/7 " not in all_refs(repo)
    meta = run_git(repo, "rev-parse", "refs/pr/7/metadata")
    assert run_git(repo, "rev-parse", meta + "^") == branch_tip
    assert "status: merged" in run_git(
        repo, "cat-file", "blob", meta + ":.forkmesh/pulls/7/pull.md")
    refs = all_refs(repo)
    assert cli(repo, key_file, "merged", str(OPEN_PULL), "--execute")[0] == 1
    assert all_refs(repo) == refs


def test_merged_adopts_a_pull_only_its_fetched_branch_carries(repo, key_file):
    index = repo.parent / "branch-index"
    env = dict(os.environ, GIT_INDEX_FILE=str(index))
    for rel, content in {".forkmesh/pulls/12/pull.md": PULL_MD.format(n=12, status="open"),
                         ".forkmesh/pulls/12/commits.mbox": "From x\n\xe9\n"}.items():
        blob = run_git(repo, "hash-object", "-w", "--stdin", input_bytes=content.encode())
        subprocess.run(["git", "-C", str(repo), "update-index", "--add", "--cacheinfo",
                        "100644,%s,%s" % (blob, rel)], check=True, env=env)
    tree = subprocess.run(["git", "-C", str(repo), "write-tree"], check=True, env=env,
                          capture_output=True).stdout.decode().strip()
    remote_tip = run_git(repo, "commit-tree", tree, "-m", "pull #12: metadata snapshot")
    run_git(repo, "update-ref", "refs/remotes/origin/forkmesh/pull/12", remote_tip)
    code, out = cli(repo, key_file, "merged", "12", "--execute")
    assert code == 0, out
    files = ledger_files(repo)
    assert "\nstatus: merged\n" in files[".forkmesh/pulls/12/pull.md"]
    assert files[".forkmesh/pulls/12/commits.mbox"] == "From x\n\xe9"
    assert run_git(repo, "log", "-2", "--format=%s", pl.LEDGER_REF).splitlines() == [
        "pull #12: merged", "pull #12: adopt"]
    meta = run_git(repo, "rev-parse", "refs/pr/12/metadata")
    assert run_git(repo, "rev-parse", meta + "^") == remote_tip
    assert "refs/heads/forkmesh/pull/12 " not in all_refs(repo)


def test_merged_moves_a_pending_merge_reward_to_pending_wallet():
    text = PULL_MD.format(n=7, status="open").replace(
        "sig: contributorsig\n",
        "sig: contributorsig\nrewardCondition: merged\nrewardStatus: pending_merge\n")
    merged = pl.with_merged(text, 1790000005000, "ownerkey")
    assert "\nrewardStatus: pending_wallet\n" in merged
    assert "\nmergedAt: 1790000005000\nmergedBy: ownerkey\nts: " in merged
    other = text.replace("rewardCondition: merged", "rewardCondition: payout")
    assert "\nrewardStatus: pending_merge\n" in pl.with_merged(other, 1, "k")
