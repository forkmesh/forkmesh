import asyncio
import base64
import hashlib
import json
import re
import shutil
import sqlite3
import subprocess
import sys
from pathlib import Path

import pytest


PACKAGE = Path(__file__).resolve().parents[1]
REPO = PACKAGE.parents[2]
APP = REPO / "app"
sys.path.insert(0, str(PACKAGE / "content" / "worker"))
sys.path.insert(0, str(APP / "src"))
sys.path.insert(0, str(APP / "tools"))
sys.path.insert(0, str(REPO / "marketplace" / "tools"))
import internet_archive as archive  # noqa: E402
import build_app_plugins  # noqa: E402
import plugin_data  # noqa: E402
import schema  # noqa: E402
import urls  # noqa: E402

ACCESS = "AccessKey0123456"
SECRET = "SecretKey9876543"
HEAD = "a" * 40
OLD_HEAD = "b" * 40
ROUTING = "c" * 64
OLD_ROUTING = "d" * 64
SHA1 = "e" * 40
OLD_SHA1 = "f" * 40


class Response:
    def __init__(self, body, status):
        self.body = body
        self.status = status


class Runtime:
    def __init__(self, db, actor="alice"):
        self.db = db
        self.actor = actor
        self.body = {}
        self.method_value = "POST"
        self.clock = 1_800_000_000_000
        self.private = set()
        self.admins = {("alice", "alice", "engine"), ("alice", "acme", "engine")}
        self.installed = True
        self.authorized = True
        self.items = {"existing-item": {"metadata": {"identifier": "existing-item"}}}
        self.calls = []
        self.mirrors = None

    def use(self, method="POST", body=None, actor=None):
        self.method_value = method
        self.body = body or {}
        if actor is not None:
            self.actor = actor
        return self

    def method(self):
        return self.method_value

    def same_origin(self):
        return True

    async def json_body(self, _limit):
        return self.body, ""

    async def ensure_schema(self):
        return None

    async def session(self, _data=None):
        if not self.actor:
            return "", None
        return "bi:" + self.actor, {"name": self.actor}

    async def account_plugin_installed(self, _account_bi):
        return self.installed

    async def blind(self, value):
        return hashlib.sha256(("bi:" + str(value)).encode()).hexdigest()

    async def seal(self, value):
        return base64.b64encode(json.dumps(value).encode()).decode()

    async def open(self, value):
        try:
            return json.loads(base64.b64decode(value).decode())
        except Exception:
            return None

    async def d1_first(self, sql, *args):
        row = self.db.execute(sql, args).fetchone()
        return dict(row) if row else None

    async def d1_all(self, sql, *args):
        return [dict(row) for row in self.db.execute(sql, args).fetchall()]

    async def d1_run(self, sql, *args):
        self.db.execute(sql, args)
        self.db.commit()

    def now(self):
        return self.clock

    def public_origin(self):
        return "https://forkmesh.com"

    def response(self, payload, status=200, cache_control=None, extra_headers=None):
        assert "no-store" in cache_control
        return Response(payload, status)

    async def repository_facts(self, _owner, repo):
        return {"exists": True, "private": repo in self.private, "hosted": True}

    async def repository_administrator(self, owner, repo, _data=None):
        return self.actor if (self.actor, owner, repo) in self.admins else ""

    async def repository_mirrors(self, _owner, _repo):
        return self.mirrors

    async def outbound_json(self, url, method="GET", headers=None):
        headers = headers or {}
        self.calls.append({"url": url, "method": method, "headers": headers})
        if url == archive.S3_ORIGIN + "/?check_auth=1":
            if headers.get("authorization") != "LOW %s:%s" % (ACCESS, SECRET) \
                    or not self.authorized:
                return {"ok": True, "status": 403, "data": {"error": "Invalid S3 keys"}}
            return {"ok": True, "status": 200,
                    "data": {"authorized": True, "screenname": "alice-archivist"}}
        if url.startswith(archive.METADATA_ORIGIN):
            assert "authorization" not in headers
            item = self.items.get(url[len(archive.METADATA_ORIGIN):])
            if item is None:
                return {"ok": False, "error": "outbound_unavailable"}
            return {"ok": True, "status": 200, "data": item}
        return {"ok": True, "status": 404, "data": {}}


def _db():
    db = sqlite3.connect(":memory:")
    db.row_factory = sqlite3.Row
    db.execute("CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT)")
    db.executescript((APP / "migrations" / "0288_repo_archive_syncs.sql").read_text("utf-8"))
    return db


def _registry():
    namespace = {}
    exec(build_app_plugins.registry_source(), namespace)
    return namespace["PLUGINS"]


REGISTRY = _registry()


@pytest.fixture(autouse=True)
def _fresh_cache(monkeypatch):
    archive._WITNESS_CACHE.clear()
    monkeypatch.setattr(plugin_data, "REGISTRY", REGISTRY)


def run(coro):
    return asyncio.run(coro)


def call(runtime, body, owner="alice", repo="engine", **use):
    runtime.use("POST", body, **use)
    return run(archive.handle(runtime, "%s/repo/%s/%s" % (archive.PREFIX, owner, repo)))


def connect(runtime, owner="alice", actor=None, **fields):
    body = {"action": "save", "identifier": "alice-engine",
            "accessKey": ACCESS, "secret": SECRET}
    body.update(fields)
    return call(runtime, body, owner=owner, actor=actor)


def witness(runtime, owner="alice", repo="engine", actor=None):
    runtime.use("GET", actor=actor)
    return run(archive.handle(runtime, "%s/repo/%s/%s/witness" % (archive.PREFIX, owner, repo)))


def report(at, head=HEAD, routing=ROUTING, sha1=SHA1, ok=True, revision=None, **extra):
    body = {"revision": revision or "archive:1", "digest": "9" * 64, "ok": ok, "at": at,
            "error": "" if ok else "HTTP 503", "bytes": 4096 if ok else 0}
    if ok:
        body.update({"sha1": sha1, "routingDigest": routing,
                     "head": {"name": "refs/heads/main", "commit": head},
                     "refs": [{"name": "refs/heads/main", "commit": head},
                              {"name": "refs/tags/v1", "commit": "1" * 40},
                              {"name": "bad ref", "commit": "zz"}],
                     "refCount": 3})
    body.update(extra)
    return body


def config(runtime, owner="alice", aliases=(), keys=None):
    return run(archive.archive_upload_config(runtime, owner, "engine", aliases, keys))


def record(runtime, body, owner="alice"):
    return run(archive.record_archive_upload(runtime, owner, "engine", (), body))


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
    assert archive.download_url("alice-engine", "engine") == (
        "https://archive.org/download/alice-engine/engine.bundle")


def test_connect_checks_the_keys_and_never_returns_the_secret():
    db = _db()
    runtime = Runtime(db)
    response = connect(runtime, interval="daily")
    assert response.status == 200, response.body
    view = response.body["archive"]
    assert view["identifier"] == "alice-engine"
    assert view["itemUrl"] == "https://archive.org/details/alice-engine"
    assert view["interval"] == "daily" and view["active"] is True
    assert view["secretHint"] == "…6543"
    assert view["status"]["username"] == "alice-archivist"
    assert view["status"]["itemExists"] is False
    assert SECRET not in json.dumps(response.body)
    assert runtime.calls[0]["headers"]["authorization"] == "LOW %s:%s" % (ACCESS, SECRET)
    assert SECRET not in db.execute("SELECT data FROM repo_archive_syncs").fetchone()["data"]
    kept = call(runtime, {"action": "save", "identifier": "existing-item",
                          "accessKey": ACCESS, "secret": ""})
    assert kept.status == 200
    assert kept.body["archive"]["interval"] == "daily"
    assert kept.body["archive"]["status"]["itemExists"] is True
    assert [item["key"] for item in response.body["catalog"]["intervals"]] == [
        "daily", "weekly", "monthly"]


def test_bad_input_and_refused_keys_are_reported():
    db = _db()
    runtime = Runtime(db)
    assert connect(runtime, identifier="x").body["error"] == "invalid_identifier"
    assert connect(runtime, accessKey="").body["error"] == "invalid_access_key"
    assert connect(runtime, secret="").body["error"] == "secret_required"
    assert connect(runtime, secret="bad secret!").body["error"] == "invalid_secret"
    assert connect(runtime, interval="hourly").body["error"] == "invalid_interval"
    runtime.authorized = False
    refused = connect(runtime)
    assert refused.status == 400 and refused.body["error"] == "verification_failed"
    assert "Invalid S3 keys" in refused.body["message"]
    assert db.execute("SELECT COUNT(*) FROM repo_archive_syncs").fetchone()[0] == 0
    assert call(runtime, {"action": "sync"}).body["error"] == "not_configured"


def test_private_repositories_other_accounts_and_uninstalled_are_refused():
    db = _db()
    runtime = Runtime(db)
    runtime.private.add("engine")
    assert connect(runtime).body["error"] == "private_repository"
    runtime.private.clear()
    assert connect(runtime, actor="mallory").status == 403
    runtime.installed = False
    assert connect(runtime, actor="alice").body["error"] == "internet_archive_not_installed"
    runtime.installed = True
    runtime.actor = ""
    assert connect(runtime).status == 401
    runtime.use("GET")
    assert run(archive.handle(runtime, archive.PREFIX + "/repo/alice/engine")).status == 405


def test_daemon_config_and_report_round_trip():
    db = _db()
    runtime = Runtime(db)
    connect(runtime, interval="monthly")
    keys = run(archive.archive_upload_keys(runtime))
    assert len(keys) == 1
    assert config(runtime, keys=set()) is None
    settings = config(runtime, keys=keys)
    assert settings["url"] == "https://s3.us.archive.org/alice-engine/engine.bundle"
    assert settings["accessKey"] == ACCESS and settings["secret"] == SECRET
    assert settings["intervalSeconds"] == 30 * 24 * 3600
    assert settings["collection"] == "open_source_software"
    assert settings["title"] == "alice/engine"
    assert settings["source"] == "https://forkmesh.com/alice/engine"
    assert "uploaded" not in settings
    revision = settings["revision"]
    first = report(123, revision=revision)
    assert record(runtime, first) is True
    assert record(runtime, first) is False
    again = config(runtime)
    assert again["revision"] == revision
    assert again["uploaded"] == {"revision": revision, "digest": "9" * 64, "at": 123}
    view = call(runtime, {"action": "get"}).body["archive"]
    upload = view["status"]["upload"]
    assert upload["sha1"] == SHA1 and upload["routingDigest"] == ROUTING
    assert upload["head"] == {"name": "refs/heads/main", "commit": HEAD}
    assert view["uploads"][0]["at"] == 123
    stored = run(runtime.open(db.execute("SELECT data FROM repo_archive_syncs").fetchone()["data"]))
    assert [ref["name"] for ref in stored["refs"]] == ["refs/heads/main", "refs/tags/v1"]
    record(runtime, report(456, ok=False, revision=revision))
    view = call(runtime, {"action": "get"}).body["archive"]
    assert view["status"]["upload"]["ok"] is False
    assert view["status"]["upload"]["lastOkAt"] == 123
    assert view["history"][0]["message"] == "HTTP 503"
    assert [item["at"] for item in view["uploads"]] == [456, 123]
    assert "uploaded" not in config(runtime)
    assert record(runtime, report(456, ok=False, revision=revision)) is False


def test_back_up_now_changes_the_revision_once_per_cooldown():
    db = _db()
    runtime = Runtime(db)
    connect(runtime)
    first = config(runtime)
    runtime.clock += 1000
    assert call(runtime, {"action": "sync"}).body["requested"] is True
    assert config(runtime)["revision"] != first["revision"]
    assert call(runtime, {"action": "sync"}).body["error"] == "sync_too_soon"
    runtime.clock += archive.SYNC_COOLDOWN_MS
    assert call(runtime, {"action": "sync"}).body["requested"] is True


def test_paused_disconnected_or_private_repositories_send_nothing():
    db = _db()
    runtime = Runtime(db)
    connect(runtime)
    call(runtime, {"action": "toggle", "active": False})
    assert config(runtime) is None
    assert run(archive.archive_upload_keys(runtime)) == set()
    call(runtime, {"action": "toggle", "active": True})
    assert config(runtime)
    runtime.private.add("engine")
    assert config(runtime) is None
    runtime.private.clear()
    assert call(runtime, {"action": "disconnect"}).body["archive"] is None
    assert config(runtime) is None


def test_org_aliases_find_the_settings():
    db = _db()
    runtime = Runtime(db)
    connect(runtime, owner="acme", identifier="acme-engine")
    settings = config(runtime, owner="alice", aliases=("acme",))
    assert settings["title"] == "acme/engine"


def test_backups_list_and_removal_cover_only_your_own():
    db = _db()
    runtime = Runtime(db)
    connect(runtime)
    runtime.admins.add(("bob", "bob", "engine"))
    connect(runtime, owner="bob", identifier="bob-engine", actor="bob")
    runtime.use("GET", actor="alice")
    listed = run(archive.handle(runtime, archive.PREFIX + "/backups")).body["backups"]
    assert [(item["owner"], item["repo"]) for item in listed] == [("alice", "engine")]
    assert "accessKey" not in listed[0]
    run(archive.purge_account_data(runtime, "bi:alice", "alice"))
    assert db.execute("SELECT COUNT(*) FROM repo_archive_syncs").fetchone()[0] == 1

    class PurgeOnly:
        d1_all = Runtime.d1_all
        d1_run = Runtime.d1_run

        def __init__(self, db):
            self.db = db

    run(archive.purge_account_data(PurgeOnly(db), "bi:bob", "bob"))
    assert db.execute("SELECT COUNT(*) FROM repo_archive_syncs").fetchone()[0] == 1


def _states():
    return [
        {"at": 2_000, "ok": True, "sha1": SHA1, "routingDigest": ROUTING,
         "head": {"name": "refs/heads/main", "commit": HEAD}, "file": "engine.bundle"},
        {"at": 1_000, "ok": True, "sha1": OLD_SHA1, "routingDigest": OLD_ROUTING,
         "head": {"name": "refs/heads/main", "commit": OLD_HEAD}, "file": "engine.bundle"},
    ]


def test_each_copy_is_graded_against_every_archived_state():
    states = _states()
    verdict = lambda **copy: archive.copy_verdict(states, copy)["verdict"]  # noqa: E731
    assert verdict(directRefsSha256=ROUTING, commit="0" * 40) == "match"
    assert verdict(commit=HEAD, branch="main") == "head"
    assert verdict(commit=HEAD, branch="develop", lastSync=1_500) == "differs"
    assert verdict(directRefsSha256=OLD_ROUTING) == "earlier"
    assert verdict(commit=OLD_HEAD, branch="main") == "earlier"
    assert verdict(commit="0" * 40, lastSync=3_000) == "newer"
    assert verdict(commit="0" * 40, lastSync=1_500) == "differs"
    assert verdict() == "unknown"
    assert archive.copy_verdict([], {"commit": HEAD})["verdict"] == "unknown"


def test_the_archive_org_copy_is_checked_by_its_checksum():
    states = _states()
    now = 2_000 + archive.INGEST_GRACE_MS + 1
    files = lambda sha1: {"files": [{"name": "engine.bundle", "sha1": sha1,  # noqa: E731
                                     "size": "4096", "mtime": "1700000000"}]}
    held = archive.witness_state(states, files(SHA1), now)
    assert held["state"] == "verified" and held["file"]["size"] == 4096
    assert held["file"]["mtime"] == 1_700_000_000_000
    assert archive.witness_state(states, files(OLD_SHA1), now)["state"] == "older"
    assert archive.witness_state(states, files("0" * 40), now)["state"] == "changed"
    assert archive.witness_state(states, files("0" * 40), 2_001)["state"] == "pending"
    assert archive.witness_state(states, {"files": []}, now)["state"] == "missing"
    assert archive.witness_state(states, None, now)["state"] == "unreachable"
    assert archive.witness_state([], {}, now)["state"] == "none"
    unrecorded = [dict(states[0], sha1="")]
    assert archive.witness_state(unrecorded, files(SHA1), now)["state"] == "unrecorded"


def test_witness_compares_the_source_and_every_mirror_with_the_archive():
    db = _db()
    runtime = Runtime(db)
    connect(runtime)
    record(runtime, report(1_000, head=OLD_HEAD, routing=OLD_ROUTING, sha1=OLD_SHA1))
    runtime.clock += 1
    record(runtime, report(runtime.clock, revision="archive:2"))
    runtime.items["alice-engine"] = {"files": [{"name": "engine.bundle", "sha1": SHA1, "size": "4096"}]}
    runtime.mirrors = {
        "sourceOfTruth": {"node": "alice-desktop", "status": "offline", "commit": HEAD, "branch": "main"},
        "mirrors": [
            {"node": "n1", "status": "online", "cloneAvailable": True, "directRefsSha256": ROUTING},
            {"node": "n2", "status": "online", "cloneAvailable": True, "commit": OLD_HEAD, "branch": "main"},
            {"node": "n3", "status": "online", "commit": "0" * 40, "lastSync": runtime.clock + 5},
            {"node": "n4", "status": "offline", "commit": "0" * 40, "lastSync": 10},
            {"node": "n1", "status": "online", "directRefsSha256": ROUTING},
        ],
    }
    body = witness(runtime, actor="bob").body
    assert body["witness"]["state"] == "verified"
    assert body["archive"]["latest"]["head"]["commit"] == HEAD
    assert body["archive"]["downloadUrl"].endswith("/alice-engine/engine.bundle")
    assert [(copy["role"], copy["node"], copy["verdict"]) for copy in body["copies"]] == [
        ("source", "alice-desktop", "head"), ("mirror", "n1", "match"),
        ("mirror", "n2", "earlier"), ("mirror", "n3", "newer"), ("mirror", "n4", "differs")]
    summary = body["summary"]
    assert summary["sourceOnline"] is False and summary["sourceKnown"] is True
    assert summary["servingMirrors"] == 3 and summary["agreeingMirrors"] == 1
    assert summary["verdicts"]["differs"] == 1
    assert SECRET not in json.dumps(body) and ACCESS not in json.dumps(body)
    calls = len(runtime.calls)
    witness(runtime)
    assert len(runtime.calls) == calls
    runtime.private.add("engine")
    assert witness(runtime).status == 404
    assert witness(runtime, repo="other").body["archive"] is None


def test_the_host_wires_the_plugin_in_place_of_the_old_core_module():
    entry = REGISTRY["internet-archive"]
    assert entry["module"] == "internet_archive" and entry["api"] == archive.PREFIX
    assert set(entry["outboundOrigins"]) == {"https://archive.org", "https://s3.us.archive.org"}
    assert {"repository.archive.upload", "repository.mirrors.read"} <= set(entry["permissions"])
    assert entry["accountData"] is True
    assert plugin_data._archive_plugins() == ["internet-archive"]
    assert not (APP / "src" / "repository_archive_sync.py").exists()
    assert not hasattr(urls, "REPO_ARCHIVE_SYNC_RE")
    declared = {match.group(1) for statement in schema.SCHEMA_STATEMENTS
                for match in [re.search(r"CREATE TABLE IF NOT EXISTS (\w+)", statement)] if match}
    assert "repo_archive_syncs" in declared
    source = (APP / "src" / "entry.py").read_text("utf-8")
    scoped = source.split("_REPO_SCOPED_TABLES = (", 1)[1].split(")\n", 1)[0]
    assert '("repo_archive_syncs", "repo_bi")' in scoped
    for name in ("repository_facts", "repository_administrator", "repository_mirrors",
                 "plugin_archive_upload_config", "plugin_record_archive_upload"):
        assert name in source, name
    assert 'PLUGIN_REPOSITORY_ARCHIVE_PERMISSION = "repository.archive.upload"' in source
    assert 'PLUGIN_REPOSITORY_MIRRORS_PERMISSION = "repository.mirrors.read"' in source
    hosted = (APP / "src" / "hosted_git.py").read_text("utf-8")
    assert "plugin_archive_upload_keys(self.env)" in hosted
    assert "repo_archive_syncs" not in hosted
    assert 'item["archiveSync"] = archive' in (APP / "src" / "hosted_repos.py").read_text("utf-8")
    assert '"repo_archive_syncs": ("repo_bi",),' in (REPO / "tools" / "next_rekey.py").read_text("utf-8")


def test_hooks_run_through_the_plugin_engine():
    db = _db()
    runtime = Runtime(db)
    connect(runtime)
    keys = run(plugin_data.archive_upload_keys(lambda plugin: runtime))
    assert len(keys) == 1
    settings = run(plugin_data.archive_upload_config(lambda plugin: runtime, "alice", "engine", (), keys))
    assert settings["title"] == "alice/engine"
    assert run(plugin_data.record_archive_upload(
        lambda plugin: runtime, "alice", "engine", (), report(5))) is True


def test_repository_settings_link_to_the_plugin_page():
    fragments = APP / "public" / "dashboard" / "js"
    detail = (fragments / "08-repo-detail-network.js").read_text("utf-8")
    bundle = (APP / "public" / "dashboard.js").read_text("utf-8")
    for text in (detail, bundle):
        assert "data-repo-archive-link-card" in text
        assert "/dashboard/internet-archive?repo=" in text
        assert "archive-sync" not in text
        assert "renderRepoArchiveSync" not in text


@pytest.mark.skipif(shutil.which("node") is None, reason="node is not installed")
def test_the_page_script_parses_and_registers():
    script = PACKAGE / "content" / "web" / "page.js"
    result = subprocess.run([shutil.which("node"), "--check", str(script)],
                            capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stderr
    text = script.read_text("utf-8")
    assert 'registerPage("internet-archive"' in text
    view = (PACKAGE / "content" / "web" / "view.html").read_text("utf-8")
    assert 'data-view="internet-archive"' in view
    for marker in ("data-ia-map", "data-ia-timeline", "data-ia-copies", "data-ia-settings-body"):
        assert marker in view and marker in text, marker
