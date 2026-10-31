from __future__ import annotations

import asyncio
import importlib.util
import json
import sqlite3
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
MIGRATION = ROOT / "migrations" / "0262_affiliates_and_plugin_race.sql"
DOWN = ROOT / "migrations" / "down" / "0262_affiliates_and_plugin_race.sql"

sys.path.insert(0, str(SRC))
import affiliates  # noqa: E402


def _module(name):
    spec = importlib.util.spec_from_file_location(name, SRC / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


urls = _module("urls")
static_routes = _module("static_routes")

DAY = 24 * 60 * 60 * 1000


class Response:
    def __init__(self, data, status):
        self.data = data
        self.status = status


class Runtime:
    def __init__(self, pipeline_status="created"):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.db.executescript(
            "CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT NOT NULL);")
        self.db.executescript(MIGRATION.read_text(encoding="utf-8"))
        self._method = "POST"
        self._body = {}
        self._now = 1_800_000_000_000
        self._ids = 0
        self.same = True
        self.account = ("acct-ana", {"name": "ana", "email": "ana@example.org",
                                     "email_verified": True})
        self.admins = {"boss"}
        self.pipeline_status = pipeline_status
        self.leads = []
        self.audits = []

    def call(self, path, body=None, method="POST"):
        self._method = method
        self._body = body or {}
        return asyncio.run(affiliates.handle(self, path))

    def method(self):
        return self._method

    def same_origin(self):
        return self.same

    def now(self):
        return self._now

    def new_id(self):
        self._ids += 1
        return "%08x-0000-4000-8000-%012x" % (self._ids, self._ids)

    def response(self, data, status=200, cache_control=None, extra_headers=None):
        return Response(json.loads(json.dumps(data)), status)

    async def ensure_schema(self):
        return None

    async def json_body(self, limit):
        return dict(self._body), ""

    async def session(self, data=None):
        if not self.account:
            return "", None
        return self.account

    async def is_admin(self, name):
        return name in self.admins

    async def blind(self, value):
        return "bi:" + str(value)

    async def seal(self, value):
        return json.dumps(value)

    async def open(self, value):
        try:
            return json.loads(value)
        except Exception:
            return None

    async def d1_all(self, sql, *args):
        return [dict(row) for row in self.db.execute(sql, args).fetchall()]

    async def d1_first(self, sql, *args):
        row = self.db.execute(sql, args).fetchone()
        return dict(row) if row else None

    async def d1_run(self, sql, *args):
        self.db.execute(sql, args)
        self.db.commit()

    async def audit(self, actor, action, resource_type, resource_id, details):
        self.audits.append((actor, action, resource_id))

    async def deliver_lead(self, lead):
        self.leads.append(lead)
        return {"ok": self.pipeline_status in ("created", "exists"),
                "status": self.pipeline_status, "contactId": "c" * 32}


LEAD = {
    "company": "Acme Robotics",
    "website": "https://www.Acme-Robotics.example/about",
    "contactName": "Jo Park",
    "contactEmail": "Jo@Acme-Robotics.example",
    "contactTitle": "CTO",
    "notes": "Self-hosts GitLab today.",
    "consent": True,
}


def join(runtime):
    return runtime.call(affiliates.JOIN_PATH, {
        "channel": "I run a dev agency.", "acceptTerms": True})


def test_website_domain_keeps_only_a_public_business_host():
    assert affiliates.website_domain("https://www.Example.com/pricing") == "example.com"
    assert affiliates.website_domain("example.co.uk") == "example.co.uk"
    assert affiliates.website_domain("https://forkmesh.com") == ""
    assert affiliates.website_domain("https://app.forkmesh.com") == ""
    assert affiliates.website_domain("http://10.0.0.1") == ""
    assert affiliates.website_domain("ftp://example.com") == ""
    assert affiliates.website_domain("https://user:pw@example.com") == ""
    assert affiliates.website_domain("not a site") == ""


def test_join_needs_a_verified_account_the_terms_and_a_channel():
    runtime = Runtime()
    runtime.account = ("acct-ana", {"name": "ana", "email_verified": False})
    assert join(runtime).status == 403
    runtime.account = ("acct-ana", {"name": "ana", "email_verified": True})
    assert runtime.call(affiliates.JOIN_PATH, {"channel": "x"}).data["error"] == "terms_required"
    assert runtime.call(affiliates.JOIN_PATH, {"acceptTerms": True}).data["error"] == "channel_required"
    response = join(runtime)
    assert response.status == 201
    assert response.data["me"]["affiliate"]["status"] == "active"
    assert join(runtime).status == 200
    runtime.account = None
    assert join(runtime).status == 401


def test_a_lead_lands_in_the_sales_pipeline_with_the_affiliate_named():
    runtime = Runtime()
    assert runtime.call(affiliates.LEADS_PATH, LEAD).data["error"] == "join_first"
    join(runtime)
    response = runtime.call(affiliates.LEADS_PATH, LEAD)
    assert response.status == 201
    assert response.data["lead"]["domain"] == "acme-robotics.example"
    assert response.data["lead"]["status"] == "new"
    assert response.data["lead"]["inPipeline"] is True
    sent = runtime.leads[0]
    assert sent["email"] == "jo@acme-robotics.example"
    assert sent["source"] == "Affiliate: @ana"
    assert sent["nextStep"] == "Check with @ana before contacting them"
    assert sent["flow"] == "customers" and sent["stage"] == "lead"
    assert "affiliate" in sent["tags"] and "affiliate-ana" in sent["tags"]
    assert sent["fields"]["affiliate"] == "@ana"
    assert "Self-hosts GitLab today." in sent["notes"]
    listing = runtime.call(affiliates.PREFIX, method="GET").data
    assert [lead["company"] for lead in listing["me"]["leads"]] == ["Acme Robotics"]
    assert response.data["lead"]["expiresAt"] == runtime._now + 7 * DAY


def test_a_claim_needs_the_company_and_consent_only_with_contact_details():
    runtime = Runtime()
    join(runtime)
    for field, error in (("company", "company_required"),
                         ("website", "website_required")):
        body = dict(LEAD, **{field: ""})
        assert runtime.call(affiliates.LEADS_PATH, body).data["error"] == error
    assert runtime.call(affiliates.LEADS_PATH, dict(LEAD, consent=False)).data["error"] == "consent_required"
    assert runtime.call(affiliates.LEADS_PATH, dict(LEAD, contactEmail="nope")).data["error"] == "invalid_contact_email"
    assert runtime.leads == []
    response = runtime.call(affiliates.LEADS_PATH, {
        "company": "Quiet Co", "website": "quiet.example"})
    assert response.status == 201
    assert runtime.leads[0]["name"] == "Quiet Co" and runtime.leads[0]["email"] == ""
    assert "happy to hear" not in runtime.leads[0]["notes"]


def test_affiliates_cannot_refer_themselves():
    runtime = Runtime()
    join(runtime)
    body = dict(LEAD, website="example.org", contactEmail="boss@example.org")
    assert runtime.call(affiliates.LEADS_PATH, body).data["error"] == "self_referral"
    body = dict(LEAD, contactEmail="ana@example.org")
    assert runtime.call(affiliates.LEADS_PATH, body).data["error"] == "self_referral"


def test_the_first_affiliate_to_add_a_business_keeps_it_until_the_claim_lapses():
    runtime = Runtime()
    join(runtime)
    assert runtime.call(affiliates.LEADS_PATH, LEAD).status == 201
    runtime.account = ("acct-bo", {"name": "bo", "email": "bo@example.net",
                                   "email_verified": True})
    join(runtime)
    again = dict(LEAD, website="acme-robotics.example", contactEmail="ceo@acme-robotics.example")
    assert runtime.call(affiliates.LEADS_PATH, again).status == 409
    runtime._now += 7 * DAY + 1
    assert runtime.call(affiliates.LEADS_PATH, again).status == 201


def test_a_business_already_in_the_pipeline_is_known_and_blocks_nobody():
    runtime = Runtime(pipeline_status="exists")
    join(runtime)
    response = runtime.call(affiliates.LEADS_PATH, LEAD)
    assert response.status == 201
    assert response.data["known"] is True
    assert response.data["lead"]["status"] == "known"
    assert response.data["lead"]["expiresAt"] == 0
    for index in range(4):
        body = dict(LEAD, website="biz%d.example" % index)
        assert runtime.call(affiliates.LEADS_PATH, body).status == 201


def test_a_lead_is_kept_when_the_pipeline_is_unreachable():
    runtime = Runtime(pipeline_status="delivery_failed")
    join(runtime)
    response = runtime.call(affiliates.LEADS_PATH, LEAD)
    assert response.status == 201
    assert response.data["lead"]["status"] == "new"
    assert response.data["lead"]["inPipeline"] is False


def test_ten_companies_at_a_time():
    runtime = Runtime()
    join(runtime)
    for index in range(affiliates.MAX_OPEN_LEADS):
        body = dict(LEAD, website="biz%d.example" % index)
        assert runtime.call(affiliates.LEADS_PATH, body).status == 201
    body = dict(LEAD, website="eleven.example")
    assert runtime.call(affiliates.LEADS_PATH, body).data["error"] == "open_lead_limit"


def activity_path(lead_id):
    return "%s/%s/activity" % (affiliates.LEADS_PATH, lead_id)


def test_logged_outreach_keeps_a_claim_alive_and_silence_lets_it_lapse():
    runtime = Runtime()
    join(runtime)
    lead_id = runtime.call(affiliates.LEADS_PATH, LEAD).data["lead"]["id"]
    assert runtime.call(activity_path(lead_id), {}).data["error"] == "note_required"
    runtime._now += 6 * DAY
    response = runtime.call(activity_path(lead_id), {"note": "Demo call booked for Tuesday."})
    assert response.status == 200
    assert response.data["lead"]["expiresAt"] == runtime._now + 7 * DAY
    assert response.data["lead"]["activity"][-1]["note"] == "Demo call booked for Tuesday."
    runtime._now += 6 * DAY
    listing = runtime.call(affiliates.PREFIX, method="GET").data["me"]["leads"]
    assert listing[0]["status"] == "new"
    runtime._now += 2 * DAY
    assert runtime.call(activity_path(lead_id), {"note": "Too late"}).data["error"] == "claim_expired"
    runtime.account = ("acct-bo", {"name": "bo", "email_verified": True})
    assert runtime.call(activity_path(lead_id), {"note": "x"}).status == 404
    assert runtime.call(activity_path("not-a-lead!"), {"note": "x"}).status == 404


def test_paused_affiliates_cannot_add_leads():
    runtime = Runtime()
    join(runtime)
    runtime.account = ("acct-boss", {"name": "boss", "email_verified": True})
    assert runtime.call(affiliates.ADMIN_PATH, {"affiliate": "ana", "status": "paused"}).status == 200
    runtime.account = ("acct-ana", {"name": "ana", "email": "ana@example.org", "email_verified": True})
    assert runtime.call(affiliates.LEADS_PATH, LEAD).data["error"] == "affiliate_not_active"


def test_admin_marks_a_win_and_the_commission_is_half_the_first_month():
    runtime = Runtime()
    join(runtime)
    lead_id = runtime.call(affiliates.LEADS_PATH, LEAD).data["lead"]["id"]
    assert runtime.call(affiliates.ADMIN_PATH, {"lead": lead_id, "status": "won"}).status == 403
    runtime.account = ("acct-boss", {"name": "boss", "email_verified": True})
    assert runtime.call(affiliates.ADMIN_PATH, {"lead": lead_id, "status": "won"}).data["error"] == "first_month_required"
    response = runtime.call(affiliates.ADMIN_PATH, {
        "lead": lead_id, "status": "won", "firstMonthCents": 5000, "note": "Team plan"})
    assert response.status == 200
    assert response.data["lead"]["commissionCents"] == 2500
    listing = runtime.call(affiliates.ADMIN_PATH, method="GET").data
    assert listing["leads"][0]["affiliate"] == "ana"
    assert listing["leads"][0]["contactEmail"] == "jo@acme-robotics.example"
    assert listing["affiliates"][0]["channel"] == "I run a dev agency."
    assert listing["leads"][0]["expiresAt"] == 0


def test_the_terms_match_the_revenue_contest():
    terms = " ".join(affiliates.program()["terms"]).lower()
    for phrase in ("50% of each eligible customer's first month",
                   "maximum of 10 companies", "before any outreach",
                   "expires after 7 days without meaningful outreach",
                   "not already in an active forkmesh sales conversation",
                   "through patreon", "own payment method", "refunds, chargebacks",
                   "in good standing for 30 days", "no self-referrals",
                   "form w-9 (us persons)", "w-8ben (non-us individuals)",
                   "w-8ben-e (non-us entities)", "tax reporting or withholding"):
        assert phrase in terms, phrase


def test_posts_need_an_allowed_origin():
    runtime = Runtime()
    runtime.same = False
    assert join(runtime).status == 403


def test_migration_round_trips():
    connection = sqlite3.connect(":memory:")
    connection.execute("CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT NOT NULL)")
    connection.executescript(MIGRATION.read_text(encoding="utf-8"))
    connection.executescript(DOWN.read_text(encoding="utf-8"))
    names = {row[0] for row in connection.execute(
        "SELECT name FROM sqlite_master WHERE type='table'")}
    assert names == {"schema_meta"}
    assert connection.execute("SELECT COUNT(*) FROM schema_meta").fetchone()[0] == 0


def test_routes_are_wired():
    for path in ("/api/affiliates", "/api/affiliates/join", "/api/affiliates/leads",
                 "/api/affiliates/leads/0000000a-0000-4000-8000-00000000000a/activity",
                 "/api/admin/affiliates"):
        assert urls.AFFILIATES_RE.match(path), path
    assert not urls.AFFILIATES_RE.match("/api/affiliates/other")
    app_runtime = (SRC / "app_runtime.py").read_text(encoding="utf-8")
    entry = (SRC / "entry.py").read_text(encoding="utf-8")
    assert "affiliates_handler(self.env, request, url.path)" in app_runtime
    assert 'affiliates = _LazyModule("affiliates")' in entry
    assert static_routes.external_site_route("/affiliates") == "www"


def test_core_never_names_the_sales_plugin():
    for name in ("affiliates.py", "programs_runtime.py", "plugin_data.py"):
        assert "crm" not in (SRC / name).read_text(encoding="utf-8").lower(), name
