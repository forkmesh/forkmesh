"""Admin D1 report: daily Free allowances, attribution, and storage."""
import ast
import asyncio
import json
from pathlib import Path
from types import SimpleNamespace


ROOT = Path(__file__).resolve().parents[2]
ENTRY = ROOT / "app" / "src" / "entry.py"
USAGE_REPORTING = ROOT / "app" / "src" / "usage_reporting.py"
ENTRY_TEXT = (
    ENTRY.read_text(encoding="utf-8") + "\n" +
    USAGE_REPORTING.read_text(encoding="utf-8"))
NOW = 1_787_608_800_000  # 2026-08-24T22:00:00Z


def _load(*names, extra_globals=None):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    wanted = set(names)
    wanted_assignments = {
        "D1_FREE_ROWS_READ_PER_DAY",
        "D1_FREE_ROWS_WRITTEN_PER_DAY",
        "D1_USAGE_DETAIL_GROUP_LIMIT",
        "D1_QUERY_INSIGHT_LIMIT",
        "USAGE_BUDGET_GRAPHQL_URL",
    }
    module = ast.Module(body=[], type_ignores=[])
    for node in tree.body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            if node.name in wanted:
                module.body.append(node)
        elif isinstance(node, ast.Assign):
            targets = {
                target.id
                for target in node.targets
                if isinstance(target, ast.Name)
            }
            if targets & wanted_assignments:
                module.body.append(node)
    namespace = {
        "json": json, "time": __import__("time"), "re": __import__("re")}
    namespace.update(extra_globals or {})
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace


def test_d1_query_reads_complete_totals_and_bounded_database_groups():
    captured = {}

    async def fake_fetch(url, options, _timeout):
        captured["url"] = url
        captured["body"] = json.loads(options["body"])

        class Response:
            status = 200

            async def text(self):
                return json.dumps({
                    "data": {"viewer": {"accounts": [{
                        "totals": [
                            {"sum": {
                                "readQueries": "1000",
                                "writeQueries": 60,
                                "rowsRead": "1200000",
                                "rowsWritten": 45000,
                            }},
                            {"sum": {
                                "readQueries": "200",
                                "writeQueries": 20,
                                "rowsRead": "50000",
                                "rowsWritten": 5000,
                            }},
                        ],
                        "databases": [
                            {
                                "dimensions": {"databaseId": "quiet-db"},
                                "sum": {"rowsRead": 25,
                                        "rowsWritten": 100},
                            },
                            {
                                "dimensions": {"databaseId": "busy-db"},
                                "sum": {"readQueries": 1100,
                                        "writeQueries": 70,
                                        "rowsRead": 1249975,
                                        "rowsWritten": 49900},
                            },
                        ],
                        "hours": [
                            {"dimensions": {
                                "datetimeHour": "2026-08-24T21:00:00Z"},
                             "sum": {"rowsRead": 10,
                                     "rowsWritten": 2}},
                        ],
                        "queries": [
                            {"dimensions": {"query":
                                "SELECT * FROM users WHERE id='private' "
                                "AND retry=42 -- internal"},
                             "count": 8,
                             "sum": {"rowsRead": 800,
                                     "rowsWritten": 0,
                                     "rowsReturned": 80,
                                     "queryDurationMs": 24},
                             "avg": {"rowsRead": 100,
                                     "rowsWritten": 0,
                                     "rowsReturned": 10,
                                     "queryDurationMs": 3}},
                        ],
                    }]}},
                })

        return Response()

    ns = _load(
        "_cloudflare_d1_usage_today",
        "_safe_d1_query_text",
        "_usage_budget_utc",
        extra_globals={
            "js_fetch_with_timeout": fake_fetch,
            "CRON_OUTBOUND_FETCH_TIMEOUT_SECONDS": 10,
        },
    )
    detail = asyncio.run(
        ns["_cloudflare_d1_usage_today"]("account", "token", NOW)
    )
    query = captured["body"]["query"]
    assert query.count("d1AnalyticsAdaptiveGroups") == 3
    assert "totals:" in query and "databases:" in query and "hours:" in query
    assert "d1QueriesAdaptiveGroups" in query
    assert "orderBy:[sum_rowsRead_DESC]" in query
    assert "dimensions{datetimeHour}" in query
    assert "rowsRead rowsWritten" in query
    assert "dimensions{databaseId}" in query
    assert "date_geq:$day,date_leq:$day" in query
    assert captured["body"]["variables"] == {
        "acct": "account",
        "day": ns["_usage_budget_utc"](NOW)[0],
        "start": ns["_usage_budget_utc"](NOW)[1],
        "end": ns["_usage_budget_utc"](NOW)[2],
    }
    assert captured["url"] == ns["USAGE_BUDGET_GRAPHQL_URL"]
    assert detail["totals"]["rowsRead"] == 1_250_000
    assert detail["accountWide"] is True
    assert detail["analyticsDelayed"] is True
    assert [row["databaseId"] for row in detail["databases"]] == [
        "busy-db", "quiet-db"]
    assert detail["databases"][1]["readQueries"] == 0
    assert detail["timeline"] == [{
        "at": "2026-08-24T21:00:00Z", "readQueries": 0.0,
        "writeQueries": 0.0, "rowsRead": 10.0, "rowsWritten": 2.0,
    }]
    insight = detail["queryInsights"][0]
    assert insight["query"] == (
        "SELECT * FROM users WHERE id=? AND retry=?")
    assert insight["runs"] == 8
    assert insight["totalRowsRead"] == 800
    assert insight["avgRowsRead"] == 100.0
    assert insight["efficiency"] == 0.1


def test_query_text_redacts_literals_comments_and_has_a_hard_length_cap():
    ns = _load("_safe_d1_query_text", extra_globals={"re": __import__("re")})
    assert ns["_safe_d1_query_text"](
        "SELECT x'CAFE', name FROM t /* note */ WHERE n=123 "
        "AND secret='value' -- tail\n ORDER BY name"
    ) == "SELECT ?, name FROM t WHERE n=? AND secret=? ORDER BY name"
    assert len(ns["_safe_d1_query_text"]("SELECT " + "x" * 500)) == 360


def test_payload_states_current_free_limits_and_provider_storage():
    ns = _load("_d1_usage_report_payload", "_usage_budget_utc")
    detail = {
        "totals": {"rowsRead": 1_250_000, "rowsWritten": 50_000},
        "databases": [{
            "databaseId": "2d33ad9e-1ab2-4ca0-9075-6105d0e04d11",
            "rowsRead": 1_250_000,
            "rowsWritten": 50_000,
            "readQueries": 1200,
            "writeQueries": 80,
        }],
        "detailTruncated": False,
        "timeline": [{"at": "2026-08-24T21:00:00Z",
                      "rowsRead": 15, "rowsWritten": 2}],
        "queryInsights": [{"query": "SELECT * FROM users", "runs": 3}],
        "queryInsightLimit": 15,
    }
    storage = {
        "bytes": 14_811_136,
        "freeDatabaseLimitBytes": 500_000_000,
        "paidDatabaseLimitBytes": 10_000_000_000,
        "includedAccountStorageBytes": 5_000_000_000,
    }
    payload = ns["_d1_usage_report_payload"](detail, storage, NOW)
    budgets = {row["metric"]: row for row in payload["budgets"]}
    assert budgets["rows-read"] == {
        "metric": "rows-read", "label": "Rows read", "unit": "rows",
        "allowance": 5_000_000.0, "used": 1_250_000.0,
        "percent": 25.0,
    }
    assert budgets["rows-written"]["allowance"] == 100_000.0
    assert budgets["rows-written"]["percent"] == 50.0
    assert payload["storage"] == storage
    assert payload["accountWide"] is True
    assert payload["analyticsDelayed"] is True
    assert payload["databases"][0]["readQueries"] == 1200
    assert payload["timeline"][0] == {
        "at": "2026-08-24T21:00:00Z",
        "rows-read": 15,
        "rows-written": 2,
    }
    assert payload["queryInsights"][0]["runs"] == 3
    assert payload["queryInsightLimit"] == 15


def _endpoint(authorized=True, analytics=None, token="token"):
    calls = []
    responses = []

    async def fake_authorized(_env, _node, _ts, _sig, canonical):
        assert canonical.startswith(b"forkmesh-admin-d1-usage-report-v1\n")
        return {"name": "admin"} if authorized else None

    async def fake_analytics(_account, _token, _now):
        calls.append("analytics")
        return analytics

    class Infrastructure:
        async def d1_storage_usage(self, _env):
            calls.append("storage")
            return {
                "bytes": 42,
                "freeDatabaseLimitBytes": 500_000_000,
                "includedAccountStorageBytes": 5_000_000_000,
            }

    def fake_json_response(body, status=200, cache_control=None):
        response = {
            "body": body,
            "status": status,
            "cache_control": cache_control,
        }
        responses.append(response)
        return response

    ns = _load(
        "_admin_d1_usage_report",
        "_d1_read_guard_fields",
        "_d1_usage_report_payload",
        "_usage_budget_utc",
        extra_globals={
            "parse_qs": __import__("urllib.parse", fromlist=[""]).parse_qs,
            "urlparse": __import__("urllib.parse", fromlist=[""]).urlparse,
            "clean_string": lambda value, _maximum: str(value or ""),
            "MAX_NODE_NAME": 64,
            "_admin_authorized": fake_authorized,
            "_cloudflare_d1_usage_today": fake_analytics,
            "_world_infrastructure_module": lambda: Infrastructure(),
            "json_response": fake_json_response,
            "Date": SimpleNamespace(now=lambda: NOW),
        },
    )
    env = SimpleNamespace(
        WORKERS_OBSERVABILITY_ACCOUNT_ID="account",
        USAGE_ANALYTICS_API_TOKEN=token,
    )
    request = SimpleNamespace(
        url="https://forkmesh.com/api/accounts/admin-d1-usage-report"
            "?node=admin&ts=1&sig=x"
    )
    asyncio.run(ns["_admin_d1_usage_report"](env, request))
    return calls, responses[-1]


def test_endpoint_requires_admin_and_preserves_storage_without_analytics():
    calls, response = _endpoint(authorized=False)
    assert response["status"] == 401
    assert calls == []

    calls, response = _endpoint(token="")
    assert response["status"] == 200
    assert response["body"]["ok"] is False
    assert response["body"]["configured"] is False
    assert response["body"]["error"] == "analytics_unconfigured"
    assert response["body"]["storage"]["bytes"] == 42
    assert calls == ["storage"]
    assert "no-store" in response["cache_control"]


def test_endpoint_returns_daily_metrics_with_direct_storage():
    detail = {
        "totals": {"rowsRead": 5, "rowsWritten": 2},
        "databases": [],
    }
    calls, response = _endpoint(analytics=detail)
    assert response["status"] == 200
    assert response["body"]["ok"] is True
    assert response["body"]["sampledAt"] == NOW
    assert response["body"]["storage"]["bytes"] == 42
    assert calls == ["storage", "analytics"]


def test_d1_usage_report_route_is_dispatched():
    assert (
        'if url.path == "/api/accounts/admin-d1-usage-report" '
        'and method == "GET":'
    ) in ENTRY_TEXT
    assert "await _admin_d1_usage_report(env, request)" in ENTRY_TEXT
