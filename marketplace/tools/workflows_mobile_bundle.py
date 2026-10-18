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

import workflow_actions as actions  # noqa: E402
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
SCRIPT_CORPUS = REPOSITORY_ROOT / "app" / "tests" / "fixtures" / "workflow_script_corpus.json"
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
            "maxWaitSeconds": actions.MAX_WAIT_SECONDS,
            "maxRepeat": actions.MAX_REPEAT,
            "maxWorkflowDepth": actions.MAX_WORKFLOW_DEPTH,
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


def _case(name, workflow, responses=(), credentials=None, payload=None, kind="manual", randoms=None,
          workflows=None) -> dict:
    case = {"name": name, "trigger": kind, "workflow": workflow, "payload": payload,
            "credentials": credentials or {}, "responses": list(responses)}
    if randoms is not None:
        case["randoms"] = list(randoms)
    if workflows is not None:
        case["workflows"] = workflows
    return case


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


def _n(node_id, node_type, title, **params) -> dict:
    return {"id": node_id, "type": node_type, "name": title, "params": params}


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
        ("clean unknown type", {"nodes": [_n("a", "core.nothing", "A")], "connections": []}),
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


def _step_case(name, node, payload, **extra) -> dict:
    trigger = _trigger(payload)
    node = dict(node, id="a", name=node.get("name") or "Step")
    return _case(name, {"name": name, "nodes": [trigger, node], "connections": _chain(trigger, node)},
                 payload=None, **extra)


def _over(name, node, values, **extra) -> dict:
    trigger = _trigger({"list": values})
    split = _n("s", "core.split", "Split", field="list")
    node = dict(node, id="a", name=node.get("name") or "Step")
    return _case(name, {"name": name, "nodes": [trigger, split, node],
                        "connections": _chain(trigger, split, node)}, **extra)


TEXT = "Hello big World. Visit https://example.com/a?b=1). Mail ada@example.org, call +44 (20) 7946-0958.\n" \
    "Tags #one #two_2 and @someone; numbers -3, 1,234.5 and 7.\r\nSecond line\n"


def action_cases() -> list[dict]:
    cases = []
    text = {"text": TEXT, "unicode": "Ünïcødé 😀 ß", "empty": "", "n": 7, "list": ["a", 1, True, None, {"k": 1}]}

    def step(name, node_type, payload=None, **params):
        cases.append(_step_case(name, _n("a", node_type, "Step", **params), text if payload is None else payload))

    for mode in ("upper", "lower", "capitalize", "words"):
        step("case " + mode, "core.textCase", case=mode)
        step("case unicode " + mode, "core.textCase", value="{{ $json.unicode }}", case=mode)
    step("case of a missing value", "core.textCase", value="{{ $json.nothing }}")
    step("replace plain", "core.textReplace", find="o", replace="0", field="out")
    step("replace ignoring case", "core.textReplace", find="hello", replace="$1 Bye", ignoreCase="true")
    step("replace regex groups", "core.textReplace", find="(\\w+)@(\\w+)", replace="$2 at $1 ($&) $$", regex="true")
    step("replace regex named", "core.textReplace", find="(?<a>#\\w+)", replace="[$<a>]", regex="true")
    step("replace regex empty matches", "core.textReplace", value="abc", find="x*", replace="-", regex="true")
    step("replace bad regex", "core.textReplace", find="(", regex="true")
    step("replace needs find", "core.textReplace", find="")
    for result in ("all", "first", "groups", "test"):
        step("match " + result, "core.textMatch", pattern="(\\d+)([.,]\\d+)?", result=result)
        step("match nothing " + result, "core.textMatch", pattern="zzz", result=result)
    step("match ignoring case", "core.textMatch", pattern="^hello", ignoreCase="true", result="test")
    step("match anchors and classes", "core.textMatch", pattern="\\bline\\b|\\s+$|^\\S+", result="all")
    step("match dot and lines", "core.textMatch", pattern="[.]\\n.", result="all")
    for separator in ("newlines", "spaces", "commas", "characters", "custom"):
        step("split " + separator, "core.textSplit", separator=separator, custom=". ")
        step("split keeping empty " + separator, "core.textSplit", separator=separator, custom="o", skipEmpty="false")
    step("split unicode characters", "core.textSplit", value="{{ $json.unicode }}", separator="characters")
    step("split needs a custom separator", "core.textSplit", separator="custom", custom="")
    for separator in ("newlines", "spaces", "commas", "none", "custom"):
        step("combine " + separator, "core.textCombine", value="{{ $json.list }}", separator=separator,
             custom=" | ")
    step("combine a single value", "core.textCombine", value="{{ $json.n }}")
    for method in ("base64Encode", "urlEncode", "htmlEscape"):
        step("encode " + method, "core.textEncode", method=method)
        step("encode unicode " + method, "core.textEncode", value="{{ $json.unicode }}", method=method)
    for value in ("SGVsbG8g8J+YgA==", "SGVsbG8g8J-YgA", " SGVs\nbG8 ", "SGVsbG8=x", "A", "/w=="):
        step("decode base64 " + value, "core.textEncode", payload={"text": value}, method="base64Decode")
    for value in ("a%20b%2Fc%C3%A9", "%zz%2", "100%", "%E2%82", "a+b"):
        step("decode url " + value, "core.textEncode", payload={"text": value}, method="urlDecode")
    step("html unescape", "core.textEncode", payload={
        "text": "&lt;b&gt; &amp;amp; &quot;q&quot; &#39; &#x27; &apos; &nbsp; &#0; &#x110000; &#128512; &copy; &bogus"},
        method="htmlUnescape")
    for algorithm in ("md5", "sha1", "sha256", "sha512"):
        step("hash " + algorithm, "core.hash", algorithm=algorithm)
        step("hmac " + algorithm, "core.hash", algorithm=algorithm, key="sécret", encoding="base64")
    step("hash unicode", "core.hash", value="{{ $json.unicode }}", algorithm="sha256")
    step("hmac with a long key", "core.hash", algorithm="sha256", key="k" * 200)
    for kind in ("urls", "emails", "numbers", "phones", "hashtags", "mentions"):
        step("extract " + kind, "core.textExtract", kind=kind)
    step("extract wrapped links", "core.textExtract", payload={
        "text": "(see https://en.example.org/wiki/A_(b)) and http://x.example/y, 'https://q.example/z'"},
        kind="urls")
    for what in ("characters", "words", "lines", "items"):
        step("count " + what, "core.count", what=what)
        step("count list " + what, "core.count", value="{{ $json.list }}", what=what)
        step("count empty " + what, "core.count", value="{{ $json.empty }}", what=what)
    step("count unicode characters", "core.count", value="{{ $json.unicode }}")
    numbers = {"a": 7, "b": 2.5, "neg": -7, "zero": 0, "text": "3.5", "bad": "x", "big": 1e300}
    for operator in ("add", "subtract", "multiply", "divide", "modulo", "power", "min", "max", "sqrt", "abs"):
        step("math " + operator, "core.math", numbers, a="{{ $json.a }}", operator=operator, b="{{ $json.b }}")
        step("math negative " + operator, "core.math", numbers, a="{{ $json.neg }}", operator=operator, b="3")
    step("math modulo by a negative", "core.math", numbers, a="7", operator="modulo", b="-3")
    step("math divide by zero", "core.math", numbers, a="1", operator="divide", b="{{ $json.zero }}")
    step("math modulo by zero", "core.math", numbers, a="1", operator="modulo", b="0")
    step("math text number", "core.math", numbers, a="{{ $json.text }}", operator="add", b=" 1e2 ")
    step("math not a number", "core.math", numbers, a="{{ $json.bad }}", operator="add", b="1")
    step("math missing second", "core.math", numbers, a="1", operator="add")
    step("math overflow", "core.math", numbers, a="{{ $json.big }}", operator="multiply", b="{{ $json.big }}")
    step("math power overflow", "core.math", numbers, a="10", operator="power", b="400")
    step("math power of a negative", "core.math", numbers, a="-8", operator="power", b="0.5")
    step("math zero to a negative power", "core.math", numbers, a="0", operator="power", b="-1")
    step("math square root of a negative", "core.math", numbers, a="-4", operator="sqrt")
    step("math decimals", "core.math", numbers, a="0.1", operator="add", b="0.2")
    for value, decimals, mode in (("2.675", "2", "nearest"), ("-2.5", "0", "nearest"), ("1234.5", "-2", "nearest"),
                                  ("1.21", "1", "up"), ("-1.21", "1", "up"), ("1.29", "1", "down"),
                                  ("-1.21", "1", "down"), ("0.5", "0", "nearest"), ("1e21", "2", "nearest"),
                                  ("123.456", "20", "nearest"), ("5", "-20", "up"), ("9.995", "2", "nearest"),
                                  ("1.005", "2", "nearest"), ("-0.4", "0", "nearest")):
        step("round " + value + " " + decimals + " " + mode, "core.round", numbers, value=value, decimals=decimals,
             mode=mode)
    for value, decimals, grouping in (("1234567.891", "2", "true"), ("-1234.5", "0", "true"), ("0.005", "2", "false"),
                                      ("-0.001", "2", "true"), ("1e21", "1", "true"), ("999.999", "2", "true"),
                                      ("12", "3", "false"), ("1234", "2.7", "true")):
        step("format number " + value + " " + decimals + " " + grouping, "core.formatNumber", numbers, value=value,
             decimals=decimals, grouping=grouping)
    for low, high, whole in (("1", "6", "true"), ("0", "1", "false"), ("1.5", "2.5", "true"), ("5", "1", "true"),
                             ("-3", "-1", "true"), ("2", "1", "false")):
        cases.append(_step_case("random " + low + " " + high + " " + whole,
                                _n("a", "core.random", "Step", min=low, max=high, whole=whole), {},
                                randoms=[0.0, 0.999999, 0.5]))
    rows = [{"n": 3, "name": "c", "tag": "x"}, {"n": 1, "name": "a"}, {"n": "2", "name": "B", "tag": "y"},
            {"n": 1, "name": "a", "tag": "x"}, {"name": "none", "tag": None}, {"n": [4, 5], "name": "list"}]
    for operation in ("count", "sum", "average", "median", "min", "max", "range", "stdev"):
        cases.append(_over("statistics " + operation, _n("a", "core.statistics", "Stats", value="{{ $json.n }}",
                                                         operation=operation), rows))
    cases.append(_over("statistics without numbers", _n("a", "core.statistics", "Stats", value="{{ $json.name }}",
                                                        operation="average"), rows))
    cases.append(_over("statistics sum of nothing", _n("a", "core.statistics", "Stats", value="{{ $json.name }}",
                                                       operation="sum", field="totals.sum"), rows))
    cases.append(_over("statistics deviation of one", _n("a", "core.statistics", "Stats", value="{{ $json.n }}",
                                                         operation="stdev"), rows[:1]))
    cases.append(_over("combine whole items", _n("a", "core.combineItems", "All"), rows))
    cases.append(_over("combine a field", _n("a", "core.combineItems", "All", value="{{ $json.tag }}",
                                             field="out.tags"), rows))
    for by in ("n", "name", "tag", "", "$json.name", "missing"):
        for order in ("ascending", "descending"):
            cases.append(_over("sort " + (by or "first field") + " " + order,
                               _n("a", "core.sort", "Sort", by=by, order=order), rows))
    cases.append(_over("sort reverse", _n("a", "core.sort", "Sort", order="reverse"), rows))
    cases.append(_over("sort shuffle", _n("a", "core.sort", "Sort", order="shuffle"), rows,
                       randoms=[0.9, 0.1, 0.5, 0.3, 0.7]))
    for by in ("n", "tag", "", "name"):
        cases.append(_over("dedupe " + (by or "whole"), _n("a", "core.dedupe", "Dedupe", by=by), rows))
    for which, index, to in (("first", "1", "2"), ("last", "1", "2"), ("index", "2", "2"), ("index", "-2", "2"),
                             ("index", "0", "2"), ("index", "99", "2"), ("range", "2", "4"), ("range", "5", "99"),
                             ("range", "4", "2"), ("range", "-1", "2"), ("random", "1", "2")):
        cases.append(_over("pick " + which + " " + index + " " + to,
                           _n("a", "core.pick", "Pick", which=which, index=index, to=to), rows, randoms=[0.6]))
    json_payload = {"body": "{\"a\": [1, 2.5, \"é\"], \"b\": {\"c\": null}}", "bad": "{nope", "nan": "NaN",
                    "obj": {"x": [1, {"y": "ü"}], "z": True}, "num": 3}
    for value in ("{{ $json.body }}", "{{ $json.bad }}", "{{ $json.nan }}", "{{ $json.obj }}", "{{ $json.num }}",
                  "{{ $json.missing }}", "[1, 2"):
        step("json parse " + value, "core.json", json_payload, value=value)
    for pretty in ("false", "true"):
        step("json stringify " + pretty, "core.json", json_payload, operation="stringify", value="{{ $json.obj }}",
             pretty=pretty, field="text")
        step("json stringify text " + pretty, "core.json", json_payload, operation="stringify",
             value="{{ $json.body }}", pretty=pretty)
    for url in ("HTTPS://User:pw@Example.COM:8443/a/b%20c?x=1&y=a+b%26&x=2&flag&=v#frag",
                "https://[2001:db8::1]:443/p", "mailto:ada@example.com", "/relative/path?q", "example.com/x",
                "https://host:notaport/", "https://host:/x", "", "?a=%zz&b=%C3%A9"):
        step("url " + url, "core.url", {"url": url})
    dates = {"iso": "2026-03-29T00:30:00Z", "local": "2026-03-29 02:30", "date": "2026-10-25",
             "offset": "2026-10-25T01:30:00+05:30", "ms": "2026-01-31T10:00:00.123Z", "unix": 1790000000,
             "unixMs": 1790000000123, "text": "next week", "bad": "2026-02-30", "leap": "2028-02-29T12:00:00Z"}
    for fmt in ("iso", "date", "time", "datetime", "long", "unix", "unixMs"):
        for zone in ("", "Europe/Berlin", "America/New_York", "Asia/Kolkata", "-03:30"):
            step("date format " + fmt + " " + (zone or "utc"), "core.dateFormat", dates, value="{{ $json.iso }}",
                 format=fmt, timeZone=zone)
    for pattern in ("YYYY-MM-DD HH:mm:ss.SSS", "dddd, MMMM D, YYYY h:mm a", "ddd D MMM YY [at] H:m:s A",
                    "[Today is] dddd Z ZZ", "X x", "M/D/YYYY hh:mm"):
        for zone in ("", "Europe/Berlin", "Asia/Kolkata"):
            step("date pattern " + pattern + " " + (zone or "utc"), "core.dateFormat", dates, value="{{ $json.ms }}",
                 format="custom", pattern=pattern, timeZone=zone)
    for key in ("local", "date", "offset", "ms", "unix", "unixMs", "text", "bad", "leap", "missing"):
        for zone in ("", "Europe/Berlin", "America/New_York"):
            step("date read " + key + " " + (zone or "utc"), "core.dateFormat", dates, value="{{ $json." + key + " }}",
                 timeZone=zone)
    step("date unknown zone", "core.dateFormat", dates, value="{{ $json.iso }}", timeZone="Mars/Base")
    step("date bad zone", "core.dateFormat", dates, value="{{ $json.iso }}", timeZone="../etc")
    step("date custom without pattern", "core.dateFormat", dates, value="{{ $json.iso }}", format="custom")
    for operation in ("add", "subtract", "startOf", "endOf"):
        for unit in ("seconds", "minutes", "hours", "days", "weeks", "months", "years"):
            for zone in ("", "Europe/Berlin"):
                step("date " + operation + " " + unit + " " + (zone or "utc"), "core.dateAdjust", dates,
                     value="{{ $json.ms }}", operation=operation, amount="13", unit=unit, timeZone=zone)
    step("date add across a clock change", "core.dateAdjust", dates, value="2026-03-28T02:30:00+01:00",
         amount="1", unit="days", timeZone="Europe/Berlin")
    step("date add into a clock gap", "core.dateAdjust", dates, value="2026-03-28T02:30:00Z",
         amount="1", unit="days", timeZone="Europe/Berlin")
    step("date add into a repeated hour", "core.dateAdjust", dates, value="2026-10-24T00:30:00Z",
         amount="1", unit="days", timeZone="Europe/Berlin")
    step("date add months at month end", "core.dateAdjust", dates, value="2026-01-31T10:00:00Z", amount="1",
         unit="months")
    step("date add leap year", "core.dateAdjust", dates, value="{{ $json.leap }}", amount="1", unit="years")
    step("date add fractional hours", "core.dateAdjust", dates, value="{{ $json.iso }}", amount="1.5", unit="hours")
    step("date add fractional days", "core.dateAdjust", dates, value="{{ $json.iso }}", amount="1.5", unit="days")
    step("date subtract negative", "core.dateAdjust", dates, value="{{ $json.iso }}", operation="subtract",
         amount="-2", unit="weeks")
    step("date add without amount", "core.dateAdjust", dates, value="{{ $json.iso }}", amount="")
    step("date start of week on sunday", "core.dateAdjust", dates, value="2026-10-18T10:00:00Z", operation="startOf",
         unit="weeks")
    step("date out of range", "core.dateAdjust", dates, value="9999-12-31T00:00:00Z", amount="1", unit="days")
    for unit in ("seconds", "minutes", "hours", "days", "weeks", "months", "years"):
        step("date diff " + unit, "core.dateDiff", dates, start="{{ $json.leap }}", end="{{ $json.ms }}", unit=unit)
        step("date diff forward " + unit, "core.dateDiff", dates, start="2026-01-31T10:00:00Z",
             end="2027-03-30T09:00:00Z", unit=unit)
    step("date diff bad start", "core.dateDiff", dates, start="{{ $json.text }}", end="{{ $json.iso }}")
    flow_rows = [{"n": 1, "status": "new"}, {"n": 2, "status": "open"}, {"n": 3, "status": "closed"},
                 {"n": 4, "status": "other"}, {"n": 5, "status": 2}]
    cases.append(_over("switch routes values", _n("a", "core.switch", "Choose", value="{{ $json.status }}",
                                                  case1="new", case2="2", case3="closed"), flow_rows))
    cases.append(_over("switch with empty cases", _n("a", "core.switch", "Choose", value="{{ $json.status }}",
                                                     case1="", case2="open"), flow_rows))
    cases.append(_over("switch needs a value", _n("a", "core.switch", "Choose", value="{{ $json.nothing }}"),
                       flow_rows))
    for times in ("3", "0", "2.9", "500", "-1", "x"):
        cases.append(_over("repeat " + times, _n("a", "core.repeat", "Repeat", times=times, field="round.i"),
                           flow_rows[:2]))
    for mode, value in (("set", "{{ $json.n }}"), ("append", "{{ $json.status }}"), ("add", "{{ $json.n }}"),
                        ("add", "{{ $json.status }}"), ("set", "")):
        trigger = _trigger({"list": flow_rows})
        nodes = [trigger, _n("s", "core.split", "Split", field="list"),
                 _n("v", "core.variable", "Var", name="total", value=value, mode=mode),
                 _n("e", "core.set", "Echo", values="{\"v\": \"{{ $vars.total }}\", \"t\": \"[{{ $vars.total }}]\"}")]
        cases.append(_case("variable " + mode + " " + value, {"name": "v", "nodes": nodes,
                                                              "connections": _chain(*nodes)}))
    cases.append(_step_case("variable bad name", _n("a", "core.variable", "Var", name="a-b", value="1"), {}))
    cases.append(_step_case("variable add to text", _n("a", "core.variable", "Var", name="x", value="a", mode="add"),
                            {}))
    for seconds in ("0", "1.25", "60", "61", "-1", "44.9", "45.1"):
        trigger = _trigger({"x": 1})
        nodes = [trigger, _n("w", "core.wait", "Wait", seconds=seconds), _n("e", "core.noop", "After")]
        cases.append(_case("wait " + seconds, {"name": "w", "nodes": nodes, "connections": _chain(*nodes)}))
    for outcome, message in (("success", ""), ("error", "Nothing to do"), ("error", "")):
        trigger = _trigger({"x": 1})
        nodes = [trigger, _n("x", "core.stop", "Stop", outcome=outcome, message=message), _n("e", "core.noop", "Never")]
        cases.append(_case("stop " + outcome + " " + message, {"name": "s", "nodes": nodes,
                                                               "connections": _chain(*nodes)}))
    trigger = _trigger({"x": 1})
    branches = [trigger, _n("i", "core.if", "If", left="1", operator="equals", right="1"),
                _n("x", "core.stop", "Stop"), _n("o", "core.noop", "Other"), _n("z", "core.noop", "Later")]
    cases.append(_case("stop skips the other branch", {"name": "s", "nodes": branches, "connections": [
        {"from": "t", "to": "i"}, {"from": "i", "output": "true", "to": "x"}, {"from": "i", "output": "true", "to": "o"},
        {"from": "o", "to": "z"}]}))
    trigger = _trigger({"nested": {"deep": {"value": "ok"}}})
    nodes = [trigger, _n("a", "core.textCase", "Upper", value="{{ $json.nested.deep.value }}",
                         field="nested.deep.loud"),
             _n("e", "core.set", "Echo", values="{\"before\": \"{{ $trigger.nested.deep }}\"}")]
    cases.append(_case("save as a nested path copies the item", {"name": "n", "nodes": nodes,
                                                                "connections": _chain(*nodes)}))
    cases.append(_step_case("comment passes items", _n("a", "core.comment", "Note", text="hello"), {"x": 1}))
    for left, operator, right in (("order-12", "matches", "^order-\\d+$"), ("Order-12", "matches", "^order"),
                                  ("abc", "notMatches", "b"), ("abc", "matches", "(")):
        trigger = _trigger({})
        node = _n("i", "core.if", "If", left=left, operator=operator, right=right)
        cases.append(_case("if " + operator + " " + left + " " + right, {"name": "i", "nodes": [
            trigger, node, _n("y", "core.noop", "Yes"), _n("n", "core.noop", "No")], "connections": [
            {"from": "t", "to": "i"}, {"from": "i", "output": "true", "to": "y"},
            {"from": "i", "output": "false", "to": "n"}]}))
    double = {"name": "Double it", "nodes": [_trigger({}), _n("m", "core.math", "Times", a="{{ $json.n }}",
                                                               operator="multiply", b="2", field="n"),
                                             _n("x", "core.textCase", "Name", value="{{ $json.name }}")],
              "connections": [{"from": "t", "to": "m"}, {"from": "m", "to": "x"}]}
    failing = {"name": "Fails", "nodes": [_trigger({}), _n("h", "core.http", "Call", method="GET",
                                                           url="https://api.example.com/fail")],
               "connections": [{"from": "t", "to": "h"}]}
    looping = {"name": "Loop", "nodes": [_trigger({}), _n("r", "core.runWorkflow", "Again", workflow="loop")],
               "connections": [{"from": "t", "to": "r"}]}
    stopping = {"name": "Stops", "nodes": [_trigger({}), _n("x", "core.stop", "Halt"),
                                           _n("n", "core.noop", "Never")],
                "connections": [{"from": "t", "to": "x"}, {"from": "x", "to": "n"}]}
    workflows = [{"id": "a" * 32, "name": "Double it", "workflow": double},
                 {"id": "b" * 32, "name": "Fails", "workflow": failing},
                 {"id": "e" * 32, "name": "Loop", "workflow": looping},
                 {"id": "f" * 32, "name": "Stops", "workflow": stopping}]
    for ref in ("Double it", " double IT ", "a" * 32, "Fails", "Loop", "Stops", "Nope", ""):
        cases.append(_over("run workflow " + ref, _n("a", "core.runWorkflow", "Run", workflow=ref),
                           [{"n": 1, "name": "ada"}, {"n": 2, "name": "bo"}],
                           responses=[{"status": 500, "text": "down"}], workflows=workflows))
    return cases


SCRIPT_ERRORS = [
    "return (", "let x = 1 2", "'open", "`open ${1}", "/* never", "break", "class A {}", "const x;", "return {a = 1}",
    "1 = 2", "return /(/", "return 1n", "a\n.b\n.c(", "throw new Error('boom')", "throw 'plain'", "throw {code: 4}",
    "null.x", "undefined.y = 1", "nope()", "const c = 1; c = 2", "let a = [1]\na.nope()", "while (true) {}",
    "function f() { return f() }\nf()", "return 'x'.repeat(2e6)", "return new Array(1e9)", "let o = {}; o.o = o; return o",
    "for (const x of 5) {}", "return " + "(" * 80 + "1" + ")" * 80, "try { while (true) {} } catch (e) {}",
    "return JSON.parse('{bad')", "return decodeURIComponent('%E0%A4%A')", "return new RegExp('(')",
    "return 'a'.replaceAll(/a/, 'b')", "return new Set", "return Set()", "return [].reduce((a, b) => a)",
    "return (1).toFixed(101)", "return (1).toString(1)", "return new Date(NaN).toISOString()", "let a = 1; let a = 2",
    "return $utils.base64Decode('*')", "return $utils.formatDate('nope')", "return $utils.hash('x', 'crc')",
    "return x => x", "return 'a' in 'abc'",
]


def script_cases() -> list[dict]:
    corpus = json.loads(SCRIPT_CORPUS.read_text(encoding="utf-8"))
    cases = []
    payload = {"list": [{"n": 1, "name": "ada", "tags": ["x"]}, {"n": 2, "name": "bo"}, {"n": 3}],
               "when": "2026-10-18T09:30:00Z"}

    def add(name, code, mode="all", after=None, randoms=None):
        trigger = _trigger(payload)
        nodes = [trigger, _n("s", "core.split", "Split", field="list"),
                 _n("c", "core.code", "Script", mode=mode, code=code)]
        if after:
            nodes.append(after)
        cases.append(_case(name, {"name": name, "nodes": nodes, "connections": _chain(*nodes)}, randoms=randoms))

    for index, entry in enumerate(corpus):
        add("script corpus %03d" % index, "return {result: (() => {\n" + entry["code"] + "\n})()}")
    for index, code in enumerate(SCRIPT_ERRORS):
        add("script error %02d" % index, code)
    add("script items and globals", "console.log(items.length, $now, $today, $workflow.name, $trigger.when);\n"
        "console.log($node.Split.json, $input.first().json.n, $input.last().json, $input.all().length);\n"
        "return items.map((it, i) => ({...it, i, upper: (it.name || '').toUpperCase()}))")
    add("script each item", "console.log($index, $json.n, item === $json, $input.item.json.n);\n"
        "$json.double = $json.n * 2; if ($index === 1) return null; if ($index === 2) return [$json, {extra: true}]",
        mode="each")
    add("script each item without return", "$json.seen = $index", mode="each")
    add("script returns n8n items", "return items.map(json => ({json, pairedItem: 0}))")
    add("script returns values", "return [1, 'two', [3], null, {four: 4}]")
    add("script returns one object", "return {count: items.length}")
    add("script returns nothing", "items.pop()")
    add("script returns null", "return null")
    add("script returns a number", "return 42")
    add("script variables", "$vars.count = items.length; $vars.names = items.map(i => i.name ?? '?');\n"
        "$vars.nested = {at: $now}", after=_n("e", "core.set", "Echo",
                                             values="{\"c\": \"{{ $vars.count }}\", \"n\": \"{{ $vars.names }}\", "
                                                    "\"a\": \"{{ $vars.nested.at }}\"}"))
    add("script random", "return [Math.random(), Math.random()].map(n => ({n}))", randoms=[0.25, 0.75])
    add("script utils", "return {\n"
        "  sha: $utils.hash('abc'), md5: $utils.hash('abc', 'md5', 'base64'),\n"
        "  hmac: $utils.hmac('data', 'key'), hmac512: $utils.hmac('data', 'key', 'sha512'),\n"
        "  b64: $utils.base64Encode('héllo'), back: $utils.base64Decode('aMOpbGxv'),\n"
        "  url: $utils.urlEncode('a b/é'), unurl: $utils.urlDecode('a%20b'),\n"
        "  date: $utils.formatDate(), day: $utils.formatDate($trigger.when, 'dddd D MMMM', 'Europe/Berlin'),\n"
        "  unix: $utils.formatDate(new Date(0), 'unix'), long: $utils.formatDate(1790000000, 'long'),\n"
        "  money: $utils.formatNumber(1234.567), plain: $utils.formatNumber(1234.567, 1, false),\n"
        "  nan: $utils.formatNumber('x')\n}")
    add("script logs objects", "console.log({a: [1, 'x']}, [1, [2]], null, undefined, true, 1.5, () => 1);\n"
        "console.info('info'); console.warn('warn'); console.error(new Error('e'));\n"
        "for (let i = 0; i < 150; i++) console.log(i); console.log('x'.repeat(3000))")
    add("script error keeps logs", "console.log('before');\nnull.boom")
    add("script mutation stays inside", "items[0].n = 99; items.length = 1; return []",
        after=_n("e", "core.set", "Echo", values="{\"seen\": \"{{ $node.Split.json.n }}\"}"))
    add("script braces stay code", "return {t: '{{ $json.n }}', u: `${'{{'}x}}`}")
    add("script default code", actions.DEFAULT_SCRIPT)
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

    randoms = list(case.get("randoms") or [])
    waits = []

    async def sleep(ms):
        waits.append(ms)

    async def load(ref):
        for entry in case.get("workflows") or []:
            if entry["id"] == ref:
                break
        else:
            for entry in case.get("workflows") or []:
                if entry["name"].strip().lower() == ref.lower():
                    break
            else:
                return None
        child, child_error = engine.clean_workflow(entry["workflow"])
        if child_error:
            return None
        return child, case["credentials"], entry["id"]

    max_requests, budget = runtime.LIMITS[case["trigger"]]
    report = await engine.execute(workflow, trigger_item, http, case["credentials"], max_requests, budget,
                                  lambda: 0, NOW_MS, WORKFLOW_ID, random=lambda: randoms.pop(0) if randoms else 0.5,
                                  sleep=sleep, load_workflow=load)
    report["trigger"] = case["trigger"]
    expect = {"requests": requests, "report": report}
    if waits:
        expect["waits"] = waits
    return expect


def cases_payload() -> dict:
    cases = flow_cases() + action_cases() + script_cases() + template_cases() + operation_cases()
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
