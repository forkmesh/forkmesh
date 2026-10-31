import asyncio
import hashlib
import json
import sqlite3

import pytest

import mail_activity
import mail_log
import schema

NOW = 1_800_000_000_000
SEND_ID = "0123456789abcdef0123456789abcdef"


class _MailLog:
    def __init__(self):
        self.notes = []

    @staticmethod
    def note(*args, **kwargs):
        return mail_log.note(*args, **kwargs)

    @staticmethod
    def excerpt(*args, **kwargs):
        return mail_log.excerpt(*args, **kwargs)

    async def enqueue(self, _env, entry):
        self.notes.append(entry)
        return True


class _Follower:
    def __init__(self):
        self.calls = []

    async def mail_event(self, *args):
        self.calls.append(args[1:])
        return True


@pytest.fixture
def harness(monkeypatch):
    db = sqlite3.connect(":memory:")
    db.row_factory = sqlite3.Row
    for statement in schema.SCHEMA_STATEMENTS:
        if ("TABLE IF NOT EXISTS email_sends " in statement
                or "TABLE IF NOT EXISTS email_footers " in statement
                or " ON email_sends(" in statement):
            db.execute(statement)

    async def d1_first(_env, sql, *args):
        row = db.execute(sql, args).fetchone()
        return dict(row) if row else None

    async def d1_run(_env, sql, *args):
        db.execute(sql, args)

    async def blind_index(_env, value):
        return hashlib.sha256(str(value).encode()).hexdigest()

    async def encrypt_row(_env, value):
        return "sealed:" + json.dumps(value)

    async def decrypt_row(_env, value):
        return json.loads(value[len("sealed:"):])

    log = _MailLog()
    follower = _Follower()
    for name, value in {
        "d1_first": d1_first, "d1_run": d1_run, "blind_index": blind_index,
        "encrypt_row": encrypt_row, "decrypt_row": decrypt_row,
        "mail_log": log, "task_email": follower,
    }.items():
        monkeypatch.setattr(mail_activity, name, value, raising=False)
    return db, log, follower


def _run(coroutine):
    return asyncio.run(coroutine)


def test_any_kind_of_email_is_recorded_and_its_opens_and_clicks_logged(
        harness):
    db, log, follower = harness
    assert _run(mail_activity.sent(
        None, SEND_ID, "badge", "Ada@Example.com", "You earned a badge", NOW,
        recipient="ada", message_id="<m-1@forkmesh.com>",
        body="Well done, you earned the Trailblazer badge."))
    row = dict(db.execute("SELECT * FROM email_sends").fetchone())
    assert (row["kind"], row["ref"], row["sent_at"]) == ("badge", "", NOW)
    assert row["recipient_bi"] == _run(
        mail_activity.recipient_index(None, "ada@example.com"))
    assert json.loads(row["data"][len("sealed:"):]) == {
        "email": "Ada@Example.com", "subject": "You earned a badge",
        "body": "Well done, you earned the Trailblazer badge.",
        "recipient": "ada", "messageId": "m-1@forkmesh.com"}
    assert _run(mail_activity.send_details(None, SEND_ID)) == {
        "subject": "You earned a badge",
        "body": "Well done, you earned the Trailblazer badge."}
    assert _run(mail_activity.send_details(None, "f" * 32)) == {}

    assert _run(mail_activity.tracked(None, SEND_ID, "open", NOW + 1))
    assert _run(mail_activity.tracked(None, SEND_ID.upper(), "open", NOW + 2))
    assert _run(mail_activity.tracked(None, SEND_ID, "click", NOW + 3, "m-2"))
    assert not _run(mail_activity.tracked(None, SEND_ID, "bounce", NOW))
    assert not _run(mail_activity.tracked(None, "f" * 32, "open", NOW))
    assert not _run(mail_activity.tracked(None, "not-an-id", "open", NOW))

    row = dict(db.execute("SELECT * FROM email_sends").fetchone())
    assert (row["opened_at"], row["open_count"]) == (NOW + 1, 2)
    assert (row["clicked_at"], row["click_count"]) == (NOW + 3, 1)
    assert [(note["kind"], note["result"], note["messageId"],
             note["detail"]) for note in log.notes] == [
        ("badge", "opened", "m-1@forkmesh.com", "@ada opened the email"),
        ("badge", "opened", "m-1@forkmesh.com",
         "@ada opened the email (2 times)"),
        ("badge", "clicked", "m-2", "@ada clicked a link in the email")]
    assert mail_log.headline(log.notes[0]).startswith(
        "🏅 Badge awarded · 👀 opened")
    assert all(note["body"] == "Well done, you earned the Trailblazer badge."
               for note in log.notes)
    assert follower.calls == []


def test_an_email_without_a_known_recipient_still_logs(harness):
    db, log, _follower = harness
    _run(mail_activity.sent(None, SEND_ID, "admin_test", "x@example.com",
                            "Test", NOW, recipient="Not A Name"))
    assert _run(mail_activity.tracked(None, SEND_ID, "open", NOW))
    assert log.notes[0]["detail"] == "The recipient opened the email"


def test_a_send_without_an_id_or_address_is_not_recorded(harness):
    db, _log, _follower = harness
    assert not _run(mail_activity.sent(None, "", "badge", "a@b.c", "s", NOW))
    assert not _run(mail_activity.sent(None, SEND_ID, "badge", "", "s", NOW))
    assert db.execute("SELECT COUNT(*) FROM email_sends").fetchone()[0] == 0


def test_hourly_counts_are_per_kind_and_address(harness):
    _db, _log, _follower = harness
    for number, kind in enumerate(("task", "task", "badge")):
        _run(mail_activity.sent(None, "%032x" % number, kind,
                                "bob@example.com", "s", NOW - number))
    _run(mail_activity.sent(None, "%032x" % 9, "task", "eve@example.com",
                            "s", NOW))
    assert _run(mail_activity.sent_since(
        None, "task", "Bob@Example.com", NOW - 10)) == 2
    assert _run(mail_activity.sent_since(
        None, "task", "bob@example.com", NOW - 1)) == 1


def test_a_follower_hears_each_event_with_its_ref(harness):
    _db, _log, follower = harness
    _run(mail_activity.sent(None, SEND_ID, "task", "bob@example.com",
                            "New task: X", NOW, ref="task:org:" + "a" * 32,
                            recipient="bob"))
    _run(mail_activity.tracked(None, SEND_ID, "open", NOW + 1))
    _run(mail_activity.tracked(None, SEND_ID, "open", NOW + 2))
    assert follower.calls == [
        ("task:org:" + "a" * 32, "open", "bob", "New task: X", True, NOW + 1),
        ("task:org:" + "a" * 32, "open", "bob", "New task: X", False,
         NOW + 2)]


def test_a_view_needs_a_signed_token_for_the_same_ref(harness):
    db, log, _follower = harness
    ref = "task:org:" + "a" * 32
    _run(mail_activity.sent(None, SEND_ID, "task", "bob@example.com", "S",
                            NOW, ref=ref, recipient="bob"))
    token = _run(mail_activity.view_token(None, SEND_ID))
    assert mail_activity.parse_token(token.upper()) == tuple(token.split("."))
    assert mail_activity.parse_token(SEND_ID + ".short") == ("", "")
    assert not _run(mail_activity.viewed(
        None, SEND_ID + "." + "0" * 24, ref, "bob", NOW))
    assert not _run(mail_activity.viewed(
        None, token, "task:org:" + "b" * 32, "bob", NOW))
    assert not _run(mail_activity.viewed(None, token, "", "bob", NOW))
    assert _run(mail_activity.viewed(None, token, ref, "carol", NOW + 5))
    row = dict(db.execute("SELECT * FROM email_sends").fetchone())
    assert (row["viewed_at"], row["view_count"]) == (NOW + 5, 1)
    assert log.notes[-1]["detail"] == (
        "@carol opened the page linked in the email sent to @bob")


def test_a_send_keeps_its_sender_and_package_variables(harness):
    _db, log, _follower = harness
    assert _run(mail_activity.sent(
        None, SEND_ID, "package", "jo@example.com", "Hello", NOW,
        sender="alice", variables={
            "forkmesh_crm_send_id": "c1", "forkmesh_crm_org_bi": "org",
            "category": "dropped", "forkmesh_bad": {"nested": 1}}))
    assert _run(mail_activity.send_variables(None, SEND_ID)) == {
        "forkmesh_crm_send_id": "c1", "forkmesh_crm_org_bi": "org"}
    assert _run(mail_activity.send_variables(None, "f" * 32)) == {}
    assert _run(mail_activity.tracked(None, SEND_ID, "open", NOW + 1))
    assert log.notes[0]["detail"] == (
        "The recipient opened the email from @alice")
    assert log.notes[0]["to"] == "jo@example.com"


def test_the_footer_is_one_per_account_and_its_opens_reach_the_log(harness):
    db, log, _follower = harness
    first = _run(mail_activity.footer(
        None, "acct-bi", "alice", "alice@forkmesh.com", NOW))
    again = _run(mail_activity.footer(
        None, "acct-bi", "alice", "alice@zoho.example", NOW + 5))
    assert first["footer_id"] == again["footer_id"]
    assert len(first["footer_id"]) == 32
    assert db.execute("SELECT COUNT(*) FROM email_footers").fetchone()[0] == 1

    token = _run(mail_activity.footer_token(None, first["footer_id"]))
    image = "https://app.forkmesh.com/api/mail/t/f/%s.png" % token
    html = mail_activity.footer_html(image)
    assert image in html and "https://forkmesh.com" in html
    assert "ForkMesh" in html

    assert _run(mail_activity.footer_opened(None, token, NOW + 10))
    assert _run(mail_activity.footer_opened(None, token, NOW + 20))
    assert _run(mail_activity.footer_opened(
        None, token, NOW + 10 + mail_activity.FOOTER_REPEAT_MS + 1))
    assert not _run(mail_activity.footer_opened(
        None, first["footer_id"] + "." + "0" * 24, NOW))
    assert not _run(mail_activity.footer_opened(None, "nope", NOW))

    row = dict(db.execute("SELECT * FROM email_footers").fetchone())
    assert (row["open_count"], row["opened_at"]) == (2, NOW + 10)
    assert row["last_open_at"] == NOW + 10 + mail_activity.FOOTER_REPEAT_MS + 1
    assert [(note["kind"], note["result"], note["from"]) for note in
            log.notes] == [("footer", "opened", "alice@zoho.example")] * 2
    assert log.notes[1]["detail"] == (
        "@alice's tracked footer was opened in an email sent from "
        "alice@zoho.example (2 opens so far)")
    assert mail_log.headline(log.notes[0]).startswith(
        "🖼️ Own mail (tracked footer) · 👀 opened")


def test_package_text_mail_becomes_html_with_the_logo_footer():
    html = mail_activity.add_footer(
        mail_activity.text_html("Hi <Jo>\nthanks"), "https://x/l.png")
    assert html.startswith('<div style="white-space:pre-wrap">Hi &lt;Jo&gt;')
    assert 'src="https://x/l.png"' in html
    wrapped = mail_activity.add_footer("<p>x</p></body>", "https://x/l.png")
    assert wrapped.index("l.png") < wrapped.index("</body>")


def test_package_mail_gets_the_logo_footer_and_keeps_its_links():
    import ast
    import types
    from pathlib import Path

    import mail_tracking

    entry = Path(__file__).resolve().parents[1] / "src" / "entry.py"
    tree = ast.parse(entry.read_text(encoding="utf-8"))
    selected = [node for node in tree.body
                if isinstance(node, ast.AsyncFunctionDef)
                and node.name == "_mail_tracked_html"]

    async def blind_index(_env, value):
        return hashlib.sha256(str(value).encode()).hexdigest()

    import instance_privacy

    namespace = {"blind_index": blind_index, "mail_tracking": mail_tracking,
                 "mail_activity": mail_activity,
                 "instance_privacy": instance_privacy}
    exec(compile(ast.Module(body=selected, type_ignores=[]), str(entry),
                 "exec"), namespace)
    env = types.SimpleNamespace(API_ORIGIN="https://app.example")
    send_id = "0123abcd-0123-4567-89ab-0123456789ab"
    token = mail_tracking.token(send_id, hashlib.sha256(
        ("mail-track:" + send_id).encode()).hexdigest())

    html, unsubscribe = _run(namespace["_mail_tracked_html"](
        env, send_id, '<p><a href="https://example.org/x">x</a></p>',
        footer=True))
    assert 'href="https://example.org/x"' in html
    assert 'src="https://app.example/api/mail/t/l/%s.png"' % token in html
    assert 'width="1"' not in html
    assert unsubscribe == "https://app.example/api/mail/t/u/" + token

    tracked, _ = _run(namespace["_mail_tracked_html"](
        env, send_id, '<p><a href="https://example.org/x">x</a></p>'))
    assert "/api/mail/t/c/" in tracked and "/api/mail/t/o/" in tracked
