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
import invoices


DAY = 24 * 60 * 60 * 1000
CONTACT = "c" * 32


class Runtime:
    def __init__(self, installed=True, crm=True):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.db.execute("CREATE TABLE schema_meta(k TEXT PRIMARY KEY,v TEXT NOT NULL)")
        self.db.executescript(
            sorted((APP / "migrations").glob("0*_org_plugin_installs.sql"))[-1]
            .read_text(encoding="utf-8")
        )
        self.db.executescript("""
          CREATE TABLE orgs(org_bi TEXT PRIMARY KEY,name TEXT,data TEXT,created_at INTEGER);
          CREATE TABLE org_members(org_bi TEXT,member_bi TEXT,role TEXT,name TEXT,
            created_at INTEGER,PRIMARY KEY(org_bi,member_bi));
          INSERT INTO orgs VALUES('org:acme','acme','sealed:{}',1);
          INSERT INTO org_members VALUES('org:acme','bi:alice','owner','alice',1);
          INSERT INTO org_members VALUES('org:acme','bi:charlie','member','charlie',1);
          CREATE TABLE crm_contacts(contact_id TEXT PRIMARY KEY,org_bi TEXT,updated_at INTEGER,
            data TEXT);
          CREATE TABLE crm_settings(org_bi TEXT PRIMARY KEY,data TEXT);
        """)
        if installed:
            self.install("invoices")
        if crm:
            self.install("crm")
            self.db.execute(
                "INSERT INTO crm_contacts VALUES(?,?,?,?)",
                (CONTACT, "org:acme", 1, "sealed:" + json.dumps({
                    "name": "Grace Hopper", "email": "grace@navy.example",
                    "company": "Navy Labs", "stage": "customer"})))
            self.db.execute(
                "INSERT INTO crm_settings VALUES(?,?)",
                ("org:acme", "sealed:" + json.dumps({
                    "fromEmail": "billing@acme.example", "fromName": "Acme Billing"})))
        self.actor = "alice"
        self.method_value = "GET"
        self.body = {}
        self.clock = 2_000_000_000_000
        self.ids = 0
        self.audits = []
        self.mails = []
        self.pings = []
        self.mail_ok = True
        self.agent = ""

    def install(self, plugin):
        self.db.execute(
            "INSERT INTO org_plugin_installs VALUES(?,?,?,?,?,?)",
            ("org:acme", plugin, "acme", "bi:alice", "alice", 1),
        )

    def use(self, method, actor="alice", body=None):
        self.method_value, self.actor, self.body = method, actor, body or {}
        return self

    def method(self): return self.method_value
    def same_origin(self): return True
    def now(self): self.clock += 1; return self.clock
    def new_id(self): self.ids += 1; return f"{self.ids:032x}"
    def request_origin(self): return "https://app.forkmesh.com"
    def request_visitor(self): return {"ip": "", "agent": self.agent}
    def package_secret(self, _name): return ""
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
    async def blind(self, value): return hashlib.sha256(value.encode()).hexdigest()
    async def seal(self, value): return "sealed:" + json.dumps(value, sort_keys=True)
    async def open(self, value): return json.loads(str(value).removeprefix("sealed:"))
    async def notify(self, recipient, kind, title, body="", href="", dedupe="", **_):
        self.pings.append({"to": recipient, "kind": kind, "title": title, "href": href,
                           "dedupe": dedupe})
        return True
    async def send_mailtrap(self, token, payload):
        self.mails.append((token, payload))
        if not self.mail_ok:
            return {"accepted": False, "error": "mailtrap_http_500"}
        return {"accepted": True, "messageId": "msg-1", "error": ""}
    async def d1_all(self, sql, *args):
        return [dict(row) for row in self.db.execute(sql, args).fetchall()]
    async def d1_first(self, sql, *args):
        row = self.db.execute(sql, args).fetchone()
        return dict(row) if row else None
    async def d1_run(self, sql, *args):
        cursor = self.db.execute(sql, args); self.db.commit(); return cursor
    async def audit(self, *args, **kwargs): self.audits.append((args, kwargs))


def call(runtime, path):
    return asyncio.run(invoices.handle(runtime, path))


def create(runtime, **extra):
    body = {
        "contactId": CONTACT,
        "items": [
            {"description": "Design sprint", "quantity": 2, "unitAmount": 150000},
            {"description": "Hosting", "quantity": 1, "unitAmount": 9900},
        ],
        "taxRate": 1000,
        "paymentUrl": "https://pay.example/inv",
    }
    body.update(extra)
    response = call(runtime.use("POST", body=body), "/api/invoices/acme/items")
    assert response["status"] == 201, response
    return response["data"]["invoice"]


def test_invoice_links_crm_contact_numbers_and_seals_records():
    runtime = Runtime()
    root = call(runtime.use("GET"), "/api/invoices")
    assert root["data"]["organizations"] == [{"name": "acme", "role": "owner", "installed": True}]
    contacts = call(runtime.use("GET"), "/api/invoices/acme/contacts")
    assert contacts["data"]["contacts"][0]["email"] == "grace@navy.example"
    invoice = create(runtime)
    assert invoice["number"] == "INV-0001"
    assert invoice["status"] == "draft"
    assert invoice["contactId"] == CONTACT
    assert invoice["client"]["name"] == "Grace Hopper"
    assert invoice["client"]["company"] == "Navy Labs"
    assert (invoice["subtotal"], invoice["tax"], invoice["total"]) == (309900, 30990, 340890)
    assert invoice["dueAt"] - invoice["issuedAt"] == 30 * DAY
    assert invoice["publicUrl"] == ""
    stored = runtime.db.execute("SELECT data,total FROM invoice_records").fetchone()
    assert stored["data"].startswith("sealed:")
    assert stored["total"] == 340890
    second = create(runtime, contactId="")
    assert second["number"] == "INV-0002"
    assert second["contactId"] == ""
    taken = call(runtime.use("POST", body={"number": "INV-0001"}), "/api/invoices/acme/items")
    assert taken["status"] == 409
    missing = call(runtime.use("POST", body={"contactId": "d" * 32}), "/api/invoices/acme/items")
    assert missing["data"]["error"] == "crm_contact_not_found"


def test_send_emails_a_tracked_invoice_and_falls_back_to_crm_sender():
    runtime = Runtime()
    invoice = create(runtime)
    sent = call(runtime.use("POST", body={}), f"/api/invoices/acme/items/{invoice['id']}/send")
    assert sent["status"] == 201, sent
    token, payload = runtime.mails[-1]
    assert token == ""
    assert payload["to"] == [{"email": "grace@navy.example"}]
    assert payload["from"] == {"email": "billing@acme.example", "name": "Acme Billing"}
    assert payload["tracked"] is True
    assert payload["subject"] == "Invoice INV-0001 from acme"
    view = sent["data"]["invoice"]["publicUrl"]
    assert view.startswith("https://app.forkmesh.com/invoice?t=" + invoice["id"] + ".")
    assert view in payload["html"] and view in payload["text"]
    assert "/api/invoices/-/t/o/" + view.split("t=")[1] + ".png" in payload["html"]
    assert "<script" not in payload["html"]
    result = sent["data"]["invoice"]
    assert (result["status"], result["sentTo"], result["sentBy"]) == ("sent", "grace@navy.example", "alice")
    assert [event["kind"] for event in result["events"]] == ["sent", "created"]
    again = call(runtime.use("POST", body={"message": "Nudge"}),
                 f"/api/invoices/acme/items/{invoice['id']}/send")
    assert again["data"]["invoice"]["events"][0]["kind"] == "reminder"
    assert runtime.mails[-1][1]["subject"].startswith("Reminder: invoice INV-0001")
    assert "Nudge" in runtime.mails[-1][1]["text"]


def test_send_failure_and_missing_sender_are_reported():
    runtime = Runtime(crm=False)
    invoice = create(runtime, contactId="", client={"name": "Ada", "email": "ada@example.com"})
    no_sender = call(runtime.use("POST", body={}), f"/api/invoices/acme/items/{invoice['id']}/send")
    assert no_sender["data"]["error"] == "invalid_sender"
    call(runtime.use("PATCH", body={"fromEmail": "me@acme.example"}), "/api/invoices/acme/settings")
    runtime.mail_ok = False
    failed = call(runtime.use("POST", body={}), f"/api/invoices/acme/items/{invoice['id']}/send")
    assert failed["status"] == 502
    row = runtime.db.execute("SELECT status FROM invoice_records").fetchone()
    assert row["status"] == "draft"
    crm = call(runtime.use("GET"), "/api/invoices/acme/contacts")
    assert crm["status"] == 409


def test_open_pixel_view_and_pay_click_track_analytics_and_ping_once():
    runtime = Runtime()
    invoice = create(runtime)
    sent = call(runtime.use("POST", body={}), f"/api/invoices/acme/items/{invoice['id']}/send")
    token = sent["data"]["invoice"]["publicUrl"].split("t=")[1]
    early = call(runtime.use("GET", actor="nobody"), f"/api/invoices/-/t/o/{token}.png")
    assert early["status"] == 302
    assert runtime.db.execute("SELECT opens FROM invoice_records").fetchone()[0] == 0
    runtime.clock += 60_000
    runtime.agent = "Mozilla/5.0 (via ggpht.com GoogleImageProxy)"
    for _ in range(2):
        opened = call(runtime.use("GET", actor="nobody"), f"/api/invoices/-/t/o/{token}.png")
        assert opened["headers"]["location"] == invoices.TRACK_LOGO
    public = call(runtime.use("GET", actor="nobody"), f"/api/invoices/-/public/{token}")
    assert public["status"] == 200
    shown = public["data"]["invoice"]
    assert shown["number"] == "INV-0001"
    assert "email" not in shown["client"]
    assert shown["payUrl"].endswith(f"/api/invoices/-/t/c/{token}")
    assert "id" not in shown and "contactId" not in shown
    clicked = call(runtime.use("GET", actor="nobody"), f"/api/invoices/-/t/c/{token}")
    assert clicked["headers"]["location"] == "https://pay.example/inv"
    row = runtime.db.execute("SELECT opens,views,clicks,first_opened_at FROM invoice_records").fetchone()
    assert (row["opens"], row["views"], row["clicks"]) == (2, 1, 1)
    assert row["first_opened_at"] > 0
    titles = [ping["title"] for ping in runtime.pings]
    assert titles == [
        "📬 Grace Hopper opened invoice INV-0001",
        "👀 Grace Hopper viewed invoice INV-0001",
        "💳 Grace Hopper clicked pay on invoice INV-0001",
    ]
    assert all(ping["to"] == "alice" and ping["kind"] == "plugin_activity" for ping in runtime.pings)
    overview = call(runtime.use("GET"), "/api/invoices/acme")["data"]
    summary = overview["summary"]
    assert (summary["sent"], summary["opened"], summary["openRate"], summary["viewRate"]) == (1, 1, 100, 100)
    assert summary["outstanding"] == {"USD": 340890}
    kinds = [event["kind"] for event in overview["activity"]]
    assert kinds[:3] == ["clicked", "viewed", "opened"]
    assert overview["activity"][2]["client"] == "Gmail"
    assert overview["activity"][0]["number"] == "INV-0001"
    forged = call(runtime.use("GET", actor="nobody"), f"/api/invoices/-/public/{invoice['id']}.{'0' * 24}")
    assert forged["status"] == 404


def test_drafts_are_not_public_and_payments_settle_invoice():
    runtime = Runtime()
    invoice = create(runtime)
    token = asyncio.run(invoices._track_token(runtime, invoice["id"]))
    assert call(runtime.use("GET", actor="nobody"), f"/api/invoices/-/public/{token}")["status"] == 404
    call(runtime.use("POST", body={}), f"/api/invoices/acme/items/{invoice['id']}/send")
    partial = call(runtime.use("POST", body={"amount": 40890, "method": "Card"}),
                   f"/api/invoices/acme/items/{invoice['id']}/paid")
    assert partial["data"]["invoice"]["displayStatus"] == "partial"
    assert partial["data"]["invoice"]["balance"] == 300000
    paid = call(runtime.use("POST", body={}), f"/api/invoices/acme/items/{invoice['id']}/paid")
    result = paid["data"]["invoice"]
    assert (result["status"], result["balance"], result["paidAmount"]) == ("paid", 0, 340890)
    assert len(result["payments"]) == 2
    locked = call(runtime.use("PATCH", body={"notes": "late edit"}), f"/api/invoices/acme/items/{invoice['id']}")
    assert locked["data"]["error"] == "invoice_locked"
    public = call(runtime.use("GET", actor="nobody"), f"/api/invoices/-/public/{token}")["data"]["invoice"]
    assert (public["status"], public["payUrl"]) == ("paid", "")
    assert call(runtime.use("DELETE"), f"/api/invoices/acme/items/{invoice['id']}")["status"] == 409
    reopened = call(runtime.use("POST", body={}), f"/api/invoices/acme/items/{invoice['id']}/reopen")
    assert (reopened["data"]["invoice"]["status"], reopened["data"]["invoice"]["paidAmount"]) == ("sent", 0)
    call(runtime.use("POST", body={}), f"/api/invoices/acme/items/{invoice['id']}/void")
    deleted = call(runtime.use("DELETE"), f"/api/invoices/acme/items/{invoice['id']}")
    assert deleted["data"]["deleted"] is True
    assert runtime.db.execute("SELECT COUNT(*) FROM invoice_events").fetchone()[0] == 0


def test_crm_contact_card_lists_invoices_with_totals():
    runtime = Runtime()
    first = create(runtime)
    create(runtime, contactId="")
    call(runtime.use("POST", body={}), f"/api/invoices/acme/items/{first['id']}/send")
    call(runtime.use("POST", body={"amount": 40890}), f"/api/invoices/acme/items/{first['id']}/paid")
    card = call(runtime.use("GET", actor="charlie"), f"/api/invoices/acme/contacts/{CONTACT}")["data"]
    assert [item["number"] for item in card["invoices"]] == ["INV-0001"]
    assert card["totals"] == {"billed": {"USD": 340890}, "paid": {"USD": 40890},
                              "outstanding": {"USD": 300000}}


def test_overdue_sweep_pings_sender_once():
    runtime = Runtime()
    invoice = create(runtime, dueAt=runtime.clock + DAY)
    call(runtime.use("POST", body={}), f"/api/invoices/acme/items/{invoice['id']}/send")
    assert asyncio.run(invoices.deliver_reminders(runtime)) == {"delivered": 0}
    runtime.clock += 2 * DAY
    assert asyncio.run(invoices.deliver_reminders(runtime)) == {"delivered": 1}
    assert asyncio.run(invoices.deliver_reminders(runtime)) == {"delivered": 0}
    assert runtime.pings[-1]["title"] == "⏰ Invoice INV-0001 to Grace Hopper is overdue"
    overview = call(runtime.use("GET"), "/api/invoices/acme")["data"]
    assert overview["invoices"][0]["displayStatus"] == "overdue"
    assert overview["summary"]["counts"]["overdue"] == 1


def test_settings_need_admin_and_access_needs_install():
    runtime = Runtime()
    denied = call(runtime.use("PATCH", actor="charlie", body={"prefix": "X-"}), "/api/invoices/acme/settings")
    assert denied["status"] == 403
    saved = call(runtime.use("PATCH", body={"prefix": "ACME-", "nextNumber": 100, "taxRate": 2000,
                                             "paymentUrl": "http://insecure"}), "/api/invoices/acme/settings")
    assert saved["data"]["error"] == "invalid_payment_url"
    saved = call(runtime.use("PATCH", body={"prefix": "ACME-", "nextNumber": 100, "taxRate": 2000}),
                 "/api/invoices/acme/settings")
    assert saved["data"]["settings"]["prefix"] == "ACME-"
    invoice = create(runtime, taxRate=2000)
    assert invoice["number"] == "ACME-0100"
    bad_link = call(runtime.use("POST", body={"paymentUrl": "javascript:alert(1)"}), "/api/invoices/acme/items")
    assert bad_link["data"]["error"] == "invalid_payment_url"
    missing = Runtime(installed=False)
    response = call(missing.use("GET"), "/api/invoices/acme")
    assert response["status"] == 409
    assert response["data"]["canInstall"] is True
    assert call(missing.use("GET", actor="nobody"), "/api/invoices/-/public/" + "a" * 32 + "." + "b" * 24)["status"] == 404
    assert call(runtime.use("GET", actor="nobody"), "/api/invoices/acme")["status"] == 401


def test_lifecycle_exports_and_purges_all_rows():
    runtime = Runtime()
    invoice = create(runtime)
    call(runtime.use("POST", body={}), f"/api/invoices/acme/items/{invoice['id']}/send")
    exported = asyncio.run(invoices.export_org_data(runtime, "org:acme", "acme"))
    assert exported["format"] == "forkmesh-invoices-export-v1"
    assert exported["invoices"][0]["number"] == "INV-0001"
    assert [event["kind"] for event in exported["invoices"][0]["events"]] == ["sent", "created"]
    assert asyncio.run(invoices.org_has_data(runtime, "org:acme")) is True
    asyncio.run(invoices.purge_org_data(runtime, "org:acme"))
    assert asyncio.run(invoices.org_has_data(runtime, "org:acme")) is False
    for statement in invoices.DATA_SCHEMA_DOWN:
        runtime.db.execute(statement)
    assert not runtime.db.execute(
        "SELECT name FROM sqlite_master WHERE name LIKE 'invoice_%'").fetchall()


def test_package_owns_dashboard_public_page_and_crm_card_contracts():
    manifest = json.loads((PACKAGE / "manifest.json").read_text(encoding="utf-8"))
    page = (PACKAGE / "content/web/page.js").read_text(encoding="utf-8")
    view = (PACKAGE / "content/web/view.html").read_text(encoding="utf-8")
    public = (PACKAGE / "content/www/invoice.js").read_text(encoding="utf-8")
    crm_page = (REPO / "marketplace/packages/crm/content/web/page.js").read_text(encoding="utf-8")
    assert manifest["worker"] == {"module": "invoices", "api": "/api/invoices"}
    assert manifest["dataLifecycle"] == {"scope": "organization"}
    assert manifest["install"] == {"bundled": True, "default": False}
    assert manifest["dependencies"] == []
    routes = {item["socket"]: item["config"]["route"] for item in manifest["contributions"]}
    assert routes == {"dashboard.page": "/dashboard/invoices", "www.page": "/invoice"}
    assert 'registerPage("invoices"' in page
    assert view.count("fm-btn-cta") == 1
    for marker in ("data-invoice-install", "data-invoice-uninstall", "data-invoice-export"):
        assert marker in view
    assert "/api/invoices/-/public/" in public
    assert "innerHTML" not in public
    assert "/api/invoices/" in crm_page and "data-crm-detail-invoices" in crm_page
    assert "http://" not in page + public
