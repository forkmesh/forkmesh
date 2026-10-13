#!/usr/bin/env python3
"""System Capacity discovers Durable Objects and counts what they relay."""

import ast
import asyncio
import importlib.util
import re
import tomllib
from pathlib import Path
from types import SimpleNamespace
from _maintenance_stubs import with_maintenance_off
from world_scene_source import world_scene_source


ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
WORLD_DURABLE_OBJECTS = (
    ROOT.parent / "world" / "src" / "world_durable_objects.py"
)
SCHEMA = ROOT / "src" / "schema.py"
WRANGLER = ROOT / "wrangler.toml"
MIGRATION = ROOT / "migrations" / "0083_durable_object_traffic.sql"
WORLD_APP = ROOT.parent / "world" / "public" / "world" / "world.js"
WORLD_SCENE = ROOT.parent / "world" / "public" / "world" / "world-scene.js"
ENTRY_TEXT = ENTRY.read_text(encoding="utf-8")
ENTRY_AST = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
WORLD_DURABLE_OBJECTS_AST = ast.parse(
    WORLD_DURABLE_OBJECTS.read_text(encoding="utf-8"),
    filename=str(WORLD_DURABLE_OBJECTS),
)
INFRASTRUCTURE = ROOT.parent / "world" / "src" / "world_infrastructure.py"
INFRASTRUCTURE_SPEC = importlib.util.spec_from_file_location(
    "forkmesh_world_infrastructure",
    INFRASTRUCTURE,
)
world_infrastructure = importlib.util.module_from_spec(INFRASTRUCTURE_SPEC)
INFRASTRUCTURE_SPEC.loader.exec_module(world_infrastructure)


def _load(names, namespace):
    for name in names:
        node = next(
            node for node in ENTRY_AST.body
            if isinstance(
                node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef))
            and node.name == name
        )
        module = ast.fix_missing_locations(
            ast.Module(body=[node], type_ignores=[]))
        exec(compile(module, str(ENTRY), "exec"), with_maintenance_off(namespace))
    return namespace


def _traffic_namespace(now=1_000_000, flush_bytes=262_144):
    writes = []

    async def d1_run(_env, sql, *args):
        writes.append((sql, args))
        return None

    namespace = _load(
        ("durable_object_traffic_note", "durable_object_traffic_flush"),
        {
            "Date": SimpleNamespace(now=lambda: now),
            "d1_run": d1_run,
            "DURABLE_OBJECT_BINDING_RE": re.compile(r"[A-Z][A-Z0-9_]{0,63}"),
            "DURABLE_OBJECT_TRAFFIC_FLUSH_MS": 60_000,
            "DURABLE_OBJECT_TRAFFIC_FLUSH_BYTES": flush_bytes,
            "DURABLE_OBJECT_TRAFFIC_MAX": 9_007_199_254_740_991,
        },
    )
    return namespace, writes


def test_bindings_are_discovered_from_the_environment_not_a_hardcoded_list():
    namespace = _load(
        ("durable_object_bindings",),
        {
            "Object": SimpleNamespace(keys=lambda env: list(vars(env))),
            "re": re,
            "DURABLE_OBJECT_BINDING_RE": re.compile(r"[A-Z][A-Z0-9_]{0,63}"),
        },
    )
    namespace_stub = SimpleNamespace(
        idFromName=lambda name: name, newUniqueId=lambda: "id", get=lambda _id: None)
    env = SimpleNamespace(
        FORKMESH_WORLD=namespace_stub,
        # A class bound after this code was written is still reported.
        FORKMESH_FUTURE_THING=namespace_stub,
        # Neither D1, KV, AI, assets, nor plain string vars are namespaces.
        DB=SimpleNamespace(prepare=lambda sql: None, batch=lambda rows: None),
        SESSIONS=SimpleNamespace(get=lambda key: None, put=lambda k, v: None),
        AI=SimpleNamespace(run=lambda *_args: None),
        ASSETS=SimpleNamespace(fetch=lambda request: None),
        NODE_NAME="forkmesh-mainnode",
        lowercase_binding=namespace_stub,
        MISSING=None,
    )

    assert namespace["durable_object_bindings"](env) == [
        "FORKMESH_FUTURE_THING", "FORKMESH_WORLD"]
    # A hostile or unavailable environment degrades to "nothing discovered".
    assert namespace["durable_object_bindings"](object()) == []


def test_bindings_are_still_discovered_when_the_environment_hides_its_keys():
    """A runtime whose env exposes nothing enumerable still reports objects.

    `Object.keys` returning an empty list previously meant "no Durable
    Objects", which silently reduced System Capacity to the two connections
    the browser can observe by itself.
    """
    namespace_stub = SimpleNamespace(
        idFromName=lambda name: name, newUniqueId=lambda: "id")

    class HiddenEnv:
        FORKMESH_WORLD = namespace_stub
        FORKMESH_NODES = namespace_stub
        NODE_NAME = "forkmesh-mainnode"

    namespace = _load(
        ("durable_object_bindings",),
        {
            "Object": SimpleNamespace(
                keys=lambda _env: [],
                getOwnPropertyNames=lambda _env: ["FORKMESH_NODES"],
            ),
            "re": re,
            "DURABLE_OBJECT_BINDING_RE": re.compile(r"[A-Z][A-Z0-9_]{0,63}"),
        },
    )

    # Own-property names and attribute discovery are merged, so a binding that
    # is not an own enumerable key of the environment is still reported.
    assert namespace["durable_object_bindings"](HiddenEnv()) == [
        "FORKMESH_NODES", "FORKMESH_WORLD"]


def test_binding_labels_are_derived_from_the_binding_name():
    namespace = _load(("durable_object_label",), {})
    label = namespace["durable_object_label"]

    assert label("FORKMESH_WORLD") == "World"
    assert label("FORKMESH_OFFICE_ROOM") == "Office room"
    assert label("FORKMESH_FUTURE_THING") == "Future thing"
    assert label("") == ""


def test_relayed_bytes_are_batched_into_one_bounded_upsert():
    namespace, writes = _traffic_namespace()
    note = namespace["durable_object_traffic_note"]
    flush = namespace["durable_object_traffic_flush"]
    room = SimpleNamespace(traffic_binding="FORKMESH_MAINNODE_ROOM",
                           env=object())

    note(room, bytes_in=120, messages=1)
    note(room, bytes_out=480)
    # The first frame after a wake-up banks immediately, so an object that
    # hibernates again straight away still reports what it relayed.
    assert asyncio.run(flush(room)) is True
    sql, args = writes[0]
    assert "INSERT INTO durable_object_traffic" in sql
    assert "ON CONFLICT(binding) DO UPDATE SET" in sql
    assert sql.count("MIN(") == 3
    assert args[:5] == ("FORKMESH_MAINNODE_ROOM", 120, 480, 1, 1_000_000)
    assert args[5:] == (9_007_199_254_740_991,) * 3

    # Flushed counts are not double-reported on the next flush.
    assert asyncio.run(flush(room, force=True)) is False
    assert len(writes) == 1

    # A small, recent window is then kept in memory instead of writing every
    # frame; closing a socket banks it before the object hibernates.
    note(room, bytes_in=64, messages=1)
    assert asyncio.run(flush(room)) is False
    assert len(writes) == 1
    assert asyncio.run(flush(room, force=True)) is True
    assert writes[1][1][:4] == ("FORKMESH_MAINNODE_ROOM", 64, 0, 1)


def test_a_large_batch_of_bytes_flushes_without_waiting_for_the_timer():
    namespace, writes = _traffic_namespace(flush_bytes=1_000)
    namespace["durable_object_traffic_note"](
        room := SimpleNamespace(traffic_binding="FORKMESH_WORLD", env=object()),
        bytes_out=4_096,
        messages=2,
    )

    assert asyncio.run(namespace["durable_object_traffic_flush"](room)) is True
    assert writes[0][1][:4] == ("FORKMESH_WORLD", 0, 4_096, 2)


def test_traffic_accounting_never_breaks_a_live_relay():
    namespace, writes = _traffic_namespace()
    note = namespace["durable_object_traffic_note"]
    flush = namespace["durable_object_traffic_flush"]

    # An unnamed object writes nothing; a database failure is swallowed.
    unnamed = SimpleNamespace(env=object())
    note(unnamed, bytes_in=10, messages=1)
    assert asyncio.run(flush(unnamed, force=True)) is False

    async def failing_d1_run(*_args):
        raise RuntimeError("no such table: durable_object_traffic")

    namespace["d1_run"] = failing_d1_run
    broken = SimpleNamespace(traffic_binding="FORKMESH_NODES", env=object())
    note(broken, bytes_in=10, messages=1)
    assert asyncio.run(flush(broken, force=True)) is False
    assert writes == []


def test_capacity_reports_every_binding_with_its_relayed_totals():
    queries = []

    async def d1_all(_env, sql, *args):
        queries.append((sql, args))
        return [
            {"binding": "FORKMESH_WORLD", "bytes_in": 400, "bytes_out": 1_600,
             "messages": 12, "updated_at": 1_700},
            {"binding": "RETIRED_BINDING", "bytes_in": 5, "bytes_out": 5,
             "messages": 1, "updated_at": 9},
            "not-a-row",
        ]

    namespace = _load(
        ("_world_durable_objects",),
        {
            "d1_all": d1_all,
            "_world_infrastructure_module": lambda: world_infrastructure,
            "durable_object_bindings": lambda _env: [
                "FORKMESH_NODES", "FORKMESH_WORLD"],
            "durable_object_label": lambda binding: binding.title(),
            "WORLD_SYSTEM_CAPACITY_MAX_DURABLE_OBJECTS": 32,
            "DURABLE_OBJECT_TRAFFIC_MAX": 9_007_199_254_740_991,
        },
    )
    result = asyncio.run(namespace["_world_durable_objects"](object()))

    # Discovered bindings are always listed; a binding with no traffic yet
    # reports zero rather than disappearing, and a stale counter row for a
    # binding that no longer exists is not reported at all.
    assert [record["binding"] for record in result] == [
        "FORKMESH_NODES", "FORKMESH_WORLD"]
    assert result[0]["bytesTotal"] == 0
    assert result[1]["bytesIn"] == 400
    assert result[1]["bytesOut"] == 1_600
    assert result[1]["bytesTotal"] == 2_000
    assert result[1]["messages"] == 12
    assert len(queries) == 1
    assert "FROM durable_object_traffic" in queries[0][0]
    assert queries[0][1] == (32,)


def test_capacity_still_lists_bindings_when_the_counter_table_is_missing():
    async def d1_all(*_args):
        raise RuntimeError("no such table: durable_object_traffic")

    namespace = _load(
        ("_world_durable_objects",),
        {
            "d1_all": d1_all,
            "_world_infrastructure_module": lambda: world_infrastructure,
            "durable_object_bindings": lambda _env: ["FORKMESH_WORLD"],
            "durable_object_label": lambda binding: binding,
            "WORLD_SYSTEM_CAPACITY_MAX_DURABLE_OBJECTS": 32,
            "DURABLE_OBJECT_TRAFFIC_MAX": 9_007_199_254_740_991,
        },
    )
    result = asyncio.run(namespace["_world_durable_objects"](object()))

    assert result == [{
        "id": "FORKMESH_WORLD",
        "binding": "FORKMESH_WORLD",
        "name": "FORKMESH_WORLD",
        "bytesIn": 0,
        "bytesOut": 0,
        "bytesTotal": 0,
        "messages": 0,
        "updatedAt": 0,
    }]


def test_every_bound_durable_object_class_accounts_for_its_traffic():
    config = tomllib.loads(WRANGLER.read_text(encoding="utf-8"))
    bindings = {
        entry["name"]: entry["class_name"]
        for entry in config["durable_objects"]["bindings"]
    }
    accounted = {
        node.name: value.value
        for tree in (ENTRY_AST, WORLD_DURABLE_OBJECTS_AST)
        for node in tree.body
        if isinstance(node, ast.ClassDef)
        for statement in node.body
        if isinstance(statement, ast.Assign)
        and any(
            isinstance(target, ast.Name) and target.id == "traffic_binding"
            for target in statement.targets
        )
        for value in [statement.value]
        if isinstance(value, ast.Constant)
    }

    # Every bound class declares the binding its byte counts belong to, so a
    # newly bound Durable Object cannot silently report zero traffic forever.
    assert {accounted[class_name] for class_name in bindings.values()} == set(
        bindings)


def test_counter_table_is_content_free_in_schema_and_migration():
    schema = SCHEMA.read_text(encoding="utf-8")
    migration = MIGRATION.read_text(encoding="utf-8")

    for source in (schema, migration):
        assert "CREATE TABLE IF NOT EXISTS durable_object_traffic" in source
        assert "binding TEXT PRIMARY KEY" in source
        assert "bytes_in INTEGER NOT NULL DEFAULT 0" in source
        assert "bytes_out INTEGER NOT NULL DEFAULT 0" in source
        # No room key, account, instance id, or payload column exists.
        for forbidden in ("room", "account", "instance", "data", "peer"):
            assert forbidden not in source[
                source.index("durable_object_traffic"):
                source.index("durable_object_traffic") + 600]


def test_world_client_renders_discovered_objects_and_their_traffic():
    app = WORLD_APP.read_text(encoding="utf-8")
    scene = world_scene_source()

    assert "ticket?.systemCapacity?.durableObjects" in app
    assert "this.systemCapacityDurableObjects" in app
    assert "systemCapacityLiveConnections()" in app
    # Live connection counts are keyed by binding so they merge onto the
    # server's discovered records instead of duplicating them.
    assert 'live.set("FORKMESH_WORLD"' in app
    assert 'live.set("FORKMESH_MAINNODE_ROOM"' in app
    assert "function formatCapacityBytes" in scene
    assert "record?.bytesTotal" in scene
    assert "relayed" in scene
    assert "bytesRelayed" in scene
    assert "Math.log1p(record.bytes)" in scene
