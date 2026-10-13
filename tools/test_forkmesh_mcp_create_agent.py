#!/usr/bin/env python3
import importlib.util
import json
import os
import socket
import sys
import tempfile
import threading
from pathlib import Path

import pytest

_SANDBOX = tempfile.mkdtemp(prefix="fm-mcp-agent-")
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
    "fm_mcp_create_agent", HERE / "forkmesh_mcp_server.py")
srv = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(srv)

GOOD = {"repository": "forkmesh/forkmesh", "prompt": "Fix the login redirect"}


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


def test_tool_is_listed_as_a_write():
    tool = srv.TOOLS_BY_NAME["create_agent"]
    schema = tool["inputSchema"]
    assert schema["required"] == ["repository", "prompt"]
    assert set(schema["properties"]) == {
        "repository", "prompt", "title", "branch", "worktree", "paused",
        "priority", "provider", "model"}
    assert schema["properties"]["prompt"]["maxLength"] == 8000
    assert schema["properties"]["paused"]["type"] == "boolean"
    assert "create_agent" in srv.WRITE_TOOLS
    assert "create_agent" not in srv.DESTRUCTIVE_TOOLS
    listed = {t["name"]: t for t in srv.tools_list_payload()["tools"]}
    assert listed["create_agent"]["annotations"]["readOnlyHint"] is False
    assert listed["create_agent"]["annotations"]["openWorldHint"] is False


def test_instructions_name_the_tool():
    reply = srv.handle_request({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                                "params": {}})
    assert "create_agent" in reply["result"]["instructions"]


def test_params_are_normalized():
    params = srv.create_agent_params({
        **GOOD, "title": "  Login   redirect ", "branch": "643-continue",
        "worktree": "/home/u/src/.worktrees/agent-643", "paused": True,
        "priority": 7, "provider": "claude-code", "model": "claude-opus-4-1[1m]"})
    assert params == {
        "repository": "forkmesh/forkmesh", "prompt": "Fix the login redirect",
        "title": "Login redirect", "branch": "643-continue",
        "worktree": "/home/u/src/.worktrees/agent-643", "paused": True,
        "priority": 7, "provider": "claude-code", "model": "claude-opus-4-1[1m]"}
    assert srv.create_agent_params(GOOD) == {**GOOD, "paused": False}


@pytest.mark.parametrize("change", [
    {"repository": "forkmesh"},
    {"repository": "a/b/c"},
    {"repository": "../etc"},
    {"repository": None},
    {"prompt": "   "},
    {"prompt": "x" * 8001},
    {"prompt": 5},
    {"title": "t" * 201},
    {"branch": "agent/../main"},
    {"branch": "-x"},
    {"branch": "a.lock"},
    {"worktree": "relative/path"},
    {"worktree": "/a/b/../c"},
    {"provider": "Claude Code"},
    {"model": "m; rm -rf"},
    {"paused": "yes"},
    {"priority": 0},
    {"priority": 100},
    {"priority": 2.5},
    {"priority": True},
    {"priority": "5"},
])
def test_bad_input_is_refused_before_any_request(change, monkeypatch):
    sent = []
    monkeypatch.setattr(srv, "agent_intake_request",
                        lambda method, params: sent.append(params) or {"ok": True})
    with pytest.raises(ValueError):
        srv.create_agent({**GOOD, **change})
    assert sent == []


def test_prompt_at_the_limit_is_accepted():
    assert len(srv.create_agent_params({**GOOD, "prompt": "x" * 8000})["prompt"]) == 8000


def test_request_carries_method_and_params(monkeypatch):
    sent = []

    def fake(method, params):
        sent.append((method, params))
        return {"ok": True, "id": 12, "state": "paused", "paused": True,
                "repository": "forkmesh/forkmesh", "branch": "12-fix-login"}

    monkeypatch.setattr(srv, "agent_intake_request", fake)
    text, error = call_tool("create_agent", {**GOOD, "paused": True, "priority": 3})
    assert not error
    assert sent == [("agents.create", {**GOOD, "paused": True, "priority": 3})]
    result = json.loads(text)
    assert result["id"] == 12 and result["state"] == "paused"
    assert "ok" not in result


def test_desktop_refusal_becomes_a_tool_error(monkeypatch):
    monkeypatch.setattr(srv, "agent_intake_request", lambda method, params: {
        "ok": False, "error": "no_checkout",
        "message": "forkmesh/forkmesh is not a repository this desktop has checked out."})
    text, error = call_tool("create_agent", GOOD)
    assert error and "no_checkout" in text


def test_without_the_desktop_channel_it_refuses():
    text, error = call_tool("create_agent", GOOD)
    assert error and "FORKMESH_AGENT_INTAKE" in text


def test_a_published_connector_requires_its_token(monkeypatch):
    monkeypatch.setattr(srv, "connector_record", lambda: {"token": "fmcp_" + "a" * 43})
    monkeypatch.setattr(srv, "presented_token", lambda: "")
    called = []
    monkeypatch.setattr(srv, "agent_intake_request",
                        lambda method, params: called.append(1) or {"ok": True})
    text, error = call_tool("create_agent", GOOD)
    assert error and "connector token" in text
    assert called == []


def test_a_channel_that_is_not_a_socket_is_refused(monkeypatch, tmp_path):
    plain = tmp_path / "not-a-socket"
    plain.write_text("")
    monkeypatch.setenv("FORKMESH_AGENT_INTAKE", str(plain))
    monkeypatch.setenv("FORKMESH_AGENT_INTAKE_TOKEN", "3.secret")
    with pytest.raises(ValueError, match="not a socket"):
        srv.create_agent(GOOD)


def _serve_once(path, reply, seen):
    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(path)
    os.chmod(path, 0o600)
    server.listen(1)

    def run():
        conn, _ = server.accept()
        data = b""
        while b"\n" not in data:
            chunk = conn.recv(4096)
            if not chunk:
                break
            data += chunk
        seen.append(json.loads(data.decode()))
        conn.sendall(reply)
        conn.close()
        server.close()

    thread = threading.Thread(target=run, daemon=True)
    thread.start()
    return thread


def test_round_trip_over_the_unix_socket(monkeypatch):
    directory = tempfile.mkdtemp(prefix="fmi-", dir="/tmp")
    path = os.path.join(directory, "intake")
    seen = []
    thread = _serve_once(path, json.dumps({
        "ok": True, "id": 41, "state": "queued", "paused": False}).encode() + b"\n",
        seen)
    monkeypatch.setenv("FORKMESH_AGENT_INTAKE", path)
    monkeypatch.setenv("FORKMESH_AGENT_INTAKE_TOKEN", "9.secret-token")
    result = srv.create_agent({**GOOD, "branch": "41-keep-going"})
    thread.join(5)
    assert result == {"id": 41, "state": "queued", "paused": False}
    assert seen == [{"token": "9.secret-token", "method": "agents.create",
                     "params": {**GOOD, "branch": "41-keep-going", "paused": False}}]
    os.unlink(path)
    os.rmdir(directory)


def test_whoami_reports_the_channel(monkeypatch):
    text, _ = call_tool("whoami", {})
    assert json.loads(text)["canCreateAgents"] is False
    monkeypatch.setenv("FORKMESH_AGENT_INTAKE", "/tmp/x")
    monkeypatch.setenv("FORKMESH_AGENT_INTAKE_TOKEN", "1.y")
    text, _ = call_tool("whoami", {})
    assert json.loads(text)["canCreateAgents"] is True


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-q"]))
