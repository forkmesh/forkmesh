import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import runner_leases  # noqa: E402
import runner_setup  # noqa: E402
from test_runner_leases import NOW, Key  # noqa: E402
from test_runner_logins import FakeRuntime, browser  # noqa: E402


def make():
    runtime = FakeRuntime()
    runtime.audits = []
    return runtime


def mint(runtime, actor="alice", name="alice-box", **extra):
    return browser(runtime, actor, "/api/runners/setup-tokens",
                   {"name": name, **extra})


def register(runtime, token, name="alice-box", key=None, ts=None,
             proof_token=None):
    key = key or Key()
    ts = NOW if ts is None else ts
    proof = key.sign(runner_setup.register_message(
        name, key.public, runner_setup.token_hash(proof_token or token), ts))
    return browser(runtime, "", "/api/runners/register-token", {
        "token": token, "name": name, "publicKey": key.public,
        "proof": proof, "ts": ts}), key


def test_routes():
    assert runner_setup.match("setup-tokens") == ("mint",)
    assert runner_setup.match("register-token") == ("register",)
    assert runner_setup.match("setup-tokens/abc") == ("status", "abc")
    assert runner_setup.match("setup-tokens/abc/revoke") == ("revoke", "abc")
    assert runner_setup.match("claim") is None
    assert runner_setup.match("r1/login") is None
    assert runner_setup.register_message("n", "k", "d", 5) == (
        "forkmesh-runner-register-token-v1\nn\nk\nd\n5")
    assert runner_leases.register_message("n", "k", "d", 5) != (
        runner_setup.register_message("n", "k", "d", 5))


def test_mint_returns_a_one_time_command_and_stores_only_a_hash():
    runtime = make()
    assert mint(runtime, actor="")["status"] == 401
    answer = mint(runtime)
    assert answer["status"] == 201
    body = answer["data"]
    token = body["token"]
    assert runner_setup.TOKEN_RE.fullmatch(token)
    assert body["expiresAt"] == NOW + 30 * 60 * 1000
    assert body["command"] == (
        "curl -fsSL https://forkmesh.com/install-runner.sh | sudo bash -s -- "
        "--token %s --name alice-box --app https://app.forkmesh.com" % token)
    rows = runtime.db.execute(
        "SELECT * FROM runner_setup_tokens").fetchall()
    assert len(rows) == 1
    row = dict(rows[0])
    assert token not in json.dumps(row)
    assert row["token_hash"] == runner_setup.token_hash(token)
    assert row["owner"] == "alice" and row["runner"] == "alice-box"
    assert ("alice", "runner.setup_token.create", "alice-box") in runtime.audits
    assert all(token not in json.dumps(entry) for entry in runtime.audits)
    status = browser(runtime, "alice", "/api/runners/setup-tokens/" + body["id"],
                     method="GET")
    assert status["data"]["setup"]["state"] == "pending"
    assert token not in json.dumps(status)
    assert browser(runtime, "bob", "/api/runners/setup-tokens/" + body["id"],
                   method="GET")["status"] == 404


def test_names_are_checked_before_a_token_is_made():
    runtime = make()
    assert mint(runtime, name="Bad Name")["status"] == 400
    assert mint(runtime, name="bob")["status"] == 409
    assert mint(runtime, name="core-a")["status"] == 409
    runtime.add_runner("pool-9", Key(), scope="pool", owners=["alice"],
                       registered_by="root")
    assert mint(runtime, name="pool-9")["status"] == 409
    runtime.add_runner("alice-old", Key(), scope="personal", owners=["alice"],
                       registered_by="alice")
    assert mint(runtime, name="alice-old")["status"] == 201
    assert mint(runtime, actor="bob", name="alice-old")["status"] == 409


def test_token_registers_one_personal_runner_once():
    runtime = make()
    token = mint(runtime)["data"]["token"]
    answer, key = register(runtime, token)
    assert answer["status"] == 201
    assert answer["data"]["runner"] == {
        "name": "alice-box", "scope": "personal", "allowedOwners": ["alice"],
        "registeredBy": "alice", "publicKey": key.public}
    row = dict(runtime.db.execute(
        "SELECT * FROM runner_nodes WHERE name='alice-box'").fetchone())
    assert row["registered_by"] == "alice" and row["scope"] == "personal"
    assert json.loads(row["allowed_owners"]) == ["alice"]
    assert row["status"] == "active"
    again, _ = register(runtime, token)
    assert again["status"] == 401
    assert again["data"]["error"] == "invalid_token"
    token_row = dict(runtime.db.execute(
        "SELECT state,used_at FROM runner_setup_tokens").fetchone())
    assert token_row == {"state": "used", "used_at": NOW}
    listed = browser(runtime, "alice", "/api/runners", method="GET")
    assert [r["name"] for r in listed["data"]["runners"]] == ["alice-box"]


def test_token_expires_after_thirty_minutes():
    runtime = make()
    token = mint(runtime)["data"]["token"]
    runtime.clock += 30 * 60 * 1000 + 1
    answer, _ = register(runtime, token, ts=runtime.clock)
    assert answer["status"] == 401 and answer["data"]["error"] == "invalid_token"
    assert runtime.db.execute(
        "SELECT COUNT(*) FROM runner_nodes WHERE name='alice-box'").fetchone()[0] == 0


def test_token_is_bound_to_its_name_owner_and_a_valid_proof():
    runtime = make()
    token = mint(runtime)["data"]["token"]
    wrong_name, _ = register(runtime, token, name="other-box")
    assert wrong_name["status"] == 401
    forged, _ = register(runtime, token, proof_token="fmrt_" + "1" * 64)
    assert forged["status"] == 401 and forged["data"]["error"] == "invalid_proof"
    stale, _ = register(runtime, token, ts=NOW - 10 * 60 * 1000)
    assert stale["status"] == 400
    bogus, _ = register(runtime, "fmrt_" + "0" * 64)
    assert bogus["status"] == 401
    malformed, _ = register(runtime, "nope")
    assert malformed["status"] == 400
    ok, _ = register(runtime, token)
    assert ok["status"] == 201


def test_revoked_token_cannot_register_and_limits_apply():
    runtime = make()
    minted = mint(runtime)["data"]
    revoked = browser(runtime, "alice", "/api/runners/setup-tokens/%s/revoke"
                      % minted["id"], {})
    assert revoked["data"]["setup"]["state"] == "revoked"
    answer, _ = register(runtime, minted["token"])
    assert answer["status"] == 401
    for index in range(runner_setup.MAX_OPEN_TOKENS):
        assert mint(runtime, name="alice-box-%d" % index)["status"] == 201
    assert mint(runtime, name="alice-box-x")["status"] == 429


def test_status_reports_used_runner_and_expiry():
    runtime = make()
    minted = mint(runtime)["data"]
    register(runtime, minted["token"])
    status = browser(runtime, "alice", "/api/runners/setup-tokens/" +
                     minted["id"], method="GET")["data"]["setup"]
    assert status["state"] == "used" and status["runnerStatus"] == "active"
    other = mint(runtime, name="alice-two")["data"]
    runtime.clock += 31 * 60 * 1000
    status = browser(runtime, "alice", "/api/runners/setup-tokens/" +
                     other["id"], method="GET")["data"]["setup"]
    assert status["state"] == "expired"
