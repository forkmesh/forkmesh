import html
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
