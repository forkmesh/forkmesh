"""Owner-authorized web merges pin and propagate the merged repository state.

An organization owner merges a pull request through one online mirror while
the source-of-truth desktop may be offline. The relay must then (1) admit the
mirror's node-signed post-merge refs state into the repository's pin history,
(2) treat that state as the current generation while the source stays silent
so the rest of the fleet converges on it, and (3) never let an unsigned or
stale attestation drive either step.
"""

import ast
import asyncio
import hashlib
import hmac
import json
from pathlib import Path
import re
import sqlite3
import sys

import pytest


ROOT = Path(__file__).resolve().parents[2]
WORKER = ROOT / "app"
ENTRY = WORKER / "src" / "entry.py"
ENTRY_TEXT = (
    # entry.py + its lazily-split domain modules
    (ENTRY.parent / "node_events_runtime.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + ENTRY.read_text(encoding="utf-8")
    + "\n\n\n"
)
ROUTE_HANDLERS_TEXT = (
    WORKER / "src" / "route_handlers.py"
).read_text(encoding="utf-8")
MERGE_HANDLER_TEXT = ROUTE_HANDLERS_TEXT[
    ROUTE_HANDLERS_TEXT.index("async def repository_pull_merge_handler"):
]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(WORKER / "src"))

import mirror_gateway as gateway  # noqa: E402


OID_A = "a" * 40
OID_B = "b" * 40
OID_C = "c" * 40
STATE = "1" * 64
SOURCE_STATE = "2" * 64
SIGNATURE = "s" * 86


def _entry_functions(*names, constants=("HTTPS_MIRROR_PULL_MUTATIONS",)):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    nodes = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in names
    ]
    assert {node.name for node in nodes} == set(names)
    assignments = [
        node for node in tree.body
        if isinstance(node, ast.Assign)
        and len(node.targets) == 1
        and isinstance(node.targets[0], ast.Name)
        and node.targets[0].id in set(constants)
    ]
    assert {node.targets[0].id for node in assignments} == set(constants)
    namespace = {
        "HTTPS_MIRROR_MERGE_BODY_MAX_BYTES": 8 * 1024,
        "HTTPS_MIRROR_MERGE_REQUEST_RE": re.compile(
            r"^[A-Za-z0-9_-]{12,80}$"),
        "HTTPS_MIRROR_MERGE_ATTESTATION_SIG_RE": re.compile(
            r"^[A-Za-z0-9_-]{86}$"),
        "HTTPS_MIRROR_MERGE_ATTESTATION_MAX_SKEW_MS": 5 * 60 * 1000,
        "STATE_PIN_HISTORY": 100,
        "MAX_NODE_NAME": 63,
        "clean_string": lambda value, maximum: str(value or "")[:maximum],
        "valid_node_name": lambda value: bool(
            re.fullmatch(r"[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?", value or "")),
        "json": json,
        "re": re,
        "hmac": hmac,
        "_https_mirror_public_context_cache_clear": lambda: None,
    }
    exec(compile(
        ast.fix_missing_locations(ast.Module(
            body=assignments + nodes, type_ignores=[])),
        str(ENTRY),
        "exec",
    ), namespace)
    return namespace


def merge_request():
    return {
        "schemaVersion": 1,
        "type": "forkmesh.pull-merge-v1",
        "pullNumber": 7,
        "requestId": "merge_request_0001",
        "expectedBaseOid": OID_A,
        "expectedHeadOid": OID_B,
        "expectedPullsOid": OID_C,
    }


def merged_result(**changes):
    value = {
        "ok": True,
        "status": "merged",
        "requestId": "merge_request_0001",
        "published": True,
        "baseBefore": OID_A,
        "head": OID_B,
        "pullsBefore": OID_C,
        "baseAfter": "d" * 40,
        "pullsAfter": "e" * 40,
    }
    value.update(changes)
    return value


def test_worker_result_carries_only_a_well_formed_attestation():
    ns = _entry_functions("_https_mirror_merge_result")
    validate = ns["_https_mirror_merge_result"]
    request = merge_request()
    plain = validate(merged_result(), request)
    assert plain["status"] == "merged"
    assert "state" not in plain and "stateSig" not in plain
    attested = validate(merged_result(
        state=STATE.upper(), stateTs="1700000000000", stateSig=SIGNATURE,
        sourceState=SOURCE_STATE), request)
    assert attested["state"] == STATE
    assert attested["stateTs"] == "1700000000000"
    assert attested["stateSig"] == SIGNATURE
    assert attested["sourceState"] == SOURCE_STATE
    # Malformed attestation pieces are dropped together, never half-kept.
    for broken in (
        {"state": "zz" * 32, "stateTs": "1700000000000", "stateSig": SIGNATURE},
        {"state": STATE, "stateTs": "soon", "stateSig": SIGNATURE},
        {"state": STATE, "stateTs": "1700000000000", "stateSig": "short"},
        {"state": STATE, "stateTs": "1700000000000"},
    ):
        checked = validate(merged_result(**broken), request)
        assert checked["status"] == "merged"
        assert not {"state", "stateTs", "stateSig"} & set(checked)
    # The reviewed OIDs stay pinned regardless of the attestation.
    assert validate(merged_result(
        head="f" * 40, state=STATE, stateTs="1700000000000",
        stateSig=SIGNATURE), request) is None


def test_public_merge_response_hides_pin_bookkeeping():
    ns = _entry_functions("_https_mirror_merge_response")
    captured = {}

    def json_response(value, status=200, cache_control=""):
        captured["value"] = value
        captured["status"] = status
        return value

    ns["json_response"] = json_response
    ns["_https_mirror_merge_response"](merged_result(
        state=STATE, stateTs="1700000000000", stateSig=SIGNATURE,
        sourceState=SOURCE_STATE))
    assert captured["status"] == 200
    assert captured["value"]["status"] == "merged"
    assert captured["value"]["baseAfter"] == "d" * 40
    assert not {"state", "stateTs", "stateSig", "sourceState"} & set(
        captured["value"])


def _d1(database):
    async def d1_run(_env, statement, *arguments):
        database.execute(statement, arguments)
        database.commit()

    async def d1_first(_env, statement, *arguments):
        row = database.execute(statement, arguments).fetchone()
        return dict(row) if row is not None else None

    return d1_run, d1_first


def test_owner_merge_pin_applies_only_while_the_source_stays_silent():
    ns = _entry_functions("_owner_merge_pin_state")
    database = sqlite3.connect(":memory:")
    database.row_factory = sqlite3.Row
    database.executescript((
        WORKER / "migrations" / "0070_repository_pull_merge_jobs.sql"
    ).read_text(encoding="utf-8"))
    d1_run, d1_first = _d1(database)
    ns["d1_run"], ns["d1_first"] = d1_run, d1_first
    ns["Date"] = type("Date", (), {"now": staticmethod(lambda: 10_000)})
    pin = ns["_owner_merge_pin_state"]

    def add(identifier, status, result, updated, expires=1_000_000):
        database.execute(
            """INSERT INTO repo_merge_jobs
                 (request_id,request_digest,repo_bi,actor_bi,pull_number,
                  selected_node,status,result,created_at,updated_at,expires_at)
               VALUES (?,?,?,?,?,?,?,?,?,?,?)""",
            (
                identifier, "d" * 64, "repo", "a" * 64, 7, "mirror9",
                status, json.dumps(result), updated, updated, expires,
            ),
        )
        database.commit()

    # No job at all: no override.
    assert asyncio.run(pin(None, "repo", SOURCE_STATE)) == ""
    add("older", "succeeded", merged_result(
        state="3" * 64, sourceState=SOURCE_STATE), 100)
    add("newest", "succeeded", merged_result(
        state=STATE, sourceState=SOURCE_STATE), 200)
    add("failed", "failed", {"ok": False, "status": "failed",
                             "error": "merge_conflict"}, 300)
    # The newest succeeded job wins while the source still shows the state
    # it had when that merge ran.
    assert asyncio.run(pin(None, "repo", SOURCE_STATE)) == STATE
    # Once the source publishes anything else — converged onto the merge or
    # moved on — the override retires by itself.
    assert asyncio.run(pin(None, "repo", STATE)) == ""
    assert asyncio.run(pin(None, "repo", "9" * 64)) == ""
    # A job recorded without a verified attestation never drives routing.
    add("unsigned", "succeeded", merged_result(sourceState=SOURCE_STATE), 400)
    assert asyncio.run(pin(None, "repo", SOURCE_STATE)) == ""
    # Expired rows are ignored even before the cleanup sweep removes them.
    add("expired", "succeeded", merged_result(
        state="4" * 64, sourceState=SOURCE_STATE), 500, expires=5_000)
    assert asyncio.run(pin(None, "repo", SOURCE_STATE)) == ""
    # Malformed source-state input fails closed.
    assert asyncio.run(pin(None, "repo", "")) == ""
    assert asyncio.run(pin(None, "", SOURCE_STATE)) == ""


def test_admit_owner_merge_state_verifies_the_endpoint_signature_first():
    ns = _entry_functions("_admit_owner_merge_state")
    database = sqlite3.connect(":memory:")
    database.row_factory = sqlite3.Row
    database.executescript(
        "CREATE TABLE repo_state_history (key_bi TEXT NOT NULL, "
        "state_hash TEXT NOT NULL, ts INTEGER NOT NULL, "
        "PRIMARY KEY (key_bi, state_hash))")
    d1_run, d1_first = _d1(database)
    ns["d1_run"], ns["d1_first"] = d1_run, d1_first
    ns["Date"] = type(
        "Date", (), {"now": staticmethod(lambda: 1_700_000_000_000)})
    verifications = []

    async def ed25519_verify(public_key, signature, canonical):
        verifications.append((public_key, signature, canonical))
        return public_key == "endpoint-key" and signature == SIGNATURE

    async def claimed(_env, node):
        return ["claimed-key"]

    ns["ed25519_verify"] = ed25519_verify
    ns["_claimed_node_signing_pubkeys"] = claimed
    admit = ns["_admit_owner_merge_state"]
    context = {"repo": "forkmesh", "repoBi": "repo-bi"}
    endpoint = {"node": "mirror9", "publicKey": "endpoint-key"}
    fresh = merged_result(
        state=STATE, stateTs="1700000000000", stateSig=SIGNATURE)
    assert asyncio.run(admit(None, context, endpoint, fresh)) == STATE
    expected_canonical = (
        "forkmesh-repostate-v1\nmirror9\nforkmesh\n" + STATE
        + "\n1700000000000").encode()
    assert verifications[0] == ("endpoint-key", SIGNATURE, expected_canonical)
    rows = database.execute(
        "SELECT key_bi,state_hash FROM repo_state_history").fetchall()
    assert [tuple(row) for row in rows] == [("repo-bi", STATE)]

    # A stale attestation is never admitted.
    stale = merged_result(
        state="5" * 64, stateTs="1600000000000", stateSig=SIGNATURE)
    assert asyncio.run(admit(None, context, endpoint, stale)) == ""
    # A signature the registered endpoint key (or any claimed node key)
    # does not verify is refused, and a receipt without attestation is inert.
    forged = merged_result(
        state="6" * 64, stateTs="1700000000000", stateSig="x" * 86)
    assert asyncio.run(admit(None, context, endpoint, forged)) == ""
    assert asyncio.run(admit(None, context, endpoint, merged_result())) == ""
    assert database.execute(
        "SELECT COUNT(*) FROM repo_state_history").fetchone()[0] == 1


def test_merge_handler_pins_notifies_fleet_and_strips_unverified_state():
    # Only a signature-verified attestation is persisted with the job row,
    # and a success wakes every mirror with the admitted digest as well as the
    # source node.
    assert "_admit_owner_merge_state(" in MERGE_HANDLER_TEXT
    assert 'notify_repo_mirrors(' in MERGE_HANDLER_TEXT
    assert '"commits",\n            admitted_state' in MERGE_HANDLER_TEXT
    assert "if admitted_state:\n            generation" not in MERGE_HANDLER_TEXT
    assert 'notify_repo_host(' in MERGE_HANDLER_TEXT
    assert '"mirror_merge"' in MERGE_HANDLER_TEXT
    strip = MERGE_HANDLER_TEXT.index(
        'for field in ("state", "stateTs", "stateSig"):')
    assert MERGE_HANDLER_TEXT.index("stored.pop(field, None)") > strip
    assert MERGE_HANDLER_TEXT.index('stored["state"] = admitted_state') > strip
    assert MERGE_HANDLER_TEXT.index('stored["sourceState"] = source_state') > strip
    assert MERGE_HANDLER_TEXT.index("encoded = json.dumps(\n        stored") > (
        MERGE_HANDLER_TEXT.index('stored["sourceState"]'))
    assert '"statePinned": bool(admitted_state)' in MERGE_HANDLER_TEXT


def test_routing_context_prefers_the_owner_merged_generation():
    context = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _https_mirror_public_context("):
        ENTRY_TEXT.index("\n\n\ndef _https_mirror_endpoint_projection")
    ]
    assert "_owner_merge_pin_state(" in context
    # The override joins both the admitted and the preferred pin sets, and
    # every member already serving it becomes a current routing node.
    assert "pins.add(override_state)" in context
    assert "current_pins.add(override_state)" in context
    assert "current_nodes.add(node)" in context
    assert '"sourceState": source_state' in context
    assert '"ownerMergeState": override_state' in context
    # The provenance check runs against the canonical record that routing
    # actually resolved, after the ordinary pin gate decided who may serve.
    assert context.index("canonical = source or target") < context.index(
        "_owner_merge_pin_state(")


def test_gateway_passes_executor_attestation_through_and_refreshes_it():
    executor_result = merged_result(
        generationReady=True, state=STATE, stateTs="1700000000000",
        stateSig=SIGNATURE)
    response = gateway.GatewayApplication._merge_response(executor_result)
    body = json.loads(response.body)
    assert body["state"] == STATE and body["stateSig"] == SIGNATURE
    assert body["stateTs"] == "1700000000000"
    # Garbage never reaches the relay as an attestation.
    response = gateway.GatewayApplication._merge_response(merged_result(
        generationReady=True, state="nope", stateTs="1", stateSig="x"))
    assert not {"state", "stateTs", "stateSig"} & set(json.loads(response.body))

    assert gateway._merge_attestation_stale(
        {"state": STATE, "stateTs": "1700000000000", "stateSig": SIGNATURE},
        1700000000000 + 30 * 1000) is False
    assert gateway._merge_attestation_stale(
        {"state": STATE, "stateTs": "1700000000000", "stateSig": SIGNATURE},
        1700000000000 + 10 * 60 * 1000) is True

    calls = []

    class Executor:
        def call(self, request, timeout=20):
            calls.append(request["action"])
            return merged_result(
                generationReady=True, state="7" * 64,
                stateTs="1700000900000", stateSig="r" * 86)

    import threading
    app = object.__new__(gateway.GatewayApplication)
    app.merge_executor = Executor()
    app._merge_lock = threading.Lock()
    app._merge_threads = {}
    app._merge_payload_digests = {}
    app.clock_ms = lambda: 1700000900000
    cached = merged_result(
        state=STATE, stateTs="1700000000000", stateSig=SIGNATURE)
    app._merge_results = {"merge_request_0001": cached}
    body = json.dumps({
        "schemaVersion": 1, "type": "forkmesh.pull-merge-v1",
        "pullNumber": 7, "requestId": "merge_request_0001",
        "expectedBaseOid": OID_A, "expectedHeadOid": OID_B,
        "expectedPullsOid": OID_C,
    }).encode()
    served = app._dispatch_merge_pull("mirror9", "forkmesh", body)
    payload = json.loads(served.body)
    assert calls == ["status"]
    assert payload["state"] == "7" * 64 and payload["stateTs"] == "1700000900000"
    assert payload["baseAfter"] == "d" * 40 and payload["published"] is True
    # A fresh attestation is answered from cache without another executor call.
    app._dispatch_merge_pull("mirror9", "forkmesh", body)
    assert calls == ["status"]


def test_a_merge_on_a_hosted_repository_is_recorded_like_a_push():
    success = MERGE_HANDLER_TEXT.index('    if terminal == "succeeded":\n        try:')
    recorded = MERGE_HANDLER_TEXT.index(
        'await record_hosted_push_state(\n'
        '                env, context["owner"], context["repo"], endpoint,\n'
        '                pusher=actor)')
    assert success < recorded < MERGE_HANDLER_TEXT.index("notify_repo_host(")
