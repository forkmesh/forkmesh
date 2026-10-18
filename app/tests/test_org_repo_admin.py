import asyncio
import re
from pathlib import Path

import pytest

from _org_repo_admin_stubs import (
    REPO_ADMIN_FUNCTIONS, d1_for, entry_functions, link, member, org_database,
    team)

SRC = Path(__file__).resolve().parents[1] / "src"


def run(coroutine):
    return asyncio.run(coroutine)


class Request:
    def __init__(self, method="GET", actor=""):
        self.method = method
        self.actor = actor


def harness():
    db = org_database()
    link(db, "acme", "alice", "site")
    member(db, "acme", "alice", "owner")
    member(db, "acme", "owen", "owner")
    member(db, "acme", "ada", "admin")
    member(db, "acme", "max", "maintainer")
    member(db, "acme", "mia", "member")
    member(db, "acme", "aud", "auditor")
    member(db, "acme", "tia", "member")
    member(db, "acme", "gus", "guest")
    team(db, "acme", "release", "admin", ["tia", "aud", "gus"])
    member(db, "other", "olga", "owner")
    d1_all, d1_first, _ = d1_for(db)
    nodes = {"alice": "alice", "laptop": "alice"}

    async def owns(_env, actor, node):
        actor = str(actor or "").lower()
        node = str(node or "").lower()
        return bool(actor and node) and (
            actor == node or nodes.get(node) == actor)

    async def authed(_env, request, data=None):
        return request.actor

    def json_response(data, status=200, **kwargs):
        return {"status": status, "data": data, "headers": kwargs}

    async def nothing(*_args):
        return None

    ns = entry_functions(
        list(REPO_ADMIN_FUNCTIONS) + [
            "repo_viewer_permission_handler", "_authorize_repo_owner_web"], {
            "d1_all": d1_all, "d1_first": d1_first,
            "_account_owns_node": owns,
            "_authed_account_name": authed,
            "json_response": json_response,
            "method_name": lambda request: request.method,
            "clean_string": lambda value, limit: str(value or "")[:limit],
            "MAX_NODE_NAME": 63,
            "ensure_schema": nothing,
            "_owner_pubkey": nothing,
        })
    return ns, db


@pytest.mark.parametrize("actor, allowed", [
    ("alice", True),
    ("owen", True),
    ("ada", True),
    ("tia", True),
    ("max", False),
    ("mia", False),
    ("aud", False),
    ("gus", False),
    ("olga", False),
    ("mallory", False),
    ("", False),
])
def test_repository_admin_follows_the_organization_permission(actor, allowed):
    ns, _db = harness()
    assert run(ns["_repo_admin_allowed"](
        None, actor, "alice", "site")) is allowed
    assert run(ns["_repo_admin_allowed"](
        None, actor, "alice", "SITE")) is allowed


def test_an_unlinked_repository_is_administered_by_its_node_owner_only():
    ns, _db = harness()
    assert run(ns["_repo_admin_allowed"](None, "alice", "alice", "notes"))
    assert not run(ns["_repo_admin_allowed"](None, "owen", "alice", "notes"))
    assert not run(ns["_repo_admin_allowed"](None, "ada", "alice", "notes"))


def test_the_owning_user_of_a_node_still_administers_it():
    ns, db = harness()
    link(db, "acme", "laptop", "tool")
    assert run(ns["_repo_admin_allowed"](None, "alice", "laptop", "tool"))
    assert run(ns["_repo_admin_allowed"](None, "ada", "laptop", "tool"))
    assert not run(ns["_repo_admin_allowed"](None, "max", "laptop", "tool"))


def test_a_lookup_error_never_grants_admin():
    ns, _db = harness()

    async def broken(*_args):
        raise RuntimeError("db down")

    ns["d1_all"] = broken
    assert not run(ns["_repo_admin_allowed"](None, "ada", "alice", "site"))
    assert run(ns["_repo_admin_allowed"](None, "alice", "alice", "site"))


@pytest.mark.parametrize("actor, permission", [
    ("alice", "admin"),
    ("owen", "admin"),
    ("ada", "admin"),
    ("tia", "admin"),
    ("max", "maintain"),
    ("mia", "read"),
    ("aud", "read"),
    ("gus", "read"),
    ("mallory", ""),
    ("", ""),
])
def test_viewer_permission_reports_the_signed_in_account(actor, permission):
    ns, _db = harness()
    response = run(ns["repo_viewer_permission_handler"](
        None, Request("GET", actor), "alice", "site"))
    assert response["status"] == 200
    assert response["data"] == {
        "ok": True, "viewer": actor, "permission": permission,
        "viewerCanAdmin": permission == "admin"}
    assert response["headers"]["cache_control"] == "private, no-store"


def test_viewer_permission_is_read_only():
    ns, _db = harness()
    response = run(ns["repo_viewer_permission_handler"](
        None, Request("POST", "ada"), "alice", "site"))
    assert response["status"] == 405


@pytest.mark.parametrize("actor, allowed", [
    ("alice", True), ("owen", True), ("ada", True), ("max", False),
    ("mia", False), ("mallory", False), ("", False)])
def test_settings_endpoints_share_the_repository_admin_gate(actor, allowed):
    ns, _db = harness()
    assert run(ns["_authorize_repo_owner_web"](
        None, Request("POST", actor), "alice", "site", {})) is allowed


def test_settings_endpoints_use_the_shared_gate():
    entry = (SRC / "entry.py").read_text(encoding="utf-8")
    about = entry[entry.index("async def repo_about_handler"):]
    about = about[:about.index("\nasync def ")]
    assert "_repo_admin_allowed(env, actor, owner, repo)" in about
    web = entry[entry.index("async def _authorize_repo_owner_web"):]
    web = web[:web.index("\nasync def ")]
    assert "_repo_admin_allowed(env, actor, owner, repo)" in web
    assert "_org_owner_of_linked_repo" not in web + about
    for module, handler in (
            ("repository_webhooks.py", "repo_webhooks_handler"),
            ("repository_provider_mirror.py",
             "repo_provider_mirror_handler"),
            ("fediverse_routes.py", "ap_digest_handler")):
        text = (SRC / module).read_text(encoding="utf-8")
        body = text[text.index("async def " + handler):]
        body = body[:body.index("\nasync def ")] if "\nasync def " in body \
            else body
        assert "_authorize_repo_owner_web(env, request, owner, repo" in body
    secrets = (SRC / "actions_secrets.py").read_text(encoding="utf-8")
    assert "_repo_admin_allowed(env, actor, owner, repo)" in secrets
    catalog = (SRC / "catalog_routes.py").read_text(encoding="utf-8")
    assert "_repo_admin_allowed(env, session_actor, owner, name)" in catalog
    assert "_org_repo_node(env, owner, name)" in catalog
    runtime = (SRC / "app_runtime.py").read_text(encoding="utf-8")
    assert re.search(
        r'action == "viewer-permission".*?repo_viewer_permission_handler',
        runtime, re.S)
    rewrite = runtime.index("org_alias_rewrite(self.env, request, url)")
    assert rewrite < runtime.index('action == "viewer-permission"')
