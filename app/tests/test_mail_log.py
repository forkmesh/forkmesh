import asyncio
import json
from pathlib import Path
import sqlite3
import sys


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import chat_channels_api as api  # noqa: E402
import mail_log  # noqa: E402
from test_chat_channels_api import FakeRuntime  # noqa: E402


SECRET_BITS = (
    "https://forkmesh.com/reset?token=",
    "Zm9ya21lc2gtcmVzZXQtdG9rZW4tMTIzNDU2Nzg5",
    "482913",
)


def test_note_shows_addresses_and_scrubs_links_tokens_and_codes():
    entry = mail_log.note(
        "password_reset", "Alice@Example.com",
        "Reset code 482913 https://forkmesh.com/reset?token="
        "Zm9ya21lc2gtcmVzZXQtdG9rZW4tMTIzNDU2Nzg5 for alice@example.com",
        "rejected", status=403,
        error="Token Zm9ya21lc2gtcmVzZXQtdG9rZW4tMTIzNDU2Nzg5 rejected",
        at=1_790_000_000_000)
    text = mail_log.line(entry)
    for secret in SECRET_BITS:
        assert secret not in text
    assert "📨 To: alice@example.com" in text
    assert "for alice@example.com" in text
    assert text.startswith(
        "🔑 Password reset · ⛔ REJECTED · 2026-09-21 ")
    assert "\n⚠️ Reason: HTTP 403: " in text
    assert "[link]" in text and "[code]" in text


def test_addresses_and_message_id_are_shown_in_full():
    entry = mail_log.note(
        "outside_app", "Jett@ForkMesh.com", "", "delivered", at=1,
        message_id="<172755101234.4242.9876543210123456789@forkmesh.com>",
        node="n1", sender="no-reply@forkmesh.com", via="postfix")
    assert mail_log.line(entry) == (
        "📮 Not sent by the ForkMesh app · 📥 delivered"
        " · 1970-01-01 00:00:00 UTC\n"
        "📨 To: jett@forkmesh.com\n"
        "📤 From: no-reply@forkmesh.com\n"
        "🛤️ Route: local mail (Postfix on the core) on n1\n"
        "🧰 Source: no app send record: a tool, a cron job or a bounce\n"
        "🆔 Message ID: 172755101234.4242.9876543210123456789"
        "@forkmesh.com")
    outside = mail_log.note("account", "a@b.co", "", "accepted",
                            sender="Root@Example.org")
    assert mail_log.line(outside).endswith(
        "\n📤 From: root@example.org")


def test_note_without_body_or_budget_carries_neither():
    entry = mail_log.note("security", "a@b.co", "IDS alert", "suppressed")
    assert set(entry) <= {"at", "kind", "to", "subject", "result", "status",
                          "error", "messageId"}
    assert "suppressed" in mail_log.line(entry)
    assert "IDS alert" in mail_log.line(entry)


def test_body_excerpt_and_budget_are_shown_and_scrubbed():
    body = mail_log.excerpt(
        "Your code is 482913.\nReset: https://forkmesh.com/reset?token="
        "Zm9ya21lc2gtcmVzZXQtdG9rZW4tMTIzNDU2Nzg5 " + "word " * 80)
    entry = mail_log.note(
        "password_reset", "a@b.co", "Reset", "suppressed",
        error="daily mail budget spent", body=body,
        budget="150 of 150 sent today (UTC)")
    text = mail_log.line(entry)
    for secret in SECRET_BITS:
        assert secret not in text
    assert "\n📄 Body: Your code is [code]. Reset: [link] " in text
    assert len(entry["body"]) == mail_log.BODY_LIMIT
    assert text.startswith(
        "🔑 Password reset · 🚫 suppressed · ")
    assert ("\n⚠️ Reason: daily mail budget spent\n"
            "📊 Budget: 150 of 150 sent today (UTC)") in text
    html = mail_log.note(
        "package", "a@b.co", "Hi", "accepted", body="",
        body_html="<html><head><style>p{}</style></head>"
                  "<body><p>Hello &amp; welcome</p></body>")
    assert html["body"] == "Hello & welcome"


def test_compose_splits_lines_into_bounded_messages():
    entries = [mail_log.note("account", "u%d@x.io" % n, "S" * 100,
                             "accepted", at=n, body="word " * 30)
               for n in range(400)]
    messages = mail_log.compose(entries, dropped=3)
    assert len(messages) > 1
    assert all(len(text) <= mail_log.MAX_TEXT for text, _, _ in messages)
    assert all(len(blocks) <= mail_log.MAX_BLOCKS
               for _, _, blocks in messages)
    assert sum(count for _, count, _ in messages) == 400
    assert "3 older notes dropped" in messages[-1][0]
    assert messages[-1][2][-1] == {
        "type": "context",
        "text": "3 older notes dropped: the log backlog was over 1000."}
    assert sum(len(text.split("\n\n")) for text, _, _ in messages) == 401
    sections = [block for _, _, blocks in messages for block in blocks
                if block["type"] == "section"]
    assert len(sections) == 400
    assert sections[7]["text"].startswith(
        "🔔 Account notification · ✅ accepted")
    assert sections[7]["fields"][0] == "📨 To: u7@x.io"
    assert all(len(block["fields"]) <= 10 and
               all(len(field) <= 300 for field in block["fields"])
               for block in sections)


def test_webhook_follow_ups_map_delivery_bounce_and_complaint():
    base = {"email": "bob@example.org", "message_id": "m-1",
            "category": "Password reset", "event_id": "e"}
    delivered = mail_log.webhook_note(dict(base, event="delivery"), 5)
    bounced = mail_log.webhook_note(dict(
        base, event="bounce", response="550 mailbox unavailable",
        response_code=550), 5)
    complained = mail_log.webhook_note(dict(base, event="spam"), 5)
    assert mail_log.webhook_note(dict(base, event="open"), 5) is None
    assert delivered["result"] == "delivered"
    assert bounced["result"] == "bounced" and bounced["status"] == 550
    assert complained["result"] == "complained"
    assert "Password reset" in mail_log.line(delivered)
    assert "\n🛤️ Route: Mailtrap\n" in mail_log.line(delivered)
    assert "\n📨 To: bob@example.org\n" in mail_log.line(bounced)
    assert "COMPLAINED" in mail_log.line(complained)


class _Env:
    MAIL_LOG_OWNER = "jett"


class _Store:
    def __init__(self):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.db.executescript(
            (ROOT / "migrations" / "0069_private_chat_channels.sql")
            .read_text(encoding="utf-8")
            + (ROOT / "migrations" / "0209_mail_log_outbox.sql")
            .read_text(encoding="utf-8"))
        self.fail_insert = False

    async def d1_all(self, _env, sql, *args):
        return [dict(r) for r in self.db.execute(sql, args).fetchall()]

    async def d1_first(self, _env, sql, *args):
        row = self.db.execute(sql, args).fetchone()
        return dict(row) if row is not None else None

    async def d1_run(self, _env, sql, *args):
        if self.fail_insert and sql.startswith("INSERT INTO mail_log_outbox"):
            raise RuntimeError("d1 down")
        self.db.execute(sql, args)
        self.db.commit()


def _bind(store):
    ids = iter(range(1, 10_000))

    async def encrypt_row(_env, value):
        return "sealed:" + json.dumps(value)

    async def decrypt_row(_env, value):
        return json.loads(value[len("sealed:"):])

    async def blind_index(_env, value):
        return "bi:" + value

    async def account_row(_env, name):
        return "bi:" + name, {"status": "active", "kind": "user",
                              "email": name + "@gmail.com"}

    async def notify_account_event(_env, _owner, _topic):
        return None

    names = {
        "d1_all": store.d1_all, "d1_first": store.d1_first,
        "d1_run": store.d1_run, "encrypt_row": encrypt_row,
        "decrypt_row": decrypt_row, "blind_index": blind_index,
        "_account_row": account_row, "_account_kind": lambda rec: "user",
        "notify_account_event": notify_account_event,
        "_ap_uuid": lambda: "%032x" % next(ids),
    }
    for name, value in names.items():
        setattr(mail_log, name, value)


def test_failed_post_keeps_notes_for_retry_and_never_raises():
    store = _Store()
    _bind(store)
    env = _Env()

    async def scenario():
        assert await mail_log.enqueue(env, mail_log.note(
            "verification", "a@b.co", "Welcome", "accepted", at=1))

        async def broken(*_args):
            raise RuntimeError("room down")

        first = await mail_log.drain(env, broken)
        assert first == {"posted": 0, "retry": 1}
        row = store.db.execute(
            "SELECT attempts,next_at FROM mail_log_outbox").fetchone()
        assert row["attempts"] == 1 and row["next_at"] > 1
        store.db.execute("UPDATE mail_log_outbox SET next_at=0")
        posted = []

        async def deliver(channel_id, key_version, plain):
            posted.append((channel_id, key_version, plain))

        second = await mail_log.drain(env, deliver)
        assert second["posted"] == 1
        assert store.db.execute(
            "SELECT COUNT(*) FROM mail_log_outbox").fetchone()[0] == 0
        channel_id, key_version, plain = posted[0]
        assert key_version == 1
        assert plain["senderId"] == "system:mail-log"
        assert "\n📨 To: a@b.co" in plain["text"]
        assert plain["blocks"] == [mail_log.section(mail_log.note(
            "verification", "a@b.co", "Welcome", "accepted", at=1))]
        channel = store.db.execute(
            "SELECT data,created_by_bi FROM chat_channels").fetchone()
        record = json.loads(channel["data"][len("sealed:"):])
        assert record["kind"] == "notices"
        assert record["visibility"] == "private"
        assert channel["created_by_bi"] == "bi:jett"
        await mail_log.enqueue(env, mail_log.note(
            "account", "jett@gmail.com", "Hi", "accepted", at=2))
        await mail_log.drain(env, deliver)
        assert posted[1][0] == channel_id
        assert "jett@gmail.com" in posted[1][2]["text"]
        store.fail_insert = True
        assert await mail_log.enqueue(env, mail_log.note(
            "account", "x@y.z", "Hi", "accepted")) is False

    asyncio.run(scenario())


def test_a_long_backlog_posts_several_messages_and_retries_only_the_rest():
    store = _Store()
    _bind(store)
    env = _Env()

    async def scenario():
        for n in range(40):
            assert await mail_log.enqueue(env, mail_log.note(
                "account", "u%d@x.io" % n, "S" * 100, "accepted", at=1 + n,
                body="word " * 38))
        posted = []

        async def flaky(channel_id, key_version, plain):
            if len(posted) == 1:
                raise RuntimeError("room down")
            posted.append(plain)

        first = await mail_log.drain(env, flaky)
        sent = len(posted[0]["blocks"])
        assert sent == len(posted[0]["text"].split("\n\n"))
        assert first == {"posted": sent, "retry": 40 - sent}
        assert store.db.execute(
            "SELECT COUNT(*) FROM mail_log_outbox").fetchone()[0] == 40 - sent
        store.db.execute("UPDATE mail_log_outbox SET next_at=0")

        async def deliver(channel_id, key_version, plain):
            posted.append(plain)

        second = await mail_log.drain(env, deliver)
        assert second["posted"] == 40 - sent
        sections = [block for plain in posted for block in plain["blocks"]]
        assert len(sections) == 40
        assert len({block["fields"][0] for block in sections}) == 40
        assert store.db.execute(
            "SELECT COUNT(*) FROM mail_log_outbox").fetchone()[0] == 0

    asyncio.run(scenario())


def test_without_an_owner_nothing_is_queued():
    store = _Store()
    _bind(store)

    class NoOwner:
        MAIL_LOG_OWNER = ""

    assert asyncio.run(mail_log.enqueue(NoOwner(), mail_log.note(
        "account", "a@b.co", "s", "accepted"))) is False


def _notices_runtime():
    runtime = FakeRuntime()
    runtime.admins = {"admin"}
    runtime.accounts = {"admin", "alice", "bob"}
    record = {"name": "email-log", "createdBy": "alice",
              "visibility": "private", "kind": "notices",
              "notices": "email-log", "topic": "Server-visible notices"}
    runtime.db.execute(
        "INSERT INTO chat_channels (channel_id,name_bi,data,created_by_bi,"
        "created_at,updated_at,key_version) VALUES (?,?,?,?,?,?,1)",
        ("a" * 32, "notices-bi", json.dumps(record, sort_keys=True),
         "bi:alice", 1, 1))
    runtime.db.commit()
    return runtime


def _call(runtime, method, path, actor, data=None):
    runtime.use(method, actor, data)
    return asyncio.run(api.handle(runtime, path))


def test_only_the_owner_sees_the_notices_channel_not_even_admins():
    runtime = _notices_runtime()
    base = "/api/chat/channels/" + "a" * 32
    owner = _call(runtime, "GET", "/api/chat/channels", "alice")
    channels = owner["data"]["channels"]
    assert [c["name"] for c in channels] == ["email-log"]
    assert channels[0]["serverVisible"] is True
    assert channels[0]["readOnly"] is True
    assert channels[0]["canManage"] is False
    assert _call(runtime, "GET", base + "/room-access", "alice")[
        "status"] == 200
    assert _call(runtime, "GET", base + "/history", "alice")["status"] == 200
    for other in ("admin", "bob"):
        listed = _call(runtime, "GET", "/api/chat/channels", other)
        assert listed["data"]["channels"] == []
        for suffix in ("/room-access", "/history", "/members",
                       "/webhooks"):
            response = _call(runtime, "GET", base + suffix, other)
            assert response["status"] == 404, (other, suffix)
        assert _call(runtime, "PATCH", base, other, {"topic": "x"})[
            "status"] == 404
        assert _call(runtime, "DELETE", base, other, {})["status"] == 404
        assert _call(runtime, "POST", base + "/members", other,
                     {"username": other})["status"] == 404
    assert not [a for a in runtime.audits if a["target"] == "a" * 32]


def test_owner_cannot_open_the_notices_channel_to_anyone():
    runtime = _notices_runtime()
    base = "/api/chat/channels/" + "a" * 32
    for method, path, data in (
            ("PATCH", base, {"visibility": "public"}),
            ("DELETE", base, {}),
            ("POST", base + "/members", {"username": "bob"}),
            ("POST", base + "/webhooks", {"name": "leak"})):
        response = _call(runtime, method, path, "alice", data)
        assert response["status"] == 403, (method, path)
        assert response["data"]["error"] == "notices_channel_read_only"
    record = json.loads(runtime.db.execute(
        "SELECT data FROM chat_channels").fetchone()[0])
    assert record["visibility"] == "private"
    assert runtime.db.execute(
        "SELECT COUNT(*) FROM chat_channel_members").fetchone()[0] == 0


def test_app_mail_names_the_transport():
    entry = mail_log.note("verification", "a@b.co", "Hi", "accepted",
                          node="n1", via="postfix")
    assert "\n🛤️ Route: local mail (Postfix on the core) on n1" in (
        mail_log.line(entry))
    entry = mail_log.note("verification", "a@b.co", "Hi", "accepted",
                          via="mailtrap")
    assert "\n🛤️ Route: Mailtrap API" in mail_log.line(entry)


def test_a_tool_note_from_a_core_carries_its_body_and_source():
    entry = mail_log.node_note({
        "kind": "outside_app", "via": "postfix", "result": "accepted",
        "to": "jett@forkmesh.com", "from": "no-reply@forkmesh.com",
        "subject": "Ad hoc iPhone build: ForkMesh 1.4.0 (18) at c3f94cc775e6",
        "body": "ForkMesh 1.4.0 (18) built on the local Mac. Install: "
                "https://forkmesh.com/ios-install.html#abc",
        "messageId": "<179066419708.2521672.2998348115286080844@forkmesh.com>",
        "source": "the Mac ad hoc build tool"},
        "n1", 1)
    assert mail_log.line(entry) == (
        "📮 Not sent by the ForkMesh app · ✅ accepted"
        " · 1970-01-01 00:00:00 UTC\n"
        "📨 To: jett@forkmesh.com\n"
        "📤 From: no-reply@forkmesh.com\n"
        "📝 Subject: Ad hoc iPhone build: ForkMesh 1.4.0 (18) at "
        "c3f94cc775e6\n"
        "📄 Body: ForkMesh 1.4.0 (18) built on the local Mac. "
        "Install: [link]\n"
        "🛤️ Route: local mail (Postfix on the core) on n1\n"
        "🧰 Source: sent by the Mac ad hoc build tool\n"
        "🆔 Message ID: 179066419708.2521672.2998348115286080844"
        "@forkmesh.com")


def test_a_delivery_follow_up_repeats_the_subject_and_body_of_its_send():
    event = {"email": "bob@example.org", "message_id": "m-1",
             "category": "Password reset", "event": "bounce",
             "response": "550 mailbox unavailable", "response_code": 550}
    note = mail_log.webhook_note(
        event, 5, subject="Reset your ForkMesh password",
        body="Hi bob, use this link to pick a new password.")
    text = mail_log.line(note)
    assert "\n📝 Subject: Reset your ForkMesh password\n" in text
    assert ("\n📄 Body: Hi bob, use this link to pick a new "
            "password.\n") in text
    bare = mail_log.webhook_note(event, 5)
    assert "Subject" not in mail_log.line(bare)
    assert "Body" not in mail_log.line(bare)


def test_every_kind_and_result_has_its_own_emoji():
    assert set(mail_log.KIND_EMOJI) == set(mail_log.KIND_LABELS)
    assert set(mail_log.RESULT_EMOJI) == set(mail_log.RESULTS)
    assert len(set(mail_log.KIND_EMOJI.values())) == len(
        mail_log.KIND_EMOJI)
    assert len(set(mail_log.RESULT_EMOJI.values())) == len(
        mail_log.RESULT_EMOJI)
    assert len(set(mail_log.FIELD_EMOJI.values())) == len(
        mail_log.FIELD_EMOJI)
    assert mail_log.DEFAULT_KIND_EMOJI not in set(
        mail_log.KIND_EMOJI.values())
    unknown = mail_log.note("made_up_kind", "a@b.co", "Hi", "accepted", at=1)
    assert mail_log.headline(unknown).startswith(
        "✉️ made_up_kind · ✅ accepted")


def test_every_field_label_carries_an_emoji():
    outside = mail_log.note("outside_app", "a@b.co", "", "accepted",
                            source="a tool")
    assert set(mail_log.FIELD_EMOJI) >= {
        label for label, _ in mail_log.fields(outside)}
    entry = mail_log.note(
        "task", "a@b.co", "Subject", "soft_bounced", status=451,
        error="greylisted", at=1, message_id="m-1", sender="x@y.z",
        via="mailtrap", node="n1", body="Body text", budget="1 of 2",
        detail="detail text")
    labels = [field.split(":", 1)[0] for field in mail_log.section(entry)[
        "fields"]]
    assert labels == [
        "📨 To", "📤 From", "📝 Subject",
        "📄 Body", "⚠️ Reason", "🔍 Detail",
        "📊 Budget", "🛤️ Route", "🆔 Message ID"]
    assert mail_log.headline(entry).startswith(
        "🗂️ Task activity · ↪️ soft-bounced")


def test_a_node_note_cannot_claim_a_mailtrap_transport():
    entry = mail_log.node_note({"result": "accepted", "via": "mailtrap"},
                               "r1", 1)
    assert entry["via"] == "Mailtrap API, IDS direct"
