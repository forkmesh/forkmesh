#!/usr/bin/env python3

import ast
import asyncio
import sqlite3
from pathlib import Path

ENTRY = Path(__file__).resolve().parents[1] / "src" / "entry.py"
ENTRY_TEXT = ENTRY.read_text(encoding="utf-8")
NOW = 1_800_000_000_000

FUNCS = {
    "_account_totp_ok",
    "_account_profile",
    "_login_locked_until",
    "_login_record_fail",
    "_login_clear",
    "_login_failure_response",
    "_resolve_user_by_password",
    "_verify_password_miss",
}
ASSIGNS = {"UNVERIFIED_PROFILE_FIELDS", "PASSWORD_MISS_SALT",
           "PASSWORD_MISS_HASH"}


class _Request:
    method = "POST"

    def __init__(self, body):
        self.body = body


def _load(account, session_name=""):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    selected = [
        node for node in tree.body
        if (isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
            and node.name in FUNCS)
        or (isinstance(node, ast.Assign) and {
            t.id for t in node.targets if isinstance(t, ast.Name)} & ASSIGNS)
    ]
    assert FUNCS <= {getattr(node, "name", "") for node in selected}
    database = sqlite3.connect(":memory:")
    database.execute(
        "CREATE TABLE login_attempts (id_bi TEXT PRIMARY KEY, "
        "fails INTEGER NOT NULL, first_fail_ts INTEGER NOT NULL, "
        "locked_until INTEGER NOT NULL)")
    deleted = []
    totp_checks = []
    password_checks = []

    def _row(cursor):
        names = [col[0] for col in cursor.description or []]
        row = cursor.fetchone()
        return dict(zip(names, row)) if row else None

    async def d1_first(_env, sql, *args):
        row = _row(database.execute(sql, args))
        database.commit()
        return row

    async def d1_run(_env, sql, *args):
        database.execute(sql, args)
        database.commit()

    async def blind_index(_env, value):
        return "bi:" + str(value)

    async def bounded_json_request(request):
        return dict(request.body)

    async def _account_session_record(_env, _request, _data,
                                      allow_unverified=False):
        if session_name:
            return "bi:" + session_name, {"name": session_name}
        return "", None

    async def _account_row(_env, name):
        if name == account["name"]:
            return "bi:" + name, dict(account)
        return "", None

    async def verify_password(password, salt, _hash):
        password_checks.append(salt)
        return password == "right password"

    async def totp_verify(_secret, code):
        totp_checks.append(code)
        return code == "123456"

    async def _delete_account_namespace(_env, name_bi, _rec):
        deleted.append(name_bi)

    async def _login_attempt_keys(_env, _request, identifier):
        return ["source", "pair:" + identifier]

    async def _login_source_key(_env, _request):
        return "source"

    def json_response(body, status=200, **_kwargs):
        return {"status": status, "body": body}

    namespace = {
        "Date": type("Date", (), {"now": staticmethod(lambda: NOW)}),
        "LOGIN_FAIL_WINDOW_MS": 15 * 60 * 1000,
        "LOGIN_MAX_FAILS": 10,
        "LOGIN_LOCKOUT_MS": 15 * 60 * 1000,
        "MAX_NODE_NAME": 64,
        "clean_string": lambda value, maximum=240: str(value or "")[:maximum],
        "valid_node_name": lambda value: bool(value) and "@" not in value,
        "json_response": json_response,
        "d1_first": d1_first,
        "d1_run": d1_run,
        "blind_index": blind_index,
        "bounded_json_request": bounded_json_request,
        "_account_session_record": _account_session_record,
        "_account_row": _account_row,
        "decrypt_row": None,
        "verify_password": verify_password,
        "totp_verify": totp_verify,
        "_login_attempt_keys": _login_attempt_keys,
        "_login_source_key": _login_source_key,
        "_account_email_unverified": lambda _rec: False,
        "_email_not_verified_response": lambda: json_response({}, 403),
        "_delete_account_namespace": _delete_account_namespace,
    }
    exec(compile(ast.Module(body=selected, type_ignores=[]), str(ENTRY),
                 "exec"), namespace)
    namespace["deleted"] = deleted
    namespace["totp_checks"] = totp_checks
    namespace["password_checks"] = password_checks
    return namespace


def _enrolled():
    return {"name": "alice", "status": "active", "pass_hash": "h",
            "pass_salt": "s", "totp_enrolled": True, "totp_secret": "S"}


def _profile(ns, body):
    return asyncio.run(ns["_account_profile"](object(), _Request(body)))


def test_password_alone_cannot_act_on_a_totp_account():
    ns = _load(_enrolled())
    response = _profile(ns, {"identifier": "alice", "password": "right password",
                             "deleteAccount": True})
    assert response["status"] == 401
    assert response["body"] == {"error": "bad_totp"}
    assert ns["deleted"] == []


def test_password_and_code_act_on_a_totp_account():
    ns = _load(_enrolled())
    response = _profile(ns, {"identifier": "alice", "password": "right password",
                             "totp": "123456", "deleteAccount": True})
    assert response["body"] == {"ok": True, "accountDeleted": True}
    assert ns["deleted"] == ["bi:alice"]


def test_own_session_plus_password_needs_no_code():
    ns = _load(_enrolled(), session_name="alice")
    response = _profile(ns, {"identifier": "alice", "password": "right password",
                             "sessionToken": "t", "deleteAccount": True})
    assert response["body"] == {"ok": True, "accountDeleted": True}
    assert ns["totp_checks"] == []


def test_another_accounts_session_does_not_waive_the_code():
    ns = _load(_enrolled(), session_name="mallory")
    response = _profile(ns, {"identifier": "alice", "password": "right password",
                             "sessionToken": "t", "deleteAccount": True})
    assert response["body"] == {"error": "bad_totp"}
    assert ns["deleted"] == []


def test_profile_password_failures_spend_the_login_budget():
    ns = _load(_enrolled())
    statuses = [
        _profile(ns, {"identifier": "alice", "password": "wrong",
                      "deleteAccount": True})["status"]
        for _ in range(3)
    ]
    assert statuses == [401, 401, 429]


def test_profile_password_for_an_unknown_account_still_spends_one_hash():
    ns = _load(_enrolled())
    response = _profile(ns, {"identifier": "nobody", "password": "wrong",
                             "deleteAccount": True})
    assert response["body"] == {"error": "invalid_credentials"}
    assert ns["password_checks"] == [ns["PASSWORD_MISS_SALT"]]
    response = _profile(ns, {"identifier": "alice", "password": "wrong",
                             "deleteAccount": True})
    assert ns["password_checks"] == [ns["PASSWORD_MISS_SALT"], "s"]


def test_totp_guesses_lock_the_account_code_even_for_the_right_code():
    ns = _load(_enrolled())
    totp_ok = ns["_account_totp_ok"]
    for guess in range(10):
        assert not asyncio.run(totp_ok(object(), _enrolled(), "%06d" % guess))
    assert not asyncio.run(totp_ok(object(), _enrolled(), "123456"))
    assert ns["totp_checks"][-1] == "%06d" % 9


def test_right_code_clears_the_totp_budget():
    ns = _load(_enrolled())
    totp_ok = ns["_account_totp_ok"]
    for guess in range(9):
        assert not asyncio.run(totp_ok(object(), _enrolled(), "%06d" % guess))
    assert asyncio.run(totp_ok(object(), _enrolled(), "123456"))
    for guess in range(9):
        assert not asyncio.run(totp_ok(object(), _enrolled(), "%06d" % guess))
    assert asyncio.run(totp_ok(object(), _enrolled(), "123456"))


def test_every_totp_check_goes_through_the_budgeted_helper():
    callers = [
        node.name for node in ast.walk(ast.parse(ENTRY_TEXT))
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and any(isinstance(call, ast.Call)
                and getattr(call.func, "id", "") == "totp_verify"
                for call in ast.walk(node))
    ]
    assert sorted(callers) == ["_account_admin_totp_enrollment",
                               "_account_totp_ok"]


def _claim_password(ns, body):
    return asyncio.run(ns["_resolve_user_by_password"](
        object(), body, _Request(body)))


def test_claim_password_alone_cannot_act_on_a_totp_account():
    ns = _load(_enrolled())
    _bi, rec, refusal = _claim_password(
        ns, {"identifier": "alice", "password": "right password"})
    assert rec is None
    assert refusal["body"] == {"error": "bad_totp"}
    _bi, rec, refusal = _claim_password(
        ns, {"identifier": "alice", "password": "right password",
             "totp": "123456"})
    assert rec["name"] == "alice" and refusal is None


def test_claim_password_guesses_lock_the_source_even_for_the_right_password():
    ns = _load({**_enrolled(), "totp_enrolled": False})
    statuses = [
        _claim_password(ns, {"identifier": "alice", "password": "wrong"})[2][
            "status"]
        for _ in range(10)
    ]
    assert statuses[:2] == [401, 401] and statuses[-1] == 429
    _bi, rec, refusal = _claim_password(
        ns, {"identifier": "alice", "password": "right password"})
    assert rec is None
    assert refusal["status"] == 429
    assert refusal["body"]["error"] == "too_many_attempts"


def test_a_claim_password_for_an_unknown_account_still_spends_one_hash():
    ns = _load(_enrolled())
    _bi, rec, refusal = _claim_password(
        ns, {"identifier": "nobody", "password": "wrong"})
    assert rec is None and refusal["body"] == {"error": "invalid_credentials"}
    assert ns["password_checks"] == [ns["PASSWORD_MISS_SALT"]]


def test_a_missing_claim_code_never_spends_the_accounts_code_budget():
    ns = _load(_enrolled())
    for _ in range(12):
        _bi, rec, refusal = _claim_password(
            ns, {"identifier": "alice", "password": "right password"})
        assert rec is None and refusal["body"] == {"error": "bad_totp"}
    assert ns["totp_checks"] == []
    _bi, rec, refusal = _claim_password(
        ns, {"identifier": "alice", "password": "right password",
             "totp": "123456"})
    assert rec["name"] == "alice" and refusal is None


def test_a_wrong_claim_code_after_the_right_password_is_counted():
    ns = _load(_enrolled())
    for guess in range(10):
        _claim_password(ns, {"identifier": "alice", "password": "right password",
                             "totp": "%06d" % guess})
    _bi, rec, _refusal = _claim_password(
        ns, {"identifier": "alice", "password": "right password",
             "totp": "123456"})
    assert rec is None
