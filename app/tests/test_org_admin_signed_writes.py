#!/usr/bin/env python3
"""Key-signed organization management from the desktop (adhoc #204).

The desktop's Settings -> Organizations tab is the twin of the website's
organization panel, but a desktop that launched normally authenticated
silently: it holds its account's Ed25519 key and no session token at all, so
every one of those requests is signed onto the URL instead. These checks pin
how narrow that credential is:

  * one proof prefix per operation, and no proof stands in for another;
  * the canonical binds the request path, so a proof for one organization
    never works against a different one;
  * it binds every body field that decides what the write does, so a captured
    member-add can never be replayed as a different role, member, or team;
  * a signed write may carry nothing the proof does not cover;
  * the world-access, federation, Discord, succession and bot-token
    subresources are not signable at all — they still need a real session;
  * only an active user account resolves, and an unconfirmed email address is
    held back from writes exactly as the session path holds it back.

AST-extraction harness in the style of test_org_task_signed_writes.py.

Run: python3 -m pytest app/tests/test_org_admin_signed_writes.py
"""

import ast
import asyncio
import hashlib
import re
from pathlib import Path
from urllib.parse import parse_qs, urlparse

import pytest


ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
CATALOG = ROOT / "src" / "catalog.py"
URLS = ROOT / "src" / "urls.py"
SOURCE_TEXT = (
    ENTRY.read_text(encoding="utf-8")
    + "\n"
    + CATALOG.read_text(encoding="utf-8")
    + "\n"
    + URLS.read_text(encoding="utf-8")
)

FUNCS = {
    "_org_admin_operation",
    "_org_admin_canonical_field",
    "_org_admin_signed_triple",
    "_org_admin_signed_session",
    "_owner_signing_pubkeys",
    "_account_signing_pubkeys",
    "_verify_owner_signature",
    "_account_email_unverified",
    "clean_string",
}
CONSTANTS = {
    "ORG_ADMIN_LIST_PROOF",
    "ORG_ADMIN_CREATE_PROOF",
    "ORG_ADMIN_READ_PROOF",
    "ORG_ADMIN_PROFILE_PROOF",
    "ORG_ADMIN_DELETE_PROOF",
    "ORG_ADMIN_MEMBER_SET_PROOF",
    "ORG_ADMIN_MEMBER_REMOVE_PROOF",
    "ORG_ADMIN_TEAM_SET_PROOF",
    "ORG_ADMIN_TEAM_REMOVE_PROOF",
    "ORG_ADMIN_TEAM_MEMBER_ADD_PROOF",
    "ORG_ADMIN_TEAM_MEMBER_REMOVE_PROOF",
    "ORG_ADMIN_REPO_LINK_PROOF",
    "ORG_ADMIN_REPO_UNLINK_PROOF",
    "ORG_ADMIN_FIELD_MAX",
    "ORGS_RE",
    "ORG_RE",
    "ORG_MEMBERS_RE",
    "ORG_TEAMS_RE",
    "ORG_TEAM_MEMBERS_RE",
    "ORG_REPOS_RE",
    "UNVERIFIED_SAFE_METHODS",
    "UNVERIFIED_EMAIL_ERROR",
}


def _load(extra):
    tree = ast.parse(SOURCE_TEXT, filename=str(ENTRY))
    selected = []
    for node in tree.body:
        if (
            isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
            and node.name in FUNCS
        ):
            selected.append(node)
        elif isinstance(node, ast.Assign) and any(
            isinstance(target, ast.Name) and target.id in CONSTANTS
            for target in node.targets
        ):
            selected.append(node)
    found = set()
    for node in selected:
        found.add(node.name if hasattr(node, "name") else node.targets[0].id)
    assert found == FUNCS | CONSTANTS, \
        "missing: %s" % sorted((FUNCS | CONSTANTS) - found)
    module = ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[]))
    namespace = dict(extra)
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace


class _Headers:
    def __init__(self, mapping=None):
        self._m = {str(k).lower(): v for k, v in (mapping or {}).items()}

    def get(self, name, default=None):
        return self._m.get(str(name).lower(), default)


class _Request:
    def __init__(self, method="POST", url="", headers=None):
        self.method = method
        self.url = url
        self.headers = _Headers(headers)


def _fake_sig(pubkey, canonical):
    digest = hashlib.sha256(canonical).hexdigest()[:32]
    return "sig-" + pubkey + "-" + digest


def _harness(accounts, ts_ok=True, devices=None):
    devices = devices or {}

    async def _account_row(_env, name):
        key = str(name or "").strip().lower()
        record = accounts.get(key)
        if record is None:
            return "bi:" + key, None
        record = dict(record)
        record.setdefault("name", key)
        return "bi:" + key, record

    async def _owner_pubkey(_env, owner):
        _bi, record = await _account_row(_env, owner)
        return record.get("pubkey", "") if record else ""

    async def ed25519_verify(pubkey, sig, canonical):
        return bool(pubkey) and sig == _fake_sig(pubkey, canonical)

    async def blind_index(_env, value):
        return "bi:" + str(value or "").strip().lower()

    async def _account_devices_list(_env, account_bi):
        return [dict(device) for device in devices.get(account_bi, [])]

    namespace = _load({
        "re": re,
        "parse_qs": parse_qs,
        "urlparse": urlparse,
        "method_name": lambda request: getattr(request, "method", "GET"),
        "_ts_ok": lambda _ts: ts_ok,
        "_owner_pubkey": _owner_pubkey,
        "_account_row": _account_row,
        "_account_devices_list": _account_devices_list,
        "blind_index": blind_index,
        "_account_kind": lambda record: record.get("kind", "user"),
        "ed25519_verify": ed25519_verify,
        "MAX_NODE_NAME": 63,
    })
    return namespace, object()


def _device(pubkey, capabilities=("owner_sign",), enabled=True):
    return {
        "pubkey": pubkey,
        "capabilities": list(capabilities),
        "enabled": enabled,
    }


def _user(**overrides):
    record = {"pubkey": "PK-alice", "status": "active", "kind": "user"}
    record.update(overrides)
    return record


def _canonical(namespace, proof_name, path, fields=(), node="alice",
               ts="1700000000000"):
    canonical = namespace[proof_name] + "\n" + node + "\n" + path + "\n"
    for value in fields:
        canonical += ("true" if value is True else (value or "")) + "\n"
    return canonical + ts


def _signed_url(namespace, path, proof_name, fields=(), node="alice",
                pubkey="PK-alice", ts="1700000000000"):
    sig = _fake_sig(
        pubkey,
        _canonical(namespace, proof_name, path, fields, node, ts).encode())
    return ("https://forkmesh.test" + path
            + "?node=" + node + "&ts=" + ts + "&sig=" + sig)


def _resolve(namespace, env, method, path, proof, fields=(), body=None,
             **kwargs):
    return asyncio.run(namespace["_org_admin_signed_session"](
        env,
        _Request(method=method,
                 url=_signed_url(namespace, path, proof, fields, **kwargs)),
        body))


def test_every_panel_operation_has_its_own_proof():
    namespace, env = _harness({"alice": _user()})
    cases = (
        ("GET", "/api/orgs", "ORG_ADMIN_LIST_PROOF", (), None),
        ("POST", "/api/orgs", "ORG_ADMIN_CREATE_PROOF",
         ("acme", "Acme Inc", "We make anvils"),
         {"name": "acme", "displayName": "Acme Inc",
          "description": "We make anvils"}),
        ("GET", "/api/orgs/acme", "ORG_ADMIN_READ_PROOF", (), None),
        ("PATCH", "/api/orgs/acme", "ORG_ADMIN_PROFILE_PROOF",
         ("Acme Inc", "Anvils"),
         {"displayName": "Acme Inc", "description": "Anvils"}),
        ("DELETE", "/api/orgs/acme", "ORG_ADMIN_DELETE_PROOF", (), {}),
        ("GET", "/api/orgs/acme/members", "ORG_ADMIN_READ_PROOF", (), None),
        ("POST", "/api/orgs/acme/members", "ORG_ADMIN_MEMBER_SET_PROOF",
         ("bob", "admin"), {"member": "bob", "role": "admin"}),
        ("DELETE", "/api/orgs/acme/members", "ORG_ADMIN_MEMBER_REMOVE_PROOF",
         ("bob",), {"member": "bob"}),
        ("GET", "/api/orgs/acme/teams", "ORG_ADMIN_READ_PROOF", (), None),
        ("POST", "/api/orgs/acme/teams", "ORG_ADMIN_TEAM_SET_PROOF",
         ("infra", "write"), {"team": "infra", "permission": "write"}),
        ("DELETE", "/api/orgs/acme/teams", "ORG_ADMIN_TEAM_REMOVE_PROOF",
         ("infra",), {"team": "infra"}),
        ("GET", "/api/orgs/acme/teams/infra/members", "ORG_ADMIN_READ_PROOF",
         (), None),
        ("POST", "/api/orgs/acme/teams/infra/members",
         "ORG_ADMIN_TEAM_MEMBER_ADD_PROOF", ("bob", True),
         {"member": "bob", "external": True}),
        ("DELETE", "/api/orgs/acme/teams/infra/members",
         "ORG_ADMIN_TEAM_MEMBER_REMOVE_PROOF", ("bob",), {"member": "bob"}),
        ("GET", "/api/orgs/acme/repos", "ORG_ADMIN_READ_PROOF", (), None),
        ("POST", "/api/orgs/acme/repos", "ORG_ADMIN_REPO_LINK_PROOF",
         ("anvil", "alice"), {"repo": "anvil", "node": "alice"}),
        ("DELETE", "/api/orgs/acme/repos", "ORG_ADMIN_REPO_UNLINK_PROOF",
         ("anvil",), {"repo": "anvil"}),
    )
    for method, path, proof, fields, body in cases:
        account_bi, record = _resolve(
            namespace, env, method, path, proof, fields, body)
        assert account_bi == "bi:alice", (method, path)
        assert record["name"] == "alice", (method, path)


def test_no_proof_stands_in_for_another_operation():
    namespace, env = _harness({"alice": _user()})
    path = "/api/orgs/acme/members"
    body = {"member": "bob", "role": "admin"}
    fields = ("bob", "admin")
    for proof in ("ORG_ADMIN_READ_PROOF", "ORG_ADMIN_LIST_PROOF",
                  "ORG_ADMIN_CREATE_PROOF", "ORG_ADMIN_TEAM_SET_PROOF",
                  "ORG_ADMIN_MEMBER_REMOVE_PROOF"):
        assert _resolve(namespace, env, "POST", path, proof, fields,
                        body) == ("", None), proof
    # A read of the roster is never replayable as a change to it.
    assert _resolve(namespace, env, "POST", path, "ORG_ADMIN_READ_PROOF",
                    (), body) == ("", None)
    # ...nor a member change as a removal.
    assert _resolve(namespace, env, "DELETE", path,
                    "ORG_ADMIN_MEMBER_SET_PROOF", ("bob",),
                    {"member": "bob"}) == ("", None)


def test_a_proof_is_bound_to_the_organization_in_its_path():
    namespace, env = _harness({"alice": _user()})
    signed_for = "/api/orgs/acme/members"
    replayed = "https://forkmesh.test/api/orgs/rival/members?node=alice&ts=" \
        "1700000000000&sig=" + _fake_sig(
            "PK-alice",
            _canonical(namespace, "ORG_ADMIN_MEMBER_SET_PROOF", signed_for,
                       ("bob", "owner")).encode())
    assert asyncio.run(namespace["_org_admin_signed_session"](
        env, _Request(method="POST", url=replayed),
        {"member": "bob", "role": "owner"})) == ("", None)
    # The same signature against the organization it names does resolve.
    assert _resolve(namespace, env, "POST", signed_for,
                    "ORG_ADMIN_MEMBER_SET_PROOF", ("bob", "owner"),
                    {"member": "bob", "role": "owner"})[0] == "bi:alice"


def test_a_write_is_bound_to_every_field_that_decides_it():
    namespace, env = _harness({"alice": _user()})
    path = "/api/orgs/acme/members"
    # Signed to make bob a member; replayed with role=owner.
    assert _resolve(namespace, env, "POST", path,
                    "ORG_ADMIN_MEMBER_SET_PROOF", ("bob", "member"),
                    {"member": "bob", "role": "owner"}) == ("", None)
    # ...or against a different member.
    assert _resolve(namespace, env, "POST", path,
                    "ORG_ADMIN_MEMBER_SET_PROOF", ("bob", "member"),
                    {"member": "mallory", "role": "member"}) == ("", None)
    # A team-permission proof is bound to the exact permission it grants.
    teams = "/api/orgs/acme/teams"
    assert _resolve(namespace, env, "POST", teams, "ORG_ADMIN_TEAM_SET_PROOF",
                    ("infra", "read"),
                    {"team": "infra", "permission": "admin"}) == ("", None)
    # ...and a repo link to the exact node namespace it names.
    repos = "/api/orgs/acme/repos"
    assert _resolve(namespace, env, "POST", repos, "ORG_ADMIN_REPO_LINK_PROOF",
                    ("anvil", "alice"),
                    {"repo": "anvil", "node": "mallory"}) == ("", None)


def test_a_signed_write_carries_nothing_the_proof_does_not_cover():
    namespace, env = _harness({"alice": _user()})
    # worldAccess smuggled onto a signed profile edit: the field set no longer
    # matches what the proof covers, so nothing resolves.
    assert _resolve(
        namespace, env, "PATCH", "/api/orgs/acme",
        "ORG_ADMIN_PROFILE_PROOF", ("Acme Inc", "Anvils"),
        {"displayName": "Acme Inc", "description": "Anvils",
         "worldAccess": {"lobby": "public"}}) == ("", None)
    # A missing field is refused just as firmly as an extra one.
    assert _resolve(
        namespace, env, "PATCH", "/api/orgs/acme",
        "ORG_ADMIN_PROFILE_PROOF", ("Acme Inc", "Anvils"),
        {"displayName": "Acme Inc"}) == ("", None)
    # A session token may ride along (the website's shape) without breaking it.
    assert _resolve(
        namespace, env, "PATCH", "/api/orgs/acme",
        "ORG_ADMIN_PROFILE_PROOF", ("Acme Inc", "Anvils"),
        {"displayName": "Acme Inc", "description": "Anvils",
         "sessionToken": "v2.x.y"})[0] == "bi:alice"
    # A GET signs the path only; a body on a read is unsigned payload.
    assert _resolve(namespace, env, "GET", "/api/orgs/acme/members",
                    "ORG_ADMIN_READ_PROOF", (),
                    {"member": "bob"}) == ("", None)


def test_control_characters_in_a_bound_field_are_unsignable():
    """A newline could shift the canonical's field boundaries."""
    namespace, env = _harness({"alice": _user()})
    field = namespace["_org_admin_canonical_field"]
    assert field("Acme Inc") == "Acme Inc"
    assert field(True) == "true"
    assert field(False) == ""
    assert field(None) == ""
    assert field({"lobby": "public"}) is None
    assert field("Acme\nInc") is None
    assert field("Acme\r") is None
    assert len(field("x" * 900)) == namespace["ORG_ADMIN_FIELD_MAX"]
    assert _resolve(
        namespace, env, "PATCH", "/api/orgs/acme",
        "ORG_ADMIN_PROFILE_PROOF", ("Acme\nInc", ""),
        {"displayName": "Acme\nInc", "description": ""}) == ("", None)


def test_the_sensitive_org_subresources_are_never_signable():
    namespace, env = _harness({"alice": _user()})
    for method, path in (
        ("POST", "/api/orgs/acme/bot-tokens"),
        ("GET", "/api/orgs/acme/bot-tokens"),
        ("POST", "/api/orgs/acme/discord"),
        ("POST", "/api/orgs/acme/discord/oauth/start"),
        ("POST", "/api/orgs/acme/succession"),
        ("POST", "/api/orgs/acme/fediverse"),
        ("POST", "/api/orgs/acme/repos/anvil/agent-bots"),
        ("PUT", "/api/orgs/acme/members"),
        ("PATCH", "/api/orgs/acme/members"),
        ("POST", "/api/orgs/acme"),
    ):
        for proof in ("ORG_ADMIN_READ_PROOF", "ORG_ADMIN_PROFILE_PROOF",
                      "ORG_ADMIN_MEMBER_SET_PROOF", "ORG_ADMIN_LIST_PROOF"):
            assert _resolve(namespace, env, method, path, proof) == ("", None), \
                (method, path, proof)


def test_only_an_active_user_account_resolves():
    namespace, env = _harness({
        "alice": _user(),
        "bot": _user(kind="node"),
        "gone": _user(status="closed"),
    })
    assert _resolve(namespace, env, "GET", "/api/orgs",
                    "ORG_ADMIN_LIST_PROOF")[0] == "bi:alice"
    for node, pubkey in (("bot", "PK-alice"), ("gone", "PK-alice"),
                         ("nobody", "PK-alice")):
        assert _resolve(namespace, env, "GET", "/api/orgs",
                        "ORG_ADMIN_LIST_PROOF", node=node,
                        pubkey=pubkey) == ("", None), node
    # A stale timestamp is refused however well-formed the signature is.
    stale, env_stale = _harness({"alice": _user()}, ts_ok=False)
    assert _resolve(stale, env_stale, "GET", "/api/orgs",
                    "ORG_ADMIN_LIST_PROOF") == ("", None)


def test_a_registered_device_key_signs_after_a_restart():
    """adhoc #63's family: the primary pubkey is not the only account key."""
    namespace, env = _harness(
        {"alice": _user()},
        devices={"bi:alice": [
            _device("PK-alice-laptop"),
            _device("PK-alice-retired", enabled=False),
            _device("PK-alice-readonly", capabilities=()),
        ]},
    )
    assert _resolve(namespace, env, "POST", "/api/orgs/acme/members",
                    "ORG_ADMIN_MEMBER_SET_PROOF", ("bob", "member"),
                    {"member": "bob", "role": "member"},
                    pubkey="PK-alice-laptop")[0] == "bi:alice"
    for pubkey in ("PK-alice-retired", "PK-alice-readonly"):
        assert _resolve(namespace, env, "GET", "/api/orgs",
                        "ORG_ADMIN_LIST_PROOF",
                        pubkey=pubkey) == ("", None), pubkey


def test_an_unconfirmed_address_is_held_back_from_writes():
    namespace, env = _harness({
        "alice": _user(email="alice@example.test", email_verified=False)})
    # Reads still work — the same split the session path applies.
    assert _resolve(namespace, env, "GET", "/api/orgs",
                    "ORG_ADMIN_LIST_PROOF")[0] == "bi:alice"
    with pytest.raises(RuntimeError) as raised:
        _resolve(namespace, env, "POST", "/api/orgs/acme/members",
                 "ORG_ADMIN_MEMBER_SET_PROOF", ("bob", "member"),
                 {"member": "bob", "role": "member"})
    assert str(raised.value) == namespace["UNVERIFIED_EMAIL_ERROR"]


def test_the_triple_detector_only_reports_a_complete_credential():
    namespace, _env = _harness({"alice": _user()})
    present = namespace["_org_admin_signed_triple"]
    base = "https://forkmesh.test/api/orgs"
    assert present(_Request(url=base + "?node=alice&ts=1&sig=abc"))
    for query in ("", "?node=alice", "?node=alice&ts=1", "?ts=1&sig=abc",
                  "?node=&ts=1&sig=abc"):
        assert not present(_Request(url=base + query)), query
