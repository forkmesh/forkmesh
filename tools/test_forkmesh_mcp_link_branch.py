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

_SANDBOX = tempfile.mkdtemp(prefix="fm-mcp-link-")
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
    "fm_mcp_link_branch", HERE / "forkmesh_mcp_server.py")
srv = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(srv)

WORKTREE = "/home/u/src/.worktrees/20-agent-link-branch"


@pytest.fixture(autouse=True)
def _clean_channel(monkeypatch):
    monkeypatch.delenv("FORKMESH_AGENT_INTAKE", raising=False)
    monkeypatch.delenv("FORKMESH_AGENT_INTAKE_TOKEN", raising=False)
    monkeypatch.setattr(srv, "connector_record", lambda: {})


def _with_channel(monkeypatch):
    monkeypatch.setenv("FORKMESH_AGENT_INTAKE", "/tmp/fm-intake")
    monkeypatch.setenv("FORKMESH_AGENT_INTAKE_TOKEN", "4.secret")


def call_tool(name, arguments):
    reply = srv.handle_request({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                                "params": {"name": name, "arguments": arguments}})
    result = reply["result"]
    return result["content"][0]["text"], bool(result.get("isError"))


def listed_tools():
    return {t["name"]: t for t in srv.tools_list_payload()["tools"]}


def test_tool_is_a_write_with_branch_and_worktree():
    schema = srv.TOOLS_BY_NAME["link_branch"]["inputSchema"]
    assert schema["required"] == ["branch"]
    assert set(schema["properties"]) == {"branch", "worktree"}
    assert "link_branch" in srv.WRITE_TOOLS
    assert "link_branch" not in srv.DESTRUCTIVE_TOOLS
    description = srv.TOOLS_BY_NAME["link_branch"]["description"]
    assert "own agent session" in description
    assert "diff viewer" in description and "merge" in description


def test_listed_only_where_the_desktop_channel_is(monkeypatch):
    assert "link_branch" not in listed_tools()
    assert "create_agent" in listed_tools()
    _with_channel(monkeypatch)
    listed = listed_tools()
    assert "link_branch" in listed
    assert listed["link_branch"]["annotations"]["readOnlyHint"] is False
    assert listed["link_branch"]["annotations"]["destructiveHint"] is False
    assert listed["link_branch"]["annotations"]["openWorldHint"] is False


def test_a_half_set_channel_does_not_list_it(monkeypatch):
    monkeypatch.setenv("FORKMESH_AGENT_INTAKE", "/tmp/fm-intake")
    assert "link_branch" not in listed_tools()


def test_instructions_name_the_tool():
    reply = srv.handle_request({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                                "params": {}})
    assert "link_branch" in reply["result"]["instructions"]


def test_params_are_normalized():
    assert srv.link_branch_params({"branch": " 20-agent-link-branch "}) == {
        "branch": "20-agent-link-branch"}
    assert srv.link_branch_params({"branch": "agent/20", "worktree": WORKTREE}) == {
        "branch": "agent/20", "worktree": WORKTREE}
    assert srv.link_branch_params({"branch": "x", "worktree": None}) == {
        "branch": "x"}
    assert srv.link_branch_params({"branch": "x", "worktree": "  "}) == {
        "branch": "x"}


@pytest.mark.parametrize("args", [
    {},
    {"branch": ""},
    {"branch": "   "},
    {"branch": None},
    {"branch": 5},
    {"branch": "agent/../main"},
    {"branch": "-x"},
    {"branch": "a.lock"},
    {"branch": "a//b"},
    {"branch": "x", "worktree": "relative/path"},
    {"branch": "x", "worktree": "/a/b/../c"},
    {"branch": "x", "worktree": "/a/\x00b"},
    {"branch": "x", "worktree": 7},
    {"branch": "x", "worktree": "/" + "a" * 4096},
])
def test_bad_input_is_refused_before_any_request(args, monkeypatch):
    sent = []
    monkeypatch.setattr(srv, "agent_intake_request",
                        lambda method, params: sent.append(params) or {"ok": True})
    with pytest.raises(ValueError):
        srv.link_branch(args)
    assert sent == []


def test_non_object_arguments_are_refused():
    with pytest.raises(ValueError):
        srv.link_branch_params(["branch"])


def test_request_carries_method_and_params(monkeypatch):
    sent = []

    def fake(method, params):
        sent.append((method, params))
        return {"ok": True, "branch": "20-agent-link-branch", "worktree": WORKTREE}

    monkeypatch.setattr(srv, "agent_intake_request", fake)
    text, error = call_tool("link_branch", {"branch": "20-agent-link-branch",
                                            "worktree": WORKTREE})
    assert not error
    assert sent == [("agents.link_branch",
                     {"branch": "20-agent-link-branch", "worktree": WORKTREE})]
    assert json.loads(text) == {"branch": "20-agent-link-branch",
                                "worktree": WORKTREE}


@pytest.mark.parametrize("code", ["bad_branch", "branch_not_found",
                                  "worktree_mismatch", "not_running",
                                  "no_session"])
def test_desktop_refusal_becomes_a_tool_error(code, monkeypatch):
    monkeypatch.setattr(srv, "agent_intake_request", lambda method, params: {
        "ok": False, "error": code, "message": "refused by the desktop"})
    text, error = call_tool("link_branch", {"branch": "20-agent-link-branch"})
    assert error and code in text and "refused by the desktop" in text


def test_without_the_desktop_channel_it_refuses():
    text, error = call_tool("link_branch", {"branch": "20-agent-link-branch"})
    assert error and "FORKMESH_AGENT_INTAKE" in text


def test_a_published_connector_requires_its_token(monkeypatch):
    monkeypatch.setattr(srv, "connector_record", lambda: {"token": "fmcp_" + "a" * 43})
    monkeypatch.setattr(srv, "presented_token", lambda: "")
    called = []
    monkeypatch.setattr(srv, "agent_intake_request",
                        lambda method, params: called.append(1) or {"ok": True})
    text, error = call_tool("link_branch", {"branch": "20-agent-link-branch"})
    assert error and "connector token" in text
    assert called == []


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
    directory = tempfile.mkdtemp(prefix="fml-", dir="/tmp")
    path = os.path.join(directory, "intake")
    seen = []
    thread = _serve_once(path, json.dumps({
        "ok": True, "branch": "20-agent-link-branch", "worktree": ""}).encode() + b"\n",
        seen)
    monkeypatch.setenv("FORKMESH_AGENT_INTAKE", path)
    monkeypatch.setenv("FORKMESH_AGENT_INTAKE_TOKEN", "6.secret-token")
    result = srv.link_branch({"branch": "20-agent-link-branch"})
    thread.join(5)
    assert result == {"branch": "20-agent-link-branch", "worktree": ""}
    assert seen == [{"token": "6.secret-token", "method": "agents.link_branch",
                     "params": {"branch": "20-agent-link-branch"}}]
    os.unlink(path)
    os.rmdir(directory)


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-q"]))
