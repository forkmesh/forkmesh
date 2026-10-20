#!/usr/bin/env python3
"""Empty repositories created from the web, and their push credential.

Issue #676: the create-repository modal offered a clone URL (the repository
already exists elsewhere), a local path (a desktop node must be running to
read it), or a provider import (an account on another forge). This covers the
fourth source: the relay creates an empty repository for a browser session and
answers with the Basic credential a stock ``git push`` uses, so the first push
creates the bare repository on the hosted write gateway.

``app/src/hosted_git.py`` is stdlib-only apart from the entrypoint primitives
entry.py binds into it, so these tests import it directly and bind fakes.
"""

import asyncio
import base64
import importlib
import re
import sys
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import catalog  # noqa: E402
import hosted_git  # noqa: E402

from _dashboard_bundle import assembled_dashboard_js  # noqa: E402

ENTRY = (SRC / "entry.py").read_text(encoding="utf-8")
APP_RUNTIME = (SRC / "app_runtime.py").read_text(encoding="utf-8")
MODULE = (SRC / "hosted_git.py").read_text(encoding="utf-8")
FOOTPRINT = (
    ROOT.parent / "world" / "tools" / "build_worker_footprint.py"
).read_text(encoding="utf-8")
NEW_REPO_MODAL = (
    ROOT / "public" / "dashboard" / "partials" / "new-repo-modal.html"
).read_text(encoding="utf-8")
REPO_LIST_JS = (
    ROOT / "public" / "dashboard" / "js" / "05-repo-list-explorer.js"
).read_text(encoding="utf-8")
NETWORK_JS = (
    ROOT / "public" / "dashboard" / "js" / "08-repo-detail-network.js"
).read_text(encoding="utf-8")
BUNDLE = assembled_dashboard_js()

NOW = 1_783_000_000_000
SESSION_ID = "S" * 32
SESSION_TOKEN = "v2." + SESSION_ID + "." + ("t" * 40)
WRITE_NODE = {
    "node": "hosted-write",
    "baseUrl": "https://write.example.test",
    "publicKey": "N" * 43,
}


# ---------------------------------------------------------------------------
# Test doubles for the primitives entry.py binds
# ---------------------------------------------------------------------------


class _Env:
    ADMIN_PASS = "admin-pass"
    DATA_KEY = "data-key"
    HOSTED_GIT_WRITE_NODE = "hosted-write"


class _Request:
    def __init__(self, body=None, method="POST",
                 url="https://app.forkmesh.test/api/hosted-repositories",
                 headers=None):
        self.body = dict(body or {})
        self.method = method
        self.url = url
        self.headers = headers or {}


class _Response:
    def __init__(self, data, status, headers):
        self.data = data
        self.status = status
        self.headers = headers


def _json_response(data, status=200, extra_headers=None, **_kwargs):
    return _Response(data, status, dict(extra_headers or {}))


class _Recorder:
    """Collects the audit trail and the catalog row a create would write."""

    def __init__(self):
        self.audits = []
        self.rows = []
        self.republished = []
        self.webhook_events = []
        self.statements = []


def _install(*, recorder, account="alice", owner_of=("alice",),
             write_node=WRITE_NODE, existing=(), repo_count=0,
             session=None, org_write=False, rate_limited=None,
             tombstone=False, stored=None,
             proof=("refs_mismatch", "d" * 64), org_nodes=None,
             core_nodes=(), core_owners=()):
    """Bind one scenario's fakes into the module under test."""
    session_row = {
        "account_bi": "bi:alice",
        "revoked_at": 0,
        "expires_at": NOW + 60_000,
    } if session is None else session

    async def _authed_account_name(_env, _request, _data=None):
        return account

    async def _account_owns_node(_env, actor, owner):
        return bool(actor) and actor == account and owner in owner_of

    async def _org_write_allowed(_env, _owner, _repo, _account):
        return org_write

    async def _org_repo_node(_env, org, repo):
        return (org_nodes or {}).get((org, repo), "")

    async def _core_push_permitted(_env, node, repo, actor):
        return (node, repo, actor) in core_owners

    async def _hosted_git_write_endpoint(_env):
        return dict(write_node) if write_node else None

    async def _account_row(_env, owner):
        return "bi:" + owner, {"name": owner}

    async def blind_index(_env, value):
        return "bi:" + str(value).strip().lower()

    async def d1_first(_env, sql, *args):
        if "SELECT owner_bi,data FROM repositories" in sql:
            if stored is None:
                return None
            return {"owner_bi": "bi:alice", "data": "stored"}
        if "FROM repositories WHERE key_bi=?" in sql:
            return {"one": 1} if args[0] in {
                "bi:" + name for name in existing} else None
        if "COUNT(*) AS c FROM repositories" in sql:
            return {"c": repo_count}
        return None

    async def d1_run(_env, sql, *args):
        recorder.statements.append((" ".join(sql.split()), args))
        return None

    async def decrypt_row(_env, _stored):
        return dict(stored) if stored else None

    async def _https_mirror_repository_proof(
            _env, _endpoint, context, operation, force_refresh=False,
            details=None, **_kwargs):
        assert operation == "git-upload-pack"
        assert force_refresh is True
        # Routing asks for the proof with the pins it already trusts; the pin
        # recorder deliberately asks with none so the node's own digest comes
        # back even when it matches nothing yet.
        assert context["pins"] == frozenset()
        reason, digest = proof
        if isinstance(details, dict):
            details.update({"reason": reason, "refsSha256": digest})
        return reason == "verified"

    async def d1_first_primary(_env, sql, *args):
        assert "FROM account_sessions" in sql
        return session_row if args[0] == SESSION_ID else None

    async def _repo_delete_tombstone_active(_env, _key_bi):
        return tombstone

    async def _clear_repo_delete_tombstone(_env, key_bi):
        recorder.audits.append(("tombstone_cleared", key_bi))

    async def catalog_rate_check(_env, _owner_bi):
        return rate_limited

    async def encrypt_row(_env, record):
        return "encrypted:" + record["owner"] + "/" + record["name"]

    async def _contribution_write_catalog_state(
            _env, record, owner_bi, _owner_rec, key_bi, encrypted):
        recorder.rows.append((key_bi, owner_bi, encrypted, record))

    async def edge_cache_delete(_key):
        return True

    async def _bump_catalog_rev(_env):
        return 1

    async def schedule_public_catalog_republish(_env, owner, name):
        recorder.republished.append((owner, name))

    async def queue_repo_webhook_event(_env, owner, name, event_type, data,
                                       digest, **_kwargs):
        recorder.webhook_events.append((owner, name, event_type, digest, data))
        return 0

    async def _audit_sensitive_action(
            _env, actor, action, kind, target, outcome, details):
        recorder.audits.append((actor, action, kind, target, outcome, details))

    async def ensure_schema(_env):
        return None

    async def bounded_json_request(request):
        return dict(request.body)

    primitives = {
        "urlparse": __import__(
            "urllib.parse", fromlist=["urlparse"]).urlparse,
        "base64": base64,
        "clean_string": catalog.clean_string,
        "safe_segment": catalog.safe_segment,
        "MAX_NODE_NAME": 63,
        "MAX_REPO_SEGMENT": catalog.MAX_REPO_SEGMENT,
        "valid_node_name": lambda value: bool(
            re.fullmatch(r"[a-z0-9][a-z0-9-]{0,62}", (value or "").strip())),
        "safe_catalog_record": catalog.safe_catalog_record,
        "json_response": _json_response,
        "method_name": lambda request: request.method,
        "bounded_json_request": bounded_json_request,
        "_authed_account_name": _authed_account_name,
        "_account_owns_node": _account_owns_node,
        "_org_write_allowed": _org_write_allowed,
        "_org_repo_node": _org_repo_node,
        "_core_push_permitted": _core_push_permitted,
        "_core_push_nodes": lambda _env: set(core_nodes),
        "_is_blocked_catalog_identity": lambda _env, owner, name: (
            name == "blocked-name"),
        "_hosted_git_write_endpoint": _hosted_git_write_endpoint,
        "_account_row": _account_row,
        "blind_index": blind_index,
        "d1_first": d1_first,
        "d1_first_primary": d1_first_primary,
        "d1_run": d1_run,
        "decrypt_row": decrypt_row,
        "_https_mirror_repository_proof": _https_mirror_repository_proof,
        "STATE_PIN_HISTORY": 100,
        "_repo_delete_tombstone_active": _repo_delete_tombstone_active,
        "_clear_repo_delete_tombstone": _clear_repo_delete_tombstone,
        "CATALOG_MAX_RECORDS_PER_OWNER": 100,
        "catalog_rate_check": catalog_rate_check,
        "encrypt_row": encrypt_row,
        "_contribution_write_catalog_state": (
            _contribution_write_catalog_state),
        "_https_mirror_public_context_cache_clear": lambda: None,
        "edge_cache_delete": edge_cache_delete,
        "CATALOG_CACHE_KEY": "catalog-cache",
        "_bump_catalog_rev": _bump_catalog_rev,
        "schedule_public_catalog_republish": (
            schedule_public_catalog_republish),
        "queue_repo_webhook_event": queue_repo_webhook_event,
        "_audit_sensitive_action": _audit_sensitive_action,
        "ensure_schema": ensure_schema,
        "Date": type("Date", (), {"now": staticmethod(lambda: NOW)}),
        "_account_session_secret": lambda env: (
            env.ADMIN_PASS + "|" + env.DATA_KEY).encode(),
        "_request_account_session_id": lambda request, data: (
            (re.fullmatch(
                r"v2\.([A-Za-z0-9_-]{24,64})\.([A-Za-z0-9_-]{32,96})",
                str((data or {}).get("sessionToken") or "")) or [None, ""]
            )[1] if isinstance(data, dict) else ""),
        "EXPECTED_DEGRADED_HEADERS": {"X-ForkMesh-Expected-Degraded": "1"},
        "_forget_repo_closed": lambda _owner, _name: None,
    }
    # A reload drops the previous scenario's bindings, so one test can never
    # inherit another's fakes.
    importlib.reload(hosted_git)
    for name, value in primitives.items():
        setattr(hosted_git, name, value)
    return hosted_git


def _basic(username, password):
    raw = (username + ":" + password).encode()
    return "Basic " + base64.b64encode(raw).decode()


def _credential(module, account="alice", owner="alice", repo="app",
                session_id=SESSION_ID, expires_at=NOW + 60_000):
    return module.hosted_push_password(
        _Env(), account, owner, repo, session_id, expires_at)


# ---------------------------------------------------------------------------
# The push credential
# ---------------------------------------------------------------------------


def test_credential_round_trips_and_audits_the_authorized_push():
    recorder = _Recorder()
    module = _install(recorder=recorder)
    header = _basic("alice", _credential(module))
    account = asyncio.run(module.verify_hosted_push_credential(
        _Env(), "alice", "app", header, "POST"))
    assert account == "alice"
    assert recorder.audits == [(
        "alice", "git.push_authorize", "repository", "alice/app", "success",
        {"operation": "git-receive-pack", "method": "POST",
         "credential": "session-mac"},
    )]


def test_credential_is_bound_to_one_repository_owner_and_account():
    recorder = _Recorder()
    module = _install(recorder=recorder)
    issued = _credential(module, owner="alice", repo="app")
    # The same MAC presented for a different repository, a different owner's
    # namespace, or under a different account name must not verify.
    assert asyncio.run(module.verify_hosted_push_credential(
        _Env(), "alice", "other", _basic("alice", issued))) == ""
    assert asyncio.run(module.verify_hosted_push_credential(
        _Env(), "bob", "app", _basic("alice", issued))) == ""
    assert asyncio.run(module.verify_hosted_push_credential(
        _Env(), "alice", "app", _basic("bob", issued))) == ""


def test_expired_or_tampered_credentials_are_refused():
    recorder = _Recorder()
    module = _install(recorder=recorder)
    expired = _credential(module, expires_at=NOW - 1)
    assert asyncio.run(module.verify_hosted_push_credential(
        _Env(), "alice", "app", _basic("alice", expired))) == ""
    # An expired credential is one this relay really issued, so the refusal
    # is audited.
    assert [audit[4] for audit in recorder.audits] == ["denied"]
    issued = _credential(module)
    tampered = issued[:-1] + ("0" if issued[-1] != "0" else "1")
    assert asyncio.run(module.verify_hosted_push_credential(
        _Env(), "alice", "app", _basic("alice", tampered))) == ""
    # A forged one is probe traffic: refused without spending a D1 write.
    assert [audit[4] for audit in recorder.audits] == ["denied"]


@pytest.mark.parametrize("session", [
    {"account_bi": "bi:alice", "revoked_at": NOW - 1,
     "expires_at": NOW + 60_000},
    {"account_bi": "bi:alice", "revoked_at": 0, "expires_at": NOW - 1},
    {"account_bi": "bi:bob", "revoked_at": 0, "expires_at": NOW + 60_000},
    None,
])
def test_credential_dies_with_its_browser_session(session):
    # Revocation is the whole reason the credential carries a session id:
    # nothing is stored, so signing out (or a session that expired, or one
    # that belongs to someone else) is what takes the credential away.
    recorder = _Recorder()
    module = _install(
        recorder=recorder, session=session if session is not None else {
            "account_bi": "bi:alice", "revoked_at": 0,
            "expires_at": NOW + 60_000})
    if session is None:
        credential = _credential(module, session_id="Z" * 32)
    else:
        credential = _credential(module)
    assert asyncio.run(module.verify_hosted_push_credential(
        _Env(), "alice", "app", _basic("alice", credential))) == ""


def test_credential_never_exceeds_its_accounts_push_permission():
    recorder = _Recorder()
    module = _install(recorder=recorder, owner_of=())
    credential = _credential(module, owner="bob")
    assert asyncio.run(module.verify_hosted_push_credential(
        _Env(), "bob", "app", _basic("alice", credential))) == ""

    # An organization member with write permission is authorized by the same
    # check the signed-header path uses.
    recorder = _Recorder()
    module = _install(recorder=recorder, owner_of=(), org_write=True)
    credential = _credential(module, owner="bob")
    assert asyncio.run(module.verify_hosted_push_credential(
        _Env(), "bob", "app", _basic("alice", credential))) == "alice"


@pytest.mark.parametrize("header", [
    "", "Bearer abc", "Basic", "Basic !!!!", _basic("alice", ""),
])
def test_unusable_authorization_headers_are_not_credentials(header):
    recorder = _Recorder()
    module = _install(recorder=recorder)
    assert module.has_hosted_push_credential(header) is False
    assert asyncio.run(module.verify_hosted_push_credential(
        _Env(), "alice", "app", header)) == ""


def test_a_basic_header_is_recognized_before_the_body_is_read():
    recorder = _Recorder()
    module = _install(recorder=recorder)
    assert module.has_hosted_push_credential(
        _basic("alice", _credential(module))) is True


ALICE_PASSWORD = "correct horse battery"


def _with_password_login(module, *, totp=False, admin=False, status="active",
                         kind="user"):
    attempts = {"fails": [], "cleared": []}
    record = {"name": "alice", "status": status, "totp_enrolled": totp,
              "kind": kind}

    async def _account_password_record(_env, data):
        identifier = data.get("identifier", "")
        if identifier not in ("alice", "alice@example.test"):
            return "", None, "invalid_credentials"
        if data.get("password") != ALICE_PASSWORD:
            return "", None, "invalid_credentials"
        if record["status"] != "active":
            return "", None, "invalid_credentials"
        if record["kind"] != "user":
            return "", None, "not_a_user"
        return "bi:alice", dict(record), ""

    async def _login_attempt_keys(_env, _request, identifier):
        return ["source:" + identifier]

    async def _login_record_fail(_env, key):
        attempts["fails"].append(key)
        return {"fails": len(attempts["fails"]), "lockedUntil": 0}

    async def _login_clear(_env, key):
        attempts["cleared"].append(key)

    async def _is_admin_flag(_env, name):
        return admin and name == "alice"

    async def d1_first(_env, sql, *args):
        if "FROM users WHERE email_bi=?" in sql:
            if args[0] == "bi:alice@example.test":
                return {"data": "alice-user"}
            return None
        return None

    async def decrypt_row(_env, stored):
        if stored == "alice-user":
            return {"name": "alice", "status": "active"}
        return None

    for name, value in {
        "_account_password_record": _account_password_record,
        "_login_attempt_keys": _login_attempt_keys,
        "_login_record_fail": _login_record_fail,
        "_login_clear": _login_clear,
        "_is_admin_flag": _is_admin_flag,
        "d1_first": d1_first,
        "decrypt_row": decrypt_row,
    }.items():
        setattr(module, name, value)
    return attempts


@pytest.mark.parametrize("username", ["alice", "alice@example.test",
                                      "Alice@Example.test"])
def test_account_name_or_email_and_password_push(username):
    recorder = _Recorder()
    module = _install(recorder=recorder)
    attempts = _with_password_login(module)
    account = asyncio.run(module.verify_hosted_push_credential(
        _Env(), "alice", "app", _basic(username, ALICE_PASSWORD), "POST",
        request=_Request()))
    assert account == "alice"
    assert recorder.audits[-1] == (
        "alice", "git.push_authorize", "repository", "alice/app", "success",
        {"operation": "git-receive-pack", "method": "POST",
         "credential": "password"})
    assert attempts["fails"] == []
    assert attempts["cleared"] == ["source:" + username.lower()]


def test_a_password_keeps_the_accounts_push_permission():
    recorder = _Recorder()
    module = _install(recorder=recorder, owner_of=())
    _with_password_login(module)
    assert asyncio.run(module.verify_hosted_push_credential(
        _Env(), "bob", "app", _basic("alice", ALICE_PASSWORD), "POST",
        request=_Request())) == ""
    assert recorder.audits[-1][4] == "denied"

    recorder = _Recorder()
    module = _install(recorder=recorder, owner_of=(), org_write=True)
    _with_password_login(module)
    assert asyncio.run(module.verify_hosted_push_credential(
        _Env(), "bob", "app", _basic("alice", ALICE_PASSWORD), "POST",
        request=_Request())) == "alice"


def test_a_wrong_password_counts_toward_the_login_lockout():
    recorder = _Recorder()
    module = _install(recorder=recorder)
    attempts = _with_password_login(module)
    assert asyncio.run(module.verify_hosted_push_credential(
        _Env(), "alice", "app", _basic("alice", "wrong"), "POST",
        request=_Request())) == ""
    assert attempts["fails"] == ["source:alice"]
    assert recorder.audits == []


@pytest.mark.parametrize("kwargs", [{"totp": True}, {"admin": True}])
def test_admin_and_two_factor_accounts_push_with_the_password(kwargs):
    recorder = _Recorder()
    module = _install(recorder=recorder)
    _with_password_login(module, **kwargs)
    assert asyncio.run(module.verify_hosted_push_credential(
        _Env(), "alice", "app", _basic("alice", ALICE_PASSWORD), "POST",
        request=_Request())) == "alice"


@pytest.mark.parametrize("kwargs", [{"status": "disabled"}, {"kind": "node"}])
def test_password_push_is_refused_for_disabled_or_node_accounts(kwargs):
    recorder = _Recorder()
    module = _install(recorder=recorder)
    _with_password_login(module, **kwargs)
    assert asyncio.run(module.verify_hosted_push_credential(
        _Env(), "alice", "app", _basic("alice", ALICE_PASSWORD), "POST",
        request=_Request())) == ""


def test_an_operator_can_turn_password_pushes_off():
    class _Off(_Env):
        GIT_PASSWORD_AUTH = "0"

    recorder = _Recorder()
    module = _install(recorder=recorder)
    attempts = _with_password_login(module)
    assert asyncio.run(module.verify_hosted_push_credential(
        _Off(), "alice", "app", _basic("alice", ALICE_PASSWORD), "POST",
        request=_Request())) == ""
    assert attempts == {"fails": [], "cleared": []}
    assert asyncio.run(module.verify_hosted_push_credential(
        _Off(), "alice", "app", _basic("alice", _credential(module)),
        "POST")) == "alice"


def test_a_push_credential_works_with_the_account_email():
    recorder = _Recorder()
    module = _install(recorder=recorder)
    _with_password_login(module)
    assert asyncio.run(module.verify_hosted_push_credential(
        _Env(), "alice", "app",
        _basic("alice@example.test", _credential(module)), "POST")) == "alice"


def test_a_private_clone_accepts_the_account_password():
    recorder = _Recorder()
    module = _install(recorder=recorder)
    _with_password_login(module)

    async def _hosted_read_allowed(_env, owner, repo, account):
        return (owner, repo, account) == ("alice", "app", "alice")

    module._hosted_read_allowed = _hosted_read_allowed
    assert asyncio.run(module.verify_hosted_push_credential(
        _Env(), "alice", "app", _basic("alice@example.test", ALICE_PASSWORD),
        "GET", access="read", request=_Request())) == "alice"


# ---------------------------------------------------------------------------
# Creating the empty repository
# ---------------------------------------------------------------------------


def _create(module, body=None, **kwargs):
    payload = {"name": "app", "sessionToken": SESSION_TOKEN}
    payload.update(body or {})
    return asyncio.run(module.hosted_repository_handler(
        _Env(), _Request(payload, **kwargs), "/api/hosted-repositories"))


def test_create_writes_a_public_hosted_record_and_returns_push_commands():
    recorder = _Recorder()
    module = _install(recorder=recorder)
    response = _create(module, {"description": "Notes"})
    assert response.status == 201
    record = response.data["repository"]
    assert record["owner"] == "alice" and record["name"] == "app"
    assert record["visibility"] == "public"
    assert record["source"] == "hosted-web"
    # The hosting node's key maintains the record: a web-only account holds no
    # signing key of its own.
    assert record["maintainer"] == WRITE_NODE["publicKey"]
    assert record["description"] == "Notes"
    assert record["cloneUrl"] == "https://app.forkmesh.test/alice/app"
    # The row actually written is the same record, under the repo blind index.
    assert recorder.rows[0][0] == "bi:alice/app"
    assert recorder.rows[0][3] is record
    assert recorder.republished == [("alice", "app")]

    push = response.data["push"]
    assert push["username"] == "alice"
    # Smart HTTP serves /<owner>/<repo>/info/refs exactly, so an advertised
    # .git suffix would address a repository named "app.git".
    assert push["remoteUrl"] == "https://app.forkmesh.test/alice/app"
    assert not push["remoteUrl"].endswith(".git")
    assert "git push -u forkmesh main" in push["commands"]
    assert any(
        line == "git remote add forkmesh " + push["remoteUrl"]
        for line in push["commands"])
    # The credential is not in the remote, so it cannot land in .git/config.
    assert push["password"] not in " ".join(push["commands"])
    assert push["expiresAt"] == NOW + module.HOSTED_PUSH_TTL_MS
    assert asyncio.run(module.verify_hosted_push_credential(
        _Env(), "alice", "app",
        _basic(push["username"], push["password"]))) == "alice"
    assert (
        "alice", "repository.create_hosted", "repository", "alice/app",
        "success", {"source": "hosted-web", "hostedNode": "hosted-write"},
    ) in recorder.audits


@pytest.mark.parametrize("repo_count,expected", [
    (0, [("bi:alice", "first_mirror_created", {"repo": "alice/app"})]),
    (1, []),
])
def test_create_emits_the_activation_event_for_the_first_repository_only(
        repo_count, expected):
    recorder = _Recorder()
    module = _install(recorder=recorder, repo_count=repo_count)
    emitted = []

    async def emit_for_user(_env, user_bi, event_name, metadata=None):
        emitted.append((user_bi, event_name, metadata))
        return True

    module._attribution = type(
        "Attribution", (), {"emit_for_user": staticmethod(emit_for_user)})
    assert _create(module).status == 201
    assert emitted == expected


def test_create_refuses_private_instead_of_publishing_a_readable_mirror():
    recorder = _Recorder()
    module = _install(recorder=recorder)
    response = _create(module, {"visibility": "private"})
    assert response.status == 400
    assert response.data["error"] == "private_hosted_unsupported"
    assert recorder.rows == []


def test_create_requires_a_session_that_owns_the_namespace():
    recorder = _Recorder()
    module = _install(recorder=recorder, account="")
    assert _create(module).status == 401

    recorder = _Recorder()
    module = _install(recorder=recorder, owner_of=())
    response = _create(module, {"owner": "bob"})
    assert response.status == 403
    assert recorder.rows == []


@pytest.mark.parametrize("name,status,error", [
    ("", 400, "invalid_name"),
    ("../escape", 400, "invalid_name"),
    ("has space", 400, "invalid_name"),
    ("a" * 65, 400, "invalid_name"),
    ("blocked-name", 403, "blocked"),
    ("taken", 409, "repository_exists"),
])
def test_create_rejects_unusable_names(name, status, error):
    recorder = _Recorder()
    module = _install(recorder=recorder, existing=("alice/taken",))
    response = _create(module, {"name": name})
    assert (response.status, response.data["error"]) == (status, error)
    assert recorder.rows == []


def test_create_applies_the_owner_record_cap_and_the_publish_rate_limit():
    recorder = _Recorder()
    module = _install(recorder=recorder, repo_count=100)
    response = _create(module)
    assert (response.status, response.data["error"]) == (429, "too_many_repos")

    recorder = _Recorder()
    module = _install(
        recorder=recorder,
        rate_limited=_json_response({"error": "rate_limited"}, status=429))
    assert _create(module).data["error"] == "rate_limited"
    assert recorder.rows == []


def test_recreating_a_deleted_name_lifts_only_its_own_tombstone():
    # The signed publish path allows this for an explicit user-initiated
    # publish; here the session is that proof of intent.
    recorder = _Recorder()
    module = _install(recorder=recorder, tombstone=True)
    assert _create(module).status == 201
    assert ("tombstone_cleared", "bi:alice/app") in recorder.audits


def test_create_without_a_hosted_write_node_is_expected_degradation():
    recorder = _Recorder()
    module = _install(recorder=recorder, write_node=None)
    response = _create(module)
    assert response.status == 503
    assert response.data["error"] == "hosted_git_unavailable"
    # A deliberate configuration state must not spend the error budget.
    assert response.headers.get("X-ForkMesh-Expected-Degraded") == "1"
    assert recorder.rows == []


def test_create_needs_a_revocable_browser_session_to_bind_the_credential_to():
    recorder = _Recorder()
    module = _install(recorder=recorder)
    # Signing the session out is the credential's only revocation path, so a
    # session with no opaque id is refused as such — and creates nothing.
    response = _create(module, {"sessionToken": "not-a-token"})
    assert (response.status, response.data["error"]) == (
        401, "session_required")
    assert recorder.rows == []


def test_only_post_creates_and_unknown_subpaths_are_not_found():
    recorder = _Recorder()
    module = _install(recorder=recorder)
    assert _create(module, method="PUT").status == 405
    # GET answers what this relay offers the create form (no core hosting in
    # this scenario: one hosted write node, public repositories only).
    settings = _create(module, method="GET")
    assert settings.status == 200
    assert settings.data["mode"] == "single-node"
    assert settings.data["privateSupported"] is False
    assert recorder.rows == []
    assert asyncio.run(module.hosted_repository_handler(
        _Env(), _Request({}), "/api/hosted-repositories/nope")).status == 404


def test_push_credential_endpoint_remints_for_a_pushable_repository():
    recorder = _Recorder()
    module = _install(recorder=recorder)
    response = asyncio.run(module.hosted_repository_handler(
        _Env(),
        _Request({"name": "app", "sessionToken": SESSION_TOKEN}),
        "/api/hosted-repositories/push-credential"))
    assert response.status == 200
    assert asyncio.run(module.verify_hosted_push_credential(
        _Env(), "alice", "app",
        _basic("alice", response.data["push"]["password"]))) == "alice"

    recorder = _Recorder()
    module = _install(recorder=recorder, owner_of=())
    denied = asyncio.run(module.hosted_repository_handler(
        _Env(),
        _Request({"owner": "bob", "name": "app",
                  "sessionToken": SESSION_TOKEN}),
        "/api/hosted-repositories/push-credential"))
    assert denied.status == 403


def test_push_credential_through_an_org_alias_binds_the_backing_core_node():
    scenario = dict(
        org_nodes={("forkmesh", "forkmesh"): "fmnext-n1"},
        core_nodes=("fmnext-n1",),
        core_owners={("fmnext-n1", "forkmesh", "alice")}, owner_of=())
    module = _install(recorder=_Recorder(), **scenario)
    response = asyncio.run(module.hosted_repository_handler(
        _Env(),
        _Request({"owner": "forkmesh", "name": "forkmesh",
                  "sessionToken": SESSION_TOKEN}),
        "/api/hosted-repositories/push-credential"))
    assert response.status == 200
    assert response.data["push"]["remoteUrl"].endswith("/forkmesh/forkmesh")
    password = response.data["push"]["password"]
    assert module.hosted_push_password(
        _Env(), "alice", "fmnext-n1", "forkmesh",
        password.split(".")[1], int(password.split(".")[2])) == password

    module = _install(recorder=_Recorder(), org_write=True,
                      **dict(scenario, core_owners=set()))
    denied = asyncio.run(module.hosted_repository_handler(
        _Env(),
        _Request({"owner": "forkmesh", "name": "forkmesh",
                  "sessionToken": SESSION_TOKEN}),
        "/api/hosted-repositories/push-credential"))
    assert denied.status == 403


# ---------------------------------------------------------------------------
# The refs pin a hosted push leaves behind
# ---------------------------------------------------------------------------


def _hosted_record(**changes):
    record = {
        "owner": "alice",
        "name": "app",
        "visibility": "public",
        "source": "hosted-web",
        "stateHash": "",
        "updatedAt": "1",
        "lastSync": "1",
    }
    record.update(changes)
    return record


def test_hosted_push_records_the_nodes_signed_refs_digest():
    # Without this the repository is pushable and then unreadable: a public
    # read is routed only against an authenticated refs attestation, and a
    # hosted repository has no node of its own to publish one.
    recorder = _Recorder()
    module = _install(recorder=recorder, stored=_hosted_record())
    digest = asyncio.run(module.record_hosted_push_state(
        _Env(), "alice", "app", dict(WRITE_NODE)))
    assert digest == "d" * 64
    written = recorder.rows[0][3]
    assert written["stateHash"] == "d" * 64
    assert written["lastSync"] == str(NOW)
    # The pin also lands in history, which is what keeps reads working across
    # the next push before the pin is refreshed.
    inserts = [
        args for sql, args in recorder.statements
        if sql.startswith("INSERT INTO repo_state_history")]
    assert inserts == [("bi:alice/app", "d" * 64, NOW)]
    assert any(
        sql.startswith("DELETE FROM repo_state_history")
        for sql, _args in recorder.statements)
    assert recorder.republished == [("alice", "app")]
    # A plain-git push to a web-created repository has no signed catalog
    # publication behind it, so this pin is where it becomes an event any
    # registered webhook can be told about (adhoc #207).
    # "before" is empty here because the stored record carried no pin yet:
    # this is the repository's first push.
    assert recorder.webhook_events == [
        ("alice", "app", "repository.push", "d" * 64,
         {"before": "", "refsDigest": "d" * 64, "hosted": True})]


def test_hosted_push_pings_code_push_naming_the_authorized_pusher():
    # The pin is the only evidence a hosted push happened, so it is where the
    # code-push ping is raised; the pusher is the account receive-pack
    # authorized, never anything the push itself claimed.
    recorder = _Recorder()
    module = _install(recorder=recorder, stored=_hosted_record())
    pings = []

    async def notify_code_push(_env, owner, repo, **kwargs):
        pings.append((owner, repo, kwargs))
        return 1

    module.notify_code_push = notify_code_push
    asyncio.run(module.record_hosted_push_state(
        _Env(), "alice", "app", dict(WRITE_NODE), pusher="mona"))
    assert pings == [("alice", "app", {
        "pusher": "mona", "pusher_account": "mona", "key": "d" * 64,
        "refs_digest": "d" * 64, "meta": {"hosted": True}})]

    # A re-pin of the same state is not a push and pings nobody.
    pings.clear()
    module = _install(recorder=_Recorder(),
                      stored=_hosted_record(stateHash="d" * 64))
    module.notify_code_push = notify_code_push
    asyncio.run(module.record_hosted_push_state(
        _Env(), "alice", "app", dict(WRITE_NODE), pusher="mona"))
    assert pings == []


def test_pin_recording_never_touches_a_node_published_record():
    recorder = _Recorder()
    module = _install(
        recorder=recorder, stored=_hosted_record(source="local-node"))
    assert asyncio.run(module.record_hosted_push_state(
        _Env(), "alice", "app", dict(WRITE_NODE))) == ""
    assert recorder.rows == [] and recorder.statements == []


@pytest.mark.parametrize("proof", [
    ("repository_signature_invalid", "d" * 64),
    ("mirror_unhealthy", "d" * 64),
    ("refs_mismatch", "not-a-digest"),
])
def test_an_unproven_digest_is_never_pinned(proof):
    recorder = _Recorder()
    module = _install(recorder=recorder, stored=_hosted_record(), proof=proof)
    assert asyncio.run(module.record_hosted_push_state(
        _Env(), "alice", "app", dict(WRITE_NODE))) == ""
    assert recorder.rows == [] and recorder.statements == []


def test_an_unchanged_digest_is_not_rewritten():
    recorder = _Recorder()
    module = _install(
        recorder=recorder, stored=_hosted_record(stateHash="d" * 64))
    assert asyncio.run(module.record_hosted_push_state(
        _Env(), "alice", "app", dict(WRITE_NODE))) == "d" * 64
    assert recorder.rows == [] and recorder.statements == []


def test_pin_recording_ignores_a_repository_that_does_not_exist():
    recorder = _Recorder()
    module = _install(recorder=recorder, stored=None)
    assert asyncio.run(module.record_hosted_push_state(
        _Env(), "alice", "app", dict(WRITE_NODE))) == ""
    assert asyncio.run(module.record_hosted_push_state(
        _Env(), "", "app", dict(WRITE_NODE))) == ""


# ---------------------------------------------------------------------------
# Worker wiring
# ---------------------------------------------------------------------------


def test_module_is_lazy_and_routed():
    assert '_hosted_git = _LazyModule("hosted_git")' in ENTRY
    assert 'hosted_repository_handler = _hosted_git.export(' in ENTRY
    assert '"hosted_git.py",' in FOOTPRINT
    assert '/api/hosted-repositories' in APP_RUNTIME
    assert "await hosted_repository_handler(" in APP_RUNTIME


def test_receive_pack_accepts_the_basic_credential_and_challenges_for_it():
    start = ENTRY.index("async def _https_mirror_push_proxy(")
    push_proxy = ENTRY[start:ENTRY.index("\ndef _cron_due_phases(", start)]
    # Signed device headers stay the primary path; the Basic credential is
    # only consulted when they are absent.
    assert "signed_headers = bool(pubkey and ts and sig)" in push_proxy
    assert "has_hosted_push_credential(authorization)" in push_proxy
    assert "await verify_hosted_push_access(" in push_proxy
    assert "_git_push_basic_challenge()" in push_proxy
    # A successful push is where a hosted repository's refs pin comes from.
    assert ("await record_hosted_push_state(\n"
            "                    env, owner, repo, endpoint, pusher=account)"
            ) in push_proxy
    # git only sends credentials after a challenge naming the scheme.
    challenge = ENTRY[
        ENTRY.index("def _git_push_basic_challenge():"):
        ENTRY.index("async def _https_mirror_push_proxy(")]
    assert 'WWW-Authenticate' in challenge
    assert 'Basic realm="forkmesh"' in challenge
    assert "status=401" in challenge


def test_hosted_records_are_marked_and_never_claim_a_node_signature():
    assert 'HOSTED_SOURCE = "hosted-web"' in MODULE
    # A hosted record never carries a catalog signature it cannot produce.
    assert "catalogSig" not in MODULE
    # The credential is a MAC, not a stored row: the only table this module
    # inserts into is the refs pin history.
    assert [
        line.strip() for line in MODULE.splitlines()
        if "INSERT INTO" in line
    ] == ['"INSERT INTO repo_state_history (key_bi, state_hash, ts) "']


# ---------------------------------------------------------------------------
# The create modal
# ---------------------------------------------------------------------------


def test_modal_offers_an_empty_repository_source_by_default():
    assert 'data-new-repo-source="empty"' in NEW_REPO_MODAL
    empty_button = NEW_REPO_MODAL[
        NEW_REPO_MODAL.index('data-new-repo-source="empty"'):]
    assert 'aria-pressed="true"' in empty_button[:200]
    assert "New repository" in empty_button[:400]
    # The three pre-existing sources are still offered.
    for source in ("remote", "local", "provider"):
        assert 'data-new-repo-source="%s"' % source in NEW_REPO_MODAL
    assert 'data-new-repo-source="remote" type="button" aria-pressed="false"' \
        in NEW_REPO_MODAL


def test_empty_rail_mirror_link_opens_the_remote_clone_source():
    assert ('<a href="/dashboard/repos/new?source=remote" class="text-accent '
            'hover:underline">Mirror one from GitHub, GitLab or Codeberg, '
            'or create one</a>') in REPO_LIST_JS
    assert "function requestedNewRepoSource()" in REPO_LIST_JS
    assert 'new URLSearchParams(location.search).get("source")' in REPO_LIST_JS
    assert ('$$("[data-new-repo-source]").some((btn) => '
            'btn.dataset.newRepoSource === requested)') in REPO_LIST_JS
    prepare = REPO_LIST_JS[REPO_LIST_JS.index("function prepareNewRepoForm()"):]
    prepare = prepare[:prepare.index("\n  }\n")]
    assert "setNewRepoSource(requestedNewRepoSource() || newRepoSource());" \
        in prepare
    assert "function requestedNewRepoSource()" in BUNDLE
    assert "/dashboard/repos/new?source=remote" in BUNDLE


def test_modal_shows_the_push_commands_and_the_credential_terms():
    assert "data-new-repo-push" in NEW_REPO_MODAL
    assert "data-new-repo-push-commands" in NEW_REPO_MODAL
    assert "data-new-repo-push-username" in NEW_REPO_MODAL
    assert "data-new-repo-push-password" in NEW_REPO_MODAL
    assert "data-new-repo-push-expiry" in NEW_REPO_MODAL
    assert "signing this session out revokes it immediately" in NEW_REPO_MODAL
    assert "not stored by ForkMesh" in NEW_REPO_MODAL
    assert "Repository created — push your code" in NEW_REPO_MODAL
    # The generated, actually served document carries the same markup.
    assert "data-new-repo-push-commands" in (
        ROOT / "public" / "dashboard" / "repos" / "index.html"
    ).read_text(encoding="utf-8")


def test_frontend_creates_the_repository_and_reveals_the_credential_once():
    assert 'fetch("/api/hosted-repositories"' in REPO_LIST_JS
    assert 'if (source === "empty") {' in REPO_LIST_JS
    assert "await createHostedRepository({ name, visibility, description, "\
        "...hostedCreateExtras() });" in REPO_LIST_JS
    assert "function renderNewRepoPush(repository, push, hosting = null)" \
        in REPO_LIST_JS
    assert "function setNewRepoPushOpen(open)" in REPO_LIST_JS
    # Closing the modal takes the credential out of the document.
    close_section = REPO_LIST_JS[
        REPO_LIST_JS.index("const token = $(\"[data-new-repo-provider-token]\")"):
        REPO_LIST_JS.index("// The dashboard home rail's \"New\"")]
    assert 'password.textContent = ""' in close_section
    assert "localStorage" not in close_section
    # An unconfigured relay falls back to the handoff that does work.
    assert 'code === "hosted_git_unavailable"' in REPO_LIST_JS
    assert "setNewRepoHandoffOpen(true, {" in REPO_LIST_JS
    assert "data-new-repo-push-copy" in NETWORK_JS
    for fragment in (REPO_LIST_JS, NETWORK_JS):
        assert fragment in BUNDLE


def test_private_visibility_is_not_offered_for_a_hosted_repository():
    # Only where one hosted write node (no core hosting) holds the repository:
    # the relay's settings say whether private is supported.
    assert "const privateBlocked = hostedHere && !hostedSettings?.privateSupported;" \
        in REPO_LIST_JS
    assert "privateOption.disabled = privateBlocked;" in REPO_LIST_JS
    assert 'newRepoVisibility() === "private") setNewRepoVisibility("public")' \
        in REPO_LIST_JS
    assert "private_hosted_unsupported" in REPO_LIST_JS


def test_core_hosting_form_fields_and_import():
    """docs/HOSTED_REPOS.md: owner, README, .gitignore, license, import."""
    for marker in ("data-new-repo-hosted-options", "data-new-repo-owner",
                   "data-new-repo-readme", "data-new-repo-gitignore",
                   "data-new-repo-license", "data-new-repo-quota",
                   "data-new-repo-push-visibility", "data-new-repo-push-intro"):
        assert marker in NEW_REPO_MODAL
    assert 'fetch("/api/hosted-repositories", {' in REPO_LIST_JS
    assert "loadHostedSettings();" in REPO_LIST_JS
    assert 'source === "remote" && hostedImportAllowed(sourceValue)' \
        in REPO_LIST_JS
    assert "importUrl: sourceValue" in REPO_LIST_JS
    assert 'code === "hosted_quota_exceeded"' in REPO_LIST_JS
    assert "data-new-repo-hosted-options" in (
        ROOT / "public" / "dashboard" / "repos" / "index.html"
    ).read_text(encoding="utf-8")
    assert "function hostedCreateExtras()" in BUNDLE


def test_local_path_placeholder_matches_the_users_platform():
    # The Linux-shaped example was the reported UX wart for Windows users.
    assert "function localRepositoryPlaceholder()" in REPO_LIST_JS
    assert "C:\\\\Users\\\\you\\\\code\\\\my-project" in REPO_LIST_JS
    assert "/home/you/code/my-project" in REPO_LIST_JS
    assert "value.placeholder = localRepositoryPlaceholder();" in REPO_LIST_JS


def test_a_new_pin_moves_the_catalog_head_to_the_default_branch_tip():
    recorder = _Recorder()
    module = _install(recorder=recorder, stored=_hosted_record(
        branch="main", commit="1" * 40, stateHash="c" * 64))
    reads = []

    async def head(_env, endpoint, owner, repo):
        reads.append((endpoint["node"], owner, repo))
        return "main", "2" * 40

    module._hosted_head = head
    assert asyncio.run(module.record_hosted_push_state(
        _Env(), "alice", "app", dict(WRITE_NODE))) == "d" * 64
    written = recorder.rows[0][3]
    assert (written["stateHash"], written["branch"], written["commit"]) == (
        "d" * 64, "main", "2" * 40)
    assert reads == [(WRITE_NODE["node"], "alice", "app")]


def test_an_admitted_state_pins_without_a_second_proof_or_notifications():
    recorder = _Recorder()
    module = _install(recorder=recorder, stored=_hosted_record(
        branch="main", commit="1" * 40), proof=("mirror_unhealthy", ""))

    async def head(_env, _endpoint, _owner, _repo):
        return "main", "3" * 40

    module._hosted_head = head
    assert asyncio.run(module.record_hosted_push_state(
        _Env(), "alice", "app", dict(WRITE_NODE), state="e" * 64,
        notify=False)) == "e" * 64
    written = recorder.rows[0][3]
    assert (written["stateHash"], written["commit"]) == ("e" * 64, "3" * 40)
    assert recorder.webhook_events == []


def test_an_unreadable_head_still_pins_the_state_and_keeps_the_commit():
    recorder = _Recorder()
    module = _install(recorder=recorder, stored=_hosted_record(
        branch="main", commit="1" * 40))

    async def head(_env, _endpoint, _owner, _repo):
        return None

    module._hosted_head = head
    asyncio.run(module.record_hosted_push_state(
        _Env(), "alice", "app", dict(WRITE_NODE)))
    written = recorder.rows[0][3]
    assert (written["stateHash"], written["commit"]) == ("d" * 64, "1" * 40)


@pytest.mark.parametrize("listing, head", [
    ({"ok": True, "defaultBranch": "main", "branches": [
        {"name": "dev", "commit": "4" * 40},
        {"name": "main", "commit": "5" * 40}]}, ("main", "5" * 40)),
    ({"ok": True, "defaultBranch": "main", "branches": [
        {"name": "dev", "commit": "4" * 40}]}, None),
    ({"ok": True, "defaultBranch": "main", "branches": [
        {"name": "main", "commit": "not-an-oid"}]}, None),
    ({"ok": True, "defaultBranch": "../x", "branches": []}, None),
    ({"ok": False}, None),
    ([], None),
])
def test_the_head_comes_only_from_a_well_formed_branch_listing(listing, head):
    module = _install(recorder=_Recorder())
    assert module.hosted_head_from_branches(listing) == head


def _hosted_stats(commit="2" * 40, count=62162):
    return {"commit": commit, "commitCount": count,
            "activityWeeks": [0] * 51 + [3]}


def test_hosted_stats_update_only_the_record_whose_pin_they_describe():
    recorder = _Recorder()
    module = _install(recorder=recorder, stored=_hosted_record(
        commit="2" * 40, stateHash="c" * 64, commitCount="10000"))
    assert asyncio.run(module.record_hosted_stats(
        _Env(), "alice", "app", "c" * 64, _hosted_stats())) is True
    written = recorder.rows[0][3]
    assert written["commitCount"] == "62162"
    assert written["activityWeeks"] == [0] * 51 + [3]
    assert (written["stateHash"], written["updatedAt"]) == ("c" * 64, "1")


def test_hosted_stats_skip_a_stale_pin_another_commit_or_a_node_record():
    for stored, digest, stats in (
        (_hosted_record(stateHash="c" * 64), "d" * 64, _hosted_stats()),
        (_hosted_record(stateHash="c" * 64, commit="3" * 40), "c" * 64,
         _hosted_stats()),
        (_hosted_record(stateHash="c" * 64, source="remote-clone"), "c" * 64,
         _hosted_stats()),
        (_hosted_record(stateHash="c" * 64, commitCount="62162",
                        activityWeeks=[0] * 51 + [3]), "c" * 64,
         _hosted_stats()),
        (_hosted_record(stateHash="c" * 64), "c" * 64,
         _hosted_stats(commit="nope")),
    ):
        recorder = _Recorder()
        module = _install(recorder=recorder, stored=stored)
        assert asyncio.run(module.record_hosted_stats(
            _Env(), "alice", "app", digest, stats)) is False
        assert recorder.rows == []
