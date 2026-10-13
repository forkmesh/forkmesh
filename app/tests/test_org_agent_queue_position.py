import ast
import asyncio
import sqlite3
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ROUTES = ROOT / "src" / "route_handlers.py"
sys.path.insert(0, str(ROOT / "src"))

NOW = 1_000_000


def _load():
    tree = ast.parse(ROUTES.read_text(encoding="utf-8"), filename=str(ROUTES))
    wanted = {"_org_agent_queue_projection"}
    body = [
        node for node in tree.body
        if (isinstance(node, ast.AsyncFunctionDef) and node.name in wanted)
        or (isinstance(node, ast.Assign) and any(
            isinstance(t, ast.Name) and t.id.startswith("_ORG_AGENT_QUEUE")
            or isinstance(t, ast.Name) and t.id.startswith("_ORG_AGENT_RUNNER")
            for t in node.targets))
    ]
    db = sqlite3.connect(":memory:")
    db.row_factory = sqlite3.Row
    db.executescript(
        "CREATE TABLE org_agent_jobs (id INTEGER PRIMARY KEY, session_id TEXT,"
        " target_node TEXT, repo TEXT, status TEXT, created_at INTEGER);"
        "CREATE TABLE runner_nodes (name TEXT PRIMARY KEY, status TEXT,"
        " last_seen_at INTEGER);"
        "CREATE TABLE runner_job_leases (job_id INTEGER PRIMARY KEY,"
        " runner TEXT, lease_id TEXT, expires_at INTEGER, created_at INTEGER);")

    async def d1_first(_env, sql, *args):
        found = db.execute(sql, args).fetchone()
        return dict(found) if found else None

    async def d1_all(_env, sql, *args):
        return [dict(item) for item in db.execute(sql, args).fetchall()]

    class _Date:
        @staticmethod
        def now():
            return NOW

    namespace = {"d1_first": d1_first, "d1_all": d1_all, "Date": _Date}
    exec(compile(ast.fix_missing_locations(
        ast.Module(body=body, type_ignores=[])), str(ROUTES), "exec"),
         namespace)
    return db, namespace["_org_agent_queue_projection"]


def _jobs(db, rows):
    db.executemany(
        "INSERT INTO org_agent_jobs (id,session_id,target_node,repo,status,"
        "created_at) VALUES (?,?,?,?,?,?)", rows)


ROW = {"session_id": "mine", "status": "queued", "target_node": "fmnext-n1"}
RECORD = {"runnerNode": "fmnext-r1", "route": "runner_pool"}


def test_a_queued_run_learns_its_place_and_who_else_waits():
    db, project = _load()
    _jobs(db, [
        (1, "a", "fmnext-n1", "forkmesh", "running", 10),
        (2, "b", "fmnext-n1", "widgets", "queued", 20),
        (3, "mine", "fmnext-n1", "forkmesh", "queued", 30),
        (4, "c", "fmnext-n1", "forkmesh", "queued", 40),
        (5, "d", "other", "forkmesh", "queued", 50),
    ])
    db.execute("INSERT INTO runner_nodes VALUES ('fmnext-r1','active',?)",
               (NOW - 90_000,))
    db.execute("INSERT INTO runner_job_leases VALUES (1,'fmnext-r1','L1',?,?)",
               (NOW + 3_600_000, NOW - 600_000))
    queue = asyncio.run(project(None, ROW, RECORD))
    assert queue["position"] == 2
    assert queue["ahead"] == 1
    assert queue["waiting"] == 3
    assert queue["running"] == 1
    assert queue["repoBusy"] is False
    assert queue["queuedAt"] == 30
    assert queue["now"] == NOW
    runner = queue["runner"]
    assert runner["name"] == "fmnext-r1"
    assert runner["busy"] is True and runner["online"] is True
    assert runner["nextCheckAt"] == 0
    assert runner["busySince"] == NOW - 600_000
    assert runner["freeBy"] == NOW + 3_600_000


def test_an_idle_runner_shows_when_its_held_claim_ends():
    db, project = _load()
    _jobs(db, [(7, "mine", "fmnext-n1", "forkmesh", "queued", 30)])
    db.execute("INSERT INTO runner_nodes VALUES ('fmnext-r1','active',?)",
               (NOW - 5_000,))
    runner = asyncio.run(project(None, ROW, RECORD))["runner"]
    assert runner["busy"] is False and runner["online"] is True
    assert runner["nextCheckAt"] == NOW + 15_000


def test_a_runner_silent_past_its_claim_window_reads_offline():
    db, project = _load()
    _jobs(db, [(7, "mine", "fmnext-n1", "forkmesh", "queued", 30)])
    db.execute("INSERT INTO runner_nodes VALUES ('fmnext-r1','active',?)",
               (NOW - 120_000,))
    assert asyncio.run(project(None, ROW, RECORD))["runner"]["online"] is False


def test_a_running_session_has_no_queue():
    db, project = _load()
    _jobs(db, [(7, "mine", "fmnext-n1", "forkmesh", "queued", 30)])
    assert asyncio.run(project(None, dict(ROW, status="running"), RECORD)) == {}
