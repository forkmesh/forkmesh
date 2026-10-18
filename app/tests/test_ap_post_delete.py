#!/usr/bin/env python3
"""Repo owner manages federated posts (issue #426).

POST /api/repo/{owner}/{repo}/ap-posts lets the repo owner (session token, or
the desktop's owner-key signed token) list the repo actor's federated posts and
delete one. A delete drops the local ap_objects row AND queues a
Delete(Tombstone) to every follower inbox so the post disappears from Mastodon.

These load the real handler out of src/entry.py and pin:

  * an unauthorized caller is rejected before any read/write;
  * list returns the repo actor's posts (id/content/url/published);
  * delete removes the local object and queues a Delete to each follower
    (deduping shared inboxes), then reports federated=True;
  * delete of another actor's / unknown object 404s and writes nothing;
  * delete still drops the local object when federation is disabled (no
    outbox rows), honoring the owner's intent.

Run: python3 -m pytest cloudflare_worker/tests/test_ap_post_delete.py
"""

import ast
import asyncio
import json
from pathlib import Path
from types import SimpleNamespace

from worker_test_helpers import json_from_request_double

ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
ENTRY_TEXT = (
    # entry.py + its lazily-split domain modules
    (ENTRY.parent / "forkbot.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "fediverse_routes.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + ENTRY.read_text(encoding="utf-8")
    + "\n\n\n"
)

MODULE_PATH = ROOT / "src" / "activitypub.py"
import importlib.util  # noqa: E402
spec = importlib.util.spec_from_file_location("activitypub", MODULE_PATH)
ap = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ap)


def _load(*names, extra_globals=None):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    selected = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in names
    ]
    found = {n.name for n in selected}
    missing = set(names) - found
    assert not missing, "missing functions: %s" % sorted(missing)
    module = ast.fix_missing_locations(ast.Module(body=selected, type_ignores=[]))
    namespace = dict(extra_globals or {})
    namespace.setdefault("bounded_json_request", json_from_request_double)
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace


def _run(coro):
    return asyncio.new_event_loop().run_until_complete(coro)


def _json_response(data, status=200, **_kw):
    return {"status": status, "data": data}


def _env(log, *, authed_owner=False, enabled=True, objects=None, followers=None):
    objects = objects if objects is not None else {}
    followers = followers if followers is not None else []

    async def ensure_schema(env):
        return None

    async def _authed_account_name(env, request, data):
        return "alice" if authed_owner else ""

    async def _account_owns_node(env, actor, owner):
        return authed_owner and actor == "alice" and owner == "alice"

    async def _is_admin(env, actor):
        return False

    async def _owner_pubkey(env, owner):
        return "PUB"

    async def ed25519_verify(pub, sig, canonical):
        return False

    async def _ap_actor_bi(env, kind, handle):
        return "actor:" + kind + ":" + handle

    async def _ap_enabled(env):
        return enabled

    async def d1_all(env, sql, *args):
        if "FROM ap_objects" in sql:
            return list(objects.values())
        if "FROM ap_followers" in sql:
            return list(followers)
        raise AssertionError("unexpected d1_all: " + sql)

    async def d1_first(env, sql, *args):
        if "FROM ap_objects" in sql:
            return objects.get(args[0])
        raise AssertionError("unexpected d1_first: " + sql)

    async def d1_run(env, sql, *args):
        log.append(("d1_run", sql, args))
        if "DELETE FROM ap_objects" in sql:
            objects.pop(args[0], None)

    async def decrypt_row(env, data):
        return data

    async def encrypt_row(env, rec):
        return rec

    async def _ap_drain_outbox(env, limit):
        log.append(("drain", limit))

    async def edge_cache_delete(key):
        log.append(("purge", key))

    async def _ap_public_repo_owner(env, request, owner, repo):
        return owner

    return _load(
        "ap_posts_handler", "_authorize_repo_owner_web",
        extra_globals={
            "_ap_public_repo_owner": _ap_public_repo_owner,
            "json_response": _json_response,
            "method_name": lambda request: request.method,
            "ensure_schema": ensure_schema,
            "_authed_account_name": _authed_account_name,
            "_account_owns_node": _account_owns_node,
        "_repo_admin_allowed": (
            lambda env, actor, owner, repo: _account_owns_node(
                env, actor, owner)),
            "_is_admin": _is_admin,
            "_owner_pubkey": _owner_pubkey,
            "ed25519_verify": ed25519_verify,
            "_ap_actor_bi": _ap_actor_bi,
            "_ap_enabled": _ap_enabled,
            "d1_all": d1_all,
            "d1_first": d1_first,
            "d1_run": d1_run,
            "decrypt_row": decrypt_row,
            "encrypt_row": encrypt_row,
            "_ap_drain_outbox": _ap_drain_outbox,
            "edge_cache_delete": edge_cache_delete,
            "clean_string": lambda value, cap: str(value or "")[:cap],
            "_ap_origin": lambda env, request=None: "https://forkmesh.com",
            "_ap_actor_url": lambda origin, kind, handle:
                origin + "/ap/repos/alice/proj",
            "ap": ap,
            "json": json,
            "AP_ACTOR_REPO": "repo",
            "AP_IMMEDIATE_DELIVERIES": 5,
            "Date": SimpleNamespace(now=lambda: 1750000000000),
        })


def _post(payload):
    async def read_json():
        return payload
    return SimpleNamespace(method="POST", json=read_json,
                           url="https://forkmesh.com/api/repo/alice/proj/ap-posts",
                           headers={})


def _outbox_inserts(log):
    return [e for e in log if e[0] == "d1_run"
            and "INSERT INTO ap_outbox" in e[1]]


def test_unauthorized_caller_rejected():
    log = []
    ns = _env(log, authed_owner=False)
    resp = _run(ns["ap_posts_handler"](
        None, _post({"action": "list"}), "alice", "proj"))
    assert resp["status"] == 403
    assert log == []


def test_list_returns_repo_posts():
    objs = {"uuid-1": {
        "object_uuid": "uuid-1", "published": 1750000000000,
        "data": {"note": {"content": "<p>new issue</p>",
                          "url": "https://forkmesh.com/alice/proj/issues/1"}}}}
    log = []
    ns = _env(log, authed_owner=True, objects=objs)
    resp = _run(ns["ap_posts_handler"](
        None, _post({"action": "list"}), "alice", "proj"))
    assert resp["status"] == 200
    assert resp["data"]["posts"] == [{
        "id": "uuid-1", "content": "<p>new issue</p>",
        "url": "https://forkmesh.com/alice/proj/issues/1",
        "published": 1750000000000}]


def test_delete_removes_object_and_federates_tombstone():
    objs = {"uuid-1": {"object_uuid": "uuid-1", "published": 1,
                       "data": {"note": {}}}}
    followers = [
        {"inbox": "https://mastodon.social/users/a/inbox",
         "shared_inbox": "https://mastodon.social/inbox"},
        {"inbox": "https://mastodon.social/users/b/inbox",
         "shared_inbox": "https://mastodon.social/inbox"},  # dupe shared inbox
        {"inbox": "https://fosstodon.org/users/c/inbox", "shared_inbox": ""},
    ]
    log = []
    ns = _env(log, authed_owner=True, objects=objs, followers=followers)
    resp = _run(ns["ap_posts_handler"](
        None, _post({"action": "delete", "id": "uuid-1"}), "alice", "proj"))
    assert resp["status"] == 200
    assert resp["data"] == {"ok": True, "deleted": "uuid-1", "federated": True}
    # Local object gone.
    assert any(e[0] == "d1_run" and "DELETE FROM ap_objects" in e[1] for e in log)
    assert "uuid-1" not in objs
    # One outbox row per unique inbox (shared inbox deduped → 2, not 3).
    inserts = _outbox_inserts(log)
    assert len(inserts) == 2
    inboxes = {e[2][0] for e in inserts}
    assert inboxes == {"https://mastodon.social/inbox",
                       "https://fosstodon.org/users/c/inbox"}
    # The queued body is a Delete(Tombstone) from the repo actor.
    payload = inserts[0][2][1]
    body = json.loads(payload["body"])
    assert body["type"] == "Delete"
    assert body["object"]["type"] == "Tombstone"
    assert body["object"]["id"] == "https://forkmesh.com/ap/o/uuid-1"
    assert payload["actorKind"] == "repo"
    assert any(e[0] == "drain" for e in log)


def test_delete_unknown_object_404s_and_writes_nothing():
    log = []
    ns = _env(log, authed_owner=True, objects={})
    resp = _run(ns["ap_posts_handler"](
        None, _post({"action": "delete", "id": "nope"}), "alice", "proj"))
    assert resp["status"] == 404
    assert not any(e[0] == "d1_run" for e in log)


def test_delete_drops_local_even_when_federation_disabled():
    objs = {"uuid-1": {"object_uuid": "uuid-1", "published": 1,
                       "data": {"note": {}}}}
    log = []
    ns = _env(log, authed_owner=True, enabled=False, objects=objs,
              followers=[{"inbox": "https://x/inbox", "shared_inbox": ""}])
    resp = _run(ns["ap_posts_handler"](
        None, _post({"action": "delete", "id": "uuid-1"}), "alice", "proj"))
    assert resp["status"] == 200
    assert resp["data"]["federated"] is False
    assert "uuid-1" not in objs
    assert _outbox_inserts(log) == []
