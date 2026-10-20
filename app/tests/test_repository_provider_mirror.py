import ast
import asyncio
import base64
import hashlib
import json
import re
import sqlite3
import sys
from pathlib import Path
from urllib.parse import urlparse

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent
sys.path.insert(0, str(ROOT / "src"))

import repository_provider_mirror as mirror  # noqa: E402
import schema  # noqa: E402
import urls  # noqa: E402


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


class FakeForge:
    def __init__(self, provider="github"):
        self.provider = provider
        self.calls = []
        self.repo_exists = True
        self.user_status = 200
        self.issues = []
        self.pulls = []
        self.comments = {}
        self.discussions = []
        self.created_issues = []
        self.created_comments = []
        self.created_releases = []
        self.created_repos = []
        self.next_number = 100

    async def __call__(self, method, url, headers, body=None):
        self.calls.append({"method": method, "url": url, "headers": headers,
                           "body": body})
        path = urlparse(url).path
        if url.startswith("https://gitlab.com/api/v4"):
            path = path[len("/api/v4"):]
        if path.endswith("/graphql"):
            return 200, {"data": {"repository": {
                "id": "R_1", "hasDiscussionsEnabled": True,
                "discussionCategories": {"nodes": [
                    {"id": "C_1", "name": "General"}]},
                "discussions": {"nodes": self.discussions}}}}
        if path == "/user":
            if self.user_status != 200:
                return self.user_status, {"message": "Bad credentials"}
            return 200, {"login": "octo", "username": "octo"}
        if method == "POST" and path in ("/user/repos", "/projects"):
            self.created_repos.append(body)
            self.repo_exists = True
            return 201, {"id": 1}
        if path.startswith("/namespaces/"):
            return 200, {"id": 42}
        repo_base = ("/repos/octo/engine" if self.provider == "github"
                     else "/projects/octo%2Fengine")
        if path == repo_base or path == "/projects/octo/engine":
            if not self.repo_exists:
                return 404, {"message": "Not Found"}
            return 200, {"private": False, "permissions": {"push": True},
                         "default_branch": "main", "visibility": "public"}
        rest = path.split("engine", 1)[1] if "engine" in path else path
        if method == "GET" and rest == "/issues":
            return 200, self.issues
        if method == "GET" and rest in ("/pulls", "/merge_requests"):
            return 200, self.pulls
        match = re.fullmatch(r"/issues/(\d+)/(comments|notes)", rest)
        if match and method == "GET":
            return 200, self.comments.get(int(match.group(1)), [])
        if match and method == "POST":
            self.created_comments.append((int(match.group(1)), body))
            return 201, {"id": 900 + len(self.created_comments)}
        if method == "POST" and rest == "/issues":
            self.next_number += 1
            self.created_issues.append(body)
            return 201, {"number": self.next_number, "iid": self.next_number}
        if method == "POST" and rest == "/releases":
            self.created_releases.append(body)
            return 201, {"id": 5}
        return 404, {"message": "unexpected " + method + " " + path}


def _db():
    db = sqlite3.connect(":memory:")
    db.row_factory = sqlite3.Row
    db.execute("CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT)")
    db.executescript((ROOT / "migrations" / "0257_repo_provider_mirrors.sql")
                     .read_text("utf-8"))
    db.executescript("""
        CREATE TABLE repositories (key_bi TEXT PRIMARY KEY);
        CREATE TABLE issue_inbox (id INTEGER PRIMARY KEY AUTOINCREMENT,
            repo_bi TEXT, data TEXT, submitter_bi TEXT);
        CREATE TABLE discussion_inbox (id INTEGER PRIMARY KEY AUTOINCREMENT,
            repo_bi TEXT, data TEXT, submitter_bi TEXT);
        CREATE TABLE hosted_repos (id TEXT, key_bi TEXT,
            deleted_at INTEGER DEFAULT 0);
    """)
    return db


def _bi(value):
    return hashlib.sha256(("bi:" + str(value).strip().lower()).encode()
                          ).hexdigest()


def _install(db, forge=None, authorized=True, private=()):
    forge = forge or FakeForge()
    numbers = [10]
    hosts = []

    async def d1_run(_env, query, *args):
        db.execute(query, args)
        db.commit()

    async def d1_first(_env, query, *args):
        row = db.execute(query, args).fetchone()
        return dict(row) if row else None

    async def d1_all(_env, query, *args):
        return [dict(row) for row in db.execute(query, args).fetchall()]

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

    async def alias_owner(_env, owner, _repo):
        return owner

    async def next_issue_number(_env, _repo_bi, _owner, _repo, kind="issue"):
        numbers[0] += 1
        return numbers[0]

    async def notify_repo_host(_env, owner, repo, kind):
        hosts.append((owner, repo, kind))

    async def js_fetch_with_timeout(*_args, **_kwargs):
        raise AssertionError("tests route every call through _http")

    def json_response(payload, status=200, **_kwargs):
        return _Response(payload, status)

    bindings = {
        "Date": _Date,
        "d1_run": d1_run, "d1_first": d1_first, "d1_all": d1_all,
        "blind_index": blind_index, "encrypt_row": encrypt_row,
        "decrypt_row": decrypt_row,
        "bounded_json_request": bounded_json_request,
        "ensure_schema": ensure_schema, "json_response": json_response,
        "method_name": lambda request: request.method,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "MAX_NODE_NAME": 40, "MAX_PENDING_ISSUES": 500,
        "MAX_PENDING_DISCUSSIONS": 500,
        "_authorize_repo_owner_web": authorize,
        "_repo_is_private": repo_is_private,
        "_ap_org_alias_owner": alias_owner,
        "_forkbot_next_issue_number": next_issue_number,
        "notify_repo_host": notify_repo_host,
        "js_fetch_with_timeout": js_fetch_with_timeout,
    }
    mirror._bind_runtime(bindings)
    mirror.__dict__.update(bindings)
    mirror.__dict__["_http"] = forge
    db.execute("INSERT OR IGNORE INTO repositories (key_bi) VALUES (?)",
               (_bi("alice/engine"),))
    db.commit()
    return forge, hosts


def _call(body, owner="alice", repo="engine"):
    return asyncio.run(mirror.repo_provider_mirror_handler(
        object(), _Request("POST", body), owner, repo))


def _connect(resources=None, provider="github"):
    return _call({"action": "save", "provider": provider,
                  "remote": "octo/engine", "token": "ghp_secret_token_1234",
                  "resources": resources or {}})


def _inbox(db, table="issue_inbox"):
    rows = db.execute("SELECT data FROM %s ORDER BY id" % table).fetchall()
    return [json.loads(base64.b64decode(row["data"]).decode()) for row in rows]


def _gh_issue(number, title="Bug", body="It breaks", state="open",
              updated="2026-10-01T10:00:00Z", pull=False):
    item = {"number": number, "title": title, "body": body, "state": state,
            "user": {"login": "carol"}, "html_url":
            "https://github.com/octo/engine/issues/%d" % number,
            "created_at": "2026-09-30T10:00:00Z", "updated_at": updated,
            "labels": [{"name": "Bug"}], "comments": 0}
    if pull:
        item["pull_request"] = {"url": "x"}
    return item


def test_remote_and_token_parsing():
    assert mirror.clean_remote("github", "octo/engine")["path"] == "octo/engine"
    assert mirror.clean_remote(
        "github", "https://github.com/octo/engine.git")["name"] == "engine"
    assert mirror.clean_remote("github", "https://gitlab.com/octo/x") is None
    assert mirror.clean_remote("github", "octo/sub/engine") is None
    assert mirror.clean_remote("gitlab", "group/sub/engine") is None
    assert mirror.clean_remote("github", "https://user:pw@github.com/a/b") is None
    assert mirror.clean_remote("github", "../etc/passwd") is None
    assert mirror.clean_token("  ghp_abc  ") == "ghp_abc"
    assert mirror.clean_token("has space") == ""
    assert mirror.token_hint("ghp_secret_token_1234") == "…1234"


def test_resources_respect_provider_support_and_allowed_directions():
    resources = mirror.normalize_resources({
        "code": {"direction": "pull"},
        "issues": {"direction": "both", "mode": "manual"},
        "discussions": {"direction": "pull"},
        "releases": {"direction": "both"},
    }, "gitlab")
    assert resources["code"]["direction"] == "off"
    assert resources["issues"] == {"direction": "both", "mode": "manual",
                                   "comments": True}
    assert resources["discussions"]["direction"] == "off"
    assert resources["releases"]["direction"] == "off"
    assert mirror.normalize_resources({}, "github")["code"]["direction"] == "push"


def test_iso_timestamps_convert_to_epoch_milliseconds():
    assert mirror._iso_ms("1970-01-01T00:00:00Z") == 0
    assert mirror._iso_ms("2026-10-13T12:30:15Z") == 1791894615000
    assert mirror._iso_ms("") == 0


def test_connect_verifies_the_token_and_never_returns_it():
    db = _db()
    forge, _hosts = _install(db)
    response = _connect({"issues": {"direction": "both"}})
    assert response.status == 200, response.body
    view = response.body["mirror"]
    assert view["remote"] == "octo/engine"
    assert view["remoteUrl"] == "https://github.com/octo/engine"
    assert view["tokenHint"] == "…1234"
    assert view["status"]["login"] == "octo"
    assert view["status"]["canPush"] is True
    assert "ghp_secret_token_1234" not in json.dumps(response.body)
    auth = [call["headers"].get("authorization") for call in forge.calls]
    assert "Bearer ghp_secret_token_1234" in auth
    stored = db.execute("SELECT data FROM repo_provider_mirrors").fetchone()
    assert "ghp_secret" not in stored["data"]

    kept = _call({"action": "save", "provider": "github",
                  "remote": "octo/engine", "token": "",
                  "resources": {"issues": {"direction": "pull"}}})
    assert kept.status == 200
    assert kept.body["mirror"]["resources"]["issues"]["direction"] == "pull"


def test_a_refused_token_or_missing_remote_is_reported():
    db = _db()
    forge, _hosts = _install(db)
    forge.user_status = 401
    refused = _connect()
    assert refused.status == 400
    assert refused.body["error"] == "verification_failed"
    forge.user_status = 200
    forge.repo_exists = False
    missing = _connect()
    assert missing.body["error"] == "remote_missing"
    created = _call({"action": "save", "provider": "github",
                     "remote": "octo/engine", "token": "ghp_secret_token_1234",
                     "createRemote": True})
    assert created.status == 200, created.body
    assert created.body["created"] is True
    assert forge.created_repos[0]["name"] == "engine"


def test_only_the_owner_may_manage_the_mirror():
    db = _db()
    _install(db, authorized=False)
    assert _connect().status == 403


def test_automatic_sync_imports_issues_and_pulls_into_the_inbox():
    db = _db()
    forge, hosts = _install(db)
    _connect({"issues": {"direction": "pull"},
              "pulls": {"direction": "pull"}})
    forge.issues = [
        _gh_issue(7, state="closed"),
        _gh_issue(8, pull=True),
        _gh_issue(9, body="From ForkMesh\n" + mirror.MARKER),
    ]
    forge.comments = {7: [{"id": 55, "user": {"login": "dave"},
                           "body": "Same here", "html_url": "u",
                           "created_at": "2026-10-01T09:00:00Z"}]}
    forge.pulls = [{"number": 8, "title": "Add cache", "body": "Faster",
                    "state": "open", "user": {"login": "erin"},
                    "html_url": "https://github.com/octo/engine/pull/8",
                    "created_at": "2026-09-30T10:00:00Z",
                    "updated_at": "2026-10-02T10:00:00Z",
                    "head": {"label": "erin:cache"}, "base": {"ref": "main"},
                    "labels": [], "comments": 0}]
    synced = _call({"action": "sync"})
    assert synced.status == 200
    assert synced.body["sync"]["imported"] == 2
    rows = _inbox(db)
    opens = [row for row in rows if row["event"]["type"] == "open"]
    assert [row["titleIfNew"] for row in opens] == ["Bug", "Add cache"]
    assert opens[0]["meta"]["labels"] == ["github", "bug"]
    assert opens[1]["meta"]["labels"][:2] == ["github", "pull-request"]
    assert "erin:cache" in opens[1]["event"]["body"]
    assert mirror.MARKER in opens[0]["event"]["body"]
    assert opens[0]["event"]["authorName"] == "@carol on GitHub"
    statuses = [row for row in rows if row["event"]["type"] == "status"]
    assert statuses and statuses[0]["event"]["status"] == "closed"
    comments = [row for row in rows if row["event"]["type"] == "comment"]
    assert comments[0]["number"] == opens[0]["number"]
    assert ("alice", "engine", "issues") in hosts

    again = _call({"action": "sync"})
    assert again.body["sync"]["imported"] == 0
    assert len(_inbox(db)) == len(rows)


def test_manual_mode_lists_candidates_and_imports_only_the_picked_ones():
    db = _db()
    forge, _hosts = _install(db)
    _connect({"issues": {"direction": "pull", "mode": "manual"}})
    forge.issues = [_gh_issue(1, title="One"), _gh_issue(2, title="Two")]
    assert _call({"action": "sync"}).body["sync"]["imported"] == 0
    listed = _call({"action": "candidates", "kind": "issues"})
    assert [item["key"] for item in listed.body["items"]] == ["1", "2"]
    assert not any(item["imported"] for item in listed.body["items"])
    imported = _call({"action": "import", "kind": "issues", "keys": ["2"]})
    assert imported.body["imported"] == 1
    assert [row["titleIfNew"] for row in _inbox(db)] == ["Two"]
    listed = _call({"action": "candidates", "kind": "issues"})
    flags = {item["key"]: item["imported"] for item in listed.body["items"]}
    assert flags == {"1": False, "2": True}


def test_github_discussions_import_with_their_replies():
    db = _db()
    forge, _hosts = _install(db)
    _connect({"discussions": {"direction": "pull"}})
    forge.discussions = [{
        "id": "D_1", "number": 3, "title": "Roadmap", "body": "Ideas?",
        "url": "https://github.com/octo/engine/discussions/3",
        "closed": False, "createdAt": "2026-10-01T00:00:00Z",
        "updatedAt": "2026-10-02T00:00:00Z", "author": {"login": "fay"},
        "category": {"name": "Ideas"},
        "comments": {"totalCount": 1, "nodes": [
            {"id": "C1", "body": "More speed", "url": "u",
             "createdAt": "2026-10-01T01:00:00Z", "author": {"login": "gus"}}]},
    }]
    assert _call({"action": "sync"}).body["sync"]["imported"] == 1
    row = _inbox(db, "discussion_inbox")[0]
    assert row["event"]["title"] == "Roadmap"
    assert row["event"]["category"] == "Ideas"
    assert "More speed" in row["event"]["body"]


def test_forkmesh_activity_is_sent_out_and_comments_follow_their_issue():
    db = _db()
    forge, _hosts = _install(db)
    _connect({"issues": {"direction": "push"},
              "releases": {"direction": "push"}})
    env = object()
    assert asyncio.run(mirror.note_event(env, "alice", "engine",
                                         "issue.opened", {
                                             "number": 4, "title": "Crash",
                                             "author": "alice",
                                             "body": "Steps"})) == 1
    assert forge.created_issues[0]["title"] == "Crash"
    assert mirror.MARKER in forge.created_issues[0]["body"]
    assert "/alice/engine/issues/4" in forge.created_issues[0]["body"]
    asyncio.run(mirror.note_event(env, "alice", "engine", "issue.comment", {
        "number": 4, "author": "bob", "body": "Me too"}))
    assert forge.created_comments[0][0] == 101
    asyncio.run(mirror.note_event(env, "alice", "engine",
                                  "release.published", {
                                      "tag": "v1.0", "name": "One",
                                      "notes": "Notes", "author": "alice"}))
    assert forge.created_releases[0]["tag_name"] == "v1.0"
    assert db.execute("SELECT COUNT(*) FROM repo_provider_mirror_outbox"
                      ).fetchone()[0] == 0
    echoed = asyncio.run(mirror.note_event(env, "alice", "engine",
                                           "issue.opened", {
                                               "number": 5, "title": "Echo",
                                               "body": "x\n" + mirror.MARKER}))
    assert echoed == 0
    assert asyncio.run(mirror.note_event(env, "alice", "engine",
                                         "pull.opened", {
                                             "number": 1, "title": "P"})) == 0


def test_failed_deliveries_stay_queued_with_backoff():
    db = _db()
    forge, _hosts = _install(db)
    _connect({"issues": {"direction": "push"}})

    async def down(method, url, headers, body=None):
        if urlparse(url).path.endswith("/issues") and method == "POST":
            return 503, {"message": "busy"}
        return await FakeForge()(method, url, headers, body)

    mirror.__dict__["_http"] = down
    asyncio.run(mirror.note_event(object(), "alice", "engine",
                                  "issue.opened", {"number": 1,
                                                   "title": "Later"}))
    row = db.execute("SELECT attempts, next_ts FROM repo_provider_mirror_outbox"
                     ).fetchone()
    assert row["attempts"] == 1
    assert row["next_ts"] > _Date.clock[0]


def test_paused_or_disconnected_mirrors_do_nothing():
    db = _db()
    forge, _hosts = _install(db)
    _connect({"issues": {"direction": "both"}})
    assert _call({"action": "toggle", "active": False}).body["mirror"][
        "active"] is False
    assert asyncio.run(mirror.note_event(object(), "alice", "engine",
                                         "issue.opened", {"number": 1})) == 0
    assert asyncio.run(mirror.push_config(object(), "alice", "engine")) is None
    _call({"action": "toggle", "active": True})
    gone = _call({"action": "disconnect"})
    assert gone.body["mirror"] is None
    for table in ("repo_provider_mirrors", "repo_provider_mirror_links",
                  "repo_provider_mirror_outbox"):
        assert db.execute("SELECT COUNT(*) FROM %s" % table).fetchone()[0] == 0


def test_push_config_and_report_round_trip_for_the_core_daemon():
    db = _db()
    _install(db)
    _connect()
    config = asyncio.run(mirror.push_config(object(), "alice", "engine"))
    assert config["url"] == "https://github.com/octo/engine.git"
    assert config["username"] == "x-access-token"
    assert config["token"] == "ghp_secret_token_1234"
    assert config["prune"] is False
    revision = config["revision"]
    report = {"revision": revision, "digest": "a" * 64, "ok": True,
              "at": 123, "error": "", "refs": 4}
    assert asyncio.run(mirror.record_push(object(), "alice", "engine",
                                          report)) is True
    assert asyncio.run(mirror.record_push(object(), "alice", "engine",
                                          report)) is False
    again = asyncio.run(mirror.push_config(object(), "alice", "engine"))
    assert again["revision"] == revision
    view = _call({"action": "get"}).body["mirror"]
    assert view["status"]["codePush"]["refs"] == 4
    _call({"action": "save", "provider": "github", "remote": "octo/engine",
           "token": "", "resources": {"code": {"direction": "off"}}})
    assert asyncio.run(mirror.push_config(object(), "alice", "engine")) is None


def test_gitlab_uses_private_token_and_project_paths():
    db = _db()
    forge, _hosts = _install(db, FakeForge("gitlab"))
    response = _connect({"issues": {"direction": "pull"}}, provider="gitlab")
    assert response.status == 200, response.body
    assert any(call["headers"].get("private-token") == "ghp_secret_token_1234"
               for call in forge.calls)
    assert any("/projects/octo%2Fengine" in call["url"]
               for call in forge.calls)
    config = asyncio.run(mirror.push_config(object(), "alice", "engine"))
    assert config["url"] == "https://gitlab.com/octo/engine.git"
    assert config["username"] == "oauth2"


def test_the_route_schema_and_registries_ship_with_the_module():
    assert urls.REPO_PROVIDER_MIRROR_RE.match(
        "/api/repo/alice/engine/provider-mirror").groups() == (
        "alice", "engine")
    tables = {"repo_provider_mirrors", "repo_provider_mirror_links",
              "repo_provider_mirror_outbox"}
    declared = {
        match.group(1)
        for statement in schema.SCHEMA_STATEMENTS
        for match in [re.search(r"CREATE TABLE IF NOT EXISTS (\w+)",
                                statement)]
        if match
    }
    assert tables <= declared
    reverse = (ROOT / "migrations" / "down"
               / "0257_repo_provider_mirrors.sql").read_text("utf-8")
    assert tables <= set(re.findall(r"DROP TABLE IF EXISTS (\w+)", reverse))
    entry = (ROOT / "src" / "entry.py").read_text("utf-8")
    assert '_LazyModule("repository_provider_mirror")' in entry
    scoped = entry.split("_REPO_SCOPED_TABLES = (", 1)[1].split(")\n", 1)[0]
    for table in tables:
        assert '("%s", "repo_bi")' % table in scoped, table
    assert 'phases.append("provider-mirror")' in entry
    assert 'if phase == "provider-mirror":' in entry
    activity = (ROOT / "src" / "cron_activity.py").read_text("utf-8")
    assert '("provider-mirror", 5, (4,), "integrations",' in activity
    footprint = (REPO / "world" / "tools"
                 / "build_worker_footprint.py").read_text("utf-8")
    assert '"repository_provider_mirror.py",' in footprint
    runtime = (ROOT / "src" / "app_runtime.py").read_text("utf-8")
    assert "repo_provider_mirror_handler(" in runtime
    apps = (ROOT / "src" / "apps_runtime.py").read_text("utf-8")
    assert 'globals().get("provider_mirror_note_event")' in apps
    releases = (ROOT / "src" / "fediverse_routes.py").read_text("utf-8")
    assert "provider_mirror_note_event(" in releases
    hosted = (ROOT / "src" / "hosted_repos.py").read_text("utf-8")
    assert 'item["pushMirror"] = mirror' in hosted


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
    module = ast.parse((ROOT / "src" / "repository_provider_mirror.py")
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


def test_the_settings_card_and_code_button_ship_in_the_bundle():
    public = ROOT / "public"
    fragments = public / "dashboard" / "js"
    detail = (fragments / "08-repo-detail-network.js").read_text("utf-8")
    content = (fragments / "06-repo-content.js").read_text("utf-8")
    explorer = (fragments / "05-repo-list-explorer.js").read_text("utf-8")
    bundle = (public / "dashboard.js").read_text("utf-8")
    settings_panel = detail.split("const settingsPanel = ", 1)[1].split(
        "detail.innerHTML = ", 1)[0]
    assert "data-repo-forge-mirror-card" in settings_panel
    assert "data-repo-forge-mirror-body" in settings_panel
    toolbar = detail.split("data-repo-root-toolbar", 1)[1].split(
        "data-repo-code-workspace", 1)[0]
    assert toolbar.count("${mirrorButton(") == 2
    assert "const mirrorButton = (compact) => canSeeSettingsTab" in detail
    assert "loadRepoProviderMirror(state.selectedRepo)" in explorer
    for action in ("save", "sync", "verify", "toggle", "disconnect",
                   "candidates", "import"):
        assert ('action: "%s"' % action) in content or (
            '"%s"]' % action) in content, action
    assert "escapeHtml(item.title)" in content
    assert "escapeHtml(mirror.remote)" in content
    for name in ("renderRepoForgeMirror", "handleRepoForgeMirrorClick",
                 "data-repo-forge-mirror-open", "data-repo-forge-mirror-card"):
        assert name in bundle, name
    defined = re.findall(r"\n  (?:async )?function (\w+)\(", bundle)
    duplicates = {name for name in defined if defined.count(name) > 1}
    assert not {name for name in duplicates if "ForgeMirror" in name}


def test_push_refs_setting_is_saved_returned_and_sent_to_the_daemon():
    db = _db()
    _install(db)
    view = _connect().body["mirror"]
    assert view["refs"] == "all"
    config = asyncio.run(mirror.push_config(object(), "alice", "engine"))
    assert config["refs"] == "all"
    first_revision = config["revision"]

    _Date.clock[0] += 1000
    saved = _call({"action": "save", "provider": "github",
                   "remote": "octo/engine", "token": "", "prune": True,
                   "refs": "default"})
    assert saved.status == 200, saved.body
    assert saved.body["mirror"]["refs"] == "default"
    assert saved.body["mirror"]["prune"] is True
    config = asyncio.run(mirror.push_config(object(), "alice", "engine"))
    assert config["refs"] == "default" and config["prune"] is True
    assert config["revision"] != first_revision
    assert _call({"action": "get"}).body["mirror"]["refs"] == "default"

    # Leaving refs out keeps the stored choice; a bad value is refused.
    _Date.clock[0] += 1000
    _call({"action": "save", "provider": "github", "remote": "octo/engine",
           "token": "", "prune": True})
    assert asyncio.run(mirror.push_config(
        object(), "alice", "engine"))["refs"] == "default"
    bad = _call({"action": "save", "provider": "github",
                 "remote": "octo/engine", "token": "", "refs": "feature"})
    assert bad.status == 400 and bad.body["error"] == "invalid_refs"
    assert _call({"action": "get"}).body["mirror"]["refs"] == "default"

    _Date.clock[0] += 1000
    _call({"action": "save", "provider": "github", "remote": "octo/engine",
           "token": "", "refs": "ALL"})
    assert asyncio.run(mirror.push_config(
        object(), "alice", "engine"))["refs"] == "all"


def test_records_saved_before_the_refs_setting_push_every_branch():
    db = _db()
    _install(db)
    _connect()
    row, record = asyncio.run(mirror._load(object(), _bi("alice/engine")))
    record.pop("refs", None)
    asyncio.run(mirror._store(object(), _bi("alice/engine"), record))
    assert _call({"action": "get"}).body["mirror"]["refs"] == "all"
    assert asyncio.run(mirror.push_config(
        object(), "alice", "engine"))["refs"] == "all"


def test_the_mirror_form_offers_the_branches_choice():
    public = ROOT / "public"
    content = (public / "dashboard" / "js" / "06-repo-content.js").read_text(
        "utf-8")
    form = content.split("function renderRepoForgeMirrorResourceRows", 1)[
        1].split("function renderRepoForgeMirrorForm", 1)[0]
    assert "data-repo-forge-mirror-refs" in form
    assert '<option value="all"' in form and "All branches" in form
    assert '<option value="default"' in form
    assert "Only the default branch" in form
    submit = content.split("async function handleRepoForgeMirrorSubmit", 1)[
        1].split("\n  }\n", 1)[0]
    assert 'querySelector("[data-repo-forge-mirror-refs]")' in submit
    assert "refs:" in submit
    bundle = (public / "dashboard.js").read_text("utf-8")
    assert "data-repo-forge-mirror-refs" in bundle
