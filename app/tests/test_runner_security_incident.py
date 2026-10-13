"""POST /api/runners/security-incident: a runner's IDS reports (docs/IDS.md);
a lockdown revokes that runner and nothing else."""

import json

from test_runner_leases import FakeRuntime, Key, ROOT, call, signed

PATH = "/api/runners/security-incident"


def runtime_with_incidents():
    runtime = FakeRuntime()
    runtime.db.executescript(
        (ROOT / "migrations" / "0197_security_ids.sql").read_text())
    return runtime


def incidents(runtime):
    return [dict(r) for r in runtime.db.execute(
        "SELECT node_id, severity, detector, summary FROM security_incidents")]


def status(runtime, name):
    return runtime.db.execute("SELECT status FROM runner_nodes WHERE name=?",
                              (name,)).fetchone()[0]


def test_alert_is_recorded_and_the_runner_stays_active():
    runtime = runtime_with_incidents()
    key = Key()
    runtime.add_runner("fmnext-r1", key)
    body = {"severity": "alert", "detector": "listener_unexpected",
            "summary": "nc listens on 4444", "detail": {"port": 4444}}
    reply = call(signed(runtime, "fmnext-r1", key, PATH, body), PATH)
    assert reply["status"] == 200 and reply["data"]["revoked"] is False
    assert incidents(runtime) == [{"node_id": "fmnext-r1", "severity": "alert",
                                   "detector": "listener_unexpected",
                                   "summary": "nc listens on 4444"}]
    assert status(runtime, "fmnext-r1") == "active"


def test_lockdown_revokes_only_the_caller():
    runtime = runtime_with_incidents()
    key, other = Key(), Key()
    runtime.add_runner("fmnext-r1", key)
    runtime.add_runner("fmnext-r2", other)
    body = {"severity": "lockdown", "detector": "job_user_outside_scope",
            "summary": "escape", "detail": {}, "name": "fmnext-r2"}
    reply = call(signed(runtime, "fmnext-r1", key, PATH, body), PATH)
    assert reply["data"]["revoked"] is True
    assert status(runtime, "fmnext-r1") == "revoked"
    assert status(runtime, "fmnext-r2") == "active"
    detail = json.loads(runtime.db.execute(
        "SELECT detail FROM security_incidents").fetchone()[0])
    assert detail["lockdown"] == {
        "revoke": [True, "the app revoked runner fmnext-r1"]}
    # Revoked: its signatures no longer verify.
    again = call(signed(runtime, "fmnext-r1", key, PATH, body), PATH)
    assert again["status"] == 401


def test_unsigned_forged_and_bad_bodies_are_refused():
    runtime = runtime_with_incidents()
    key, stranger = Key(), Key()
    runtime.add_runner("fmnext-r1", key)
    body = {"severity": "lockdown", "detector": "x", "summary": "y"}
    assert call(runtime.use(json.dumps(body).encode()), PATH)["status"] == 401
    assert call(signed(runtime, "fmnext-r1", stranger, PATH, body),
                PATH)["status"] == 401
    assert status(runtime, "fmnext-r1") == "active"
    bad = {"severity": "catastrophic", "detector": "x", "summary": "y"}
    assert call(signed(runtime, "fmnext-r1", key, PATH, bad), PATH)[
        "status"] == 400


def test_incidents_are_rate_limited_per_runner():
    runtime = runtime_with_incidents()
    key = Key()
    runtime.add_runner("fmnext-r1", key)
    body = {"severity": "info", "detector": "x", "summary": "y"}
    for _ in range(30):
        assert call(signed(runtime, "fmnext-r1", key, PATH, body), PATH)[
            "status"] == 200
    assert call(signed(runtime, "fmnext-r1", key, PATH, body), PATH)[
        "status"] == 429


def test_the_runner_ids_incident_id_is_kept_but_never_overwrites():
    runtime = runtime_with_incidents()
    key = Key()
    runtime.add_runner("fmnext-r1", key)
    body = {"id": "tqrGew4LJHuz72kz", "severity": "test",
            "detector": "test_fire", "summary": "test", "detail": {}}
    reply = call(signed(runtime, "fmnext-r1", key, PATH, body), PATH)
    assert reply["data"]["id"] == "tqrGew4LJHuz72kz"
    # The same id again (a replay, or a guess at another row) gets a new one.
    again = call(signed(runtime, "fmnext-r1", key, PATH, body), PATH)
    assert again["status"] == 200
    assert again["data"]["id"] != "tqrGew4LJHuz72kz"
    # A malformed id is ignored.
    body["id"] = "../../x"
    odd = call(signed(runtime, "fmnext-r1", key, PATH, body), PATH)
    assert odd["data"]["id"] not in ("../../x", "tqrGew4LJHuz72kz")
    assert len(incidents(runtime)) == 3
