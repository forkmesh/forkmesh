from __future__ import annotations

import asyncio
import importlib.util
import json
import sqlite3
import types
from pathlib import Path

from forkmesh_host import ids_alerts

APP = Path(__file__).resolve().parents[2]
MIGRATIONS = ("0197_security_ids.sql", "0208_mail_budget.sql")
HOUR = 3600 * 1000
T0 = 1_790_000_000_000


def load_mail_budget():
    spec = importlib.util.spec_from_file_location(
        "mail_budget_under_test", APP / "src" / "mail_budget.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


mail_budget = load_mail_budget()


def run(coroutine):
    return asyncio.run(coroutine)


class SqliteRqlite:
    def __init__(self):
        self.conn = sqlite3.connect(":memory:")
        self.conn.row_factory = sqlite3.Row
        for name in MIGRATIONS:
            self.conn.executescript(
                (APP / "migrations" / name).read_text("utf-8"))

    async def query(self, sql, *params, level="none"):
        return [dict(r) for r in self.conn.execute(sql, params)]

    async def execute(self, statements, transaction=True):
        out = []
        for statement in statements:
            cursor = self.conn.execute(statement[0], statement[1:])
            out.append({"rows_affected": cursor.rowcount})
        return out

    def add(self, rid, severity="lockdown", node="n2",
            detector="ssh_unknown_key", created=T0, detail=None):
        if detail is None:
            detail = {"ip": "203.0.113.9"}
            if severity == "lockdown":
                detail["lockdown"] = {"seal": [True, "sealed"]}
        self.conn.execute(
            "INSERT INTO security_incidents (id, node_id, created_ms, "
            "severity, detector, summary, detail) VALUES (?,?,?,?,?,?,?)",
            (rid, node, created, severity, detector,
             "SSH login with a key that is not the fleet key",
             json.dumps(detail)))

    def row(self, rid):
        return dict(self.conn.execute(
            "SELECT * FROM security_incidents WHERE id=?", (rid,)).fetchone())


class FakeEntry:
    mail_budget = mail_budget

    def __init__(self, fail_mail=False, addresses=("ops@example.org.uk",)):
        self.pings = []
        self.mails = []
        self.fail_mail = fail_mail
        self.addresses = list(addresses)

    async def d1_all(self, env, sql, *args):
        return [{"data": "admin-a"}, {"data": "admin-b"}, {"data": "bad"}]

    async def decrypt_row(self, env, data):
        if data == "bad":
            raise ValueError("undecryptable")
        return {"name": data}

    @staticmethod
    def valid_node_name(name):
        return name.startswith("admin")

    async def enqueue_notification(self, env, admin, kind, title, **kw):
        self.pings.append((admin, kind, title, kw))
        return True

    async def _repository_monitor_admin_emails(self, env):
        return list(self.addresses)

    async def _send_email(self, env, to, subject, text, email_kind="x"):
        if self.fail_mail:
            raise RuntimeError("mailtrap down")
        self.mails.append((to, subject, text, email_kind))
        return True


class Clock:
    def __init__(self, value=T0):
        self.value = value

    def __call__(self):
        return self.value


def dispatcher(node, rq, entry, clock=None, sealed=False):
    runtime = types.SimpleNamespace(sealed=sealed, entry=entry, env=object(),
                                    _db_scope=None)
    membership = types.SimpleNamespace(rqlite=rq, hosting=True, node_id=node)
    return ids_alerts.IncidentDispatcher(runtime, membership, node,
                                         clock=clock or Clock())


def delivery(rq, rid):
    return json.loads(rq.row(rid)["delivery"])


def test_routing_info_records_alert_pings_lockdown_and_test_also_mail():
    rq = SqliteRqlite()
    rq.add("i1", "lockdown")
    rq.add("i2", "info", detector="reboot")
    rq.add("i3", "alert", detector="privilege_tool")
    rq.add("i4", "test", detector="test")
    entry = FakeEntry()
    first = dispatcher("n1", rq, entry)
    assert run(first.round()) == 3
    assert run(dispatcher("n3", rq, entry).round()) == 0
    assert rq.row("i2")["delivered_ms"] is None
    pinged = {p[3]["dedupe"] for p in entry.pings}
    assert pinged == {"ids:i1", "ids:i3", "ids:i4"}
    assert len(entry.pings) == 6
    assert {p[1] for p in entry.pings} == {"security_alert"}
    lockdown = [p for p in entry.pings if p[3]["dedupe"] == "ids:i1"][0]
    assert lockdown[2].startswith("LOCKDOWN on n2:")
    assert "next_ids.py release n2" in lockdown[3]["body"]
    mailed = sorted(m[1].split(":")[0] for m in entry.mails)
    assert mailed == ["IDS test (harmless) on n2", "LOCKDOWN on n2"]
    assert {m[3] for m in entry.mails} == {"security"}
    assert delivery(rq, "i1") == {"by": "n1", "pings": 2, "emails": 1,
                                  "errors": [], "email": "sent"}
    assert delivery(rq, "i3")["email"] == "severity"
    assert delivery(rq, "i3")["emails"] == 0
    assert rq.row("i3")["email_state"] == "severity"
    assert rq.row("i4")["email_state"] == "sent"


def test_one_email_per_node_and_detector_per_hour():
    rq = SqliteRqlite()
    clock = Clock()
    entry = FakeEntry()
    d = dispatcher("n1", rq, entry, clock)
    rq.add("a1")
    run(d.round())
    clock.value += 60_000
    rq.add("a2", created=clock.value)
    rq.add("b1", node="n3", created=clock.value)
    rq.add("c1", detector="ptrace", created=clock.value)
    run(d.round())
    assert [m[1].split(" on ")[1][:2] for m in entry.mails] == [
        "n2", "n3", "n2"]
    assert delivery(rq, "a2")["email"] == "dedupe"
    assert rq.row("a2")["email_state"] == "digested"
    assert "1 more IDS alert suppressed" in entry.mails[1][2]
    assert "suppressed" not in entry.mails[2][2]
    assert len([p for p in entry.pings if p[3]["dedupe"] == "ids:a2"]) == 2
    clock.value = T0 + HOUR
    rq.add("a3", created=clock.value)
    run(d.round())
    assert rq.row("a3")["email_state"] == "sent"
    assert len(entry.mails) == 4


def test_daily_cap_then_digest_in_the_next_allowed_email():
    rq = SqliteRqlite()
    clock = Clock()
    entry = FakeEntry()
    d = dispatcher("n1", rq, entry, clock)
    for n in range(8):
        rq.add("x%d" % n, detector="det%d" % n, created=clock.value + n)
    run(d.round())
    run(d.round())
    assert len(entry.mails) == 5
    states = [rq.row("x%d" % n)["email_state"] for n in range(8)]
    assert states == ["sent"] * 5 + ["cap"] * 3
    assert len({p[3]["dedupe"] for p in entry.pings}) == 8
    for text in (m[2] for m in entry.mails):
        assert "suppressed" not in text
    clock.value += 24 * HOUR
    rq.add("next", detector="det9", created=clock.value)
    run(d.round())
    assert len(entry.mails) == 6
    assert "3 more IDS alerts suppressed" in entry.mails[-1][2]
    assert delivery(rq, "next")["digest"] == 3
    assert all(rq.row("x%d" % n)["email_state"] == "digested"
               for n in range(5, 8))
    rq.add("later", detector="det10", created=clock.value + 1)
    run(d.round())
    assert "suppressed" not in entry.mails[-1][2]


def test_failed_email_refunds_dedupe_and_cap():
    rq = SqliteRqlite()
    clock = Clock()
    entry = FakeEntry(fail_mail=True)
    d = dispatcher("n1", rq, entry, clock)
    rq.add("f1")
    run(d.round())
    assert rq.row("f1")["email_state"] == "failed"
    assert rq.row("f1")["delivered_ms"]
    entry.fail_mail = False
    rq.add("f2", created=clock.value + 1)
    run(d.round())
    assert rq.row("f2")["email_state"] == "sent"
    used = rq.conn.execute(
        "SELECT used FROM mail_budget WHERE bucket=?",
        ("ids-mail:" + mail_budget.day_key(clock.value),)).fetchone()[0]
    assert used == 1


def test_no_deliverable_recipient_counts_as_failed_and_refunds():
    rq = SqliteRqlite()
    entry = FakeEntry(addresses=())
    d = dispatcher("n1", rq, entry)
    rq.add("z1")
    run(d.round())
    assert rq.row("z1")["email_state"] == "failed"
    assert rq.row("z1")["delivered_ms"]
    entry.addresses = ["ops@example.org.uk"]
    rq.add("z2", created=T0 + 1)
    run(d.round())
    assert rq.row("z2")["email_state"] == "sent"


def test_dispatcher_keeps_trying_when_every_channel_fails():
    rq = SqliteRqlite()
    rq.add("i1")
    entry = FakeEntry(fail_mail=True)

    async def broken(*args, **kw):
        raise RuntimeError("apns down")
    entry.enqueue_notification = broken
    d = dispatcher("n1", rq, entry)
    assert run(d.round()) == 0
    assert rq.row("i1")["delivered_ms"] is None
    assert rq.row("i1")["claimed_by"] == "n1"


def test_sealed_node_does_not_dispatch():
    rq = SqliteRqlite()
    rq.add("i1")
    entry = FakeEntry()
    assert run(dispatcher("n1", rq, entry, sealed=True).round()) == 0
    assert entry.pings == [] and rq.row("i1")["claimed_ms"] is None


def test_lockdown_waits_for_its_steps_then_pings_everything():
    rq = SqliteRqlite()
    clock = Clock()
    entry = FakeEntry()
    d = dispatcher("n1", rq, entry, clock)
    processes = [{"pid": 4000 + n, "exe": "/usr/bin/nc" + "x" * 200}
                 for n in range(6)]
    detail = {"key%02d" % n: "v%d" % n for n in range(25)}
    detail["processes"] = processes
    rq.add("l1", detail=detail)
    assert run(d.round()) == 0
    assert rq.row("l1")["claimed_ms"] is None
    detail["lockdown"] = {"seal": [True, "sealed"],
                          "firewall": [False, "nft: exit 1"],
                          "rqlite": [True, "kept: no quorum without it"]}
    rq.conn.execute("UPDATE security_incidents SET detail=? WHERE id='l1'",
                    (json.dumps(detail),))
    clock.value += 2_000
    assert run(d.round()) == 1
    body = entry.pings[0][3]["body"]
    assert len(body) > 900
    assert "key24: v24" in body
    assert json.dumps(processes, sort_keys=True) in body
    assert "  seal: ok (sealed)" in body
    assert "  firewall: FAILED (nft: exit 1)" in body
    assert "  rqlite: ok (kept: no quorum without it)" in body
    assert "next_ids.py release n2" in body
    assert entry.pings[0][3]["meta"]["detector"] == "ssh_unknown_key"
    assert entry.mails[0][2] == body


def test_lockdown_without_steps_pings_once_the_settle_time_passes():
    rq = SqliteRqlite()
    clock = Clock()
    entry = FakeEntry()
    d = dispatcher("n1", rq, entry, clock)
    rq.add("l2", detail={"ip": "203.0.113.9"})
    assert run(d.round()) == 0
    clock.value += ids_alerts.LOCKDOWN_SETTLE_MS
    assert run(d.round()) == 1
    assert "Lockdown steps" not in entry.pings[0][3]["body"]
