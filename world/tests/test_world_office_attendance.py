#!/usr/bin/env python3
"""Contracts for the shared in-world Office attendance board."""

import ast
import asyncio
from pathlib import Path
import re
import sqlite3
from types import SimpleNamespace


ROOT = Path(__file__).resolve().parents[2] / "app"
ENTRY = ROOT / "src" / "entry.py"
SCHEMA = ROOT / "src" / "schema.py"
MIGRATION = ROOT / "migrations" / "0085_world_office_attendance.sql"
LIVE_MIGRATION = (
    ROOT / "migrations" / "0090_world_office_live_attendance.sql"
)
SCOPE_MIGRATION = (
    ROOT / "migrations" / "0100_world_office_attendance_scope.sql"
)
# The World and Office route bodies moved into sibling modules so Cloudflare's
# Python startup validation never parses them. This contract still covers the
# whole route family, so their source is read alongside entry.py.
WORLD_ROUTE_SOURCE = "".join(
    (Path(__file__).resolve().parents[1] / "src" / name).read_text(
        encoding="utf-8")
    for name in ("world_routes.py", "world_office_routes.py")
)
ENTRY_TEXT = (
    ENTRY.read_text(encoding="utf-8")
    + WORLD_ROUTE_SOURCE
    + (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
)


def _literal_assignment(path, name):
    tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    for node in tree.body:
        if not isinstance(node, ast.Assign):
            continue
        if any(
            isinstance(target, ast.Name) and target.id == name
            for target in node.targets
        ):
            return ast.literal_eval(node.value)
    raise AssertionError(f"missing {name}")


def _load_handler(extra_globals):
    names = {
        "_office_attendance_floor",
        "_office_attendance_visit",
        "_office_attendance_recent",
        "_office_attendance_leaderboard",
        "office_attendance_handler",
    }
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    nodes = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in names
    ]
    assert {node.name for node in nodes} == names
    namespace = dict(extra_globals)
    exec(
        compile(
            ast.fix_missing_locations(
                ast.Module(body=nodes, type_ignores=[])),
            str(ENTRY),
            "exec",
        ),
        namespace,
    )
    return namespace["office_attendance_handler"]


def _run(value):
    return asyncio.run(value)


class _Clock:
    value = 1_800_000_000_000

    @classmethod
    def now(cls):
        return cls.value


class _Request:
    def __init__(self, data=None, method="POST", invalid_json=False):
        self.data = {} if data is None else data
        self.method = method
        self.invalid_json = invalid_json


def _clean_display_name(value, fallback="Guest"):
    clean = "".join(
        character for character in str(value or "")[:128]
        if character.isalnum() or character in (" ", ".", "_", "-")
    )
    clean = " ".join(clean.split()).strip(" ._-")[:32].strip()
    return clean or str(fallback or "Guest")[:32]


def _runtime():
    database = sqlite3.connect(":memory:")
    database.row_factory = sqlite3.Row
    database.executescript(MIGRATION.read_text(encoding="utf-8"))
    database.executescript(LIVE_MIGRATION.read_text(encoding="utf-8"))
    database.executescript(SCOPE_MIGRATION.read_text(encoding="utf-8"))
    state = {
        "account_bi": "",
        "account": None,
        "session_payloads": [],
        "next_id": 1000,
    }

    async def bounded_json_request(request):
        if request.invalid_json:
            raise ValueError("invalid_json")
        return request.data

    async def account_session(_env, _request, data):
        state["session_payloads"].append(data)
        return state["account_bi"], state["account"]

    async def d1_all(_env, sql, *params):
        return [
            dict(row) for row in database.execute(sql, params).fetchall()
        ]

    async def d1_run(_env, sql, *params):
        database.execute(sql, params)
        database.commit()

    def new_id():
        state["next_id"] += 1
        return f"{state['next_id']:032x}"

    def response(payload, status=200, **kwargs):
        return {"payload": payload, "status": status, **kwargs}

    handler = _load_handler({
        "Date": _Clock,
        "OFFICE_ATTENDANCE_FLOOR_LABELS": {
            "lobby": "Lobby",
            "marketing": "Marketing",
            "engineering": "Engineering",
            "infrastructure": "Infrastructure",
            "rooftop": "Rooftop",
        },
        "OFFICE_ATTENDANCE_LIVE_TTL_MS": 75_000,
        "_account_kind": lambda account: account.get("kind", ""),
        "_account_session_record": account_session,
        "bounded_json_request": bounded_json_request,
        "d1_all": d1_all,
        "d1_run": d1_run,
        "ensure_schema": lambda _env: _async_none(),
        "json_response": response,
        "method_name": lambda request: request.method,
        "new_world_peer_id": new_id,
        "re": re,
        "world_protocol": SimpleNamespace(
            clean_display_name=_clean_display_name),
    })
    return handler, database, state


async def _async_none():
    return None


def _sign_in(state, account_bi="a" * 64, name="alice"):
    state["account_bi"] = account_bi
    state["account"] = {
        "name": name,
        "status": "active",
        "kind": "user",
    }


def test_schema_and_idempotent_migration_store_only_bounded_visit_fields():
    migration = MIGRATION.read_text(encoding="utf-8")
    live_migration = LIVE_MIGRATION.read_text(encoding="utf-8")
    scope_migration = SCOPE_MIGRATION.read_text(encoding="utf-8")
    schema = SCHEMA.read_text(encoding="utf-8")
    for source in (migration + live_migration + scope_migration, schema):
        assert "CREATE TABLE IF NOT EXISTS world_office_attendance" in source
        assert "idx_world_office_attendance_open" in source
        assert "WHERE out_at IS NULL" in source
        assert "idx_world_office_attendance_recent" in source

    database = sqlite3.connect(":memory:")
    database.executescript(migration)
    database.executescript(migration)
    database.executescript(live_migration)
    database.executescript(scope_migration)
    columns = {
        row[1] for row in database.execute(
            "PRAGMA table_info(world_office_attendance)")
    }
    assert columns == {
        "visit_id", "account_bi", "account_name", "in_at", "out_at",
        "last_seen_at", "floor_id", "visit_scope",
    }
    assert not columns.intersection({
        "ip", "ip_address", "user_agent", "ua", "session", "session_token",
    })

    database.execute(
        "INSERT INTO world_office_attendance "
        "(visit_id,account_bi,account_name,in_at,out_at) "
        "VALUES (?,?,?,?,NULL)",
        ("1" * 32, "a" * 64, "alice", 100),
    )
    try:
        database.execute(
            "INSERT INTO world_office_attendance "
            "(visit_id,account_bi,account_name,in_at,out_at) "
            "VALUES (?,?,?,?,NULL)",
            ("2" * 32, "a" * 64, "alice", 101),
        )
    except sqlite3.IntegrityError:
        pass
    else:
        raise AssertionError("an account must have at most one open visit")


def test_lazy_schema_upgrades_office_columns_before_dependent_indexes():
    pre_alters = _literal_assignment(
        ENTRY, "SCHEMA_PRE_CREATE_ALTER_STATEMENTS"
    )
    schema_statements = _literal_assignment(SCHEMA, "SCHEMA_STATEMENTS")
    database = sqlite3.connect(":memory:")
    try:
        database.executescript(
            """
            CREATE TABLE world_office_attendance (
                visit_id TEXT PRIMARY KEY,
                account_bi TEXT NOT NULL,
                account_name TEXT NOT NULL,
                in_at INTEGER NOT NULL,
                out_at INTEGER
            );
            """
        )
        for statement in pre_alters:
            if "world_office_attendance" in statement:
                database.execute(statement)
        for statement in schema_statements:
            if "world_office_attendance" in statement:
                database.execute(statement)
        columns = {
            row[1]
            for row in database.execute("PRAGMA table_info(world_office_attendance)")
        }
        indexes = {
            row[1]
            for row in database.execute("PRAGMA index_list(world_office_attendance)")
        }
    finally:
        database.close()

    assert {"last_seen_at", "floor_id", "visit_scope"}.issubset(columns)
    assert {
        "idx_world_office_attendance_live",
        "idx_world_office_attendance_scope",
    }.issubset(indexes)


def test_get_returns_only_the_newest_twenty_bounded_public_visits():
    handler, database, _state = _runtime()
    for index in range(25):
        in_at = 1_700_000_000_000 + index
        database.execute(
            "INSERT INTO world_office_attendance "
            "(visit_id,account_bi,account_name,in_at,out_at) "
            "VALUES (?,?,?,?,?)",
            (
                f"{index + 1:032x}",
                f"{index + 1:064x}",
                "visitor-" + str(index),
                in_at,
                in_at + 1,
            ),
        )
    database.commit()

    response = _run(handler(None, _Request(method="GET")))
    assert response["status"] == 200
    assert response["cache_control"] == (
        "no-store, max-age=0, must-revalidate")
    visits = response["payload"]["visits"]
    assert response["payload"]["ok"] is True
    assert response["payload"]["asOfAt"] == _Clock.value
    assert len(visits) == 20
    assert [visit["inAt"] for visit in visits] == sorted(
        (visit["inAt"] for visit in visits), reverse=True)
    assert visits[0] == {
        "id": f"{25:032x}",
        "account": "visitor-24",
        "inAt": 1_700_000_000_024,
        "outAt": 1_700_000_000_025,
        "durationMs": 1,
        "floor": "",
    }
    assert all(
        set(visit) == {
            "id", "account", "inAt", "outAt", "durationMs",
            "floor",
        }
        for visit in visits
    )
    assert response["payload"]["leaderboard"] == []


def test_leaderboard_has_one_member_row_ranked_by_longest_office_stay():
    handler, database, _state = _runtime()
    database.executemany(
        "INSERT INTO world_office_attendance "
        "(visit_id,account_bi,account_name,in_at,out_at,last_seen_at,"
        "floor_id,visit_scope) VALUES (?,?,?,?,?,?,?,?)",
        [
            (
                "1" * 32, "a" * 64, "alice",
                _Clock.value - 600_000, _Clock.value - 300_000,
                _Clock.value - 300_000, "", "office",
            ),
            (
                "2" * 32, "a" * 64, "alice",
                _Clock.value - 120_000, None,
                _Clock.value, "engineering", "office",
            ),
            (
                "3" * 32, "b" * 64, "bob",
                _Clock.value - 500_000, _Clock.value - 100_000,
                _Clock.value - 100_000, "", "office",
            ),
            (
                "4" * 32, "c" * 64, "legacy-user",
                _Clock.value - 900_000, _Clock.value,
                _Clock.value, "", "legacy",
            ),
        ],
    )
    database.commit()

    payload = _run(handler(None, _Request(method="GET")))["payload"]

    assert payload["leaderboard"] == [
        {
            "account": "bob",
            "longestDurationMs": 400_000,
            "activeDurationMs": 0,
            "present": False,
            "floor": "",
        },
        {
            "account": "alice",
            "longestDurationMs": 300_000,
            "activeDurationMs": 120_000,
            "present": True,
            "floor": "Engineering",
        },
    ]


def test_repeated_in_opens_one_visit_and_out_closes_that_same_row_once():
    handler, database, state = _runtime()
    _sign_in(state, name="Alice<script>")

    first = _run(handler(None, _Request({
        "action": "in",
        "account": "forged-user",
        "floor": "engineering",
    })))
    _Clock.value += 500
    duplicate = _run(handler(None, _Request({"action": "IN"})))

    rows = database.execute(
        "SELECT * FROM world_office_attendance").fetchall()
    assert len(rows) == 1
    assert rows[0]["account_bi"] == "a" * 64
    assert rows[0]["account_name"] == "Alicescript"
    assert rows[0]["out_at"] is None
    assert rows[0]["floor_id"] == "engineering"
    assert first["payload"]["visits"][0]["id"] == (
        duplicate["payload"]["visits"][0]["id"])
    assert first["payload"]["visits"][0]["durationMs"] == 0
    assert duplicate["payload"]["visits"][0]["durationMs"] == 500
    assert state["session_payloads"][0]["account"] == "forged-user"

    _Clock.value += 500
    checked_out_at = _Clock.value
    checked_out = _run(handler(
        None, _Request({"action": "out", "account": "forged-user"})))
    _Clock.value += 500
    repeated_out = _run(handler(None, _Request({"action": "out"})))
    row = database.execute(
        "SELECT * FROM world_office_attendance").fetchone()
    assert row["out_at"] == checked_out_at
    assert checked_out["payload"]["visits"] == repeated_out["payload"]["visits"]
    assert checked_out["payload"]["visits"][0]["outAt"] == checked_out_at
    assert checked_out["payload"]["visits"][0]["durationMs"] == 1_000
    assert checked_out["payload"]["visits"][0]["floor"] == ""


def test_new_visit_id_closes_reloaded_page_at_its_last_heartbeat():
    handler, database, state = _runtime()
    _sign_in(state, name="Alice")
    first_id = "1" * 32
    second_id = "2" * 32

    _run(handler(None, _Request({
        "action": "in",
        "visitId": first_id,
        "floor": "engineering",
    })))
    _Clock.value += 30_000
    _run(handler(None, _Request({
        "action": "heartbeat",
        "visitId": first_id,
        "floor": "engineering",
    })))
    last_heartbeat = _Clock.value
    _Clock.value += 10_000
    response = _run(handler(None, _Request({
        "action": "in",
        "visitId": second_id,
        "floor": "lobby",
    })))

    rows = database.execute(
        "SELECT visit_id,in_at,out_at,last_seen_at "
        "FROM world_office_attendance ORDER BY in_at,visit_id"
    ).fetchall()
    assert len(rows) == 2
    assert rows[0]["visit_id"] == first_id
    assert rows[0]["out_at"] == last_heartbeat
    assert rows[1]["visit_id"] == second_id
    assert rows[1]["in_at"] == _Clock.value
    assert rows[1]["out_at"] is None
    assert response["payload"]["leaderboard"][0] == {
        "account": "Alice",
        "longestDurationMs": 30_000,
        "activeDurationMs": 0,
        "present": True,
        "floor": "Lobby",
    }

    # Delayed writes (including an IN retry) from the replaced page cannot
    # extend or close this visit.
    _Clock.value += 5_000
    _run(handler(None, _Request({
        "action": "in",
        "visitId": first_id,
        "floor": "rooftop",
    })))
    _run(handler(None, _Request({
        "action": "heartbeat",
        "visitId": first_id,
        "floor": "rooftop",
    })))
    _run(handler(None, _Request({
        "action": "out",
        "visitId": first_id,
    })))
    current = database.execute(
        "SELECT out_at,last_seen_at,floor_id "
        "FROM world_office_attendance WHERE visit_id=?",
        (second_id,),
    ).fetchone()
    assert current["out_at"] is None
    assert current["last_seen_at"] == rows[1]["last_seen_at"]
    assert current["floor_id"] == "lobby"


def test_stale_open_visit_is_closed_and_live_floor_tracks_heartbeat():
    handler, database, state = _runtime()
    _sign_in(state, name="Alice")

    entered = _run(handler(None, _Request({
        "action": "in",
        "floor": "engineering",
    })))
    assert entered["payload"]["visits"][0]["outAt"] is None
    assert entered["payload"]["visits"][0]["floor"] == "Engineering"

    _Clock.value += 30_000
    heartbeat = _run(handler(None, _Request({
        "action": "heartbeat",
        "floor": "infrastructure",
    })))
    assert heartbeat["payload"]["visits"][0]["floor"] == "Infrastructure"

    _Clock.value += 75_001
    stale = _run(handler(None, _Request(method="GET")))
    visit = stale["payload"]["visits"][0]
    assert visit["outAt"] == _Clock.value - 75_001
    assert visit["floor"] == ""
    assert database.execute(
        "SELECT COUNT(*) FROM world_office_attendance "
        "WHERE out_at IS NULL"
    ).fetchone()[0] == 0


def test_duration_is_server_timed_and_malformed_rows_are_bounded():
    handler, database, _state = _runtime()
    database.executemany(
        "INSERT INTO world_office_attendance "
        "(visit_id,account_bi,account_name,in_at,out_at) "
        "VALUES (?,?,?,?,?)",
        [
            (
                "1" * 32,
                "1" * 64,
                "closed",
                _Clock.value - 125_000,
                _Clock.value - 5_000,
            ),
            (
                "2" * 32,
                "2" * 64,
                "open",
                _Clock.value - 65_000,
                None,
            ),
            (
                "3" * 32,
                "3" * 64,
                "future-clock",
                _Clock.value + 30_000,
                None,
            ),
        ],
    )
    database.commit()

    response = _run(handler(None, _Request(method="GET")))
    visits = {
        visit["account"]: visit for visit in response["payload"]["visits"]
    }

    assert response["payload"]["asOfAt"] == _Clock.value
    assert visits["closed"]["durationMs"] == 120_000
    assert visits["open"]["durationMs"] == 65_000
    assert visits["future-clock"]["durationMs"] == 0


def test_only_active_user_sessions_can_punch_and_actions_are_validated():
    handler, database, state = _runtime()

    unauthenticated = _run(handler(
        None, _Request({"action": "in"})))
    assert unauthenticated["status"] == 401
    assert unauthenticated["payload"] == {"error": "login_required"}

    state["account_bi"] = "b" * 64
    state["account"] = {
        "name": "build-node",
        "status": "active",
        "kind": "node",
    }
    assert _run(handler(
        None, _Request({"action": "in"})))["status"] == 401

    _sign_in(state, account_bi="c" * 64, name="carol")
    invalid_action = _run(handler(
        None, _Request({"action": "arrive"})))
    assert invalid_action["status"] == 400
    assert invalid_action["payload"] == {"error": "invalid_action"}
    invalid_visit = _run(handler(None, _Request({
        "action": "in",
        "visitId": "not-a-visit",
    })))
    assert invalid_visit["status"] == 400
    assert invalid_visit["payload"] == {"error": "invalid_visit"}
    assert _run(handler(
        None, _Request(invalid_json=True)))["status"] == 400
    assert database.execute(
        "SELECT COUNT(*) FROM world_office_attendance").fetchone()[0] == 0


def test_different_accounts_can_hold_independent_open_visits():
    handler, database, state = _runtime()
    _sign_in(state, account_bi="d" * 64, name="dana")
    _run(handler(None, _Request({"action": "in"})))
    _Clock.value += 1
    _sign_in(state, account_bi="e" * 64, name="eli")
    response = _run(handler(None, _Request({"action": "in"})))

    assert database.execute(
        "SELECT COUNT(*) FROM world_office_attendance "
        "WHERE out_at IS NULL").fetchone()[0] == 2
    assert [visit["account"] for visit in response["payload"]["visits"]] == [
        "eli", "dana",
    ]


def test_route_methods_csrf_helper_and_privacy_boundary_are_explicit():
    assert '"/api/world/office/attendance"' in ENTRY_TEXT
    assert "office_attendance_handler(self.env, request)" in ENTRY_TEXT

    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    handler_node = next(
        node for node in tree.body
        if isinstance(node, ast.AsyncFunctionDef)
        and node.name == "office_attendance_handler"
    )
    source = ast.unparse(handler_node)
    assert "_account_session_record(env, request, data)" in source
    lowered = source.lower()
    for forbidden in (
        "cf-connecting-ip",
        "x-forwarded-for",
        "user-agent",
        "ip_address",
    ):
        assert forbidden not in lowered

    handler, _database, _state = _runtime()
    rejected = _run(handler(None, _Request(method="DELETE")))
    assert rejected["status"] == 405
    assert rejected["extra_headers"]["allow"] == "GET, POST"
    assert rejected["cache_control"] == (
        "no-store, max-age=0, must-revalidate")
