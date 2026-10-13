"""Competitor Watch package API, schema, and dashboard contracts."""

import asyncio
import hashlib
import json
from pathlib import Path
import sqlite3
import sys


PACKAGE = Path(__file__).resolve().parents[1]
REPO = PACKAGE.parents[2]
APP = REPO / "app"
sys.path.insert(0, str(PACKAGE / "content" / "worker"))
import competitor


class Runtime:
    def __init__(self, installed=True):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.db.execute("CREATE TABLE schema_meta(k TEXT PRIMARY KEY,v TEXT NOT NULL)")
        self.db.executescript(
            sorted((APP / "migrations").glob("0*_org_plugin_installs.sql"))[-1]
            .read_text(encoding="utf-8")
        )
        if installed:
            self.db.executescript(";\n".join(competitor.DATA_SCHEMA_UP))
            self.db.execute(
                "INSERT INTO org_plugin_installs VALUES(?,?,?,?,?,?)",
                ("org:acme", "competitor", "acme", "bi:alice", "alice", 1),
            )
        self.db.executescript("""
          CREATE TABLE orgs(org_bi TEXT PRIMARY KEY,name TEXT,data TEXT,created_at INTEGER);
          CREATE TABLE org_members(org_bi TEXT,member_bi TEXT,role TEXT,name TEXT,
            created_at INTEGER,PRIMARY KEY(org_bi,member_bi));
          INSERT INTO orgs VALUES('org:acme','acme','sealed:{}',1);
          INSERT INTO org_members VALUES('org:acme','bi:alice','owner','alice',1);
          INSERT INTO org_members VALUES('org:acme','bi:charlie','member','charlie',1);
        """)
        self.actor = "alice"
        self.method_value = "GET"
        self.body = {}
        self.clock = 2_000_000_000_000
        self.ids = 0
        self.audits = []

    def use(self, method, actor="alice", body=None):
        self.method_value, self.actor, self.body = method, actor, body or {}
        return self

    def method(self): return self.method_value
    def same_origin(self): return True
    def now(self): self.clock += 1; return self.clock
    def new_id(self): self.ids += 1; return f"{self.ids:032x}"
    def response(self, payload, status=200, cache_control=None, extra_headers=None):
        return {"status": status, "data": payload, "headers": extra_headers or {}}
    async def ensure_schema(self): pass
    async def json_body(self, _limit): return self.body, ""
    async def session(self, _data=None):
        bi = "bi:" + self.actor if self.actor in {"alice", "charlie"} else ""
        return (bi, {"name": self.actor}) if bi else ("", None)
    async def organization(self, name, account_bi):
        row = self.db.execute(
            "SELECT o.org_bi,m.role FROM orgs o JOIN org_members m ON "
            "m.org_bi=o.org_bi WHERE o.name=? AND m.member_bi=?",
            (name, account_bi),
        ).fetchone()
        return (row["org_bi"], row["role"]) if row else ("", "")
    async def plugin_installed(self, org_bi, plugin):
        return bool(self.db.execute(
            "SELECT 1 FROM org_plugin_installs WHERE org_bi=? AND plugin=?",
            (org_bi, plugin),
        ).fetchone())
    async def seal(self, value): return "sealed:" + json.dumps(value, sort_keys=True)
    async def open(self, value): return json.loads(str(value).removeprefix("sealed:"))
    async def d1_all(self, sql, *args):
        return [dict(row) for row in self.db.execute(sql, args).fetchall()]
    async def d1_first(self, sql, *args):
        row = self.db.execute(sql, args).fetchone()
        return dict(row) if row else None
    async def d1_run(self, sql, *args):
        cursor = self.db.execute(sql, args); self.db.commit(); return cursor
    async def audit(self, *args, **kwargs): self.audits.append((args, kwargs))


def call(runtime, path):
    return asyncio.run(competitor.handle(runtime, path))


def test_competitor_profiles_are_encrypted_shared_and_filter_ready():
    runtime = Runtime()
    root = call(runtime.use("GET"), "/api/competitors")
    assert root["data"]["organizations"] == [
        {"name": "acme", "role": "owner", "installed": True}
    ]
    created = call(runtime.use("POST", body={
        "name": "Northstar", "website": "https://northstar.example",
        "category": "Developer tools", "status": "primary",
        "positioning": "Hosted collaboration", "strengths": "Polished UI",
        "weaknesses": "Centralized", "notes": "Review pricing quarterly",
    }), "/api/competitors/acme/items")
    assert created["status"] == 201
    item = created["data"]["competitor"]
    assert (item["name"], item["status"]) == ("Northstar", "primary")
    stored = runtime.db.execute("SELECT data FROM competitor_profiles").fetchone()[0]
    assert stored.startswith("sealed:")
    assert runtime.db.execute("SELECT COUNT(*) FROM competitor_profiles").fetchone()[0] == 1

    overview = call(runtime.use("GET", actor="charlie"), "/api/competitors/acme")
    assert overview["data"]["summary"] == {
        "watchlist": 1, "primary": 1, "archived": 0, "needsReview": 0,
    }
    assert overview["data"]["competitors"][0]["strengths"] == "Polished UI"

    updated = call(runtime.use("PATCH", body={"status": "archived"}),
                   f"/api/competitors/acme/items/{item['id']}")
    assert updated["data"]["competitor"]["status"] == "archived"
    deleted = call(runtime.use("DELETE"),
                   f"/api/competitors/acme/items/{item['id']}")
    assert deleted["data"]["deleted"] is True


def test_competitor_rejects_bad_urls_and_uninstalled_access():
    runtime = Runtime()
    bad = call(runtime.use("POST", body={"name": "Bad", "website": "http://bad"}),
               "/api/competitors/acme/items")
    assert bad["status"] == 400
    assert bad["data"]["error"] == "invalid_website"
    missing = Runtime(installed=False)
    response = call(missing.use("GET"), "/api/competitors/acme")
    assert response["status"] == 409
    assert response["data"]["canInstall"] is True


def test_competitor_lifecycle_exports_and_purges_all_rows():
    runtime = Runtime()
    call(runtime.use("POST", body={"name": "Waypoint"}),
         "/api/competitors/acme/items")
    exported = asyncio.run(competitor.export_org_data(runtime, "org:acme", "acme"))
    assert exported["format"] == "forkmesh-competitor-export-v1"
    assert [item["name"] for item in exported["competitors"]] == ["Waypoint"]
    assert asyncio.run(competitor.org_has_data(runtime, "org:acme")) is True
    asyncio.run(competitor.purge_org_data(runtime, "org:acme"))
    assert asyncio.run(competitor.org_has_data(runtime, "org:acme")) is False


def test_competitor_package_owns_dashboard_and_reversible_data_contracts():
    manifest = json.loads((PACKAGE / "manifest.json").read_text(encoding="utf-8"))
    page = (PACKAGE / "content/web/page.js").read_text(encoding="utf-8")
    view = (PACKAGE / "content/web/view.html").read_text(encoding="utf-8")
    assert manifest["worker"] == {"module": "competitor", "api": "/api/competitors"}
    assert manifest["dataLifecycle"] == {"scope": "organization"}
    assert manifest["install"] == {"bundled": True, "default": False}
    assert "data-competitor-install" in view
    assert "data-competitor-uninstall" in view
    assert "data-competitor-export" in view
    assert 'registerPage("competitors"' in page
    assert 'pluginPath("/uninstall")' in page
    assert "http://" not in page
