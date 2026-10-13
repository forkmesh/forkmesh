import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import runner_auth  # noqa: E402
from test_runner_leases import NOW, claim  # noqa: E402
from test_runner_logins import (  # noqa: E402
    FakeRuntime as LoginRuntime, browser, runner_post, setup as login_setup)


class FakeRuntime(LoginRuntime):
    def __init__(self):
        super().__init__()
        self.alerts = []

    async def alert_runner_auth(self, **alert):
        self.alerts.append(alert)
        return 1


def setup():
    base, pool_key, own_key = login_setup()
    runtime = FakeRuntime()
    runtime.db = base.db
    runtime.db.execute(
        "UPDATE runner_nodes SET allowed_owners=? WHERE name='pool-1'",
        (json.dumps(["alice"]),))
    runtime.db.commit()
    return runtime, pool_key, own_key


def availability(state="ok", login="available", failed_at=0, expires_at=0,
                 with_auth=True, provider="claude-code"):
    entry = {"binaryFound": True, "loginState": login,
             "source": "device login", "accounts": []}
    if with_auth:
        entry["auth"] = {"state": state, "checkedAt": NOW,
                         "expiresAt": expires_at, "hasRefresh": True,
                         "reason": "job_failed" if failed_at else "",
                         "message": "Claude Code login on this node expired.",
                         "failedAt": failed_at, "failedJob": 198 if failed_at else 0}
    return {provider: entry}


def poll(runtime, key, value, runner="pool-1"):
    response = runner_post(runtime, runner, key, "poll", {"availability": value})
    assert response["status"] == 200
    return response


def stored(runtime, runner="pool-1"):
    row = runtime.db.execute(
        "SELECT data FROM runner_availability WHERE runner=?", (runner,)).fetchone()
    return json.loads(row[0]) if row else {}


def add_lease(runtime, job_id, runner="pool-1", lease="lease-%d"):
    runtime.db.execute(
        "INSERT INTO runner_job_leases (job_id,runner,session_id,lease_id,"
        "grant_id,target_node,repo,ref,expires_at,created_at) "
        "VALUES (?,?,?,?,?,?,?,?,?,?)",
        (job_id, runner, "s%d" % job_id, lease % job_id, "g", "core-a", "proj",
         "refs/heads/agent/s%d" % job_id, NOW + 3600_000, NOW))
    runtime.db.commit()
    return lease % job_id


def report(runtime, key, job_id, status, result="", **extra):
    lease = add_lease(runtime, job_id)
    body = {"jobId": job_id, "leaseId": lease, "status": status,
            "securityVerdict": "approved", "securityReason": "Prompt dispatched.",
            "result": result, "availability": {"provider": "claude-code",
                                               "loginState": "available"},
            **extra}
    return runner_post(runtime, "pool-1", key, "report", body)


def test_classification_matches_provider_failures_only():
    assert runner_auth.auth_failure_text(
        "Failed to authenticate: OAuth session expired and could not be refreshed")
    assert runner_auth.auth_failure_text("", "API Error: 401 authentication_error")
    assert runner_auth.auth_failure_text("refresh_token_reused")
    assert not runner_auth.auth_failure_text("The agent changed 401 lines.")
    assert not runner_auth.auth_failure_text("API Error: 529 overloaded", "")


def test_clean_keeps_known_fields_and_drops_junk():
    cleaned = runner_auth.clean({
        "state": "expired", "checkedAt": NOW, "reason": "job_failed",
        "message": "bad\x1b[31m text", "failedAt": NOW, "failedJob": 9,
        "token": "sk-ant-secret"})
    assert cleaned["state"] == "expired" and cleaned["failedJob"] == 9
    assert "token" not in cleaned and "\x1b" not in cleaned["message"]
    assert runner_auth.clean({"state": "hacked"}) is None
    assert runner_auth.clean({"state": "ok", "reason": "DROP TABLE"})["reason"] == ""


def test_transitions_and_dedupe_are_per_runner_and_provider():
    old = availability()
    new = availability("expired", login="expired", failed_at=NOW)
    changes = runner_auth.transitions(old, new)
    assert [(p, s) for p, s, _a in changes] == [("claude-code", "expired")]
    assert runner_auth.transitions(new, new) == []
    key = runner_auth.dedupe_key("fmnext-r1", "claude-code", "expired", changes[0][2], NOW)
    assert key == "runner-auth:fmnext-r1:claude-code:expired:%d" % NOW
    assert runner_auth.dedupe_key("fmnext-r1", "codex", "expired", {}, NOW) != key
    title, body = runner_auth.alert_text("fmnext-r1", "claude-code", "expired", changes[0][2])
    assert title == "fmnext-r1: Claude login expired — reconnect"
    assert "Connect Claude" in body
    title, _body = runner_auth.alert_text(
        "fmnext-r1", "codex", "expiring", {"expiresAt": NOW + 3600_000})
    assert title == "fmnext-r1: Codex login expires soon — reconnect"


def test_status_reason_and_submit_block():
    expired = availability("expired", login="expired", failed_at=NOW)
    assert runner_auth.status_reason("fmnext-r1", expired) == (
        "fmnext-r1: Claude login expired — reconnect")
    assert runner_auth.status_reason("fmnext-r1", availability()) == ""
    assert runner_auth.status_reason("fmnext-r1", availability(
        "expired", login="expired", with_auth=False)) != ""
    blocked = runner_auth.submit_block(
        [{"node": "core-a", "providers": [],
          "needsLogin": {"claude-code": ["fmnext-r1"]}}], "claude-code")
    assert blocked["error"] == "runner_login_expired"
    assert blocked["message"].startswith("Runner fmnext-r1 needs Claude reconnected")
    assert blocked["requiredAction"] == "reconnect_runner_login"
    assert runner_auth.submit_block(
        [{"needsLogin": {"claude-code": ["fmnext-r1"]}}], "codex") is None


def test_poll_with_expired_login_alerts_once_and_is_stored():
    runtime, pool_key, _own = setup()
    poll(runtime, pool_key, availability())
    assert runtime.alerts == []
    poll(runtime, pool_key, availability("expired", login="expired", failed_at=NOW))
    poll(runtime, pool_key, availability("expired", login="expired", failed_at=NOW))
    assert len(runtime.alerts) == 1
    alert = runtime.alerts[0]
    assert alert["runner"] == "pool-1" and alert["provider"] == "claude-code"
    assert alert["state"] == "expired" and alert["email"] is False
    assert alert["href"] == "/dashboard/runners?runner=pool-1&provider=claude-code"
    assert alert["dedupe"] == "runner-auth:pool-1:claude-code:expired:%d" % NOW
    assert stored(runtime)["claude-code"]["auth"]["state"] == "expired"
    listed = browser(runtime, "root", "/api/runners", method="GET")
    pool = [r for r in listed["data"]["runners"] if r["name"] == "pool-1"][0]
    assert pool["availability"]["claude-code"]["auth"]["reason"] == "job_failed"


def test_expiring_login_warns_without_email_and_personal_runner_owner_is_told():
    runtime, _pool_key, own_key = setup()
    poll(runtime, own_key, availability("expiring", expires_at=NOW + 6 * 3600_000),
         runner="alice-box")
    poll(runtime, own_key, availability("expired", login="expired", failed_at=NOW),
         runner="alice-box")
    assert [a["state"] for a in runtime.alerts] == ["expiring", "expired"]
    assert runtime.alerts[0]["email"] is False
    assert runtime.alerts[1]["email"] is True
    assert runtime.alerts[1]["extra"] == ["alice"]


def test_reconnect_then_expire_again_alerts_again():
    runtime, pool_key, _own = setup()
    poll(runtime, pool_key, availability("expired", login="expired", failed_at=NOW))
    poll(runtime, pool_key, availability())
    poll(runtime, pool_key, availability("expired", login="expired", failed_at=NOW + 5))
    assert [a["dedupe"] for a in runtime.alerts] == [
        "runner-auth:pool-1:claude-code:expired:%d" % NOW,
        "runner-auth:pool-1:claude-code:expired:%d" % (NOW + 5)]


def test_old_runner_auth_failure_report_marks_login_expired_until_reconnected():
    runtime, pool_key, _own = setup()
    poll(runtime, pool_key, availability(with_auth=False))
    response = report(runtime, pool_key, 198, "failed",
                      "Failed to authenticate: OAuth session expired and could not be refreshed")
    assert response["status"] == 200
    assert stored(runtime)["claude-code"]["loginState"] == "expired"
    assert [a["state"] for a in runtime.alerts] == ["expired"]

    poll(runtime, pool_key, availability(with_auth=False))
    assert stored(runtime)["claude-code"]["loginState"] == "expired"
    job = runtime.add_job(provider="claude-code")
    leased = claim(runtime, "pool-1", pool_key, {"max": 1, "providers": ["claude-code"]})
    assert leased["status"] == 200 and leased["data"]["jobs"] == []
    assert runtime.job_status(job) == "queued"
    assert len(runtime.alerts) == 1

    runtime.db.execute(
        "INSERT INTO runner_login_sessions (id,runner,provider,state,created_by,"
        "created_at,updated_at,expires_at) VALUES (?,?,?,?,?,?,?,?)",
        ("f" * 32, "pool-1", "claude-code", "succeeded", "root", NOW, NOW + 10,
         NOW + 600_000))
    runtime.db.commit()
    poll(runtime, pool_key, availability(with_auth=False))
    assert stored(runtime)["claude-code"]["loginState"] == "available"
    leased = claim(runtime, "pool-1", pool_key, {"max": 1, "providers": ["claude-code"]})
    assert [j["jobId"] for j in leased["data"]["jobs"]] == [job]


def test_failure_kind_marks_expired_and_ordinary_failures_do_not():
    runtime, pool_key, _own = setup()
    report(runtime, pool_key, 200, "failed", "Claude Code exited with status 1: tests failed")
    assert runtime.alerts == []
    report(runtime, pool_key, 201, "failed", "", failureKind="auth_expired")
    assert stored(runtime)["claude-code"]["auth"]["failedJob"] == 201
    assert len(runtime.alerts) == 1


def test_new_runner_with_fresh_login_is_not_held_by_stored_expiry():
    runtime, pool_key, _own = setup()
    poll(runtime, pool_key, availability("expired", login="expired", failed_at=NOW))
    job = runtime.add_job(provider="claude-code")
    body = {"max": 1, "providers": ["claude-code"], "availability": availability()}
    leased = claim(runtime, "pool-1", pool_key, body)
    assert [j["jobId"] for j in leased["data"]["jobs"]] == [job]
