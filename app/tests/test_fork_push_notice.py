"""After a push to a fork, git shows where to open the pull request.

``git_http.with_sideband_message`` adds one band-2 (progress) packet before
the final flush of a receive-pack result, which stock git prints as
``remote:`` lines. ``_fork_push_notice`` (entry.py) decides when: a new or
updated branch other than the default, on a repository whose catalog record
says it is a fork, or a contributor's ``pr/<account>/`` branch on the
repository itself. docs/CORE_PUSH.md.
"""

import ast
import asyncio
import json
import re
import subprocess
import sys
from pathlib import Path
from urllib.parse import quote, urlparse

import pytest

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import catalog  # noqa: E402
import git_http  # noqa: E402

ENTRY = SRC / "entry.py"
TEXT = ENTRY.read_text(encoding="utf-8")
TREE = ast.parse(TEXT, filename=str(ENTRY))
OLD = "a" * 40
NEW = "b" * 40
ZERO = "0" * 40


def _report(*lines):
    return b"".join(git_http.pkt_line(line) for line in lines) + b"0000"


def _sideband_result():
    inner = _report(b"unpack ok\n", b"ok refs/heads/fix\n")
    return git_http.pkt_line(b"\x01" + inner) + b"0000"


def test_the_message_rides_band_two_before_the_final_flush():
    body = _sideband_result()
    patched = git_http.with_sideband_message(body, "\nOpen it\n")
    assert patched.startswith(body[:-4])
    assert patched.endswith(git_http.pkt_line(b"\x02\nOpen it\n") + b"0000")


@pytest.mark.parametrize("body", [
    b"",
    _report(b"unpack ok\n", b"ok refs/heads/fix\n"),
    git_http.pkt_line(b"\x01unpack ok\n"),
    b"zzzz\x01" + b"0000",
])
def test_no_message_without_side_band_or_a_closing_flush(body):
    assert git_http.with_sideband_message(body, "hello") is None


class _Headers(dict):
    def get(self, name, default=None):
        for key, value in self.items():
            if key.lower() == name.lower():
                return value
        return default


class _Upstream:
    def __init__(self, updates, body=None):
        self.headers = _Headers({"x-forkmesh-ref-updates": json.dumps(
            {"total": len(updates), "updates": updates})})
        self.raw = body if body is not None else _sideband_result()
        self.reads = 0

    async def arrayBuffer(self):
        self.reads += 1
        return self.raw


class _Request:
    url = "https://git.example.test/alice/BLT.git/git-receive-pack"


def _notice(records):
    nodes = [
        node for node in TREE.body
        if isinstance(node, (ast.AsyncFunctionDef, ast.FunctionDef))
        and node.name in {"_fork_push_notice", "_ref_updates_from"}
    ]
    assert len(nodes) == 2
    module = ast.fix_missing_locations(ast.Module(body=nodes, type_ignores=[]))

    async def _repo_catalog_record(_env, owner, repo):
        return records.get((owner, repo))

    async def _repo_public_path(_env, owner, name):
        return "/%s/%s" % (owner, name)

    namespace = {
        "re": re,
        "json": json,
        "quote": quote,
        "urlparse": urlparse,
        "clean_string": catalog.clean_string,
        "js_nullish": lambda value: value is None,
        "_repo_catalog_record": _repo_catalog_record,
        "_repo_public_path": _repo_public_path,
        "with_sideband_message": git_http.with_sideband_message,
        "Uint8Array": type("U8", (), {"new": staticmethod(
            lambda value: type("V", (), {"to_py": lambda self: value})())}),
    }
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace["_fork_push_notice"]


FORK = {("alice", "BLT"): {"owner": "alice", "name": "BLT",
                           "forkedFrom": "owasp/blt", "branch": "main"}}


def _run(records, updates, body=None, alias=""):
    upstream = _Upstream(updates, body)
    result = asyncio.run(_notice(records)(
        object(), _Request(), "alice", "BLT", alias, upstream))
    return result, upstream


def _branch(name, old=OLD, new=NEW):
    return {"ref": "refs/heads/" + name, "old": old, "new": new,
            "commits": 1}


def test_a_new_branch_on_a_fork_gets_the_compare_link():
    result, upstream = _run(FORK, [_branch("demo/demo", old=ZERO)])
    assert upstream.reads == 1
    band = git_http.pkt_line(
        b"\x02\nOpen a pull request for 'demo/demo' by visiting:\n"
        b"     https://git.example.test/alice/BLT/compare/main...demo/demo"
        b"\n\n")
    assert result == _sideband_result()[:-4] + band + b"0000"


def test_a_contributor_branch_on_the_repository_itself_gets_the_link():
    plain = {("alice", "BLT"): {"owner": "alice", "name": "BLT",
                                "branch": "main"}}
    result, _upstream = _run(plain, [_branch("pr/bob/fix", old=ZERO)])
    assert b"compare/main...pr/bob/fix" in result
    assert b"Open a pull request for 'pr/bob/fix'" in result


@pytest.mark.parametrize("records,updates", [
    ({}, [_branch("fix")]),
    ({("alice", "BLT"): {"owner": "alice", "name": "BLT"}}, [_branch("fix")]),
    (FORK, [_branch("main")]),
    (FORK, [{"ref": "refs/tags/v1", "old": ZERO, "new": NEW, "commits": 0}]),
    (FORK, []),
])
def test_no_link_and_no_body_read_when_it_does_not_apply(records, updates):
    result, upstream = _run(records, updates)
    assert result is None
    assert upstream.reads == 0


def test_a_result_without_side_band_comes_back_unchanged():
    plain = _report(b"unpack ok\n", b"ok refs/heads/fix\n")
    result, upstream = _run(FORK, [_branch("fix")], body=plain)
    assert result == plain


def _backend_server(project_root, patch):
    import http.server
    import os
    import threading

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass

        def _cgi(self, body=b""):
            path, _, query = self.path.partition("?")
            env = {
                "PATH": os.environ.get("PATH", ""),
                "GIT_PROJECT_ROOT": str(project_root),
                "GIT_HTTP_EXPORT_ALL": "1",
                "REQUEST_METHOD": self.command,
                "PATH_INFO": path,
                "QUERY_STRING": query,
                "CONTENT_TYPE": self.headers.get("Content-Type", ""),
                "CONTENT_LENGTH": str(len(body)),
                "REMOTE_USER": "alice",
                "REMOTE_ADDR": "127.0.0.1",
                "GIT_CONFIG_NOSYSTEM": "1",
                "HOME": str(project_root),
            }
            out = subprocess.run(
                ["git", "http-backend"], input=body, env=env,
                capture_output=True, timeout=60).stdout
            head, _, payload = out.partition(b"\r\n\r\n")
            headers = [line.split(b":", 1) for line in head.split(b"\r\n")
                       if b":" in line]
            if path.endswith("/git-receive-pack"):
                payload = patch(payload) or payload
            status = 200
            self.send_response(status)
            for name, value in headers:
                if name.strip().lower() in (b"content-type",):
                    self.send_header(name.decode().strip(),
                                     value.decode().strip())
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

        def do_GET(self):
            self._cgi()

        def do_POST(self):
            length = int(self.headers.get("Content-Length") or 0)
            self._cgi(self.rfile.read(length))

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def test_stock_git_prints_the_link_as_remote_lines(tmp_path):
    import os
    import shutil
    if not shutil.which("git"):
        pytest.skip("needs git")
    env = dict(os.environ, HOME=str(tmp_path), GIT_CONFIG_NOSYSTEM="1",
               XDG_CONFIG_HOME=str(tmp_path / "xdg"),
               GIT_TERMINAL_PROMPT="0")
    for name in ("FORKMESH_HOME", "FORKMESH_PROJECTS_DIR"):
        env.pop(name, None)

    def git(*args, cwd=tmp_path):
        return subprocess.run(["git", *args], cwd=str(cwd), env=env,
                              capture_output=True, text=True, timeout=60)

    served = tmp_path / "served"
    served.mkdir()
    git("init", "-q", "--bare", "-b", "main", str(served / "BLT.git"))
    git("--git-dir", str(served / "BLT.git"), "config",
        "http.receivepack", "true")
    work = tmp_path / "work"
    work.mkdir()
    git("init", "-q", "-b", "main", cwd=work)
    (work / "README.md").write_text("hello\n")
    git("add", "README.md", cwd=work)
    git("-c", "user.name=E", "-c", "user.email=e@example.test", "commit",
        "-q", "-m", "first", cwd=work)
    message = ("\nOpen a pull request for 'fix' by visiting:\n"
               "     https://git.example.test/alice/BLT/compare/main...fix"
               "\n\n")
    server = _backend_server(
        served, lambda body: git_http.with_sideband_message(body, message))
    try:
        remote = "http://127.0.0.1:%d/BLT.git" % server.server_address[1]
        pushed = git("push", remote, "main:fix", cwd=work)
    finally:
        server.shutdown()
    assert pushed.returncode == 0, pushed.stderr
    assert ("remote: Open a pull request for 'fix' by visiting:"
            in pushed.stderr), pushed.stderr
    assert ("remote:      https://git.example.test/alice/BLT/compare/"
            "main...fix" in pushed.stderr), pushed.stderr
    assert "[new branch]" in pushed.stderr


def test_a_forced_update_names_both_tips_and_recommends_a_lease():
    plain = {("alice", "BLT"): {"owner": "alice", "name": "BLT",
                                "branch": "main"}}
    forced = dict(_branch("feature"), forced=True, pulls=[3],
                  kept="refs/forkmesh/history/feature/0000000001")
    result, upstream = _run(plain, [forced])
    assert upstream.reads == 1
    assert ("Forced update of 'feature' from %s to %s; the previous tip is "
            "kept." % (OLD[:7], NEW[:7])).encode() in result
    assert ("https://git.example.test/alice/BLT/compare/%s...%s"
            % (OLD, NEW)).encode() in result
    assert b"git push --force-with-lease" in result
    assert b"Open a pull request" not in result
