#!/usr/bin/env python3
import importlib.util
import json
import os
import tempfile
from pathlib import Path

import pytest

_SANDBOX = tempfile.mkdtemp(prefix="fm-mcp-terminals-")
for _name in ("FORKMESH_HOME", "FORKMESH_PROJECTS_DIR", "FORKMESH_PROJECT",
              "FORKMESH_PROJECT_DIR", "FORKMESH_DATA_DIR", "FORKMESH_CONFIG_DIR",
              "FORKMESH_MCP_TOKEN", "FORKMESH_MCP_CONNECTOR",
              "FORKMESH_AGENT_INTAKE", "FORKMESH_AGENT_INTAKE_TOKEN",
              "FORKMESH_TERMINAL_ID"):
    os.environ.pop(_name, None)
os.environ["HOME"] = _SANDBOX
os.environ["XDG_DATA_HOME"] = os.path.join(_SANDBOX, "data")
os.environ["XDG_CONFIG_HOME"] = os.path.join(_SANDBOX, "config")
os.environ["XDG_CACHE_HOME"] = os.path.join(_SANDBOX, "cache")

HERE = Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location(
    "fm_mcp_terminals", HERE / "forkmesh_mcp_server.py")
srv = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(srv)

TERM = "0123456789abcdef"


@pytest.fixture(autouse=True)
def _clean_channel(monkeypatch):
    monkeypatch.delenv("FORKMESH_AGENT_INTAKE", raising=False)
    monkeypatch.delenv("FORKMESH_AGENT_INTAKE_TOKEN", raising=False)
    monkeypatch.setattr(srv, "connector_record", lambda: {})


def call_tool(name, arguments):
    reply = srv.handle_request({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                                "params": {"name": name, "arguments": arguments}})
    result = reply["result"]
    return result["content"][0]["text"], bool(result.get("isError"))


def test_terminal_tools_are_listed_with_read_hints():
    listed = {t["name"]: t for t in srv.tools_list_payload()["tools"]}
    for name in srv.TERMINAL_TOOLS:
        assert name in listed
    assert listed["list_terminals"]["annotations"]["readOnlyHint"] is True
    assert listed["read_terminal"]["annotations"]["readOnlyHint"] is True
    assert listed["type_in_terminal"]["annotations"]["readOnlyHint"] is False
    assert listed["run_in_terminal"]["inputSchema"]["required"] == ["command"]


def test_instructions_name_the_terminal_tools():
    reply = srv.handle_request({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                                "params": {}})
    assert "run_in_terminal" in reply["result"]["instructions"]


@pytest.mark.parametrize("args", [
    {"terminal_id": TERM},
    {"terminal_id": "../etc", "text": "ls"},
    {"terminal_id": TERM, "keys": ["hyper-q"]},
    {"terminal_id": TERM, "text": "x" * (srv.TERMINAL_MAX_INPUT + 1)},
    {"terminal_id": TERM, "enter": "yes"},
])
def test_bad_input_is_refused_before_any_request(args, monkeypatch):
    monkeypatch.setattr(srv, "agent_intake_request",
                        lambda *a, **k: pytest.fail("no request expected"))
    with pytest.raises(ValueError):
        srv.terminal_input_params(args)


def test_open_params_check_kind_and_command():
    assert srv.open_terminal_params({}) == {"kind": "shell"}
    assert srv.open_terminal_params({"kind": "command", "command": "npm test"}) == {
        "kind": "command", "command": "npm test"}
    with pytest.raises(ValueError):
        srv.open_terminal_params({"kind": "shell", "command": "rm -rf ~"})
    with pytest.raises(ValueError):
        srv.open_terminal_params({"kind": "command"})
    with pytest.raises(ValueError):
        srv.open_terminal_params({"kind": "python"})
    with pytest.raises(ValueError):
        srv.open_terminal_params({"repository": "no-slash"})


def test_type_sends_text_keys_and_enter(monkeypatch):
    sent = []

    def fake(method, params, **kwargs):
        sent.append((method, params))
        return {"ok": True, "terminal": TERM}

    monkeypatch.setattr(srv, "agent_intake_request", fake)
    text, error = call_tool("type_in_terminal", {"terminal_id": TERM.upper(), "text": "ls",
                                                 "keys": ["Ctrl-C"], "enter": True})
    assert not error, text
    assert sent == [("terminals.input", {"terminal": TERM, "text": "ls",
                                         "keys": ["ctrl-c"], "enter": True})]


def test_output_since_drops_what_was_already_on_screen():
    assert srv.terminal_output_since("a\n$ ", "a\n$ ls\nfile\n$ ") == "$ ls\nfile\n$ "
    assert srv.terminal_output_since("", "hello") == "hello"
    assert srv.terminal_output_since("x\ny", "y\nz") == "z"


def test_run_types_the_command_and_waits_for_quiet(monkeypatch):
    calls = []
    screens = iter(["prompt$ ", "prompt$ make\nbuilt\nprompt$ "])
    idle = iter([10, 5000])

    def fake(method, params, **kwargs):
        calls.append(method)
        if method == "terminals.read":
            return {"ok": True, "terminal": TERM, "running": True, "text": next(screens)}
        if method == "terminals.list":
            return {"ok": True, "terminals": [{"terminal": TERM, "running": True,
                                               "idle_ms": next(idle)}]}
        return {"ok": True, "terminal": TERM}

    monkeypatch.setattr(srv, "agent_intake_request", fake)
    clock = iter(range(1000))
    result = srv.run_in_terminal({"terminal_id": TERM, "command": "make\n"},
                                 sleep=lambda _s: None, clock=lambda: next(clock))
    assert result["settled"] is True
    assert result["output"] == "prompt$ make\nbuilt\nprompt$ "
    assert calls == ["terminals.read", "terminals.input", "terminals.list",
                     "terminals.list", "terminals.read"]


def test_run_gives_up_at_the_deadline(monkeypatch):
    def fake(method, params, **kwargs):
        if method == "terminals.read":
            return {"ok": True, "text": ""}
        if method == "terminals.list":
            return {"ok": True, "terminals": [{"terminal": TERM, "running": True,
                                               "idle_ms": 0}]}
        return {"ok": True}

    monkeypatch.setattr(srv, "agent_intake_request", fake)
    clock = iter(range(0, 10000, 10))
    result = srv.run_in_terminal({"terminal_id": TERM, "command": "yes",
                                  "timeout_seconds": 30},
                                 sleep=lambda _s: None, clock=lambda: next(clock))
    assert result["settled"] is False


def test_desktop_refusal_becomes_a_tool_error(monkeypatch):
    monkeypatch.setattr(srv, "agent_intake_request", lambda *a, **k: {
        "ok": False, "error": "mode_too_low", "message": "only Auto mode"})
    text, error = call_tool("type_in_terminal", {"terminal_id": TERM, "text": "ls"})
    assert error
    assert "mode_too_low" in text


def test_read_asks_for_a_large_reply(monkeypatch):
    seen = {}

    def fake(method, params, max_reply=None):
        seen["max_reply"] = max_reply
        return {"ok": True, "text": "hi"}

    monkeypatch.setattr(srv, "agent_intake_request", fake)
    text, error = call_tool("read_terminal", {"terminal_id": TERM, "lines": 10})
    assert not error, text
    assert json.loads(text)["text"] == "hi"
    assert seen["max_reply"] == srv.TERMINAL_READ_REPLY


def test_env_file_loads_the_forkmesh_server_env(monkeypatch, tmp_path):
    for name in ("FORKMESH_AGENT_INTAKE", "FORKMESH_AGENT_INTAKE_TOKEN",
                 "FORKMESH_TERMINAL_ID", "FORKMESH_REPOS_DIR"):
        monkeypatch.setenv(name, "")
        monkeypatch.delenv(name)
    path = tmp_path / "mcp-term.json"
    path.write_text(json.dumps({"mcpServers": {"forkmesh": {"env": {
        "FORKMESH_AGENT_INTAKE": "/run/sock",
        "FORKMESH_AGENT_INTAKE_TOKEN": "term.abc",
        "FORKMESH_TERMINAL_ID": TERM,
        "PATH": "/evil",
        "FORKMESH_REPOS_DIR": 7}}}}))
    path_before = os.environ.get("PATH")
    assert srv.load_env_file(str(path)) == 3
    assert os.environ["FORKMESH_AGENT_INTAKE_TOKEN"] == "term.abc"
    assert os.environ["FORKMESH_TERMINAL_ID"] == TERM
    assert os.environ.get("PATH") == path_before
    assert "FORKMESH_REPOS_DIR" not in os.environ
    assert srv.load_env_file(str(tmp_path / "missing.json")) == 0
