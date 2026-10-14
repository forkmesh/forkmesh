import asyncio
import json

from test_d1_read_diet import _notification_db, _notification_writer
from test_notifications import (
    NOW, _add_rows, _load_digest_sender, _row, _verified_account)


def _instant(ns, name):
    rec = _verified_account(name, delivery="instant")
    rec["last_digest_ts"] = NOW - 5 * 60 * 1000
    ns["accounts"][name + "-bi"] = rec
    return rec


def _deliver(ns, name, rec):
    return asyncio.run(ns["_deliver_instant_notifications"](
        object(), name + "-bi", dict(rec), rec, rec["email"], NOW))


def test_two_consumers_reading_the_same_watermark_email_a_ping_once():
    sent, saved = [], []
    ns = _load_digest_sender(sent, saved)
    rec = _instant(ns, "ada")
    _add_rows(ns, _row("ada-bi", NOW - 30_000, "Reply"))

    assert _deliver(ns, "ada", rec) == 1
    assert _deliver(ns, "ada", rec) == 1

    assert [item["subject"] for item in sent] == ["ForkMesh: Reply"]
    assert saved[-1][1]["last_digest_ts"] == NOW - 30_000
    assert len(ns["email_sends"]) == 1
    assert all(key.startswith("notification-email:") and key.endswith("-bi")
               for key in ns["email_sends"])


def test_different_pings_to_the_same_address_both_send():
    sent, saved = [], []
    ns = _load_digest_sender(sent, saved)
    rec = _instant(ns, "ada")
    _add_rows(ns, _row("ada-bi", NOW - 31_000, "First"),
              _row("ada-bi", NOW - 30_000, "Second"))

    assert _deliver(ns, "ada", rec) == 2
    assert _deliver(ns, "ada", rec) == 2

    assert [item["subject"] for item in sent] == [
        "ForkMesh: First", "ForkMesh: Second"]
    assert len(ns["email_sends"]) == 2


def test_a_failed_send_releases_its_claim_so_the_retry_mails_it():
    sent, saved = [], []
    ns = _load_digest_sender(sent, saved)
    rec = _instant(ns, "ada")
    _add_rows(ns, _row("ada-bi", NOW - 30_000, "Reply"))
    real_send = ns["_send_email"]

    async def failing(*_args, **_kwargs):
        return False

    ns["_send_email"] = failing
    assert _deliver(ns, "ada", rec) == 0
    assert ns["email_sends"] == {}
    ns["_send_email"] = real_send
    assert _deliver(ns, "ada", rec) == 1
    assert [item["subject"] for item in sent] == ["ForkMesh: Reply"]


def test_a_digest_leaves_out_items_another_core_already_mailed():
    sent, saved = [], []
    ns = _load_digest_sender(sent, saved)
    rec = _verified_account("grace")
    ns["accounts"]["grace-bi"] = rec
    old = NOW - 10 * 60 * 1000
    _add_rows(ns, _row("grace-bi", old, "Ping 0"),
              _row("grace-bi", old + 1000, "Ping 1"))

    asyncio.run(ns["send_notification_digests"](
        object(), include_general_chat=False))
    rec.pop("last_digest_ts", None)
    asyncio.run(ns["send_notification_digests"](
        object(), include_general_chat=False))

    assert [item["subject"] for item in sent] == [
        "ForkMesh: 2 new notifications"]
    assert saved[-1][1]["last_digest_ts"] == old + 1000

    _add_rows(ns, _row("grace-bi", old + 2000, "Ping 2"))
    rec.pop("last_digest_ts", None)
    asyncio.run(ns["send_notification_digests"](
        object(), include_general_chat=False))
    assert [item["subject"] for item in sent] == [
        "ForkMesh: 2 new notifications", "ForkMesh: 1 new notifications"]


def test_a_refile_with_the_same_content_keeps_its_ts_and_is_not_mailed_again():
    db = _notification_db()
    enqueue, _runs = _notification_writer(db, None)

    def stored():
        return db.execute(
            "SELECT ts, data FROM notifications WHERE dedupe_bi=?",
            ("bi:notification:alice:x",)).fetchone()

    assert asyncio.run(enqueue(object(), "alice", "issue", "t", ts=100,
                               dedupe="x"))
    first = stored()
    assert asyncio.run(enqueue(object(), "alice", "issue", "t", ts=200,
                               dedupe="x"))
    assert stored() == first
    assert json.loads(first[1])["ts"] == 100

    assert asyncio.run(enqueue(object(), "alice", "issue", "t2", ts=300,
                               dedupe="x"))
    ts, data = stored()
    assert ts == 300 and json.loads(data)["title"] == "t2"


def test_old_send_claims_are_pruned_and_fresh_ones_kept():
    ns = _load_digest_sender([], [])
    retain = ns["NOTIFICATION_EMAIL_SEND_RETAIN_MS"]
    assert retain == 30 * 24 * 60 * 60 * 1000
    ns["email_sends"].update({
        "old": NOW - retain - 1,
        "edge": NOW - retain,
        "fresh": NOW - 1000,
    })
    asyncio.run(ns["prune_notification_email_sends"](object()))
    assert sorted(ns["email_sends"]) == ["edge", "fresh"]


def test_the_prune_rides_the_hourly_referrer_prune_phase():
    from test_notifications import ENTRY_TEXT
    phase = ENTRY_TEXT.split('if phase == "referrer-prune":', 1)[1]
    phase = phase.split("if phase ==", 1)[0]
    assert "await prune_notification_email_sends(env)" in phase
