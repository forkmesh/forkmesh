"""Phase 5 runner tier: registration, signed claims, eligibility, grants."""

import asyncio
import base64
import hashlib
import json
from pathlib import Path
import sqlite3
import sys

from cryptography.hazmat.primitives.asymmetric.ed25519 import (
    Ed25519PrivateKey, Ed25519PublicKey)
from cryptography.hazmat.primitives import serialization
from cryptography.exceptions import InvalidSignature
import pytest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
import edge_routing  # noqa: E402
import runner_actions  # noqa: E402
import runner_leases  # noqa: E402


NOW = 2_100_000_000_000


def b64(raw):
    return base64.urlsafe_b64encode(raw).decode().rstrip("=")


def unb64(value):
    return base64.urlsafe_b64decode(value + "=" * (-len(value) % 4))


class Key:
    def __init__(self):
        self.private = Ed25519PrivateKey.generate()
        self.public = b64(self.private.public_key().public_bytes(
            serialization.Encoding.Raw, serialization.PublicFormat.Raw))

    def sign(self, message):
        return b64(self.private.sign(message.encode()))


ROUTER = Key()


class FakeRuntime:
    def __init__(self):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.db.executescript(
            (ROOT / "migrations" / "0193_runner_nodes.sql").read_text()
            .split("-- The runtime schema text changed")[0])
        self.db.executescript(
            (ROOT / "migrations" / "0195_runner_login_sessions.sql").read_text()
            .split("-- The runtime schema text changed")[0])
        self.db.executescript(
            (ROOT / "migrations" / "0220_runner_account_requests.sql").read_text()
            .split("INSERT INTO schema_meta")[0])
        self.db.executescript(
            (ROOT / "migrations" / "0223_runner_setup.sql").read_text()
            .split("INSERT INTO schema_meta")[0])
        self.db.executescript(
            (ROOT / "migrations" / "0236_runner_terminals.sql").read_text()
            .split("INSERT INTO schema_meta")[0])
        self.db.executescript(
            (ROOT / "migrations" / "0248_repo_action_jobs.sql").read_text()
            .split("INSERT INTO schema_meta")[0])
        self.db.execute("""CREATE TABLE org_agent_jobs (
            id INTEGER PRIMARY KEY AUTOINCREMENT, session_id TEXT NOT NULL,
            org_bi TEXT NOT NULL, target_node TEXT NOT NULL,
            repo TEXT NOT NULL, provider TEXT NOT NULL,
            status TEXT NOT NULL DEFAULT 'queued',
            lease_id TEXT NOT NULL DEFAULT '', data TEXT NOT NULL,
            created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL)""")
        self.clock = NOW
        self.ids = 0
        self.method_value = "POST"
        self.headers = {}
        self.raw = b""
        self.actor = ""
        self.admins = {"root"}
        self.accounts = {"alice", "bob", "root"}
        self.claimed_nodes = {"core-a": []}
        self.pool = {"core-a"}
        self.gateways = {"core-a": "https://core-a.next.test"}
        self.applied = []
        self.apply_status = 200
        self.slept = []
        self.on_sleep = None
        self.stopping_value = False
        self.attachments = {}

    # request plumbing
    def use(self, raw=b"", headers=None, actor="", method="POST"):
        self.raw, self.headers = raw, {
            k.lower(): v for k, v in (headers or {}).items()}
        self.actor, self.method_value = actor, method
        return self

    def method(self): return self.method_value
    def header(self, name): return self.headers.get(name.lower(), "")
    def now(self): return self.clock
    def stopping(self): return self.stopping_value
    async def sleep(self, ms):
        self.slept.append(ms)
        self.clock += ms
        if self.on_sleep:
            self.on_sleep(len(self.slept))
    def new_id(self):
        self.ids += 1
        return "%032x" % self.ids
    def response(self, data, status=200): return {"status": status, "data": data}
    @staticmethod
    def bytes_response(data): return {"status": 200, "bytes": data}
    async def read_attachment(self, session_id, org_bi, attachment_id):
        return self.attachments.get((session_id, org_bi, attachment_id), b"")
    @staticmethod
    def status_of(response): return response["status"]
    async def ensure_schema(self): pass
    async def body_bytes(self, limit):
        return self.raw if len(self.raw) <= limit else None
    async def session(self, _data):
        return ("bi-" + self.actor, {"name": self.actor}) if self.actor else ("", None)
    async def is_admin(self, name): return name in self.admins
    async def name_taken(self, name):
        return name in self.accounts or name in self.claimed_nodes
    async def node_keys(self, name): return list(self.claimed_nodes.get(name, []))
    async def verify(self, public_key, signature, message):
        try:
            Ed25519PublicKey.from_public_bytes(unb64(public_key)).verify(
                unb64(signature), message)
            return True
        except (InvalidSignature, ValueError):
            return False
    async def sign_router(self, message): return ROUTER.sign(message)
    def pool_targets(self): return set(self.pool)
    async def gateway_url(self, node): return self.gateways.get(node, "")
    async def apply_result(self, target, repo, job_id, data):
        self.applied.append((target, repo, job_id, data))
        return {"status": self.apply_status, "data": {"ok": True}}
    async def open(self, value): return json.loads(value)
    async def seal(self, value): return json.dumps(value)
    async def audit(self, *_args): pass
    async def d1_all(self, sql, *args):
        return [dict(r) for r in self.db.execute(sql, args).fetchall()]
    async def d1_first(self, sql, *args):
        row = self.db.execute(sql, args).fetchone()
        return dict(row) if row else None
    async def d1_run(self, sql, *args):
        cursor = self.db.execute(sql, args)
        self.db.commit()
        return cursor
    @staticmethod
    def changes(cursor): return cursor.rowcount

    # fixtures
    def add_runner(self, name, key, scope="pool", owners=("alice",), status="active"):
        registrant = "root" if scope == "pool" or not owners else owners[0]
        self.db.execute(
            "INSERT INTO runner_nodes (name,public_key,registered_by,scope,"
            "allowed_owners,status,created_at,updated_at) VALUES (?,?,?,?,?,?,?,?)",
            (name, key.public, registrant, scope, json.dumps(list(owners)),
             status, NOW, NOW))
        self.db.commit()

    def add_job(self, target="core-a", repo="proj", session="s1",
                provider="claude-code", status="queued", **record):
        data = {"kind": "start", "sessionId": session, "organization": "alice",
                "repositoryOwner": target, "repositoryName": repo,
                "provider": provider, "prompt": "do it", "model": "",
                "requestedBy": "alice", **record}
        cursor = self.db.execute(
            "INSERT INTO org_agent_jobs (session_id,org_bi,target_node,repo,"
            "provider,status,data,created_at,updated_at) VALUES (?,?,?,?,?,?,?,?,?)",
            (session, "org-alice", target, repo, provider, status,
             json.dumps(data), NOW - 1000, NOW - 1000))
        self.db.commit()
        return cursor.lastrowid

    def job_status(self, job_id):
        return self.db.execute(
            "SELECT status FROM org_agent_jobs WHERE id=?", (job_id,)).fetchone()[0]


def call(runtime, path):
    return asyncio.run(runner_leases.handle(runtime, path))


_request_counter = [0]


def signed(runtime, name, key, path, body, request_id=None, issued_at=None,
           tamper=False):
    raw = json.dumps(body).encode()
    _request_counter[0] += 1
    request_id = request_id or "request-%08d" % _request_counter[0]
    issued_at = runtime.clock if issued_at is None else issued_at
    digest = hashlib.sha256(raw).hexdigest()
    message = edge_routing.request_message(
        name, "POST", path, digest, request_id, issued_at)
    signature = key.sign(message)
    if tamper:
        raw = raw + b" "
    return runtime.use(raw, {
        "X-ForkMesh-Node": name, "X-ForkMesh-Request-Id": request_id,
        "X-ForkMesh-Issued-At": str(issued_at),
        "X-ForkMesh-Body-Sha256": digest, "X-ForkMesh-Signature": signature,
    })


def claim(runtime, name, key, body=None, **kwargs):
    return call(signed(runtime, name, key, "/api/runners/claim",
                       body or {"max": 4}, **kwargs), "/api/runners/claim")


def report(runtime, name, key, body):
    return call(signed(runtime, name, key, "/api/runners/report", body),
                "/api/runners/report")


def decode_grant(token):
    encoded, signature = token.split(".")
    message = unb64(encoded).decode()
    Ed25519PublicKey.from_public_bytes(unb64(ROUTER.public)).verify(
        unb64(signature), message.encode())
    return message.split("\n")


# --- edge_routing ---------------------------------------------------------

def test_runner_grant_message_golden_and_rejects():
    message = edge_routing.runner_grant_message(
        "Core-A", "core-a", "proj", "push", "refs/heads/agent/s1", "run-1",
        "grant-000000001", 1000, 1000 + 3600_000)
    assert message == (
        "forkmesh-runner-grant-v1\ncore-a\ncore-a\nproj\npush\n"
        "refs/heads/agent/s1\nrun-1\ngrant-000000001\n1000\n3601000")
    good = dict(node="core-a", owner="core-a", repository="proj",
                access="push", ref="refs/heads/agent/s1", runner="run-1",
                grant_id="grant-000000001", issued_at=1000,
                expires_at=2000)
    assert edge_routing.runner_grant_message(**good)
    for change in (
        {"ref": "refs/heads/main"}, {"ref": "refs/heads/agent/"},
        {"ref": "refs/heads/agent/a/../b"}, {"ref": "refs/heads/agent/x.lock"},
        {"ref": "refs/tags/agent/x"}, {"ref": "refs/heads/agent/a@{1}"},
        {"access": "read"},  # read grants carry no ref
        {"access": "write"}, {"runner": "Bad_Name"},
        {"grant_id": "short"}, {"expires_at": 1000},
        {"expires_at": 1000 + 3 * 3600_000 + 1}, {"issued_at": 0},
        {"repository": "a/b"},
    ):
        assert edge_routing.runner_grant_message(**{**good, **change}) == ""
    assert edge_routing.runner_grant_message(
        **{**good, "access": "read", "ref": ""}).split("\n")[5] == ""


# --- registration ---------------------------------------------------------

def register(runtime, actor, key, name, scope="personal", owners=None, ts=NOW,
             proof_key=None, registered_by=None):
    owners = [actor] if owners is None else owners
    message = runner_leases.register_message(
        name, key.public, registered_by or actor, ts)
    body = {"name": name, "publicKey": key.public, "scope": scope,
            "allowedOwners": owners, "ts": ts,
            "proof": (proof_key or key).sign(message)}
    return call(runtime.use(json.dumps(body).encode(), actor=actor),
                "/api/runners/register")


def test_registration_rules():
    runtime = FakeRuntime()
    key = Key()
    assert register(runtime, "", key, "run-x")["status"] == 401
    # Pool runners must list the owners they serve (opt-in, audit H4).
    assert register(runtime, "root", key, "pool-1", "pool", [])["status"] == 400
    assert register(runtime, "root", key, "pool-1", "pool", ["alice"])["status"] == 201
    assert register(runtime, "alice", Key(), "alice-box")["status"] == 201
    # Non-admin: no pool runner, no serving other owners.
    assert register(runtime, "alice", Key(), "alice-p", "pool", [])["status"] == 403
    assert register(runtime, "alice", Key(), "alice-2", owners=["bob"])["status"] == 403
    # Admins too: a personal runner serves only the account that added it.
    assert register(runtime, "root", Key(), "root-2", owners=["alice"])["status"] == 403
    assert register(runtime, "root", Key(), "root-3",
                    owners=["root", "alice"])["status"] == 403
    # Proof must come from the runner key, name the registrant, and be fresh.
    assert register(runtime, "bob", Key(), "bob-box",
                    proof_key=Key())["status"] == 401
    assert register(runtime, "bob", Key(), "bob-box",
                    registered_by="alice")["status"] == 401
    assert register(runtime, "bob", Key(), "bob-box",
                    ts=NOW - 10 * 60_000)["status"] == 400
    # Names: taken by another registrant, an account, or a pool target.
    assert register(runtime, "bob", Key(), "alice-box",
                    owners=["bob"])["status"] == 409
    assert register(runtime, "bob", Key(), "alice")["status"] == 409
    assert register(runtime, "root", Key(), "core-a", "pool", ["alice"])["status"] == 409
    # The registrant may rotate its own runner's key.
    assert register(runtime, "alice", Key(), "alice-box")["status"] == 200
    # Revocation: a personal runner only by its registrant (anyone else is
    # told it does not exist); a pool runner also by an admin.
    revoke = lambda actor, name="alice-box": call(runtime.use(  # noqa: E731
        json.dumps({"name": name}).encode(), actor=actor),
        "/api/runners/revoke")
    assert revoke("bob")["status"] == 404
    assert revoke("root")["status"] == 404
    assert revoke("alice")["status"] == 200
    runtime.admins.add("ops")
    assert revoke("alice", "pool-1")["status"] == 404
    assert revoke("ops", "pool-1")["status"] == 200
    assert runtime.db.execute(
        "SELECT status FROM runner_nodes WHERE name='alice-box'"
    ).fetchone()[0] == "revoked"


# --- claim auth -----------------------------------------------------------

def test_claim_authentication_failures():
    runtime = FakeRuntime()
    key = Key()
    runtime.add_runner("pool-1", key)
    runtime.add_runner("gone-1", key, status="revoked")
    runtime.add_job()
    assert claim(runtime, "pool-1", Key())["status"] == 401  # wrong key
    assert claim(runtime, "nobody", key)["status"] == 401  # unknown
    assert claim(runtime, "gone-1", key)["status"] == 401  # revoked
    assert claim(runtime, "pool-1", key,
                 issued_at=NOW - 61_000)["status"] == 401  # stale
    assert claim(runtime, "pool-1", key, tamper=True)["status"] == 401
    first = claim(runtime, "pool-1", key, request_id="replayed-000001")
    assert first["status"] == 200
    assert claim(runtime, "pool-1", key,
                 request_id="replayed-000001")["status"] == 401
    # A request signed for claim does not authorize report.
    request = signed(runtime, "pool-1", key, "/api/runners/claim", {"jobId": 1})
    assert call(request, "/api/runners/report")["status"] == 401


# --- claim eligibility and grants -----------------------------------------

def test_claim_returns_exact_grants_and_holds_the_lease():
    runtime = FakeRuntime()
    key = Key()
    runtime.add_runner("pool-1", key)
    job = runtime.add_job(session="sess-9", title="Fix it", createPr=True)
    result = claim(runtime, "pool-1", key)
    assert result["status"] == 200
    [leased] = result["data"]["jobs"]
    assert leased["jobId"] == job
    assert leased["pushRef"] == "refs/heads/agent/sess-9"
    assert leased["branch"] == "agent/sess-9"
    assert leased["cloneUrl"] == (
        "https://core-a.next.test/v1/runner/core-a/proj.git")
    assert leased["repositoryOwner"] == "core-a"
    assert leased["createPr"] is True and leased["timeoutMs"] == 7_200_000
    assert leased["grantExpiresAt"] == NOW + 7_200_000
    push = decode_grant(leased["pushGrant"])
    assert push[:7] == ["forkmesh-runner-grant-v1", "core-a", "core-a", "proj",
                        "push", "refs/heads/agent/sess-9", "pool-1"]
    assert int(push[9]) - int(push[8]) <= 3 * 3600_000
    read = decode_grant(leased["readGrant"])
    assert read[4:6] == ["read", ""]
    assert runtime.job_status(job) == "leased"
    lease = runtime.db.execute(
        "SELECT runner,lease_id,ref FROM runner_job_leases WHERE job_id=?",
        (job,)).fetchone()
    assert tuple(lease) == ("pool-1", leased["leaseId"], "refs/heads/agent/sess-9")
    assert claim(runtime, "pool-1", key)["data"]["jobs"] == []


def test_eligibility_filters():
    runtime = FakeRuntime()
    pool_key, own_key = Key(), Key()
    runtime.add_runner("pool-1", pool_key, owners=["alice"])
    runtime.add_runner("alice-box", own_key, scope="personal", owners=["alice"])
    runtime.gateways["core-b"] = "https://core-b.next.test"
    other_pin = runtime.add_job(repo="r1", session="p1", runner="someone")
    not_pool = runtime.add_job(target="desk-1", repo="r2", session="p2")
    codex = runtime.add_job(repo="r3", session="p3", provider="codex")
    bad_continue = runtime.add_job(repo="r4", session="p4", kind="continue",
                                   branch="main")
    orphan_steer = runtime.add_job(repo="r5", session="p5", kind="steer")
    bob_job = runtime.add_job(repo="r6", session="p6", organization="bob")
    no_gateway = runtime.add_job(target="core-b", repo="r7", session="p7")
    runtime.pool.add("core-b")
    del runtime.gateways["core-b"]
    good = runtime.add_job(repo="r8", session="p8", kind="continue",
                           branch="agent/handoff-1")

    # The personal runner takes nothing unpinned from the pool.
    assert claim(runtime, "alice-box", own_key)["data"]["jobs"] == []

    jobs = claim(runtime, "pool-1", pool_key, {
        "max": 4, "providers": ["claude-code"],
        "allowedOwners": ["alice"]})["data"]["jobs"]
    assert [j["jobId"] for j in jobs] == [good]
    assert jobs[0]["pushRef"] == "refs/heads/agent/handoff-1"
    for job in (other_pin, not_pool, codex, bad_continue, orphan_steer,
                bob_job, no_gateway):
        assert runtime.job_status(job) == "queued"
    # Owner allow-list on the pool runner row itself.
    runtime.db.execute(
        "UPDATE runner_nodes SET allowed_owners='[\"carol\"]' WHERE name='pool-1'")
    runtime.db.commit()
    assert claim(runtime, "pool-1", pool_key,
                 {"max": 4, "providers": ["codex"]})["data"]["jobs"] == []
    # Pinned to the personal runner: only alice's own jobs, requested by
    # alice, ever reach it, whatever its owner list says.
    bob_asked = runtime.add_job(repo="r10", session="p10", runner="alice-box",
                                requestedBy="bob")
    no_requester = runtime.add_job(repo="r11", session="p11",
                                   runner="alice-box", requestedBy="")
    org_job = runtime.add_job(repo="r12", session="p12", runner="alice-box",
                              organization="acme")
    runtime.db.execute(
        "UPDATE runner_nodes SET allowed_owners='[\"alice\",\"acme\"]' "
        "WHERE name='alice-box'")
    runtime.db.commit()
    pinned = runtime.add_job(repo="r9", session="p9", runner="alice-box")
    jobs = claim(runtime, "alice-box", own_key, {"max": 4})["data"]["jobs"]
    assert [j["jobId"] for j in jobs] == [pinned]
    for job in (bob_asked, no_requester, org_job):
        assert runtime.job_status(job) == "queued"


def test_pool_runner_without_owners_serves_nobody():
    runtime = FakeRuntime()
    key = Key()
    runtime.add_runner("pool-1", key, owners=[])
    job = runtime.add_job(repo="r1", session="o1")
    assert claim(runtime, "pool-1", key, {"max": 4})["data"]["jobs"] == []
    # Even when the runner itself asks for that owner.
    assert claim(runtime, "pool-1", key, {
        "max": 4, "allowedOwners": ["alice"]})["data"]["jobs"] == []
    assert runtime.job_status(job) == "queued"
    runtime.db.execute(
        "UPDATE runner_nodes SET allowed_owners='[\"alice\"]' WHERE name='pool-1'")
    runtime.db.commit()
    assert [j["jobId"] for j in claim(runtime, "pool-1", key)["data"]["jobs"]] == [job]


def test_fifo_max_and_steer_affinity():
    runtime = FakeRuntime()
    a_key, b_key = Key(), Key()
    runtime.add_runner("pool-a", a_key)
    runtime.add_runner("pool-b", b_key)
    first = runtime.add_job(repo="same", session="s1")
    second = runtime.add_job(repo="same", session="s2")
    others = [runtime.add_job(repo="r%d" % i, session="o%d" % i)
              for i in range(5)]
    jobs = claim(runtime, "pool-a", a_key, {"max": 9})["data"]["jobs"]
    # One per (node, repo) at a time, at most four per claim.
    assert [j["jobId"] for j in jobs] == [first] + others[:3]
    assert runtime.job_status(second) == "queued"

    # Finish s1 on pool-a, then a steer for s1 goes only to pool-a.
    lease = jobs[0]["leaseId"]
    done = report(runtime, "pool-a", a_key, {
        "jobId": first, "leaseId": lease, "securityVerdict": "approved",
        "status": "completed"})
    assert done["status"] == 200
    runtime.db.execute("UPDATE org_agent_jobs SET status='done' WHERE id=?",
                       (first,))
    runtime.db.commit()
    steer = runtime.add_job(repo="steer-repo", session="s1", kind="steer")
    runtime.clock += 1
    got_b = claim(runtime, "pool-b", b_key, {"max": 4})["data"]["jobs"]
    assert steer not in [j["jobId"] for j in got_b]
    runtime.clock += 1
    got_a = claim(runtime, "pool-a", a_key, {"max": 4})["data"]["jobs"]
    steered = [j for j in got_a if j["jobId"] == steer]
    assert steered and steered[0]["pushRef"] == "refs/heads/agent/s1"


def test_expired_lease_is_requeued_and_old_lease_cannot_report():
    runtime = FakeRuntime()
    key = Key()
    runtime.add_runner("pool-1", key)
    job = runtime.add_job()
    first = claim(runtime, "pool-1", key)["data"]["jobs"][0]
    runtime.clock += 2 * 3600_000 + 1
    second = claim(runtime, "pool-1", key)["data"]["jobs"]
    assert [j["jobId"] for j in second] == [job]
    assert second[0]["leaseId"] != first["leaseId"]
    stale = report(runtime, "pool-1", key, {
        "jobId": job, "leaseId": first["leaseId"], "status": "completed",
        "securityVerdict": "approved"})
    assert stale["status"] == 409


def test_report_requires_the_callers_lease_and_delegates():
    runtime = FakeRuntime()
    key, other = Key(), Key()
    runtime.add_runner("pool-1", key)
    runtime.add_runner("pool-2", other)
    job = runtime.add_job()
    leased = claim(runtime, "pool-1", key)["data"]["jobs"][0]
    body = {"jobId": job, "leaseId": leased["leaseId"],
            "securityVerdict": "approved", "status": "running"}
    assert report(runtime, "pool-2", other, body)["status"] == 409
    assert report(runtime, "pool-1", key,
                  {**body, "leaseId": "wrong"})["status"] == 409
    assert runtime.applied == []
    assert report(runtime, "pool-1", key, body)["status"] == 200
    assert runtime.applied[-1][:3] == ("core-a", "proj", job)
    # Running keeps the lease; a terminal report releases it (kept for steer).
    assert report(runtime, "pool-1", key,
                  {**body, "status": "completed"})["status"] == 200
    row = runtime.db.execute(
        "SELECT lease_id,runner FROM runner_job_leases WHERE job_id=?",
        (job,)).fetchone()
    assert tuple(row) == ("", "pool-1")
    assert report(runtime, "pool-1", key,
                  {**body, "status": "completed"})["status"] == 409


def attachment(runtime, name, key, body):
    return call(signed(runtime, name, key, "/api/runners/attachment", body),
                "/api/runners/attachment")


def test_claim_carries_attachments_and_only_the_lease_holder_downloads_them():
    runtime = FakeRuntime()
    key, other = Key(), Key()
    runtime.add_runner("pool-1", key)
    runtime.add_runner("pool-2", other)
    image = b"\xff\xd8\xff" + b"x" * 40
    meta = {"id": "a" * 32, "name": "photo.jpg", "mediaType": "image/jpeg",
            "size": len(image), "sha256": hashlib.sha256(image).hexdigest()}
    job = runtime.add_job(session="sess-img", attachments=[meta, "junk"])
    runtime.attachments[("sess-img", "org-alice", "a" * 32)] = image
    [leased] = claim(runtime, "pool-1", key)["data"]["jobs"]
    assert leased["attachments"] == [meta]
    body = {"jobId": job, "leaseId": leased["leaseId"], "attachmentId": "a" * 32}
    assert attachment(runtime, "pool-1", key, body) == {
        "status": 200, "bytes": image}
    assert attachment(runtime, "pool-2", other, body)["status"] == 409
    assert attachment(runtime, "pool-1", key,
                      {**body, "leaseId": "wrong"})["status"] == 409
    assert attachment(runtime, "pool-1", key,
                      {**body, "attachmentId": "b" * 32})["status"] == 410
    runtime.attachments[("sess-img", "org-alice", "a" * 32)] = image + b"!"
    assert attachment(runtime, "pool-1", key, body)["status"] == 410
    runtime.attachments[("sess-img", "org-alice", "a" * 32)] = image
    assert report(runtime, "pool-1", key, {
        "jobId": job, "leaseId": leased["leaseId"],
        "securityVerdict": "approved", "status": "completed"})["status"] == 200
    assert attachment(runtime, "pool-1", key, body)["status"] == 409


def test_a_job_without_images_leases_an_empty_attachment_list():
    runtime = FakeRuntime()
    key = Key()
    runtime.add_runner("pool-1", key)
    runtime.add_job()
    [leased] = claim(runtime, "pool-1", key)["data"]["jobs"]
    assert leased["attachments"] == []


# --- pool status ----------------------------------------------------------

def test_pool_status_is_signed_by_a_pool_core_node():
    runtime = FakeRuntime()
    core, runner_key = Key(), Key()
    runtime.claimed_nodes["core-a"] = [core.public]
    runtime.add_runner("pool-1", runner_key)
    runtime.add_runner("alice-box", Key(), scope="personal", owners=["alice"])
    runtime.add_job(repo="r1", session="q1")
    runtime.add_job(repo="r2", session="q2", runner="alice-box")
    runtime.add_job(repo="r3", session="q3")
    claim(runtime, "pool-1", runner_key, {"max": 1})

    def status(name, key, **kwargs):
        return call(signed(runtime, name, key, "/api/runners/pool-status", {},
                           **kwargs), "/api/runners/pool-status")

    result = status("core-a", core)
    assert result["status"] == 200
    assert result["data"]["queued"] == 1  # r3; r1 leased, r2 personal pin
    assert result["data"]["oldestQueuedAt"] == NOW - 1000
    assert result["data"]["runners"] == [{
        "name": "pool-1", "scope": "pool", "lastSeenAt": NOW,
        "activeLeases": 1}]
    # A runner (or any non-pool node) cannot read it; nor a replay.
    assert status("pool-1", runner_key)["status"] == 401
    assert status("core-a", Key())["status"] == 401
    assert status("core-a", core, request_id="pool-replay-01")["status"] == 200
    assert status("core-a", core, request_id="pool-replay-01")["status"] == 401


def test_unknown_route_and_method():
    runtime = FakeRuntime()
    assert call(runtime.use(b"{}"), "/api/runners/other")["status"] == 404
    assert call(runtime.use(b"", method="GET"),
                "/api/runners/claim")["status"] == 405


def test_a_core_backing_an_org_alias_is_leased_under_the_alias_owner():
    runtime = FakeRuntime()
    runtime.db.executescript(
        "CREATE TABLE orgs (org_bi TEXT PRIMARY KEY, name TEXT);"
        "CREATE TABLE org_repos (org_bi TEXT, repo TEXT, node_owner TEXT);"
        "INSERT INTO orgs VALUES ('org-1', 'acme');"
        "INSERT INTO org_repos VALUES ('org-1', 'proj', 'core-a');")
    key = Key()
    runtime.add_runner("pool-1", key)
    runtime.add_job(session="sess-7")
    [leased] = claim(runtime, "pool-1", key)["data"]["jobs"]
    assert leased["repositoryOwner"] == "acme"
    assert leased["cloneUrl"] == (
        "https://core-a.next.test/v1/runner/acme/proj.git")
    for token in (leased["pushGrant"], leased["readGrant"]):
        assert decode_grant(token)[1:3] == ["core-a", "acme"]


def test_a_held_claim_answers_when_a_job_is_queued_without_leasing_it():
    runtime = FakeRuntime()
    key = Key()
    runtime.add_runner("pool-1", key)
    queued = []
    runtime.on_sleep = lambda n: n == 3 and queued.append(
        runtime.add_job(session="late"))
    result = claim(runtime, "pool-1", key, {"max": 1, "waitMs": 20_000})
    assert result["data"]["jobs"] == [] and result["data"]["waitMs"] == 20_000
    assert runtime.slept == [250, 250, 250]
    assert runtime.job_status(queued[0]) == "queued"
    result = claim(runtime, "pool-1", key, {"max": 1, "waitMs": 20_000})
    assert [j["jobId"] for j in result["data"]["jobs"]] == queued
    assert runtime.slept == [250, 250, 250]


def test_a_held_claim_answers_empty_once_the_host_starts_stopping():
    runtime = FakeRuntime()
    key = Key()
    runtime.add_runner("pool-1", key)
    runtime.on_sleep = lambda n: n == 2 and setattr(
        runtime, "stopping_value", True)
    result = claim(runtime, "pool-1", key, {"max": 1, "waitMs": 20_000})
    assert result["data"]["jobs"] == []
    assert runtime.slept == [250, 250]
    runtime.slept.clear()
    result = claim(runtime, "pool-1", key, {"max": 1, "waitMs": 20_000})
    assert result["data"]["jobs"] == [] and runtime.slept == []


def test_worker_runtime_sleep_wakes_when_the_host_stops():
    class Env:
        HOST_STOPPING = None

    async def scenario():
        env = Env()
        env.HOST_STOPPING = asyncio.Event()
        runtime = runner_leases.WorkerRuntime(env, None)
        assert not runtime.stopping()
        loop = asyncio.get_running_loop()
        loop.call_later(0.05, env.HOST_STOPPING.set)
        started = loop.time()
        await runtime.sleep(5_000)
        assert loop.time() - started < 1
        assert runtime.stopping()
        assert not runner_leases.WorkerRuntime(object(), None).stopping()

    asyncio.run(scenario())


def test_claim_wait_is_capped_and_ignores_unrelated_queued_jobs():
    runtime = FakeRuntime()
    key = Key()
    runtime.add_runner("pool-1", key)
    runtime.add_job(provider="codex")
    result = claim(runtime, "pool-1", key,
                   {"max": 1, "providers": ["claude-code"], "waitMs": 90_000})
    assert result["data"]["jobs"] == [] and result["data"]["waitMs"] == 20_000
    assert sum(runtime.slept) == 20_000


def test_claim_does_not_wait_with_a_job_a_login_or_no_wait():
    runtime = FakeRuntime()
    key = Key()
    runtime.add_runner("pool-1", key)
    runtime.add_job()
    assert len(claim(runtime, "pool-1", key,
                     {"max": 1, "waitMs": 20_000})["data"]["jobs"]) == 1
    assert claim(runtime, "pool-1", key)["data"]["jobs"] == []
    assert "waitMs" in claim(runtime, "pool-1", key)["data"]
    runtime.db.execute(
        "INSERT INTO runner_login_sessions (id,runner,provider,state,"
        "created_by,created_at,updated_at,expires_at) VALUES "
        "('login-1','pool-1','codex','pending','root',?,?,?)",
        (NOW, NOW, NOW + 600_000))
    result = claim(runtime, "pool-1", key, {"max": 1, "waitMs": 20_000})
    assert result["data"]["jobs"] == []
    assert [x["id"] for x in result["data"]["logins"]] == ["login-1"]
    assert runtime.slept == []


def test_a_follow_up_moves_to_another_runner_once_the_first_is_gone():
    runtime = FakeRuntime()
    a_key, b_key = Key(), Key()
    runtime.add_runner("pool-a", a_key)
    runtime.add_runner("pool-b", b_key)
    first = runtime.add_job(repo="proj", session="s1")
    leased = claim(runtime, "pool-a", a_key)["data"]["jobs"][0]
    report(runtime, "pool-a", a_key, {
        "jobId": first, "leaseId": leased["leaseId"],
        "securityVerdict": "approved", "status": "completed"})
    runtime.db.execute("UPDATE org_agent_jobs SET status='done' WHERE id=?",
                       (first,))
    runtime.db.commit()
    steer = runtime.add_job(repo="proj", session="s1", kind="steer",
                            branch="agent/s1", baseBranch="main")
    runtime.clock += 1
    assert claim(runtime, "pool-b", b_key)["data"]["jobs"] == []
    runtime.clock += runner_leases.STEER_HANDOVER_MS + 1
    moved = claim(runtime, "pool-b", b_key)["data"]["jobs"]
    assert [j["jobId"] for j in moved] == [steer]
    assert moved[0]["pushRef"] == "refs/heads/agent/s1"
    assert moved[0]["baseBranch"] == "main"


def test_a_revoked_runner_hands_its_follow_ups_over_at_once():
    runtime = FakeRuntime()
    a_key, b_key = Key(), Key()
    runtime.add_runner("pool-a", a_key)
    runtime.add_runner("pool-b", b_key)
    first = runtime.add_job(repo="proj", session="s1")
    leased = claim(runtime, "pool-a", a_key)["data"]["jobs"][0]
    report(runtime, "pool-a", a_key, {
        "jobId": first, "leaseId": leased["leaseId"],
        "securityVerdict": "approved", "status": "completed"})
    runtime.db.execute("UPDATE org_agent_jobs SET status='done' WHERE id=?",
                       (first,))
    runtime.db.execute(
        "UPDATE runner_nodes SET status='revoked' WHERE name='pool-a'")
    runtime.db.commit()
    steer = runtime.add_job(repo="proj", session="s1", kind="steer")
    runtime.clock += 1
    moved = claim(runtime, "pool-b", b_key)["data"]["jobs"]
    assert [j["jobId"] for j in moved] == [steer]


def test_a_personal_runner_takes_the_follow_ups_of_the_session_it_ran():
    runtime = FakeRuntime()
    own_key, other_key = Key(), Key()
    runtime.add_runner("alice-box", own_key, scope="personal", owners=["alice"])
    runtime.add_runner("alice-two", other_key, scope="personal",
                       owners=["alice"])
    first = runtime.add_job(repo="proj", session="s1", runner="alice-box")
    leased = claim(runtime, "alice-box", own_key)["data"]["jobs"][0]
    report(runtime, "alice-box", own_key, {
        "jobId": first, "leaseId": leased["leaseId"],
        "securityVerdict": "approved", "status": "completed"})
    runtime.db.execute("UPDATE org_agent_jobs SET status='done' WHERE id=?",
                       (first,))
    runtime.db.commit()
    steer = runtime.add_job(repo="proj", session="s1", kind="steer")
    runtime.clock += 1
    assert claim(runtime, "alice-two", other_key)["data"]["jobs"] == []
    got = claim(runtime, "alice-box", own_key)["data"]["jobs"]
    assert [j["jobId"] for j in got] == [steer]
    assert got[0]["pushRef"] == leased["pushRef"]


def test_a_follow_up_for_another_owner_is_not_offered():
    runtime = FakeRuntime()
    key = Key()
    runtime.add_runner("pool-1", key, owners=["jett"])
    first = runtime.add_job(repo="proj", session="s1", organization="jett")
    leased = claim(runtime, "pool-1", key)["data"]["jobs"][0]
    report(runtime, "pool-1", key, {
        "jobId": first, "leaseId": leased["leaseId"],
        "securityVerdict": "approved", "status": "completed"})
    runtime.db.execute("UPDATE org_agent_jobs SET status='done' WHERE id=?",
                       (first,))
    runtime.db.commit()
    stuck = runtime.add_job(repo="proj", session="s1", kind="steer",
                            organization="fmnext-n1")
    runtime.clock += 1
    assert claim(runtime, "pool-1", key)["data"]["jobs"] == []
    assert runtime.job_status(stuck) == "queued"
    runtime.db.execute("UPDATE org_agent_jobs SET status='failed' WHERE id=?",
                       (stuck,))
    runtime.db.commit()
    steer = runtime.add_job(repo="proj", session="s1", kind="steer",
                            organization="jett")
    runtime.clock += 1
    assert [j["jobId"] for j in claim(runtime, "pool-1", key)["data"]["jobs"]] \
        == [steer]


def test_a_named_branch_is_the_start_ref_and_the_report_names_the_runner():
    runtime = FakeRuntime()
    key = Key()
    runtime.add_runner("pool-1", key)
    named = runtime.add_job(repo="proj", session="s1",
                            branch="agent/12-fix-login")
    bad = runtime.add_job(repo="other", session="s2", branch="main")
    jobs = claim(runtime, "pool-1", key)["data"]["jobs"]
    refs = {job["jobId"]: job["pushRef"] for job in jobs}
    assert refs[named] == "refs/heads/agent/12-fix-login"
    assert refs[bad] == "refs/heads/agent/s2"
    lease = next(job for job in jobs if job["jobId"] == named)["leaseId"]
    report(runtime, "pool-1", key, {
        "jobId": named, "leaseId": lease, "securityVerdict": "approved",
        "status": "running", "runnerName": "spoofed"})
    assert runtime.applied[-1][3]["runnerName"] == "pool-1"


def test_agent_model_grant_message_golden_and_rejects():
    key = Key()
    args = ["pool-1", key.public, "core-a", "proj", "refs/heads/agent/s1",
            "claude-code", "claude-opus-5-5", "lease-0001", 10, 20]
    assert edge_routing.agent_model_grant_message(*args) == "\n".join([
        "forkmesh-agent-model-grant-v1", "pool-1", key.public, "core-a",
        "proj", "refs/heads/agent/s1", "claude-code", "claude-opus-5-5",
        "lease-0001", "10", "20"])
    for index, bad in ((1, "short"), (4, "refs/heads/main"), (5, "gemini"),
                       (6, "bad model"), (7, "x"), (9, 10)):
        changed = list(args)
        changed[index] = bad
        assert edge_routing.agent_model_grant_message(*changed) == ""


def test_pool_claims_carry_a_router_signed_model_grant():
    runtime = FakeRuntime()
    key = Key()
    runtime.add_runner("pool-1", key)
    runtime.add_job(session="m1", model="claude-sonnet-5")
    [leased] = claim(runtime, "pool-1", key)["data"]["jobs"]
    assert decode_grant(leased["modelGrant"]) == [
        "forkmesh-agent-model-grant-v1", "pool-1", key.public, "core-a",
        "proj", "refs/heads/agent/m1", "claude-code", "claude-sonnet-5",
        leased["leaseId"], str(NOW), str(NOW + 7_200_000)]


def test_model_grants_name_the_model_that_runs_and_skip_personal_runners():
    runtime = FakeRuntime()
    pool_key, own_key = Key(), Key()
    runtime.add_runner("pool-1", pool_key)
    runtime.add_runner("alice-box", own_key, scope="personal", owners=["alice"])
    runtime.add_job(repo="r1", session="d1")
    [default] = claim(runtime, "pool-1", pool_key)["data"]["jobs"]
    assert decode_grant(default["modelGrant"])[7] == "claude-opus-5-5"
    runtime.add_job(repo="r2", session="d2", provider="codex")
    [codex] = claim(runtime, "pool-1", pool_key)["data"]["jobs"]
    assert "modelGrant" not in codex
    runtime.add_job(repo="r3", session="d3", runner="alice-box",
                    model="claude-opus-5-5")
    [personal] = claim(runtime, "alice-box", own_key)["data"]["jobs"]
    assert "modelGrant" not in personal


# --- Actions jobs (docs/server/runner-actions.md) --------------------------

COMMIT_A = "a" * 40
COMMIT_B = "b" * 40


def enqueue(runtime, owner="alice", repo="proj", commit=COMMIT_A, node="core-a"):
    return asyncio.run(runner_actions.enqueue(
        runtime, owner, repo, "main", commit, node, "alice"))


def actions_report(runtime, name, key, body):
    return call(signed(runtime, name, key, "/api/runners/actions-report", body),
                "/api/runners/actions-report")


def test_a_push_queues_one_actions_job_per_repository_and_commit():
    runtime = FakeRuntime()
    first = enqueue(runtime)
    assert first
    assert enqueue(runtime) == 0
    assert enqueue(runtime, commit=COMMIT_B) == first
    assert runtime.db.execute(
        "SELECT commit_oid FROM repo_action_jobs WHERE id=?",
        (first,)).fetchone()[0] == COMMIT_B
    assert enqueue(runtime, repo="other") not in (0, first)
    assert enqueue(runtime, owner="Bad_Name") == 0
    assert asyncio.run(runner_actions.owner_has_runner(runtime, "alice")) is False
    runtime.add_runner("pool-1", Key())
    assert asyncio.run(runner_actions.owner_has_runner(runtime, "alice")) is True


def test_actions_jobs_are_leased_only_to_runners_that_ask_with_a_read_grant():
    runtime = FakeRuntime()
    key = Key()
    runtime.add_runner("pool-1", key)
    agent_job = runtime.add_job()
    job = enqueue(runtime)
    bob = enqueue(runtime, owner="bob", repo="b1")
    assert claim(runtime, "pool-1", key, {
        "max": 1, "providers": ["claude-code"]})["data"]["jobs"][0][
            "jobId"] == agent_job
    jobs = claim(runtime, "pool-1", key, {
        "max": 1, "providers": [], "actionsMax": 1})["data"]["jobs"]
    [leased] = jobs
    assert leased["kind"] == "actions" and leased["jobId"] == job
    assert leased["commit"] == COMMIT_A and leased["branch"] == "main"
    assert leased["cloneUrl"] == (
        "https://core-a.next.test/v1/runner/alice/proj.git")
    assert "pushGrant" not in leased
    read = decode_grant(leased["readGrant"])
    assert read[1:7] == ["core-a", "alice", "proj", "read", "", "pool-1"]
    assert runtime.db.execute(
        "SELECT status FROM repo_action_jobs WHERE id=?",
        (bob,)).fetchone()[0] == "queued"
    newer = enqueue(runtime, commit=COMMIT_B)
    assert newer != job
    assert claim(runtime, "pool-1", key, {
        "max": 1, "providers": [], "actionsMax": 1})["data"]["jobs"] == []


def test_an_actions_report_stores_runs_the_status_route_serves():
    runtime = FakeRuntime()
    key, other = Key(), Key()
    runtime.add_runner("pool-1", key)
    runtime.add_runner("pool-2", other)
    job = enqueue(runtime)
    [leased] = claim(runtime, "pool-1", key, {
        "max": 1, "providers": [], "actionsMax": 1})["data"]["jobs"]
    run = {"workflow": "CI tests", "commit": COMMIT_A, "ref": "refs/heads/main",
           "status": "success", "createdAt": NOW - 10, "startedAt": NOW - 9,
           "finishedAt": NOW - 1, "logTail": "x" * 20000}
    body = {"jobId": job, "leaseId": leased["leaseId"], "status": "completed",
            "commit": COMMIT_A, "runs": [run, {"workflow": "", "status": "x"}]}
    assert actions_report(runtime, "pool-2", other, body)["status"] == 409
    assert actions_report(runtime, "pool-1", key,
                          dict(body, status="nope"))["status"] == 400
    assert actions_report(runtime, "pool-1", key, body)["status"] == 200
    assert actions_report(runtime, "pool-1", key, body)["status"] == 409
    snapshot = asyncio.run(runner_actions.snapshot(runtime, "alice", "proj"))
    assert snapshot["node"] == "pool-1" and snapshot["source"] == "runner"
    [stored] = snapshot["runs"]
    assert stored["id"] == job * 100 + 1 and stored["status"] == "success"
    assert len(stored["logTail"].encode()) == 16 * 1024
    empty = asyncio.run(runner_actions.snapshot(runtime, "alice", "x"))
    assert empty["ok"] is True and empty["source"] == "runner"
    assert empty["runs"] == [] and empty["controlAvailable"] is False
    assert asyncio.run(runner_actions.snapshot(runtime, "carol", "x")) is None


def test_an_abandoned_actions_lease_is_retried_once_then_failed():
    runtime = FakeRuntime()
    key = Key()
    runtime.add_runner("pool-1", key)
    job = enqueue(runtime)
    for _ in range(2):
        assert claim(runtime, "pool-1", key, {
            "max": 1, "providers": [], "actionsMax": 1})["data"]["jobs"]
        runtime.clock += runner_actions.JOB_TIMEOUT_MS + 1
    assert claim(runtime, "pool-1", key, {
        "max": 1, "providers": [], "actionsMax": 1})["data"]["jobs"] == []
    assert runtime.db.execute(
        "SELECT status FROM repo_action_jobs WHERE id=?",
        (job,)).fetchone()[0] == "failed"
