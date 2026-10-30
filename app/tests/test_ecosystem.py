#!/usr/bin/env python3

import ast
import asyncio
import base64
import hashlib
import html
import importlib.util
import json
import sqlite3
import sys
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import parse_qs, quote, urlencode, urlparse

from cryptography.hazmat.primitives.asymmetric.ed25519 import (
    Ed25519PrivateKey,
)


ROOT = Path(__file__).resolve().parents[1]
REPOSITORY = ROOT.parent
SRC = ROOT / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import schema  # noqa: E402

NOW = 1_789_560_000_000
CSRF = "token-123"
ENTRY_TEXT = (SRC / "entry.py").read_text(encoding="utf-8")
CACHE_KEY = "https://forkmesh.internal/api/ecosystem"

GIT_BUG = {
    "slug": "git-bug",
    "name": "git-bug",
    "tagline": "Distributed issue tracker stored in Git",
    "summary": "Stores issues as Git objects.",
    "watching": "Issues that travel with the repository.",
    "url": "https://github.com/git-bug/git-bug",
    "repo": "https://github.com/git-bug/git-bug",
    "category": "issue-tracking",
    "tags": ["Offline First", "cli"],
    "openSource": True,
    "license": "GPL-3.0-or-later",
    "gitNative": True,
    "published": True,
    "featured": True,
    "internal": {"threat": "low", "action": "interoperate",
                 "notes": "Read the DAG entity spec."},
}
RADICLE = {
    "name": "Radicle",
    "tagline": "Peer-to-peer code collaboration built on Git",
    "url": "https://radicle.xyz",
    "category": "decentralized-git",
    "internal": {"threat": "medium"},
}


def _entry_defs(*names):
    namespace = {"quote": quote}
    for node in ast.parse(ENTRY_TEXT).body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) and (
                node.name in names):
            exec(ast.get_source_segment(ENTRY_TEXT, node), namespace)
    return namespace


class FakeResponse:
    def __init__(self, body, status=200, headers=None):
        self.body = body
        self.status = status
        self.headers = dict(headers or {})


class FakeRequest:
    def __init__(self, url, method="GET", body=""):
        self.url = url
        self.method = method
        self._body = body
        self.headers = {}

    async def text(self):
        return self._body


class TooLarge(BaseException):
    pass


def _load(name):
    spec = importlib.util.spec_from_file_location(
        name + "_under_test", SRC / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class Harness:
    def __init__(self):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        for sql in schema.SCHEMA_STATEMENTS:
            try:
                self.db.execute(sql)
            except sqlite3.Error:
                pass
        self.audits = []
        self.cache = {}
        self.purged = []
        self.shell = _load("admin_shell")
        self.eco = _load("ecosystem")
        self.key = Ed25519PrivateKey.generate()
        runtime = _entry_defs("_admin_href")
        harness = self

        async def d1_all(_env, sql, *args):
            return [dict(row) for row in harness.db.execute(sql, args)]

        async def d1_first(_env, sql, *args):
            rows = await d1_all(_env, sql, *args)
            return rows[0] if rows else None

        async def d1_run(_env, sql, *args):
            harness.db.execute(sql, args)

        async def d1_run_batch(_env, statements):
            with harness.db:
                for sql, args in statements:
                    harness.db.execute(sql, args)

        async def bounded_request_text(request, max_bytes):
            text = await request.text()
            if len(text.encode("utf-8")) > max_bytes:
                raise TooLarge()
            return text

        def json_response(data, status=200, cache_seconds=None,
                          cache_control=None, extra_headers=None):
            headers = {"content-type": "application/json; charset=utf-8"}
            if cache_control is not None:
                headers["cache-control"] = cache_control
            elif cache_seconds is not None:
                headers["cache-control"] = "public, max-age=%d" % cache_seconds
            headers.update(extra_headers or {})
            return FakeResponse(json.dumps(data), status, headers)

        async def edge_cache_match(key):
            return harness.cache.get(key)

        async def edge_cache_put(key, response):
            harness.cache[key] = response

        async def edge_cache_delete(key):
            harness.cache.pop(key, None)
            harness.purged.append(key)

        async def ensure_schema(_env):
            return None

        async def blind_index(_env, value):
            return "bi:" + str(value)

        async def admin_authorized(_env, node, ts, sig, canonical):
            if node != "root":
                return None
            try:
                harness.key.public_key().verify(
                    base64.urlsafe_b64decode(sig + "=" * (-len(sig) % 4)),
                    canonical)
            except Exception:
                return None
            return {"name": node, "status": "active"}

        async def audit(env, actor, action, target_type="", target="",
                        outcome="success", details=None):
            harness.audits.append(
                (actor, action, target_type, target, outcome, details))
            harness.db.execute(
                "INSERT INTO sensitive_audit_log (ts, actor_bi, action, "
                "target_type, target_token, outcome) VALUES (?,?,?,?,?,?)",
                (NOW, "bi:" + actor, action, target_type,
                 "bi:audit:%s:%s" % (target_type, target) if target else "",
                 outcome))

        runtime.update({
            "Response": FakeResponse,
            "Date": SimpleNamespace(now=lambda: NOW),
            "d1_all": d1_all, "d1_first": d1_first, "d1_run": d1_run,
            "d1_run_batch": d1_run_batch,
            "bounded_request_text": bounded_request_text,
            "RequestBodyTooLarge": TooLarge,
            "json_response": json_response,
            "edge_cache_match": edge_cache_match,
            "edge_cache_put": edge_cache_put,
            "edge_cache_delete": edge_cache_delete,
            "ensure_schema": ensure_schema,
            "_admin_csrf_ok": lambda env, form: (
                form.get("csrf", [""])[0] == CSRF),
            "_audit_sensitive_action": audit,
            "_admin_authorized": admin_authorized,
            "blind_index": blind_index,
            "LOGIN_MAX_SKEW_MS": 300_000,
            "method_name": lambda request: request.method,
            "_admin_shell": self.shell,
        })
        self.shell._bind_runtime({
            "d1_first": d1_first, "Date": SimpleNamespace(now=lambda: NOW)})
        self.eco._bind_runtime(runtime)
        self.env = SimpleNamespace(ADMIN_PATH="ops-console")

    def get(self, query):
        request = FakeRequest("https://app.example/ops-console?" + query)
        params = parse_qs(query, keep_blank_values=True)
        return asyncio.run(self.eco.admin_handler(
            self.env, request, params, '<input type="hidden" name="csrf" '
            'value="%s">' % CSRF, "admin=root", "root"))

    def post(self, fields, query="view=ecosystem", csrf=CSRF):
        body = urlencode(dict(fields, csrf=csrf))
        request = FakeRequest(
            "https://app.example/ops-console?" + query, "POST", body)
        return asyncio.run(self.eco.admin_handler(
            self.env, request, parse_qs(query), "", "admin=root", "root"))

    def import_json(self, value, **kwargs):
        text = value if isinstance(value, str) else json.dumps(value)
        return self.post({"action": "import", "json": text}, **kwargs)

    def api(self, method="GET"):
        request = FakeRequest("https://app.example/api/ecosystem", method)
        return asyncio.run(self.eco.api_handler(self.env, request))

    def rows(self):
        return {row["slug"]: dict(row) for row in self.db.execute(
            "SELECT * FROM ecosystem_projects")}


def _location(**params):
    return "/ops-console?" + "&".join(
        ["admin=root", "view=ecosystem"]
        + ["%s=%s" % (key, value) for key, value in params.items()])


def test_only_the_ecosystem_view_is_handled_here():
    h = Harness()
    assert h.get("view=users") is None
    assert h.get("view=bugs") is None
    page = h.get("view=ecosystem")
    assert page.status == 200
    assert page.headers["cache-control"] == "no-store"
    assert "<title>Ecosystem · ForkMesh admin</title>" in page.body
    assert "No projects yet. Import some JSON above." in page.body
    assert 'class="ab-item on" href="/ops-console?view=ecosystem"' in page.body


def test_import_is_csrf_checked_audited_and_redirected():
    h = Harness()
    blocked = h.import_json([GIT_BUG], csrf="wrong")
    assert blocked.status == 303
    assert blocked.headers["location"] == _location(notice="csrf")
    assert h.rows() == {}
    assert h.audits == []

    done = h.import_json([GIT_BUG, RADICLE])
    assert done.status == 303
    assert done.headers["location"] == _location(
        notice="imported", added=2, updated=0, unchanged=0)
    rows = h.rows()
    assert set(rows) == {"git-bug", "radicle"}
    assert rows["git-bug"]["published"] == 1
    assert rows["git-bug"]["featured"] == 1
    assert rows["radicle"]["published"] == 0
    assert rows["radicle"]["position"] > rows["git-bug"]["position"]
    assert rows["radicle"]["updated_by"] == "root"
    data = json.loads(rows["git-bug"]["data"])
    assert data["tags"] == ["offline-first", "cli"]
    assert data["compatibility"] == "unknown"
    assert data["integration"] == "watching"
    assert data["reviewed"] == "2026-09"
    assert "internal" not in data
    assert json.loads(rows["git-bug"]["internal"])["action"] == "interoperate"
    assert h.audits == [("root", "admin.ecosystem_import", "ecosystem", "2",
                         "success",
                         {"added": 2, "updated": 0, "unchanged": 0})]
    assert h.purged == [CACHE_KEY]

    page = h.get("view=ecosystem&notice=imported&added=2&updated=0"
                 "&unchanged=0").body
    assert "Imported 2 projects: 2 added, 0 updated, 0 unchanged." in page
    assert "git-bug" in page and "Radicle" in page


def test_import_merges_by_slug_and_keeps_what_it_leaves_out():
    h = Harness()
    h.import_json([GIT_BUG])
    before = h.rows()["git-bug"]
    merged = h.import_json({"slug": "git-bug", "published": False,
                            "internal": {"notes": None, "threat": "medium"}})
    assert merged.headers["location"] == _location(
        notice="imported", added=0, updated=1, unchanged=0)
    after = h.rows()["git-bug"]
    assert after["published"] == 0
    assert after["created_at"] == before["created_at"]
    assert json.loads(after["data"]) == json.loads(before["data"])
    assert json.loads(after["internal"]) == {"threat": "medium",
                                             "action": "interoperate"}

    h.purged.clear()
    same = h.import_json({"projects": [{"slug": "git-bug",
                                        "tagline": GIT_BUG["tagline"]}]})
    assert same.headers["location"] == _location(
        notice="imported", added=0, updated=0, unchanged=1)
    assert h.purged == []


def test_a_bad_import_saves_nothing_and_keeps_the_pasted_text():
    h = Harness()
    bad = {"slug": "bad", "name": "Bad", "tagline": "x",
           "url": "javascript:alert(1)", "category": "games", "stars": 5,
           "internal": {"threat": "extreme", "sources": ["not a url"]}}
    text = json.dumps([GIT_BUG, bad])
    page = h.import_json(text)
    assert page.status == 422
    assert h.rows() == {}
    assert h.audits == []
    body = html.unescape(page.body)
    assert "Nothing was saved." in body
    assert 'Project 2 (bad): unknown field "stars"' in body
    assert "Project 2 (bad): url must be an http(s) URL" in body
    assert "Project 2 (bad): category must be one of: decentralized-git" in body
    assert "Project 2 (bad): internal.threat must be one of: low, medium, high" in body
    assert "Project 2 (bad): internal.sources must be a list" in body
    assert html.escape(text, quote=True) in page.body

    for payload, message in (
            ("", "Paste a project, or a list of projects, as JSON."),
            ("[1, 2", "The JSON could not be parsed"),
            ('{"slug": "x", "position": NaN}', "NaN is not allowed"),
            ("[]", "non-empty list"),
            (json.dumps([GIT_BUG, GIT_BUG]),
             "Project 2 (git-bug): appears more than once")):
        response = h.import_json(payload)
        assert response.status == 422
        assert message in html.unescape(response.body)
    assert h.rows() == {}


def test_normalize_enforces_the_documented_format():
    eco = Harness().eco
    entry, errors = eco.normalize(dict(RADICLE, tags=["Peer to Peer", "CLI",
                                                      "cli"]),
                                  now_ms=NOW, position=10)
    assert errors == []
    assert entry["tags"] == ["peer-to-peer", "cli"]
    assert entry["published"] is False
    assert entry["position"] == 10
    assert entry["openSource"] is None

    _, errors = eco.normalize({"name": "X"}, now_ms=NOW)
    assert errors == ["tagline is required", "url is required",
                      "category is required"]
    for field, value, message in (
            ("reviewed", "2026-13", "reviewed must be a date"),
            ("reviewed", "2026-02-30", "reviewed must be a date"),
            ("url", "https://user:pw@example.com", "url must be an http(s)"),
            ("url", "ftp://example.com", "url must be an http(s)"),
            ("repo", 5, "repo must be an http(s)"),
            ("position", True, "position must be a whole number"),
            ("published", "yes", "published must be true or false"),
            ("openSource", 1, "openSource must be true or false or null"),
            ("compatibility", "maybe", "compatibility must be one of"),
            ("tags", ["ok", "no spaces allowed!"], 'tag "no spaces allowed!"'),
            ("integrations", ["x" * 101], "integrations must be a list"),
            ("name", "x" * 81, "name is longer than 80 characters")):
        _, errors = eco.normalize(dict(RADICLE, **{field: value}), now_ms=NOW)
        assert any(message in error for error in errors), (field, errors)
    assert eco.normalize(dict(RADICLE, reviewed="2026-02-28"),
                         now_ms=NOW)[1] == []
    assert eco.slug_from_name("  Git Bug! (beta) ") == "git-bug-beta"
    assert eco.clean_slug("-bad") == ""
    assert eco.reviewed_label("2026-09-25") == "Sep 2026"


def test_third_parties_forkmesh_runs_on_have_their_own_vocabulary():
    eco = Harness().eco
    stripe = {"name": "Stripe", "tagline": "Online payments",
              "url": "https://stripe.com", "category": "payments",
              "integration": "uses", "published": True,
              "integrations": ["App relay: card checkout"]}
    entry, errors = eco.normalize(stripe, now_ms=NOW)
    assert errors == []
    project = eco.public_project(dict(entry, slug="stripe"))
    assert project["categoryLabel"] == "Payments & billing"
    assert project["integrationLabel"] == "Used by ForkMesh"
    for key in ("cloud-hosting", "networking", "ai-models", "messaging",
                "storage", "security", "analytics", "runtimes", "libraries",
                "platforms", "editors", "build-ci", "design-assets", "apps",
                "work-management"):
        assert eco.normalize(dict(stripe, category=key), now_ms=NOW)[1] == []
    assert len(set(eco.CATEGORY_LABELS)) == len(eco.CATEGORIES)
    assert len(set(eco.CATEGORY_LABELS.values())) == len(eco.CATEGORIES)


def test_comparison_rows_are_checked_and_public():
    eco = Harness().eco
    row = {"topic": "Where repositories live", "project": "  On  GitHub ",
           "forkmesh": "On developer-operated nodes and signed mirrors"}
    entry, errors = eco.normalize(dict(RADICLE, comparison=[row]),
                                  now_ms=NOW)
    assert errors == []
    assert entry["comparison"][0]["project"] == "On GitHub"
    project = eco.public_project(dict(entry, slug="radicle"))
    assert project["comparison"] == entry["comparison"]
    assert eco.normalize(dict(RADICLE, comparison=None),
                         now_ms=NOW)[0]["comparison"] == []
    for value in ([row] * 7, [dict(row, topic="x" * 41)],
                  [dict(row, project="x" * 201)], [dict(row, forkmesh="")],
                  [dict(row, extra="x")], [{"topic": "Focus"}], ["Focus"],
                  {"topic": "Focus"}, [dict(row, project=5)]):
        _, errors = eco.normalize(dict(RADICLE, comparison=value),
                                  now_ms=NOW)
        assert any("comparison must be a list" in error
                   for error in errors), value


def test_the_public_api_lists_only_published_projects_and_public_fields():
    h = Harness()
    h.import_json([GIT_BUG, RADICLE, dict(RADICLE, slug="tangled",
                                          name="Tangled", published=True)])
    response = h.api()
    assert response.status == 200
    assert response.headers["cache-control"] == "public, max-age=60"
    payload = json.loads(response.body)
    assert [project["slug"] for project in payload["projects"]] == [
        "git-bug", "tangled"]
    assert payload["categories"] == [
        {"id": "decentralized-git", "label": "Decentralized Git", "count": 1},
        {"id": "issue-tracking", "label": "Issue tracking", "count": 1}]
    project = payload["projects"][0]
    assert set(project) == {
        "slug", "name", "tagline", "summary", "watching", "url", "repo",
        "category", "categoryLabel", "tags", "openSource", "license",
        "gitNative", "maker", "compatibility", "compatibilityLabel",
        "integration", "integrationLabel", "integrations", "comparison",
        "reviewed", "reviewedLabel", "featured"}
    assert project["comparison"] == []
    assert project["compatibilityLabel"] == "Not yet assessed"
    assert project["integrationLabel"] == "Watching"
    assert project["reviewedLabel"] == "Sep 2026"
    assert "Read the DAG" not in response.body
    assert "interoperate" not in response.body
    assert h.api() is response

    h.post({"action": "unpublish", "project": "git-bug"})
    assert CACHE_KEY not in h.cache
    assert [project["slug"] for project in json.loads(
        h.api().body)["projects"]] == ["tangled"]

    refused = h.api("POST")
    assert refused.status == 405
    assert refused.headers["allow"] == "GET, HEAD"


def test_publish_and_feature_toggles_are_audited_and_keep_the_tab():
    h = Harness()
    h.import_json([RADICLE])
    published = h.post({"action": "publish", "project": "radicle",
                        "back": "drafts"})
    assert published.headers["location"] == _location(
        status="drafts", notice="publish", slug="radicle")
    assert h.rows()["radicle"]["published"] == 1
    again = h.post({"action": "publish", "project": "radicle"})
    assert again.headers["location"] == _location(notice="unchanged",
                                                  slug="radicle")
    h.post({"action": "feature", "project": "radicle"})
    assert h.rows()["radicle"]["featured"] == 1
    missing = h.post({"action": "feature", "project": "nope"})
    assert missing.headers["location"] == _location(notice="missing")
    assert [audit[1] for audit in h.audits] == [
        "admin.ecosystem_import", "admin.ecosystem_publish",
        "admin.ecosystem_feature"]
    notice = h.get("view=ecosystem&notice=publish&slug=radicle").body
    assert "radicle is now public." in notice


def test_editing_replaces_the_whole_project_and_keeps_its_slug():
    h = Harness()
    h.import_json([GIT_BUG])
    page = h.get("view=ecosystem&project=git-bug")
    assert page.status == 200
    assert "Read the DAG entity spec." in page.body
    assert "What visitors see on /ecosystem/git-bug." in page.body

    edited = dict(GIT_BUG, tagline="Issues in Git")
    del edited["internal"]
    del edited["featured"]
    saved = h.post({"action": "save", "project": "git-bug",
                    "json": json.dumps(edited)})
    assert saved.headers["location"] == _location(notice="saved",
                                                  slug="git-bug")
    row = h.rows()["git-bug"]
    assert json.loads(row["data"])["tagline"] == "Issues in Git"
    assert json.loads(row["internal"]) == {}
    assert row["featured"] == 0
    assert h.audits[-1][1:4] == ("admin.ecosystem_save", "ecosystem_project",
                                 "git-bug")

    renamed = h.post({"action": "save", "project": "git-bug",
                      "json": json.dumps(dict(edited, slug="bug"))})
    assert renamed.status == 422
    assert "The slug cannot change here." in renamed.body
    assert set(h.rows()) == {"git-bug"}

    missing = h.get("view=ecosystem&project=nope")
    assert "That project no longer exists." in missing.body


def test_deleting_needs_the_confirmation_box():
    h = Harness()
    h.import_json([GIT_BUG])
    unconfirmed = h.post({"action": "delete", "project": "git-bug"})
    assert unconfirmed.headers["location"] == _location(
        project="git-bug", notice="confirm_delete")
    assert set(h.rows()) == {"git-bug"}
    deleted = h.post({"action": "delete", "project": "git-bug",
                      "confirm": "yes"})
    assert deleted.headers["location"] == _location(notice="deleted",
                                                    slug="git-bug")
    assert h.rows() == {}
    assert h.audits[-1] == ("root", "admin.ecosystem_delete",
                            "ecosystem_project", "git-bug", "success", {})


def test_an_export_imports_back_unchanged():
    h = Harness()
    h.import_json([GIT_BUG, RADICLE])
    export = h.get("view=ecosystem&format=json")
    assert export.status == 200
    assert export.headers["content-disposition"] == (
        'attachment; filename="forkmesh-ecosystem-2026-09-16.json"')
    projects = json.loads(export.body)
    assert [project["slug"] for project in projects] == ["git-bug", "radicle"]
    assert projects[0]["internal"]["notes"] == "Read the DAG entity spec."
    assert list(projects[0]) == list(Harness().eco.FIELDS)
    again = h.import_json(export.body)
    assert again.headers["location"] == _location(
        notice="imported", added=0, updated=0, unchanged=2)


def test_stored_text_is_escaped_in_the_admin():
    h = Harness()
    hostile = '<img src=x onerror=alert(1)>"'
    h.import_json(dict(RADICLE, name=hostile, tagline=hostile,
                       internal={"notes": hostile}))
    for query in ("view=ecosystem", "view=ecosystem&project=radicle"):
        page = h.get(query).body
        assert "<img src=x" not in page
        assert "&lt;img src=x onerror=alert(1)&gt;" in page


def test_an_oversized_form_is_refused():
    h = Harness()
    page = h.import_json("x" * (1024 * 1024 + 1))
    assert page.status == 413
    assert "larger than 1 MB" in page.body
    assert h.rows() == {}


def test_console_router_and_sidebar_reach_the_ecosystem():
    runtime = (SRC / "app_runtime.py").read_text(encoding="utf-8")
    start = runtime.index("    async def _admin(self, request):")
    admin = runtime[start:runtime.index("\n    async def _route(", start)]
    dispatch = admin.index('if params.get("view", [""])[0] == "ecosystem":')
    assert admin.index("csrf_field = (") < dispatch
    assert dispatch < admin.index('if method_name(request) == "POST":')
    assert 'if url.path in ("/api/ecosystem", "/api/ecosystem/"):' in runtime
    assert ('if url.path in ("/api/ecosystem/import", '
            '"/api/ecosystem/import/"):') in runtime
    assert '_ecosystem = _LazyModule("ecosystem")' in ENTRY_TEXT
    assert 'ecosystem_api_handler = _ecosystem.export("api_handler")' in (
        ENTRY_TEXT)
    assert ('admin_ecosystem_handler = _ecosystem.export("admin_handler")'
            in ENTRY_TEXT)
    assert ('ecosystem_import_handler = _ecosystem.export('
            '"import_api_handler")') in ENTRY_TEXT
    footprint = (REPOSITORY / "world" / "tools" /
                 "build_worker_footprint.py").read_text(encoding="utf-8")
    assert '"ecosystem.py",' in footprint
    shell = _load("admin_shell")
    assert ("ecosystem", "Ecosystem", "globe", ("console", "ecosystem")) in (
        dict(shell.NAV)["Growth"])


def test_the_migration_builds_the_runtime_table_and_the_down_drops_it():
    up = (ROOT / "migrations" / "0213_ecosystem_projects.sql").read_text(
        encoding="utf-8")
    down = (ROOT / "migrations" / "down" /
            "0213_ecosystem_projects.sql").read_text(encoding="utf-8")
    migrated = sqlite3.connect(":memory:")
    migrated.execute("CREATE TABLE schema_meta (k TEXT PRIMARY KEY, "
                     "v TEXT NOT NULL)")
    migrated.executescript(up)
    runtime = Harness().db

    def shape(db):
        return [tuple(row)[1:] for row in db.execute(
            "PRAGMA table_info(ecosystem_projects)")]

    assert shape(migrated) == shape(runtime)
    migrated.executescript(down)
    assert migrated.execute(
        "SELECT COUNT(*) FROM sqlite_master WHERE name LIKE "
        "'%ecosystem_projects%'").fetchone()[0] == 0
    assert migrated.execute("SELECT COUNT(*) FROM schema_meta").fetchone()[
        0] == 0



def _b64url(data):
    return base64.urlsafe_b64encode(data).decode("ascii").rstrip("=")


def _signed_import(h, payload, node="root", request_id="req-0123456789abcdef",
                   key=None, signed_payload=None, method="POST"):
    text = payload if isinstance(payload, str) else json.dumps(payload)
    signed = text if signed_payload is None else signed_payload
    ts = str(NOW)
    signature = (key or h.key).sign(
        h.eco.import_canonical(node, ts, request_id, signed))
    body = json.dumps({"node": node, "ts": ts, "requestId": request_id,
                       "payload": text, "sig": _b64url(signature)})
    request = FakeRequest("https://app.example/api/ecosystem/import", method,
                          body)
    return asyncio.run(h.eco.import_api_handler(h.env, request))


def test_the_import_canonical_is_the_documented_string():
    eco = Harness().eco
    assert eco.import_canonical(
        "root", "1789560000000", "req-0123456789abcdef", "[]") == (
        "forkmesh-ecosystem-import-v1\nroot\n1789560000000\n"
        "req-0123456789abcdef\n" + hashlib.sha256(b"[]").hexdigest()).encode()


def test_a_signed_import_writes_projects_once_per_request():
    h = Harness()
    done = _signed_import(h, [GIT_BUG, RADICLE])
    assert done.status == 200
    assert done.headers["cache-control"] == "no-store"
    assert json.loads(done.body) == {"ok": True, "added": 2, "updated": 0,
                                     "unchanged": 0}
    rows = h.rows()
    assert set(rows) == {"git-bug", "radicle"}
    assert rows["git-bug"]["updated_by"] == "root"
    assert rows["radicle"]["published"] == 0
    assert [audit[1:5] for audit in h.audits] == [
        ("admin.ecosystem_api_import", "ecosystem_request",
         "req-0123456789abcdef", "requested"),
        ("admin.ecosystem_api_import", "ecosystem_request",
         "req-0123456789abcdef", "success")]
    assert h.purged == [CACHE_KEY]

    replayed = _signed_import(h, [GIT_BUG])
    assert replayed.status == 409
    assert json.loads(replayed.body)["error"] == "replayed_request"

    again = _signed_import(h, [GIT_BUG], request_id="req-fedcba9876543210")
    assert json.loads(again.body) == {"ok": True, "added": 0, "updated": 0,
                                      "unchanged": 1}


def test_a_signed_import_refuses_strangers_tampering_and_bad_projects():
    h = Harness()
    for response, status, error in (
            (_signed_import(h, [GIT_BUG], key=Ed25519PrivateKey.generate()),
             401, "unauthorized"),
            (_signed_import(h, [GIT_BUG], node="mallory"), 401,
             "unauthorized"),
            (_signed_import(h, [GIT_BUG], signed_payload="[]"), 401,
             "unauthorized"),
            (_signed_import(h, [GIT_BUG], request_id="short"), 400,
             "invalid_request"),
            (_signed_import(h, [GIT_BUG], method="GET"), 405,
             "method_not_allowed")):
        assert response.status == status
        assert json.loads(response.body)["error"] == error
    assert h.audits == []

    invalid = _signed_import(h, [GIT_BUG, dict(RADICLE, url="ftp://x")],
                             request_id="req-0000000000000001")
    assert invalid.status == 400
    body = json.loads(invalid.body)
    assert body["error"] == "invalid_projects"
    assert body["errorCount"] == 2
    assert body["errors"][0].startswith("Project 2 (radicle): url must be")
    assert body["errors"][1] == "Project 2 (radicle): url is required"
    assert h.rows() == {}
    assert h.audits[-1][4] == "failed"
