import asyncio
import base64
import importlib.util
import json
import re
import sqlite3
from pathlib import Path

from cryptography.hazmat.primitives.asymmetric.ed25519 import (
    Ed25519PrivateKey, Ed25519PublicKey)
from cryptography.hazmat.primitives.serialization import (
    Encoding, PublicFormat)


SRC = Path(__file__).resolve().parents[1] / "src"
ROOT = SRC.parents[1]


def _b64url(raw):
    return base64.urlsafe_b64encode(raw).rstrip(b"=").decode()


def _unb64url(text):
    return base64.urlsafe_b64decode(text + "=" * (-len(text) % 4))


class _Clock:
    now = 1_800_000_000_000

    @classmethod
    def now_ms(cls):
        return cls.now


class _Date:
    @staticmethod
    def now():
        return _Clock.now_ms()


class _Request:
    def __init__(self, method, body=None, owner_signed=False, url=""):
        self.method = method
        self.body = body
        self.owner_signed = owner_signed
        self.url = url


class _Response:
    def __init__(self, data, status):
        self.data = data
        self.status = status


class _Harness:
    def __init__(self):
        spec = importlib.util.spec_from_file_location(
            "desktop_commands_under_test", SRC / "desktop_commands.py")
        self.module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.module)
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.keys = {}
        self.notified = []
        self.org_links = {}
        self.org_writers = {}
        self.env = object()
        self.module._bind_runtime(self.runtime())

    def runtime(self):
        harness = self

        async def d1_run(env, sql, *args):
            harness.db.execute(sql, args)

        async def d1_first(env, sql, *args):
            row = harness.db.execute(sql, args).fetchone()
            return dict(row) if row else None

        async def d1_all(env, sql, *args):
            return [dict(r) for r in harness.db.execute(sql, args).fetchall()]

        async def encrypt_row(env, obj):
            return "enc:" + json.dumps(obj)

        async def decrypt_row(env, stored):
            return json.loads(stored[4:]) if stored.startswith("enc:") else None

        async def blind_index(env, value):
            return "bi:" + value.lower()

        async def bounded_json_request(request):
            if not isinstance(request.body, dict):
                raise ValueError("bad json")
            return request.body

        async def owner_signing_pubkeys(env, account):
            return [_b64url(key.public_key().public_bytes(
                Encoding.Raw, PublicFormat.Raw))
                for key in harness.keys.get(account, [])]

        async def ed25519_verify(public_key, sig, message):
            try:
                Ed25519PublicKey.from_public_bytes(_unb64url(public_key)).verify(
                    _unb64url(sig), message)
                return True
            except Exception:
                return False

        async def authorize_owner(env, request, owner):
            return request.owner_signed

        async def notify_repo_host(env, owner, repo, topic):
            harness.notified.append((owner, repo, topic))
            return True

        async def org_write_allowed(env, owner, repo, account):
            for org, node in harness.org_links.get(repo.lower(), {}).items():
                if node == owner.lower() and account in harness.org_writers.get(
                        org, ()):
                    return True
            return False

        async def public_repository_owner(env, request, owner, repo):
            match = re.match(r"^https://[^/]+/api/repo/([^/]+)/([^/]+)/",
                             request.url or "")
            if match and match.group(2) == repo and harness.org_links.get(
                    repo.lower(), {}).get(match.group(1).lower()) == owner:
                return match.group(1)
            return owner

        return {
            "d1_run": d1_run, "d1_first": d1_first, "d1_all": d1_all,
            "encrypt_row": encrypt_row, "decrypt_row": decrypt_row,
            "blind_index": blind_index,
            "bounded_json_request": bounded_json_request,
            "_owner_signing_pubkeys": owner_signing_pubkeys,
            "ed25519_verify": ed25519_verify,
            "_authorize_owner": authorize_owner,
            "notify_repo_host": notify_repo_host,
            "_org_write_allowed": org_write_allowed,
            "_public_repository_owner": public_repository_owner,
            "json_response": lambda data, status=200: _Response(data, status),
            "method_name": lambda request: request.method,
            "Date": _Date,
        }

    def enroll(self, account):
        key = Ed25519PrivateKey.generate()
        self.keys.setdefault(account, []).append(key)
        return key

    def signed_body(self, key, owner, repo, command="pull.merge",
                    target="pull:7", ts=None, account=None, payload=None):
        ts = str(_Clock.now if ts is None else ts)
        canonical = "\n".join((
            "forkmesh-desktop-command-v1", owner, repo, command, target, ts))
        return {
            "ownerAccount": (account or owner).lower(),
            "command": command,
            "target": target,
            "ts": ts,
            "sig": _b64url(key.sign(canonical.encode())),
            "payload": payload if payload is not None else {"pullNumber": 7},
        }

    def link(self, org, repo, node, writers=()):
        self.org_links.setdefault(repo, {})[org] = node
        self.org_writers.setdefault(org, set()).update(writers)

    def post(self, owner, repo, body, addressed=None):
        url = "https://app.forkmesh.com/api/repo/%s/%s/desktop-commands" % (
            addressed or owner, repo)
        return asyncio.run(self.module.desktop_commands_handler(
            self.env, _Request("POST", body, url=url), owner, repo))

    def drain(self, owner, repo, owner_signed=True):
        return asyncio.run(self.module.desktop_commands_handler(
            self.env, _Request("GET", owner_signed=owner_signed), owner, repo))

    def ack(self, owner, repo, ids, owner_signed=True):
        return asyncio.run(self.module.desktop_commands_ack_handler(
            self.env, _Request("POST", {"ids": ids}, owner_signed),
            owner, repo))


def test_owner_signed_command_is_queued_and_drained_by_the_desktop():
    h = _Harness()
    key = h.enroll("alice")
    response = h.post("alice", "app", h.signed_body(
        key, "alice", "app", payload={"pullNumber": 7, "head": "evil"}))
    assert response.status == 200
    assert response.data == {"ok": True, "queued": 1}
    assert h.notified == [("alice", "app", "desktop-commands")]

    drained = h.drain("alice", "app")
    assert drained.status == 200
    [command] = drained.data["commands"]
    assert command["command"] == "pull.merge"
    assert command["target"] == "pull:7"
    assert command["payload"] == {"pullNumber": 7}
    assert command["canonical"].startswith(
        "forkmesh-desktop-command-v1\nalice\napp\npull.merge\npull:7\n")
    assert drained.data["ackPath"] == "/api/repo/alice/app/desktop-commands/ack"

    assert h.ack("alice", "app", [command["id"]]).data["acknowledged"] == 1
    assert h.drain("alice", "app").data["commands"] == []


def test_non_owner_account_is_refused():
    h = _Harness()
    mallory = h.enroll("mallory")
    body = h.signed_body(mallory, "alice", "app", account="mallory")
    response = h.post("alice", "app", body)
    assert response.status == 403
    assert h.notified == []


def test_org_member_queues_for_their_own_desktop_under_the_org_name():
    h = _Harness()
    h.link("acme", "app", "alice", writers=("bob",))
    bob = h.enroll("bob")
    body = h.signed_body(bob, "acme", "app", account="bob")
    response = h.post("alice", "app", body, addressed="acme")
    assert response.status == 200
    assert h.notified == [("bob", "app", "desktop-commands")]
    assert h.drain("alice", "app").data["commands"] == []
    [command] = h.drain("bob", "app").data["commands"]
    assert command["ownerAccount"] == "bob"
    assert command["repoOwner"] == "acme"
    assert command["canonical"].startswith(
        "forkmesh-desktop-command-v1\nacme\napp\npull.merge\npull:7\n")


def test_backing_node_account_signs_with_the_org_name_it_addressed():
    h = _Harness()
    h.link("acme", "app", "alice")
    alice = h.enroll("alice")
    body = h.signed_body(alice, "acme", "app", account="alice")
    assert h.post("alice", "app", body, addressed="acme").status == 200
    [command] = h.drain("alice", "app").data["commands"]
    assert command["repoOwner"] == "acme"
    assert h.notified == [("alice", "app", "desktop-commands")]


def test_org_member_without_write_access_is_refused():
    h = _Harness()
    h.link("acme", "app", "alice", writers=("bob",))
    carol = h.enroll("carol")
    body = h.signed_body(carol, "acme", "app", account="carol")
    response = h.post("alice", "app", body, addressed="acme")
    assert response.status == 403
    assert response.data["error"] == "not_authorized"
    assert h.notified == []


def test_org_member_signature_over_the_node_name_is_refused():
    h = _Harness()
    h.link("acme", "app", "alice", writers=("bob",))
    bob = h.enroll("bob")
    body = h.signed_body(bob, "alice", "app", account="bob")
    response = h.post("alice", "app", body, addressed="acme")
    assert response.status == 401
    assert response.data["error"] == "invalid_signature"


def test_signature_from_a_key_the_owner_never_enrolled_is_refused():
    h = _Harness()
    h.enroll("alice")
    stranger = Ed25519PrivateKey.generate()
    response = h.post("alice", "app", h.signed_body(stranger, "alice", "app"))
    assert response.status == 401
    assert response.data["error"] == "invalid_signature"


def test_tampered_target_breaks_the_signature():
    h = _Harness()
    key = h.enroll("alice")
    body = h.signed_body(key, "alice", "app", target="pull:7")
    body["target"] = "pull:8"
    assert h.post("alice", "app", body).data["error"] == "invalid_signature"


def test_stale_timestamp_is_refused():
    h = _Harness()
    key = h.enroll("alice")
    body = h.signed_body(key, "alice", "app", ts=_Clock.now - 10 * 60 * 1000)
    response = h.post("alice", "app", body)
    assert response.status == 401
    assert response.data["error"] == "stale_request"


def test_replayed_request_is_refused_even_after_the_desktop_acked_it():
    h = _Harness()
    key = h.enroll("alice")
    body = h.signed_body(key, "alice", "app")
    assert h.post("alice", "app", body).status == 200
    [command] = h.drain("alice", "app").data["commands"]
    h.ack("alice", "app", [command["id"]])
    replay = h.post("alice", "app", dict(body))
    assert replay.status == 409
    assert replay.data["error"] == "replayed_request"
    assert h.drain("alice", "app").data["commands"] == []


def test_cross_repo_replay_is_refused():
    h = _Harness()
    key = h.enroll("alice")
    body = h.signed_body(key, "alice", "app")
    response = h.post("alice", "other", body)
    assert response.status == 401
    assert response.data["error"] == "invalid_signature"
    assert h.drain("alice", "other").data["commands"] == []


def test_unknown_command_and_malformed_target_are_refused():
    h = _Harness()
    key = h.enroll("alice")
    unknown = h.signed_body(key, "alice", "app", command="shell.exec",
                            target="pull:7")
    assert h.post("alice", "app", unknown).data["error"] == "unsupported_command"
    bad_target = h.signed_body(key, "alice", "app", target="run:7")
    assert h.post("alice", "app", bad_target).data["error"] == "unsupported_command"


def test_release_and_rerun_payloads_come_from_the_signed_target():
    h = _Harness()
    key = h.enroll("alice")
    assert h.post("alice", "app", h.signed_body(
        key, "alice", "app", command="release.publish",
        target="release:v1.2.4", payload={"tag": "v9"})).status == 200
    _Clock.now += 1
    assert h.post("alice", "app", h.signed_body(
        key, "alice", "app", command="action.rerun", target="run:42",
        payload={})).status == 200
    payloads = [c["payload"] for c in h.drain("alice", "app").data["commands"]]
    assert payloads == [{"tag": "v1.2.4"}, {"runId": "42"}]


def test_drain_and_ack_need_the_owner_node_signature():
    h = _Harness()
    key = h.enroll("alice")
    h.post("alice", "app", h.signed_body(key, "alice", "app"))
    assert h.drain("alice", "app", owner_signed=False).status == 401
    assert h.ack("alice", "app", [1], owner_signed=False).status == 401
    assert len(h.drain("alice", "app").data["commands"]) == 1


def test_the_route_is_wired_and_lazy():
    urls = (SRC / "urls.py").read_text(encoding="utf-8")
    assert re.search(r'REPO_DESKTOP_COMMANDS_RE = re\.compile\(\s*r"\^/api/repo/'
                     r'\(\[\^/\]\+\)/\(\[\^/\]\+\)/desktop-commands\$"\)', urls)
    runtime = (SRC / "app_runtime.py").read_text(encoding="utf-8")
    assert "REPO_DESKTOP_COMMANDS_RE.match(url.path)" in runtime
    assert "desktop_commands_ack_handler(" in runtime
    entry = (SRC / "entry.py").read_text(encoding="utf-8")
    assert '_desktop_commands = _LazyModule("desktop_commands")' in entry
    assert '"REPO_DESKTOP_COMMANDS_RE", "REPO_DESKTOP_COMMANDS_ACK_RE",' in entry
    footprint = (ROOT / "world" / "tools" / "build_worker_footprint.py").read_text(
        encoding="utf-8")
    assert '"desktop_commands.py",' in footprint
    migration = (ROOT / "app" / "migrations" / "0204_desktop_commands.sql").read_text(
        encoding="utf-8")
    assert "CREATE TABLE IF NOT EXISTS desktop_commands" in migration
