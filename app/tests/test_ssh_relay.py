#!/usr/bin/env python3
"""SSH relay mode, end to end with a real git client.

The SSH gateway in relay mode ("httpGit") runs no Git service of its own: it
authorizes each operation through the broker, then relays the session to the
control plane's HTTPS Git routes with the short-lived credential the
authorization minted. These tests drive a stock `git` through the gateway's
forced command exactly as sshd would run it (GIT_SSH_COMMAND stands in for
`ssh`, the same argv and environment the `restrict,command=` key line gives
the gateway), against:

* a fake broker on a Unix socket answering like /api/ssh/authorize in relay
  mode (it mints the real hosted_git credential);
* a fake app front that verifies that credential, applies the contributor
  namespace and branch protection the way the HTTPS push route does, records
  the post-push bookkeeping call, and forwards to the real mirror gateway
  application (tools/mirror_gateway.py), which runs real git-upload-pack and
  git-receive-pack against a bare repository.
"""

import base64
import json
import os
from pathlib import Path
import pwd
import shutil
import socket
import socketserver
import struct
import subprocess
import sys
import threading
from http.server import ThreadingHTTPServer
from urllib.parse import quote

import pytest

ROOT = Path(__file__).resolve().parents[1]
PROJECT_ROOT = ROOT.parent
for path in (ROOT / "src", PROJECT_ROOT / "tools", ROOT / "tests"):
    if str(path) not in sys.path:
        sys.path.insert(0, str(path))

import hosted_git  # noqa: E402
import ssh_gateway  # noqa: E402
import mirror_gateway as gateway  # noqa: E402
from test_mirror_gateway import (  # noqa: E402
    _PushGitHandler,
    make_push_application,
    push_capability_headers,
    run,
)


SECRET = b"relay-test-secret"
OWNER = "mobile-owner"
REPO = "app"
KEYS = {
    "sk_" + "a" * 24: "alice",   # owns the repository
    "sk_" + "b" * 24: "bob",     # contributor (HOSTED_GIT_CONTRIBUTOR_PUSH)
    "sk_" + "c" * 24: "carol",   # may read, may not push
}
NOW_MS = 1_783_000_000_000


class _Env:
    pass


@pytest.fixture(autouse=True)
def _secret(monkeypatch):
    monkeypatch.setattr(hosted_git, "_account_session_secret",
                        lambda _env: SECRET, raising=False)


def _account_for(username, password, owner, repo, access):
    match = hosted_git.SSH_GIT_PASSWORD_RE.fullmatch(password or "")
    if not match:
        return ""
    key_id, expires, mac = match.groups()
    expected = hosted_git.ssh_git_mac(
        _Env(), username, owner, repo, key_id, access, int(expires))
    if expected != mac or KEYS.get(key_id) != username:
        return ""
    return username


# ---------------------------------------------------------------------------
# Fake broker (the control plane's authorize answer in relay mode)
# ---------------------------------------------------------------------------


class _BrokerHandler(socketserver.BaseRequestHandler):
    calls = []

    def handle(self):
        size = struct.unpack("!I", self._exact(4))[0]
        request = json.loads(self._exact(size))
        type(self).calls.append(request)
        account = KEYS.get(request.get("keyId"), "")
        owner = request.get("owner") or OWNER  # short address
        operation = request.get("operation")
        allowed = (
            account
            and request.get("action") == "authorize"
            and request.get("repository", "").lower() == REPO
            and (operation == "git-upload-pack" or account in ("alice", "bob"))
        )
        if not allowed:
            reply = {"authorized": False}
        else:
            access = "push" if operation == "git-receive-pack" else "read"
            expires = NOW_MS + hosted_git.SSH_GIT_TTL_MS
            reply = {
                "authorized": True,
                "keyId": request["keyId"],
                "operation": operation,
                "owner": owner,
                "repository": REPO,
                "repositoryRelativePath": owner + "/" + REPO + ".git",
                "httpCredential": {
                    "username": account,
                    "password": hosted_git.ssh_git_password(
                        _Env(), account, owner, REPO, request["keyId"],
                        access, expires),
                    "expiresAt": expires,
                },
            }
        raw = json.dumps(reply).encode()
        self.request.sendall(struct.pack("!I", len(raw)) + raw)

    def _exact(self, size):
        data = b""
        while len(data) < size:
            chunk = self.request.recv(size - len(data))
            if not chunk:
                raise OSError("short read")
            data += chunk
        return data


# ---------------------------------------------------------------------------
# Fake app front (the HTTPS Git routes, in front of the real node gateway)
# ---------------------------------------------------------------------------


class _Front(_PushGitHandler):
    protect = ""
    # A CDN proxy's own answer to a push (e.g. 413 for its body cap), sent
    # before the body is read, the way a proxy refuses an upload.
    proxy_status = 0
    contributors = ("bob",)
    bookkeeping = []
    received_packs = []
    requests = []

    def _account(self, access):
        header = self.headers.get("Authorization", "")
        if not header.startswith("Basic "):
            return ""
        username, _sep, password = base64.b64decode(
            header[6:]).decode().partition(":")
        parts = self.path.split("?")[0].split("/")
        repo = parts[2][:-4] if parts[2].endswith(".git") else parts[2]
        return _account_for(username, password, parts[1], repo, access)

    def _deny(self, status=401):
        self._reply(status, "application/json", b'{"error":"denied"}')

    def _body(self):
        if self.headers.get("Transfer-Encoding", "").lower() == "chunked":
            body = b""
            while True:
                size = int(self.rfile.readline().strip(), 16)
                if size == 0:
                    self.rfile.readline()
                    return body
                body += self.rfile.read(size)
                self.rfile.readline()
        return self.rfile.read(int(self.headers.get("Content-Length") or 0))

    def _stream(self, response, protocol=""):
        if response.stream is None:
            self._reply(response.status, response.content_type, response.body)
            return
        completed = subprocess.run(
            response.stream.command, input=response.stream.input_bytes,
            capture_output=True, env=gateway._git_environment(protocol),
            timeout=60, check=False)
        self._reply(response.status if completed.returncode == 0 else 500,
                    response.stream.content_type, completed.stdout)

    def do_GET(self):
        path, _sep, query = self.path.partition("?")
        parts = path.split("/")
        repo = parts[2][:-4]
        receive = "service=git-receive-pack" in query
        account = self._account("push" if receive else "read")
        type(self).requests.append(("GET", path, query, account))
        if not account:
            return self._deny()
        protocol = self.headers.get("Git-Protocol", "")
        if receive:
            target = ("/v1/repositories/%s/%s/git-receive-pack"
                      "?service=git-receive-pack" % (parts[1], repo))
        else:
            target = ("/v1/repositories/%s/%s/git-info-refs"
                      "?service=git-upload-pack" % (parts[1], repo))
        headers = push_capability_headers()
        if protocol:
            headers["Git-Protocol"] = protocol
        self._stream(self.application.dispatch("GET", target, headers, b""),
                     protocol)

    def do_POST(self):
        path = self.path.split("?")[0]
        parts = path.split("/")
        repo = parts[2][:-4]
        receive = parts[3] == "git-receive-pack"
        if receive and type(self).proxy_status:
            self.send_response(type(self).proxy_status)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", "26")
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(b"<html>Entity Too Large</html>"[:26])
            self.wfile.flush()
            self._body()
            self.close_connection = True
            return
        account = self._account("push" if receive else "read")
        body = self._body()
        type(self).requests.append(("POST", path, "", account))
        if not account:
            return self._deny()
        target = "/v1/repositories/%s/%s/%s" % (parts[1], repo, parts[3])
        query = []
        if receive:
            if account in type(self).contributors:
                query.append("pushPrefix=" + quote(
                    "refs/heads/pr/%s/" % account, safe=""))
            if type(self).protect:
                query.append("protect=" + quote(type(self).protect, safe=""))
            index = body.find(b"PACK")
            if index >= 0:
                type(self).received_packs.append(
                    struct.unpack("!I", body[index + 8:index + 12])[0])
        if query:
            target += "?" + "&".join(query)
        headers = push_capability_headers(body)
        headers["Content-Type"] = self.headers.get("Content-Type", "")
        protocol = self.headers.get("Git-Protocol", "")
        if protocol:
            headers["Git-Protocol"] = protocol
        response = self.application.dispatch("POST", target, headers, body)
        if receive and response.status == 200:
            # What _https_mirror_push_send does after an accepted push:
            # record_hosted_push_state + record_accepted_push.
            updates = response.headers.get(gateway.REF_UPDATES_HEADER, "")
            type(self).bookkeeping.append((account, updates))
        self._stream(response, protocol)


# ---------------------------------------------------------------------------
# Harness
# ---------------------------------------------------------------------------


def _user():
    current = pwd.getpwuid(os.geteuid())
    if current.pw_uid == 0:
        pytest.skip("the relay test runs as an unprivileged account")
    return current.pw_name


@pytest.fixture
def relay(tmp_path, monkeypatch):
    app, root, _config_path, _logs = make_push_application(
        tmp_path, owners=(OWNER,))
    monkeypatch.setattr(
        gateway, "REFRESH_PENDING_PATH", tmp_path / "missing" / "pending")
    front = type("Front", (_Front,), {
        "application": app, "protect": "", "proxy_status": 0,
        "bookkeeping": [],
        "received_packs": [], "requests": []})
    server = ThreadingHTTPServer(("127.0.0.1", 0), front)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    port = server.server_address[1]

    socket_path = tmp_path / "authorize.sock"
    broker_handler = type("Broker", (_BrokerHandler,), {"calls": []})
    broker = socketserver.ThreadingUnixStreamServer(
        str(socket_path), broker_handler)
    threading.Thread(target=broker.serve_forever, daemon=True).start()

    capacity = tmp_path / "capacity"
    capacity.mkdir(mode=0o700)
    true_program = str(Path(shutil.which("true") or "/usr/bin/true").resolve())
    config = tmp_path / "gateway.json"
    config.write_text(json.dumps({
        "schemaVersion": 1,
        "authorizationSocket": str(socket_path),
        "authorizationBrokerUser": _user(),
        "gatewayExecutable": true_program,
        "httpGit": {
            "origin": "http://127.0.0.1:%d" % port,
            "connectAddress": "127.0.0.1:%d" % port,
        },
        "limits": {**ssh_gateway.DEFAULT_LIMITS,
                   "capacityDirectory": str(capacity)},
    }), encoding="utf-8")
    config.chmod(0o644)

    # The stand-in for `ssh`: sshd would match the key, run the forced
    # command with SSH_ORIGINAL_COMMAND and the accepted GIT_PROTOCOL, and a
    # clean environment (server/packaging/ssh/ssh-gateway-entrypoint).
    fake_ssh = tmp_path / "fake-ssh"
    fake_ssh.write_text(
        "#!%s\n"
        "import os, sys\n"
        "key = os.environ['RELAY_KEY_ID']\n"
        "env = {'PATH': '/usr/bin:/bin', 'LANG': 'C', 'LC_ALL': 'C',\n"
        "       'SSH_ORIGINAL_COMMAND': sys.argv[-1],\n"
        "       'GIT_PROTOCOL': os.environ.get('GIT_PROTOCOL', '')}\n"
        "os.execve(sys.executable, [sys.executable, '-I', %r,\n"
        "          '--config', %r, 'serve', '--key-id', key], env)\n"
        % (sys.executable, str(PROJECT_ROOT / "tools" / "ssh_gateway.py"),
           str(config)),
        encoding="utf-8")
    fake_ssh.chmod(0o755)

    # The owner's repository, created by a first (HTTPS-path) push.
    seed = tmp_path / "seed"
    run(["git", "init", "-b", "main", str(seed)], tmp_path)
    run(["git", "config", "user.email", "o@example.test"], seed)
    run(["git", "config", "user.name", "Owner"], seed)
    for index in range(40):
        (seed / ("history-%02d.txt" % index)).write_text(
            "history %d\n" % index * 50, encoding="utf-8")
        run(["git", "add", "."], seed)
        run(["git", "commit", "-q", "-m", "history %d" % index], seed)
    handler = type("Seed", (_PushGitHandler,), {"application": app})
    seed_server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
    threading.Thread(target=seed_server.serve_forever, daemon=True).start()
    run(["git", "push", "http://127.0.0.1:%d/%s/%s" % (
        seed_server.server_address[1], OWNER, REPO), "main"], seed)
    seed_server.shutdown()
    seed_server.server_close()

    def git(key, *arguments, cwd=tmp_path, check=False):
        environment = {
            "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
            "HOME": str(tmp_path),
            "GIT_CONFIG_NOSYSTEM": "1",
            "GIT_SSH_COMMAND": str(fake_ssh),
            "GIT_SSH_VARIANT": "ssh",
            "RELAY_KEY_ID": key,
            "GIT_AUTHOR_NAME": "Dev", "GIT_AUTHOR_EMAIL": "d@example.test",
            "GIT_COMMITTER_NAME": "Dev",
            "GIT_COMMITTER_EMAIL": "d@example.test",
        }
        result = subprocess.run(
            ["git", *arguments], cwd=cwd, capture_output=True, text=True,
            timeout=90, env=environment, check=False)
        if check:
            assert result.returncode == 0, result.stderr
        return result

    yield {
        "git": git,
        "front": front,
        "broker": broker_handler,
        "bare": root / OWNER / (REPO + ".git"),
        "tmp": tmp_path,
    }
    server.shutdown()
    server.server_close()
    broker.shutdown()
    broker.server_close()
    app.close()


ALICE, BOB, CAROL = list(KEYS)
URL = "git@ssh.example.test:%s/%s.git" % (OWNER, REPO)


def _rev(git_dir, ref):
    return subprocess.run(
        ["git", "--git-dir", str(git_dir), "rev-parse", "--verify", "-q", ref],
        capture_output=True, text=True, check=False).stdout.strip()


def _commit(git, work, name):
    (work / name).write_text(name + "\n", encoding="utf-8")
    git(ALICE, "add", ".", cwd=work, check=True)
    git(ALICE, "commit", "-q", "-m", name, cwd=work, check=True)
    return git(ALICE, "rev-parse", "HEAD", cwd=work, check=True).stdout.strip()


def test_clone_over_ssh_relays_protocol_v2_with_a_read_credential(relay):
    git = relay["git"]
    work = relay["tmp"] / "clone"
    git(CAROL, "clone", "-q", URL, str(work), check=True)
    assert git(CAROL, "rev-parse", "HEAD", cwd=work, check=True).stdout.strip() \
        == _rev(relay["bare"], "refs/heads/main")
    calls = relay["broker"].calls
    assert calls[0]["operation"] == "git-upload-pack"
    assert calls[0]["owner"] == OWNER
    # Every relayed request carried the read credential, never a push one.
    assert relay["front"].requests
    assert all(account == "carol" for *_rest, account in relay["front"].requests)
    # A short address (git@host:Repository.git) resolves on the control plane.
    short = relay["tmp"] / "short"
    git(CAROL, "clone", "-q", "git@ssh.example.test:%s.git" % REPO,
        str(short), check=True)
    assert relay["broker"].calls[-1]["owner"] == ""


def test_owner_push_sends_only_new_objects_and_is_bookkept(relay):
    git, front = relay["git"], relay["front"]
    work = relay["tmp"] / "owner"
    git(ALICE, "clone", "-q", URL, str(work), check=True)
    new = _commit(git, work, "owner-change.txt")
    pushed = git(ALICE, "push", "origin", "main", cwd=work)
    assert pushed.returncode == 0, pushed.stderr
    assert _rev(relay["bare"], "refs/heads/main") == new
    # The full advertisement reached the client, so the pack held only the
    # new commit, tree and blob, not the 40-commit history.
    assert front.received_packs == [3]
    assert front.bookkeeping and front.bookkeeping[-1][0] == "alice"
    assert "refs/heads/main" in front.bookkeeping[-1][1]
    # A deletion sends commands and no pack; the relay must not wait for
    # one. This node refuses deletions, and git prints its per-ref report.
    git(ALICE, "push", "origin", "main:refs/heads/topic", cwd=work, check=True)
    assert _rev(relay["bare"], "refs/heads/topic") == new
    packs = list(front.received_packs)
    deleted = git(ALICE, "push", "origin", ":topic", cwd=work)
    assert "deletion prohibited" in deleted.stderr
    assert front.received_packs == packs


def test_owner_push_into_a_protected_branch_gets_the_https_rule(relay):
    git, front = relay["git"], relay["front"]
    work = relay["tmp"] / "protected"
    git(ALICE, "clone", "-q", URL, str(work), check=True)
    before = _rev(relay["bare"], "refs/heads/main")
    _commit(git, work, "protected.txt")
    front.protect = "refs/heads/main"
    refused = git(ALICE, "push", "origin", "main", cwd=work)
    assert refused.returncode != 0
    assert _rev(relay["bare"], "refs/heads/main") == before
    # Another branch of the same repository is not protected.
    git(ALICE, "push", "origin", "HEAD:refs/heads/feature", cwd=work,
        check=True)
    assert _rev(relay["bare"], "refs/heads/feature")


def test_contributor_pushes_only_into_its_pr_namespace(relay):
    git, front = relay["git"], relay["front"]
    work = relay["tmp"] / "contributor"
    git(BOB, "clone", "-q", URL, str(work), check=True)
    main = _rev(relay["bare"], "refs/heads/main")
    change = _commit(git, work, "fix.txt")
    for refused in ("main", "HEAD:refs/heads/topic",
                    "HEAD:refs/heads/pr/alice/fix"):
        result = git(BOB, "push", "--porcelain", "origin", refused, cwd=work)
        assert result.returncode != 0
        assert "pr/bob/" in result.stdout + result.stderr
    assert _rev(relay["bare"], "refs/heads/main") == main
    accepted = git(BOB, "push", "origin", "HEAD:refs/heads/pr/bob/fix",
                   cwd=work)
    assert accepted.returncode == 0, accepted.stderr
    assert _rev(relay["bare"], "refs/heads/pr/bob/fix") == change
    assert front.received_packs[-1] == 3
    assert front.bookkeeping[-1][0] == "bob"


def test_a_reader_cannot_push_and_a_large_push_streams(relay):
    git = relay["git"]
    work = relay["tmp"] / "reader"
    git(CAROL, "clone", "-q", URL, str(work), check=True)
    _commit(git, work, "nope.txt")
    refused = git(CAROL, "push", "origin", "main", cwd=work)
    assert refused.returncode != 0
    assert "repository access denied" in refused.stderr

    # Over 1 MiB of incompressible data: the relay streams it chunked.
    big = relay["tmp"] / "owner-big"
    git(ALICE, "clone", "-q", URL, str(big), check=True)
    (big / "blob.bin").write_bytes(os.urandom(3 * 1024 * 1024))
    git(ALICE, "add", ".", cwd=big, check=True)
    git(ALICE, "commit", "-q", "-m", "big", cwd=big, check=True)
    pushed = git(ALICE, "push", "origin", "HEAD:refs/heads/big", cwd=big)
    assert pushed.returncode == 0, pushed.stderr
    assert _rev(relay["bare"], "refs/heads/big")


def test_a_shell_request_gets_a_greeting_not_a_shell(relay, monkeypatch):
    config = ssh_gateway.Config(relay["tmp"] / "gateway.json")
    monkeypatch.delenv("SSH_ORIGINAL_COMMAND", raising=False)
    assert ssh_gateway._serve(config, ALICE) == 1
    monkeypatch.setenv("SSH_ORIGINAL_COMMAND", "sh -c id")
    with pytest.raises(ssh_gateway.GatewayError):
        ssh_gateway._serve(config, ALICE)


def test_pack_end_is_found_by_parsing_not_by_eof(tmp_path):
    work = tmp_path / "w"
    run(["git", "init", "-q", "-b", "main", str(work)], tmp_path)
    run(["git", "config", "user.email", "x@example.test"], work)
    run(["git", "config", "user.name", "X"], work)
    for name in ("a", "b"):
        (work / name).write_text(name * 4000, encoding="utf-8")
        run(["git", "add", "."], work)
        run(["git", "commit", "-q", "-m", name], work)
    pack = subprocess.run(
        ["git", "pack-objects", "--stdout", "--revs", "--delta-base-offset"],
        cwd=work, input=b"HEAD\n", capture_output=True, check=True).stdout
    finder = ssh_gateway.PackEnd(20)
    for index in range(0, len(pack), 7):
        assert not finder.done
        finder.feed(pack[index:index + 7])
    assert finder.done
    with pytest.raises(ssh_gateway.RelayError):
        finder.feed(b"x")
    truncated = ssh_gateway.PackEnd(20)
    truncated.feed(pack[:-1])
    assert not truncated.done


def test_relay_configuration_is_strict(tmp_path):
    true_program = str(Path(shutil.which("true") or "/usr/bin/true").resolve())
    base = {
        "schemaVersion": 1,
        "authorizationSocket": str(tmp_path / "a.sock"),
        "authorizationBrokerUser": _user(),
        "gatewayExecutable": true_program,
    }

    def load(**fields):
        path = tmp_path / "c.json"
        path.write_text(json.dumps({**base, **fields}), encoding="utf-8")
        path.chmod(0o644)
        return ssh_gateway.Config(path)

    assert load(httpGit={"origin": "https://git.example.test",
                         "connectAddress": "127.0.0.1:443"}).http_git.host \
        == "git.example.test"
    for bad in (
        {"origin": "http://git.example.test"},
        {"origin": "https://git.example.test/path"},
        {"origin": "https://git.example.test", "connectAddress": "10.0.0.1:443"},
        {"origin": "https://git.example.test", "extra": 1},
    ):
        with pytest.raises(ssh_gateway.GatewayError):
            load(httpGit=bad)
    # Relay mode has no local repositories to name.
    with pytest.raises(ssh_gateway.GatewayError):
        load(httpGit={"origin": "https://git.example.test"}, repositories=[])


@pytest.mark.parametrize("command, expected", [
    ("git-upload-pack 'acme/widget.git'", ("git-upload-pack", "acme", "widget")),
    ("git-receive-pack '/acme/widget.git'",
     ("git-receive-pack", "acme", "widget")),
    ("git-upload-pack 'Widget.git'", ("git-upload-pack", "", "Widget")),
    ("git-upload-pack 'Docs'", ("git-upload-pack", "", "Docs")),
])
def test_relay_accepts_owner_and_short_addresses(command, expected):
    assert ssh_gateway._parse_relay_command(command) == expected


@pytest.mark.parametrize("command", [
    "git-upload-pack 'a/b/c.git'", "git-upload-pack '..'",
    "git-upload-pack 'a b'", "git-upload-pack a; id", "scp -t x",
])
def test_relay_refuses_other_paths_and_commands(command):
    with pytest.raises(ssh_gateway.GatewayError):
        ssh_gateway._parse_relay_command(command)


# ---------------------------------------------------------------------------
# Control plane: the relay credential, the authorization, the push route
# ---------------------------------------------------------------------------

import asyncio  # noqa: E402

import test_contributor_push as contributor  # noqa: E402
import test_hosted_repositories as hosted  # noqa: E402

KEY_ID = "sk_" + "k" * 24


def _key_world(*, key_account="bob", revoked=False, **kwargs):
    module, recorder, env = contributor._contributor_world(**kwargs)

    async def d1_first_primary(_env, sql, *args):
        assert "FROM account_ssh_keys" in sql and "revoked_at=0" in sql
        if revoked or args[0] != KEY_ID:
            return None
        return {"account_bi": "bi:" + key_account}

    module.d1_first_primary = d1_first_primary
    return module, recorder, env


def _relay_header(module, env, account="bob", owner="acme", repo="platform",
                  access="push", expires=None, key_id=KEY_ID):
    expires = hosted.NOW + 60_000 if expires is None else expires
    return hosted._basic(account, module.ssh_git_password(
        env, account, owner, repo, key_id, access, expires))


def test_a_relay_credential_is_a_contributor_credential_bound_to_its_key():
    module, recorder, env = _key_world()
    header = _relay_header(module, env)
    assert asyncio.run(module.verify_hosted_push_access(
        env, "acme", "platform", header, "POST")) == (
            "bob", "refs/heads/pr/bob/")
    assert recorder.audits[-1][5]["credential"] == "ssh-key"
    # The same account with full write access gets no prefix.
    module, _recorder, env = _key_world(org_write=True)
    assert asyncio.run(module.verify_hosted_push_access(
        env, "acme", "platform", _relay_header(module, env), "POST")) == (
            "bob", "")


@pytest.mark.parametrize("change", [
    {"revoked": True},            # key revoked since the authorization
    {"key_account": "mallory"},   # key moved to another account
])
def test_a_revoked_or_reassigned_key_stops_the_relay(change):
    module, recorder, env = _key_world(org_write=True, **change)
    assert asyncio.run(module.verify_hosted_push_access(
        env, "acme", "platform", _relay_header(module, env), "POST")) == (
            "", "")
    assert recorder.audits[-1][4] == "denied"


def test_a_relay_credential_is_bound_to_access_repo_and_lifetime():
    module, _recorder, env = _key_world(org_write=True)
    read = _relay_header(module, env, access="read")
    assert asyncio.run(module.verify_hosted_push_access(
        env, "acme", "platform", read, "POST")) == ("", "")
    other = _relay_header(module, env, repo="other")
    assert asyncio.run(module.verify_hosted_push_access(
        env, "acme", "platform", other, "POST")) == ("", "")
    expired = _relay_header(module, env, expires=hosted.NOW - 1)
    assert asyncio.run(module.verify_hosted_push_access(
        env, "acme", "platform", expired, "POST")) == ("", "")
    too_long = _relay_header(
        module, env, expires=hosted.NOW + module.SSH_GIT_TTL_MS + 120_000)
    assert asyncio.run(module.verify_hosted_push_access(
        env, "acme", "platform", too_long, "POST")) == ("", "")
    # Minted for an organization alias route, verified on the node route.
    aliased = _relay_header(module, env, owner="acme-org")
    assert asyncio.run(module.verify_hosted_push_access(
        env, "acme", "platform", aliased, "POST", alias="acme-org")) == (
            "bob", "")
    # A read credential reads; it is the same check as a session's.
    module._hosted_read_allowed = lambda *_args: _true()
    assert asyncio.run(module.verify_hosted_push_credential(
        env, "acme", "platform", read, "GET", access="read")) == "bob"


async def _true():
    return True


def _authorize_handler(**kwargs):
    import test_ssh_git_auth as auth
    calls = {"contributor": []}

    async def contributor_push_allowed(_env, owner, repo, account, alias=""):
        calls["contributor"].append((owner, repo, account, alias))
        return kwargs.get("contributor", False)

    async def short_setting(_env):
        return {"short": True, "owner": "alice-node", "canonical": True}

    class _Protected:
        @staticmethod
        async def is_protected(*_args):
            return True

    handler, audits = auth._authorization_handler(
        owns=kwargs.get("owns", False), org_write=kwargs.get("org_write", False),
        authoritative_private=False, visibility="public")
    handler.__globals__.update({
        "_ssh_gateway_bridge": lambda _env: kwargs.get("bridge", True),
        "contributor_push_allowed": contributor_push_allowed,
        "ssh_git_password": lambda _env, account, owner, repo, key, access,
        expires: "fms1.%s.%d.%s" % (key, expires, access[0] * 64),
        "_repo_short_setting_row": short_setting,
        "_branch_protection": _Protected,
    })
    return handler, audits, calls


def _authorize(handler, owner="alice-node", operation="git-receive-pack"):
    import test_ssh_git_auth as auth
    payload = {
        "action": "authorize", "keyId": "sk_" + "a" * 24,
        "requestId": "r" * 24, "owner": owner, "repository": "widget",
        "operation": operation,
    }
    return asyncio.run(handler(None, auth._Request(payload)))


def test_relay_authorization_mints_a_credential_and_defers_protection():
    handler, _audits, _calls = _authorize_handler(owns=True)
    response = _authorize(handler)
    assert response["status"] == 200
    credential = response["payload"]["httpCredential"]
    assert credential["username"] == "alice"
    assert credential["password"].endswith("p" * 64)
    assert credential["expiresAt"] == 1000 + 30 * 60 * 1000
    # Without relay mode a protected repository still refuses SSH pushes
    # outright, and no credential is minted.
    handler, _audits, _calls = _authorize_handler(owns=True, bridge=False)
    assert _authorize(handler)["status"] == 403
    handler, _audits, _calls = _authorize_handler(bridge=False)
    read = _authorize(handler, operation="git-upload-pack")
    assert read["status"] == 200 and "httpCredential" not in read["payload"]


def test_relay_authorization_admits_contributors_and_short_addresses():
    handler, _audits, calls = _authorize_handler(contributor=True)
    response = _authorize(handler)
    assert response["status"] == 200
    assert calls["contributor"] == [("alice-node", "widget", "alice", "")]
    handler, audits, _calls = _authorize_handler(contributor=False)
    assert _authorize(handler)["status"] == 403
    assert audits[-1][2] == "denied"
    # git@host:widget.git: the instance's short-address owner.
    handler, _audits, _calls = _authorize_handler()
    short = _authorize(handler, owner="", operation="git-upload-pack")
    assert short["status"] == 200
    assert short["payload"]["owner"] == "alice-node"
    assert short["payload"]["httpCredential"]["password"].endswith("r" * 64)


def test_a_relayed_push_takes_the_https_bookkeeping_path():
    import test_git_push_route as route
    namespace, _messages, fetches = route._proxy_harness(
        candidates=[route._endpoint()], upstreams=[route._Upstream()],
        basic_account="bob", basic_prefix="refs/heads/pr/bob/")
    request = route._Request(
        "https://forkmesh.test/alice/app/git-receive-pack",
        headers=route._Headers({"authorization": hosted._basic(
            "bob", "fms1." + KEY_ID + ".1783000060000." + "0" * 64)}),
        body=b"pack")
    response = route._run_proxy(namespace, request, is_info=False)
    assert response["init"]["status"] == 200
    assert "pushPrefix=" in fetches[0][0]
    assert namespace["_pinned_states"] == [("alice", "app", "write-node")]
    assert namespace["_recorded_pushes"][0][3] == "bob"


# ---------------------------------------------------------------------------
# Configuration values, settings page, installer
# ---------------------------------------------------------------------------


def test_the_wildcard_allowlist_covers_every_repository_and_only_alone():
    import ssh_keys
    everything = ssh_keys.parse_gateway_repository_allowlist("*")
    assert ssh_keys.gateway_bridges_all(everything)
    assert ssh_keys.gateway_repository_access(everything, "anyone", "fork") \
        == "read-write"
    assert ssh_keys.parse_gateway_repository_allowlist("*,a/b=rw") == {}
    assert not ssh_keys.gateway_bridges_all(
        ssh_keys.parse_gateway_repository_allowlist("a/b=rw"))
    assert ssh_keys.scp_repository_url("ssh.example.test", 22, "acme", "Docs") \
        == "git@ssh.example.test:acme/Docs.git"
    assert ssh_keys.scp_repository_url("ssh.example.test", 2222, "acme", "x") \
        == ""


def test_settings_page_shows_ssh_steps_and_keeps_https_fallback():
    settings = (ROOT / "public" / "dashboard" / "partials" / "views"
                / "settings.html").read_text(encoding="utf-8")
    account = (ROOT / "public" / "dashboard" / "js" / "04-account.js"
               ).read_text(encoding="utf-8")
    repo = (ROOT / "public" / "dashboard" / "js"
            / "08-repo-detail-network.js").read_text(encoding="utf-8")
    assert "data-ssh-use-panel" in settings
    assert settings.index("data-ssh-use-panel") < settings.index(
        "data-git-push-panel")
    assert "renderSshUsePanel(data)" in account
    assert "gatewayExampleUrl" in account and "ssh-keygen -t ed25519" in account
    assert "Push code over HTTPS" in settings
    assert "git@${scpMatch[1]}:${scpMatch[2]}" in repo
    routes = (ROOT / "src" / "ssh_gateway_routes.py").read_text(
        encoding="utf-8")
    assert '"gatewayExampleUrl"' in routes


def test_instance_updates_keep_the_ssh_settings_and_refresh_the_relay():
    installer = (PROJECT_ROOT / "www" / "public" / "install-instance.sh"
                 ).read_text(encoding="utf-8")
    keep = next(line for line in installer.splitlines()
                if "HOSTED_GIT_CONTRIBUTOR_PUSH|" in line)
    for key in ("SSH_GATEWAY_HOST", "SSH_GATEWAY_PORT", "SSH_GATEWAY_TOKEN",
                "SSH_GATEWAY_REPOSITORIES", "SSH_GATEWAY_NODE_HOSTS"):
        assert key in keep
    assert "refresh_ssh_relay" in installer
    relay = (PROJECT_ROOT / "tools" / "install_ssh_relay.sh").read_text(
        encoding="utf-8")
    assert "sshd -t" in relay and "--uninstall" in relay
    assert "SSH_GATEWAY_REPOSITORIES=*" in relay
    dropin = (PROJECT_ROOT / "server" / "packaging" / "ssh"
              / "60-forkmesh-git-relay.conf").read_text(encoding="utf-8")
    assert dropin.strip().endswith("Match all")
    assert "Match User git" in dropin and "PermitRootLogin" not in dropin
    assert subprocess.run(["bash", "-n", str(PROJECT_ROOT / "tools"
                                             / "install_ssh_relay.sh")]
                          ).returncode == 0



# ---------------------------------------------------------------------------
# Remote front: DNS and TLS to the public origin, CDN limits, token rotation
# ---------------------------------------------------------------------------


FRONT_NAME = "git.example.test"


def _tls_files(directory):
    """A throwaway CA and a server certificate for FRONT_NAME."""
    directory.mkdir()

    def openssl(*arguments):
        subprocess.run(["openssl", *arguments], cwd=directory, check=True,
                       capture_output=True)

    openssl("req", "-x509", "-newkey", "ec", "-pkeyopt",
            "ec_paramgen_curve:prime256v1", "-nodes", "-days", "2",
            "-subj", "/CN=Relay Test CA", "-keyout", "ca.key", "-out", "ca.pem",
            "-addext", "basicConstraints=critical,CA:TRUE",
            "-addext", "keyUsage=critical,keyCertSign,cRLSign")
    openssl("req", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1",
            "-nodes", "-subj", "/CN=" + FRONT_NAME, "-keyout", "server.key",
            "-out", "server.csr")
    (directory / "server.ext").write_text(
        "subjectAltName=DNS:%s\n"
        "basicConstraints=critical,CA:FALSE\n"
        "keyUsage=critical,digitalSignature\n"
        "extendedKeyUsage=serverAuth\n"
        "subjectKeyIdentifier=hash\n"
        "authorityKeyIdentifier=keyid\n" % FRONT_NAME, encoding="utf-8")
    openssl("x509", "-req", "-in", "server.csr", "-CA", "ca.pem", "-CAkey",
            "ca.key", "-CAcreateserial", "-days", "2", "-out", "server.pem",
            "-extfile", "server.ext")
    return directory / "ca.pem", directory / "server.pem", directory / "server.key"


@pytest.fixture
def public_origin(tmp_path, monkeypatch):
    """FRONT_NAME served over verified TLS, resolvable only through DNS.

    The name resolves (through a hosts-style override of getaddrinfo, the
    resolver every connection goes through) to a local TLS server whose
    certificate is signed by a CA the test trusts via SSL_CERT_FILE, so a
    connection succeeds only by resolving the name and verifying it.
    """
    import ssl
    from http.server import BaseHTTPRequestHandler

    ca, certificate, key = _tls_files(tmp_path / "tls")
    seen = {"requests": [], "resolved": []}

    class Origin(BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass

        def _send(self, status, body, content_type="application/json"):
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            seen["requests"].append(("GET", self.path, dict(self.headers)))
            self._send(200, b"0008NAK\n", "application/x-git-upload-pack-result")

        def do_POST(self):
            body = self.rfile.read(int(self.headers.get("Content-Length") or 0))
            seen["requests"].append(("POST", self.path, dict(self.headers)))
            request = json.loads(body)
            self._send(200, json.dumps({
                "ok": True, "authorized": True, "keyId": request["keyId"],
                "requestId": request["requestId"],
                "operation": request["operation"], "owner": "acme",
                "repository": "widget",
                "repositoryRelativePath": "acme/widget.git",
                "httpCredential": {
                    "username": "alice",
                    "password": "fms1.%s.1783000000000.%s" % (
                        request["keyId"], "a" * 64),
                    "expiresAt": 1783000000000,
                },
            }).encode())

    server = ThreadingHTTPServer(("127.0.0.1", 0), Origin)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(certificate, key)
    server.socket = context.wrap_socket(server.socket, server_side=True)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    port = server.server_address[1]

    real_getaddrinfo = socket.getaddrinfo

    def getaddrinfo(host, *args, **kwargs):
        if host in (FRONT_NAME, "other.example.test"):
            seen["resolved"].append(host)
            return real_getaddrinfo("127.0.0.1", *args, **kwargs)
        return real_getaddrinfo(host, *args, **kwargs)

    monkeypatch.setattr(socket, "getaddrinfo", getaddrinfo)
    monkeypatch.setenv("SSL_CERT_FILE", str(ca))
    monkeypatch.delenv("SSL_CERT_DIR", raising=False)
    yield {"origin": "https://%s:%d" % (FRONT_NAME, port), "port": port,
           "seen": seen}
    server.shutdown()
    server.server_close()


def test_remote_front_reaches_the_origin_by_dns_with_verified_tls(public_origin):
    target = ssh_gateway.HttpGitTarget({
        "origin": public_origin["origin"], "maxRequestBytes": 100_000_000,
        "maxSessionsPerClient": 4})
    assert target.connect_address is None
    response = ssh_gateway._relay_send(
        target, "GET", "/acme/widget.git/info/refs?service=git-upload-pack",
        ssh_gateway._relay_headers(target, "Basic eA==", {}))
    assert response.read() == b"0008NAK\n"
    response.close()
    method, path, headers = public_origin["seen"]["requests"][-1]
    assert (method, path) == ("GET",
                              "/acme/widget.git/info/refs?service=git-upload-pack")
    assert headers["Host"] == "%s:%d" % (FRONT_NAME, public_origin["port"])
    assert headers["User-Agent"] == ssh_gateway.RELAY_USER_AGENT
    assert public_origin["seen"]["resolved"][-1] == FRONT_NAME
    # The certificate really is checked against the name: the same server
    # under another name is refused, so there is no silent fallback.
    other = ssh_gateway.HttpGitTarget(
        {"origin": "https://other.example.test:%d" % public_origin["port"]})
    with pytest.raises(ssh_gateway.RelayError):
        ssh_gateway._relay_send(other, "GET", "/x", ssh_gateway._relay_headers(
            other, "Basic eA==", {}))


def test_remote_front_broker_authorizes_through_dns_without_connect_address(
        public_origin):
    import ssh_authorization_broker as broker
    config = broker.BrokerConfig(
        api_origin=public_origin["origin"], token="t" * 48,
        socket_path=Path("/run/x/authorize.sock"), socket_gid=1,
        lookup_uid=2, git_uid=3)
    assert config.api_connect_address is None
    value = {"action": "authorize", "keyId": "sk_" + "a" * 24,
             "owner": "acme", "repository": "widget",
             "operation": "git-receive-pack"}
    result = broker._authorize_upstream(config, value)
    assert result["httpCredential"]["username"] == "alice"
    method, path, headers = public_origin["seen"]["requests"][-1]
    assert (method, path) == ("POST", "/api/ssh/authorize")
    assert headers["Authorization"] == "Bearer " + "t" * 48
    assert headers["Host"] == "%s:%d" % (FRONT_NAME, public_origin["port"])
    assert public_origin["seen"]["resolved"][-1] == FRONT_NAME


class _Answer:
    def __init__(self, status, body=b"", headers=None):
        self.status = status
        self._body = body
        self._headers = {k.lower(): v for k, v in (headers or {}).items()}

    def read(self, _size=-1):
        return self._body

    def getheader(self, name, default=None):
        return self._headers.get(name.lower(), default)


def test_proxy_answers_become_readable_git_errors():
    proxy_413 = str(ssh_gateway._relay_refusal(_Answer(
        413, b"<html>413 Request Entity Too Large</html>",
        {"Content-Type": "text/html"})))
    assert "larger than the HTTPS proxy" in proxy_413
    assert "Push fewer commits" in proxy_413 and "HTTPS" in proxy_413
    # The Git route's own 413 (a quota) keeps its own explanation.
    quota = ssh_gateway._relay_refusal(_Answer(
        413, b'{"message": "repository quota exceeded"}'))
    assert str(quota) == "repository quota exceeded"
    timeout = str(ssh_gateway._relay_refusal(_Answer(524, b"<html></html>")))
    assert "may still have been applied" in timeout and "git fetch" in timeout
    blocked = str(ssh_gateway._relay_refusal(_Answer(
        403, b"<html>challenge</html>", {"cf-mitigated": "challenge"})))
    assert "bot protection" in blocked
    assert str(ssh_gateway._relay_refusal(_Answer(403, b""))) \
        == "repository access denied"


def _push_big(relay, name, size):
    git = relay["git"]
    work = relay["tmp"] / name
    git(ALICE, "clone", "-q", URL, str(work), check=True)
    (work / "blob.bin").write_bytes(os.urandom(size))
    git(ALICE, "add", ".", cwd=work, check=True)
    git(ALICE, "commit", "-q", "-m", name, cwd=work, check=True)
    return git(ALICE, "push", "origin", "HEAD:refs/heads/" + name, cwd=work)


def test_a_proxy_413_on_a_push_prints_a_readable_git_error(relay):
    relay["front"].proxy_status = 413
    pushed = _push_big(relay, "too-big", 3 * 1024 * 1024)
    assert pushed.returncode != 0
    assert "larger than the HTTPS proxy" in pushed.stderr, pushed.stderr
    assert "Push fewer commits" in pushed.stderr
    assert not _rev(relay["bare"], "refs/heads/too-big")


def test_a_push_over_the_proxy_cap_stops_at_the_gateway(relay):
    config_path = relay["tmp"] / "gateway.json"
    config = json.loads(config_path.read_text(encoding="utf-8"))
    config["httpGit"]["maxRequestBytes"] = 2 * 1024 * 1024
    config_path.write_text(json.dumps(config), encoding="utf-8")
    pushed = _push_big(relay, "capped", 3 * 1024 * 1024)
    assert pushed.returncode != 0
    assert "larger than the HTTPS proxy" in pushed.stderr, pushed.stderr
    assert "(2 MB)" in pushed.stderr
    assert relay["front"].received_packs == []
    assert not _rev(relay["bare"], "refs/heads/capped")
    # Under the cap, the same configuration pushes normally.
    small = _push_big(relay, "fits", 256 * 1024)
    assert small.returncode == 0, small.stderr


def test_sessions_per_client_are_capped_by_address(tmp_path, monkeypatch):
    capacity = tmp_path / "capacity"
    capacity.mkdir(mode=0o700)
    prefix = ssh_gateway._client_slot_prefix("203.0.113.9")
    assert prefix == ssh_gateway._client_slot_prefix("::ffff:203.0.113.9")
    assert ssh_gateway._client_slot_prefix("2001:db8::1") \
        == ssh_gateway._client_slot_prefix("2001:db8::2")
    assert ssh_gateway._client_slot_prefix("2001:db8:0:1::1") \
        != ssh_gateway._client_slot_prefix("2001:db8::1")
    assert ssh_gateway._client_slot_prefix("") is None
    held = [ssh_gateway._acquire_slot(capacity, prefix, maximum=2,
                                      reserved_receive=0, receive=False)
            for _ in range(2)]
    with pytest.raises(ssh_gateway.GatewayError, match="at capacity"):
        ssh_gateway._acquire_slot(capacity, prefix, maximum=2,
                                  reserved_receive=0, receive=False)
    for lease in held:
        lease.close()
    for bad in (0, 65, True, "4"):
        with pytest.raises(ssh_gateway.GatewayError):
            ssh_gateway.HttpGitTarget({"origin": "https://x.example.test",
                                       "maxSessionsPerClient": bad})
    for bad in (1024, True, "100000000", 2 ** 40):
        with pytest.raises(ssh_gateway.GatewayError):
            ssh_gateway.HttpGitTarget({"origin": "https://x.example.test",
                                       "maxRequestBytes": bad})


def _token_check(**settings):
    import hmac
    import ssh_keys
    import test_ssh_git_auth as auth
    namespace = auth._load_entry(
        "_ssh_gateway_tokens", "_ssh_gateway_address_allowed",
        "_ssh_gateway_token_ok",
        extra_globals={"ssh_auth": ssh_keys, "hmac": hmac})
    env = type("Env", (), settings)()

    class Request:
        def __init__(self, token, address="198.51.100.4"):
            self.headers = {"authorization": "Bearer " + token,
                            "cf-connecting-ip": address}

    return lambda token, **kw: namespace["_ssh_gateway_token_ok"](
        env, Request(token, **kw))


def test_token_rotation_accepts_both_tokens_and_only_them():
    old, new = "o" * 48, "n" * 48
    rotating = _token_check(SSH_GATEWAY_TOKEN=old, SSH_GATEWAY_TOKEN_NEXT=new)
    assert rotating(old) and rotating(new)
    assert not rotating("x" * 48) and not rotating("")
    finished = _token_check(SSH_GATEWAY_TOKEN=new)
    assert finished(new) and not finished(old)
    # A malformed NEXT is ignored, never a wildcard.
    malformed = _token_check(SSH_GATEWAY_TOKEN=old,
                             SSH_GATEWAY_TOKEN_NEXT="short\nvalue")
    assert malformed(old) and not malformed("short\nvalue")
    assert not _token_check()("o" * 48)


def test_an_address_pin_narrows_the_token_to_the_front():
    token = "o" * 48
    pinned = _token_check(SSH_GATEWAY_TOKEN=token,
                          SSH_GATEWAY_ALLOWED_ADDRESSES="203.0.113.7, 2001:db8::/64")
    assert pinned(token, address="203.0.113.7")
    assert pinned(token, address="2001:db8::5")
    assert not pinned(token, address="198.51.100.4")
    assert not pinned(token, address="")
    assert not pinned("x" * 48, address="203.0.113.7")
    broken = _token_check(SSH_GATEWAY_TOKEN=token,
                          SSH_GATEWAY_ALLOWED_ADDRESSES="203.0.113.7,not-an-ip")
    assert not broken(token, address="203.0.113.7")


def _installer(*arguments, app_env):
    return subprocess.run(
        ["bash", str(PROJECT_ROOT / "tools" / "install_ssh_relay.sh"),
         *arguments, "--print-config"],
        capture_output=True, text=True, timeout=30, check=False,
        env={"PATH": os.environ.get("PATH", "/usr/bin:/bin"),
             "FORKMESH_APP_ENV": str(app_env)})


def _printed_configs(output):
    configs = {}
    for block in output.split("# /etc/forkmesh-ssh/")[1:]:
        name, _newline, body = block.partition("\n")
        configs[name.strip()] = json.loads(body)
    return configs


def test_installer_remote_front_writes_no_loopback_and_needs_no_instance(tmp_path):
    missing = tmp_path / "no-instance" / "app.env"
    result = _installer("--remote-front", "--api-origin", "https://git.example.test",
                        "--host", "ssh.example.test", app_env=missing)
    assert result.returncode == 0, result.stderr
    assert "127.0.0.1" not in result.stdout and "::1" not in result.stdout
    assert "onnectAddress" not in result.stdout
    configs = _printed_configs(result.stdout)
    gateway_config = configs["ssh-gateway.json"]
    broker_config = configs["ssh-authorization-broker.json"]
    assert gateway_config["httpGit"] == {
        "origin": "https://git.example.test",
        "maxRequestBytes": 100_000_000,
        "maxSessionsPerClient": 4,
    }
    assert broker_config["apiOrigin"] == "https://git.example.test"
    assert broker_config["gatewayTokenFile"] == "/etc/forkmesh-ssh/ssh-gateway.token"
    # The gateway accepts exactly this block (DNS, no override).
    target = ssh_gateway.HttpGitTarget(gateway_config["httpGit"])
    assert target.connect_address is None and target.host == "git.example.test"
    # The cap is adjustable and can be switched off.
    uncapped = _printed_configs(_installer(
        "--remote-front", "--api-origin", "https://git.example.test",
        "--max-request-mb", "0", app_env=missing).stdout)
    assert "maxRequestBytes" not in uncapped["ssh-gateway.json"]["httpGit"]
    # A front without an origin is refused rather than guessed.
    assert _installer("--remote-front", app_env=missing).returncode != 0
    # On the instance itself the relay keeps using the local front end.
    app_env = tmp_path / "app.env"
    app_env.write_text("PUBLIC_BASE_URL=https://git.example.test\n",
                       encoding="utf-8")
    local = _printed_configs(_installer("--host", "ssh.example.test",
                                        app_env=app_env).stdout)
    assert local["ssh-gateway.json"]["httpGit"]["connectAddress"] \
        == "127.0.0.1:443"
    assert local["ssh-authorization-broker.json"]["apiConnectAddress"] \
        == "127.0.0.1:443"
    # The remote-front install path never writes app.env or restarts the app.
    script = (PROJECT_ROOT / "tools" / "install_ssh_relay.sh").read_text(
        encoding="utf-8")
    assert 'if [ "$ROLE" = local ]; then\n  say "publishing SSH addresses' in script
    assert "--token-file -" in script and "fail2ban" in script
    assert "MaxStartups" in script and "PerSourceMaxStartups" in script
