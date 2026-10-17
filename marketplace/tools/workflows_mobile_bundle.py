#!/usr/bin/env python3
from __future__ import annotations

import argparse
import asyncio
import copy
import hashlib
import json
from pathlib import Path
import sys

REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
APP_SRC = REPOSITORY_ROOT / "app" / "src"
if str(APP_SRC) not in sys.path:
    sys.path.insert(0, str(APP_SRC))

import workflow_integrations as integrations  # noqa: E402
import workflow_templates as templates  # noqa: E402
import workflows as engine  # noqa: E402
import workflows_runtime as runtime  # noqa: E402

PACKAGE = REPOSITORY_ROOT / "marketplace" / "packages" / "workflows"
ENGINE = PACKAGE / "content" / "mobile" / "workflow-engine.js"
IOS_RESOURCES = REPOSITORY_ROOT / "native" / "ios" / "ForkMeshCore" / "Sources" / "FMWorkflows" / "Resources"
IOS_FIXTURES = REPOSITORY_ROOT / "native" / "ios" / "ForkMeshCore" / "Tests" / "FMWorkflowsTests" / "Fixtures"
ANDROID_RESOURCES = REPOSITORY_ROOT / "native" / "android" / "core" / "src" / "main" / "resources" / "workflows"
ANDROID_FIXTURES = REPOSITORY_ROOT / "native" / "android" / "core" / "src" / "test" / "resources" / "workflows"
SPEC_NAME = "workflow-spec.json"
CASES_NAME = "workflow-cases.json"
ENGINE_NAME = "workflow-engine.js"
SCHEMA = "forkmesh-workflows-mobile-v1"
NOW_MS = 1767225600000
WORKFLOW_ID = "0123456789abcdef0123456789abcdef"
CREDENTIAL_ID = "c" * 32
OTHER_CREDENTIAL_ID = "d" * 32
OK_RESPONSE = {"status": 200, "text": "{\"ok\": true, \"id\": 42, \"items\": [{\"n\": 1}, {\"n\": 2}]}"}
CHAT_RESPONSES = {
    "anthropic": {"content": [{"type": "text", "text": "Hello "}, {"type": "tool_use"}, {"text": "Ada"}]},
    "openai": {"choices": [{"message": {"content": "Hi from the model"}}]},
    "gemini": {"candidates": [{"content": {"parts": [{"text": "Gem"}, {"text": "ini"}]}}]},
    "cohere": {"message": {"content": [{"type": "text", "text": "Co"}, {"type": "text", "text": "here"}]}},
}


def spec_payload() -> dict:
    body = {
        "schema": SCHEMA,
        "packPrefix": engine.PACK_PREFIX,
        "limits": {
            "maxNodes": engine.MAX_NODES,
            "maxConnections": engine.MAX_CONNECTIONS,
            "maxName": engine.MAX_NAME,
            "maxNodeName": engine.MAX_NODE_NAME,
            "maxValueChars": engine.MAX_VALUE_CHARS,
            "maxItems": engine.MAX_ITEMS,
            "maxResponseChars": engine.MAX_RESPONSE_CHARS,
            "maxRunItems": engine.MAX_RUN_ITEMS,
            "maxRunItemChars": engine.MAX_RUN_ITEM_CHARS,
            "positionLimit": engine.POSITION_LIMIT,
            "scheduleMinMinutes": engine.SCHEDULE_MIN_MINUTES,
            "scheduleMaxMinutes": engine.SCHEDULE_MAX_MINUTES,
            "requestTimeoutSeconds": runtime.REQUEST_TIMEOUT_SECONDS,
        },
        "runLimits": {kind: list(limit) for kind, limit in runtime.LIMITS.items()},
        "userAgent": runtime.USER_AGENT,
        "blockedHostSuffixes": list(engine.BLOCKED_HOST_SUFFIXES),
        "operators": [list(pair) for pair in engine.OPERATORS],
        "coreNodes": engine.CORE_NODES,
        "integrations": integrations.INTEGRATIONS,
    }
    digest = hashlib.sha256(json.dumps(body, sort_keys=True).encode("utf-8")).hexdigest()[:16]
    return dict(body, version=digest)


def _sample(field: dict, index: int):
    kind = field.get("type", "string")
    if kind == "number":
        return str(7 + index)
    if kind == "boolean":
        return "true" if index % 2 == 0 else "false"
    if kind == "json":
        if field.get("in") == "query_object":
            return "{\"status\": \"eq.{{ $json.name }}\"}"
        return "{\"k\": \"{{ $json.name }}\", \"n\": " + str(index) + "}"
    if kind == "select":
        options = [option["value"] for option in field.get("options") or []]
        return options[index % len(options)] if options else ""
    if kind == "text":
        return "Hello {{ $json.name }} & co/" + str(index)
    if field.get("slashes"):
        return "folder/sub dir/" + field["key"]
    return "v {{ $json.name }}/" + field["key"] + "é"


def _credential(app: dict) -> dict:
    values = {}
    for field in app["auth"]["fields"]:
        if field.get("url"):
            values[field["key"]] = "https://hooks.example.com/" + field["key"] + "/"
        else:
            values[field["key"]] = "cred " + field["key"] + "/é"
    return values


def _trigger(payload=None) -> dict:
    params = {"payload": json.dumps(payload)} if payload is not None else {}
    return {"id": "t", "type": "core.manual", "name": "Start", "params": params}


def _chain(*nodes) -> list[dict]:
    return [{"from": nodes[i]["id"], "to": nodes[i + 1]["id"]} for i in range(len(nodes) - 1)]


def _case(name, workflow, responses=(), credentials=None, payload=None, kind="manual") -> dict:
    return {"name": name, "trigger": kind, "workflow": workflow, "payload": payload,
            "credentials": credentials or {}, "responses": list(responses)}


def operation_cases() -> list[dict]:
    cases = []
    for key, app in integrations.INTEGRATIONS.items():
        for op_key, op in app["operations"].items():
            params = {"operation": op_key}
            for index, field in enumerate(op["fields"]):
                params[field["key"]] = _sample(field, index)
            step = {"id": "s", "type": "app." + key, "name": app["name"], "params": params,
                    "credential": CREDENTIAL_ID if app["auth"]["fields"] else ""}
            trigger = _trigger({"name": "Ada Lovelace"})
            credentials = {CREDENTIAL_ID: {"integration": key, "values": _credential(app)}} \
                if app["auth"]["fields"] else {}
            response = OK_RESPONSE
            if op.get("output") in CHAT_RESPONSES:
                response = {"status": 200, "text": json.dumps(CHAT_RESPONSES[op["output"]])}
            cases.append(_case("op " + key + "." + op_key,
                               {"name": app["name"] + " " + op["name"], "nodes": [trigger, step],
                                "connections": _chain(trigger, step)},
                               [response], credentials))
    return cases


def template_cases() -> list[dict]:
    cases = []
    for item in templates.TEMPLATES:
        workflow = copy.deepcopy(item["workflow"])
        credentials = {}
        for node in workflow["nodes"]:
            app_key = node["type"][4:] if node["type"].startswith("app.") else ""
            app = integrations.INTEGRATIONS.get(app_key)
            if app and app["auth"]["fields"]:
                node["credential"] = hashlib.md5(app_key.encode("utf-8")).hexdigest()
                credentials[node["credential"]] = {"integration": app_key, "values": _credential(app)}
        responses = [OK_RESPONSE] * 12
        cases.append(_case("template " + item["key"], workflow, responses, credentials,
                           {"method": "POST", "query": {"a": "1"}, "body": {
                               "action": "opened", "issue": {"title": "Crash", "labels": [{"name": "bug"}]},
                               "name": "Ada", "email": "ada@example.com", "amount": 120}}))
    return cases


def _n(node_id, node_type, name, **params) -> dict:
    return {"id": node_id, "type": node_type, "name": name, "params": params}


def flow_cases() -> list[dict]:
    t = _trigger({"name": "Ada", "age": 36, "tags": ["a", "b"], "list": [{"x": 1}, {"x": 2}, {"x": 3}],
                  "nested": {"deep": {"value": "ok"}}, "flag": True, "empty": ""})
    cases = []

    def add(name, nodes, connections=None, responses=(), credentials=None, payload=None, kind="manual"):
        cases.append(_case(name, {"name": name, "nodes": [t] + list(nodes),
                                  "connections": connections if connections is not None else _chain(t, *nodes)},
                           responses, credentials, payload, kind))

    add("set keeps fields and types", [_n("s", "core.set", "Set", values=json.dumps({
        "full": "{{ $json.name }} ({{ $json.age }})", "age": "{{ $json.age }}", "tags": "{{ $json.tags }}",
        "a.b.c": "{{ $json.nested.deep.value }}", "first": "{{ $json.list[0].x }}", "last": "{{ $json.list[-1].x }}",
        "q1": "{{ $json[\"name\"] }}", "q2": "{{ $json['age'] }}!", "q3": "{{ $json.tags[5] }}", "missing": "{{ $json.nope.deeper }}",
        "now": "{{ $now }}", "today": "{{ $today }}", "wf": "{{ $workflow.name }}", "index": "{{ $item.index }}",
        "trigger": "{{ $trigger.flag }}", "obj": "x {{ $json.nested }}", "bool": "is {{ $json.flag }}"}))])
    add("set replaces fields", [_n("s", "core.set", "Set", values="{\"only\": 1}", keep="false")])
    add("set with bad json", [_n("s", "core.set", "Set", values="{nope")])
    add("set with a list", [_n("s", "core.set", "Set", values="[1, 2]")])
    add("expression error", [_n("s", "core.set", "Set", values="{\"a\": \"{{ json.name }}\"}")])
    add("unknown variable", [_n("s", "core.set", "Set", values="{\"a\": \"{{ $env.HOME }}\"}")])
    add("unreadable expression", [_n("s", "core.set", "Set", values="{\"a\": \"{{ $json..name }}\"}")])
    add("two expressions joined", [_n("s", "core.set", "Set", values="{\"a\": \"{{ $json.name }} }} {{ $json.age }}\"}")])
    split = _n("p", "core.split", "Split", field="list")
    add("split, limit and node references", [
        split, _n("l", "core.limit", "Limit", max="2"),
        _n("s", "core.set", "Echo", values=json.dumps({"x2": "{{ $json.x }}", "from": "{{ $node[\"Split\"].json.x }}",
                                                        "start": "{{ $node.Start.json.name }}", "i": "{{ $item.index }}"}))])
    add("split with an expression field", [_n("p", "core.split", "Split", field="$trigger.list")])
    add("split a non-list", [_n("p", "core.split", "Split", field="name")])
    add("limit needs a number", [_n("l", "core.limit", "Limit", max="many")])
    add("limit 2.9", [_n("l", "core.limit", "Limit", max="2.9")], payload={"value": [1]})
    operators = [o for o, _ in engine.OPERATORS]
    for op in operators:
        for left, right in (("{{ $json.name }}", "Ada"), ("{{ $json.age }}", "30"), ("{{ $json.tags }}", "b"),
                            ("{{ $json.empty }}", ""), ("{{ $json.flag }}", "true"), ("{{ $json.nested }}", "x")):
            node = _n("i", "core.if", "If", left=left, operator=op, right=right)
            yes = _n("y", "core.noop", "Yes")
            no = _n("n", "core.noop", "No")
            cases.append(_case("if " + op + " " + left + " " + right, {
                "name": "if", "nodes": [t, node, yes, no], "connections": [
                    {"from": "t", "to": "i"}, {"from": "i", "output": "true", "to": "y"},
                    {"from": "i", "output": "false", "to": "n"}]}))
    add("filter on items", [split, _n("f", "core.filter", "Filter", left="{{ $json.x }}", operator="gte", right="2")])
    merge_nodes = [split, _n("a", "core.filter", "Big", left="{{ $json.x }}", operator="gt", right="1"),
                   _n("b", "core.filter", "Small", left="{{ $json.x }}", operator="lt", right="2"),
                   _n("m", "core.merge", "Merge")]
    add("merge two branches", merge_nodes, [{"from": "t", "to": "p"}, {"from": "p", "to": "a"}, {"from": "p", "to": "b"},
                                            {"from": "a", "to": "m"}, {"from": "b", "to": "m"}])
    add("skipped when nothing arrives", [_n("f", "core.filter", "None", left="{{ $json.name }}", operator="equals",
                                            right="Bob"), _n("z", "core.noop", "After")])
    add("disabled step passes items", [dict(_n("s", "core.set", "Off", values="{\"a\": 1}"), disabled=True)])
    add("disabled if passes to true", [dict(_n("i", "core.if", "If", left="1", operator="equals", right="2"),
                                            disabled=True), _n("y", "core.noop", "Yes")],
        [{"from": "t", "to": "i"}, {"from": "i", "output": "true", "to": "y"}])
    add("http get with query", [_n("h", "core.http", "Get", method="GET", url="https://api.example.com/items?x=1",
                                   query="{\"q\": \"{{ $json.name }} & co\", \"n\": 5, \"b\": true}",
                                   headers="{\"X-Name\": \"{{ $json.name }}\", \"X-N\": 3}")],
        responses=[{"status": 200, "text": "[1, {\"a\": 2}, \"three\"]"}])
    add("http post json", [_n("h", "core.http", "Post", method="POST", url="https://api.example.com/items",
                              body="{\"text\": \"{{ $json.name }}\", \"n\": {{ $json.age }}, \"é\": \"ü\"}")],
        responses=[{"status": 201, "text": "  {\"created\": true}  "}])
    add("http post form", [_n("h", "core.http", "Form", method="PUT", url="https://api.example.com/f", bodyType="form",
                              body="{\"a\": \"b c\", \"nested\": {\"x\": [1, 2]}, \"t\": true}")],
        responses=[{"status": 200, "text": "plain text reply"}])
    add("http post text", [_n("h", "core.http", "Text", method="PATCH", url="https://api.example.com/t", bodyType="text",
                              body="raw {{ $json.name }}", headers="{\"Content-Type\": \"text/markdown\"}")],
        responses=[{"status": 204, "text": ""}])
    add("http form needs an object", [_n("h", "core.http", "Form", method="POST", url="https://api.example.com/f",
                                         bodyType="form", body="[1]")])
    add("http bad body", [_n("h", "core.http", "Bad", method="POST", url="https://api.example.com/f", body="{x")])
    add("http bad headers", [_n("h", "core.http", "Bad", method="GET", url="https://api.example.com/f", headers="[1]")])
    add("http bad method", [_n("h", "core.http", "Bad", method="TRACE", url="https://api.example.com/f")])
    add("http url required", [_n("h", "core.http", "Bad", method="GET", url="")])
    add("http error status", [_n("h", "core.http", "Get", method="GET", url="https://api.example.com/x")],
        responses=[{"status": 404, "text": "  {\"error\": \"not found\"}  "}])
    add("http no response", [_n("h", "core.http", "Get", method="GET", url="https://api.example.com/x")],
        responses=[{"status": 0, "text": ""}])
    for url in ("http://api.example.com/x", "https://localhost/x", "https://10.0.0.1/x", "https://forkmesh.com/x",
                "https://app.forkmesh.com/x", "https://printer.local/x", "https://user@api.example.com/x",
                "https://intranet/x", "https://[::1]/x", "https://169.254.169.254/latest", "https://8.8.8.8/x",
                "https://api.example.com:0/x", "https://api.example.com:8443/x", "https://API.Example.COM./x",
                "https://[2606:4700::1111]/x", "https://db.internal/x", "https://a b/x"):
        add("http url " + url, [_n("h", "core.http", "Get", method="GET", url=url)], responses=[OK_RESPONSE])
    add("continue on fail", [dict(_n("h", "core.http", "Get", method="GET", url="https://api.example.com/x"),
                                  continueOnFail=True), _n("s", "core.set", "After", values="{\"seen\": \"{{ $json.error }}\"}")],
        responses=[{"status": 500, "text": "boom"}])
    add("error stops the run", [_n("h", "core.http", "Get", method="GET", url="https://api.example.com/x"),
                                _n("s", "core.set", "Never", values="{\"a\": 1}")],
        responses=[{"status": 401, "text": "nope"}])
    add("request limit", [split, _n("h", "core.http", "Each", method="GET", url="https://api.example.com/{{ $json.x }}")],
        responses=[OK_RESPONSE] * 3, kind="schedule",
        payload=None)
    many = {"list": [{"x": i} for i in range(8)]}
    cases.append(_case("schedule limit of six requests", {"name": "limit", "nodes": [
        {"id": "t", "type": "core.schedule", "name": "Every hour", "params": {"every": "60"}},
        _n("p", "core.split", "Split", field="list"),
        _n("h", "core.http", "Each", method="GET", url="https://api.example.com/{{ $json.x }}")],
        "connections": [{"from": "t", "to": "p"}, {"from": "p", "to": "h"}]},
        [OK_RESPONSE] * 8, payload=many, kind="schedule"))
    big = {"value": [{"text": "x" * 5000}, {"text": "é" * 900}] + [{"n": i} for i in range(130)]}
    add("big items are summarized", [_n("p", "core.split", "Split", field="value")], payload=big)
    add("credential required", [{"id": "s", "type": "app.slack", "name": "Slack",
                                  "params": {"operation": "postMessage", "channel": "#c", "text": "hi"}}])
    add("credential from another integration", [{"id": "s", "type": "app.slack", "name": "Slack",
                                                 "credential": OTHER_CREDENTIAL_ID,
                                                 "params": {"operation": "postMessage", "channel": "#c", "text": "hi"}}],
        credentials={OTHER_CREDENTIAL_ID: {"integration": "github", "values": {"token": "x"}}})
    add("missing path value", [{"id": "s", "type": "app.github", "name": "GitHub", "credential": CREDENTIAL_ID,
                                "params": {"operation": "createIssue", "owner": "", "repo": "r", "title": "t"}}],
        credentials={CREDENTIAL_ID: {"integration": "github", "values": {"token": "tok"}}})
    add("required field", [{"id": "s", "type": "app.slack", "name": "Slack", "credential": CREDENTIAL_ID,
                            "params": {"operation": "postMessage", "channel": "  "}}],
        credentials={CREDENTIAL_ID: {"integration": "slack", "values": {"token": "tok"}}})
    add("unknown operation falls back to the first", [{"id": "s", "type": "app.wikipedia", "name": "Wiki",
                                                       "params": {"operation": "nope", "title": "Ada"}}],
        responses=[OK_RESPONSE])
    cases.append(_case("webhook payload", {"name": "hook", "nodes": [
        {"id": "w", "type": "core.webhook", "name": "Hook", "params": {}},
        _n("s", "core.set", "Echo", values="{\"m\": \"{{ $json.method }}\", \"q\": \"{{ $trigger.query.a }}\"}")],
        "connections": [{"from": "w", "to": "s"}]}, payload={"method": "PUT", "query": {"a": "1"}, "body": {}},
        kind="webhook"))
    cases.append(_case("webhook default payload", {"name": "hook", "nodes": [
        {"id": "w", "type": "core.webhook", "name": "Hook", "params": {}}], "connections": []}))
    cases.append(_case("manual payload that is a list", {"name": "m", "nodes": [
        {"id": "t", "type": "core.manual", "name": "Go", "params": {"payload": "[1, 2]"}}], "connections": []}))
    cases.append(_case("manual payload that is broken", {"name": "m", "nodes": [
        {"id": "t", "type": "core.manual", "name": "Go", "params": {"payload": "{nope"}}], "connections": []}))
    cases.append(_case("no trigger", {"name": "m", "nodes": [_n("a", "core.noop", "A")], "connections": []}))
    for name, workflow in (
        ("clean cycle", {"nodes": [t, _n("a", "core.noop", "A"), _n("b", "core.noop", "B")],
                         "connections": [{"from": "a", "to": "b"}, {"from": "b", "to": "a"}]}),
        ("clean duplicate name", {"nodes": [t, _n("a", "core.noop", "Start")], "connections": []}),
        ("clean bad id", {"nodes": [dict(_n("a b", "core.noop", "A"))], "connections": []}),
        ("clean unknown type", {"nodes": [_n("a", "core.code", "A")], "connections": []}),
        ("clean two triggers", {"nodes": [t, {"id": "u", "type": "core.webhook", "name": "U", "params": {}}],
                                "connections": []}),
        ("clean into the trigger", {"nodes": [t, _n("a", "core.noop", "A")], "connections": [{"from": "a", "to": "t"}]}),
        ("clean bad output", {"nodes": [t, _n("a", "core.noop", "A")],
                              "connections": [{"from": "t", "output": "true", "to": "a"}]}),
        ("clean bad credential", {"nodes": [{"id": "s", "type": "app.slack", "name": "S", "credential": "x"}],
                                  "connections": []}),
        ("clean value too long", {"nodes": [_n("s", "core.set", "S", values="x" * 20001)], "connections": []}),
        ("clean not a list", {"nodes": {"a": 1}}),
    ):
        cases.append(_case(name, workflow))
    return cases


def clean_cases() -> list[dict]:
    raw = [
        {"name": "  Spaced\u0000 name  ", "nodes": [
            {"id": "t", "type": "core.manual", "name": " Go ", "x": "12.7", "y": 99999, "params": {
                "payload": {"a": 1}, "extra": "dropped"}, "credential": CREDENTIAL_ID, "disabled": "yes",
             "continueOnFail": True},
            {"id": "s", "type": "app.slack", "name": "Slack", "params": {"operation": "nope", "channel": 5,
                                                                        "text": None, "thread_ts": True}},
            {"id": "h", "type": "core.http", "name": "H", "params": {"url": ["a"], "method": "GET"}}],
         "connections": [{"from": "t", "to": "s"}, {"from": "t", "to": "s"}, {"from": "s", "to": "h", "output": None}]},
        {"nodes": [], "connections": []},
        {"name": "x" * 200, "nodes": None},
        "not a workflow",
    ]
    out = []
    for item in raw:
        workflow, error = engine.clean_workflow(item)
        out.append({"input": item, "workflow": workflow, "error": error,
                    "scheduleMinutes": engine.schedule_minutes(workflow) if workflow else 0})
    return out


def safe_url_cases() -> list[dict]:
    urls = ["https://api.example.com/x", "http://api.example.com", "https://localhost", "https://a.localhost",
            "https://forkmesh.com", "https://notforkmesh.com", "https://x.forkmesh.com", "https://box.local",
            "https://x.internal", "https://intranet", "https://10.1.2.3", "https://100.64.0.1", "https://172.20.0.1",
            "https://192.168.1.1", "https://127.0.0.1", "https://0.0.0.0", "https://8.8.8.8", "https://1.1.1.1:443",
            "https://[::1]", "https://[fe80::1]", "https://[2001:db8::1]", "https://[2606:4700:4700::1111]",
            "https://u:p@api.example.com", "https://@api.example.com", "https://api.example.com:99999",
            "https://api.example.com:abc", "https://api.example.com:", "https://api.example.com.", "HTTPS://API.EXAMPLE.COM",
            "https://api example.com", "https://api.example.com/a\\b", " https://api.example.com/ ", "", "https://",
            "https://[::1", "https://example.com/" + "x" * 5000, "ftp://example.com", "https:api.example.com"]
    return [{"url": url, "safe": engine.safe_url(url)} for url in urls]


async def _expected(case: dict) -> dict:
    workflow, error = engine.clean_workflow(case["workflow"])
    if error:
        return {"requests": [], "report": {"status": "error", "error": "Workflow: " + error, "requests": 0,
                                           "nodes": {}, "trigger": case["trigger"]}}
    trigger_item = runtime._manual_payload(workflow, {"payload": case["payload"]})
    responses = list(case["responses"])
    requests = []

    async def http(method, url, headers, body):
        requests.append({"method": method, "url": url, "headers": headers, "body": body})
        reply = responses.pop(0) if responses else OK_RESPONSE
        return reply["status"], reply["text"]

    max_requests, _budget = runtime.LIMITS[case["trigger"]]
    report = await engine.execute(workflow, trigger_item, http, case["credentials"], max_requests, 10 ** 15,
                                  lambda: 0, NOW_MS, WORKFLOW_ID)
    report["trigger"] = case["trigger"]
    return {"requests": requests, "report": report}


def cases_payload() -> dict:
    cases = flow_cases() + template_cases() + operation_cases()
    for case in cases:
        case["expect"] = asyncio.run(_expected(case))
    catalog = engine.catalog()
    return {
        "schema": SCHEMA,
        "nowMs": NOW_MS,
        "workflowId": WORKFLOW_ID,
        "runs": cases,
        "clean": clean_cases(),
        "safeUrl": safe_url_cases(),
        "catalog": {"core": catalog["core"], "integrations": catalog["integrations"],
                    "categories": catalog["categories"], "packs": catalog["packs"], "operators": catalog["operators"]},
    }


def render(payload: dict) -> str:
    return json.dumps(payload, indent=1, ensure_ascii=False) + "\n"


def render_compact(payload: dict) -> str:
    return json.dumps(payload, ensure_ascii=False, separators=(",", ":")) + "\n"


def outputs() -> dict[Path, str]:
    spec = render(spec_payload())
    cases = render_compact(cases_payload())
    script = ENGINE.read_text(encoding="utf-8")
    return {
        IOS_RESOURCES / SPEC_NAME: spec,
        IOS_RESOURCES / ENGINE_NAME: script,
        ANDROID_RESOURCES / SPEC_NAME: spec,
        IOS_FIXTURES / CASES_NAME: cases,
        ANDROID_FIXTURES / CASES_NAME: cases,
    }


def stale(files: dict[Path, str]) -> list[Path]:
    return [path for path, text in files.items()
            if not path.is_file() or path.read_text(encoding="utf-8") != text]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Write what the iPhone and Android Workflows engines ship: the step and integration spec "
                    "from app/src, the JavaScript engine for JavaScriptCore, and the server engine's results "
                    "as conformance cases for both phone engines.")
    parser.add_argument("--check", action="store_true", help="exit 1 if any file is out of date; write nothing")
    args = parser.parse_args(argv)
    files = outputs()
    if args.check:
        old = stale(files)
        for path in old:
            print(f"{path.relative_to(REPOSITORY_ROOT)} is out of date; run python3 marketplace/tools/workflows_mobile_bundle.py")
        return 1 if old else 0
    for path, text in files.items():
        path.parent.mkdir(parents=True, exist_ok=True)
        temporary = path.with_name(path.name + ".tmp")
        temporary.write_text(text, encoding="utf-8")
        temporary.replace(path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
