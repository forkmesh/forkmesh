"""Worker authorization and signed mirror-gateway pull merge contracts."""

import ast
import asyncio
import hashlib
import hmac
import json
from pathlib import Path
import re
import sqlite3
import sys
import threading
import time

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

import edge_routing  # noqa: E402
import mirror_gateway as gateway  # noqa: E402
import urls  # noqa: E402


OID_A = "a" * 40
OID_B = "b" * 40
OID_C = "c" * 40


def _entry_functions(*names, constants=("HTTPS_MIRROR_PULL_MUTATIONS",)):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    nodes = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in names
    ]
    assert {node.name for node in nodes} == set(names)
    # Pull the real mutation profile table in from entry.py rather than
    # restating it here, so a profile change cannot pass this test by accident.
    assignments = [
        node for node in tree.body
        if isinstance(node, ast.Assign)
        and len(node.targets) == 1
        and isinstance(node.targets[0], ast.Name)
        and node.targets[0].id in set(constants)
    ]
    assert {node.targets[0].id for node in assignments} == set(constants)
    nodes = assignments + nodes
    namespace = {
        "HTTPS_MIRROR_MERGE_BODY_MAX_BYTES": 8 * 1024,
        "HTTPS_MIRROR_MERGE_REQUEST_RE": re.compile(
            r"^[A-Za-z0-9_-]{12,80}$"),
        "HTTPS_MIRROR_MERGE_JOB_REPO_LIMIT": 256,
        "HTTPS_MIRROR_MERGE_JOB_GLOBAL_LIMIT": 10_000,
        "HTTPS_MIRROR_MERGE_JOB_CLEANUP_BATCH": 512,
        "clean_string": lambda value, maximum: str(value or "")[:maximum],
        "json": json,
        "re": re,
        "hmac": hmac,
    }
    exec(compile(
        ast.fix_missing_locations(ast.Module(body=nodes, type_ignores=[])),
        str(ENTRY),
        "exec",
    ), namespace)
    return namespace


def merge_body(**changes):
    value = {
        "schemaVersion": 1,
        "type": "forkmesh.pull-merge-v1",
        "pullNumber": 7,
        "requestId": "merge_request_0001",
        "expectedBaseOid": OID_A,
        "expectedHeadOid": OID_B,
        "expectedPullsOid": OID_C,
    }
    value.update(changes)
    return json.dumps(value, separators=(",", ":")).encode()


def test_gateway_parser_binds_exact_route_number_and_oids():
    parsed = gateway.GatewayApplication._merge_request(
        "mirror2", "forkmesh", merge_body())
    assert parsed == {
        "schemaVersion": 1,
        "type": "forkmesh.pull-merge-executor-v1",
        "owner": "mirror2",
        "repository": "forkmesh",
        "pullNumber": 7,
        "requestId": "merge_request_0001",
        "expectedBaseOid": OID_A,
        "expectedHeadOid": OID_B,
        "expectedPullsOid": OID_C,
    }
    cleanup = gateway.GatewayApplication._merge_request(
        "mirror2", "forkmesh", merge_body(deleteHead=True))
    assert cleanup["deleteHead"] is True
    with pytest.raises(gateway.GatewayError):
        gateway.GatewayApplication._merge_request(
            "mirror2", "forkmesh", merge_body(pullNumber=0))
    with pytest.raises(gateway.GatewayError):
        gateway.GatewayApplication._merge_request(
            "mirror2", "forkmesh", merge_body(expectedHeadOid="main"))
    duplicate = (
        merge_body()[:-1]
        + b',"requestId":"merge_request_0002"}'
    )
    with pytest.raises(gateway.GatewayError):
        gateway.GatewayApplication._merge_request(
            "mirror2", "forkmesh", duplicate)


def test_gateway_job_is_async_then_reports_only_published_node_confirmation():
    class Executor:
        def call(self, request, timeout=20):
            del timeout
            action = request["action"]
            if action == "status":
                return {
                    "ok": True, "status": "missing",
                    "requestId": request["requestId"],
                    "generationReady": False,
                }
            if action == "execute":
                return {
                    "ok": True, "status": "merged",
                    "requestId": request["requestId"],
                    "baseBefore": OID_A, "head": OID_B,
                    "pullsBefore": OID_C, "baseAfter": OID_B,
                    "pullsAfter": "d" * 40, "generationReady": True,
                }
            assert action == "register"
            return {
                "ok": True, "status": "registered",
                "requestId": request["requestId"],
            }

    app = object.__new__(gateway.GatewayApplication)
    app.merge_executor = Executor()
    app._merge_lock = threading.Lock()
    app._merge_threads = {}
    app._merge_results = {}
    app._merge_payload_digests = {}
    app.reload_from_disk = lambda **_: None

    accepted = app._dispatch_merge_pull(
        "mirror2", "forkmesh", merge_body())
    assert accepted.status == 202
    assert json.loads(accepted.body)["status"] == "processing"

    deadline = time.time() + 2
    while time.time() < deadline:
        completed = app._dispatch_merge_pull(
            "mirror2", "forkmesh", merge_body())
        if completed.status == 200:
            break
        time.sleep(0.01)
    assert completed.status == 200
    body = json.loads(completed.body)
    assert body["status"] == "merged"
    assert body["published"] is True
    assert "node" not in body and "origin" not in body

    with pytest.raises(gateway.GatewayError, match="already used"):
        app._dispatch_merge_pull(
            "mirror2", "forkmesh",
            merge_body(expectedHeadOid="e" * 40))


def test_worker_body_rejects_duplicates_and_result_pins_reviewed_oids():
    ns = _entry_functions(
        "_https_mirror_merge_body", "_https_mirror_merge_result")
    parse = ns["_https_mirror_merge_body"]
    validate = ns["_https_mirror_merge_result"]
    parsed = parse(merge_body().decode(), 7)
    assert parsed and "sessionToken" not in parsed
    cleanup = parse(merge_body(deleteHead=True).decode(), 7)
    assert cleanup["deleteHead"] is True
    assert parse(
        merge_body().decode()[:-1]
        + ',"requestId":"merge_request_0002"}',
        7,
    ) is None
    assert parse(merge_body(pullNumber=8).decode(), 7) is None
    merged = {
        "ok": True,
        "status": "merged",
        "requestId": parsed["requestId"],
        "published": True,
        "baseBefore": OID_A,
        "head": OID_B,
        "pullsBefore": OID_C,
        "baseAfter": OID_B,
        "pullsAfter": "d" * 40,
    }
    assert validate(merged, parsed)["status"] == "merged"
    merged["head"] = "e" * 40
    assert validate(merged, parsed) is None


def test_owner_review_waiver_travels_merge_only_and_stays_absent_otherwise():
    # A repository owner may merge without the independent-review gate. The
    # flag has to survive the Worker body check and the signed gateway parser
    # unchanged, and it must be absent from every other request so a mirror
    # that predates the waiver keeps seeing the shape it already accepts.
    ns = _entry_functions("_https_mirror_merge_body")
    parse = ns["_https_mirror_merge_body"]
    assert "overrideReview" not in parse(merge_body().decode(), 7)
    waived = parse(merge_body(overrideReview=True).decode(), 7)
    assert waived["overrideReview"] is True
    # Only a real boolean, and only a waiver: "false" would round-trip as an
    # unknown field to an older mirror for no gain.
    assert parse(merge_body(overrideReview="yes").decode(), 7) is None
    assert "overrideReview" not in parse(
        merge_body(overrideReview=False).decode(), 7)
    # Updating a branch from its base has no review gate to waive.
    assert parse(update_body(overrideReview=True).decode(), 7, "update") is None

    parsed = gateway.GatewayApplication._merge_request(
        "mirror2", "forkmesh", merge_body(overrideReview=True))
    assert parsed["overrideReview"] is True
    assert "overrideReview" not in gateway.GatewayApplication._merge_request(
        "mirror2", "forkmesh", merge_body())
    with pytest.raises(gateway.GatewayError):
        gateway.GatewayApplication._merge_request(
            "mirror2", "forkmesh", merge_body(overrideReview="yes"))
    with pytest.raises(gateway.GatewayError):
        gateway.GatewayApplication._merge_request(
            "mirror2", "forkmesh",
            update_body(overrideReview=True), "update-pull-branch")
    # The waiver is only as safe as the owner check in front of it, so every
    # audit line the handler writes says whether it was asked for.
    assert 'override_review = bool(data.get("overrideReview"))' in (
        MERGE_HANDLER_TEXT)
    # denied, requested, and the terminal outcome all carry it.
    assert MERGE_HANDLER_TEXT.count('"overrideReview": override_review') == 3


def test_worker_route_auth_idempotency_and_no_failover_contracts():
    assert urls.REPO_PULL_MERGE_RE.fullmatch(
        "/api/repo/forkmesh/forkmesh/pulls/7/merge")
    handler = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _repo_merge_jobs_make_room"):
        ENTRY_TEXT.index("\n\n\nasync def _https_mirror_proxy")
    ] + "\n\n\n" + MERGE_HANDLER_TEXT
    for required in (
        "_account_session_record",
        "_repository_owner_can_merge_repo",
        "repo_merge_jobs",
        "request_id_reused",
        "selected_node",
        "_audit_sensitive_action",
    ):
        assert required in handler
    assert "merge_node_unavailable" in handler
    # Answers the browser waits out by retrying are marked expected-degraded
    # so they add no error_log rows; no_merge_executor stays a logged fault.
    assert ('None if unavailable == "no_merge_executor"\n'
            '                    else EXPECTED_DEGRADED_HEADERS') in handler
    assert handler.count("extra_headers=EXPECTED_DEGRADED_HEADERS") >= 3
    assert "An uncertain transport result never fails over" in handler
    assert "_https_mirror_merge_proxy(" in handler
    assert 'notify_repo_host(' in handler
    assert '"mirror_merge"' in handler


def test_merge_selection_asks_for_the_mirror_that_runs_the_executor():
    # A mirror without an operator-configured executor advertises neither pull
    # mutation, so routing them like an ordinary read left the one capable
    # node outside the bounded failover window and the owner read
    # "no online mirror can merge this pull request right now".
    profiles = _entry_functions()["HTTPS_MIRROR_PULL_MUTATIONS"]
    operations = {profile["operation"] for profile in profiles.values()}
    assert operations == edge_routing.MERGE_EXECUTOR_OPERATIONS
    # The selection holds those advertised nodes inside the window it then
    # verifies with the per-repository signed proof.
    candidates = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _https_mirror_candidates"):
        ENTRY_TEXT.index("async def _https_mirror_request_candidates")
    ]
    assert "https_routing.MERGE_EXECUTOR_OPERATIONS" in candidates
    assert "_https_mirror_advertised_operation_nodes(rows, operation)" in (
        candidates)
    assert "prefer=prefer" in candidates
    assert "https_routing.cap_failover(selected, prefer=prefer)" in candidates
    # Both the first selection and every poll of the recorded node route on
    # the mutation being performed, not on the read profile.
    assert MERGE_HANDLER_TEXT.count('operation=profile["operation"]') == 2
    selection = MERGE_HANDLER_TEXT[
        :MERGE_HANDLER_TEXT.index("merge_node_unavailable")]
    assert '_pull_mutation_node(\n            env, context, candidates, profile["operation"])' in selection
    picker = ROUTE_HANDLERS_TEXT[
        ROUTE_HANDLERS_TEXT.index("async def _pull_mutation_node"):
        ROUTE_HANDLERS_TEXT.index("async def repository_pull_merge_handler")]
    assert "_https_mirror_repository_proof(" in picker


def test_mirror_merge_authorization_is_org_owner_only():
    helper = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _organization_owner_can_merge_repo"):
        ENTRY_TEXT.index("async def _repository_owner_can_merge_repo")
    ]
    assert "_org_role(env, org_bi, actor) == \"owner\"" in helper
    assert "FROM org_repos" in helper
    assert "alias_owner == canonical_owner" in helper
    assert "_org_write_allowed" not in helper
    assert "_account_owns_node" not in helper


def test_merge_capability_is_explicit_signed_and_schema_bounded():
    assert "merge-pull" in edge_routing.PUBLIC_OPERATIONS
    assert "merge-pull" not in gateway.DEFAULT_PUBLIC_OPERATIONS
    target = edge_routing.masked_target_url(
        "https://mirror.example.test",
        "mirror2",
        "forkmesh",
        "merge-pull",
    )
    assert target.endswith(
        "/v1/repositories/mirror2/forkmesh/merge-pull")
    source = (ROOT / "tools" / "mirror_gateway.py").read_text()
    assert "merge-pull requires an operator-owned mergeExecutorCommand" in source
    assert "self._authorize(method, target, headers, body)" in source
    schema = (WORKER / "src" / "schema.py").read_text()
    migration = (
        WORKER / "migrations" / "0070_repository_pull_merge_jobs.sql"
    ).read_text()
    for text in (schema, migration):
        assert "CREATE TABLE IF NOT EXISTS repo_merge_jobs" in text
        assert "request_id TEXT PRIMARY KEY" in text
        assert "selected_node TEXT NOT NULL" in text
        assert "expires_at INTEGER NOT NULL" in text
        assert "idx_repo_merge_jobs_expiry" in text
        assert "repo_bi, status, updated_at" in text
        assert "trg_repo_merge_jobs_repo_bound" in text
        assert "trg_repo_merge_jobs_global_bound" in text
        assert "repository bytes" in text.lower()
    handler = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _repo_merge_jobs_make_room"):
        ENTRY_TEXT.index("\n\n\nasync def _https_mirror_proxy")
    ] + "\n\n\n" + MERGE_HANDLER_TEXT
    for policy in (
        "HTTPS_MIRROR_MERGE_JOB_RETENTION_MS",
        "HTTPS_MIRROR_MERGE_JOB_REPO_LIMIT",
        "HTTPS_MIRROR_MERGE_JOB_GLOBAL_LIMIT",
        "_repo_merge_jobs_make_room",
        "merge_queue_full",
    ):
        assert policy in handler


def test_merge_job_cleanup_enforces_expiry_repo_and_global_hard_bounds():
    ns = _entry_functions("_repo_merge_jobs_make_room")
    ns["HTTPS_MIRROR_MERGE_JOB_REPO_LIMIT"] = 3
    ns["HTTPS_MIRROR_MERGE_JOB_GLOBAL_LIMIT"] = 5
    ns["HTTPS_MIRROR_MERGE_JOB_CLEANUP_BATCH"] = 8
    database = sqlite3.connect(":memory:")
    database.row_factory = sqlite3.Row
    migration = (
        WORKER / "migrations" / "0070_repository_pull_merge_jobs.sql"
    ).read_text(encoding="utf-8")
    database.executescript(migration)

    async def d1_run(_env, statement, *arguments):
        database.execute(statement, arguments)
        database.commit()

    async def d1_first(_env, statement, *arguments):
        row = database.execute(statement, arguments).fetchone()
        return dict(row) if row is not None else None

    ns["d1_run"] = d1_run
    ns["d1_first"] = d1_first
    make_room = ns["_repo_merge_jobs_make_room"]

    def add(identifier, repo, status, created, expires):
        database.execute(
            """INSERT INTO repo_merge_jobs
                 (request_id,request_digest,repo_bi,actor_bi,pull_number,
                  selected_node,status,result,created_at,updated_at,expires_at)
               VALUES (?,?,?,?,?,?,?,?,?,?,?)""",
            (
                identifier, "d" * 64, repo, "a" * 64, 1, "mirror2",
                status, "", created, created, expires,
            ),
        )
        database.commit()

    add("expired", "repo-a", "failed", 1, 2)
    add("old-terminal", "repo-a", "succeeded", 3, 10_000)
    add("active-a", "repo-a", "requested", 4, 10_000)
    assert asyncio.run(make_room(None, "repo-a", 100)) is True
    ids = {
        row[0] for row in database.execute(
            "SELECT request_id FROM repo_merge_jobs")
    }
    assert "expired" not in ids

    add("active-b", "repo-a", "requested", 5, 10_000)
    # At the per-repo ceiling, the oldest terminal row is evicted, while both
    # in-flight rows survive and one slot becomes available.
    assert asyncio.run(make_room(None, "repo-a", 100)) is True
    ids = {
        row[0] for row in database.execute(
            "SELECT request_id FROM repo_merge_jobs")
    }
    assert "old-terminal" not in ids
    assert {"active-a", "active-b"} <= ids

    add("active-c", "repo-a", "requested", 6, 10_000)
    # A full repo made entirely of in-flight work fails closed instead of
    # evicting an uncertain merge.
    assert asyncio.run(make_room(None, "repo-a", 100)) is False
    assert database.execute(
        "SELECT COUNT(*) FROM repo_merge_jobs WHERE repo_bi='repo-a'"
    ).fetchone()[0] == 3

    add("terminal-b", "repo-b", "failed", 7, 10_000)
    add("active-d", "repo-c", "requested", 8, 10_000)
    assert database.execute(
        "SELECT COUNT(*) FROM repo_merge_jobs").fetchone()[0] == 5
    # Global pressure evicts a terminal row but never an in-flight row.
    assert asyncio.run(make_room(None, "repo-z", 100)) is True
    ids = {
        row[0] for row in database.execute(
            "SELECT request_id FROM repo_merge_jobs")
    }
    assert "terminal-b" not in ids
    assert {"active-a", "active-b", "active-c", "active-d"} <= ids

    for number in range(256):
        add(
            "hard-%03d" % number,
            "repo-hard",
            "requested",
            1000 + number,
            20_000,
        )
    with pytest.raises(
        sqlite3.IntegrityError, match="repo_merge_jobs_repo_limit"
    ):
        add("hard-overflow", "repo-hard", "requested", 2000, 20_000)


def test_forward_migration_repairs_early_merge_job_table_without_data_loss():
    database = sqlite3.connect(":memory:")
    database.executescript(
        """
        CREATE TABLE repo_merge_jobs (
          request_id TEXT PRIMARY KEY,
          request_digest TEXT NOT NULL,
          repo_bi TEXT NOT NULL,
          actor_bi TEXT NOT NULL,
          pull_number INTEGER NOT NULL,
          selected_node TEXT NOT NULL,
          status TEXT NOT NULL,
          result TEXT NOT NULL DEFAULT '',
          created_at INTEGER NOT NULL,
          updated_at INTEGER NOT NULL
        );
        CREATE INDEX idx_repo_merge_jobs_repo
          ON repo_merge_jobs(repo_bi,status,updated_at);
        INSERT INTO repo_merge_jobs VALUES (
          'repair-me','digest','repo','actor',7,'mirror2','requested','',
          1000,2000
        );
        """
    )
    repair = (
        WORKER
        / "migrations"
        / "0071_rebuild_repository_pull_merge_jobs.sql"
    ).read_text(encoding="utf-8")
    database.executescript(repair)
    columns = {
        row[1] for row in database.execute(
            "PRAGMA table_info(repo_merge_jobs)")
    }
    assert "expires_at" in columns
    row = database.execute(
        "SELECT request_id,expires_at FROM repo_merge_jobs"
    ).fetchone()
    assert row == ("repair-me", 2000 + 7 * 24 * 60 * 60 * 1000)
    objects = {
        row[0] for row in database.execute(
            "SELECT name FROM sqlite_master "
            "WHERE tbl_name='repo_merge_jobs'")
    }
    assert {
        "idx_repo_merge_jobs_repo",
        "idx_repo_merge_jobs_expiry",
        "trg_repo_merge_jobs_repo_bound",
        "trg_repo_merge_jobs_global_bound",
    } <= objects


def test_executor_command_receives_json_stdin_without_shell_or_ambient_secrets():
    calls = []

    def runner(command, **kwargs):
        calls.append((command, kwargs))
        return type("Done", (), {
            "stdout": json.dumps({
                "ok": True,
                "status": "missing",
                "requestId": "merge_request_0001",
            })
        })()

    executor = gateway.ExternalMergeExecutor(
        ["/usr/bin/python3", "/opt/forkmesh/merge.py"], runner=runner)
    result = executor.call({
        "schemaVersion": 1,
        "type": "forkmesh.pull-merge-executor-v1",
        "requestId": "merge_request_0001",
    })
    assert result["status"] == "missing"
    command, kwargs = calls[0]
    assert command == ["/usr/bin/python3", "/opt/forkmesh/merge.py"]
    assert isinstance(kwargs["input"], str)
    assert kwargs["text"] is True and kwargs["capture_output"] is True
    assert "shell" not in kwargs
    environment = kwargs["env"]
    assert not any(
        marker in key.upper()
        for key in environment
        for marker in ("TOKEN", "SECRET", "PASSWORD", "PRIVATE")
    )


def update_body(**changes):
    value = {
        "schemaVersion": 1,
        "type": "forkmesh.pull-update-v1",
        "pullNumber": 7,
        "requestId": "merge_request_0001",
        "expectedBaseOid": OID_A,
        "expectedHeadOid": OID_B,
        "expectedPullsOid": OID_C,
    }
    value.update(changes)
    return json.dumps(value, separators=(",", ":")).encode()


def test_update_route_is_distinct_and_bound_to_one_open_pull():
    assert urls.REPO_PULL_UPDATE_RE.fullmatch(
        "/api/repo/forkmesh/forkmesh/pulls/7/update")
    # The two mutations must never be reachable through each other's path:
    # a merge request arriving on /update would move the base branch.
    assert not urls.REPO_PULL_UPDATE_RE.fullmatch(
        "/api/repo/forkmesh/forkmesh/pulls/7/merge")
    assert not urls.REPO_PULL_MERGE_RE.fullmatch(
        "/api/repo/forkmesh/forkmesh/pulls/7/update")
    assert not urls.REPO_PULL_UPDATE_RE.fullmatch(
        "/api/repo/forkmesh/forkmesh/pulls/0/update")


def test_update_envelope_is_typed_so_a_merge_body_cannot_drive_it():
    ns = _entry_functions("_https_mirror_merge_body")
    parse = ns["_https_mirror_merge_body"]
    parsed = parse(update_body().decode(), 7, "update")
    assert parsed and parsed["type"] == "forkmesh.pull-update-v1"
    assert "sessionToken" not in parsed
    # Wire types are not interchangeable in either direction.
    assert parse(merge_body().decode(), 7, "update") is None
    assert parse(update_body().decode(), 7, "merge") is None
    # The exact-OID and route-number bindings still hold for the new mutation.
    assert parse(update_body(pullNumber=8).decode(), 7, "update") is None
    assert parse(update_body(expectedHeadOid="z" * 40).decode(), 7,
                 "update") is None


def test_update_result_publishes_only_the_head_branch():
    ns = _entry_functions("_https_mirror_merge_body", "_https_mirror_merge_result")
    parsed = ns["_https_mirror_merge_body"](update_body().decode(), 7, "update")
    validate = ns["_https_mirror_merge_result"]
    updated = {
        "ok": True,
        "status": "updated",
        "requestId": parsed["requestId"],
        "published": True,
        "baseBefore": OID_A,
        "head": OID_B,
        "pullsBefore": OID_C,
        "headAfter": "d" * 40,
    }
    checked = validate(updated, parsed, "update")
    assert checked["status"] == "updated"
    # An update moves the head branch and nothing else: the merge mutation's
    # published object ids must not survive this profile.
    assert "baseAfter" not in checked and "pullsAfter" not in checked
    assert checked["headAfter"] == "d" * 40
    # A "merged" terminal status on the update profile is not a valid result.
    assert validate(dict(updated, status="merged"), parsed, "update") is None
    # Reviewed OIDs stay pinned to the request.
    assert validate(dict(updated, head="e" * 40), parsed, "update") is None


def test_update_failure_vocabulary_is_bounded_and_profile_specific():
    ns = _entry_functions("_https_mirror_merge_body", "_https_mirror_merge_result")
    parsed = ns["_https_mirror_merge_body"](update_body().decode(), 7, "update")
    validate = ns["_https_mirror_merge_result"]

    def failure(error):
        return validate({
            "ok": False,
            "status": "failed",
            "requestId": parsed["requestId"],
            "error": error,
        }, parsed, "update")

    assert failure("already_current")["error"] == "already_current"
    assert failure("merge_conflict")["error"] == "merge_conflict"
    # review_required belongs to the merge profile only: updating a branch from
    # its base needs no peer approval, so accepting it here would report a
    # blocker the update path can never actually raise.
    assert failure("review_required") is None
    assert failure("something_else") is None


def test_update_is_audited_under_its_own_action_name():
    # Both mutations share one handler, so the audited action must come from
    # the profile rather than a hardcoded string, or an update would be logged
    # as a merge and the audit trail would misdescribe what moved.
    ns = _entry_functions()
    profiles = ns["HTTPS_MIRROR_PULL_MUTATIONS"]
    assert profiles["update"]["action"] == "repository.pull_update"
    assert profiles["merge"]["action"] == "repository.pull_merge"
    assert profiles["update"]["operation"] == "update-pull-branch"
    assert 'profile["action"]' in MERGE_HANDLER_TEXT
    assert '"repository.pull_merge"' not in MERGE_HANDLER_TEXT
    # Every terminal outcome is audited, not just the successful one.
    for outcome in ('"denied"', '"requested"'):
        assert outcome in MERGE_HANDLER_TEXT
    assert '"success" if terminal == "succeeded" else "failed"' in (
        MERGE_HANDLER_TEXT)


def test_update_is_owner_authorized_through_the_same_gate_as_merge():
    # The shared handler authorizes once, before the mutation is selected, so
    # the update path cannot be reached with a weaker check than merge.
    assert "_repository_owner_can_merge_repo" in MERGE_HANDLER_TEXT
    assert MERGE_HANDLER_TEXT.count("_repository_owner_can_merge_repo") == 1
    assert MERGE_HANDLER_TEXT.index("_repository_owner_can_merge_repo") < (
        MERGE_HANDLER_TEXT.index('profile["operation"]'))


def test_update_operation_is_public_routed_but_needs_the_signed_proof():
    # Routed at the edge like the other node operations...
    assert "update-pull-branch" in edge_routing.PUBLIC_OPERATIONS
    assert "merge-preview" in edge_routing.PUBLIC_OPERATIONS
    # ...while the node still demands the per-repository signed proof for the
    # update operation specifically, not for a generic merge capability.
    proxy = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _https_mirror_merge_proxy"):
        ENTRY_TEXT.index("\n\n\nasync def _https_mirror_actions_proxy")
    ]
    assert '_https_mirror_repository_proof(\n            env, endpoint, context, profile["operation"],' in proxy
    # A signed refs lag admits the job's node only for that same operation.
    assert 'https_routing.signed_refs_lag(\n                profile["operation"], proof_details)' in proxy


REFS_LAG = {
    "reason": "refs_mismatch",
    "signed": True,
    "refsSha256": "d" * 64,
    "operations": ["blob", "merge-pull", "update-pull-branch"],
}


def _route_handler_function(name, **namespace):
    tree = ast.parse(ROUTE_HANDLERS_TEXT)
    node = next(
        node for node in tree.body
        if isinstance(node, ast.AsyncFunctionDef) and node.name == name)
    namespace.setdefault("https_routing", edge_routing)
    exec(compile(
        ast.fix_missing_locations(ast.Module(body=[node], type_ignores=[])),
        "route_handlers.py", "exec"), namespace)
    return namespace[name]


def test_merge_selection_admits_two_mirrors_signing_the_same_digest():
    # forkmesh/forkmesh#268 (2026-09-18): all four mirrors advertised
    # merge-pull and were healthy, but the pin set had not yet learned the
    # digest they signed after the latest push, so each strict proof failed
    # and the owner read "No online mirror can merge this pull request".
    verdicts = {}
    asked = []

    async def proof(env, endpoint, context, operation, **options):
        asked.append((endpoint["node"], operation, options.get("retry_failed")))
        options["details"].update(verdicts[endpoint["node"]])
        return verdicts[endpoint["node"]].get("reason") == "verified"

    pick = _route_handler_function(
        "_pull_mutation_node", _https_mirror_repository_proof=proof)

    def run(*nodes):
        asked.clear()
        return asyncio.run(pick(
            {}, {"owner": "forkmesh", "repo": "forkmesh"},
            [{"node": node} for node in nodes], "merge-pull"))

    verdicts.update({
        "mirror35": dict(REFS_LAG),
        "mirror36": dict(REFS_LAG),
        "mirror37": dict(REFS_LAG, refsSha256="e" * 64),
        "mirror38": {"reason": "verified"},
        "old": dict(REFS_LAG, reason="git_operation_unavailable"),
        "dark": {"reason": "mirror_unhealthy"},
    })
    # One lagging node alone, or two on different digests, is not enough;
    # a second node signing the same digest is.
    assert run("mirror35", "mirror37") == (None, "refs_catching_up")
    selected, reason = run("mirror35", "mirror37", "mirror36")
    assert (selected["node"], reason) == ("mirror36", "")
    # A pinned node is taken as before, and every proof is fetched fresh
    # rather than replaying a read's held failure.
    assert run("mirror38")[0]["node"] == "mirror38"
    assert asked == [("mirror38", "merge-pull", True)]
    # Only a fleet without the executor is blamed on its build.
    assert run("old") == (None, "no_merge_executor")
    assert run("dark", "old") == (None, "mirror_unverified")
    assert run() == (None, "no_online_mirror")


def test_the_job_node_answers_through_its_own_signed_refs_lag():
    # Running the job moves the executing node's refs, so a strict pin kept a
    # finished merge "pending" until the health cron re-proved two peers.
    ns = _entry_functions(
        "_https_mirror_merge_proxy", "_https_mirror_merge_result")
    verdict = {}
    fetched = []

    async def proof(env, endpoint, context, operation, details=None):
        details.update(verdict)
        return verdict.get("reason") == "verified"

    class Upstream:
        status = 200
        headers = {}

        async def text(self):
            return json.dumps({
                "ok": True, "status": "merged",
                "requestId": "merge_request_0001", "published": True,
                "baseBefore": OID_A, "head": OID_B, "pullsBefore": OID_C,
                "baseAfter": "d" * 40, "pullsAfter": "e" * 40,
            })

    async def fetch(target, init, timeout):
        fetched.append(target)
        return Upstream()

    async def sign(public_key, seed, message):
        return "signature"

    from types import SimpleNamespace
    from urllib.parse import urlparse
    ns.update({
        "https_routing": edge_routing,
        "_https_mirror_repository_proof": proof,
        "urlparse": urlparse,
        "hashlib": hashlib,
        "Date": SimpleNamespace(now=lambda: 1_789_763_791_000),
        "_b64url_encode": lambda value: "router_request_0001",
        "_random_bytes": lambda size: b"\0" * size,
        "_https_mirror_router_public_key": lambda env: "router-key",
        "_https_mirror_router_seed": lambda env: "router-seed",
        "ed25519_sign": sign,
        "js_fetch_with_timeout": fetch,
        "Uint8Array": SimpleNamespace(new=lambda value: value),
        "_to_js": lambda value: value,
        "HTTPS_MIRROR_FETCH_TIMEOUT_SECONDS": 10,
        "HTTPS_MIRROR_MERGE_RESULT_MAX_BYTES": 64 * 1024,
        "HTTPS_MIRROR_MERGE_ATTESTATION_SIG_RE": re.compile(
            r"^[A-Za-z0-9_-]{86}$"),
    })
    request = {
        "schemaVersion": 1, "type": "forkmesh.pull-merge-v1",
        "pullNumber": 7, "requestId": "merge_request_0001",
        "expectedBaseOid": OID_A, "expectedHeadOid": OID_B,
        "expectedPullsOid": OID_C,
    }

    def run():
        return asyncio.run(ns["_https_mirror_merge_proxy"](
            {}, {"node": "mirror35", "baseUrl": "https://mirror35.invalid"},
            {"owner": "forkmesh", "repo": "forkmesh"}, request))

    verdict.update(REFS_LAG)
    status, result = run()
    assert status == 200 and result["status"] == "merged"
    assert len(fetched) == 1
    # Nothing weaker than a signed lag on the merge operation reaches it.
    for broken in (
        dict(REFS_LAG, signed=False),
        dict(REFS_LAG, reason="mirror_unhealthy"),
        dict(REFS_LAG, reason="repository_signature_invalid"),
        dict(REFS_LAG, operations=["blob"]),
    ):
        verdict.clear()
        verdict.update(broken)
        assert run() == (0, None)
    assert len(fetched) == 1


def _owner_merge_namespace(linked=False, owns=("alice",)):
    import types
    from urllib.parse import urlparse
    ns = _entry_functions("_repository_owner_can_merge_repo", constants=())

    async def org_owner(_env, _request, _owner, _repo, _actor):
        return False

    async def d1_first(_env, sql, *args):
        assert "FROM org_repos WHERE repo=? AND node_owner=?" in sql
        return {"ok": 1} if linked else None

    async def owns_node(_env, actor, node):
        return actor in owns and node == "alice"

    ns.update({
        "_organization_owner_can_merge_repo": org_owner,
        "d1_first": d1_first,
        "_account_owns_node": owns_node,
        "urlparse": urlparse,
        "REPO_API_PREFIX_RE": re.compile(r"^/api/repo/([^/]+)/([^/]+)"),
        "safe_segment": lambda value: value,
    })
    return ns, types.SimpleNamespace


def _merge_request(path):
    import types
    return types.SimpleNamespace(url="https://forge.test" + path)


def test_a_personal_repository_owner_may_merge_on_the_web():
    ns, _ = _owner_merge_namespace()
    check = ns["_repository_owner_can_merge_repo"]
    request = _merge_request("/api/repo/alice/garden/pulls/3/merge")
    assert asyncio.run(check(None, request, "alice", "garden", "alice")) is True
    assert asyncio.run(check(None, request, "alice", "garden", "bob")) is False
    assert asyncio.run(check(None, request, "alice", "garden", "")) is False
    other = _merge_request("/api/repo/alice/other/pulls/3/merge")
    assert asyncio.run(check(None, other, "alice", "garden", "alice")) is False


def test_an_organization_linked_repository_keeps_the_organization_rule():
    ns, _ = _owner_merge_namespace(linked=True)
    check = ns["_repository_owner_can_merge_repo"]
    request = _merge_request("/api/repo/alice/garden/pulls/3/merge")
    assert asyncio.run(check(None, request, "alice", "garden", "alice")) is False


def test_rules_protection_and_checks_still_run_after_owner_authorization():
    authorize = MERGE_HANDLER_TEXT.index("_repository_owner_can_merge_repo")
    for gate in ("_pull_rules.merge_gate(", "_branch_protection.merge_refusal("):
        assert authorize < MERGE_HANDLER_TEXT.index(gate)
