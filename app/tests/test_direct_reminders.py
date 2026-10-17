import asyncio
import importlib.util
import json
import sqlite3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
ENTRY = (SRC / "entry.py").read_text(encoding="utf-8")
HOUR = 60 * 60 * 1000
DAY = 24 * HOUR
NOW = 1_800_000_000_000
RETAIN = 7 * DAY
AB = "a" * 32
CB = "b" * 32
ALICE = "1" * 64
BOB = "2" * 64
CAROL = "3" * 64
NAMES = {ALICE: "alice", BOB: "bob", CAROL: "carol"}


class Env:
    def __init__(self):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.db.execute(
            "CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT NOT NULL)")
        self.db.execute(
            "CREATE TABLE chat_history (room_key TEXT NOT NULL, "
            "msg_id TEXT NOT NULL, ts INTEGER NOT NULL, body TEXT NOT NULL, "
            "PRIMARY KEY (room_key, msg_id))")
        self.db.executescript("".join(
            (ROOT / "migrations" / name).read_text(encoding="utf-8")
            for name in ("0079_chat_direct_messages.sql",
                         "0084_chat_direct_message_limits.sql",
                         "0260_chat_direct_unread_reminders.sql",
                         "0269_chat_history_acked_grace.sql")))
        self.clock = NOW
        self.sent = []
        self.saved = []
        self.accounts = {
            bi: {"name": name, "status": "active", "email": name + "@x.test"}
            for bi, name in NAMES.items()
        }
        self.refs = 0

    def conversation(self, conversation_id, first, second):
        record = {"participants": sorted((NAMES[first], NAMES[second]))}
        self.db.execute(
            "INSERT INTO chat_direct_conversations (conversation_id,pair_bi,"
            "data,created_at,updated_at) VALUES (?,?,?,0,0)",
            (conversation_id, "p:" + conversation_id, json.dumps(record)))
        for account in (first, second):
            self.db.execute(
                "INSERT INTO chat_direct_participants (conversation_id,"
                "participant_bi,data,joined_at) VALUES (?,?,'{}',0)",
                (conversation_id, account))

    def store(self, conversation_id, sender, ts, notify=True):
        self.refs += 1
        ref = "%040x" % self.refs
        msg_id = ref + ":" + sender[:16] + (":n" if notify else "")
        self.db.execute(
            "INSERT INTO chat_history (room_key,msg_id,ts,body) "
            "VALUES (?,?,?,?)",
            ("chat-direct:%s:v1" % conversation_id, msg_id, ts,
             '{"kind":"cipher","body":"zz"}'))
        return ref

    def receive(self, conversation_id, ref):
        self.db.execute(
            "UPDATE chat_history SET acked_at=? WHERE room_key=? "
            "AND substr(msg_id,1,40)=?",
            (self.clock, "chat-direct:%s:v1" % conversation_id, ref))


def _load(env):
    spec = importlib.util.spec_from_file_location(
        "direct_reminders_under_test", SRC / "direct_reminders.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    async def d1_all(_env, sql, *args):
        return [dict(row) for row in env.db.execute(sql, args).fetchall()]

    async def d1_first(_env, sql, *args):
        row = env.db.execute(sql, args).fetchone()
        return dict(row) if row is not None else None

    async def d1_run(_env, sql, *args):
        env.db.execute(sql, args)
        env.db.commit()

    async def decrypt_row(_env, data):
        return json.loads(data)

    async def noop(*_args, **_kwargs):
        return None

    async def recipient(_env, account_bi):
        rec = env.accounts.get(account_bi)
        if not rec:
            return None, None, ""
        email = rec["email"] if rec.get("verified", True) else ""
        return rec, rec, email

    async def send_email(_env, to, subject, text, html, email_kind=""):
        env.sent.append({"to": to, "subject": subject, "text": text,
                         "html": html, "kind": email_kind})
        return True

    async def save_account(_env, account_bi, rec):
        env.saved.append(account_bi)

    class Date:
        @staticmethod
        def now():
            return env.clock

    module._bind_runtime({
        "d1_all": d1_all,
        "d1_first": d1_first,
        "d1_run": d1_run,
        "decrypt_row": decrypt_row,
        "ensure_schema": noop,
        "_is_main_relay": lambda _env: True,
        "Date": Date,
        "CHAT_HISTORY_RETAIN_MS": RETAIN,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "MAX_NODE_NAME": 39,
        "FORKMESH_SITE_URL": "https://forkmesh.com/",
        "_html_escape": lambda text: str(text).replace("<", "&lt;"),
        "_forkmesh_email_card_html": (
            lambda heading, intro, body, footer="", action_label="",
            action_url="": "|".join(
                (heading, intro, body, footer, action_label, action_url))),
        "_notification_email_recipient": recipient,
        "_account_kind": lambda rec: rec.get("kind", "user"),
        "ping_muted_kinds": lambda rec: rec.get("muted", []),
        "notification_email_enabled": (
            lambda rec, kind: rec.get("prefs", {}).get(kind, True)),
        "_send_email": send_email,
        "_stamp_account_email": lambda rec, kind, ok, now: rec,
        "_save_account": save_account,
    })
    return module


def _run(coro):
    return asyncio.run(coro)


def _reminded(env):
    return {
        (row["conversation_id"], row["participant_bi"]):
            row["unread_reminded_at"]
        for row in env.db.execute(
            "SELECT conversation_id,participant_bi,unread_reminded_at "
            "FROM chat_direct_participants")
    }


def test_recipient_of_a_day_old_server_copy_gets_one_reminder():
    env = Env()
    module = _load(env)
    env.conversation(AB, ALICE, BOB)
    env.store(AB, ALICE, NOW - 25 * HOUR)
    env.store(AB, ALICE, NOW - 2 * HOUR)
    env.store(AB, ALICE, NOW - 30 * HOUR, notify=False)

    assert _run(module.send_reminders(env)) == 1
    assert [mail["to"] for mail in env.sent] == ["bob@x.test"]
    mail = env.sent[0]
    assert mail["kind"] == "direct_unread"
    assert mail["subject"] == (
        "ForkMesh: 2 unread direct messages from alice waiting on the server")
    assert "- alice: 2 messages" in mail["text"]
    assert "deleted from the server" in mail["text"]
    assert "https://forkmesh.com/chat" in mail["text"]
    assert "zz" not in mail["text"] and "zz" not in mail["html"]
    assert env.saved == [BOB]
    assert _reminded(env) == {(AB, ALICE): 0, (AB, BOB): NOW}

    env.clock = NOW + 5 * 60 * 1000
    assert _run(module.send_reminders(env)) == 0
    assert len(env.sent) == 1

    env.store(AB, ALICE, NOW + HOUR)
    env.clock = NOW + 26 * HOUR
    assert _run(module.send_reminders(env)) == 1
    assert env.sent[1]["subject"] == (
        "ForkMesh: 1 unread direct message from alice waiting on the server")


def test_fresh_received_and_own_copies_send_nothing():
    env = Env()
    module = _load(env)
    env.conversation(AB, ALICE, BOB)
    env.store(AB, ALICE, NOW - 23 * HOUR)
    received = env.store(AB, BOB, NOW - 30 * HOUR)
    env.receive(AB, received)
    acked = env.store(AB, ALICE, NOW - 30 * HOUR)
    env.receive(AB, acked)
    env.store(AB, ALICE, NOW - RETAIN - HOUR)

    assert _run(module.send_reminders(env)) == 0
    assert env.sent == []
    assert set(_reminded(env).values()) == {0}


def test_opted_out_recipients_are_marked_without_mail():
    env = Env()
    module = _load(env)
    env.conversation(AB, ALICE, BOB)
    env.conversation(CB, CAROL, BOB)
    env.store(AB, BOB, NOW - 25 * HOUR)
    env.store(CB, BOB, NOW - 25 * HOUR)
    env.store(AB, ALICE, NOW - 25 * HOUR)
    env.accounts[ALICE]["muted"] = ["direct_unread"]
    env.accounts[CAROL]["prefs"] = {"direct_unread": False}
    env.accounts[BOB]["verified"] = False

    assert _run(module.send_reminders(env)) == 0
    assert env.sent == []
    assert _reminded(env) == {
        (AB, ALICE): NOW, (AB, BOB): NOW, (CB, CAROL): NOW, (CB, BOB): 0}


def test_one_email_lists_every_waiting_sender():
    env = Env()
    module = _load(env)
    env.conversation(AB, ALICE, BOB)
    env.conversation(CB, CAROL, BOB)
    for hours in (40, 30, 26):
        env.store(AB, ALICE, NOW - hours * HOUR)
    env.store(CB, CAROL, NOW - 25 * HOUR)

    assert _run(module.send_reminders(env)) == 1
    mail = env.sent[0]
    assert mail["to"] == "bob@x.test"
    assert mail["subject"] == (
        "ForkMesh: 4 unread direct messages from 2 people waiting on the "
        "server")
    assert "- alice: 3 messages\n" in mail["text"]
    assert "- carol: 1 message\n" in mail["text"]


def test_entry_wires_the_cron_phase_column_and_kind():
    assert '_direct_reminders = _LazyModule("direct_reminders")' in ENTRY
    assert 'if phase == "direct-unread-email":' in ENTRY
    assert "return await send_direct_unread_reminders(env)" in ENTRY
    assert '"direct_unread": True,' in ENTRY
    assert "ADD COLUMN unread_reminded_at INTEGER NOT NULL DEFAULT 0" in ENTRY
