#!/usr/bin/env python3

import ast
import asyncio
import base64
import hashlib
import importlib.util
import itertools
import json
import re
import sqlite3
import sys
from pathlib import Path
from urllib.parse import parse_qs, urlparse

import pytest


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import cron_activity  # noqa: E402
import dashboard_bundle  # noqa: E402
import dashboard_shell  # noqa: E402
import schema  # noqa: E402
import static_routes  # noqa: E402
import workflow_integrations as integrations  # noqa: E402
import workflow_templates  # noqa: E402
import workflows as engine  # noqa: E402

NOW = 1_790_000_000_000
PLACEHOLDER = re.compile(r"\{([A-Za-z][A-Za-z0-9_]*)\}")


def _literal(path, name):
    for node in ast.parse(path.read_text(encoding="utf-8")).body:
        if isinstance(node, ast.Assign) and any(
                getattr(target, "id", None) == name
                for target in node.targets):
            return ast.literal_eval(node.value)
    raise AssertionError(name)


def _placeholders(value):
    found = set()
    if isinstance(value, dict):
        for item in value.values():
            found |= _placeholders(item)
    elif isinstance(value, list):
        for item in value:
            found |= _placeholders(item)
    elif isinstance(value, str):
        found |= set(PLACEHOLDER.findall(value))
    return found


def _node(node_id, node_type, name, params=None, credential=""):
    return {"id": node_id, "type": node_type, "name": name,
            "params": params or {}, "credential": credential}


def _workflow(nodes, connections):
    workflow, error = engine.clean_workflow(
        {"name": "Test", "nodes": nodes, "connections": connections})
    assert error == ""
    return workflow


class FakeHttp:
    def __init__(self, responses=None):
        self.calls = []
        self.responses = responses or {}

    async def __call__(self, method, url, headers, body):
        self.calls.append({"method": method, "url": url,
                           "headers": headers, "body": body})
        for prefix, answer in self.responses.items():
            if url.startswith(prefix):
                return answer
        return 200, json.dumps({"ok": True})


def _run(workflow, trigger=None, http=None, credentials=None,
         max_requests=25):
    http = http or FakeHttp()
    report = asyncio.run(engine.execute(
        workflow, trigger or {}, http, credentials or {}, max_requests,
        NOW + 60_000, lambda: NOW, NOW, "wf"))
    return report, http


def test_catalog_has_exactly_one_hundred_integrations():
    assert len(integrations.INTEGRATIONS) == 100
    catalog = engine.catalog()
    assert len(catalog["integrations"]) == 100
    json.dumps(catalog)
    assert len(catalog["categories"]) == 12
    assert {"slack", "github", "notion", "stripe", "anthropic", "openai",
            "googleSheets", "hubspot", "airtable", "telegram"} <= set(
        integrations.INTEGRATIONS)


@pytest.mark.parametrize("key", sorted(integrations.INTEGRATIONS))
def test_every_integration_is_well_formed(key):
    app = integrations.INTEGRATIONS[key]
    assert re.fullmatch(r"[a-z][A-Za-z0-9]*", key)
    assert app["name"] and app["description"] and app["category"]
    assert re.fullmatch(r"#[0-9A-Fa-f]{6}", app["color"])
    cred_keys = {field["key"] for field in app["auth"]["fields"]}
    assert len(cred_keys) == len(app["auth"]["fields"])
    assert app["base"].startswith(("https://", "{"))
    assert _placeholders(app["base"]) <= cred_keys
    for part in ("headers", "query", "body"):
        assert _placeholders(app["auth"].get(part) or {}) <= cred_keys
    if app["auth"].get("basic"):
        assert _placeholders(list(app["auth"]["basic"])) <= cred_keys
    for template_base in re.findall(r"^\{([a-z]+)\}", app["base"]):
        field = next(f for f in app["auth"]["fields"]
                     if f["key"] == template_base)
        assert field.get("url"), (key, template_base)
    assert app["operations"]
    for op_key, op in app["operations"].items():
        assert op["method"] in ("GET", "POST", "PUT", "PATCH", "DELETE")
        field_keys = [field["key"] for field in op["fields"]]
        assert len(field_keys) == len(set(field_keys)), (key, op_key)
        assert _placeholders(op["path"]) <= set(field_keys), (key, op_key)
        assert _placeholders(op.get("body")) <= set(field_keys), (key, op_key)
        if op.get("body") is not None:
            assert op["method"] in engine.BODY_METHODS
        for field in op["fields"]:
            assert field["type"] in (
                "string", "text", "number", "boolean", "select", "json")
            if field["type"] == "select":
                assert field["options"]


def test_every_operation_builds_a_safe_request_with_sample_values():
    for key, app in integrations.INTEGRATIONS.items():
        cred = {field["key"]: (
            "https://api.example.com" if field.get("url") else "secret1234")
            for field in app["auth"]["fields"]}
        for op_key, op in app["operations"].items():
            values = {}
            for field in op["fields"]:
                sample = {"number": 3, "boolean": True, "json": {"a": 1}}.get(
                    field["type"], "value")
                if field["type"] == "select":
                    sample = field["options"][-1]["value"]
                values[field["key"]] = sample
            method, url, headers, body = engine.build_app_request(
                key, {"operation": op_key}, values, cred)
            assert engine.safe_url(url), (key, op_key, url)
            assert "{" not in urlparse(url).path, (key, op_key, url)
            if body is not None:
                assert isinstance(body, str)


def test_clean_workflow_rejects_cycles_and_bad_graphs():
    nodes = [_node("a", "core.manual", "Start"),
             _node("b", "core.noop", "One"),
             _node("c", "core.noop", "Two")]
    _, error = engine.clean_workflow({"nodes": nodes, "connections": [
        {"from": "a", "to": "b"}, {"from": "b", "to": "c"},
        {"from": "c", "to": "b"}]})
    assert error == "cycle"
    _, error = engine.clean_workflow({"nodes": nodes + [
        _node("d", "core.webhook", "Hook")], "connections": []})
    assert error == "too_many_triggers"
    _, error = engine.clean_workflow({"nodes": nodes, "connections": [
        {"from": "a", "output": "true", "to": "b"}]})
    assert error == "invalid_connection"
    _, error = engine.clean_workflow({"nodes": nodes, "connections": [
        {"from": "b", "to": "a"}]})
    assert error == "invalid_connection"
    _, error = engine.clean_workflow({"nodes": [
        _node("a", "core.manual", "Same"), _node("b", "core.noop", "Same")],
        "connections": []})
    assert error == "duplicate_node_name"
    _, error = engine.clean_workflow({"nodes": [
        _node("a", "app.nope", "X")], "connections": []})
    assert error == "unknown_node_type"
    _, error = engine.clean_workflow({"nodes": [
        _node("a", "core.set", "X", {"values": "x" * 20001})],
        "connections": []})
    assert error == "value_too_long"
    _, error = engine.clean_workflow({"nodes": [
        _node("n%d" % i, "core.noop", "N%d" % i) for i in range(61)]})
    assert error == "too_many_nodes"


def test_clean_workflow_keeps_only_known_params_and_defaults_operation():
    workflow = _workflow([
        _node("a", "app.slack", "Slack", {
            "operation": "missing", "channel": "#x", "evil": "y"},
            credential="0" * 32)], [])
    params = workflow["nodes"][0]["params"]
    assert params == {"operation": "postMessage", "channel": "#x"}
    workflow = _workflow([_node("a", "core.noop", "N", credential="0" * 32)],
                         [])
    assert workflow["nodes"][0]["credential"] == ""


def test_expressions_resolve_whole_values_and_interpolate_strings():
    context = {"json": {"n": 3, "user": {"name": "Ada"}, "tags": ["a", "b"]},
               "node": {"Fetch": {"json": {"id": 7}}}, "now": "T"}
    assert engine.resolve("{{ $json.n }}", context) == 3
    assert engine.resolve("Hi {{ $json.user.name }}!", context) == "Hi Ada!"
    assert engine.resolve("{{ $json.tags[1] }}", context) == "b"
    assert engine.resolve('{{ $node["Fetch"].json.id }}', context) == 7
    assert engine.resolve("{{ $json.missing }}", context) is None
    assert engine.resolve({"a": ["{{ $json.n }}"]}, context) == {"a": [3]}
    with pytest.raises(engine.NodeError):
        engine.resolve("{{ __import__('os') }}", context)
    with pytest.raises(engine.NodeError):
        engine.resolve("{{ $json.n + 1 }}", context)
    with pytest.raises(engine.NodeError):
        engine.resolve("{{ $env.SECRET }}", context)


def test_if_set_filter_split_limit_and_merge_route_items():
    workflow = _workflow([
        _node("t", "core.manual", "Start"),
        _node("s", "core.split", "Split", {"field": "people"}),
        _node("i", "core.if", "Adult", {
            "left": "{{ $json.age }}", "operator": "gte", "right": "18"}),
        _node("y", "core.set", "Mark adult", {
            "values": '{"group": "adult", "label": "{{ $json.name }}!"}'}),
        _node("n", "core.set", "Mark minor", {
            "values": {"group": "minor"}, "keep": "false"}),
        _node("m", "core.merge", "Merge"),
        _node("f", "core.filter", "Only A", {
            "left": "{{ $json.label }}", "operator": "startsWith",
            "right": "A"}),
        _node("l", "core.limit", "First", {"max": "1"}),
    ], [
        {"from": "t", "to": "s"}, {"from": "s", "to": "i"},
        {"from": "i", "output": "true", "to": "y"},
        {"from": "i", "output": "false", "to": "n"},
        {"from": "y", "to": "m"}, {"from": "n", "to": "m"},
        {"from": "m", "to": "f"}, {"from": "f", "to": "l"},
    ])
    report, http = _run(workflow, {"people": [
        {"name": "Ada", "age": 36}, {"name": "Bo", "age": 9},
        {"name": "Al", "age": 20}]})
    assert report["status"] == "success"
    assert http.calls == []
    nodes = report["nodes"]
    assert nodes["i"]["outputs"]["true"]["count"] == 2
    assert nodes["i"]["outputs"]["false"]["count"] == 1
    assert nodes["n"]["outputs"]["main"]["items"] == [{"group": "minor"}]
    assert nodes["m"]["outputs"]["main"]["count"] == 3
    assert nodes["f"]["outputs"]["main"]["count"] == 2
    assert nodes["l"]["outputs"]["main"]["items"] == [
        {"name": "Ada", "age": 36, "group": "adult", "label": "Ada!"}]


def test_branch_without_items_is_skipped():
    workflow = _workflow([
        _node("t", "core.manual", "Start"),
        _node("i", "core.if", "Check", {
            "left": "{{ $json.ok }}", "operator": "isTrue"}),
        _node("y", "core.noop", "Yes"), _node("n", "core.noop", "No")], [
        {"from": "t", "to": "i"},
        {"from": "i", "output": "true", "to": "y"},
        {"from": "i", "output": "false", "to": "n"}])
    report, _ = _run(workflow, {"ok": True})
    assert report["nodes"]["y"]["status"] == "success"
    assert report["nodes"]["n"]["status"] == "skipped"


def test_app_step_calls_api_with_credential_and_extracts_claude_text():
    workflow = _workflow([
        _node("t", "core.manual", "Start"),
        _node("c", "app.anthropic", "Claude", {
            "operation": "message", "prompt": "Summarize {{ $json.text }}"},
            credential="a" * 32),
        _node("s", "app.slack", "Slack", {
            "operation": "postMessage", "channel": "#news",
            "text": '{{ $node["Claude"].json.text }}'},
            credential="b" * 32),
    ], [{"from": "t", "to": "c"}, {"from": "c", "to": "s"}])
    http = FakeHttp({"https://api.anthropic.com": (200, json.dumps({
        "content": [{"type": "thinking", "thinking": ""},
                    {"type": "text", "text": "Short."}]}))})
    report, http = _run(workflow, {"text": "long"}, http, {
        "a" * 32: {"integration": "anthropic", "values": {"apiKey": "sk-1"}},
        "b" * 32: {"integration": "slack", "values": {"token": "xoxb-1"}},
    })
    assert report["status"] == "success"
    claude, slack = http.calls
    assert claude["url"] == "https://api.anthropic.com/v1/messages"
    assert claude["headers"]["x-api-key"] == "sk-1"
    assert claude["headers"]["anthropic-version"] == "2023-06-01"
    body = json.loads(claude["body"])
    assert body["model"] == "claude-opus-5-5"
    assert body["messages"] == [{"role": "user", "content": "Summarize long"}]
    assert "system" not in body
    assert slack["headers"]["Authorization"] == "Bearer xoxb-1"
    assert json.loads(slack["body"]) == {"channel": "#news", "text": "Short."}


def test_missing_or_foreign_credential_fails_the_step():
    workflow = _workflow([
        _node("t", "core.manual", "Start"),
        _node("s", "app.slack", "Slack", {
            "operation": "postMessage", "channel": "#x", "text": "hi"},
            credential="b" * 32)], [{"from": "t", "to": "s"}])
    report, http = _run(workflow, {}, None, {
        "b" * 32: {"integration": "discord", "values": {"apiKey": "x"}}})
    assert report["status"] == "error"
    assert "credential" in report["nodes"]["s"]["error"]
    assert http.calls == []


def test_http_errors_stop_the_run_unless_continue_on_fail():
    nodes = [
        _node("t", "core.manual", "Start"),
        dict(_node("h", "core.http", "Call", {
            "url": "https://api.example.com/x"})),
        _node("n", "core.noop", "After")]
    connections = [{"from": "t", "to": "h"}, {"from": "h", "to": "n"}]
    http = FakeHttp({"https://api.example.com": (500, "boom")})
    report, _ = _run(_workflow(nodes, connections), {}, http)
    assert report["status"] == "error"
    assert report["nodes"]["h"]["error"] == "HTTP 500: boom"
    assert report["nodes"]["n"]["status"] == "skipped"
    nodes[1]["continueOnFail"] = True
    report, _ = _run(_workflow(nodes, connections), {}, FakeHttp(
        {"https://api.example.com": (500, "boom")}))
    assert report["status"] == "success"
    assert report["nodes"]["n"]["outputs"]["main"]["items"] == [
        {"error": "HTTP 500: boom"}]


def test_request_limit_and_unsafe_urls_are_enforced():
    workflow = _workflow([
        _node("t", "core.manual", "Start"),
        _node("s", "core.split", "Split", {"field": "urls"}),
        _node("h", "core.http", "Call", {"url": "{{ $json.value }}"})], [
        {"from": "t", "to": "s"}, {"from": "s", "to": "h"}])
    report, http = _run(workflow, {"urls": ["https://a.example.com"] * 5},
                        max_requests=3)
    assert report["status"] == "error"
    assert "limit of 3 requests" in report["nodes"]["h"]["error"]
    assert len(http.calls) == 3
    for url in ("http://a.example.com", "https://127.0.0.1/",
                "https://localhost/", "https://app.forkmesh.com/api",
                "https://user:pw@a.example.com/", "https://10.0.0.1/",
                "file:///etc/passwd", "https://metadata/"):
        report, http = _run(workflow, {"urls": [url]})
        assert report["status"] == "error", url
        assert http.calls == [], url


def test_http_node_builds_query_headers_and_form_bodies():
    method, url, headers, body = engine.build_http_request({
        "method": "POST", "url": "https://api.example.com/x?a=1",
        "headers": {"X-Key": "k"}, "query": {"b": 2},
        "bodyType": "form", "body": {"name": "Ada", "tags": ["x", "y"]}})
    assert method == "POST"
    assert url == "https://api.example.com/x?a=1&b=2"
    assert headers["X-Key"] == "k"
    assert headers["Content-Type"] == "application/x-www-form-urlencoded"
    assert parse_qs(body) == {"name": ["Ada"], "tags": ["x", "y"]}


def test_auth_styles_build_the_documented_requests():
    _, url, headers, body = engine.build_app_request(
        "twilio", {"operation": "sendSms"},
        {"To": "+1", "From": "+2", "Body": "hi"},
        {"accountSid": "AC1", "authToken": "tok"})
    assert url == "https://api.twilio.com/2010-04-01/Accounts/AC1/Messages.json"
    assert headers["Authorization"] == "Basic " + base64.b64encode(
        b"AC1:tok").decode()
    assert parse_qs(body) == {"To": ["+1"], "From": ["+2"], "Body": ["hi"]}

    _, url, _, _ = engine.build_app_request(
        "trello", {"operation": "listCards"}, {"listId": "L1"},
        {"key": "k", "token": "t"})
    assert parse_qs(urlparse(url).query) == {"key": ["k"], "token": ["t"]}

    _, _, _, body = engine.build_app_request(
        "pushover", {"operation": "notify"}, {"message": "hi"},
        {"token": "app", "user": "usr"})
    assert json.loads(body) == {"message": "hi", "token": "app", "user": "usr"}

    _, url, headers, _ = engine.build_app_request(
        "ntfy", {"operation": "publish"}, {"topic": "t", "message": "m"},
        {"server": "https://ntfy.sh", "token": ""})
    assert url == "https://ntfy.sh"
    assert "Authorization" not in headers

    _, url, _, _ = engine.build_app_request(
        "circleci", {"operation": "triggerPipeline"},
        {"projectSlug": "gh/org/repo"}, {"apiKey": "c"})
    assert url == "https://circleci.com/api/v2/project/gh/org/repo/pipeline"

    _, url, _, _ = engine.build_app_request(
        "supabase", {"operation": "select"},
        {"table": "users", "select": "*", "filters": {"id": "eq.1"},
         "limit": 5}, {"url": "https://abc.supabase.co", "apiKey": "k"})
    assert parse_qs(urlparse(url).query) == {
        "select": ["*"], "id": ["eq.1"], "limit": ["5"]}

    _, url, _, _ = engine.build_app_request(
        "github", {"operation": "listIssues"},
        {"owner": "a/b", "repo": "c", "state": "open"}, {"token": "g"})
    assert url == "https://api.github.com/repos/a%2Fb/c/issues?state=open"

    with pytest.raises(engine.NodeError):
        engine.build_app_request(
            "github", {"operation": "listIssues"},
            {"owner": "", "repo": "c"}, {"token": "g"})


def test_credentials_validate_urls_and_hide_secrets():
    values, error = engine.credential_values(
        "supabase", {"url": "http://10.0.0.1", "apiKey": "k"})
    assert error == "invalid_credential_url"
    values, error = engine.credential_values("slack", {})
    assert error == "missing_credential_field"
    values, error = engine.credential_values(
        "ntfy", {"server": "https://ntfy.sh"})
    assert error == "" and values == {"server": "https://ntfy.sh", "token": ""}
    hint = engine.credential_hint("slack", {"token": "xoxb-123456789"})
    assert hint == {"token": "••••6789"}
    assert engine.credential_hint("trello", {"key": "abc", "token": "t"}) == {
        "key": "abc", "token": "••••"}


def test_schedule_minutes_are_clamped_and_need_a_schedule_trigger():
    def minutes(every):
        return engine.schedule_minutes(_workflow([
            _node("t", "core.schedule", "Every", {"every": every})], []))
    assert minutes("1") == 5
    assert minutes("60") == 60
    assert minutes("999999") == 10080
    assert minutes("") == 0
    assert engine.schedule_minutes(_workflow([
        _node("t", "core.manual", "Start")], [])) == 0


class FakeResponse:
    def __init__(self, data, status, headers):
        self.data = data
        self.status = status
        self.headers = headers


class FakeRequest:
    def __init__(self, method="GET", body=None, user="", url="", raw=""):
        self.method = method
        self.body = body
        self.user = user
        self.url = url or "https://app.example.com/api/workflows"
        self.raw = raw


class FakeDate:
    now_ms = NOW

    @classmethod
    def now(cls):
        return cls.now_ms


class FakeFetchResponse:
    def __init__(self, status, text):
        self.status = status
        self._text = text

    async def text(self):
        return self._text


class Harness:
    def __init__(self):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        entry = SRC / "entry.py"
        for sql in (_literal(entry, "SCHEMA_PRE_CREATE_ALTER_STATEMENTS")
                    + schema.SCHEMA_STATEMENTS
                    + _literal(entry, "SCHEMA_ALTER_STATEMENTS")):
            try:
                self.db.execute(sql)
            except sqlite3.Error:
                pass
        self.accounts = {}
        self.ids = itertools.count(1)
        self.fetches = []
        self.signed = []
        self.fetch_answer = (200, '{"ok": true}')
        spec = importlib.util.spec_from_file_location(
            "workflows_runtime_under_test", SRC / "workflows_runtime.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        harness = self

        async def d1_all(_env, sql, *args):
            return [dict(row) for row in harness.db.execute(sql, args)]

        async def d1_first(_env, sql, *args):
            row = harness.db.execute(sql, args).fetchone()
            return dict(row) if row else None

        async def d1_run(_env, sql, *args):
            harness.db.execute(sql, args)

        async def account_session_record(_env, request, _data=None):
            rec = harness.accounts.get(request.user)
            return (request.user + "-bi", rec) if rec else ("", None)

        async def bounded_json_request(request, _limit):
            if request.body == "broken":
                raise ValueError("bad json")
            return request.body

        async def bounded_request_text(request, _limit):
            if request.raw or request.body is None:
                return request.raw
            if isinstance(request.body, str):
                return request.body
            return json.dumps(request.body)

        async def plugin_scope_signed_session(_env, request, digest, proof,
                                              prefix):
            params = parse_qs(urlparse(request.url).query)
            node = params.get("node", [""])[0]
            harness.signed.append((request.method, digest, proof, prefix))
            rec = harness.accounts.get(node)
            if params.get("sig", [""])[0] != "valid" or not rec:
                return "", None
            return node + "-bi", rec

        async def encrypt_row(_env, obj):
            return "enc:" + json.dumps(obj)

        async def decrypt_row(_env, stored):
            if not str(stored or "").startswith("enc:"):
                return None
            return json.loads(stored[4:])

        async def js_fetch_with_timeout(url, init, _timeout):
            harness.fetches.append((url, init))
            return FakeFetchResponse(*harness.fetch_answer)

        def json_response(data, status=200, cache_seconds=None,
                          cache_control=None, extra_headers=None):
            return FakeResponse(data, status, dict(extra_headers or {}))

        module._bind_runtime({
            "Date": FakeDate,
            "d1_all": d1_all,
            "d1_first": d1_first,
            "d1_run": d1_run,
            "_account_session_record": account_session_record,
            "_plugin_scope_signed_session": plugin_scope_signed_session,
            "_account_kind": lambda rec: rec.get("kind", "user"),
            "bounded_json_request": bounded_json_request,
            "bounded_request_text": bounded_request_text,
            "encrypt_row": encrypt_row,
            "decrypt_row": decrypt_row,
            "js_fetch_with_timeout": js_fetch_with_timeout,
            "json_response": json_response,
            "method_name": lambda request: request.method,
            "_ap_uuid": lambda: "%032x" % next(harness.ids),
        })
        self.runtime = module

    def user(self, name, kind="user", status="active"):
        self.accounts[name] = {"name": name, "kind": kind, "status": status}

    def call(self, path, method="GET", body=None, user="", url="", raw=""):
        return asyncio.run(self.runtime.api_handler(
            None, FakeRequest(method, body, user, url, raw), path))


@pytest.fixture(autouse=True)
def _clock():
    FakeDate.now_ms = NOW
    yield
    FakeDate.now_ms = NOW


@pytest.fixture
def harness():
    h = Harness()
    h.user("alice")
    h.user("bob")
    return h


def _definition(trigger="core.manual", trigger_params=None, active=False):
    return {
        "name": "Notify",
        "active": active,
        "nodes": [
            _node("t", trigger, "Trigger", trigger_params or {}),
            _node("h", "core.http", "Call", {
                "method": "POST", "url": "https://hooks.example.com/x",
                "body": '{"who": "{{ $json.body.name }}"}'}),
        ],
        "connections": [{"from": "t", "to": "h"}],
    }


def test_api_requires_a_user_session(harness):
    harness.user("node1", kind="node")
    assert harness.call("/api/workflows").status == 401
    assert harness.call("/api/workflows", user="node1").status == 401
    assert harness.call("/api/workflows/catalog").status == 200
    response = harness.call("/api/workflows", "POST", "broken", user="alice")
    assert response.data["error"] == "invalid_json"


def test_desktop_signed_requests_reach_the_same_workflows(harness):
    created = harness.call("/api/workflows", "POST", _definition(),
                           user="alice").data["workflow"]
    signed = "https://app.example.com/api/workflows?node=alice&ts=1&sig=valid"
    listed = harness.call("/api/workflows", url=signed)
    assert listed.status == 200
    assert created["id"] in [item["id"] for item in listed.data["workflows"]]
    assert harness.signed[-1] == ("GET", "", "forkmesh-workflows-request-v1",
                                  "/api/workflows")
    raw = json.dumps(dict(_definition(), name="From desktop"),
                     separators=(",", ":"))
    made = harness.call("/api/workflows", "POST", url=signed, raw=raw)
    assert made.status == 201
    assert made.data["workflow"]["name"] == "From desktop"
    assert harness.signed[-1][:2] == (
        "POST", hashlib.sha256(raw.encode()).hexdigest())
    forged = signed.replace("sig=valid", "sig=forged")
    assert harness.call("/api/workflows", url=forged).status == 401
    harness.user("node1", kind="node")
    node = signed.replace("node=alice", "node=node1")
    assert harness.call("/api/workflows", url=node).status == 401


def test_create_list_update_and_delete_are_owner_scoped(harness):
    created = harness.call("/api/workflows", "POST", _definition(),
                           user="alice")
    assert created.status == 201
    workflow = created.data["workflow"]
    assert workflow["name"] == "Notify"
    assert re.fullmatch(r"/api/workflows/hook/[0-9a-f]{32}/[0-9a-f]{64}",
                        workflow["hookPath"])
    path = "/api/workflows/" + workflow["id"]
    listed = harness.call("/api/workflows", user="alice").data["workflows"]
    assert [item["id"] for item in listed if item["name"] != "Free disk space"] \
        == [workflow["id"]]
    assert [item["name"] for item in harness.call(
        "/api/workflows", user="bob").data["workflows"]] == ["Free disk space"]
    assert harness.call(path, user="bob").status == 404
    assert harness.call(path, "POST", _definition(), user="bob").status == 404
    stored = harness.db.execute("SELECT data FROM workflows").fetchone()[0]
    assert stored.startswith("enc:")
    updated = harness.call(path, "POST", dict(_definition(), name="Renamed"),
                           user="alice")
    assert updated.data["workflow"]["name"] == "Renamed"
    assert updated.data["workflow"]["hookPath"] == workflow["hookPath"]
    bad = dict(_definition(), connections=[{"from": "h", "to": "h"}])
    assert harness.call(path, "POST", bad, user="alice").data["error"] == \
        "invalid_connection"
    assert harness.call(path + "/delete", "POST", {}, user="bob").status == 404
    assert harness.call(path + "/delete", "POST", {}, user="alice").status \
        == 200
    assert harness.call(path, user="alice").status == 404


def test_manual_run_records_an_execution(harness):
    created = harness.call("/api/workflows", "POST", _definition(
        trigger_params={"payload": '{"body": {"name": "Ada"}}'}),
        user="alice").data["workflow"]
    path = "/api/workflows/" + created["id"]
    response = harness.call(path + "/run", "POST", {}, user="alice")
    run = response.data["run"]
    assert run["status"] == "success"
    assert run["trigger"] == "manual"
    url, init = harness.fetches[0]
    assert url == "https://hooks.example.com/x"
    assert json.loads(init["body"]) == {"who": "Ada"}
    assert init["headers"]["User-Agent"].startswith("ForkMesh-Workflows")
    runs = harness.call(path + "/runs", user="alice").data["runs"]
    assert [item["id"] for item in runs] == [run["id"]]
    detail = harness.call(path + "/runs/" + run["id"], user="alice")
    assert detail.data["run"]["nodes"]["h"]["status"] == "success"
    assert harness.call(path + "/runs/" + run["id"], user="bob").status == 404
    listed = harness.call("/api/workflows", user="alice").data["workflows"]
    assert {item["id"]: item for item in listed}[created["id"]]["lastStatus"] \
        == "success"


def test_runs_are_rate_limited_and_pruned(harness):
    created = harness.call("/api/workflows", "POST", _definition(),
                           user="alice").data["workflow"]
    path = "/api/workflows/" + created["id"] + "/run"
    for _ in range(harness.runtime.RUNS_KEPT + 3):
        FakeDate.now_ms += 1
        assert harness.call(path, "POST", {}, user="alice").status == 200
    kept = harness.db.execute("SELECT COUNT(*) FROM workflow_runs").fetchone()
    assert kept[0] == harness.runtime.RUNS_KEPT
    for i in range(harness.runtime.RUNS_PER_HOUR):
        harness.db.execute(
            "INSERT INTO workflow_runs VALUES (?,?,?,?,?,?,?,?)",
            ("r%d" % i, "other", "alice-bi", "manual", "success", NOW + 5,
             NOW + 5, "enc:{}"))
    assert harness.call(path, "POST", {}, user="alice").data["error"] == \
        "rate_limited"


def test_webhook_runs_only_when_active_with_the_right_token(harness):
    created = harness.call("/api/workflows", "POST",
                           _definition("core.webhook"),
                           user="alice").data["workflow"]
    hook = created["hookPath"]
    raw = json.dumps({"name": "Bea"})
    response = harness.call(hook, "POST", raw=raw)
    assert response.status == 409
    harness.call("/api/workflows/" + created["id"], "POST",
                 _definition("core.webhook", active=True), user="alice")
    assert harness.call(hook[:-1] + "0", "POST", raw=raw).status == 404
    response = harness.call(
        hook, "POST", raw=raw,
        url="https://app.example.com" + hook + "?source=test")
    assert response.data["status"] == "success"
    assert json.loads(harness.fetches[-1][1]["body"]) == {"who": "Bea"}
    run = harness.db.execute(
        "SELECT trigger_kind FROM workflow_runs").fetchone()[0]
    assert run == "webhook"
    assert harness.call(hook, "DELETE").status == 405


def test_schedules_run_when_due_and_reschedule(harness):
    created = harness.call("/api/workflows", "POST", _definition(
        "core.schedule", {"every": "15"}, active=True),
        user="alice").data["workflow"]
    row = harness.db.execute(
        "SELECT schedule_minutes,next_run_at FROM workflows").fetchone()
    assert tuple(row) == (15, NOW + 15 * 60_000)
    assert asyncio.run(harness.runtime.run_due_schedules(None)) == {"ran": 0}
    FakeDate.now_ms = NOW + 15 * 60_000
    assert asyncio.run(harness.runtime.run_due_schedules(None)) == {"ran": 1}
    row = harness.db.execute(
        "SELECT next_run_at,last_status FROM workflows").fetchone()
    assert tuple(row) == (NOW + 30 * 60_000, "success")
    harness.call("/api/workflows/" + created["id"], "POST", dict(
        _definition("core.schedule", {"every": "15"}), active=False),
        user="alice")
    row = harness.db.execute(
        "SELECT schedule_minutes,next_run_at FROM workflows").fetchone()
    assert tuple(row) == (0, 0)


def test_credentials_are_stored_encrypted_and_never_returned(harness):
    response = harness.call("/api/workflows/credentials", "POST", {
        "integration": "slack", "name": "Team",
        "values": {"token": "xoxb-secret-9999"}}, user="alice")
    assert response.status == 201
    credential = response.data["credential"]
    assert credential["hint"] == {"token": "••••9999"}
    assert "xoxb-secret-9999" not in json.dumps(response.data)
    stored = harness.db.execute(
        "SELECT data FROM workflow_credentials").fetchone()[0]
    assert stored.startswith("enc:")
    listed = harness.call("/api/workflows/credentials", user="alice")
    assert "xoxb-secret" not in json.dumps(listed.data)
    assert harness.call("/api/workflows/credentials", user="bob").data[
        "credentials"] == []
    definition = {"name": "S", "nodes": [
        _node("t", "core.manual", "Start"),
        _node("s", "app.slack", "Slack", {
            "operation": "postMessage", "channel": "#c", "text": "hi"},
            credential=credential["id"])],
        "connections": [{"from": "t", "to": "s"}]}
    bob = harness.call("/api/workflows", "POST", definition,
                       user="bob").data["workflow"]
    run = harness.call("/api/workflows/" + bob["id"] + "/run", "POST", {},
                       user="bob").data["run"]
    assert run["status"] == "error"
    assert harness.fetches == []
    alice = harness.call("/api/workflows", "POST", definition,
                         user="alice").data["workflow"]
    run = harness.call("/api/workflows/" + alice["id"] + "/run", "POST", {},
                       user="alice").data["run"]
    assert run["status"] == "success"
    assert harness.fetches[0][1]["headers"]["Authorization"] == \
        "Bearer xoxb-secret-9999"
    path = "/api/workflows/credentials/" + credential["id"] + "/delete"
    assert harness.call(path, "POST", {}, user="bob").status == 404
    assert harness.call(path, "POST", {}, user="alice").status == 200
    assert harness.call("/api/workflows/credentials", "POST", {
        "integration": "wikipedia", "values": {}},
        user="alice").data["error"] == "no_credential_needed"


def test_workflows_is_a_marketplace_plugin_with_nested_packs():
    sys.path.insert(0, str(ROOT / "tools"))
    sys.path.insert(0, str(ROOT.parent / "marketplace" / "tools"))
    import build_app_plugins
    packages = ROOT.parent / "marketplace" / "packages"
    assert "workflows" not in dashboard_shell.PAGES
    assert dashboard_shell.CHUNK_PAGES["workflows"] == "workflows"
    assert dashboard_bundle.LAZY_CHUNKS["workflows"] == "09-workflows.js"
    assert "dashboard-page-workflows.js" in dashboard_shell.CACHE_BUSTED_ASSETS
    assert "/dashboard/workflows" not in static_routes.DASHBOARD_PAGE_ASSETS
    entries = {row["item"].package_id: row
               for row in build_app_plugins._web_entries()}
    assert entries["workflows"]["route"] == "/dashboard/workflows"
    manifest = json.loads((packages / "workflows" / "manifest.json")
                          .read_text(encoding="utf-8"))
    assert manifest["entrypoints"]["worker"] == "builtin:workflows"
    assert manifest["entrypoints"]["desktop"] == "builtin:workflows"
    assert manifest["install"] == {"bundled": True, "default": False,
                                   "sync": "account",
                                   "installedBefore": "2026-10-14"}
    boot = (ROOT / "public" / "dashboard" / "js" / "10-boot.js").read_text(
        encoding="utf-8")
    assert '"tasks", "calendar", "notes", "workflows"' in boot
    script = (packages / "workflows" / "content" / "web" / "page.js"
              ).read_text(encoding="utf-8")
    assert 'builtinPage("workflows")' in script
    sidebar = (ROOT / "public" / "dashboard" / "partials" /
               "sidebar.html").read_text(encoding="utf-8")
    assert 'href="/dashboard/workflows"' not in sidebar
    phases = {row[0] for row in cron_activity.CRON_PHASE_CATALOG}
    assert "workflow-schedules" in phases
    children = {}
    for folder in packages.iterdir():
        path = folder / "manifest.json"
        if not path.is_file():
            continue
        data = json.loads(path.read_text(encoding="utf-8"))
        if any(d.get("id") == "workflows" for d in data.get("dependencies") or []):
            children[data["id"]] = data
    for pack in engine.packs():
        child = children[pack["id"]]
        contribution = child["contributions"][0]
        assert contribution["socket"] == "workflows.integrations"
        assert contribution["config"]["integrations"] == pack["integrations"]
        assert contribution["config"]["category"] == pack["category"]
        assert child["install"]["sync"] == "account"
        pack_file = json.loads((packages / pack["id"] / "content" / "pack.json")
                               .read_text(encoding="utf-8"))
        assert pack_file["integrations"] == pack["integrations"]
    examples = children[engine.EXAMPLES_PACKAGE]["contributions"][0]
    assert examples["config"]["examples"] == [
        t["key"] for t in workflow_templates.TEMPLATES]
    assert engine.FILES_PACKAGE in children
    assert len(children) == len(engine.packs()) + 2
    covered = [key for pack in engine.packs() for key in pack["integrations"]]
    assert sorted(covered) == sorted(integrations.INTEGRATIONS)


def test_plugin_ships_valid_repository_files():
    sys.path.insert(0, str(ROOT / "tools"))
    sys.path.insert(0, str(ROOT.parent / "marketplace" / "tools"))
    import build_app_plugins
    import workflow_files
    root = ROOT.parent / "marketplace" / "packages" / "workflows"
    manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
    shipped = build_app_plugins.content_files(manifest, root)
    assert {item["kind"] for item in shipped} == {
        "action", "shortcut", "skill", "agent"}
    for item in shipped:
        document, warnings = workflow_files.parse_file(item["path"],
                                                       item["text"])
        text, _ = workflow_files.render_file(document, 0)
        assert text == item["text"], item["path"]
        assert warnings == []


def test_templates_and_catalog_are_public(harness):
    listed = harness.call("/api/workflows/templates")
    assert listed.status == 200 and len(listed.data["templates"]) == 12
    key = listed.data["templates"][0]["key"]
    one = harness.call("/api/workflows/templates/" + key)
    assert one.data["template"]["workflow"]["nodes"]
    assert harness.call("/api/workflows/templates/nope").status == 404
    catalog = harness.call("/api/workflows/catalog").data
    assert set(catalog["files"]["kinds"]) == {
        "action", "shortcut", "skill", "agent"}


def test_convert_export_and_import_url(harness):
    template = harness.call("/api/workflows/templates/uptime-alert").data[
        "template"]["workflow"]
    exported = harness.call("/api/workflows/export", "POST", template,
                            user="alice")
    assert exported.data["filename"] == "website-uptime-alert.json"
    document = exported.data["document"]
    converted = harness.call("/api/workflows/convert", "POST",
                             {"document": json.dumps(document)},
                             user="alice")
    assert converted.data["warnings"] == []
    assert len(converted.data["workflow"]["nodes"]) == 5
    assert harness.call("/api/workflows/convert", "POST",
                        {"document": "{}"}, user="alice").data["error"] == \
        "not_a_workflow"
    assert harness.call("/api/workflows/convert", "POST",
                        {"document": "x"}).status == 401
    harness.fetch_answer = (200, json.dumps({"workflow": {"workflow":
                                                          document}}))
    imported = harness.call("/api/workflows/import-url", "POST", {
        "url": "https://n8n.io/workflows/42-uptime"}, user="alice")
    assert imported.data["source"] == \
        "https://api.n8n.io/api/templates/workflows/42"
    assert harness.fetches[-1][0] == imported.data["source"]
    for url, error in (("http://example.com/x.json", "invalid_import_url"),
                       ("https://127.0.0.1/x.json", "invalid_import_url")):
        assert harness.call("/api/workflows/import-url", "POST", {
            "url": url}, user="alice").data["error"] == error
    harness.fetch_answer = (404, "")
    assert harness.call("/api/workflows/import-url", "POST", {
        "url": "https://example.com/x.json"}, user="alice").data[
        "error"] == "import_fetch_failed"
    rewrite = harness.runtime.import_source
    assert rewrite("https://github.com/o/r/blob/main/a/b.json") == \
        "https://raw.githubusercontent.com/o/r/main/a/b.json"
    assert rewrite("https://gist.github.com/u/abc123") == \
        "https://gist.githubusercontent.com/u/abc123/raw"


def test_repository_file_routes(harness):
    parsed = harness.call("/api/workflows/files/parse", "POST", {
        "path": ".forkmesh/shortcuts/hi.md",
        "text": "# name: Hi\n# description: Say hi\n\nSay hi.\n"},
        user="alice")
    document = parsed.data["document"]
    assert document["kind"] == "shortcut"
    rendered = harness.call("/api/workflows/files/render", "POST",
                            {"document": document}, user="alice")
    assert rendered.data["text"] == \
        "# name: Hi\n# description: Say hi\n\nSay hi.\n"
    created = harness.call("/api/workflows/files/new", "POST", {
        "kind": "agent", "path": ".forkmesh/teammates/ops.json"},
        user="alice")
    assert created.data["document"]["nodes"][0]["params"]["id"] == "ops"
    bad = harness.call("/api/workflows/files/parse", "POST", {
        "path": "../x", "text": ""}, user="alice")
    assert bad.data["error"] == "unsupported_path"
    assert harness.call("/api/workflows/files/parse", "POST", {
        "path": ".forkmesh/a.yml", "text": "x"}).status == 401



def test_plugin_install_state_gates_the_api(harness, monkeypatch):
    import plugin_data
    registry = {
        "workflows": {"accountSync": True, "default": True,
                      "dependencies": (), "settings": (), "files": ({
                          "kind": "shortcut", "path": ".forkmesh/shortcuts/a.md",
                          "name": "A", "text": "# name: A\n"},)},
        "workflows-repository-files": {
            "accountSync": True, "default": True,
            "dependencies": ("workflows",), "settings": (), "files": ()},
    }
    monkeypatch.setattr(plugin_data, "REGISTRY", registry)
    monkeypatch.setattr(plugin_data, "_ACCOUNT_SCHEMA_READY", False)
    harness.db.execute("""CREATE TABLE IF NOT EXISTS account_package_installs (
        account_bi TEXT NOT NULL, package TEXT NOT NULL,
        installed INTEGER NOT NULL, settings TEXT NOT NULL DEFAULT '{}',
        updated_at INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (account_bi, package))""")
    files = harness.call("/api/workflows/package-files", user="alice")
    assert files.data["files"][0]["path"] == ".forkmesh/shortcuts/a.md"
    created = harness.call("/api/workflows", "POST", _definition(
        "core.webhook", active=True), user="alice").data["workflow"]
    harness.db.execute(
        "INSERT INTO account_package_installs(account_bi,package,installed,"
        "settings,updated_at) VALUES (?,?,?,?,?)",
        ("alice-bi", "workflows-repository-files", 0, "{}", 1))
    blocked = harness.call("/api/workflows/files/parse", "POST", {
        "path": ".forkmesh/a.yml", "text": "x"}, user="alice")
    assert blocked.status == 403
    assert blocked.data["plugin"] == "workflows-repository-files"
    assert harness.call("/api/workflows", user="alice").status == 200
    harness.db.execute(
        "INSERT INTO account_package_installs(account_bi,package,installed,"
        "settings,updated_at) VALUES (?,?,?,?,?)",
        ("alice-bi", "workflows", 0, "{}", 1))
    off = harness.call("/api/workflows", user="alice")
    assert off.status == 403 and off.data["error"] == "plugin_not_installed"
    assert harness.call(created["hookPath"], "POST", raw="{}").status == 409
    assert harness.call("/api/workflows/catalog").status == 200


def test_run_workflow_steps_load_the_owners_own_workflows(harness):
    child = harness.call("/api/workflows", "POST", {"name": "Double it", "nodes": [
        _node("t", "core.manual", "Start"),
        _node("c", "core.code", "Script", {"code": "return items.map(i => ({n: i.n * 2}))"}),
    ], "connections": [{"from": "t", "to": "c"}]}, user="alice").data["workflow"]
    harness.call("/api/workflows", "POST", {"name": "Bob's", "nodes": [
        _node("t", "core.manual", "Start")], "connections": []}, user="bob")
    parent = harness.call("/api/workflows", "POST", {"name": "Parent", "nodes": [
        _node("t", "core.manual", "Start", {"payload": '{"n": 21}'}),
        _node("r", "core.runWorkflow", "By name", {"workflow": "double IT"}),
        _node("i", "core.runWorkflow", "By id", {"workflow": child["id"]}),
        _node("b", "core.runWorkflow", "Someone else's", {"workflow": "Bob's"}),
    ], "connections": [{"from": "t", "to": "r"}, {"from": "r", "to": "i"},
                       {"from": "i", "to": "b"}]}, user="alice").data["workflow"]
    run = harness.call("/api/workflows/" + parent["id"] + "/run", "POST", {},
                       user="alice").data["run"]
    assert run["nodes"]["r"]["outputs"]["main"]["items"] == [{"n": 42}]
    assert run["nodes"]["i"]["outputs"]["main"]["items"] == [{"n": 84}]
    assert run["error"] == "Someone else's: No workflow named Bob's"
