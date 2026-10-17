import asyncio
import json
import re
from pathlib import Path
import sqlite3
import sys


PACKAGE = Path(__file__).resolve().parents[1]
REPO = PACKAGE.parents[2]
sys.path.insert(0, str(PACKAGE / "content" / "worker"))
import app_store_status  # noqa: E402


ACCOUNTS = {"alice", "bob"}


def doc_example():
    text = (REPO / "docs" / "marketplace" / "app-store-status.md").read_text(encoding="utf-8")
    return json.loads(re.search(r"```json\n(.*?)\n```", text, re.S).group(1))


class Runtime:
    def __init__(self):
        app_store_status._SCHEMA_READY = False
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.actor = "alice"
        self.method_value = "GET"
        self.body = {}
        self.origin_ok = True
        self.clock = 1_789_561_860_000
        self.audits = []

    def use(self, method, actor="alice", body=None):
        self.method_value, self.actor, self.body = method, actor, body or {}
        return self

    def method(self): return self.method_value
    def same_origin(self): return self.origin_ok
    def now(self): self.clock += 1; return self.clock
    def response(self, payload, status=200, cache_control=None, extra_headers=None):
        return {"status": status, "data": payload, "headers": extra_headers or {}}
    async def ensure_schema(self): pass
    async def json_body(self, _limit): return self.body, ""
    async def session(self, _data=None):
        bi = "bi:" + self.actor if self.actor in ACCOUNTS else ""
        return (bi, {"name": self.actor}) if bi else ("", None)
    async def seal(self, value): return "sealed:" + json.dumps(value, sort_keys=True)
    async def open(self, value):
        text = str(value or "")
        return json.loads(text.removeprefix("sealed:")) if text.startswith("sealed:") else None
    async def d1_first(self, sql, *args):
        row = self.db.execute(sql, args).fetchone()
        return dict(row) if row else None
    async def d1_run(self, sql, *args):
        cursor = self.db.execute(sql, args); self.db.commit(); return cursor
    async def d1_all(self, sql, *args):
        return [dict(row) for row in self.db.execute(sql, args).fetchall()]
    async def audit(self, *args, **kwargs): self.audits.append(args)


def call(runtime, path):
    return asyncio.run(app_store_status.handle(runtime, path))


def test_nothing_published_yet():
    runtime = Runtime()
    data = call(runtime.use("GET"), "/api/app-store-status")
    assert data == {"status": 200, "data": {
        "ok": True, "actor": "alice", "published": False, "publishedAt": 0, "snapshot": None},
        "headers": {"x-content-type-options": "nosniff"}}
    assert call(runtime.use("GET", actor="mallory"), "/api/app-store-status")["status"] == 401


def test_publish_seals_and_stays_private_to_the_account():
    runtime = Runtime()
    example = doc_example()
    stored = call(runtime.use("PUT", body=example), "/api/app-store-status/snapshot")
    assert stored["status"] == 200 and stored["data"]["publishedAt"] > 0
    data = call(runtime.use("GET"), "/api/app-store-status")["data"]
    assert data["published"] is True and data["publishedAt"] == stored["data"]["publishedAt"]
    assert data["snapshot"] == example
    raw = runtime.db.execute("SELECT data FROM app_store_status_snapshots").fetchone()[0]
    assert raw.startswith("sealed:")
    assert call(runtime.use("GET", actor="bob"), "/api/app-store-status")["data"]["snapshot"] is None
    assert runtime.audits[0][1] == "app-store-status.publish"


def test_clean_snapshot_clamps_and_drops_unknown_fields():
    dirty = doc_example()
    dirty["extra"] = "drop me"
    dirty["versions"][0]["status"]["tone"] = "purple"
    dirty["versions"][0]["createdAt"] = "<script>"
    dirty["groups"][0]["publicLink"] = "javascript:alert(1)"
    dirty["reviews"][0]["rating"] = 99
    dirty["downloads"]["total"]["downloads"] = -5
    dirty["unavailable"]["secrets"] = "nope"
    dirty["attention"] = ["x" * 999] * 50
    dirty["builds"][0]["groups"] = [{"nested": True}, "team"]
    clean = app_store_status.clean_snapshot(dirty)
    assert "extra" not in clean
    assert clean["versions"][0]["status"]["tone"] == "neutral"
    assert clean["versions"][0]["createdAt"] == ""
    assert clean["groups"][0]["publicLink"] == ""
    assert clean["reviews"][0]["rating"] == 5
    assert clean["downloads"]["total"]["downloads"] == 0
    assert clean["unavailable"] == {"feedback": "ERROR: App Store Connect GET answered 404"}
    assert len(clean["attention"]) == app_store_status.MAX_ATTENTION
    assert len(clean["attention"][0]) == 300
    assert clean["builds"][0]["groups"] == ["team"]
    assert app_store_status.clean_snapshot("junk", 7)["generatedAt"] == 7


def test_routes_methods_and_format():
    runtime = Runtime()
    assert call(runtime.use("POST"), "/api/app-store-status")["status"] == 405
    assert call(runtime.use("GET"), "/api/app-store-status/nope")["status"] == 404
    assert call(runtime.use("GET"), "/api/elsewhere")["status"] == 404
    assert call(runtime.use("PUT", body={"format": "other"}),
                "/api/app-store-status/snapshot")["status"] == 400
    runtime.origin_ok = False
    assert call(runtime.use("PUT", body=doc_example()),
                "/api/app-store-status/snapshot")["status"] == 403


def test_export_forget_and_purge():
    runtime = Runtime()
    call(runtime.use("PUT", body=doc_example()), "/api/app-store-status/snapshot")
    call(runtime.use("PUT", actor="bob", body=doc_example()), "/api/app-store-status/snapshot")
    exported = call(runtime.use("GET"), "/api/app-store-status/export")["data"]
    assert exported["filename"] == "forkmesh-app-store-status-alice.json"
    assert exported["export"]["snapshot"]["app"]["bundleId"] == "com.forkmesh.app"
    assert asyncio.run(app_store_status.account_installed(runtime, "bi:alice")) is True
    forgot = call(runtime.use("DELETE"), "/api/app-store-status/snapshot")["data"]
    assert forgot == {"ok": True, "published": False}
    assert asyncio.run(app_store_status.account_has_data(runtime, "bi:alice")) is False
    call(runtime.use("PUT", actor="bob", body=picture()), f"/api/app-store-status/screenshots/{SHOT}")
    asyncio.run(app_store_status.purge_account_data(runtime, "bi:bob", "bob"))
    count = runtime.db.execute("SELECT COUNT(*) FROM app_store_status_snapshots").fetchone()[0]
    assert count == 0
    shots = runtime.db.execute("SELECT COUNT(*) FROM app_store_status_screenshots").fetchone()[0]
    assert shots == 0


def test_manifest_matches_the_worker():
    manifest = json.loads((PACKAGE / "manifest.json").read_text(encoding="utf-8"))
    assert manifest["id"] == app_store_status.PLUGIN_ID
    assert manifest["worker"] == {"module": "app_store_status", "api": app_store_status.PREFIX,
                                  "outboundOrigins": []}
    assert manifest["dataLifecycle"] == {"scope": "account"}
    catalog = json.loads((REPO / "marketplace" / "catalog.json").read_text(encoding="utf-8"))
    row = next(item for item in catalog["packages"] if item["id"] == "app-store-status")
    assert row["version"] == manifest["version"]
    page = (PACKAGE / "content" / "web" / "page.js").read_text(encoding="utf-8")
    assert app_store_status.PREFIX in page
    report = (REPO / "native" / "ios" / "tools" / "asc_store_report.py").read_text(encoding="utf-8")
    assert app_store_status.REQUEST_PROOF in report and app_store_status.FORMAT in report


SHOT = "3f1c2a9b8d7e6f5a4b3c2d1e"
JPEG = b"\xff\xd8\xff\xe0" + b"\x00" * 64


def picture(raw=JPEG, mime="image/jpeg"):
    return {"mime": mime, "width": 1179, "height": 2556,
            "data": app_store_status.base64.b64encode(raw).decode("ascii")}


def test_screenshots_upload_only_for_the_published_snapshot_and_stay_private():
    runtime = Runtime()
    path = f"/api/app-store-status/screenshots/{SHOT}"
    assert call(runtime.use("PUT", body=picture()), path)["data"] == {
        "error": "screenshot_not_in_snapshot"}
    stored = call(runtime.use("PUT", body=doc_example()), "/api/app-store-status/snapshot")
    assert stored["data"]["missingScreenshots"] == [SHOT]
    assert call(runtime.use("GET"), path)["status"] == 404
    saved = call(runtime.use("PUT", body=picture()), path)
    assert saved["status"] == 200 and saved["data"]["id"] == SHOT
    raw = runtime.db.execute("SELECT data FROM app_store_status_screenshots").fetchone()[0]
    assert raw.startswith("sealed:")
    shown = call(runtime.use("GET"), path)["data"]
    assert shown == {"ok": True, "id": SHOT, **picture()}
    assert call(runtime.use("GET", actor="bob"), path)["status"] == 404
    again = call(runtime.use("PUT", body=doc_example()), "/api/app-store-status/snapshot")
    assert again["data"]["missingScreenshots"] == []
    assert runtime.audits[-2][1] == "app-store-status.screenshot"


def test_screenshots_must_be_real_pictures_under_the_cap():
    runtime = Runtime()
    call(runtime.use("PUT", body=doc_example()), "/api/app-store-status/snapshot")
    path = f"/api/app-store-status/screenshots/{SHOT}"
    assert call(runtime.use("PUT", body=picture(mime="image/gif")), path)["data"]["error"] == (
        "unsupported_image")
    assert call(runtime.use("PUT", body=picture(raw=b"<svg>")), path)["data"]["error"] == (
        "invalid_image")
    assert call(runtime.use("PUT", body={**picture(), "data": "%%%"}), path)["status"] == 400
    big = JPEG + b"\x00" * app_store_status.MAX_SCREENSHOT_BYTES
    assert call(runtime.use("PUT", body=picture(raw=big)), path)["status"] == 413
    assert call(runtime.use("GET"), "/api/app-store-status/screenshots/NOT-HEX")["status"] == 404
    assert call(runtime.use("DELETE"), path)["status"] == 405


def test_a_new_snapshot_drops_screenshots_it_no_longer_lists():
    runtime = Runtime()
    call(runtime.use("PUT", body=doc_example()), "/api/app-store-status/snapshot")
    call(runtime.use("PUT", body=picture()), f"/api/app-store-status/screenshots/{SHOT}")
    newer = doc_example()
    newer["feedback"][0]["screenshots"] = [{"id": "a" * 24, "width": 1, "height": 1},
                                           {"id": "../etc", "width": 1, "height": 1}]
    stored = call(runtime.use("PUT", body=newer), "/api/app-store-status/snapshot")
    assert stored["data"]["missingScreenshots"] == ["a" * 24]
    assert runtime.db.execute("SELECT COUNT(*) FROM app_store_status_screenshots").fetchone()[0] == 0
    snapshot = call(runtime.use("GET"), "/api/app-store-status")["data"]["snapshot"]
    assert snapshot["feedback"][0]["screenshots"] == [{"id": "a" * 24, "width": 1, "height": 1}]
