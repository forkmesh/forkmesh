#!/usr/bin/env python3
"""Admin quarantine-directory evidence and authorization contracts."""

import ast
import asyncio
import json
from pathlib import Path
from types import SimpleNamespace


ACCOUNT_USERS = Path(__file__).resolve().parents[1] / "src" / "account_users.py"
ACCOUNT_USERS_TEXT = ACCOUNT_USERS.read_text(encoding="utf-8")


def _load_functions(names, namespace):
    tree = ast.parse(ACCOUNT_USERS_TEXT, filename=str(ACCOUNT_USERS))
    wanted = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in names
    ]
    assert {node.name for node in wanted} == set(names)
    scope = dict(namespace)
    exec(compile(ast.fix_missing_locations(
        ast.Module(body=wanted, type_ignores=[])),
        str(ACCOUNT_USERS), "exec"), scope)
    return scope


def test_quarantined_directory_decrypts_preserved_review_evidence():
    queries = []

    async def d1_all(_env, query, limit):
        queries.append((query, limit))
        return [{
            "account_data": "account-ciphertext",
            "referral_data": "referral-ciphertext",
            "username": "fallback-name",
            "reason_codes": json.dumps([
                "rapid_referrer_signups", "shared_client_fingerprint"]),
            "quarantine_status": "pending_verification",
            "quarantined_at": 1_720_000_000_000,
            "review_after": 1_720_259_200_000,
            "reviewed_at": 0,
            "reviewed_by": "",
            "referrer_name": "guarav2631",
            "decision": "rejected",
            "risk_flags": json.dumps(["rapid_referrer_signups"]),
            "risk_score": 75,
            "referral_click_id": "click-fake-a-1",
            "referral_created_at": 1_719_999_900_000,
            "referral_verified_at": 0,
            "referral_credited_at": 1_719_999_950_000,
            "signup_ip_bi": "ip-fingerprint",
            "user_agent_bi": "agent-fingerprint",
            "client_fingerprint_bi": "client-fingerprint",
            "network_window_bi": "network-fingerprint",
            "user_bi": "user-fingerprint",
            "email_bi": "email-fingerprint",
            "ip_bi": "fallback-ip-fingerprint",
            "discord_bi": "discord-fingerprint",
            "referrer_bi": "referrer-fingerprint",
            "is_admin": 0,
            "enable_outreach": 1,
            "node_count": 2,
            "node_names": "fake-a-node,fake-a-laptop",
        }]

    async def decrypt_row(_env, ciphertext):
        if ciphertext == "account-ciphertext":
            return {
                "name": "fake-a",
                "email": "fake-a@example.test",
                "email_verified": True,
                "created_at": 1_719_999_800_000,
                "discordId": "751648360179433622",
                "discordUsername": "fake.a",
                "discordDisplayName": "Fake A",
                "profile_private": True,
                "solana": "wallet-address",
                "mastodon": "@fake@example.test",
            }
        assert ciphertext == "referral-ciphertext"
        return {
            "ip": "203.0.113.9",
            "userAgent": "Mozilla/5.0 test browser",
            "country": "in",
            "clientCategory": "browser",
        }

    def clean_string(value, limit):
        return str(value or "")[:limit]

    scope = _load_functions(
        {"_referral_flags", "_account_admin_quarantined_users"},
        {
            "json": json,
            "d1_all": d1_all,
            "decrypt_row": decrypt_row,
            "clean_string": clean_string,
            "MAX_NODE_NAME": 63,
            "valid_node_name": lambda value: bool(value),
            "_account_world_client_fields": lambda _rec: {
                "countryCode": "IN", "browser": "firefox", "os": "linux"},
            "_clean_discord_id": lambda value: str(value or ""),
        },
    )
    users = asyncio.run(scope["_account_admin_quarantined_users"](object()))

    assert len(users) == 1
    user = users[0]
    assert user["name"] == "fake-a"
    assert user["email"] == "fake-a@example.test"
    assert user["emailVerified"] is True
    assert user["quarantineStatus"] == "pending_verification"
    assert user["quarantineReasons"] == [
        "rapid_referrer_signups", "shared_client_fingerprint"]
    assert user["referralRiskFlags"] == [
        "rapid_referrer_signups", "shared_client_fingerprint"]
    assert user["referralClickId"] == "click-fake-a-1"
    assert user["referralCreditedAt"] == 1_719_999_950_000
    assert user["referredBy"] == "guarav2631"
    assert user["signupIp"] == "203.0.113.9"
    assert user["signupUserAgent"] == "Mozilla/5.0 test browser"
    assert user["signupCountry"] == "IN"
    assert user["nodeCount"] == 2
    assert user["nodes"] == ["fake-a-node", "fake-a-laptop"]
    assert user["discordUsername"] == "fake.a"
    assert user["discordDisplayName"] == "Fake A"
    assert user["discordLinked"] is True
    assert user["profilePrivate"] is True
    assert user["outreachEnabled"] is True
    assert user["discordFingerprint"] == "discord-fingerprint"
    assert "FROM quarantined_users q" in queries[0][0]
    assert "LEFT JOIN referral_attributions r" in queries[0][0]
    assert queries[0][1] == 1000


def test_quarantine_rows_are_only_attached_to_the_proven_admin_response():
    directory = ACCOUNT_USERS_TEXT[
        ACCOUNT_USERS_TEXT.index("async def _account_users_directory"):
    ]
    assert "admin_actor = await _account_users_admin_actor(env, request)" \
        in directory
    assert "await _account_admin_quarantined_users(env) if admin_actor else []" \
        in directory
    assert 'payload["quarantinedUsers"] = quarantined_users' in directory
    assert 'payload["publicUsers"] = public_out' in directory
    assert "if not admin_actor:\n        await edge_cache_put(" in directory


def test_admin_directory_includes_private_and_inactive_users_but_public_roster_does_not():
    records = {
        "public": {
            "name": "alice", "kind": "user", "status": "active",
            "created_at": 1, "email": "alice@example.test",
        },
        "private": {
            "name": "bob", "kind": "user", "status": "active",
            "profile_private": True, "created_at": 2,
            "email": "bob@example.test", "discordId": "751648360179433622",
            "discordUsername": "bob.discord", "email_notifications": False,
            "pass_hash": "never-return",
        },
        "inactive": {
            "name": "carol", "kind": "user", "status": "pending",
            "created_at": 3, "email": "carol@example.test",
        },
        "node": {"name": "machine", "kind": "node", "status": "active"},
    }
    rows = [
        {"data": key, "user_bi": "bi-" + key,
         "email_bi": "email-" + key, "ip_bi": "ip-" + key,
         "discord_bi": "discord-" + key if key == "private" else "",
         "is_admin": int(key == "public"), "enable_outreach": 0}
        for key in records
    ]
    cache_puts = []

    async def d1_all(_env, query, limit):
        assert "FROM users u" in query
        assert limit == 1000
        return rows

    async def decrypt_row(_env, value):
        return dict(records[value])

    async def admin_actor(_env, request):
        return "alice" if request.admin else ""

    async def users_cache_get():
        return None

    async def edge_cache_put(key, response):
        cache_puts.append((key, response))

    def chat_payload(rec, *_args, **_kwargs):
        return {
            "name": rec["name"], "kind": "user", "status": rec["status"],
            "createdAt": rec.get("created_at", 0),
        }

    scope = _load_functions(
        {"_account_users_directory"},
        {
            "_account_users_admin_actor": admin_actor,
            "_users_directory_cache_get": users_cache_get,
            "_credited_referrers_by_user": lambda _env: _async_value({}),
            "_account_admin_signup_fields": lambda _env: _async_value({}),
            "_account_admin_quarantined_users": lambda _env: _async_value([]),
            "d1_all": d1_all,
            "_world_user_activity_buckets": lambda _env: _async_value({}),
            "_contribution_commit_totals": lambda _env: _async_value({}),
            "decrypt_row": decrypt_row,
            "_is_public_roster_member": lambda rec: bool(
                rec and rec.get("kind") == "user"
                and rec.get("status") == "active"
                and not rec.get("profile_private")),
            "_account_kind": lambda rec: (rec or {}).get("kind", "node"),
            "clean_string": lambda value, limit: str(value or "")[:limit],
            "MAX_NODE_NAME": 63,
            "_account_chat_user_payload": chat_payload,
            "valid_node_name": lambda value: bool(value),
            "_clean_discord_id": lambda value: str(value or ""),
            "_admin_signup_referral_code_state": (
                lambda _env: _async_value({"enabled": True})),
            "edge_cache_put": edge_cache_put,
            "USERS_DIRECTORY_CACHE_KEY": "users-cache",
            "USERS_DIRECTORY_TTL": 30,
            "USERS_DIRECTORY_CLIENT_CACHE_CONTROL": "no-store",
            "json_response": lambda payload, **_kwargs: SimpleNamespace(
                data=payload),
        },
    )
    directory = scope["_account_users_directory"]

    admin = asyncio.run(directory(
        object(), SimpleNamespace(admin=True)))
    public = asyncio.run(directory(
        object(), SimpleNamespace(admin=False)))

    assert [row["name"] for row in admin.data["users"]] == [
        "alice", "bob", "carol"]
    assert [row["name"] for row in admin.data["publicUsers"]] == ["alice"]
    bob = admin.data["users"][1]
    assert bob["discordUsername"] == "bob.discord"
    assert bob["discordId"] == "751648360179433622"
    assert bob["profilePrivate"] is True
    assert bob["emailNotifications"] is False
    assert bob["discordFingerprint"] == "discord-private"
    assert "pass_hash" not in bob
    assert "emailNotifications" not in admin.data["publicUsers"][0]
    assert [row["name"] for row in public.data["users"]] == ["alice"]
    assert "emailNotifications" not in public.data["users"][0]
    assert "publicUsers" not in public.data
    assert len(cache_puts) == 1


async def _async_value(value):
    return value
