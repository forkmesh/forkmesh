import base64
import hashlib
import hmac

import org_office_routes as routes

routes._bind_runtime({"_require_data_secret": lambda env: "unit-test-key"})
ORG = "a" * 64
ACCOUNT = "b" * 64
SESSION = "s" * 40


class Env:
    OFFICE_STUN_URLS = "stun:stun.example.test:3478, javascript:alert(1)"
    OFFICE_TURN_URLS = "turn:turn.example.test:3478?transport=udp"
    OFFICE_TURN_SECRET = "0123456789abcdef0123"


def test_tickets_round_trip_and_reject_tampering_and_expiry():
    now = 1_800_000_000_000
    ticket, expires = routes.mint_ticket(Env, ORG, ACCOUNT, SESSION, now,
                                         "nonce1234")
    assert expires == now + routes.TICKET_TTL_MS
    claims = routes.read_ticket(Env, ticket, now + 1000)
    assert claims == {"org_bi": ORG, "account_bi": ACCOUNT,
                      "session_id": SESSION, "nonce": "nonce1234",
                      "expires": expires}
    assert routes.read_ticket(Env, ticket, expires) is None
    other = ticket.replace(ORG, "c" * 64)
    assert routes.read_ticket(Env, other, now) is None
    assert routes.read_ticket(Env, ticket[:-1] + "0", now) is None
    assert routes.read_ticket(Env, "v1." + "x" * 600, now) is None
    long_lived = ".".join(["v1", ORG, ACCOUNT, SESSION,
                           str(now + 10 * routes.TICKET_TTL_MS), "nonce1234"])
    signature = hmac.new(b"unit-test-key" + routes.TICKET_PURPOSE.encode(),
                         long_lived.encode(), hashlib.sha256).hexdigest()
    assert routes.read_ticket(Env, long_lived + "." + signature, now) is None


def test_ice_servers_mint_short_lived_turn_credentials():
    servers = routes.ice_servers(Env, ACCOUNT, 1_800_000_000)
    assert servers[0] == {"urls": ["stun:stun.example.test:3478"]}
    turn = servers[1]
    expiry, alias = turn["username"].split(":")
    assert int(expiry) == 1_800_000_000 + routes.TURN_TTL_S
    assert ACCOUNT not in turn["username"]
    expected = base64.b64encode(hmac.new(
        Env.OFFICE_TURN_SECRET.encode(), turn["username"].encode(),
        hashlib.sha1).digest()).decode()
    assert turn["credential"] == expected

    class Weak:
        OFFICE_TURN_URLS = "turn:turn.example.test"
        OFFICE_TURN_SECRET = "short"

    assert routes.ice_servers(Weak, ACCOUNT, 0) == []


def test_turn_credentials_expire_ten_minutes_after_minting_and_stay_off_without_a_secret():
    for now in (1_700_000_000, 1_800_000_123):
        turn = routes.ice_servers(Env, ACCOUNT, now)[1]
        assert turn["urls"] == ["turn:turn.example.test:3478?transport=udp"]
        assert int(turn["username"].split(":")[0]) - now == 600
    other = routes.ice_servers(Env, "c" * 64, 1_800_000_000)[1]
    assert other["username"] != routes.ice_servers(
        Env, ACCOUNT, 1_800_000_000)[1]["username"]

    class StunOnly:
        OFFICE_STUN_URLS = "stun:stun.example.test:3478"
        OFFICE_TURN_URLS = "turn:turn.example.test:3478"

    assert routes.ice_servers(StunOnly, ACCOUNT, 0) == [
        {"urls": ["stun:stun.example.test:3478"]}]

    class WrongScheme:
        OFFICE_TURN_URLS = "stun:turn.example.test, https://evil.example"
        OFFICE_TURN_SECRET = "0123456789abcdef0123"

    assert routes.ice_servers(WrongScheme, ACCOUNT, 0) == []
