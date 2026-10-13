import json

from test_runner_leases import FakeRuntime, Key, ROOT, call, signed

PATH = "/api/runners/mail-log-note"


def runtime_with_notes():
    runtime = FakeRuntime()
    runtime.db.executescript(
        (ROOT / "migrations" / "0216_mail_log_node_notes.sql").read_text()
        .split("INSERT INTO schema_meta")[0])
    return runtime


def notes(runtime):
    return [(r[0], json.loads(r[1])) for r in runtime.db.execute(
        "SELECT node, data FROM mail_log_node_notes")]


def test_a_signed_runner_note_is_queued_for_email_log():
    runtime = runtime_with_notes()
    key = Key()
    runtime.add_runner("fmnext-r1", key)
    body = {"kind": "ids_direct", "to": "o***@example.org", "subject": "s",
            "result": "accepted", "status": 200, "from": "no-reply@forkmesh.com"}
    reply = call(signed(runtime, "fmnext-r1", key, PATH, body), PATH)
    assert reply["status"] == 200
    [(node, data)] = notes(runtime)
    assert node == "fmnext-r1" and data["result"] == "accepted"
    assert data["to"] == "o***@example.org"


def test_a_note_keeps_the_body_route_source_and_message_id_of_its_send():
    runtime = runtime_with_notes()
    key = Key()
    runtime.add_runner("fmnext-r1", key)
    body = {"kind": "ids_direct", "to": "o***@example.org",
            "subject": "[fmnext-ids lockdown] n1: unknown binary",
            "body": "{\n  \"detector\": \"binaries\"\n}",
            "via": "postfix", "source": "the IDS on n1",
            "messageId": "179066419708.25@forkmesh.com",
            "result": "accepted", "status": 200,
            "from": "no-reply@forkmesh.com"}
    assert call(signed(runtime, "fmnext-r1", key, PATH, body), PATH)[
        "status"] == 200
    [(_node, data)] = notes(runtime)
    assert data["body"] == '{  "detector": "binaries"}'
    assert (data["via"], data["source"], data["messageId"]) == (
        "postfix", "the IDS on n1", body["messageId"])


def test_unsigned_and_empty_notes_are_refused_and_rate_limited():
    runtime = runtime_with_notes()
    key, stranger = Key(), Key()
    runtime.add_runner("fmnext-r1", key)
    body = {"result": "failed"}
    assert call(runtime.use(json.dumps(body).encode()), PATH)["status"] == 401
    assert call(signed(runtime, "fmnext-r1", stranger, PATH, body),
                PATH)["status"] == 401
    assert call(signed(runtime, "fmnext-r1", key, PATH, {}), PATH)[
        "status"] == 400
    for _ in range(120):
        assert call(signed(runtime, "fmnext-r1", key, PATH, body), PATH)[
            "status"] == 200
    assert call(signed(runtime, "fmnext-r1", key, PATH, body), PATH)[
        "status"] == 429
