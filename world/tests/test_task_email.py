import asyncio
import hashlib
import json
import sqlite3

import pytest

import mail_activity
import mail_log
import mail_tracking
import schema
import world_office_tasks
import world_task_email as task_email

NOW = 1_800_000_000_000
TASK_ID = "a" * 32
ORG_BI = "org-bi"
TABLES = ("email_sends", "organization_task_events")
META = {"organization": "forkmesh", "taskId": TASK_ID,
        "taskTitle": "Fix the login bug", "assignee": "bob",
        "assigneeKind": "user", "priority": 50}
DETAILS = '@alice created "Fix the login bug" in the forkmesh organization'


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


class _Harness:
    def __init__(self, monkeypatch, accounts=None, send_ok=True):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        for statement in schema.SCHEMA_STATEMENTS:
            if any(("TABLE IF NOT EXISTS %s " % table) in statement
                   or (" ON %s(" % table) in statement for table in TABLES):
                self.db.execute(statement)
        self.accounts = accounts if accounts is not None else {
            name: {"name": name, "email": name + "@mailbox.org",
                   "email_verified": True, "status": "active"}
            for name in ("alice", "bob", "carol")}
        self.send_ok = send_ok
        self.sent = []
        self.stamps = []
        self.mail_log = _MailLog()
        self.ids = iter("%032x" % number for number in range(1, 1000))
        harness = self

        class Date:
            @staticmethod
            def now():
                return NOW

        async def d1_first(_env, sql, *args):
            row = harness.db.execute(sql, args).fetchone()
            return dict(row) if row else None

        async def d1_run(_env, sql, *args):
            harness.db.execute(sql, args)

        async def blind_index(_env, value):
            return hashlib.sha256(str(value).encode()).hexdigest()

        async def encrypt_row(_env, value):
            return "sealed:" + json.dumps(value)

        async def decrypt_row(_env, value):
            return json.loads(value[len("sealed:"):])

        async def account_row(_env, name):
            return "bi:" + name, harness.accounts.get(name)

        async def send_email(env, to, subject, text, html, email_kind="",
                             send_id="", ref="", recipient=""):
            harness.sent.append({"to": to, "subject": subject, "text": text,
                                 "html": html, "kind": email_kind,
                                 "send_id": send_id, "ref": ref,
                                 "recipient": recipient})
            if harness.send_ok:
                await mail_activity.sent(
                    env, send_id, email_kind, to, subject, NOW, ref=ref,
                    recipient=recipient, message_id="msg-" + send_id[-2:],
                    body=text, body_html=html)
            return harness.send_ok

        async def record_account_email(_env, name, kind, ok):
            harness.stamps.append((name, kind, ok))

        def card(heading, intro, body, footer="", action_label="",
                 action_url=""):
            return "<h1>%s</h1><p>%s</p>%s%s<a href=\"%s\">%s</a>" % (
                heading, intro, body, footer, action_url, action_label)

        shared = {
            "d1_first": d1_first, "d1_run": d1_run,
            "blind_index": blind_index, "encrypt_row": encrypt_row,
            "decrypt_row": decrypt_row, "mail_log": self.mail_log,
            "task_email": task_email,
        }
        for name, value in shared.items():
            monkeypatch.setattr(mail_activity, name, value, raising=False)
        for name, value in {
            "Date": Date, "d1_first": d1_first, "d1_run": d1_run,
            "blind_index": blind_index, "encrypt_row": encrypt_row,
            "decrypt_row": decrypt_row, "_account_row": account_row,
            "_account_kind": lambda rec: rec.get("kind") or "user",
            "_send_email": send_email,
            "_record_account_email": record_account_email,
            "_ap_uuid": lambda: next(harness.ids),
            "_public_base_url": lambda env: "https://forkmesh.example",
            "FORKMESH_SITE_URL": "https://forkmesh.com/",
            "_html_escape": lambda value: str(value).replace("<", "&lt;"),
            "_forkmesh_email_card_html": card,
            "mail_log": self.mail_log,
            "mail_activity": mail_activity,
            "world_office_tasks": world_office_tasks,
        }.items():
            monkeypatch.setattr(task_email, name, value, raising=False)

    def send(self, action="created", changes=None,
             names=("alice", "bob")):
        return asyncio.run(task_email.send_activity(
            None, ORG_BI, "forkmesh", "alice", TASK_ID, action, changes,
            list(names), META, DETAILS))

    def events(self):
        return [
            (row["kind"], json.loads(row["data"][len("sealed:"):]))
            for row in self.db.execute(
                "SELECT kind,data FROM organization_task_events "
                "ORDER BY created_at,rowid")]

    def row(self, send_id):
        return dict(self.db.execute(
            "SELECT * FROM email_sends WHERE send_id=?",
            (send_id,)).fetchone())


def test_a_new_task_emails_the_creator_and_everyone_on_it():
    assert task_email.recipients(["bob", "jett", "bob"], "alice",
                                 "created") == ["alice", "bob", "jett"]
    assert task_email.recipients(["bob"], "alice", "updated") == ["bob"]
    assert task_email.recipients(["Bad Name", ""], "", "created") == []
    many = ["user%d" % number for number in range(30)]
    assert len(task_email.recipients(many, "alice", "created")) == \
        task_email.MAX_RECIPIENTS


def test_task_refs_and_links():
    ref = task_email.task_ref(ORG_BI, TASK_ID.upper())
    assert ref == "task:%s:%s" % (ORG_BI, TASK_ID)
    assert task_email.parse_ref(ref) == (ORG_BI, TASK_ID)
    assert task_email.parse_ref("task:org:bi:" + TASK_ID) == (
        "org:bi", TASK_ID)
    assert task_email.parse_ref("badge::" + TASK_ID) == ("", "")
    assert task_email.parse_ref("") == ("", "")
    assert task_email.task_url("https://a.example/", TASK_ID, "t.k") == (
        "https://a.example/dashboard/tasks/?task=%s&mail=t.k" % TASK_ID)
    assert task_email.task_url("https://a.example", TASK_ID, "t",
                               deleted=True) == \
        "https://a.example/dashboard/tasks/"


def test_created_task_sends_tracked_task_mail(monkeypatch):
    harness = _Harness(monkeypatch)
    assert harness.send() == 2
    assert [mail["to"] for mail in harness.sent] == [
        "alice@mailbox.org", "bob@mailbox.org"]
    first = harness.sent[0]
    assert first["kind"] == "task" == task_email.KIND
    assert first["kind"] in mail_tracking.UNSUBSCRIBE_KINDS
    assert first["subject"] == "New task: Fix the login bug"
    assert first["send_id"] == "%032x" % 1
    assert first["text"].startswith("Created by @alice\n\n" + DETAILS)
    assert ("Open the task: https://forkmesh.example/dashboard/tasks/?task="
            + TASK_ID + "&mail=" + first["send_id"] + ".") in first["text"]
    assert "unsubscribe" in first["text"]
    assert "Open the task" in first["html"]
    assert harness.stamps == [("alice", "task", True), ("bob", "task", True)]
    assert (first["ref"], first["recipient"]) == (
        task_email.task_ref(ORG_BI, TASK_ID), "alice")
    row = harness.row(first["send_id"])
    assert (row["kind"], row["ref"]) == ("task", "task:%s:%s" % (
        ORG_BI, TASK_ID))
    stored = json.loads(row["data"][len("sealed:"):])
    assert stored.pop("body").startswith(
        "Created by @alice " + DETAILS + " Open the task: [link]")
    assert stored == {
        "recipient": "alice", "email": "alice@mailbox.org",
        "subject": "New task: Fix the login bug", "messageId": "msg-01"}
    (kind, data), = harness.events()
    assert kind == "emailed"
    assert data["actor"] == "alice"
    assert data["summary"] == (
        "New task: Fix the login bug → @alice, @bob")


def test_every_activity_is_emailed_with_its_change(monkeypatch):
    harness = _Harness(monkeypatch)
    harness.send("updated", [{"field": "progress", "from": 40, "to": 60}],
                 names=["bob"])
    mail, = harness.sent
    assert mail["subject"] == "Task updated: Fix the login bug"
    assert mail["text"].startswith("Progress 40% → 60% by @alice")
    harness.send("commented", [{"field": "comment", "excerpt": "Looks good"}],
                 names=["bob"])
    assert harness.sent[-1]["text"].startswith(
        "New comment by @alice: Looks good")
    assert [kind for kind, _ in harness.events()] == ["emailed", "emailed"]


def test_deleted_task_mail_links_to_the_list_without_a_view_token(
        monkeypatch):
    harness = _Harness(monkeypatch)
    harness.send("deleted", names=["bob"])
    mail, = harness.sent
    assert mail["subject"].startswith("Task deleted: ")
    assert "Open the task: https://forkmesh.example/dashboard/tasks/\n" in \
        mail["text"]


@pytest.mark.parametrize("change", [
    {"email_verified": False}, {"email": ""}, {"status": "disabled"},
    {"kind": "org"}, {"email_notifications": False},
])
def test_accounts_that_cannot_or_do_not_want_mail_are_skipped(
        monkeypatch, change):
    bob = {"name": "bob", "email": "bob@mailbox.org", "email_verified": True,
           "status": "active"}
    bob.update(change)
    harness = _Harness(monkeypatch, accounts={"bob": bob})
    assert harness.send(names=["bob", "ghost"]) == 0
    assert harness.sent == []
    assert harness.events() == []


def test_a_failed_send_is_not_tracked(monkeypatch):
    harness = _Harness(monkeypatch, send_ok=False)
    assert harness.send(names=["bob"]) == 0
    assert harness.stamps == [("bob", "task", False)]
    assert harness.db.execute(
        "SELECT COUNT(*) FROM email_sends").fetchone()[0] == 0
    assert harness.events() == []


def test_hourly_cap_holds_task_mail_and_logs_it(monkeypatch):
    harness = _Harness(monkeypatch)
    bob_bi = asyncio.run(mail_activity.recipient_index(
        None, "Bob@Mailbox.org"))
    harness.db.execute(
        "INSERT INTO email_sends (send_id,kind,recipient_bi,data,sent_at) "
        "VALUES (?,?,?,?,?)", ("badge", "badge", bob_bi, "sealed:{}", NOW))
    for number in range(task_email.HOURLY_CAP - 1):
        harness.db.execute(
            "INSERT INTO email_sends (send_id,kind,recipient_bi,ref,data,"
            "sent_at) VALUES (?,?,?,?,?,?)",
            ("old%d" % number, "task", bob_bi, "", "sealed:{}", NOW - 1000))
    assert harness.send(names=["bob"]) == 1
    harness.sent.clear()
    assert harness.send(names=["bob"]) == 0
    assert harness.sent == []
    note, = harness.mail_log.notes
    assert (note["kind"], note["result"]) == ("task", "suppressed")
    assert "30 an hour" in note["error"]


def test_opens_and_clicks_land_on_the_task_and_in_the_mail_log(monkeypatch):
    harness = _Harness(monkeypatch)
    harness.send(names=["bob"])
    send_id = harness.sent[0]["send_id"]

    run = asyncio.run
    assert run(mail_activity.tracked(None, send_id, "open", NOW + 1, "m-1"))
    assert run(mail_activity.tracked(None, send_id, "open", NOW + 2, ""))
    assert run(mail_activity.tracked(None, send_id, "click", NOW + 3, "m-1"))
    assert not run(mail_activity.tracked(None, "0" * 32, "open", NOW, ""))
    assert not run(mail_activity.tracked(None, send_id, "delivery", NOW, ""))

    row = harness.row(send_id)
    assert (row["opened_at"], row["open_count"]) == (NOW + 1, 2)
    assert (row["clicked_at"], row["click_count"]) == (NOW + 3, 1)
    events = harness.events()
    assert [kind for kind, _ in events] == [
        "emailed", "email_opened", "email_clicked"]
    assert events[1][1]["actor"] == "bob"
    assert events[1][1]["summary"] == "New task: Fix the login bug"
    notes = harness.mail_log.notes
    assert [note["result"] for note in notes] == [
        "opened", "opened", "clicked"]
    assert notes[0]["to"] == "bob@mailbox.org"
    assert notes[0]["messageId"] == "m-1"
    assert notes[1]["messageId"] == "msg-01"
    assert notes[1]["detail"] == "@bob opened the email (2 times)"
    assert notes[2]["detail"] == "@bob clicked a link in the email"
    assert "🔍 Detail: @bob opened the email (2 times)" in (
        mail_log.line(notes[1]))
    assert all(note["body"].startswith("Created by @alice") for note in notes)
    assert mail_log.headline(notes[0]).startswith(
        "🗂️ Task activity · 👀 opened")


def test_task_view_from_the_email_is_logged_once_per_signed_token(
        monkeypatch):
    harness = _Harness(monkeypatch)
    harness.send(names=["bob"])
    mail = harness.sent[0]
    token = mail["text"].split("&mail=", 1)[1].split()[0]
    send_id = mail["send_id"]
    run = asyncio.run

    forged = send_id + "." + "0" * 24
    assert not run(task_email.viewed(None, ORG_BI, TASK_ID, forged, "bob",
                                     NOW))
    assert not run(task_email.viewed(None, ORG_BI, "b" * 32, token, "bob",
                                     NOW))
    assert not run(task_email.viewed(None, "other", TASK_ID, token, "bob",
                                     NOW))
    assert run(task_email.viewed(None, ORG_BI, TASK_ID, token, "carol",
                                 NOW + 5))
    events = harness.events()
    assert events[-1][0] == "email_viewed"
    assert events[-1][1]["actor"] == "carol"
    note = harness.mail_log.notes[-1]
    assert note["result"] == "viewed"
    assert note["messageId"] == "msg-01"
    assert note["detail"] == (
        "@carol opened the page linked in the email sent to @bob")
    assert harness.row(send_id)["view_count"] == 1


def test_task_get_with_a_mail_token_reports_the_view():
    calls = []

    class Runtime:
        def query(self, name):
            return {"mail": "f" * 32 + "." + "0" * 24}.get(name, "")

        async def task_email_viewed(self, *args):
            calls.append(args)

    asyncio.run(world_office_tasks._observe_email_view(
        Runtime(), ORG_BI, TASK_ID, "bob", NOW))
    assert calls == [(ORG_BI, TASK_ID, "f" * 32 + "." + "0" * 24, "bob",
                      NOW)]

    class Bare:
        pass

    asyncio.run(world_office_tasks._observe_email_view(
        Bare(), ORG_BI, TASK_ID, "bob", NOW))


def test_email_events_are_part_of_the_timeline_vocabulary():
    assert {"emailed", "email_opened", "email_clicked",
            "email_viewed"} <= world_office_tasks.TASK_EVENT_KINDS
