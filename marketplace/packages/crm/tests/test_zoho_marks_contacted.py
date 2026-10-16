import importlib.util
from pathlib import Path


spec = importlib.util.spec_from_file_location(
    "crm_test_harness", Path(__file__).with_name("test_crm.py"))
harness = importlib.util.module_from_spec(spec)
spec.loader.exec_module(harness)
call = harness.call


def _connected():
    runtime = harness.Runtime()
    call(runtime.use("PUT", body={"zoho": {
        "region": "com", "accountId": "1", "clientId": "c", "clientSecret": "s",
        "refreshToken": "r"}}), "/api/crm/acme/settings")
    return runtime


def _contact(runtime, **body):
    return call(runtime.use("POST", body=body),
                "/api/crm/acme/contacts")["data"]["contact"]["id"]


def _sent(runtime, message_id, to, at):
    return {"providerMessageId": message_id, "subject": "Hello from Acme",
            "sourceFolder": "Sent", "from": "me@acme.example", "sender": "Me",
            "to": to, "receivedAt": at}


def _contacts(runtime):
    return {item["id"]: item for item in
            call(runtime.use("GET"), "/api/crm/acme")["data"]["contacts"]}


def _sync(runtime):
    return call(runtime.use("POST", body={"limit": 20}),
                "/api/crm/acme/mail/import")["data"]


def test_zoho_sync_moves_a_reach_out_contact_to_contacted_once():
    runtime = _connected()
    reach = _contact(runtime, name="Reach Person", email="reach@example.com",
                     flow="investment", stage="sourced")
    ahead = _contact(runtime, name="Ahead Person", email="ahead@example.com",
                     flow="investment", stage="meeting")
    partner = _contact(runtime, name="Partner Person", email="partner@example.com",
                       flow="partnerships", stage="prospect",
                       fields={"work_email": "partner.work@example.org"})
    lead = _contact(runtime, name="Lead Person", email="lead@example.com",
                    flow="customers", stage="lead")
    sent_at = runtime.clock - 1000
    runtime.zoho_messages = [
        _sent(runtime, "out-1",
              "&quot;Reach Person&quot; &lt;Reach@Example.com&gt;, ahead@example.com",
              sent_at),
        _sent(runtime, "out-2", "partner.work@example.org", sent_at - 1),
        _sent(runtime, "out-3", "lead@example.com", sent_at - 2),
        _sent(runtime, "out-4", "stranger@example.net", sent_at - 3),
        {"providerMessageId": "in-1", "subject": "Inbound", "sourceFolder": "Inbox",
         "from": "reach@example.com", "to": "me@acme.example",
         "receivedAt": sent_at},
    ]
    synced = _sync(runtime)
    assert (synced["outboundRecorded"], synced["contactsAdvanced"]) == (5, 2)
    contacts = _contacts(runtime)
    assert {key: contacts[key]["stage"] for key in (reach, ahead, partner, lead)} == {
        reach: "contacted", ahead: "meeting", partner: "contacted", lead: "lead"}
    assert contacts[reach]["lastContactedAt"] == sent_at
    assert contacts[ahead]["lastContactedAt"] == sent_at
    assert contacts[partner]["lastContactedAt"] == sent_at - 1
    assert contacts[lead]["lastContactedAt"] == sent_at - 2
    emails = call(runtime.use("GET"), "/api/crm/acme")["data"]["emails"]
    assert {(item["externalId"], item["contactId"]) for item in emails} == {
        ("out-1", reach), ("out-1/ahead@example.com", ahead), ("out-2", partner),
        ("out-3", lead), ("out-4", "")}
    assert all(item["direction"] == "outbound" for item in emails)

    again = _sync(runtime)
    assert (again["outboundRecorded"], again["contactsAdvanced"]) == (0, 0)
    assert len(call(runtime.use("GET"), "/api/crm/acme")["data"]["emails"]) == 5
    assert _contacts(runtime)[reach]["stage"] == "contacted"


def test_zoho_sync_links_mail_imported_before_the_contact_existed():
    runtime = _connected()
    sent_at = runtime.clock - 5000
    runtime.zoho_messages = [_sent(runtime, "out-1", "later@example.com", sent_at)]
    first = _sync(runtime)
    assert (first["outboundRecorded"], first["contactsAdvanced"]) == (1, 0)
    later = _contact(runtime, name="Later Person", email="later@example.com",
                     flow="investment", stage="sourced")
    second = _sync(runtime)
    assert (second["outboundRecorded"], second["contactsAdvanced"]) == (0, 1)
    contact = _contacts(runtime)[later]
    assert (contact["stage"], contact["lastContactedAt"]) == ("contacted", sent_at)
    emails = call(runtime.use("GET"), "/api/crm/acme")["data"]["emails"]
    assert [(item["externalId"], item["contactId"]) for item in emails] == [
        ("out-1", later)]


def test_zoho_sync_advances_a_contact_whose_mail_was_already_linked():
    runtime = _connected()
    linked = _contact(runtime, name="Linked Person", email="linked@example.com",
                      flow="partnerships", stage="prospect")
    sent_at = runtime.clock - 2000
    runtime.zoho_messages = [_sent(runtime, "out-1", "linked@example.com", sent_at)]
    _sync(runtime)
    call(runtime.use("PATCH", body={"stage": "prospect"}),
         f"/api/crm/acme/contacts/{linked}")
    assert _contacts(runtime)[linked]["stage"] == "prospect"
    synced = _sync(runtime)
    assert (synced["outboundRecorded"], synced["contactsAdvanced"]) == (0, 1)
    assert _contacts(runtime)[linked]["stage"] == "contacted"


def test_an_outbound_link_moves_the_contact_to_contacted_but_an_inbound_one_does_not():
    runtime = harness.Runtime()
    sourced = _contact(runtime, name="Sourced Person", email="sourced@example.com",
                       flow="investment", stage="sourced")
    passed = _contact(runtime, name="Passed Person", email="passed@example.com",
                      flow="investment", stage="passed")
    inbound = call(runtime.use("POST", body={
        "provider": "zoho", "direction": "inbound", "messageId": "z-in",
        "contactEmail": "sourced@example.com", "subject": "Hi"}),
        "/api/crm/acme/emails/link")
    assert inbound["data"]["stageAdvanced"] is False
    assert _contacts(runtime)[sourced]["stage"] == "sourced"
    for message_id, address in (("z-1", "sourced@example.com"),
                                ("z-2", "passed@example.com")):
        call(runtime.use("POST", body={
            "provider": "zoho", "direction": "outbound", "messageId": message_id,
            "contactEmail": address, "subject": "Hi"}),
            "/api/crm/acme/emails/link")
    contacts = _contacts(runtime)
    assert (contacts[sourced]["stage"], contacts[passed]["stage"]) == (
        "contacted", "passed")
    moved = call(runtime.use("POST", body={
        "provider": "zoho", "direction": "outbound", "messageId": "z-3",
        "contactEmail": "sourced@example.com", "flow": "investment",
        "stage": "sourced"}), "/api/crm/acme/emails/link")
    assert moved["data"]["contact"]["stage"] == "sourced"
