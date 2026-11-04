import importlib.util
import json
from pathlib import Path
from urllib.parse import parse_qs, urlparse


spec = importlib.util.spec_from_file_location(
    "crm_test_harness", Path(__file__).with_name("test_crm.py"))
harness = importlib.util.module_from_spec(spec)
spec.loader.exec_module(harness)
call = harness.call
crm = harness.crm


class OutreachRuntime(harness.MastodonRuntime):
    def __init__(self, locked=(), following=(), requested=(), mentions=()):
        super().__init__()
        self.locked = set(locked)
        self.follows = set(following)
        self.requested = set(requested)
        self.mentions = list(mentions)
        self.posted = []

    async def outbound_json(self, url, method="GET", headers=None, body=""):
        parsed = urlparse(url)
        query = parse_qs(parsed.query)
        if parsed.path == "/api/v1/accounts/lookup" and query["acct"] != ["forkmesh"]:
            self.outbound_calls.append((url, method, dict(headers or {}), body))
            acct = query["acct"][0]
            return {"ok": True, "status": 200,
                    "data": {"id": "id-" + acct, "acct": acct, "locked": acct in self.locked}}
        if parsed.path == "/api/v1/accounts/relationships":
            self.outbound_calls.append((url, method, dict(headers or {}), body))
            acct = query["id[]"][0].removeprefix("id-")
            return {"ok": True, "status": 200, "data": [{
                "id": query["id[]"][0], "following": acct in self.follows,
                "requested": acct in self.requested}]}
        if parsed.path == "/api/v1/statuses" and method == "POST":
            self.outbound_calls.append((url, method, dict(headers or {}), body))
            self.posted.append((json.loads(body), dict(headers or {})))
            status_id = str(117558000000000000 + len(self.posted))
            return {"ok": True, "status": 200, "data": {
                "id": status_id, "url": "https://mastodon.social/@forkmesh/" + status_id,
                "created_at": "2033-05-18T03:33:20.000Z", "visibility": "direct",
                "spoiler_text": "", "content": "<p>" + json.loads(body)["status"] + "</p>",
                "account": {"acct": "forkmesh"}, "mentions": []}}
        if parsed.path == "/api/v1/notifications":
            self.outbound_calls.append((url, method, dict(headers or {}), body))
            limit = int(query.get("limit", ["40"])[0])
            notes = sorted(self.mentions, key=lambda item: int(item["id"]))
            if "min_id" in query:
                notes = [item for item in notes if int(item["id"]) > int(query["min_id"][0])]
                notes = notes[:limit]
            else:
                notes = notes[-limit:]
            return {"ok": True, "status": 200, "data": list(reversed(notes))}
        return await super().outbound_json(url, method, headers, body)


def _connect(runtime):
    assert call(runtime.use("PUT", body={"mastodon": {
        "account": "@forkmesh@mastodon.social", "token": "secret-token"}}),
        "/api/crm/acme/settings")["status"] == 200


def _contact(runtime, **body):
    return call(runtime.use("POST", body=body), "/api/crm/acme/contacts")["data"]["contact"]


def _fields(runtime, contact_id):
    return call(runtime.use("GET"), f"/api/crm/acme/contacts/{contact_id}")[
        "data"]["contact"]


def _drafts(runtime, *items, actor="alice"):
    return call(runtime.use("POST", actor=actor, body={"drafts": list(items)}),
                "/api/crm/acme/mastodon/drafts")


DRAFT = ("@ada@hachyderm.io Hi Ada, saw your release. ForkMesh keeps Git repos and "
         "releases mirrored and signed. 15 minutes? https://forkmesh.com/meet?alice/interview")


def test_mastodon_length_counts_links_and_remote_mentions_like_mastodon():
    text = "@ada@hachyderm.io see https://example.org/" + "a" * 80
    assert crm._mastodon_length(text) == len("@ada see ") + 23
    assert crm._mastodon_length("plain") == 5


def test_drafts_create_new_contacts_and_update_known_ones_by_handle():
    runtime = OutreachRuntime()
    known = _contact(runtime, name="Grace", mastodon="@grace@mastodon.social",
                     notes="Old note")
    saved = _drafts(runtime, {
        "handle": "@ada@hachyderm.io", "name": "Ada", "draft": DRAFT, "why": "New release",
        "segment": "tor", "ask": "interview", "email": "ada@example.org",
        "notes": "Found on her site", "tags": ["internet-freedom"]}, {
        "handle": "https://mastodon.social/@grace", "draft": "@grace@mastodon.social Hi",
        "why": "Talk", "segment": "nonsense", "ask": "letter", "status": "follow-first",
        "notes": "Locked account"}, {
        "handle": "not a handle", "draft": "x"}, {
        "handle": "@long@mastodon.social", "draft": "x" * 501})
    assert saved["status"] == 200
    data = saved["data"]
    assert (data["created"], data["updated"]) == (1, 1)
    assert [item["error"] for item in data["skipped"]] == ["invalid_handle", "toot_too_long"]
    ada = _fields(runtime, data["ids"][0])
    assert (ada["name"], ada["mastodon"], ada["email"], ada["flow"], ada["stage"]) == (
        "Ada", "@ada@hachyderm.io", "ada@example.org", "customers", "lead")
    assert ada["source"].startswith("Mastodon research ")
    assert ada["fields"]["mastodon_status"] == "ready"
    assert ada["fields"]["mastodon_segment"] == "tor"
    assert ada["fields"]["mastodon_draft"] == DRAFT
    assert ada["fields"]["mastodon_prepared"].endswith(" alice")
    grace = _fields(runtime, known["id"])
    assert grace["fields"]["mastodon_status"] == "follow-first"
    assert grace["fields"]["mastodon_segment"] == "other"
    assert grace["fields"]["mastodon_ask"] == "letter"
    assert grace["notes"].startswith("Old note\n\n[") and grace["notes"].endswith("Locked account")
    assert call(runtime.use("POST", body={"drafts": [{}] * 51}),
                "/api/crm/acme/mastodon/drafts")["data"]["error"] == "too_many_drafts"


def test_a_draft_never_reopens_a_contact_we_already_tooted():
    runtime = OutreachRuntime()
    sent = _contact(runtime, name="Sent", mastodon="@sent@mastodon.social",
                    fields={"mastodon_status": "sent"})
    _drafts(runtime, {"handle": "@sent@mastodon.social", "draft": "@sent@mastodon.social again"})
    fields = _fields(runtime, sent["id"])["fields"]
    assert fields["mastodon_status"] == "sent"
    assert fields["mastodon_draft"] == "@sent@mastodon.social again"


def test_outreach_fields_never_make_a_contact_match_another_handle():
    runtime = OutreachRuntime()
    ada = _contact(runtime, name="Ada", mastodon="@ada@hachyderm.io",
                   fields={"mastodon_draft": "@ada@hachyderm.io we also wrote @grace@mastodon.social"})
    grace = _drafts(runtime, {"handle": "@grace@mastodon.social", "draft": "@grace@mastodon.social Hi"})
    assert grace["data"]["created"] == 1
    assert grace["data"]["ids"][0] != ada["id"]


def test_outreach_page_lists_the_queue_counts_and_a_prepare_prompt():
    runtime = OutreachRuntime()
    _drafts(runtime,
            {"handle": "@ada@hachyderm.io", "draft": DRAFT, "segment": "tor", "prepared": "2026-11-05 alice"},
            {"handle": "@bo@mastodon.social", "draft": "@bo@mastodon.social Hi", "segment": "researcher",
             "prepared": "2026-11-04 alice"},
            {"handle": "@cy@mastodon.social", "draft": "@cy@mastodon.social Hi", "status": "follow-first"})
    skipped = _drafts(runtime, {"handle": "@dee@mastodon.social", "draft": "@dee@mastodon.social Hi"})
    call(runtime.use("PATCH", body={"fields": {"mastodon_status": "skipped"}}),
         f"/api/crm/acme/contacts/{skipped['data']['ids'][0]}")
    page = call(runtime.use("GET", query={"tz": "0"}), "/api/crm/acme/mastodon")
    assert page["status"] == 200
    data = page["data"]
    assert [item["handle"] for item in data["queue"]] == [
        "bo@mastodon.social", "ada@hachyderm.io", "cy@mastodon.social"]
    assert data["queue"][1]["mastodon_draft"] == DRAFT
    assert (data["dailyTarget"], data["dailyLimit"], data["canSend"]) == (25, 40, False)
    assert data["stats"]["today"] == {"sent": 0, "replied": 0}
    assert {row["key"]: row["prepared"] for row in data["stats"]["segments"]} == {
        "tor": 1, "researcher": 1, "other": 2}
    assert "crm_save_mastodon_drafts" in data["prompt"]
    assert "Mastodon outreach strategy" in data["prompt"]
    assert "3 toots ready" not in data["prompt"] and "2 toots ready in the queue" in data["prompt"]
    _connect(runtime)
    assert call(runtime.use("GET"), "/api/crm/acme/mastodon")["data"]["canSend"] is True


def test_send_posts_a_direct_toot_links_it_and_marks_the_contact():
    runtime = OutreachRuntime()
    ada = _drafts(runtime, {"handle": "@ada@hachyderm.io", "draft": DRAFT})["data"]["ids"][0]
    missing = call(runtime.use("POST", body={"contactId": ada}), "/api/crm/acme/mastodon/send")
    assert (missing["status"], missing["data"]["error"]) == (409, "mastodon_not_connected")
    _connect(runtime)
    member = call(runtime.use("POST", actor="charlie", body={"contactId": ada}),
                  "/api/crm/acme/mastodon/send")
    assert (member["status"], member["data"]["error"]) == (403, "outreach_forbidden")
    edited = DRAFT.replace("saw your release", "saw your new release")
    sent = call(runtime.use("POST", body={"contactId": ada, "text": edited}),
                "/api/crm/acme/mastodon/send")
    assert sent["status"] == 201, sent
    body, headers = runtime.posted[0]
    assert body == {"status": edited, "visibility": "direct"}
    assert headers["authorization"] == "Bearer secret-token"
    assert headers["idempotency-key"].startswith("crm-" + ada + "-")
    contact = _fields(runtime, ada)
    assert contact["stage"] == "contacted" and contact["lastContactedAt"] > 0
    assert contact["fields"]["mastodon_status"] == "sent"
    assert contact["fields"]["mastodon_draft"] == edited
    rows = call(runtime.use("GET"), "/api/crm/acme/emails")["data"]["emails"]
    assert [(row["provider"], row["direction"], row["contactId"]) for row in rows] == [
        ("mastodon", "outbound", ada)]
    page = call(runtime.use("GET", query={"tz": "0"}), "/api/crm/acme/mastodon")["data"]
    assert page["queue"] == []
    assert page["stats"]["last24h"] == 1


def test_send_adds_the_handle_and_refuses_long_toots():
    runtime = OutreachRuntime()
    _connect(runtime)
    bo = _drafts(runtime, {"handle": "@bo@mastodon.social", "draft": "@bo@mastodon.social Hi"})["data"]["ids"][0]
    long = call(runtime.use("POST", body={"contactId": bo, "text": "y" * 600}),
                "/api/crm/acme/mastodon/send")
    assert (long["status"], long["data"]["error"]) == (400, "toot_too_long")
    call(runtime.use("POST", body={"contactId": bo, "text": "Hello there"}),
         "/api/crm/acme/mastodon/send")
    assert runtime.posted[0][0]["status"] == "@bo@mastodon.social Hello there"


def test_a_locked_account_we_do_not_follow_asks_to_follow_first():
    runtime = OutreachRuntime(locked={"cy@mastodon.social", "dee@mastodon.social"},
                              requested={"dee@mastodon.social"})
    _connect(runtime)
    ids = _drafts(runtime, {"handle": "@cy@mastodon.social", "draft": "@cy@mastodon.social Hi"},
                  {"handle": "@dee@mastodon.social", "draft": "@dee@mastodon.social Hi"})["data"]["ids"]
    for contact_id, status in zip(ids, ("follow-first", "follow-requested")):
        refused = call(runtime.use("POST", body={"contactId": contact_id}),
                       "/api/crm/acme/mastodon/send")
        assert (refused["status"], refused["data"]["error"]) == (409, "follow_first")
        assert _fields(runtime, contact_id)["fields"]["mastodon_status"] == status
    assert runtime.posted == []
    runtime.follows.add("cy@mastodon.social")
    assert call(runtime.use("POST", body={"contactId": ids[0]}),
                "/api/crm/acme/mastodon/send")["status"] == 201


def test_the_daily_limit_stops_more_toots(monkeypatch):
    monkeypatch.setattr(crm, "MASTODON_DAILY_LIMIT", 1)
    runtime = OutreachRuntime()
    _connect(runtime)
    ids = _drafts(runtime, {"handle": "@a1@mastodon.social", "draft": "@a1@mastodon.social Hi"},
                  {"handle": "@a2@mastodon.social", "draft": "@a2@mastodon.social Hi"})["data"]["ids"]
    assert call(runtime.use("POST", body={"contactId": ids[0]}),
                "/api/crm/acme/mastodon/send")["status"] == 201
    stopped = call(runtime.use("POST", body={"contactId": ids[1]}), "/api/crm/acme/mastodon/send")
    assert (stopped["status"], stopped["data"]["error"]) == (429, "mastodon_daily_limit")


def test_a_reply_marks_the_toot_replied_and_moves_the_stage_on():
    runtime = OutreachRuntime()
    _connect(runtime)
    ada = _drafts(runtime, {"handle": "@ada@hachyderm.io", "draft": DRAFT})["data"]["ids"][0]
    call(runtime.use("POST", body={"contactId": ada}), "/api/crm/acme/mastodon/send")
    reply = call(runtime.use("POST", body={
        "url": "https://mastodon.social/@ada@hachyderm.io/117558111111111111",
        "contactId": ada, "direction": "inbound"}), "/api/crm/acme/emails/toot")
    assert reply["status"] == 201, reply
    assert reply["data"]["outreach"] == "replied"
    contact = _fields(runtime, ada)
    assert (contact["stage"], contact["fields"]["mastodon_status"]) == ("qualified", "replied")
    page = call(runtime.use("GET", query={"tz": "0"}), "/api/crm/acme/mastodon")["data"]
    assert {row["key"]: (row["sent"], row["replied"]) for row in page["stats"]["segments"]} == {
        "other": (1, 1)}


def test_a_toot_logged_from_outside_the_crm_takes_the_draft_off_the_queue():
    runtime = OutreachRuntime()
    ada = _drafts(runtime, {"handle": "@ada@hachyderm.io", "draft": DRAFT})["data"]["ids"][0]
    logged = call(runtime.use("POST", body={
        "url": "https://mastodon.social/@forkmesh/117558222222222222", "contactId": ada}),
        "/api/crm/acme/emails/toot")
    assert logged["data"]["outreach"] == "sent"
    assert _fields(runtime, ada)["fields"]["mastodon_status"] == "sent"


def test_toot_sync_logs_mentions_from_known_contacts_as_replies():
    mention = {"id": "900", "type": "mention", "status": {
        "id": "117558333333333333", "url": "https://hachyderm.io/@ada/117558333333333333",
        "created_at": "2033-05-18T03:40:00.000Z", "spoiler_text": "",
        "content": "<p>@forkmesh sure, happy to talk</p>",
        "account": {"acct": "ada@hachyderm.io"}, "mentions": [{"acct": "forkmesh"}]}}
    stranger = {"id": "901", "type": "mention", "status": dict(
        mention["status"], id="117558333333333334", account={"acct": "who@example.org"})}
    runtime = OutreachRuntime(mentions=[mention, stranger])
    _connect(runtime)
    ada = _drafts(runtime, {"handle": "@ada@hachyderm.io", "draft": DRAFT})["data"]["ids"][0]
    call(runtime.use("POST", body={"contactId": ada}), "/api/crm/acme/mastodon/send")
    synced = call(runtime.use("POST", body={}), "/api/crm/acme/emails/toots")
    assert synced["data"]["replies"] == 1, synced
    notifications = next(item for item in runtime.outbound_calls if "/notifications?" in item[0])
    assert "types[]=mention" in notifications[0]
    assert _fields(runtime, ada)["fields"]["mastodon_status"] == "replied"
    runtime.outbound_calls.clear()
    call(runtime.use("POST", body={}), "/api/crm/acme/emails/toots")
    assert "min_id=901" in next(item[0] for item in runtime.outbound_calls
                                  if "/notifications?" in item[0])


def _mention(number, acct, status_id):
    return {"id": str(number), "type": "mention", "status": {
        "id": str(status_id), "url": "https://hachyderm.io/@%s/%d" % (acct.split("@")[0], status_id),
        "created_at": "2033-05-18T03:40:00.000Z", "spoiler_text": "",
        "content": "<p>@forkmesh hello</p>", "visibility": "direct",
        "account": {"acct": acct}, "mentions": [{"acct": "forkmesh"}]}}


def test_toot_sync_pages_through_a_burst_of_mentions_oldest_first():
    runtime = OutreachRuntime(mentions=[_mention(100, "who@example.org", 117558000000000100)])
    _connect(runtime)
    ada = _drafts(runtime, {"handle": "@ada@hachyderm.io", "draft": DRAFT})["data"]["ids"][0]
    call(runtime.use("POST", body={"contactId": ada}), "/api/crm/acme/mastodon/send")
    call(runtime.use("POST", body={}), "/api/crm/acme/emails/toots")
    runtime.mentions = [_mention(100, "who@example.org", 117558000000000100),
                        _mention(101, "ada@hachyderm.io", 117558000000000101)] + [
        _mention(102 + n, "who@example.org", 117558000000000102 + n)
        for n in range(crm.MASTODON_MENTION_PAGE + 10)]
    runtime.outbound_calls.clear()
    synced = call(runtime.use("POST", body={}), "/api/crm/acme/emails/toots")
    assert synced["data"]["replies"] == 1, synced
    pages = [item[0] for item in runtime.outbound_calls if "/notifications?" in item[0]]
    assert len(pages) == 2 and "min_id=100" in pages[0]
    assert _fields(runtime, ada)["fields"]["mastodon_status"] == "replied"


def test_a_toot_to_a_contact_with_no_outreach_status_still_counts_its_reply():
    runtime = OutreachRuntime()
    ada = _contact(runtime, name="Ada", mastodon="@ada@hachyderm.io", flow="customers",
                   stage="contacted")["id"]
    sent = call(runtime.use("POST", body={
        "url": "https://mastodon.social/@forkmesh/117558444444444444", "contactId": ada}),
        "/api/crm/acme/emails/toot")
    assert sent["data"]["outreach"] == "sent"
    reply = call(runtime.use("POST", body={
        "url": "https://hachyderm.io/@ada/117558555555555555",
        "contactId": ada, "direction": "inbound"}), "/api/crm/acme/emails/toot")
    assert reply["data"]["outreach"] == "replied"
    contact = _fields(runtime, ada)
    assert (contact["stage"], contact["fields"]["mastodon_status"]) == ("qualified", "replied")


def test_the_leaderboard_counts_toots_apart_from_emails():
    runtime = OutreachRuntime()
    _connect(runtime)
    ada = _drafts(runtime, {"handle": "@ada@hachyderm.io", "draft": DRAFT})["data"]["ids"][0]
    call(runtime.use("POST", body={"contactId": ada}), "/api/crm/acme/mastodon/send")
    activity = call(runtime.use("GET"), "/api/crm/acme")["data"]["activity"]
    assert activity["kinds"] == ["emails", "outreach", "contacts", "toots"]
    alice = next(item for item in activity["members"] if item["name"] == "alice")
    assert (sum(alice["toots"]), sum(alice["emails"])) == (1, 0)
