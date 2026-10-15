import asyncio
import importlib.util
from pathlib import Path


spec = importlib.util.spec_from_file_location(
    "crm_test_harness", Path(__file__).with_name("test_crm.py"))
harness = importlib.util.module_from_spec(spec)
spec.loader.exec_module(harness)
crm = harness.crm

LEAD = {
    "actor": "ana",
    "name": "Jo Park",
    "email": "Jo@Acme-Robotics.example",
    "company": "Acme Robotics",
    "title": "CTO",
    "website": "https://acme-robotics.example",
    "source": "Affiliate: @ana",
    "notes": "Introduced by affiliate @ana.",
    "flow": "customers",
    "stage": "lead",
    "nextStep": "Reach out and mention @ana introduced them",
    "tags": ["affiliate", "affiliate-ana"],
    "fields": {"affiliate": "@ana", "affiliate_lead": "lead-1"},
    "views": ["ffffffffffffffffffffffffffffffff"],
    "owner": "ana",
}


class Runtime(harness.Runtime):
    async def blind(self, value):
        value = str(value)
        return value if value.startswith("org:") else await super().blind(value)


def record(runtime, org="acme", lead=None):
    return asyncio.run(crm.record_lead(runtime, org, dict(lead or LEAD)))


def test_a_lead_becomes_a_customer_pipeline_contact():
    runtime = Runtime()
    result = record(runtime)
    assert result["ok"] and result["status"] == "created"
    row = runtime.db.execute(
        "SELECT org_name,created_by_name,data FROM crm_contacts WHERE contact_id=?",
        (result["contactId"],)).fetchone()
    assert row["org_name"] == "acme" and row["created_by_name"] == "ana"
    contact = asyncio.run(runtime.open(row["data"]))
    assert contact["email"] == "jo@acme-robotics.example"
    assert (contact["flow"], contact["stage"]) == ("customers", "lead")
    assert contact["source"] == "Affiliate: @ana"
    assert contact["tags"] == ["affiliate", "affiliate-ana"]
    assert contact["fields"] == {"affiliate": "@ana", "affiliate_lead": "lead-1"}
    assert contact["views"] == [] and contact.get("owner", "") == ""
    assert runtime.audits[-1][0][1] == "crm.contact.create"


def test_a_known_contact_is_reported_and_left_untouched():
    runtime = Runtime()
    first = record(runtime)
    before = runtime.db.execute("SELECT data,updated_at FROM crm_contacts").fetchall()
    again = record(runtime, lead=dict(LEAD, email="jo@acme-robotics.example", stage="customer"))
    assert again == {"ok": True, "status": "exists", "contactId": first["contactId"]}
    after = runtime.db.execute("SELECT data,updated_at FROM crm_contacts").fetchall()
    assert [tuple(row) for row in before] == [tuple(row) for row in after]


def test_a_lead_needs_an_installed_pipeline_and_an_email():
    assert record(Runtime(installed=False))["status"] == "not_installed"
    assert record(Runtime(), org="nobody")["status"] == "not_installed"
    assert record(Runtime(), org="Bad Name!")["status"] == "invalid_lead"
    assert record(Runtime(), lead=dict(LEAD, email="not-an-email"))["status"] == "invalid_email"
    assert record(Runtime(), lead=dict(LEAD, email="", name=""))["status"] == "name_or_email_required"


def test_a_claim_without_contact_details_is_a_company_row():
    runtime = Runtime()
    lead = dict(LEAD, email="", name="Acme Robotics")
    first = record(runtime, lead=lead)
    second = record(runtime, lead=lead)
    assert first["status"] == second["status"] == "created"
    assert first["contactId"] != second["contactId"]


def test_the_platform_finds_the_hook_without_naming_the_plugin():
    root = Path(__file__).resolve().parents[4]
    engine = (root / "app" / "src" / "plugin_data.py").read_text(encoding="utf-8")
    assert 'getattr(module, "record_lead", None)' in engine
