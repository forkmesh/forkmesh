import json
import os
import subprocess
import sys
import threading
from http.server import ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlencode

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

ZERO = "0" * 40


class _PolicyHandler(_PushGitHandler):
    query = {}
    responses = []

    def do_POST(self):
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length)
        parts = self.path.split("/")
        target = "/v1/repositories/%s/%s/%s" % (parts[1], parts[2], parts[3])
        if type(self).query:
            target += "?" + urlencode(type(self).query)
        headers = push_capability_headers(body)
        headers["Content-Type"] = self.headers.get("Content-Type", "")
        response = self.application.dispatch("POST", target, headers, body)
        type(self).responses.append(response)
        self._reply(response.status, response.content_type, response.body)


@pytest.fixture
def forge(tmp_path, monkeypatch):
    app, root, _config_path, _logs = make_push_application(
        tmp_path, owners=("alice",))
    monkeypatch.setattr(
        gateway, "REFRESH_PENDING_PATH", tmp_path / "missing" / "pending")
    handler = type("Handler", (_PolicyHandler,), {
        "application": app, "query": {}, "responses": []})
    server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    origin = "http://127.0.0.1:%d/alice/app" % server.server_address[1]
    source = make_push_client(tmp_path, origin)
    assert _push(source, "origin", "main").returncode == 0
    yield handler, source, root / "alice" / "app.git", app
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
    completed = subprocess.run(
        ["git", "--git-dir", str(git_dir), "rev-parse", "--verify", "-q",
         ref], capture_output=True, text=True, check=False)
    return completed.stdout.strip()


def _refs(git_dir, prefix):
    output = run(["git", "--git-dir", str(git_dir), "for-each-ref",
                  "--format=%(refname) %(objectname)", prefix], git_dir)
    return dict(line.split(" ") for line in output.splitlines() if line)


def _commit(source, name):
    (source / name).write_text(name + "\n", encoding="utf-8")
    run(["git", "add", "."], source)
    run(["git", "commit", "-m", name], source)
    return run(["git", "rev-parse", "HEAD"], source)


def _updates(handler):
    header = handler.responses[-1].headers.get("X-ForkMesh-Ref-Updates", "")
    return json.loads(header) if header else {}


def _branch(source, name):
    run(["git", "checkout", "-q", "-B", name, "main"], source)
    tip = _commit(source, name.replace("/", "-") + ".txt")
    return tip


def _rewrite(source, message="rewritten"):
    run(["git", "commit", "--amend", "-q", "-m", message], source)
    return run(["git", "rev-parse", "HEAD"], source)


def test_writer_force_push_on_a_feature_branch_keeps_the_old_tip(forge):
    handler, source, git_dir, _app = forge
    handler.query = {"rewrite": "refs/heads/"}
    old = _branch(source, "feature")
    assert _push(source, "origin", "feature").returncode == 0
    new = _rewrite(source)
    plain = _push(source, "origin", "feature")
    assert plain.returncode != 0
    forced = _push(source, "--force-with-lease", "origin", "feature")
    assert forced.returncode == 0, forced.stderr
    assert _rev(git_dir, "refs/heads/feature") == new
    kept = _refs(git_dir, "refs/forkmesh/history/feature/")
    assert kept == {"refs/forkmesh/history/feature/0000000001": old}
    update = _updates(handler)["updates"][0]
    assert update == {
        "ref": "refs/heads/feature", "old": old, "new": new, "commits": 1,
        "forced": True, "kept": "refs/forkmesh/history/feature/0000000001"}
    advertised = run(["git", "ls-remote", "origin"], source)
    assert "refs/forkmesh" not in advertised and old not in advertised


def test_fast_forward_pushes_under_a_policy_keep_no_history(forge):
    handler, source, git_dir, _app = forge
    handler.query = {"rewrite": "refs/heads/"}
    _branch(source, "feature")
    assert _push(source, "origin", "feature").returncode == 0
    _commit(source, "more.txt")
    assert _push(source, "origin", "feature").returncode == 0
    assert "forced" not in _updates(handler)["updates"][0]
    assert _refs(git_dir, "refs/forkmesh/") == {}


def test_the_default_branch_stays_fast_forward_only(forge):
    handler, source, git_dir, _app = forge
    handler.query = {"rewrite": "refs/heads/"}
    run(["git", "checkout", "-q", "main"], source)
    head = _rev(git_dir, "refs/heads/main")
    _rewrite(source)
    refused = _push(source, "--force", "origin", "main")
    assert refused.returncode != 0
    assert "main is the default branch" in refused.stderr
    assert "git pull --rebase origin main" in refused.stderr
    assert _rev(git_dir, "refs/heads/main") == head
    assert _refs(git_dir, "refs/forkmesh/") == {}


def test_a_default_branch_not_named_main_is_found_from_head(forge):
    handler, source, git_dir, _app = forge
    handler.query = {"rewrite": "refs/heads/"}
    _branch(source, "trunk")
    assert _push(source, "origin", "trunk").returncode == 0
    run(["git", "--git-dir", str(git_dir), "symbolic-ref", "HEAD",
         "refs/heads/trunk"], git_dir)
    head = _rev(git_dir, "refs/heads/trunk")
    _rewrite(source)
    refused = _push(source, "--force", "origin", "trunk")
    assert refused.returncode != 0
    assert "trunk is the default branch" in refused.stderr
    assert _rev(git_dir, "refs/heads/trunk") == head


def test_protected_and_ledger_branches_are_never_rewritten(forge):
    handler, source, git_dir, _app = forge
    handler.query = {"rewrite": "refs/heads/"}
    for name in ("release", "forkmesh/notes"):
        _branch(source, name)
        assert _push(source, "origin", name).returncode == 0
    handler.query = {"rewrite": "refs/heads/", "protect": "refs/heads/release"}
    run(["git", "checkout", "-q", "release"], source)
    release = _rev(git_dir, "refs/heads/release")
    _rewrite(source)
    assert _push(source, "--force", "origin", "release").returncode != 0
    assert _rev(git_dir, "refs/heads/release") == release
    run(["git", "checkout", "-q", "forkmesh/notes"], source)
    notes = _rev(git_dir, "refs/heads/forkmesh/notes")
    _rewrite(source)
    refused = _push(source, "--force", "origin", "forkmesh/notes")
    assert refused.returncode != 0
    assert "only moves forward" in refused.stderr
    assert _rev(git_dir, "refs/heads/forkmesh/notes") == notes


def test_contributor_may_rewrite_only_inside_its_own_prefix(forge):
    handler, source, git_dir, _app = forge
    handler.query = {"rewrite": "refs/heads/"}
    _branch(source, "shared")
    assert _push(source, "origin", "shared").returncode == 0
    prefix = "refs/heads/pr/bob/"
    handler.query = {"pushPrefix": prefix, "rewrite": prefix}
    old = _branch(source, "pr/bob/fix")
    assert _push(source, "origin", "pr/bob/fix").returncode == 0
    new = _rewrite(source)
    forced = _push(source, "--force", "origin", "pr/bob/fix")
    assert forced.returncode == 0, forced.stderr
    assert _rev(git_dir, "refs/heads/pr/bob/fix") == new
    assert _refs(git_dir, "refs/forkmesh/history/pr/bob/fix/") == {
        "refs/forkmesh/history/pr/bob/fix/0000000001": old}
    run(["git", "checkout", "-q", "shared"], source)
    shared = _rev(git_dir, "refs/heads/shared")
    _rewrite(source)
    outside = _push(source, "--force", "origin", "shared")
    assert outside.returncode != 0
    assert _rev(git_dir, "refs/heads/shared") == shared


def test_a_contributor_grant_naming_refs_outside_its_prefix_is_invalid(forge):
    handler, source, git_dir, _app = forge
    prefix = "refs/heads/pr/bob/"
    handler.query = {"pushPrefix": prefix, "rewrite": "refs/heads/"}
    _branch(source, "pr/bob/fix")
    assert _push(source, "origin", "pr/bob/fix").returncode != 0
    assert _rev(git_dir, "refs/heads/pr/bob/fix") == ""


def test_history_keeps_every_replaced_tip(forge):
    handler, source, git_dir, _app = forge
    handler.query = {"rewrite": "refs/heads/"}
    tips = [_branch(source, "feature")]
    assert _push(source, "origin", "feature").returncode == 0
    for index in range(24):
        tips.append(_rewrite(source, "rewrite %d" % index))
        assert _push(source, "--force", "origin", "feature").returncode == 0
    kept = _refs(git_dir, "refs/forkmesh/history/feature/")
    assert kept == {
        "refs/forkmesh/history/feature/%010d" % (index + 1): tip
        for index, tip in enumerate(tips[:-1])}
    assert _rev(git_dir, "refs/heads/feature") == tips[-1]


def test_a_force_push_is_refused_when_the_old_tip_cannot_be_kept(
        forge, monkeypatch):
    handler, source, git_dir, _app = forge
    handler.query = {"rewrite": "refs/heads/"}
    old = _branch(source, "feature")
    assert _push(source, "origin", "feature").returncode == 0
    monkeypatch.setattr(
        gateway.GitRepository, "keep_push_history", lambda *_args: "")
    _rewrite(source)
    refused = _push(source, "--force", "origin", "feature")
    assert refused.returncode != 0
    assert _rev(git_dir, "refs/heads/feature") == old


def test_the_tip_is_kept_before_the_branch_moves(forge, monkeypatch):
    handler, source, git_dir, _app = forge
    handler.query = {"rewrite": "refs/heads/"}
    old = _branch(source, "feature")
    assert _push(source, "origin", "feature").returncode == 0
    seen = []
    keep = gateway.GitRepository.keep_push_history

    def watching(self, ref, tip):
        seen.append(_rev(git_dir, ref))
        return keep(self, ref, tip)
    monkeypatch.setattr(gateway.GitRepository, "keep_push_history", watching)
    _rewrite(source)
    assert _push(source, "--force", "origin", "feature").returncode == 0
    assert seen == [old]


def test_a_push_never_deletes_a_branch(forge):
    handler, source, git_dir, _app = forge
    handler.query = {"rewrite": "refs/heads/"}
    tip = _branch(source, "feature")
    assert _push(source, "origin", "feature").returncode == 0
    refused = _push(source, "origin", "--delete", "feature")
    assert refused.returncode != 0
    assert "Branches page" in refused.stdout + refused.stderr
    assert _rev(git_dir, "refs/heads/feature") == tip


def test_without_a_policy_every_branch_stays_fast_forward_only(forge):
    handler, source, git_dir, _app = forge
    _branch(source, "feature")
    assert _push(source, "origin", "feature").returncode == 0
    tip = _rev(git_dir, "refs/heads/feature")
    _rewrite(source)
    assert _push(source, "--force", "origin", "feature").returncode != 0
    assert _rev(git_dir, "refs/heads/feature") == tip


def test_a_missing_hook_fails_closed(forge):
    handler, source, git_dir, app = forge
    handler.query = {"rewrite": "refs/heads/"}
    _branch(source, "feature")
    assert _push(source, "origin", "feature").returncode == 0
    tip = _rev(git_dir, "refs/heads/feature")
    os.unlink(Path(app.rewrite_hooks_path) / "pre-receive")
    _rewrite(source)
    assert _push(source, "--force", "origin", "feature").returncode != 0
    assert _rev(git_dir, "refs/heads/feature") == tip


def _open_pull(git_dir, source, number, head, status="open"):
    worktree = source.parent / ("ledger-%d" % number)
    run(["git", "init", "-q", "-b", "ledger", str(worktree)], source.parent)
    record = worktree / ".forkmesh" / "pulls" / str(number) / "pull.md"
    record.parent.mkdir(parents=True)
    record.write_text(
        "---\nschema: forkmesh-pull-v1\nnumber: %d\ntitle: t\nbase: main\n"
        "head: %s\nstatus: %s\n---\n\nbody\n" % (number, head, status),
        encoding="utf-8")
    run(["git", "add", "."], worktree)
    run(["git", "-c", "user.name=a", "-c", "user.email=a@example.test",
         "commit", "-q", "-m", "pull"], worktree)
    run(["git", "push", "-q", str(git_dir),
         "HEAD:refs/heads/forkmesh/pull/%d" % number], worktree)


def test_a_forced_update_names_the_open_pulls_of_that_branch(forge):
    handler, source, git_dir, _app = forge
    handler.query = {"rewrite": "refs/heads/"}
    _branch(source, "feature")
    assert _push(source, "origin", "feature").returncode == 0
    _open_pull(git_dir, source, 1, "feature")
    _open_pull(git_dir, source, 2, "feature", status="merged")
    _open_pull(git_dir, source, 3, "feature-two")
    _open_pull(git_dir, source, 4, "feature")
    run(["git", "checkout", "-q", "feature"], source)
    _rewrite(source)
    assert _push(source, "--force", "origin", "feature").returncode == 0
    assert _updates(handler)["updates"][0]["pulls"] == [1, 4]


def _hook(tmp_path, git_dir, lines, rewrite="", default="refs/heads/main",
          next_hook=""):
    directory = gateway.install_push_rewrite_hook(
        tmp_path / "hooks", next_hook)
    environment = dict(os.environ, GIT_DIR=str(git_dir))
    environment[gateway.PUSH_HOOK_REWRITE_ENV] = rewrite
    environment[gateway.PUSH_HOOK_DEFAULT_ENV] = default
    return subprocess.run(
        [str(directory / "pre-receive")], input="".join(
            "%s %s %s\n" % line for line in lines),
        capture_output=True, text=True, env=environment, check=False)


def test_hook_refuses_rewrites_the_policy_does_not_name(forge, tmp_path):
    _handler, source, git_dir, _app = forge
    base = _rev(git_dir, "refs/heads/main")
    side = _commit(source, "side.txt")
    run(["git", "push", "-q", str(git_dir), "HEAD:refs/heads/x"], source)
    rewind = (side, base, "refs/heads/x")
    assert _hook(tmp_path, git_dir, [rewind]).returncode == 1
    assert _hook(tmp_path, git_dir, [rewind],
                 rewrite="refs/heads/x").returncode == 0
    assert _hook(tmp_path, git_dir, [rewind],
                 rewrite="refs/heads/y refs/heads/x").returncode == 0
    assert _hook(tmp_path, git_dir, [(base, side, "refs/heads/x")]
                 ).returncode == 0
    assert _hook(tmp_path, git_dir, [(ZERO, side, "refs/heads/new")]
                 ).returncode == 0
    assert _hook(tmp_path, git_dir, [(side, base, "refs/tags/t")]
                 ).returncode == 0


def test_hook_hands_an_admitted_push_to_the_operator_hook(forge, tmp_path):
    _handler, source, git_dir, _app = forge
    base = _rev(git_dir, "refs/heads/main")
    side = _commit(source, "side.txt")
    run(["git", "push", "-q", str(git_dir), "HEAD:refs/heads/x"], source)
    seen = tmp_path / "seen"
    operator = tmp_path / "operator hook"
    operator.write_text(
        "#!/bin/sh\ncat > '%s'\nexit 7\n" % seen, encoding="utf-8")
    operator.chmod(0o700)
    line = (side, base, "refs/heads/x")
    admitted = _hook(tmp_path, git_dir, [line], rewrite="refs/heads/x",
                     next_hook=str(operator))
    assert admitted.returncode == 7
    assert seen.read_text(encoding="utf-8") == "%s %s %s\n" % line
    seen.unlink()
    refused = _hook(tmp_path, git_dir, [line], next_hook=str(operator))
    assert refused.returncode == 1 and not seen.exists()


@pytest.mark.parametrize("value", [
    "refs/tags/", "refs/heads", "refs/heads/../x", "main",
    " ".join("refs/heads/b%d" % index for index in range(9)),
])
def test_malformed_rewrite_grants_are_refused(value):
    with pytest.raises(gateway.GatewayError):
        gateway.parse_rewrite_refs(value)


def test_rewritable_refs_excludes_default_protected_and_new_refs():
    one, two = "1" * 40, "2" * 40
    commands = [
        (one, two, "refs/heads/main"), (one, two, "refs/heads/feature"),
        (one, two, "refs/heads/release/1"), (ZERO, two, "refs/heads/new"),
        (one, two, "refs/heads/forkmesh/pull/1"), (one, two, "refs/tags/v1"),
        (one, two, "refs/heads/pr/bob/x"),
    ]
    assert gateway.rewritable_refs(
        commands, ["refs/heads/"], ["refs/heads/release/"],
        "refs/heads/main") == {"refs/heads/feature", "refs/heads/pr/bob/x"}
    assert gateway.rewritable_refs(
        commands, ["refs/heads/pr/bob/"], [], "refs/heads/main") == {
            "refs/heads/pr/bob/x"}
