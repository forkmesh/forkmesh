import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from test_crm import ZohoAppRuntime, ZohoSelfClientRuntime, _zoho_state, call  # noqa: E402


class MemberZohoRuntime(ZohoSelfClientRuntime):
    def __init__(self):
        super().__init__()
        self.zoho_sends = []

    async def outbound_json(self, url, method="GET", headers=None, body=""):
        if url.endswith("/api/accounts/4242") and method == "GET":
            return {"ok": True, "status": 200, "data": {"data": {
                "accountId": "4242", "primaryEmailAddress": "charlie@acme.example"}}}
        if url.endswith("/api/accounts/4242/messages") and method == "POST":
            self.zoho_sends.append(json.loads(body))
            return {"ok": True, "status": 200, "data": {
                "status": {"code": 200}, "data": {"messageId": "charlie-out-1"}}}
        return await super().outbound_json(url, method, headers, body)


SELF_CLIENT = {"clientId": "self-id", "clientSecret": "self-secret", "code": "fresh",
               "region": "eu", "mine": True}


def _settings(runtime, actor):
    return call(runtime.use("GET", actor), "/api/crm/acme/settings")["data"]["settings"]


def _connect_charlie(runtime):
    done = call(runtime.use("POST", "charlie", body=SELF_CLIENT), "/api/crm/acme/zoho/exchange")
    assert done["status"] == 200, done
    return done


def test_any_member_connects_their_own_zoho_without_touching_the_organization_one():
    runtime = MemberZohoRuntime()
    body = dict(SELF_CLIENT)
    body.pop("mine")
    assert call(runtime.use("POST", "charlie", body=body),
                "/api/crm/acme/zoho/exchange")["status"] == 403
    assert call(runtime.use("POST", "bob", body=SELF_CLIENT),
                "/api/crm/acme/zoho/exchange")["status"] == 404

    done = _connect_charlie(runtime)
    mine = done["data"]["settings"]["zohoMine"]
    assert (mine["connected"], mine["own"], mine["accountId"], mine["region"]) == (
        True, True, "4242", "eu")
    assert "self-secret" not in json.dumps(done["data"])
    assert done["data"]["settings"]["zoho"]["connected"] is False

    raw = runtime.db.execute("SELECT member_name,data FROM crm_zoho_members").fetchall()
    assert [row["member_name"] for row in raw] == ["charlie"]
    assert raw[0]["data"].startswith("sealed:") and "self-refresh" in raw[0]["data"]
    assert runtime.db.execute("SELECT COUNT(*) FROM crm_settings").fetchone()[0] == 0

    assert _settings(runtime, "charlie")["zohoMine"]["connected"] is True
    assert _settings(runtime, "alice")["zohoMine"]["connected"] is False
    assert [step["key"] for step in _settings(runtime, "alice")["zohoMine"]["steps"]] == [
        "connect"]


def test_one_click_connect_for_yourself_needs_no_admin_and_keeps_the_app_secret_out():
    runtime = ZohoAppRuntime()
    started = call(runtime.use("POST", "charlie", body={"mine": True}),
                   "/api/crm/acme/zoho/connect")
    assert started["status"] == 200
    state = _zoho_state(started["data"]["authorizationUrl"])
    done = call(runtime.use("GET", query={"state": state, "code": "good"}),
                "/api/crm/-/zoho/callback")
    assert done["headers"]["location"] == "/dashboard/crm?org=acme&zoho=connected_mine"
    raw = runtime.db.execute("SELECT member_bi,data FROM crm_zoho_members").fetchone()
    assert raw["member_bi"] == "bi:charlie"
    assert "granted-refresh" in raw["data"] and "platform-secret" not in raw["data"]
    mine = _settings(runtime, "charlie")["zohoMine"]
    assert (mine["connected"], mine["managed"], mine["canSend"]) == (True, True, True)
    assert _settings(runtime, "alice")["zoho"]["connected"] is False

    runtime.db.execute("DELETE FROM org_members WHERE name='charlie'")
    again = call(runtime.use("GET", query={"state": state, "code": "good"}),
                 "/api/crm/-/zoho/callback")
    assert again["headers"]["location"].endswith("&zoho=forbidden")


def test_a_members_sync_reads_their_mailbox_and_keeps_it_to_them():
    runtime = MemberZohoRuntime()
    _connect_charlie(runtime)
    call(runtime.use("POST", body={"name": "Sam", "email": "sam@example.com"}),
         "/api/crm/acme/contacts")
    runtime.outbound_calls = []
    runtime.zoho_messages = [
        {"providerMessageId": "in-1", "subject": "Private hello", "from": "pal@example.com",
         "to": "charlie@acme.example", "receivedAt": 2000},
        {"providerMessageId": "out-1", "subject": "Deck", "from": "charlie@acme.example",
         "to": "sam@example.com", "sourceFolder": "Sent", "receivedAt": 3000},
    ]
    synced = call(runtime.use("POST", "charlie", body={"limit": 10}), "/api/crm/acme/mail/import")
    assert synced["status"] == 200, synced
    assert (synced["data"]["mailbox"], synced["data"]["created"]) == ("mine", 2)
    assert "refresh_token=self-refresh" in runtime.outbound_calls[0][3]
    assert {item["subject"] for item in synced["data"]["mail"]} == {"Private hello", "Deck"}
    assert all(item["mine"] and "ownerBi" not in item for item in synced["data"]["mail"])
    assert synced["data"]["settings"]["zohoMine"]["lastSyncAt"] > 0
    assert synced["data"]["settings"]["zoho"]["lastSyncAt"] == 0

    alice = call(runtime.use("GET"), "/api/crm/acme")["data"]
    assert alice["mail"] == []
    sam = next(item for item in alice["contacts"] if item["email"] == "sam@example.com")
    assert sam["lastContactedAt"] == 3000
    message = synced["data"]["mail"][0]["id"]
    assert call(runtime.use("GET"), f"/api/crm/acme/mail/{message}")["status"] == 404
    assert call(runtime.use("DELETE"), f"/api/crm/acme/mail/{message}")["status"] == 404
    assert call(runtime.use("GET", "charlie"), f"/api/crm/acme/mail/{message}")["status"] == 200

    skipped = call(runtime.use("POST", "charlie", body={"auto": True}), "/api/crm/acme/mail/import")
    assert skipped["data"]["skipped"] is True
    assert call(runtime.use("POST", body={}), "/api/crm/acme/mail/import")["data"]["error"] == (
        "zoho_not_connected")


def test_compose_sends_from_your_own_zoho_and_others_keep_mailtrap():
    runtime = MemberZohoRuntime()
    _connect_charlie(runtime)
    contact = call(runtime.use("POST", body={"name": "Sam", "email": "sam@example.com"}),
                   "/api/crm/acme/contacts")["data"]["contact"]
    sent = call(runtime.use("POST", "charlie", body={
        "contactId": contact["id"], "subject": "Hi", "text": "Line one\nLine <two>"}),
        "/api/crm/acme/emails/send")
    assert sent["status"] == 201, sent
    assert (sent["data"]["via"], sent["data"]["fromEmail"]) == ("zoho", "charlie@acme.example")
    assert runtime.mailtrap_calls == []
    payload = runtime.zoho_sends[-1]
    assert payload["fromAddress"] == "charlie@acme.example"
    assert payload["content"] == "<div>Line one<br>Line &lt;two&gt;</div>"
    assert sent["data"]["email"]["status"] == "accepted"
    assert _settings(runtime, "charlie")["zohoMine"]["lastSentAt"] > 0

    call(runtime.use("PUT", body={"fromEmail": "team@acme.example"}), "/api/crm/acme/settings")
    other = call(runtime.use("POST", body={
        "to": "sam@example.com", "subject": "Hi", "text": "Hello"}), "/api/crm/acme/emails/send")
    assert other["status"] == 201 and len(runtime.mailtrap_calls) == 1


def test_disconnecting_your_zoho_leaves_everyone_elses():
    runtime = MemberZohoRuntime()
    _connect_charlie(runtime)
    call(runtime.use("POST", body=SELF_CLIENT), "/api/crm/acme/zoho/exchange")
    gone = call(runtime.use("POST", "charlie", body={"mine": True}),
                "/api/crm/acme/zoho/disconnect")
    assert gone["status"] == 200
    assert gone["data"]["settings"]["zohoMine"]["connected"] is False
    names = [row[0] for row in runtime.db.execute("SELECT member_name FROM crm_zoho_members")]
    assert names == ["alice"]
    assert call(runtime.use("POST", "charlie", body={}),
                "/api/crm/acme/zoho/disconnect")["status"] == 403
