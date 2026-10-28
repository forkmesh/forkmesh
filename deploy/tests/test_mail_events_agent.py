import collections
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "core"))
import mail_events_agent as agent  # noqa: E402


def feed(lines):
    ids, deferred = collections.OrderedDict(), collections.OrderedDict()
    return [e for e in (agent.event_for(line, ids, deferred, "n1")
                        for line in lines) if e]


def test_delivery_bounce_and_first_deferral():
    events = feed([
        "4AB: message-id=<m1@forkmesh.com>",
        "4AB: to=<a@example.com>, relay=mx[1.2.3.4]:25, delay=1, dsn=4.7.0, "
        "status=deferred (host mx said: 421 try later)",
        "4AB: to=<a@example.com>, relay=mx[1.2.3.4]:25, delay=9, dsn=4.7.0, "
        "status=deferred (host mx said: 421 try later)",
        "4AB: to=<a@example.com>, relay=mx[1.2.3.4]:25, delay=20, dsn=2.0.0, "
        "status=sent (250 ok)",
        "5CD: to=<b@example.com>, relay=mx[1.2.3.4]:25, delay=1, dsn=5.1.1, "
        "status=bounced (host mx said: 550 5.1.1 unknown user)",
    ])
    assert [e["event"] for e in events] == ["soft bounce", "delivery", "bounce"]
    assert events[1]["message_id"] == "m1@forkmesh.com"
    assert events[2]["response_code"] == 550 and events[2]["message_id"] == ""
    assert len({e["event_id"] for e in events}) == 3


def test_spool_names_sort_by_time(tmp_path, monkeypatch):
    tmp_path = tmp_path / "spool"
    tmp_path.mkdir()
    monkeypatch.setattr(agent, "SPOOL", str(tmp_path))
    for index in range(3):
        agent.write({"event_id": "smtp:%040d" % index, "event": "delivery"})
    names = sorted(p.name for p in tmp_path.iterdir())
    assert len(names) == 3 and all(n.endswith(".json") for n in names)


def test_events_carry_the_sender_postfix_queued():
    ids, deferred = collections.OrderedDict(), collections.OrderedDict()
    senders = collections.OrderedDict()
    lines = ["7EF: from=<root@n2.next.forkmesh.com>, size=512, nrcpt=1 (queue active)",
             "7EF: to=<a@example.com>, relay=mx[1.2.3.4]:25, dsn=2.0.0, "
             "status=sent (250 ok)",
             "8AA: from=<>, size=900, nrcpt=1 (queue active)",
             "8AA: to=<b@example.com>, relay=mx[1.2.3.4]:25, dsn=2.0.0, "
             "status=sent (250 ok)"]
    events = [e for e in (agent.event_for(line, ids, deferred, "n2", senders)
                          for line in lines) if e]
    assert [e["sender"] for e in events] == ["root@n2.next.forkmesh.com", "<>"]
    assert events[0]["node"] == "n2"


def test_a_handoff_to_the_relay_core_is_not_a_delivery():
    events = feed([
        "4AB: message-id=<m1@forkmesh.com>",
        "4AB: to=<a@yahoo.com>, relay=10.77.0.4[10.77.0.4]:25, delay=0.1, "
        "dsn=2.0.0, status=sent (250 2.0.0 Ok: queued as 9F1)",
        "4AB: to=<a@yahoo.com>, relay=10.77.0.4[10.77.0.4]:25, delay=0.1, "
        "dsn=4.4.1, status=deferred (connect to 10.77.0.4[10.77.0.4]:25: "
        "Connection refused)",
    ])
    assert [e["event"] for e in events] == ["soft bounce"]
