import asyncio
import base64
import hashlib
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import runner_ssh_setup  # noqa: E402
from test_runner_leases import NOW  # noqa: E402
from test_runner_logins import FakeRuntime, browser  # noqa: E402

ED_BLOB = base64.b64encode(b"\x00\x00\x00\x0bssh-ed25519" + b"k" * 36).decode()
RSA_BLOB = base64.b64encode(b"\x00\x00\x00\x07ssh-rsa" + b"r" * 80).decode()
PASSWORD = "hunter2-correct-horse"  # forkmesh-secret-scan:ignore-line
KEY = ("-----BEGIN OPENSSH PRIVATE KEY-----\n" + "b3BlbnNzaC1rZXktdjEAAAAA" * 4  # forkmesh-secret-scan:ignore-line
       + "\n-----END OPENSSH PRIVATE KEY-----\n")


class SshRuntime(FakeRuntime):
    def __init__(self):
        super().__init__()
        self.audits = []
        self.requests = []
        self.spawned = []
        self.addresses = {"box.example.com": ["203.0.113.7", "8.8.8.8"],
                          "vps.example.com": ["8.8.4.4"],
                          "lan.example.com": ["192.168.1.5"]}
        self.install_events = [{"out": "Installing forkmesh runner"},
                               {"exit": 0}]

    def ssh_helper_socket(self):
        return "/run/test.sock"

    def ssh_helper(self, request):
        self.requests.append(json.loads(json.dumps(request)))

        async def events():
            if request.get("op") == "keyscan":
                yield {"keys": [{"type": "ssh-rsa", "key": RSA_BLOB},
                                {"type": "ssh-ed25519", "key": ED_BLOB},
                                {"type": "bogus", "key": "x"}]}
                return
            for event in self.install_events:
                yield dict(event)
        return events()

    async def resolve_host(self, host):
        return self.addresses.get(host, [])

    def spawn(self, coroutine):
        self.spawned.append(coroutine)

    def run_spawned(self):
        while self.spawned:
            asyncio.run(self.spawned.pop(0))


def fingerprint(blob):
    digest = hashlib.sha256(base64.b64decode(blob)).digest()
    return "SHA256:" + base64.b64encode(digest).decode().rstrip("=")


def probe(runtime, host="vps.example.com", port=22, actor="alice"):
    return browser(runtime, actor, "/api/runners/ssh/probe",
                   {"host": host, "port": port})


def install(runtime, setup_id, actor="alice", **extra):
    body = {"id": setup_id, "fingerprint": fingerprint(ED_BLOB),
            "username": "root", "password": PASSWORD, "name": "alice-vps"}
    body.update(extra)
    return browser(runtime, actor, "/api/runners/ssh/install", body)


def everything(runtime, *answers):
    rows = [dict(r) for r in runtime.db.execute(
        "SELECT * FROM runner_ssh_setups").fetchall()]
    rows += [dict(r) for r in runtime.db.execute(
        "SELECT * FROM runner_setup_tokens").fetchall()]
    return json.dumps([rows, runtime.audits, list(answers)], default=str)


def test_routes():
    assert runner_ssh_setup.match("ssh/probe") == ("probe",)
    assert runner_ssh_setup.match("ssh/install") == ("install",)
    assert runner_ssh_setup.match("ssh/abc") == ("status", "abc")
    assert runner_ssh_setup.match("ssh") is None
    assert runner_ssh_setup.match("ssh/a/b") is None


def test_probe_pins_a_public_address_and_shows_fingerprints():
    runtime = SshRuntime()
    assert probe(runtime, actor="")["status"] == 401
    answer = probe(runtime)
    assert answer["status"] == 200
    body = answer["data"]
    assert body["address"] == "8.8.4.4" and body["port"] == 22
    assert body["hostKeys"] == [
        {"type": "ssh-ed25519", "fingerprint": fingerprint(ED_BLOB)},
        {"type": "ssh-rsa", "fingerprint": fingerprint(RSA_BLOB)},
    ]
    assert body["installs"]
    assert runtime.requests[-1] == {"op": "keyscan", "address": "8.8.4.4",
                                    "port": 22}


def test_probe_refuses_private_and_mixed_answers():
    runtime = SshRuntime()
    for host in ("lan.example.com", "box.example.com", "10.77.0.2",
                 "127.0.0.1", "nowhere.example.com"):
        answer = probe(runtime, host=host)
        assert answer["status"] == 400, host
    assert probe(runtime, host="bad host!")["status"] == 400
    assert probe(runtime, port=70000)["status"] == 400
    assert runtime.requests == []


def test_install_uses_the_password_once_and_never_returns_or_stores_it():
    runtime = SshRuntime()
    setup = probe(runtime)["data"]
    answer = install(runtime, setup["id"])
    assert answer["status"] == 202
    assert answer["data"]["state"] == "running"
    assert answer["data"]["runner"] == "alice-vps"
    assert PASSWORD not in everything(runtime, answer)
    runtime.install_events = [
        {"out": "echo " + PASSWORD},
        {"out": "token fmrt_" + "a" * 64},
        {"out": "\x1b[32mdone\x1b[0m"},
        {"exit": 0},
    ]
    runtime.run_spawned()
    sent = runtime.requests[-1]
    assert sent["op"] == "install" and sent["address"] == "8.8.4.4"
    assert sent["hostKey"] == {"type": "ssh-ed25519", "key": ED_BLOB}
    assert sent["password"] == PASSWORD and sent["user"] == "root"
    assert sent["name"] == "alice-vps"
    assert runner_ssh_setup.runner_setup.TOKEN_RE.fullmatch(sent["token"])
    status = browser(runtime, "alice", "/api/runners/ssh/" + setup["id"],
                     method="GET")
    body = status["data"]["setup"]
    assert body["state"] == "succeeded"
    assert body["fingerprint"] == fingerprint(ED_BLOB)
    assert "[hidden]" in body["log"] and "done" in body["log"]
    assert "fmrt_[hidden]" in body["log"]
    dump = everything(runtime, status)
    assert PASSWORD not in dump and sent["token"] not in dump
    assert "\x1b" not in body["log"]
    assert browser(runtime, "bob", "/api/runners/ssh/" + setup["id"],
                   method="GET")["status"] == 404


def test_install_needs_a_confirmed_fingerprint_fresh_probe_and_one_secret():
    runtime = SshRuntime()
    setup = probe(runtime)["data"]
    assert install(runtime, setup["id"], fingerprint="SHA256:nope")["status"] == 400
    assert install(runtime, setup["id"], username="Root!")["status"] == 400
    assert install(runtime, setup["id"], privateKey=KEY)["status"] == 400
    assert install(runtime, setup["id"], password="",
                   privateKey="not a key")["status"] == 400
    assert install(runtime, setup["id"], actor="bob")["status"] == 404
    assert install(runtime, setup["id"], name="bob")["status"] == 409
    runtime.clock += 11 * 60 * 1000
    assert install(runtime, setup["id"])["status"] == 409
    fresh = probe(runtime)["data"]
    keyed = install(runtime, fresh["id"], password="", privateKey=KEY,
                    passphrase="pp-secret-1")
    assert keyed["status"] == 202
    assert install(runtime, fresh["id"])["status"] == 409
    runtime.run_spawned()
    assert runtime.requests[-1]["privateKey"] == KEY
    dump = everything(runtime, keyed)
    assert "b3BlbnNzaC1rZXktdjEAAAAA" not in dump and "pp-secret-1" not in dump


def test_failures_and_a_silent_helper_end_in_failed():
    runtime = SshRuntime()
    setup = probe(runtime)["data"]
    install(runtime, setup["id"])
    runtime.install_events = [{"exit": 255, "error": "SSH could not sign in"}]
    runtime.run_spawned()
    body = browser(runtime, "alice", "/api/runners/ssh/" + setup["id"],
                   method="GET")["data"]["setup"]
    assert body["state"] == "failed" and "could not sign in" in body["message"]
    second = probe(runtime)["data"]
    install(runtime, second["id"], name="alice-vps2")
    runtime.spawned.pop().close()
    runtime.clock += runner_ssh_setup.RUN_STALE_MS + 1
    body = browser(runtime, "alice", "/api/runners/ssh/" + second["id"],
                   method="GET")["data"]["setup"]
    assert body["state"] == "failed"


def test_without_a_helper_the_ssh_path_says_so():
    runtime = SshRuntime()
    runtime.ssh_helper_socket = lambda: ""
    answer = probe(runtime)
    assert answer["status"] == 503
    assert answer["data"]["error"] == "ssh_setup_unavailable"


def test_scrub_and_fingerprint_helpers():
    assert runner_ssh_setup.fingerprint("!!") == ""
    assert runner_ssh_setup.scrub("a\x1b[1mb fmrt_0123456789abcdef x", ["x"]) == (
        "ab fmrt_[hidden] x")
    assert runner_ssh_setup.scrub("pw=secretpw", ["secretpw"]) == "pw=[hidden]"
    log = ""
    for index in range(200):
        log = runner_ssh_setup.append_log(log, "line %d" % index)
    assert log.split("\n")[-1] == "line 199"
    assert len(log.split("\n")) == runner_ssh_setup.LOG_LINES
    assert runner_ssh_setup.public_address("::ffff:10.0.0.1") is None
    assert str(runner_ssh_setup.public_address("8.8.8.8")) == "8.8.8.8"
    assert NOW
