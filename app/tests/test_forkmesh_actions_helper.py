import json
import os
import subprocess
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "forkmesh-actions.sh"
TOKEN = "job-token-" + "c" * 32


class Stub:
    def __init__(self, status=200):
        self.status = status
        self.calls = []
        stub = self

        class Handler(BaseHTTPRequestHandler):
            def answer(self):
                length = int(self.headers.get("content-length") or 0)
                raw = self.rfile.read(length) if length else b""
                stub.calls.append({
                    "method": self.command, "path": self.path,
                    "auth": self.headers.get("authorization"),
                    "type": self.headers.get("content-type"),
                    "body": json.loads(raw) if raw else None,
                })
                data = json.dumps({"ok": stub.status < 300, "number": 7,
                                   "path": self.path}).encode()
                self.send_response(stub.status)
                self.send_header("content-type", "application/json")
                self.send_header("content-length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            do_GET = answer
            do_POST = answer

            def log_message(self, *_args):
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.url = "http://127.0.0.1:%d" % self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def close(self):
        self.server.shutdown()
        self.server.server_close()


@pytest.fixture
def stub():
    server = Stub()
    yield server
    server.close()


def sandbox_env(tmp_path, url, **extra):
    env = {key: value for key, value in os.environ.items()
           if key not in ("FORKMESH_HOME", "FORKMESH_PROJECTS_DIR")}
    for name in ("HOME", "XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME"):
        path = tmp_path / name.lower()
        path.mkdir(exist_ok=True)
        env[name] = str(path)
    env.pop("FORKMESH_EVENT_PATH", None)
    env.update({"FORKMESH_API_URL": url + "/", "FORKMESH_TOKEN": TOKEN})
    env.update(extra)
    return env


def run(tmp_path, url, *args, stdin="", **extra):
    return subprocess.run(["bash", str(SCRIPT), *args], input=stdin,
                          capture_output=True, text=True, timeout=60,
                          env=sandbox_env(tmp_path, url, **extra))


@pytest.mark.parametrize("json_tool", ["jq", "python"])
def test_every_subcommand_calls_the_job_api(tmp_path, stub, json_tool):
    extra = {"FORKMESH_ACTIONS_JSON": json_tool}
    cases = [
        (["event"], "", ("GET", "/api/actions/job/event", None)),
        (["issue", "12"], "", ("GET", "/api/actions/job/issues/12", None)),
        (["comment", "12", 'Thanks "a" & $(b)'], "",
         ("POST", "/api/actions/job/issues/12/comments", {"body": 'Thanks "a" & $(b)'})),
        (["comment", "12", "-"], "from stdin\nline 2",
         ("POST", "/api/actions/job/issues/12/comments", {"body": "from stdin\nline 2"})),
        (["label", "12", "--add", "a, b", "--remove", "c"], "",
         ("POST", "/api/actions/job/issues/12/labels", {"add": ["a", "b"], "remove": ["c"]})),
        (["label", "12", "--remove=c"], "",
         ("POST", "/api/actions/job/issues/12/labels", {"add": [], "remove": ["c"]})),
        (["close", "12"], "", ("POST", "/api/actions/job/issues/12/state", {"state": "closed"})),
        (["reopen", "12"], "", ("POST", "/api/actions/job/issues/12/state", {"state": "open"})),
        (["open-issue", "Broken build", "-", "bug,ci"], "details",
         ("POST", "/api/actions/job/issues",
          {"title": "Broken build", "body": "details", "labels": ["bug", "ci"]})),
        (["open-issue", "Plain", "text"], "",
         ("POST", "/api/actions/job/issues", {"title": "Plain", "body": "text", "labels": []})),
    ]
    for args, stdin, (method, path, body) in cases:
        result = run(tmp_path, stub.url, *args, stdin=stdin, **extra)
        assert result.returncode == 0, (args, result.stderr)
        assert json.loads(result.stdout)["path"] == path
        call = stub.calls[-1]
        assert (call["method"], call["path"], call["body"]) == (method, path, body), args
        assert call["auth"] == "Bearer " + TOKEN
        if body is not None:
            assert call["type"] == "application/json"


def test_event_reads_the_payload_file_first(tmp_path, stub):
    payload = tmp_path / "event.json"
    payload.write_text('{"action": "opened"}')
    result = run(tmp_path, stub.url, "event", FORKMESH_EVENT_PATH=str(payload))
    assert result.returncode == 0
    assert json.loads(result.stdout) == {"action": "opened"}
    assert stub.calls == []


def test_errors_and_bad_arguments_fail_without_leaking_the_token(tmp_path):
    server = Stub(status=403)
    try:
        refused = run(tmp_path, server.url, "close", "3")
    finally:
        server.close()
    assert refused.returncode == 1
    assert "HTTP 403" in refused.stderr
    assert TOKEN not in refused.stdout + refused.stderr
    for args in (["issue", "x"], ["comment", "3"], ["label", "3"], ["open-issue"], ["bogus"]):
        assert run(tmp_path, "http://127.0.0.1:9", *args).returncode == 2, args
    missing = subprocess.run(["bash", str(SCRIPT), "issue", "3"], capture_output=True,
                             text=True, timeout=60,
                             env=sandbox_env(tmp_path, "http://x", FORKMESH_TOKEN=""))
    assert missing.returncode == 1 and "FORKMESH_TOKEN" in missing.stderr


def test_the_token_never_reaches_a_command_line():
    text = SCRIPT.read_text()
    assert "-H @<(printf 'Authorization: Bearer %s\\n' \"$FORKMESH_TOKEN\")" in text
    assert "Bearer $FORKMESH_TOKEN\"" not in text
    assert "#" not in "".join(
        line for line in text.splitlines()[1:] if line.lstrip().startswith("#"))
