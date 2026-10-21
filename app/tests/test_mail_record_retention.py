import ast
import asyncio
import sqlite3
from pathlib import Path

import pytest

import mail_tracking
import schema

ROOT = Path(__file__).resolve().parents[1]
ROUTES = ROOT / "src" / "route_handlers.py"
ENTRY = ROOT / "src" / "entry.py"
DAY = 24 * 60 * 60 * 1000
NOW = 400 * DAY
TABLES = ("mail_sends", "mailtrap_email_sends", "mailtrap_webhook_events",
          "mail_suppressions", "email_sends")


def _load(path, names, namespace):
    tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    selected = [node for node in tree.body
                if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
                and node.name in names]
    assert sorted(node.name for node in selected) == sorted(names)
    exec(compile(ast.Module(body=selected, type_ignores=[]), str(path), "exec"),
         namespace)
    return namespace


class Date:
    @staticmethod
    def now():
        return NOW


class PluginData:
    def __init__(self):
        self.events = []

    async def deliver_mailtrap_event(self, _runtime, event, status, _now):
        self.events.append((status, event.get("custom_variables")))
        return None


class MailActivity:
    def __init__(self):
        self.calls = []
        self.sends = []

    async def sent(self, _env, send_id, kind, email, subject, now, ref="",
                   recipient="", message_id="", sender="", variables=None,
                   body="", body_html=""):
        self.sends.append((send_id, kind, email, subject, now, ref,
                           recipient, message_id, body, body_html))
        return True

    async def send_variables(self, _env, send_id):
        return {"forkmesh_crm_send_id": "crm-" + send_id}

    async def tracked(self, _env, send_id, status, now, message_id=""):
        self.calls.append((send_id, status, now, message_id))
        return True

    async def send_details(self, _env, send_id):
        return {"subject": "Subject of " + send_id,
                "body": "Body of " + send_id}


class MailLog:
    WEBHOOK_RESULTS = {"delivery": "delivered", "bounce": "bounced"}

    def __init__(self):
        self.notes = []

    @staticmethod
    def webhook_note(event, now, subject="", body=""):
        return {"event": event.get("event"), "at": now,
                "subject": subject, "body": body}

    async def enqueue(self, _env, entry):
        self.notes.append(entry)
        return True


def _clean_string(value, max_length=240):
    return value.strip()[:max_length] if isinstance(value, str) else ""


@pytest.fixture
def db():
    connection = sqlite3.connect(":memory:")
    connection.row_factory = sqlite3.Row
    for statement in schema.SCHEMA_STATEMENTS:
        head = statement.split("(", 1)[0]
        if any(head.rstrip().endswith(" " + table) for table in TABLES) or any(
                (" ON %s(" % table) in statement for table in TABLES):
            connection.execute(statement)
    return connection


def _namespace(db):
    async def d1_first(_env, sql, *args):
        row = db.execute(sql, args).fetchone()
        return dict(row) if row else None

    async def d1_run(_env, sql, *args):
        db.execute(sql, args)

    return {
        "d1_first": d1_first, "d1_run": d1_run, "Date": Date,
        "clean_string": _clean_string, "mail_tracking": mail_tracking,
        "MAILTRAP_WEBHOOK_EVENTS": {"delivery", "open", "click", "bounce"},
        "plugin_data": PluginData(), "mail_log": MailLog(),
        "_PluginWebhookRuntime": lambda env: env,
        "mail_activity": MailActivity(),
    }


def _routes(db):
    return _load(ROUTES, ("prune_mail_records", "record_core_mail_event",
                          "process_mail_event", "_mail_send_row"),
                 _namespace(db))


def _seed(db, send_id, sent_at):
    db.execute("INSERT INTO mail_sends(send_id,message_id,kind,account_bi,"
               "recipient_bi,sent_at,status,status_at) VALUES(?,?,?,?,?,?,?,?)",
               (send_id, "msg-" + send_id, "account", "acct", "rcpt", sent_at,
                "accepted", sent_at))
    db.execute("INSERT INTO mailtrap_email_sends(send_id,account_bi,kind,"
               "sent_at,accepted,status,status_at,message_id,opened_at,"
               "open_count) VALUES(?,?,?,?,1,'open',?,?,?,1)",
               (send_id, "acct", "account", sent_at, sent_at,
                "msg-" + send_id, sent_at))
    db.execute("INSERT INTO mailtrap_webhook_events(event_id,received_at) "
               "VALUES(?,?)", ("open:%s:0" % send_id, sent_at))


def _ids(db, table, column="send_id"):
    return sorted(row[0] for row in db.execute(
        "SELECT %s FROM %s" % (column, table)))


def _assert_only_recent_kept(db, *recent):
    assert _ids(db, "mail_sends") == sorted(recent)
    assert _ids(db, "mailtrap_email_sends") == sorted(recent)


@pytest.mark.parametrize("event", [
    {"event_id": "open:current:1", "event": "open", "send_id": "current"},
    {"event_id": "click:current:1", "event": "click", "send_id": "current"},
    {"event_id": "postfix-1", "event": "delivery", "message_id": "msg-current"},
])
def test_a_core_mail_event_deletes_tracking_rows_older_than_90_days(db, event):
    _seed(db, "old", NOW - 91 * DAY)
    _seed(db, "kept", NOW - 89 * DAY)
    _seed(db, "current", NOW - 60_000)
    routes = _routes(db)

    assert asyncio.run(routes["record_core_mail_event"](None, event, NOW))

    _assert_only_recent_kept(db, "kept", "current")
    assert _ids(db, "mailtrap_webhook_events", "event_id") == sorted([
        "open:kept:0", "open:current:0", event["event_id"]])
    assert db.execute("SELECT status FROM mailtrap_email_sends "
                      "WHERE send_id='current'").fetchone()[0] == event["event"]


@pytest.mark.parametrize("dsn,stored", [
    ("4.4.3", "4.4.3"), ("5.1.10", "5.1.10"), ("4.4.3; drop", ""), ("", ""),
])
def test_a_postfix_event_records_its_dsn_on_the_send(db, dsn, stored):
    _seed(db, "current", NOW - 60_000)
    routes = _routes(db)
    event = {"event_id": "smtp:deferred", "event": "soft bounce",
             "message_id": "msg-current", "dsn": dsn}

    asyncio.run(routes["record_core_mail_event"](None, event, NOW))

    assert tuple(db.execute("SELECT status,dsn FROM mail_sends "
                            "WHERE send_id='current'").fetchone()) == (
        "soft bounce", stored)


@pytest.mark.parametrize("dsn,reason,cause", [
    ("5.7.1", "host mail.tutanota.de[185.205.69.214] said: 550 5.7.1 "
     "<someone@tutamail.com>: Recipient address rejected: Recipient not "
     "available (in reply to RCPT TO command)", "recipient"),
    ("5.7.1", "host mx.example.net[192.0.2.1] said: 550 5.7.1 Service "
     "unavailable; client host blocked using Spamhaus", ""),
    ("5.7.1", "550 5.7.1 Recipient address rejected: user unknown, "
     "SPF check failed", ""),
])
def test_a_postfix_bounce_records_whether_the_recipient_is_missing(
        db, dsn, reason, cause):
    _seed(db, "current", NOW - 60_000)
    routes = _routes(db)
    event = {"event_id": "smtp:bounced", "event": "bounce",
             "message_id": "msg-current", "dsn": dsn, "reason": reason}

    asyncio.run(routes["record_core_mail_event"](None, event, NOW))

    assert tuple(db.execute("SELECT status,dsn,cause FROM mail_sends "
                            "WHERE send_id='current'").fetchone()) == (
        "bounce", dsn, cause)
    assert db.execute("SELECT reason FROM mail_suppressions "
                      "WHERE recipient_bi='rcpt'").fetchone()[0] == "bounce"


@pytest.mark.parametrize("event,dsn,reason,cause", [
    ("bounce", "5.1.1", "550 5.1.1 No such user here", "recipient"),
    ("bounce", "5.2.1", "550 The email account that you tried to reach "
     "does not exist", "recipient"),
    ("soft bounce", "4.7.1", "450 4.7.1 Recipient not available", ""),
    ("bounce", "5.7.1", "550 5.7.1 Message rejected by policy", ""),
    ("bounce", "5.7.1", "550 5.7.1 <blockchain@example.com>: Recipient "
     "address rejected: User unknown", "recipient"),
    ("bounce", "", "550 user unknown", ""),
])
def test_bounce_cause_only_names_a_missing_recipient(event, dsn, reason,
                                                     cause):
    assert mail_tracking.bounce_cause(event, dsn, reason) == cause


def test_an_smtp_send_deletes_tracking_rows_older_than_90_days(db):
    _seed(db, "old", NOW - 91 * DAY)
    _seed(db, "kept", NOW - 89 * DAY)
    namespace = _routes(db)

    async def blind_index(_env, value):
        return "bi-" + value

    async def _mail_suppressed(*_args):
        return ""

    async def _mail_tracked_html(_env, _send_id, html):
        return html, "https://app.example/unsubscribe"

    async def _mailtrap_account_for_email(*_args):
        return "acct"

    async def log(*_args, **_kwargs):
        return None

    class SmtpMail:
        @staticmethod
        def build(*args, **kwargs):
            return args

        @staticmethod
        async def send(_env, _message):
            return "<msg-new>"

    namespace.update({
        "_ap_uuid": lambda: "new", "blind_index": blind_index,
        "_mail_suppressed": _mail_suppressed,
        "_mail_tracked_html": _mail_tracked_html,
        "_mailtrap_account_for_email": _mailtrap_account_for_email,
        "smtp_mail": SmtpMail,
    })
    _load(ENTRY, ("_send_email_smtp", "_record_mailtrap_send",
                  "_record_email_send"), namespace)

    sent = asyncio.run(namespace["_send_email_smtp"](
        None, "no-reply@example.com", "ForkMesh", "ada@example.com", "Hi",
        "text", "<p>html</p>", "account", log))

    assert sent is True
    assert namespace["mail_activity"].sends == [
        ("new", "account", "ada@example.com", "Hi", NOW, "", "", "msg-new",
         "text", "<p>html</p>")]
    _assert_only_recent_kept(db, "kept", "new")
    assert _ids(db, "mailtrap_webhook_events", "event_id") == ["open:kept:0"]


def test_opens_and_clicks_reach_mail_activity_and_old_rows_are_pruned(
        db):
    _seed(db, "current", NOW - 60_000)
    for send_id, sent_at in (("old", NOW - 91 * DAY), ("current", NOW)):
        db.execute("INSERT INTO email_sends (send_id,kind,recipient_bi,"
                   "data,sent_at) VALUES (?,?,?,?,?)",
                   (send_id, "account", "rcpt", "{}", sent_at))
    routes = _routes(db)
    for event in ({"event_id": "open:current:9", "event": "open",
                   "send_id": "current"},
                  {"event_id": "click:current:9", "event": "click",
                   "send_id": "current"},
                  {"event_id": "postfix-9", "event": "delivery",
                   "message_id": "msg-current"}):
        assert asyncio.run(routes["record_core_mail_event"](None, event, NOW))

    assert routes["mail_activity"].calls == [
        ("current", "open", NOW, "msg-current"),
        ("current", "click", NOW, "msg-current")]
    assert routes["plugin_data"].events[0] == ("open", {
        "forkmesh_crm_send_id": "crm-current",
        "forkmesh_send_id": "current", "forkmesh_account_bi": "acct",
        "forkmesh_kind": "account"})
    assert routes["mail_log"].notes == [
        {"event": "open", "at": NOW, "subject": "", "body": ""},
        {"event": "click", "at": NOW, "subject": "", "body": ""},
        {"event": "delivery", "at": NOW, "subject": "Subject of current",
         "body": "Body of current"}]
    assert _ids(db, "email_sends") == ["current"]
