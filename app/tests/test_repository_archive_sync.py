import ast
import asyncio
import base64
import hashlib
import json
import re
import sqlite3
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent
sys.path.insert(0, str(ROOT / "src"))

import repository_archive_sync as archive  # noqa: E402
import schema  # noqa: E402
import urls  # noqa: E402

ACCESS = "AccessKey0123456"
SECRET = "SecretKey9876543"


class _Date:
    clock = [1_800_000_000_000]

    @staticmethod
    def now():
        return _Date.clock[0]


class _Response:
    def __init__(self, body, status=200):
        self.body = body
        self.status = status


class _Request:
    def __init__(self, method, body):
        self.method = method
        self.body = body


class FakeArchive:
    def __init__(self):
        self.calls = []
        self.authorized = True
        self.items = {"existing-item": {"metadata": {"identifier": "existing-item"}}}

    async def __call__(self, method, url, headers):
        self.calls.append({"method": method, "url": url, "headers": headers})
        if url == archive.S3_ORIGIN + "/?check_auth=1":
            if headers.get("authorization") != "LOW %s:%s" % (ACCESS, SECRET) \
                    or not self.authorized:
                return 403, {"error": "Invalid S3 keys"}
            return 200, {"authorized": True, "screenname": "alice-archivist",
                         "username": "alice@example.test"}
        if url.startswith(archive.METADATA_ORIGIN):
            assert "authorization" not in headers
            return 200, self.items.get(url[len(archive.METADATA_ORIGIN):], {})
        return 404, None


def _db():
    db = sqlite3.connect(":memory:")
    db.row_factory = sqlite3.Row
    db.execute("CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT)")
    db.executescript((ROOT / "migrations" / "0288_repo_archive_syncs.sql")
                     .read_text("utf-8"))
    db.execute("CREATE TABLE hosted_repos (id TEXT, key_bi TEXT,"
               " deleted_at INTEGER DEFAULT 0)")
    return db


def _bi(value):
    return hashlib.sha256(("bi:" + str(value).strip().lower()).encode()
                          ).hexdigest()


def _install(db, authorized=True, private=()):
    fake = FakeArchive()

    async def d1_run(_env, query, *args):
        db.execute(query, args)
        db.commit()

    async def d1_first(_env, query, *args):
        row = db.execute(query, args).fetchone()
        return dict(row) if row else None

    async def blind_index(_env, value):
        return _bi(value)

    async def encrypt_row(_env, obj):
        return base64.b64encode(json.dumps(obj).encode()).decode()

    async def decrypt_row(_env, stored):
        try:
            return json.loads(base64.b64decode(stored).decode())
        except Exception:
            return None

    async def bounded_json_request(request):
        return request.body

    async def ensure_schema(_env):
        return None

    async def authorize(_env, _request, _owner, _repo, _data):
        return authorized

    async def repo_is_private(_env, _owner, repo):
        return str(repo).lower() in private

    async def js_fetch_with_timeout(*_args, **_kwargs):
        raise AssertionError("tests route every call through _http")

    def json_response(payload, status=200, **_kwargs):
        return _Response(payload, status)

    bindings = {
        "Date": _Date,
        "d1_run": d1_run, "d1_first": d1_first,
        "blind_index": blind_index, "encrypt_row": encrypt_row,
        "decrypt_row": decrypt_row,
        "bounded_json_request": bounded_json_request,
        "ensure_schema": ensure_schema, "json_response": json_response,
        "method_name": lambda request: request.method,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "_authorize_repo_owner_web": authorize,
        "_repo_is_private": repo_is_private,
        "js_fetch_with_timeout": js_fetch_with_timeout,
    }
    archive._bind_runtime(bindings)
    archive.__dict__.update(bindings)
    archive.__dict__["_http"] = fake
    return fake


def _call(body, owner="alice", repo="engine"):
    return asyncio.run(archive.repo_archive_sync_handler(
        object(), _Request("POST", body), owner, repo))


def _connect(**extra):
    body = {"action": "save", "identifier": "alice-engine",
            "accessKey": ACCESS, "secret": SECRET}
    body.update(extra)
    return _call(body)


def test_identifier_and_key_parsing():
    assert archive.clean_identifier("alice-engine") == "alice-engine"
    assert archive.clean_identifier(
        "https://archive.org/details/alice-engine?tab=about") == "alice-engine"
    assert archive.clean_identifier("abc") == ""
    assert archive.clean_identifier("has space here") == ""
    assert archive.clean_identifier("-leading-dash") == ""
    assert archive.clean_identifier("a..b..c") == ""
    assert archive.clean_key(" " + ACCESS + " ") == ACCESS
    assert archive.clean_key("with:colon1234") == ""
    assert archive.clean_interval("Weekly") == "weekly"
    assert archive.clean_interval("hourly") == ""
    assert archive.bundle_name("my.repo") == "my.repo.bundle"
    assert archive.upload_url("alice-engine", "engine") == (
        "https://s3.us.archive.org/alice-engine/engine.bundle")


def test_connect_checks_the_keys_and_never_returns_the_secret():
    db = _db()
    fake = _install(db)
    response = _connect(interval="daily")
    assert response.status == 200, response.body
    view = response.body["archive"]
    assert view["identifier"] == "alice-engine"
    assert view["itemUrl"] == "https://archive.org/details/alice-engine"
    assert view["interval"] == "daily"
    assert view["active"] is True
    assert view["secretHint"] == "…6543"
    assert view["status"]["username"] == "alice-archivist"
    assert view["status"]["itemExists"] is False
    assert SECRET not in json.dumps(response.body)
    assert fake.calls[0]["headers"]["authorization"] == "LOW %s:%s" % (
        ACCESS, SECRET)
    stored = db.execute("SELECT data FROM repo_archive_syncs").fetchone()
    assert SECRET not in stored["data"]
    kept = _call({"action": "save", "identifier": "existing-item",
                  "accessKey": ACCESS, "secret": ""})
    assert kept.status == 200
    assert kept.body["archive"]["interval"] == "daily"
    assert kept.body["archive"]["status"]["itemExists"] is True
    choices = [item["key"] for item in response.body["catalog"]["intervals"]]
    assert choices == ["daily", "weekly", "monthly"]


def test_bad_input_and_refused_keys_are_reported():
    db = _db()
    fake = _install(db)
    assert _connect(identifier="x").body["error"] == "invalid_identifier"
    assert _connect(accessKey="").body["error"] == "invalid_access_key"
    assert _connect(secret="").body["error"] == "secret_required"
    assert _connect(secret="bad secret!").body["error"] == "invalid_secret"
    assert _connect(interval="hourly").body["error"] == "invalid_interval"
    fake.authorized = False
    refused = _connect()
    assert refused.status == 400
    assert refused.body["error"] == "verification_failed"
    assert "Invalid S3 keys" in refused.body["message"]
    assert db.execute("SELECT COUNT(*) FROM repo_archive_syncs").fetchone()[0] == 0
    assert _call({"action": "sync"}).body["error"] == "not_configured"


def test_private_repositories_and_other_accounts_are_refused():
    db = _db()
    _install(db, private=("engine",))
    assert _connect().body["error"] == "private_repository"
    _install(db, authorized=False)
    assert _connect().status == 403


def test_daemon_config_and_report_round_trip():
    db = _db()
    _install(db)
    _connect(interval="monthly")
    config = asyncio.run(archive.sync_config(object(), "alice", "engine"))
    assert config["url"] == "https://s3.us.archive.org/alice-engine/engine.bundle"
    assert config["accessKey"] == ACCESS and config["secret"] == SECRET
    assert config["intervalSeconds"] == 30 * 24 * 3600
    assert config["collection"] == "open_source_software"
    assert config["title"] == "alice/engine"
    assert config["source"] == "https://forkmesh.com/alice/engine"
    assert "uploaded" not in config
    revision = config["revision"]
    report = {"revision": revision, "digest": "a" * 64, "ok": True,
              "at": 123, "error": "", "bytes": 4096}
    assert asyncio.run(archive.record_sync(object(), "alice", "engine",
                                           report)) is True
    assert asyncio.run(archive.record_sync(object(), "alice", "engine",
                                           report)) is False
    again = asyncio.run(archive.sync_config(object(), "alice", "engine"))
    assert again["revision"] == revision
    assert again["uploaded"] == {"revision": revision, "digest": "a" * 64,
                                 "at": 123}
    failed = dict(report, ok=False, at=456, error="HTTP 503", bytes=0)
    asyncio.run(archive.record_sync(object(), "alice", "engine", failed))
    view = _call({"action": "get"}).body["archive"]
    assert view["status"]["upload"]["ok"] is False
    assert view["status"]["upload"]["lastOkAt"] == 123
    assert view["history"][0]["message"] == "HTTP 503"
    assert "uploaded" not in asyncio.run(
        archive.sync_config(object(), "alice", "engine"))


def test_back_up_now_changes_the_revision_once_per_cooldown():
    db = _db()
    _install(db)
    _connect()
    first = asyncio.run(archive.sync_config(object(), "alice", "engine"))
    _Date.clock[0] += 1000
    requested = _call({"action": "sync"})
    assert requested.body["requested"] is True
    second = asyncio.run(archive.sync_config(object(), "alice", "engine"))
    assert second["revision"] != first["revision"]
    assert _call({"action": "sync"}).body["error"] == "sync_too_soon"
    _Date.clock[0] += archive.SYNC_COOLDOWN_MS
    assert _call({"action": "sync"}).body["requested"] is True


def test_paused_disconnected_or_private_repositories_send_nothing():
    db = _db()
    _install(db)
    _connect()
    _call({"action": "toggle", "active": False})
    assert asyncio.run(archive.sync_config(object(), "alice", "engine")) is None
    _call({"action": "toggle", "active": True})
    assert asyncio.run(archive.sync_config(object(), "alice", "engine"))
    _install(db, private=("engine",))
    assert asyncio.run(archive.sync_config(object(), "alice", "engine")) is None
    _install(db)
    assert _call({"action": "disconnect"}).body["archive"] is None
    assert asyncio.run(archive.sync_config(object(), "alice", "engine")) is None


def test_org_aliases_find_the_settings():
    db = _db()
    _install(db)
    _call({"action": "save", "identifier": "acme-engine",
           "accessKey": ACCESS, "secret": SECRET}, owner="acme")
    config = asyncio.run(archive.sync_config(object(), "alice", "engine",
                                             ("acme",)))
    assert config["title"] == "acme/engine"


def test_the_route_schema_and_registries_ship_with_the_module():
    assert urls.REPO_ARCHIVE_SYNC_RE.match(
        "/api/repo/alice/engine/archive-sync").groups() == ("alice", "engine")
    declared = {
        match.group(1)
        for statement in schema.SCHEMA_STATEMENTS
        for match in [re.search(r"CREATE TABLE IF NOT EXISTS (\w+)",
                                statement)]
        if match
    }
    assert "repo_archive_syncs" in declared
    reverse = (ROOT / "migrations" / "down"
               / "0288_repo_archive_syncs.sql").read_text("utf-8")
    assert "DROP TABLE IF EXISTS repo_archive_syncs" in reverse
    entry = (ROOT / "src" / "entry.py").read_text("utf-8")
    assert '_LazyModule("repository_archive_sync")' in entry
    scoped = entry.split("_REPO_SCOPED_TABLES = (", 1)[1].split(")\n", 1)[0]
    assert '("repo_archive_syncs", "repo_bi")' in scoped
    footprint = (REPO / "world" / "tools"
                 / "build_worker_footprint.py").read_text("utf-8")
    assert '"repository_archive_sync.py",' in footprint
    runtime = (ROOT / "src" / "app_runtime.py").read_text("utf-8")
    assert "repo_archive_sync_handler(" in runtime
    hosted = (ROOT / "src" / "hosted_repos.py").read_text("utf-8")
    assert 'item["archiveSync"] = archive' in hosted
    rekey = (REPO / "tools" / "next_rekey.py").read_text("utf-8")
    assert '"repo_archive_syncs": ("repo_bi",),' in rekey


def _free_names(tree):
    import builtins
    bound = set(dir(builtins))
    for node in ast.walk(tree):
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef,
                             ast.ClassDef)):
            bound.add(node.name)
        elif isinstance(node, ast.Name) and isinstance(node.ctx, ast.Store):
            bound.add(node.id)
        elif isinstance(node, (ast.Import, ast.ImportFrom)):
            for alias in node.names:
                bound.add((alias.asname or alias.name).split(".")[0])
        elif isinstance(node, ast.arg):
            bound.add(node.arg)
    read = {node.id for node in ast.walk(tree)
            if isinstance(node, ast.Name) and isinstance(node.ctx, ast.Load)}
    return read - bound


def test_every_runtime_name_the_module_reads_is_one_entry_supplies():
    module = ast.parse((ROOT / "src" / "repository_archive_sync.py")
                       .read_text("utf-8"))
    entry = ast.parse((ROOT / "src" / "entry.py").read_text("utf-8"))
    supplied = set()
    for node in entry.body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef,
                             ast.ClassDef)):
            supplied.add(node.name)
        elif isinstance(node, ast.Assign):
            supplied.update(target.id for target in node.targets
                            if isinstance(target, ast.Name))
        elif isinstance(node, (ast.Import, ast.ImportFrom)):
            for alias in node.names:
                supplied.add((alias.asname or alias.name).split(".")[0])
    needed = _free_names(module)
    assert needed <= supplied, sorted(needed - supplied)


def test_the_settings_card_ships_in_the_bundle():
    public = ROOT / "public"
    fragments = public / "dashboard" / "js"
    detail = (fragments / "08-repo-detail-network.js").read_text("utf-8")
    content = (fragments / "06-repo-content.js").read_text("utf-8")
    explorer = (fragments / "05-repo-list-explorer.js").read_text("utf-8")
    bundle = (public / "dashboard.js").read_text("utf-8")
    settings_panel = detail.split("const settingsPanel = ", 1)[1].split(
        "detail.innerHTML = ", 1)[0]
    assert "data-repo-archive-sync-card" in settings_panel
    assert "loadRepoArchiveSync(state.selectedRepo)" in explorer
    for action in ("save", "sync", "verify", "toggle", "disconnect"):
        assert ('"%s"]' % action) in content or (
            'action: "%s"' % action) in content, action
    assert "escapeHtml(archive.identifier)" in content
    for name in ("renderRepoArchiveSync", "handleRepoArchiveSyncClick",
                 "handleRepoArchiveSyncSubmit", "data-repo-archive-sync-card"):
        assert name in bundle, name
