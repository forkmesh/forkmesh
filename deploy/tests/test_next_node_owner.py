import base64
import io
import json
import subprocess
import sys
from pathlib import Path

import pytest

pytest.importorskip("cryptography")

from cryptography.hazmat.primitives import serialization  # noqa: E402
from cryptography.hazmat.primitives.asymmetric.ed25519 import (  # noqa: E402
    Ed25519PrivateKey, Ed25519PublicKey)

import next_node_owner as tool  # noqa: E402
from next_common import GuardError, Runner  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
FLEET = {"nodes": [{"name": "n1", "public_ipv4": "192.0.2.1"}],
         "runners": [{"name": "r1", "public_ipv4": "192.0.2.11"}]}


def b64(raw):
    return base64.urlsafe_b64encode(raw).decode().rstrip("=")


def unb64(text):
    return base64.urlsafe_b64decode(text + "=" * (-len(text) % 4))


class FakeDb:
    def __init__(self, pubkey, owner="fmnext", registrant="fmnext",
                 owners=("jett",), admin=True):
        self.owner = owner
        self.pubkey = pubkey
        self.runner = {"name": "fmnext-r1", "public_key": "k",
                       "registered_by": registrant, "scope": "pool",
                       "allowed_owners": json.dumps(list(owners)),
                       "status": "active", "last_seen_at": 0}
        self.admin = admin
        self.executed = []

    def record(self, table, key, name):
        return {"kind": "user", "status": "active", "name": name,
                "nodes": [] if name == "fmnext" else ["mirror35"]}

    def account(self, name):
        if name == "fmnext-n1":
            return {"kind": "node", "owner": self.owner, "status": "active"}
        return self.record("users", "user_bi", name)

    def node_row(self, name):
        return {"pubkey": self.pubkey, "username": self.owner}

    def runner_row(self, name):
        return dict(self.runner)

    def is_admin(self, name):
        return self.admin

    def audit_statement(self, actor, action, target_type, target, details):
        return ["AUDIT", actor, action, target_type, target, details]

    def execute(self, statements):
        self.executed.append(statements)
        for statement in statements:
            if statement[0].startswith("UPDATE runner_nodes"):
                self.runner["registered_by"] = statement[1]
                return [{"rows_affected": 1}]
        return [{}]


class NodeSsh:
    def __init__(self, pem_path):
        self.pem_path = pem_path
        self.commands = []

    def run(self, host, script, input_bytes=None, check=True, timeout=0):
        self.commands.append(script)
        args = script.split()
        assert args[:3] == ["python3", "-I", "-"]
        args[3] = str(self.pem_path)
        return subprocess.run([sys.executable, "-I", "-", *args[3:]],
                              input=input_bytes, capture_output=True,
                              check=True)


@pytest.fixture
def node_key(tmp_path):
    key = Ed25519PrivateKey.generate()
    pem = tmp_path / "ed25519.pem"
    pem.write_bytes(key.private_bytes(
        serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
        serialization.NoEncryption()))
    raw = key.public_key().public_bytes(serialization.Encoding.Raw,
                                        serialization.PublicFormat.Raw)
    return pem, b64(raw)


NEXT_ENTRY = Path(__file__).resolve().parents[2] / "app" / "src" / "entry.py"


def pem_key(tmp_path, name):
    key = Ed25519PrivateKey.generate()
    pem = tmp_path / name
    pem.write_bytes(key.private_bytes(
        serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
        serialization.NoEncryption()))
    return pem, key.public_key()


def test_signer_signs_a_v2_grant_naming_the_target(node_key):
    pem, pubkey = node_key
    ssh = NodeSsh(pem)
    got_pub, sig = tool.sign_grant(ssh, FLEET["nodes"][0], "fmnext-n1",
                                   "jett", "123", "nonce-0123456789abcdef")
    assert got_pub == pubkey
    Ed25519PublicKey.from_public_bytes(unb64(pubkey)).verify(
        unb64(sig), b"forkmesh-link-grant-v2\nfmnext-n1\njett\n123\n"
        b"nonce-0123456789abcdef")
    assert tool.grant_canonical("fmnext-n1", "jett", "123",
                                "nonce-0123456789abcdef") == (
        b"forkmesh-link-grant-v2\nfmnext-n1\njett\n123\n"
        b"nonce-0123456789abcdef")
    assert "PRIVATE" not in ssh.commands[0]


def test_canonicals_match_the_next_app():
    if not NEXT_ENTRY.exists():
        pytest.skip("ForkMesh Next checkout not present")
    entry = NEXT_ENTRY.read_text()
    if "forkmesh-link-accept-v1" not in entry:
        pytest.skip("Next main predates signed link-grant accepts")
    assert ('"forkmesh-link-grant-v2\\n" + node_name + "\\n" + named +\n'
            '                     "\\n" + ts + "\\n" + nonce') in entry
    assert ('"forkmesh-link-accept-v1\\n" + node_name + "\\n" + user +\n'
            '                     "\\n" + grant_sig + "\\n" + accept_ts') in entry


def test_transfer_posts_a_named_grant_the_target_accepts(node_key, tmp_path,
                                                         monkeypatch):
    pem, pubkey = node_key
    target_pem, target_pub = pem_key(tmp_path, "jett.pem")
    db = FakeDb(pubkey)
    posted = []

    def fake_post(app_url, body, session=""):
        Ed25519PublicKey.from_public_bytes(unb64(pubkey)).verify(
            unb64(body["sig"]), tool.grant_canonical(
                body["nodeName"], body["user"], body["ts"], body["nonce"]))
        target_pub.verify(unb64(body["acceptSig"]), tool.accept_canonical(
            body["nodeName"], "jett", body["sig"], body["acceptTs"]))
        assert session == ""
        posted.append((body["nodeName"], body["user"]))
        db.owner = body["user"]
        return 200, {"ok": True, "linked": True, "user": body["user"]}

    monkeypatch.setattr(tool, "post_grant", fake_post)
    out = io.StringIO()
    tool.transfer(db, NodeSsh(pem), Runner(execute=True, out=out), FLEET,
                  "fmnext", "jett", "https://app.invalid",
                  tool.Acceptor(key_path=str(target_pem)))
    assert posted == [("fmnext-n1", "jett")]
    assert db.runner["registered_by"] == "jett"
    actions = [s[2] for batch in db.executed for s in batch
               if s[0] == "AUDIT"]
    assert actions == ["node.owner_transfer", "runner.owner_transfer"]


def test_transfer_by_session_sends_the_target_session(node_key, monkeypatch):
    pem, pubkey = node_key
    db = FakeDb(pubkey)
    sessions = []

    def fake_post(app_url, body, session=""):
        assert "acceptSig" not in body
        sessions.append(session)
        db.owner = body["user"]
        return 200, {"ok": True, "linked": True, "user": body["user"]}

    monkeypatch.setattr(tool, "post_grant", fake_post)
    monkeypatch.setenv("JETT_SESSION", "session-value")
    tool.transfer(db, NodeSsh(pem), Runner(execute=True, out=io.StringIO()),
                  FLEET, "fmnext", "jett", "https://app.invalid",
                  tool.Acceptor(session_env="JETT_SESSION"))
    assert sessions == ["session-value"]


def test_transfer_refuses_without_the_targets_acceptance(node_key,
                                                         monkeypatch):
    pem, pubkey = node_key
    db = FakeDb(pubkey)
    monkeypatch.setattr(tool, "post_grant", lambda *a: pytest.fail("posted"))
    with pytest.raises(GuardError):
        tool.transfer(db, NodeSsh(pem), Runner(execute=True,
                                               out=io.StringIO()),
                      FLEET, "fmnext", "jett", "https://app.invalid")
    assert db.owner == "fmnext"


def test_transfer_is_idempotent(node_key, monkeypatch):
    pem, pubkey = node_key
    db = FakeDb(pubkey, owner="jett", registrant="jett")
    monkeypatch.setattr(tool, "post_grant", lambda *a: pytest.fail("posted"))
    tool.transfer(db, NodeSsh(pem), Runner(execute=True, out=io.StringIO()),
                  FLEET, "fmnext", "jett", "https://app.invalid")
    assert db.executed == []


def test_refuses_a_node_key_that_is_not_the_registered_one(node_key,
                                                          monkeypatch):
    pem, _pubkey = node_key
    db = FakeDb(b64(b"\0" * 32))
    monkeypatch.setattr(tool, "post_grant", lambda *a: pytest.fail("posted"))
    with pytest.raises(GuardError):
        tool.transfer(db, NodeSsh(pem), Runner(execute=True,
                                               out=io.StringIO()),
                      FLEET, "fmnext", "jett", "https://app.invalid",
                      tool.Acceptor(session_env="JETT_SESSION"))


def test_refuses_an_unexpected_current_owner(node_key):
    pem, pubkey = node_key
    with pytest.raises(GuardError):
        tool.transfer(FakeDb(pubkey, owner="someone"), NodeSsh(pem),
                      Runner(execute=True, out=io.StringIO()), FLEET,
                      "fmnext", "jett", "https://app.invalid")


def test_runner_needs_the_target_in_allowed_owners(node_key):
    pem, pubkey = node_key
    db = FakeDb(pubkey, owner="jett", owners=("alice",))
    with pytest.raises(GuardError):
        tool.transfer(db, NodeSsh(pem), Runner(execute=True,
                                               out=io.StringIO()),
                      FLEET, "fmnext", "jett", "https://app.invalid")


def test_pool_runner_registrant_must_be_admin(node_key):
    pem, pubkey = node_key
    db = FakeDb(pubkey, owner="jett", admin=False)
    with pytest.raises(GuardError):
        tool.transfer(db, NodeSsh(pem), Runner(execute=True,
                                               out=io.StringIO()),
                      FLEET, "fmnext", "jett", "https://app.invalid")


def test_dry_run_changes_nothing(node_key, monkeypatch):
    pem, pubkey = node_key
    db = FakeDb(pubkey)
    ssh = NodeSsh(pem)
    monkeypatch.setattr(tool, "post_grant", lambda *a: pytest.fail("posted"))
    tool.transfer(db, ssh, Runner(execute=False, out=io.StringIO()), FLEET,
                  "fmnext", "jett", "https://app.invalid")
    assert db.executed == [] and ssh.commands == []


class MissingDb:
    def __init__(self):
        self.pubkey = ""

    def account(self, name):
        return {"kind": "node", "status": "active"} if self.pubkey else None

    def node_row(self, name):
        return {"pubkey": self.pubkey, "username": None} if self.pubkey else None


def test_register_creates_a_missing_core_account_with_the_node_key(
        node_key, monkeypatch):
    pem, pubkey = node_key
    db = MissingDb()
    posted = []

    def fake_post(app_url, path, body):
        key = Ed25519PublicKey.from_public_bytes(unb64(body["pubkey"]))
        if path == "reserve":
            key.verify(unb64(body["sig"]), ("forkmesh-reserve-v1\n%s\n%s" % (
                body["nodeName"], body["ts"])).encode())
            posted.append(path)
            return 201, {"ok": True}
        key.verify(unb64(body["sig"]), ("forkmesh-finalize-v1\n%s\n\n%s" % (
            body["nodeName"], body["ts"])).encode())
        assert body["linkCode"] == "" and body["email"] == ""
        posted.append(path)
        db.pubkey = body["pubkey"]
        return 201, {"ok": True}

    monkeypatch.setattr(tool, "post_account", fake_post)
    ssh = NodeSsh(pem)
    tool.register(db, ssh, Runner(execute=True, out=io.StringIO()), FLEET,
                  "https://app.invalid")
    assert posted == ["reserve", "finalize"] and db.pubkey == pubkey
    assert "PRIVATE" not in ssh.commands[0]


def test_register_skips_an_active_core_and_dry_run_posts_nothing(
        node_key, monkeypatch):
    pem, pubkey = node_key
    monkeypatch.setattr(tool, "post_account",
                        lambda *a: pytest.fail("posted"))
    ssh = NodeSsh(pem)
    tool.register(FakeDb(pubkey), ssh, Runner(execute=True,
                                              out=io.StringIO()),
                  FLEET, "https://app.invalid")
    tool.register(MissingDb(), ssh, Runner(execute=False, out=io.StringIO()),
                  FLEET, "https://app.invalid")
    assert ssh.commands == []


def test_register_canonicals_match_the_next_app():
    if not NEXT_ENTRY.exists():
        pytest.skip("ForkMesh Next checkout not present")
    entry = NEXT_ENTRY.read_text()
    handlers = (REPO / "app" / "src" / "route_handlers.py").read_text()
    assert '("forkmesh-reserve-v1\\n" + name + "\\n" + ts)' in entry
    assert ('("forkmesh-finalize-v1\\n" + name + "\\n" + email + "\\n" +\n'
            '                 ts)') in handlers
