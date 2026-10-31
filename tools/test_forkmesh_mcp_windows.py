#!/usr/bin/env python3
import base64
import importlib.util
import json
import os
import socket
import tempfile
import threading
from pathlib import Path

import pytest

_SANDBOX = tempfile.mkdtemp(prefix="fm-mcp-windows-")
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
    "fm_mcp_windows", HERE / "forkmesh_mcp_server.py")
srv = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(srv)

WINDOW = "w0123456789abcdef"
BASIC = {"title": "Release plan", "body": "# Steps\n- tag\n- publish"}


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


def test_tools_are_listed_and_connector_gated():
    listed = {t["name"]: t for t in srv.tools_list_payload()["tools"]}
    for name in ("show_window", "read_window", "close_window"):
        assert name in listed
        assert name in srv.WINDOW_TOOLS
    assert listed["show_window"]["inputSchema"]["required"] == ["title", "body"]
    assert listed["show_window"]["annotations"]["readOnlyHint"] is False
    assert listed["read_window"]["annotations"]["readOnlyHint"] is True


def test_instructions_name_the_window_tools():
    reply = srv.handle_request({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                                "params": {}})
    assert "show_window" in reply["result"]["instructions"]


def test_params_are_normalized():
    params = srv.show_window_params({
        **BASIC, "title": "  Release   plan ", "format": "HTML",
        "buttons": [" Approve ", "Reject"], "input_label": "Why?",
        "width": 600, "window_id": WINDOW})
    assert params == {
        "window_id": WINDOW, "title": "Release plan", "body": BASIC["body"],
        "format": "html", "buttons": ["Approve", "Reject"],
        "input_label": "Why?", "input_multiline": False, "width": 600}
    assert srv.show_window_params(BASIC) == {**BASIC, "format": "markdown"}


@pytest.mark.parametrize("change", [
    {"title": ""},
    {"title": "x" * 121},
    {"body": "  "},
    {"body": "x" * 40001},
    {"format": "javascript"},
    {"buttons": "Approve"},
    {"buttons": ["a"] * 7},
    {"buttons": ["Yes", "Yes"]},
    {"buttons": [""]},
    {"buttons": [3]},
    {"input_label": "x" * 201},
    {"input_label": "Why", "input_multiline": "yes"},
    {"width": 100},
    {"height": 2000},
    {"width": 300.5},
    {"window_id": "../etc"},
    {"body": "é" * 40000},
])
def test_bad_input_is_refused_before_any_request(change, monkeypatch):
    sent = []
    monkeypatch.setattr(srv, "agent_intake_request",
                        lambda method, params: sent.append(params) or {"ok": True})
    with pytest.raises(ValueError):
        srv.show_window({**BASIC, **change})
    assert sent == []


def test_wait_seconds_is_bounded():
    for bad in (-1, 601, "5", True):
        with pytest.raises(ValueError):
            srv.show_window({**BASIC, "wait_seconds": bad})


def test_show_returns_at_once_without_controls(monkeypatch):
    sent = []

    def fake(method, params):
        sent.append((method, params))
        return {"ok": True, "window_id": WINDOW, "open": True, "answered": False,
                "has_input": False, "buttons": []}

    monkeypatch.setattr(srv, "agent_intake_request", fake)
    text, error = call_tool("show_window", {**BASIC, "wait_seconds": 30})
    assert not error
    assert [m for m, _ in sent] == ["windows.show"]
    assert json.loads(text)["window_id"] == WINDOW


def test_show_waits_for_an_answer(monkeypatch):
    replies = [
        {"ok": True, "window_id": WINDOW, "open": True, "answered": False,
         "has_input": False, "buttons": ["Approve"]},
        {"ok": True, "window_id": WINDOW, "open": True, "answered": False},
        {"ok": True, "window_id": WINDOW, "open": True, "answered": True,
         "button": "Approve"},
    ]
    sent = []

    def fake(method, params):
        sent.append((method, params))
        return dict(replies.pop(0))

    monkeypatch.setattr(srv, "agent_intake_request", fake)
    monkeypatch.setattr(srv.time, "sleep", lambda _s: None)
    result = srv.show_window({**BASIC, "buttons": ["Approve"], "wait_seconds": 60})
    assert result["answered"] is True and result["button"] == "Approve"
    assert [m for m, _ in sent] == ["windows.show", "windows.read", "windows.read"]
    assert sent[1][1] == {"window_id": WINDOW}


def test_wait_stops_when_the_window_closes():
    reads = iter([{"ok": True, "window_id": WINDOW, "open": False, "answered": False}])
    original = srv.window_request
    srv.window_request = lambda method, params: dict(next(reads))
    try:
        state = srv.wait_for_window(WINDOW, 60, sleep=lambda _s: None)
    finally:
        srv.window_request = original
    assert state["open"] is False


def test_wait_gives_up_at_the_deadline():
    now = [0.0]

    def clock():
        return now[0]

    def sleep(seconds):
        now[0] += seconds

    original = srv.window_request
    srv.window_request = lambda method, params: {
        "window_id": WINDOW, "open": True, "answered": False}
    try:
        state = srv.wait_for_window(WINDOW, 2, sleep=sleep, clock=clock)
    finally:
        srv.window_request = original
    assert state["answered"] is False and now[0] >= 2


def test_read_and_close_need_a_window_id(monkeypatch):
    monkeypatch.setattr(srv, "agent_intake_request", lambda method, params: {"ok": True})
    for tool in ("read_window", "close_window"):
        text, error = call_tool(tool, {})
        assert error and "window_id" in text


def test_desktop_refusal_becomes_a_tool_error(monkeypatch):
    monkeypatch.setattr(srv, "agent_intake_request", lambda method, params: {
        "ok": False, "error": "too_many_windows", "message": "close one first"})
    text, error = call_tool("show_window", BASIC)
    assert error and "too_many_windows" in text


def test_without_the_desktop_channel_it_refuses():
    text, error = call_tool("show_window", BASIC)
    assert error and "FORKMESH_AGENT_INTAKE" in text


def test_whoami_reports_windows(monkeypatch):
    text, _ = call_tool("whoami", {})
    assert json.loads(text)["canOpenWindows"] is False
    monkeypatch.setenv("FORKMESH_AGENT_INTAKE", "/tmp/x")
    monkeypatch.setenv("FORKMESH_AGENT_INTAKE_TOKEN", "1.y")
    text, _ = call_tool("whoami", {})
    assert json.loads(text)["canOpenWindows"] is True


def test_browser_tool_is_listed():
    listed = {t["name"]: t for t in srv.tools_list_payload()["tools"]}
    tool = listed["open_browser_window"]
    assert tool["inputSchema"]["required"] == ["url"]
    assert tool["annotations"]["readOnlyHint"] is False
    assert "open_browser_window" in srv.WINDOW_TOOLS


def test_browser_params_are_normalized():
    assert srv.open_browser_window_params({
        "url": " http://localhost:3000/app ", "title": "  Dev   server ",
        "width": 800, "window_id": WINDOW}) == {
        "url": "http://localhost:3000/app", "window_id": WINDOW,
        "title": "Dev server", "width": 800}


@pytest.mark.parametrize("url", [
    "", "file:///etc/passwd", "javascript:alert(1)", "ftp://x.org/a",
    "https://", "https://user:pw@example.com/", "https://exa mple.com",
    "https://example.com/\x07", "https://" + "a" * 2050 + ".com", 42,
])
def test_bad_urls_are_refused_before_any_request(url, monkeypatch):
    sent = []
    monkeypatch.setattr(srv, "agent_intake_request",
                        lambda method, params: sent.append(params) or {"ok": True})
    with pytest.raises(ValueError):
        srv.open_browser_window({"url": url})
    assert sent == []


def test_browser_request_carries_the_url(monkeypatch):
    sent = []

    def fake(method, params):
        sent.append((method, params))
        return {"ok": True, "window_id": WINDOW, "kind": "browser",
                "embedded": True, "open": True, "url": params["url"]}

    monkeypatch.setattr(srv, "agent_intake_request", fake)
    text, error = call_tool("open_browser_window", {"url": "https://forkmesh.com/"})
    assert not error
    assert sent == [("windows.browse", {"url": "https://forkmesh.com/"})]
    assert json.loads(text)["embedded"] is True


PNG = base64.b64encode(b"\x89PNG\r\n\x1a\n" + b"\x00" * 16).decode()


def test_read_page_is_read_only():
    listed = {t["name"]: t for t in srv.tools_list_payload()["tools"]}
    tool = listed["read_browser_page"]
    assert tool["inputSchema"]["required"] == ["window_id"]
    assert tool["annotations"]["readOnlyHint"] is True
    assert "read_browser_page" in srv.WINDOW_TOOLS


def test_read_page_params():
    assert srv.read_browser_page_params({"window_id": WINDOW}) == {
        "window_id": WINDOW, "max_chars": 20000, "offset": 0}
    assert srv.read_browser_page_params({
        "window_id": WINDOW, "max_chars": 500, "offset": 1000,
        "include_links": True, "screenshot": True}) == {
        "window_id": WINDOW, "max_chars": 500, "offset": 1000,
        "include_links": True, "screenshot": True}


@pytest.mark.parametrize("change", [
    {"window_id": ""}, {"max_chars": 0}, {"max_chars": 100001},
    {"offset": -1}, {"screenshot": "yes"}, {"include_links": 1},
])
def test_bad_page_reads_are_refused(change, monkeypatch):
    sent = []
    monkeypatch.setattr(srv, "agent_intake_request",
                        lambda *a, **k: sent.append(a) or {"ok": True})
    with pytest.raises(ValueError):
        srv.read_browser_page({"window_id": WINDOW, **change})
    assert sent == []


def test_read_page_returns_text_and_screenshot(monkeypatch):
    seen = []

    def fake(method, params, max_reply=None):
        seen.append((method, params, max_reply))
        return {"ok": True, "window_id": WINDOW, "kind": "page", "open": True,
                "title": "Login", "url": "http://localhost:3000/",
                "text": "Sign in", "total_chars": 7, "truncated": False,
                "console": [{"level": "error", "message": "boom"}],
                "screenshot_png": PNG}

    monkeypatch.setattr(srv, "agent_intake_request", fake)
    reply = srv.handle_request({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                                "params": {"name": "read_browser_page",
                                           "arguments": {"window_id": WINDOW,
                                                         "screenshot": True}}})
    content = reply["result"]["content"]
    assert seen[0][0] == "windows.capture" and seen[0][2] == srv.PAGE_MAX_REPLY
    meta = json.loads(content[0]["text"])
    assert "\n" not in content[0]["text"]
    assert meta["title"] == "Login" and meta["has_screenshot"] is True
    assert meta["console"][0]["message"] == "boom"
    assert "text" not in meta and "screenshot_png" not in meta
    assert content[1] == {"type": "text", "text": "Sign in"}
    assert content[2] == {"type": "image", "data": PNG, "mimeType": "image/png"}


def test_a_bad_screenshot_is_dropped(monkeypatch):
    monkeypatch.setattr(srv, "agent_intake_request", lambda method, params, max_reply=None: {
        "ok": True, "window_id": WINDOW, "text": "", "screenshot_png": "bm90IGEgcG5n"})
    blocks = srv.read_browser_page({"window_id": WINDOW, "screenshot": True})
    assert len(blocks) == 2
    assert json.loads(blocks[0]["text"])["has_screenshot"] is False
    assert blocks[1]["text"] == "(the page shows no text here)"


def test_not_shared_is_a_tool_error(monkeypatch):
    monkeypatch.setattr(srv, "agent_intake_request", lambda method, params, max_reply=None: {
        "ok": False, "error": "not_shared", "message": "turned off"})
    text, error = call_tool("read_browser_page", {"window_id": WINDOW})
    assert error and "not_shared" in text


ALL_WINDOW_CALLS = [
    ("show_window", BASIC),
    ("read_window", {"window_id": WINDOW}),
    ("close_window", {"window_id": WINDOW}),
    ("open_browser_window", {"url": "https://forkmesh.com/"}),
    ("read_browser_page", {"window_id": WINDOW}),
    ("browser_action", {"window_id": WINDOW, "action": "reload"}),
    ("show_widget", {"title": "Counter", "html": "<button>+</button>"}),
    ("suggest_session", {"prompt": "Fix the flaky upload test"}),
]


def test_every_window_tool_is_gated():
    assert srv.WINDOW_TOOLS == {name for name, _ in ALL_WINDOW_CALLS}


@pytest.mark.parametrize("name,arguments", ALL_WINDOW_CALLS)
def test_window_tools_need_the_connector_token(name, arguments, monkeypatch):
    sent = []
    monkeypatch.setattr(srv, "connector_record", lambda: {"token": "connector-secret"})
    monkeypatch.setattr(srv, "agent_intake_request",
                        lambda method, params, **_kw: sent.append(method) or {
                            "ok": True, "window_id": WINDOW, "url": "https://forkmesh.com/",
                            "text": "", "open": True})
    monkeypatch.delenv("FORKMESH_MCP_TOKEN", raising=False)
    text, error = call_tool(name, arguments)
    assert error and "connector token" in text and "FORKMESH_MCP_TOKEN" in text
    monkeypatch.setenv("FORKMESH_MCP_TOKEN", "wrong")
    text, error = call_tool(name, arguments)
    assert error and "connector token" in text
    assert sent == []
    monkeypatch.setenv("FORKMESH_MCP_TOKEN", "connector-secret")
    text, error = call_tool(name, arguments)
    assert "connector token" not in text
    assert len(sent) == 1


def test_whoami_withholds_windows_without_the_connector_token(monkeypatch):
    monkeypatch.setenv("FORKMESH_AGENT_INTAKE", "/tmp/x")
    monkeypatch.setenv("FORKMESH_AGENT_INTAKE_TOKEN", "1.y")
    monkeypatch.setattr(srv, "connector_record", lambda: {"token": "connector-secret"})
    monkeypatch.delenv("FORKMESH_MCP_TOKEN", raising=False)
    info = json.loads(call_tool("whoami", {})[0])
    assert info["canOpenWindows"] is False
    assert set(info["windowTools"]) == srv.WINDOW_TOOLS
    monkeypatch.setenv("FORKMESH_MCP_TOKEN", "connector-secret")
    assert json.loads(call_tool("whoami", {})[0])["canOpenWindows"] is True


def fake_desktop():
    directory = tempfile.mkdtemp(prefix="fmintake-", dir="/tmp")
    path = os.path.join(directory, "intake.sock")
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    listener.bind(path)
    listener.listen(4)
    listener.settimeout(0.1)
    seen = []
    stop = threading.Event()

    def serve():
        while not stop.is_set():
            try:
                conn, _ = listener.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            answer(conn)

    def answer(conn):
        conn.settimeout(5)
        with conn:
            data = b""
            while not data.endswith(b"\n"):
                chunk = conn.recv(4096)
                if not chunk:
                    break
                data += chunk
            seen.append(json.loads(data))
            conn.sendall(json.dumps({
                "ok": True, "window_id": WINDOW, "open": True, "answered": False,
                "has_input": False, "buttons": []}).encode() + b"\n")

    thread = threading.Thread(target=serve, daemon=True)
    thread.start()

    def close():
        stop.set()
        listener.close()
        thread.join(5)
        os.unlink(path)
        os.rmdir(directory)

    return path, seen, close


def test_window_requests_carry_the_intake_token(monkeypatch):
    path, seen, close = fake_desktop()
    try:
        monkeypatch.setenv("FORKMESH_AGENT_INTAKE", path)
        monkeypatch.setenv("FORKMESH_AGENT_INTAKE_TOKEN", "7.secret")
        text, error = call_tool("show_window", BASIC)
    finally:
        close()
    assert not error and json.loads(text)["window_id"] == WINDOW
    assert seen == [{"token": "7.secret", "method": "windows.show",
                     "params": {**BASIC, "format": "markdown"}}]


def test_window_tools_refuse_without_the_intake_token(monkeypatch):
    path, seen, close = fake_desktop()
    try:
        monkeypatch.setenv("FORKMESH_AGENT_INTAKE", path)
        for name, arguments in ALL_WINDOW_CALLS:
            text, error = call_tool(name, arguments)
            assert error and "FORKMESH_AGENT_INTAKE_TOKEN" in text
    finally:
        close()
    assert seen == []
