#!/usr/bin/env python3
import importlib.util
import json
import os
import tempfile
from pathlib import Path

import pytest

_SANDBOX = tempfile.mkdtemp(prefix="fm-mcp-agent-hub-")
for _name in ("FORKMESH_HOME", "FORKMESH_PROJECTS_DIR", "FORKMESH_PROJECT",
              "FORKMESH_PROJECT_DIR", "FORKMESH_DATA_DIR", "FORKMESH_CONFIG_DIR",
              "FORKMESH_MCP_TOKEN", "FORKMESH_MCP_CONNECTOR",
              "FORKMESH_AGENT_INTAKE", "FORKMESH_AGENT_INTAKE_TOKEN"):
    os.environ.pop(_name, None)
os.environ["HOME"] = _SANDBOX
os.environ["XDG_DATA_HOME"] = os.path.join(_SANDBOX, "data")
os.environ["XDG_CONFIG_HOME"] = os.path.join(_SANDBOX, "config")
os.environ["XDG_CACHE_HOME"] = os.path.join(_SANDBOX, "cache")

HERE = Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location(
    "fm_mcp_agent_hub", HERE / "forkmesh_mcp_server.py")
srv = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(srv)

WINDOW = "w0123456789abcdef"


@pytest.fixture(autouse=True)
def _clean_channel(monkeypatch):
    monkeypatch.delenv("FORKMESH_AGENT_INTAKE", raising=False)
    monkeypatch.delenv("FORKMESH_AGENT_INTAKE_TOKEN", raising=False)
    monkeypatch.setattr(srv, "connector_record", lambda: {})


@pytest.fixture
def desktop(monkeypatch):
    sent = []
    replies = []

    def fake(method, params, **_kw):
        sent.append((method, params))
        if replies:
            return dict(replies.pop(0))
        return {"ok": True, "window_id": WINDOW, "open": True}

    monkeypatch.setattr(srv, "agent_intake_request", fake)
    monkeypatch.setattr(srv.time, "sleep", lambda _s: None)
    return sent, replies


def call_tool(name, arguments):
    reply = srv.handle_request({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                                "params": {"name": name, "arguments": arguments}})
    result = reply["result"]
    return result["content"][0]["text"], bool(result.get("isError"))


def test_tools_are_listed_with_their_hints():
    listed = {t["name"]: t for t in srv.tools_list_payload()["tools"]}
    for name in ("suggest_session", "list_agents", "send_agent_message",
                 "list_runners", "notify_user", "show_widget", "browser_action"):
        assert name in listed
    assert listed["list_agents"]["annotations"]["readOnlyHint"] is True
    assert listed["list_runners"]["annotations"]["readOnlyHint"] is True
    for name in ("send_agent_message", "notify_user", "suggest_session",
                 "show_widget", "browser_action"):
        assert listed[name]["annotations"]["readOnlyHint"] is False
    assert listed["suggest_session"]["inputSchema"]["required"] == ["prompt"]
    assert listed["browser_action"]["inputSchema"]["properties"]["action"]["enum"] == \
        list(srv.BROWSER_ACTIONS)


def test_instructions_name_the_new_tools():
    reply = srv.handle_request({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                                "params": {}})
    text = reply["result"]["instructions"]
    for name in ("suggest_session", "list_agents", "send_agent_message",
                 "list_runners", "notify_user", "show_widget", "browser_action"):
        assert name in text


def test_suggest_session_normalizes_and_sends(desktop):
    sent, _ = desktop
    text, error = call_tool("suggest_session", {
        "prompt": "  Fix the flaky upload test  ", "title": " Flaky   upload ",
        "reason": " outside  this task ", "repository": "forkmesh/forkmesh"})
    assert not error
    assert sent == [("sessions.suggest", {
        "prompt": "Fix the flaky upload test", "title": "Flaky upload",
        "reason": "outside this task", "repository": "forkmesh/forkmesh"})]
    assert json.loads(text)["window_id"] == WINDOW


@pytest.mark.parametrize("arguments", [
    {},
    {"prompt": "   "},
    {"prompt": "x" * 8001},
    {"prompt": "x", "repository": "not a repo"},
    {"prompt": "x", "reason": "r" * 301},
    {"prompt": "x", "provider": "Bad Provider"},
    {"prompt": "x", "wait_seconds": 601},
])
def test_suggest_session_refuses_bad_input_before_any_request(arguments, desktop):
    sent, _ = desktop
    text, error = call_tool("suggest_session", arguments)
    assert error
    assert sent == []


def test_suggest_session_waits_for_the_users_choice(desktop):
    sent, replies = desktop
    replies.extend([
        {"ok": True, "window_id": WINDOW, "open": True, "answered": False,
         "suggestion": "offered"},
        {"ok": True, "window_id": WINDOW, "open": True, "answered": False,
         "suggestion": "offered"},
        {"ok": True, "window_id": WINDOW, "open": False, "answered": True,
         "button": "Start session", "suggestion": "started", "session_id": 41},
    ])
    state = srv.suggest_session({"prompt": "Do it", "wait_seconds": 30})
    assert state["suggestion"] == "started" and state["session_id"] == 41
    assert [m for m, _ in sent] == ["sessions.suggest", "windows.read", "windows.read"]


def test_list_agents_params(desktop):
    sent, _ = desktop
    call_tool("list_agents", {})
    call_tool("list_agents", {"include_finished": True, "limit": 5})
    assert sent == [("agents.list", {}),
                    ("agents.list", {"include_finished": True, "limit": 5})]
    for bad in ({"limit": 0}, {"limit": 101}, {"limit": True}, {"include_finished": "yes"}):
        _, error = call_tool("list_agents", bad)
        assert error
    assert len(sent) == 2


def test_send_agent_message_params(desktop):
    sent, _ = desktop
    _, error = call_tool("send_agent_message", {"agent_id": 3, "message": "  done  "})
    assert not error
    assert sent == [("agents.message", {"agent_id": 3, "message": "done"})]
    for bad in ({"agent_id": True, "message": "x"}, {"agent_id": "3", "message": "x"},
                {"agent_id": 0, "message": "x"}, {"agent_id": 3, "message": " "},
                {"agent_id": 3, "message": "x" * 8001}):
        _, error = call_tool("send_agent_message", bad)
        assert error
    assert len(sent) == 1


def test_desktop_refusals_become_tool_errors(desktop):
    _, replies = desktop
    replies.append({"ok": False, "error": "more_permissive",
                    "message": "Agent #3 runs in Auto mode"})
    text, error = call_tool("send_agent_message", {"agent_id": 3, "message": "go"})
    assert error and "more_permissive" in text


def test_list_runners_and_notify(desktop):
    sent, replies = desktop
    replies.append({"ok": True, "runners": [{"name": "r1", "status": "online"}]})
    text, error = call_tool("list_runners", {})
    assert not error and json.loads(text)["runners"][0]["name"] == "r1"
    _, error = call_tool("notify_user", {"title": "  Build   done ", "body": " green "})
    assert not error
    assert sent[-1] == ("desktop.notify", {"title": "Build done", "body": "green"})
    for bad in ({}, {"title": "x" * 121}, {"title": "x", "body": "b" * 1001}):
        _, error = call_tool("notify_user", bad)
        assert error
    assert len(sent) == 2


def test_show_widget_params(desktop):
    sent, _ = desktop
    _, error = call_tool("show_widget", {"title": "Counter", "html": "<b>0</b>",
                                         "data": {"n": 0}, "width": 300})
    assert not error
    assert sent[-1] == ("windows.widget", {"title": "Counter", "html": "<b>0</b>",
                                           "data": {"n": 0}, "width": 300})
    _, error = call_tool("show_widget", {"window_id": WINDOW, "data": [1, 2]})
    assert not error
    assert sent[-1] == ("windows.widget", {"window_id": WINDOW, "data": [1, 2]})
    for bad in ({"html": "<b>x</b>"}, {"title": "x"}, {"window_id": WINDOW},
                {"title": "x", "html": "h" * 50001},
                {"window_id": WINDOW, "data": "d" * 16001}):
        _, error = call_tool("show_widget", bad)
        assert error
    assert len(sent) == 2


def test_show_widget_waits_for_an_event(desktop):
    sent, replies = desktop
    replies.extend([
        {"ok": True, "window_id": WINDOW, "kind": "widget", "open": True,
         "answered": False, "events": []},
        {"ok": True, "window_id": WINDOW, "kind": "widget", "open": True,
         "answered": True, "events": [{"picked": "b"}]},
    ])
    state = srv.show_widget({"title": "Pick", "html": "<p>x</p>", "wait_seconds": 10})
    assert state["events"] == [{"picked": "b"}]
    assert [m for m, _ in sent] == ["windows.widget", "windows.read"]


def test_browser_action_params(desktop):
    sent, _ = desktop
    calls = [
        {"window_id": WINDOW, "action": "Click", "target": "  Sign in "},
        {"window_id": WINDOW, "action": "type", "selector": "#q", "value": " a ",
         "submit": True},
        {"window_id": WINDOW, "action": "type", "selector": "#q", "clear": True},
        {"window_id": WINDOW, "action": "navigate", "url": "http://localhost:3000/"},
        {"window_id": WINDOW, "action": "scroll", "delta": -400},
        {"window_id": WINDOW, "action": "wait_for", "selector": ".done", "wait_ms": 5000},
        {"window_id": WINDOW, "action": "press", "value": "Escape"},
    ]
    for arguments in calls:
        _, error = call_tool("browser_action", arguments)
        assert not error, arguments
    assert [p for _, p in sent] == [
        {"window_id": WINDOW, "action": "click", "target": "Sign in"},
        {"window_id": WINDOW, "action": "type", "selector": "#q", "value": " a ",
         "submit": True},
        {"window_id": WINDOW, "action": "type", "selector": "#q", "clear": True},
        {"window_id": WINDOW, "action": "navigate", "url": "http://localhost:3000/"},
        {"window_id": WINDOW, "action": "scroll", "delta": -400},
        {"window_id": WINDOW, "action": "wait_for", "selector": ".done", "wait_ms": 5000},
        {"window_id": WINDOW, "action": "press", "value": "Escape"},
    ]
    assert {m for m, _ in sent} == {"windows.act"}


@pytest.mark.parametrize("arguments", [
    {"action": "reload"},
    {"window_id": WINDOW, "action": "eval"},
    {"window_id": WINDOW, "action": "click"},
    {"window_id": WINDOW, "action": "navigate"},
    {"window_id": WINDOW, "action": "navigate", "url": "file:///etc/passwd"},
    {"window_id": WINDOW, "action": "press", "value": "  "},
    {"window_id": WINDOW, "action": "wait_for", "target": "x", "wait_ms": 9000},
    {"window_id": WINDOW, "action": "click", "target": "x", "submit": "yes"},
    {"window_id": WINDOW, "action": "click", "selector": "s" * 501},
])
def test_browser_action_refuses_bad_input(arguments, desktop):
    sent, _ = desktop
    _, error = call_tool("browser_action", arguments)
    assert error
    assert sent == []


@pytest.mark.parametrize("name,arguments", [
    ("list_agents", {}),
    ("send_agent_message", {"agent_id": 2, "message": "hi"}),
    ("list_runners", {}),
    ("notify_user", {"title": "hi"}),
])
def test_agent_tools_need_the_connector_token(name, arguments, desktop, monkeypatch):
    sent, _ = desktop
    monkeypatch.setattr(srv, "connector_record", lambda: {"token": "connector-secret"})
    monkeypatch.delenv("FORKMESH_MCP_TOKEN", raising=False)
    text, error = call_tool(name, arguments)
    assert error and "connector token" in text
    assert sent == []
    monkeypatch.setenv("FORKMESH_MCP_TOKEN", "connector-secret")
    text, error = call_tool(name, arguments)
    assert "connector token" not in text
    assert len(sent) == 1


def test_whoami_lists_the_agent_tools():
    text, _ = call_tool("whoami", {})
    assert set(json.loads(text)["agentTools"]) == srv.HUB_TOOLS


def test_without_the_desktop_channel_they_refuse():
    for name, arguments in (("list_agents", {}), ("suggest_session", {"prompt": "x"})):
        text, error = call_tool(name, arguments)
        assert error and "FORKMESH_AGENT_INTAKE" in text
