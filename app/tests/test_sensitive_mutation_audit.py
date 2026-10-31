#!/usr/bin/env python3
"""Focused audit coverage for privileged mutation handlers.

The tests execute AST-extracted production handlers with a small fake runtime.
They pin success, denial, and storage-failure outcomes without requiring the
Workers JavaScript runtime or a real D1 database.
"""

import ast
import importlib.util
import asyncio
import hashlib
import json
import re
import sys
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import urlparse

import pytest

from worker_test_helpers import json_from_request_double

ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"

sys.path.insert(0, str(ROOT / "src"))
import plugin_data  # noqa: E402
ENTRY_TEXT = (
    # entry.py + its lazily-split domain modules
    (ENTRY.parent / "catalog_routes.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "forkbot.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "fediverse_routes.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + ENTRY.read_text(encoding="utf-8")
    + "\n\n\n"
)


def _organization_rbac():
    spec = importlib.util.spec_from_file_location(
        "organization_rbac", ROOT / "src" / "organization_rbac.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _load(*names, extra_globals=None):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    selected = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in names
    ]
    found = {node.name for node in selected}
    assert found == set(names), "missing functions: %s" % (
        sorted(set(names) - found))
    namespace = dict(extra_globals or {})
    namespace.setdefault("bounded_json_request", json_from_request_double)
    namespace.setdefault("_org_rbac", _organization_rbac())
    if (
        "_org_management_session" not in namespace
        and "_account_session_record" in namespace
    ):
        namespace["_org_management_session"] = namespace[
            "_account_session_record"]
    exec(compile(ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[])), str(ENTRY), "exec"),
         namespace)
    return namespace


def _run(coro):
    return asyncio.run(coro)


class _Date:
    @staticmethod
    def now():
        return 1_700_000_000_000


class _Request:
    def __init__(self, data, path="/", method="POST"):
        self._data = data
        self.url = "https://forkmesh.test" + path
        self.method = method

    async def json(self):
        return self._data


def _json_response(payload, status=200, **kwargs):
    return {"payload": payload, "status": status, **kwargs}


def _clean_string(value, limit):
    return str(value or "")[:limit]


async def _noop(*args, **kwargs):
    return None


def _audit_recorder():
    rows = []

    async def audit(env, actor, action, target_type="", target="",
                    outcome="success", details=None):
        rows.append({
            "actor": actor,
            "action": action,
            "targetType": target_type,
            "target": target,
            "outcome": outcome,
            "details": details or {},
        })

    return rows, audit


def _ap_handler(*, authorized=True, fail_mutation=False):
    audits, audit = _audit_recorder()
    writes = []

    async def d1_run(env, sql, *params):
        writes.append((sql, params))
        if fail_mutation:
            raise RuntimeError("d1 mutation failed")

    async def admin_authorized(*args):
        return authorized

    async def settings(env, fresh=False):
        return {"enabled": True, "blocked": set()}

    ns = _load("_admin_ap_update", extra_globals={
        "ensure_schema": _noop,
        "clean_string": _clean_string,
        "MAX_NODE_NAME": 64,
        "Date": _Date,
        "d1_run": d1_run,
        "_admin_authorized": admin_authorized,
        "_audit_sensitive_action": audit,
        "_ap_settings": settings,
        "ap": SimpleNamespace(valid_domain=lambda value: value == "bad.example"),
        "json_response": _json_response,
    })
    return ns["_admin_ap_update"], audits, writes


@pytest.mark.parametrize("action", ["enable", "disable", "block", "unblock"])
def test_activitypub_admin_mutations_audit_success(action):
    handler, audits, writes = _ap_handler()
    response = _run(handler(None, _Request({
        "node": "admin", "ts": "1", "sig": "ok", "action": action,
        "domain": "bad.example" if action in ("block", "unblock") else "",
    })))
    assert response["status"] == 200
    assert writes
    assert audits[-1]["action"] == "admin.activitypub_" + action
    assert audits[-1]["outcome"] == "success"


def test_activitypub_admin_mutation_audits_denial_and_failure():
    handler, audits, writes = _ap_handler(authorized=False)
    response = _run(handler(None, _Request({
        "node": "mallory", "ts": "1", "sig": "bad",
        "action": "block", "domain": "bad.example",
    })))
    assert response["status"] == 401
    assert writes == []
    assert audits[-1]["outcome"] == "denied"

    handler, audits, _ = _ap_handler(fail_mutation=True)
    with pytest.raises(RuntimeError, match="d1 mutation failed"):
        _run(handler(None, _Request({
            "node": "admin", "ts": "1", "sig": "ok",
            "action": "enable", "domain": "",
        })))
    assert audits[-1]["action"] == "admin.activitypub_enable"
    assert audits[-1]["outcome"] == "failed"


async def _bool_result(value):
    return value


def _org_members_handler(*, caller_role="owner", fail_mutation=False):
    audits, audit = _audit_recorder()

    async def org_row(env, org):
        return "org-bi", {"name": org}

    async def session(*args):
        return "bi:owner", {"name": "owner"}

    async def org_role(env, org_bi, account):
        return caller_role

    async def blind_index(env, value):
        return "bi:" + value

    async def d1_first(env, sql, *params):
        if "SELECT role FROM org_members" in sql:
            return None
        if "COUNT(*)" in sql:
            return {"n": 0}
        raise AssertionError("unexpected query: " + sql)

    async def account_row(env, member):
        return "bi:" + member, {"name": member, "status": "active"}

    async def d1_run(env, sql, *params):
        if fail_mutation:
            raise RuntimeError("organization mutation failed")

    ns = _load("org_members_handler", extra_globals={
        "ensure_schema": _noop,
        "method_name": lambda request: request.method,
        "_org_row": org_row,
        "_org_management_session": session,
        "_org_role": org_role,
        "clean_string": _clean_string,
        "MAX_NODE_NAME": 64,
        "blind_index": blind_index,
        "d1_first": d1_first,
        "_org_owner_count": lambda *args: _count_result(2),
        "ORG_ROLES": ("owner", "admin", "member"),
        "_account_row": account_row,
        "MAX_ORG_MEMBERS": 200,
        "d1_run": d1_run,
        "_audit_sensitive_action": audit,
        "enqueue_notification": _noop,
        "Date": _Date,
        "json_response": _json_response,
    })
    return ns["org_members_handler"], audits


async def _count_result(value):
    return value


def test_organization_member_mutation_audits_success_denial_and_failure():
    handler, audits = _org_members_handler()
    response = _run(handler(None, _Request({
        "member": "alice", "role": "member",
    }), "acme"))
    assert response["status"] == 200
    assert audits[-1]["action"] == "organization.member_add"
    assert audits[-1]["outcome"] == "success"

    handler, audits = _org_members_handler(caller_role="member")
    response = _run(handler(None, _Request({
        "member": "alice", "role": "admin",
    }), "acme"))
    assert response["status"] == 403
    assert audits[-1]["action"] == "organization.member_change"
    assert audits[-1]["outcome"] == "denied"

    handler, audits = _org_members_handler(fail_mutation=True)
    with pytest.raises(RuntimeError, match="organization mutation failed"):
        _run(handler(None, _Request({
            "member": "alice", "role": "member",
        }), "acme"))
    assert audits[-1]["action"] == "organization.member_add"
    assert audits[-1]["outcome"] == "failed"


def test_all_organization_privilege_mutators_emit_audit_outcomes():
    expected_actions = {
        "org_members_handler": (
            "organization.member_add", "organization.member_remove",
            "organization.role_change"),
        "org_teams_handler": (
            "organization.team_create", "organization.team_delete",
            "organization.team_permission_change"),
        "org_team_members_handler": (
            "organization.team_member_grant",
            "organization.team_member_revoke"),
        "org_repos_handler": (
            "organization.repo_link", "organization.repo_unlink"),
    }
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    handlers = {
        node.name: ast.get_source_segment(ENTRY_TEXT, node)
        for node in tree.body
        if isinstance(node, ast.AsyncFunctionDef) and node.name in expected_actions
    }
    for name, actions in expected_actions.items():
        source = handlers[name]
        assert "_audit_sensitive_action(" in source
        assert '"success"' in source
        assert '"denied"' in source
        assert '"failed"' in source
        for action in actions:
            assert action in source


def _catalog_delete_handler(
        *, authorized=True, fail_mutation=False, session_actor="",
        session_owns=False, admin_account=""):
    audits, audit = _audit_recorder()
    writes = []

    async def d1_first(env, sql, *params):
        return {"data": "encrypted-catalog-record"}

    async def d1_run(env, sql, *params):
        writes.append((sql, params))
        if fail_mutation:
            raise RuntimeError("repository delete failed")

    ns = _load("catalog_handler", extra_globals={
        "ensure_schema": _noop,
        "method_name": lambda request: request.method,
        "parse_qs": __import__("urllib.parse", fromlist=["parse_qs"]).parse_qs,
        "urlparse": urlparse,
        "safe_segment": lambda value: str(value or ""),
        "clean_string": _clean_string,
        "_authed_account_name": (
            lambda env, request: _text_result(session_actor)),
        "_repo_admin_allowed": (
            lambda env, actor, owner, repo: _bool_result(session_owns)),
        "_org_repo_node": lambda env, owner, repo: _text_result(""),
        "Date": _Date,
        "LOGIN_MAX_SKEW_MS": 300_000,
        "blind_index": lambda env, value: _text_result("bi:" + value),
        "d1_first": d1_first,
        "decrypt_row": lambda env, value: _dict_result({"owner": "alice"}),
        "_owner_pubkey": lambda env, owner: _text_result("owner-public-key"),
        "_verify_owner_signature": (
            lambda *args: _bool_result(authorized)),
        "ed25519_verify": (
            lambda *args: _bool_result(authorized)),
        "_record_repo_delete_tombstone": _noop,
        "invalidate_public_catalog_snapshots": _noop,
        "_delete_repo_scoped_state": _noop,
        "_delete_repo_named_state": _noop,
        "MAX_NODE_NAME": 63,
        "_admin_authorized": (
            lambda env, user, ts, sig, canonical: _dict_result(
                {"name": user} if user and user == admin_account else None)),
        "d1_run": d1_run,
        "_delete_bounties_namespace": _noop,
        "purge_catalog_related_caches": _noop,
        "schedule_public_catalog_republish": _noop,
        "_bump_catalog_rev": _noop,
        "_audit_sensitive_action": audit,
        "json_response": _json_response,
    })
    return ns["catalog_handler"], audits, writes


async def _text_result(value):
    return value


async def _dict_result(value):
    return value


def _catalog_delete_request():
    return _Request(
        {},
        path=(
            "/api/catalog?owner=alice&name=private-project"
            "&ts=1700000000000&sig=owner-signature"
        ),
        method="DELETE",
    )


def test_repository_delete_audits_success_denial_and_failure():
    handler, audits, writes = _catalog_delete_handler()
    response = _run(handler(None, _catalog_delete_request()))
    assert response["status"] == 200
    assert writes
    assert audits[-1] == {
        "actor": "alice",
        "action": "repository.delete",
        "targetType": "repository",
        "target": "alice/private-project",
        "outcome": "success",
        "details": {"deleted": True},
    }

    handler, audits, writes = _catalog_delete_handler(authorized=False)
    response = _run(handler(None, _catalog_delete_request()))
    assert response["status"] == 401
    assert writes == []
    assert audits[-1]["action"] == "repository.delete"
    assert audits[-1]["outcome"] == "denied"
    assert set(audits[-1]["details"]) == {"reason"}

    handler, audits, _ = _catalog_delete_handler(fail_mutation=True)
    with pytest.raises(RuntimeError, match="repository delete failed"):
        _run(handler(None, _catalog_delete_request()))
    assert audits[-1]["action"] == "repository.delete"
    assert audits[-1]["outcome"] == "failed"
    assert set(audits[-1]["details"]) == {"reason"}


def test_repository_delete_accepts_only_the_owning_web_session():
    handler, audits, writes = _catalog_delete_handler(
        session_actor="alice-user",
        session_owns=True,
        authorized=False,
    )
    request = _Request(
        {},
        path="/api/repositories?owner=alice&name=private-project",
        method="DELETE",
    )
    response = _run(handler(None, request))
    assert response["status"] == 200
    assert writes
    assert audits[-1]["outcome"] == "success"

    handler, audits, writes = _catalog_delete_handler(
        session_actor="mallory",
        session_owns=False,
    )
    response = _run(handler(None, request))
    assert response["status"] == 403
    assert response["payload"]["error"] == "not_authorized"
    assert writes == []
    assert audits[-1]["actor"] == "mallory"
    assert audits[-1]["outcome"] == "denied"


def _shares_handler(*, authorized=True, fail_mutation=False):
    audits, audit = _audit_recorder()
    writes = []

    async def owner_pubkey(env, account):
        return "public-key:" + account

    async def d1_first(env, sql, *params):
        if "COUNT(*)" in sql:
            return {"n": 0}
        return None

    async def d1_run(env, sql, *params):
        writes.append((sql, params))
        if fail_mutation:
            raise RuntimeError("collaborator mutation failed")

    async def recipient_bundles(env, account_bis):
        return {
            account_bis[0]: {
                "keyId": "public-key-id",
                "publicBundle": {
                    "v": 1, "x25519": "public-x", "mlkem768": "public-mlkem",
                },
                "createdAt": 1,
            }
        }

    ns = _load("shares_handler", extra_globals={
        "ensure_schema": _noop,
        "method_name": lambda request: request.method,
        "blind_index": lambda env, value: _text_result("bi:" + value),
        "_owner_pubkey": owner_pubkey,
        "parse_qs": __import__("urllib.parse", fromlist=["parse_qs"]).parse_qs,
        "urlparse": urlparse,
        "clean_string": _clean_string,
        "MAX_NODE_NAME": 64,
        "_ts_ok": lambda value: True,
        "ed25519_verify": lambda *args: _bool_result(authorized),
        "_audit_sensitive_action": audit,
        "d1_run": d1_run,
        "d1_first": d1_first,
        "_active_public_recipient_bundles": recipient_bundles,
        "MAX_REPO_GRANTEES": 100,
        "Date": _Date,
        "encrypt_row": lambda env, value: _text_result("encrypted-share"),
        "enqueue_notification": _noop,
        "repo_web_href": lambda owner, repo: "/" + owner + "/" + repo,
        "json_response": _json_response,
    })
    return ns["shares_handler"], audits, writes


@pytest.mark.parametrize(
    ("action", "expected_action"),
    [
        ("add", "repository.collaborator_grant"),
        ("remove", "repository.collaborator_revoke"),
    ],
)
def test_private_collaborator_mutations_audit_success(action, expected_action):
    handler, audits, writes = _shares_handler()
    response = _run(handler(None, _Request({
        "action": action,
        "grantee": "bob",
        "ts": "1700000000000",
        "sig": "owner-signature",
    }), "alice", "private-project"))
    assert response["status"] == 200
    assert writes
    assert audits[-1]["action"] == expected_action
    assert audits[-1]["targetType"] == "repository_collaborator"
    assert audits[-1]["outcome"] == "success"
    assert "sig" not in json.dumps(audits)


def test_private_collaborator_mutations_audit_denial_and_failure():
    handler, audits, writes = _shares_handler(authorized=False)
    response = _run(handler(None, _Request({
        "action": "add", "grantee": "bob", "ts": "1700000000000",
        "sig": "forged-signature",
    }), "alice", "private-project"))
    assert response["status"] == 401
    assert writes == []
    assert audits[-1]["action"] == "repository.collaborator_grant"
    assert audits[-1]["outcome"] == "denied"

    handler, audits, _ = _shares_handler(fail_mutation=True)
    with pytest.raises(RuntimeError, match="collaborator mutation failed"):
        _run(handler(None, _Request({
            "action": "remove", "grantee": "bob", "ts": "1700000000000",
            "sig": "owner-signature",
        }), "alice", "private-project"))
    assert audits[-1]["action"] == "repository.collaborator_revoke"
    assert audits[-1]["outcome"] == "failed"


def _org_delete_handler(*, caller_role="owner", fail_mutation=False):
    audits, audit = _audit_recorder()

    async def d1_run(env, sql, *params):
        if fail_mutation:
            raise RuntimeError("organization delete failed")

    ns = _load("org_handler", extra_globals={
        "ensure_schema": _noop,
        "method_name": lambda request: request.method,
        "_org_row": lambda env, org: _pair_result(
            "org-bi", {"name": "acme", "data": "encrypted"}),
        "_org_management_session": lambda *args: _pair_result(
            "bi:alice", {"name": "alice"}),
        "_org_role": lambda *args: _text_result(caller_role),
        "d1_all": lambda *args: _list_result([{"repo": "widget"}]),
        "d1_run": d1_run,
        "_ap_digest_scope_bi": lambda env, scope: _text_result("scope-bi"),
        "_ORG_ALIAS_MEMO": SimpleNamespace(clear=lambda: None),
        "purge_catalog_related_caches": _noop,
        "schedule_public_catalog_republish": _noop,
        "_audit_sensitive_action": audit,
        "plugin_data": plugin_data,
        "org_office_chat": SimpleNamespace(dissolve=_noop),
        "_PluginPurgeRuntime": lambda env: SimpleNamespace(
            d1_all=lambda sql, *args: _list_result([]),
            d1_first=lambda sql, *args: _list_result(None),
            d1_run=lambda sql, *args: d1_run(env, sql, *args)),
        "json_response": _json_response,
    })
    return ns["org_handler"], audits


async def _pair_result(first, second):
    return first, second


async def _list_result(value):
    return value


def test_organization_delete_audits_success_denial_and_failure():
    handler, audits = _org_delete_handler()
    response = _run(handler(
        None, _Request({}, method="DELETE"), "acme"))
    assert response["status"] == 200
    assert audits[-1]["action"] == "organization.delete"
    assert audits[-1]["outcome"] == "success"

    handler, audits = _org_delete_handler(caller_role="admin")
    response = _run(handler(
        None, _Request({}, method="DELETE"), "acme"))
    assert response["status"] == 403
    assert audits[-1]["action"] == "organization.delete"
    assert audits[-1]["outcome"] == "denied"

    handler, audits = _org_delete_handler(fail_mutation=True)
    with pytest.raises(RuntimeError, match="organization delete failed"):
        _run(handler(None, _Request({}, method="DELETE"), "acme"))
    assert audits[-1]["action"] == "organization.delete"
    assert audits[-1]["outcome"] == "failed"


def _org_fediverse_handler(
    *, caller_role="owner", fail_settings=False, fail_queue=False
):
    audits, audit = _audit_recorder()

    async def d1_run(env, sql, *params):
        if fail_settings and "INSERT INTO ap_org_digest_settings" in sql:
            raise RuntimeError("fediverse settings failed")
        if fail_queue and "DELETE FROM ap_digest_queues" in sql:
            raise RuntimeError("fediverse queue purge failed")

    async def d1_all(env, sql, *params):
        if "SELECT repo FROM org_repos" in sql:
            return [{"repo": "widget"}]
        return []

    ns = _load("org_fediverse_handler", extra_globals={
        "ensure_schema": _noop,
        "method_name": lambda request: request.method,
        "_org_row": lambda env, org: _pair_result(
            "org-bi", {"name": "acme"}),
        "_account_session_record": lambda *args: _pair_result(
            "bi:alice", {"name": "alice"}),
        "_org_role": lambda *args: _text_result(caller_role),
        "_ap_org_digest_settings_get": lambda *args: _dict_result({
            "enabled": True, "preview": False}),
        "fedi_digest": SimpleNamespace(
            normalize_controls=lambda value: {
                "enabled": bool(value["enabled"]), "preview": False}),
        "d1_run": d1_run,
        "d1_all": d1_all,
        "_ap_digest_scope_bi": lambda env, scope: _text_result("scope-bi"),
        "MAX_ORG_REPOS": 100,
        "_audit_sensitive_action": audit,
        "json": json,
        "Date": _Date,
        "clean_string": _clean_string,
        "MAX_REPO_SEGMENT": 80,
        "MAX_NODE_NAME": 64,
        "_repo_is_private": lambda *args: _bool_result(False),
        "_ap_repo_settings_get": lambda *args: _dict_result({
            "federate": True, "broadcastEvents": True}),
        "_ap_digest_preview": lambda *args: _dict_result({}),
        "json_response": _json_response,
    })
    return ns["org_fediverse_handler"], audits


@pytest.mark.parametrize(
    ("enabled", "action"),
    [
        (True, "organization.fediverse_enable"),
        (False, "organization.fediverse_disable"),
    ],
)
def test_org_fediverse_toggle_and_queue_purge_audits_success(enabled, action):
    handler, audits = _org_fediverse_handler()
    response = _run(handler(
        None, _Request({"enabled": enabled}), "acme"))
    assert response["status"] == 200
    assert any(
        row["action"] == action and row["outcome"] == "success"
        for row in audits)
    if not enabled:
        assert any(
            row["action"] == "organization.fediverse_queue_purge"
            and row["outcome"] == "success"
            for row in audits)


def test_org_fediverse_mutations_audit_denial_and_each_failure_stage():
    handler, audits = _org_fediverse_handler(caller_role="member")
    response = _run(handler(
        None, _Request({"enabled": False}), "acme"))
    assert response["status"] == 403
    assert audits[-1]["action"] == "organization.fediverse_disable"
    assert audits[-1]["outcome"] == "denied"

    handler, audits = _org_fediverse_handler(fail_settings=True)
    with pytest.raises(RuntimeError, match="fediverse settings failed"):
        _run(handler(None, _Request({"enabled": True}), "acme"))
    assert audits[-1]["action"] == "organization.fediverse_enable"
    assert audits[-1]["outcome"] == "failed"

    handler, audits = _org_fediverse_handler(fail_queue=True)
    with pytest.raises(RuntimeError, match="fediverse queue purge failed"):
        _run(handler(None, _Request({"enabled": False}), "acme"))
    assert any(
        row["action"] == "organization.fediverse_disable"
        and row["outcome"] == "success"
        for row in audits)
    assert audits[-1]["action"] == "organization.fediverse_queue_purge"
    assert audits[-1]["outcome"] == "failed"


def test_sensitive_audit_persistence_blind_indexes_targets_and_sanitizes_details():
    writes = []

    async def blind_index(env, value):
        return hashlib.sha256(value.encode()).hexdigest()

    async def d1_run(env, sql, *params):
        writes.append((sql, params))

    controls = SimpleNamespace(
        sanitize_audit_details=lambda details: {
            "reason": str(details.get("reason") or "")[:160]})
    ns = _load("_audit_sensitive_action", extra_globals={
        "ensure_schema": _noop,
        "clean_string": _clean_string,
        "MAX_NODE_NAME": 64,
        "re": re,
        "blind_index": blind_index,
        "security_control": controls,
        "d1_run": d1_run,
        "Date": _Date,
        "json": json,
    })
    raw_target = "alice/private-project/bob"
    _run(ns["_audit_sensitive_action"](
        None, "alice", "repository.collaborator_grant",
        "repository_collaborator", raw_target, "denied",
        {"reason": "bad_signature", "signature": "must-not-persist"}))
    assert len(writes) == 1
    params = writes[0][1]
    assert raw_target not in json.dumps(params)
    assert "must-not-persist" not in json.dumps(params)
    assert params[5] == hashlib.sha256(
        ("audit:repository_collaborator:" + raw_target).encode()).hexdigest()
