#!/usr/bin/env python3

import json
import sys
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import workflow_formats as formats  # noqa: E402
import workflow_templates as templates  # noqa: E402
import workflows as engine  # noqa: E402


N8N_DOCUMENT = {
    "name": "n8n export",
    "nodes": [
        {"id": "0f5532f9-36ba-4bef-86c7-30d607400b15", "name": "Webhook",
         "type": "n8n-nodes-base.webhook", "typeVersion": 2,
         "position": [0, 0], "parameters": {"path": "abc"}},
        {"id": "2", "name": "Check status", "type": "n8n-nodes-base.if",
         "typeVersion": 2, "position": [220, 0], "parameters": {
             "conditions": {"conditions": [
                 {"leftValue": "={{ $json.body.status }}",
                  "rightValue": "open",
                  "operator": {"type": "string", "operation": "equals"}},
                 {"leftValue": "x", "rightValue": "y",
                  "operator": {"type": "string", "operation": "equals"}}],
                 "combinator": "and"}}},
        {"id": "3", "name": "Notify", "type": "n8n-nodes-base.httpRequest",
         "typeVersion": 4.2, "position": [440, -100], "parameters": {
             "method": "POST", "url": "https://hooks.example.com/x",
             "sendHeaders": True, "headerParameters": {"parameters": [
                 {"name": "X-Token", "value": "abc"}]},
             "sendBody": True, "specifyBody": "json",
             "jsonBody": "={\"title\": \"{{ $('Webhook').item.json.body.title }}\"}"}},
        {"id": "4", "name": "Tidy", "type": "n8n-nodes-base.set",
         "typeVersion": 3.4, "position": [440, 100], "parameters": {
             "assignments": {"assignments": [
                 {"id": "a", "name": "state", "value": "closed",
                  "type": "string"}]}, "includeOtherFields": True}},
        {"id": "5", "name": "Slack", "type": "n8n-nodes-base.slack",
         "typeVersion": 2, "position": [660, 100], "parameters": {
             "channel": "#ops", "text": "done"},
         "credentials": {"slackApi": {"id": "9", "name": "Slack"}}},
        {"id": "6", "name": "Mystery", "type": "n8n-nodes-base.rssFeedRead",
         "typeVersion": 1, "position": [880, 100], "parameters": {}},
        {"id": "7", "name": "Note", "type": "n8n-nodes-base.stickyNote",
         "typeVersion": 1, "position": [0, 300], "parameters": {}},
    ],
    "connections": {
        "Webhook": {"main": [[{"node": "Check status", "type": "main",
                               "index": 0}]]},
        "Check status": {"main": [
            [{"node": "Notify", "type": "main", "index": 0}],
            [{"node": "Tidy", "type": "main", "index": 0}]]},
        "Tidy": {"main": [[{"node": "Slack", "type": "main", "index": 0}]]},
        "Slack": {"main": [[{"node": "Mystery", "type": "main",
                             "index": 0}]]},
    },
}


def _by_name(workflow):
    return {node["name"]: node for node in workflow["nodes"]}


def test_n8n_workflow_imports_with_mapped_steps_and_warnings():
    workflow, warnings = formats.from_document(json.dumps(N8N_DOCUMENT))
    nodes = _by_name(workflow)
    assert "Note" not in nodes
    assert nodes["Webhook"]["type"] == "core.webhook"
    assert nodes["Check status"]["type"] == "core.if"
    assert nodes["Check status"]["params"] == {
        "left": "{{ $json.body.status }}", "operator": "equals",
        "right": "open"}
    http = nodes["Notify"]["params"]
    assert http["method"] == "POST"
    assert json.loads(http["headers"]) == {"X-Token": "abc"}
    assert http["body"] == ('{"title": "{{ $node["Webhook"].json.body.'
                            'title }}"}')
    assert json.loads(nodes["Tidy"]["params"]["values"]) == {
        "state": "closed"}
    assert nodes["Slack"]["type"] == "app.slack"
    assert nodes["Slack"]["params"]["channel"] == "#ops"
    assert nodes["Mystery"]["type"] == "core.noop"
    outputs = {(nodes_by_id["from"], c["output"], nodes_by_id["to"])
               for c in workflow["connections"]
               for nodes_by_id in [{
                   "from": next(n["name"] for n in workflow["nodes"]
                                if n["id"] == c["from"]),
                   "to": next(n["name"] for n in workflow["nodes"]
                              if n["id"] == c["to"])}]}
    assert ("Check status", "true", "Notify") in outputs
    assert ("Check status", "false", "Tidy") in outputs
    text = "\n".join(warnings)
    assert "only the first condition" in text
    assert "Slack: imported as Slack" in text
    assert "Slack: choose a credential" in text
    assert "rssFeedRead is not available" in text


def test_document_wrappers_from_template_sites_are_unwrapped():
    wrapped = {"workflow": {"id": 1, "workflow": N8N_DOCUMENT}}
    workflow, _ = formats.from_document(wrapped)
    assert workflow["name"] == "n8n export"
    with pytest.raises(formats.FormatError):
        formats.from_document("not json")
    with pytest.raises(formats.FormatError):
        formats.from_document({"hello": "world"})


def test_extra_triggers_and_loops_are_neutralised():
    document = {
        "name": "loops",
        "nodes": [
            {"name": "A", "type": "n8n-nodes-base.manualTrigger",
             "position": [0, 0], "parameters": {}},
            {"name": "B", "type": "n8n-nodes-base.scheduleTrigger",
             "position": [0, 200], "parameters": {"rule": {"interval": [
                 {"field": "hours", "hoursInterval": 2}]}}},
            {"name": "C", "type": "n8n-nodes-base.noOp", "position": [200, 0],
             "parameters": {}},
            {"name": "D", "type": "n8n-nodes-base.noOp", "position": [400, 0],
             "parameters": {}},
        ],
        "connections": {
            "A": {"main": [[{"node": "C"}]]},
            "C": {"main": [[{"node": "D"}]]},
            "D": {"main": [[{"node": "C"}]]},
        },
    }
    workflow, warnings = formats.from_document(document)
    nodes = _by_name(workflow)
    assert nodes["B"]["type"] == "core.noop"
    assert len(workflow["connections"]) == 2
    assert any("loop back" in w for w in warnings)


def test_export_is_n8n_shaped_and_round_trips():
    for item in templates.TEMPLATES:
        workflow, error = engine.clean_workflow(item["workflow"])
        assert error == ""
        document = formats.to_document(workflow)
        assert document["meta"]["format"] == formats.FORMAT
        for node in document["nodes"]:
            assert isinstance(node["position"], list)
            assert node["type"].startswith(("n8n-nodes-base.", "forkmesh.app.")) \
                or node["forkmeshType"] in engine.actions.NODES
        back, warnings = formats.from_document(json.dumps(document))
        assert warnings == []
        assert [(n["type"], n["name"], n["params"]) for n in back["nodes"]] \
            == [(n["type"], n["name"], n["params"]) for n in workflow["nodes"]]


def test_core_steps_keep_their_logic_without_forkmesh_fields():
    workflow, _ = engine.clean_workflow(templates.template(
        "uptime-alert")["workflow"])
    document = formats.to_document(workflow)
    for node in document["nodes"]:
        node.pop("forkmeshType")
        node.pop("forkmeshParams")
    back, _ = formats.from_document(document)
    nodes = _by_name(back)
    assert nodes["Every 5 minutes"]["params"] == {"every": "5"}
    assert nodes["Request failed?"]["params"]["operator"] == "isNotEmpty"
    assert nodes["Check the site"]["continueOnFail"] is True


def test_twelve_examples_are_valid_runnable_workflows():
    assert len(templates.TEMPLATES) == 12
    keys = [t["key"] for t in templates.TEMPLATES]
    assert len(set(keys)) == 12
    for item in templates.TEMPLATES:
        workflow, error = engine.clean_workflow(item["workflow"])
        assert error == "", item["key"]
        assert engine.trigger_node(workflow) is not None
        assert item["description"] and item["tags"]
    listed = templates.template_list()
    assert [t["key"] for t in listed] == keys
    assert templates.template("nope") is None
