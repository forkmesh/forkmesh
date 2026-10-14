"""Web sign-in of a runner's provider CLI (runner_logins.py)."""

import asyncio
import base64
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import runner_logins  # noqa: E402
from test_runner_leases import (  # noqa: E402
    FakeRuntime as LeaseRuntime, Key, NOW, call, claim, signed)


class FakeRuntime(LeaseRuntime):
    def __init__(self):
        super().__init__()
        self.query_values = {}
        self.audits = []

    def use(self, raw=b"", headers=None, actor="", method="POST", query=None):
        self.query_values = dict(query or {})
        return super().use(raw, headers, actor, method)

    def query(self, name):
        return str(self.query_values.get(name, ""))

    async def audit(self, *args):
        self.audits.append(args)

    def add_runner(self, name, key, scope="pool", owners=(), status="active",
                   registered_by="root"):
        super().add_runner(name, key, scope, owners, status)
        self.db.execute("UPDATE runner_nodes SET registered_by=? WHERE name=?",
                        (registered_by, name))
        self.db.commit()


def browser(runtime, actor, path, body=None, method="POST", **query):
    raw = json.dumps(body).encode() if body is not None else b""
    return call(runtime.use(raw, actor=actor, method=method, query=query), path)


def start(runtime, actor, runner, provider="claude-code"):
    return browser(runtime, actor, "/api/runners/%s/login" % runner,
                   {"provider": provider})


def runner_post(runtime, name, key, action, body, **kwargs):
    path = "/api/runners/" + action
    return call(signed(runtime, name, key, path, body, **kwargs), path)


def b64(text):
    return base64.b64encode(text.encode()).decode()


def chunk_rows(runtime, session_id, direction=None):
    sql = "SELECT direction,seq FROM runner_login_chunks WHERE session_id=?"
    args = [session_id]
    if direction:
        sql += " AND direction=?"
        args.append(direction)
    return runtime.db.execute(sql, args).fetchall()


def setup():
    runtime = FakeRuntime()
    pool_key, own_key = Key(), Key()
    runtime.add_runner("pool-1", pool_key)
    runtime.add_runner("alice-box", own_key, scope="personal",
                       owners=["alice"], registered_by="alice")
    return runtime, pool_key, own_key


def test_routes_are_matched_without_shadowing_existing_actions():
    assert runner_logins.match("") == ("list",)
    assert runner_logins.match("poll") == ("poll",)
    assert runner_logins.match("login/output") == ("runner-output",)
    assert runner_logins.match("login/input-poll") == ("runner-input-poll",)
    assert runner_logins.match("r1/login") == ("create", "r1")
    assert runner_logins.match("login/login") == ("create", "login")
    assert runner_logins.match("r1/login/abc/cancel") == ("cancel", "r1", "abc")
    assert runner_logins.match("r1/login/abc/resize") == ("resize", "r1", "abc")
    for action in ("claim", "report", "register", "revoke", "pool-status",
                   "r1/login/abc/other", "a/b"):
        assert runner_logins.match(action) is None


def test_only_the_registrant_sees_or_drives_a_runner():
    runtime, _pool, _own = setup()
    runtime.admins.add("ops")
    assert start(runtime, "", "pool-1")["status"] == 401
    # Nobody but the account that added a runner learns it exists, admins
    # included: someone else's runner answers like a missing one.
    assert start(runtime, "alice", "pool-1")["status"] == 404
    assert start(runtime, "bob", "alice-box")["status"] == 404
    assert start(runtime, "root", "alice-box", "codex")["status"] == 404
    assert start(runtime, "ops", "pool-1")["status"] == 404
    assert start(runtime, "alice", "nope-1")["status"] == 404
    created = start(runtime, "alice", "alice-box")
    assert created["status"] == 201
    assert created["data"]["session"]["state"] == "pending"
    assert created["data"]["session"]["expiresAt"] == NOW + 10 * 60_000
    assert start(runtime, "root", "pool-1")["status"] == 201
    assert start(runtime, "root", "pool-1", "gemini")["status"] == 400
    sid = created["data"]["id"]
    base = "/api/runners/alice-box/login/" + sid
    for actor in ("bob", "root"):
        assert browser(runtime, actor, base + "/output",
                       method="GET")["status"] == 404
        assert browser(runtime, actor, base + "/cancel", {})["status"] == 404
        assert browser(runtime, actor, base + "/input",
                       {"data": "x"})["status"] == 404
        assert browser(runtime, actor, "/api/runners/alice-box/accounts", {
            "provider": "claude-code", "account": "a",
            "action": "activate"})["status"] == 404
    listed = browser(runtime, "alice", "/api/runners", method="GET")
    assert [r["name"] for r in listed["data"]["runners"]] == ["alice-box"]
    listed = browser(runtime, "root", "/api/runners", method="GET")
    assert [r["name"] for r in listed["data"]["runners"]] == ["pool-1"]
    for actor in ("bob", "ops"):
        assert browser(runtime, actor, "/api/runners",
                       method="GET")["data"]["runners"] == []
    assert browser(runtime, "root", "/api/runners")["status"] == 405
    # Revoked runners take no new sessions.
    runtime.db.execute("UPDATE runner_nodes SET status='revoked' WHERE name='pool-1'")
    runtime.db.commit()
    assert start(runtime, "root", "pool-1", "codex")["status"] == 409
    assert ("root", "runner.login.start", "pool-1:claude-code") in runtime.audits


def test_one_active_session_per_runner_and_provider():
    runtime, _pool, _own = setup()
    first = start(runtime, "root", "pool-1")
    again = start(runtime, "root", "pool-1")
    assert again["status"] == 409
    assert again["data"]["error"] == "login_active"
    assert again["data"]["id"] == first["data"]["id"]
    assert start(runtime, "root", "pool-1", "codex")["status"] == 201
    sid = first["data"]["id"]
    cancelled = browser(runtime, "root",
                        "/api/runners/pool-1/login/%s/cancel" % sid, {})
    assert cancelled["data"]["state"] == "cancelled"
    assert start(runtime, "root", "pool-1")["status"] == 201


def test_ttl_expiry_and_cleanup():
    runtime, pool_key, _own = setup()
    sid = start(runtime, "root", "pool-1")["data"]["id"]
    runtime.clock += 10 * 60_000 + 1
    out = browser(runtime, "root", "/api/runners/pool-1/login/%s/output" % sid,
                  method="GET")
    assert out["data"]["state"] == "expired"
    # Expired sessions are no longer handed to the runner, and a new one can
    # be started.
    polled = runner_post(runtime, "pool-1", pool_key, "poll", {})
    assert polled["data"]["logins"] == []
    assert polled["data"]["fastPollMs"] == 0
    assert start(runtime, "root", "pool-1")["status"] == 201
    # Sweep expires sessions nobody is watching, too.
    runtime2, key2, _ = setup()
    sid2 = start(runtime2, "root", "pool-1")["data"]["id"]
    runtime2.clock += 11 * 60_000
    runner_post(runtime2, "pool-1", key2, "poll", {})
    state = runtime2.db.execute(
        "SELECT state FROM runner_login_sessions WHERE id=?", (sid2,)).fetchone()
    assert state[0] == "expired"


def test_full_relay_round_trip_with_signed_runner_calls():
    runtime, pool_key, _own = setup()
    sid = start(runtime, "root", "pool-1")["data"]["id"]
    base = "/api/runners/pool-1/login/" + sid

    # The claim response (and /poll) hands the session over once, marks it
    # running, stores availability, and asks for fast polling.
    availability = {"claude-code": {"binaryFound": True,
                                    "loginState": "missing", "evil": "x"},
                    "codex": {"binaryFound": False, "loginState": "missing"},
                    "other": {"binaryFound": True}}
    claimed = claim(runtime, "pool-1", pool_key,
                    {"max": 1, "availability": availability})
    assert claimed["status"] == 200
    [login] = claimed["data"]["logins"]
    assert login["id"] == sid and login["provider"] == "claude-code"
    assert login["ttlMs"] == 10 * 60_000
    assert claimed["data"]["fastPollMs"] == 1000
    stored = json.loads(runtime.db.execute(
        "SELECT data FROM runner_availability WHERE runner='pool-1'"
    ).fetchone()[0])
    assert set(stored) == {"claude-code", "codex"}
    assert "evil" not in stored["claude-code"]
    listed = browser(runtime, "root", "/api/runners", method="GET")
    pool = [r for r in listed["data"]["runners"] if r["name"] == "pool-1"][0]
    assert pool["availability"]["claude-code"]["binaryFound"] is True
    assert pool["logins"][0]["state"] == "running"
    # Not handed over twice while the runner reports it as running...
    again = runner_post(runtime, "pool-1", pool_key, "poll",
                        {"activeLogins": [sid]})
    assert again["data"]["logins"] == [] and again["data"]["fastPollMs"] == 1000
    # ...but re-offered when the runner lost it (restart).
    assert [x["id"] for x in runner_post(
        runtime, "pool-1", pool_key, "poll", {})["data"]["logins"]] == [sid]

    # Output in order; a retry of a stored seq is harmless; a gap is refused.
    url_text = "Open https://claude.ai/oauth/authorize?code=true&x=1\r\n"
    r = runner_post(runtime, "pool-1", pool_key, "login/output",
                    {"id": sid, "seq": 1, "data": b64(url_text)})
    assert r["status"] == 200 and r["data"]["state"] == "running"
    assert runner_post(runtime, "pool-1", pool_key, "login/output",
                       {"id": sid, "seq": 1, "data": b64(url_text)})["status"] == 200
    assert runner_post(runtime, "pool-1", pool_key, "login/output",
                       {"id": sid, "seq": 3, "data": b64("x")})["status"] == 409
    # Token-shaped text is scrubbed by the app as well.
    runner_post(runtime, "pool-1", pool_key, "login/output",
                {"id": sid, "seq": 2,
                 "data": b64("token sk-ant-oat01-ABCDEFGHIJKLMNOP done\r\n")})  # forkmesh-secret-scan:ignore-line
    out = browser(runtime, "root", base + "/output", method="GET", after="0")
    texts = [base64.b64decode(c["data"]).decode() for c in out["data"]["chunks"]]
    assert texts[0] == url_text
    assert "sk-ant" not in texts[1] and "[redacted]" in texts[1]
    assert out["data"]["nextSeq"] == 2
    assert browser(runtime, "root", base + "/output", method="GET",
                   after="2")["data"]["chunks"] == []

    # Input goes browser -> runner and is deleted once the runner read it.
    assert browser(runtime, "root", base + "/input",
                   {"data": "code#123\r"})["data"]["seq"] == 1
    got = runner_post(runtime, "pool-1", pool_key, "login/input-poll",
                      {"id": sid, "after": 0})
    assert [base64.b64decode(c["data"]) for c in got["data"]["input"]] == [
        b"code#123\r"]
    runner_post(runtime, "pool-1", pool_key, "login/output",
                {"id": sid, "seq": 0, "inputAfter": 1})
    assert chunk_rows(runtime, sid, "in") == []

    # Final state from the runner ends the session and drops its input.
    browser(runtime, "root", base + "/input", {"data": "late\n"})
    done = runner_post(runtime, "pool-1", pool_key, "login/output",
                       {"id": sid, "seq": 3, "data": b64("Login successful\r\n"),
                        "state": "succeeded", "exitCode": 0})
    assert done["data"]["state"] == "succeeded" and done["data"]["input"] == []
    assert chunk_rows(runtime, sid, "in") == []
    final = browser(runtime, "root", base + "/output", method="GET", after="2")
    assert final["data"]["state"] == "succeeded"
    assert final["data"]["exitCode"] == 0
    assert base64.b64decode(final["data"]["chunks"][0]["data"]) == (
        b"Login successful\r\n")
    assert browser(runtime, "root", base + "/input",
                   {"data": "x"})["status"] == 409
    # Output chunks go ENDED_KEEP_MS after the end; the row a day later.
    runtime.clock += runner_logins.ENDED_KEEP_MS + 1
    runner_post(runtime, "pool-1", pool_key, "poll", {})
    assert chunk_rows(runtime, sid) == []
    runtime.clock += runner_logins.SESSION_ROW_KEEP_MS
    runner_post(runtime, "pool-1", pool_key, "poll", {})
    assert runtime.db.execute(
        "SELECT COUNT(*) FROM runner_login_sessions WHERE id=?",
        (sid,)).fetchone()[0] == 0


def test_runner_calls_are_signed_scoped_and_replay_protected():
    runtime, pool_key, own_key = setup()
    sid = start(runtime, "root", "pool-1")["data"]["id"]
    runner_post(runtime, "pool-1", pool_key, "poll", {})
    body = {"id": sid, "seq": 1, "data": b64("hi")}
    assert runner_post(runtime, "pool-1", own_key, "login/output",
                       body)["status"] == 401  # wrong key
    # Another runner cannot touch this runner's session.
    assert runner_post(runtime, "alice-box", own_key, "login/output",
                       body)["status"] == 404
    assert runner_post(runtime, "pool-1", pool_key, "login/output", body,
                       request_id="login-replay-01")["status"] == 200
    assert runner_post(runtime, "pool-1", pool_key, "login/output", body,
                       request_id="login-replay-01")["status"] == 401
    # A signature for one path does not authorize another.
    request = signed(runtime, "pool-1", pool_key, "/api/runners/poll",
                     {"id": sid, "after": 0})
    assert call(request, "/api/runners/login/input-poll")["status"] == 401
    # Oversized or malformed output is refused.
    big = base64.b64encode(b"x" * (runner_logins.MAX_OUTPUT_BYTES + 1)).decode()
    assert runner_post(runtime, "pool-1", pool_key, "login/output",
                       {"id": sid, "seq": 2, "data": big})["status"] == 400
    assert runner_post(runtime, "pool-1", pool_key, "login/output",
                       {"id": sid, "seq": 2, "data": "!!"})["status"] == 400


def test_input_bounds_and_backlog():
    runtime, _pool, _own = setup()
    sid = start(runtime, "root", "pool-1")["data"]["id"]
    path = "/api/runners/pool-1/login/%s/input" % sid
    for bad in ("", None, 5, "a\x1b[A", "\x03", "x" * 4097, "é" * 2049):
        assert browser(runtime, "root", path, {"data": bad})["status"] == 400
    assert browser(runtime, "root", path, {"data": "x" * 4096})["status"] == 200
    assert browser(runtime, "root", path, {"data": "é\tcode\r\n"})["status"] == 200
    for _ in range(runner_logins.MAX_PENDING_INPUT - 2):
        assert browser(runtime, "root", path, {"data": "a"})["status"] == 200
    assert browser(runtime, "root", path, {"data": "a"})["status"] == 429


def test_output_keeps_only_a_bounded_tail():
    runtime, pool_key, _own = setup()
    sid = start(runtime, "root", "pool-1")["data"]["id"]
    runner_post(runtime, "pool-1", pool_key, "poll", {})
    total = runner_logins.OUTPUT_KEEP_CHUNKS + 10
    for seq in range(1, total + 1):
        assert runner_post(runtime, "pool-1", pool_key, "login/output",
                           {"id": sid, "seq": seq,
                            "data": b64("line %d\n" % seq)})["status"] == 200
    assert len(chunk_rows(runtime, sid, "out")) == runner_logins.OUTPUT_KEEP_CHUNKS
    out = browser(runtime, "root", "/api/runners/pool-1/login/%s/output" % sid,
                  method="GET", after="0")
    assert out["data"]["truncated"] is True
    assert out["data"]["chunks"][0]["seq"] == 11


def test_cancel_reaches_the_runner():
    runtime, pool_key, _own = setup()
    sid = start(runtime, "root", "pool-1")["data"]["id"]
    runner_post(runtime, "pool-1", pool_key, "poll", {})
    browser(runtime, "root", "/api/runners/pool-1/login/%s/input" % sid,
            {"data": "abc"})
    browser(runtime, "root", "/api/runners/pool-1/login/%s/cancel" % sid, {})
    assert chunk_rows(runtime, sid, "in") == []
    got = runner_post(runtime, "pool-1", pool_key, "login/input-poll",
                      {"id": sid, "after": 0})
    assert got["data"]["state"] == "cancelled" and got["data"]["input"] == []
    # The runner's own final report cannot resurrect or overwrite it.
    runner_post(runtime, "pool-1", pool_key, "login/output",
                {"id": sid, "seq": 1, "data": b64("bye"), "state": "failed"})
    row = runtime.db.execute(
        "SELECT state,output_seq FROM runner_login_sessions WHERE id=?",
        (sid,)).fetchone()
    assert tuple(row) == ("cancelled", 0)


def test_list_counts_each_runners_live_job_leases():
    runtime, _pool, _own = setup()
    for job_id, runner, lease, expires in (
            (1, "pool-1", "lease-a", NOW + 60_000),
            (2, "pool-1", "lease-b", NOW + 60_000),
            (3, "pool-1", "", 0),
            (4, "pool-1", "lease-old", NOW - 1),
            (5, "alice-box", "lease-c", NOW + 60_000)):
        runtime.db.execute(
            "INSERT INTO runner_job_leases (job_id,runner,session_id,lease_id,"
            "grant_id,target_node,repo,ref,expires_at,created_at) "
            "VALUES (?,?,?,?,?,?,?,?,?,?)",
            (job_id, runner, "s%d" % job_id, lease, "g", "core-a", "proj",
             "refs/heads/agent/s%d" % job_id, expires, NOW))
    runtime.db.commit()
    leases = {}
    for actor in ("root", "alice"):
        listed = browser(runtime, actor, "/api/runners", method="GET")
        leases.update(
            {r["name"]: r["activeLeases"] for r in listed["data"]["runners"]})
    assert leases == {"alice-box": 1, "pool-1": 2}


ACCOUNT_A = "a" * 32
ACCOUNT_B = "b" * 32


def report_accounts(runtime, key, runner="pool-1", **extra):
    availability = {"claude-code": {
        "binaryFound": True, "loginState": "available", "source": "device login",
        "accounts": [
            {"id": ACCOUNT_A, "label": "one@example.com\x1b[31m",
             "email": "one@example.com", "plan": "max", "active": True,
             "addedAt": 5, "subject": "secret", "usage": {
                 "fetchedAt": NOW, "plan": "", "error": "", "windows": [
                     {"id": "five_hour", "label": "5-hour session",
                      "usedPercent": 42.5, "resetsAt": NOW + 3600_000,
                      "windowMinutes": 300},
                     {"id": "Bad Id", "usedPercent": 1},
                     {"id": "seven_day", "usedPercent": "high"}]}},
            {"id": ACCOUNT_B, "label": "two@example.com", "active": 1},
            {"id": "not-an-id", "label": "x"}]}}
    return runner_post(runtime, runner, key, "poll",
                       {"availability": availability, **extra})


def test_accounts_and_usage_are_reported_and_sanitised():
    runtime, pool_key, _own = setup()
    assert report_accounts(runtime, pool_key)["status"] == 200
    listed = browser(runtime, "root", "/api/runners", method="GET")
    pool = [r for r in listed["data"]["runners"] if r["name"] == "pool-1"][0]
    accounts = pool["availability"]["claude-code"]["accounts"]
    assert [a["id"] for a in accounts] == [ACCOUNT_A, ACCOUNT_B]
    first, second = accounts
    assert first["label"] == "one@example.com[31m" and first["active"] is True
    assert "subject" not in first and second["active"] is False
    assert first["usage"]["windows"] == [
        {"id": "five_hour", "label": "5-hour session", "usedPercent": 42.5,
         "resetsAt": NOW + 3600_000, "windowMinutes": 300}]
    assert "usage" not in second
    assert pool["accountRequests"] == []


def test_switching_and_removing_accounts_round_trip():
    runtime, pool_key, own_key = setup()
    path = "/api/runners/pool-1/accounts"
    body = {"provider": "claude-code", "account": ACCOUNT_B, "action": "activate"}
    assert browser(runtime, "root", path, body)["status"] == 404
    report_accounts(runtime, pool_key)
    assert browser(runtime, "alice", path, body)["status"] == 404
    assert browser(runtime, "", path, body)["status"] == 401
    assert browser(runtime, "root", path, dict(body, action="drop"))["status"] == 400
    assert browser(runtime, "root", path, dict(body, provider="gemini"))["status"] == 400
    assert browser(runtime, "root", path, dict(body, account="x"))["status"] == 400
    assert browser(runtime, "root", path, dict(body, account="c" * 32))["status"] == 404
    assert browser(runtime, "root", path, method="GET")["status"] == 405
    created = browser(runtime, "root", path, body)
    assert created["status"] == 202
    request_id = created["data"]["id"]
    assert created["data"]["request"]["state"] == "pending"
    assert runtime.audits[-1][1:] == (
        "runner.account.activate", "pool-1:claude-code:" + ACCOUNT_B)

    handed = report_accounts(runtime, pool_key)["data"]
    assert handed["accountRequests"] == [{
        "id": request_id, "provider": "claude-code", "action": "activate",
        "account": ACCOUNT_B}]
    assert handed["fastPollMs"] == 1000
    assert report_accounts(runtime, pool_key)["data"]["accountRequests"]

    report_accounts(runtime, own_key, runner="alice-box",
                    accountResults=[{"id": request_id, "ok": True}])
    done = report_accounts(runtime, pool_key, accountResults=[
        {"id": request_id, "ok": True}, {"id": "bogus"}, "x"])["data"]
    assert done["accountRequests"] == [] and done["fastPollMs"] == 0
    listed = browser(runtime, "root", "/api/runners", method="GET")
    pool = [r for r in listed["data"]["runners"] if r["name"] == "pool-1"][0]
    assert [(r["id"], r["state"]) for r in pool["accountRequests"]] == [
        (request_id, "done")]

    removed = browser(runtime, "root", path, dict(body, action="remove"))
    report_accounts(runtime, pool_key, accountResults=[{
        "id": removed["data"]["id"], "ok": False,
        "message": "busy sk-ant-abcdefghijkl"}])
    row = runtime.db.execute(
        "SELECT state,message FROM runner_account_requests WHERE id=?",
        (removed["data"]["id"],)).fetchone()
    assert row[0] == "failed" and "sk-ant" not in row[1]

    report_accounts(runtime, own_key, runner="alice-box")
    assert browser(runtime, "alice", "/api/runners/alice-box/accounts",
                   body)["status"] == 202


def test_account_requests_are_bounded_and_expire():
    runtime, pool_key, _own = setup()
    report_accounts(runtime, pool_key)
    path = "/api/runners/pool-1/accounts"
    body = {"provider": "claude-code", "account": ACCOUNT_A, "action": "activate"}
    for _ in range(runner_logins.MAX_PENDING_ACCOUNT_REQUESTS):
        assert browser(runtime, "root", path, body)["status"] == 202
    assert browser(runtime, "root", path, body)["status"] == 429
    runtime.clock += runner_logins.ACCOUNT_REQUEST_TTL_MS + 1
    assert report_accounts(runtime, pool_key)["data"]["accountRequests"] == []
    states = {r[0] for r in runtime.db.execute(
        "SELECT state FROM runner_account_requests")}
    assert states == {"failed"}
    runtime.clock += runner_logins.SESSION_ROW_KEEP_MS + 1
    report_accounts(runtime, pool_key)
    assert runtime.db.execute(
        "SELECT COUNT(*) FROM runner_account_requests").fetchone()[0] == 0
    assert runner_logins.match("r1/accounts") == ("accounts", "r1")


def terminal(runtime, actor, runner, provider="claude-code", **size):
    return browser(runtime, actor, "/api/runners/%s/login" % runner,
                   {"provider": provider, "kind": "terminal", **size})


def test_terminal_sessions_need_the_runner_feature():
    runtime, pool_key, _own = setup()
    availability = {"claude-code": {"binaryFound": True,
                                    "loginState": "available"}}
    runner_post(runtime, "pool-1", pool_key, "poll",
                {"availability": availability})
    refused = terminal(runtime, "root", "pool-1")
    assert refused["status"] == 409
    assert refused["data"]["error"] == "terminal_unsupported"
    assert browser(runtime, "root", "/api/runners/pool-1/login",
                   {"provider": "claude-code", "kind": "shell"}
                   )["status"] == 400
    runner_post(runtime, "pool-1", pool_key, "poll",
                {"availability": availability,
                 "features": ["terminal", "bogus"]})
    listed = browser(runtime, "root", "/api/runners", method="GET")
    pool = [r for r in listed["data"]["runners"] if r["name"] == "pool-1"][0]
    assert pool["features"] == ["terminal"]
    assert terminal(runtime, "alice", "pool-1")["status"] == 404
    created = terminal(runtime, "root", "pool-1", cols=500, rows=2)
    assert created["status"] == 201
    session = created["data"]["session"]
    assert session["kind"] == "terminal"
    assert (session["cols"], session["rows"]) == (300, 5)
    assert session["expiresAt"] == NOW + 60 * 60_000
    assert ("root", "runner.terminal.start", "pool-1:claude-code") in (
        runtime.audits)
    # A downgraded runner stops advertising the feature.
    runner_post(runtime, "pool-1", pool_key, "poll",
                {"availability": availability})
    assert terminal(runtime, "root", "pool-1", "codex")["status"] == 409


def test_terminal_relays_control_keys_and_size_beside_a_sign_in():
    runtime, pool_key, _own = setup()
    features = {"availability": {"claude-code": {"binaryFound": True}},
                "features": ["terminal"]}
    runner_post(runtime, "pool-1", pool_key, "poll", features)
    tid = terminal(runtime, "root", "pool-1")["data"]["id"]
    again = terminal(runtime, "root", "pool-1")
    assert again["status"] == 409
    assert again["data"]["error"] == "terminal_active"
    assert again["data"]["id"] == tid
    lid = start(runtime, "root", "pool-1")["data"]["id"]
    listed = browser(runtime, "root", "/api/runners", method="GET")
    pool = [r for r in listed["data"]["runners"] if r["name"] == "pool-1"][0]
    assert [s["id"] for s in pool["logins"]] == [lid]
    assert [s["id"] for s in pool["terminals"]] == [tid]
    assert pool["terminals"][0]["cols"] == 100

    # A runner that does not report the feature gets only the sign-in.
    old = runner_post(runtime, "pool-1", pool_key, "poll", {})
    assert [x["id"] for x in old["data"]["logins"]] == [lid]
    assert old["data"]["logins"][0]["kind"] == "login"
    polled = runner_post(runtime, "pool-1", pool_key, "poll",
                         {**features, "activeLogins": [lid]})
    [handed] = polled["data"]["logins"]
    assert handed["id"] == tid and handed["kind"] == "terminal"
    assert (handed["cols"], handed["rows"]) == (100, 40)
    assert handed["ttlMs"] == 60 * 60_000

    base = "/api/runners/pool-1/login/" + tid
    assert browser(runtime, "root", base + "/input",
                   {"data": "\x1b[A\x03"})["status"] == 200
    assert browser(runtime, "root", base + "/input",
                   {"data": "a\x00b"})["status"] == 400
    assert browser(runtime, "root", "/api/runners/pool-1/login/%s/input" % lid,
                   {"data": "\x1b[A"})["status"] == 400
    resized = browser(runtime, "root", base + "/resize",
                      {"cols": 80, "rows": 24})
    assert resized["status"] == 200
    assert (resized["data"]["cols"], resized["data"]["rows"]) == (80, 24)
    assert browser(runtime, "root",
                   "/api/runners/pool-1/login/%s/resize" % lid,
                   {"cols": 80, "rows": 24})["status"] == 400
    reply = runner_post(runtime, "pool-1", pool_key, "login/output",
                        {"id": tid, "seq": 1, "data": b64("\x1b[2J> ")})
    assert (reply["data"]["cols"], reply["data"]["rows"]) == (80, 24)
    assert [base64.b64decode(c["data"]) for c in reply["data"]["input"]] == [
        b"\x1b[A\x03"]

    # A terminal that exits cleanly is not a sign-in.
    runner_post(runtime, "pool-1", pool_key, "login/output",
                {"id": tid, "seq": 0, "state": "succeeded", "exitCode": 0})
    assert not asyncio.run(runner_logins._relogged(
        runtime, "pool-1", "claude-code", 0))
    cancelled = browser(runtime, "root",
                        "/api/runners/pool-1/login/%s/cancel" % lid, {})
    assert cancelled["data"]["state"] == "cancelled"


def test_terminal_ends_at_its_own_limit():
    runtime, pool_key, _own = setup()
    runner_post(runtime, "pool-1", pool_key, "poll",
                {"availability": {"codex": {"binaryFound": True}},
                 "features": ["terminal"]})
    tid = terminal(runtime, "root", "pool-1", "codex")["data"]["id"]
    base = "/api/runners/pool-1/login/" + tid
    runtime.clock += 11 * 60_000
    assert browser(runtime, "root", base + "/output",
                   method="GET")["data"]["state"] == "pending"
    runtime.clock += 50 * 60_000
    out = browser(runtime, "root", base + "/output", method="GET")
    assert out["data"]["state"] == "expired"
    assert out["data"]["message"] == runner_logins.TERMINAL_EXPIRED
