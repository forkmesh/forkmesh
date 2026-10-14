import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import runner_logins  # noqa: E402
from test_runner_leases import NOW  # noqa: E402
from test_runner_logins import browser, setup, start  # noqa: E402


def set_availability(runtime, runner, data, updated_at=NOW):
    runtime.db.execute(
        "INSERT OR REPLACE INTO runner_availability (runner,data,updated_at) "
        "VALUES (?,?,?)", (runner, json.dumps(data), updated_at))
    runtime.db.commit()


EXPIRED_CLAUDE = {
    "claude-code": {"binaryFound": True, "loginState": "expired",
                    "auth": {"state": "expired", "expiresAt": 5}},
    "codex": {"binaryFound": False, "loginState": "missing",
              "auth": {"state": "missing"}},
}


def attention(runtime, actor, method="GET"):
    return browser(runtime, actor, "/api/runners/attention", method=method)


def test_attention_lists_manageable_runners_that_need_sign_in():
    runtime, _pool, _own = setup()
    runtime.db.execute("UPDATE runner_nodes SET last_seen_at=?", (NOW - 1000,))
    runtime.db.commit()
    set_availability(runtime, "pool-1", EXPIRED_CLAUDE)
    set_availability(runtime, "alice-box", {
        "claude-code": {"binaryFound": True, "loginState": "available",
                        "auth": {"state": "ok"}},
        "codex": {"binaryFound": True, "loginState": "available",
                  "auth": {"state": "expiring", "expiresAt": NOW + 3600}},
    })
    assert attention(runtime, "")["status"] == 401
    assert attention(runtime, "root", "POST")["status"] == 405
    admin = attention(runtime, "root")
    assert admin["status"] == 200
    body = admin["data"]
    assert body["count"] == 1 and body["expired"] == 1
    assert body["runners"] == 1
    assert body["pollMs"] == 5 * 60 * 1000
    assert body["checkedAt"] == NOW
    [first] = body["items"]
    assert first == {
        "runner": "pool-1", "provider": "claude-code",
        "providerLabel": "Claude", "state": "expired", "scope": "pool",
        "canConnect": True, "online": True, "lastSeenAt": NOW - 1000,
        "expiresAt": 5, "title": "pool-1 needs Claude sign-in",
        "connectUrl": "/dashboard/runners?runner=pool-1&provider=claude-code"
                      "&connect=1",
        "loginId": "",
    }
    own = attention(runtime, "alice")
    [second] = own["data"]["items"]
    assert (second["runner"], second["provider"], second["state"]) == (
        "alice-box", "codex", "expiring")
    assert second["title"] == "alice-box: Codex sign-in expires soon"
    none = attention(runtime, "bob")
    assert none["data"]["items"] == [] and none["data"]["runners"] == 0


def test_attention_links_an_open_sign_in_and_skips_revoked_runners():
    runtime, _pool, _own = setup()
    set_availability(runtime, "pool-1", EXPIRED_CLAUDE)
    session = start(runtime, "root", "pool-1")["data"]["id"]
    item = attention(runtime, "root")["data"]["items"][0]
    assert item["loginId"] == session and item["online"] is False
    runtime.db.execute("UPDATE runner_nodes SET status='revoked'")
    runtime.db.commit()
    assert attention(runtime, "root")["data"]["items"] == []


def test_attention_ignores_missing_binaries_and_signed_in_providers():
    runtime, _pool, _own = setup()
    set_availability(runtime, "pool-1", {
        "claude-code": {"binaryFound": False, "loginState": "expired",
                        "auth": {"state": "expired"}},
        "codex": {"binaryFound": True, "loginState": "available"},
    })
    set_availability(runtime, "alice-box", "not an object")
    assert attention(runtime, "root")["data"]["count"] == 0


def test_attention_is_private_cacheable_when_the_runtime_supports_it():
    runtime, _pool, _own = setup()
    seen = []

    def cached_response(data, max_age):
        seen.append(max_age)
        return {"status": 200, "data": data}

    runtime.cached_response = cached_response
    assert attention(runtime, "root")["status"] == 200
    assert seen == [60]
    assert runner_logins.match("attention") == ("attention",)
