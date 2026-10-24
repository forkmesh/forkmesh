#!/usr/bin/env python3
"""The first error of a group pings the platform administrators (adhoc #77).

The admin error view groups by (status, method, path, message). The row that
opens a group is the interesting event — "something new is broken" — while the
repeats behind it are noise, so only the first one raises a notification. This
rides the error path, so it must also stay bounded and never raise.

AST-extraction harness in the style of test_account_alert_signed_inbox.py.
"""

import ast
import asyncio
from pathlib import Path
import sys
from types import SimpleNamespace
from _maintenance_stubs import with_maintenance_off

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
import admin_path  # noqa: E402


ENTRY = Path(__file__).resolve().parents[1] / "src" / "entry.py"
ENTRY_TEXT = (
    ENTRY.read_text(encoding="utf-8") + "\n"
    + ENTRY.with_name("admin_console.py").read_text(encoding="utf-8")
)

FUNCS = {
    "_write_error_log",
    "_notify_new_error_group",
    "_admin_error_source",
    "log_blocked_signup",
    "log_d1_unavailable",
}
CONSTS = {"BLOCKING_SIGNUP_REFUSALS", "SIGNUP_PATH", "NODE_RESERVE_PATH",
          "NODE_FINALIZE_PATH"}


def _load(extra):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    selected = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in FUNCS
    ]
    found = {node.name for node in selected}
    assert found == FUNCS, "missing: %s" % sorted(FUNCS - found)
    constants = [
        node for node in tree.body
        if isinstance(node, ast.Assign) and {
            target.id for target in node.targets
            if isinstance(target, ast.Name)
        } & CONSTS
    ]
    selected = constants + selected
    module = ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[]))
    namespace = dict(extra)
    exec(compile(module, str(ENTRY), "exec"), with_maintenance_off(namespace))
    return namespace


class _Date:
    @staticmethod
    def now():
        return 1700000000000


def _harness(admins=("root",)):
    rows = []
    sent = []

    async def ensure_schema(_env):
        return None

    async def d1_run(_env, sql, *params):
        if sql.strip().startswith("INSERT INTO error_log"):
            rows.append(params)
        return None

    async def d1_first(_env, sql, *params):
        if "FROM error_log" not in sql:
            return None
        status, method, path, message = params
        for row in rows:
            if (row[1], row[2], row[3], row[4]) == (
                    status, method, path, message):
                return {"hit": 1}
        return None

    async def d1_all(_env, sql, *_params):
        if "is_admin=1" not in sql:
            return []
        return [{"data": name} for name in admins]

    async def decrypt_row(_env, data, key=None):
        return {"name": data}

    async def enqueue_notification(_env, recipient, kind, title, **kwargs):
        sent.append({"recipient": recipient, "kind": kind, "title": title,
                     **kwargs})
        return True

    async def _error_log_actor(_env, _request):
        return ""

    namespace = _load({
        "ensure_schema": ensure_schema,
        "d1_run": d1_run,
        "d1_first": d1_first,
        "d1_all": d1_all,
        "decrypt_row": decrypt_row,
        "enqueue_notification": enqueue_notification,
        "valid_node_name": lambda name: bool(name) and name.isalnum(),
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "_privacy_safe_error_text": lambda _path, message: message,
        "instance_privacy": SimpleNamespace(active=lambda _env: False),
        "_safe_error_text": str,
        "_error_log_actor": _error_log_actor,
        "method_name": lambda request: "GET",
        "Date": _Date,
        "MAX_ERROR_LOG": 5000,
        "MAX_NODE_NAME": 63,
        "_admin_path_rules": admin_path,
    })
    return namespace, object(), rows, sent


def test_the_first_error_of_a_group_notifies_every_administrator():
    namespace, env, rows, sent = _harness(admins=("root", "alice"))
    write = namespace["_write_error_log"]

    asyncio.run(write(env, 500, "GET", "/api/sync", "boom"))
    assert len(rows) == 1
    assert [item["recipient"] for item in sent] == ["root", "alice"]
    assert {item["kind"] for item in sent} == {"error_group"}
    assert "500" in sent[0]["title"]
    assert "/api/sync" in sent[0]["body"]
    assert sent[0]["meta"]["path"] == "/api/sync"


def test_repeats_of_a_known_group_are_silent():
    namespace, env, rows, sent = _harness()
    write = namespace["_write_error_log"]

    for _ in range(4):
        asyncio.run(write(env, 500, "GET", "/api/sync", "boom"))
    assert len(rows) == 4
    assert len(sent) == 1  # only the row that opened the group

    # A different group is its own first sighting.
    asyncio.run(write(env, 500, "GET", "/api/sync", "different boom"))
    assert len(sent) == 2


def test_browser_and_worker_errors_are_labelled_apart():
    namespace, env, _rows, sent = _harness()
    write = namespace["_write_error_log"]

    asyncio.run(write(env, 500, "JS", "/client-error/world", "boom"))
    assert sent[0]["meta"]["errorSource"] == "JavaScript"
    asyncio.run(write(env, 500, "GET", "/api/sync", "boom"))
    assert sent[1]["meta"]["errorSource"] == "Worker"


def test_a_database_outage_is_recorded_but_never_pings():
    namespace, env, rows, sent = _harness()

    for path in ("/private-or-unpublished-repository/[git-info-refs]",
                 "/api/sync"):
        asyncio.run(namespace["log_d1_unavailable"](
            env, _Request(), path, RuntimeError("D1_ERROR: overloaded")))
    assert len(rows) == 2
    assert rows[0][4] == "d1 unavailable: D1_ERROR: overloaded"
    assert sent == []

    asyncio.run(namespace["_write_error_log"](
        env, 500, "GET", "/api/sync", "boom"))
    assert len(sent) == 1


def test_desktop_reports_are_their_own_source_of_error_groups():
    # adhoc #1538: a warning dialog on a machine nobody is watching now reaches
    # the administrators, and reads as neither a Worker fault nor a browser one.
    namespace, env, _rows, sent = _harness()
    write = namespace["_write_error_log"]

    asyncio.run(write(
        env, 521, "APP", "/desktop-error/headless",
        "desktop dialog [headless] Sync inbox — relay is rate-limited"))
    assert sent[0]["meta"]["errorSource"] == "Desktop"
    assert sent[0]["title"] == "New desktop error group (521)"
    assert "Sync inbox" in sent[0]["body"]


def test_a_failing_notification_never_breaks_error_logging():
    namespace, env, rows, _sent = _harness()

    async def explode(*_args, **_kwargs):
        raise RuntimeError("relay is down")

    namespace["enqueue_notification"] = explode
    asyncio.run(namespace["_write_error_log"](
        env, 500, "GET", "/api/sync", "boom"))
    assert len(rows) == 1  # the error itself was still recorded


def test_the_group_is_read_before_the_row_that_would_answer_it():
    body = ENTRY_TEXT[ENTRY_TEXT.index("async def _write_error_log("):]
    body = body[:body.index("def _sanitize_client_error_text")]
    assert body.index("SELECT 1 AS hit FROM error_log") < body.index(
        "INSERT INTO error_log")
    assert "_notify_new_error_group(" in body


def test_error_group_is_a_recognized_notification_kind():
    kinds = ENTRY_TEXT[ENTRY_TEXT.index("NOTIFICATION_KINDS = frozenset({"):]
    kinds = kinds[:kinds.index("})")]
    assert '"error_group"' in kinds


# A signup nobody can complete is answered with a deliberate 4xx, so it raises
# nothing and used to leave no trace at all: the platform looked healthy while
# every visitor bounced off it (adhoc #70).
class _Request:
    class _Headers:
        @staticmethod
        def get(_name):
            return "8f2c-LHR"

    headers = _Headers()


def test_a_blocked_signup_pings_the_administrators():
    namespace, env, rows, sent = _harness(admins=("root", "alice"))

    asyncio.run(namespace["log_blocked_signup"](
        env, _Request(), namespace["SIGNUP_PATH"], 400,
        "inappropriate_node_name"))

    assert len(rows) == 1
    assert [item["recipient"] for item in sent] == ["root", "alice"]
    assert "inappropriate_node_name" in sent[0]["body"]
    assert namespace["SIGNUP_PATH"] in sent[0]["body"]
    assert sent[0]["meta"]["status"] == 400


def test_ordinary_form_validation_never_pings():
    # A taken name or a short password is the visitor's to fix. Recording those
    # would evict the rows that matter and ping on every typo.
    namespace, env, rows, sent = _harness()
    for reason in ("node_name_taken", "email_taken", "password_too_short",
                   "valid_email_required", "invalid_node_name"):
        asyncio.run(namespace["log_blocked_signup"](
            env, _Request(), namespace["SIGNUP_PATH"], 409, reason))
    assert rows == []
    assert sent == []


def test_each_account_creation_route_is_its_own_error_group():
    # "The website cannot sign anyone up" and "no node can finish installing"
    # are different outages and each deserves its own ping.
    namespace, env, _rows, sent = _harness()
    for path in (namespace["SIGNUP_PATH"], namespace["NODE_RESERVE_PATH"],
                 namespace["NODE_FINALIZE_PATH"]):
        for _ in range(3):
            asyncio.run(namespace["log_blocked_signup"](
                env, _Request(), path, 400, "inappropriate_node_name"))
    assert len(sent) == 3
    assert {item["meta"]["path"] for item in sent} == {
        namespace["SIGNUP_PATH"], namespace["NODE_RESERVE_PATH"],
        namespace["NODE_FINALIZE_PATH"]}
    # One row per group, not one per attempt: a visitor who can trip a refusal
    # on demand must not be able to roll real 5xx faults out of the log.
    assert len(_rows) == 3


def test_a_broken_request_object_never_costs_the_record():
    namespace, env, rows, sent = _harness()

    class Hostile:
        @property
        def headers(self):
            raise RuntimeError("no headers here")

    asyncio.run(namespace["log_blocked_signup"](
        env, Hostile(), namespace["SIGNUP_PATH"], 403, "signup_page_required"))
    assert len(rows) == 1
    assert len(sent) == 1


def test_every_blocking_signup_refusal_is_recorded():
    # The structural guard: a future refusal added to this handler without a
    # matching record would once again be a silent way for signup to die.
    handler = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _account_signup(env, request):"):
        ENTRY_TEXT.index("# The atomic one-time transition is the final gate")]
    for reason in ("signup_page_required", "invalid_signup_referral_code",
                   "account_pending_verification", "rate_limited"):
        assert reason in handler
        assert handler.count('"%s"' % reason) >= 1
    # Two expired-intent sites, the referral gate, quarantine, the per-IP
    # throttle, and username moderation.
    assert handler.count("await log_blocked_signup(") == 6
    assert handler.count("SIGNUP_PATH") == 7


def test_node_claim_routes_record_their_blocking_refusals():
    reserve = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _account_reserve(env, request):"):]
    reserve = reserve[:reserve.index("    rec = existing or {}")]
    assert "log_blocked_signup(\n            env, request, NODE_RESERVE_PATH" \
        in reserve
    handlers = (ENTRY.with_name("route_handlers.py")
                .read_text(encoding="utf-8"))
    finalize = handlers[handlers.index("async def _account_finalize"):]
    finalize = finalize[:finalize.index("    # Email + password are optional")]
    assert finalize.count("await log_blocked_signup(") == 2
    assert "NODE_FINALIZE_PATH" in finalize
