#!/usr/bin/env python3

import asyncio
import json
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import workflow_actions as actions  # noqa: E402
import workflow_formats as formats  # noqa: E402
import workflows as engine  # noqa: E402

NOW = 1_790_000_000_000


def _node(node_id, node_type, title, **params):
    return {"id": node_id, "type": node_type, "name": title, "params": params}


def _chain(*nodes, payload=None):
    trigger = {"id": "t", "type": "core.manual", "name": "Start",
               "params": {"payload": json.dumps(payload or {})}}
    all_nodes = [trigger] + list(nodes)
    connections = [{"from": a["id"], "to": b["id"]}
                   for a, b in zip(all_nodes, all_nodes[1:])]
    workflow, error = engine.clean_workflow(
        {"name": "Test", "nodes": all_nodes, "connections": connections})
    assert error == "", error
    return workflow


def _run(workflow, trigger=None, randoms=None, loader=None, clock=None,
         deadline=NOW + 60_000):
    waits = []
    values = list(randoms or [])

    async def sleep(ms):
        waits.append(ms)

    async def http(method, url, headers, body):
        return 200, json.dumps({"url": url})

    if trigger is None:
        start = engine.trigger_node(workflow)
        trigger = json.loads(start["params"].get("payload") or "{}")
    report = asyncio.run(engine.execute(
        workflow, trigger, http, {}, 25,
        deadline, clock or (lambda: NOW), NOW, "wf",
        random=(lambda: values.pop(0)) if randoms is not None else None,
        sleep=sleep, load_workflow=loader))
    return report, waits


def _items(report, node_id, output="main"):
    return report["nodes"][node_id]["outputs"][output]["items"]


def _one(node_type, payload, **params):
    report, _ = _run(_chain(_node("a", node_type, "Step", **params)),
                     payload)
    assert report["status"] == "success", report
    return _items(report, "a")


def test_every_new_step_is_in_the_catalog_with_groups_in_order():
    catalog = engine.catalog()
    core = {spec["type"]: spec for spec in catalog["core"]}
    for node_type, spec in actions.NODES.items():
        assert core[node_type]["group"] in actions.GROUP_ORDER
        assert spec["fields"] or node_type == "core.comment" or spec["fields"] == []
        for field in spec["fields"]:
            assert field["type"] in ("string", "number", "boolean", "select",
                                     "json", "text", "code", "value")
            if field["type"] == "select":
                assert field.get("default") in [o["value"] for o in field["options"]]
    groups = [spec["group"] for spec in catalog["core"]]
    assert groups == sorted(groups, key=actions.GROUP_ORDER.index)
    assert len(actions.NODES) == 31
    assert {"core.code", "core.variable", "core.wait", "core.stop",
            "core.switch", "core.repeat", "core.runWorkflow"} <= set(core)


def test_text_steps():
    payload = {"text": "hello big world"}
    assert _one("core.textCase", payload, case="words")[0]["text"] == \
        "Hello Big World"
    assert _one("core.textCase", payload, case="upper")[0]["text"] == \
        "HELLO BIG WORLD"
    assert _one("core.textCase", payload, case="capitalize")[0]["text"] == \
        "Hello big world"
    assert _one("core.textReplace", payload, find="o", replace="0",
                field="out")[0]["out"] == "hell0 big w0rld"
    assert _one("core.textReplace", payload, find="(b)(i)g", replace="$2$1",
                regex="true", field="out")[0]["out"] == "hello ib world"
    assert _one("core.textReplace", {"text": "A a"}, find="a", replace="$1",
                ignoreCase="true")[0]["text"] == "$1 $1"
    assert _one("core.textMatch", {"text": "a1 b22 c333"}, pattern="[0-9]+")[
        0]["matches"] == ["1", "22", "333"]
    assert _one("core.textMatch", {"text": "id=42"}, pattern="id=(\\d+)",
                result="groups")[0]["matches"] == ["42"]
    assert _one("core.textMatch", {"text": "x"}, pattern="y",
                result="test")[0]["matches"] is False
    assert _one("core.textSplit", {"text": "a\r\nb\n\nc"})[0]["parts"] == \
        ["a", "b", "c"]
    assert _one("core.textSplit", {"text": "a, b,,c"}, separator="commas",
                skipEmpty="false")[0]["parts"] == ["a", "b", "", "c"]
    assert _one("core.textSplit", {"text": "héé"}, separator="characters")[
        0]["parts"] == ["h", "é", "é"]
    assert _one("core.textCombine", {"parts": ["a", 1, True]},
                separator="commas")[0]["text"] == "a, 1, true"
    assert _one("core.textEncode", {"text": "é&<"}, method="base64Encode")[
        0]["text"] == "w6kmPA=="
    assert _one("core.textEncode", {"text": "w6kmPA"}, method="base64Decode")[
        0]["text"] == "é&<"
    assert _one("core.textEncode", {"text": "a b/é"}, method="urlEncode")[
        0]["text"] == "a%20b%2F%C3%A9"
    assert _one("core.textEncode", {"text": "a%20b%2F%C3%A9%zz"},
                method="urlDecode")[0]["text"] == "a b/é%zz"
    assert _one("core.textEncode", {"text": "<a href='x'>&</a>"},
                method="htmlEscape")[0]["text"] == \
        "&lt;a href=&#x27;x&#x27;&gt;&amp;&lt;/a&gt;"
    assert _one("core.textEncode", {"text": "&lt;&#65;&#x42;&bogus;&amp;"},
                method="htmlUnescape")[0]["text"] == "<AB&bogus;&"
    assert _one("core.hash", {"text": "abc"}, algorithm="md5")[0]["hash"] == \
        "900150983cd24fb0d6963f7d28e17f72"
    assert _one("core.hash", {"text": "abc"}, algorithm="sha256")[0][
        "hash"].startswith("ba7816bf")
    assert _one("core.hash", {"text": "data"}, algorithm="sha256", key="k",
                encoding="base64")[0]["hash"] == \
        "mHVc56DUMbXXuOyONDraO8qk9sRMpGjP/LqpwnNjTdc="
    found = _one("core.textExtract", {"text": "See https://a.example/x?y=1). "
                                              "Mail bo@ex.org or +1 (555) 123-4567, #tag @me 1,234.5"},
                 kind="urls")[0]["found"]
    assert found == ["https://a.example/x?y=1"]
    text = {"text": "Mail bo@ex.org or +1 (555) 123-4567, #tag @me 1,234.5"}
    assert _one("core.textExtract", text, kind="emails")[0]["found"] == ["bo@ex.org"]
    assert _one("core.textExtract", text, kind="phones")[0]["found"] == \
        ["+1 (555) 123-4567"]
    assert _one("core.textExtract", text, kind="hashtags")[0]["found"] == ["#tag"]
    assert _one("core.textExtract", text, kind="mentions")[0]["found"] == ["@me"]
    assert _one("core.textExtract", {"text": "-3 and 1,234.5 and 7"},
                kind="numbers")[0]["found"] == [-3, 1234.5, 7]
    assert _one("core.count", {"text": "two words\nand more\n"}, what="words")[
        0]["count"] == 4
    assert _one("core.count", {"text": "a\nb\n"}, what="lines")[0]["count"] == 2
    assert _one("core.count", {"text": "😀é"}, what="characters")[0]["count"] == 2
    assert _one("core.count", {"text": [1, 2, 3]}, what="items")[0]["count"] == 3


def test_number_steps():
    assert _one("core.math", {"p": 4}, a="{{ $json.p }}", operator="multiply",
                b="2.5")[0]["result"] == 10
    assert _one("core.math", {}, a="-7", operator="modulo", b="3")[0][
        "result"] == 2
    assert _one("core.math", {}, a="9", operator="sqrt")[0]["result"] == 3
    report, _ = _run(_chain(_node("a", "core.math", "Div", a="1",
                                  operator="divide", b="0")))
    assert report["error"] == "Div: Cannot divide by zero"
    report, _ = _run(_chain(_node("a", "core.math", "Bad", a="x",
                                  operator="add", b="1")))
    assert report["error"] == "Bad: Number must be a number"
    assert _one("core.round", {}, value="2.675", decimals="2")[0]["result"] == 2.68
    assert _one("core.round", {}, value="-2.5")[0]["result"] == -3
    assert _one("core.round", {}, value="1234", decimals="-2")[0]["result"] == 1200
    assert _one("core.round", {}, value="1.21", decimals="1", mode="up")[0][
        "result"] == 1.3
    assert _one("core.round", {}, value="-1.21", decimals="1", mode="down")[0][
        "result"] == -1.3
    assert _one("core.formatNumber", {}, value="1234567.891")[0]["text"] == \
        "1,234,567.89"
    assert _one("core.formatNumber", {}, value="-0.001", decimals="2",
                grouping="false")[0]["text"] == "0.00"
    report, _ = _run(_chain(_node("a", "core.random", "Dice", min="1",
                                  max="6")), randoms=[0.999])
    assert _items(report, "a")[0]["number"] == 6
    report, _ = _run(_chain(_node("a", "core.random", "Real", min="0",
                                  max="2", whole="false")), randoms=[0.25])
    assert _items(report, "a")[0]["number"] == 0.5


def _list_chain(*nodes, values=(3, 1, 2)):
    split = _node("s", "core.split", "Split", field="list")
    return _chain(split, *nodes, payload={"list": [
        {"n": value, "name": "x" + str(value)} for value in values]})


def test_list_steps():
    report, _ = _run(_list_chain(_node("a", "core.statistics", "Stats",
                                       value="{{ $json.n }}",
                                       operation="average")))
    assert _items(report, "a") == [{"result": 2}]
    for operation, expected in (("sum", 6), ("median", 2), ("min", 1),
                                ("max", 3), ("range", 2), ("count", 3),
                                ("stdev", 1)):
        report, _ = _run(_list_chain(_node("a", "core.statistics", "Stats",
                                           value="{{ $json.n }}",
                                           operation=operation, field="r")))
        assert _items(report, "a") == [{"r": expected}], operation
    report, _ = _run(_list_chain(_node("a", "core.combineItems", "All",
                                       value="{{ $json.n }}", field="ns")))
    assert _items(report, "a") == [{"ns": [3, 1, 2]}]
    report, _ = _run(_list_chain(_node("a", "core.sort", "Sort", by="n")))
    assert [i["n"] for i in _items(report, "a")] == [1, 2, 3]
    report, _ = _run(_list_chain(_node("a", "core.sort", "Sort", by="name",
                                       order="descending")))
    assert [i["n"] for i in _items(report, "a")] == [3, 2, 1]
    report, _ = _run(_list_chain(_node("a", "core.sort", "Sort",
                                       order="shuffle")), randoms=[0.0, 0.0])
    assert [i["n"] for i in _items(report, "a")] == [1, 2, 3]
    report, _ = _run(_list_chain(_node("a", "core.dedupe", "Unique", by="n"),
                                 values=(1, 2, 1, 3, 2)))
    assert [i["n"] for i in _items(report, "a")] == [1, 2, 3]
    for which, extra, expected in (("first", {}, [3]), ("last", {}, [2]),
                                   ("index", {"index": "2"}, [1]),
                                   ("index", {"index": "-1"}, [2]),
                                   ("index", {"index": "9"}, []),
                                   ("range", {"index": "2", "to": "3"}, [1, 2])):
        report, _ = _run(_list_chain(_node("a", "core.pick", "Pick",
                                           which=which, **extra)))
        assert [i["n"] for i in _items(report, "a")] == expected
    assert _one("core.json", {"body": '{"a": [1, 2]}'})[0]["data"] == {"a": [1, 2]}
    assert _one("core.json", {"body": {"a": 1}}, operation="stringify",
                field="text")[0]["text"] == '{"a": 1}'
    report, _ = _run(_chain(_node("a", "core.json", "Parse")),
                     {"body": "{nope"})
    assert report["error"] == "Parse: The text is not valid JSON"
    parts = _one("core.url", {"url": "HTTPS://User@Example.com:8443/a/b?x=1&y=a+b&x=2#top"})[0]["link"]
    assert parts == {"scheme": "https", "host": "example.com", "port": 8443,
                     "path": "/a/b", "query": {"x": "2", "y": "a b"},
                     "fragment": "top", "origin": "https://example.com:8443",
                     "user": "User"}


def test_date_steps():
    when = "2026-03-29T00:30:00Z"
    assert _one("core.dateFormat", {}, value=when)[0]["date"] == \
        "2026-03-29T00:30:00Z"
    assert _one("core.dateFormat", {}, value=when, format="custom",
                pattern="dddd D MMM YYYY, h:mm A [at] Z",
                timeZone="Europe/Berlin")[0]["date"] == \
        "Sunday 29 Mar 2026, 1:30 AM at +01:00"
    assert _one("core.dateFormat", {}, value="2026-03-29 03:30",
                timeZone="Europe/Berlin")[0]["date"] == \
        "2026-03-29T03:30:00+02:00"
    assert _one("core.dateFormat", {}, value="1790000000", format="date")[
        0]["date"] == "2026-09-21"
    assert _one("core.dateFormat", {}, value=when, format="unix")[0]["date"] == \
        1774744200
    assert _one("core.dateAdjust", {}, value="2026-01-31T10:00:00Z",
                amount="1", unit="months")[0]["date"] == "2026-02-28T10:00:00Z"
    assert _one("core.dateAdjust", {}, value="2026-03-28T12:00:00+01:00",
                amount="1", unit="days", timeZone="Europe/Berlin")[0][
        "date"] == "2026-03-29T12:00:00+02:00"
    assert _one("core.dateAdjust", {}, value="2026-10-15T18:20:00Z",
                operation="startOf", unit="weeks")[0]["date"] == \
        "2026-10-12T00:00:00Z"
    assert _one("core.dateAdjust", {}, value="2026-02-10T18:20:00Z",
                operation="endOf", unit="months")[0]["date"] == \
        "2026-02-28T23:59:59Z"
    assert _one("core.dateAdjust", {}, value="2026-02-10T18:20:00Z",
                operation="subtract", amount="90", unit="minutes")[0][
        "date"] == "2026-02-10T16:50:00Z"
    assert _one("core.dateDiff", {}, start="2026-01-31", end="2026-03-30",
                unit="months")[0]["difference"] == 1
    assert _one("core.dateDiff", {}, start="2026-03-30", end="2026-01-31",
                unit="days")[0]["difference"] == -58
    report, _ = _run(_chain(_node("a", "core.dateFormat", "Fmt",
                                  value="next tuesday")))
    assert report["error"] == "Fmt: Cannot read the date next tuesday"
    report, _ = _run(_chain(_node("a", "core.dateFormat", "Fmt",
                                  value="2026-01-01", timeZone="Mars/Base")))
    assert report["error"] == "Fmt: Unknown time zone Mars/Base"


def test_flow_steps_variables_switch_repeat_wait_and_stop():
    nodes = [
        _node("s", "core.split", "Split", field="list"),
        _node("v", "core.variable", "Total", name="total", value="{{ $json.n }}",
              mode="add"),
        _node("l", "core.variable", "Names", name="names",
              value="{{ $json.name }}", mode="append"),
        _node("c", "core.switch", "Choose", value="{{ $json.name }}",
              case1="b", case2="a"),
    ]
    trigger = {"id": "t", "type": "core.manual", "name": "Start",
               "params": {"payload": json.dumps({"list": [
                   {"n": 1, "name": "a"}, {"n": 2, "name": "b"},
                   {"n": 4, "name": "z"}]})}}
    after = _node("e", "core.set", "Echo", values=json.dumps(
        {"total": "{{ $vars.total }}", "names": "{{ $vars.names }}"}))
    workflow, error = engine.clean_workflow({"name": "Vars", "nodes": [
        trigger] + nodes + [after], "connections": [
        {"from": "t", "to": "s"}, {"from": "s", "to": "v"},
        {"from": "v", "to": "l"}, {"from": "l", "to": "c"},
        {"from": "c", "output": "otherwise", "to": "e"}]})
    assert error == ""
    report, _ = _run(workflow)
    assert report["status"] == "success"
    outputs = report["nodes"]["c"]["outputs"]
    assert [i["name"] for i in outputs["case1"]["items"]] == ["b"]
    assert [i["name"] for i in outputs["case2"]["items"]] == ["a"]
    assert outputs["case3"]["count"] == 0
    assert _items(report, "e") == [{"n": 4, "name": "z", "total": 7,
                                    "names": ["a", "b", "z"]}]
    assert [i["repeatIndex"] for i in _one("core.repeat", {"x": 1},
                                           times="3")] == [1, 2, 3]
    report, waits = _run(_chain(_node("w", "core.wait", "Pause", seconds="1.5"),
                                _node("n", "core.noop", "After")))
    assert waits == [1500] and report["nodes"]["n"]["status"] == "success"
    report, waits = _run(_chain(_node("w", "core.wait", "Pause", seconds="50")),
                         deadline=NOW + 45_000)
    assert waits == []
    assert report["error"] == ("Pause: Waiting would run past this run's "
                               "time limit")
    report, _ = _run(_chain(_node("x", "core.stop", "Done"),
                            _node("n", "core.noop", "Never")))
    assert report["status"] == "success" and report["stopped"] == "Done"
    assert report["nodes"]["n"]["status"] == "skipped"
    report, _ = _run(_chain(_node("x", "core.stop", "Halt", outcome="error",
                                  message="Nothing to do")))
    assert report["error"] == "Halt: Nothing to do"
    report, _ = _run(_chain(_node("v", "core.variable", "Bad", name="1x",
                                  value="1")))
    assert report["error"] == "Bad: Variable names use letters, digits and _"


def test_if_matches_a_regular_expression():
    assert engine.check_condition("order-123", "matches", "^order-[0-9]+$")
    assert engine.check_condition("Order", "notMatches", "^order")
    with pytest.raises(engine.NodeError):
        engine.check_condition("x", "matches", "(")


def test_run_workflow_uses_the_loader_shares_limits_and_stops_deep_nesting():
    child, _ = engine.clean_workflow({"name": "Double", "nodes": [
        {"id": "t", "type": "core.manual", "name": "Start", "params": {}},
        _node("m", "core.math", "Times", a="{{ $json.n }}", operator="multiply",
              b="2", field="n"),
    ], "connections": [{"from": "t", "to": "m"}]})
    loop, _ = engine.clean_workflow({"name": "Loop", "nodes": [
        {"id": "t", "type": "core.manual", "name": "Start", "params": {}},
        _node("r", "core.runWorkflow", "Again", workflow="Loop"),
    ], "connections": [{"from": "t", "to": "r"}]})
    asked = []

    async def loader(ref):
        asked.append(ref)
        found = {"double": child, "loop": loop}.get(ref.lower())
        return (found, {}, "id-" + ref) if found else None

    report, _ = _run(_chain(_node("r", "core.runWorkflow", "Run",
                                  workflow="Double")), {"n": 21},
                     loader=loader)
    assert report["status"] == "success"
    assert _items(report, "r") == [{"n": 42}]
    report, _ = _run(_chain(_node("r", "core.runWorkflow", "Run",
                                  workflow="Missing")), loader=loader)
    assert report["error"] == "Run: No workflow named Missing"
    report, _ = _run(_chain(_node("r", "core.runWorkflow", "Run",
                                  workflow="Loop")), loader=loader)
    assert report["error"] == ("Run: Loop failed: Again: Loop failed: Again: "
                               "Workflows can run other workflows at most 2 deep")


def test_script_step_runs_all_items_or_each_item_and_keeps_variables():
    code = ("console.log('count', items.length);\n"
            "$vars.seen = items.map(i => i.n);\n"
            "return items.filter(i => i.n > 1).map(i => ({...i, double: i.n * 2}))")
    report, _ = _run(_list_chain(_node("c", "core.code", "Script", code=code),
                                 _node("e", "core.set", "Echo", values=json.dumps(
                                     {"seen": "{{ $vars.seen }}"}))))
    assert report["status"] == "success", report
    assert report["nodes"]["c"]["logs"] == ["count 3"]
    assert _items(report, "c") == [{"n": 3, "name": "x3", "double": 6},
                                   {"n": 2, "name": "x2", "double": 4}]
    assert _items(report, "e")[0]["seen"] == [3, 1, 2]
    report, _ = _run(_list_chain(_node("c", "core.code", "Script", mode="each",
                                       code="$json.index = $index; "
                                            "if ($json.n === 1) return null")))
    assert [(i["n"], i["index"]) for i in _items(report, "c")] == [(3, 0), (2, 2)]
    report, _ = _run(_chain(_node("c", "core.code", "Script",
                                  code="return [{json: {a: 1}}, 5]")))
    assert _items(report, "c") == [{"a": 1}, {"value": 5}]
    report, _ = _run(_chain(_node("c", "core.code", "Broken",
                                  code="console.log('before');\nnope()")))
    assert report["error"] == "Broken: Line 2: ReferenceError: nope is not defined"
    assert report["nodes"]["c"]["logs"] == ["before"]
    report, _ = _run(_chain(_node("c", "core.code", "Syntax", code="return (")))
    assert report["error"] == "Syntax: Line 1: Unexpected end of script"


def test_script_code_is_not_treated_as_an_expression():
    report, _ = _run(_chain(_node("c", "core.code", "Script",
                                  code="return {text: '{{ $json.x }}'}")),
                     {"x": 1})
    assert _items(report, "c") == [{"text": "{{ $json.x }}"}]


def test_n8n_code_wait_and_sort_nodes_import():
    document = {"nodes": [
        {"name": "Start", "type": "n8n-nodes-base.manualTrigger",
         "parameters": {}, "position": [0, 0]},
        {"name": "Code", "type": "n8n-nodes-base.code", "position": [200, 0],
         "parameters": {"jsCode": "return $input.all();",
                        "mode": "runOnceForEachItem"}},
        {"name": "Wait", "type": "n8n-nodes-base.wait", "position": [400, 0],
         "parameters": {"amount": 2, "unit": "minutes"}},
        {"name": "Dedupe", "type": "n8n-nodes-base.removeDuplicates",
         "position": [600, 0], "parameters": {}},
        {"name": "Sort", "type": "n8n-nodes-base.sort", "position": [800, 0],
         "parameters": {"sortFieldsUi": {"sortField": [
             {"fieldName": "n", "order": "descending"}]}}},
    ], "connections": {}}
    workflow, warnings = formats.from_document(document)
    by_name = {node["name"]: node for node in workflow["nodes"]}
    assert by_name["Code"]["type"] == "core.code"
    assert by_name["Code"]["params"] == {"mode": "each",
                                         "code": "return $input.all();"}
    assert by_name["Wait"]["params"] == {"seconds": "60"}
    assert by_name["Dedupe"]["type"] == "core.dedupe"
    assert by_name["Sort"]["params"] == {"by": "n", "order": "descending"}
    assert any("Wait" in warning for warning in warnings)
    exported = formats.to_document(workflow)
    again, _ = formats.from_document(exported)
    assert [n["params"] for n in again["nodes"]] == \
        [n["params"] for n in workflow["nodes"]]


def test_a_runaway_regular_expression_is_stopped():
    report, _ = _run(_chain(_node("m", "core.textMatch", "Match",
                                  pattern="(a+)+$", result="test")),
                     {"text": "a" * 40 + "b"})
    assert report["error"] == ("Match: Regular expressions took too long in "
                               "this run")
    report, _ = _run(_chain(_node("c", "core.code", "Script",
                                  code="return /(a|aa)+$/.test('a'.repeat(60) + 'b')")))
    assert report["error"] == ("Script: Line 1: Regular expressions took too "
                               "long in this run")
