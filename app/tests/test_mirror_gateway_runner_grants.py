"""Runner grants: one clone grant, one push grant for exactly one agent ref.

A Phase 5 runner is untrusted compute. The gateway lets it clone the one
repository its job names and push exactly ``refs/heads/agent/<session>`` of
it, fast-forward only. Every other ref update, a tag, a delete, or a second
command in the same push is refused before git runs.
"""

import base64
import os
import shutil
import subprocess
import sys
import threading
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from test_mirror_gateway import (  # noqa: E402
    NOW,
    SIGNATURE,
    AcceptVerifier,
    FakeArchiveMaterializer,
    HealthSigner,
    gateway,
    make_bare_repository,
    run,
    write_config,
)


AGENT_REF = "refs/heads/agent/session-7f3a"
ZERO = "0" * 40


def b64(data):
    return base64.urlsafe_b64encode(data).decode().rstrip("=")


def grant(
    access="push",
    ref=AGENT_REF,
    *,
    node="mirror-a",
    owner="alice",
    repository="project",
    runner="fmnext-run-1",
    grant_id="grant_0123456789",
    issued_at=NOW - 1000,
    expires_at=NOW + 3_600_000,
    signature=SIGNATURE,
):
    if access == "read":
        ref = ""
    message = gateway.runner_grant_message(
        node, owner, repository, access, ref, runner, grant_id,
        issued_at, expires_at)
    assert message
    return b64(message.encode()) + "." + signature


@pytest.fixture
def served(tmp_path):
    bare, commit = make_bare_repository(tmp_path)
    config = gateway.load_config(write_config(tmp_path, bare))
    verifier = AcceptVerifier()
    app = gateway.GatewayApplication(
        config,
        verifier=verifier,
        health_signer=HealthSigner(),
        materializer=FakeArchiveMaterializer(bare),
        clock_ms=lambda: NOW,
        log=lambda _entry: None,
    )
    yield app, bare, commit, verifier, tmp_path
    app.close()


def refs(bare):
    return run(["git", "for-each-ref", "--format=%(refname) %(objectname)"], bare)


def pkt(line):
    data = line.encode() if isinstance(line, str) else line
    return b"%04x" % (len(data) + 4) + data


def new_commit(tmp_path, bare, parent_ref="main", name="work"):
    """A commit on top of parent_ref, built outside the served repository."""
    work = tmp_path / name
    if not work.exists():
        run(["git", "clone", "--quiet", str(bare), str(work)], tmp_path)
        run(["git", "config", "user.email", "r@example.test"], work)
        run(["git", "config", "user.name", "Runner"], work)
    run(["git", "checkout", "--quiet", "--detach", "origin/" + parent_ref],
        work)
    (work / (name + ".txt")).write_text(name + os.urandom(4).hex(), encoding="utf-8")
    run(["git", "add", "."], work)
    run(["git", "commit", "--quiet", "-m", "agent " + name], work)
    return work, run(["git", "rev-parse", "HEAD"], work)


def pack_for(work, new, old):
    revs = new + "\n" + ("^" + old + "\n" if old and old != ZERO else "")
    return subprocess.run(
        ["git", "pack-objects", "--stdout", "--revs"],
        cwd=work, input=revs.encode(), capture_output=True, check=True,
    ).stdout


def push_body(commands, pack=b""):
    body = b""
    for index, (old, new, ref) in enumerate(commands):
        line = f"{old} {new} {ref}"
        if index == 0:
            line += "\0report-status side-band-64k"
        body += pkt(line + "\n")
    return body + b"0000" + pack


def receive(app, token, body, owner="alice", repo="project"):
    headers = {"Content-Type": "application/x-git-receive-pack-request"}
    if token is not None:
        headers["X-ForkMesh-Runner-Grant"] = token
    return app.dispatch(
        "POST", f"/v1/runner/{owner}/{repo}.git/git-receive-pack",
        headers, body)


def test_grant_message_is_the_shared_golden_string():
    assert gateway.runner_grant_message(
        "mirror-a", "alice", "project", "push", AGENT_REF, "fmnext-run-1",
        "grant_0123456789", 1000, 2000,
    ) == (
        "forkmesh-runner-grant-v1\nmirror-a\nalice\nproject\npush\n"
        "refs/heads/agent/session-7f3a\nfmnext-run-1\ngrant_0123456789\n"
        "1000\n2000"
    )
    assert gateway.runner_grant_message(
        "mirror-a", "alice", "project", "read", "", "fmnext-run-1",
        "grant_0123456789", 1000, 2000,
    ).split("\n")[5] == ""
    bad = [
        ("read", "refs/heads/agent/x", 1000, 2000),
        ("push", "", 1000, 2000),
        ("push", "refs/heads/main", 1000, 2000),
        ("push", "refs/heads/agent/../main", 1000, 2000),
        ("push", "refs/heads/agent/x.lock", 1000, 2000),
        ("push", "refs/heads/agent/a//b", 1000, 2000),
        ("push", "refs/heads/agent/a@{1}", 1000, 2000),
        ("push", "refs/heads/agent/", 1000, 2000),
        ("push", "refs/tags/agent/x", 1000, 2000),
        ("admin", "", 1000, 2000),
        ("push", AGENT_REF, 2000, 2000),
        ("push", AGENT_REF, 1000, 1000 + 10_800_001),
        ("push", AGENT_REF, 0, 10),
    ]
    for access, ref, issued, expires in bad:
        assert gateway.runner_grant_message(
            "mirror-a", "alice", "project", access, ref, "fmnext-run-1",
            "grant_0123456789", issued, expires) == "", (access, ref)


def test_token_parse_rejects_malformed_tokens():
    good = grant()
    assert gateway.parse_runner_grant(good).ref == AGENT_REF
    message = good.split(".")[0]
    for token in (
        "", "abc", good + ".x", message + ".short", "!!!." + SIGNATURE,
        b64(b"forkmesh-runner-grant-v1\nmirror-a") + "." + SIGNATURE,
        b64(b"forkmesh-runner-grant-v2" + base64.urlsafe_b64decode(
            message + "=" * (-len(message) % 4))[24:]) + "." + SIGNATURE,
        # Non-canonical: trailing newline.
        b64(base64.urlsafe_b64decode(message + "=" * (-len(message) % 4))
            + b"\n") + "." + SIGNATURE,
    ):
        with pytest.raises(gateway.GatewayError):
            gateway.parse_runner_grant(token)


def test_read_grant_clones_and_push_grant_does_not_read(served):
    app, _bare, _commit, verifier, _tmp = served
    target = "/v1/runner/alice/project.git/info/refs?service=git-upload-pack"
    advert = app.dispatch(
        "GET", target, {"X-ForkMesh-Runner-Grant": grant("read")}, b"")
    assert advert.status == 200
    assert advert.body.startswith(b"001e# service=git-upload-pack\n0000")
    assert verifier.messages[-1][0].startswith("forkmesh-runner-grant-v1\n")
    upload = app.dispatch(
        "POST", "/v1/runner/alice/project.git/git-upload-pack",
        {"X-ForkMesh-Runner-Grant": grant("read"),
         "Content-Type": "application/x-git-upload-pack-request"},
        b"0000")
    assert upload.status == 200 and upload.stream.kind == "process"
    # A push grant is not a read grant, and no grant is no access at all.
    assert app.dispatch(
        "GET", target, {"X-ForkMesh-Runner-Grant": grant("push")}, b""
    ).status == 404
    assert app.dispatch("GET", target, {}, b"").status == 404
    # And a read grant does not open the receive-pack side.
    assert app.dispatch(
        "GET",
        "/v1/runner/alice/project.git/info/refs?service=git-receive-pack",
        {"X-ForkMesh-Runner-Grant": grant("read")}, b"",
    ).status == 404
    advert = app.dispatch(
        "GET",
        "/v1/runner/alice/project.git/info/refs?service=git-receive-pack",
        {"X-ForkMesh-Runner-Grant": grant("push")}, b"")
    assert advert.status == 200
    assert b"service=git-receive-pack" in advert.body


def test_push_grant_moves_exactly_its_agent_ref(served):
    app, bare, commit, _verifier, tmp = served
    before = refs(bare)
    work, new = new_commit(tmp, bare)
    response = receive(app, grant(), push_body(
        [(ZERO, new, AGENT_REF)], pack_for(work, new, ZERO)))
    assert response.status == 200, response.body
    assert b"ok " + AGENT_REF.encode() in response.body
    after = refs(bare)
    assert run(["git", "rev-parse", AGENT_REF], bare) == new
    # Nothing but the one agent ref changed.
    assert set(after.splitlines()) - set(before.splitlines()) == {
        f"{AGENT_REF} {new}"}
    assert run(["git", "rev-parse", "refs/heads/main"], bare) == commit

    # A later fast-forward of the same ref is accepted too.
    run(["git", "fetch", "--quiet", "origin"], work)
    (work / "more.txt").write_text("more", encoding="utf-8")
    run(["git", "add", "."], work)
    run(["git", "commit", "--quiet", "-m", "more"], work)
    newer = run(["git", "rev-parse", "HEAD"], work)
    response = receive(app, grant(), push_body(
        [(new, newer, AGENT_REF)], pack_for(work, newer, new)))
    assert response.status == 200
    assert run(["git", "rev-parse", AGENT_REF], bare) == newer


@pytest.mark.parametrize("ref", [
    "refs/heads/main",
    "refs/tags/x",
    "refs/heads/agent/other-session",
    "refs/forkmesh/merge-jobs/x",
])
def test_push_outside_the_grant_ref_is_refused_before_git(served, ref):
    app, bare, _commit, _verifier, tmp = served
    before = refs(bare)
    work, new = new_commit(tmp, bare)
    old = ZERO
    if ref == "refs/heads/main":
        old = run(["git", "rev-parse", "refs/heads/main"], bare)
    response = receive(app, grant(), push_body(
        [(old, new, ref)], pack_for(work, new, old)))
    assert response.status == 404
    assert refs(bare) == before


def test_second_command_delete_and_force_are_refused(served):
    app, bare, _commit, _verifier, tmp = served
    work, new = new_commit(tmp, bare)
    main = run(["git", "rev-parse", "refs/heads/main"], bare)
    before = refs(bare)
    # The allowed ref plus any other in the same push: whole push refused.
    both = receive(app, grant(), push_body(
        [(ZERO, new, AGENT_REF), (main, new, "refs/heads/main")],
        pack_for(work, new, ZERO)))
    assert both.status == 404
    # The same command twice is still more than one command.
    twice = receive(app, grant(), push_body(
        [(ZERO, new, AGENT_REF), (ZERO, new, AGENT_REF)],
        pack_for(work, new, ZERO)))
    assert twice.status == 404
    assert refs(bare) == before

    assert receive(app, grant(), push_body(
        [(ZERO, new, AGENT_REF)], pack_for(work, new, ZERO))).status == 200
    # Deleting the granted ref is refused.
    delete = receive(app, grant(), push_body([(new, ZERO, AGENT_REF)]))
    assert delete.status == 404
    assert run(["git", "rev-parse", AGENT_REF], bare) == new
    # A non-fast-forward rewrite of the granted ref: git's own
    # receive.denyNonFastForwards refuses it.
    _other, rewritten = new_commit(tmp, bare, name="rewrite")
    forced = receive(app, grant(), push_body(
        [(new, rewritten, AGENT_REF)], pack_for(_other, rewritten, ZERO)))
    assert b"ok " + AGENT_REF.encode() not in forced.body
    assert run(["git", "rev-parse", AGENT_REF], bare) == new


def test_malformed_command_sections_are_refused():
    oid = "a" * 40
    for body in (
        b"0000",  # no commands
        b"00",  # truncated
        b"zzzz",  # not hex
        pkt(f"{oid} {oid}\n") + b"0000",  # missing ref
        pkt(f"{oid[:39]} {oid} {AGENT_REF}\n") + b"0000",  # short oid
        pkt(f"{oid} {'b' * 64} {AGENT_REF}\n") + b"0000",  # mixed lengths
        pkt(f"{oid} {oid} {AGENT_REF}\0push-cert=nonce\n") + b"0000",
        pkt("push-cert\0report-status\n") + b"0000",
        pkt(f"{oid} {oid} {AGENT_REF}\n"),  # no flush
    ):
        with pytest.raises(gateway.GatewayError):
            gateway.enforce_runner_push_scope(
                gateway.receive_pack_commands(body), AGENT_REF)
    shallow = pkt("shallow " + oid + "\n") + pkt(
        f"{ZERO} {oid} {AGENT_REF}\0report-status\n") + b"0000PACK"
    assert gateway.receive_pack_commands(shallow) == [(ZERO, oid, AGENT_REF)]


@pytest.mark.parametrize("change", [
    {"node": "mirror-b"},
    {"owner": "bob"},
    {"repository": "other"},
    {"issued_at": NOW - 7_200_000, "expires_at": NOW - 1},
    {"issued_at": NOW + 120_000, "expires_at": NOW + 3_600_000},
    {"signature": base64.urlsafe_b64encode(b"X" * 64).decode().rstrip("=")},
])
def test_grants_for_elsewhere_expired_or_forged_are_refused(served, change):
    app, bare, _commit, _verifier, tmp = served
    before = refs(bare)
    work, new = new_commit(tmp, bare)
    response = receive(app, grant(**change), push_body(
        [(ZERO, new, AGENT_REF)], pack_for(work, new, ZERO)))
    assert response.status == 404
    assert refs(bare) == before


def test_unknown_repository_is_never_created(served):
    app, bare, _commit, _verifier, tmp = served
    work, new = new_commit(tmp, bare)
    response = receive(
        app, grant(repository="brand-new"),
        push_body([(ZERO, new, AGENT_REF)], pack_for(work, new, ZERO)),
        repo="brand-new")
    assert response.status == 404
    disabled = receive(
        app, grant(repository="private-project"),
        push_body([(ZERO, new, AGENT_REF)], pack_for(work, new, ZERO)),
        repo="private-project")
    assert disabled.status == 404


def test_body_limit_widens_only_for_a_verified_push_grant(served):
    app, *_rest = served
    target = "/v1/runner/alice/project.git/git-receive-pack"
    wide = max(gateway.MAX_REQUEST_BODY, app.config.receive_max_input_bytes)
    assert app.request_body_limit(
        "POST", target, {"X-ForkMesh-Runner-Grant": grant()}) == wide
    assert app.request_body_limit(
        "POST", target, {"X-ForkMesh-Runner-Grant": grant("read")}
    ) == gateway.MAX_REQUEST_BODY
    assert app.request_body_limit(
        "POST", target, {}) == gateway.MAX_REQUEST_BODY


@pytest.mark.skipif(shutil.which("git") is None, reason="git is required")
def test_real_git_clone_and_push_over_http(served):
    app, bare, commit, _verifier, tmp = served
    server = gateway.MirrorGatewayServer(("127.0.0.1", 0), app)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    env = dict(os.environ, GIT_TERMINAL_PROMPT="0", HOME=str(tmp),
               XDG_CONFIG_HOME=str(tmp / "xdg"), GIT_CONFIG_NOSYSTEM="1")
    env.pop("FORKMESH_HOME", None)
    env.pop("FORKMESH_PROJECTS_DIR", None)
    url = f"http://127.0.0.1:{server.server_address[1]}/v1/runner/alice/project.git"

    def git(*args, token, cwd=tmp, check=True):
        return subprocess.run(
            ["git", "-c", "http.extraHeader=X-ForkMesh-Runner-Grant: " + token,
             *args], cwd=cwd, env=env, capture_output=True, text=True,
            check=check, timeout=60)

    try:
        clone = tmp / "runner-clone"
        git("clone", "--quiet", url, str(clone), token=grant("read"))
        assert run(["git", "rev-parse", "HEAD"], clone) == commit
        run(["git", "config", "user.email", "r@example.test"], clone)
        run(["git", "config", "user.name", "Runner"], clone)
        run(["git", "checkout", "--quiet", "-b", "agent/session-7f3a"], clone)
        (clone / "agent.txt").write_text("done", encoding="utf-8")
        run(["git", "add", "."], clone)
        run(["git", "commit", "--quiet", "-m", "agent work"], clone)
        tip = run(["git", "rev-parse", "HEAD"], clone)
        # Pushing with the read grant fails; the push grant succeeds.
        assert git("push", url, "HEAD:" + AGENT_REF, token=grant("read"),
                   cwd=clone, check=False).returncode != 0
        git("push", url, "HEAD:" + AGENT_REF, token=grant(), cwd=clone)
        assert run(["git", "rev-parse", AGENT_REF], bare) == tip
        # The same grant cannot move main or create a tag.
        assert git("push", url, "HEAD:refs/heads/main", token=grant(),
                   cwd=clone, check=False).returncode != 0
        assert git("push", url, "HEAD:refs/tags/v9", token=grant(),
                   cwd=clone, check=False).returncode != 0
        assert run(["git", "rev-parse", "refs/heads/main"], bare) == commit
        assert subprocess.run(
            ["git", "rev-parse", "--verify", "--quiet", "refs/tags/v9"],
            cwd=bare, capture_output=True).returncode != 0
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)
