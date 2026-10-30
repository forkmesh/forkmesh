#!/usr/bin/env python3
"""Contributor pushes into a hosted repository's pr/<account>/ namespace.

An operator lists repositories in HOSTED_GIT_CONTRIBUTOR_PUSH. Any active,
email-verified user account may then mint the ordinary session-bound push
credential for them and push branches under refs/heads/pr/<account>/; the
routing Worker signs that namespace into the receive-pack capability and the
gateway refuses every other ref. Owners and writers keep their full access.
"""

import asyncio
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent
for path in (ROOT / "src", REPO / "tools", ROOT / "tests"):
    if str(path) not in sys.path:
        sys.path.insert(0, str(path))

import edge_routing  # noqa: E402
import mirror_gateway  # noqa: E402
import test_hosted_repositories as hosted  # noqa: E402
from test_branch_protection import receive_pack_body  # noqa: E402

ENTRY = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
CORE_PUSH = (ROOT / "src" / "core_push.py").read_text(encoding="utf-8")
GATEWAY = (REPO / "tools" / "mirror_gateway.py").read_text(encoding="utf-8")
ACCOUNT_JS = (
    ROOT / "public" / "dashboard" / "js" / "04-account.js"
).read_text(encoding="utf-8")
SETTINGS_HTML = (
    ROOT / "public" / "dashboard" / "partials" / "views" / "settings.html"
).read_text(encoding="utf-8")

ZERO, ONE, TWO = "0" * 40, "1" * 40, "2" * 40


class _Env(hosted._Env):
    HOSTED_GIT_CONTRIBUTOR_PUSH = "acme/platform"


def _contributor_world(*, verified=True, status="active", kind="user",
                       private=False, listed=True, org_write=False,
                       owner_of=(), account="bob", **kwargs):
    recorder = hosted._Recorder()
    module = hosted._install(
        recorder=recorder, account=account, owner_of=owner_of,
        org_write=org_write, **kwargs)
    users = {
        "bob": {"name": "bob", "status": status, "kind": kind,
                "email_verified": verified},
        "alice": {"name": "alice", "status": "active", "kind": "user",
                  "email_verified": True},
    }

    async def _account_row(_env, name):
        return "bi:" + name, users.get(name)

    async def _repo_is_private(_env, _owner, _repo):
        return private

    module._account_row = _account_row
    module._repo_is_private = _repo_is_private
    module._account_kind = lambda rec: (rec or {}).get("kind", "node")
    env = _Env()
    if not listed:
        env.HOSTED_GIT_CONTRIBUTOR_PUSH = ""
    return module, recorder, env


def _session_row():
    # The fake session belongs to whichever account the test names.
    return {"account_bi": "bi:bob", "revoked_at": 0,
            "expires_at": hosted.NOW + 60_000}


def _bob_header(module, owner="acme", repo="platform"):
    password = module.hosted_push_password(
        _Env(), "bob", owner, repo, hosted.SESSION_ID, hosted.NOW + 60_000)
    return hosted._basic("bob", password)


# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------


def test_the_allowlist_parses_strictly_and_matches_alias_or_node():
    module, _recorder, _env = _contributor_world()

    class Env:
        HOSTED_GIT_CONTRIBUTOR_PUSH = (
            " Acme/Platform, org/*  bad entry ../x * ")

    assert module.contributor_push_patterns(Env()) == [
        "acme/platform", "org/*", "*"]
    Env.HOSTED_GIT_CONTRIBUTOR_PUSH = "acme/platform org/*"
    assert module.contributor_push_enabled(Env(), ("acme",), "platform")
    assert module.contributor_push_enabled(Env(), ("", "acme"), "PLATFORM")
    assert module.contributor_push_enabled(Env(), ("org",), "anything")
    assert not module.contributor_push_enabled(Env(), ("acme",), "other")
    assert not module.contributor_push_enabled(Env(), ("bob",), "platform")
    assert module.contributor_push_patterns(object()) == []
    assert not module.contributor_push_enabled(object(), ("acme",), "x")


@pytest.mark.parametrize("account, prefix", [
    ("bob", "refs/heads/pr/bob/"),
    ("Bob-2", "refs/heads/pr/bob-2/"),
    ("a.b_c", "refs/heads/pr/a.b_c/"),
    ("", ""), ("-x", ""), ("a..b", ""), ("a.lock", ""), ("a.", ""),
    ("a/b", ""), ("a b", ""),
])
def test_the_namespace_is_ref_safe(account, prefix):
    module, _recorder, _env = _contributor_world()
    assert module.contributor_ref_prefix(account) == prefix


# ---------------------------------------------------------------------------
# Who counts as a contributor
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("changes, allowed", [
    ({}, True),
    ({"verified": False}, False),
    ({"verified": None}, False),
    ({"status": "suspended"}, False),
    ({"kind": "node"}, False),
    ({"private": True}, False),
    ({"listed": False}, False),
])
def test_only_active_verified_users_on_listed_public_repositories(
        changes, allowed):
    module, _recorder, env = _contributor_world(**changes)
    assert asyncio.run(module.contributor_push_allowed(
        env, "acme", "platform", "bob")) is allowed


def test_a_listed_org_alias_admits_the_backing_node():
    module, _recorder, env = _contributor_world()
    env.HOSTED_GIT_CONTRIBUTOR_PUSH = "acme-org/platform"
    assert asyncio.run(module.contributor_push_allowed(
        env, "acme", "platform", "bob", alias="acme-org"))
    assert not asyncio.run(module.contributor_push_allowed(
        env, "acme", "platform", "bob"))


# ---------------------------------------------------------------------------
# Verifying the credential
# ---------------------------------------------------------------------------


def test_a_contributor_credential_carries_only_its_namespace():
    module, recorder, env = _contributor_world(session=_session_row())
    header = _bob_header(module)
    assert asyncio.run(module.verify_hosted_push_access(
        env, "acme", "platform", header, "POST")) == (
            "bob", "refs/heads/pr/bob/")
    assert recorder.audits[-1] == (
        "bob", "git.push_authorize", "repository", "acme/platform",
        "success", {"operation": "git-receive-pack", "method": "POST",
                    "credential": "session-mac",
                    "refPrefix": "refs/heads/pr/bob/"})
    # The legacy full-access check never answers for a contributor.
    assert asyncio.run(module.verify_hosted_push_credential(
        env, "acme", "platform", header, "POST")) == ""


def test_a_contributor_signing_in_with_a_password_gets_only_its_namespace():
    module, recorder, env = _contributor_world(account="alice")
    hosted._with_password_login(module)
    header = hosted._basic("alice@example.test", hosted.ALICE_PASSWORD)
    assert asyncio.run(module.verify_hosted_push_access(
        env, "acme", "platform", header, "POST",
        request=hosted._Request())) == ("alice", "refs/heads/pr/alice/")
    assert recorder.audits[-1][-1] == {
        "operation": "git-receive-pack", "method": "POST",
        "credential": "password", "refPrefix": "refs/heads/pr/alice/"}


def test_a_writer_keeps_full_access_with_no_prefix():
    module, _recorder, env = _contributor_world(
        session=_session_row(), org_write=True)
    assert asyncio.run(module.verify_hosted_push_access(
        env, "acme", "platform", _bob_header(module), "POST")) == ("bob", "")


@pytest.mark.parametrize("changes", [
    {"verified": False}, {"listed": False}, {"private": True},
])
def test_an_ineligible_contributor_is_refused_and_audited(changes):
    module, recorder, env = _contributor_world(
        session=_session_row(), **changes)
    assert asyncio.run(module.verify_hosted_push_access(
        env, "acme", "platform", _bob_header(module), "POST")) == ("", "")
    assert recorder.audits[-1][4] == "denied"


def test_a_forged_credential_never_reaches_the_contributor_check():
    module, recorder, env = _contributor_world(session=_session_row())
    header = hosted._basic("bob", "fmp1." + "S" * 32 + ".1783000060000."
                           + "0" * 64)
    assert asyncio.run(module.verify_hosted_push_access(
        env, "acme", "platform", header, "POST")) == ("", "")
    assert recorder.audits == []


# ---------------------------------------------------------------------------
# Minting it from the website
# ---------------------------------------------------------------------------


def _mint(module, env, owner="acme", name="platform"):
    return asyncio.run(module.hosted_repository_handler(
        env,
        hosted._Request({"owner": owner, "name": name,
                         "sessionToken": hosted.SESSION_TOKEN}),
        "/api/hosted-repositories/push-credential"))


def test_a_contributor_mints_a_namespaced_credential():
    module, _recorder, env = _contributor_world(session=_session_row())
    response = _mint(module, env)
    assert response.status == 200
    push = response.data["push"]
    assert push["access"] == "contributor"
    assert push["refPrefix"] == "refs/heads/pr/bob/"
    assert push["branchPrefix"] == "pr/bob/"
    assert push["username"] == "bob"
    assert "git push origin pr/bob/my-change" in push["commands"]
    assert asyncio.run(module.verify_hosted_push_access(
        env, "acme", "platform", hosted._basic("bob", push["password"]),
        "POST")) == ("bob", "refs/heads/pr/bob/")


def test_a_writer_mints_a_write_credential():
    module, _recorder, env = _contributor_world(
        session=_session_row(), org_write=True)
    assert _mint(module, env).data["push"]["access"] == "write"


def test_an_unverified_contributor_is_told_why():
    module, _recorder, env = _contributor_world(verified=False)
    response = _mint(module, env)
    assert response.status == 403
    assert "verified email" in response.data["message"]
    module, _recorder, env = _contributor_world(listed=False)
    response = _mint(module, env)
    assert response.status == 403
    assert "contributor pushes to be enabled" in response.data["message"]


def test_settings_advertise_the_listed_repositories():
    module, _recorder, env = _contributor_world()
    response = asyncio.run(module.hosted_repository_handler(
        env, hosted._Request(method="GET"), "/api/hosted-repositories"))
    assert response.data["contributorPush"] == ["acme/platform"]


# ---------------------------------------------------------------------------
# The gateway enforces the namespace
# ---------------------------------------------------------------------------


def _commands(*updates):
    return mirror_gateway.receive_pack_commands(
        receive_pack_body(*updates), 16)


def test_the_gateway_parses_only_a_well_formed_namespace():
    assert mirror_gateway.parse_push_prefix("") == ""
    assert mirror_gateway.parse_push_prefix(
        "refs/heads/pr/bob/") == "refs/heads/pr/bob/"
    for bad in ("refs/heads/pr/bob", "refs/heads/main/", "refs/heads/pr//",
                "refs/heads/pr/a/b/", "refs/heads/pr/a..b/",
                "refs/tags/pr/bob/", "refs/heads/pr/Bob/"):
        with pytest.raises(mirror_gateway.GatewayError):
            mirror_gateway.parse_push_prefix(bad)
    assert "pushPrefix" in mirror_gateway.OPERATION_QUERY_FIELDS[
        "git-receive-pack"]


def test_the_gateway_refuses_refs_outside_the_namespace():
    prefix = "refs/heads/pr/bob/"
    mirror_gateway.enforce_push_prefix(
        _commands((ZERO, ONE, prefix + "fix"),
                  (ONE, TWO, prefix + "topic/deeper")), prefix, {})
    for ref in ("refs/heads/main", "refs/heads/pr/alice/fix",
                "refs/heads/pr/bobby/fix", "refs/tags/v1", prefix[:-1]):
        with pytest.raises(mirror_gateway.GatewayError, match="pr/bob/"):
            mirror_gateway.enforce_push_prefix(
                _commands((ZERO, ONE, prefix + "ok"), (ZERO, ONE, ref)),
                prefix, {})
    # No prefix: nothing to enforce (owners and writers).
    mirror_gateway.enforce_push_prefix(
        _commands((ONE, TWO, "refs/heads/main")), "", None)


def test_the_gateway_caps_a_contributors_branches():
    prefix = "refs/heads/pr/bob/"
    limit = mirror_gateway.MAX_CONTRIBUTOR_BRANCHES
    existing = {prefix + "b%d" % index: ONE for index in range(limit)}
    # Updating an existing branch at the cap is fine; a new one is not.
    mirror_gateway.enforce_push_prefix(
        _commands((ONE, TWO, prefix + "b0")), prefix, existing)
    with pytest.raises(mirror_gateway.GatewayError, match="at most"):
        mirror_gateway.enforce_push_prefix(
            _commands((ZERO, ONE, prefix + "new")), prefix, existing)


def test_prefix_refs_lists_only_the_namespace(tmp_path):
    work = tmp_path / "work"
    bare = tmp_path / "repo.git"
    env = {"GIT_CONFIG_GLOBAL": "/dev/null", "GIT_CONFIG_NOSYSTEM": "1",
           "HOME": str(tmp_path), "PATH": "/usr/bin:/bin"}

    def git(*args, cwd=None):
        subprocess.run(["git", *args], cwd=cwd, env=env, check=True,
                       capture_output=True)

    git("init", "-q", "-b", "main", str(work))
    git("-c", "user.name=t", "-c", "user.email=t@example.test", "commit",
        "-q", "--allow-empty", "-m", "one", cwd=work)
    git("clone", "-q", "--bare", str(work), str(bare))
    git("--git-dir", str(bare), "branch", "pr/bob/fix", "main")
    git("--git-dir", str(bare), "branch", "pr/bobby/fix", "main")
    git("--git-dir", str(bare), "config", "--add", "safe.directory", "*")
    refs = mirror_gateway.prefix_refs(bare, "refs/heads/pr/bob/")
    assert list(refs) == ["refs/heads/pr/bob/fix"]


def test_the_receive_pack_route_enforces_the_signed_namespace():
    route = GATEWAY.split('if operation == "git-receive-pack":', 1)[1].split(
        "self._run_receive_pack(", 1)[0]
    assert "PUSH_PREFIX_QUERY_FIELD" in route
    assert "parse_push_prefix(" in route
    assert "enforce_push_prefix(" in route
    assert "prefix_refs(repository.git_dir, push_prefix)" in route


# ---------------------------------------------------------------------------
# The routing Worker signs it and keeps contributors away from owner paths
# ---------------------------------------------------------------------------


def test_the_signed_target_carries_the_namespace():
    url = edge_routing.masked_target_url(
        "https://git.example.test", "acme", "platform", "git-receive-pack",
        {"pushPrefix": "refs/heads/pr/bob/"})
    assert url.endswith("?pushPrefix=refs%2Fheads%2Fpr%2Fbob%2F")


def test_the_push_proxy_threads_the_namespace_into_the_capability():
    proxy = ENTRY.split("async def _https_mirror_push_proxy(", 1)[1].split(
        "\nasync def _https_mirror_push_send(", 1)[0]
    assert "await verify_hosted_push_access(" in proxy
    assert "alias=alias_owner" in proxy
    # The organization-owner rule for core-held repositories applies to full
    # pushes; a contributor's push is confined by its prefix instead.
    assert "if core_held and not push_prefix and not await" in proxy
    assert "push_prefix=push_prefix" in proxy
    send = ENTRY.split("async def _https_mirror_push_send(", 1)[1].split(
        "\ndef _cron_due_phases(", 1)[0]
    assert "query = dict(query or {}, pushPrefix=push_prefix)" in send
    assert send.index("pushPrefix=") < send.index("masked_target_url(")
    assert "and not push_prefix else {}" in send
    assert "contributor=bool(push_prefix)" in send
    assert "not contributor:" in CORE_PUSH.split(
        "async def record_accepted_push(", 1)[1].split("\ndef ", 1)[0]


# ---------------------------------------------------------------------------
# Settings page
# ---------------------------------------------------------------------------


def test_settings_page_explains_how_to_push():
    assert "data-git-push-panel" in SETTINGS_HTML
    assert "data-git-push-repo" in SETTINGS_HTML
    assert "data-git-push-mint" in SETTINGS_HTML
    assert "/api/hosted-repositories/push-credential" in ACCOUNT_JS
    assert "branchPrefix" in ACCOUNT_JS
    # An unconfigured SSH gateway says so plainly and points at HTTPS.
    assert "SSH pushes are not available on this server" in ACCOUNT_JS


# ---------------------------------------------------------------------------
# End to end: a stock git push through the gateway with a signed namespace
# ---------------------------------------------------------------------------


@pytest.fixture
def contributor_gateway(tmp_path, monkeypatch):
    import threading
    from http.server import ThreadingHTTPServer
    from urllib.parse import quote

    from test_core_push_ref_policy import _GrantingHandler, _push
    from test_mirror_gateway import (
        gateway, make_push_application, make_push_client,
        push_capability_headers)

    class Handler(_GrantingHandler):
        push_prefix = ""

        def do_POST(self):
            length = int(self.headers.get("Content-Length") or 0)
            body = self.rfile.read(length)
            parts = self.path.split("/")
            target = "/v1/repositories/%s/%s/%s" % (
                parts[1], parts[2], parts[3])
            if type(self).push_prefix:
                target += "?pushPrefix=" + quote(
                    type(self).push_prefix, safe="")
            headers = push_capability_headers(body)
            headers["Content-Type"] = self.headers.get("Content-Type", "")
            response = self.application.dispatch("POST", target, headers, body)
            type(self).responses.append(response)
            self._reply(response.status, response.content_type, response.body)

    app, root, _config_path, _logs = make_push_application(tmp_path)
    monkeypatch.setattr(
        gateway, "REFRESH_PENDING_PATH", tmp_path / "missing" / "pending")
    handler = type("H", (Handler,), {
        "application": app, "push_prefix": "", "responses": []})
    server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    origin = "http://127.0.0.1:%d/mobile-owner/app" % server.server_address[1]
    source = make_push_client(tmp_path, origin)
    # The owner's first push, with no namespace.
    first = _push(source, "origin", "main")
    assert first.returncode == 0, first.stderr
    yield handler, source, root / "mobile-owner" / "app.git", _push
    server.shutdown()
    server.server_close()
    thread.join(timeout=5)
    app.close()


def test_a_stock_git_contributor_push_lands_only_in_its_namespace(
        contributor_gateway):
    from test_core_push_ref_policy import _commit, _rev
    handler, source, git_dir, push = contributor_gateway
    main = _rev(git_dir, "refs/heads/main")
    handler.push_prefix = "refs/heads/pr/bob/"
    change = _commit(source, "fix.txt")

    for refused in ("main", "HEAD:refs/heads/topic",
                    "HEAD:refs/heads/pr/alice/fix"):
        result = push(source, "origin", refused)
        assert result.returncode != 0
        # A per-ref refusal git prints, not an opaque HTTP failure.
        assert "remote rejected" in result.stdout
        assert "pr/bob/" in result.stdout
    assert _rev(git_dir, "refs/heads/main") == main
    listed = subprocess.run(
        ["git", "--git-dir", str(git_dir), "for-each-ref",
         "--format=%(refname)"], capture_output=True, text=True,
        check=True).stdout.split()
    assert listed == ["refs/heads/main"]

    accepted = push(source, "origin", "HEAD:refs/heads/pr/bob/fix")
    assert accepted.returncode == 0, accepted.stderr
    assert _rev(git_dir, "refs/heads/pr/bob/fix") == change
    assert _rev(git_dir, "refs/heads/main") == main


def test_a_refusal_report_is_framed_the_way_the_client_asked():
    commands = [(ZERO, ONE, "refs/heads/main")]

    def body(capabilities):
        text = "%s %s refs/heads/main\0%s\n" % (ZERO, ONE, capabilities)
        return ("%04x%s0000" % (len(text) + 4, text)).encode()

    report = mirror_gateway.receive_pack_rejection(
        body("report-status"), commands, "no")
    assert report == (b"000eunpack ok\n" b"001ang refs/heads/main no\n"
                      b"0000")
    framed = mirror_gateway.receive_pack_rejection(
        body("report-status side-band-64k"), commands, "no")
    assert framed == b"%04x\x01" % (len(report) + 5) + report + b"0000"
    assert mirror_gateway.receive_pack_rejection(
        body("ofs-delta"), commands, "no") == b"0000"
