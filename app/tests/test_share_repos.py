#!/usr/bin/env python3
"""Private-repo collaborator sharing contract checks (stdlib only, issue #9).

The share feature spans the worker and the Qt client and is held together by
Ed25519-signed canonical strings that must match byte-for-byte on both sides
(the same way test_private_repos.py pins the catalog-view token). The flow:

  * Owner grants/revokes a collaborator   -> forkmesh-share-v1 (owner-signed)
  * Owner lists collaborators             -> forkmesh-shares-list-v1 (owner-signed)
  * Grantee browses/clones a shared repo  -> forkmesh-share-view-v1 (grantee-signed)

These tests don't import the Workers-only JS runtime; they assert the wire
contract and the key security guards as source substrings so a one-sided edit
(worker OR client) can't silently break sharing or weaken its access control.
"""

import ast
import asyncio
import base64
import json
from pathlib import Path
import sys
from types import SimpleNamespace
from urllib.parse import parse_qs, urlparse

from worker_test_helpers import json_from_request_double


ENTRY = Path(__file__).resolve().parents[1] / "src" / "entry.py"
sys.path.insert(0, str(ENTRY.parent))
import security_controls  # noqa: E402
# MainWindow.cpp is split into feature TUs (MainWindow*.cpp); scan them all.
QT_SRC = Path(__file__).resolve().parents[2] / "desktop" / "src"
QT_HDR = QT_SRC / "MainWindow.h"

# SCHEMA_STATEMENTS (D1 DDL) was extracted from entry.py into schema.py;
# concatenate it so the schema source-contract assertions below still resolve.
SCHEMA = ENTRY.parent / "schema.py"
ENTRY_TEXT = (
    ENTRY.read_text(encoding="utf-8")
    + "\n"
    + (ENTRY.parent / "catalog_routes.py").read_text(encoding="utf-8")
    + "\n"
    + SCHEMA.read_text(encoding="utf-8")
)
QT_TEXT = "\n".join(
    p.read_text(encoding="utf-8") for p in sorted(QT_SRC.glob("MainWindow*.cpp"))
)
QT_HDR_TEXT = QT_HDR.read_text(encoding="utf-8") if QT_HDR.exists() else ""


# --- forkmesh-share-view-v1: the grantee read/clone token -------------------

def test_share_view_token_canonical_matches_across_worker_and_client():
    # The grantee signs viewer + owner + repo + ts under a distinct prefix; both
    # sides must agree on the leading fields (the trailing repo/name + ts field
    # is on the next wrapped line in each source file).
    common = '"forkmesh-share-view-v1\\n" + viewer + "\\n" + owner + "\\n"'
    assert common in ENTRY_TEXT
    assert common in QT_TEXT


def test_share_view_token_requires_an_active_share_row():
    # A valid grantee signature is NOT sufficient: access is granted only when a
    # repo_shares row exists for (repo, grantee). This is the heart of the ACL —
    # losing it would let any logged-in account clone any private repo.
    assert "return await _repo_shared_with(env, owner, repo, viewer)" in ENTRY_TEXT
    assert ("SELECT 1 AS one FROM repo_shares WHERE repo_bi=? AND grantee_bi=?"
            in ENTRY_TEXT)


def test_git_basic_auth_branches_on_username():
    # Git clone selects the owner vs grantee path by the Basic-auth username, so a
    # grantee token (signed with the grantee's key) is verified against the right
    # key and a forged owner-named token can't take the grantee branch.
    assert "if username and username != owner:" in ENTRY_TEXT
    assert ("return await verify_share_view_token(env, username, owner, repo, ts, sig)"
            in ENTRY_TEXT)


# --- forkmesh-share-v1 / -shares-list-v1: owner-only ACL admin --------------

def test_grant_token_is_owner_signed_and_action_bound():
    # The action ("add"/"remove") is bound into the signature so a grant token
    # can never be replayed to revoke (or vice versa), on both sides.
    assert '"forkmesh-share-v1\\n" + owner + "\\n" + repo + "\\n"' in ENTRY_TEXT
    assert 'action + "\\n" + str(ts)' in ENTRY_TEXT
    assert '"forkmesh-share-v1\\n" + repo.owner + "\\n" + repo.name' in QT_TEXT
    assert 'action + "\\n" + ts' in QT_TEXT


def test_list_token_canonical_matches_across_worker_and_client():
    assert '"forkmesh-shares-list-v1\\n" + owner + "\\n" + repo + "\\n"' in ENTRY_TEXT
    assert '"forkmesh-shares-list-v1\\n" + repo.owner + "\\n" + repo.name' in QT_TEXT


def test_grant_requires_a_real_grantee_account():
    # A token can only ever verify if the grantee is a registered account, so the
    # add path rejects unknown accounts rather than storing a dead share row.
    assert "unknown_account" in ENTRY_TEXT
    assert "if not await _owner_pubkey(env, grantee):" in ENTRY_TEXT


# --- Catalog visibility -----------------------------------------------------

def test_authenticated_catalog_includes_repos_shared_to_the_viewer():
    # A logged-in viewer additionally sees private repos shared WITH them, joined
    # through the repo_shares ACL by their own blind index.
    assert ("SELECT repo_bi FROM repo_shares WHERE grantee_bi = ?" in ENTRY_TEXT)
    # ...and the public-only branch (anonymous callers) stays public-only.
    assert ("SELECT key_bi, owner_bi, data FROM repositories WHERE is_private = 0"
            in ENTRY_TEXT)


def test_shared_repos_are_flagged_for_the_client():
    # The relay marks a shared (not owned) private repo so the client badges it
    # and clones it with the grantee token instead of the owner token.
    assert 'rec["sharedWithMe"]' in ENTRY_TEXT


# --- Schema + storage -------------------------------------------------------

def test_repo_shares_table_is_defined():
    assert "CREATE TABLE IF NOT EXISTS repo_shares" in ENTRY_TEXT
    assert "idx_repo_shares_grantee" in ENTRY_TEXT
    # And a parity D1 migration file exists for the file-based schema path.
    mig = ENTRY.resolve().parents[1] / "migrations" / "0014_repo_shares.sql"
    assert mig.exists()


# --- Client wiring ----------------------------------------------------------

def test_client_declares_collaborator_api():
    for decl in ("sharesApiUrl", "addRepoCollaborator", "removeRepoCollaborator",
                 "refreshRepoCollaborators"):
        assert decl in QT_HDR_TEXT, decl
        assert decl in QT_TEXT, decl


# --- Executable owner-only collaborator/bundle contract --------------------

def _load_share_functions(namespace):
    namespace.setdefault("bounded_json_request", json_from_request_double)
    wanted = {"_active_public_recipient_bundles", "shares_handler"}
    tree = ast.parse(ENTRY.read_text(encoding="utf-8"), filename=str(ENTRY))
    nodes = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in wanted
    ]
    assert {node.name for node in nodes} == wanted
    module = ast.fix_missing_locations(ast.Module(body=nodes, type_ignores=[]))
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace


def _b64url(raw):
    return base64.urlsafe_b64encode(raw).decode().rstrip("=")


def _public_bundle(seed=1):
    bundle = {
        "v": 1,
        "x25519": _b64url(bytes([seed]) * 32),
        "mlkem768": _b64url(bytes([seed + 1]) * 1184),
        # The handler must project this away even if a corrupt/manual DB row
        # somehow contains extra material.
        "privateKey": "must-never-leave-the-server-row",
    }
    valid = security_controls.validate_owner_public_bundle(bundle)
    assert valid
    return bundle, valid["kid"]


class _Request:
    def __init__(self, method, *, query="", payload=None):
        self.method = method
        self.url = "https://forkmesh.test/api/repo/alice/secret/shares" + query
        self._payload = payload or {}

    async def json(self):
        return self._payload


class _ShareHarness:
    def __init__(self):
        self.accounts = {"alice", "bob", "carol"}
        self.keys = {}
        self.shares = {}
        self.notifications = []
        self.queries = []
        self.audits = []

        class _Date:
            @staticmethod
            def now():
                return 1_700_000_000_000

        async def d1_all(_env, sql, *args):
            self.queries.append((sql, args))
            if "FROM repo_shares" in sql:
                repo_bi = args[0]
                rows = [
                    {
                        "grantee_bi": grantee_bi,
                        "data": record["data"],
                        "ts": record["ts"],
                    }
                    for (stored_repo_bi, grantee_bi), record
                    in self.shares.items()
                    if stored_repo_bi == repo_bi
                ]
                return sorted(rows, key=lambda row: row["ts"])
            if "FROM owner_encryption_keys" in sql:
                rows = []
                for account_bi in args:
                    for row in self.keys.get(account_bi, []):
                        if int(row.get("revoked_at") or 0) == 0:
                            rows.append({"account_bi": account_bi, **row})
                return sorted(
                    rows, key=lambda row: int(row.get("created_at") or 0),
                    reverse=True)
            raise AssertionError(sql)

        async def d1_first(_env, sql, *args):
            self.queries.append((sql, args))
            if "SELECT 1 AS one FROM repo_shares" in sql:
                return (
                    {"one": 1} if (args[0], args[1]) in self.shares else None
                )
            if "SELECT COUNT(*) AS n FROM repo_shares" in sql:
                return {
                    "n": sum(
                        1 for repo_bi, _grantee_bi in self.shares
                        if repo_bi == args[0])
                }
            raise AssertionError(sql)

        async def d1_run(_env, sql, *args):
            self.queries.append((sql, args))
            if sql.startswith("DELETE FROM repo_shares"):
                self.shares.pop((args[0], args[1]), None)
                return None
            if sql.startswith("INSERT INTO repo_shares"):
                self.shares[(args[0], args[1])] = {
                    "data": args[2], "ts": args[3]}
                return None
            raise AssertionError(sql)

        async def owner_pubkey(_env, name):
            return ("pub:" + name) if name in self.accounts else None

        async def verify(_pub, signature, _canonical):
            return signature == "valid"

        async def notify(_env, recipient, kind, title, **kwargs):
            self.notifications.append({
                "recipient": recipient, "kind": kind, "title": title,
                **kwargs,
            })

        async def audit(
            _env, actor, action, target_type="", target="",
            outcome="success", details=None,
        ):
            self.audits.append({
                "actor": actor,
                "action": action,
                "targetType": target_type,
                "target": target,
                "outcome": outcome,
                "details": details or {},
            })

        def json_response(data, status=200, cache_control=None, **_kwargs):
            return SimpleNamespace(
                data=data, status=status, cache_control=cache_control)

        namespace = {
            "MAX_REPO_GRANTEES": 100,
            "MAX_NODE_NAME": 80,
            "Date": _Date,
            "json": json,
            "parse_qs": parse_qs,
            "urlparse": urlparse,
            "security_control": security_controls,
            "ensure_schema": lambda _env: _async_none(),
            "method_name": lambda request: request.method,
            "blind_index": lambda _env, value: _async_value("bi:" + value),
            "_owner_pubkey": owner_pubkey,
            "_ts_ok": lambda ts: ts == "123",
            "ed25519_verify": verify,
            "d1_all": d1_all,
            "d1_first": d1_first,
            "d1_run": d1_run,
            "decrypt_row": lambda _env, value: _async_value(value),
            "encrypt_row": lambda _env, value: _async_value(value),
            "clean_string": lambda value, size: str(value or "")[:size],
            "json_response": json_response,
            "enqueue_notification": notify,
            "repo_web_href": lambda owner, repo: "/" + owner + "/" + repo,
            "_audit_sensitive_action": audit,
        }
        self.api = _load_share_functions(namespace)

    def active_key(self, account, seed=1, *, created_at=10):
        bundle, kid = _public_bundle(seed)
        self.keys.setdefault("bi:" + account, []).append({
            "key_id": kid,
            "public_bundle": json.dumps(bundle),
            "created_at": created_at,
            "revoked_at": 0,
        })
        return bundle, kid

    async def call(self, request):
        return await self.api["shares_handler"](
            object(), request, "alice", "secret")


async def _async_none():
    return None


async def _async_value(value):
    return value


def _share_payload(action, grantee, signature="valid"):
    return {
        "action": action,
        "grantee": grantee,
        "ts": "123",
        "sig": signature,
    }


def test_add_lists_public_bundle_and_remove_survives_key_loss():
    harness = _ShareHarness()
    original, key_id = harness.active_key("bob")
    added = asyncio.run(harness.call(_Request(
        "POST", payload=_share_payload("add", "bob"))))

    assert added.status == 200
    assert added.data["shared"] is True
    assert added.data["recipient"] == {
        "grantee": "bob",
        "keyId": key_id,
        "publicBundle": {
            "v": 1,
            "x25519": original["x25519"],
            "mlkem768": original["mlkem768"],
        },
        "createdAt": 10,
    }
    assert "must-never-leave-the-server-row" not in json.dumps(added.data)
    assert "privateKey" not in added.data["recipient"]["publicBundle"]
    assert len(harness.notifications) == 1

    listed = asyncio.run(harness.call(_Request(
        "GET", query="?ts=123&sig=valid")))
    assert listed.status == 200
    assert listed.cache_control == "no-store"
    assert listed.data["grantees"] == ["bob"]
    assert listed.data["recipients"] == [added.data["recipient"]]
    assert listed.data["encryptionReady"] is True
    assert listed.data["missingEncryptionKeys"] == []
    assert listed.data["privateKeysStored"] is False

    # Losing/revoking the public registration must not trap an owner in an ACL
    # they can no longer edit. Removal does not require the grantee key.
    harness.keys["bi:bob"].clear()
    removed = asyncio.run(harness.call(_Request(
        "POST", payload=_share_payload("remove", "bob"))))
    assert removed.status == 200
    assert removed.data == {"ok": True, "grantee": "bob", "shared": False}
    assert harness.shares == {}


def test_add_fails_closed_without_active_valid_recipient_key():
    harness = _ShareHarness()
    failed = asyncio.run(harness.call(_Request(
        "POST", payload=_share_payload("add", "bob"))))
    assert failed.status == 409
    assert failed.data["error"] == "grantee_encryption_key_required"
    assert failed.data["shared"] is False
    assert harness.shares == {}
    assert harness.notifications == []

    # A revoked registration is equally unusable.
    harness.active_key("bob")
    harness.keys["bi:bob"][0]["revoked_at"] = 99
    failed = asyncio.run(harness.call(_Request(
        "POST", payload=_share_payload("add", "bob"))))
    assert failed.status == 409
    assert harness.shares == {}


def test_owner_list_reports_legacy_share_missing_key_as_not_ready():
    harness = _ShareHarness()
    harness.shares[("bi:alice/secret", "bi:bob")] = {
        "data": {"grantee": "bob", "owner": "alice", "repo": "secret"},
        "ts": 1,
    }
    listed = asyncio.run(harness.call(_Request(
        "GET", query="?ts=123&sig=valid")))
    assert listed.status == 200
    assert listed.data["grantees"] == ["bob"]
    assert listed.data["recipients"] == []
    assert listed.data["encryptionReady"] is False
    assert listed.data["missingEncryptionKeys"] == ["bob"]


def test_unauthorized_share_list_cannot_discover_private_names_or_acl_state():
    harness = _ShareHarness()
    harness.shares[("bi:alice/secret", "bi:bob")] = {
        "data": {"grantee": "bob", "owner": "alice", "repo": "secret"},
        "ts": 1,
    }
    with_share = asyncio.run(harness.call(_Request(
        "GET", query="?ts=123&sig=invalid")))
    queries_with_share = list(harness.queries)

    harness.shares.clear()
    harness.queries.clear()
    without_share = asyncio.run(harness.call(_Request(
        "GET", query="?ts=123&sig=invalid")))

    assert with_share.status == without_share.status == 401
    assert with_share.data == without_share.data == {"error": "unauthorized"}
    assert queries_with_share == harness.queries == []
    assert "alice" not in json.dumps(with_share.data)
    assert "secret" not in json.dumps(with_share.data)
    assert "bob" not in json.dumps(with_share.data)
