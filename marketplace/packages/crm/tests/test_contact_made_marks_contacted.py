import importlib.util
import sys
from pathlib import Path


spec = importlib.util.spec_from_file_location(
    "crm_test_harness", Path(__file__).with_name("test_crm.py"))
harness = importlib.util.module_from_spec(spec)
spec.loader.exec_module(harness)
call = harness.call


def _contact(runtime, **body):
    return call(runtime.use("POST", body=body),
                "/api/crm/acme/contacts")["data"]["contact"]["id"]


def _contacts(runtime):
    return {item["id"]: item for item in
            call(runtime.use("GET"), "/api/crm/acme")["data"]["contacts"]}


def _runtime():
    runtime = harness.Runtime()
    _contacts(runtime)
    return runtime


def test_mark_contacted_moves_a_contact_before_contacted_to_it():
    runtime = _runtime()
    sourced = _contact(runtime, name="Sourced", email="s@example.com",
                       flow="investment", stage="sourced")
    meeting = _contact(runtime, name="Meeting", email="m@example.com",
                       flow="investment", stage="meeting")
    lead = _contact(runtime, name="Lead", email="l@example.com",
                    flow="customers", stage="lead")
    stamp = runtime.clock - 1000
    single = call(runtime.use("PATCH", body={"lastContactedAt": stamp}),
                  f"/api/crm/acme/contacts/{sourced}")
    assert single["data"]["contact"]["stage"] == "contacted"
    bulk = call(runtime.use("POST", body={"edits": [
        {"id": meeting, "lastContactedAt": stamp},
        {"id": lead, "lastContactedAt": stamp}]}), "/api/crm/acme/contacts/bulk")
    assert {item["id"]: item["stage"] for item in bulk["data"]["contacts"]} == {
        meeting: "meeting", lead: "lead"}
    contacts = _contacts(runtime)
    assert [contacts[key]["stage"] for key in (sourced, meeting, lead)] == [
        "contacted", "meeting", "lead"]
    assert all(contacts[key]["lastContactedAt"] == stamp
               for key in (sourced, meeting, lead))


def test_an_edit_naming_a_stage_or_clearing_the_date_keeps_its_stage():
    runtime = _runtime()
    named = _contact(runtime, name="Named", email="n@example.com",
                     flow="partnerships", stage="prospect")
    cleared = _contact(runtime, name="Cleared", email="c@example.com",
                       flow="partnerships", stage="prospect",
                       lastContactedAt=runtime.clock - 5000)
    call(runtime.use("PATCH", body={"lastContactedAt": runtime.clock,
                                    "stage": "prospect"}),
         f"/api/crm/acme/contacts/{named}")
    call(runtime.use("PATCH", body={"lastContactedAt": 0}),
         f"/api/crm/acme/contacts/{cleared}")
    contacts = _contacts(runtime)
    assert (contacts[named]["stage"], contacts[cleared]["stage"]) == (
        "prospect", "prospect")


def test_tracked_outreach_and_recorded_sends_move_the_contact_to_contacted():
    runtime = _runtime()
    tracked = _contact(runtime, name="Tracked", email="t@example.com",
                       flow="investment", stage="sourced")
    outreach = _contact(runtime, name="Outreach", email="o@example.com",
                        flow="partnerships", stage="prospect")
    recorded = _contact(runtime, name="Recorded", email="r@example.com",
                        flow="investment", stage="sourced")
    failed = _contact(runtime, name="Failed", email="f@example.com",
                      flow="investment", stage="sourced")
    call(runtime.use("PUT", body={"fromEmail": "hello@acme.example"}),
         "/api/crm/acme/settings")
    assert call(runtime.use("POST", body={
        "contactId": tracked, "subject": "Hello", "text": "A note",
        "mailtrapKey": "key"}), "/api/crm/acme/emails/send")["status"] == 201
    assert call(runtime.use("POST", body={
        "to": "o@example.com", "subject": "Sponsoring", "body": "Hi there",
        "template": "sponsorship"}), "/api/crm/acme/outreach/send")["status"] == 200
    sent_at = runtime.clock - 3000
    assert call(runtime.use("POST", body={
        "to": "r@example.com", "contactId": recorded, "subject": "Follow-up",
        "externalId": "provider-1", "status": "delivered", "sentAt": sent_at}),
        "/api/crm/acme/emails")["status"] == 201
    assert call(runtime.use("POST", body={
        "to": "f@example.com", "contactId": failed, "subject": "Bounced",
        "externalId": "provider-2", "status": "failed"}),
        "/api/crm/acme/emails")["status"] == 201
    contacts = _contacts(runtime)
    assert [contacts[key]["stage"] for key in (tracked, outreach, recorded, failed)] == [
        "contacted", "contacted", "contacted", "sourced"]
    assert contacts[recorded]["lastContactedAt"] == sent_at
    assert contacts[failed]["lastContactedAt"] == 0


def test_contacts_stamped_before_the_fix_are_moved_once():
    runtime = harness.Runtime()
    stamp = runtime.clock - 10000
    stuck = _contact(runtime, name="Stuck", email="stuck@example.com",
                     flow="investment", stage="sourced", lastContactedAt=stamp)
    never = _contact(runtime, name="Never", email="never@example.com",
                     flow="investment", stage="sourced")
    past = _contact(runtime, name="Past", email="past@example.com",
                    flow="investment", stage="diligence", lastContactedAt=stamp)
    contacts = _contacts(runtime)
    assert [contacts[key]["stage"] for key in (stuck, never, past)] == [
        "contacted", "sourced", "diligence"]
    assert contacts[stuck]["lastContactedAt"] == stamp
    call(runtime.use("PATCH", body={"stage": "sourced"}),
         f"/api/crm/acme/contacts/{stuck}")
    assert _contacts(runtime)[stuck]["stage"] == "sourced"
    assert [row["name"] for row in runtime.db.execute(
        "SELECT name FROM crm_backfills")] == ["contacted-stages"]


def test_an_automatic_outreach_send_moves_the_contact_to_contacted():
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from test_auto_email import AiRuntime, _draft, _send
    runtime = AiRuntime([_draft("A story for your readers")])
    _contacts(runtime)
    ivy = _contact(runtime, name="Ivy Investor", email="ivy@fund.example",
                   flow="investment", stage="sourced")
    assert _send(runtime, ivy)["status"] == 201
    contact = _contacts(runtime)[ivy]
    assert contact["stage"] == "contacted"
    assert contact["lastContactedAt"] > 0
