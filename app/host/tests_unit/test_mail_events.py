import asyncio
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from forkmesh_host import mail_events  # noqa: E402


class Entry:
    def __init__(self, fail_on=None):
        self.seen = []
        self.fail_on = fail_on

    async def record_core_mail_event(self, env, event):
        if event.get("event_id") == self.fail_on:
            raise RuntimeError("db down")
        self.seen.append(event["event_id"])


class Runtime:
    env = object()
    sealed = False

    def __init__(self, entry):
        self.entry = entry


def spool(tmp_path, count):
    directory = tmp_path / "spool"
    directory.mkdir()
    for index in range(count):
        (directory / ("%020d-x.json" % index)).write_text(
            json.dumps({"event_id": "e%d" % index}))
    (directory / "bad.json").write_text("{")
    return directory


def test_drain_advances_cursor_and_skips_seen(tmp_path):
    directory = spool(tmp_path, 3)
    entry = Entry()
    worker = mail_events.MailEvents(Runtime(entry), directory,
                                    tmp_path / "cursor")
    asyncio.run(worker.drain())
    asyncio.run(worker.drain())
    assert entry.seen == ["e0", "e1", "e2"]


def test_drain_stops_at_failure_and_retries(tmp_path):
    directory = spool(tmp_path, 3)
    entry = Entry(fail_on="e1")
    worker = mail_events.MailEvents(Runtime(entry), directory,
                                    tmp_path / "cursor")
    asyncio.run(worker.drain())
    entry.fail_on = None
    asyncio.run(worker.drain())
    assert entry.seen == ["e0", "e1", "e2"]
