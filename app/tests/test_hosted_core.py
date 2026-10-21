"""Repositories the Next core nodes host (docs/HOSTED_REPOS.md).

``hosted_git.py`` (create, push routing, quotas, private reads) and
``hosted_repos.py`` (the signed replica exchange the core daemons use) run
here against a real in-memory SQLite database carrying migration 0199, with
the Worker primitives entry.py would bind replaced by small fakes. The node
signature on the replica exchange is a real Ed25519 signature over the
router's canonical request message, the same bytes server/hosted.go sends.
"""

import asyncio
import base64
import hashlib
import importlib
import json
import re
import sqlite3
import sys
from pathlib import Path

import pytest
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import (
    Ed25519PrivateKey,
    Ed25519PublicKey,
)

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import catalog  # noqa: E402
import edge_routing  # noqa: E402
import hosted_git  # noqa: E402
import hosted_repos  # noqa: E402

NOW = 1_790_000_000_000
SESSION_ID = "S" * 32
SESSION_TOKEN = "v2." + SESSION_ID + "." + ("t" * 40)
CORES = ("fmnext-n1", "fmnext-n2", "fmnext-n3")
ROUTER_SEED = b"r" * 32


def b64(raw):
    return base64.urlsafe_b64encode(raw).decode().rstrip("=")


def b64d(text):
    return base64.urlsafe_b64decode(text + "=" * (-len(text) % 4))


NODE_KEYS = {name: Ed25519PrivateKey.from_private_bytes(
    hashlib.sha256(name.encode()).digest()) for name in CORES + ("intruder",)}


def public_b64(key):
    return b64(key.public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw))


class Env:
    ADMIN_PASS = "admin-pass"
    DATA_KEY = "data-key"
    HOSTED_GIT_CORE_NODES = ",".join(CORES)
    HOSTED_REPO_MAX_PER_ACCOUNT = "3"
    HOSTED_REPO_MAX_BYTES = str(10 * 1024 * 1024)


class Headers(dict):
    def get(self, name, default=None):
        for key, value in self.items():
            if key.lower() == name.lower():
                return value
        return default


class Request:
    def __init__(self, body=None, method="POST",
                 url="https://app.next.test/api/hosted-repositories",
                 headers=None, raw=None):
        self.body = dict(body or {})
        self.method = method
        self.url = url
        self.headers = Headers(headers or {})
        self.raw = raw

    async def text(self):
        return (self.raw or b"").decode("utf-8")

    async def bytes(self):
        return self.raw or b""


class Response:
    def __init__(self, data, status, headers=None):
        self.data = data
        self.status = status
        self.headers = headers or {}


class World:
    """One scenario: the database, the session, and everything recorded."""

    def __init__(self, account="alice", owned=("alice",), healthy=CORES,
                 org_roles=None):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.db.executescript("""
            CREATE TABLE repositories (key_bi TEXT PRIMARY KEY, owner_bi TEXT,
                data TEXT, is_private INTEGER);
            CREATE TABLE repo_state_history (key_bi TEXT, state_hash TEXT,
                ts INTEGER, PRIMARY KEY (key_bi, state_hash));
            CREATE TABLE orgs (org_bi TEXT PRIMARY KEY, name TEXT, data TEXT,
                created_at INTEGER);
            CREATE TABLE org_members (org_bi TEXT, member_bi TEXT, role TEXT,
                name TEXT, created_at INTEGER, PRIMARY KEY (org_bi, member_bi));
            CREATE TABLE org_repos (org_bi TEXT, repo TEXT, node_owner TEXT,
                created_at INTEGER, PRIMARY KEY (org_bi, repo));
            CREATE TABLE mirror_https_endpoints (node_name TEXT PRIMARY KEY,
                base_url TEXT, mirror_url TEXT, public_key TEXT,
                registration_sig TEXT, issued_at INTEGER, checked_at INTEGER,
                latency_ms INTEGER, region TEXT, healthy INTEGER,
                integrity TEXT, abuse_blocked INTEGER,
                forkmesh_refs_sha256 TEXT);
            CREATE TABLE runner_request_nonces (runner TEXT, request_id TEXT,
                expires_at INTEGER, PRIMARY KEY (runner, request_id));
        """)
        self.db.executescript(
            (ROOT / "migrations" / "0199_hosted_repositories.sql").read_text())
        for index, node in enumerate(CORES):
            self.db.execute(
                "INSERT INTO mirror_https_endpoints VALUES "
                "(?,?,?,?,?,?,?,?,?,?,?,?,?)",
                (node, "https://n%d.next.test" % (index + 1), "", "K" * 43,
                 "", 0, NOW, 5, "ewr", 1 if node in healthy else 0, "ok", 0,
                 ""))
        self.account = account
        self.owned = set(owned)
        self.org_roles = org_roles or {}
        for org, role in self.org_roles.items():
            self.db.execute("INSERT INTO orgs VALUES (?,?,?,?)",
                            ("bi:org:" + org, org, "{}", NOW))
            self.db.execute(
                "INSERT INTO org_members VALUES (?,?,?,?,?)",
                ("bi:org:" + org, "bi:" + account, role, account, NOW))
        self.audits = []
        self.doorbells = []
        self.fetches = []
        self.upstream = {}
        self.republished = []

    # -- D1 over sqlite ----------------------------------------------------
    async def d1_first(self, _env, sql, *args):
        row = self.db.execute(sql, args).fetchone()
        return dict(row) if row else None

    async def d1_all(self, _env, sql, *args):
        return [dict(row) for row in self.db.execute(sql, args).fetchall()]

    async def d1_run(self, _env, sql, *args):
        cursor = self.db.execute(sql, args)
        return {"meta": {"changes": cursor.rowcount}}

    def catalog(self, owner, name):
        row = self.db.execute(
            "SELECT data,is_private FROM repositories WHERE key_bi=?",
            ("bi:%s/%s" % (owner, name),)).fetchone()
        return (json.loads(row["data"]), row["is_private"]) if row else None

    def hosted(self, owner, name):
        row = self.db.execute(
            "SELECT * FROM hosted_repos WHERE key_bi=?",
            ("bi:%s/%s" % (owner, name),)).fetchone()
        if not row:
            return None
        row = dict(row)
        row["data"] = json.loads(row["data"])
        return row


def install(world):
    """Bind one world's fakes into a freshly loaded hosted_git."""
    importlib.reload(hosted_repos)
    importlib.reload(hosted_git)

    async def _authed_account_name(_env, request, _data=None):
        if getattr(request, "unverified", False):
            raise RuntimeError("email_not_verified")
        return world.account

    async def _account_owns_node(_env, actor, owner):
        # Only alice's session owns the alice namespace (and her others).
        return bool(actor) and actor == "alice" and owner in world.owned

    async def _account_row(_env, owner):
        return "bi:" + owner, {"name": owner}

    async def blind_index(_env, value):
        return "bi:" + str(value).strip().lower()

    async def encrypt_row(_env, record):
        return json.dumps(record, sort_keys=True)

    async def decrypt_row(_env, value):
        return json.loads(value) if value else None

    async def _contribution_write_catalog_state(
            _env, record, owner_bi, _rec, key_bi, encrypted):
        world.db.execute(
            "INSERT INTO repositories VALUES (?,?,?,?) ON CONFLICT(key_bi) DO "
            "UPDATE SET data=excluded.data,is_private=excluded.is_private",
            (key_bi, owner_bi, encrypted,
             1 if record.get("visibility") == "private" else 0))

    async def _org_row(_env, org):
        row = world.db.execute(
            "SELECT name,data,created_at FROM orgs WHERE org_bi=?",
            ("bi:org:" + org,)).fetchone()
        return "bi:org:" + org, (dict(row) if row else None)

    async def _org_permission(_env, org_bi, account):
        row = world.db.execute(
            "SELECT role FROM org_members WHERE org_bi=? AND member_bi=?",
            (org_bi, "bi:" + account)).fetchone()
        role = row["role"] if row else ""
        return {"owner": "admin", "admin": "admin", "maintainer": "maintain",
                "member": "read"}.get(role, "")

    ranks = ("read", "triage", "write", "maintain", "admin")

    async def notify_repo_host(_env, node, repo, topic, event_owner=""):
        world.doorbells.append((node, event_owner + "/" + repo, topic))
        return True

    async def _audit_sensitive_action(_env, actor, action, *rest):
        world.audits.append((actor, action) + tuple(rest[:3]))

    async def ed25519_sign(public_key, seed, message):
        key = Ed25519PrivateKey.from_private_bytes(b64d(seed))
        return b64(key.sign(message))

    async def ed25519_verify(public_key, signature, message):
        try:
            Ed25519PublicKey.from_public_bytes(b64d(public_key)).verify(
                b64d(signature), message)
            return True
        except Exception:
            return False

    async def _claimed_node_signing_pubkeys(_env, name):
        key = NODE_KEYS.get(name)
        return [public_b64(key)] if key else []

    class _Upstream:
        def __init__(self, status, body=b"upstream-bytes"):
            self.status = status
            self.body = body
            self.headers = Headers({"content-type": "application/x-test"})

    async def js_fetch_until_response(target, init, _timeout):
        world.fetches.append((target, init))
        node = init["headers"]["X-ForkMesh-Node"]
        return _Upstream(world.upstream.get(node, 200))

    async def noop(*_args, **_kwargs):
        return None

    async def schedule_public_catalog_republish(_env, owner, name):
        world.republished.append((owner, name))

    async def _https_mirror_repository_proof(
            _env, endpoint, context, operation, force_refresh=False,
            details=None, **_kwargs):
        if isinstance(details, dict):
            details.update({"reason": "refs_mismatch",
                            "refsSha256": world.proof_digest})
        return False

    world.proof_digest = "d" * 64
    primitives = {
        "urlparse": __import__("urllib.parse", fromlist=["urlparse"]).urlparse,
        "base64": base64,
        "clean_string": catalog.clean_string,
        "safe_segment": catalog.safe_segment,
        "MAX_NODE_NAME": 63,
        "MAX_REPO_SEGMENT": catalog.MAX_REPO_SEGMENT,
        "valid_node_name": lambda value: bool(re.fullmatch(
            r"[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?", (value or "").strip())),
        "safe_catalog_record": catalog.safe_catalog_record,
        "json_response": lambda data, status=200, extra_headers=None, **_k: (
            Response(data, status, dict(extra_headers or {}))),
        "method_name": lambda request: request.method,
        "bounded_json_request": lambda request: _async(dict(request.body)),
        "_authed_account_name": _authed_account_name,
        "_account_owns_node": _account_owns_node,
        "_org_write_allowed": lambda *_a: _async(False),
        "_is_blocked_catalog_identity": lambda _env, owner, name: (
            name == "blocked-name"),
        "_hosted_git_write_endpoint": lambda _env: _async(None),
        "_account_row": _account_row,
        "blind_index": blind_index,
        "d1_first": lambda env, sql, *a: world.d1_first(env, sql, *a),
        "d1_first_primary": lambda env, sql, *a: _async({
            "account_bi": "bi:" + world.account, "revoked_at": 0,
            "expires_at": NOW + 60_000}),
        "d1_all": lambda env, sql, *a: world.d1_all(env, sql, *a),
        "d1_run": lambda env, sql, *a: world.d1_run(env, sql, *a),
        "decrypt_row": decrypt_row,
        "encrypt_row": encrypt_row,
        "_https_mirror_repository_proof": _https_mirror_repository_proof,
        "STATE_PIN_HISTORY": 100,
        "_repo_delete_tombstone_active": lambda *_a: _async(False),
        "_clear_repo_delete_tombstone": noop,
        "CATALOG_MAX_RECORDS_PER_OWNER": 100,
        "catalog_rate_check": lambda *_a: _async(None),
        "_contribution_write_catalog_state": (
            _contribution_write_catalog_state),
        "_https_mirror_public_context_cache_clear": lambda: None,
        "edge_cache_delete": noop,
        "CATALOG_CACHE_KEY": "catalog-cache",
        "_bump_catalog_rev": noop,
        "schedule_public_catalog_republish": schedule_public_catalog_republish,
        "queue_repo_webhook_event": noop,
        "notify_repo_activity": noop,
        "_audit_sensitive_action": _audit_sensitive_action,
        "ensure_schema": noop,
        "Date": type("Date", (), {"now": staticmethod(lambda: NOW)}),
        "_account_session_secret": lambda env: (
            env.ADMIN_PASS + "|" + env.DATA_KEY).encode(),
        "_request_account_session_id": lambda request, data: (
            SESSION_ID if (data or {}).get("sessionToken") == SESSION_TOKEN
            else ""),
        "EXPECTED_DEGRADED_HEADERS": {"X-ForkMesh-Expected-Degraded": "1"},
        "_forget_repo_closed": lambda _owner, _name: None,
        "_org_row": _org_row,
        "_org_permission": _org_permission,
        "team_permission_rank": lambda p: (
            ranks.index(p) if p in ranks else -1),
        "MAX_ORG_REPOS": 200,
        "notify_repo_host": notify_repo_host,
        "_random_bytes": lambda n: hashlib.sha256(
            str(len(world.audits)).encode()).digest()[:n],
        "_b64url_encode": b64,
        "https_routing": edge_routing,
        "_https_mirror_endpoint_projection": lambda row: {
            "node": row.get("node_name"), "baseUrl": row.get("base_url"),
            "publicKey": row.get("public_key"),
            "healthy": bool(row.get("healthy")),
            "abuseBlocked": bool(row.get("abuse_blocked"))},
        "_https_mirror_router_public_key": lambda _env: public_b64(
            Ed25519PrivateKey.from_private_bytes(ROUTER_SEED)),
        "_https_mirror_router_seed": lambda _env: b64(ROUTER_SEED),
        "ed25519_sign": ed25519_sign,
        "ed25519_verify": ed25519_verify,
        "_claimed_node_signing_pubkeys": _claimed_node_signing_pubkeys,
        "_https_mirror_request_query": lambda url, op, release_sha="": (
            {"service": "git-upload-pack"} if op == "git-info-refs" else {}),
        "HTTPS_MIRROR_BODY_MAX_BYTES": 8 * 1024 * 1024,
        "HTTPS_MIRROR_RETRY_STATUSES": frozenset({408, 429}),
        "HTTPS_MIRROR_GIT_INITIAL_RESPONSE_TIMEOUT_SECONDS": 20,
        "js_fetch_until_response": js_fetch_until_response,
        "Uint8Array": type("U8", (), {"new": staticmethod(lambda v: v)}),
        "_to_js": lambda value, **_k: value,
        "to_js": lambda value: value,
        "JsResponse": type("JsResponse", (), {"new": staticmethod(
            lambda body, init: Response(body, init["status"],
                                        init["headers"]))}),
    }
    for name, value in primitives.items():
        setattr(hosted_git, name, value)
    return hosted_git


def _async(value):
    async def inner():
        return value
    return inner()


def create(module, body=None, **kwargs):
    payload = {"name": "app", "sessionToken": SESSION_TOKEN}
    payload.update(body or {})
    return asyncio.run(module.hosted_repository_handler(
        Env(), Request(payload, **kwargs), "/api/hosted-repositories"))


def signed_replicas(module, node, reports, request_id="request_0123456789ab",
                    issued_at=NOW, key=None):
    raw = json.dumps({"repositories": reports}).encode()
    digest = hashlib.sha256(raw).hexdigest()
    message = edge_routing.request_message(
        node, "POST", hosted_repos.REPLICAS_PATH, digest, request_id,
        issued_at)
    signature = b64((key or NODE_KEYS[node]).sign(message.encode()))
    request = Request(
        method="POST", raw=raw,
        url="https://app.next.test" + hosted_repos.REPLICAS_PATH,
        headers={
            "X-ForkMesh-Node": node,
            "X-ForkMesh-Request-Id": request_id,
            "X-ForkMesh-Issued-At": str(issued_at),
            "X-ForkMesh-Body-Sha256": digest,
            "X-ForkMesh-Signature": signature,
        })
    return asyncio.run(module.hosted_repository_handler(
        Env(), request, hosted_repos.REPLICAS_PATH))


# ---------------------------------------------------------------------------
# Pure helpers
# ---------------------------------------------------------------------------


def test_rendezvous_is_stable_and_spreads_primaries():
    order = hosted_repos.rendezvous(list(CORES), "hr" + "0" * 24)
    assert sorted(order) == sorted(CORES)
    assert hosted_repos.rendezvous(list(reversed(CORES)), "hr" + "0" * 24) \
        == order
    # Removing a node never reorders the others (only its repos move).
    survivors = [n for n in order if n != order[0]]
    assert hosted_repos.rendezvous(survivors, "hr" + "0" * 24) == survivors
    primaries = {hosted_repos.rendezvous(list(CORES), "hr%024x" % i)[0]
                 for i in range(60)}
    assert primaries == set(CORES)


@pytest.mark.parametrize("url,expected", [
    ("https://github.com/owner/repo.git", "https://github.com/owner/repo.git"),
    ("https://gitlab.com/group/sub/repo/", "https://gitlab.com/group/sub/repo"),
    ("http://github.com/owner/repo", ""),
    ("https://user:pw@github.com/owner/repo", ""),
    ("https://github.com:444/owner/repo", ""),
    ("https://github.com/owner", ""),
    ("https://evil.example/owner/repo", ""),
    ("https://github.com/owner/repo?x=1", ""),
    ("https://github.com/../etc/passwd", ""),
])
def test_import_urls_are_public_https_on_allowed_forges(url, expected):
    assert hosted_repos.import_url(
        url, hosted_repos.DEFAULT_IMPORT_HOSTS) == expected


def test_init_is_deterministic_and_bounded():
    init, error = hosted_repos.build_init(
        "alice", "app", "Notes", readme=True, gitignore="python",
        license_id="mit", author="alice",
        author_email="alice@users.noreply.next.test", created_at_ms=NOW)
    assert error == ""
    paths = [f["path"] for f in init["files"]]
    assert paths == ["README.md", ".gitignore", "LICENSE"]
    assert init["files"][0]["content"] == "# app\n\nNotes\n"
    assert "Copyright (c) 2026 alice" in init["files"][2]["content"]
    assert init["time"] == NOW // 1000
    again, _ = hosted_repos.build_init(
        "alice", "app", "Notes", readme=True, gitignore="python",
        license_id="mit", author="alice",
        author_email="alice@users.noreply.next.test", created_at_ms=NOW)
    assert again == init
    assert hosted_repos.build_init(
        "alice", "app", "", readme=False, gitignore="", license_id="",
        author="a", author_email="e", created_at_ms=NOW) == (None, "")
    assert hosted_repos.build_init(
        "alice", "app", "", readme=True, gitignore="cobol", license_id="",
        author="a", author_email="e", created_at_ms=NOW)[1] == \
        "invalid_gitignore"
    assert hosted_repos.noreply_email(
        "alice", "https://app.forkmesh.com") == \
        "alice@users.noreply.forkmesh.com"


@pytest.mark.parametrize("name,ok", [
    ("app", True), ("my.app-2", True), (".hidden", False),
    ("repo.git", False), ("a" * 65, False), ("sp ace", False),
])
def test_names(name, ok):
    assert hosted_repos.valid_name(name) is ok


# ---------------------------------------------------------------------------
# Create
# ---------------------------------------------------------------------------


def test_create_public_with_readme_on_the_core():
    world = World()
    module = install(world)
    response = create(module, {"description": "Notes", "gitignore": "node",
                                "license": "mit"})
    assert response.status == 201, response.data
    record = response.data["repository"]
    assert record["visibility"] == "public"
    assert record["source"] == "hosted-web"
    assert record["cloneUrl"] == "https://app.next.test/alice/app"
    hosted = world.hosted("alice", "app")
    assert hosted["visibility"] == "public"
    assert hosted["creator_bi"] == "bi:alice"
    assert re.fullmatch(r"hr[0-9a-f]{24}", hosted["id"])
    init = hosted["data"]["init"]
    assert [f["path"] for f in init["files"]] == [
        "README.md", ".gitignore", "LICENSE"]
    assert init["authorEmail"] == "alice@users.noreply.next.test"
    # The remote has a first commit, so the instructions start from a clone.
    push = response.data["push"]
    assert push["commands"] == [
        "git clone https://app.next.test/alice/app", "cd app"]
    assert push["username"] == "alice"
    assert response.data["hosting"]["nodes"] == list(CORES)
    assert response.data["hosting"]["primary"] in CORES
    assert response.data["quota"] == {
        "used": 1, "limit": 3, "repositoryBytes": 10 * 1024 * 1024}
    # Every core daemon is woken for the new repository.
    assert sorted(world.doorbells) == sorted(
        (node, "alice/app", "hosted") for node in CORES)
    assert ("alice", "repository.create_hosted") in [
        audit[:2] for audit in world.audits]


def test_create_without_readme_keeps_the_push_commands():
    world = World()
    module = install(world)
    response = create(module, {"readme": False})
    assert response.status == 201
    assert world.hosted("alice", "app")["data"]["init"] is None
    assert "git push -u forkmesh main" in response.data["push"]["commands"]


def test_create_private_is_access_controlled_not_public():
    world = World()
    module = install(world)
    response = create(module, {"visibility": "private"})
    assert response.status == 201, response.data
    data, is_private = world.catalog("alice", "app")
    assert data["visibility"] == "private" and is_private == 1
    assert world.hosted("alice", "app")["visibility"] == "private"


def test_create_import_from_an_allowed_forge():
    world = World()
    module = install(world)
    bad = create(module, {"importUrl": "https://evil.example/a/b"})
    assert bad.status == 400 and bad.data["error"] == "invalid_import_url"
    response = create(module, {"importUrl": "https://github.com/a/b.git"})
    assert response.status == 201
    init = world.hosted("alice", "app")["data"]["init"]
    assert init["importUrl"] == "https://github.com/a/b.git"
    assert init["files"] == []
    assert response.data["hosting"]["importing"] is True


def test_first_core_repository_emits_the_activation_event_once():
    world = World()
    module = install(world)
    emitted = []

    async def emit_for_user(_env, user_bi, event_name, metadata=None):
        emitted.append((user_bi, event_name, metadata))
        return True

    module._attribution = type(
        "Attribution", (), {"emit_for_user": staticmethod(emit_for_user)})
    response = create(module, {"importUrl": "https://github.com/a/b.git"})
    assert response.status == 201
    assert emitted == [
        ("bi:alice", "first_mirror_created", {"repo": "alice/app"})]
    assert create(module, {"name": "second"}).status == 201
    assert len(emitted) == 1


def test_quota_per_account():
    world = World()
    module = install(world)
    for index in range(3):
        assert create(module, {"name": "r%d" % index}).status == 201
    response = create(module, {"name": "r3"})
    assert response.status == 429
    assert response.data["error"] == "hosted_quota_exceeded"
    assert response.data["limit"] == 3
    assert world.catalog("alice", "r3") is None


def test_create_needs_an_owned_namespace_and_a_verified_session():
    world = World()
    module = install(world)
    assert create(module, {"owner": "bob"}).status == 403
    world.account = ""
    assert create(module).status == 401
    world.account = "alice"
    request = Request({"name": "app", "sessionToken": SESSION_TOKEN})
    request.unverified = True
    with pytest.raises(RuntimeError, match="email_not_verified"):
        asyncio.run(module.hosted_repository_handler(
            Env(), request, "/api/hosted-repositories"))
    assert create(module, {"sessionToken": ""}).status == 401
    assert create(module).status == 201
    assert create(module).status == 409


def test_organization_owner_needs_maintain():
    world = World(org_roles={"acme": "maintainer", "club": "member"})
    module = install(world)
    denied = create(module, {"organization": "club"})
    assert denied.status == 403
    response = create(module, {"organization": "acme", "visibility": "private"})
    assert response.status == 201, response.data
    assert response.data["repository"]["cloneUrl"] == \
        "https://app.next.test/acme/app"
    link = world.db.execute("SELECT * FROM org_repos").fetchall()
    assert [tuple(row)[:3] for row in link] == [("bi:org:acme", "app", "alice")]
    hosted = world.hosted("alice", "app")
    assert hosted["data"]["aliases"] == ["acme"]
    assert create(module, {"organization": "nope"}).status == 404


def test_organization_alias_of_a_mixed_case_name_is_stored_lowercase():
    world = World(org_roles={"acme": "maintainer"})
    module = install(world)
    response = create(module, {"name": "Bolt", "organization": "acme"})
    assert response.status == 201, response.data
    assert response.data["repository"]["cloneUrl"] == \
        "https://app.next.test/acme/Bolt"
    link = world.db.execute("SELECT * FROM org_repos").fetchall()
    assert [tuple(row)[:3] for row in link] == [("bi:org:acme", "bolt", "alice")]
    assert create(module, {"name": "bolt", "organization": "acme"}).status == 409


def test_linking_again_repairs_a_mixed_case_organization_alias():
    world = World(org_roles={"acme": "maintainer"})
    module = install(world)
    world.db.execute("INSERT INTO org_repos VALUES (?,?,?,?)",
                     ("bi:org:acme", "Bolt", "alice", NOW))
    runtime = module._ReplicaRuntime(Env())
    asyncio.run(hosted_repos.link_organization(
        runtime, "bi:org:acme", "Bolt", "alice", NOW + 1))
    asyncio.run(hosted_repos.link_organization(
        runtime, "bi:org:acme", "Bolt", "alice", NOW + 2))
    link = world.db.execute("SELECT * FROM org_repos").fetchall()
    assert [tuple(row) for row in link] == [
        ("bi:org:acme", "bolt", "alice", NOW)]


def test_settings_describe_core_hosting():
    world = World(org_roles={"acme": "admin", "club": "member"})
    module = install(world)
    create(module)
    response = asyncio.run(module.hosted_repository_handler(
        Env(), Request(method="GET"), "/api/hosted-repositories"))
    body = response.data
    assert body["mode"] == "core" and body["privateSupported"] is True
    assert body["used"] == 1
    assert body["limits"]["repositoriesPerAccount"] == 3
    assert body["organizations"] == ["acme"]
    assert "mit" in body["licenseTemplates"]
    assert "github.com" in body["importHosts"]


# ---------------------------------------------------------------------------
# The replica exchange
# ---------------------------------------------------------------------------


def _created(world, module, body=None):
    assert create(module, body).status == 201
    return world.hosted("alice", "app")["id"]


def test_replicas_require_a_core_signature_and_a_fresh_request_id():
    world = World()
    module = install(world)
    _created(world, module)
    assert signed_replicas(module, "intruder", {}).status == 401
    forged = signed_replicas(module, "fmnext-n1", {},
                             key=NODE_KEYS["fmnext-n2"])
    assert forged.status == 401
    stale = signed_replicas(module, "fmnext-n1", {}, issued_at=NOW - 120_000,
                            request_id="request_stale_000001")
    assert stale.status == 401
    ok = signed_replicas(module, "fmnext-n1", {})
    assert ok.status == 200
    # The same request id is single-use.
    assert signed_replicas(module, "fmnext-n1", {}).status == 401


def test_primary_initializes_and_replicas_fetch_with_grants():
    world = World()
    module = install(world)
    repo_id = _created(world, module)
    primary = hosted_repos.rendezvous(list(CORES), repo_id)[0]
    replica = [n for n in CORES if n != primary][0]

    answer = signed_replicas(module, primary, {}).data
    item = answer["repositories"][0]
    assert item["id"] == repo_id and item["primary"] is True
    assert item["owner"] == "alice" and item["name"] == "app"
    assert [f["path"] for f in item["init"]["files"]] == ["README.md"]
    # An empty primary has nothing to fetch from yet.
    assert item.get("sources", []) == []

    # The primary initializes and reports its content: that becomes the pin
    # and wakes the other cores.
    world.doorbells.clear()
    digest = "a" * 64
    signed_replicas(module, primary, {repo_id: {
        "pushedAt": 0, "empty": False, "routingDigest": digest,
        "sizeBytes": 1234}}, request_id="request_0123456789ac")
    assert world.catalog("alice", "app")[0]["stateHash"] == digest
    assert len(world.doorbells) == 3

    answer = signed_replicas(module, replica, {repo_id: {
        "pushedAt": 0, "empty": True, "sizeBytes": 0}},
        request_id="request_0123456789ad").data
    item = answer["repositories"][0]
    assert item["primary"] is False and "init" not in item
    sources = item["sources"]
    assert sources[0]["node"] == primary
    assert all(s["node"] != replica for s in sources)
    assert sources[0]["url"].endswith("/v1/runner/alice/app.git")
    # The grant is the runner grant format, for the source node, naming the
    # asking core, read-only, signed by the router key.
    encoded, signature = sources[0]["grant"].split(".")
    message = b64d(encoded).decode()
    fields = message.split("\n")
    assert fields[:7] == ["forkmesh-runner-grant-v1", primary, "alice", "app",
                          "read", "", replica]
    Ed25519PrivateKey.from_private_bytes(ROUTER_SEED).public_key().verify(
        b64d(signature), message.encode())


def test_a_push_makes_the_others_fetch_but_not_the_pusher():
    world = World()
    module = install(world)
    repo_id = _created(world, module, {"readme": False})
    pusher, other = CORES[1], CORES[2]
    world.db.execute("UPDATE hosted_repos SET pushed_at=?,pushed_by=?",
                     (NOW, pusher))
    state = {repo_id: {"pushedAt": 5, "empty": False,
                       "routingDigest": "b" * 64, "sizeBytes": 10}}
    at_pusher = signed_replicas(module, pusher, state).data["repositories"][0]
    assert at_pusher.get("sources", []) == []
    assert at_pusher["pushedAt"] == NOW
    at_other = signed_replicas(module, other, state,
                               request_id="request_0123456789ae").data
    sources = at_other["repositories"][0]["sources"]
    assert sources[0]["node"] == pusher
    # Caught up: nothing more to fetch.
    state[repo_id]["pushedAt"] = NOW
    caught_up = signed_replicas(module, other, state,
                                request_id="request_0123456789af").data
    assert caught_up["repositories"][0].get("sources", []) == []
    # Copies were recorded per node; the quota reads the largest.
    rows = world.db.execute(
        "SELECT node,size_bytes FROM hosted_repo_copies ORDER BY node"
    ).fetchall()
    assert [tuple(r) for r in rows] == [(pusher, 10), (other, 10)]


def _stats(commit, count, weeks=None):
    return {"commit": commit, "commitCount": count,
            "mergeCount": 2, "activityWeeks": weeks or [0] * 51 + [5],
            "mergeActivityWeeks": [0] * 51 + [2]}


def test_a_current_copy_reports_the_true_commit_count_and_activity():
    world = World()
    module = install(world)
    repo_id = _created(world, module)
    primary = hosted_repos.rendezvous(list(CORES), repo_id)[0]
    digest = "a" * 64
    report = {"pushedAt": 0, "empty": False, "routingDigest": digest,
              "sizeBytes": 1234, **_stats("1" * 40, 45204)}
    signed_replicas(module, primary, {repo_id: report})
    record = world.catalog("alice", "app")[0]
    assert record["stateHash"] == digest
    assert record["commitCount"] == "45204"
    assert record["mergeCount"] == "2"
    assert record["activityWeeks"] == [0] * 51 + [5]
    assert record["mergeActivityWeeks"] == [0] * 51 + [2]


def test_stats_of_a_copy_behind_the_pin_never_reach_the_catalog():
    world = World()
    module = install(world)
    repo_id = _created(world, module)
    primary = hosted_repos.rendezvous(list(CORES), repo_id)[0]
    other = [n for n in CORES if n != primary][0]
    signed_replicas(module, primary, {repo_id: {
        "pushedAt": 0, "empty": False, "routingDigest": "a" * 64,
        "sizeBytes": 1, **_stats("1" * 40, 10)}})
    signed_replicas(module, other, {repo_id: {
        "pushedAt": 0, "empty": False, "routingDigest": "b" * 64,
        "sizeBytes": 1, **_stats("2" * 40, 99)}},
        request_id="request_0123456789ac")
    assert world.catalog("alice", "app")[0]["commitCount"] == "10"


def test_report_stats_are_bounded_and_need_a_commit():
    clean = hosted_repos.clean_stats
    assert clean(_stats("1" * 40, 7))["activityWeeks"][-1] == 5
    assert clean({**_stats("1" * 40, 7), "activityWeeks": [1, 2]})[
        "activityWeeks"] == [0] * 50 + [1, 2]
    assert clean(_stats("short", 7)) is None
    assert clean(_stats("1" * 40, 0)) is None
    assert clean(_stats("1" * 40, 10 ** 10)) is None
    assert clean({**_stats("1" * 40, 7), "activityWeeks": [0] * 53}) is None
    assert clean({**_stats("1" * 40, 7), "activityWeeks": [-1]}) is None
    no_merges = clean({**_stats("1" * 40, 7), "mergeCount": 9})
    assert "mergeCount" not in no_merges
    assert "mergeActivityWeeks" not in no_merges


def test_a_deleted_catalog_row_is_removed_everywhere():
    world = World()
    module = install(world)
    repo_id = _created(world, module)
    world.db.execute("DELETE FROM repositories")
    answer = signed_replicas(module, CORES[0], {}).data
    assert answer["repositories"] == []
    assert answer["removed"] == [
        {"id": repo_id, "owner": "alice", "name": "app"}]
    assert world.hosted("alice", "app")["deleted_at"] == NOW
    # The name is free again, and the quota counts only live repositories.
    assert create(module).status == 201


def test_a_node_publication_taking_the_record_over_releases_the_copies():
    """Moving to your own node: once its signed publication replaces the
    relay-authored record, the cores drop their copies and the quota frees."""
    world = World()
    module = install(world)
    repo_id = _created(world, module)
    data, _ = world.catalog("alice", "app")
    data["source"] = "local-node"
    world.db.execute("UPDATE repositories SET data=?", (json.dumps(data),))
    answer = signed_replicas(module, CORES[1], {}).data
    assert answer["removed"] == [
        {"id": repo_id, "owner": "alice", "name": "app"}]
    assert asyncio.run(module.hosted_push_candidates(
        Env(), "alice", "app")) is None
    settings = asyncio.run(module.hosted_repository_handler(
        Env(), Request(method="GET"), "/api/hosted-repositories")).data
    assert settings["used"] == 0


# ---------------------------------------------------------------------------
# Push routing, size limit, private reads
# ---------------------------------------------------------------------------


def test_push_candidates_follow_the_last_pusher_then_rendezvous():
    world = World(healthy=CORES[:2])
    module = install(world)
    repo_id = _created(world, module)
    candidates = asyncio.run(module.hosted_push_candidates(
        Env(), "alice", "app"))
    nodes = [c["node"] for c in candidates]
    live = hosted_repos.rendezvous(list(CORES[:2]), repo_id)
    assert nodes == live + [CORES[2]]
    world.db.execute("UPDATE hosted_repos SET pushed_by=?", (live[1],))
    candidates = asyncio.run(module.hosted_push_candidates(
        Env(), "alice", "app"))
    assert [c["node"] for c in candidates][0] == live[1]
    assert asyncio.run(module.hosted_push_candidates(
        Env(), "alice", "other")) is None


def test_push_size_limit():
    world = World()
    module = install(world)
    repo_id = _created(world, module)
    request = Request(headers={"content-length": "100"})
    assert asyncio.run(module.hosted_push_limit(
        Env(), request, "alice", "app", "POST")) is None
    world.db.execute(
        "INSERT INTO hosted_repo_copies VALUES (?,?,?,?,?,?,?)",
        (repo_id, CORES[0], 0, 0, "", 10 * 1024 * 1024, NOW))
    refused = asyncio.run(module.hosted_push_limit(
        Env(), request, "alice", "app", "POST"))
    assert refused.status == 413
    assert refused.data["error"] == "repository_quota_exceeded"
    big = Request(headers={"content-length": str(11 * 1024 * 1024)})
    world.db.execute("DELETE FROM hosted_repo_copies")
    assert asyncio.run(module.hosted_push_limit(
        Env(), big, "alice", "app", "POST")).status == 413
    # The advertisement (GET) is never refused.
    assert asyncio.run(module.hosted_push_limit(
        Env(), big, "alice", "app", "GET")) is None


def test_record_push_notes_the_pusher_even_when_the_pin_is_unchanged():
    world = World()
    module = install(world)
    _created(world, module, {"readme": False})
    world.doorbells.clear()
    endpoint = {"node": CORES[2], "baseUrl": "https://n3.next.test"}
    digest = asyncio.run(module.record_hosted_push_state(
        Env(), "alice", "app", endpoint))
    assert digest == world.proof_digest
    hosted = world.hosted("alice", "app")
    assert hosted["pushed_by"] == CORES[2] and hosted["pushed_at"] == NOW
    assert len(world.doorbells) == 3
    # A second push that leaves the default branch alone still moves pushed_at
    # and wakes the replicas.
    world.doorbells.clear()
    asyncio.run(module.record_hosted_push_state(
        Env(), "alice", "app", endpoint))
    assert len(world.doorbells) == 3


def _git_read(module, headers=None, operation="git-info-refs"):
    url = ("https://app.next.test/alice/app/info/refs?service=git-upload-pack"
           if operation == "git-info-refs"
           else "https://app.next.test/api/repo/alice/app/tree")
    return asyncio.run(module.hosted_private_read(
        Env(), Request(method="GET", url=url, headers=headers),
        "alice", "app", operation))


def test_private_reads_need_a_reader_and_use_a_capability():
    world = World()
    module = install(world)
    repo_id = _created(world, module, {"visibility": "private"})
    order = hosted_repos.rendezvous(list(CORES), repo_id)
    # Anonymous git gets a Basic challenge; the website gets a plain 404.
    world.account = ""
    challenge = _git_read(module)
    assert challenge.status == 401
    assert challenge.headers["WWW-Authenticate"] == 'Basic realm="forkmesh"'
    assert _git_read(module, operation="tree").status == 404
    # Someone else's session is not a reader.
    world.account = "mallory"
    assert _git_read(module, operation="tree").status == 404
    # The owner's hosted credential clones; the first candidate that has the
    # repository answers (a 404 is a replica still materializing it).
    world.account = "alice"
    password = module.hosted_push_password(
        Env(), "alice", "alice", "app", SESSION_ID, NOW + 60_000)
    header = "Basic " + base64.b64encode(
        ("alice:" + password).encode()).decode()
    world.upstream = {order[0]: 404}
    response = _git_read(module, {"authorization": header})
    assert response.status == 200
    assert response.headers["cache-control"] == "private, no-store"
    targets = [target for target, _init in world.fetches]
    assert targets[0].startswith("https://n%d.next.test/v1/repositories/"
                                 "alice/app/git-info-refs" % (
                                     CORES.index(order[0]) + 1))
    assert world.fetches[1][1]["headers"]["X-ForkMesh-Node"] == order[1]
    # The capability is the router's signature over the exact target.
    init = world.fetches[1][1]
    message = edge_routing.request_message(
        order[1], "GET",
        "/v1/repositories/alice/app/git-info-refs?service=git-upload-pack",
        hashlib.sha256(b"").hexdigest(),
        init["headers"]["X-ForkMesh-Request-Id"], NOW)
    Ed25519PrivateKey.from_private_bytes(ROUTER_SEED).public_key().verify(
        b64d(init["headers"]["X-ForkMesh-Signature"]), message.encode())
    # A public hosted repository never takes this path.
    assert _created is not None
    world2 = World()
    module2 = install(world2)
    _created(world2, module2)
    assert asyncio.run(module2.hosted_private_read(
        Env(), Request(method="GET"), "alice", "app", "git-info-refs")) is None


def test_private_pulse_reads_answer_exactly_like_the_other_reads():
    world = World()
    module = install(world)
    _created(world, module, {"visibility": "private"})

    def read(operation):
        return asyncio.run(module.hosted_private_read(
            Env(), Request(method="GET", url=(
                "https://app.next.test/api/repo/alice/app/%s?period=1w"
                % operation)), "alice", "app", operation))
    for account in ("", "mallory"):
        world.account = account
        pulse, stats = read("pulse"), read("stats")
        assert pulse.status == stats.status == 404
        assert pulse.data == stats.data == {"error": "not_found"}
    world.account = "alice"
    world.fetches.clear()
    response = read("pulse")
    assert response.status == 200
    assert response.headers["cache-control"] == "private, no-store"
    assert "/v1/repositories/alice/app/pulse" in world.fetches[0][0]


def test_org_readers_can_read_a_private_org_repository():
    world = World(org_roles={"acme": "maintainer"})
    module = install(world)
    assert create(module, {"organization": "acme",
                           "visibility": "private"}).status == 201
    world.account = "bob"
    world.db.execute("INSERT INTO org_members VALUES (?,?,?,?,?)",
                     ("bi:org:acme", "bi:bob", "member", "bob", NOW))
    assert _git_read(module, operation="tree").status == 200
    world.db.execute("DELETE FROM org_members WHERE member_bi='bi:bob'")
    assert _git_read(module, operation="tree").status == 404


def test_worker_wiring():
    entry = (SRC / "entry.py").read_text(encoding="utf-8")
    runtime = (SRC / "app_runtime.py").read_text(encoding="utf-8")
    assert 'hosted_push_candidates = _hosted_git.export(' in entry
    assert 'globals().get("hosted_push_candidates")' in entry
    assert 'globals().get("hosted_push_limit")' in entry
    assert 'if target.get("source") == "hosted-web" and callable(' in entry
    assert runtime.count('globals().get("hosted_private_read")') == 2
    toml = (ROOT / "wrangler.toml").read_text(encoding="utf-8")
    assert 'HOSTED_GIT_CORE_NODES = "fmnext-n1,fmnext-n2,fmnext-n3"' in toml


def test_core_owners_add_retired_cores_only_while_hosting_is_on():
    env = type("Env", (), {
        "HOSTED_GIT_CORE_NODES": "fmnext-n2,fmnext-n3",
        "HOSTED_GIT_CORE_OWNERS": "fmnext-n2, FMNEXT-N1,bad name,fmnext-n3"})()
    assert hosted_repos.core_owners(env) == ["fmnext-n2", "fmnext-n3", "fmnext-n1"]
    assert hosted_repos.core_nodes(env) == ["fmnext-n2", "fmnext-n3"]
    assert hosted_git.hosted_core_owners(env) == hosted_repos.core_owners(env)
    env.HOSTED_GIT_CORE_NODES = ""
    assert hosted_repos.core_owners(env) == []
