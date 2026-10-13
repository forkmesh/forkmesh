#!/usr/bin/env python3
import importlib.util
import json
import os
import sys
import tempfile
from pathlib import Path

import pytest

_SANDBOX = tempfile.mkdtemp(prefix="fm-mcp-release-")
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
    "fm_mcp_releases", HERE / "forkmesh_mcp_server.py")
srv = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(srv)

REPO = {"repository": "forkmesh/forkmesh"}


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


def test_tools_are_listed_with_the_right_access():
    listed = {t["name"]: t for t in srv.tools_list_payload()["tools"]}
    assert listed["list_releases"]["annotations"]["readOnlyHint"] is True
    for name in ("create_release", "publish_release"):
        assert name in srv.WRITE_TOOLS
        assert name not in srv.DESTRUCTIVE_TOOLS
        assert listed[name]["annotations"]["readOnlyHint"] is False
    assert srv.TOOLS_BY_NAME["create_release"]["inputSchema"]["required"] == ["repository"]
    assert set(srv.TOOLS_BY_NAME["create_release"]["inputSchema"]["properties"]) == {
        "repository", "tag", "target", "title", "notes", "rollout"}
    assert srv.TOOLS_BY_NAME["publish_release"]["inputSchema"]["required"] == [
        "repository", "tag"]


def test_instructions_name_the_tools():
    reply = srv.handle_request({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                                "params": {}})
    text = reply["result"]["instructions"]
    for name in ("list_releases", "create_release", "publish_release"):
        assert name in text


def test_create_params_are_normalized():
    assert srv.create_release_params(REPO) == REPO
    assert srv.create_release_params({
        **REPO, "tag": " v1.2.7 ", "target": "main", "title": "  ForkMesh   v1.2.7 ",
        "notes": "\n## What's Changed\n* x\n", "rollout": False}) == {
        **REPO, "tag": "v1.2.7", "target": "main", "title": "ForkMesh v1.2.7",
        "notes": "## What's Changed\n* x", "rollout": False}


@pytest.mark.parametrize("change", [
    {"repository": "forkmesh"},
    {"repository": None},
    {"tag": "../v1"},
    {"tag": "v1..2"},
    {"tag": "-v1"},
    {"tag": "v1.lock"},
    {"tag": "v 1"},
    {"tag": 7},
    {"target": "a/../main"},
    {"title": "t" * 201},
    {"notes": "n" * 20001},
    {"rollout": "yes"},
])
def test_bad_create_input_is_refused_before_any_request(change, monkeypatch):
    sent = []
    monkeypatch.setattr(srv, "agent_intake_request",
                        lambda method, params: sent.append(params) or {"ok": True})
    with pytest.raises(ValueError):
        srv.create_release({**REPO, **change})
    assert sent == []


def test_publish_needs_a_tag():
    with pytest.raises(ValueError, match="tag is required"):
        srv.publish_release_params(REPO)
    assert srv.publish_release_params({**REPO, "tag": "v1.2.7"}) == {**REPO, "tag": "v1.2.7"}


@pytest.mark.parametrize("count", [0, 51, 2.5, True, "5"])
def test_list_count_is_bounded(count):
    with pytest.raises(ValueError):
        srv.list_releases_params({**REPO, "count": count})


def test_requests_carry_method_and_params(monkeypatch):
    sent = []

    def fake(method, params):
        sent.append((method, params))
        return {"ok": True, "tag": "v1.2.7", "rollout": True}

    monkeypatch.setattr(srv, "agent_intake_request", fake)
    text, error = call_tool("create_release", {**REPO, "tag": "v1.2.7"})
    assert not error
    assert json.loads(text) == {"tag": "v1.2.7", "rollout": True}
    call_tool("publish_release", {**REPO, "tag": "v1.2.7"})
    call_tool("list_releases", {**REPO, "count": 3})
    assert sent == [("releases.create", {**REPO, "tag": "v1.2.7"}),
                    ("releases.publish", {**REPO, "tag": "v1.2.7"}),
                    ("releases.list", {**REPO, "count": 3})]


def test_desktop_refusal_becomes_a_tool_error(monkeypatch):
    monkeypatch.setattr(srv, "agent_intake_request", lambda method, params: {
        "ok": False, "error": "tag_exists", "message": "Release v1.2.7 already exists."})
    text, error = call_tool("create_release", {**REPO, "tag": "v1.2.7"})
    assert error and "tag_exists" in text


def test_without_the_desktop_channel_it_refuses():
    text, error = call_tool("create_release", REPO)
    assert error and "FORKMESH_AGENT_INTAKE" in text


def test_a_published_connector_requires_its_token(monkeypatch):
    monkeypatch.setattr(srv, "connector_record", lambda: {"token": "fmcp_" + "a" * 43})
    monkeypatch.setattr(srv, "presented_token", lambda: "")
    called = []
    monkeypatch.setattr(srv, "agent_intake_request",
                        lambda method, params: called.append(1) or {"ok": True})
    text, error = call_tool("create_release", REPO)
    assert error and "connector token" in text
    assert called == []


def test_whoami_reports_release_access(monkeypatch):
    text, _ = call_tool("whoami", {})
    assert json.loads(text)["canManageReleases"] is False
    monkeypatch.setenv("FORKMESH_AGENT_INTAKE", "/tmp/x")
    monkeypatch.setenv("FORKMESH_AGENT_INTAKE_TOKEN", "1.y")
    text, _ = call_tool("whoami", {})
    assert json.loads(text)["canManageReleases"] is True


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-q"]))
