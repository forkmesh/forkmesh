import subprocess
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from forkmesh_host import pull_resync  # noqa: E402
from forkmesh_host.instance_repo import Git, RepoError  # noqa: E402

ENV = {"GIT_AUTHOR_NAME": "Test", "GIT_AUTHOR_EMAIL": "t@example.test",
       "GIT_COMMITTER_NAME": "Test", "GIT_COMMITTER_EMAIL": "t@example.test",
       "PATH": "/usr/bin:/bin", "HOME": "/nonexistent"}


def git(cwd, *args):
    return subprocess.run(["git", *args], cwd=cwd, env=ENV, check=True,
                          capture_output=True, text=True).stdout.strip()


def commit(work, name):
    (work / name).write_text(name + "\n", encoding="utf-8")
    git(work, "add", name)
    git(work, "commit", "-q", "-m", name)
    return git(work, "rev-parse", "HEAD")


def record(number, head, extra=""):
    return ("---\nschema: forkmesh-pull-v1\nnumber: %d\ntitle: t\nbase: main\n"
            "head: %s\nstatus: open\nauthor: carol\nsig: s\n%s---\n\nbody\n"
            % (number, head, extra))


@pytest.fixture
def forge(tmp_path):
    work = tmp_path / "work"
    bare = tmp_path / "hr0123456789abcdef01234567.git"
    git(tmp_path, "init", "-q", "-b", "main", str(work))
    main = commit(work, "readme")
    git(tmp_path, "init", "-q", "--bare", str(bare))
    git(work, "push", "-q", str(bare), "main")
    git(work, "checkout", "-q", "-b", "pr/carol/work")
    first = commit(work, "first")
    mbox = git(work, "format-patch", "--stdout", "main..HEAD")
    git(work, "push", "-q", str(bare), "HEAD:refs/heads/pr/carol/work")
    git(work, "checkout", "-q", "--orphan", "ledger")
    git(work, "rm", "-q", "-rf", ".")
    pulls = {
        3: (record(3, "pr/carol/work",
                   "headBaseOid: %s\nheadOid: %s\n" % (main, "f" * 40)), mbox),
        4: (record(4, "pr/carol/work", "derive: branch\n"), ""),
        5: (record(5, "carol:pr/carol/work",
                   "headBaseOid: %s\n" % main), mbox),
        6: (record(6, "pr/carol/work", "headBaseOid: %s\n" % main),
            mbox.replace("Subject: [PATCH] first", "Subject: [PATCH] other")),
        7: (record(7, "pr/carol/work",
                   "headBaseOid: %s\nsyncedHeadOid: %s\n" % (main, first)),
            mbox),
    }
    for number, (text, series) in pulls.items():
        folder = work / ".forkmesh" / "pulls" / str(number)
        folder.mkdir(parents=True)
        (folder / "pull.md").write_text(text, encoding="utf-8")
        if series:
            (folder / "commits.mbox").write_text(series + "\n",
                                                 encoding="utf-8")
    git(work, "add", "-A")
    git(work, "commit", "-q", "-m", "pulls")
    git(work, "push", "-q", str(bare), "HEAD:refs/heads/forkmesh/pulls")
    git(work, "checkout", "-q", "pr/carol/work")
    return SimpleNamespace(work=work, bare=bare, main=main, first=first)


def by_number(items):
    return {item["number"]: item for item in items}


def test_the_plan_moves_only_patch_pulls_whose_branch_moved(forge):
    tip = commit(forge.work, "second")
    git(forge.work, "push", "-q", str(forge.bare),
        "HEAD:refs/heads/pr/carol/work")
    pulls = by_number(pull_resync.plan(Git(), forge.bare))
    assert pulls[3]["action"] == "sync"
    assert (pulls[3]["tip"], pulls[3]["before"], pulls[3]["commits"]) == (
        tip, "f" * 40, 2)
    assert pulls[4]["reason"] == "follows its branch already"
    assert pulls[5]["reason"] == "not a branch of this repository"
    assert pulls[6]["reason"] == "the branch's commits do not match the pull's"
    assert pulls[7]["action"] == "sync"


def test_a_named_pull_skips_the_series_match_but_not_the_base_check(forge):
    pulls = by_number(pull_resync.plan(Git(), forge.bare, [6, 7]))
    assert set(pulls) == {6, 7}
    assert pulls[6]["action"] == "sync"
    assert pulls[7] == dict(pulls[7], action="skip", reason="in sync")
    git(forge.work, "checkout", "-q", "--orphan", "unrelated")
    git(forge.work, "rm", "-q", "-rf", ".")
    commit(forge.work, "elsewhere")
    git(forge.work, "push", "-q", "-f", str(forge.bare),
        "HEAD:refs/heads/pr/carol/work")
    again = by_number(pull_resync.plan(Git(), forge.bare, [6]))
    assert again[6]["reason"] == "the branch does not contain the pull's base"


def test_a_gone_branch_or_a_closed_pull_is_left_alone(forge):
    git(forge.bare, "update-ref", "-d", "refs/heads/pr/carol/work")
    pulls = by_number(pull_resync.plan(Git(), forge.bare))
    assert pulls[3]["reason"] == "the head branch is gone"


def test_spec_validation():
    args = SimpleNamespace(owner="DonnieBLT", name="BLT", actor="",
                           hosted_root="/var/lib/forkmesh/hosted",
                           number=[3, 3], dry_run=True)
    spec = pull_resync.read_spec(args)
    assert spec == {"owner": "donnieblt", "name": "BLT", "actor": "donnieblt",
                    "root": "/var/lib/forkmesh/hosted", "numbers": [3],
                    "dryRun": True, "nodeConfig": "/etc/forkmesh/mirror-node.json"}
    with pytest.raises(RepoError):
        pull_resync.read_spec(SimpleNamespace(**dict(
            vars(args), hosted_root="relative")))
    with pytest.raises(RepoError):
        pull_resync.read_spec(SimpleNamespace(**dict(vars(args), name="..")))


def test_folded_mbox_subjects_match_one_line_commit_subjects():
    mbox = ("From 0 Mon Sep 17 00:00:00 2001\nSubject: [PATCH 1/2] blt-next: "
            "serve leaderboard from BLT-API instead of hardcoded\n data\n\n"
            "From 1 Mon Sep 17 00:00:00 2001\nSubject: short\n\n")
    assert pull_resync._subjects(mbox) == {
        "blt-next: serve leaderboard from BLT-API instead of hardcoded data",
        "short"}


def test_the_node_is_woken_on_its_loopback_sync_endpoint(tmp_path):
    import json
    import threading
    from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

    seen = []

    class Handler(BaseHTTPRequestHandler):
        def do_POST(self):
            seen.append(self.path)
            self.send_response(200)
            self.send_header("Content-Length", "2")
            self.end_headers()
            self.wfile.write(b"{}")

        def log_message(self, *_args):
            pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        config = tmp_path / "mirror-node.json"
        config.write_text(json.dumps(
            {"listen": "127.0.0.1:%d" % server.server_address[1]}))
        listen = pull_resync.node_listen(str(config))
        assert pull_resync.wake_node(listen)
        assert seen == ["/v1/control/sync"]
    finally:
        server.shutdown()
        server.server_close()
    config.write_text(json.dumps({"listen": "0.0.0.0:8791"}))
    assert pull_resync.node_listen(str(config)) == "127.0.0.1:8791"
    assert pull_resync.node_listen(str(tmp_path / "missing")) == "127.0.0.1:8791"
    assert not pull_resync.wake_node("example.com:80")
