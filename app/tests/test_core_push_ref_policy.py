import json
import subprocess
import sys
import threading
from http.server import ThreadingHTTPServer
from pathlib import Path
from urllib.parse import quote

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from test_mirror_gateway import (  # noqa: E402
    _PushGitHandler,
    gateway,
    make_push_application,
    make_push_client,
    push_capability_headers,
    run,
)


class _GrantingHandler(_PushGitHandler):
    tag_moves = ""
    responses = []

    def do_POST(self):
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length)
        parts = self.path.split("/")
        target = "/v1/repositories/%s/%s/%s" % (parts[1], parts[2], parts[3])
        if type(self).tag_moves:
            target += "?tagMoves=" + quote(type(self).tag_moves, safe="")
        headers = push_capability_headers(body)
        headers["Content-Type"] = self.headers.get("Content-Type", "")
        response = self.application.dispatch("POST", target, headers, body)
        type(self).responses.append(response)
        self._reply(response.status, response.content_type, response.body)


@pytest.fixture
def pushed(tmp_path, monkeypatch):
    app, root, _config_path, _logs = make_push_application(tmp_path)
    monkeypatch.setattr(
        gateway, "REFRESH_PENDING_PATH", tmp_path / "missing" / "pending")
    handler = type("Handler", (_GrantingHandler,), {
        "application": app, "tag_moves": "", "responses": []})
    server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    origin = "http://127.0.0.1:%d/mobile-owner/app" % server.server_address[1]
    source = make_push_client(tmp_path, origin)
    first = _push(source, "origin", "main")
    assert first.returncode == 0, first.stderr
    yield handler, source, root / "mobile-owner" / "app.git"
    server.shutdown()
    server.server_close()
    thread.join(timeout=5)
    app.close()


def _push(source, *arguments):
    return subprocess.run(
        ["git", "push", "--porcelain", *arguments], cwd=source,
        capture_output=True, text=True, timeout=30, check=False,
        env=gateway._git_environment())


def _rev(git_dir, ref):
    return run(["git", "--git-dir", str(git_dir), "rev-parse", ref], git_dir)


def _commit(source, name):
    (source / name).write_text(name + "\n", encoding="utf-8")
    run(["git", "add", "."], source)
    run(["git", "commit", "-m", name], source)
    return run(["git", "rev-parse", "HEAD"], source)


def _updates(handler):
    header = handler.responses[-1].headers.get("X-ForkMesh-Ref-Updates", "")
    return json.loads(header) if header else {}


def test_accepted_push_reports_each_ref_transition_with_commit_counts(pushed):
    handler, source, git_dir = pushed
    before = _rev(git_dir, "refs/heads/main")
    _commit(source, "one.txt")
    after = _commit(source, "two.txt")
    run(["git", "tag", "v1"], source)
    result = _push(source, "origin", "main", "v1")
    assert result.returncode == 0, result.stderr
    report = _updates(handler)
    assert report["total"] == 2
    assert {
        "ref": "refs/heads/main", "old": before, "new": after, "commits": 2,
    } in report["updates"]
    assert {
        "ref": "refs/tags/v1", "old": "0" * 40, "new": after,
    } in report["updates"]
    header = handler.responses[-1].headers["X-ForkMesh-Ref-Updates"]
    assert header.isascii() and "\n" not in header


def test_forced_tag_move_without_a_grant_is_refused(pushed):
    handler, source, git_dir = pushed
    run(["git", "tag", "-a", "v1", "-m", "first"], source)
    assert _push(source, "origin", "v1").returncode == 0
    original = _rev(git_dir, "refs/tags/v1")
    _commit(source, "later.txt")
    run(["git", "tag", "-f", "-a", "v1", "-m", "moved"], source)
    forced = _push(source, "--force", "origin", "v1")
    assert forced.returncode != 0
    assert _rev(git_dir, "refs/tags/v1") == original
    assert "X-ForkMesh-Ref-Updates" not in handler.responses[-1].headers


def test_a_grant_moves_exactly_the_named_tag_to_the_named_object(pushed):
    handler, source, git_dir = pushed
    run(["git", "tag", "-a", "v1", "-m", "first"], source)
    run(["git", "tag", "v2"], source)
    assert _push(source, "origin", "v1", "v2").returncode == 0
    original = _rev(git_dir, "refs/tags/v1")
    _commit(source, "release.txt")
    run(["git", "tag", "-f", "-a", "v1", "-m", "moved"], source)
    run(["git", "tag", "-f", "v2"], source)
    moved = run(["git", "rev-parse", "refs/tags/v1"], source)

    handler.tag_moves = "refs/tags/v1:" + "f" * 40
    assert _push(source, "--force", "origin", "main", "v1").returncode != 0
    assert _rev(git_dir, "refs/tags/v1") == original

    handler.tag_moves = "refs/tags/v1:" + moved
    other = _push(source, "--force", "origin", "v1", "v2")
    assert other.returncode != 0
    assert _rev(git_dir, "refs/tags/v1") == original

    granted = _push(source, "--force", "origin", "main", "v1")
    assert granted.returncode == 0, granted.stderr
    assert _rev(git_dir, "refs/tags/v1") == moved
    assert {
        "ref": "refs/tags/v1", "old": original, "new": moved,
    } in _updates(handler)["updates"]


def test_tags_and_branches_are_never_deleted_and_branches_never_rewound(
        pushed):
    handler, source, git_dir = pushed
    run(["git", "tag", "v1"], source)
    assert _push(source, "origin", "v1").returncode == 0
    tag = _rev(git_dir, "refs/tags/v1")
    handler.tag_moves = "refs/tags/v1:" + tag
    assert _push(source, "origin", "--delete", "v1").returncode != 0
    assert _rev(git_dir, "refs/tags/v1") == tag
    head = _rev(git_dir, "refs/heads/main")
    run(["git", "push", "origin", "main:side"], source)
    assert _push(source, "origin", "--delete", "side").returncode != 0
    run(["git", "commit", "--amend", "-m", "rewritten"], source)
    assert _push(source, "--force", "origin", "main").returncode != 0
    assert _rev(git_dir, "refs/heads/main") == head


@pytest.mark.parametrize("value", [
    "refs/heads/main:" + "a" * 40,
    "refs/tags/v1:" + "a" * 39,
    "refs/tags/v1",
    "refs/tags/:" + "a" * 40,
    "refs/tags/v1:%s refs/tags/v1:%s" % ("a" * 40, "b" * 40),
    "refs/tags/../x:" + "a" * 40,
    " ".join("refs/tags/v%d:%s" % (index, "a" * 40) for index in range(9)),
])
def test_malformed_tag_move_grants_are_refused(value):
    with pytest.raises(gateway.GatewayError):
        gateway.parse_tag_moves(value)


def test_tag_move_grants_parse_to_exact_pairs():
    assert gateway.parse_tag_moves("") == {}
    assert gateway.parse_tag_moves(
        "refs/tags/v1.2.4:%s refs/tags/rc/1:%s" % ("a" * 40, "b" * 64)) == {
            "refs/tags/v1.2.4": "a" * 40, "refs/tags/rc/1": "b" * 64}


def test_policy_allows_new_tags_and_branch_updates_only():
    policy = gateway.enforce_push_ref_policy
    zero, one, two = "0" * 40, "1" * 40, "2" * 40
    policy([(zero, one, "refs/tags/v1"), (one, two, "refs/heads/x")], {})
    policy([(one, two, "refs/tags/v1")], {"refs/tags/v1": two})
    with pytest.raises(gateway.GatewayError):
        policy([(one, two, "refs/tags/v1")], {})
    with pytest.raises(gateway.GatewayError):
        policy([(one, zero, "refs/tags/v1")], {"refs/tags/v1": zero})
    for ref in ("refs/notes/commits", "refs/forkmesh/x", "HEAD"):
        with pytest.raises(gateway.GatewayError):
            policy([(zero, one, ref)], {})
