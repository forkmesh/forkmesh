"""Free-plan usage sampler: threshold Pings once per level per UTC day."""
import ast
import asyncio
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ENTRY = ROOT / "app" / "src" / "entry.py"
USAGE_REPORTING = ROOT / "app" / "src" / "usage_reporting.py"
ENTRY_TEXT = (
    ENTRY.read_text(encoding="utf-8") + "\n" +
    USAGE_REPORTING.read_text(encoding="utf-8"))


def _load(*names, extra_globals=None):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    wanted = set(names)
    want_assigns = {
        "USAGE_BUDGET_THRESHOLDS", "USAGE_BUDGET_METRICS",
        "USAGE_BUDGET_GRAPHQL_URL", "USAGE_BUDGET_PING_BATCH",
        "D1_FREE_ROWS_READ_PER_DAY",
    }
    module = ast.Module(body=[], type_ignores=[])
    for node in tree.body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            if node.name in wanted:
                module.body.append(node)
        elif isinstance(node, ast.Assign):
            targets = {
                t.id for t in node.targets if isinstance(t, ast.Name)}
            if targets & want_assigns:
                module.body.append(node)
    namespace = {"time": __import__("time"), "json": json}
    namespace.update(extra_globals or {})
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace


class _FakeD1:
    """Just enough of d1_first/d1_run for the single-state-row upsert."""

    def __init__(self):
        self.state = {}

    async def first(self, _env, sql, *args):
        assert "FROM usage_budget_state" in sql
        return self.state.get(args[0])

    async def run(self, _env, sql, *args):
        assert "INSERT INTO usage_budget_state" in sql
        metric, day, percent, pinged, sampled = args
        self.state[metric] = {
            "metric": metric, "day": day, "percent": percent,
            "pinged_threshold": pinged, "sampled_at": sampled,
        }


def _sampler(usage, now_ms, d1, pings):
    async def fake_usage(_account, _token, _now):
        return dict(usage)

    async def fake_ping(_env, title, body, dedupe, _now):
        pings.append({"title": title, "body": body, "dedupe": dedupe})
        return 1

    async def fake_ensure_schema(_env):
        return None

    namespace = _load(
        "record_usage_budget_sample", "_usage_budget_utc",
        "_usage_budget_amount",
        extra_globals={
            "Date": type("Date", (), {"now": staticmethod(lambda: now_ms)}),
            "clean_string": lambda value, _max: str(value or ""),
            "_cloudflare_usage_today": fake_usage,
            "_usage_budget_ping_admins": fake_ping,
            "ensure_schema": fake_ensure_schema,
            "d1_first": d1.first,
            "d1_run": d1.run,
        },
    )
    env = type("Env", (), {
        "WORKERS_OBSERVABILITY_ACCOUNT_ID": "acct",
        "USAGE_ANALYTICS_API_TOKEN": "token",
    })()
    return namespace, env


DAY_MS = 24 * 60 * 60 * 1000
NOON = (20_000 * DAY_MS) + 12 * 60 * 60 * 1000


def test_thresholds_ping_once_each_and_only_the_highest_crossed():
    d1 = _FakeD1()
    pings = []
    quiet = {"do-duration": 100.0, "do-requests": 10.0,
             "worker-requests": 10.0}
    ns, env = _sampler(quiet, NOON, d1, pings)
    asyncio.run(ns["record_usage_budget_sample"](env))
    assert pings == []

    # 55% crosses 10 and 50 in one sample: exactly one Ping, for 50.
    ns, env = _sampler({**quiet, "do-duration": 13_000 * 0.55}, NOON, d1,
                       pings)
    asyncio.run(ns["record_usage_budget_sample"](env))
    assert len(pings) == 1
    assert pings[0]["title"] == "Cloudflare usage over 50%"
    assert "Durable Object duration" in pings[0]["body"]
    assert "resets at 00:00 UTC" in pings[0]["body"]

    # The same level ten minutes later stays quiet.
    ns, env = _sampler({**quiet, "do-duration": 13_000 * 0.56},
                       NOON + 600_000, d1, pings)
    asyncio.run(ns["record_usage_budget_sample"](env))
    assert len(pings) == 1

    # Crossing 80 later pings again, exactly once.
    ns, env = _sampler({**quiet, "do-duration": 13_000 * 0.84},
                       NOON + 1_200_000, d1, pings)
    asyncio.run(ns["record_usage_budget_sample"](env))
    assert len(pings) == 2
    assert pings[1]["title"] == "Cloudflare usage over 80%"


def test_a_new_utc_day_resets_the_ping_ladder():
    d1 = _FakeD1()
    pings = []
    usage = {"do-duration": 13_000 * 0.2, "do-requests": 10.0,
             "worker-requests": 10.0}
    ns, env = _sampler(usage, NOON, d1, pings)
    asyncio.run(ns["record_usage_budget_sample"](env))
    assert len(pings) == 1 and pings[0]["title"] == "Cloudflare usage over 10%"

    ns, env = _sampler(usage, NOON + DAY_MS, d1, pings)
    asyncio.run(ns["record_usage_budget_sample"](env))
    assert len(pings) == 2
    day_one = pings[0]["dedupe"].rsplit(":", 1)[1]
    day_two = pings[1]["dedupe"].rsplit(":", 1)[1]
    assert day_one != day_two


def test_each_metric_pings_independently_with_stable_dedupe_keys():
    d1 = _FakeD1()
    pings = []
    usage = {"do-duration": 13_000 * 0.51, "do-requests": 91_000.0,
             "worker-requests": 11_000.0}
    ns, env = _sampler(usage, NOON, d1, pings)
    asyncio.run(ns["record_usage_budget_sample"](env))
    dedupes = sorted(p["dedupe"] for p in pings)
    day = ns["_usage_budget_utc"](NOON)[0]
    assert dedupes == sorted([
        "usage-budget:do-duration:50:" + day,
        "usage-budget:do-requests:90:" + day,
        "usage-budget:worker-requests:10:" + day,
    ])


def test_missing_credentials_or_analytics_failures_stay_silent():
    d1 = _FakeD1()
    pings = []
    ns, env = _sampler({}, NOON, d1, pings)
    env.USAGE_ANALYTICS_API_TOKEN = ""
    asyncio.run(ns["record_usage_budget_sample"](env))
    assert pings == [] and d1.state == {}

    async def broken_usage(_account, _token, _now):
        return None

    ns, env = _sampler({}, NOON, d1, pings)
    ns["record_usage_budget_sample"].__globals__[
        "_cloudflare_usage_today"] = broken_usage
    asyncio.run(ns["record_usage_budget_sample"](env))
    assert pings == [] and d1.state == {}


def test_usage_budget_phase_is_scheduled_and_dispatched():
    assert 'phases.append("usage-budget")' in ENTRY_TEXT
    assert 'if phase == "usage-budget":' in ENTRY_TEXT
    assert "await record_usage_budget_sample(env)" in ENTRY_TEXT
    schema = (ROOT / "app" / "src" / "schema.py").read_text(encoding="utf-8")
    assert "usage_budget_state" in schema
    deploy = (ROOT / "app" / "deploy.sh").read_text(encoding="utf-8")
    assert " USAGE_ANALYTICS_API_TOKEN " in deploy
