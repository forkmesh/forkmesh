#!/usr/bin/env python3
from __future__ import annotations

import base64
import hashlib
import http.server
import importlib.util
import json
import os
import re
import subprocess
import sys
import threading
from pathlib import Path

import pytest
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import (
    Ed25519PrivateKey, Ed25519PublicKey)

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
_spec = importlib.util.spec_from_file_location("source_publisher", HERE / "source_publisher.py")
sp = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(sp)
import forkmesh_paths  # noqa: E402

VECTORS = HERE.parent / "conformance" / "protocol" / "v1" / "vectors" / "signatures.json"
GIT_ENV = {
    "GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@example.invalid",
    "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@example.invalid",
    "GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_NOSYSTEM": "1",
}


def b64url_decode(text):
    return base64.urlsafe_b64decode(text + "=" * (-len(text) % 4))


def sh(*args, cwd=None):
    env = dict(os.environ, **GIT_ENV)
    completed = subprocess.run(["git", *args], cwd=cwd, env=env, capture_output=True,
                               text=True, check=True)
    return completed.stdout.strip()


def commit(repo: Path, name: str) -> str:
    (repo / name).write_text(name)
    sh("add", name, cwd=repo)
    sh("commit", "-q", "-m", name, cwd=repo)
    return sh("rev-parse", "HEAD", cwd=repo)


@pytest.fixture
def box(tmp_path, monkeypatch):
    for name in ("FORKMESH_HOME", "FORKMESH_PROJECTS_DIR", "FORKMESH_PROJECT",
                 "FORKMESH_DATA_DIR", "FORKMESH_CONFIG_DIR", "FORKMESH_FANOUT_SCRIPT"):
        monkeypatch.delenv(name, raising=False)
    for name, value in GIT_ENV.items():
        monkeypatch.setenv(name, value)
    home = tmp_path / "home"
    projects = home / "projects"
    paths = forkmesh_paths.resolve(environ={
        "FORKMESH_PROJECTS_DIR": str(projects),
        "FORKMESH_HOME": str(projects / "forkmesh")}, home=home)
    paths.source.mkdir(parents=True)
    sh("init", "-q", "-b", "main", cwd=paths.source)
    commit(paths.source, "one")
    paths.bare_mirror.parent.mkdir(parents=True)
    sh("clone", "-q", "--bare", str(paths.source), str(paths.bare_mirror))
    return paths


def systemctl_fake(states=None, calls=None):
    states = states if states is not None else {}

    def run(command, **kwargs):
        if command[0] == "systemctl":
            if calls is not None:
                calls.append(command)
            if command[2] == "show":
                active = states.get(command[-1], "active")
                return subprocess.CompletedProcess(
                    command, 0, "LoadState=loaded\nActiveState=%s\n" % active, "")
            return subprocess.CompletedProcess(command, 0, "", "")
        return subprocess.run(command, **kwargs)
    return run


def test_sync_fast_forwards_and_never_rewinds(box):
    head = commit(box.source, "two")
    result = sp.sync(box.source, box.bare_mirror)
    assert result["ok"] and "refs/heads/main" in result["updated"]
    assert sp.rev(box.bare_mirror, "refs/heads/main") == head
    sh("checkout", "-q", "--orphan", "rewrite", cwd=box.source)
    commit(box.source, "other")
    sh("branch", "-f", "main", "rewrite", cwd=box.source)
    result = sp.sync(box.source, box.bare_mirror)
    assert not result["ok"]
    assert any("main" in item for item in result["rejected"])
    assert sp.rev(box.bare_mirror, "refs/heads/main") == head


def test_publish_runs_fanout_then_skips_until_the_source_moves(box, monkeypatch):
    marker = box.home_dir / "fanout-runs"
    script = box.home_dir / "bin" / "forkmesh-mirror-fanout.sh"
    script.parent.mkdir(parents=True)
    script.write_text("#!/bin/sh\necho run >> '%s'\n" % marker)
    script.chmod(0o755)
    run = systemctl_fake()
    logs = []
    assert sp.publish(box, run=run, sleep=lambda s: None, log=logs.append) == 0
    assert marker.read_text().count("run") == 1
    assert sp.publish(box, if_changed=True, run=run, sleep=lambda s: None,
                      log=logs.append) == 0
    assert marker.read_text().count("run") == 1
    head = commit(box.source, "two")
    assert sp.publish(box, if_changed=True, run=run, sleep=lambda s: None,
                      log=logs.append) == 0
    assert marker.read_text().count("run") == 2
    status = json.loads((box.publisher_state / "status.json").read_text())
    assert status["ok"] and status["sourceMain"] == head


def test_publish_repeats_when_the_source_moves_during_a_pass(box):
    marker = box.home_dir / "fanout-runs"
    script = box.home_dir / "bin" / "forkmesh-mirror-fanout.sh"
    script.parent.mkdir(parents=True)
    script.write_text("#!/bin/sh\necho run >> '%s'\n" % marker)
    script.chmod(0o755)
    base = systemctl_fake()
    moved = []

    def run(command, **kwargs):
        result = base(command, **kwargs)
        if command[0] == str(script) and not moved:
            moved.append(commit(box.source, "during"))
        return result

    sp.publish(box, run=run, sleep=lambda s: None, log=lambda *_: None)
    assert marker.read_text().count("run") == 2
    assert sp.rev(box.bare_mirror, "refs/heads/main") == moved[0]


def test_publish_failure_does_not_spin(box):
    script = box.home_dir / "bin" / "forkmesh-mirror-fanout.sh"
    script.parent.mkdir(parents=True)
    script.write_text("#!/bin/sh\nexit 3\n")
    script.chmod(0o755)
    counter = []
    base = systemctl_fake()

    def run(command, **kwargs):
        if command[0] == str(script):
            counter.append(1)
        return base(command, **kwargs)

    assert sp.publish(box, run=run, sleep=lambda s: None, log=lambda *_: None) == 3
    assert len(counter) == 1
    assert sp.publish(box, if_changed=True, run=run, sleep=lambda s: None,
                      log=lambda *_: None) == 3
    assert len(counter) == 2


def test_a_push_right_after_a_pass_waits_seconds_not_half_a_minute(box):
    run = systemctl_fake()
    now = [1000.0]
    slept = []
    assert sp.publish(box, run=run, sleep=slept.append, clock=lambda: now[0],
                      log=lambda *_: None) == 0
    commit(box.source, "right after")
    now[0] += 0.5
    assert sp.publish(box, if_changed=True, run=run, sleep=slept.append,
                      clock=lambda: now[0], log=lambda *_: None) == 0
    assert slept == [sp.MIN_PASS_SPACING_SECONDS - 0.5]
    assert sp.MIN_PASS_SPACING_SECONDS <= 2
    commit(box.source, "later")
    now[0] += 10
    sp.publish(box, if_changed=True, run=run, sleep=slept.append,
               clock=lambda: now[0], log=lambda *_: None)
    assert len(slept) == 1


def test_push_signature_matches_the_conformance_vector():
    vectors = json.loads(VECTORS.read_text())
    vector = next(item for item in vectors["canonicalStrings"]
                  if item["id"] == "git-push-v1-advertisement")
    fields = dict(vector["fields"])
    message = sp.push_signature_message(
        fields["owner"], fields["repo"], fields["method"], fields["ts"],
        fields["bodySha256"])
    assert message.decode() == vector["messageText"]
    Ed25519PublicKey.from_public_bytes(b64url_decode(vector["publicKey"])).verify(
        b64url_decode(vector["signature"]), message)


class FakeApp:
    def __init__(self, root: Path, public_key: str, owner="forkmesh", repo="forkmesh"):
        self.root = root
        self.public_key = public_key
        self.owner = owner
        self.repo = repo
        self.seen = []
        self.tag_moves = []
        self.tag_move_status = 200
        self.branch_deletes = []
        self.branch_delete_processing = False
        app = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                return

            def do_GET(self):
                app.handle(self, "GET")

            def do_POST(self):
                app.handle(self, "POST")

        self.server = http.server.ThreadingHTTPServer(("127.0.0.2", 0), Handler)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    @property
    def origin(self):
        return "http://127.0.0.2:%d" % self.server.server_address[1]

    def handle(self, handler, method):
        length = int(handler.headers.get("Content-Length") or 0)
        body = handler.rfile.read(length) if length else b""
        digest = hashlib.sha256(body).hexdigest()
        path, _, query = handler.path.partition("?")
        if path == sp.TAG_MOVE_PATH:
            self.tag_move(handler, body)
            return
        if path == sp.BRANCH_DELETE_PATH:
            self.branch_delete(handler, body)
            return
        pieces = path.strip("/").split("/")
        repo = pieces[1][:-4] if pieces[1].endswith(".git") else pieces[1]
        message = sp.push_signature_message(
            pieces[0], repo, method, handler.headers["X-ForkMesh-Ts"], digest)
        ok = handler.headers.get("X-ForkMesh-Pubkey") == self.public_key
        try:
            Ed25519PublicKey.from_public_bytes(b64url_decode(self.public_key)).verify(
                b64url_decode(handler.headers["X-ForkMesh-Sig"]), message)
        except Exception:
            ok = False
        if method == "POST" and handler.headers.get("X-ForkMesh-Body-Sha256") != digest:
            ok = False
        if pieces[0] != self.owner or repo != self.repo:
            ok = False
        self.seen.append((method, ok))
        if not ok:
            handler.send_response(401)
            handler.send_header("Content-Length", "0")
            handler.end_headers()
            return
        env = dict(os.environ, GIT_PROJECT_ROOT=str(self.root), GIT_HTTP_EXPORT_ALL="1",
                   REQUEST_METHOD=method, PATH_INFO=path, QUERY_STRING=query,
                   CONTENT_TYPE=handler.headers.get("Content-Type", ""),
                   CONTENT_LENGTH=str(len(body)), REMOTE_USER="publisher",
                   REMOTE_ADDR="127.0.0.1", GIT_CONFIG_COUNT="1",
                   GIT_CONFIG_KEY_0="http.receivepack", GIT_CONFIG_VALUE_0="true")
        completed = subprocess.run(["git", "http-backend"], input=body, env=env,
                                   capture_output=True, check=False)
        head, _, payload = completed.stdout.partition(b"\r\n\r\n")
        status = 200
        headers = []
        for line in head.split(b"\r\n"):
            name, _, value = line.decode().partition(":")
            if name.lower() == "status":
                status = int(value.strip().split()[0])
            elif name:
                headers.append((name, value.strip()))
        handler.send_response(status)
        for name, value in headers:
            if name.lower() != "content-length":
                handler.send_header(name, value)
        handler.send_header("Content-Length", str(len(payload)))
        handler.end_headers()
        handler.wfile.write(payload)

    def tag_move(self, handler, body):
        data = json.loads(body)
        message = sp.tag_move_message(
            data["owner"], data["repo"], data["ref"], data["target"],
            handler.headers["X-ForkMesh-Ts"])
        try:
            Ed25519PublicKey.from_public_bytes(b64url_decode(self.public_key)).verify(
                b64url_decode(handler.headers["X-ForkMesh-Sig"]), message)
            verified = handler.headers["X-ForkMesh-Pubkey"] == self.public_key
        except Exception:
            verified = False
        self.tag_moves.append((data, verified))
        status = self.tag_move_status if verified else 401
        handler.send_response(status)
        handler.send_header("Content-Length", "2")
        handler.end_headers()
        handler.wfile.write(b"{}")

    def branch_delete(self, handler, body):
        data = json.loads(body)
        message = sp.branch_delete_message(
            data["owner"], data["repo"], data["branch"], data["expect"],
            handler.headers["X-ForkMesh-Ts"])
        try:
            Ed25519PublicKey.from_public_bytes(b64url_decode(self.public_key)).verify(
                b64url_decode(handler.headers["X-ForkMesh-Sig"]), message)
            verified = handler.headers["X-ForkMesh-Pubkey"] == self.public_key
        except Exception:
            verified = False
        self.branch_deletes.append((data, verified))
        answer = {"error": "unauthorized"}
        status = 401
        if verified and self.branch_delete_processing and not data.get("requestId"):
            status = 202
            answer = {"status": "processing", "requestId": "r" * 32,
                      "node": "fmnext-n1", "pinBase": "main"}
        elif verified:
            git_dir = self.root / data["owner"] / (data["repo"] + ".git")
            deleted = subprocess.run(
                ["git", "--git-dir", str(git_dir), "update-ref", "-d",
                 "refs/heads/" + data["branch"], data["expect"]],
                capture_output=True, check=False).returncode == 0
            status = 200 if deleted else 409
            answer = {"status": "deleted"} if deleted else {
                "status": "failed", "error": "stale_head"}
        payload = json.dumps(answer).encode()
        handler.send_response(status)
        handler.send_header("Content-Length", str(len(payload)))
        handler.end_headers()
        handler.wfile.write(payload)

    def close(self):
        self.server.shutdown()
        self.server.server_close()


def write_key(path: Path) -> str:
    key = Ed25519PrivateKey.generate()
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(key.private_bytes(serialization.Encoding.PEM,
                                       serialization.PrivateFormat.PKCS8,
                                       serialization.NoEncryption()))
    path.chmod(0o600)
    return sp.b64url(key.public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw))


def test_gateway_push_signs_every_request_and_lands_refs(box, tmp_path):
    public_key = write_key(box.identity_key)
    served = tmp_path / "served"
    (served / "forkmesh").mkdir(parents=True)
    sh("clone", "-q", "--bare", str(box.bare_mirror), str(served / "forkmesh" / "forkmesh.git"))
    head = commit(box.source, "two")
    sh("tag", "v1", cwd=box.source)
    sp.sync(box.source, box.bare_mirror)
    app = FakeApp(served, public_key)
    try:
        box.fanout_gateways.parent.mkdir(parents=True, exist_ok=True)
        box.fanout_gateways.write_text(
            "# cores through the app\ncores %s/forkmesh/forkmesh.git\n"
            "plain http://example.invalid/forkmesh/forkmesh.git\n" % app.origin)
        gateways = sp.read_gateways(box.fanout_gateways, box.identity_key)
        assert [item["name"] for item in gateways] == ["cores"]
        result = sp.push_gateway(box.bare_mirror, gateways[0])
    finally:
        app.close()
    assert result["ok"], result
    assert "refs/heads/main" in result["updated"] and "refs/tags/v1" in result["updated"]
    assert app.seen and all(ok for _, ok in app.seen)
    assert {method for method, _ in app.seen} == {"GET", "POST"}
    assert sh("--git-dir", str(served / "forkmesh" / "forkmesh.git"),
              "rev-parse", "refs/heads/main") == head


def test_gateway_push_reports_diverged_main(box, tmp_path):
    public_key = write_key(box.identity_key)
    served = tmp_path / "served"
    (served / "forkmesh").mkdir(parents=True)
    target = served / "forkmesh" / "forkmesh.git"
    sh("clone", "-q", "--bare", str(box.bare_mirror), str(target))
    work = tmp_path / "intake"
    sh("clone", "-q", str(target), str(work))
    commit(work, "intake")
    sh("push", "-q", "origin", "main", cwd=work)
    commit(box.source, "two")
    sp.sync(box.source, box.bare_mirror)
    app = FakeApp(served, public_key)
    try:
        result = sp.push_gateway(box.bare_mirror, {
            "name": "cores", "url": app.origin + "/forkmesh/forkmesh.git",
            "key": box.identity_key})
    finally:
        app.close()
    assert not result["ok"]
    assert result["diverged"] == ["refs/heads/main"]


HUNG_UP = ("error: RPC failed; HTTP 502 curl 22 The requested URL returned error: 502\n"
           "send-pack: unexpected disconnect while reading sideband packet\n"
           "fatal: the remote end hung up unexpectedly\n")


def test_a_gateway_that_hangs_up_is_retried_once(box):
    write_key(box.identity_key)
    calls, slept = [], []

    def run(command, **kwargs):
        calls.append(command)
        if len(calls) == 1:
            return subprocess.CompletedProcess(command, 128, "", HUNG_UP)
        return subprocess.CompletedProcess(
            command, 0, "To x\n \trefs/heads/main:refs/heads/main\t1..2\nDone\n", "")

    result = sp.push_gateway(box.bare_mirror, {
        "name": "cores", "url": "http://127.0.0.1:9/forkmesh/forkmesh.git",
        "key": box.identity_key}, run=run, sleep=slept.append,
        probe=lambda port: True)
    assert len(calls) == 2 and slept == [sp.GATEWAY_RETRY_SECONDS]
    assert result["ok"] and not result["transient"]
    assert result["updated"] == ["refs/heads/main"]


def test_main_refused_while_a_core_job_holds_its_ref_lock_is_retried_once(box):
    write_key(box.identity_key)
    calls, slept = [], []

    def run(command, **kwargs):
        calls.append(command)
        if len(calls) == 1:
            return subprocess.CompletedProcess(
                command, 1, "To x\n!\trefs/heads/main:refs/heads/main\t"
                "[remote rejected] (failed to update ref)\nDone\n", "")
        return subprocess.CompletedProcess(
            command, 0, "To x\n \trefs/heads/main:refs/heads/main\t1..2\nDone\n", "")

    result = sp.push_gateway(box.bare_mirror, {
        "name": "cores", "url": "http://127.0.0.1:9/forkmesh/forkmesh.git",
        "key": box.identity_key}, run=run, sleep=slept.append,
        probe=lambda port: True)
    assert len(calls) == 2 and slept == [sp.GATEWAY_RETRY_SECONDS]
    assert result["ok"] and result["updated"] == ["refs/heads/main"]


UNPACK_RACE = (
    "To x\n!\trefs/heads/main:refs/heads/main\t[remote rejected] (unpacker error)\n"
    "!\trefs/heads/old:refs/heads/old\t[rejected] (non-fast-forward)\nDone\n",
    "remote: fatal: did not receive expected object 4c238df84bfecc1f927905eef138d86a48db52e9\n"
    "error: remote unpack failed: index-pack abnormal exit\n")


def test_a_push_unpacked_by_a_core_missing_an_advertised_object_is_retried_once(box):
    write_key(box.identity_key)
    calls, slept = [], []

    def run(command, **kwargs):
        calls.append(command)
        if len(calls) == 1:
            return subprocess.CompletedProcess(command, 1, *UNPACK_RACE)
        return subprocess.CompletedProcess(
            command, 0, "To x\n \trefs/heads/main:refs/heads/main\t1..2\nDone\n", "")

    result = sp.push_gateway(box.bare_mirror, {
        "name": "cores", "url": "http://127.0.0.1:9/forkmesh/forkmesh.git",
        "key": box.identity_key}, run=run, sleep=slept.append,
        probe=lambda port: True)
    assert len(calls) == 2 and slept == [sp.GATEWAY_RETRY_SECONDS]
    assert result["ok"] and result["updated"] == ["refs/heads/main"]


def test_an_unpacker_error_after_the_retry_still_fails(box):
    write_key(box.identity_key)
    calls = []

    def run(command, **kwargs):
        calls.append(command)
        return subprocess.CompletedProcess(command, 1, *UNPACK_RACE)

    result = sp.push_gateway(box.bare_mirror, {
        "name": "cores", "url": "http://127.0.0.1:9/forkmesh/forkmesh.git",
        "key": box.identity_key}, run=run, sleep=lambda seconds: None,
        probe=lambda port: True)
    assert len(calls) == 2
    assert not result["ok"] and result["diverged"] == ["refs/heads/main"]


def test_a_gateway_still_down_after_the_retry_is_unreachable_not_failed(box):
    write_key(box.identity_key)
    calls = []

    def run(command, **kwargs):
        calls.append(command)
        return subprocess.CompletedProcess(command, 128, "", HUNG_UP)

    result = sp.push_gateway(box.bare_mirror, {
        "name": "cores", "url": "http://127.0.0.1:9/forkmesh/forkmesh.git",
        "key": box.identity_key}, run=run, sleep=lambda seconds: None,
        probe=lambda port: True)
    assert len(calls) == 2
    assert not result["ok"] and result["transient"]


def test_a_blocked_loopback_fails_fast_without_running_git(box):
    write_key(box.identity_key)
    calls, probed = [], []

    def run(command, **kwargs):
        calls.append(command)
        return subprocess.CompletedProcess(command, 0, "", "")

    def probe(port):
        probed.append(port)
        return False

    result = sp.push_gateway(box.bare_mirror, {
        "name": "cores", "url": "http://127.0.0.1:9/forkmesh/forkmesh.git",
        "key": box.identity_key}, run=run, sleep=lambda seconds: None, probe=probe)
    assert calls == [] and len(probed) == 3
    assert not result["ok"] and result["transient"] and result["loopbackBlocked"]
    assert "last port %d" % probed[-1] in result["error"]
    assert "firewall" in result["error"] and result["diverged"] == []


def test_signing_proxy_uses_ipv6_when_ipv4_loopback_is_blocked(box):
    write_key(box.identity_key)
    probed, calls = [], []

    def probe(port):
        probed.append(port)
        return len(probed) == 2

    def run(command, **kwargs):
        calls.append(command)
        assert kwargs["env"]["NO_PROXY"] == "127.0.0.1,::1,127.0.0.2"
        return subprocess.CompletedProcess(command, 0, "", "")

    result = sp.push_gateway(box.bare_mirror, {
        "name": "cores", "url": "https://app.example.invalid/forkmesh/forkmesh.git",
        "key": box.identity_key}, run=run, probe=probe)
    assert result["ok"] and len(calls) == 1
    assert any(value.startswith("http://[::1]:") for value in calls[0])


def test_publishing_failure_pings_with_a_fix_and_deduplicates_until_recovery(box):
    write_key(box.identity_key)
    box.settings_file.parent.mkdir(parents=True, exist_ok=True)
    box.settings_file.write_text("nodeName=forkmesh\n")
    box.fanout_gateways.parent.mkdir(parents=True, exist_ok=True)
    box.fanout_gateways.write_text("cores https://app.example.invalid/forkmesh/forkmesh.git\n")
    requests = []

    class Response:
        def __enter__(self):
            return self

        def __exit__(self, *args):
            return False

        def read(self):
            return b'{"pings":1}'

    def opener(request, timeout):
        requests.append(request)
        return Response()

    failed = {"ok": False, "gateways": [{"ok": False, "loopbackBlocked": True,
                                         "error": "loopback blocked"}]}
    sp.notify_publish_failure(box, failed, opener=opener)
    sp.notify_publish_failure(box, failed, opener=opener)
    assert len(requests) == 1
    payload = json.loads(requests[0].data)
    assert "Fix:" in payload["body"] and "OpenSnitch" in payload["body"]
    assert "python3 tools/source_publisher.py publish" in payload["body"]
    sp.notify_publish_failure(box, {"ok": True}, opener=opener)
    sp.notify_publish_failure(box, failed, opener=opener)
    assert len(requests) == 2


def test_a_refused_pull_ledger_waits_for_the_desktop_before_it_pings(box):
    write_key(box.identity_key)
    box.settings_file.parent.mkdir(parents=True, exist_ok=True)
    box.settings_file.write_text("nodeName=forkmesh\n")
    box.fanout_gateways.parent.mkdir(parents=True, exist_ok=True)
    box.fanout_gateways.write_text("cores https://app.example.invalid/forkmesh/forkmesh.git\n")
    requests = []

    class Response:
        def __enter__(self):
            return self

        def __exit__(self, *args):
            return False

        def read(self):
            return b'{"pings":1}'

    def opener(request, timeout):
        requests.append(request)
        return Response()

    now = [1000.0]
    ledger = {"ok": False, "fanout": {"ok": True}, "gateways": [{
        "ok": False, "rejected": [
            {"ref": "refs/heads/forkmesh/pulls", "reason": "fetch first"},
            {"ref": "refs/heads/old-branch", "reason": "non-fast-forward"}]}]}
    sp.notify_publish_failure(box, ledger, opener=opener, clock=lambda: now[0])
    now[0] += sp.LEDGER_GRACE_SECONDS - 1
    sp.notify_publish_failure(box, ledger, opener=opener, clock=lambda: now[0])
    assert requests == []
    now[0] += 2
    sp.notify_publish_failure(box, ledger, opener=opener, clock=lambda: now[0])
    assert len(requests) == 1
    assert "refs/heads/forkmesh/pulls" in json.loads(requests[0].data)["body"]
    sp.notify_publish_failure(box, {"ok": True}, opener=opener, clock=lambda: now[0])
    main = {"ok": False, "fanout": {"ok": True}, "gateways": [{
        "ok": False, "rejected": [{"ref": "refs/heads/main", "reason": "fetch first"}]}]}
    sp.notify_publish_failure(box, main, opener=opener, clock=lambda: now[0])
    assert len(requests) == 2


def test_git_timing_out_on_the_proxy_port_is_reported_as_blocked_loopback(box):
    write_key(box.identity_key)
    calls = []

    def run(command, **kwargs):
        calls.append(command)
        port = re.search(r"http://127\.0\.0\.1:(\d+)/", " ".join(command)).group(1)
        return subprocess.CompletedProcess(
            command, 128, "",
            "fatal: unable to access 'http://127.0.0.1:%s/x/': Failed to connect to "
            "127.0.0.1 port %s after 133323 ms: Could not connect to server\n" % (port, port))

    result = sp.push_gateway(box.bare_mirror, {
        "name": "cores", "url": "http://127.0.0.1:9/forkmesh/forkmesh.git",
        "key": box.identity_key}, run=run, sleep=lambda seconds: None,
        probe=lambda port: True)
    assert len(calls) == 1
    assert not result["ok"] and result["loopbackBlocked"]
    assert "OpenSnitch" in result["error"]


def test_a_rejected_push_is_not_retried(box):
    write_key(box.identity_key)
    calls = []

    def run(command, **kwargs):
        calls.append(command)
        return subprocess.CompletedProcess(
            command, 1, "To x\n!\trefs/heads/main:refs/heads/main\t[rejected] (fetch first)\n",
            "error: failed to push some refs to 'x'\n")

    result = sp.push_gateway(box.bare_mirror, {
        "name": "cores", "url": "http://127.0.0.1:9/forkmesh/forkmesh.git",
        "key": box.identity_key}, run=run, sleep=lambda seconds: None,
        probe=lambda port: True)
    assert len(calls) == 1
    assert not result["ok"] and not result["transient"]
    assert result["diverged"] == ["refs/heads/main"]


def test_gateway_push_with_an_unknown_key_is_refused(box, tmp_path):
    write_key(box.identity_key)
    served = tmp_path / "served"
    (served / "forkmesh").mkdir(parents=True)
    sh("clone", "-q", "--bare", str(box.bare_mirror), str(served / "forkmesh" / "forkmesh.git"))
    other = write_key(tmp_path / "other.pem")
    app = FakeApp(served, other)
    try:
        result = sp.push_gateway(box.bare_mirror, {
            "name": "cores", "url": app.origin + "/forkmesh/forkmesh.git",
            "key": box.identity_key})
    finally:
        app.close()
    assert not result["ok"]
    assert app.seen and not any(ok for _, ok in app.seen)


def test_signing_proxy_refuses_foreign_paths(box):
    write_key(box.identity_key)
    signer = sp.Signer(box.identity_key)
    calls = []

    class Opener:
        def open(self, request, timeout=None):
            calls.append(request.full_url)
            raise AssertionError("must not forward")

    import urllib.request
    with sp.SigningProxy("https://app.example.invalid", signer, "forkmesh", "forkmesh",
                         opener=Opener(), host="127.0.0.2") as proxy:
        base = proxy.url.rsplit("/", 3)[0]
        for path in ("/wrong-token/forkmesh/forkmesh.git/info/refs?service=git-receive-pack",
                     "/%s/other/forkmesh.git/info/refs?service=git-receive-pack" % proxy.token,
                     "/%s/forkmesh/forkmesh.git/info/refs?service=git-upload-pack" % proxy.token):
            try:
                urllib.request.build_opener(urllib.request.ProxyHandler({})).open(base + path, timeout=10)
                raise AssertionError("expected 404 for " + path)
            except urllib.error.HTTPError as error:
                assert error.code == 404
    assert calls == []


def test_parse_push_porcelain():
    output = ("To http://x\n"
              "*\trefs/heads/new:refs/heads/new\t[new branch]\n"
              " \trefs/heads/agent/a:refs/heads/agent/a\t1..2\n"
              "!\trefs/heads/main:refs/heads/main\t[rejected] (fetch first)\n"
              "=\trefs/tags/v1:refs/tags/v1\t[up to date]\n"
              "Done\n")
    result = sp.parse_push_porcelain(output)
    assert result["updated"] == ["refs/heads/new", "refs/heads/agent/a"]
    assert result["diverged"] == ["refs/heads/main"]


def test_guard_starts_a_stopped_flagship_unless_held(box):
    calls = []
    run = systemctl_fake({sp.SOURCE_UNIT: "inactive"}, calls)
    assert sp.guard(box, run=run, log=lambda *_: None) == 0
    assert ["systemctl", "--user", "start", "--no-block", sp.SOURCE_UNIT] in calls
    calls.clear()
    sp.hold(box, "2h", "rebuilding by hand")
    assert sp.guard(box, run=run, log=lambda *_: None) == 0
    assert not any(command[2] == "start" for command in calls)
    assert sp.unhold(box)
    calls.clear()
    assert sp.guard(box, run=systemctl_fake({sp.SOURCE_UNIT: "active"}, calls),
                    log=lambda *_: None) == 0
    assert not any(command[2] == "start" for command in calls)


def test_desktop_close_hold_keeps_the_flagship_stopped(box):
    sp.write_json(box.publisher_state / "source-hold.json",
                  {"until": 1000 + 100 * 365 * 86400, "reason": "closed from the desktop window",
                   "setAt": 1000, "setBy": "desktop"})
    calls = []
    run = systemctl_fake({sp.SOURCE_UNIT: "inactive"}, calls)
    assert sp.guard(box, run=run, clock=lambda: 2000.0, log=lambda *_: None) == 0
    assert not any(command[2] == "start" for command in calls)


def test_expired_hold_does_not_block(box):
    sp.hold(box, "1m", "short", clock=lambda: 0.0)
    assert sp.active_hold(box.publisher_state / "source-hold.json") is None
    with pytest.raises(sp.PublishError):
        sp.parse_duration("soon")


def test_install_writes_units_and_links(box, tmp_path):
    user_units = tmp_path / "user-units"
    assert sp.install(box, user_units=user_units, enable=False, log=lambda *_: None) == 0
    assert (box.publisher_bin / "source_publisher.py").is_file()
    assert (box.publisher_bin / "forkmesh_paths.py").is_file()
    for name in sp.UNIT_NAMES:
        assert (user_units / name).is_symlink()
        assert os.readlink(user_units / name) == str(box.systemd_units / name)
    path_unit = (box.systemd_units / "forkmesh-source-publish.path").read_text()
    assert str(box.source / ".git" / "refs" / "heads") in path_unit
    assert str(box.action_spool) in path_unit
    fanout = (box.systemd_units / "forkmesh-mirror-fanout.service").read_text()
    assert "source_publisher.py publish" in fanout
    watch = (box.systemd_units / "forkmesh-source-cores.service").read_text()
    assert "source_publisher.py watch-cores" in watch
    assert "OnUnitActiveSec=20s" in (box.systemd_units / "forkmesh-source-cores.timer").read_text()
    deletes = (box.systemd_units / sp.BRANCH_DELETE_UNIT).read_text()
    assert "source_publisher.py delete-branches" in deletes
    assert sp.install(box, user_units=user_units, enable=False, log=lambda *_: None) == 0


def test_fleet_paths_follow_the_move(tmp_path):
    home = tmp_path / "home"
    projects = home / "projects"
    environ = {"FORKMESH_PROJECTS_DIR": str(projects),
               "FORKMESH_HOME": str(projects / "forkmesh")}
    (projects / "forkmesh").mkdir(parents=True)
    legacy_config = projects / "forkmesh-next" / "config"
    legacy_key = projects / "forkmesh-next" / "ssh" / "next_fleet_ed25519"
    legacy_config.mkdir(parents=True)
    legacy_key.parent.mkdir(parents=True)
    legacy_key.write_text("k")
    paths = forkmesh_paths.resolve(environ=environ, home=home)
    assert paths.fleet_config == legacy_config
    assert paths.fleet_ssh_key == legacy_key
    (projects / "forkmesh" / "config" / "fleet").mkdir(parents=True)
    (projects / "forkmesh" / "ssh" / "fleet").mkdir(parents=True)
    paths = forkmesh_paths.resolve(environ=environ, home=home)
    assert paths.fleet_config == projects / "forkmesh" / "config" / "fleet"
    assert paths.fleet_ssh_key == projects / "forkmesh" / "ssh" / "fleet" / "next_fleet_ed25519"


def test_fleet_data_and_logs_follow_the_move(tmp_path):
    home = tmp_path / "home"
    projects = home / "projects"
    environ = {"FORKMESH_PROJECTS_DIR": str(projects),
               "FORKMESH_HOME": str(projects / "forkmesh")}
    (projects / "forkmesh-next" / "data" / "bin").mkdir(parents=True)
    (projects / "forkmesh-next" / "logs").mkdir(parents=True)
    paths = forkmesh_paths.resolve(environ=environ, home=home)
    assert paths.fleet_data == projects / "forkmesh-next" / "data"
    assert paths.fleet_logs == projects / "forkmesh-next" / "logs"
    assert paths.fleet_ssh == projects / "forkmesh" / "data" / "core" / "ssh" / "fleet"
    (projects / "forkmesh" / "data" / "fleet").mkdir(parents=True)
    (projects / "forkmesh" / "logs" / "fleet").mkdir(parents=True)
    paths = forkmesh_paths.resolve(environ=environ, home=home)
    assert paths.fleet_data == projects / "forkmesh" / "data" / "fleet"
    assert paths.fleet_logs == projects / "forkmesh" / "logs" / "fleet"
    (projects / "forkmesh-next").rename(projects / "forkmesh-backup")
    paths = forkmesh_paths.resolve(environ=environ, home=home)
    assert paths.fleet_config == projects / "forkmesh" / "data" / "core" / "config" / "fleet"
    assert paths.fleet_ssh_key == projects / "forkmesh" / "data" / "core" / "ssh" / "fleet" / "next_fleet_ed25519"
    binary = paths.fleet_data / "bin" / "rqlited"
    binary.parent.mkdir()
    binary.write_text("#!/bin/sh\n")
    binary.chmod(0o755)
    assert paths.fleet_binary("rqlited") == str(binary)


def served_copy(box, tmp_path):
    served = tmp_path / "served"
    (served / "forkmesh").mkdir(parents=True)
    target = served / "forkmesh" / "forkmesh.git"
    sh("clone", "-q", "--bare", str(box.bare_mirror), str(target))
    return served, target


def test_a_moved_tag_is_pushed_after_an_audited_tag_move(box, tmp_path):
    public_key = write_key(box.identity_key)
    sh("tag", "-a", "v1", "-m", "first", cwd=box.source)
    sp.sync(box.source, box.bare_mirror)
    served, target = served_copy(box, tmp_path)
    old = sh("--git-dir", str(target), "rev-parse", "refs/tags/v1")
    commit(box.source, "release")
    sh("tag", "-f", "-a", "v1", "-m", "moved", cwd=box.source)
    sh("--git-dir", str(box.bare_mirror), "fetch", "-q", str(box.source),
       "+refs/tags/v1:refs/tags/v1", "refs/heads/main:refs/heads/main")
    moved = sh("--git-dir", str(box.bare_mirror), "rev-parse", "refs/tags/v1")
    app = FakeApp(served, public_key)
    try:
        result = sp.push_gateway(box.bare_mirror, {
            "name": "cores", "url": app.origin + "/forkmesh/forkmesh.git",
            "key": box.identity_key})
    finally:
        app.close()
    assert result["ok"], result
    assert result["tagsMoved"] == ["refs/tags/v1"]
    assert result["rejected"] == []
    assert app.tag_moves == [({"owner": "forkmesh", "repo": "forkmesh",
                               "ref": "refs/tags/v1", "target": moved}, True)]
    assert old != moved
    assert sh("--git-dir", str(target), "rev-parse", "refs/tags/v1") == moved


def test_a_refused_tag_move_leaves_the_tag_and_fails_the_pass(box, tmp_path):
    public_key = write_key(box.identity_key)
    sh("tag", "v1", cwd=box.source)
    sp.sync(box.source, box.bare_mirror)
    served, target = served_copy(box, tmp_path)
    old = sh("--git-dir", str(target), "rev-parse", "refs/tags/v1")
    commit(box.source, "release")
    sh("--git-dir", str(box.bare_mirror), "fetch", "-q", str(box.source),
       "refs/heads/main:refs/heads/main")
    sh("--git-dir", str(box.bare_mirror), "tag", "-f", "v1", "main")
    app = FakeApp(served, public_key)
    app.tag_move_status = 403
    try:
        result = sp.push_gateway(box.bare_mirror, {
            "name": "cores", "url": app.origin + "/forkmesh/forkmesh.git",
            "key": box.identity_key})
    finally:
        app.close()
    assert not result["ok"]
    assert [item["ref"] for item in result["rejected"]] == ["refs/tags/v1"]
    assert result["tagMoves"][0]["status"] == 403
    assert sh("--git-dir", str(target), "rev-parse", "refs/tags/v1") == old


def test_a_rewritten_side_branch_is_reported_stale_not_failed(box, tmp_path):
    public_key = write_key(box.identity_key)
    sh("checkout", "-q", "-b", "side", cwd=box.source)
    commit(box.source, "side-one")
    sh("checkout", "-q", "main", cwd=box.source)
    sp.sync(box.source, box.bare_mirror)
    served, target = served_copy(box, tmp_path)
    sh("branch", "-f", "side", "main", cwd=box.source)
    sh("checkout", "-q", "side", cwd=box.source)
    commit(box.source, "side-rewritten")
    sh("checkout", "-q", "main", cwd=box.source)
    sh("--git-dir", str(box.bare_mirror), "fetch", "-q", str(box.source),
       "+refs/heads/side:refs/heads/side")
    app = FakeApp(served, public_key)
    try:
        result = sp.push_gateway(box.bare_mirror, {
            "name": "cores", "url": app.origin + "/forkmesh/forkmesh.git",
            "key": box.identity_key})
    finally:
        app.close()
    assert result["ok"], result
    assert result["stale"] == ["refs/heads/side"]


def _cores_with(box, tmp_path, name):
    cores = tmp_path / "cores.git"
    sh("clone", "-q", "--bare", str(box.source), str(cores))
    work = tmp_path / "owner"
    sh("clone", "-q", str(cores), str(work))
    pushed = commit(work, name)
    sh("push", "-q", "origin", "main", cwd=work)
    return cores, pushed


def test_absorb_fast_forwards_the_checkout_to_an_owner_push(box, tmp_path):
    cores, pushed = _cores_with(box, tmp_path, "owner-change")
    outcome = sp.absorb_cores(box.source, str(cores))
    assert outcome["absorbed"] and outcome["mode"] == "fast-forward"
    assert sh("rev-parse", "main", cwd=box.source) == pushed
    assert (box.source / "owner-change").is_file()
    again = sp.absorb_cores(box.source, str(cores))
    assert not again["absorbed"] and again["reason"] == "already contained"


def test_absorb_merges_a_clean_divergence_and_refuses_a_conflict(box, tmp_path):
    cores, pushed = _cores_with(box, tmp_path, "owner-change")
    local = commit(box.source, "local-change")
    outcome = sp.absorb_cores(box.source, str(cores))
    assert outcome["absorbed"] and outcome["mode"] == "merge"
    parents = sh("show", "-s", "--format=%P", "main", cwd=box.source).split()
    assert parents == [local, pushed]
    assert sh("show", "-s", "--format=%s", "main", cwd=box.source) == \
        sp.CORES_MERGE_MESSAGE

    work = tmp_path / "owner"
    (work / "shared").write_text("owner\n")
    sh("add", "shared", cwd=work)
    sh("commit", "-q", "-m", "owner shared", cwd=work)
    sh("push", "-q", "origin", "main", cwd=work)
    (box.source / "shared").write_text("local\n")
    sh("add", "shared", cwd=box.source)
    sh("commit", "-q", "-m", "local shared", cwd=box.source)
    before = sh("rev-parse", "main", cwd=box.source)
    refused = sp.absorb_cores(box.source, str(cores))
    assert not refused["absorbed"] and "conflict" in refused["reason"]
    assert sh("rev-parse", "main", cwd=box.source) == before
    assert sh("status", "--porcelain", cwd=box.source) == ""


def _detach_main(box):
    sh("checkout", "-q", "--detach", cwd=box.source)


def test_absorb_fast_forwards_main_when_no_worktree_has_it(box, tmp_path):
    cores, pushed = _cores_with(box, tmp_path, "runner-merge")
    _detach_main(box)
    before = sh("rev-parse", "HEAD", cwd=box.source)
    outcome = sp.absorb_cores(box.source, str(cores))
    assert outcome["absorbed"] and outcome["mode"] == "fast-forward"
    assert sh("rev-parse", "main", cwd=box.source) == pushed
    assert sh("rev-parse", "HEAD", cwd=box.source) == before


def test_absorb_merges_main_when_no_worktree_has_it(box, tmp_path):
    cores, pushed = _cores_with(box, tmp_path, "runner-merge")
    local = commit(box.source, "local-change")
    _detach_main(box)
    hook = box.source / ".git" / "hooks" / "post-merge"
    marker = tmp_path / "hook-ran"
    hook.write_text("#!/bin/sh\ntouch '%s'\n" % marker)
    hook.chmod(0o755)
    outcome = sp.absorb_cores(box.source, str(cores))
    assert outcome["absorbed"] and outcome["mode"] == "merge"
    assert sh("show", "-s", "--format=%P", "main", cwd=box.source).split() == [local, pushed]
    assert sh("ls-tree", "--name-only", "main", cwd=box.source).split() == \
        ["local-change", "one", "runner-merge"]
    assert marker.is_file()


def test_absorb_without_a_worktree_refuses_a_conflict(box, tmp_path):
    cores, _ = _cores_with(box, tmp_path, "shared")
    (box.source / "shared").write_text("local\n")
    sh("add", "shared", cwd=box.source)
    sh("commit", "-q", "-m", "local shared", cwd=box.source)
    before = sh("rev-parse", "main", cwd=box.source)
    _detach_main(box)
    refused = sp.absorb_cores(box.source, str(cores))
    assert not refused["absorbed"] and "conflict" in refused["reason"]
    assert sh("rev-parse", "main", cwd=box.source) == before


def _watch(box, monkeypatch, cores_oid, contained_after=True):
    monkeypatch.setattr(sp, "read_gateways", lambda path, key: [
        {"name": "cores", "url": "https://cores.example/forkmesh/forkmesh.git"}])
    monkeypatch.setattr(sp, "cores_main", lambda url: cores_oid)
    passes = []

    def publish(paths, run=None, log=None):
        passes.append(paths)
        if contained_after:
            sh("merge", "-q", "--ff-only", cores_oid, cwd=box.source)
        return 0
    monkeypatch.setattr(sp, "publish", publish)
    logs = []
    code = sp.watch_cores(box, run=systemctl_fake(), log=logs.append)
    return code, passes, logs


def test_watch_does_nothing_while_the_checkout_has_the_cores_main(box, monkeypatch):
    main = sh("rev-parse", "main", cwd=box.source)
    code, passes, logs = _watch(box, monkeypatch, main)
    assert code == 0 and passes == [] and logs == []


def test_watch_publishes_when_a_web_merge_moved_the_cores_main(box, tmp_path, monkeypatch):
    cores, pushed = _cores_with(box, tmp_path, "runner-merge")
    sh("fetch", "-q", str(cores), "main", cwd=box.source)
    code, passes, logs = _watch(box, monkeypatch, pushed)
    assert code == 0 and len(passes) == 1
    assert any("publishing now" in line for line in logs)
    assert not (box.publisher_state / "cores-watch.json").exists()


def test_watch_does_not_repeat_a_pass_that_could_not_take_the_cores_main(
        box, tmp_path, monkeypatch):
    cores, pushed = _cores_with(box, tmp_path, "runner-merge")
    code, passes, logs = _watch(box, monkeypatch, pushed, contained_after=False)
    assert len(passes) == 1
    assert any("2-minute pass keeps trying" in line for line in logs)
    code, passes, logs = _watch(box, monkeypatch, pushed, contained_after=False)
    assert passes == [] and logs == []


def test_cores_main_reads_the_served_main(box, tmp_path):
    cores, pushed = _cores_with(box, tmp_path, "runner-merge")
    assert sp.cores_main(str(cores)) == pushed
    assert sp.cores_main(str(tmp_path / "missing.git")) == ""


def _diverged_gateway(ref="refs/heads/main"):
    return {"name": "cores", "url": "https://cores.example/forkmesh/forkmesh.git",
            "ok": False, "updated": [], "other": [],
            "rejected": [{"ref": ref, "reason": "non-fast-forward"}],
            "diverged": [ref], "stale": [],
            "error": "error: failed to push some refs to 'http://127.0.0.1:1/x'"}


def _publish_with_gateway(box, monkeypatch, absorb):
    monkeypatch.setattr(sp, "run_fanout", lambda script, run=None: {"ok": True, "code": 0})
    monkeypatch.setattr(sp, "read_gateways", lambda path, key: [{"name": "cores"}])
    monkeypatch.setattr(sp, "push_gateway", lambda bare, gateway, run=None: _diverged_gateway())
    monkeypatch.setattr(sp, "absorb_cores", lambda source, url: absorb)
    logs = []
    code = sp.publish(box, run=systemctl_fake(), sleep=lambda s: None, log=logs.append)
    return code, logs


def test_a_pass_outrun_by_a_newer_publish_is_not_a_failure(box, monkeypatch):
    commit(box.source, "newer")
    code, logs = _publish_with_gateway(box, monkeypatch, {
        "absorbed": False, "reason": "already contained",
        "cores": sh("rev-parse", "main", cwd=box.source)})
    assert code == 0
    assert not any("FAILED" in line or "DIVERGED" in line for line in logs)
    assert any("already past this pass" in line for line in logs)
    status = json.loads((box.publisher_state / "status.json").read_text())
    assert status["ok"] and status["gateways"][0]["outrun"]


def test_a_refused_push_to_cores_that_are_behind_is_not_outrun(box, monkeypatch):
    behind = sh("rev-parse", "main", cwd=box.source)
    commit(box.source, "merge the cores lack")
    code, logs = _publish_with_gateway(box, monkeypatch, {
        "absorbed": False, "reason": "already contained", "cores": behind})
    assert code != 0
    assert not any("already past this pass" in line for line in logs)
    assert any("the cores are behind and refused main: non-fast-forward" in line
               for line in logs)
    status = json.loads((box.publisher_state / "status.json").read_text())
    assert not status["ok"] and not status["gateways"][0].get("outrun")


def test_a_real_core_divergence_still_fails(box, monkeypatch):
    code, logs = _publish_with_gateway(box, monkeypatch, {
        "absorbed": False, "cores": "c" * 40,
        "reason": "main and the cores' main conflict; merge cccccccccccc by hand"})
    assert code != 0
    assert any("FAILED" in line for line in logs)
    assert any("DIVERGED" in line and "by hand" in line for line in logs)


def _publish_ledger_rejection(box, monkeypatch, contained):
    ref = "refs/heads/forkmesh/pulls"
    monkeypatch.setattr(sp, "run_fanout", lambda script, run=None: {"ok": True, "code": 0})
    monkeypatch.setattr(sp, "read_gateways", lambda path, key: [{"name": "cores"}])
    monkeypatch.setattr(sp, "push_gateway", lambda bare, gateway, run=None: _diverged_gateway(ref))
    monkeypatch.setattr(sp, "cores_ref_contained", lambda source, url, name, pushed: contained)
    logs = []
    code = sp.publish(box, run=systemctl_fake(), sleep=lambda s: None, log=logs.append)
    return code, logs


def test_a_ledger_push_outrun_by_a_newer_publish_is_not_a_failure(box, monkeypatch):
    code, logs = _publish_ledger_rejection(box, monkeypatch, contained=True)
    assert code == 0
    assert not any("FAILED" in line or "DIVERGED" in line for line in logs)
    assert any("refs/heads/forkmesh/pulls already past this pass" in line for line in logs)


def test_a_real_ledger_divergence_still_fails(box, monkeypatch):
    code, logs = _publish_ledger_rejection(box, monkeypatch, contained=False)
    assert code != 0
    assert any("FAILED" in line for line in logs)
    assert any("DIVERGED" in line and "forkmesh/pulls" in line for line in logs)


def test_a_ledger_read_from_a_lagging_core_is_read_again(box, monkeypatch):
    reads, waits = iter([False, True]), []
    monkeypatch.setattr(sp, "run_fanout", lambda script, run=None: {"ok": True, "code": 0})
    monkeypatch.setattr(sp, "read_gateways", lambda path, key: [{"name": "cores"}])
    monkeypatch.setattr(sp, "push_gateway", lambda bare, gateway, run=None:
                        _diverged_gateway("refs/heads/forkmesh/pulls"))
    monkeypatch.setattr(sp, "cores_ref_contained", lambda source, url, name, pushed: next(reads))
    logs = []
    code = sp.publish(box, run=systemctl_fake(), sleep=waits.append, log=logs.append)
    assert code == 0
    assert sp.GATEWAY_RETRY_SECONDS in waits
    assert not any("FAILED" in line or "DIVERGED" in line for line in logs)


def test_a_main_read_from_a_lagging_core_is_read_again(box, monkeypatch):
    behind = sh("rev-parse", "main", cwd=box.source)
    commit(box.source, "merge one core has not replicated yet")
    caught_up = sh("rev-parse", "main", cwd=box.source)
    reads = iter([{"absorbed": False, "reason": "already contained", "cores": behind},
                  {"absorbed": False, "reason": "already contained", "cores": caught_up}])
    monkeypatch.setattr(sp, "run_fanout", lambda script, run=None: {"ok": True, "code": 0})
    monkeypatch.setattr(sp, "read_gateways", lambda path, key: [{"name": "cores"}])
    monkeypatch.setattr(sp, "push_gateway", lambda bare, gateway, run=None: _diverged_gateway())
    monkeypatch.setattr(sp, "absorb_cores", lambda source, url: next(reads))
    logs = []
    code = sp.publish(box, run=systemctl_fake(), sleep=lambda s: None, log=logs.append)
    assert code == 0
    assert any("already past this pass" in line for line in logs)


def test_a_failure_ping_names_the_refused_ref_not_the_git_output():
    gateway = _diverged_gateway("refs/heads/forkmesh/pulls")
    gateway["stale"] = ["refs/heads/old"]
    gateway["rejected"].append({"ref": "refs/heads/old", "reason": "non-fast-forward"})
    gateway["error"] = "t you pushed in some\nremote: other way."
    cause = sp.gateway_failure_cause(gateway)
    assert cause == "the cores refused refs/heads/forkmesh/pulls (non-fast-forward)"
    assert sp.gateway_failure_cause({"error": "loopback blocked"}) == "loopback blocked"


def test_cores_ref_contained_reads_the_cores_value(box, tmp_path):
    served = tmp_path / "cores.git"
    sh("clone", "-q", "--bare", str(box.source), str(served))
    sh("branch", "forkmesh/pulls", cwd=box.source)
    sh("push", "-q", str(served), "forkmesh/pulls", cwd=box.source)
    commit(box.source, "ledger ahead")
    sh("branch", "-f", "forkmesh/pulls", "main", cwd=box.source)
    assert sp.cores_ref_contained(box.source, str(served), "refs/heads/forkmesh/pulls")
    work = tmp_path / "core-work"
    sh("clone", "-q", "-b", "forkmesh/pulls", str(served), str(work))
    commit(work, "core-only ledger record")
    sh("push", "-q", "origin", "forkmesh/pulls", cwd=work)
    assert not sp.cores_ref_contained(box.source, str(served), "refs/heads/forkmesh/pulls")
    sh("pull", "-q", "--no-rebase", "--no-edit", str(served), "forkmesh/pulls", cwd=box.source)
    sh("branch", "-f", "forkmesh/pulls", "main", cwd=box.source)
    ledger = sh("rev-parse", "forkmesh/pulls", cwd=box.source)
    assert sp.cores_ref_contained(box.source, str(served), "refs/heads/forkmesh/pulls")
    assert not sp.cores_ref_contained(box.source, str(served),
                                      "refs/heads/forkmesh/pulls", ledger)


def _empty_object_in_bare(box, name):
    oid = sh("rev-parse", "main:" + name, cwd=box.source)
    path = box.bare_mirror / "objects" / oid[:2] / oid[2:]
    if path.exists():
        os.replace(path, path.with_name(path.name + ".linked"))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"")
    return oid


def test_heal_moves_empty_objects_aside_and_restores_them_from_the_checkout(box, tmp_path):
    oid = _empty_object_in_bare(box, "one")
    assert sp.empty_loose_objects(box.bare_mirror) == [oid]
    quarantine = tmp_path / "quarantine"
    healed = sp.heal_empty_objects(box.source, box.bare_mirror, quarantine)
    assert healed == {"moved": [oid], "restored": [oid], "missing": []}
    assert (quarantine / oid[:2] / oid[2:]).stat().st_size == 0
    assert sh("--git-dir", str(box.bare_mirror), "cat-file", "-p", oid) == "one"
    assert sp.empty_loose_objects(box.bare_mirror) == []


def test_a_sync_blocked_by_an_empty_object_heals_and_retries(box, monkeypatch, tmp_path):
    oid = _empty_object_in_bare(box, "one")
    head = commit(box.source, "two")
    real_sync = sp.sync
    calls = []

    def sync(source, bare):
        calls.append(1)
        if len(calls) == 1:
            return {"ok": False, "updated": [], "rejected": [], "error": (
                "error: object file %s/objects/%s/%s is empty\n"
                "fatal: pack has 1 unresolved delta" % (bare, oid[:2], oid[2:]))}
        return real_sync(source, bare)
    monkeypatch.setattr(sp, "sync", sync)
    logs = []
    synced = sp.sync_healing(box.source, box.bare_mirror, tmp_path / "q", log=logs.append)
    assert synced["ok"] and synced["healed"]["restored"] == [oid]
    assert sp.rev(box.bare_mirror, "refs/heads/main") == head
    assert any("empty object file" in line for line in logs)


def test_a_failed_sync_fails_the_pass(box, monkeypatch):
    monkeypatch.setattr(sp, "sync", lambda source, bare: {
        "ok": False, "updated": [], "rejected": [],
        "error": "fatal: fetch-pack: invalid index-pack output"})
    monkeypatch.setattr(sp, "run_fanout", lambda script, run=None: {"ok": True, "code": 0})
    monkeypatch.setattr(sp, "read_gateways", lambda path, key: [])
    logs = []
    code = sp.publish(box, run=systemctl_fake(), sleep=lambda s: None, log=logs.append)
    assert code == 1
    assert any(line.startswith("sync FAILED") for line in logs)
    status = json.loads((box.publisher_state / "status.json").read_text())
    assert status["ok"] is False


def test_core_read_urls_lists_every_core_from_the_fleet(box):
    (box.source / "deploy").mkdir()
    (box.source / "deploy" / "fleet.toml").write_text(
        'domain = "next.example.com"\n'
        '[[nodes]]\nname = "n1"\nrole = "core"\n'
        '[[nodes]]\nname = "n2"\n'
        '[[nodes]]\nname = "a1"\nrole = "app"\n'
        '[[runners]]\nname = "r1"\n')
    assert sp.core_read_urls(box.source) == [
        {"name": "core n1", "url": "https://n1.next.example.com/forkmesh/forkmesh.git"},
        {"name": "core n2", "url": "https://n2.next.example.com/forkmesh/forkmesh.git"}]
    assert sp.core_read_urls(box.source / "missing") == []


def test_watch_takes_a_main_only_one_core_holds(box, tmp_path, monkeypatch):
    cores, pushed = _cores_with(box, tmp_path, "runner-merge")
    local = commit(box.source, "desktop-change")
    monkeypatch.setattr(sp, "core_read_urls", lambda source: [
        {"name": "core n1", "url": str(cores)}])
    monkeypatch.setattr(sp, "read_gateways", lambda path, key: [])
    passes = []
    monkeypatch.setattr(sp, "publish", lambda paths, run=None, log=None: passes.append(1) or 0)
    logs = []
    assert sp.watch_cores(box, run=systemctl_fake(), log=logs.append) == 0
    assert passes == [1]
    assert sh("show", "-s", "--format=%P", "main", cwd=box.source).split() == [local, pushed]
    assert any("core n1: took main" in line for line in logs)
    logs.clear()
    assert sp.watch_cores(box, run=systemctl_fake(), log=logs.append) == 0
    assert passes == [1] and logs == []


def test_watch_records_a_core_main_that_conflicts(box, tmp_path, monkeypatch):
    cores, pushed = _cores_with(box, tmp_path, "shared")
    (box.source / "shared").write_text("local\n")
    sh("add", "shared", cwd=box.source)
    sh("commit", "-q", "-m", "local shared", cwd=box.source)
    monkeypatch.setattr(sp, "core_read_urls", lambda source: [
        {"name": "core n1", "url": str(cores)}])
    monkeypatch.setattr(sp, "read_gateways", lambda path, key: [])
    passes = []
    monkeypatch.setattr(sp, "publish", lambda paths, run=None, log=None: passes.append(1) or 0)
    logs = []
    sp.watch_cores(box, run=systemctl_fake(), log=logs.append)
    assert any("core n1 DIVERGED" in line for line in logs)
    assert any("merge it into main by hand" in line for line in logs)
    seen = json.loads((box.publisher_state / "cores-watch.json").read_text())
    assert seen == {"core n1": pushed}
    logs.clear()
    sp.watch_cores(box, run=systemctl_fake(), log=logs.append)
    assert passes == [1] and logs == []


def test_branches_the_source_deleted_are_deleted_on_the_cores(box, tmp_path):
    public_key = write_key(box.identity_key)
    tips = {}
    for name in ("gone", "kept", "redone", "advanced"):
        sh("checkout", "-q", "-b", name, "main", cwd=box.source)
        tips[name] = commit(box.source, name)
    sh("checkout", "-q", "main", cwd=box.source)
    sp.sync(box.source, box.bare_mirror)
    served, target = served_copy(box, tmp_path)
    clone = tmp_path / "advance"
    sh("clone", "-q", str(target), str(clone))
    sh("checkout", "-q", "advanced", cwd=clone)
    ahead = commit(clone, "advanced-later")
    sh("push", "-q", "origin", "advanced", cwd=clone)
    for name in ("gone", "redone", "advanced"):
        sh("update-ref", sp.TOMBSTONE_PREFIX + name, tips[name], cwd=box.source)
        sh("branch", "-D", name, cwd=box.source)
    sh("update-ref", sp.TOMBSTONE_PREFIX + "main", "main", cwd=box.source)
    sh("branch", "redone", "main", cwd=box.source)
    gateway = {"name": "cores", "url": None, "key": box.identity_key}
    app = FakeApp(served, public_key)
    logs = []
    try:
        gateway["url"] = app.origin + "/forkmesh/forkmesh.git"
        result = sp.propagate_branch_deletions(
            box.source, gateway, heads=sp.cores_branches(str(target)),
            log=logs.append)
        again = sp.propagate_branch_deletions(
            box.source, gateway, heads=sp.cores_branches(str(target)),
            log=logs.append)
    finally:
        app.close()
    assert result["pending"] == 1 and result["results"][0]["ok"], result
    assert app.branch_deletes == [({"owner": "forkmesh", "repo": "forkmesh",
                                    "branch": "gone", "expect": tips["gone"]},
                                   True)]
    assert again == {"ran": True, "pending": 0, "results": []}
    remaining = sp.cores_branches(str(target))
    assert "gone" not in remaining
    assert remaining["kept"] == tips["kept"]
    assert remaining["redone"] == tips["redone"]
    assert remaining["advanced"] == ahead
    assert remaining["main"] == sh("rev-parse", "main", cwd=box.source)
    assert any("deleted 1 branch(es)" in line for line in logs)


def test_a_refused_branch_delete_is_logged_and_retried_next_pass(box, tmp_path):
    public_key = write_key(box.identity_key)
    sh("checkout", "-q", "-b", "gone", "main", cwd=box.source)
    tip = commit(box.source, "gone")
    sh("checkout", "-q", "main", cwd=box.source)
    sh("update-ref", sp.TOMBSTONE_PREFIX + "gone", tip, cwd=box.source)
    sh("branch", "-D", "gone", cwd=box.source)
    write_key(box.identity_key.with_name("other.pem"))
    gateway = {"name": "cores", "url": None,
               "key": box.identity_key.with_name("other.pem")}
    app = FakeApp(tmp_path, public_key)
    logs = []
    try:
        gateway["url"] = app.origin + "/forkmesh/forkmesh.git"
        result = sp.propagate_branch_deletions(
            box.source, gateway, heads={"gone": tip, "main": "0" * 40},
            log=logs.append)
    finally:
        app.close()
    assert result["pending"] == 1
    assert result["results"][0]["status"] == 401
    assert not result["results"][0]["ok"]
    assert any("deleting gone refused (HTTP 401 unauthorized)" in line
               for line in logs)


def test_a_processing_branch_delete_is_resumed_until_it_finishes(box, tmp_path):
    public_key = write_key(box.identity_key)
    sh("checkout", "-q", "-b", "gone", "main", cwd=box.source)
    tip = commit(box.source, "gone")
    sh("checkout", "-q", "main", cwd=box.source)
    sp.sync(box.source, box.bare_mirror)
    served, target = served_copy(box, tmp_path)
    sh("update-ref", sp.TOMBSTONE_PREFIX + "gone", tip, cwd=box.source)
    sh("branch", "-D", "gone", cwd=box.source)
    app = FakeApp(served, public_key)
    app.branch_delete_processing = True
    naps = []
    try:
        result = sp.propagate_branch_deletions(
            box.source, {"name": "cores", "key": box.identity_key,
                         "url": app.origin + "/forkmesh/forkmesh.git"},
            heads=sp.cores_branches(str(target)), log=lambda line: None,
            sleep=naps.append)
    finally:
        app.close()
    assert result["results"][0]["ok"], result
    assert naps == [sp.BRANCH_DELETE_POLL_SECONDS]
    assert [data.get("requestId") for data, _ in app.branch_deletes] == [
        None, "r" * 32]
    assert app.branch_deletes[1][0]["node"] == "fmnext-n1"
    assert app.branch_deletes[1][0]["pinBase"] == "main"
    assert all(verified for _, verified in app.branch_deletes)
    assert "gone" not in sp.cores_branches(str(target))


def test_a_branch_one_core_still_holds_is_deleted_again(box, monkeypatch):
    sh("checkout", "-q", "-b", "gone", "main", cwd=box.source)
    tip = commit(box.source, "gone")
    sh("checkout", "-q", "main", cwd=box.source)
    sh("update-ref", sp.TOMBSTONE_PREFIX + "gone", tip, cwd=box.source)
    sh("branch", "-D", "gone", cwd=box.source)
    listings = {"https://n1/x.git": {"main": "1" * 40},
                "https://n2/x.git": {"main": "1" * 40, "gone": tip},
                "https://edge/x.git": {"main": "1" * 40}}
    monkeypatch.setattr(sp, "core_read_urls", lambda source: [
        {"name": "n1", "url": "https://n1/x.git"},
        {"name": "n2", "url": "https://n2/x.git"}])
    monkeypatch.setattr(sp, "cores_branches", lambda url: listings[url])
    heads = sp.every_core_branches(box.source, "https://edge/x.git")
    assert heads["gone"] == {tip}
    assert sp.deleted_branches_on_cores(box.source, heads) == [("gone", tip)]
    listings["https://n2/x.git"] = None
    assert "gone" not in sp.every_core_branches(box.source, "https://edge/x.git")


def test_publish_hands_branch_deletes_to_their_own_unit(box, monkeypatch):
    calls = []
    inline = []
    monkeypatch.setattr(sp, "delete_branches",
                        lambda *args, **kwargs: inline.append(args))
    assert sp.publish(box, run=systemctl_fake(calls=calls),
                      sleep=lambda s: None, log=lambda *_: None) == 0
    assert ["systemctl", "--user", "start", "--no-block",
            sp.BRANCH_DELETE_UNIT] in calls
    assert inline == []
    with sp.StateLock(box.publisher_state / "publish.lock", wait_seconds=0):
        pass


def test_branch_deletes_run_inline_when_their_unit_cannot_start(box, monkeypatch):
    inline = []
    monkeypatch.setattr(sp, "delete_branches",
                        lambda *args, **kwargs: inline.append(args))

    def run(command, **kwargs):
        if command[:3] == ["systemctl", "--user", "start"]:
            return subprocess.CompletedProcess(command, 5, "", "not found")
        return systemctl_fake()(command, **kwargs)
    sp.publish(box, run=run, sleep=lambda s: None, log=lambda *_: None)
    assert len(inline) == 1


def test_a_second_branch_delete_run_leaves_the_first_alone(box, monkeypatch):
    inline = []
    monkeypatch.setattr(sp, "propagate_branch_deletions",
                        lambda *args, **kwargs: inline.append(args))
    monkeypatch.setattr(sp, "read_gateways", lambda *args: [{"name": "cores"}])
    logs = []
    with sp.StateLock(box.publisher_state / "branch-deletes.lock", wait_seconds=0):
        assert sp.delete_branches(box, log=logs.append) == 0
    assert inline == [] and len(logs) == 1
    assert sp.delete_branches(box, log=logs.append) == 0
    assert len(inline) == 1


def github_fake(outcomes, calls):
    def run(command, **kwargs):
        if command[0] == "git" and "push" in command and "https://github.com/" in command[-3]:
            calls.append((command, kwargs["env"]))
            code, stdout, stderr = outcomes.pop(0) if outcomes else (0, "", "")
            return subprocess.CompletedProcess(command, code, stdout, stderr)
        return systemctl_fake()(command, **kwargs)
    return run


def test_github_mirrors_read_only_github_https_lines(box):
    box.fanout_github.parent.mkdir(parents=True, exist_ok=True)
    box.fanout_github.write_text(
        "# mirrors\n"
        "github https://github.com/forkmesh/forkmesh.git\n"
        "plain http://github.com/forkmesh/forkmesh.git\n"
        "other https://gitlab.com/forkmesh/forkmesh.git\n"
        "named https://github.com/forkmesh/other.git ~/token\n")
    mirrors = sp.read_github_mirrors(box.fanout_github)
    assert [item["name"] for item in mirrors] == ["github", "named"]
    assert mirrors[0]["key"] == box.fanout_github.parent / "github-token"
    assert mirrors[1]["key"] == Path(os.path.expanduser("~/token"))


def test_github_push_sends_main_and_tags_with_the_token_out_of_argv(box):
    token = box.home_dir / "github-token"
    token.write_text("ghp_secret\n")
    calls = []
    run = github_fake([(0, "To github\n*\trefs/heads/main:refs/heads/main\t[new branch]\nDone\n",
                        "")], calls)
    result = sp.push_github(box.bare_mirror, {
        "name": "github", "url": "https://github.com/forkmesh/forkmesh.git",
        "key": token}, run=run)
    assert result["ok"] and result["updated"] == ["refs/heads/main"]
    command, env = calls[0]
    assert command[-2:] == list(sp.GITHUB_REFSPECS)
    assert not any(arg.startswith("+") or arg in ("--force", "--mirror") for arg in command)
    assert "ghp_secret" not in " ".join(command)
    assert env["GIT_CONFIG_KEY_0"] == "http.extraHeader"
    assert env["GIT_CONFIG_VALUE_0"] == "Authorization: Basic " + base64.b64encode(
        b"x-access-token:ghp_secret").decode()


def test_github_push_reports_a_rejection_without_the_token(box):
    token = box.home_dir / "github-token"
    token.write_text("ghp_secret")
    run = github_fake([(1, "!\trefs/heads/main:refs/heads/main\t[rejected] (fetch first)\n",
                        "error: failed to push to https://ghp_secret@github.com\n")], [])
    result = sp.push_github(box.bare_mirror, {
        "name": "github", "url": "https://github.com/forkmesh/forkmesh.git",
        "key": token}, run=run)
    assert not result["ok"]
    assert result["rejected"][0]["ref"] == "refs/heads/main"
    assert "ghp_secret" not in result["error"]


def test_github_push_without_a_token_does_not_run_git(box):
    calls = []
    result = sp.push_github(box.bare_mirror, {
        "name": "github", "url": "https://github.com/forkmesh/forkmesh.git",
        "key": box.home_dir / "missing"}, run=github_fake([], calls))
    assert not result["ok"] and result["error"].startswith("token:")
    assert calls == []


def test_publish_retries_a_failed_github_push_on_the_next_pass(box):
    box.fanout_github.parent.mkdir(parents=True, exist_ok=True)
    box.fanout_github.write_text("github https://github.com/forkmesh/forkmesh.git\n")
    (box.fanout_github.parent / "github-token").write_text("ghp_secret")
    calls, logs = [], []
    run = github_fake([(128, "", "fatal: unable to access\n"), (0, "", "")], calls)
    assert sp.publish(box, run=run, sleep=lambda s: None, log=logs.append) == 0
    status = json.loads((box.publisher_state / "status.json").read_text())
    assert status["github"][0]["ok"] is False
    assert any(line.startswith("github github: FAILED") for line in logs)
    sp.publish(box, if_changed=True, run=run, sleep=lambda s: None, log=logs.append)
    assert len(calls) == 2
    status = json.loads((box.publisher_state / "status.json").read_text())
    assert status["github"][0]["ok"] is True
    sp.publish(box, if_changed=True, run=run, sleep=lambda s: None, log=logs.append)
    assert len(calls) == 2
    assert sp.status_report(box, run=run)["github"] == [
        {"name": "github", "ok": True, "rejected": []}]
