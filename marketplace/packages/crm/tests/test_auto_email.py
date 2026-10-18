import html
import json
import re
import sys
from pathlib import Path
from urllib.parse import parse_qs, urlparse

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_crm import Runtime, call, crm  # noqa: E402


class AiRuntime(Runtime):
    def __init__(self, drafts=None):
        super().__init__()
        self.drafts = list(drafts or [])
        self.prompts = []

    async def ai_json(self, account_bi, system_prompt, user_prompt, schema,
                      image_data_url="", max_tokens=700):
        self.prompts.append(user_prompt)
        if not self.drafts:
            return {"ok": False, "error": "provider_error"}
        draft = self.drafts.pop(0)
        if callable(draft):
            draft = draft(user_prompt)
        return {"ok": True, "data": draft, "model": "test-model"}


def _contact(runtime, **body):
    return call(runtime.use("POST", body=body),
                "/api/crm/acme/contacts")["data"]["contact"]["id"]


def _draft(subject, link_from_prompt=True, extra=""):
    def write(prompt):
        link = prompt.split("## The tracked link (include it exactly once, on its own line)\n")[1].split("\n")[0]
        body = ("Hi there, I saw your recent piece on code hosting and thought "
                "ForkMesh fits the story you are telling.\n\n"
                + (link if link_from_prompt else "") + extra + "\n\nAcme Founders")
        return {"subject": subject, "body": body}
    return write


def _send(runtime, contact_id, actor="alice", **body):
    return call(runtime.use("POST", actor, dict(body, contactId=contact_id)),
                "/api/crm/acme/outreach/auto")


def _tracking_urls(sent_html):
    opener = re.search(r'<img src="([^"]+)"', sent_html).group(1)
    links = [html.unescape(href) for href in re.findall(r'href="([^"]+)"', sent_html)]
    return opener, links


def _visit(runtime, url):
    parsed = urlparse(url)
    query = {key: values[0] for key, values in parse_qs(parsed.query).items()}
    runtime.clock += crm.TRACK_QUIET_MS
    return call(runtime.use("GET", "nobody", query=query), parsed.path)


def test_an_investor_gets_a_model_written_email_with_their_tracked_deck_link():
    runtime = AiRuntime([_draft("A story for your readers")])
    ivy = _contact(runtime, name="Ivy Investor", email="ivy@fund.example",
                   flow="investment", stage="sourced")
    sent = _send(runtime, ivy, list="Angels")
    assert sent["status"] == 201, sent
    data = sent["data"]
    assert data["linkKind"] == "deck"
    assert "/pitch-deck/" in data["link"] and data["link"] in data["text"]
    assert data["email"]["automated"] is True and data["email"]["linkKind"] == "deck"
    assert "as an investor" in runtime.prompts[0]
    assert "No tracked emails yet" in runtime.prompts[0]
    token, payload = runtime.mailtrap_calls[-1]
    assert token == "" and payload["tracked"] is True
    assert payload["custom_variables"]["forkmesh_crm_outreach_template"] == "auto"
    assert payload["from"]["email"] == crm.OUTREACH_FROM_EMAIL
    opener, links = _tracking_urls(payload["html"])
    assert "/api/crm/-/t/o/" in opener and opener.endswith(".png")
    assert all("/api/crm/-/t/c/" in link for link in links)
    contact = call(runtime.use("GET"), f"/api/crm/acme/contacts/{ivy}")["data"]["contact"]
    assert contact["lastContactedAt"] > 0
    log = runtime.db.execute("SELECT COUNT(*) FROM crm_outreach_log").fetchone()[0]
    assert log == 1


def test_everyone_else_gets_the_senders_referral_link():
    runtime = AiRuntime([_draft("Try ForkMesh on your repos")])
    sam = _contact(runtime, name="Sam Dev", email="sam@example.com", flow="customers")
    sent = _send(runtime, sam)
    assert sent["status"] == 201, sent
    assert sent["data"]["link"] == "https://forkmesh.com/r/alice"
    assert runtime.db.execute("SELECT COUNT(*) FROM crm_deck_links").fetchone()[0] == 0


def test_opens_and_clicks_are_tracked_pinged_once_and_never_redirect_elsewhere():
    runtime = AiRuntime([_draft("Hello")])
    sam = _contact(runtime, name="Sam Dev", email="sam@example.com", flow="customers")
    send_id = _send(runtime, sam)["data"]["email"]["id"]
    opener, links = _tracking_urls(runtime.mailtrap_calls[-1][1]["html"])
    runtime.pings.clear()

    opened = _visit(runtime, opener)
    assert opened["status"] == 302 and opened["headers"]["location"] == crm.TRACK_LOGO
    _visit(runtime, opener)
    referral = next(link for link in links if "forkmesh.com%2Fr%2Falice" in link)
    clicked = _visit(runtime, referral)
    assert clicked["status"] == 302
    assert clicked["headers"]["location"] == "https://forkmesh.com/r/alice"
    row = runtime.db.execute(
        "SELECT opens,clicks FROM crm_email_tracking WHERE send_id=?", (send_id,)).fetchone()
    assert tuple(row) == (2, 1)
    email = call(runtime.use("GET"), f"/api/crm/acme/emails/{send_id}")["data"]["email"]
    assert email["status"] == "click" and email["opens"] == 2 and email["clicks"] == 1
    titles = [ping["title"] for ping in runtime.pings if ping["recipient"] == "alice"]
    assert titles == ["📬 Sam Dev opened your email", "🔗 Sam Dev clicked the link in your email"]

    forged = _visit(runtime, referral.replace("forkmesh.com%2Fr%2Falice", "evil.example"))
    assert forged["status"] == 404
    bad_token = _visit(runtime, opener.replace(send_id, "f" * 32))
    assert bad_token["headers"]["location"] == crm.TRACK_LOGO
    assert runtime.db.execute(
        "SELECT opens FROM crm_email_tracking WHERE send_id=?", (send_id,)).fetchone()[0] == 2


def test_a_scanner_hit_right_after_sending_is_not_counted():
    runtime = AiRuntime([_draft("Hello")])
    sam = _contact(runtime, name="Sam Dev", email="sam@example.com", flow="customers")
    send_id = _send(runtime, sam)["data"]["email"]["id"]
    opener, _ = _tracking_urls(runtime.mailtrap_calls[-1][1]["html"])
    path = urlparse(opener).path
    assert call(runtime.use("GET", "nobody"), path)["status"] == 302
    assert runtime.db.execute(
        "SELECT COUNT(*) FROM crm_email_tracking WHERE send_id=?", (send_id,)).fetchone()[0] == 0


def test_the_next_email_learns_from_what_got_clicks_and_what_went_unopened():
    runtime = AiRuntime([_draft("Clicked subject"), _draft("Ignored subject"),
                         _draft("Third")])
    first = _contact(runtime, name="Ann", email="ann@example.com", flow="customers")
    second = _contact(runtime, name="Bo", email="bo@example.com", flow="customers")
    third = _contact(runtime, name="Cy", email="cy@example.com", flow="customers")
    _send(runtime, first)
    _, links = _tracking_urls(runtime.mailtrap_calls[-1][1]["html"])
    _visit(runtime, links[0])
    _send(runtime, second)
    runtime.clock += crm.AUTO_EMAIL_SETTLE_MS
    sent = _send(runtime, third)
    assert sent["status"] == 201
    prompt = runtime.prompts[-1]
    lessons = prompt.split("## What our past emails taught us")[1]
    assert "Of our last 2 tracked emails, 50% were opened and 50% had a link clicked." in lessons
    clicked = lessons.split("Emails that got a click")[1].split("Nobody opened")[0]
    assert '"Clicked subject"' in clicked
    assert '"Ignored subject"' in lessons.split("Nobody opened these after two days")[1]
    assert sent["data"]["learning"]["sent"] == 2


def test_guards_roster_recent_send_closed_stage_and_missing_email():
    runtime = AiRuntime([_draft("One"), _draft("Two")])
    sam = _contact(runtime, name="Sam", email="sam@example.com", flow="customers")
    assert _send(runtime, sam, actor="charlie")["status"] == 403
    assert _send(runtime, sam)["status"] == 201
    again = _send(runtime, sam)
    assert again["status"] == 409 and again["data"]["error"] == "recently_emailed"
    gone = _contact(runtime, name="Gone", email="gone@example.com", flow="customers",
                    stage="inactive")
    assert _send(runtime, gone)["data"]["error"] == "contact_closed"
    nomail = _contact(runtime, name="No Mail", flow="customers")
    assert _send(runtime, nomail)["data"]["error"] == "contact_has_no_email"
    assert len(runtime.mailtrap_calls) == 1


def test_without_host_ai_the_prompt_comes_back_for_the_desktop_agent():
    runtime = AiRuntime([])
    sam = _contact(runtime, name="Sam", email="sam@example.com", flow="customers")
    failed = _send(runtime, sam)
    assert failed["status"] == 503
    assert failed["data"]["error"] == "ai_unavailable"
    assert "## The tracked link" in failed["data"]["prompt"]
    link = failed["data"]["link"]
    assert runtime.mailtrap_calls == []
    sent = _send(runtime, sam, subject="Subject: From the agent",
                 text="Hi Sam, a short note from our desktop agent about ForkMesh.\n\n" + link,
                 model="Claude Code")
    assert sent["status"] == 201, sent
    assert sent["data"]["subject"] == "From the agent"
    assert sent["data"]["model"] == "Claude Code"


def test_drafts_with_placeholders_or_invented_links_are_refused():
    runtime = AiRuntime([_draft("Hi [Name]"),
                         _draft("Fine", extra="\nhttps://invented.example/x")])
    sam = _contact(runtime, name="Sam", email="sam@example.com", flow="customers")
    refused = _send(runtime, sam)
    assert refused["status"] == 422 and refused["data"]["error"] == "draft_unusable"
    assert len(runtime.prompts) == crm.AUTO_EMAIL_ATTEMPTS
    assert runtime.mailtrap_calls == []


def test_a_missing_link_is_appended_and_preview_sends_nothing():
    runtime = AiRuntime([_draft("Preview", link_from_prompt=False)])
    sam = _contact(runtime, name="Sam", email="sam@example.com", flow="customers")
    preview = _send(runtime, sam, preview=True)
    assert preview["status"] == 200 and preview["data"]["preview"] is True
    assert preview["data"]["text"].endswith("\n\nhttps://forkmesh.com/r/alice")
    assert runtime.mailtrap_calls == []
    assert runtime.db.execute("SELECT COUNT(*) FROM crm_email_sends").fetchone()[0] == 0


class ZohoSendRuntime(AiRuntime):
    def __init__(self, drafts=None, send_status=200, send_error=""):
        super().__init__(drafts)
        self.send_status = send_status
        self.send_error = send_error
        self.zoho_sends = []

    async def outbound_json(self, url, method="GET", headers=None, body=""):
        if url.endswith("/api/accounts/1") and method == "GET":
            self.outbound_calls.append((url, method, dict(headers or {}), body))
            return {"ok": True, "status": 200, "data": {"data": {
                "accountId": "1", "primaryEmailAddress": "me@acme.example",
                "sendMailDetails": [{"fromAddress": "founders@acme.example"}]}}}
        if url.endswith("/api/accounts/1/messages") and method == "POST":
            self.zoho_sends.append((dict(headers or {}), json.loads(body)))
            if self.send_status != 200:
                return {"ok": True, "status": self.send_status, "data": {
                    "data": {"errorCode": self.send_error},
                    "status": {"code": self.send_status}}}
            return {"ok": True, "status": 200, "data": {
                "status": {"code": 200}, "data": {"messageId": "zoho-out-1"}}}
        return await super().outbound_json(url, method, headers, body)


def _zoho_connected(runtime, from_email=""):
    body = {"zoho": {"region": "com", "accountId": "1", "clientId": "c",
                     "clientSecret": "s", "refreshToken": "r"}}
    if from_email:
        body["fromEmail"] = from_email
    assert call(runtime.use("PUT", body=body), "/api/crm/acme/settings")["status"] == 200
    return runtime


def test_with_zoho_connected_the_email_goes_out_from_the_zoho_mailbox():
    runtime = _zoho_connected(ZohoSendRuntime([_draft("Through Zoho")]),
                              "founders@acme.example")
    sam = _contact(runtime, name="Sam Dev", email="sam@example.com", flow="customers")
    sent = _send(runtime, sam)
    assert sent["status"] == 201, sent
    assert runtime.mailtrap_calls == []
    headers, payload = runtime.zoho_sends[-1]
    assert headers["authorization"] == "Zoho-oauthtoken request-local-token"
    assert payload["fromAddress"] == "founders@acme.example"
    assert payload["toAddress"] == "sam@example.com"
    assert payload["subject"] == "Through Zoho" and payload["mailFormat"] == "html"
    assert "/api/crm/-/t/o/" in payload["content"]
    assert sent["data"]["via"] == "zoho"
    email = sent["data"]["email"]
    assert email["provider"] == "zoho" and email["externalId"] == "zoho-out-1"
    assert email["status"] == "accepted" and email["automated"] is True
    again = _send(runtime, sam)
    assert again["status"] == 409 and again["data"]["error"] == "recently_emailed"

    runtime.zoho_messages = [{
        "providerMessageId": "zoho-out-1", "subject": "Through Zoho",
        "sourceFolder": "Sent", "from": "founders@acme.example", "sender": "Acme",
        "to": "sam@example.com", "receivedAt": runtime.now()}]
    synced = call(runtime.use("POST", body={"limit": 20}), "/api/crm/acme/mail/import")
    assert synced["status"] == 200 and synced["data"]["outboundRecorded"] == 0
    assert runtime.db.execute("SELECT COUNT(*) FROM crm_email_sends").fetchone()[0] == 1


def test_an_outreach_sender_zoho_does_not_own_falls_back_to_the_mailbox_address():
    runtime = _zoho_connected(ZohoSendRuntime([_draft("Hello")]))
    sam = _contact(runtime, name="Sam", email="sam@example.com", flow="customers")
    sent = _send(runtime, sam)
    assert sent["status"] == 201, sent
    assert runtime.zoho_sends[-1][1]["fromAddress"] == "me@acme.example"
    assert sent["data"]["fromEmail"] == "me@acme.example"


def test_a_read_only_zoho_connection_asks_for_a_reconnect_and_never_uses_mailtrap():
    runtime = _zoho_connected(ZohoSendRuntime(
        [_draft("Hello")], send_status=404, send_error="INVALID_OAUTHSCOPE"))
    sam = _contact(runtime, name="Sam", email="sam@example.com", flow="customers")
    refused = _send(runtime, sam)
    assert refused["status"] == 409
    assert refused["data"]["error"] == "zoho_send_not_allowed"
    assert "Reconnect" in refused["data"]["detail"]
    assert runtime.mailtrap_calls == []
    status = runtime.db.execute("SELECT status FROM crm_email_sends").fetchone()[0]
    assert status == "failed"
    contact = call(runtime.use("GET"), f"/api/crm/acme/contacts/{sam}")["data"]["contact"]
    assert not contact.get("lastContactedAt")


def test_zoho_sends_teach_the_next_email_like_mailtrap_ones():
    runtime = _zoho_connected(ZohoSendRuntime([_draft("First"), _draft("Second")]))
    first = _contact(runtime, name="Ann", email="ann@example.com", flow="customers")
    second = _contact(runtime, name="Bo", email="bo@example.com", flow="customers")
    _send(runtime, first)
    sent = _send(runtime, second)
    assert sent["status"] == 201
    assert sent["data"]["learning"]["sent"] == 1


GMAIL_PROXY = "Mozilla/5.0 (Windows NT 5.1; rv:11.0) Gecko Firefox/11.0 (via ggpht.com GoogleImageProxy)"


class VisitorRuntime(AiRuntime):
    def __init__(self, drafts=None, ip="66.249.84.10", agent=GMAIL_PROXY, rdap=True):
        super().__init__(drafts)
        self.visitor = {"ip": ip, "agent": agent}
        self.rdap = rdap

    def request_visitor(self):
        return dict(self.visitor)

    async def outbound_json(self, url, method="GET", headers=None, body=""):
        if url.startswith(crm.WHOIS_RDAP):
            self.outbound_calls.append((url, method, dict(headers or {}), body))
            if not self.rdap:
                return {"ok": False, "error": "outbound_unavailable"}
            return {"ok": True, "status": 200, "data": {
                "name": "GOOGLE", "country": "us",
                "cidr0_cidrs": [{"v4prefix": "66.249.64.0", "length": 19}],
                "entities": [
                    {"roles": ["abuse"], "vcardArray": ["vcard", [["fn", {}, "text", "Abuse"]]]},
                    {"roles": ["registrant"], "vcardArray": ["vcard", [
                        ["version", {}, "text", "4.0"], ["fn", {}, "text", "Google LLC"]]]}]}}
        return await super().outbound_json(url, method, headers, body)


def test_the_first_open_ping_names_the_whois_owner_and_the_mail_client():
    runtime = VisitorRuntime([_draft("Hello")])
    sam = _contact(runtime, name="Sam Dev", email="sam@example.com", flow="customers")
    _send(runtime, sam)
    opener, _ = _tracking_urls(runtime.mailtrap_calls[-1][1]["html"])
    runtime.pings.clear()
    _visit(runtime, opener)
    _visit(runtime, opener)
    pings = [ping for ping in runtime.pings if ping["recipient"] == "alice"]
    assert len(pings) == 1
    body = pings[0]["body"].split("\n")
    assert body[0] == "“Hello”"
    assert body[1] == "Whois 66.249.84.10: Google LLC (GOOGLE, 66.249.64.0/19, US)"
    assert body[2] == "Agent: Gmail image proxy · " + GMAIL_PROXY
    assert [call[0] for call in runtime.outbound_calls] == [crm.WHOIS_RDAP + "66.249.84.10"]


def test_a_private_address_is_never_looked_up_and_a_failed_lookup_still_pings():
    runtime = VisitorRuntime([_draft("Hello"), _draft("Again")], ip="10.0.0.5",
                             agent="Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X) "
                                   "AppleWebKit/605.1.15 (KHTML, like Gecko) Mobile/15E148")
    sam = _contact(runtime, name="Sam Dev", email="sam@example.com", flow="customers")
    _send(runtime, sam)
    opener, _ = _tracking_urls(runtime.mailtrap_calls[-1][1]["html"])
    runtime.pings.clear()
    _visit(runtime, opener)
    assert runtime.outbound_calls == []
    assert runtime.pings[-1]["body"].split("\n")[1].startswith("Agent: iPhone · Apple Mail · ")

    runtime.visitor = {"ip": "2001:4860:4860::8888", "agent": ""}
    runtime.rdap = False
    ann = _contact(runtime, name="Ann", email="ann@example.com", flow="customers")
    _send(runtime, ann)
    opener, _ = _tracking_urls(runtime.mailtrap_calls[-1][1]["html"])
    _visit(runtime, opener)
    assert runtime.pings[-1]["body"] == "“Again”\nWhois 2001:4860:4860::8888: no answer"


def test_a_rating_and_note_reach_the_next_prompt():
    runtime = AiRuntime([_draft("Quick idea for your repos"), _draft("Second")])
    sam = _contact(runtime, name="Sam Dev", email="sam@example.com", flow="customers")
    send_id = _send(runtime, sam)["data"]["email"]["id"]
    rate = lambda actor, **body: call(runtime.use("POST", actor, body), "/api/crm/acme/outreach/rate")
    assert rate("alice", emailId=send_id, rating=0)["data"]["error"] == "rating_invalid"
    assert rate("alice", emailId="f" * 32, rating=3)["status"] == 404
    assert rate("charlie", emailId=send_id, rating=3)["status"] == 403
    rated = rate("alice", emailId=send_id, rating=2, note="  Too long, and   drop the jargon. ")
    assert rated["status"] == 200, rated
    assert rated["data"]["email"]["rating"] == 2
    assert rated["data"]["email"]["ratingNote"] == "Too long, and drop the jargon."
    assert runtime.audits[-1][0][1] == "crm.outreach.rate"

    ann = _contact(runtime, name="Ann", email="ann@example.com", flow="customers")
    assert _send(runtime, ann)["status"] == 201
    prompt = runtime.prompts[-1]
    assert "The sender rated these 1 or 2 stars (do not write like these):" in prompt
    assert '- 2/5 for "Quick idea for your repos": Too long, and drop the jargon.' in prompt


def test_manual_sends_cannot_be_rated():
    runtime = AiRuntime()
    sam = _contact(runtime, name="Sam Dev", email="sam@example.com", flow="customers")
    logged = call(runtime.use("POST", body={
        "contactId": sam, "to": "sam@example.com", "subject": "Hi",
        "externalId": "provider-1", "status": "delivered"}),
        "/api/crm/acme/emails")["data"]["email"]["id"]
    refused = call(runtime.use("POST", body={"emailId": logged, "rating": 5}),
                   "/api/crm/acme/outreach/rate")
    assert refused["data"]["error"] == "email_not_automated"


class ScopedZohoRuntime(ZohoSendRuntime):
    def __init__(self, drafts=None, scope="", token_ok=True, **kwargs):
        super().__init__(drafts, **kwargs)
        self.scope = scope
        self.token_ok = token_ok

    async def outbound_json(self, url, method="GET", headers=None, body=""):
        if url.endswith("/oauth/v2/token"):
            self.outbound_calls.append((url, method, dict(headers or {}), body))
            if not self.token_ok:
                return {"ok": True, "status": 400, "data": {"error": "invalid_code"}}
            data = {"access_token": "request-local-token"}
            if self.scope:
                data["scope"] = self.scope
            return {"ok": True, "status": 200, "data": data}
        return await super().outbound_json(url, method, headers, body)


def _zoho_status(runtime):
    return call(runtime.use("GET"), "/api/crm/acme/settings")["data"]["settings"]["zoho"]


def _forget_connected_at(runtime):
    raw = runtime.db.execute("SELECT data FROM crm_settings").fetchone()[0]
    settings = json.loads(raw.removeprefix("sealed:"))
    settings["zoho"].pop("connectedAt")
    runtime.db.execute("UPDATE crm_settings SET data=?",
                       ("sealed:" + json.dumps(settings),))


def test_a_connection_made_before_sending_existed_asks_for_a_reconnect_until_zoho_grants_it():
    runtime = _zoho_connected(ScopedZohoRuntime(scope="ZohoMail.messages.READ,ZohoMail.folders.READ"))
    _forget_connected_at(runtime)
    status = _zoho_status(runtime)
    assert status["canSend"] is False
    assert [step["key"] for step in status["steps"]] == ["send", "sync"]
    assert status["steps"][0]["action"] == "reconnect"

    synced = call(runtime.use("POST", body={"limit": 5}), "/api/crm/acme/mail/import")
    assert synced["status"] == 200
    status = synced["data"]["settings"]["zoho"]
    assert status["lastSyncAt"] > 0 and status["canSend"] is False
    assert [step["key"] for step in status["steps"]] == ["send"]

    runtime.scope = "ZohoMail.messages.READ ZohoMail.messages.CREATE ZohoMail.folders.READ"
    runtime.clock += crm.ZOHO_AUTO_SYNC_MS
    status = call(runtime.use("POST", body={"limit": 5}),
                  "/api/crm/acme/mail/import")["data"]["settings"]["zoho"]
    assert status["canSend"] is True and status["steps"] == []


def test_a_refused_send_shows_the_reconnect_step_and_a_new_sign_in_clears_it():
    runtime = _zoho_connected(ScopedZohoRuntime(
        [_draft("Hello")], send_status=404, send_error="INVALID_OAUTHSCOPE"))
    assert _zoho_status(runtime)["canSend"] is True
    sam = _contact(runtime, name="Sam", email="sam@example.com", flow="customers")
    refused = _send(runtime, sam)
    assert refused["data"]["error"] == "zoho_send_not_allowed"
    assert "Reconnect Zoho Mail at the top" in refused["data"]["detail"]
    status = _zoho_status(runtime)
    assert status["canSend"] is False
    assert status["steps"][0]["key"] == "send"
    assert status["steps"][0]["text"].startswith("Zoho refused")

    body = {"zoho": {"region": "com", "accountId": "1", "clientId": "c",
                     "clientSecret": "s", "refreshToken": "new-grant"}}
    saved = call(runtime.use("PUT", body=body), "/api/crm/acme/settings")
    assert saved["data"]["settings"]["zoho"]["canSend"] is True


def test_a_rejected_sign_in_or_failed_sync_becomes_a_step():
    runtime = _zoho_connected(ScopedZohoRuntime(token_ok=False))
    failed = call(runtime.use("POST", body={}), "/api/crm/acme/mail/import")
    assert failed["status"] == 502
    status = _zoho_status(runtime)
    assert status["canSend"] is False and status["lastSyncError"] == "zoho_token_rejected"
    assert [step["key"] for step in status["steps"]] == ["token"]

    runtime.token_ok = True
    runtime.clock += crm.ZOHO_AUTO_SYNC_MS
    status = call(runtime.use("POST", body={}),
                  "/api/crm/acme/mail/import")["data"]["settings"]["zoho"]
    assert status["canSend"] is True and status["steps"] == []


def test_the_page_sync_runs_at_most_every_fifteen_minutes():
    runtime = _zoho_connected(ScopedZohoRuntime())
    first = call(runtime.use("POST", body={"auto": True}), "/api/crm/acme/mail/import")
    assert first["status"] == 200 and not first["data"].get("skipped")
    calls = len(runtime.outbound_calls)
    again = call(runtime.use("POST", body={"auto": True}), "/api/crm/acme/mail/import")
    assert again["data"]["skipped"] is True and len(runtime.outbound_calls) == calls
    manual = call(runtime.use("POST", body={}), "/api/crm/acme/mail/import")
    assert not manual["data"].get("skipped") and len(runtime.outbound_calls) > calls
    runtime.clock += crm.ZOHO_AUTO_SYNC_MS
    later = call(runtime.use("POST", body={"auto": True}), "/api/crm/acme/mail/import")
    assert not later["data"].get("skipped")
