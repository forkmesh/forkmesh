import asyncio
import importlib.util
import json
import sqlite3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
ENTRY = (SRC / "entry.py").read_text(encoding="utf-8")
MINUTE = 60 * 1000
NOW = 1_800_000_000_000
CHANNEL = "a" * 32
PUBLIC = "b" * 32
NOTICES = "c" * 32
ALICE = "1" * 64
BOB = "2" * 64
CAROL = "3" * 64
DAVE = "4" * 64


def _bi(name):
    return {"alice": ALICE, "bob": BOB, "carol": CAROL, "dave": DAVE}[name]


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
        self.db.executescript(
            (ROOT / "migrations" / "0069_private_chat_channels.sql")
            .read_text(encoding="utf-8")
            + (ROOT / "migrations" / "0231_chat_channel_reads.sql")
            .read_text(encoding="utf-8"))
        self.clock = NOW
        self.sent = []
        self.saved = []
        self.accounts = {
            bi: {"name": name, "status": "active", "email": name + "@x.test"}
            for name, bi in (("alice", ALICE), ("bob", BOB),
                             ("carol", CAROL), ("dave", DAVE))
        }
        self.messages = 0

    def channel(self, channel_id, creator, members=(), visibility="private",
                kind=""):
        record = {"name": "design" if channel_id == CHANNEL else channel_id[:4],
                  "visibility": visibility}
        if kind:
            record["kind"] = kind
        self.db.execute(
            "INSERT INTO chat_channels (channel_id,name_bi,data,created_by_bi,"
            "created_at,updated_at,key_version) VALUES (?,?,?,?,?,?,1)",
            (channel_id, "n:" + channel_id, json.dumps(record), _bi(creator),
             0, 0))
        for member in members:
            self.db.execute(
                "INSERT INTO chat_channel_members (channel_id,member_bi,data,"
                "invited_by_bi,joined_at) VALUES (?,?,?,?,0)",
                (channel_id, _bi(member), "{}", _bi(creator)))

    def post(self, channel_id, ts, notify=True):
        self.messages += 1
        body = {"kind": "cipher", "v": 1, "persist": True, "data": "zz"}
        if notify:
            body["notify"] = True
        self.db.execute(
            "INSERT INTO chat_history (room_key,msg_id,ts,body) "
            "VALUES (?,?,?,?)",
            ("chat-channel:%s:v1" % channel_id, "m%d" % self.messages, ts,
             json.dumps(body, separators=(",", ":"))))


def _load(env):
    spec = importlib.util.spec_from_file_location(
        "channel_reminders_under_test", SRC / "channel_reminders.py")
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


def _reads(env):
    return {
        row["account_bi"]: (row["read_at"], row["reminded_at"])
        for row in env.db.execute(
            "SELECT account_bi,read_at,reminded_at FROM chat_channel_reads")
    }


def test_members_with_unread_messages_get_one_reminder_until_they_read():
    env = Env()
    module = _load(env)
    env.channel(CHANNEL, "alice", ["bob", "carol"])
    posted = NOW - 40 * MINUTE
    env.post(CHANNEL, posted)
    env.post(CHANNEL, posted + 1, notify=False)
    _run(module.mark_read(env, CHANNEL, [ALICE], posted))
    _run(module.mark_read(env, CHANNEL, [CAROL], posted + 5 * MINUTE))

    assert _run(module.send_reminders(env)) == 1
    assert [mail["to"] for mail in env.sent] == ["bob@x.test"]
    mail = env.sent[0]
    assert mail["kind"] == "channel_unread"
    assert mail["subject"] == "ForkMesh: 1 unread message in #design"
    assert "#design: 1 unread message" in mail["text"]
    assert "https://forkmesh.com/chat" in mail["text"]
    assert "zz" not in mail["text"] and "zz" not in mail["html"]
    assert env.saved == [BOB]
    assert _reads(env)[BOB] == (0, NOW)

    env.post(CHANNEL, NOW + MINUTE)
    _run(module.mark_read(env, CHANNEL, [ALICE, CAROL], NOW + MINUTE))
    env.clock = NOW + 40 * MINUTE
    assert _run(module.send_reminders(env)) == 0
    assert len(env.sent) == 1

    _run(module.mark_read(env, CHANNEL, [BOB], env.clock))
    env.post(CHANNEL, env.clock + MINUTE)
    _run(module.mark_read(env, CHANNEL, [ALICE, CAROL], env.clock + MINUTE))
    env.clock += 45 * MINUTE
    assert _run(module.send_reminders(env)) == 1
    assert [mail["to"] for mail in env.sent] == ["bob@x.test", "bob@x.test"]
    assert env.sent[1]["subject"] == "ForkMesh: 1 unread message in #design"


def test_fresh_public_and_notice_messages_send_nothing():
    env = Env()
    module = _load(env)
    env.channel(CHANNEL, "alice", ["bob"])
    env.channel(PUBLIC, "alice", visibility="public")
    env.channel(NOTICES, "alice", kind="notices")
    env.post(CHANNEL, NOW - 10 * MINUTE)
    env.post(PUBLIC, NOW - 40 * MINUTE)
    env.post(NOTICES, NOW - 40 * MINUTE)

    assert _run(module.send_reminders(env)) == 0
    assert env.sent == []
    assert _reads(env) == {}


def test_opted_out_members_are_marked_without_mail():
    env = Env()
    module = _load(env)
    env.channel(CHANNEL, "alice", ["bob", "carol", "dave"])
    env.post(CHANNEL, NOW - 40 * MINUTE)
    env.accounts[ALICE]["muted"] = ["channel_unread"]
    env.accounts[BOB]["prefs"] = {"channel_unread": False}
    env.accounts[CAROL]["verified"] = False

    assert _run(module.send_reminders(env)) == 1
    assert [mail["to"] for mail in env.sent] == ["dave@x.test"]
    assert {bi for bi, (_read, reminded) in _reads(env).items()
            if reminded == NOW} == {ALICE, BOB, CAROL, DAVE}


def test_one_email_lists_every_channel_with_unread_messages():
    env = Env()
    module = _load(env)
    env.channel(CHANNEL, "alice", ["bob"])
    env.channel(PUBLIC, "alice", ["bob"])
    for offset in (50, 45, 40):
        env.post(CHANNEL, NOW - offset * MINUTE)
    env.post(PUBLIC, NOW - 35 * MINUTE)
    _run(module.mark_read(env, CHANNEL, [ALICE], NOW))
    _run(module.mark_read(env, PUBLIC, [ALICE], NOW))

    assert _run(module.send_reminders(env)) == 1
    mail = env.sent[0]
    assert mail["subject"] == "ForkMesh: 4 unread messages in 2 channels"
    assert "#design: 3 unread messages" in mail["text"]
    assert "#bbbb: 1 unread message\n" in mail["text"]


def test_mark_read_only_moves_forward_and_ignores_bad_ids():
    env = Env()
    module = _load(env)
    assert _run(module.mark_read(env, CHANNEL, [ALICE, ALICE, "x", ""], 50)) == 1
    assert _run(module.mark_read(env, CHANNEL, [ALICE, BOB], 20)) == 2
    assert _run(module.mark_read(env, "nope", [ALICE], 90)) == 0
    assert _reads(env) == {ALICE: (50, 0), BOB: (20, 0)}


def test_entry_wires_the_room_stamp_cron_phase_and_kind():
    assert '_channel_reminders = _LazyModule("channel_reminders")' in ENTRY
    assert 'if phase == "channel-unread-email":' in ENTRY
    assert "return await send_channel_unread_reminders(env)" in ENTRY
    assert "await chat_channel_mark_read(" in ENTRY
    assert '"channel_unread": True,' in ENTRY
