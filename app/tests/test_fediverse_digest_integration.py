"""Worker integration contracts for privacy-safe daily Fediverse digests."""

import ast
import asyncio
import hashlib
import importlib.util
import json
import re
import sys
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import parse_qs, urlparse


ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
sys.path.insert(0, str(ROOT / "src"))
import instance_features  # noqa: E402
ENTRY_TEXT = (
    # entry.py + its lazily-split domain modules
    (ENTRY.parent / "forkbot.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "fediverse_routes.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + ENTRY.read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "app_runtime.py").read_text(encoding="utf-8")
    + "\n\n\n"
)
SCHEMA_TEXT = (ROOT / "src" / "schema.py").read_text(encoding="utf-8")
URLS_TEXT = (ROOT / "src" / "urls.py").read_text(encoding="utf-8")
PUBLIC = ROOT / "public"

spec = importlib.util.spec_from_file_location(
    "fediverse_digest", ROOT / "src" / "fediverse_digest.py")
fedi_digest = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fedi_digest)

ap_spec = importlib.util.spec_from_file_location(
    "activitypub", ROOT / "src" / "activitypub.py")
ap = importlib.util.module_from_spec(ap_spec)
ap_spec.loader.exec_module(ap)


def _load(*names, extra_globals=None):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    nodes = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in names
    ]
    assert {node.name for node in nodes} == set(names)
    namespace = {"instance_features": instance_features}
    namespace.update(extra_globals or {})
    exec(compile(ast.fix_missing_locations(
        ast.Module(body=nodes, type_ignores=[])), str(ENTRY), "exec"),
         namespace)
    return namespace


def _run(coro):
    return asyncio.new_event_loop().run_until_complete(coro)


def test_schema_migration_and_routes_are_deployed():
    migration = (
        ROOT / "migrations" / "0046_fediverse_digests.sql"
    ).read_text(encoding="utf-8")
    for table in ("ap_digest_queues", "ap_org_digest_settings"):
        assert "CREATE TABLE IF NOT EXISTS " + table in SCHEMA_TEXT
        assert "CREATE TABLE IF NOT EXISTS " + table in migration
    assert "dedupe_bi TEXT" in SCHEMA_TEXT
    assert "idx_ap_outbox_dedupe" in migration
    assert "REPO_AP_DIGEST_RE" in URLS_TEXT
    assert "ORG_FEDIVERSE_RE" in URLS_TEXT
    assert "return await ap_digest_handler(" in ENTRY_TEXT
    assert "return await org_fediverse_handler(" in ENTRY_TEXT


def test_automatic_hook_queues_and_only_explicit_manual_post_is_immediate():
    body = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _ap_publish_repo_event"):
        ENTRY_TEXT.index("async def _ap_forget_remote")
    ]
    enqueue = body.index("await _ap_enqueue_repo_digest(")
    immediate_object = body.index("INSERT INTO ap_objects")
    assert "manual=False" in body
    assert "if not manual:" in body
    assert enqueue < immediate_object
    handler = ENTRY_TEXT[
        ENTRY_TEXT.index("async def ap_publish_handler"):
        ENTRY_TEXT.index("async def _authorize_repo_owner_web")
    ]
    assert 'manual = data.get("manual") is True' in handler
    assert 'manual=manual' in handler
    assert '"mode": "manual_immediate" if manual else "daily_digest"' in handler


def test_manual_user_post_does_not_require_repository_ownership():
    calls = []

    async def authorize_repo(_env, _request, _owner, _repo):
        return False

    async def authorize_author(_env, _request, author):
        calls.append(("author", author))
        return author == "jett"

    async def publish(*args, **kwargs):
        calls.append(("publish", args[9], kwargs))

    async def bounded_json(_request):
        return {
            "kind": "discussion",
            "eventType": "open",
            "title": "Hello fediverse",
            "body": "Posted from ForkMesh",
            "author": "jett",
            "manual": True,
        }

    async def ensure_schema(_env):
        return None

    async def repo_is_private(*_args):
        return False

    ns = _publish_handler(
        bounded_json, authorize_repo, authorize_author, publish)
    response = _run(ns["ap_publish_handler"](
        None, object(), "forkmesh", "forkmesh"))

    assert response["status"] == 202
    assert calls == [
        ("author", "jett"),
        ("publish", "jett", {
            "manual": True,
            "include_repo_actor": False,
            "public_owner": "forkmesh",
            "link_url": None,
        }),
    ]


def _publish_handler(bounded_json, authorize_repo, authorize_author, publish,
                     org_nodes=None):
    async def ensure_schema(_env):
        return None

    async def repo_is_private(*_args):
        return False

    async def org_repo_node(_env, alias, repo):
        return (org_nodes or {}).get((alias, repo), "")

    return _load("ap_publish_handler", "_ap_public_repo_owner", extra_globals={
        "method_name": lambda _request: "POST",
        "ensure_schema": ensure_schema,
        "_repo_is_private": repo_is_private,
        "bounded_json_request": bounded_json,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "AP_FEDI_KINDS": ("discussion", "release"),
        "AP_ANNOUNCEMENT_KIND": "announcement",
        "MAX_NODE_NAME": 63,
        "REPO_API_PREFIX_RE": re.compile(r"^/api/repo/([^/]+)/([^/]+)(?:/.*)?$"),
        "urlparse": urlparse,
        "safe_segment": lambda value: str(value or ""),
        "_org_repo_node": org_repo_node,
        "_ap_origin": lambda _env, _request=None: "https://forkmesh.com",
        "_authorize_repo_inbox_owner": authorize_repo,
        "_ap_publish_authorized": authorize_author,
        "_ap_publish_repo_event": publish,
        "json_response": lambda body, status=200: {"body": body, "status": status},
    })


def _announcement_request(payload, repo_authorized=True):
    calls = []

    async def authorize_repo(_env, _request, _owner, _repo):
        return repo_authorized

    async def authorize_author(_env, _request, _author):
        return False

    async def publish(*args, **kwargs):
        calls.append((args[2], args[4], args[7], args[8], kwargs))

    async def bounded_json(_request):
        return payload

    class Request:
        url = ("https://app.forkmesh.com/api/repo/forkmesh/forkmesh/ap-publish"
               "?owner=forkmesh&ts=1&sig=proof")

    ns = _publish_handler(
        bounded_json, authorize_repo, authorize_author, publish,
        org_nodes={("forkmesh", "forkmesh"): "jett-node"})
    response = _run(ns["ap_publish_handler"](
        None, Request(), "jett-node", "forkmesh"))
    return response, calls


def test_org_repo_announcement_publishes_as_the_org_actor_with_its_link():
    response, calls = _announcement_request({
        "kind": "announcement",
        "eventType": "publish",
        "title": "ForkMesh is on Open Collective",
        "body": "Details on the blog.",
        "url": "https://forkmesh.com/blog/open-collective/",
        "manual": True,
        "author": "jett",
    })

    assert response == {
        "body": {"ok": True, "mode": "manual_immediate"}, "status": 202}
    assert calls == [(
        "jett-node", "announcement", "ForkMesh is on Open Collective",
        "Details on the blog.", {
            "manual": True,
            "include_repo_actor": True,
            "public_owner": "forkmesh",
            "link_url": "https://forkmesh.com/blog/open-collective/",
        })]


def test_announcement_must_be_manual_and_link_to_this_site():
    base = {"kind": "announcement", "title": "Hi", "manual": True,
            "url": "https://forkmesh.com/blog/open-collective/"}
    response, calls = _announcement_request({**base, "manual": False})
    assert response["status"] == 400
    assert response["body"] == {"error": "invalid_kind"}
    for url in ("", "https://evil.example/blog/",
                "https://forkmesh.com.evil.example/x", "javascript:alert(1)"):
        response, calls = _announcement_request({**base, "url": url})
        assert response == {"body": {"error": "invalid_url"}, "status": 400}
        assert calls == []


def test_public_owner_ignores_an_alias_that_is_not_linked_to_the_node():
    response, calls = _announcement_request({
        "kind": "discussion", "title": "Hi", "manual": True,
    })
    assert calls[0][4]["public_owner"] == "forkmesh"

    ns = _publish_handler(None, None, None, None, org_nodes={
        ("forkmesh", "forkmesh"): "other-node"})

    class Request:
        url = "https://app.forkmesh.com/api/repo/forkmesh/forkmesh/ap-publish"

    assert _run(ns["_ap_public_repo_owner"](
        None, Request(), "jett-node", "forkmesh")) == "jett-node"


def test_org_repo_event_uses_the_public_owner_for_identity_but_node_for_data():
    body = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _ap_publish_repo_event"):
        ENTRY_TEXT.index("async def _ap_forget_remote")
    ]
    assert "public = (public_owner or owner).strip().lower()" in body
    assert "repo_web_href(public, repo)" in body
    assert "ap.repo_handle(public, repo.lower())" in body
    assert "ap.context_key(public, repo, kind, ref)" in body
    assert '"context": {"owner": public,' in body
    assert "_ap_repo_settings_get(env, owner, repo)" in body
    assert "privacy_reader(env, owner, repo)" in body
    assert "(announcement and not manual)" in body


def test_manual_post_author_must_match_signer_or_linked_node_owner():
    class Request:
        url = "https://forkmesh.test/api/repo/elsewhere/project/ap-publish" \
              "?owner=jett-node&ts=1000&sig=proof"

    async def authorized(_env, _request, signer):
        return "verified-key" if signer == "jett-node" else ""

    ownership_checks = []

    async def owns_node(_env, author, signer):
        ownership_checks.append((author, signer))
        return author == "jett" and signer == "jett-node"

    ns = _load("_ap_publish_authorized", extra_globals={
        "parse_qs": parse_qs,
        "urlparse": urlparse,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "MAX_NODE_NAME": 63,
        "valid_node_name": lambda value: bool(value),
        "_authorized_owner_signing_key": authorized,
        "_account_owns_node": owns_node,
    })

    assert _run(ns["_ap_publish_authorized"](
        None, Request(), "jett")) is True
    assert _run(ns["_ap_publish_authorized"](
        None, Request(), "mallory")) is False
    assert ownership_checks == [
        ("jett", "jett-node"),
        ("mallory", "jett-node"),
    ]


def test_scope_queue_is_encrypted_deduplicated_and_bounded():
    now = 10_000
    rows = {}
    sealed = {}
    sequence = [0]

    async def blind_index(env, value):
        return "bi:" + value

    async def d1_first(env, sql, *args):
        return rows.get(args[0])

    async def encrypt_row(env, value):
        sequence[0] += 1
        token = "sealed-%d" % sequence[0]
        sealed[token] = json.loads(json.dumps(value))
        return token

    async def decrypt_row(env, token):
        return json.loads(json.dumps(sealed.get(token))) if token else None

    async def d1_run(env, sql, *args):
        assert "ap_digest_queues" in sql
        rows[args[0]] = {
            "scope_bi": args[0],
            "next_ts": args[1],
            "last_published_at": args[2],
            "data": args[3],
        }

    class Date:
        @staticmethod
        def now():
            return now

    ns = _load(
        "_ap_digest_scope_bi", "_ap_digest_queue_record",
        "_ap_enqueue_digest_scope",
        extra_globals={
            "blind_index": blind_index,
            "d1_first": d1_first,
            "d1_run": d1_run,
            "encrypt_row": encrypt_row,
            "decrypt_row": decrypt_row,
            "clean_string": lambda value, limit: str(value or "")[:limit],
            "valid_node_name": lambda value: bool(value),
            "MAX_NODE_NAME": 63,
            "MAX_REPO_SEGMENT": 160,
            "Date": Date,
            "fedi_digest": fedi_digest,
        },
    )

    for index in range(fedi_digest.MAX_PENDING_EVENTS + 15):
        event = fedi_digest.normalize_event({
            "type": "issue",
            "title": "Issue %d" % index,
            "url": "https://forkmesh.test/alice/repo/issues/%d" % index,
            "timestamp": index + 1,
            "scope": "alice/repo",
        })
        assert _run(ns["_ap_enqueue_digest_scope"](
            None, "alice", "repo", "alice", event))

    row = rows["bi:ap-digest:alice/repo"]
    payload = sealed[row["data"]]
    assert len(payload["events"]) == fedi_digest.MAX_PENDING_EVENTS
    assert payload["events"][0]["title"] == "Issue 15"
    assert row["next_ts"] == now + fedi_digest.DAY_MS
    # The D1 row contains only an opaque encrypted token, not public titles.
    assert row["data"].startswith("sealed-")
    assert "Issue" not in row["data"]


def test_publish_consumes_events_in_same_batch_as_object_and_outbox():
    now = fedi_digest.DAY_MS * 3
    event = fedi_digest.normalize_event({
        "type": "release",
        "title": "Release 2.0",
        "url": "https://forkmesh.test/alice/repo/releases/v2",
        "timestamp": now - 1000,
        "scope": "alice/repo",
    })
    queue = {
        "scope": "alice/repo",
        "scopeOwner": "alice",
        "sourceOwner": "alice",
        "repo": "repo",
        "events": [event],
        "lastPublishedAt": 0,
    }
    sealed = {"queue": queue}
    batches = []
    deletes = []

    async def decrypt_row(env, token):
        return json.loads(json.dumps(sealed[token]))

    async def encrypt_row(env, value):
        token = "enc-%d" % len(sealed)
        sealed[token] = json.loads(json.dumps(value))
        return token

    async def d1_all(env, sql, *args):
        return [
            {"follower_id": "1", "inbox": "https://social.test/inbox", "shared_inbox": ""},
            {"follower_id": "2", "inbox": "https://social.test/inbox", "shared_inbox": ""},
            {"follower_id": "3", "inbox": "https://other.test/inbox", "shared_inbox": ""},
        ]

    async def d1_run(env, sql, *args):
        deletes.append((sql, args))

    async def batch(env, statements):
        batches.append(statements)

    async def blind_index(env, value):
        return hashlib.sha256(value.encode()).hexdigest()

    class Date:
        @staticmethod
        def now():
            return now

    ns = _load("_ap_publish_digest_row", extra_globals={
        "json": json,
        "hashlib": hashlib,
        "re": re,
        "Date": Date,
        "fedi_digest": fedi_digest,
        "decrypt_row": decrypt_row,
        "encrypt_row": encrypt_row,
        "_ap_digest_scope_allowed": lambda env, rec: _true(),
        "_ap_origin": lambda env: "https://forkmesh.test",
        "_ap_local_actor": lambda env, kind, handle: _value({
            "actorBi": "actor-bi", "privkey": "private"}),
        "d1_all": d1_all,
        "d1_run": d1_run,
        "_ap_actor_url": lambda origin, kind, handle: (
            origin + "/ap/repos/" + handle.replace(".", "/")),
        "repo_web_href": lambda owner, repo: "/" + owner + "/" + repo,
        "blind_index": blind_index,
        "_contribution_run_batch": batch,
        "ap": ap,
        "AP_ACTOR_REPO": "repo",
        "AP_DIGEST_FANOUT_BATCH": 10,
    })
    assert _run(ns["_ap_publish_digest_row"](None, {
        "scope_bi": "scope-bi",
        "last_published_at": 0,
        "data": "queue",
    }))
    assert deletes == []
    assert len(batches) == 1
    statements = batches[0]
    sql = "\n".join(statement for statement, _ in statements)
    assert "INSERT OR IGNORE INTO ap_objects" in sql
    assert sql.count("INSERT OR IGNORE INTO ap_outbox") == 2
    assert "UPDATE ap_digest_queues" in sql
    updated = sealed[statements[-1][1][2]]
    assert updated["events"] == []
    assert updated["lastPublishedAt"] == now
    object_uuid = statements[0][1][0]
    assert len(object_uuid) == 32
    assert all(args[-1] for statement, args in statements[1:-1])


def test_publish_resumes_follower_fanout_before_consuming_events():
    now = fedi_digest.DAY_MS * 4
    event = fedi_digest.normalize_event({
        "type": "release",
        "title": "Release 3.0",
        "url": "https://forkmesh.test/alice/repo/releases/v3",
        "timestamp": now - 1000,
        "scope": "alice/repo",
    })
    sealed = {"queue": {
        "scope": "alice/repo",
        "scopeOwner": "alice",
        "sourceOwner": "alice",
        "repo": "repo",
        "events": [event],
        "lastPublishedAt": 0,
    }}
    followers = [{
        "follower_id": f"{index:03d}",
        "inbox": f"https://social{index}.test/inbox",
        "shared_inbox": "",
    } for index in range(1, 14)]
    batches = []
    current = {"data": "queue", "last_published_at": 0}

    async def decrypt_row(env, token):
        return json.loads(json.dumps(sealed[token]))

    async def encrypt_row(env, value):
        token = "enc-%d" % len(sealed)
        sealed[token] = json.loads(json.dumps(value))
        return token

    async def d1_all(env, sql, actor_bi, cursor, limit):
        return [
            dict(row) for row in followers
            if row["follower_id"] > cursor
        ][:limit]

    async def batch(env, statements):
        batches.append(statements)
        update_args = statements[-1][1]
        current["last_published_at"] = update_args[1]
        current["data"] = update_args[2]

    async def blind_index(env, value):
        return hashlib.sha256(value.encode()).hexdigest()

    class Date:
        @staticmethod
        def now():
            return now

    ns = _load("_ap_publish_digest_row", extra_globals={
        "json": json,
        "hashlib": hashlib,
        "re": re,
        "Date": Date,
        "fedi_digest": fedi_digest,
        "decrypt_row": decrypt_row,
        "encrypt_row": encrypt_row,
        "_ap_digest_scope_allowed": lambda env, rec: _true(),
        "_ap_origin": lambda env: "https://forkmesh.test",
        "_ap_local_actor": lambda env, kind, handle: _value({
            "actorBi": "actor-bi", "privkey": "private"}),
        "d1_all": d1_all,
        "d1_run": lambda env, sql, *args: _value(None),
        "_ap_actor_url": lambda origin, kind, handle: (
            origin + "/ap/repos/" + handle.replace(".", "/")),
        "repo_web_href": lambda owner, repo: "/" + owner + "/" + repo,
        "blind_index": blind_index,
        "_contribution_run_batch": batch,
        "ap": ap,
        "AP_ACTOR_REPO": "repo",
        "AP_DIGEST_FANOUT_BATCH": 10,
    })

    row = {"scope_bi": "scope-bi", **current}
    assert _run(ns["_ap_publish_digest_row"](None, row))
    first = sealed[current["data"]]
    assert first["events"] == [event]
    assert first["fanout"]["cursor"] == "010"
    assert current["last_published_at"] == 0
    assert sum(
        "INSERT OR IGNORE INTO ap_outbox" in sql
        for sql, _args in batches[0]
    ) == 10

    row = {"scope_bi": "scope-bi", **current}
    assert _run(ns["_ap_publish_digest_row"](None, row))
    final = sealed[current["data"]]
    assert final["events"] == []
    assert "fanout" not in final
    assert current["last_published_at"] == now
    assert sum(
        "INSERT OR IGNORE INTO ap_outbox" in sql
        for sql, _args in batches[1]
    ) == 3


async def _true():
    return True


async def _value(value):
    return value


def test_private_or_revoked_scope_is_deleted_without_publication():
    deletes = []

    async def d1_run(env, sql, *args):
        deletes.append((sql, args))

    ns = _load("_ap_publish_digest_row", extra_globals={
        "decrypt_row": lambda env, value: _value({
            "scope": "alice/repo",
            "scopeOwner": "alice",
            "sourceOwner": "alice",
            "repo": "repo",
            "events": [],
        }),
        "_ap_digest_scope_allowed": lambda env, rec: _value(False),
        "d1_run": d1_run,
    })
    assert not _run(ns["_ap_publish_digest_row"](None, {
        "scope_bi": "secret-scope", "data": "encrypted"}))
    assert deletes == [(
        "DELETE FROM ap_digest_queues WHERE scope_bi=?",
        ("secret-scope",),
    )]


def test_scope_without_local_actor_is_deleted_so_it_cannot_block_the_queue():
    now = fedi_digest.DAY_MS * 5
    event = fedi_digest.normalize_event({
        "type": "release",
        "title": "Release 4.0",
        "url": "https://forkmesh.test/core-node/repo/releases/v4",
        "timestamp": now - 1000,
        "scope": "core-node/repo",
    })
    deletes = []
    batches = []

    async def d1_run(env, sql, *args):
        deletes.append((sql, args))

    async def batch(env, statements):
        batches.append(statements)

    class Date:
        @staticmethod
        def now():
            return now

    ns = _load("_ap_publish_digest_row", extra_globals={
        "json": json,
        "hashlib": hashlib,
        "re": re,
        "Date": Date,
        "fedi_digest": fedi_digest,
        "decrypt_row": lambda env, value: _value({
            "scope": "core-node/repo",
            "scopeOwner": "core-node",
            "sourceOwner": "core-node",
            "repo": "repo",
            "events": [event],
            "lastPublishedAt": 0,
        }),
        "_ap_digest_scope_allowed": lambda env, rec: _true(),
        "_ap_origin": lambda env: "https://forkmesh.test",
        "_ap_local_actor": lambda env, kind, handle: _value(None),
        "d1_run": d1_run,
        "_contribution_run_batch": batch,
        "ap": ap,
        "AP_ACTOR_REPO": "repo",
    })
    assert not _run(ns["_ap_publish_digest_row"](None, {
        "scope_bi": "unfollowed-scope",
        "last_published_at": 0,
        "data": "encrypted",
    }))
    assert deletes == [(
        "DELETE FROM ap_digest_queues WHERE scope_bi=?",
        ("unfollowed-scope",),
    )]
    assert batches == []


def test_owner_and_org_admin_previews_are_no_store_and_private_safe():
    repo_handler = ENTRY_TEXT[
        ENTRY_TEXT.index("async def ap_digest_handler"):
        ENTRY_TEXT.index("async def ap_posts_handler")
    ]
    assert "await _repo_is_private(env, owner, repo)" in repo_handler
    assert "await _authorize_repo_owner_web" in repo_handler
    assert 'cache_control="no-store"' in repo_handler

    org_handler = ENTRY_TEXT[
        ENTRY_TEXT.index("async def org_fediverse_handler"):
        ENTRY_TEXT.index("# --- Admins + email-delivery follow-up")
    ]
    assert 'not in ("owner", "admin")' in org_handler
    assert "await _repo_is_private(env, source_owner, repo)" in org_handler
    assert "ownerEnabled" in org_handler
    assert "Organization controls cannot override repository-owner settings." \
        in org_handler


def test_dashboard_exposes_repo_and_org_controls_with_exact_preview():
    assembled = (PUBLIC / "dashboard.js").read_text(encoding="utf-8")
    for marker in (
        "data-repo-digest-preview",
        "loadRepoDigestPreview",
        "/fediverse-digest",
        "data-org-fediverse-enabled",
        '"/fediverse"',
        "at most one clearly automated post per 24 hours",
        "No meaningful public updates are queued.",
    ):
        assert marker in assembled
    assert (Path(__file__).resolve().parents[2]
            / "www" / "docs" / "fediverse-digests.md").is_file()
