#!/usr/bin/env python3
"""Session-token authorization for per-account endpoints (security fix).

`notifications_handler`, `poll_handler` and `repo_about_handler` used to trust a
self-asserted `node` / `ownerAccount` request field, so anyone who knew a name
could read another account's decrypted notifications, poll its profile/unread
digest, or overwrite a repo's description (and read back the full private-repo
record). They now require a signed account **session token** — a POST-body
`sessionToken` or an `Authorization: Bearer` header — resolving to that account.

Same AST-extraction harness as test_repo_agents.py: pull the functions under
test out of entry.py and stub the D1/crypto/JS primitives they call.
"""

import ast
import asyncio
import hmac
import re
from pathlib import Path
from urllib.parse import parse_qs, unquote, urlparse

from worker_test_helpers import json_from_request_double


ENTRY = Path(__file__).resolve().parents[1] / "src" / "entry.py"
CATALOG = ENTRY.parent / "catalog.py"
SCHEMA = ENTRY.parent / "schema.py"
ENTRY_TEXT = (
    (
    # entry.py + its lazily-split domain modules
    (ENTRY.parent / "forkbot.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "fediverse_routes.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + ENTRY.read_text(encoding="utf-8")
    + "\n\n\n"
) + "\n" + CATALOG.read_text(encoding="utf-8")
    + "\n" + SCHEMA.read_text(encoding="utf-8"))

FUNCS = {
    "notifications_handler", "poll_handler", "repo_about_handler",
    "_alert_inbox_account_name", "ping_pause_until", "_apply_ping_pause",
    "valid_node_name", "clean_string",
    "_owner_pubkey", "_catalog_record_matches_identity",
    "_repo_identity_from_clone_url", "safe_segment", "method_name",
    "_account_owns_node", "_owned_nodes", "_repo_admin_allowed",
}


def _load_functions(extra_globals):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    selected = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in FUNCS
    ]
    found = {node.name for node in selected}
    assert found == FUNCS, "missing functions: %s" % sorted(FUNCS - found)
    module = ast.fix_missing_locations(ast.Module(body=selected, type_ignores=[]))
    namespace = dict(extra_globals)
    namespace.setdefault("bounded_json_request", json_from_request_double)
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace


class _Headers:
    def __init__(self, mapping=None):
        self._m = {str(k).lower(): v for k, v in (mapping or {}).items()}

    def get(self, name, default=None):
        return self._m.get(str(name).lower(), default)


class _Request:
    def __init__(self, method="GET", url="https://forkmesh.test/x", body=None,
                 headers=None):
        self.method = method
        self.url = url
        self._body = body
        self.headers = _Headers(headers)

    async def json(self):
        if self._body is None:
            raise ValueError("no body")
        return self._body


async def _swallow_side_effect(awaitable):
    try:
        await awaitable
    except Exception:
        pass


async def _ap_broadcast_stub(_env, _request, _kind, _handle):
    return None


async def _notify_repo_host_stub(_env, _owner, _repo, _topic):
    return None


async def _notify_account_event_stub(_env, _owner, _topic, _push_alert=None):
    return None


class _ApStub:
    @staticmethod
    def repo_handle(owner, repo):
        return "%s.%s" % (owner, repo)


def _harness(accounts, notifications=None, repositories=None,
             org_admins=None):
    org_admins = set(org_admins or ())
    notifications = list(notifications or [])   # {recipient_bi, dedupe_bi, ts, read_at, data}
    repositories = list(repositories or [])     # {key_bi, data(dict), is_private}
    now = [1_000_000_000]

    class _DateStub:
        @staticmethod
        def now():
            return now[0]

    def json_response(data, status=200, **_kwargs):
        return {"status": status, "data": data}

    async def ensure_schema(_env):
        return None

    async def blind_index(_env, value):
        return "bi:" + str(value or "").strip().lower()

    async def _account_row(_env, name):
        key = str(name or "").strip().lower()
        rec = accounts.get(key)
        if rec is None:
            return "bi:" + key, None
        rec = dict(rec)
        rec.setdefault("name", key)
        return "bi:" + key, rec

    async def _is_admin(_env, name):
        return bool(accounts.get(str(name or "").strip().lower(), {}).get("is_admin"))

    def _account_session_token(_env, name):
        return "test-session:" + str(name or "").strip().lower()

    async def _account_session_record(_env, request, data=None):
        payload = data if isinstance(data, dict) else {}
        token = str(payload.get("sessionToken") or "")
        if not token:
            auth = request.headers.get("authorization") or ""
            token = auth[7:] if auth.lower().startswith("bearer ") else ""
        prefix = "test-session:"
        name = token[len(prefix):] if token.startswith(prefix) else ""
        rec = accounts.get(name)
        if not rec or rec.get("status") != "active":
            return "", None
        record = dict(rec)
        record.setdefault("name", name)
        return "bi:" + name, record

    async def _authed_account_name(_env, request, data=None):
        _, rec = await _account_session_record(_env, request, data)
        return str((rec or {}).get("name") or "").strip().lower()

    async def _account_alert_signed_session(_env, _request, _resource=""):
        # None of these requests carry the desktop's account-key signature, so
        # the alert inbox's signed fallback resolves to nobody and the session
        # gate below is what decides. The proof itself is pinned separately, in
        # test_account_alert_signed_inbox.py.
        return ""

    async def decrypt_row(_env, stored, key=None):
        return dict(stored) if isinstance(stored, dict) else None

    async def encrypt_row(_env, obj):
        return dict(obj)

    async def _data_key(_env):
        return None

    async def purge_catalog_related_caches(_env=None):
        return None

    async def schedule_public_catalog_republish(_env=None, *args, **kwargs):
        return False

    def _build_rev(_env):
        return "dev"

    async def d1_all(_env, sql, *args):
        if "FROM notifications" in sql:
            recipient_bi = args[0]
            rows = [r for r in notifications if r["recipient_bi"] == recipient_bi]
            rows.sort(key=lambda r: r["ts"], reverse=True)
            limit = args[1] if len(args) > 1 else len(rows)
            offset = args[2] if len(args) > 2 else 0
            return [dict(r) for r in rows[offset:offset + limit]]
        if "FROM repositories" in sql:
            return [dict(r) for r in repositories]
        if "FROM repo_media" in sql:
            return []
        raise AssertionError("unexpected d1_all: " + sql)

    async def d1_first(_env, sql, *args):
        if "FROM notifications" in sql:
            recipient_bi = args[0]
            rows = [r for r in notifications if r["recipient_bi"] == recipient_bi]
            total = len(rows)
            unread = sum(1 for r in rows if not r.get("read_at"))
            latest = max((r["ts"] for r in rows), default=0)
            # The inbox GET asks for unread and total as separate counts.
            count = unread if "read_at=0" in sql else total
            return {"total": total, "unread": unread, "latest": latest,
                    "n": count}
        raise AssertionError("unexpected d1_first: " + sql)

    async def d1_run(_env, sql, *args):
        if sql.startswith("UPDATE notifications SET read_at=? WHERE recipient_bi=?") \
                and "dedupe_bi" not in sql:
            read_at, recipient_bi = args
            for r in notifications:
                if r["recipient_bi"] == recipient_bi:
                    r["read_at"] = read_at
            return
        if "UPDATE notifications SET read_at=? WHERE recipient_bi=? AND dedupe_bi=?" in sql:
            read_at, recipient_bi, dedupe_bi = args
            for r in notifications:
                if r["recipient_bi"] == recipient_bi and r["dedupe_bi"] == dedupe_bi:
                    r["read_at"] = read_at
            return
        if sql == "DELETE FROM notifications WHERE recipient_bi=?":
            (recipient_bi,) = args
            notifications[:] = [
                row for row in notifications
                if row["recipient_bi"] != recipient_bi]
            return
        if sql == (
                "DELETE FROM notifications "
                "WHERE recipient_bi=? AND dedupe_bi=?"):
            recipient_bi, dedupe_bi = args
            notifications[:] = [
                row for row in notifications
                if not (row["recipient_bi"] == recipient_bi and
                        row["dedupe_bi"] == dedupe_bi)]
            return
        if sql.startswith("UPDATE repositories SET data=?, is_private=?"):
            data, is_private, key_bi = args
            for r in repositories:
                if r["key_bi"] == key_bi:
                    r["data"] = data
                    r["is_private"] = is_private
            return
        if "repo_media" in sql or "about_inbox" in sql:
            return
        raise AssertionError("unexpected d1_run: " + sql)

    async def _org_admin_of_linked_repo(_env, node, repo, actor):
        return (str(node).lower(), str(repo).lower(),
                str(actor).lower()) in org_admins

    ns = _load_functions({
        "Date": _DateStub,
        "_org_admin_of_linked_repo": _org_admin_of_linked_repo,
        "json_response": json_response,
        "ensure_schema": ensure_schema,
        "blind_index": blind_index,
        "_account_row": _account_row,
        "_account_session_token": _account_session_token,
        "_account_session_record": _account_session_record,
        "_authed_account_name": _authed_account_name,
        "_account_alert_signed_session": _account_alert_signed_session,
        "_is_admin": _is_admin,
        "decrypt_row": decrypt_row,
        "encrypt_row": encrypt_row,
        "_data_key": _data_key,
        "purge_catalog_related_caches": purge_catalog_related_caches,
        "schedule_public_catalog_republish": schedule_public_catalog_republish,
        "_build_rev": _build_rev,
        "d1_all": d1_all,
        "d1_first": d1_first,
        "d1_run": d1_run,
        "parse_qs": parse_qs,
        "urlparse": urlparse,
        "unquote": unquote,
        "re": re,
        "hmac": hmac,
        "MAX_NODE_NAME": 63,
        "NODE_NAME_RE": re.compile(r"^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$"),
        "MAX_NOTIFICATIONS_FETCH": 200,
        "MAX_NOTIFICATIONS_PER_RECIPIENT": 500,
        "ACCOUNT_SESSION_TTL_MS": 1826 * 24 * 60 * 60 * 1000,
        "ADMIN_SESSION_TTL_MS": 12 * 60 * 60 * 1000,
        "MAX_REPO_SEGMENT": 80,
        # Repo branding (fediverse actor images) rides through repo_about;
        # the auth tests never upload one, so the validator is a pass-through
        # and the follower Update broadcast is a no-op.
        "MAX_REPO_LOGO_BYTES": 256 * 1024,
        "MAX_REPO_BANNER_BYTES": 1024 * 1024,
        "clean_media_png": lambda value, max_bytes: ("", ""),
        "AP_ACTOR_REPO": "repo",
        "_best_effort_inbox_side_effect": _swallow_side_effect,
        "_ap_broadcast_actor_update": _ap_broadcast_stub,
        "notify_repo_host": _notify_repo_host_stub,
        "notify_account_event": _notify_account_event_stub,
        "ap": _ApStub,
    })
    ns["_notifications"] = notifications
    ns["_repositories"] = repositories
    return ns


def _user(is_admin=False):
    return {"pubkey": "PK", "status": "active", "pass_hash": "h",
            "pass_salt": "s", "is_admin": is_admin}


def _bearer(ns, env, name):
    return {"authorization": "Bearer " + ns["_account_session_token"](env, name)}


# --- notifications GET -------------------------------------------------------

def test_notifications_get_requires_matching_session():
    accounts = {"alice": _user(), "mallory": _user()}
    notes = [{"recipient_bi": "bi:alice", "dedupe_bi": "n1", "ts": 5,
              "read_at": 0, "data": {"title": "secret alert", "body": "private"}}]
    ns = _harness(accounts, notifications=notes)
    env = object()
    url = "https://forkmesh.test/api/notifications?node=alice"

    # No session token -> 401, nothing leaked.
    anon = asyncio.run(ns["notifications_handler"](env, _Request("GET", url)))
    assert anon["status"] == 401
    assert "secret alert" not in str(anon["data"])

    # Someone else's valid session -> 401, nothing leaked.
    other = asyncio.run(ns["notifications_handler"](
        env, _Request("GET", url, headers=_bearer(ns, env, "mallory"))))
    assert other["status"] == 401
    assert "secret alert" not in str(other["data"])

    # The owner's own session -> the inbox.
    owner = asyncio.run(ns["notifications_handler"](
        env, _Request("GET", url, headers=_bearer(ns, env, "alice"))))
    assert owner["status"] == 200
    assert owner["data"]["unread"] == 1
    assert owner["data"]["notifications"][0]["title"] == "secret alert"


def test_notifications_get_pages_rows_but_counts_the_full_inbox():
    accounts = {"alice": _user()}
    notes = [
        {"recipient_bi": "bi:alice", "dedupe_bi": "n%03d" % n,
         "ts": n, "read_at": 0 if n % 2 else n,
         "data": {"title": "alert %d" % n}}
        for n in range(1, 46)
    ]
    ns = _harness(accounts, notifications=notes)
    env = object()
    result = asyncio.run(ns["notifications_handler"](
        env,
        _Request(
            "GET",
            "https://forkmesh.test/api/notifications"
            "?node=alice&limit=40&offset=40",
            headers=_bearer(ns, env, "alice"),
        ),
    ))

    assert result["status"] == 200
    assert result["data"]["total"] == 45
    assert result["data"]["unread"] == 23
    assert result["data"]["limit"] == 40
    assert result["data"]["offset"] == 40
    assert [item["title"] for item in result["data"]["notifications"]] == [
        "alert 5", "alert 4", "alert 3", "alert 2", "alert 1",
    ]


# --- notifications POST (mark read) ------------------------------------------

def test_notifications_post_mark_read_requires_matching_session():
    accounts = {"alice": _user(), "mallory": _user()}
    notes = [{"recipient_bi": "bi:alice", "dedupe_bi": "n1", "ts": 5,
              "read_at": 0, "data": {"title": "x"}}]
    ns = _harness(accounts, notifications=notes)
    env = object()
    url = "https://forkmesh.test/api/notifications"

    # Attacker (no/other session) cannot mark the victim's notifications read.
    griefed = asyncio.run(ns["notifications_handler"](
        env, _Request("POST", url, body={"node": "alice", "all": True},
                      headers=_bearer(ns, env, "mallory"))))
    assert griefed["status"] == 401
    assert notes[0]["read_at"] == 0

    # The owner can, via a sessionToken in the body.
    ok = asyncio.run(ns["notifications_handler"](
        env, _Request("POST", url, body={
            "node": "alice", "all": True,
            "sessionToken": ns["_account_session_token"](env, "alice"),
        })))
    assert ok["status"] == 200
    assert notes[0]["read_at"] == 1_000_000_000


def test_notifications_delete_all_requires_matching_session():
    accounts = {"alice": _user(), "mallory": _user()}
    notes = [
        {"recipient_bi": "bi:alice", "dedupe_bi": "a" * 64, "ts": 5,
         "read_at": 0, "data": {"kind": "operational_alert"}},
        {"recipient_bi": "bi:alice", "dedupe_bi": "b" * 64, "ts": 4,
         "read_at": 0, "data": {"kind": "mention"}},
        {"recipient_bi": "bi:mallory", "dedupe_bi": "c" * 64, "ts": 3,
         "read_at": 0, "data": {"kind": "operational_alert"}},
    ]
    ns = _harness(accounts, notifications=notes)
    env = object()
    url = "https://forkmesh.test/api/notifications"

    denied = asyncio.run(ns["notifications_handler"](
        env, _Request("DELETE", url, body={"node": "alice", "all": True},
                      headers=_bearer(ns, env, "mallory"))))
    assert denied["status"] == 401
    assert len(notes) == 3

    cleared = asyncio.run(ns["notifications_handler"](
        env, _Request("DELETE", url, body={
            "node": "alice", "all": True,
            "sessionToken": ns["_account_session_token"](env, "alice"),
        })))
    assert cleared["status"] == 200
    assert [row["recipient_bi"] for row in ns["_notifications"]] == [
        "bi:mallory"]


# --- poll digest -------------------------------------------------------------

def test_poll_digest_omits_account_data_without_matching_session():
    accounts = {"alice": _user(), "mallory": _user()}
    notes = [{"recipient_bi": "bi:alice", "dedupe_bi": "n1", "ts": 7,
              "read_at": 0, "data": {"title": "x"}}]
    ns = _harness(accounts, notifications=notes)
    env = object()
    url = "https://forkmesh.test/api/poll?node=alice"

    anon = asyncio.run(ns["poll_handler"](env, _Request("GET", url)))
    assert anon["status"] == 200
    assert "profile" not in anon["data"] and "notif" not in anon["data"]

    other = asyncio.run(ns["poll_handler"](
        env, _Request("GET", url, headers=_bearer(ns, env, "mallory"))))
    assert "profile" not in other["data"] and "notif" not in other["data"]

    mine = asyncio.run(ns["poll_handler"](
        env, _Request("GET", url, headers=_bearer(ns, env, "alice"))))
    assert mine["data"]["notif"]["unread"] == 1
    assert "profile" in mine["data"]


# --- repo about --------------------------------------------------------------

def _repo_row(owner, name, private=False):
    return {"key_bi": "bi:%s/%s" % (owner, name),
            "data": {"owner": owner, "name": name,
                     "cloneUrl": "https://forkmesh.test/%s/%s.git" % (owner, name),
                     "visibility": "private" if private else "public",
                     "stateHash": "SECRET-HASH"},
            "is_private": 1 if private else 0}


def test_repo_about_requires_owner_session_and_hides_record():
    accounts = {"alice": _user(), "mallory": _user()}
    repos = [_repo_row("alice", "proj", private=True)]
    ns = _harness(accounts, repositories=repos)
    env = object()
    url = "https://forkmesh.test/api/repo/alice/proj/about"

    # Self-asserted ownerAccount, no session -> rejected, description unchanged.
    spoof = asyncio.run(ns["repo_about_handler"](
        env, _Request("POST", url, body={
            "ownerAccount": "alice", "description": "defaced"}),
        "alice", "proj"))
    assert spoof["status"] == 403
    assert repos[0]["data"].get("description") != "defaced"

    # A non-owner session cannot edit either.
    other = asyncio.run(ns["repo_about_handler"](
        env, _Request("POST", url, body={
            "description": "defaced",
            "sessionToken": ns["_account_session_token"](env, "mallory")},
            headers=_bearer(ns, env, "mallory")),
        "alice", "proj"))
    assert other["status"] == 403

    # The owner can, and the response never echoes the full private record.
    ok = asyncio.run(ns["repo_about_handler"](
        env, _Request("POST", url, body={
            "description": "a real description",
            "sessionToken": ns["_account_session_token"](env, "alice")}),
        "alice", "proj"))
    assert ok["status"] == 200
    assert ok["data"]["description"] == "a real description"
    assert ok["data"]["isPrivate"] is True
    assert "repository" not in ok["data"]
    assert "SECRET-HASH" not in str(ok["data"])
    assert repos[0]["data"]["description"] == "a real description"


def test_repo_about_allows_user_who_owns_the_node():
    # A repo's owner is a NODE account ("laptop"); the human logs in with a
    # separate USER account ("alice") that owns that node (adhoc #53). The
    # owning user must be able to edit About even though their name != owner.
    accounts = {
        # Link recorded on BOTH sides, as _link_node_to_user writes it.
        "alice": _user(),
        "laptop": {"pubkey": "PK", "status": "active", "owner": "alice"},
        "stranger": _user(),
    }
    accounts["alice"]["nodes"] = ["laptop"]
    repos = [_repo_row("laptop", "proj")]
    ns = _harness(accounts, repositories=repos)
    env = object()
    url = "https://forkmesh.test/api/repo/laptop/proj/about"

    # The owning user's session is authorized.
    ok = asyncio.run(ns["repo_about_handler"](
        env, _Request("POST", url, body={
            "description": "owned via node link",
            "sessionToken": ns["_account_session_token"](env, "alice")}),
        "laptop", "proj"))
    assert ok["status"] == 200
    assert repos[0]["data"]["description"] == "owned via node link"

    # An unrelated user still cannot.
    nope = asyncio.run(ns["repo_about_handler"](
        env, _Request("POST", url, body={
            "description": "defaced",
            "sessionToken": ns["_account_session_token"](env, "stranger")}),
        "laptop", "proj"))
    assert nope["status"] == 403
    assert repos[0]["data"]["description"] == "owned via node link"


def test_repo_about_allows_an_organization_admin_of_the_linked_repo():
    accounts = {"alice": _user(), "carol": _user(), "mallory": _user()}
    repos = [_repo_row("alice", "proj")]
    ns = _harness(accounts, repositories=repos,
                  org_admins={("alice", "proj", "carol")})
    env = object()
    url = "https://code.example.org/api/repo/acme/proj/about"

    ok = asyncio.run(ns["repo_about_handler"](
        env, _Request("POST", url, body={
            "description": "set by the organization",
            "sessionToken": ns["_account_session_token"](env, "carol")}),
        "alice", "proj"))
    assert ok["status"] == 200
    assert repos[0]["data"]["description"] == "set by the organization"

    nope = asyncio.run(ns["repo_about_handler"](
        env, _Request("POST", url, body={
            "description": "defaced",
            "sessionToken": ns["_account_session_token"](env, "mallory")}),
        "alice", "proj"))
    assert nope["status"] == 403
    assert repos[0]["data"]["description"] == "set by the organization"

    creator = asyncio.run(ns["repo_about_handler"](
        env, _Request("POST", url, body={
            "description": "creator edit",
            "sessionToken": ns["_account_session_token"](env, "alice")}),
        "alice", "proj"))
    assert creator["status"] == 200
