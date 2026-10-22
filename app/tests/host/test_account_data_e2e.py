from __future__ import annotations

import json
import re
import sqlite3
import subprocess
import sys

import pytest

from forkmesh_host import paths

from test_host_e2e import client, server  # noqa: F401


def _ops(server, *args):
    return subprocess.run(
        [sys.executable, "-m", "forkmesh_host", "ops", *args],
        cwd=str(paths.HOST_DIR), env=server["env"], capture_output=True,
        text=True, timeout=120)


def _account(client, server, name):
    page = client.get("/signup")
    field = re.search(r"contact_[0-9a-f]{18}", page.text).group(0)
    signup = client.post("/api/accounts/signup", json={
        "nodeName": name, field: name + "@example.com",
        "password": "correct-horse-battery",
        "turnstileToken": "XXXX.DUMMY.TOKEN.XXXX"})
    assert signup.status_code == 201, signup.text[:300]
    done = _ops(server, "verify-email", name)
    assert done.returncode == 0, done.stderr[-2000:]
    login = client.post("/api/accounts/login", json={
        "nodeName": name, "password": "correct-horse-battery"})
    assert login.status_code == 200, login.text[:300]
    return {"authorization": "Bearer " + login.json()["sessionToken"]}


@pytest.fixture(scope="module")
def people(client, server):  # noqa: F811
    owner = _account(client, server, "dataowner")
    org = client.post("/api/orgs", headers=owner,
                      json={"name": "datacrew", "displayName": "Data Crew"})
    assert org.status_code in (200, 201), org.text[:300]
    return {"owner": owner, "other": _account(client, server, "dataother")}


def _db_path(server):
    value = str(server["env"].get("FORKMESH_HOST_DB") or "")
    if value.startswith("sqlite:"):
        return value.split(":", 1)[1]
    return str(server["env"]["FORKMESH_HOST_DATA_DIR"]) + "/d1.sqlite"


def test_store_is_sealed_listed_and_private(client, server, people):  # noqa: F811
    owner, other = people["owner"], people["other"]
    url = "/api/accounts/data/store/ios.rss"
    assert client.put(url, json={"value": 1}).status_code == 401
    saved = client.put(url, headers=owner, json={
        "kind": "subscriptions", "label": "News reader subscriptions",
        "value": {"feeds": ["https://example.com/feed.xml"]}})
    assert saved.status_code == 200, saved.text[:300]
    got = client.get(url, headers=owner).json()
    assert got["item"]["value"] == {"feeds": ["https://example.com/feed.xml"]}
    assert client.get(url, headers=other).status_code == 404
    with sqlite3.connect(_db_path(server)) as db:
        row = db.execute("SELECT data FROM account_store").fetchone()
        wrapped = db.execute("SELECT wrapped FROM account_store_keys").fetchone()
    assert "example.com" not in row[0] and "feed.xml" not in row[0]
    assert wrapped and len(wrapped[0]) > 40


def test_inventory_rows_delete_and_export(client, people):  # noqa: F811
    owner = people["owner"]
    inventory = client.get("/api/accounts/data?refresh=1", headers=owner)
    assert inventory.status_code == 200, inventory.text[:500]
    payload = inventory.json()
    ids = [s["id"] for s in payload["subjects"]]
    assert ids[0] == "account" and "org:datacrew" in ids
    tables = {g["table"] for g in payload["groups"] if g["subject"] == "account"}
    assert {"users", "account_sessions"} <= tables
    assert [i["key"] for i in payload["store"]] == ["ios.rss"]
    users = client.get("/api/accounts/data/rows?subject=account&table=users",
                       headers=owner).json()
    record = users["rows"][0]["fields"]["data"]
    assert record["name"] == "dataowner"
    assert "correct-horse" not in json.dumps(users)
    org = client.get("/api/accounts/data/rows?subject=org:datacrew&table=orgs",
                     headers=owner).json()
    assert org["readable"] is True and org["rows"]
    exported = client.get("/api/accounts/export", headers=owner)
    assert exported.status_code == 200, exported.text[:500]
    body = exported.json()
    assert body["schema"] == "forkmesh-account-export-v1"
    assert body["account"]["name"] == "dataowner"
    assert body["store"][0]["value"] == {"feeds": ["https://example.com/feed.xml"]}
    assert client.get("/api/accounts/export", headers=owner).status_code == 429
    assert client.delete("/api/accounts/data/store", headers=owner).status_code == 200
    assert client.get("/api/accounts/data/store", headers=owner).json()["items"] == []
