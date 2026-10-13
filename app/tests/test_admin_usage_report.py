"""Admin usage report: per-namespace/per-Worker Cloudflare usage detail."""
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
        "USAGE_BUDGET_METRICS", "USAGE_BUDGET_GRAPHQL_URL",
        "USAGE_DO_NAMESPACE_LABELS", "USAGE_DETAIL_GROUP_LIMIT",
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


NOW = 1_787_176_800_000  # 2026-08-19T22:00:00Z


def _detail_fixture():
    """Shaped like the live GraphQL answer (verified 2026-08-19)."""
    return {
        "periodic": [
            {"dimensions": {"namespaceId": "f562a9fa632f4b4f82e3ad2a6df57f6d"},
             "sum": {"activeTime": 5_865_076_834.0, "cpuTime": 165_190_933.0,
                     "storageReadUnits": 12.0, "storageWriteUnits": 3.0,
                     "storageDeletes": 1.0, "subrequests": 2398.0,
                     "inboundWebsocketMsgCount": 0.0,
                     "outboundWebsocketMsgCount": 0.0}},
            {"dimensions": {"namespaceId": "7b3133892ed24adeaee2689c3eee2d7f"},
             "sum": {"activeTime": 903_157_283.0, "cpuTime": 12_437_168.0,
                     "storageReadUnits": 0.0, "storageWriteUnits": 0.0,
                     "storageDeletes": 0.0, "subrequests": 0.0,
                     "inboundWebsocketMsgCount": 55.0,
                     "outboundWebsocketMsgCount": 941.0}},
            {"dimensions": {"namespaceId": "feedfacefeedfacefeedfacefeedface"},
             "sum": {"activeTime": 128_000_000.0, "cpuTime": 1_000_000.0,
                     "storageReadUnits": 0.0, "storageWriteUnits": 0.0,
                     "storageDeletes": 0.0, "subrequests": 0.0,
                     "inboundWebsocketMsgCount": 0.0,
                     "outboundWebsocketMsgCount": 0.0}},
        ],
        "invocations": [
            {"dimensions": {"namespaceId": "f562a9fa632f4b4f82e3ad2a6df57f6d",
                            "scriptName": "forkmesh-app"},
             "sum": {"requests": 3574.0, "errors": 37.0,
                     "wallTime": 11_939_092_632.0,
                     "responseBodySize": 110_252_662.0}},
            {"dimensions": {"namespaceId": "7b3133892ed24adeaee2689c3eee2d7f",
                            "scriptName": "forkmesh-app"},
             "sum": {"requests": 715.0, "errors": 79.0,
                     "wallTime": 27_879_229_646.0,
                     "responseBodySize": 1_636_898.0}},
        ],
        "workers": [
            {"dimensions": {"scriptName": "forkmesh-www"},
             "sum": {"requests": 462.0, "errors": 0.0, "subrequests": 483.0,
                     "duration": 40.48}},
            {"dimensions": {"scriptName": "forkmesh-app"},
             "sum": {"requests": 2801.0, "errors": 186.0,
                     "subrequests": 3649.0, "duration": 652.30}},
        ],
        "durationHours": [
            {"dimensions": {"datetimeHour": "2026-08-19T21:00:00Z"},
             "sum": {"activeTime": 1_000_000.0}},
        ],
        "doRequestHours": [
            {"dimensions": {"datetimeHour": "2026-08-19T21:00:00Z"},
             "sum": {"requests": 12.0}},
        ],
        "workerRequestHours": [
            {"dimensions": {"datetimeHour": "2026-08-19T21:00:00Z"},
             "sum": {"requests": 8.0}},
        ],
    }


def test_payload_merges_joins_and_ranks_by_gb_seconds():
    ns = _load("_usage_report_payload", "_usage_budget_utc")
    payload = ns["_usage_report_payload"](_detail_fixture(), NOW)
    assert payload["ok"] is True

    durable = payload["durableObjects"]
    assert [row["label"] for row in durable[:2]] == [
        "ForkMeshCronRunner", "ForkMeshRoom"]
    runner = durable[0]
    # 5,865.076834 s of activeTime billed at 128 MB -> 750.73 GB-s.
    assert runner["gbSeconds"] == 750.73
    assert runner["script"] == "forkmesh-app"
    assert runner["requests"] == 3574 and runner["errors"] == 37
    assert runner["wallTimeMs"] == 11_939_092.6
    assert runner["subrequests"] == 2398
    room = durable[1]
    assert room["websocketOut"] == 941 and room["websocketIn"] == 55
    # A namespace the label map has never heard of still shows up, unnamed.
    unknown = durable[2]
    assert unknown["label"] == "" and unknown["gbSeconds"] == 16.38
    assert unknown["requests"] == 0  # periodic-only: no invocations row

    workers = payload["workers"]
    assert [row["script"] for row in workers] == [
        "forkmesh-app", "forkmesh-www"]
    assert workers[0]["gbSeconds"] == 652.3

    budgets = {row["metric"]: row for row in payload["budgets"]}
    assert budgets["do-duration"]["used"] == 750.73 + 115.6 + 16.38
    assert budgets["do-requests"]["used"] == 3574 + 715
    assert budgets["worker-requests"]["used"] == 2801 + 462
    assert budgets["do-duration"]["allowance"] == 13_000.0
    assert budgets["worker-requests"]["percent"] == round(
        (2801 + 462) / 100_000 * 100.0, 2)
    assert payload["timeline"] == [{
        "at": "2026-08-19T21:00:00Z", "do-duration": 0.13,
        "do-requests": 12.0, "worker-requests": 8.0,
    }]
    economics = payload["unitEconomics"]
    assert economics["schema"] == "forkmesh-hosted-usage-v1"
    assert economics["scope"] == "network-aggregate"
    assert economics["costStatus"] == (
        "provider-invoice-reconciliation-required")
    assert economics["measurements"] == [
        {"unit": "worker-request", "quantity": "3263"},
        {"unit": "durable-object-request", "quantity": "4289"},
        {"unit": "durable-object-gb-second", "quantity": "882.71"},
    ]
    assert economics["providerDiagnostics"] == {
        "durableObjectStorageReadOperations": 12,
        "durableObjectStorageWriteOperations": 3,
        "durableObjectStorageDeleteOperations": 1,
        "durableObjectResponseBytes": 111_889_560,
    }
    day, start, end = ns["_usage_budget_utc"](NOW)
    assert (payload["day"], payload["windowStart"], payload["windowEnd"]) == (
        day, start, end)


def test_detail_query_uses_only_verified_dimensions():
    captured = {}

    async def fake_fetch(url, options, _timeout):
        captured["url"] = url
        captured["body"] = json.loads(options["body"])

        class Response:
            status = 200

            async def text(self):
                return json.dumps({"data": {"viewer": {"accounts": [{
                    "periodic": [
                        {"dimensions": {"namespaceId": "abc"},
                         "sum": {"activeTime": "1000000"}}],
                    "invocations": [],
                    "workers": None,
                }]}}})
        return Response()

    ns = _load(
        "_cloudflare_usage_detail_today", "_usage_budget_utc",
        extra_globals={
            "js_fetch_with_timeout": fake_fetch,
            "CRON_OUTBOUND_FETCH_TIMEOUT_SECONDS": 10,
        })
    detail = asyncio.run(
        ns["_cloudflare_usage_detail_today"]("acct", "token", NOW))
    query = captured["body"]["query"]
    # durableObjectsPeriodicGroups has no scriptName dimension (the live API
    # rejects it); the scriptName association must come from invocations.
    assert "durableObjectsPeriodicGroups" in query
    assert query.count("dimensions{namespaceId}") == 1
    assert "dimensions{scriptName namespaceId}" in query
    assert "workersInvocationsAdaptive" in query
    assert query.count("dimensions{datetimeHour}") == 3
    assert query.count("orderBy:[datetimeHour_ASC]") == 3
    assert captured["body"]["variables"]["acct"] == "acct"
    assert captured["url"] == ns["USAGE_BUDGET_GRAPHQL_URL"]
    # Numeric strings from the wire come back as floats, absent groups as [].
    assert detail["periodic"][0]["sum"]["activeTime"] == 1_000_000.0
    assert detail["invocations"] == [] and detail["workers"] == []
    assert detail["durationHours"] == []
    assert detail["doRequestHours"] == []
    assert detail["workerRequestHours"] == []


def _report_namespace(authorized, detail, d1_rows=None, d1_first=None):
    async def fake_authorized(_env, _node, _ts, _sig, canonical):
        assert canonical.startswith(b"forkmesh-admin-usage-report-v1\n")
        return {"name": "admin"} if authorized else None

    async def fake_detail(_account, _token, _now):
        return detail

    async def fake_d1_all(_env, _sql, *_args):
        return list(d1_rows or [])

    async def fake_d1_first(_env, _sql, *_args):
        return d1_first

    responses = []

    def fake_json_response(body, status=200, cache_control=None):
        responses.append(
            {"body": body, "status": status, "cache_control": cache_control})
        return responses[-1]

    ns = _load(
        "_admin_usage_report", "_usage_report_payload", "_usage_budget_utc",
        extra_globals={
            "parse_qs": __import__("urllib.parse", fromlist=[""]).parse_qs,
            "urlparse": __import__("urllib.parse", fromlist=[""]).urlparse,
            "clean_string": lambda value, _max: str(value or ""),
            "MAX_NODE_NAME": 64,
            "_admin_authorized": fake_authorized,
            "_cloudflare_usage_detail_today": fake_detail,
            "json_response": fake_json_response,
            "d1_all": fake_d1_all,
            "d1_first": fake_d1_first,
            "Date": type("Date", (), {"now": staticmethod(lambda: NOW)}),
        })
    env = type("Env", (), {
        "WORKERS_OBSERVABILITY_ACCOUNT_ID": "acct",
        "USAGE_ANALYTICS_API_TOKEN": "token",
    })()
    request = type("Request", (), {
        "url": "https://forkmesh.com/api/accounts/admin-usage-report"
               "?node=admin&ts=1&sig=x",
    })()
    return ns, env, request, responses


def test_endpoint_requires_an_admin_signature():
    ns, env, request, responses = _report_namespace(False, _detail_fixture())
    asyncio.run(ns["_admin_usage_report"](env, request))
    assert responses[-1]["status"] == 401


def test_endpoint_names_missing_credentials_and_analytics_failures():
    ns, env, request, responses = _report_namespace(True, _detail_fixture())
    env.USAGE_ANALYTICS_API_TOKEN = ""
    asyncio.run(ns["_admin_usage_report"](env, request))
    assert responses[-1]["status"] == 200
    assert responses[-1]["body"] == {
        "ok": False, "configured": False, "error": "analytics_unconfigured"}
    assert "no-store" in responses[-1]["cache_control"]

    ns, env, request, responses = _report_namespace(True, None)
    asyncio.run(ns["_admin_usage_report"](env, request))
    assert responses[-1]["status"] == 502
    assert responses[-1]["body"]["error"] == "analytics_unavailable"


def test_endpoint_attaches_alert_state_and_abort_counters():
    ns, env, request, responses = _report_namespace(
        True, _detail_fixture(),
        d1_rows=[
            {"metric": "do-duration", "day": "2026-08-19",
             "pinged_threshold": 50, "sampled_at": NOW - 300_000},
            {"metric": "do-requests", "day": "2026-08-18",  # stale day
             "pinged_threshold": 90, "sampled_at": NOW - 90_000_000},
        ],
        d1_first={"aborts24h": 7, "duration24h": 5,
                  "aborts60m": 2, "duration60m": 1})
    asyncio.run(ns["_admin_usage_report"](env, request))
    body = responses[-1]["body"]
    assert body["ok"] is True and body["sampledAt"] == NOW
    budgets = {row["metric"]: row for row in body["budgets"]}
    assert budgets["do-duration"]["pingedThreshold"] == 50
    assert budgets["do-duration"]["cronSampledAt"] == NOW - 300_000
    # Yesterday's ladder does not leak into today's meters.
    assert budgets["do-requests"]["pingedThreshold"] == 0
    assert body["aborts"] == {
        "aborts24h": 7, "durationAborts24h": 5,
        "aborts60m": 2, "durationAborts60m": 1}
    assert "no-store" in responses[-1]["cache_control"]


def test_usage_report_route_is_dispatched():
    assert ('if url.path == "/api/accounts/admin-usage-report" '
            'and method == "GET":') in ENTRY_TEXT
    assert "await _admin_usage_report(env, request)" in ENTRY_TEXT
