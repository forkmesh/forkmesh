#!/usr/bin/env python3
"""Referral-program contracts: counters, attribution, leaderboard, surfaces."""

import ast
import asyncio
import base64
import hashlib
import hmac
import importlib.util
import ipaddress
import json
import re
import sqlite3
import sys
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import parse_qs, quote, urlparse

import pytest
from world_scene_source import world_scene_source


ROOT = Path(__file__).resolve().parents[1]
if str(ROOT / "src") not in sys.path:
    sys.path.insert(0, str(ROOT / "src"))

import dashboard_shell  # noqa: E402
import static_routes  # noqa: E402


# MainWindowChat.cpp was split into feature files; read them in the original
# source order so section slices that span functions still line up.
MAIN_WINDOW_CHAT_SOURCES = (
    "MainWindowChat.cpp",
    "MainWindowNetworkInternal.h",
    "MainWindowDiagnostics.cpp",
    "MainWindowLogs.cpp",
    "MainWindowNavigation.cpp",
    "MainWindowNetworkRepos.cpp",
    "MainWindowHosts.cpp",
    "MainWindowNodes.cpp",
    "MainWindowNetworkDiagnostics.cpp",
    "MainWindowUsers.cpp",
    "MainWindowVultrMirrors.cpp",
    "MainWindowHostInstall.cpp",
)

_OG_SPEC = importlib.util.spec_from_file_location(
    "og_card", ROOT / "src" / "og_card.py")
og_card = importlib.util.module_from_spec(_OG_SPEC)
_OG_SPEC.loader.exec_module(og_card)
ENTRY_PATH = ROOT / "src" / "entry.py"
ENTRY = ENTRY_PATH.read_text(encoding="utf-8")
ENTRY_TREE = ast.parse(ENTRY, filename=str(ENTRY_PATH))
ACCOUNT_USERS_PATH = ROOT / "src" / "account_users.py"
ACCOUNT_USERS = ACCOUNT_USERS_PATH.read_text(encoding="utf-8")
ACCOUNT_USERS_TREE = ast.parse(
    ACCOUNT_USERS, filename=str(ACCOUNT_USERS_PATH))
SCHEMA = (ROOT / "src" / "schema.py").read_text(encoding="utf-8")
MIGRATION = ROOT / "migrations" / "0081_referral_stats.sql"
ATTRIBUTION_MIGRATION = ROOT / "migrations" / "0133_referral_attributions.sql"
LEGACY_TOTALS_MIGRATION = (
    ROOT / "migrations" / "0134_referral_legacy_totals.sql")
CLICK_AUDIT_MIGRATION = (
    ROOT / "migrations" / "0135_referral_click_audit.sql")
BURST_QUARANTINE_MIGRATION = (
    ROOT / "migrations" / "0136_referral_burst_quarantine.sql")
QUARANTINE_DISCORD_MIGRATION = (
    ROOT / "migrations" / "0155_quarantine_discord_identity.sql")
SIGNUP_JS = (ROOT / "public" / "signup.js").read_text(encoding="utf-8")
DASHBOARD_REFERRALS_JS = (
    ROOT / "public" / "dashboard" / "js" / "09-referrals.js"
).read_text(encoding="utf-8")
DASHBOARD_REFERRALS_VIEW = (
    ROOT / "public" / "dashboard" / "partials" / "views" / "referrals.html"
).read_text(encoding="utf-8")
DASHBOARD_PROFILE_JS = (
    ROOT / "public" / "dashboard" / "js" / "04-account.js"
).read_text(encoding="utf-8")
DASHBOARD_PROFILE_IO_JS = (
    ROOT / "public" / "dashboard" / "js" / "03-issues-profile-io.js"
).read_text(encoding="utf-8")
DASHBOARD_PROFILE_VIEW = (
    ROOT / "public" / "dashboard" / "partials" / "views"
    / "profile-overview.html"
).read_text(encoding="utf-8")
DASHBOARD_SIDEBAR = (
    ROOT / "public" / "dashboard" / "partials" / "sidebar.html"
).read_text(encoding="utf-8")
DASHBOARD_BOOT_JS = (
    ROOT / "public" / "dashboard" / "js" / "10-boot.js"
).read_text(encoding="utf-8")
WORLD = (ROOT.parent / "world" / "public" / "world" / "world.js").read_text(encoding="utf-8")
SCENE = world_scene_source()
DESKTOP_MESSAGES = (
    ROOT.parent / "desktop" / "src" / "MainWindowMessages.cpp"
).read_text(encoding="utf-8")
DESKTOP_CHAT = "".join(
    (ROOT.parent / "desktop" / "src" / name).read_text(encoding="utf-8")
    for name in MAIN_WINDOW_CHAT_SOURCES
)
DESKTOP_SETUP = (
    ROOT.parent / "desktop" / "src" / "MainWindowSetup.cpp"
).read_text(encoding="utf-8")
DESKTOP_INTERNAL = (
    ROOT.parent / "desktop" / "src" / "MainWindowInternal.h"
).read_text(encoding="utf-8")

NAME_RE = re.compile(r"^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$")


def _top_level_node(name):
    for tree in (ENTRY_TREE, ACCOUNT_USERS_TREE):
        for node in tree.body:
            if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
                if node.name == name:
                    return node
    raise AssertionError(f"{name} not found")


def _entry_constant(name):
    for tree in (ENTRY_TREE, ACCOUNT_USERS_TREE):
        for node in tree.body:
            if isinstance(node, ast.Assign):
                for target in node.targets:
                    if isinstance(target, ast.Name) and target.id == name:
                        return ast.literal_eval(node.value)
    raise AssertionError(f"{name} not found")


def _load_function(name, namespace):
    module = ast.fix_missing_locations(
        ast.Module(body=[_top_level_node(name)], type_ignores=[]))
    exec(compile(module, name, "exec"), namespace)
    return namespace[name]


ACCOUNTS = {
    "alice": ("bi-alice", {"name": "alice", "status": "active", "kind": "user",
                              "email_verified": True, "created_at": 100}),
    "bob": ("bi-bob", {"name": "bob", "status": "active", "kind": "user",
                          "email_verified": True, "created_at": 110}),
    "carol": ("bi-carol", {"name": "carol", "status": "active", "kind": "user",
                              "email_verified": False, "created_at": 200}),
    "dave": ("bi-dave", {"name": "dave", "status": "active", "kind": "user",
                            "email_verified": False, "created_at": 300}),
    "eve": ("bi-eve", {"name": "eve", "status": "active", "kind": "user",
                          "email_verified": False, "created_at": 400}),
    "frank": ("bi-frank", {"name": "frank", "status": "active", "kind": "user",
                              "email_verified": False, "created_at": 500}),
    "nodey": ("bi-nodey", {"status": "active", "kind": "node"}),
    "ghost": ("bi-ghost", {"status": "pending", "kind": "user"}),
}


def _namespace(db):
    accounts = {
        name: (name_bi, dict(record))
        for name, (name_bi, record) in ACCOUNTS.items()
    }

    async def d1_run(_env, sql, *args):
        db.execute(sql, args)
        db.commit()

    async def d1_all(_env, sql, *args):
        return [dict(row) for row in db.execute(sql, args).fetchall()]

    async def _contribution_run_batch(_env, statements):
        try:
            for sql, args in statements:
                db.execute(sql, args)
            db.commit()
        except Exception:
            db.rollback()
            raise

    async def _account_row(_env, name):
        return accounts.get(name, ("", None))

    async def _account_identity_rec_by_bi(_env, name_bi):
        row = db.execute(
            "SELECT data FROM users WHERE user_bi=?", (name_bi,)).fetchone()
        if not row:
            row = db.execute(
                "SELECT data FROM nodes WHERE node_bi=?",
                (name_bi,)).fetchone()
        return json.loads(row["data"]) if row and row["data"] else None

    deleted_namespaces = []

    async def _delete_account_namespace(_env, name_bi, rec):
        # The real routine erases dozens of account-keyed tables; the rows this
        # in-memory schema actually has are enough to prove it ran per identity.
        deleted_namespaces.append((name_bi, dict(rec or {})))
        for sql in (
                "DELETE FROM users WHERE user_bi=?",
                "DELETE FROM nodes WHERE node_bi=?",
                "DELETE FROM account_devices WHERE account_bi=?",
                "DELETE FROM account_sessions WHERE account_bi=?",
                "DELETE FROM account_presence WHERE name_bi=?",
                "DELETE FROM referral_attributions WHERE referred_bi=?",
        ):
            db.execute(sql, (name_bi,))
        db.commit()

    async def blind_index(_env, value):
        value = str(value or "")
        if value in accounts:
            return accounts[value][0]
        return "bi-" + hashlib.sha256(value.encode()).hexdigest()

    async def encrypt_row(_env, value):
        return json.dumps(value, separators=(",", ":"))

    async def decrypt_row(_env, value):
        return json.loads(value) if value else None

    async def edge_cache_delete(_key):
        return None

    async def bounded_json_request(request):
        return dict(getattr(request, "payload", {}) or {})

    async def _authed_account_name(_env, request):
        authorization = str(
            getattr(request, "headers", {}).get("authorization") or "")
        return "alice" if authorization == "Bearer admin-session" else ""

    async def _is_admin(_env, name):
        return name == "alice"

    verified_admin_proofs = []

    async def _verify_owner_signature(_env, actor, signature, canonical):
        verified_admin_proofs.append((actor, signature, canonical))
        return signature == "valid-signature"

    audit_events = []

    async def _audit_sensitive_action(
            _env, actor, action, subject_kind, subject, outcome, details):
        audit_events.append({
            "actor": actor, "action": action, "subjectKind": subject_kind,
            "subject": subject, "outcome": outcome, "details": details,
        })

    async def _award_badge(*_args, **_kwargs):
        return None

    async def ensure_schema(_env):
        return None

    cache_puts = []
    random_counter = [0]

    def _random_bytes(length):
        random_counter[0] += 1
        seed = hashlib.sha256(str(random_counter[0]).encode()).digest()
        return (seed * ((length + len(seed) - 1) // len(seed)))[:length]

    async def edge_cache_match(_key):
        return None

    async def edge_cache_put(key, resp):
        cache_puts.append((key, resp))

    def json_response(data, status=200, cache_seconds=None,
                      cache_control=None, extra_headers=None):
        return SimpleNamespace(
            data=data, status=status, cache_seconds=cache_seconds)

    class Response:
        def __init__(self, body, status=200, headers=None):
            self.body = body
            self.status = status
            self.headers = headers or {}

    async def d1_first(_env, sql, *args):
        row = db.execute(sql, args).fetchone()
        return dict(row) if row else None

    namespace = {
        "d1_run": d1_run,
        "d1_all": d1_all,
        "d1_first": d1_first,
        "_contribution_run_batch": _contribution_run_batch,
        "_account_row": _account_row,
        "_account_identity_rec_by_bi": _account_identity_rec_by_bi,
        "_delete_account_namespace": _delete_account_namespace,
        "blind_index": blind_index,
        "encrypt_row": encrypt_row,
        "decrypt_row": decrypt_row,
        "edge_cache_delete": edge_cache_delete,
        "_users_directory_cache_forget": lambda: edge_cache_delete(None),
        "bounded_json_request": bounded_json_request,
        "_authed_account_name": _authed_account_name,
        "_is_admin": _is_admin,
        "_ts_ok": lambda stamp: stamp == "1720000000000",
        "_verify_owner_signature": _verify_owner_signature,
        "_audit_sensitive_action": _audit_sensitive_action,
        "_award_badge": _award_badge,
        "_account_kind": lambda rec: rec.get("kind", "user"),
        "ensure_schema": ensure_schema,
        "edge_cache_match": edge_cache_match,
        "edge_cache_put": edge_cache_put,
        "json_response": json_response,
        "Response": Response,
        "quote": quote,
        "parse_qs": parse_qs,
        "urlparse": urlparse,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "valid_node_name": lambda name: bool(NAME_RE.match(name or "")),
        "MAX_NODE_NAME": 63,
        "Date": SimpleNamespace(now=lambda: 1_720_000_000_000),
        "base64": base64,
        "hmac": hmac,
        "json": json,
        "re": re,
        "ipaddress": ipaddress,
        "world_request_country": lambda request: str(
            request.headers.get("cf-ipcountry") or "")[:2].upper(),
        "REFERRAL_NETWORK_WINDOW_MS": 30 * 24 * 60 * 60 * 1000,
        "REFERRAL_ACCOUNT_MATURITY_MS": 7 * 24 * 60 * 60 * 1000,
        "REFERRAL_DEVICE_OBSERVATION_MS": 24 * 60 * 60 * 1000,
        "REFERRAL_DEVICE_RECENT_MS": 48 * 60 * 60 * 1000,
        "REFERRAL_BURST_WINDOW_MS": 60 * 60 * 1000,
        "REFERRAL_BURST_MAX_AUTOMATIC_CREDITS": 14,
        "REFERRAL_BURST_SCAN_LIMIT": 128,
        "REFERRAL_QUALIFICATION_FLAGS": (
            "account_maturity_pending", "desktop_activity_unproven"),
        "REFERRAL_QUARANTINE_REVIEW_MS": 72 * 60 * 60 * 1000,
        "REFERRAL_CLICK_CLAIM_TTL_MS": 7 * 24 * 60 * 60 * 1000,
        "REFERRAL_RISK_REVIEW_SCORE": 60,
        "REFERRAL_RAPID_SIGNUP_WINDOW_MS": 10 * 60 * 1000,
        "REFERRAL_RAPID_SIGNUP_COUNT": 3,
        "REFERRAL_RISK_WEIGHTS": {
            "network_unavailable": 100,
            "user_agent_unavailable": 100,
            "same_network_as_referrer": 45,
            "automated_client": 100,
            "join_chronology_unavailable": 100,
            "referrer_joined_after_referred_user": 100,
            "shared_signup_network": 25,
            "shared_client_fingerprint": 35,
            "shared_user_agent": 5,
            "rapid_referrer_signups": 40,
            "referrer_rewards_frozen": 100,
        },
        "REFERRAL_CLICK_WINDOW_MS": 24 * 60 * 60 * 1000,
        "REFERRAL_CLICK_RETENTION_MS": 35 * 24 * 60 * 60 * 1000,
        "REFERRAL_CLICK_AUDIT_RETENTION_MS": 180 * 24 * 60 * 60 * 1000,
        "USERS_DIRECTORY_CACHE_KEY": _entry_constant(
            "USERS_DIRECTORY_CACHE_KEY"),
        "ACCOUNT_LOOKUP_CACHE_PREFIX": _entry_constant(
            "ACCOUNT_LOOKUP_CACHE_PREFIX"),
        "REFERRAL_LEADERBOARD_CACHE_KEY": "https://forkmesh.internal/test",
        "REFERRAL_LEADERBOARD_TTL": 30,
        "REFERRAL_LEADERBOARD_LIMIT": 10,
        "REFERRAL_PREVIEW_AGENTS": _entry_constant("REFERRAL_PREVIEW_AGENTS"),
        "ACCOUNT_USERS_ADMIN_PROOF": _entry_constant(
            "ACCOUNT_USERS_ADMIN_PROOF"),
        "ACCOUNT_USER_QUARANTINE_PROOF": _entry_constant(
            "ACCOUNT_USER_QUARANTINE_PROOF"),
        "ACCOUNT_USER_UNQUARANTINE_PROOF": _entry_constant(
            "ACCOUNT_USER_UNQUARANTINE_PROOF"),
        "ACCOUNT_USER_DELETE_PROOF": _entry_constant(
            "ACCOUNT_USER_DELETE_PROOF"),
        "ACCOUNT_ADMIN_DELETE_NODE_BATCH": _entry_constant(
            "ACCOUNT_ADMIN_DELETE_NODE_BATCH"),
        "ACCOUNT_PENDING_QUARANTINE_PROOF": _entry_constant(
            "ACCOUNT_PENDING_QUARANTINE_PROOF"),
        "ACCOUNT_ADMIN_QUARANTINE_PENDING_BATCH": _entry_constant(
            "ACCOUNT_ADMIN_QUARANTINE_PENDING_BATCH"),
        "ACCOUNT_SESSION_COOKIE": _entry_constant("ACCOUNT_SESSION_COOKIE"),
        "_MEMBER_COUNT_MEMO": {"count": 6, "ts": 1, "refresh_ts": 1},
        "_require_data_secret": lambda _env: "test-referral-data-key",
        "_random_bytes": _random_bytes,
        "_b64url_encode": lambda value: base64.urlsafe_b64encode(
            value).decode().rstrip("="),
        "og_card": og_card,
        "_ap_origin": lambda _env, _request=None: "https://forkmesh.com",
        "_html_escape": lambda text: (
            str(text or "").replace("&", "&amp;").replace("<", "&lt;")
            .replace(">", "&gt;").replace('"', "&quot;")),
    }
    for name in ("_transient_client_address", "_account_session_client_ip",
                 "_generalized_client_category",
                 "_signup_metadata", "_referral_user_agent", "_referral_flags",
                 "_referral_client_fingerprint", "_referral_risk_score",
                 "_referral_dynamic_risk_flags",
                 "_referral_click_token_signature",
                 "_referral_click_token_claims",
                 "_issue_referral_click_token", "_referral_click_source",
                 "_consume_referral_click", "_referral_updated_flags",
                 "_referral_qualification_flags",
                 "_referral_join_chronology_flags",
                 "_referral_account", "_referral_bump", "_referral_counts",
                 "_refresh_referral_signup_count",
                 "_referral_burst_cohort", "_quarantine_referral_burst",
                 "_quarantine_invalid_referral",
                 "_account_users_admin_actor",
                 "_quarantine_user_account",
                 "_account_admin_quarantine_user",
                 "_account_admin_quarantine_pending",
                 "_account_admin_unquarantine_user",
                 "_account_admin_delete_user",
                 "_referral_control_state",
                 "_is_link_preview_agent", "_referral_preview_page",
                 "_record_unique_referral_click", "referral_click",
                 "_capture_referral_source",
                 "_record_referral_signup", "_finalize_referral_signup",
                 "_credited_referrals_by_referrer",
                 "_referral_public_summary",
                 "referral_leaderboard"):
        _load_function(name, namespace)
    namespace["_cache_puts"] = cache_puts
    namespace["_accounts"] = accounts
    namespace["_audit_events"] = audit_events
    namespace["_verified_admin_proofs"] = verified_admin_proofs
    namespace["_deleted_namespaces"] = deleted_namespaces
    return namespace


def _stats_db(current_referral_schema=True):
    db = sqlite3.connect(":memory:")
    db.row_factory = sqlite3.Row
    db.execute("CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT NOT NULL)")
    db.executescript(MIGRATION.read_text(encoding="utf-8"))
    db.executescript(ATTRIBUTION_MIGRATION.read_text(encoding="utf-8"))
    db.executescript(LEGACY_TOTALS_MIGRATION.read_text(encoding="utf-8"))
    db.executescript(CLICK_AUDIT_MIGRATION.read_text(encoding="utf-8"))
    db.execute(
        "CREATE TABLE users (user_bi TEXT PRIMARY KEY,data TEXT NOT NULL,"
        "email_bi TEXT,username TEXT,is_admin INTEGER NOT NULL DEFAULT 0,"
        "ip_bi TEXT,enable_outreach INTEGER NOT NULL DEFAULT 0,"
        "discord_bi TEXT)")
    db.executemany(
        "INSERT INTO users (user_bi,data,email_bi,username,ip_bi) "
        "VALUES (?,?,?,?,?)",
        [("bi-" + name, "{}", "email-" + name, name, "ip-" + name)
         for name in ("alice", "bob", "carol", "dave", "eve", "frank")],
    )
    db.execute(
        "CREATE TABLE nodes (node_bi TEXT PRIMARY KEY,user_bi TEXT,pubkey TEXT,"
        "data TEXT NOT NULL,name TEXT,last_seen INTEGER NOT NULL DEFAULT 0)")
    db.executemany(
        "INSERT INTO nodes (node_bi,user_bi,pubkey,data,name,last_seen) "
        "VALUES (?,?,?,?,?,?)",
        [("bi-" + name, "bi-" + name, "pubkey-" + name, "{}", name, 1)
         for name in ("alice", "bob", "carol", "dave", "eve", "frank")],
    )
    db.execute(
        "CREATE TABLE account_devices (device_bi TEXT PRIMARY KEY,"
        "account_bi TEXT NOT NULL,pubkey TEXT NOT NULL,kind TEXT NOT NULL,"
        "capabilities TEXT NOT NULL,enabled INTEGER NOT NULL,"
        "created_at INTEGER NOT NULL,last_seen INTEGER NOT NULL,"
        "revoked_at INTEGER NOT NULL)")
    db.execute(
        "CREATE TABLE badge_awards (badge_slug TEXT NOT NULL,"
        "account_bi TEXT NOT NULL,name TEXT NOT NULL,granted_by TEXT NOT NULL)")
    db.execute(
        "CREATE TABLE account_sessions (session_id TEXT PRIMARY KEY,"
        "account_bi TEXT NOT NULL,revoked_at INTEGER NOT NULL DEFAULT 0)")
    db.execute(
        "CREATE TABLE account_presence (name_bi TEXT PRIMARY KEY)")
    db.execute(
        "CREATE TABLE pending_verifications "
        "(name_bi TEXT PRIMARY KEY,data TEXT NOT NULL)")
    if current_referral_schema:
        db.executescript(BURST_QUARANTINE_MIGRATION.read_text(encoding="utf-8"))
        db.executescript(
            QUARANTINE_DISCORD_MIGRATION.read_text(encoding="utf-8"))
    db.commit()
    return db


def _referral_token(namespace, referrer="alice"):
    account = ACCOUNTS.get(referrer)
    if not account:
        return ""
    return asyncio.run(namespace["_issue_referral_click_token"](
        object(), account[0]))


def _record_referral(namespace, referred, referrer, request):
    return asyncio.run(namespace["_record_referral_signup"](
        object(), referred, referrer, request,
        referral_token=_referral_token(namespace, referrer)))


def _qualify_referral_device(db, account_bi, sequence=1):
    now = 1_720_000_000_000
    observation = 24 * 60 * 60 * 1000
    db.execute(
        "INSERT INTO account_devices "
        "(device_bi,account_bi,pubkey,kind,capabilities,enabled,created_at,"
        "last_seen,revoked_at) VALUES (?,?,?,?,?,?,?,?,0)",
        ("device-" + account_bi + "-" + str(sequence), account_bi,
         "pubkey-" + account_bi + "-" + str(sequence), "desktop_node",
         "browse,owner_sign,contribution_key_proof", 1,
         now - 2 * observation, now),
    )
    db.commit()


def test_referral_migration_is_counters_only_and_matches_lazy_schema():
    db = _stats_db()
    columns = {
        row[1] for row in db.execute(
            "PRAGMA table_info(referral_stats)").fetchall()
    }
    assert columns == {"referrer_bi", "name", "clicks", "signups", "last_ts"}
    assert "CREATE TABLE IF NOT EXISTS referral_stats" in SCHEMA
    # Privacy contract: only per-referrer aggregates, never who was referred.
    assert "referred_bi" not in columns
    assert "ip_address" not in columns


def test_attribution_migration_keeps_automatic_blind_indexed_evidence():
    db = _stats_db()
    columns = {
        row[1] for row in db.execute(
            "PRAGMA table_info(referral_attributions)").fetchall()
    }
    assert {
        "referred_bi", "referred_name", "referrer_bi", "referrer_name",
        "signup_ip_bi", "user_agent_bi", "client_fingerprint_bi",
        "referral_click_id", "network_window_bi", "risk_score", "decision",
        "risk_flags", "data",
        "created_at", "verified_at", "credited_at",
    } == columns
    migration = ATTRIBUTION_MIGRATION.read_text(encoding="utf-8")
    assert "referral_click_seen" in migration
    assert "signup_ip_bi" in SCHEMA
    assert "user_agent_bi" in SCHEMA
    assert "raw source addresses and user-agent strings live only in the encrypted" \
        in migration.lower()


def test_legacy_counter_remainder_is_audited_separately_from_credit():
    db = _stats_db()
    columns = {
        row[1] for row in db.execute(
            "PRAGMA table_info(referral_legacy_totals)").fetchall()
    }
    assert columns == {
        "referrer_bi", "name", "unattributed_signups", "migrated_at"}
    assert "informational only" in LEGACY_TOTALS_MIGRATION.read_text(
        encoding="utf-8").lower()


def test_click_audit_migration_retains_only_encrypted_and_indexed_evidence():
    db = _stats_db()
    columns = {
        row[1] for row in db.execute(
            "PRAGMA table_info(referral_click_audit)").fetchall()
    }
    assert columns == {
        "visit_bi", "referrer_bi", "ip_bi", "user_agent_bi", "decision",
        "risk_flags", "data", "first_seen_at", "last_seen_at",
        "repeat_count",
    }
    migration = CLICK_AUDIT_MIGRATION.read_text(encoding="utf-8").lower()
    assert "encrypted data envelope" in migration
    assert "raw_ip" not in columns
    assert "user_agent" not in columns


def test_burst_quarantine_migration_removes_existing_bulk_credit():
    db = _stats_db(current_referral_schema=False)
    created_at = 1_720_000_000_000
    names = tuple("fake%02d" % index for index in range(15))
    db.executemany(
        "INSERT INTO users (user_bi,data,email_bi,username,ip_bi) "
        "VALUES (?,?,?,?,?)",
        [("bi-" + name, "{}", "email-" + name, name, "ip-" + name)
         for name in names],
    )
    db.executemany(
        "INSERT INTO nodes (node_bi,user_bi,pubkey,data,name,last_seen) "
        "VALUES (?,?,?,?,?,?)",
        [("bi-" + name, "bi-" + name, "pubkey-" + name, "{}", name, 1)
         for name in names],
    )
    credited_names = names[:7]
    db.execute(
        "INSERT INTO referral_stats "
        "(referrer_bi,name,clicks,signups,last_ts) VALUES (?,?,?,?,?)",
        ("bi-alice", "alice", 15, len(credited_names), created_at))
    for offset, name in enumerate(names):
        decision = "credited" if name in credited_names else "rejected"
        db.execute(
            "INSERT INTO referral_attributions "
            "(referred_bi,referred_name,referrer_bi,referrer_name,decision,"
            "risk_flags,data,created_at,verified_at,credited_at) "
            "VALUES (?,?,?,?,?,?,?,?,?,?)",
            ("bi-" + name, name, "bi-alice", "alice", decision, "[]",
             "{}", created_at + offset * 1000, created_at,
             created_at if decision == "credited" else 0))
    db.execute(
        "INSERT INTO badge_awards "
        "(badge_slug,account_bi,name,granted_by) VALUES (?,?,?,?)",
        ("first_referral", "bi-alice", "alice", "system"))
    db.commit()

    db.executescript(BURST_QUARANTINE_MIGRATION.read_text(encoding="utf-8"))
    rows = db.execute(
        "SELECT decision,risk_flags,credited_at FROM referral_attributions"
    ).fetchall()
    assert {row["decision"] for row in rows} == {"pending", "rejected"}
    assert {row["credited_at"] for row in rows} == {0}
    assert all("referrer_signup_burst" in json.loads(row["risk_flags"])
               for row in rows)
    assert db.execute(
        "SELECT signups FROM referral_stats WHERE referrer_bi='bi-alice'"
    ).fetchone()[0] == 0
    assert db.execute("SELECT COUNT(*) FROM badge_awards").fetchone()[0] == 0
    assert db.execute(
        "SELECT COUNT(*) FROM users WHERE user_bi IN "
        "(SELECT referred_bi FROM referral_attributions "
        "WHERE referrer_bi='bi-alice')"
    ).fetchone()[0] == 0
    assert db.execute(
        "SELECT COUNT(*) FROM quarantined_users "
        "WHERE referrer_bi='bi-alice' AND status='pending_verification'"
    ).fetchone()[0] == 15
    assert db.execute(
        "SELECT COUNT(*) FROM nodes WHERE node_bi IN "
        "(SELECT referred_bi FROM referral_attributions "
        "WHERE referrer_bi='bi-alice')"
    ).fetchone()[0] == 0
    assert db.execute(
        "SELECT COUNT(*) FROM quarantined_nodes "
        "WHERE status='pending_verification'"
    ).fetchone()[0] == 15
    assert db.execute(
        "SELECT state FROM referral_referrer_controls "
        "WHERE referrer_bi='bi-alice'"
    ).fetchone()[0] == "frozen"
    with pytest.raises(sqlite3.IntegrityError, match="rewards frozen"):
        db.execute(
            "UPDATE referral_attributions SET decision='credited' "
            "WHERE referred_bi=?",
            ("bi-" + names[0],),
        )


def test_admin_can_quarantine_user_and_revoke_active_identity():
    db = _stats_db()
    ns = _namespace(db)
    now = 1_720_000_000_000
    db.execute(
        "UPDATE users SET data=?,discord_bi=? WHERE user_bi='bi-bob'",
        (json.dumps({
            "name": "bob", "kind": "user", "status": "active",
            "discordId": "751648360179433622",
            "discordUsername": "bob.discord",
        }), "discord-bi-bob"),
    )
    db.execute(
        "INSERT INTO account_devices "
        "(device_bi,account_bi,pubkey,kind,capabilities,enabled,created_at,"
        "last_seen,revoked_at) VALUES (?,?,?,?,?,?,?,?,?)",
        ("device-bob", "bi-bob", "pubkey-bob-2", "desktop_node", "browse",
         1, 1, 1, 0),
    )
    db.execute(
        "INSERT INTO account_sessions(session_id,account_bi,revoked_at) "
        "VALUES (?,?,0)", ("session-bob", "bi-bob"))
    db.execute(
        "INSERT INTO account_presence(name_bi) VALUES (?)", ("bi-bob",))
    db.execute(
        "INSERT INTO referral_attributions "
        "(referred_bi,referred_name,referrer_bi,referrer_name,decision,"
        "risk_flags,data,created_at,verified_at,credited_at) "
        "VALUES (?,?,?,?,?,?,?,?,?,?)",
        ("bi-bob", "bob", "bi-alice", "alice", "credited", "[]", "{}",
         1, 1, 1),
    )
    db.commit()
    request = SimpleNamespace(
        url="https://forkmesh.test/api/accounts/admin-quarantine",
        headers={"authorization": "Bearer admin-session"},
        payload={"target": "bob"},
    )

    response = asyncio.run(
        ns["_account_admin_quarantine_user"](object(), request))

    assert response.status == 200
    assert response.data == {"ok": True, "target": "bob"}
    assert not db.execute(
        "SELECT 1 FROM users WHERE user_bi='bi-bob'").fetchone()
    quarantined = db.execute(
        "SELECT status,reason_codes,reviewed_at FROM quarantined_users "
        "WHERE user_bi='bi-bob'").fetchone()
    assert quarantined["status"] == "pending_verification"
    assert json.loads(quarantined["reason_codes"]) == [
        "manual_admin_quarantine"]
    assert quarantined["reviewed_at"] == 0
    assert db.execute(
        "SELECT discord_bi FROM quarantined_users "
        "WHERE user_bi='bi-bob'").fetchone()[0] == "discord-bi-bob"
    assert not db.execute(
        "SELECT 1 FROM nodes WHERE node_bi='bi-bob'").fetchone()
    assert tuple(db.execute(
        "SELECT status,quarantined_user_bi FROM quarantined_nodes "
        "WHERE node_bi='bi-bob'").fetchone()) == (
            "pending_verification", "bi-bob")
    assert tuple(db.execute(
        "SELECT enabled,revoked_at FROM account_devices "
        "WHERE device_bi='device-bob'").fetchone()) == (0, now)
    assert db.execute(
        "SELECT revoked_at FROM account_sessions "
        "WHERE session_id='session-bob'").fetchone()[0] == now
    assert not db.execute(
        "SELECT 1 FROM account_presence WHERE name_bi='bi-bob'").fetchone()
    referral = db.execute(
        "SELECT decision,credited_at,risk_flags FROM referral_attributions "
        "WHERE referred_bi='bi-bob'").fetchone()
    assert referral["decision"] == "rejected"
    assert referral["credited_at"] == 0
    assert "manual_admin_quarantine" in json.loads(referral["risk_flags"])
    assert ns["_MEMBER_COUNT_MEMO"]["count"] is None
    assert ns["_audit_events"][-1]["outcome"] == "success"


def test_admin_can_unquarantine_account_and_nodes_without_reviving_sessions():
    db = _stats_db()
    ns = _namespace(db)
    now = 1_720_000_000_000
    db.execute(
        "UPDATE users SET data=?,discord_bi=? WHERE user_bi='bi-bob'",
        (json.dumps({
            "name": "bob", "kind": "user", "status": "active",
            "discordId": "751648360179433622",
            "discordUsername": "bob.discord",
        }), "discord-bi-bob"),
    )
    db.execute(
        "INSERT INTO nodes (node_bi,user_bi,pubkey,data,name,last_seen) "
        "VALUES (?,?,?,?,?,?)",
        ("bi-bob-laptop", "bi-bob", "pubkey-bob-laptop", "{}",
         "bob-laptop", 7),
    )
    db.execute(
        "INSERT INTO account_devices "
        "(device_bi,account_bi,pubkey,kind,capabilities,enabled,created_at,"
        "last_seen,revoked_at) VALUES (?,?,?,?,?,?,?,?,0)",
        ("device-bob", "bi-bob", "pubkey-device-bob", "desktop_node",
         "browse", 1, 1, 1),
    )
    db.execute(
        "INSERT INTO account_sessions(session_id,account_bi,revoked_at) "
        "VALUES (?,?,0)", ("session-bob", "bi-bob"))
    db.commit()
    quarantine = SimpleNamespace(
        url="https://forkmesh.test/api/accounts/admin-quarantine",
        headers={"authorization": "Bearer admin-session"},
        payload={"target": "bob"},
    )
    restored = SimpleNamespace(
        url="https://forkmesh.test/api/accounts/admin-unquarantine",
        headers={"authorization": "Bearer admin-session"},
        payload={"target": "bob"},
    )

    assert asyncio.run(ns["_account_admin_quarantine_user"](
        object(), quarantine)).data["ok"] is True
    response = asyncio.run(ns["_account_admin_unquarantine_user"](
        object(), restored))

    assert response.status == 200
    assert response.data["target"] == "bob"
    assert response.data["nodesRestored"] == ["bob", "bob-laptop"]
    user = db.execute(
        "SELECT discord_bi,data FROM users WHERE user_bi='bi-bob'").fetchone()
    assert user["discord_bi"] == "discord-bi-bob"
    assert json.loads(user["data"])["discordUsername"] == "bob.discord"
    assert {
        row[0] for row in db.execute(
            "SELECT name FROM nodes WHERE user_bi='bi-bob'")
    } == {"bob", "bob-laptop"}
    assert tuple(db.execute(
        "SELECT status,reviewed_at,reviewed_by FROM quarantined_users "
        "WHERE user_bi='bi-bob'").fetchone()) == (
            "restored", now, "alice")
    assert {
        row[0] for row in db.execute(
            "SELECT status FROM quarantined_nodes "
            "WHERE quarantined_user_bi='bi-bob'")
    } == {"restored"}
    assert tuple(db.execute(
        "SELECT enabled,revoked_at FROM account_devices "
        "WHERE device_bi='device-bob'").fetchone()) == (0, now)
    assert db.execute(
        "SELECT revoked_at FROM account_sessions "
        "WHERE session_id='session-bob'").fetchone()[0] == now
    assert ns["_audit_events"][-1]["action"] == \
        "admin.user_unquarantine"
    assert ns["_audit_events"][-1]["outcome"] == "success"

    second = asyncio.run(ns["_account_admin_unquarantine_user"](
        object(), restored))
    assert second.status == 409
    assert second.data["error"] == "already_restored"


def test_admin_unquarantine_refuses_an_active_identity_collision():
    db = _stats_db()
    ns = _namespace(db)
    db.execute(
        "UPDATE users SET data=?,discord_bi=? WHERE user_bi='bi-bob'",
        (json.dumps({"name": "bob", "kind": "user", "status": "active"}),
         "discord-bi-bob"),
    )
    db.commit()
    quarantine = SimpleNamespace(
        url="https://forkmesh.test/api/accounts/admin-quarantine",
        headers={"authorization": "Bearer admin-session"},
        payload={"target": "bob"},
    )
    restore = SimpleNamespace(
        url="https://forkmesh.test/api/accounts/admin-unquarantine",
        headers={"authorization": "Bearer admin-session"},
        payload={"target": "bob"},
    )
    assert asyncio.run(ns["_account_admin_quarantine_user"](
        object(), quarantine)).data["ok"] is True
    db.execute(
        "INSERT INTO users "
        "(user_bi,data,email_bi,username,discord_bi) VALUES (?,?,?,?,?)",
        ("bi-intruder", "{}", "email-intruder", "intruder",
         "discord-bi-bob"),
    )
    db.commit()

    response = asyncio.run(ns["_account_admin_unquarantine_user"](
        object(), restore))

    assert response.status == 409
    assert response.data == {
        "error": "restore_conflict", "account": "intruder"}
    assert not db.execute(
        "SELECT 1 FROM users WHERE user_bi='bi-bob'").fetchone()
    assert db.execute(
        "SELECT status FROM quarantined_users WHERE user_bi='bi-bob'"
    ).fetchone()[0] == "pending_verification"


def test_admin_user_quarantine_signed_proof_is_bound_to_target():
    db = _stats_db()
    ns = _namespace(db)
    request = SimpleNamespace(
        url=("https://forkmesh.test/api/accounts/admin-quarantine"
             "?node=alice&ts=1720000000000&sig=valid-signature"),
        headers={}, payload={"target": "bob"})

    actor = asyncio.run(ns["_account_users_admin_actor"](
        object(), request, ns["ACCOUNT_USER_QUARANTINE_PROOF"], "bob"))

    assert actor == "alice"
    assert ns["_verified_admin_proofs"] == [(
        "alice", "valid-signature",
        b"forkmesh-account-user-quarantine-v1\nalice\nbob\n1720000000000",
    )]

    ns["_verified_admin_proofs"].clear()
    unquarantine_request = SimpleNamespace(
        url=("https://forkmesh.test/api/accounts/admin-unquarantine"
             "?node=alice&ts=1720000000000&sig=valid-signature"),
        headers={}, payload={"target": "bob"})
    unquarantine_actor = asyncio.run(ns["_account_users_admin_actor"](
        object(), unquarantine_request,
        ns["ACCOUNT_USER_UNQUARANTINE_PROOF"], "bob"))
    assert unquarantine_actor == "alice"
    assert ns["_verified_admin_proofs"] == [(
        "alice", "valid-signature",
        b"forkmesh-account-user-unquarantine-v1\nalice\nbob\n1720000000000",
    )]


def test_admin_user_quarantine_rejects_unauthorized_self_and_admin_targets():
    db = _stats_db()
    ns = _namespace(db)
    for name in ("alice", "bob"):
        db.execute(
            "UPDATE users SET data=? WHERE user_bi=?",
            (json.dumps({
                "name": name, "kind": "user", "status": "active"}),
             "bi-" + name),
        )
    db.execute("UPDATE users SET is_admin=1 WHERE user_bi='bi-bob'")
    db.commit()
    unauthorized = SimpleNamespace(
        url="https://forkmesh.test/api/accounts/admin-quarantine",
        headers={}, payload={"target": "bob"})
    self_request = SimpleNamespace(
        url=unauthorized.url,
        headers={"authorization": "Bearer admin-session"},
        payload={"target": "alice"})
    admin_target = SimpleNamespace(
        url=unauthorized.url,
        headers={"authorization": "Bearer admin-session"},
        payload={"target": "bob"})

    assert asyncio.run(ns["_account_admin_quarantine_user"](
        object(), unauthorized)).status == 401
    assert asyncio.run(ns["_account_admin_quarantine_user"](
        object(), self_request)).data["error"] == "self_quarantine_forbidden"
    assert asyncio.run(ns["_account_admin_quarantine_user"](
        object(), admin_target)).data["error"] == "target_is_admin"
    assert db.execute("SELECT COUNT(*) FROM users").fetchone()[0] == 6
    assert db.execute("SELECT COUNT(*) FROM quarantined_users").fetchone()[0] == 0


def _pending_queue_db(names):
    """Active users still waiting on an email confirmation, one queue row each."""
    db = _stats_db()
    for name in names:
        db.execute(
            "UPDATE users SET data=? WHERE user_bi=?",
            (json.dumps({"name": name, "kind": "user", "status": "active",
                         "email": name + "@example.test",
                         "email_verified": False}),
             "bi-" + name),
        )
        db.execute(
            "INSERT INTO pending_verifications (name_bi,data) VALUES (?,?)",
            ("bi-" + name, json.dumps(
                {"name": name, "email": name + "@example.test",
                 "joinedAt": 1})),
        )
    db.commit()
    return db


def _pending_sweep_request():
    return SimpleNamespace(
        url="https://forkmesh.test/api/accounts/admin-quarantine-pending",
        headers={"authorization": "Bearer admin-session"}, payload={})


def test_admin_quarantine_all_pending_empties_the_confirmation_queue():
    db = _pending_queue_db(("bob", "carol", "dave"))
    ns = _namespace(db)

    response = asyncio.run(
        ns["_account_admin_quarantine_pending"](object(), _pending_sweep_request()))

    assert response.status == 200
    assert response.data == {
        "ok": True, "quarantined": ["bob", "carol", "dave"],
        "cleared": 0, "held": 0, "remaining": 0}
    # Quarantine keeps the evidence and drops the queue row, so the dialog
    # that listed these accounts comes back empty instead of unchanged.
    assert db.execute(
        "SELECT COUNT(*) FROM pending_verifications").fetchone()[0] == 0
    assert sorted(row[0] for row in db.execute(
        "SELECT user_bi FROM quarantined_users")) == [
            "bi-bob", "bi-carol", "bi-dave"]
    assert db.execute(
        "SELECT COUNT(*) FROM users WHERE user_bi IN "
        "('bi-bob','bi-carol','bi-dave')").fetchone()[0] == 0
    assert ns["_audit_events"][-1] == {
        "actor": "alice", "action": "admin.pending_quarantine_sweep",
        "subjectKind": "account", "subject": "", "outcome": "success",
        "details": {"quarantined": 3, "cleared": 0, "held": 0,
                    "remaining": 0},
    }


def test_new_user_notification_uses_the_queue_sweep_for_quarantine_all():
    """The notification must not leave its displayed snapshot queued again."""
    notification = DESKTOP_SETUP[
        DESKTOP_SETUP.index("void MainWindow::pollPendingUsers()"):
        DESKTOP_SETUP.index("void MainWindow::fetchRoomPassphrase()")
    ]
    assert "adminQuarantinePendingUsers(this);" in notification
    assert "quarantinePendingUsers(fresh);" not in notification


def test_admin_quarantine_all_pending_is_capped_and_resumes_on_the_next_press():
    db = _pending_queue_db(("bob", "carol", "dave"))
    ns = _namespace(db)
    ns["ACCOUNT_ADMIN_QUARANTINE_PENDING_BATCH"] = 2

    first = asyncio.run(
        ns["_account_admin_quarantine_pending"](object(), _pending_sweep_request()))
    assert first.data["quarantined"] == ["bob", "carol"]
    assert first.data["remaining"] == 1

    second = asyncio.run(
        ns["_account_admin_quarantine_pending"](object(), _pending_sweep_request()))
    assert second.data["quarantined"] == ["dave"]
    assert second.data["remaining"] == 0
    assert db.execute(
        "SELECT COUNT(*) FROM pending_verifications").fetchone()[0] == 0


def test_admin_quarantine_all_pending_holds_admins_and_drops_dead_rows():
    db = _pending_queue_db(("alice", "bob", "carol"))
    ns = _namespace(db)
    db.execute("UPDATE users SET is_admin=1 WHERE user_bi='bi-carol'")
    # A queue row whose account is already gone: nothing left to quarantine,
    # and leaving it would stall every later press on the same dead entry.
    db.execute(
        "INSERT INTO pending_verifications (name_bi,data) VALUES (?,?)",
        ("bi-ghost", json.dumps({"name": "ghost", "email": "g@example.test"})))
    db.commit()

    response = asyncio.run(
        ns["_account_admin_quarantine_pending"](object(), _pending_sweep_request()))

    assert response.data == {
        "ok": True, "quarantined": ["bob"], "cleared": 1,
        "held": 2, "remaining": 0}
    # alice is the administrator running the sweep and carol is another
    # administrator; both keep their row and their account.
    assert sorted(row[0] for row in db.execute(
        "SELECT name_bi FROM pending_verifications")) == ["bi-alice", "bi-carol"]
    assert db.execute(
        "SELECT COUNT(*) FROM users WHERE user_bi IN "
        "('bi-alice','bi-carol')").fetchone()[0] == 2


def test_admin_quarantine_all_skips_protected_rows_before_applying_its_cap():
    db = _pending_queue_db(("alice", "bob", "carol", "dave"))
    ns = _namespace(db)
    ns["ACCOUNT_ADMIN_QUARANTINE_PENDING_BATCH"] = 1
    db.execute("UPDATE users SET is_admin=1 WHERE user_bi='bi-carol'")
    db.commit()

    # Alice (the caller) and Carol (another admin) stay in the queue, but they
    # cannot consume the one-item batch and starve Bob and Dave behind them.
    first = asyncio.run(
        ns["_account_admin_quarantine_pending"](
            object(), _pending_sweep_request()))
    assert first.data == {
        "ok": True, "quarantined": ["bob"], "cleared": 0,
        "held": 2, "remaining": 1}

    second = asyncio.run(
        ns["_account_admin_quarantine_pending"](
            object(), _pending_sweep_request()))
    assert second.data == {
        "ok": True, "quarantined": ["dave"], "cleared": 0,
        "held": 2, "remaining": 0}
    assert sorted(row[0] for row in db.execute(
        "SELECT name_bi FROM pending_verifications")) == [
            "bi-alice", "bi-carol"]


def test_admin_quarantine_all_clears_a_stale_verified_queue_entry_safely():
    db = _pending_queue_db(("bob",))
    ns = _namespace(db)
    db.execute(
        "UPDATE users SET data=? WHERE user_bi='bi-bob'",
        (json.dumps({"name": "bob", "kind": "user", "status": "active",
                     "email": "bob@example.test", "email_verified": True}),),
    )
    db.commit()

    response = asyncio.run(
        ns["_account_admin_quarantine_pending"](
            object(), _pending_sweep_request()))

    assert response.data == {
        "ok": True, "quarantined": [], "cleared": 1,
        "held": 0, "remaining": 0}
    assert db.execute(
        "SELECT 1 FROM users WHERE user_bi='bi-bob'").fetchone()
    assert not db.execute(
        "SELECT 1 FROM pending_verifications WHERE name_bi='bi-bob'").fetchone()


def test_admin_quarantine_all_pending_needs_a_proof_bound_to_the_queue():
    db = _pending_queue_db(("bob",))
    ns = _namespace(db)
    unauthorized = SimpleNamespace(
        url="https://forkmesh.test/api/accounts/admin-quarantine-pending",
        headers={}, payload={})

    assert asyncio.run(ns["_account_admin_quarantine_pending"](
        object(), unauthorized)).status == 401
    assert db.execute(
        "SELECT COUNT(*) FROM pending_verifications").fetchone()[0] == 1

    signed = SimpleNamespace(
        url=("https://forkmesh.test/api/accounts/admin-quarantine-pending"
             "?node=alice&ts=1720000000000&sig=valid-signature"),
        headers={}, payload={})
    assert asyncio.run(ns["_account_users_admin_actor"](
        object(), signed, ns["ACCOUNT_PENDING_QUARANTINE_PROOF"],
        "pending")) == "alice"
    # Naming a resource is what keeps a bare browser cookie off this route.
    assert ns["_verified_admin_proofs"][-1] == (
        "alice", "valid-signature",
        b"forkmesh-account-pending-quarantine-v1\nalice\npending"
        b"\n1720000000000",
    )


def _delete_ready_db():
    """bob: one active user with a linked node account and live sessions."""
    db = _stats_db()
    db.execute(
        "UPDATE users SET data=? WHERE user_bi='bi-bob'",
        (json.dumps({"name": "bob", "kind": "user", "status": "active"}),),
    )
    db.execute(
        "INSERT INTO nodes (node_bi,user_bi,pubkey,data,name,last_seen) "
        "VALUES (?,?,?,?,?,?)",
        ("bi-bob-laptop", "bi-bob", "pubkey-bob-laptop",
         json.dumps({"name": "bob-laptop", "kind": "node", "owner": "bob"}),
         "bob-laptop", 1),
    )
    db.execute(
        "INSERT INTO account_devices "
        "(device_bi,account_bi,pubkey,kind,capabilities,enabled,created_at,"
        "last_seen,revoked_at) VALUES (?,?,?,?,?,?,?,?,?)",
        ("device-bob", "bi-bob", "pubkey-bob-2", "desktop_node", "browse",
         1, 1, 1, 0),
    )
    db.execute(
        "INSERT INTO account_sessions(session_id,account_bi,revoked_at) "
        "VALUES (?,?,0)", ("session-bob", "bi-bob"))
    db.execute(
        "INSERT INTO account_presence(name_bi) VALUES (?)", ("bi-bob",))
    db.commit()
    return db


def test_admin_delete_erases_the_user_its_nodes_and_everything_owned():
    db = _delete_ready_db()
    ns = _namespace(db)
    request = SimpleNamespace(
        url="https://forkmesh.test/api/accounts/admin-delete",
        headers={"authorization": "Bearer admin-session"},
        payload={"target": "bob"},
    )

    response = asyncio.run(ns["_account_admin_delete_user"](object(), request))

    assert response.status == 200
    assert response.data["ok"] is True
    assert response.data["accountDeleted"] is True
    assert response.data["nodesDeleted"] == ["bob-laptop"]
    assert response.data["nodesRemaining"] == 0
    # A member's repos hang off their node accounts, so the node namespace has
    # to be deleted in its own right rather than merely unlinked.
    assert [name_bi for name_bi, _ in ns["_deleted_namespaces"]] == [
        "bi-bob-laptop", "bi-bob"]
    for table, column in (
            ("users", "user_bi"), ("nodes", "node_bi"),
            ("account_devices", "account_bi"),
            ("account_sessions", "account_bi"),
            ("account_presence", "name_bi")):
        assert not db.execute(
            "SELECT 1 FROM %s WHERE %s='bi-bob'" % (table, column)).fetchone()
    assert not db.execute(
        "SELECT 1 FROM nodes WHERE user_bi='bi-bob'").fetchone()
    # Unlike quarantine, deletion keeps no reviewable copy of the account.
    assert not db.execute("SELECT 1 FROM quarantined_users").fetchone()
    assert ns["_MEMBER_COUNT_MEMO"]["count"] is None
    audit = ns["_audit_events"][-1]
    assert audit["action"] == "admin.user_delete"
    assert audit["outcome"] == "success"


def test_admin_delete_finishes_over_two_presses_when_nodes_exceed_the_batch():
    db = _delete_ready_db()
    ns = _namespace(db)
    batch = ns["ACCOUNT_ADMIN_DELETE_NODE_BATCH"]
    for index in range(batch):
        db.execute(
            "INSERT INTO nodes (node_bi,user_bi,pubkey,data,name,last_seen) "
            "VALUES (?,?,?,?,?,?)",
            ("bi-bob-%d" % index, "bi-bob", "pubkey-bob-%d" % index,
             json.dumps({"name": "bob-%d" % index, "kind": "node"}),
             "bob-%d" % index, 1),
        )
    db.commit()
    request = SimpleNamespace(
        url="https://forkmesh.test/api/accounts/admin-delete",
        headers={"authorization": "Bearer admin-session"},
        payload={"target": "bob"},
    )

    first = asyncio.run(ns["_account_admin_delete_user"](object(), request))

    # The account outlives a partial sweep so the next press can resume; a
    # response that claimed the deletion had happened would be a lie.
    assert first.data["accountDeleted"] is False
    assert first.data["nodesRemaining"] == 1
    assert len(first.data["nodesDeleted"]) == batch
    assert db.execute(
        "SELECT 1 FROM users WHERE user_bi='bi-bob'").fetchone()

    second = asyncio.run(ns["_account_admin_delete_user"](object(), request))

    assert second.data["accountDeleted"] is True
    assert second.data["nodesRemaining"] == 0
    assert not db.execute(
        "SELECT 1 FROM users WHERE user_bi='bi-bob'").fetchone()
    assert not db.execute(
        "SELECT 1 FROM nodes WHERE user_bi='bi-bob'").fetchone()


def test_admin_delete_refuses_unauthorized_self_and_admin_targets():
    db = _delete_ready_db()
    ns = _namespace(db)
    db.execute(
        "UPDATE users SET data=?,is_admin=1 WHERE user_bi='bi-carol'",
        (json.dumps({"name": "carol", "kind": "user", "status": "active"}),),
    )
    db.commit()
    unauthorized = SimpleNamespace(
        url="https://forkmesh.test/api/accounts/admin-delete",
        headers={}, payload={"target": "bob"})
    self_request = SimpleNamespace(
        url=unauthorized.url,
        headers={"authorization": "Bearer admin-session"},
        payload={"target": "alice"})
    admin_target = SimpleNamespace(
        url=unauthorized.url,
        headers={"authorization": "Bearer admin-session"},
        payload={"target": "carol"})
    missing = SimpleNamespace(
        url=unauthorized.url,
        headers={"authorization": "Bearer admin-session"},
        payload={"target": "nobody"})

    assert asyncio.run(ns["_account_admin_delete_user"](
        object(), unauthorized)).status == 401
    assert asyncio.run(ns["_account_admin_delete_user"](
        object(), self_request)).data["error"] == "self_delete_forbidden"
    assert asyncio.run(ns["_account_admin_delete_user"](
        object(), admin_target)).data["error"] == "target_is_admin"
    assert asyncio.run(ns["_account_admin_delete_user"](
        object(), missing)).status == 404
    assert ns["_deleted_namespaces"] == []
    assert db.execute(
        "SELECT 1 FROM users WHERE user_bi='bi-bob'").fetchone()


def test_admin_delete_signed_proof_is_bound_to_target():
    db = _delete_ready_db()
    ns = _namespace(db)
    request = SimpleNamespace(
        url=("https://forkmesh.test/api/accounts/admin-delete"
             "?node=alice&ts=1720000000000&sig=valid-signature"),
        headers={}, payload={"target": "bob"})

    actor = asyncio.run(ns["_account_users_admin_actor"](
        object(), request, ns["ACCOUNT_USER_DELETE_PROOF"], "bob"))

    assert actor == "alice"
    assert ns["_verified_admin_proofs"] == [(
        "alice", "valid-signature",
        b"forkmesh-account-user-delete-v1\nalice\nbob\n1720000000000",
    )]
    # The desktop signs the same canonical, so the two spellings must match.
    assert 'QStringLiteral("forkmesh-account-user-delete-v1")' in \
        DESKTOP_INTERNAL


def test_desktop_users_page_offers_delete_beside_quarantine():
    assert '"/api/accounts/admin-delete" and method == "POST"' in ENTRY
    assert "kAccountUserDeleteProof" in DESKTOP_CHAT
    assert 'accountsApiUrl(QStringLiteral("admin-delete"))' in DESKTOP_CHAT
    # Deleting a member is irreversible: the desktop must say so and demand
    # the name be typed rather than firing on a single click.
    assert "cannot be undone" in DESKTOP_CHAT
    assert 'QStringLiteral("Type %1 to confirm this deletion:")' in DESKTOP_CHAT


def test_desktop_and_worker_offer_guarded_unquarantine():
    assert '"/api/accounts/admin-unquarantine" and method == "POST"' in ENTRY
    assert "kAccountUserUnquarantineProof" in DESKTOP_CHAT
    assert 'accountsApiUrl(QStringLiteral("admin-unquarantine"))' in \
        DESKTOP_CHAT
    assert "Revoked sessions and devices stay revoked" in DESKTOP_CHAT
    assert "referral credit remains" in DESKTOP_CHAT.lower()


def test_click_counts_one_source_per_day_and_redirects_to_signup():
    db = _stats_db()
    ns = _namespace(db)
    request = _request(BROWSER_UA, ip="203.0.113.10")
    resp = asyncio.run(ns["referral_click"](object(), "alice", request))
    assert resp.status == 302
    target = urlparse(resp.headers["location"])
    assert target.path == "/signup"
    assert parse_qs(target.query)["ref"] == ["alice"]
    assert parse_qs(target.query)["rid"]
    assert "no-store" in resp.headers["cache-control"]
    resp = asyncio.run(ns["referral_click"](object(), "alice", request))
    replay_target = urlparse(resp.headers["location"])
    assert parse_qs(replay_target.query)["ref"] == ["alice"]
    assert parse_qs(replay_target.query)["rid"] != parse_qs(target.query)["rid"]
    row = db.execute(
        "SELECT clicks, signups FROM referral_stats "
        "WHERE referrer_bi='bi-alice'").fetchone()
    assert (row["clicks"], row["signups"]) == (1, 0)
    assert db.execute("SELECT COUNT(*) FROM referral_click_seen").fetchone()[0] == 1
    audit = db.execute(
        "SELECT decision,repeat_count FROM referral_click_audit").fetchone()
    assert (audit["decision"], audit["repeat_count"]) == ("accepted", 2)


def test_click_audit_rejects_missing_and_automated_but_scores_shared_network():
    db = _stats_db()
    ns = _namespace(db)
    alice_ip_bi = asyncio.run(ns["blind_index"](object(), "198.51.100.1"))
    db.execute("UPDATE users SET ip_bi=? WHERE user_bi='bi-alice'", (alice_ip_bi,))
    db.commit()
    for request in (
            _request("", ip=""),
            _request("curl/8.0", ip="203.0.113.30")):
        assert asyncio.run(ns["_record_unique_referral_click"](
            object(), "bi-alice", request)) is False
    rows = db.execute(
        "SELECT decision,risk_flags FROM referral_click_audit "
        "ORDER BY first_seen_at,visit_bi").fetchall()
    assert len(rows) == 2
    assert {row["decision"] for row in rows} == {"rejected"}
    flags = set().union(*(json.loads(row["risk_flags"]) for row in rows))
    assert {"network_unavailable", "user_agent_unavailable",
            "automated_client"}.issubset(flags)

    # Sharing the referrer's network is recorded, but cannot by itself block a
    # legitimate household member from obtaining a signed click claim.
    same_network = _request(BROWSER_UA, ip="198.51.100.1")
    assert asyncio.run(ns["_record_unique_referral_click"](
        object(), "bi-alice", same_network)) == "accepted"
    flagged = db.execute(
        "SELECT risk_flags FROM referral_click_audit "
        "WHERE decision='accepted'").fetchone()
    assert "same_network_as_referrer" in json.loads(flagged["risk_flags"])

    # A client-controlled forwarding value cannot replace the authenticated
    # edge address for a countable click.
    forwarded = {"user-agent": BROWSER_UA,
                 "x-forwarded-for": "203.0.113.99"}
    request = SimpleNamespace(headers=SimpleNamespace(
        get=lambda key: forwarded.get(key)))
    assert asyncio.run(ns["_record_unique_referral_click"](
        object(), "bi-alice", request)) is False


def test_click_ignores_unknown_node_and_inactive_referrers():
    db = _stats_db()
    ns = _namespace(db)
    for raw in ("nobody", "nodey", "ghost", "Not A Name!", ""):
        resp = asyncio.run(ns["referral_click"](
            object(), raw, _request(BROWSER_UA, ip="203.0.113.10")))
        assert resp.status == 302
        assert resp.headers["location"] == "/signup"
    assert db.execute("SELECT COUNT(*) FROM referral_stats").fetchone()[0] == 0


def test_signup_attribution_waits_for_verified_email_and_never_self_credits():
    db = _stats_db()
    ns = _namespace(db)
    request = _request(BROWSER_UA, ip="203.0.113.20")
    _record_referral(ns, "carol", "alice", request)
    _record_referral(ns, "alice", "alice", request)
    _record_referral(ns, "carol", "nodey", request)
    _record_referral(ns, "carol", "nobody", request)
    attribution = db.execute(
        "SELECT decision FROM referral_attributions "
        "WHERE referred_bi='bi-carol'").fetchone()
    assert attribution["decision"] == "pending"
    assert db.execute("SELECT COUNT(*) FROM referral_stats").fetchone()[0] == 0
    # Calling the finalizer before verification is a no-op.
    asyncio.run(ns["_finalize_referral_signup"](object(), "bi-carol"))
    assert db.execute("SELECT COUNT(*) FROM referral_stats").fetchone()[0] == 0
    ns["_accounts"]["carol"][1]["email_verified"] = True
    asyncio.run(ns["_finalize_referral_signup"](object(), "bi-carol"))
    pending = db.execute(
        "SELECT decision,risk_flags FROM referral_attributions "
        "WHERE referred_bi='bi-carol'").fetchone()
    assert pending["decision"] == "pending"
    assert "desktop_activity_unproven" in json.loads(pending["risk_flags"])
    assert db.execute("SELECT COUNT(*) FROM referral_stats").fetchone()[0] == 0
    _qualify_referral_device(db, "bi-carol")
    asyncio.run(ns["_finalize_referral_signup"](object(), "bi-carol"))
    rows = db.execute(
        "SELECT referrer_bi, clicks, signups FROM referral_stats").fetchall()
    assert [(r["referrer_bi"], r["clicks"], r["signups"]) for r in rows] == [
        ("bi-alice", 0, 1),
    ]
    assert db.execute(
        "SELECT decision FROM referral_attributions "
        "WHERE referred_bi='bi-carol'").fetchone()[0] == "credited"


def test_signup_scores_same_referrer_network_and_rejects_missing_signals():
    db = _stats_db()
    ns = _namespace(db)
    # Match alice's persisted signup network blind index exactly.
    same_network = _request(BROWSER_UA, ip="198.51.100.1")
    alice_ip_bi = asyncio.run(ns["blind_index"](object(), "198.51.100.1"))
    db.execute("UPDATE users SET ip_bi=? WHERE user_bi='bi-alice'", (alice_ip_bi,))
    db.commit()
    _record_referral(ns, "carol", "alice", same_network)
    row = db.execute(
        "SELECT decision,risk_flags FROM referral_attributions "
        "WHERE referred_bi='bi-carol'").fetchone()
    assert row["decision"] == "pending"
    assert "same_network_as_referrer" in json.loads(row["risk_flags"])

    # A request with neither an edge address nor a UA can still create an
    # account, but it cannot create a reward-bearing referral conversion.
    _record_referral(ns, "dave", "alice", _request("", ip=""))
    row = db.execute(
        "SELECT decision,risk_flags FROM referral_attributions "
        "WHERE referred_bi='bi-dave'").fetchone()
    assert row["decision"] == "rejected"
    assert {"network_unavailable", "user_agent_unavailable"}.issubset(
        json.loads(row["risk_flags"]))
    assert "hard_referral_check_failed" in json.loads(row["risk_flags"])
    assert db.execute(
        "SELECT COUNT(*) FROM users WHERE user_bi='bi-dave'"
    ).fetchone()[0] == 0
    quarantined = db.execute(
        "SELECT status,reason_codes FROM quarantined_users "
        "WHERE user_bi='bi-dave'"
    ).fetchone()
    assert quarantined["status"] == "rejected"
    assert "network_unavailable" in json.loads(quarantined["reason_codes"])


def test_signup_rejects_referrer_who_joined_after_referred_account():
    db = _stats_db()
    ns = _namespace(db)
    ns["_accounts"]["carol"][1]["created_at"] = 50
    _record_referral(
        ns, "carol", "alice", _request(BROWSER_UA, ip="203.0.113.20"))
    row = db.execute(
        "SELECT decision,risk_flags FROM referral_attributions "
        "WHERE referred_bi='bi-carol'").fetchone()
    assert row["decision"] == "rejected"
    assert "referrer_joined_after_referred_user" in json.loads(
        row["risk_flags"])


def test_frozen_referrer_signup_never_remains_in_active_users():
    db = _stats_db()
    ns = _namespace(db)
    db.execute(
        "INSERT INTO referral_referrer_controls "
        "(referrer_bi,state,reason_codes,updated_at) VALUES (?,?,?,?)",
        ("bi-alice", "frozen", '["referrer_signup_burst"]', 1),
    )
    db.commit()

    result = _record_referral(
        ns, "carol", "alice",
        _request(BROWSER_UA, ip="203.0.113.20"))

    assert result == "quarantined"
    assert db.execute(
        "SELECT COUNT(*) FROM users WHERE user_bi='bi-carol'"
    ).fetchone()[0] == 0
    row = db.execute(
        "SELECT status,reason_codes FROM quarantined_users "
        "WHERE user_bi='bi-carol'"
    ).fetchone()
    assert row["status"] == "pending_verification"
    assert {"referrer_signup_burst", "referrer_rewards_frozen"}.issubset(
        json.loads(row["reason_codes"]))


def test_signup_holds_repeat_network_and_client_cohort_for_review():
    db = _stats_db()
    ns = _namespace(db)
    request = _request(BROWSER_UA, ip="203.0.113.44")
    shared_ip_bi = asyncio.run(ns["blind_index"](
        object(), "203.0.113.44"
    ))
    for name in ("carol", "dave"):
        _record_referral(ns, name, "alice", request)
        db.execute(
            "UPDATE users SET ip_bi=? WHERE user_bi=?",
            (shared_ip_bi, ns["_accounts"][name][0]),
        )
        ns["_accounts"][name][1]["email_verified"] = True
        _qualify_referral_device(db, ns["_accounts"][name][0])
        asyncio.run(ns["_finalize_referral_signup"](
            object(), ns["_accounts"][name][0]))
    rows = db.execute(
        "SELECT referred_name,decision,risk_flags FROM referral_attributions "
        "ORDER BY referred_name").fetchall()
    assert [(row["referred_name"], row["decision"]) for row in rows] == [
        ("carol", "credited"), ("dave", "pending")]
    assert "risk_review_required" in json.loads(rows[1]["risk_flags"])
    assert db.execute(
        "SELECT signups FROM referral_stats WHERE referrer_bi='bi-alice'"
    ).fetchone()[0] == 1


def test_finalizer_rechecks_duplicate_cohort_before_first_credit():
    db = _stats_db()
    ns = _namespace(db)
    request = _request(BROWSER_UA, ip="203.0.113.44")
    shared_ip_bi = asyncio.run(ns["blind_index"](
        object(), "203.0.113.44"
    ))
    for name in ("carol", "dave"):
        _record_referral(ns, name, "alice", request)
        db.execute(
            "UPDATE users SET ip_bi=? WHERE user_bi=?",
            (shared_ip_bi, ns["_accounts"][name][0]),
        )
        ns["_accounts"][name][1]["email_verified"] = True
        _qualify_referral_device(db, ns["_accounts"][name][0])

    outcome = asyncio.run(ns["_finalize_referral_signup"](
        object(), ns["_accounts"]["carol"][0]
    ))
    row = db.execute(
        "SELECT decision,risk_score,risk_flags FROM referral_attributions "
        "WHERE referred_bi='bi-carol'"
    ).fetchone()

    assert outcome == "pending_review"
    assert row["decision"] == "pending"
    assert row["risk_score"] >= 60
    assert {
        "shared_signup_network", "shared_client_fingerprint",
        "risk_review_required",
    }.issubset(json.loads(row["risk_flags"]))


def test_rapid_verified_referral_cohort_quarantines_every_credit():
    db = _stats_db()
    ns = _namespace(db)
    names = tuple("fake%02d" % index for index in range(15))
    for offset, name in enumerate(names):
        account_bi = "bi-" + name
        ns["_accounts"][name] = (
            account_bi,
            {"name": name, "status": "active", "kind": "user",
             "email_verified": True, "created_at": 1000 + offset},
        )
        db.execute(
            "INSERT INTO users (user_bi,data,email_bi,username,ip_bi) "
            "VALUES (?,?,?,?,?)",
            (account_bi, "{}", "email-" + name, name, "ip-" + name),
        )
        db.execute(
            "INSERT INTO nodes (node_bi,user_bi,pubkey,data,name,last_seen) "
            "VALUES (?,?,?,?,?,?)",
            (account_bi, account_bi, "pubkey-" + name, "{}", name, 1),
        )
    db.commit()
    for offset, name in enumerate(names):
        # Distinct networks reproduce the rotating-proxy bypass from the admin
        # report; a shared browser signature remains only supporting evidence.
        _record_referral(
            ns, name, "alice",
            _request(BROWSER_UA, ip="203.0.113.%d" % (20 + offset)))
        _qualify_referral_device(db, ns["_accounts"][name][0])
        asyncio.run(ns["_finalize_referral_signup"](
            object(), ns["_accounts"][name][0]))

    rows = db.execute(
        "SELECT decision,risk_flags,credited_at FROM referral_attributions "
        "WHERE referrer_bi='bi-alice' ORDER BY referred_name").fetchall()
    assert len(rows) == 15
    assert {row["decision"] for row in rows} == {"pending"}
    assert {row["credited_at"] for row in rows} == {0}
    assert all("referrer_signup_burst" in json.loads(row["risk_flags"])
               for row in rows)
    stats = db.execute(
        "SELECT signups FROM referral_stats WHERE referrer_bi='bi-alice'"
    ).fetchone()
    assert stats["signups"] == 0
    assert db.execute(
        "SELECT COUNT(*) FROM users WHERE user_bi IN "
        "(SELECT referred_bi FROM referral_attributions "
        "WHERE referrer_bi='bi-alice')"
    ).fetchone()[0] == 0
    assert db.execute(
        "SELECT COUNT(*) FROM quarantined_users "
        "WHERE referrer_bi='bi-alice' AND status='pending_verification'"
    ).fetchone()[0] == 15
    assert db.execute(
        "SELECT COUNT(*) FROM nodes WHERE node_bi IN "
        "(SELECT referred_bi FROM referral_attributions "
        "WHERE referrer_bi='bi-alice')"
    ).fetchone()[0] == 0
    assert db.execute(
        "SELECT COUNT(*) FROM quarantined_nodes "
        "WHERE status='pending_verification'"
    ).fetchone()[0] == 15
    assert db.execute(
        "SELECT state FROM referral_referrer_controls "
        "WHERE referrer_bi='bi-alice'"
    ).fetchone()[0] == "frozen"


def test_signup_attribution_failures_never_raise():
    db = _stats_db()
    ns = _namespace(db)
    token = _referral_token(ns)

    async def broken_d1_run(_env, sql, *args):
        raise RuntimeError("d1 down")

    ns["d1_run"] = broken_d1_run
    asyncio.run(ns["_record_referral_signup"](
        object(), "carol", "alice",
        _request(BROWSER_UA, ip="203.0.113.20"),
        referral_token=token))


def test_unsigned_or_replayed_browser_referrer_cannot_create_attribution():
    db = _stats_db()
    ns = _namespace(db)
    request = _request(BROWSER_UA, ip="203.0.113.20")
    asyncio.run(ns["_record_referral_signup"](
        object(), "carol", "alice", request))
    assert db.execute(
        "SELECT COUNT(*) FROM referral_attributions").fetchone()[0] == 0

    token = _referral_token(ns)
    asyncio.run(ns["_record_referral_signup"](
        object(), "carol", "alice", request, referral_token=token))
    asyncio.run(ns["_record_referral_signup"](
        object(), "dave", "alice", request, referral_token=token))
    rows = db.execute(
        "SELECT referred_name,referral_click_id FROM referral_attributions"
    ).fetchall()
    assert [(row["referred_name"], bool(row["referral_click_id"]))
            for row in rows] == [("carol", True)]


def test_risk_score_combines_signals_without_rejecting_user_agent_alone():
    db = _stats_db()
    ns = _namespace(db)
    for offset, name in enumerate(("carol", "dave", "eve", "frank")):
        _record_referral(
            ns, name, "alice",
            _request(BROWSER_UA, ip="203.0.113.%d" % (20 + offset)))
    rows = {
        row["referred_name"]: row for row in db.execute(
            "SELECT referred_name,decision,risk_score,risk_flags "
            "FROM referral_attributions")
    }
    assert rows["dave"]["decision"] == "pending"
    assert rows["dave"]["risk_score"] < 60
    assert "shared_user_agent" in json.loads(rows["dave"]["risk_flags"])
    assert rows["frank"]["risk_score"] >= 60
    assert "risk_review_required" in json.loads(rows["frank"]["risk_flags"])


def test_signup_capture_marker_distinguishes_direct_referral_and_failure():
    db = _stats_db()
    ns = _namespace(db)
    token = _referral_token(ns)
    assert asyncio.run(ns["_capture_referral_source"](
        object(), "carol", "alice", token)) == {
            "referralCaptured": True, "referrer": "alice"}
    assert asyncio.run(ns["_capture_referral_source"](
        object(), "carol", "nobody")) == {
            "referralCaptured": True, "referrer": ""}

    async def broken_lookup(*_args):
        raise RuntimeError("d1 down")

    failure_token = _referral_token(ns)
    ns["_referral_account"] = broken_lookup
    assert asyncio.run(ns["_capture_referral_source"](
        object(), "carol", "alice", failure_token)) == {
            "referralCaptured": False, "referrer": ""}


def test_leaderboard_orders_by_signups_then_clicks():
    db = _stats_db()
    ns = _namespace(db)
    seed = [
        ("bi-alice", "alice", 5, 2),
        ("bi-bob", "bob", 90, 1),
        ("bi-carol", "carol", 4, 2),
        ("bi-quiet", "quiet", 0, 0),
    ]
    for referrer_bi, name, clicks, signups in seed:
        db.execute(
            "INSERT INTO referral_stats "
            "(referrer_bi, name, clicks, signups, last_ts) VALUES (?,?,?,?,0)",
            (referrer_bi, name, clicks, signups))
    db.execute(
        "INSERT INTO referral_attributions "
        "(referred_bi,referred_name,referrer_bi,referrer_name,decision,data,"
        "created_at,credited_at) VALUES (?,?,?,?,?,?,?,?)",
        ("bi-dave", "dave", "bi-alice", "alice", "credited", "{}", 1, 2),
    )
    db.execute(
        "INSERT INTO referral_attributions "
        "(referred_bi,referred_name,referrer_bi,referrer_name,decision,data,"
        "created_at,credited_at) VALUES (?,?,?,?,?,?,?,?)",
        ("bi-carol", "carol", "bi-alice", "alice", "credited", "{}", 1, 2),
    )
    db.executemany(
        "INSERT INTO referral_attributions "
        "(referred_bi,referred_name,referrer_bi,referrer_name,decision,data,"
        "created_at,credited_at) VALUES (?,?,?,?,?,?,?,?)",
        [
            ("bi-eve", "eve", "bi-carol", "carol", "credited", "{}", 1, 2),
            ("bi-frank", "frank", "bi-carol", "carol", "credited", "{}", 1, 2),
            ("bi-grace", "grace", "bi-bob", "bob", "credited", "{}", 1, 2),
        ],
    )
    db.execute(
        "INSERT INTO referral_legacy_totals "
        "(referrer_bi,name,unattributed_signups,migrated_at) "
        "VALUES (?,?,?,?)", ("bi-alice", "alice", 7, 3))
    db.commit()
    resp = asyncio.run(ns["referral_leaderboard"](object()))
    assert resp.data["ok"] is True
    assert [(r["name"], r["clicks"], r["signups"])
            for r in resp.data["board"]] == [
        ("alice", 5, 2),
        ("carol", 4, 2),
        ("bob", 90, 1),
    ]
    assert resp.cache_seconds == 30
    assert len(ns["_cache_puts"]) == 1
    assert resp.data["board"][0]["referredUsers"] == ["carol", "dave"]
    assert resp.data["board"][0]["legacyUnattributed"] == 7
    assert resp.data["board"][1]["legacyUnattributed"] == 0


def _request(user_agent, ip="203.0.113.10", country="US"):
    values = {
        "user-agent": user_agent,
        "cf-connecting-ip": ip,
        "cf-ipcountry": country,
    }
    return SimpleNamespace(headers=SimpleNamespace(
        get=lambda key: values.get(key)))


BROWSER_UA = ("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
              "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126 Safari/537.36")


def _seed(db, referrer_bi, name, clicks, signups):
    db.execute(
        "INSERT INTO referral_stats "
        "(referrer_bi, name, clicks, signups, last_ts) VALUES (?,?,?,?,0)",
        (referrer_bi, name, clicks, signups))
    db.commit()


def test_link_preview_crawlers_get_a_card_with_the_live_counters():
    db = _stats_db()
    ns = _namespace(db)
    _seed(db, "bi-alice", "alice", 7, 2)
    db.executemany(
        "INSERT INTO referral_attributions "
        "(referred_bi,referred_name,referrer_bi,referrer_name,decision,data,"
        "created_at,credited_at) VALUES (?,?,?,?,?,?,?,?)",
        [
            ("bi-carol", "carol", "bi-alice", "alice", "credited", "{}", 1, 2),
            ("bi-dave", "dave", "bi-alice", "alice", "credited", "{}", 1, 2),
        ],
    )
    db.commit()
    resp = asyncio.run(ns["referral_click"](
        object(), "alice",
        _request("http.rb/5.1.1 (Mastodon/4.2.1; +https://mastodon.social/)")))
    assert resp.status == 200
    assert resp.headers["content-type"].startswith("text/html")
    body = resp.body
    assert ('<meta property="og:image" '
            'content="https://forkmesh.com/api/referrals/alice/card.png">'
            in body)
    assert 'content="summary_large_image"' in body
    assert "7 clicks and 2 signups so far." in body
    assert "Join ForkMesh with @alice" in body
    # A human who trips the heuristic still reaches the signup funnel.
    assert 'url=/signup?ref=alice' in body
    # Preview fetches never move the counters, and are never cached stale.
    assert "no-store" in resp.headers["cache-control"]
    row = db.execute(
        "SELECT clicks FROM referral_stats WHERE referrer_bi='bi-alice'"
    ).fetchone()
    assert row["clicks"] == 7


def test_browsers_still_bounce_to_signup_and_count_the_click():
    db = _stats_db()
    ns = _namespace(db)
    resp = asyncio.run(
        ns["referral_click"](object(), "alice", _request(BROWSER_UA)))
    assert resp.status == 302
    target = urlparse(resp.headers["location"])
    assert target.path == "/signup"
    assert parse_qs(target.query)["ref"] == ["alice"]
    assert parse_qs(target.query)["rid"]
    row = db.execute(
        "SELECT clicks FROM referral_stats WHERE referrer_bi='bi-alice'"
    ).fetchone()
    assert row["clicks"] == 1


def test_unknown_referrers_never_render_a_preview():
    db = _stats_db()
    ns = _namespace(db)
    resp = asyncio.run(ns["referral_click"](
        object(), "nobody", _request("Twitterbot/1.0")))
    assert resp.status == 302
    assert resp.headers["location"] == "/signup"


def test_preview_agent_matching_covers_the_common_unfurlers():
    db = _stats_db()
    ns = _namespace(db)
    is_preview = ns["_is_link_preview_agent"]
    for agent in ("Mastodon/4.2.1", "Twitterbot/1.0", "facebookexternalhit/1.1",
                  "Slackbot-LinkExpanding 1.0", "Discordbot/2.0",
                  "TelegramBot (like TwitterBot)", "WhatsApp/2.23",
                  "Iframely/1.3", "Misskey/2024.2", "curl/8.4.0"):
        assert is_preview(_request(agent)), agent
    for agent in (BROWSER_UA, "", None,
                  "Mozilla/5.0 (X11; Linux x86_64) Firefox/126.0"):
        assert not is_preview(_request(agent)), agent
    assert not is_preview(object())  # header access failures fall back to human


def test_preview_counts_read_straight_from_the_counter_row():
    db = _stats_db()
    ns = _namespace(db)
    assert asyncio.run(ns["_referral_counts"](object(), "bi-alice")) == (
        0, 0, 0)
    _seed(db, "bi-alice", "alice", 5, 3)
    assert asyncio.run(ns["_referral_counts"](object(), "bi-alice")) == (
        5, 0, 0)
    db.execute(
        "INSERT INTO referral_attributions "
        "(referred_bi,referred_name,referrer_bi,referrer_name,decision,data,"
        "created_at,credited_at) VALUES (?,?,?,?,?,?,?,?)",
        ("bi-carol", "carol", "bi-alice", "alice", "credited", "{}", 1, 2),
    )
    db.commit()
    assert asyncio.run(ns["_referral_counts"](object(), "bi-alice")) == (
        5, 1, 0)


def test_signup_handler_and_page_carry_the_referral_code():
    signup = ast.unparse(_top_level_node("_account_signup"))
    assert "_record_referral_signup(" in signup
    assert "data.get('ref', '')" in signup
    assert "signup_ip" in signup
    assert "ip_bi" in signup
    assert "_capture_referral_source" in signup
    assert "quarantined_users" in signup
    assert "account_pending_verification" in signup
    assert "data.get('referralToken', '')" in signup
    assert "referrer_joined_after_referred_user" in ACCOUNT_USERS
    assert "_finalize_referral_signup(env, name_bi)" in ENTRY
    assert "forkmesh.referral" in SIGNUP_JS
    assert "ref: referralCode()" in SIGNUP_JS
    assert "referralToken: referralToken()" in SIGNUP_JS


def test_public_directory_exposes_only_accepted_referrer_name():
    directory = ast.unparse(_top_level_node("_account_users_directory"))
    helper = ast.unparse(_top_level_node("_credited_referrers_by_user"))
    assert "credited_referrers" in directory
    assert "user_payload['referredBy']" in directory
    assert "decision='credited'" in helper
    for private_field in (
            "signup_ip_bi", "user_agent_bi", "risk_flags", "data"):
        assert private_field not in helper


def test_admin_directory_exposes_full_signup_evidence_without_public_caching():
    auth = ast.unparse(_top_level_node("_account_users_admin_actor"))
    fields = ast.unparse(_top_level_node("_account_admin_signup_fields"))
    directory = ast.unparse(_top_level_node("_account_users_directory"))
    assert "ACCOUNT_USERS_ADMIN_PROOF" in auth
    assert "_verify_owner_signature" in auth
    assert "_is_admin" in auth
    for field in (
            "referralStatus", "referralRiskFlags", "signupIp",
            "referralRiskScore", "referralClickId",
            "signupIpFingerprint", "signupUserAgent",
            "signupUserAgentFingerprint", "signupClientFingerprint",
            "referralNetworkFingerprint",
            "signupCountry",
            "signupClientCategory", "referralClicks", "creditedReferrals",
            "referralAuditedUniqueClicks", "referralDuplicateClickAttempts",
            "referralRejectedClickAttempts", "referralLegacyClickCount",
            "referredUsers", "legacyUnattributed", "referralLastAt"):
        assert field in fields
    assert "admin_actor" in directory
    assert "signup_fields.update" in directory
    assert "if not admin_actor" in directory
    assert "referralCaptured" in directory
    assert "legacy-unattributed" in directory


def test_admin_referrer_totals_preserve_legacy_count_without_inventing_names():
    db = _stats_db()
    db.execute(
        "INSERT INTO referral_stats "
        "(referrer_bi,name,clicks,signups,last_ts) VALUES (?,?,?,?,?)",
        ("bi-alice", "alice", 91, 35, 123),
    )
    db.execute(
        "INSERT INTO referral_attributions "
        "(referred_bi,referred_name,referrer_bi,referrer_name,decision,data,"
        "created_at,credited_at) VALUES (?,?,?,?,?,?,?,?)",
        ("bi-dave", "dave", "bi-alice", "alice", "credited", "{}", 1, 2),
    )
    db.execute(
        "INSERT INTO referral_legacy_totals "
        "(referrer_bi,name,unattributed_signups,migrated_at) "
        "VALUES (?,?,?,?)", ("bi-alice", "alice", 34, 3))
    db.executemany(
        "INSERT INTO referral_click_audit "
        "(visit_bi,referrer_bi,decision,risk_flags,data,first_seen_at,"
        "last_seen_at,repeat_count) VALUES (?,?,?,?,?,?,?,?)",
        [
            ("click-a", "bi-alice", "accepted", "[]", "{}", 1, 2, 3),
            ("click-b", "bi-alice", "duplicate", "[]", "{}", 1, 2, 2),
            ("click-c", "bi-alice", "rejected", "[]", "{}", 1, 2, 4),
        ],
    )
    db.commit()
    namespace = _namespace(db)
    fields = _load_function("_account_admin_signup_fields", namespace)
    result = asyncio.run(fields(object()))["bi-alice"]
    assert result["referralClicks"] == 91
    assert result["referralAuditedUniqueClicks"] == 1
    assert result["referralDuplicateClickAttempts"] == 4
    assert result["referralRejectedClickAttempts"] == 4
    assert result["referralLegacyClickCount"] == 90
    assert result["creditedReferrals"] == 1
    assert result["referredUsers"] == ["dave"]
    assert result["legacyUnattributed"] == 34


def test_admin_directory_surfaces_shared_ip_risk_for_direct_signups():
    db = _stats_db()
    db.execute(
        "UPDATE users SET ip_bi='shared-signup-ip' "
        "WHERE user_bi IN ('bi-carol','bi-dave')"
    )
    db.commit()
    namespace = _namespace(db)
    fields = _load_function("_account_admin_signup_fields", namespace)

    result = asyncio.run(fields(object()))

    for account_bi in ("bi-carol", "bi-dave"):
        assert "shared_signup_network" in result[account_bi][
            "referralRiskFlags"
        ]
        assert result[account_bi]["referralRiskScore"] == 25


def test_admin_directory_gate_requires_a_valid_is_admin_identity():
    async def authed(_env, request):
        return request.session_actor

    async def verify(_env, actor, signature, _canonical):
        return actor == "alice" and signature == "valid"

    async def is_admin(_env, actor):
        return actor == "alice"

    namespace = {
        "parse_qs": parse_qs,
        "urlparse": urlparse,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "MAX_NODE_NAME": 63,
        "ACCOUNT_SESSION_COOKIE": "forkmesh_account",
        "ACCOUNT_USERS_ADMIN_PROOF": _entry_constant(
            "ACCOUNT_USERS_ADMIN_PROOF"),
        "_authed_account_name": authed,
        "_verify_owner_signature": verify,
        "_is_admin": is_admin,
        "_ts_ok": lambda value: str(value).isdigit(),
    }
    gate = _load_function("_account_users_admin_actor", namespace)

    def request(url, authorization="", session_actor=""):
        values = {"authorization": authorization, "cookie": ""}
        return SimpleNamespace(
            url=url, session_actor=session_actor,
            headers=SimpleNamespace(get=lambda key: values.get(key)))

    assert asyncio.run(gate(
        object(), request("https://forkmesh.test/api/accounts/users",
                          "Bearer session", "alice"))) == "alice"
    assert asyncio.run(gate(
        object(), request("https://forkmesh.test/api/accounts/users",
                          "Bearer session", "bob"))) == ""
    assert asyncio.run(gate(
        object(), request(
            "https://forkmesh.test/api/accounts/users?node=alice&ts=1&sig=valid"
        ))) == "alice"
    assert asyncio.run(gate(
        object(), request(
            "https://forkmesh.test/api/accounts/users?node=alice&ts=1&sig=bad"
        ))) == ""


def test_desktop_admin_users_list_authenticates_and_shows_evidence_columns():
    assert "forkmesh-account-users-admin-v1" in DESKTOP_INTERNAL
    assert "kAccountUsersAdminProof" in DESKTOP_MESSAGES
    assert 'request.setRawHeader(\n                "Authorization"' in DESKTOP_MESSAGES
    assert "needsAdminPayload" in DESKTOP_CHAT
    assert "m_chatDirectoryFetchedMs = 0" in DESKTOP_CHAT
    assert "protectedFields" in DESKTOP_MESSAGES
    assert ('const bool hasCompleteAdminDirectory =\n'
            '            obj.contains(QStringLiteral("publicUsers"))') \
        in DESKTOP_MESSAGES
    assert "if (hasCompleteAdminDirectory && m_isAdmin)" in DESKTOP_MESSAGES
    for field in ("referralRiskScore", "referralClickId",
                  "signupClientFingerprint", "emailNotifications"):
        assert field in DESKTOP_CHAT
    for header in (
            "Email", "Referred by", "Referral status", "Referral risk",
            "Signup IP", "IP fingerprint", "Signup agent",
            "Agent fingerprint", "Referral network fingerprint",
            "Signup country", "Signup client", "Audited unique clicks",
            "Duplicate click attempts", "Rejected click attempts",
            "Legacy unaudited clicks"):
        assert f'QStringLiteral("{header}")' in DESKTOP_CHAT


def test_web_and_world_surfaces_show_the_board_and_share_link():
    assert '"/api/leaderboards"' in WORLD
    assert "copyReferralLink" in WORLD
    assert 'addLeaderboardCard("referrals"' in SCENE
    assert "leaderboardCardState.referralRows" in SCENE
    assert 'boardId === "referrals"' in SCENE
    assert "updateReferralLeaderboard" in SCENE


def test_referrals_page_is_one_composed_document_on_its_public_url():
    # The referral program is a composed dashboard page, but it keeps its own
    # top-level /referral URL the site header, footer and privacy page link,
    # and the hand-written duplicate that used to serve it is gone.
    assert dashboard_shell.PAGES["referrals"]["route"] == "/referral"
    assert static_routes.DASHBOARD_PAGE_ASSETS["/referral"] == \
        "referral/index.html"
    assert not (ROOT / "public" / "referrals.html").exists()
    assert not (ROOT / "public" / "referrals.js").exists()
    assert not (ROOT / "public" / "referrals").exists()
    assert "referrals.js" not in dashboard_shell.CACHE_BUSTED_ASSETS


def test_retired_plural_referrals_url_redirects_to_the_singular_page():
    # /referrals shipped first and is linked from posts, chat and old docs, so
    # it permanently 308s to /referral (query preserved) instead of 404ing.
    assert static_routes.legacy_page_redirect("/referrals", "") == "/referral"
    assert static_routes.legacy_page_redirect("/referrals/", "") == "/referral"
    assert static_routes.legacy_page_redirect("/referrals", "mock=1") == \
        "/referral?mock=1"
    assert static_routes.legacy_page_redirect("/referral", "") is None
    # The redirect only happens if the Worker sees the request at all.
    run_worker_first = (ROOT / "wrangler.toml").read_text(encoding="utf-8")
    assert '\n  "/referrals",' in run_worker_first
    assert '\n  "/referrals/",' in run_worker_first
    # Both /referrals and /referral stay reserved, so neither is read as an
    # /owner/repo shortcut while the redirect lives.
    assert not static_routes.looks_like_repo_route("/referrals/alice")
    assert not static_routes.looks_like_repo_route("/referral/alice")


def test_referrals_page_shows_the_board_and_share_link():
    # The page reads the same public top-10 endpoint the World island reads, so
    # no surface can drift on how the board ranks.
    assert '"/api/referrals/leaderboard"' in DASHBOARD_REFERRALS_JS
    assert "/r/${encodeURIComponent(name)}" in DASHBOARD_REFERRALS_JS
    # Node sessions own no referral link (same acceptance rule as the World).
    assert 'session.kind === "node"' in DASHBOARD_REFERRALS_JS
    # Getting-started instructions plus the board live in the one view.
    assert 'data-view="referrals"' in DASHBOARD_REFERRALS_VIEW
    assert "data-referrals-board" in DASHBOARD_REFERRALS_VIEW
    assert "Getting started" in DASHBOARD_REFERRALS_VIEW
    # Wired into the per-page boot dispatch and the sidebar.
    assert '"referrals": initReferralsPage,' in DASHBOARD_BOOT_JS
    assert 'data-nav="referrals"' in DASHBOARD_SIDEBAR
    assert 'href="/referral"' in DASHBOARD_SIDEBAR
    assert "row.referredUsers" in DASHBOARD_REFERRALS_JS
    assert "legacy identities" not in DASHBOARD_REFERRALS_JS


def test_public_profiles_show_exact_credited_referrals_without_legacy_gap():
    public_lookup = ENTRY[ENTRY.index("match = ACCOUNTS_RE.match(url.path)"):]
    assert "await _referral_public_summary(env, name_bi)" in public_lookup
    assert '"referredUsers": names' in ACCOUNT_USERS
    assert '"SELECT unattributed_signups FROM referral_legacy_totals "' in \
        ACCOUNT_USERS
    assert "referredUsers: Array.isArray(body.referredUsers)" in \
        DASHBOARD_PROFILE_IO_JS
    assert "data-profile-referrals-panel" in DASHBOARD_PROFILE_VIEW
    assert "renderProfileReferrals(session)" in DASHBOARD_PROFILE_JS
    assert "legacy referral" not in DASHBOARD_PROFILE_JS
    assert "Member for ${formatCount(elapsedDays)}" in DASHBOARD_PROFILE_JS
    assert "createdAt: Number(body.createdAt" in DASHBOARD_PROFILE_IO_JS


if __name__ == "__main__":
    test_referral_migration_is_counters_only_and_matches_lazy_schema()
    test_attribution_migration_keeps_automatic_blind_indexed_evidence()
    test_burst_quarantine_migration_removes_existing_bulk_credit()
    test_click_counts_one_source_per_day_and_redirects_to_signup()
    test_click_ignores_unknown_node_and_inactive_referrers()
    test_signup_attribution_waits_for_verified_email_and_never_self_credits()
    test_signup_rejects_same_referrer_network_and_missing_signals()
    test_signup_rejects_repeat_network_conversion_for_same_referrer()
    test_rapid_verified_referral_cohort_quarantines_every_credit()
    test_signup_attribution_failures_never_raise()
    test_leaderboard_orders_by_signups_then_clicks()
    test_link_preview_crawlers_get_a_card_with_the_live_counters()
    test_browsers_still_bounce_to_signup_and_count_the_click()
    test_unknown_referrers_never_render_a_preview()
    test_preview_agent_matching_covers_the_common_unfurlers()
    test_preview_counts_read_straight_from_the_counter_row()
    test_signup_handler_and_page_carry_the_referral_code()
    test_public_directory_exposes_only_accepted_referrer_name()
    test_web_and_world_surfaces_show_the_board_and_share_link()
    test_referrals_page_is_one_composed_document_on_its_public_url()
    test_retired_plural_referrals_url_redirects_to_the_singular_page()
    test_referrals_page_shows_the_board_and_share_link()
