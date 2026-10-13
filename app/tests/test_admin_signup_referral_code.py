#!/usr/bin/env python3
"""Contracts for the optional administrator-issued one-use signup code."""

import ast
import asyncio
import base64
import hashlib
import hmac
import re
import sqlite3
import tomllib
from pathlib import Path
from types import SimpleNamespace

import pytest


ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
ACCOUNT_USERS = ROOT / "src" / "account_users.py"
SCHEMA = ROOT / "src" / "schema.py"
MIGRATION = ROOT / "migrations" / "0139_admin_signup_referral_code.sql"
TOGGLE_MIGRATION = (
    ROOT / "migrations" / "0141_admin_signup_referral_code_toggle.sql")
ENTRY_TEXT = (
    ENTRY.read_text(encoding="utf-8") + "\n\n\n"
    + ENTRY.with_name("app_runtime.py").read_text(encoding="utf-8")
)
ACCOUNT_USERS_TEXT = ACCOUNT_USERS.read_text(encoding="utf-8")
SCHEMA_TEXT = SCHEMA.read_text(encoding="utf-8")


def _namespace():
    wanted = {
        "_admin_signup_referral_code_default",
        "_admin_signup_referral_code_mode",
        "_admin_signup_referral_code_normalize",
        "_new_admin_signup_referral_code",
        "_admin_signup_referral_code_bi",
        "_ensure_admin_signup_referral_code",
        "_admin_signup_referral_code_state",
        "_admin_signup_referral_code_matches",
        "_rotate_admin_signup_referral_code",
        "_set_admin_signup_referral_code_mode",
    }
    assignments = {"ADMIN_SIGNUP_REFERRAL_CODE_CONTEXT"}
    selected = []
    for node in ast.parse(ENTRY_TEXT, filename=str(ENTRY)).body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            if node.name in wanted:
                selected.append(node)
        elif (isinstance(node, ast.Assign)
              and any(isinstance(target, ast.Name)
                      and target.id in assignments for target in node.targets)):
            selected.append(node)

    database = {"row": None}
    clock = {"now": 1_700_000_000_000}
    random_counter = {"value": 0}

    def random_bytes(size):
        random_counter["value"] += 1
        seed = random_counter["value"].to_bytes(2, "big")
        return (seed * (size // len(seed) + 1))[:size]

    async def blind_index(_env, value):
        return hashlib.sha256(str(value).encode()).hexdigest()

    async def encrypt_row(_env, value):
        return dict(value)

    async def decrypt_row(_env, value):
        return dict(value) if isinstance(value, dict) else None

    async def d1_first_primary(_env, sql, *args):
        row = database["row"]
        if not row:
            return None
        if "AND code_bi=?" in sql and row["code_bi"] != args[0]:
            return None
        return dict(row)

    async def d1_run(_env, sql, *args):
        if "INSERT OR IGNORE" in sql and database["row"] is None:
            code_bi, data, enabled, created_at, updated_at = args
            database["row"] = {
                "code_bi": code_bi,
                "data": data,
                "enabled": enabled,
                "generation": 1,
                "created_at": created_at,
                "updated_at": updated_at,
                "last_used_at": 0,
                "last_used_by_bi": "",
            }
        elif "SET enabled=?" in sql and database["row"] is not None:
            enabled, updated_at = args
            database["row"].update({
                "enabled": enabled,
                "updated_at": updated_at,
            })
        return SimpleNamespace()

    async def d1_first(_env, sql, *args):
        row = database["row"]
        if not row:
            return None
        if "last_used_by_bi" in sql:
            next_bi, data, updated_at, last_used_at, used_by, supplied_bi = args
            if row["code_bi"] != supplied_bi:
                return None
            row.update({
                "code_bi": next_bi,
                "data": data,
                "generation": row["generation"] + 1,
                "updated_at": updated_at,
                "last_used_at": last_used_at,
                "last_used_by_bi": used_by,
            })
            return {"generation": row["generation"]}
        next_bi, data, updated_at, prior_bi = args
        if row["code_bi"] != prior_bi:
            return None
        row.update({
            "code_bi": next_bi,
            "data": data,
            "generation": row["generation"] + 1,
            "updated_at": updated_at,
        })
        return dict(row)

    namespace = {
        "base64": base64,
        "blind_index": blind_index,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "Date": SimpleNamespace(now=lambda: clock["now"]),
        "d1_first": d1_first,
        "d1_first_primary": d1_first_primary,
        "d1_run": d1_run,
        "decrypt_row": decrypt_row,
        "encrypt_row": encrypt_row,
        "hmac": hmac,
        "re": re,
        "_random_bytes": random_bytes,
    }
    module = ast.fix_missing_locations(ast.Module(selected, type_ignores=[]))
    exec(compile(module, str(ENTRY), "exec"), namespace)
    namespace["_database"] = database
    return namespace


def test_deployment_default_is_explicit_and_production_starts_disabled():
    ns = _namespace()
    default = ns["_admin_signup_referral_code_default"]
    assert default(SimpleNamespace()) is False
    assert default(SimpleNamespace(SIGNUP_REFERRAL_CODE_MODE="false")) is False
    assert default(SimpleNamespace(SIGNUP_REFERRAL_CODE_MODE="true")) is True
    # Production starts disabled through the code default above; the manifest
    # no longer restates it (Workers Free caps a Worker at 64 variables).
    manifest = tomllib.loads((ROOT / "wrangler.toml").read_text("utf-8"))
    assert "SIGNUP_REFERRAL_CODE_MODE" not in manifest["vars"]


def test_current_code_is_generated_once_and_rotates_after_one_use():
    ns = _namespace()
    env = SimpleNamespace(SIGNUP_REFERRAL_CODE_MODE="true")
    first = asyncio.run(ns["_admin_signup_referral_code_state"](env))
    assert first["enabled"] is True
    assert first["generation"] == 1
    assert re.fullmatch(r"FM(?:-[A-Z2-7]{4}){4}", first["code"])
    assert asyncio.run(ns["_admin_signup_referral_code_matches"](
        env, first["code"].lower().replace("-", " ")))

    used_generation = asyncio.run(ns["_rotate_admin_signup_referral_code"](
        env, first["code"], "account-bi"))
    second = asyncio.run(ns["_admin_signup_referral_code_state"](env))
    assert used_generation == 1
    assert second["generation"] == 2
    assert second["code"] != first["code"]
    assert not asyncio.run(ns["_admin_signup_referral_code_matches"](
        env, first["code"]))
    assert asyncio.run(ns["_rotate_admin_signup_referral_code"](
        env, first["code"], "other-account-bi")) == -1


def test_disabled_mode_persists_off_and_does_not_require_a_code():
    ns = _namespace()
    env = SimpleNamespace(SIGNUP_REFERRAL_CODE_MODE="off")
    state = asyncio.run(ns["_admin_signup_referral_code_state"](env))
    assert state == {
        "enabled": False,
        "code": "",
        "generation": 1,
        "updatedAt": 1_700_000_000_000,
        "lastUsedAt": 0,
    }
    assert asyncio.run(ns["_admin_signup_referral_code_matches"](
        env, "")) is True
    assert ns["_database"]["row"]["enabled"] == 0


def test_admin_toggle_persists_without_replacing_the_current_code():
    ns = _namespace()
    env = SimpleNamespace(SIGNUP_REFERRAL_CODE_MODE="false")
    assert asyncio.run(ns["_admin_signup_referral_code_mode"](env)) is False
    original_bi = ns["_database"]["row"]["code_bi"]

    assert asyncio.run(ns["_set_admin_signup_referral_code_mode"](
        env, True)) is True
    enabled = asyncio.run(ns["_admin_signup_referral_code_state"](env))
    assert enabled["enabled"] is True
    assert re.fullmatch(r"FM(?:-[A-Z2-7]{4}){4}", enabled["code"])
    assert ns["_database"]["row"]["code_bi"] == original_bi

    assert asyncio.run(ns["_set_admin_signup_referral_code_mode"](
        env, False)) is False
    disabled = asyncio.run(ns["_admin_signup_referral_code_state"](env))
    assert disabled["enabled"] is False
    assert disabled["code"] == ""
    assert asyncio.run(ns["_admin_signup_referral_code_matches"](
        env, "")) is True


def test_signup_consumes_intent_then_atomically_rotates_before_user_write():
    signup = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _account_signup(env, request):"):
        ENTRY_TEXT.index("async def _account_password_record")
    ]
    assert "invalid_signup_referral_code" in signup
    assert signup.index("_admin_signup_referral_code_matches") < signup.index(
        "_account_row")
    assert signup.index("_consume_account_signup_intent") < signup.index(
        "_rotate_admin_signup_referral_code") < signup.index("_save_account")
    rotation = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _rotate_admin_signup_referral_code"):
        ENTRY_TEXT.index("def _account_signup_intent_signature")
    ]
    assert "WHERE id=1 AND code_bi=? RETURNING generation" in rotation
    assert "generation=generation+1" in rotation


def test_code_is_encrypted_at_rest_and_returned_only_in_admin_directory():
    table = SCHEMA_TEXT[
        SCHEMA_TEXT.index("CREATE TABLE IF NOT EXISTS admin_signup_referral_code"):
        SCHEMA_TEXT.index("idx_admin_signup_referral_code_bi")
    ]
    assert "code_bi TEXT NOT NULL" in table
    assert "data TEXT NOT NULL" in table
    assert "enabled INTEGER NOT NULL DEFAULT 0" in table
    assert "code TEXT" not in table
    directory = ACCOUNT_USERS_TEXT[
        ACCOUNT_USERS_TEXT.index("async def _account_users_directory"):
    ]
    admin_branch = directory[directory.index("if admin_actor:"):]
    assert 'payload["signupReferralCode"]' in admin_branch
    assert "_admin_signup_referral_code_state" in admin_branch
    assert admin_branch.index('payload["signupReferralCode"]') < (
        admin_branch.index("if not admin_actor:"))


def test_signup_page_and_frontend_require_the_field_only_when_flagged():
    signup_html = (ROOT / "public" / "signup.html").read_text("utf-8")
    signup_js = (ROOT / "public" / "signup.js").read_text("utf-8")
    serve = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _serve_signup_page"):
        ENTRY_TEXT.index("async def _git_host")
    ]
    assert "__FORKMESH_SIGNUP_REFERRAL_CODE_VISIBILITY__" in signup_html
    assert 'id="signup-referral-code"' in signup_html
    assert 'name="signupReferralCode"' in signup_html
    assert 'signupReferralCode: signupReferralCodeInput' in signup_js
    assert "invalid_signup_referral_code" in signup_js
    assert "_admin_signup_referral_code_mode" in serve
    assert "await _admin_signup_referral_code_mode" in serve
    assert '"" if referral_code_required else "hidden"' in serve


def test_admin_console_renders_and_dispatches_the_signup_toggle():
    console = (ROOT / "src" / "admin_console.py").read_text("utf-8")
    admin = ENTRY_TEXT[
        ENTRY_TEXT.index("    async def _admin(self, request):"):
        ENTRY_TEXT.index("\n    async def _route(", ENTRY_TEXT.index(
            "    async def _admin(self, request):"))
    ]
    assert "def _render_admin_signup_referral_code(" in console
    assert 'name="enabled" value="1"' in console
    assert "Require referral code" in console
    assert 'action="set_signup_referral_code"' in console
    assert 'action == "set_signup_referral_code"' in admin
    assert "_set_admin_signup_referral_code_mode" in admin
    assert 'audit_details = {"enabled": enabled}' in admin


def test_native_migration_matches_the_runtime_table_shape():
    migration = MIGRATION.read_text(encoding="utf-8")
    assert "CREATE TABLE IF NOT EXISTS admin_signup_referral_code" in migration
    assert "idx_admin_signup_referral_code_bi" in migration
    assert re.search(r"schema_fingerprint:[0-9a-f]{64}", migration)
    toggle = TOGGLE_MIGRATION.read_text(encoding="utf-8")
    assert "ADD COLUMN enabled INTEGER NOT NULL DEFAULT 0" in toggle
    assert "CHECK (enabled IN (0, 1))" in toggle
    assert re.search(r"schema_fingerprint:[0-9a-f]{64}", toggle)


def test_toggle_migration_turns_the_existing_requirement_off():
    connection = sqlite3.connect(":memory:")
    try:
        connection.executescript("""
            CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT NOT NULL);
            CREATE TABLE admin_signup_referral_code (
              id INTEGER PRIMARY KEY CHECK (id = 1),
              code_bi TEXT NOT NULL,
              data TEXT NOT NULL,
              generation INTEGER NOT NULL DEFAULT 1 CHECK (generation >= 1),
              created_at INTEGER NOT NULL,
              updated_at INTEGER NOT NULL,
              last_used_at INTEGER NOT NULL DEFAULT 0,
              last_used_by_bi TEXT NOT NULL DEFAULT ''
            );
            INSERT INTO admin_signup_referral_code
              (id,code_bi,data,created_at,updated_at)
            VALUES (1,'code-bi','ciphertext',1,1);
        """)
        connection.executescript(TOGGLE_MIGRATION.read_text(encoding="utf-8"))
        row = connection.execute(
            "SELECT enabled,code_bi,data FROM admin_signup_referral_code "
            "WHERE id=1").fetchone()
        assert row == (0, "code-bi", "ciphertext")
        with pytest.raises(sqlite3.IntegrityError):
            connection.execute(
                "UPDATE admin_signup_referral_code SET enabled=2 WHERE id=1")
    finally:
        connection.close()
