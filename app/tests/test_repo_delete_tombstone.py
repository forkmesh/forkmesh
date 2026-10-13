"""Deleting a repository from the website has to stay deleted (adhoc #91).

The owner's node keeps its local mirror and republishes the catalog record on
every heartbeat, so DELETE /api/repositories used to drop the row only for as
long as it took the node to publish again — from the website it looked like the
delete button did nothing. The delete now writes a tombstone that refuses those
automatic republishes; only an explicitly user-initiated publish from the
desktop ("publishIntent":"user", still owner-signature-gated) lifts it.
"""

import ast
import asyncio
import hashlib
import json
import re
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import parse_qs, urlparse

import pytest
from _maintenance_stubs import with_maintenance_off

# MainWindowRepos.cpp was split into feature files; read them in the original
# source order so section slices that span functions still line up.
MAIN_WINDOW_REPOS_SOURCES = (
    "MainWindowRepos.cpp",
    "MainWindowReposInternal.h",
    "MainWindowRepoSettings.cpp",
    "MainWindowRepoSync.cpp",
    "MainWindowMirrorPush.cpp",
    "MainWindowRepoFetch.cpp",
    "MainWindowAccounts.cpp",
)

ENTRY = Path(__file__).resolve().parents[1] / "src" / "entry.py"
CATALOG_ROUTES = ENTRY.parent / "catalog_routes.py"

NOW = 1_700_000_000_000
WANTED = (
    "catalog_handler",
    "catalog_mirror_update_needed",
    "catalog_push_observed",
    "_repo_delete_tombstone_active",
    "_record_repo_delete_tombstone",
    "_clear_repo_delete_tombstone",
    "_repo_publication_authority",
)


def _run(coro):
    return asyncio.get_event_loop_policy().new_event_loop().run_until_complete(coro)


class _Relay:
    """Just enough of the Worker's D1 + crypto surface for catalog_handler."""

    def __init__(self, *, session_owner="alice", repo_exists=True,
                 admin_account=""):
        self.session_owner = session_owner
        self.admin_account = admin_account
        self.tombstones = {}       # repo_bi -> expires_at
        self.repos = {"bi:alice/demo": {"owner": "alice", "name": "demo"}} \
            if repo_exists else {}
        self.deleted_scoped = []
        self.deleted_named = []
        self.admin_canonicals = []

    # --- D1 -------------------------------------------------------------
    async def d1_run(self, env, sql, *args):
        if "INSERT INTO repo_deletions" in sql:
            self.tombstones[args[0]] = args[3]
        elif "DELETE FROM repo_deletions WHERE repo_bi=?" in sql:
            self.tombstones.pop(args[0], None)
        elif "DELETE FROM repo_deletions WHERE expires_at<=?" in sql:
            for key, expires in list(self.tombstones.items()):
                if expires <= args[0]:
                    self.tombstones.pop(key)
        elif "DELETE FROM repositories WHERE key_bi=?" in sql:
            self.repos.pop(args[0], None)
        return None

    async def d1_first(self, env, sql, *args):
        if "FROM repo_deletions" in sql:
            expires = self.tombstones.get(args[0])
            return {"repo_bi": args[0]} if expires and expires > args[1] else None
        if "COUNT(*)" in sql:
            return {"c": len(self.repos)}
        if "FROM repositories WHERE key_bi=?" in sql:
            record = self.repos.get(args[0])
            return {"data": record} if record else None
        return None

    async def d1_all(self, env, sql, *args):
        return []

    # --- handler namespace ----------------------------------------------
    def namespace(self, body=None):
        relay = self

        async def noop(*a, **k):
            return None

        async def authed_account_name(env, request):
            return relay.session_owner

        async def account_owns_node(env, actor, node):
            return bool(actor)

        async def blind_index(env, value):
            return "bi:" + str(value)

        async def decrypt_row(env, data):
            return data

        async def encrypt_row(env, data):
            return data

        async def account_row(env, name):
            return "bi:" + name, {"pubkey": "pk"}

        # Publication authority accepts the primary key or any enabled
        # owner_sign device key; this harness account has just its primary.
        async def owner_signing_pubkeys(env, owner):
            return ["pk"]

        async def publication_key(env, owner, maintainer):
            return "pk"

        async def ed25519_verify(pub, sig, canonical):
            return True

        async def bounded_json_request(request):
            return body or {}

        async def contribution_ingest(*a, **k):
            return {"accepted": False, "warning": ""}

        async def delete_repo_scoped_state(env, repo_bi):
            relay.deleted_scoped.append(repo_bi)

        async def delete_repo_named_state(env, owner, repo):
            relay.deleted_named.append(owner + "/" + repo)

        async def admin_authorized(env, user, ts, sig, canonical):
            relay.admin_canonicals.append(canonical.decode())
            return {"name": user} if user == relay.admin_account else None

        def safe_catalog_record(data):
            return {
                "owner": data.get("owner", ""),
                "name": data.get("name", ""),
                "maintainer": "pk",
                "visibility": "public",
                "updatedAt": str(NOW),
            }

        def json_response(payload, status=200, **kwargs):
            return SimpleNamespace(status=status, payload=payload)

        return {
            "ast": ast, "json": json, "re": re, "hashlib": hashlib,
            "parse_qs": parse_qs, "urlparse": urlparse,
            "Date": type("D", (), {"now": staticmethod(lambda: NOW)}),
            "LOGIN_MAX_SKEW_MS": 60_000,
            "CATALOG_MAX_RECORDS_PER_OWNER": 100,
            "MAX_CATALOG_REPOS": 1000,
            "MAX_NODE_NAME": 63,
            "CATALOG_TTL": 10,
            "STATE_PIN_HISTORY": 100,
            "ensure_schema": noop,
            "method_name": lambda request: request.method,
            "safe_segment": lambda value: str(value or "").strip().lower(),
            "clean_string": lambda value, limit=0: str(value or "")[:limit or None],
            "_authed_account_name": authed_account_name,
            "_account_owns_node": account_owns_node,
            "_owner_signing_pubkeys": owner_signing_pubkeys,
            "_audit_sensitive_action": noop,
            "blind_index": blind_index,
            "d1_run": relay.d1_run,
            "d1_first": relay.d1_first,
            "d1_all": relay.d1_all,
            "decrypt_row": decrypt_row,
            "encrypt_row": encrypt_row,
            "_delete_repo_scoped_state": delete_repo_scoped_state,
            "_delete_repo_named_state": delete_repo_named_state,
            "_admin_authorized": admin_authorized,
            "_delete_bounties_namespace": noop,
            "invalidate_public_catalog_snapshots": noop,
            "purge_catalog_related_caches": noop,
            "schedule_public_catalog_republish": noop,
            # This harness treats every accepted publish as a change so the
            # legacy expectations keep exercising the full write path.
            "catalog_meaningful_change": lambda prior, record: True,
            "_catalog_ms": lambda value: 0,
            "CATALOG_UNCHANGED_LEASE_MS": 8 * 60 * 1000,
            "_bump_catalog_rev": noop,
            "notify_repo_host": noop,
            "notify_repo_mirrors": noop,
            "_owner_pubkey": noop,
            "_verify_owner_signature": noop,
            "bounded_json_request": bounded_json_request,
            "safe_catalog_record": safe_catalog_record,
            "_is_blocked_catalog_identity": lambda env, *values: False,
            "_account_row": account_row,
            "_catalog_publication_key": publication_key,
            "ed25519_verify": ed25519_verify,
            "touch_registered_node": noop,
            "catalog_rate_check": noop,
            "_contribution_write_catalog_state": noop,
            "_index_repo_fork_edge": noop,
            "_contribution_ingest_snapshot": contribution_ingest,
            "_https_mirror_refresh_catalog_publisher_health": noop,
            "_promote_hosted_repository_import": noop,
            "_world_broadcast_mirror_push": noop,
            "json_response": json_response,
        }


def _handlers(relay, body=None):
    source = (
        CATALOG_ROUTES.read_text(encoding="utf-8")
        + "\n\n\n"
        + (
    # entry.py + its lazily-split domain modules
    (ENTRY.parent / "node_events_runtime.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + ENTRY.read_text(encoding="utf-8")
    + "\n\n\n"
)
    )
    tree = ast.parse(source, filename=str(ENTRY))
    nodes = [
        n for n in tree.body
        if (isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef))
            and n.name in WANTED)
        or (isinstance(n, ast.Assign)
            and any(isinstance(t, ast.Name)
                    and t.id == "REPO_DELETE_TOMBSTONE_TTL_MS"
                    for t in n.targets))
    ]
    assert {getattr(n, "name", "") for n in nodes} >= set(WANTED)
    module = ast.fix_missing_locations(ast.Module(body=nodes, type_ignores=[]))
    namespace = relay.namespace(body)
    exec(compile(module, str(ENTRY), "exec"), with_maintenance_off(namespace))
    return namespace


def _delete_request():
    return SimpleNamespace(
        method="DELETE",
        url="https://forkmesh.com/api/repositories?owner=alice&name=demo",
        headers={},
    )


def _publish_request():
    return SimpleNamespace(
        method="POST", url="https://forkmesh.com/api/repositories", headers={})


def _publish_body(intent=None):
    body = {"owner": "alice", "name": "demo", "maintainer": "pk",
            "catalogSigVersion": 2, "catalogSig": "sig"}
    if intent is not None:
        body["publishIntent"] = intent
    return body


def test_website_delete_records_a_tombstone():
    relay = _Relay()
    handler = _handlers(relay)["catalog_handler"]
    response = _run(handler(None, _delete_request()))
    assert response.payload == {"ok": True, "deleted": True}
    assert relay.repos == {}
    assert "bi:alice/demo" in relay.tombstones


def test_tombstone_is_recorded_even_when_the_row_is_already_gone():
    # The owner node may be mid-republish when the delete lands; a missing row
    # is exactly the race that made the delete look like a no-op.
    relay = _Relay(repo_exists=False)
    handler = _handlers(relay)["catalog_handler"]
    response = _run(handler(None, _delete_request()))
    assert response.payload == {"ok": True, "deleted": False}
    assert "bi:alice/demo" in relay.tombstones


def test_owner_signed_desktop_delete_leaves_no_tombstone():
    # The desktop signs its own deletes and also uses them to move a repo to a
    # renamed owner. It is the publisher, so it needs no tombstone.
    relay = _Relay(session_owner="")
    namespace = _handlers(relay)

    async def owner_pubkey(env, owner):
        return "pk"

    async def verify_owner_signature(env, owner, sig, canonical):
        return True

    namespace["_owner_pubkey"] = owner_pubkey
    namespace["_verify_owner_signature"] = verify_owner_signature
    request = _delete_request()
    request.url += "&ts=%d&sig=deadbeef" % NOW
    response = _run(namespace["catalog_handler"](None, request))
    assert response.payload == {"ok": True, "deleted": True}
    assert relay.tombstones == {}


def test_heartbeat_republish_is_refused_after_a_website_delete():
    relay = _Relay()
    _run(_handlers(relay)["catalog_handler"](None, _delete_request()))
    handler = _handlers(relay, _publish_body("auto"))["catalog_handler"]
    response = _run(handler(None, _publish_request()))
    assert response.status == 410
    assert response.payload == {"error": "repository_deleted"}
    assert relay.repos == {}
    # Still tombstoned, so the next heartbeat is refused too.
    assert "bi:alice/demo" in relay.tombstones


def test_publish_without_an_intent_field_is_treated_as_a_heartbeat():
    # Older desktop builds send no publishIntent at all; their republish must
    # not resurrect the repository either.
    relay = _Relay()
    _run(_handlers(relay)["catalog_handler"](None, _delete_request()))
    handler = _handlers(relay, _publish_body())["catalog_handler"]
    response = _run(handler(None, _publish_request()))
    assert response.status == 410


def test_user_initiated_publish_lifts_the_tombstone():
    relay = _Relay()
    _run(_handlers(relay)["catalog_handler"](None, _delete_request()))
    handler = _handlers(relay, _publish_body("user"))["catalog_handler"]
    response = _run(handler(None, _publish_request()))
    assert response.status == 201
    assert relay.tombstones == {}


def test_expired_tombstone_stops_blocking_publishes():
    relay = _Relay()
    _run(_handlers(relay)["catalog_handler"](None, _delete_request()))
    relay.tombstones["bi:alice/demo"] = NOW - 1
    handler = _handlers(relay, _publish_body("auto"))["catalog_handler"]
    response = _run(handler(None, _publish_request()))
    assert response.status == 201


def test_tombstone_ttl_is_bounded_and_generous():
    source = ENTRY.read_text(encoding="utf-8")
    match = re.search(
        r"REPO_DELETE_TOMBSTONE_TTL_MS\s*=\s*([0-9 */+]+)", source)
    assert match, "the tombstone TTL constant should stay declarative"
    ttl = eval(match.group(1))  # noqa: S307 - a literal arithmetic expression
    day = 24 * 60 * 60 * 1000
    assert 30 * day <= ttl <= 365 * day


@pytest.mark.parametrize("statement", [
    "CREATE TABLE IF NOT EXISTS repo_deletions",
    "idx_repo_deletions_expires",
])
def test_schema_declares_the_tombstone_table(statement):
    schema = (Path(__file__).resolve().parents[1] / "src" / "schema.py") \
        .read_text(encoding="utf-8")
    assert statement in schema


REPOS_CPP = "".join(
    (Path(__file__).resolve().parents[2] / "desktop" / "src" / name).read_text(encoding="utf-8")
    for name in MAIN_WINDOW_REPOS_SOURCES
)


def test_desktop_does_not_publish_catalog_records():
    assert "void MainWindow::publishRepositoryNow(" not in REPOS_CPP
    assert 'metadata.insert(QStringLiteral("publishIntent")' not in REPOS_CPP
    assert "m_catalogPublishServeState" not in REPOS_CPP


def test_desktop_cannot_resurrect_a_worker_deleted_repository():
    assert 'QLatin1String("repository_deleted")' not in REPOS_CPP
    assert "scheduleCatalogPublish(" not in REPOS_CPP
