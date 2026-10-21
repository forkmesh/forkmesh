"""Organization CRM ACL, CSV, email, and plugin-surface contracts."""

import asyncio
import hashlib
import json
from pathlib import Path
import sqlite3
import sys
from urllib.parse import parse_qs, urlparse


CRM_PACKAGE = Path(__file__).resolve().parents[1]
REPO = CRM_PACKAGE.parents[2]
ROOT = REPO / "app"
sys.path.insert(0, str(ROOT / "src"))
sys.path.insert(0, str(CRM_PACKAGE / "content" / "worker"))
import crm


class Runtime:
    def __init__(self, installed=True):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.db.execute("CREATE TABLE schema_meta(k TEXT PRIMARY KEY,v TEXT NOT NULL)")
        self.db.executescript(sorted(
            (ROOT / "migrations").glob("0*_org_plugin_installs.sql"))[-1]
            .read_text())
        if installed:
            self.db.executescript(";\n".join(crm.DATA_SCHEMA_UP))
            self.db.execute(
                "INSERT INTO org_plugin_installs VALUES(?,?,?,?,?,?)",
                ("org:acme", "crm", "acme", "bi:alice", "alice", 1))
        self.db.executescript("""
          CREATE TABLE orgs(org_bi TEXT PRIMARY KEY,name TEXT,data TEXT,created_at INTEGER);
          CREATE TABLE org_members(
            org_bi TEXT,member_bi TEXT,role TEXT,name TEXT,created_at INTEGER,
            PRIMARY KEY(org_bi,member_bi));
          INSERT INTO orgs VALUES('org:acme','acme','sealed:{}',1);
          INSERT INTO org_members VALUES('org:acme','bi:alice','owner','alice',1);
          INSERT INTO org_members VALUES('org:acme','bi:charlie','member','charlie',1);
        """)
        self.accounts = {name: "bi:" + name for name in ("alice", "bob", "charlie")}
        self.actor = "alice"
        self.method_value = "GET"
        self.body = {}
        self.query = {}
        self.clock = 2_000_000_000_000
        self.ids = 0
        self.audits = []
        self.mailtrap_calls = []
        self.outbound_calls = []
        self.zoho_messages = []
        self.pings = []

    def use(self, method, actor="alice", body=None, query=None):
        self.method_value = method
        self.actor = actor
        self.body = body or {}
        self.query = query or {}
        return self

    def method(self): return self.method_value
    def query_params(self): return dict(self.query)
    def same_origin(self): return True
    def now(self):
        self.clock += 1
        return self.clock
    def new_id(self):
        self.ids += 1
        return f"{self.ids:032x}"
    def response(self, payload, status=200, cache_control=None, extra_headers=None):
        return {"status": status, "data": payload, "headers": extra_headers or {}}
    async def ensure_schema(self): pass
    async def json_body(self, _limit): return self.body, ""
    async def session(self, _data=None):
        account_bi = self.accounts.get(self.actor, "")
        return (account_bi, {"name": self.actor}) if account_bi else ("", None)
    async def organization(self, name, account_bi):
        row = self.db.execute(
            "SELECT o.org_bi,m.role FROM orgs o JOIN org_members m "
            "ON m.org_bi=o.org_bi WHERE o.name=? AND m.member_bi=?",
            (name, account_bi),
        ).fetchone()
        return (row["org_bi"], row["role"]) if row else ("", "")
    async def plugin_installed(self, org_bi, plugin):
        return bool(self.db.execute(
            "SELECT 1 FROM org_plugin_installs WHERE org_bi=? AND plugin=?",
            (org_bi, plugin),
        ).fetchone())
    async def blind(self, value):
        return hashlib.sha256(str(value).encode()).hexdigest()
    async def seal(self, value):
        return "sealed:" + json.dumps(value, sort_keys=True)
    async def open(self, value):
        return json.loads(str(value).removeprefix("sealed:"))
    async def d1_all(self, sql, *args):
        return [dict(row) for row in self.db.execute(sql, args).fetchall()]
    async def d1_first(self, sql, *args):
        row = self.db.execute(sql, args).fetchone()
        return dict(row) if row else None
    async def d1_run(self, sql, *args):
        cursor = self.db.execute(sql, args)
        self.db.commit()
        return cursor
    async def audit(self, *args, **kwargs):
        self.audits.append((args, kwargs))
    async def notify(self, recipient, kind, title, **kwargs):
        self.pings.append(dict(kwargs, recipient=recipient, kind=kind, title=title))
    def mailtrap_configured(self): return False
    async def send_mailtrap(self, token, payload):
        self.mailtrap_calls.append((token, payload))
        return {"accepted": True, "messageId": "mailtrap-123", "error": ""}
    async def outbound_json(self, url, method="GET", headers=None, body=""):
        self.outbound_calls.append((url, method, dict(headers or {}), body))
        if url.endswith("/oauth/v2/token"):
            return {"ok": True, "status": 200,
                    "data": {"access_token": "request-local-token"}}
        if url.endswith("/folders"):
            names = list(dict.fromkeys(
                item.get("sourceFolder") or "Inbox" for item in self.zoho_messages))
            return {"ok": True, "status": 200, "data": {"data": [
                {"folderId": f"folder-{index}", "folderName": name}
                for index, name in enumerate(names)]}}
        if "/messages/view?" in url:
            params = parse_qs(urlparse(url).query)
            folder_id = params.get("folderId", [""])[0]
            names = list(dict.fromkeys(
                item.get("sourceFolder") or "Inbox" for item in self.zoho_messages))
            index = int(folder_id.removeprefix("folder-"))
            messages = [item for item in self.zoho_messages
                        if (item.get("sourceFolder") or "Inbox") == names[index]]
            return {"ok": True, "status": 200, "data": {"data": [{
                "messageId": item.get("providerMessageId"),
                "threadId": item.get("threadId"), "subject": item.get("subject"),
                "summary": item.get("summary"), "sender": item.get("sender"),
                "fromAddress": item.get("from"), "toAddress": item.get("to"),
                "flagid": item.get("flag"),
                "hasAttachment": "1" if item.get("hasAttachment") else "0",
                "receivedTime": str(item.get("receivedAt") or 0),
            } for item in messages]}}
        return {"ok": False, "status": 404, "error": "unexpected_test_url"}


def call(runtime, path):
    return asyncio.run(crm.handle(runtime, path))


def test_contacts_are_encrypted_and_shared_only_with_organization_members():
    runtime = Runtime()
    root = call(runtime.use("GET"), "/api/crm")
    assert root["data"]["organizations"] == [
        {"name": "acme", "role": "owner", "installed": True}]

    created = call(runtime.use("POST", body={
        "name": "Ada Lovelace", "email": "ada@example.com",
        "company": "Analytical Engines", "stage": "qualified",
        "twitter": "@ada", "mastodon": "@ada@mathstodon.xyz",
        "bluesky": "ada.bsky.social", "linkedin": "linkedin.com/in/ada",
        "github": "@ada", "website": "https://ada.example",
        "location": "London, UK",
        "tags": ["Founder", "Priority"], "notes": "Warm introduction",
    }), "/api/crm/acme/contacts")
    assert created["status"] == 201
    contact_id = created["data"]["contact"]["id"]
    stored = runtime.db.execute(
        "SELECT email_bi,data FROM crm_contacts WHERE contact_id=?", (contact_id,)
    ).fetchone()
    assert stored["email_bi"] and "ada@example.com" not in stored["email_bi"]
    assert stored["data"].startswith("sealed:")
    assert created["data"]["contact"]["twitter"] == "@ada"
    assert created["data"]["contact"]["mastodon"] == "@ada@mathstodon.xyz"
    assert created["data"]["contact"]["bluesky"] == "ada.bsky.social"
    assert created["data"]["contact"]["linkedin"] == "linkedin.com/in/ada"
    assert created["data"]["contact"]["github"] == "@ada"
    assert created["data"]["contact"]["website"] == "https://ada.example"
    assert created["data"]["contact"]["location"] == "London, UK"

    member = call(runtime.use("GET", "charlie"), "/api/crm/acme")
    assert member["status"] == 200
    assert member["data"]["contacts"][0]["name"] == "Ada Lovelace"
    updated = call(runtime.use("PATCH", "charlie", {
        "stage": "customer", "lastContactedAt": 2_000_000_000_100,
    }), f"/api/crm/acme/contacts/{contact_id}")
    assert updated["data"]["contact"]["stage"] == "customer"

    outsider = call(runtime.use("GET", "bob"), "/api/crm/acme")
    assert outsider["status"] == 404


def test_the_idempotent_schema_replay_runs_once_per_isolate():
    # Replaying thirty CREATE statements ahead of every read is most of what a
    # package API request costs, and it is what made a single contact fetch
    # answer 1102 (Worker resource limits exceeded) in production. A fresh
    # isolate still replays once, so a release that adds a table upgrades an
    # organization that installed an earlier one.
    crm._SCHEMA_REPLAYED.clear()
    runtime = Runtime()
    statements = []
    plain = runtime.d1_run

    async def counted(sql, *args):
        statements.append(sql)
        return await plain(sql, *args)

    runtime.d1_run = counted
    first = call(runtime.use("GET"), "/api/crm/acme")
    assert first["status"] == 200
    replayed = [sql for sql in statements if sql in crm.DATA_SCHEMA_UP]
    assert replayed == list(crm.DATA_SCHEMA_UP)

    statements.clear()
    again = call(runtime.use("GET"), "/api/crm/acme")
    assert again["status"] == 200
    assert [sql for sql in statements if sql in crm.DATA_SCHEMA_UP] == []

    # A new isolate has replayed nothing yet.
    crm._SCHEMA_REPLAYED.clear()
    statements.clear()
    assert call(runtime.use("GET"), "/api/crm/acme")["status"] == 200
    assert [sql for sql in statements
            if sql in crm.DATA_SCHEMA_UP] == list(crm.DATA_SCHEMA_UP)
    crm._SCHEMA_REPLAYED.clear()


def test_crm_requires_the_organization_to_install_the_plugin_first():
    runtime = Runtime(installed=False)
    denied = call(runtime.use("GET"), "/api/crm/acme")
    assert denied["status"] == 409
    assert denied["data"]["error"] == "plugin_not_installed"
    assert denied["data"]["canInstall"] is True

    member = call(runtime.use("GET", "charlie"), "/api/crm/acme")
    assert (member["status"], member["data"]["canInstall"]) == (409, False)
    blocked = call(runtime.use("POST", body={"name": "Ada"}),
                   "/api/crm/acme/contacts")
    assert blocked["status"] == 409

    root = call(runtime.use("GET"), "/api/crm")
    assert root["data"]["organizations"] == [
        {"name": "acme", "role": "owner", "installed": False}]


def test_csv_import_upserts_by_org_scoped_email_and_export_is_safe():
    runtime = Runtime()
    imported = call(runtime.use("POST", body={"csv": (
        "name,email,company,twitter,mastodon,bsky,linkedin url,github,"
        "homepage,city,stage,tags,notes\r\n"
        "Ada,ada@example.com,Engines,@ada,@ada@mathstodon.xyz,ada.bsky.social,"
        "linkedin.com/in/ada,@ada,https://ada.example,London,lead,founder;vip,First\r\n"
        "Grace,grace@example.com,Navy,@grace,@grace@hachyderm.io,,,,,,customer,"
        "=FORMULA;compiler, =HYPERLINK(1)\r\n"
    )}), "/api/crm/acme/contacts/import")
    assert imported["data"]["ok"] is True
    assert (imported["data"]["created"], imported["data"]["updated"]) == (2, 0)
    imported_contacts = call(runtime.use("GET"), "/api/crm/acme")["data"]["contacts"]
    ada = next(item for item in imported_contacts if item["email"] == "ada@example.com")
    assert ada["mastodon"] == "@ada@mathstodon.xyz"
    # Column headings vary by export: bsky, "linkedin url", homepage, and city
    # all land on the profile fields the CRM shows.
    assert ada["bluesky"] == "ada.bsky.social"
    assert ada["linkedin"] == "linkedin.com/in/ada"
    assert ada["github"] == "@ada"
    assert ada["website"] == "https://ada.example"
    assert ada["location"] == "London"

    merged = call(runtime.use("POST", body={"csv": (
        "full name,email address,organization,status\n"
        "Ada Byron,ADA@example.com,Analytical Engines,qualified\n"
    )}), "/api/crm/acme/contacts/import")
    assert (merged["data"]["created"], merged["data"]["updated"]) == (0, 1)
    assert runtime.db.execute("SELECT COUNT(*) FROM crm_contacts").fetchone()[0] == 2

    exported = call(runtime.use("GET"), "/api/crm/acme/contacts/export")
    assert exported["data"]["filename"] == "forkmesh-crm-acme.csv"
    # The merge above carried no profile columns, so the researched handles are
    # still on the record rather than blanked by the narrower CSV.
    assert "Ada Byron" in exported["data"]["csv"]
    assert "' =HYPERLINK(1)" in exported["data"]["csv"]
    assert "'=formula;compiler" in exported["data"]["csv"]
    assert "'@grace@hachyderm.io" in exported["data"]["csv"]
    assert "bluesky,linkedin,github,website,location" in exported["data"]["csv"]
    assert "ada.bsky.social" in exported["data"]["csv"]

    # Real Excel and Zoho exports often include a BOM, a locale-specific
    # semicolon delimiter, and split name/account columns.
    zoho = call(runtime.use("POST", "charlie", {"csv": (
        "\ufeffFirst Name;Last Name;E-mail Address;Account Name;Contact Owner\r\n"
        "Katherine;Johnson;katherine@example.com;NASA;Charlie\r\n"
    )}), "/api/crm/acme/contacts/import")
    assert (zoho["data"]["created"], zoho["data"]["skipped"]) == (1, 0)
    shared = call(runtime.use("GET"), "/api/crm/acme")
    imported_contact = next(
        item for item in shared["data"]["contacts"]
        if item["email"] == "katherine@example.com")
    assert imported_contact["name"] == "Katherine Johnson"
    assert imported_contact["company"] == "NASA"
    assert imported_contact["owner"] == "Charlie"


def test_mailtrap_key_is_ephemeral_and_send_history_keeps_provider_ids():
    runtime = Runtime()
    settings = call(runtime.use("PUT", body={
        "providerName": "Mailtrap",
        "providerUrlTemplate": "https://mailtrap.io/inboxes/sends/{id}",
        "fromName": "Acme Team",
        "fromEmail": "hello@acme.example",
    }), "/api/crm/acme/settings")
    assert settings["status"] == 200
    # A click on an address composes in Zoho Mail unless the organization
    # picks Gmail; anything else falls back to the default.
    assert settings["data"]["settings"]["mailProvider"] == "zoho"
    for choice, stored in (("gmail", "gmail"), ("outlook", "zoho"), ("", "zoho")):
        saved = call(runtime.use("PUT", body={"mailProvider": choice}),
                     "/api/crm/acme/settings")
        assert saved["status"] == 200
        assert saved["data"]["settings"]["mailProvider"] == stored
    denied = call(runtime.use("PUT", "charlie", {
        "fromEmail": "other@acme.example",
    }), "/api/crm/acme/settings")
    assert denied["status"] == 403

    sent = call(runtime.use("POST", body={
        "to": "customer@example.com", "subject": "Hello",
        "text": "A one-off note", "mailtrapKey": "secret-mailtrap-key",
    }), "/api/crm/acme/emails/send")
    assert sent["status"] == 201
    email = sent["data"]["email"]
    assert (email["status"], email["externalId"]) == ("accepted", "mailtrap-123")
    assert email["providerUrl"].endswith("/mailtrap-123")
    assert runtime.mailtrap_calls[0][0] == "secret-mailtrap-key"
    assert runtime.mailtrap_calls[0][1]["custom_variables"]["forkmesh_crm_send_id"]
    assert runtime.mailtrap_calls[0][1]["custom_variables"][
        "forkmesh_sender"] == "alice"
    stored = runtime.db.execute("SELECT data FROM crm_email_sends").fetchone()[0]
    assert "secret-mailtrap-key" not in stored

    linked = call(runtime.use("POST", body={
        "to": "customer@example.com", "subject": "Campaign follow-up",
        "externalId": "provider-77", "status": "delivered",
    }), "/api/crm/acme/emails")
    assert linked["status"] == 201
    assert linked["data"]["email"]["providerUrl"].endswith("/provider-77")

    summary = call(runtime.use("GET"), "/api/crm/acme")
    assert summary["data"]["summary"]["sent"] == 2
    assert all("text" not in item and "html" not in item
               for item in summary["data"]["emails"])


def test_resource_actions_cannot_be_replayed_against_other_resource_types():
    runtime = Runtime()
    assert call(runtime.use("POST"), "/api/crm/acme/settings/send")["status"] == 405
    assert call(runtime.use("GET"), "/api/crm/acme/contacts/send")["status"] == 405
    assert call(runtime.use("POST"), "/api/crm/acme/emails/import")["status"] == 405
    assert call(runtime.use("GET"), "/api/crm/acme/emails/export")["status"] == 405


def test_external_provider_links_are_https_and_do_not_fall_back_when_invalid():
    runtime = Runtime()
    call(runtime.use("PUT", body={
        "providerUrlTemplate": "https://mailtrap.io/inboxes/sends/{id}",
    }), "/api/crm/acme/settings")
    rejected = call(runtime.use("POST", body={
        "to": "customer@example.com", "externalId": "provider-88",
        "providerUrl": "http://mailtrap.io/inboxes/sends/provider-88",
    }), "/api/crm/acme/emails")
    assert rejected["status"] == 400
    assert rejected["data"]["error"] == "invalid_provider_url"
    assert runtime.db.execute("SELECT COUNT(*) FROM crm_email_sends").fetchone()[0] == 0
    assert crm._provider_url("https://mailtrap.io/sends/{id}", "bad\nid") == ""

    missing_id = call(runtime.use("POST", body={
        "to": "customer@example.com",
    }), "/api/crm/acme/emails")
    assert missing_id["status"] == 400
    assert missing_id["data"]["error"] == "external_id_required"


def test_email_contact_references_must_be_exact_existing_ids():
    runtime = Runtime()
    invalid = call(runtime.use("POST", body={
        "contactId": "a" * 33, "to": "customer@example.com",
        "fromEmail": "hello@acme.example", "subject": "Hello", "text": "Note",
    }), "/api/crm/acme/emails/send")
    assert invalid["status"] == 400
    assert invalid["data"]["error"] == "invalid_contact_id"

    missing = call(runtime.use("POST", body={
        "contactId": "a" * 32, "to": "customer@example.com",
        "fromEmail": "hello@acme.example", "subject": "Hello", "text": "Note",
    }), "/api/crm/acme/emails/send")
    assert missing["status"] == 404
    assert missing["data"]["error"] == "contact_not_found"
    assert runtime.mailtrap_calls == []


def test_failed_mailtrap_send_is_recorded_and_audited_as_failed():
    runtime = Runtime()

    async def reject_mailtrap(token, payload):
        runtime.mailtrap_calls.append((token, payload))
        return {"accepted": False, "messageId": "", "error": "rejected"}

    runtime.send_mailtrap = reject_mailtrap
    failed = call(runtime.use("POST", body={
        "to": "customer@example.com", "fromEmail": "hello@acme.example",
        "subject": "Hello", "text": "A one-off note", "mailtrapKey": "ephemeral",
    }), "/api/crm/acme/emails/send")
    assert failed["status"] == 502
    assert failed["data"]["email"]["status"] == "failed"
    assert runtime.audits[-1][1]["outcome"] == "failed"
    assert "ephemeral" not in runtime.db.execute(
        "SELECT data FROM crm_email_sends").fetchone()[0]


def test_starter_flows_are_customizable_and_contacts_keep_their_flow():
    runtime = Runtime()
    initial = call(runtime.use("GET"), "/api/crm/acme")
    assert [flow["id"] for flow in initial["data"]["settings"]["flows"]] == [
        "investment", "applicants", "partnerships", "customers", "grants"]
    assert initial["data"]["settings"]["flows"][-1] == {
        "id": "grants", "name": "Grants", "icon": "grants",
        "stages": ["researching", "eligible", "drafting", "submitted",
                   "awarded", "declined"]}

    saved = call(runtime.use("PUT", body={"flows": [
        {"id": "fundraising", "name": "Fundraising",
         "stages": ["Warm intro", "Pitch", "Diligence", "Closed"]},
        {"id": "customers", "name": "Customers",
         "stages": ["Lead", "Trial", "Won"]},
    ]}), "/api/crm/acme/settings")
    assert saved["data"]["settings"]["flows"][0] == {
        "id": "fundraising", "name": "Fundraising", "icon": "pipeline",
        "stages": ["warm-intro", "pitch", "diligence", "closed"]}
    assert saved["data"]["settings"]["flows"][1]["icon"] == "customers"

    # An icon picked sticks, and a save that leaves icons out keeps them.
    iconed = call(runtime.use("PUT", body={"flows": [
        {"id": "fundraising", "name": "Fundraising", "icon": "investors",
         "stages": ["pitch"]},
        {"id": "customers", "name": "Customers", "icon": "not-an-icon",
         "stages": ["lead"]},
    ]}), "/api/crm/acme/settings")
    assert [flow["icon"] for flow in iconed["data"]["settings"]["flows"]] == [
        "investors", "customers"]
    kept = call(runtime.use("PUT", body={"flows": [
        {"id": "fundraising", "name": "Fundraising", "stages": ["pitch"]},
    ]}), "/api/crm/acme/settings")
    assert kept["data"]["settings"]["flows"][0]["icon"] == "investors"
    call(runtime.use("PUT", body={"flows": [
        {"id": "fundraising", "name": "Fundraising",
         "stages": ["Warm intro", "Pitch", "Diligence", "Closed"]},
        {"id": "customers", "name": "Customers",
         "stages": ["Lead", "Trial", "Won"]},
    ]}), "/api/crm/acme/settings")

    created = call(runtime.use("POST", body={
        "name": "Ada", "flow": "fundraising", "stage": "Warm intro",
    }), "/api/crm/acme/contacts")
    assert created["data"]["contact"]["flow"] == "fundraising"
    assert created["data"]["contact"]["stage"] == "warm-intro"


def test_contacts_land_in_a_stage_their_flow_has():
    runtime = Runtime()
    call(runtime.use("PUT", body={"flows": [
        {"id": "investors", "name": "Investors", "stages": ["new", "complete"]},
        {"id": "customers", "name": "Customers",
         "stages": ["new", "in-progress", "complete"]},
    ]}), "/api/crm/acme/settings")

    created = call(runtime.use("POST", body={"name": "Ada"}),
                   "/api/crm/acme/contacts")["data"]["contact"]
    assert (created["flow"], created["stage"]) == ("customers", "new")
    wrong = call(runtime.use("POST", body={
        "name": "Bea", "flow": "investors", "stage": "lead"}),
        "/api/crm/acme/contacts")["data"]["contact"]
    assert (wrong["flow"], wrong["stage"]) == ("investors", "new")

    imported = call(runtime.use("POST", body={
        "csv": "name,email\nCy,cy@example.com\n", "fileName": "signups.csv"}),
        "/api/crm/acme/contacts/import")
    assert imported["data"]["created"] == 1
    contacts = {contact["name"]: contact for contact in call(
        runtime.use("GET"), "/api/crm/acme/contacts")["data"]["contacts"]}
    assert (contacts["Cy"]["flow"], contacts["Cy"]["stage"]) == ("customers", "new")

    moved = call(runtime.use("PATCH", body={"flow": "investors"}),
                 f"/api/crm/acme/contacts/{created['id']}")["data"]["contact"]
    assert (moved["flow"], moved["stage"]) == ("investors", "new")
    renamed = call(runtime.use("PATCH", body={"nextStep": "Call"}),
                   f"/api/crm/acme/contacts/{moved['id']}")["data"]["contact"]
    assert renamed["stage"] == "new"

    bulk = call(runtime.use("POST", body={"edits": [
        {"id": wrong["id"], "flow": "customers", "owner": "charlie"},
        {"id": contacts["Cy"]["id"], "owner": "alice"},
    ]}), "/api/crm/acme/contacts/bulk")["data"]["contacts"]
    by_id = {contact["id"]: contact for contact in bulk}
    assert (by_id[wrong["id"]]["flow"], by_id[wrong["id"]]["stage"]) == ("customers", "new")
    assert by_id[wrong["id"]]["owner"] == "charlie"
    assert by_id[contacts["Cy"]["id"]]["owner"] == "alice"


def test_zoho_connection_is_encrypted_redacted_and_imports_grouped_mail_boxes():
    runtime = Runtime()
    connected = call(runtime.use("PUT", body={
        "mailBoxes": ["Inbox", "Follow-up", "Important", "Archive", "Later"],
        "zoho": {
            "region": "eu", "accountId": "123456789", "clientId": "client-id",
            "clientSecret": "client-secret", "refreshToken": "refresh-secret",
        },
    }), "/api/crm/acme/settings")
    public = connected["data"]["settings"]["zoho"]
    assert public.pop("connectedAt") > 0
    assert public == {"connected": True, "region": "eu",
                      "accountId": "123456789", "lastSyncAt": 0,
                      "clientId": "client-id", "hasClientSecret": True,
                      "oneClick": False, "managed": False, "canSend": True,
                      "lastSentAt": 0, "lastSyncError": "",
                      "lastSyncErrorAt": 0,
                      "steps": [{"key": "sync", "action": "sync",
                                 "text": "Sync Zoho Mail once to bring in the mailbox."}]}
    raw = runtime.db.execute("SELECT data FROM crm_settings").fetchone()[0]
    assert raw.startswith("sealed:") and "refresh-secret" in raw
    assert "refreshToken" not in json.dumps(public)
    assert "client-secret" not in json.dumps(public)

    runtime.zoho_messages = [
        {"providerMessageId": "m1", "threadId": "thread-7",
         "subject": "Launch plan", "summary": "First", "from": "a@example.com",
         "to": "team@example.com", "sourceFolder": "Inbox",
         "flag": "important", "receivedAt": 2000,
         "providerUrl": "https://mail.zoho.eu/zm/#mail/folder/1/p/m1"},
        {"providerMessageId": "m2", "threadId": "thread-7",
         "subject": "Re: Launch plan", "summary": "Second",
         "from": "team@example.com", "to": "a@example.com",
         "sourceFolder": "Sent", "receivedAt": 3000,
         "providerUrl": "https://mail.zoho.eu/zm/#mail/folder/2/p/m2"},
    ]
    imported = call(runtime.use("POST", body={"limit": 50}),
                    "/api/crm/acme/mail/import")
    assert (imported["data"]["created"], imported["data"]["updated"]) == (2, 0)
    messages = imported["data"]["mail"]
    assert len({item["groupKey"] for item in messages}) == 1
    assert {item["box"] for item in messages} == {"Important", "Inbox"}
    token_call = runtime.outbound_calls[0]
    assert token_call[0] == "https://accounts.zoho.eu/oauth/v2/token"
    assert "client_secret=client-secret" in token_call[3]
    assert any("/messages/view?" in call[0] for call in runtime.outbound_calls)

    moved = call(runtime.use("PATCH", body={"box": "Later"}),
                 f"/api/crm/acme/mail/{messages[0]['id']}")
    assert moved["data"]["message"]["box"] == "Later"
    repeated = call(runtime.use("POST", body={}), "/api/crm/acme/mail/import")
    assert (repeated["data"]["created"], repeated["data"]["updated"]) == (0, 2)
    assert next(item for item in repeated["data"]["mail"]
                if item["id"] == messages[0]["id"])["box"] == "Later"

    teammate_sync = call(
        runtime.use("POST", "charlie", {}), "/api/crm/acme/mail/import")
    assert teammate_sync["status"] == 200
    assert (teammate_sync["data"]["created"],
            teammate_sync["data"]["updated"]) == (0, 2)
    audit_args, _audit_kwargs = runtime.audits[-1]
    assert audit_args[0] == "charlie"
    assert audit_args[1] == "crm.zoho.import"

    disconnected = call(runtime.use("PUT", body={"zohoDisconnect": True}),
                        "/api/crm/acme/settings")
    assert disconnected["data"]["settings"]["zoho"]["connected"] is False


def test_plugin_web_and_desktop_entrypoints_are_declared():
    manifest = json.loads((CRM_PACKAGE / "manifest.json").read_text())
    assert manifest["entrypoints"] == {
        "desktop": "builtin:crm",
        "dashboard-view": "content/web/view.html",
        "dashboard-script": "content/web/page.js",
        "worker": "content/worker/crm.py",
        "settings": "builtin:package",
    }
    page = next(
        item for item in manifest["contributions"]
        if item["socket"] == "dashboard.page"
    )
    assert page["entrypoint"] == "dashboard-view"
    assert page["config"]["route"] == "/dashboard/crm"
    plugin = json.loads(
        (CRM_PACKAGE / ".codex-plugin" / "plugin.json").read_text())
    assert plugin["name"] == "crm"
    assert plugin["interface"]["displayName"] == "CRM"

    script = (CRM_PACKAGE / "content" / "web" / "page.js").read_text()
    assert "data-crm-contact-compose" in script
    assert 'data-crm-contact-email="${' not in script
    assert 'addEventListener("close"' in script


def _columns(view):
    return [column["field"] for column in view["columns"] if not column["hidden"]]


class ZohoAppRuntime(Runtime):
    def __init__(self):
        super().__init__()
        self.origin = "https://app.example.com"

    def request_origin(self):
        return self.origin

    def package_secret(self, name):
        return {"ZOHO_OAUTH_CLIENT_ID": "platform-client",
                "ZOHO_OAUTH_CLIENT_SECRET": "platform-secret"}.get(name, "")

    async def outbound_json(self, url, method="GET", headers=None, body=""):
        if url.endswith("/oauth/v2/token") and "grant_type=authorization_code" in body:
            self.outbound_calls.append((url, method, dict(headers or {}), body))
            if "code=bad" in body:
                return {"ok": True, "status": 400, "data": {"error": "invalid_code"}}
            return {"ok": True, "status": 200, "data": {
                "access_token": "first-access", "refresh_token": "granted-refresh"}}
        if url.endswith("/api/accounts"):
            self.outbound_calls.append((url, method, dict(headers or {}), body))
            return {"ok": True, "status": 200, "data": {"data": [
                {"accountId": "98765", "primaryEmailAddress": "team@example.com"}]}}
        return await super().outbound_json(url, method, headers, body)


def _zoho_state(authorization_url):
    return parse_qs(urlparse(authorization_url).query)["state"][0]


def test_zoho_one_click_needs_the_platform_app():
    runtime = Runtime()
    settings = call(runtime.use("GET"), "/api/crm/acme/settings")["data"]["settings"]
    assert settings["zoho"]["oneClick"] is False
    assert call(runtime.use("POST"), "/api/crm/acme/zoho/connect")["status"] == 409
    assert call(runtime.use("GET"), "/api/crm/acme/zoho/connect")["status"] == 405


def test_zoho_one_click_connects_with_the_platform_app_and_syncs():
    runtime = ZohoAppRuntime()
    assert call(runtime.use("GET"), "/api/crm/acme/settings")[
        "data"]["settings"]["zoho"]["oneClick"] is True
    assert call(runtime.use("POST", "charlie"), "/api/crm/acme/zoho/connect")["status"] == 403
    assert call(runtime.use("POST", "bob"), "/api/crm/acme/zoho/connect")["status"] == 404

    started = call(runtime.use("POST"), "/api/crm/acme/zoho/connect")
    assert started["status"] == 200
    url = urlparse(started["data"]["authorizationUrl"])
    query = parse_qs(url.query)
    assert (url.scheme, url.netloc, url.path) == ("https", "accounts.zoho.com", "/oauth/v2/auth")
    assert query["client_id"] == ["platform-client"]
    assert query["redirect_uri"] == ["https://app.example.com/api/crm/-/zoho/callback"]
    assert query["access_type"] == ["offline"] and query["prompt"] == ["consent"]
    assert set(query["scope"][0].split(",")) == {
        "ZohoMail.messages.READ", "ZohoMail.messages.CREATE", "ZohoMail.folders.READ",
        "ZohoMail.accounts.READ"}
    state = _zoho_state(started["data"]["authorizationUrl"])
    assert "platform-secret" not in started["data"]["authorizationUrl"]

    denied = call(runtime.use("GET", query={"state": state, "error": "access_denied"}),
                  "/api/crm/-/zoho/callback")
    assert denied["status"] == 302
    assert denied["headers"]["location"] == "/dashboard/crm?org=acme&zoho=denied"
    rejected = call(runtime.use("GET", query={"state": state, "code": "bad"}),
                    "/api/crm/-/zoho/callback")
    assert rejected["headers"]["location"].endswith("&zoho=token_rejected")

    done = call(runtime.use("GET", "bob", query={
        "state": state, "code": "good", "location": "eu",
        "accounts-server": "https://accounts.zoho.eu"}), "/api/crm/-/zoho/callback")
    assert done["status"] == 302
    assert done["headers"]["location"] == "/dashboard/crm?org=acme&zoho=connected"
    exchange = next(item for item in runtime.outbound_calls
                    if "code=good" in item[3])
    assert exchange[0] == "https://accounts.zoho.eu/oauth/v2/token"
    assert "client_secret=platform-secret" in exchange[3]
    assert "redirect_uri=https%3A%2F%2Fapp.example.com%2Fapi%2Fcrm%2F-%2Fzoho%2Fcallback" in exchange[3]
    assert runtime.outbound_calls[-1][0] == "https://mail.zoho.eu/api/accounts"

    zoho = call(runtime.use("GET"), "/api/crm/acme/settings")["data"]["settings"]["zoho"]
    assert {key: zoho[key] for key in (
        "connected", "region", "accountId", "lastSyncAt", "oneClick", "managed",
        "canSend")} == {"connected": True, "region": "eu", "accountId": "98765",
                        "lastSyncAt": 0, "oneClick": True, "managed": True,
                        "canSend": True}
    assert [step["key"] for step in zoho["steps"]] == ["sync"]
    raw = runtime.db.execute("SELECT data FROM crm_settings").fetchone()[0]
    assert "granted-refresh" in raw and "platform-secret" not in raw

    runtime.outbound_calls = []
    runtime.zoho_messages = [{"providerMessageId": "m1", "subject": "Hello",
                              "from": "a@example.com", "to": "team@example.com",
                              "receivedAt": 2000}]
    synced = call(runtime.use("POST", body={"limit": 10}), "/api/crm/acme/mail/import")
    assert synced["data"]["created"] == 1
    refresh = runtime.outbound_calls[0]
    assert refresh[0] == "https://accounts.zoho.eu/oauth/v2/token"
    assert "client_secret=platform-secret" in refresh[3]
    assert "refresh_token=granted-refresh" in refresh[3]


class ZohoSelfClientRuntime(Runtime):
    async def outbound_json(self, url, method="GET", headers=None, body=""):
        if url.endswith("/oauth/v2/token") and "grant_type=authorization_code" in body:
            self.outbound_calls.append((url, method, dict(headers or {}), body))
            if not url.startswith("https://accounts.zoho.eu/"):
                return {"ok": True, "status": 400, "data": {"error": "invalid_client"}}
            if "code=fresh" not in body or "client_secret=self-secret" not in body:
                return {"ok": True, "status": 400, "data": {"error": "invalid_code"}}
            return {"ok": True, "status": 200, "data": {
                "access_token": "self-access", "refresh_token": "self-refresh"}}
        if url == "https://mail.zoho.eu/api/accounts":
            self.outbound_calls.append((url, method, dict(headers or {}), body))
            return {"ok": True, "status": 200, "data": {"data": [{"accountId": "4242"}]}}
        return await super().outbound_json(url, method, headers, body)


def test_zoho_self_client_code_is_exchanged_on_the_server():
    runtime = ZohoSelfClientRuntime()
    path = "/api/crm/acme/zoho/exchange"
    body = {"clientId": "self-id", "clientSecret": "self-secret", "code": "fresh"}
    assert call(runtime.use("GET"), path)["status"] == 405
    assert call(runtime.use("POST", "charlie", body=body), path)["status"] == 403
    assert call(runtime.use("POST", body={"code": "fresh"}), path)["data"]["error"] == "zoho_code_incomplete"

    rejected = call(runtime.use("POST", body={**body, "code": "stale"}), path)
    assert rejected["status"] == 400
    assert rejected["data"] == {"error": "zoho_code_rejected", "zohoError": "invalid_client"}
    assert len(runtime.outbound_calls) == 6

    runtime.outbound_calls = []
    done = call(runtime.use("POST", body={**body, "region": "jp"}), path)
    assert done["status"] == 200
    hosts = [urlparse(item[0]).netloc for item in runtime.outbound_calls]
    assert hosts[0] == "accounts.zoho.jp" and hosts[-2:] == ["accounts.zoho.eu", "mail.zoho.eu"]
    assert all("redirect_uri" not in item[3] for item in runtime.outbound_calls)
    zoho = done["data"]["settings"]["zoho"]
    assert (zoho["connected"], zoho["region"], zoho["accountId"], zoho["managed"]) == (
        True, "eu", "4242", False)
    raw = runtime.db.execute("SELECT data FROM crm_settings").fetchone()[0]
    assert "self-refresh" in raw and "self-secret" in raw

    runtime.outbound_calls = []
    again = call(runtime.use("POST", body={"code": "fresh", "region": "eu"}), path)
    assert again["status"] == 200
    assert "client_id=self-id" in runtime.outbound_calls[0][3]
    assert "client_secret=self-secret" in runtime.outbound_calls[0][3]

    assert again["data"]["settings"]["zoho"]["clientId"] == "self-id"
    assert again["data"]["settings"]["zoho"]["hasClientSecret"] is True
    changed = call(runtime.use("POST", body={
        "clientId": "different-client", "code": "fresh"}), path)
    assert changed["data"]["error"] == "zoho_code_incomplete"


def test_zoho_credentials_survive_settings_saves_and_reloads():
    runtime = ZohoSelfClientRuntime()
    connected = call(runtime.use("POST", body={
        "clientId": "self-id", "clientSecret": "self-secret", "code": "fresh"}),
        "/api/crm/acme/zoho/exchange")
    public = connected["data"]["settings"]["zoho"]
    for update in ({"fromName": "Saved name"}, {"zoho": public},
                   {"zoho": {"clientId": "", "clientSecret": "", "refreshToken": ""}}):
        assert call(runtime.use("PUT", body=update), "/api/crm/acme/settings")["status"] == 200
        saved = call(runtime.use("GET"), "/api/crm/acme/settings")["data"]["settings"]["zoho"]
        assert saved["connected"] and saved["hasClientSecret"]
        assert saved["clientId"] == "self-id"
        assert "self-secret" not in json.dumps(saved)
        raw = runtime.db.execute("SELECT data FROM crm_settings").fetchone()[0]
        stored = json.loads(raw.removeprefix("sealed:"))["zoho"]
        assert stored["clientSecret"] == "self-secret"
        assert stored["refreshToken"] == "self-refresh"


def test_zoho_callback_refuses_stale_forged_and_demoted_states():
    runtime = ZohoAppRuntime()
    state = _zoho_state(call(runtime.use("POST"), "/api/crm/acme/zoho/connect")[
        "data"]["authorizationUrl"])
    forged = call(runtime.use("GET", query={"state": state[:-4] + "AAAA", "code": "good"}),
                  "/api/crm/-/zoho/callback")
    assert forged["headers"]["location"] == "/dashboard/crm?zoho=expired"
    other = call(runtime.use("GET", query={
        "state": state, "code": "good", "accounts-server": "https://accounts.evil.example"}),
        "/api/crm/-/zoho/callback")
    assert other["headers"]["location"].endswith("&zoho=unavailable")

    runtime.db.execute("UPDATE org_members SET role='member' WHERE name='alice'")
    demoted = call(runtime.use("GET", query={"state": state, "code": "good"}),
                   "/api/crm/-/zoho/callback")
    assert demoted["headers"]["location"].endswith("&zoho=forbidden")
    runtime.db.execute("UPDATE org_members SET role='owner' WHERE name='alice'")

    runtime.clock += crm.ZOHO_STATE_TTL_MS + 1
    stale = call(runtime.use("GET", query={"state": state, "code": "good"}),
                 "/api/crm/-/zoho/callback")
    assert stale["headers"]["location"] == "/dashboard/crm?zoho=expired"
    assert call(runtime.use("GET"), "/api/crm/acme/settings")[
        "data"]["settings"]["zoho"]["connected"] is False


def test_a_csv_import_opens_a_sheet_named_after_the_file_carrying_every_column():
    runtime = Runtime()
    imported = call(runtime.use("POST", body={
        "fileName": "Q3 pipeline_export.csv",
        "csv": (
            "Email,Full Name,Account Name,Deal Size,Renewal Quarter\r\n"
            "ada@example.com,Ada Lovelace,Engines,42000,FY26Q1\r\n"
            "grace@example.com,Grace Hopper,Navy,,FY26Q2\r\n"
        ),
    }), "/api/crm/acme/contacts/import")
    assert imported["data"]["created"] == 2
    view = imported["data"]["view"]
    # The tab is named for the file, not for its extension or separators.
    assert view["name"] == "Q3 pipeline export"
    assert (view["source"], view["fileName"]) == ("csv", "Q3 pipeline_export.csv")
    # Columns lead in the file's own order; the ones the file did not carry
    # stay available but hidden.
    assert _columns(view) == ["email", "name", "company", "deal_size",
                              "renewal_quarter"]
    assert [column["label"] for column in view["columns"][:5]] == [
        "Email", "Name", "Company", "Deal Size", "Renewal Quarter"]
    assert {column["field"] for column in view["columns"]} >= {
        "phone", "twitter", "notes", "lastContactedAt"}
    custom = [column for column in view["columns"] if column["custom"]]
    assert [column["field"] for column in custom] == ["deal_size",
                                                      "renewal_quarter"]

    # Headers with no built-in home are kept on the contact rather than dropped,
    # and the imported rows are joined to the sheet the import opened.
    contacts = call(runtime.use("GET"), "/api/crm/acme")["data"]["contacts"]
    ada = next(item for item in contacts if item["email"] == "ada@example.com")
    assert ada["fields"] == {"deal_size": "42000", "renewal_quarter": "FY26Q1"}
    assert ada["views"] == [view["id"]]
    grace = next(item for item in contacts if item["email"] == "grace@example.com")
    # An empty cell stores nothing instead of an empty custom field.
    assert grace["fields"] == {"renewal_quarter": "FY26Q2"}

    # An export round-trips the imported columns instead of losing them.
    exported = call(runtime.use("GET"), "/api/crm/acme/contacts/export")
    assert "deal_size,renewal_quarter" in exported["data"]["csv"]
    assert "42000" in exported["data"]["csv"]


def test_a_csv_import_opens_its_tab_under_the_name_typed_on_the_import_step():
    runtime = Runtime()
    imported = call(runtime.use("POST", body={
        "fileName": "journalists.csv", "sheetName": "  Press list  ",
        "csv": "name,email\r\nAda Lovelace,ada@example.com\r\n",
    }), "/api/crm/acme/contacts/import")
    view = imported["data"]["view"]
    assert view["name"] == "Press list"
    assert view["fileName"] == "journalists.csv"

    # A later part - or a re-import into that tab - keeps the tab's name.
    again = call(runtime.use("POST", body={
        "fileName": "journalists.csv", "sheetName": "Something else",
        "viewId": view["id"],
        "csv": "name,email\r\nGrace Hopper,grace@example.com\r\n",
    }), "/api/crm/acme/contacts/import")
    assert again["data"]["view"]["name"] == "Press list"

    # A blank name falls back to the file's.
    blank = call(runtime.use("POST", body={
        "fileName": "q3_leads.csv", "sheetName": "   ",
        "csv": "name,email\r\nKatherine Johnson,kj@example.com\r\n",
    }), "/api/crm/acme/contacts/import")
    assert blank["data"]["view"]["name"] == "q3 leads"


def test_re_importing_into_a_sheet_reuses_the_tab_and_adds_only_new_columns():
    runtime = Runtime()
    first = call(runtime.use("POST", body={
        "fileName": "leads.csv",
        "csv": "email,name,deal_size\nada@example.com,Ada,10\n",
    }), "/api/crm/acme/contacts/import")
    view_id = first["data"]["viewId"]

    again = call(runtime.use("POST", body={
        "fileName": "leads-refresh.csv", "viewId": view_id,
        "csv": "email,name,deal_size,owner_region\nada@example.com,Ada,20,EMEA\n",
    }), "/api/crm/acme/contacts/import")
    assert (again["data"]["viewId"], again["data"]["updated"]) == (view_id, 1)
    # The sheet keeps its name and column order; the new header is appended.
    assert again["data"]["view"]["name"] == "leads"
    assert _columns(again["data"]["view"]) == ["email", "name", "deal_size",
                                               "owner_region"]
    assert len(call(runtime.use("GET"), "/api/crm/acme")["data"]["views"]) == 1
    contacts = call(runtime.use("GET"), "/api/crm/acme")["data"]["contacts"]
    assert contacts[0]["fields"] == {"deal_size": "20", "owner_region": "EMEA"}
    # One membership, not one per import.
    assert contacts[0]["views"] == [view_id]


def test_a_split_csv_import_pings_start_and_finish_and_explains_skipped_rows():
    runtime = Runtime()
    envelope = {"id": "import-0001", "parts": 2, "rows": 3}
    first = call(runtime.use("POST", body={
        "fileName": "leads.csv",
        "csv": "name;email\r\nAda;ada@example.com\r\nBob;not-an-email\r\n",
        "import": dict(envelope, part=0),
    }), "/api/crm/acme/contacts/import")["data"]
    assert (first["created"], first["skipped"], first["delimiter"]) == (1, 1, ";")
    assert first["errors"] == [{
        "row": 3, "error": "invalid_email",
        "message": "Row 3 has an email address that is not valid."}]
    assert first["import"] == {"id": "import-0001", "part": 0, "parts": 2,
                               "done": False, "created": 1, "updated": 0,
                               "skipped": 1}
    assert [ping["title"] for ping in runtime.pings] == [
        "CRM import started: leads.csv"]
    started = runtime.pings[0]
    assert (started["recipient"], started["kind"], started["href"]) == (
        "alice", "plugin_activity", "/dashboard/crm")
    assert started["body"] == "3 rows importing into leads in acme."

    # The second part names the sheet and delimiter the first one reported, and
    # carries the totals so far; row numbers continue from its offset.
    second = call(runtime.use("POST", body={
        "fileName": "leads.csv", "viewId": first["viewId"],
        "csv": "name;email\r\n;\r\n",
        "import": dict(envelope, part=1, offset=2, delimiter=";", created=1,
                       skipped=1, errors=[{"row": 3, "error": "invalid_email"}]),
    }), "/api/crm/acme/contacts/import")["data"]
    assert second["viewId"] == first["viewId"]
    assert second["errors"][0]["row"] == 4
    assert second["import"]["done"] is True
    assert (second["import"]["created"], second["import"]["skipped"]) == (1, 2)
    finished = runtime.pings[1]
    assert finished["title"] == "CRM import finished with 2 skipped rows: leads.csv"
    assert finished["body"] == (
        "leads: 1 created, 0 updated, 2 skipped. "
        "Row 3 has an email address that is not valid. "
        "Row 4 needs a name, an email or a company.")
    assert started["dedupe"] != finished["dedupe"]
    assert len(call(runtime.use("GET"), "/api/crm/acme")["data"]["views"]) == 1

    # A whole-file request is its own first and last part.
    call(runtime.use("POST", body={
        "fileName": "one.csv", "csv": "name,email\nGrace,grace@example.com\n",
    }), "/api/crm/acme/contacts/import")
    assert [ping["title"] for ping in runtime.pings[2:]] == [
        "CRM import started: one.csv", "CRM import finished: one.csv"]


def test_an_abandoned_split_import_pings_failure_and_drops_its_empty_sheet():
    runtime = Runtime()
    envelope = {"id": "import-0002", "parts": 3, "rows": 250}
    first = call(runtime.use("POST", body={
        "fileName": "bad.csv", "csv": "name,email\n,\n",
        "import": dict(envelope, part=0),
    }), "/api/crm/acme/contacts/import")["data"]
    # Not the last part, so the sheet stays for the parts still to come.
    assert first["viewId"]
    abandoned = call(runtime.use("POST", body={
        "fileName": "bad.csv", "viewId": first["viewId"],
        "import": dict(envelope, part=1, offset=100, skipped=1,
                       failed="HTTP 502"),
    }), "/api/crm/acme/contacts/import")["data"]
    assert abandoned == {"ok": True, "failed": True}
    assert call(runtime.use("GET"), "/api/crm/acme")["data"]["views"] == []
    assert runtime.pings[-1]["title"] == "CRM import failed: bad.csv"
    assert runtime.pings[-1]["body"] == (
        "HTTP 502. Before it stopped: 0 created, 0 updated, 1 skipped.")

    # A part that would push the file past the row cap fails on the worker,
    # which closes the import with its own failure ping.
    over = call(runtime.use("POST", body={
        "fileName": "huge.csv", "csv": "name,email\nA,a@example.com\nB,b@example.com\n",
        "import": {"id": "import-0003", "part": 20, "parts": 21, "offset": 1999},
    }), "/api/crm/acme/contacts/import")
    assert (over["status"], over["data"]["error"]) == (413, "too_many_csv_rows")
    assert runtime.pings[-1]["title"] == "CRM import failed: huge.csv"
    assert runtime.pings[-1]["body"].startswith("The file has more than 2000 rows.")


def test_an_import_past_the_contact_cap_skips_the_rows_that_do_not_fit():
    runtime = Runtime()
    call(runtime.use("GET"), "/api/crm/acme")
    triggers = {row[0] for row in runtime.db.execute(
        "SELECT name FROM sqlite_master WHERE type='trigger'")}
    assert "trg_crm_contact_limit_5000" in triggers
    assert "trg_crm_contact_limit" not in triggers
    runtime.db.executemany(
        "INSERT INTO crm_contacts(contact_id,org_bi,org_name,email_bi,"
        "created_by_bi,created_by_name,updated_by_bi,updated_by_name,"
        "created_at,updated_at,data) VALUES(?,?,?,?,?,?,?,?,?,?,?)",
        [("seed%028d" % index, "org:acme", "acme", "", "", "alice", "", "alice",
          1, 1, "{}") for index in range(crm.MAX_CONTACTS - 1)])
    result = call(runtime.use("POST", body={
        "fileName": "late.csv",
        "csv": "name,email\nAda,ada@example.com\nBob,bob@example.com\n",
    }), "/api/crm/acme/contacts/import")
    assert result["status"] == 200
    data = result["data"]
    assert (data["created"], data["skipped"]) == (1, 1)
    assert data["errors"] == [{
        "row": 3, "error": "crm_full",
        "message": "Row 3 was not added because the CRM already holds 5000 contacts."}]


def test_sheets_are_renamed_reordered_and_shared_within_the_organization():
    runtime = Runtime()
    runtime.db.execute(
        "INSERT INTO org_members VALUES('org:acme','bi:bob','member','bob',1)")
    created = call(runtime.use("POST", "charlie", {
        "name": "Charlie's pipeline", "visibility": "private",
        "columns": [{"field": "name"}, {"field": "email"}, {"field": "stage"}],
    }), "/api/crm/acme/views")
    assert created["status"] == 201
    view = created["data"]["view"]
    assert (view["name"], view["owner"]) == ("Charlie's pipeline", "charlie")
    assert _columns(view) == ["name", "email", "stage"]

    # A private sheet is invisible to a member it was not shared with, and the
    # organization's owner still sees it.
    assert call(runtime.use("GET", "bob"), "/api/crm/acme")["data"]["views"] == []
    assert call(runtime.use("GET", "bob"),
                f"/api/crm/acme/views/{view['id']}")["status"] == 404
    assert len(call(runtime.use("GET"), "/api/crm/acme")["data"]["views"]) == 1
    # The dashboard offers the roster the sharing list is picked from.
    assert call(runtime.use("GET"), "/api/crm/acme")["data"]["members"] == [
        "alice", "bob", "charlie"]

    shared = call(runtime.use("PATCH", "charlie", {"sharedWith": ["bob", "bob"]}),
                  f"/api/crm/acme/views/{view['id']}")
    assert shared["data"]["view"]["sharedWith"] == ["bob"]
    assert [item["name"] for item in
            call(runtime.use("GET", "bob"), "/api/crm/acme")["data"]["views"]] == [
        "Charlie's pipeline"]

    # Everyone the sheet reaches may rename it and move or drop its columns...
    moved = call(runtime.use("PATCH", "bob", {
        "name": "Shared pipeline",
        "columns": [{"field": "stage"}, {"field": "name"},
                    {"field": "email", "hidden": True}],
    }), f"/api/crm/acme/views/{view['id']}")
    assert moved["data"]["view"]["name"] == "Shared pipeline"
    assert _columns(moved["data"]["view"]) == ["stage", "name"]
    # ...but who else can open it stays with its owner and the org's managers.
    assert call(runtime.use("PATCH", "bob", {"visibility": "organization"}),
                f"/api/crm/acme/views/{view['id']}")[
        "data"]["view"]["visibility"] == "private"
    assert call(runtime.use("DELETE", "bob", {}),
                f"/api/crm/acme/views/{view['id']}")["status"] == 403

    deleted = call(runtime.use("DELETE", "charlie", {}),
                   f"/api/crm/acme/views/{view['id']}")
    assert deleted["data"]["deleted"] is True
    assert call(runtime.use("GET"), "/api/crm/acme")["data"]["views"] == []


def test_a_pipeline_tab_is_a_sheet_bound_to_one_flow():
    runtime = Runtime()
    created = call(runtime.use("POST", "alice", {
        "name": "Partners", "flow": "Partnerships"}), "/api/crm/acme/views")
    assert created["status"] == 201
    view = created["data"]["view"]
    assert view["flow"] == "partnerships"
    # Renaming keeps the binding; a plain sheet has none.
    renamed = call(runtime.use("PATCH", "alice", {"name": "Partner deals"}),
                   f"/api/crm/acme/views/{view['id']}")
    assert renamed["data"]["view"]["flow"] == "partnerships"
    plain = call(runtime.use("POST", "alice", {"name": "Plain"}), "/api/crm/acme/views")
    assert plain["data"]["view"]["flow"] == ""
    assert crm._view_data({"flow": "!!!"})["flow"] == ""


def test_a_grants_csv_adds_the_grants_pipeline_and_binds_its_new_list():
    runtime = Runtime()
    call(runtime.use("PUT", body={"flows": [
        {"id": "customers", "name": "Customers", "stages": ["lead", "won"]},
    ]}), "/api/crm/acme/settings")
    grants_csv = (
        "Name,Website,Pipeline,Stage,Amount,Rank\r\n"
        "Commons Fund,https://example.org/commons,grants,eligible,50000,1\r\n"
        "Tech Fund,https://example.org/tech,grants,researching,150000,2\r\n"
    )

    # A member's import cannot change the organization's pipelines.
    member = call(runtime.use("POST", "charlie", {
        "fileName": "member grants.csv", "csv": grants_csv,
    }), "/api/crm/acme/contacts/import")
    assert member["data"]["view"]["flow"] == ""
    flows = call(runtime.use("GET"), "/api/crm/acme")["data"]["settings"]["flows"]
    assert [flow["id"] for flow in flows] == ["customers"]

    imported = call(runtime.use("POST", "alice", {
        "fileName": "ForkMesh grants.csv", "csv": grants_csv,
    }), "/api/crm/acme/contacts/import")
    assert imported["data"]["view"]["flow"] == "grants"
    flows = call(runtime.use("GET"), "/api/crm/acme")["data"]["settings"]["flows"]
    assert [(flow["id"], flow["icon"]) for flow in flows] == [
        ("customers", "customers"), ("grants", "grants")]
    contacts = call(runtime.use("GET"), "/api/crm/acme")["data"]["contacts"]
    commons = next(item for item in contacts if item["name"] == "Commons Fund")
    assert (commons["flow"], commons["stage"], commons["value"]) == (
        "grants", "eligible", 50000)
    assert runtime.audits[-1][0][1] == "crm.contacts.import"
    assert any(audit[0][1] == "crm.flows.add" for audit in runtime.audits)

    # A file mixing pipelines opens a plain list.
    mixed = call(runtime.use("POST", "alice", {
        "fileName": "mixed.csv",
        "csv": "Name,Pipeline\r\nOne,grants\r\nTwo,customers\r\n",
    }), "/api/crm/acme/contacts/import")
    assert mixed["data"]["view"]["flow"] == ""


def test_a_csv_whose_rows_are_all_rejected_leaves_no_empty_tab_behind():
    runtime = Runtime()
    rejected = call(runtime.use("POST", body={
        "fileName": "broken.csv", "csv": "title,notes\nEngineer,no name or email\n",
    }), "/api/crm/acme/contacts/import")
    assert (rejected["data"]["created"], rejected["data"]["skipped"]) == (0, 1)
    assert rejected["data"]["viewId"] == ""
    assert rejected["data"]["view"] is None
    assert call(runtime.use("GET"), "/api/crm/acme")["data"]["views"] == []


def test_an_import_preview_guesses_each_column_and_writes_nothing():
    runtime = Runtime()
    preview = call(runtime.use("POST", body={"preview": True, "csv": (
        "Contact,Primary,Company Name,Job,Deal owner,Profile,Status,Region code\r\n"
        "Ada Lovelace,ada@example.com,Engines,Founder,Charlie,"
        "https://linkedin.com/in/ada,lead,UK\r\n"
        "Grace Hopper,grace@example.com,Navy,Admiral,Charlie,"
        "https://www.linkedin.com/in/grace,customer,US\r\n"
    )}), "/api/crm/acme/contacts/import")
    assert preview["status"] == 200
    columns = preview["data"]["columns"]
    # Header words, then the cells themselves ("Primary" holds addresses),
    # stand in for headers the CRM has no exact spelling for.
    assert [column["field"] for column in columns] == [
        "name", "email", "company", "title", "owner", "linkedin", "stage",
        "location"]
    assert columns[0] == {"index": 0, "header": "Contact", "field": "name",
                          "samples": ["Ada Lovelace", "Grace Hopper"]}
    assert preview["data"]["delimiter"] == ","
    # A preview opens no sheet, stores no contact, and pings nobody.
    assert runtime.db.execute("SELECT COUNT(*) FROM crm_contacts").fetchone()[0] == 0
    assert call(runtime.use("GET"), "/api/crm/acme")["data"]["views"] == []
    assert runtime.pings == []
    unreadable = call(runtime.use("POST", body={"preview": True, "csv": " "}),
                      "/api/crm/acme/contacts/import")
    assert (unreadable["status"], unreadable["data"]["error"]) == (400, "csv_required")
    assert runtime.pings == []


def test_a_list_of_people_under_an_unfamiliar_header_imports_into_a_new_tab():
    # A press list: nothing is spelled "name" or "email", but the Journalist
    # cells read like people, so the guess makes them the contact and the
    # import opens its own tab instead of skipping every row.
    runtime = Runtime()
    csv_text = (
        "Rank,Journalist,Publication / beat,Mastodon\r\n"
        "1,Mike Masnick,Techdirt \u2014 internet/open platforms,@mmasnick@mastodon.social\r\n"
        "2,Cory Doctorow,\"Journalist/author \u2014 digital rights\",@doctorow@mamot.fr\r\n"
        "3,Jeremy B. Merrill,Washington Post \u2014 technology,@jeremybmerrill@mastodon.social\r\n"
        "4,Ludwig van Beethoven,Wiener Zeitung,\r\n"
    )
    preview = call(runtime.use("POST", body={"preview": True, "csv": csv_text}),
                   "/api/crm/acme/contacts/import")["data"]
    mapping = [column["field"] for column in preview["columns"]]
    assert mapping == ["custom", "name", "company", "mastodon"]
    imported = call(runtime.use("POST", body={
        "fileName": "journalists.csv", "csv": csv_text, "mapping": mapping,
    }), "/api/crm/acme/contacts/import")["data"]
    assert (imported["created"], imported["skipped"]) == (4, 0)
    views = call(runtime.use("GET"), "/api/crm/acme")["data"]["views"]
    assert [view["name"] for view in views] == ["journalists"]
    assert imported["viewId"] == views[0]["id"]

    # Cells that only look like names never outrank what the header says.
    guessed = crm._csv_guess_mapping(
        ["Company name", "Speaker"],
        [{"Company name": "Analytical Engines", "Speaker": "Ada Lovelace"}])
    assert guessed == ["company", "name"]
    assert not crm._csv_looks_like_names(["Techdirt \u2014 internet", "WIRED"])
    assert not crm._csv_looks_like_names(["@ada@example.social"])

def test_a_follower_import_without_emails_matches_by_mastodon_handle():
    runtime = Runtime()
    # A contact already known by a handle in its website field.
    call(runtime.use("POST", body={
        "name": "Grace Hopper", "email": "grace@example.com",
        "website": "https://hachyderm.io/@grace",
    }), "/api/crm/acme/contacts")
    csv_text = (
        "Name,Mastodon,Notes\r\n"
        "Ada,@ada@mastodon.social,Engines\r\n"
        "Grace,@Grace@hachyderm.io,\r\n"
    )
    mapping = ["name", "mastodon", "notes"]
    first = call(runtime.use("POST", body={
        "fileName": "mastodon-followers-ada@mastodon.social.csv",
        "sheetName": "Followers of @ada@mastodon.social",
        "csv": csv_text, "mapping": mapping,
    }), "/api/crm/acme/contacts/import")["data"]
    assert (first["created"], first["updated"]) == (1, 1)
    assert first["view"]["name"] == "Followers of @ada@mastodon.social"
    again = call(runtime.use("POST", body={
        "fileName": "followers.csv", "viewId": first["viewId"],
        "csv": csv_text + "Ada twin,@ada@mastodon.social,\r\n",
        "mapping": mapping,
    }), "/api/crm/acme/contacts/import")["data"]
    # Every row matched a contact: the second pass adds nobody, and a repeat
    # of a handle inside one file lands on the contact its first row made.
    assert (again["created"], again["updated"]) == (0, 3)
    contacts = call(runtime.use("GET"), "/api/crm/acme")["data"]["contacts"]
    assert len(contacts) == 2
    grace = next(item for item in contacts if item["email"] == "grace@example.com")
    assert grace["name"] == "Grace" and first["viewId"] in grace["views"]
    ada = next(item for item in contacts if item["mastodon"] == "@ada@mastodon.social")
    assert ada["name"] == "Ada twin" and ada["notes"] == "Engines"


def test_a_github_contributor_list_keeps_its_counts_as_own_columns():
    runtime = Runtime()
    call(runtime.use("POST", body={
        "name": "Ada Lovelace", "email": "ada@example.com", "notes": "Met at PyCon",
    }), "/api/crm/acme/contacts")
    csv_text = (
        "Name,Email,Commits,Last commit\r\n"
        "ada,Ada@Example.com,3,2025-06-06\r\n"
        "Katherine Johnson,kj@nasa.gov,1,2025-06-06\r\n"
    )
    preview = call(runtime.use("POST", body={"preview": True, "csv": csv_text}),
                   "/api/crm/acme/contacts/import")["data"]
    mapping = [column["field"] for column in preview["columns"]]
    assert mapping == ["name", "email", "custom", "custom"]
    result = call(runtime.use("POST", body={
        "fileName": "github-contributors-ada-engine.csv",
        "sheetName": "Contributors of ada/engine",
        "csv": csv_text, "mapping": mapping,
    }), "/api/crm/acme/contacts/import")["data"]
    assert (result["created"], result["updated"]) == (1, 1)
    assert result["view"]["name"] == "Contributors of ada/engine"
    contacts = call(runtime.use("GET"), "/api/crm/acme")["data"]["contacts"]
    ada = next(item for item in contacts if item["email"] == "ada@example.com")
    assert ada["notes"] == "Met at PyCon" and result["viewId"] in ada["views"]

def test_an_import_follows_the_confirmed_mapping_and_needs_no_email():
    runtime = Runtime()
    imported = call(runtime.use("POST", body={
        "fileName": "founders.csv",
        "csv": (
            "Who,Email,Firm,Status,Internal id\r\n"
            "Ada Lovelace,,Engines,warm,17\r\n"
            ",,Analytical Society,cold,18\r\n"
        ),
        # Who is the name, the empty Email column is skipped, Status stays a
        # column of its own instead of becoming the stage, and the id is dropped.
        "mapping": ["name", "", "company", "custom", ""],
    }), "/api/crm/acme/contacts/import")["data"]
    assert (imported["created"], imported["skipped"]) == (2, 0)
    assert _columns(imported["view"]) == ["name", "company", "custom_status"]
    status = next(column for column in imported["view"]["columns"]
                  if column["field"] == "custom_status")
    assert (status["label"], status["custom"]) == ("Status", True)
    contacts = call(runtime.use("GET"), "/api/crm/acme")["data"]["contacts"]
    ada = next(item for item in contacts if item["name"] == "Ada Lovelace")
    assert (ada["email"], ada["company"], ada["stage"]) == ("", "Engines", "lead")
    assert ada["fields"] == {"custom_status": "warm"}
    # A row with neither a name nor an email is known by its company.
    society = next(item for item in contacts if item["company"] == "Analytical Society")
    assert society["name"] == "Analytical Society"

    # A split name maps onto the name column; a caller that sends no mapping
    # keeps exact header spellings only, so "Owner region" is not the owner.
    split = call(runtime.use("POST", body={
        "fileName": "people.csv",
        "csv": "Given,Family,Owner region\nKatherine,Johnson,EMEA\n",
        "mapping": ["first_name", "last_name", "custom"],
    }), "/api/crm/acme/contacts/import")["data"]
    assert _columns(split["view"]) == ["name", "owner_region"]
    unmapped = call(runtime.use("POST", body={
        "fileName": "plain.csv", "csv": "name,owner region\nDorothy,APAC\n",
    }), "/api/crm/acme/contacts/import")["data"]
    assert _columns(unmapped["view"]) == ["name", "owner_region"]
    contacts = call(runtime.use("GET"), "/api/crm/acme")["data"]["contacts"]
    assert any(item["name"] == "Katherine Johnson" for item in contacts)
    dorothy = next(item for item in contacts if item["name"] == "Dorothy")
    assert (dorothy["owner"], dorothy["fields"]) == ("", {"owner_region": "APAC"})


def test_a_mail_message_is_tied_to_a_contact_and_reopens_in_its_mailbox():
    runtime = Runtime()
    # The extension sends the Gmail message it has open; the other party is
    # unknown, so the contact is created and the message id kept on it.
    linked = call(runtime.use("POST", body={
        "provider": "gmail", "messageId": "FMfcgzQbdrKvLxmgbXcSMSbzfXcxMvRj",
        "threadId": "18f3b2c4d5e6f7a8", "accountIndex": 1,
        "direction": "inbound", "contactEmail": "Ada@Example.com",
        "contactName": "Ada Lovelace", "mailbox": "me@acme.example",
        "subject": "Re: Engines", "sentAt": 1_700_000_000_000,
    }), "/api/crm/acme/emails/link")
    assert linked["status"] == 201
    assert linked["data"]["contactCreated"] is True
    contact = linked["data"]["contact"]
    assert (contact["name"], contact["email"]) == ("Ada Lovelace", "ada@example.com")
    assert contact["source"] == "gmail email"
    assert contact["lastContactedAt"] == 1_700_000_000_000
    email = linked["data"]["email"]
    assert email["externalId"] == "FMfcgzQbdrKvLxmgbXcSMSbzfXcxMvRj"
    assert email["providerUrl"] == (
        "https://mail.google.com/mail/u/1/#all/FMfcgzQbdrKvLxmgbXcSMSbzfXcxMvRj")
    assert (email["provider"], email["direction"]) == ("gmail", "inbound")
    assert (email["fromEmail"], email["to"]) == ("ada@example.com", "me@acme.example")
    assert email["contactId"] == contact["id"]
    # The message id and address live in the sealed envelope only.
    raw = runtime.db.execute("SELECT data FROM crm_email_sends").fetchone()[0]
    assert raw.startswith("sealed:")

    # The extension asks who an address is before offering to link.
    found = call(runtime.use("POST", body={"email": "ada@example.com"}),
                 "/api/crm/acme/contacts/lookup")
    assert found["data"]["contact"]["id"] == contact["id"]
    assert [item["externalId"] for item in found["data"]["emails"]] == [
        "FMfcgzQbdrKvLxmgbXcSMSbzfXcxMvRj"]
    unknown = call(runtime.use("POST", body={"email": "nobody@example.com"}),
                   "/api/crm/acme/contacts/lookup")
    assert (unknown["data"]["contact"], unknown["data"]["emails"]) == (None, [])

    # Linking the same message again refreshes the one row instead of
    # duplicating it, and never moves "last contacted" backwards.
    again = call(runtime.use("POST", body={
        "provider": "gmail", "messageId": "FMfcgzQbdrKvLxmgbXcSMSbzfXcxMvRj",
        "contactEmail": "ada@example.com", "subject": "Re: Engines (edited)",
        "sentAt": 1_600_000_000_000,
    }), "/api/crm/acme/emails/link")
    assert (again["status"], again["data"]["relinked"]) == (200, True)
    assert again["data"]["email"]["id"] == email["id"]
    assert again["data"]["email"]["subject"] == "Re: Engines (edited)"
    assert again["data"]["contact"]["lastContactedAt"] == 1_700_000_000_000
    assert len(again["data"]["emails"]) == 1

    # Zoho Mail carries its own permalink; it must be https.
    zoho = call(runtime.use("POST", body={
        "provider": "zoho", "messageId": "1700000000000010001",
        "direction": "outbound", "contactId": contact["id"],
        "providerUrl": "https://mail.zoho.eu/zm/#mail/folder/inbox/p/1700000000000010001",
        "subject": "Proposal",
    }), "/api/crm/acme/emails/link")
    assert zoho["status"] == 201
    assert zoho["data"]["email"]["providerUrl"].startswith("https://mail.zoho.eu/")
    assert zoho["data"]["email"]["to"] == "ada@example.com"
    assert call(runtime.use("POST", body={
        "provider": "zoho", "messageId": "2", "contactId": contact["id"],
        "providerUrl": "http://mail.zoho.eu/zm/#mail/p/2",
    }), "/api/crm/acme/emails/link")["data"]["error"] == "invalid_provider_url"
    assert call(runtime.use("POST", body={
        "provider": "gmail", "contactEmail": "ada@example.com",
    }), "/api/crm/acme/emails/link")["data"]["error"] == "message_id_required"
    assert call(runtime.use("POST", body={
        "provider": "gmail", "messageId": "abc123def456",
    }), "/api/crm/acme/emails/link")["data"]["error"] == "contact_email_required"
    assert call(runtime.use("POST", body={
        "provider": "gmail", "messageId": "abc123def456",
        "contactEmail": "new@example.com", "createContact": False,
    }), "/api/crm/acme/emails/link")["status"] == 404

    # The dashboard sees provider and direction on every recorded email, and
    # an unlink is the ordinary delete.
    summary = call(runtime.use("GET"), "/api/crm/acme")["data"]
    assert {(item["provider"], item["direction"]) for item in summary["emails"]} == {
        ("gmail", "inbound"), ("zoho", "outbound")}
    assert summary["summary"]["sent"] == 2
    assert call(runtime.use("DELETE", body={}),
                f"/api/crm/acme/emails/{email['id']}")["data"]["deleted"] is True
    assert len(call(runtime.use("GET"), "/api/crm/acme")["data"]["emails"]) == 1


def test_a_mastodon_conversation_is_tracked_by_handle_beside_the_emails():
    runtime = Runtime()
    # The extension sends the open conversation. Mastodon knows people by
    # handle, so there is no address to match the contact on.
    linked = call(runtime.use("POST", body={
        "provider": "mastodon", "messageId": "113489001122334455",
        "threadId": "113489001122330000", "direction": "inbound",
        "handle": "@Grace@Mastodon.social", "account": "founder@acme.social",
        "contactName": "Grace Hopper", "subject": "CW: pipeline talk",
        "providerUrl": "https://mastodon.social/@grace/113489001122334455",
        "sentAt": 1_700_000_100_000, "flow": "partnerships", "stage": "contacted",
    }), "/api/crm/acme/emails/link")
    assert linked["status"] == 201 and linked["data"]["contactCreated"] is True
    contact = linked["data"]["contact"]
    assert (contact["name"], contact["mastodon"], contact["email"]) == (
        "Grace Hopper", "@grace@mastodon.social", "")
    assert contact["source"] == "mastodon conversation"
    assert (contact["flow"], contact["stage"]) == ("partnerships", "contacted")
    conversation = linked["data"]["email"]
    assert (conversation["provider"], conversation["handle"], conversation["account"]) == (
        "mastodon", "grace@mastodon.social", "founder@acme.social")
    assert conversation["providerUrl"].startswith("https://mastodon.social/@grace/")

    # The lookup finds that record by handle, however the handle is written.
    found = call(runtime.use("POST", body={"handle": "https://mastodon.social/@grace"}),
                 "/api/crm/acme/contacts/lookup")
    assert found["data"]["contact"]["id"] == contact["id"]
    assert [item["externalId"] for item in found["data"]["emails"]] == [
        "113489001122334455"]

    # A record typed or imported by hand is found too, however loosely its
    # Mastodon field names the account, the way the desktop column reads it.
    for stored in ("http://mastodon.social/@dctrjack/113489001122",
                   "mastodon.social/@dctrjack?lang=en",
                   "https://mastodon.social/users/dctrjack",
                   "Mastodon: @dctrjack @ mastodon.social."):
        saved = call(runtime.use("POST", body={"name": "Jack", "mastodon": stored}),
                     "/api/crm/acme/contacts")
        hit = call(runtime.use("POST", body={"handle": "dctrjack@mastodon.social"}),
                   "/api/crm/acme/contacts/lookup")
        assert hit["data"]["contact"]["id"] == saved["data"]["contact"]["id"], stored
        call(runtime.use("DELETE", body={}),
             "/api/crm/acme/contacts/" + saved["data"]["contact"]["id"])
    imported = call(runtime.use("POST", body={
        "name": "Jack", "fields": {"mastodon_url": "https://mastodon.social/@dctrjack"}}),
        "/api/crm/acme/contacts")
    hit = call(runtime.use("POST", body={"handle": "@dctrjack@mastodon.social"}),
               "/api/crm/acme/contacts/lookup")
    assert hit["data"]["contact"]["id"] == imported["data"]["contact"]["id"]
    # Someone else on the same instance is not a match.
    assert call(runtime.use("POST", body={"handle": "jack@mastodon.social"}),
                "/api/crm/acme/contacts/lookup")["data"]["contact"] is None

    # A later post from the same person lands on the one record, and an email
    # to the address put on it keeps both in a single history.
    again = call(runtime.use("POST", body={
        "provider": "mastodon", "messageId": "113489009988776655",
        "handle": "grace@mastodon.social", "direction": "outbound",
        "subject": "reply", "sentAt": 1_700_000_200_000,
    }), "/api/crm/acme/emails/link")
    assert again["data"]["contactCreated"] is False
    assert again["data"]["contact"]["id"] == contact["id"]
    assert again["data"]["contact"]["lastContactedAt"] == 1_700_000_200_000
    assert len(again["data"]["emails"]) == 2
    mail = call(runtime.use("POST", body={
        "provider": "gmail", "messageId": "FMfcgzQbdrKvLxmgbXcSMSbzfXcxMvRj",
        "contactId": contact["id"], "contactEmail": "grace@example.com",
    }), "/api/crm/acme/emails/link")
    assert len(mail["data"]["emails"]) == 3
    assert {item["provider"] for item in mail["data"]["emails"]} == {
        "mastodon", "gmail"}

    # A conversation with nobody on it, and something that is not a handle.
    assert call(runtime.use("POST", body={
        "provider": "mastodon", "messageId": "113489009988776600",
    }), "/api/crm/acme/emails/link")["data"]["error"] == "contact_handle_required"
    assert call(runtime.use("POST", body={
        "provider": "mastodon", "messageId": "113489009988776601",
        "handle": "grace",
    }), "/api/crm/acme/emails/link")["data"]["error"] == "invalid_handle"
    assert call(runtime.use("POST", body={}),
                "/api/crm/acme/contacts/lookup")["data"]["error"] == "invalid_email"


def test_linking_an_email_can_put_the_contact_in_a_pipeline_stage():
    runtime = Runtime()
    # The lookup hands the extension the organization's pipelines.
    found = call(runtime.use("POST", body={"email": "grace@example.com"}),
                 "/api/crm/acme/contacts/lookup")
    assert found["data"]["contact"] is None
    assert [flow["id"] for flow in found["data"]["flows"]] == [
        "investment", "applicants", "partnerships", "customers", "grants"]

    # A sent Zoho message creates the contact straight into a stage, with the
    # details the extension read off the email.
    added = call(runtime.use("POST", body={
        "provider": "zoho", "messageId": "1700000000000020001",
        "direction": "outbound", "contactEmail": "grace@example.com",
        "contactName": "Gracie", "mailbox": "founder@acme.example",
        "subject": "Forkmesh on your show",
        "flow": "partnerships", "stage": "contacted",
        "contact": {"company": "Show Co", "website": "https://show.example",
                    "notes": "not a link field"},
    }), "/api/crm/acme/emails/link")
    assert added["status"] == 201 and added["data"]["contactCreated"] is True
    contact = added["data"]["contact"]
    assert (contact["flow"], contact["stage"]) == ("partnerships", "contacted")
    assert (contact["name"], contact["company"], contact["website"]) == (
        "Gracie", "Show Co", "https://show.example")
    assert contact["notes"] == ""
    assert added["data"]["flows"][0]["id"] == "investment"

    # Moving the stage later keeps the email link one row; extracted details
    # only fill blanks, never overwrite what the CRM already holds.
    moved = call(runtime.use("POST", body={
        "provider": "zoho", "messageId": "1700000000000020001",
        "contactEmail": "grace@example.com", "contactName": "G. Hopper",
        "stage": "discovery",
        "contact": {"company": "Other Co", "title": "Host"},
    }), "/api/crm/acme/emails/link")
    assert moved["status"] == 200 and moved["data"]["relinked"] is True
    contact = moved["data"]["contact"]
    assert (contact["flow"], contact["stage"]) == ("partnerships", "discovery")
    assert (contact["name"], contact["company"], contact["title"]) == (
        "Gracie", "Show Co", "Host")
    assert len(moved["data"]["emails"]) == 1

    # A flow alone lands on that flow's first stage; plain links leave the
    # pipeline alone.
    switched = call(runtime.use("POST", body={
        "provider": "zoho", "messageId": "1700000000000020001",
        "contactEmail": "grace@example.com", "flow": "customers",
    }), "/api/crm/acme/emails/link")
    assert (switched["data"]["contact"]["flow"],
            switched["data"]["contact"]["stage"]) == ("customers", "lead")
    plain = call(runtime.use("POST", body={
        "provider": "zoho", "messageId": "1700000000000020001",
        "contactEmail": "grace@example.com",
    }), "/api/crm/acme/emails/link")
    assert plain["data"]["contact"]["stage"] == "lead"

    # Unknown flows and stages are refused before anything is written.
    assert call(runtime.use("POST", body={
        "provider": "zoho", "messageId": "99", "contactEmail": "new@example.com",
        "flow": "nope", "stage": "lead",
    }), "/api/crm/acme/emails/link")["data"]["error"] == "invalid_flow"
    assert call(runtime.use("POST", body={
        "provider": "zoho", "messageId": "99", "contactEmail": "new@example.com",
        "flow": "customers", "stage": "hired",
    }), "/api/crm/acme/emails/link")["data"]["error"] == "invalid_stage"
    assert call(runtime.use("POST", body={"email": "new@example.com"}),
                "/api/crm/acme/contacts/lookup")["data"]["contact"] is None


def test_a_link_finds_the_persons_box_by_name_and_other_fields():
    runtime = Runtime()

    def box(**fields):
        return call(runtime.use("POST", body=fields),
                    "/api/crm/acme/contacts")["data"]["contact"]

    def link(message_id, **fields):
        return call(runtime.use("POST", body={
            "provider": "gmail", "messageId": message_id, **fields,
        }), "/api/crm/acme/emails/link")["data"]

    def boxes():
        return call(runtime.use("GET"), "/api/crm/acme")["data"]["contacts"]

    # Robin is already in a pipeline, filed by name with no address.
    robin = box(name="Robin Moss", company="Acme", flow="partnerships",
                stage="discovery")
    found = call(runtime.use("POST", body={
        "email": "robin@acme.example", "name": "Robin Moss"}),
        "/api/crm/acme/contacts/lookup")["data"]
    assert (found["contact"]["id"], found["matchedBy"]) == (robin["id"], "name")

    # Linking her email lands on that box, in its pipeline, and the box takes
    # the address so the next message matches it directly.
    linked = link("FMfcgzQbdrKvLxmgbXcSMSbzfRobin01",
                  contactEmail="robin@acme.example", contactName="Robin Moss")
    assert (linked["contactCreated"], linked["matchedBy"]) == (False, "name")
    assert linked["contact"]["id"] == robin["id"]
    assert (linked["contact"]["flow"], linked["contact"]["stage"]) == (
        "partnerships", "discovery")
    assert linked["contact"]["email"] == "robin@acme.example"
    assert len(boxes()) == 1

    # Her personal address and a first-name-only display name still find the
    # box; the second address is kept beside the first.
    personal = link("FMfcgzQbdrKvLxmgbXcSMSbzfRobin02",
                    contactEmail="robin@gmail.com", contactName="Robin")
    assert (personal["contactCreated"], personal["contact"]["id"]) == (False, robin["id"])
    assert personal["contact"]["email"] == "robin@acme.example"
    assert personal["contact"]["fields"]["other_emails"] == "robin@gmail.com"
    again = call(runtime.use("POST", body={"email": "robin@gmail.com"}),
                 "/api/crm/acme/contacts/lookup")["data"]
    assert (again["contact"]["id"], again["matchedBy"]) == (robin["id"], "email")
    assert len(again["emails"]) == 2
    assert len(boxes()) == 1

    # A different name at the same company is a different person.
    robyn = link("FMfcgzQbdrKvLxmgbXcSMSbzfRobyn01",
                 contactEmail="robyn@acme.example", contactName="Robyn Moss")
    assert robyn["contactCreated"] is True

    # A profile link or a phone number names the person even when the display
    # name is written differently.
    sam = box(name="Sam Lee", linkedin="https://www.linkedin.com/in/samlee/")
    assert link("FMfcgzQbdrKvLxmgbXcSMSbzfSamLee01",
                contactEmail="samuel@lee.example", contactName="Samuel Lee",
                contact={"linkedin": "linkedin.com/in/SamLee"},
                )["contact"]["id"] == sam["id"]
    pat = box(name="Pat", phone="+1 (415) 555-0100")
    assert link("FMfcgzQbdrKvLxmgbXcSMSbzfPatDoe01",
                contactEmail="pat@doe.example", contactName="Pat Doe",
                contact={"phone": "415.555.0100"})["contact"]["id"] == pat["id"]

    # A name that fits two boxes equally matches neither, and a name alone
    # with nothing to back it up is not enough.
    box(name="Alex")
    box(name="Alex", company="Other")
    assert link("FMfcgzQbdrKvLxmgbXcSMSbzfAlex0001", contactEmail="alex@x.example",
                contactName="Alex")["contactCreated"] is True
    box(name="Jordan")
    assert link("FMfcgzQbdrKvLxmgbXcSMSbzfJordan01", contactEmail="jp@y.example",
                contactName="Jordan Park")["contactCreated"] is True


def _install_second_organization(runtime, name="globex", owner="bob"):
    runtime.db.execute(
        "INSERT INTO orgs VALUES(?,?,?,?)", ("org:" + name, name, "sealed:{}", 1))
    runtime.db.execute(
        "INSERT INTO org_members VALUES(?,?,?,?,?)",
        ("org:" + name, "bi:" + owner, "owner", owner, 1))
    runtime.db.execute(
        "INSERT INTO org_plugin_installs VALUES(?,?,?,?,?,?)",
        ("org:" + name, "crm", name, "bi:" + owner, owner, 1))
    runtime.db.commit()


def test_sheets_shared_with_other_forkmesh_users_are_read_only_and_copyable():
    runtime = Runtime()
    _install_second_organization(runtime)
    imported = call(runtime.use("POST", body={
        "fileName": "partners.csv",
        "csv": ("email,name,deal_size,notes\n"
                "ada@example.com,Ada,42000,internal only\n"
                "grace@example.com,Grace,,ssh\n"),
    }), "/api/crm/acme/contacts/import")
    view_id = imported["data"]["viewId"]
    columns = [dict(column, hidden=column["field"] == "notes" or column["hidden"])
               for column in imported["data"]["view"]["columns"]]
    call(runtime.use("PATCH", body={"columns": columns}),
         f"/api/crm/acme/views/{view_id}")

    # Nothing reaches bob until acme's sheet names him.
    assert call(runtime.use("GET", "bob"), "/api/crm/-/shared")["data"]["views"] == []
    shared = call(runtime.use("PATCH", body={"sharedWith": ["Bob"]}),
                  f"/api/crm/acme/views/{view_id}")
    assert shared["data"]["view"]["sharedWith"] == ["bob"]
    assert runtime.db.execute(
        "SELECT COUNT(*) FROM crm_view_shares WHERE view_id=?",
        (view_id,)).fetchone()[0] == 1

    listing = call(runtime.use("GET", "bob"), "/api/crm/-/shared")["data"]["views"]
    assert [(item["name"], item["organization"], item["shared"], item["canManage"])
            for item in listing] == [("partners", "acme", True, False)]
    # Who else the sheet reaches is the owner's business, and hidden columns
    # do not travel.
    assert listing[0]["sharedWith"] == []
    assert _columns(listing[0]) == ["email", "name", "deal_size"]
    assert "notes" not in {column["field"] for column in listing[0]["columns"]}

    detail = call(runtime.use("GET", "bob"), f"/api/crm/-/shared/{view_id}")["data"]
    assert detail["view"]["id"] == view_id
    rows = {contact["email"]: contact for contact in detail["contacts"]}
    assert rows["ada@example.com"]["fields"] == {"deal_size": "42000"}
    assert "notes" not in rows["ada@example.com"]
    assert "phone" not in rows["ada@example.com"]
    # Bob is not a member of acme, so the organization routes stay closed.
    assert call(runtime.use("PATCH", "bob", {"name": "Mine"}),
                f"/api/crm/acme/views/{view_id}")["status"] == 404
    assert call(runtime.use("GET", "bob"), "/api/crm/acme")["status"] == 404
    # The sheet also shows up alongside bob's own organization summary.
    summary = call(runtime.use("GET", "bob"), "/api/crm/globex")["data"]
    assert [item["id"] for item in summary["sharedViews"]] == [view_id]

    # Bob copies it into his own CRM as a new tab with the same columns.
    copied = call(runtime.use("POST", "bob", {"viewId": view_id}),
                  "/api/crm/globex/views/copy")
    assert copied["status"] == 201
    assert (copied["data"]["created"], copied["data"]["updated"]) == (2, 0)
    copy = copied["data"]["view"]
    assert (copy["name"], copy["source"], copy["visibility"]) == (
        "partners", "shared", "organization")
    assert _columns(copy) == ["email", "name", "deal_size"]
    globex = call(runtime.use("GET", "bob"), "/api/crm/globex")["data"]
    ada = next(item for item in globex["contacts"] if item["email"] == "ada@example.com")
    assert ada["fields"] == {"deal_size": "42000"}
    assert ada["views"] == [copy["id"]]
    assert ada["notes"] == ""
    assert ada["source"] == "shared"
    # Acme's rows are untouched by the copy.
    acme = call(runtime.use("GET"), "/api/crm/acme")["data"]
    assert len(acme["contacts"]) == 2
    assert acme["contacts"][0]["views"] == [view_id]
    assert call(runtime.use("POST", "bob", {"viewId": "0" * 32}),
                "/api/crm/globex/views/copy")["status"] == 404

    # A teammate the sheet is shared with sees it in the organization's own
    # tab strip, not as an outside share.
    call(runtime.use("PATCH", body={"sharedWith": ["bob", "charlie"]}),
         f"/api/crm/acme/views/{view_id}")
    charlie = call(runtime.use("GET", "charlie"), "/api/crm/acme")["data"]
    assert [item["id"] for item in charlie["views"]] == [view_id]
    assert charlie["sharedViews"] == []

    # Unsharing withdraws the sheet at once.
    call(runtime.use("PATCH", body={"sharedWith": []}),
         f"/api/crm/acme/views/{view_id}")
    assert call(runtime.use("GET", "bob"), "/api/crm/-/shared")["data"]["views"] == []
    assert call(runtime.use("GET", "bob"),
                f"/api/crm/-/shared/{view_id}")["status"] == 404

    # Deleting the sheet leaves no share index behind.
    call(runtime.use("PATCH", body={"sharedWith": ["bob"]}),
         f"/api/crm/acme/views/{view_id}")
    call(runtime.use("DELETE", body={}), f"/api/crm/acme/views/{view_id}")
    assert runtime.db.execute(
        "SELECT COUNT(*) FROM crm_view_shares").fetchone()[0] == 0


def test_share_names_are_checked_against_accounts_when_the_host_can():
    runtime = Runtime()

    async def account_exists(name):
        return name in ("bob", "charlie")

    runtime.account_exists = account_exists
    created = call(runtime.use("POST", body={
        "name": "Leads", "sharedWith": ["bob", "nobody", "charlie"],
    }), "/api/crm/acme/views")
    assert created["data"]["view"]["sharedWith"] == ["bob", "charlie"]
    assert runtime.db.execute(
        "SELECT COUNT(*) FROM crm_view_shares").fetchone()[0] == 2
    # Nothing is shared anywhere before any organization installs the plugin.
    empty = Runtime(installed=False)
    assert call(empty.use("GET"), "/api/crm/-/shared")["data"] == {
        "ok": True, "views": []}
    assert call(empty.use("GET"), "/api/crm/-/shared/" + "0" * 32)["status"] == 404


def _outreach_send(runtime, actor="alice", **overrides):
    body = {"to": "sponsor@example.com", "subject": "Sponsoring ForkMesh",
            "body": "Hi Sam, would Acme sponsor the relays?", "template": "sponsorship"}
    body.update(overrides)
    return call(runtime.use("POST", actor, body), "/api/crm/acme/outreach/send")


def test_outreach_is_gated_by_role_or_roster_and_sends_from_the_org_sender():
    runtime = Runtime()
    # A plain member is not on the roster yet: no templates, roster, or history
    # leaks, and sending is refused outright.
    denied = call(runtime.use("GET", "charlie"), "/api/crm/acme/outreach")
    assert denied["status"] == 200
    assert denied["data"]["allowed"] is False
    assert not {"templates", "recent", "members"} & set(denied["data"])
    assert _outreach_send(runtime, "charlie")["status"] == 403
    assert runtime.mailtrap_calls == []
    # Only owners and admins manage the roster; the refusal is audited.
    forbidden = call(runtime.use("POST", "charlie", {
        "action": "add", "name": "charlie"}), "/api/crm/acme/outreach/team")
    assert forbidden["status"] == 403
    assert runtime.audits[-1][0][1] == "crm.outreach_team_grant"
    assert runtime.audits[-1][1]["outcome"] == "denied"
    # Roster seats go to organization members only.
    missing = call(runtime.use("POST", body={
        "action": "add", "name": "bob"}), "/api/crm/acme/outreach/team")
    assert missing["status"] == 404
    assert runtime.audits[-1][1]["outcome"] == "denied"
    assert call(runtime.use("POST", body={
        "action": "promote", "name": "charlie"}),
        "/api/crm/acme/outreach/team")["status"] == 400
    added = call(runtime.use("POST", body={
        "action": "add", "name": "Charlie"}), "/api/crm/acme/outreach/team")
    assert added["status"] == 200
    assert [m["name"] for m in added["data"]["members"]] == ["charlie"]
    assert added["data"]["members"][0]["addedBy"] == "alice"
    assert runtime.audits[-1][0][1:] == (
        "crm.outreach_team_grant", "crm_outreach_member", "charlie")
    assert runtime.audits[-1][1].get("outcome", "success") == "success"

    state = call(runtime.use("GET", "charlie"), "/api/crm/acme/outreach")
    assert state["data"]["allowed"] is True
    assert state["data"]["canManage"] is False
    assert "members" not in state["data"]
    assert [t["key"] for t in state["data"]["templates"]] == [
        "blank", "sponsorship", "investor", "partnership", "strategic-support",
        "followup"]
    assert all(set(t) == {"key", "label", "subject", "body"}
               for t in state["data"]["templates"])
    assert state["data"]["fromEmail"] == "founders@forkmesh.com"
    assert state["data"]["sentToday"] == 0
    assert state["data"]["dailyLimit"] == crm.OUTREACH_DAILY_LIMIT

    # The organization's sender defaults replace the package default, both as
    # the envelope sender and inside the template signatures.
    assert call(runtime.use("PUT", body={
        "fromName": "Acme Founders", "fromEmail": "founders@acme.example",
    }), "/api/crm/acme/settings")["status"] == 200
    state = call(runtime.use("GET", "charlie"), "/api/crm/acme/outreach")
    assert state["data"]["fromEmail"] == "founders@acme.example"
    assert state["data"]["templates"][1]["body"].endswith(
        "ForkMesh — founders@acme.example")
    assert "{fromEmail}" not in state["data"]["templates"][1]["body"]

    contact = call(runtime.use("POST", body={
        "name": "Sam Sponsor", "email": "Sponsor@Example.com"}),
        "/api/crm/acme/contacts")
    assert contact["status"] == 201
    sent = _outreach_send(runtime, "charlie", to="Sponsor@Example.com")
    assert sent["status"] == 200
    assert (sent["data"]["to"], sent["data"]["from"], sent["data"]["sentToday"]) == (
        "sponsor@example.com", "founders@acme.example", 1)
    token, payload = runtime.mailtrap_calls[-1]
    assert token == ""
    assert payload["from"] == {"email": "founders@acme.example", "name": "Acme Founders"}
    assert payload["to"] == [{"email": "sponsor@example.com"}]
    assert payload["category"] == "CRM outreach"
    assert "html" not in payload or not payload["html"]
    assert runtime.audits[-1][0][1] == "crm.outreach.send"
    # The recipient and subject only exist inside the sealed envelope.
    row = runtime.db.execute(
        "SELECT org_bi,sender_name,ok,data FROM crm_outreach_log").fetchone()
    assert (row["org_bi"], row["sender_name"], row["ok"]) == ("org:acme", "charlie", 1)
    assert row["data"].startswith("sealed:")
    assert "sponsor@example.com" not in row["data"].split(":", 1)[0]
    # A recipient who is already a contact gets their last-contacted stamp.
    refreshed = call(runtime.use("GET"), "/api/crm/acme/contacts/" + contact["data"]["contact"]["id"])
    assert refreshed["data"]["contact"]["lastContactedAt"] > 0

    state = call(runtime.use("GET"), "/api/crm/acme/outreach")
    assert state["data"]["canManage"] is True
    assert [m["name"] for m in state["data"]["members"]] == ["charlie"]
    recent = state["data"]["recent"]
    assert [(r["to"], r["template"], r["ok"], r["sender"]) for r in recent] == [
        ("sponsor@example.com", "sponsorship", True, "charlie")]
    # The owner's own daily budget is untouched by charlie's send.
    assert state["data"]["sentToday"] == 0
    summary = call(runtime.use("GET"), "/api/crm/acme")
    assert summary["data"]["outreach"]["allowed"] is True
    assert summary["data"]["outreach"]["recent"] == recent

    removed = call(runtime.use("POST", body={
        "action": "remove", "name": "charlie"}), "/api/crm/acme/outreach/team")
    assert removed["status"] == 200 and removed["data"]["members"] == []
    assert runtime.audits[-1][0][1] == "crm.outreach_team_revoke"
    assert call(runtime.use("GET", "charlie"),
                "/api/crm/acme/outreach")["data"]["allowed"] is False
    assert _outreach_send(runtime, "charlie")["status"] == 403


def test_outreach_caps_daily_volume_per_sender_and_records_failed_sends():
    runtime = Runtime()
    assert _outreach_send(runtime, to="nobody")["status"] == 400
    assert _outreach_send(runtime, subject=" ")["status"] == 400
    assert _outreach_send(runtime, body="")["status"] == 400
    assert runtime.mailtrap_calls == []
    for _ in range(crm.OUTREACH_DAILY_LIMIT):
        assert _outreach_send(runtime)["status"] == 200
    capped = _outreach_send(runtime)
    assert capped["status"] == 429
    assert capped["data"] == {"error": "daily_limit_reached",
                              "dailyLimit": crm.OUTREACH_DAILY_LIMIT}
    assert len(runtime.mailtrap_calls) == crm.OUTREACH_DAILY_LIMIT
    state = call(runtime.use("GET"), "/api/crm/acme/outreach")
    assert state["data"]["sentToday"] == crm.OUTREACH_DAILY_LIMIT
    assert len(state["data"]["recent"]) == crm.OUTREACH_LOG_LIST_LIMIT

    # A provider rejection is reported, logged as failed, audited, and still
    # counts against the sender's budget.
    call(runtime.use("POST", body={"action": "add", "name": "charlie"}),
         "/api/crm/acme/outreach/team")

    async def reject_mailtrap(token, payload):
        runtime.mailtrap_calls.append((token, payload))
        return {"accepted": False, "messageId": "", "error": "mailtrap_http_401"}

    runtime.send_mailtrap = reject_mailtrap
    failed = _outreach_send(runtime, "charlie", template="not a key")
    assert failed["status"] == 502
    assert failed["data"]["error"] == "send_failed"
    assert "mailtrap_http_401" in failed["data"]["detail"]
    assert failed["data"]["sentToday"] == 1
    assert runtime.audits[-1][0][1] == "crm.outreach.send"
    assert runtime.audits[-1][1]["outcome"] == "failed"
    state = call(runtime.use("GET", "charlie"), "/api/crm/acme/outreach")
    assert state["data"]["sentToday"] == 1
    assert (state["data"]["recent"][0]["ok"], state["data"]["recent"][0]["error"],
            state["data"]["recent"][0]["template"]) == (False, "mailtrap_http_401", "blank")


def test_outreach_rows_are_exported_and_purged_with_the_organization():
    runtime = Runtime()
    call(runtime.use("POST", body={"action": "add", "name": "charlie"}),
         "/api/crm/acme/outreach/team")
    assert _outreach_send(runtime)["status"] == 200
    assert asyncio.run(crm.org_has_data(runtime, "org:acme")) is True
    export = asyncio.run(crm.export_org_data(runtime, "org:acme", "acme"))
    assert [m["name"] for m in export["outreach"]["team"]] == ["charlie"]
    assert [(r["to"], r["ok"]) for r in export["outreach"]["sent"]] == [
        ("sponsor@example.com", True)]
    asyncio.run(crm.purge_org_data(runtime, "org:acme"))
    assert runtime.db.execute("SELECT COUNT(*) FROM crm_outreach_log").fetchone()[0] == 0
    assert runtime.db.execute("SELECT COUNT(*) FROM crm_outreach_team").fetchone()[0] == 0
    assert asyncio.run(crm.org_has_data(runtime, "org:acme")) is False


def test_a_new_sheet_opens_showing_its_columns_and_keeps_the_ones_hidden_later():
    runtime = Runtime()
    # A sheet that leads with no columns of its own - an empty tab, or a
    # pipeline - used to open with every column hidden, which left no heading
    # to right-click and no way back.
    sheet = call(runtime.use("POST", body={"name": "Warm intros"}),
                 "/api/crm/acme/views")["data"]["view"]
    assert _columns(sheet) == [field for field, _, _ in crm.CONTACT_COLUMNS]
    pipeline = call(runtime.use("POST", body={"name": "Deals", "flow": "customers"}),
                    "/api/crm/acme/views")["data"]["view"]
    assert _columns(pipeline) == _columns(sheet)

    # Hiding fields sticks: through the reply, a re-read, a rename, and a share
    # edit that carries no columns of its own.
    hidden = {"twitter", "mastodon", "bluesky", "github", "notes"}
    layout = [dict(column, hidden=column["field"] in hidden or column["hidden"])
              for column in sheet["columns"]]
    saved = call(runtime.use("PATCH", body={"columns": layout}),
                 f"/api/crm/acme/views/{sheet['id']}")["data"]["view"]
    kept = _columns(saved)
    assert not hidden & set(kept)
    for body in ({"name": "Intros"}, {"visibility": "private",
                                      "sharedWith": ["charlie"]}):
        assert _columns(call(runtime.use("PATCH", body=body),
                             f"/api/crm/acme/views/{sheet['id']}")["data"]["view"]) == kept
    summary = call(runtime.use("GET"), "/api/crm/acme")["data"]["views"]
    assert _columns(next(v for v in summary if v["id"] == sheet["id"])) == kept


def test_tabs_keep_the_order_a_member_drags_them_into():
    runtime = Runtime()
    ids = [call(runtime.use("POST", body={"name": name}),
                "/api/crm/acme/views")["data"]["view"]["id"]
           for name in ("Partners", "Investors", "Hiring")]
    listing = call(runtime.use("GET"), "/api/crm/acme/views")["data"]["views"]
    assert [view["name"] for view in listing] == ["Partners", "Investors", "Hiring"]

    moved = call(runtime.use("POST", body={"order": [ids[2], ids[0], ids[1]]}),
                 "/api/crm/acme/views/order")
    assert moved["status"] == 200
    assert [view["name"] for view in moved["data"]["views"]] == [
        "Hiring", "Partners", "Investors"]
    assert [view["position"] for view in moved["data"]["views"]] == [0, 1, 2]
    assert runtime.audits[-1][0][1] == "crm.view.order"

    # The strip is the organization's, so every member opens the same order,
    # and any member may move a tab the way any of them may rename one.
    for actor in ("alice", "charlie"):
        assert [view["name"] for view
                in call(runtime.use("GET", actor), "/api/crm/acme")["data"]["views"]] == [
            "Hiring", "Partners", "Investors"]
    member = call(runtime.use("POST", "charlie", {"order": [ids[1]]}),
                  "/api/crm/acme/views/order")
    assert [view["name"] for view in member["data"]["views"]] == [
        "Investors", "Hiring", "Partners"]

    # Junk, unknown ids and a half-filled order leave the rest where they were.
    partial = call(runtime.use("POST", body={"order": ["nonsense", ids[2]]}),
                   "/api/crm/acme/views/order")
    assert [view["name"] for view in partial["data"]["views"]] == [
        "Hiring", "Investors", "Partners"]
    assert call(runtime.use("GET"), "/api/crm/acme/views/order")["status"] == 405


def test_a_sheet_a_member_cannot_open_keeps_its_place_in_their_order():
    runtime = Runtime()
    shared = call(runtime.use("POST", body={"name": "Open"}),
                  "/api/crm/acme/views")["data"]["view"]["id"]
    secret = call(runtime.use("POST", body={
        "name": "Secret", "visibility": "private"}),
        "/api/crm/acme/views")["data"]["view"]["id"]
    # charlie sees only the open sheet, so ordering by it must not disturb the
    # private one alice still owns.
    assert [view["id"] for view
            in call(runtime.use("GET", "charlie"), "/api/crm/acme/views")["data"]["views"]] == [shared]
    call(runtime.use("POST", "charlie", {"order": [shared]}),
         "/api/crm/acme/views/order")
    assert [view["name"] for view
            in call(runtime.use("GET"), "/api/crm/acme/views")["data"]["views"]] == [
        "Open", "Secret"]
    assert [view["id"] for view
            in call(runtime.use("GET", "charlie"), "/api/crm/acme/views")["data"]["views"]] == [shared]


def test_a_tab_created_after_a_reorder_joins_the_end_of_the_strip():
    runtime = Runtime()
    ids = [call(runtime.use("POST", body={"name": name}),
                "/api/crm/acme/views")["data"]["view"]["id"]
           for name in ("Partners", "Investors")]
    call(runtime.use("POST", body={"order": [ids[1], ids[0]]}),
         "/api/crm/acme/views/order")
    call(runtime.use("POST", body={"name": "Hiring"}), "/api/crm/acme/views")
    assert [view["name"] for view
            in call(runtime.use("GET"), "/api/crm/acme/views")["data"]["views"]] == [
        "Investors", "Partners", "Hiring"]


def _sheet(runtime, name):
    return call(runtime.use("POST", body={"name": name}),
                "/api/crm/acme/views")["data"]["view"]["id"]


def test_moving_a_contact_between_lists_replaces_and_removes_memberships():
    runtime = Runtime()
    prospects = _sheet(runtime, "Prospects")
    partners = _sheet(runtime, "Partners")
    archive = _sheet(runtime, "Archive")
    contact = call(runtime.use("POST", body={
        "name": "Ada", "views": [prospects, partners]}),
        "/api/crm/acme/contacts")["data"]["contact"]
    assert sorted(contact["views"]) == sorted([prospects, partners])
    path = "/api/crm/acme/contacts/" + contact["id"]

    # `views` still only adds, so the browser extension and CSV import keep
    # putting a contact on another list without disturbing the rest.
    added = call(runtime.use("PATCH", body={"views": [archive]}), path)
    assert sorted(added["data"]["contact"]["views"]) == sorted(
        [prospects, partners, archive])

    # "Move to list" is a replacement: the target and nothing else.
    moved = call(runtime.use("PATCH", body={"setViews": [partners]}), path)
    assert moved["data"]["contact"]["views"] == [partners]

    # "Remove from this list" drops just that one.
    call(runtime.use("PATCH", body={"views": [archive]}), path)
    removed = call(runtime.use("PATCH", body={"removeViews": [partners]}), path)
    assert removed["data"]["contact"]["views"] == [archive]

    # Removing the last list leaves the contact in the organization, unlisted.
    emptied = call(runtime.use("PATCH", body={"removeViews": [archive]}), path)
    assert emptied["data"]["contact"]["views"] == []
    assert emptied["data"]["contact"]["name"] == "Ada"


def test_a_list_carries_a_pipeline_type_that_owners_can_set_and_clear():
    runtime = Runtime()
    view_id = _sheet(runtime, "Fundraising")
    path = "/api/crm/acme/views/" + view_id
    assert call(runtime.use("GET"), path)["data"]["view"]["flow"] == ""

    bound = call(runtime.use("PATCH", body={"flow": "investment"}), path)
    assert bound["data"]["view"]["flow"] == "investment"

    # A member the sheet only reaches may rename it and move its columns, but
    # not change which pipeline the whole organization reads off it.
    blocked = call(runtime.use("PATCH", actor="charlie",
                               body={"flow": "customers"}), path)
    assert blocked["data"]["view"]["flow"] == "investment"

    assert call(runtime.use("PATCH", body={"flow": ""}),
                path)["data"]["view"]["flow"] == ""


def test_contacts_carry_a_deal_value_and_a_next_step():
    runtime = Runtime()
    created = call(runtime.use("POST", body={
        "name": "Ada", "value": "$12,500.40", "nextStep": "  Send the deck  ",
    }), "/api/crm/acme/contacts")["data"]["contact"]
    assert created["value"] == 12500
    assert created["nextStep"] == "Send the deck"

    path = "/api/crm/acme/contacts/" + created["id"]
    assert call(runtime.use("PATCH", body={"value": -5}),
                path)["data"]["contact"]["value"] == 0
    assert call(runtime.use("PATCH", body={"value": "n/a"}),
                path)["data"]["contact"]["value"] == 0
    assert call(runtime.use("PATCH", body={"value": 900}),
                path)["data"]["contact"]["value"] == 900


def test_a_csv_round_trip_keeps_the_pipeline_deal_value_and_next_step():
    runtime = Runtime()
    call(runtime.use("POST", body={
        "name": "Ada", "flow": "investment", "stage": "diligence",
        "value": 4200, "nextStep": "Book the partner meeting",
    }), "/api/crm/acme/contacts")
    exported = call(runtime.use("GET"), "/api/crm/acme/contacts/export")
    header, row = exported["data"]["csv"].splitlines()[:2]
    columns = header.split(",")
    for column in ("flow", "stage", "value", "next_step"):
        assert column in columns
    cells = dict(zip(columns, row.split(",")))
    assert cells["flow"] == "investment"
    assert cells["stage"] == "diligence"
    assert cells["value"] == "4200"
    assert cells["next_step"] == "Book the partner meeting"


def _save_flows(runtime, actor="alice", org="acme"):
    call(runtime.use("PUT", actor, {"flows": [
        {"id": "fundraising", "name": "Fundraising",
         "stages": ["Warm intro", "Pitch", "Closed"]},
        {"id": "partners", "name": "Partners", "stages": ["Prospect", "Signed"]},
    ]}), f"/api/crm/{org}/settings")


def _placements(runtime, actor="alice", org="acme"):
    contacts = call(runtime.use("GET", actor), f"/api/crm/{org}")["data"]["contacts"]
    return {item["email"]: (item["flow"], item["stage"]) for item in contacts}


def test_a_csv_import_only_writes_stages_the_contacts_flow_has():
    runtime = Runtime()
    _save_flows(runtime)
    imported = call(runtime.use("POST", body={"csv": (
        "name,email,flow,stage\n"
        "Ada,ada@example.com,fundraising,\n"
        "Grace,grace@example.com,Partners,Signed\n"
        "Linus,linus@example.com,partners,won\n"
        "Katherine,katherine@example.com,,\n"
        "Alan,alan@example.com,customers,lead\n"
    )}), "/api/crm/acme/contacts/import")
    assert (imported["data"]["created"], imported["data"]["skipped"]) == (5, 0)
    assert _placements(runtime) == {
        "ada@example.com": ("fundraising", "warm-intro"),
        "grace@example.com": ("partners", "signed"),
        "linus@example.com": ("partners", "prospect"),
        "katherine@example.com": ("fundraising", "warm-intro"),
        "alan@example.com": ("fundraising", "warm-intro"),
    }


def test_the_starter_flows_still_default_an_import_to_their_first_stage():
    runtime = Runtime()
    call(runtime.use("POST", body={"csv": (
        "name,email,flow\n"
        "Ada,ada@example.com,\n"
        "Grace,grace@example.com,investment\n"
        "Linus,linus@example.com,applicants\n"
        "Alan,alan@example.com,partnerships\n"
    )}), "/api/crm/acme/contacts/import")
    assert _placements(runtime) == {
        "ada@example.com": ("customers", "lead"),
        "grace@example.com": ("investment", "sourced"),
        "linus@example.com": ("applicants", "applied"),
        "alan@example.com": ("partnerships", "prospect"),
    }


def test_re_importing_a_stranded_contact_moves_it_into_its_flows_first_stage():
    runtime = Runtime()
    _save_flows(runtime)
    call(runtime.use("POST", body={"csv": "name,email\nAda,ada@example.com\n"}),
         "/api/crm/acme/contacts/import")
    contact_id, data = runtime.db.execute(
        "SELECT contact_id,data FROM crm_contacts").fetchone()
    stranded = dict(json.loads(data.removeprefix("sealed:")),
                    flow="fundraising", stage="lead")
    runtime.db.execute("UPDATE crm_contacts SET data=? WHERE contact_id=?",
                       ("sealed:" + json.dumps(stranded), contact_id))
    runtime.db.commit()

    merged = call(runtime.use("POST", body={
        "csv": "name,email,company\nAda,ada@example.com,Engines\n"}),
        "/api/crm/acme/contacts/import")
    assert merged["data"]["updated"] == 1
    assert _placements(runtime) == {"ada@example.com": ("fundraising", "warm-intro")}


def test_a_pipeline_shared_for_editing_lets_an_outside_partner_manage_only_it():
    runtime = Runtime()
    call(runtime.use("PUT", body={"flows": [
        {"id": "investors", "name": "Investors", "stages": ["new", "complete"]},
        {"id": "partner-bob", "name": "Bob's users",
         "stages": ["new", "contacted", "signed-up"]},
    ]}), "/api/crm/acme/settings")
    private = call(runtime.use("POST", body={
        "name": "Vera", "email": "vera@example.com", "flow": "investors"}),
        "/api/crm/acme/contacts")["data"]["contact"]
    sheet = call(runtime.use("POST", body={
        "name": "Bob's users", "flow": "partner-bob", "visibility": "private",
        "sharedWith": ["bob"], "shareAccess": "edit"}), "/api/crm/acme/views")
    assert sheet["status"] == 201
    view_id = sheet["data"]["view"]["id"]
    assert sheet["data"]["view"]["shareAccess"] == "edit"
    base = f"/api/crm/-/shared/{view_id}"

    opened = call(runtime.use("GET", "bob"), base)["data"]
    assert opened["view"]["canEdit"] is True
    assert opened["view"]["stages"] == ["new", "contacted", "signed-up"]
    assert opened["contacts"] == []

    added = call(runtime.use("POST", "bob", {
        "name": "Dee", "email": "dee@example.com", "stage": "contacted",
        "owner": "mallory", "flow": "investors", "views": ["x" * 32]}), base + "/contacts")
    assert added["status"] == 201
    dee = added["data"]["contact"]
    assert (dee["flow"], dee["stage"], dee["name"]) == ("partner-bob", "contacted", "Dee")
    stored = next(contact for contact in call(runtime.use("GET"), "/api/crm/acme/contacts")[
        "data"]["contacts"] if contact["id"] == dee["id"])
    assert (stored["owner"], stored["source"], stored["views"]) == (
        "bob", "shared sheet (bob)", [view_id])
    assert call(runtime.use("POST", "bob", {"name": "Vera again", "email": "vera@example.com"}),
                base + "/contacts")["status"] == 409

    moved = call(runtime.use("PATCH", "bob", {"stage": "signed-up", "flow": "investors",
                                              "owner": "mallory", "nextStep": "Onboard"}),
                 f"{base}/contacts/{dee['id']}")
    assert moved["status"] == 200
    assert moved["data"]["contact"]["stage"] == "signed-up"
    after = next(contact for contact in call(runtime.use("GET"), "/api/crm/acme/contacts")[
        "data"]["contacts"] if contact["id"] == dee["id"])
    assert (after["flow"], after["owner"], after["nextStep"]) == ("partner-bob", "bob", "Onboard")
    assert call(runtime.use("PATCH", "bob", {"stage": "complete"}),
                f"{base}/contacts/{dee['id']}")["status"] == 400
    assert call(runtime.use("PATCH", "bob", {"name": "Mine"}),
                f"{base}/contacts/{private['id']}")["status"] == 404
    assert call(runtime.use("DELETE", "bob"), f"{base}/contacts/{dee['id']}")["status"] == 405
    assert call(runtime.use("POST", "charlie", {"name": "Eve"}), base + "/contacts")["status"] == 404

    columns = [dict(column, hidden=column["field"] == "email" or column["hidden"])
               for column in sheet["data"]["view"]["columns"]]
    call(runtime.use("PATCH", body={"columns": columns}), f"/api/crm/acme/views/{view_id}")
    call(runtime.use("PATCH", "bob", {"email": "other@example.com"}),
         f"{base}/contacts/{dee['id']}")
    assert next(contact for contact in call(runtime.use("GET"), "/api/crm/acme/contacts")[
        "data"]["contacts"] if contact["id"] == dee["id"])["email"] == "dee@example.com"

    assert call(runtime.use("PATCH", "charlie", {"shareAccess": "view"}),
                f"/api/crm/acme/views/{view_id}")["status"] == 404
    call(runtime.use("PATCH", body={"shareAccess": "view"}), f"/api/crm/acme/views/{view_id}")
    assert call(runtime.use("GET", "bob"), base)["data"]["view"]["canEdit"] is False
    assert call(runtime.use("POST", "bob", {"name": "Fay"}), base + "/contacts")["status"] == 403


def test_copying_a_shared_sheet_places_rows_in_the_copying_organizations_flows():
    runtime = Runtime()
    _install_second_organization(runtime)
    _save_flows(runtime)
    imported = call(runtime.use("POST", body={
        "csv": "name,email,flow,stage\nAda,ada@example.com,fundraising,pitch\n"}),
        "/api/crm/acme/contacts/import")
    view_id = imported["data"]["viewId"]
    call(runtime.use("PATCH", body={"sharedWith": ["bob"]}),
         f"/api/crm/acme/views/{view_id}")

    copied = call(runtime.use("POST", "bob", {"viewId": view_id}),
                  "/api/crm/globex/views/copy")
    assert copied["data"]["created"] == 1
    assert _placements(runtime, "bob", "globex") == {
        "ada@example.com": ("investment", "sourced")}
    assert _placements(runtime) == {"ada@example.com": ("fundraising", "pitch")}


def test_a_bulk_move_updates_every_contact_in_one_request():
    runtime = Runtime()
    prospects = _sheet(runtime, "Prospects")
    archive = _sheet(runtime, "Archive")
    ids = [call(runtime.use("POST", body={"name": name, "views": [prospects]}),
                "/api/crm/acme/contacts")["data"]["contact"]["id"]
           for name in ("Ada", "Grace", "Linus")]
    gone = "f" * 32
    path = "/api/crm/acme/contacts/bulk"
    moved = call(runtime.use("POST", body={"edits": [
        {"id": ids[0], "flow": "investment", "stage": "sourced"},
        # Membership only: an email riding along is ignored.
        {"id": ids[1], "setViews": [archive], "email": "someone@example.com"},
        {"id": ids[2], "views": [archive], "removeViews": [prospects]},
        {"id": gone, "stage": "lead"},
    ]}), path)
    assert moved["status"] == 200
    contacts = {item["id"]: item for item in moved["data"]["contacts"]}
    assert (contacts[ids[0]]["flow"], contacts[ids[0]]["stage"]) == ("investment", "sourced")
    assert contacts[ids[0]]["views"] == [prospects]
    assert contacts[ids[1]]["views"] == [archive]
    assert contacts[ids[1]]["email"] == ""
    assert contacts[ids[2]]["views"] == [archive]
    assert moved["data"]["missing"] == [gone]

    # What came back is what was stored, and it was one audited change.
    listed = {item["id"]: item for item in call(
        runtime.use("GET"), "/api/crm/acme/contacts")["data"]["contacts"]}
    for contact_id in ids:
        assert listed[contact_id] == contacts[contact_id]
    assert [args[1] for args, _ in runtime.audits].count("crm.contacts.bulk_update") == 1

    assert call(runtime.use("POST", body={"edits": [{"id": "nope"}]}), path)["status"] == 400
    assert call(runtime.use("POST", body={}), path)["status"] == 400
    assert call(runtime.use("PATCH", body={"edits": []}), path)["status"] == 405


def test_deleting_a_custom_column_clears_every_contact_and_sheet():
    runtime = Runtime()
    sheet = call(runtime.use("POST", body={"name": "Imported", "columns": [
        {"field": "name"}, {"field": "city"}, {"field": "twitter_id"}]}),
        "/api/crm/acme/views")["data"]["view"]["id"]
    ada = call(runtime.use("POST", body={
        "name": "Ada", "fields": {"city": "London", "twitter_id": "42"}}),
        "/api/crm/acme/contacts")["data"]["contact"]["id"]
    grace = call(runtime.use("POST", body={
        "name": "Grace", "fields": {"twitter_id": "7"}}),
        "/api/crm/acme/contacts")["data"]["contact"]["id"]
    path = "/api/crm/acme/contacts/delete-column"
    deleted = call(runtime.use("POST", body={"field": "City"}), path)
    assert deleted["status"] == 200
    assert deleted["data"] == {"ok": True, "field": "city", "contacts": 1, "views": 1}

    listed = {item["id"]: item for item in call(
        runtime.use("GET"), "/api/crm/acme/contacts")["data"]["contacts"]}
    assert listed[ada]["fields"] == {"twitter_id": "42"}
    assert listed[grace]["fields"] == {"twitter_id": "7"}
    view = call(runtime.use("GET"), f"/api/crm/acme/views/{sheet}")["data"]["view"]
    assert [column["field"] for column in view["columns"]
            if column["custom"]] == ["twitter_id"]
    assert [args[1] for args, _ in runtime.audits].count("crm.column.delete") == 1

    # Name, email, pipeline and stage are what a record is; they can only be hidden.
    assert call(runtime.use("POST", body={"field": "email"}), path)["status"] == 400
    assert call(runtime.use("POST", body={}), path)["status"] == 400
    assert call(runtime.use("DELETE", body={"field": "twitter_id"}), path)["status"] == 405


def test_deleting_a_builtin_column_blanks_it_hides_it_everywhere_and_is_for_admins():
    runtime = Runtime()
    sheet = call(runtime.use("POST", body={"name": "Imported", "columns": [
        {"field": "name"}, {"field": "twitter"}]}), "/api/crm/acme/views")["data"]["view"]["id"]
    ada = call(runtime.use("POST", body={"name": "Ada", "twitter": "@ada"}),
               "/api/crm/acme/contacts")["data"]["contact"]["id"]
    path = "/api/crm/acme/contacts/delete-column"
    assert call(runtime.use("POST", "charlie", {"field": "twitter"}), path)["status"] == 403
    deleted = call(runtime.use("POST", body={"field": "twitter"}), path)
    assert deleted["status"] == 200
    assert deleted["data"]["contacts"] == 1
    assert deleted["data"]["removedColumns"] == ["twitter"]
    listed = {item["id"]: item for item in call(
        runtime.use("GET"), "/api/crm/acme/contacts")["data"]["contacts"]}
    assert listed[ada]["twitter"] == ""
    view = call(runtime.use("GET"), f"/api/crm/acme/views/{sheet}")["data"]["view"]
    assert [column["hidden"] for column in view["columns"]
            if column["field"] == "twitter"] == [True]
    settings = call(runtime.use("GET"), "/api/crm/acme/settings")["data"]["settings"]
    assert settings["removedColumns"] == ["twitter"]
    for field in ("name", "email", "flow", "stage", "lastContactedAt"):
        assert call(runtime.use("POST", body={"field": field}), path)["status"] == 400
    restored = call(runtime.use("PUT", body={"removedColumns": ["twitter", "bogus"][1:]}),
                    "/api/crm/acme/settings")
    assert restored["data"]["settings"]["removedColumns"] == []


def test_table_views_are_saved_in_settings_by_any_member():
    runtime = Runtime()
    path = "/api/crm/acme/settings"
    assert call(runtime.use("GET"), path)["data"]["settings"]["tableViews"] == []
    saved = call(runtime.use("PUT", "charlie", {"tableViews": [
        {"id": "outreach", "name": " Outreach ", "columns": [
            "name", "stage", "field:bad", "commits", "mastodon-latest", "name", "a b"]},
        {"id": "default", "name": "Shadow", "columns": ["name"]},
        {"id": "all", "name": "Shadow", "columns": ["name"]},
        {"id": "outreach", "name": "Twice", "columns": []},
        {"id": "untitled", "name": "", "columns": ["name"]},
        "junk",
    ], "fromName": "Ignored"}), path)
    assert saved["status"] == 200
    settings = saved["data"]["settings"]
    assert settings["tableViews"] == [{"id": "outreach", "name": "Outreach", "columns": [
        "name", "stage", "commits", "mastodon-latest"]}]
    assert not settings.get("fromName")
    assert call(runtime.use("PUT", "charlie", {"fromName": "Charlie"}), path)["status"] == 403
    kept = call(runtime.use("PUT", body={"fromName": "Acme"}), path)["data"]["settings"]
    assert kept["tableViews"][0]["id"] == "outreach"
    cleared = call(runtime.use("PUT", "charlie", {"tableViews": []}), path)
    assert cleared["data"]["settings"]["tableViews"] == []


def test_lists_sharing_a_pipeline_never_hold_the_same_contact():
    views = [{"id": "a", "flow": "investors"}, {"id": "b", "flow": "investors"},
             {"id": "plain", "flow": ""}, {"id": "c", "flow": "customers"}]
    lists = crm._pipeline_lists(views)
    assert lists == {"investors": ["a", "b"], "customers": ["c"]}

    def holding(contact):
        return [view["id"] for view in views if crm._contact_in_view(contact, view, lists)]

    assert holding({"flow": "investors", "views": ["b", "plain"]}) == ["b", "plain"]
    assert holding({"flow": "investors", "views": []}) == ["a"]
    assert holding({"flow": "investors", "views": ["b", "a"]}) == ["a"]
    assert holding({"flow": "customers", "views": ["a"]}) == ["c"]
    assert holding({"flow": "partners", "views": ["b"]}) == []


def test_a_duplicate_contact_merges_into_the_one_kept():
    runtime = Runtime()
    prospects = _sheet(runtime, "Prospects")
    archive = _sheet(runtime, "Archive")
    kept = call(runtime.use("POST", body={
        "name": "Ada Lovelace", "email": "ada@example.com", "company": "",
        "tags": ["founder"], "notes": "Met at the conference",
        "views": [prospects], "stage": "qualified", "lastContactedAt": 100,
        "fields": {"city": "London"},
    }), "/api/crm/acme/contacts")["data"]["contact"]["id"]
    duplicate = call(runtime.use("POST", body={
        "name": "Ada L.", "email": "ada@engines.example",
        "company": "Analytical Engines", "tags": ["priority", "founder"],
        "notes": "Wants a demo", "views": [archive], "stage": "lead",
        "lastContactedAt": 900, "fields": {"city": "Paris", "twitter_id": "42"},
    }), "/api/crm/acme/contacts")["data"]["contact"]["id"]
    runtime.db.execute(
        "INSERT INTO crm_email_sends(send_id,org_bi,org_name,contact_id,sender_bi,"
        "sender_name,transport,status,created_at,data) "
        "VALUES('e1','org:acme','acme',?,'bi:alice','alice','external','external',1,'sealed:{}')",
        (duplicate,))
    path = "/api/crm/acme/contacts/merge"

    merged = call(runtime.use("POST", "charlie", {"from": duplicate, "into": kept}), path)
    assert merged["status"] == 200
    contact = merged["data"]["contact"]
    assert (contact["id"], merged["data"]["merged"]) == (kept, duplicate)
    # The kept record's own values win; the duplicate only fills its blanks.
    assert (contact["name"], contact["email"], contact["stage"]) == (
        "Ada Lovelace", "ada@example.com", "qualified")
    assert contact["company"] == "Analytical Engines"
    assert contact["tags"] == ["founder", "priority"]
    assert contact["views"] == [prospects, archive]
    assert contact["notes"] == "Met at the conference\n\nWants a demo"
    assert contact["lastContactedAt"] == 900
    assert contact["fields"] == {"city": "London", "twitter_id": "42",
                                 "other_emails": "ada@engines.example"}
    assert runtime.db.execute(
        "SELECT contact_id FROM crm_email_sends WHERE send_id='e1'").fetchone()[0] == kept
    listed = call(runtime.use("GET"), "/api/crm/acme/contacts")["data"]["contacts"]
    assert [item["id"] for item in listed] == [kept]
    assert [args[1] for args, _ in runtime.audits].count("crm.contact.merge") == 1

    # The duplicate's address is free again, and the kept one still unique.
    again = call(runtime.use("POST", body={"email": "ada@engines.example"}),
                 "/api/crm/acme/contacts")
    assert again["status"] == 201
    assert call(runtime.use("POST", body={"from": kept, "into": kept}), path)["status"] == 400
    assert call(runtime.use("POST", body={"from": duplicate, "into": kept}), path)["status"] == 404
    assert call(runtime.use("POST", body={"from": "nope", "into": kept}), path)["status"] == 400
    assert call(runtime.use("GET"), path)["status"] == 405
    assert call(runtime.use("POST", "bob", {"from": again["data"]["contact"]["id"],
                                            "into": kept}), path)["status"] == 404


def test_a_merge_takes_the_duplicate_address_when_the_kept_box_has_none():
    runtime = Runtime()
    kept = call(runtime.use("POST", body={"name": "Grace Hopper"}),
                "/api/crm/acme/contacts")["data"]["contact"]["id"]
    duplicate = call(runtime.use("POST", body={"email": "grace@navy.example"}),
                     "/api/crm/acme/contacts")["data"]["contact"]["id"]
    merged = call(runtime.use("POST", body={"from": duplicate, "into": kept}),
                  "/api/crm/acme/contacts/merge")["data"]["contact"]
    assert (merged["name"], merged["email"], merged["fields"]) == (
        "Grace Hopper", "grace@navy.example", {})
    assert call(runtime.use("POST", body={"email": "grace@navy.example"}),
                "/api/crm/acme/contacts")["status"] == 409


def test_summary_rereads_with_an_unchanged_revision_skip_the_rows():
    runtime = Runtime()
    call(runtime.use("POST", body={"name": "Ada", "email": "ada@example.com"}),
         "/api/crm/acme/contacts")
    first = call(runtime.use("GET"), "/api/crm/acme")["data"]
    revision = first["revision"]
    assert len(revision) == 32 and first["contacts"]

    again = call(runtime.use("GET", query={"since": revision}), "/api/crm/acme")
    assert again["data"] == {"ok": True, "unchanged": True, "revision": revision}

    # Another member's write changes the revision, so the full rows come back.
    call(runtime.use("POST", actor="charlie", body={"name": "Grace"}),
         "/api/crm/acme/contacts")
    changed = call(runtime.use("GET", query={"since": revision}), "/api/crm/acme")["data"]
    assert "unchanged" not in changed and changed["revision"] != revision
    assert {item["name"] for item in changed["contacts"]} == {"Ada", "Grace"}

    # The revision covers the caller's own view of the organization too.
    revision = changed["revision"]
    stale = call(runtime.use("GET", actor="charlie", query={"since": revision}),
                 "/api/crm/acme")["data"]
    assert "unchanged" not in stale


def test_the_summary_ranks_members_by_daily_activity_in_the_callers_time_zone():
    runtime = Runtime()
    for name in ("Ada", "Grace"):
        assert call(runtime.use("POST", body={"name": name}),
                    "/api/crm/acme/contacts")["status"] == 201
    assert call(runtime.use("POST", "charlie", {"name": "Linus"}),
                "/api/crm/acme/contacts")["status"] == 201
    assert _outreach_send(runtime)["status"] == 200
    day = crm.DAY_MS
    now = runtime.clock
    for index, (sender, status, created) in enumerate((
            ("charlie", "external", now),
            ("charlie", "delivered", now - day),
            ("charlie", "draft", now),
            ("charlie", "external", now - crm.ACTIVITY_DAYS * day),
            ("bob", "external", now))):
        runtime.db.execute(
            "INSERT INTO crm_email_sends(send_id,org_bi,org_name,sender_bi,"
            "sender_name,transport,status,created_at,data) VALUES(?,?,?,?,?,?,?,?,?)",
            (f"{index:032x}", "org:acme", "acme", "bi:" + sender, sender,
             "external", status, created, "sealed:{}"))

    activity = call(runtime.use("GET"), "/api/crm/acme")["data"]["activity"]
    assert activity["kinds"] == ["emails", "outreach", "contacts"]
    assert len(activity["days"]) == crm.ACTIVITY_DAYS
    assert activity["days"][-1] == "2033-05-18"
    members = {item["name"]: item for item in activity["members"]}
    assert [item["name"] for item in activity["members"]] == ["charlie", "alice", "bob"]
    assert members["charlie"]["emails"][-2:] == [1, 1]
    assert sum(members["charlie"]["emails"]) == 2
    assert members["charlie"]["contacts"][-1] == 1
    assert members["alice"]["outreach"][-1] == 1
    assert members["alice"]["contacts"][-1] == 2
    assert sum(members["bob"]["emails"]) == 1

    west = call(runtime.use("GET", query={"tz": "-300"}),
                "/api/crm/acme")["data"]["activity"]
    assert west["tzOffset"] == -300
    assert west["days"][-1] == "2033-05-17"
    assert call(runtime.use("GET", query={"tz": "99999"}),
                "/api/crm/acme")["data"]["activity"]["tzOffset"] == 14 * 60


def test_an_inbound_link_to_a_picked_contact_keeps_the_senders_name_and_new_subject():
    runtime = Runtime()
    zach = call(runtime.use("POST", body={"name": "Zach Holman", "email": "zach@holman.example"}),
                "/api/crm/acme/contacts")["data"]["contact"]

    def link(**fields):
        return call(runtime.use("POST", body={
            "provider": "zoho", "messageId": "1790715944800154100", "direction": "inbound",
            "contactEmail": "holman+x1@mail.signed.example", "contactId": zach["id"],
            "providerUrl": "https://mail.zoho.com/zm/#mail/folder/spam/p/1790715944800154100",
            **fields,
        }), "/api/crm/acme/emails/link")["data"]

    first = link(subject="Spam")
    assert first["email"]["fromName"] == "Zach Holman"
    assert first["email"]["fromEmail"] == "holman+x1@mail.signed.example"
    fixed = link(subject="Intro: ForkMesh", contactName="Zach H.")
    assert fixed["relinked"] is True
    assert fixed["email"]["subject"] == "Intro: ForkMesh"
    assert fixed["email"]["fromName"] == "Zach H."


def _contact(runtime, **body):
    return call(runtime.use("POST", body=body),
                "/api/crm/acme/contacts")["data"]["contact"]["id"]


def test_the_investor_deck_only_goes_to_contacts_in_an_investor_pipeline():
    runtime = Runtime()
    call(runtime.use("PUT", body={"fromEmail": "hello@acme.example"}),
         "/api/crm/acme/settings")
    signup = _contact(runtime, name="Sam Signup", email="sam@example.com",
                      flow="customers", stage="lead")
    investor = _contact(runtime, name="Ivy Investor", email="ivy@fund.example",
                        flow="investment", stage="sourced")

    refused = call(runtime.use("POST", body={"contactId": signup}),
                   "/api/crm/acme/decks")
    assert refused["status"] == 409
    assert refused["data"]["error"] == "deck_needs_investor_pipeline"
    assert refused["data"]["flow"] == "customers"
    assert runtime.audits[-1][1]["outcome"] == "denied"
    assert runtime.db.execute("SELECT COUNT(*) FROM crm_deck_links").fetchone()[0] == 0

    made = call(runtime.use("POST", body={"contactId": investor}), "/api/crm/acme/decks")
    assert made["status"] == 201
    link = made["data"]["link"]["url"]

    by_id = call(runtime.use("POST", body={
        "contactId": signup, "subject": "Our deck", "text": "Have a look: " + link,
    }), "/api/crm/acme/emails/send")
    by_address = call(runtime.use("POST", body={
        "to": "SAM@example.com", "subject": "Our deck", "text": "Have a look: " + link,
    }), "/api/crm/acme/emails/send")
    outreach = _outreach_send(runtime, to="sam@example.com", template="investor")
    assert [item["status"] for item in (by_id, by_address, outreach)] == [409, 409, 409]
    assert runtime.mailtrap_calls == []

    assert call(runtime.use("POST", body={
        "contactId": investor, "subject": "Our deck", "text": "Have a look: " + link,
    }), "/api/crm/acme/emails/send")["status"] == 201
    assert _outreach_send(runtime, to="new@fund.example", template="investor")["status"] == 200
    assert call(runtime.use("POST", body={
        "contactId": signup, "subject": "Welcome", "text": "Glad you joined",
    }), "/api/crm/acme/emails/send")["status"] == 201
    assert len(runtime.mailtrap_calls) == 3


def test_a_bulk_edit_adds_and_removes_tags_on_every_contact():
    runtime = Runtime()
    ids = [_contact(runtime, name=name, tags=["hackathon", "cold"])
           for name in ("Ada", "Grace")]
    moved = call(runtime.use("POST", body={"edits": [
        {"id": contact_id, "addTags": ["Priority", "hackathon"],
         "removeTags": ["cold"], "owner": "charlie"} for contact_id in ids]}),
        "/api/crm/acme/contacts/bulk")
    assert moved["status"] == 200
    for contact in moved["data"]["contacts"]:
        assert contact["tags"] == ["hackathon", "priority"]
        assert contact["owner"] == "charlie"


def test_possible_duplicates_are_grouped_by_what_they_share():
    runtime = Runtime()
    personal = _contact(runtime, name="Zach Example", email="zach@home.example",
                        github="https://github.com/zachx")
    work = _contact(runtime, name="Zach Example", email="zach@work.example",
                    github="zachx")
    _contact(runtime, name="", email="deals@fund.example")
    _contact(runtime, name="Pat Partner", email="pat@fund.example",
             fields={"other_emails": "deals@fund.example"})
    mastodon = _contact(runtime, name="Mo", mastodon="@mo@example.social")
    profile = _contact(runtime, name="Mo Example", website="https://example.social/@mo")
    _contact(runtime, name="Lone Person", email="lone@example.com")
    _contact(runtime, name="Madonna")
    _contact(runtime, name="Madonna")

    found = call(runtime.use("GET", "charlie"), "/api/crm/acme/contacts/duplicates")
    assert found["status"] == 200
    groups = found["data"]["groups"]
    assert {"contacts": [personal, work], "signals": ["profile", "name"]} in groups
    assert {"contacts": [mastodon, profile], "signals": ["handle"]} in groups
    assert len(groups) == 2
    assert call(runtime.use("POST"), "/api/crm/acme/contacts/duplicates")["status"] == 405


def test_a_link_without_an_organization_resolves_to_where_the_caller_can_open_it():
    runtime = Runtime()
    open_path = "/api/crm/-/open/"
    shared = call(runtime.use("POST", body={
        "name": "Bob's users", "flow": "partnerships", "visibility": "private",
        "sharedWith": ["bob"], "shareAccess": "edit"}),
        "/api/crm/acme/views")["data"]["view"]["id"]
    team = call(runtime.use("POST", body={"name": "Team"}),
                "/api/crm/acme/views")["data"]["view"]["id"]
    contact = _contact(runtime, name="Ada", email="ada@example.com")

    assert call(runtime.use("GET"), open_path + shared)["data"] == {
        "ok": True, "kind": "list", "organization": "acme"}
    assert call(runtime.use("GET", "bob"), open_path + shared)["data"] == {
        "ok": True, "kind": "list", "shared": True}
    assert call(runtime.use("GET", "charlie"), open_path + team)["data"][
        "organization"] == "acme"
    assert call(runtime.use("GET", "charlie"), open_path + shared)["status"] == 404
    assert call(runtime.use("GET", "bob"), open_path + team)["status"] == 404

    assert call(runtime.use("GET", "charlie"), open_path + contact)["data"] == {
        "ok": True, "kind": "contact", "organization": "acme"}
    assert call(runtime.use("GET", "bob"), open_path + contact)["status"] == 404
    assert call(runtime.use("GET"), open_path + "f" * 32)["status"] == 404
    assert call(runtime.use("POST"), open_path + shared)["status"] == 405
    assert call(runtime.use("GET", "nobody"), open_path + shared)["status"] == 401


class EvidenceRuntime(Runtime):
    def __init__(self, admins=("alice",)):
        super().__init__()
        self.admins = set(admins)
        self.evidence_calls = []
        active = {"tier": "pro", "since": 1, "until": 9, "active": True}
        self.found = {
            "payer@example.com": {"account": "payer", "signedUpAt": 5, "paid": active,
                                  "promotion": None, "trial": None},
            "trial@example.com": {"account": "trialist", "signedUpAt": 6, "paid": None,
                                  "promotion": None,
                                  "trial": {"startedAt": 1, "endsAt": 9, "active": True}},
            "plain@example.com": {"account": "plain", "signedUpAt": 7, "paid": None,
                                  "promotion": None, "trial": None},
            "work@example.com": {"account": "worker", "signedUpAt": 8, "paid": active,
                                 "promotion": None, "trial": None},
        }

    async def account_evidence(self, actor, emails):
        self.evidence_calls.append(list(emails))
        if actor not in self.admins:
            return None
        return {email: dict(self.found[email]) for email in emails if email in self.found}


def test_forkmesh_account_evidence_proves_customers_and_suggests_their_stage():
    runtime = EvidenceRuntime()
    ids = {
        "payer": _contact(runtime, name="Payer", email="payer@example.com",
                          flow="customers", stage="lead"),
        "trialist": _contact(runtime, name="Trialist", email="trial@example.com",
                             flow="customers", stage="lead"),
        "plain": _contact(runtime, name="Plain", email="plain@example.com",
                          flow="customers", stage="lead"),
        "already": _contact(runtime, name="Already", email="other@example.com",
                            flow="customers", stage="customer",
                            fields={"work_email": "work@example.com"}),
        "investor": _contact(runtime, name="Ivy", email="ivy@fund.example",
                             flow="investment", stage="sourced"),
        "nameless": _contact(runtime, name="No Address"),
    }
    path = "/api/crm/acme/contacts/evidence"
    assert call(runtime.use("POST", "charlie", {}), path)["status"] == 403
    first = call(runtime.use("POST", body={}), path)
    assert first["status"] == 200
    assert {key: first["data"][key] for key in (
        "checked", "accounts", "paying", "trial", "ahead", "changed")} == {
        "checked": 5, "accounts": 4, "paying": 2, "trial": 1, "ahead": 2, "changed": 5}
    assert runtime.audits[-1][0][1] == "crm.contacts.account_evidence"

    listed = {item["id"]: item for item in call(
        runtime.use("GET"), "/api/crm/acme/contacts")["data"]["contacts"]}
    payer = listed[ids["payer"]]["account"]
    assert payer["account"] == "payer" and payer["paid"]["active"]
    assert payer["suggestedStage"] == "customer" and payer["email"] == "payer@example.com"
    assert listed[ids["payer"]]["stage"] == "lead"
    assert listed[ids["trialist"]]["account"]["suggestedStage"] == "trial"
    assert listed[ids["plain"]]["account"]["suggestedStage"] == ""
    already = listed[ids["already"]]["account"]
    assert already["account"] == "worker" and already["email"] == "work@example.com"
    assert already["suggestedStage"] == ""
    assert listed[ids["investor"]]["account"]["account"] == ""
    assert listed[ids["nameless"]]["account"] == {}

    again = call(runtime.use("POST", body={"ids": [ids["payer"]]}), path)["data"]
    assert (again["checked"], again["changed"]) == (1, 0)
    assert runtime.evidence_calls[-1] == ["payer@example.com"]

    outsider = EvidenceRuntime(admins=())
    _contact(outsider, name="Payer", email="payer@example.com")
    assert call(outsider.use("POST", body={}), path)["status"] == 403
    assert call(Runtime().use("POST", body={}), path)["status"] == 403

    sheet = call(runtime.use("POST", body={
        "name": "Partner", "flow": "customers", "sharedWith": ["bob"]}),
        "/api/crm/acme/views")["data"]["view"]["id"]
    shared = call(runtime.use("GET", "bob"), f"/api/crm/-/shared/{sheet}")["data"]
    assert shared["contacts"] and all("account" not in row for row in shared["contacts"])


def _history(runtime, view_id, actor="alice"):
    return call(runtime.use("GET", actor, query={"view": view_id}),
                "/api/crm/acme/views/activity")


def test_a_list_keeps_a_history_of_who_opened_it_and_what_they_changed():
    runtime = Runtime()
    prospects = _sheet(runtime, "Prospects")
    partners = _sheet(runtime, "Partners")
    seen = call(runtime.use("POST", "charlie", {"view": prospects}),
                "/api/crm/acme/views/activity")
    assert seen["data"] == {"ok": True}
    call(runtime.use("POST", "charlie", {"view": prospects}),
         "/api/crm/acme/views/activity")
    contact = call(runtime.use("POST", body={
        "name": "Ada Lovelace", "views": [prospects]}),
        "/api/crm/acme/contacts")["data"]["contact"]
    path = "/api/crm/acme/contacts/" + contact["id"]
    call(runtime.use("PATCH", "charlie", {"stage": "customer", "notes": "Met"}), path)
    call(runtime.use("PATCH", body={"setViews": [partners]}), path)
    call(runtime.use("PATCH", body={"name": "Prospects 2026"}),
         "/api/crm/acme/views/" + prospects)

    entries = _history(runtime, prospects)["data"]["entries"]
    assert [(item["actor"], item["action"]) for item in entries] == [
        ("alice", "list.updated"),
        ("alice", "contact.removed"),
        ("charlie", "contact.updated"),
        ("alice", "contact.added"),
        ("charlie", "list.viewed"),
        ("alice", "list.created"),
    ]
    texts = [item["text"] for item in entries]
    assert texts[0] == "renamed it from “Prospects” to “Prospects 2026”"
    assert texts[1] == "took Ada Lovelace off the list"
    assert texts[2] == "edited Ada Lovelace (Stage, Notes)"
    assert texts[3] == "added Ada Lovelace"
    assert texts[4] == "opened the list"
    stored = runtime.db.execute(
        "SELECT data FROM crm_list_activity WHERE action='contact.added'").fetchone()
    assert stored["data"].startswith("sealed:")

    call(runtime.use("DELETE"), path)
    partner_actions = [item["action"] for item in
                       _history(runtime, partners)["data"]["entries"]]
    assert partner_actions[:2] == ["contact.deleted", "contact.added"]


def test_list_history_is_only_readable_where_the_list_is():
    runtime = Runtime()
    private = call(runtime.use("POST", body={
        "name": "Board", "visibility": "private"}),
        "/api/crm/acme/views")["data"]["view"]["id"]
    assert _history(runtime, private, "charlie")["status"] == 404
    assert call(runtime.use("POST", "charlie", {"view": private}),
                "/api/crm/acme/views/activity")["status"] == 404
    assert _history(runtime, private, "bob")["status"] == 404
    assert _history(runtime, "0" * 32)["status"] == 404
    assert [item["action"] for item in
            _history(runtime, private)["data"]["entries"]] == ["list.created"]


def test_bulk_moves_land_in_list_history_as_one_entry_and_go_with_the_list():
    runtime = Runtime()
    leads = _sheet(runtime, "Leads")
    ids = [call(runtime.use("POST", body={"name": name}),
                "/api/crm/acme/contacts")["data"]["contact"]["id"]
           for name in ("Ada", "Grace", "Linus")]
    call(runtime.use("POST", body={"edits": [
        {"id": item, "views": [leads]} for item in ids]}),
        "/api/crm/acme/contacts/bulk")
    entries = _history(runtime, leads)["data"]["entries"]
    assert entries[0]["text"] == "added 3 contacts: Ada, Grace, Linus"
    assert entries[0]["count"] == 3
    call(runtime.use("DELETE"), "/api/crm/acme/views/" + leads)
    assert runtime.db.execute(
        "SELECT COUNT(*) FROM crm_list_activity WHERE view_id=?",
        (leads,)).fetchone()[0] == 0


def test_zoho_sync_keeps_room_for_the_sent_folder_and_reports_outbound_and_won_deals():
    runtime = Runtime()
    call(runtime.use("PUT", body={"zoho": {
        "region": "com", "accountId": "1", "clientId": "c", "clientSecret": "s",
        "refreshToken": "r"}}), "/api/crm/acme/settings")
    runtime.zoho_messages = [
        {"providerMessageId": f"in-{index}", "subject": "Inbound",
         "sourceFolder": "Inbox", "receivedAt": runtime.clock - index}
        for index in range(10)] + [
        {"providerMessageId": f"out-{index}", "subject": "Outbound",
         "sourceFolder": "Sent", "receivedAt": runtime.clock - index}
        for index in range(3)]
    imported = call(runtime.use("POST", body={"limit": 6}), "/api/crm/acme/mail/import")
    assert imported["data"]["created"] == 6
    assert imported["data"]["outboundRecorded"] == 3
    ids = {item["providerMessageId"] for item in imported["data"]["mail"]}
    assert {"out-0", "out-1", "out-2"} <= ids
    emails = call(runtime.use("GET"), "/api/crm/acme")["data"]["emails"]
    assert {(item["externalId"], item["provider"], item["direction"])
            for item in emails} == {
                ("out-0", "zoho", "outbound"),
                ("out-1", "zoho", "outbound"),
                ("out-2", "zoho", "outbound"),
            }
    activity = call(runtime.use("GET"), "/api/crm/acme")["data"]["activity"]
    assert sum(next(item for item in activity["members"]
                    if item["name"] == "alice")["emails"]) == 3
    repeated = call(runtime.use("POST", body={"limit": 6}), "/api/crm/acme/mail/import")
    assert repeated["data"]["outboundRecorded"] == 0
    assert len(call(runtime.use("GET"), "/api/crm/acme")["data"]["emails"]) == 3
    call(runtime.use("POST", body={"name": "Won", "email": "won@example.com",
                                   "stage": "customer", "value": "$4,000"}),
         "/api/crm/acme/contacts")
    call(runtime.use("POST", body={"name": "Open", "email": "open@example.com",
                                   "stage": "qualified", "value": 900}),
         "/api/crm/acme/contacts")
    report = asyncio.run(crm.email_channel_report(runtime, "org:acme", 0))
    assert (report["status"], report["outboundCrm"], report["outboundZoho"],
            report["outbound"]) == ("ok", 3, 0, 3)
    assert (report["closedDeals"], report["closedValue"]) == (1, 4000)
    assert report["zoho"]["connected"] is True
    assert report["zoho"]["mailServer"] == "https://mail.zoho.com"
    assert asyncio.run(crm.email_channel_report(Runtime(installed=False), "org:acme", 0)) == {
        "status": "not_installed"}


def test_a_tracked_open_is_kept_when_a_later_delivery_event_arrives():
    runtime = Runtime()
    call(runtime.use("PUT", body={"fromEmail": "hello@acme.example"}),
         "/api/crm/acme/settings")
    sent = call(runtime.use("POST", body={
        "to": "customer@example.com", "subject": "Hello", "text": "Hi",
    }), "/api/crm/acme/emails/send")
    send_id = sent["data"]["email"]["id"]
    variables = runtime.mailtrap_calls[0][1]["custom_variables"]

    def status_after(event, at):
        asyncio.run(crm.mailtrap_webhook_event(
            runtime, {"custom_variables": variables, "message_id": "m-1"},
            event, at))
        return runtime.db.execute(
            "SELECT status FROM crm_email_sends WHERE send_id=?",
            (send_id,)).fetchone()[0]

    assert status_after("open", 10) == "open"
    assert status_after("delivery", 11) == "open"
    assert status_after("click", 12) == "click"
    assert status_after("open", 13) == "click"
    assert status_after("bounce", 14) == "bounce"


def test_every_tracked_email_rings_a_celebration_on_the_senders_devices():
    runtime = Runtime()
    linked = call(runtime.use("POST", body={
        "provider": "gmail", "messageId": "FMfcgzQbdrKvLxmgbXcSMSbzfXcxMvRj",
        "direction": "outbound", "contactEmail": "ada@example.com",
        "contactName": "Ada Lovelace", "subject": "Engines",
    }), "/api/crm/acme/emails/link")
    assert linked["status"] == 201
    ping = runtime.pings[-1]
    assert (ping["recipient"], ping["kind"]) == ("alice", "plugin_activity")
    assert ping["title"] == "🎉 Email tracked in the CRM"
    assert ping["body"] == "“Engines” with Ada Lovelace"
    assert ping["meta"] == {"celebrate": "crm-email-tracked"}
    assert ping["dedupe"] == "crm-email-tracked:" + linked["data"]["email"]["id"]

    relinked = call(runtime.use("POST", body={
        "provider": "gmail", "messageId": "FMfcgzQbdrKvLxmgbXcSMSbzfXcxMvRj",
        "contactEmail": "ada@example.com", "subject": "Engines (edited)",
    }), "/api/crm/acme/emails/link")
    assert relinked["data"]["relinked"] is True
    assert len(runtime.pings) == 1

    recorded = call(runtime.use("POST", body={
        "to": "grace@example.com", "subject": "Campaign follow-up",
        "externalId": "provider-77",
    }), "/api/crm/acme/emails")
    assert recorded["status"] == 201
    assert runtime.pings[-1]["body"] == "“Campaign follow-up” with grace@example.com"

    sent = call(runtime.use("POST", body={
        "to": "linus@example.com", "subject": "Hello",
        "text": "A one-off note", "fromEmail": "hello@acme.example",
    }), "/api/crm/acme/emails/send")
    assert sent["status"] == 201
    assert runtime.pings[-1]["meta"] == {"celebrate": "crm-email-tracked"}
    assert len(runtime.pings) == 3

    runtime.send_mailtrap = lambda *_: _refused()
    refused = call(runtime.use("POST", body={
        "to": "linus@example.com", "subject": "Hello again",
        "text": "A one-off note", "fromEmail": "hello@acme.example",
    }), "/api/crm/acme/emails/send")
    assert refused["status"] == 502
    assert len(runtime.pings) == 3


def test_a_linked_mastodon_post_pings_who_it_added_to_the_crm(monkeypatch):
    monkeypatch.setattr(crm, "_activity_clock", lambda: 5_000_000)
    crm._ACTIVITY_SEEN.clear()
    runtime = Runtime()
    post = {"provider": "mastodon", "messageId": "113489000000000001",
            "direction": "outbound", "handle": "@grace@hachyderm.io",
            "account": "forkmesh@mastodon.social", "subject": "Shipping today",
            "providerUrl": "https://mastodon.social/@forkmesh/113489000000000001"}
    added = call(runtime.use("POST", "charlie", post), "/api/crm/acme/emails/link")
    assert added["status"] == 201 and added["data"]["contactCreated"] is True
    assert added["data"]["contact"]["mastodon"] == "@grace@hachyderm.io"
    assert [(ping["recipient"], ping["title"], ping["body"]) for ping in runtime.pings] == [
        ("charlie", "🎉 New CRM contact from Mastodon",
         "“Shipping today” with @grace@hachyderm.io"),
        ("alice", "Mastodon post linked in the CRM", "By charlie in acme: Shipping today")]

    runtime.pings.clear()
    again = call(runtime.use("POST", "charlie", post), "/api/crm/acme/emails/link")
    assert again["data"]["relinked"] is True
    assert [ping["recipient"] for ping in runtime.pings] == ["alice"]

    runtime.pings.clear()
    later = call(runtime.use("POST", "charlie", dict(
        post, messageId="113489000000000002",
        providerUrl="https://mastodon.social/@forkmesh/113489000000000002")),
        "/api/crm/acme/emails/link")
    assert later["data"]["contactCreated"] is False
    assert runtime.pings[0]["title"] == "🎉 Mastodon post tracked in the CRM"


def test_crm_activity_pings_the_member_and_the_organization_managers(monkeypatch):
    monkeypatch.setattr(crm, "_activity_clock", lambda: 5_000_000)
    crm._ACTIVITY_SEEN.clear()
    runtime = Runtime()
    created = call(runtime.use("POST", "charlie", {"name": "Ada Lovelace"}),
                   "/api/crm/acme/contacts")
    assert created["status"] == 201
    assert [(ping["recipient"], ping["title"], ping["body"], ping["href"])
            for ping in runtime.pings] == [
        ("charlie", "New CRM contact", "By you in acme: Ada Lovelace", "/dashboard/crm"),
        ("alice", "New CRM contact", "By charlie in acme: Ada Lovelace", "/dashboard/crm")]

    runtime.pings.clear()
    contact_id = created["data"]["contact"]["id"]
    for company in ("Analytical Engines", "Difference Engines"):
        assert call(runtime.use("PATCH", "charlie", {"company": company}),
                    f"/api/crm/acme/contacts/{contact_id}")["status"] == 200
    assert [ping["push"] for ping in runtime.pings] == [True, True, False, False]
    assert len({ping["dedupe"] for ping in runtime.pings}) == 1

    runtime.pings.clear()
    sent = call(runtime.use("POST", "charlie", {
        "to": "linus@example.com", "subject": "Hello",
        "text": "A one-off note", "fromEmail": "hello@acme.example",
    }), "/api/crm/acme/emails/send")
    assert sent["status"] == 201
    assert [(ping["recipient"], ping["title"]) for ping in runtime.pings] == [
        ("charlie", "🎉 Email tracked in the CRM"),
        ("alice", "Email sent from the CRM")]
    assert runtime.pings[-1]["body"] == "By charlie in acme: Hello"

    runtime.pings.clear()
    assert call(runtime.use("PUT", "charlie", {"tableViews": {}}),
                "/api/crm/acme/settings")["status"] == 200
    assert call(runtime.use("PATCH", "charlie", {"company": "x" * 5000}),
                "/api/crm/acme/contacts/" + "f" * 32)["status"] == 404
    assert call(runtime.use("GET", "charlie"), "/api/crm/acme/contacts")["status"] == 200
    assert runtime.pings == []


async def _refused():
    return {"accepted": False, "messageId": "", "error": "rejected"}


class MastodonRuntime(Runtime):
    def __init__(self, statuses=(), single=None, status=200):
        super().__init__()
        self.statuses = list(statuses)
        self.single = single or {}
        self.mastodon_status = status

    async def outbound_json(self, url, method="GET", headers=None, body=""):
        self.outbound_calls.append((url, method, dict(headers or {}), body))
        parsed = urlparse(url)
        query = parse_qs(parsed.query)
        if parsed.path == "/api/v1/accounts/lookup":
            return {"ok": True, "status": 200, "data": {"id": "42"}}
        if parsed.path == "/api/v1/accounts/42/statuses":
            if self.mastodon_status != 200:
                return {"ok": True, "status": self.mastodon_status, "data": {}}
            items = sorted(self.statuses, key=lambda item: -int(item["id"]))
            if "min_id" in query:
                items = [item for item in items if int(item["id"]) > int(query["min_id"][0])]
            if "max_id" in query:
                items = [item for item in items if int(item["id"]) < int(query["max_id"][0])]
            return {"ok": True, "status": 200, "data": items[:int(query["limit"][0])]}
        if parsed.path.startswith("/api/v1/statuses/"):
            found = self.single.get(parsed.path.rsplit("/", 1)[1])
            return ({"ok": True, "status": 200, "data": found} if found
                    else {"ok": True, "status": 404, "data": {"error": "Not Found"}})
        return {"ok": False, "status": 404, "error": "unexpected_test_url"}


def _toot(status_id, *mentions, visibility="direct"):
    return {
        "id": str(status_id), "created_at": "2026-10-14T21:57:27.736Z",
        "visibility": visibility, "spoiler_text": "",
        "url": f"https://mastodon.social/@forkmesh/{status_id}",
        "content": "<p><span class=\"h-card\">@mmasnick</span> Hi &amp; hello</p><p>Second</p>",
        "account": {"acct": "forkmesh"},
        "mentions": [{"acct": acct, "url": ""} for acct in mentions],
    }


def _connect_mastodon(runtime):
    saved = call(runtime.use("PUT", body={"mastodon": {
        "account": "@forkmesh@mastodon.social", "token": "secret-token"}}),
        "/api/crm/acme/settings")
    assert saved["status"] == 200
    return saved["data"]["settings"]["mastodon"]


def test_mastodon_settings_keep_the_token_server_side_and_check_the_instance():
    runtime = MastodonRuntime()
    assert call(runtime.use("GET"), "/api/crm/acme/settings")["data"]["settings"][
        "mastodon"] == {"account": "", "connected": False, "lastSyncAt": 0, "lastError": ""}
    assert _connect_mastodon(runtime) == {
        "account": "forkmesh@mastodon.social", "connected": True,
        "lastSyncAt": 0, "lastError": ""}
    assert "secret-token" not in json.dumps(
        call(runtime.use("GET"), "/api/crm/acme")["data"])
    bad = call(runtime.use("PUT", body={"mastodon": {"account": "@me@example.org"}}),
               "/api/crm/acme/settings")
    assert (bad["status"], bad["data"]["error"]) == (400, "invalid_mastodon_account")
    member = call(runtime.use("PUT", actor="charlie", body={"mastodon": {
        "account": "@x@mastodon.social", "token": "t"}}), "/api/crm/acme/settings")
    assert member["status"] == 403
    gone = call(runtime.use("PUT", body={"mastodonDisconnect": True}),
                "/api/crm/acme/settings")
    assert gone["data"]["settings"]["mastodon"]["connected"] is False


def test_toot_sync_logs_sent_toots_that_mention_known_contacts():
    runtime = MastodonRuntime([
        _toot(117441498168443197, "mmasnick@mastodon.social"),
        _toot(117441498168443100, "stranger@hachyderm.io"),
        _toot(117441498168443300, "mmasnick@mastodon.social", "ada@hachyderm.io",
              visibility="public"),
    ])
    assert call(runtime.use("POST", body={}), "/api/crm/acme/emails/toots")[
        "data"]["error"] == "mastodon_not_connected"
    _connect_mastodon(runtime)
    mike = call(runtime.use("POST", body={
        "name": "Mike Masnick", "mastodon": "@mmasnick@mastodon.social"}),
        "/api/crm/acme/contacts")["data"]["contact"]
    ada = call(runtime.use("POST", body={
        "name": "Ada", "website": "https://hachyderm.io/@ada"}),
        "/api/crm/acme/contacts")["data"]["contact"]

    synced = call(runtime.use("POST", actor="charlie", body={"auto": True}),
                  "/api/crm/acme/emails/toots")
    assert synced["status"] == 200
    assert (synced["data"]["checked"], synced["data"]["logged"]) == (3, 3)
    assert synced["data"]["contacts"] == [mike["id"], ada["id"]]
    statuses_call = next(item for item in runtime.outbound_calls
                         if "/statuses?" in item[0])
    assert statuses_call[2]["authorization"] == "Bearer secret-token"
    rows = call(runtime.use("GET"), "/api/crm/acme/emails")["data"]["emails"]
    toots = {row["externalId"]: row for row in rows}
    assert set(toots) == {"117441498168443197", "117441498168443300",
                          "117441498168443300/ada@hachyderm.io"}
    first = toots["117441498168443197"]
    assert (first["provider"], first["direction"], first["contactId"]) == (
        "mastodon", "outbound", mike["id"])
    assert first["providerUrl"] == "https://mastodon.social/@forkmesh/117441498168443197"
    assert first["subject"] == "@mmasnick Hi & hello Second"
    assert toots["117441498168443300/ada@hachyderm.io"]["contactId"] == ada["id"]
    contact = call(runtime.use("GET"), f"/api/crm/acme/contacts/{mike['id']}")["data"]["contact"]
    assert contact["lastContactedAt"] == 1792015047736
    assert not [ping for ping in runtime.pings if ping.get("meta")]
    synced_pings = [ping for ping in runtime.pings
                    if ping["title"] == "3 Mastodon posts logged in the CRM"]
    assert [(ping["recipient"], ping["body"]) for ping in synced_pings] == [
        ("charlie", "By you in acme"), ("alice", "By charlie in acme")]

    again = call(runtime.use("POST", body={"auto": True}), "/api/crm/acme/emails/toots")
    assert again["data"]["skipped"] is True
    runtime.statuses.append(_toot(117441498168443400, "mmasnick@mastodon.social"))
    runtime.outbound_calls.clear()
    now = call(runtime.use("POST", body={}), "/api/crm/acme/emails/toots")
    assert (now["data"]["checked"], now["data"]["logged"]) == (1, 1)
    assert "min_id=117441498168443300" in next(
        item[0] for item in runtime.outbound_calls if "/statuses?" in item[0])


def test_toot_sync_reports_a_rejected_token():
    runtime = MastodonRuntime(status=401)
    _connect_mastodon(runtime)
    failed = call(runtime.use("POST", body={}), "/api/crm/acme/emails/toots")
    assert (failed["status"], failed["data"]["error"]) == (502, "mastodon_token_rejected")
    assert call(runtime.use("GET"), "/api/crm/acme/settings")["data"]["settings"][
        "mastodon"]["lastError"] == "mastodon_token_rejected"


def test_a_toot_is_logged_by_its_link():
    runtime = MastodonRuntime(single={
        "117441498168443300": _toot(117441498168443300, "mmasnick@mastodon.social")})
    mike = call(runtime.use("POST", body={
        "name": "Mike Masnick", "mastodon": "@mmasnick@mastodon.social"}),
        "/api/crm/acme/contacts")["data"]["contact"]
    url = "https://mastodon.social/@forkmesh/117441498168443197"
    logged = call(runtime.use("POST", body={"url": url, "contactId": mike["id"]}),
                  "/api/crm/acme/emails/toot")
    assert logged["status"] == 201
    email = logged["data"]["email"]
    assert (email["provider"], email["direction"], email["externalId"]) == (
        "mastodon", "outbound", "117441498168443197")
    assert email["providerUrl"] == url
    assert logged["data"]["fetched"] is False
    assert logged["data"]["contact"]["lastContactedAt"] == 1792015047736
    assert call(runtime.use("POST", body={"url": url, "contactId": mike["id"]}),
                "/api/crm/acme/emails/toot")["status"] == 200

    found = call(runtime.use("POST", body={
        "url": "https://mastodon.social/@forkmesh/117441498168443300"}),
        "/api/crm/acme/emails/toot")
    assert found["status"] == 201 and found["data"]["fetched"] is True
    assert found["data"]["contact"]["id"] == mike["id"]
    assert found["data"]["email"]["subject"] == "@mmasnick Hi & hello Second"

    reply = call(runtime.use("POST", body={
        "url": "https://mastodon.social/@mmasnick/117441498168449999",
        "contactId": mike["id"]}), "/api/crm/acme/emails/toot")
    assert reply["data"]["email"]["direction"] == "inbound"
    assert call(runtime.use("POST", body={"url": "https://example.com/x"}),
                "/api/crm/acme/emails/toot")["data"]["error"] == "invalid_toot_url"
    assert call(runtime.use("POST", body={
        "url": "https://mastodon.social/@forkmesh/117441498168440000"}),
        "/api/crm/acme/emails/toot")["data"]["error"] == "toot_contact_not_found"


def test_crm_mastodon_hosts_match_the_manifest_origins():
    manifest = json.loads((CRM_PACKAGE / "manifest.json").read_text())
    origins = set(manifest["worker"]["outboundOrigins"])
    assert {"https://" + host for host in crm.MASTODON_HOSTS} <= origins
