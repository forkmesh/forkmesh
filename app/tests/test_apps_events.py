import asyncio
import json
import time

from apps_fakes import Runtime, World, call, create_app, install
import apps_core as core  # noqa: E402
import apps_events as events  # noqa: E402

EVENT_URL = "https://hello.example.org/events"


def _world():
    world = World()
    world.repos[("alice", "demo")] = True
    return world


def _answer(world):
    def handler(url, headers, body):
        payload = json.loads(body)
        if payload.get("type") == "url_verification":
            return 200, json.dumps({"challenge": payload["challenge"]})
        return 200, ""
    world.http_handler = handler


def _app(world, **overrides):
    body = dict(events=["issue.opened", "issue.comment", "app.mention",
                        "ping.created", "chat.message"],
                eventUrl=EVENT_URL,
                scopes=["issues:read", "issues:write", "pings:read",
                        "chat:read", "chat:write"],
                bot={"enabled": True})
    body.update(overrides)
    created = create_app(world, **body)
    _answer(world)
    verified = call(world, "POST", "/api/apps/%s/verify" % created["app"]["id"])
    assert verified["status"] == 200, verified["data"]
    assert verified["data"]["app"]["eventUrlVerified"] is True
    return created


def _emit(world, event_type, data, owner="alice", repo="demo", actor=""):
    return asyncio.run(events.emit_repo_event(
        Runtime(world), owner, repo, event_type, data, actor))


def _deliveries(world, created):
    return call(world, "GET", "/api/apps/%s/deliveries"
                % created["app"]["id"])["data"]["deliveries"]


def test_url_verification_needs_the_echoed_challenge():
    world = _world()
    created = create_app(world, events=["issue.opened"], eventUrl=EVENT_URL,
                         scopes=["issues:read"])
    world.http_handler = lambda url, headers, body: (200, "{}")
    failed = call(world, "POST", "/api/apps/%s/verify" % created["app"]["id"])
    assert failed["data"]["error"] == "verification_failed"
    install(world, created, scope="issues:read")
    assert _emit(world, "issue.opened", {"number": 1}) == 0
    assert _deliveries(world, created)[0]["detail"] == "challenge_mismatch"


def test_signed_delivery_of_subscribed_events_only():
    world = _world()
    created = _app(world)
    install(world, created)
    world.http = []
    assert _emit(world, "issue.opened", {"number": 7, "title": "Bug",
                                         "author": "bob"}) == 1
    [request] = world.http
    assert request["url"] == EVENT_URL
    headers = request["headers"]
    body = request["body"].encode()
    assert core.verify_signature(
        created["signingSecret"], headers["ForkMesh-Webhook-Id"],
        headers["ForkMesh-Webhook-Timestamp"], body,
        headers["ForkMesh-Webhook-Signature"], world.now // 1000)
    assert not core.verify_signature(
        created["signingSecret"], headers["ForkMesh-Webhook-Id"],
        headers["ForkMesh-Webhook-Timestamp"], body + b" ",
        headers["ForkMesh-Webhook-Signature"], world.now // 1000)
    assert not core.verify_signature(
        created["signingSecret"], headers["ForkMesh-Webhook-Id"],
        headers["ForkMesh-Webhook-Timestamp"], body,
        headers["ForkMesh-Webhook-Signature"], world.now // 1000 + 3600)
    payload = json.loads(body)
    assert payload["event"] == {"type": "issue.opened", "number": 7,
                                "title": "Bug", "author": "bob",
                                "repository": "alice/demo"}
    assert payload["target"] == {"kind": "user", "name": "alice"}
    assert headers["ForkMesh-Event-Type"] == "issue.opened"
    assert _emit(world, "pull.opened", {"number": 1}) == 0
    assert _emit(world, "issue.opened", {"number": 1}, owner="bob",
                 repo="other") == 0
    assert _emit(world, "issue.opened", {"number": 2},
                 actor="app:hello-app") == 0


def test_scope_is_required_even_when_subscribed():
    world = _world()
    created = _app(world)
    install(world, created, scope="issues:write")
    world.http = []
    assert _emit(world, "issue.opened", {"number": 1}) == 0
    assert world.http == []


def test_mentions_become_app_mention_events():
    world = _world()
    created = _app(world)
    install(world, created)
    world.http = []
    assert _emit(world, "issue.comment", {"number": 3,
                                          "body": "ping @hello-app please"}) == 2
    types = [json.loads(r["body"])["event"]["type"] for r in world.http]
    assert types == ["issue.comment", "app.mention"]
    assert not events.mentions("email me@hello-app.com", "hello-app")


def test_retry_backoff_then_disable_after_repeated_failures():
    world = _world()
    created = _app(world)
    install(world, created)
    world.http_handler = lambda url, headers, body: (503, "")
    _emit(world, "issue.opened", {"number": 1})
    row = world.db.execute("SELECT attempts, next_ts FROM app_event_outbox"
                           ).fetchone()
    assert row[0] == 1 and row[1] == world.now + core.backoff_ms(1)
    world.http_handler = lambda url, headers, body: (410, "")
    world.now = row[1]
    assert asyncio.run(events.drain(Runtime(world), 10)) == 0
    assert world.db.execute("SELECT COUNT(*) FROM app_event_outbox"
                            ).fetchone()[0] == 0
    for number in range(core.DISABLE_AFTER_FAILURES):
        _emit(world, "issue.opened", {"number": number})
    app = call(world, "GET", "/api/apps/" + created["app"]["id"])["data"]["app"]
    assert app["eventsDisabled"] is True
    assert any("switched off" in n[1] for n in world.notices)
    world.http = []
    assert _emit(world, "issue.opened", {"number": 99}) == 0
    _answer(world)
    call(world, "POST", "/api/apps/%s/verify" % created["app"]["id"])
    assert _emit(world, "issue.opened", {"number": 100}) == 1
    log = _deliveries(world, created)
    assert log[0]["outcome"] == "delivered"
    assert {"retry", "dropped"} & {entry["outcome"] for entry in log}


def test_redirects_are_dropped_not_followed():
    world = _world()
    created = _app(world)
    install(world, created)
    world.http_handler = lambda url, headers, body: (302, "")
    _emit(world, "issue.opened", {"number": 1})
    assert _deliveries(world, created)[0]["outcome"] == "dropped"


def test_ssrf_destinations_are_refused_at_registration():
    world = _world()
    for url in ("https://127.0.0.1/events", "https://10.0.0.5/events",
                "https://169.254.169.254/latest", "https://[::1]/x",
                "https://[fd00::1]/x", "http://hello.example.org/events",
                "https://localhost/events", "https://app.forkmesh.com/x",
                "https://internal.local/x", "https://user@hello.example.org/"):
        result = call(world, "POST", "/api/apps", {
            "name": "Probe", "redirectUris": ["https://probe.example.org/cb"],
            "scopes": ["issues:read"], "events": ["issue.opened"],
            "eventUrl": url})
        assert result["data"].get("error") == "invalid_event_url", url
    commands = call(world, "POST", "/api/apps", {
        "name": "Probe", "redirectUris": ["https://probe.example.org/cb"],
        "scopes": ["commands"], "commands": [
            {"command": "probe", "url": "https://192.168.1.1/c"}]})
    assert commands["data"]["error"] == "invalid_commands"


def test_pings_and_uninstall_events():
    world = _world()
    created = _app(world)
    install(world, created)
    world.http = []
    assert asyncio.run(events.emit_ping(Runtime(world), "alice",
                                        "New review", "/x")) == 1
    assert asyncio.run(events.emit_ping(Runtime(world), "bob", "x", "")) == 0
    install_id = call(world, "GET", "/api/apps/installed")["data"][
        "installs"][0]["installId"]
    call(world, "DELETE", "/api/apps/installs/" + install_id)
    last = json.loads(world.http[-1]["body"])
    assert last["event"]["type"] == "app.uninstalled"
    assert last["installId"] == install_id
    world.http = []
    assert _emit(world, "issue.opened", {"number": 1}) == 0


def test_chat_events_from_app_posts_reach_other_apps_only():
    world = _world()
    world.channels["c" * 32] = {"public": True, "name": "general",
                                "key_version": 1}
    listener = _app(world)
    install(world, listener)
    poster = create_app(world, name="Poster",
                        redirectUris=["https://poster.example.org/cb"],
                        scopes=["chat:write"], bot={"enabled": True})
    token = install(world, poster)["access_token"]
    world.http = []
    posted = call(world, "POST", "/api/apps/v1/chat.postMessage",
                  {"channel": "c" * 32, "text": "hi @hello-app"}, user="",
                  headers={"authorization": "Bearer " + token})
    assert posted["status"] == 200
    types = [json.loads(r["body"])["event"]["type"] for r in world.http]
    assert types == ["chat.message", "app.mention"]
    first = json.loads(world.http[0]["body"])["event"]
    assert first["sender"] == {"id": "app:poster", "name": "Poster (app)"}


def test_delivery_log_is_private_to_the_developer():
    world = _world()
    created = _app(world)
    assert call(world, "GET", "/api/apps/%s/deliveries" % created["app"]["id"],
                user="mallory")["status"] == 404
    assert _deliveries(world, created)[0]["kind"] == "verify"


def test_receiver_rejects_replayed_old_timestamps():
    body = b'{"a":1}'
    now = int(time.time())
    signature = core.sign("fmss_x", "id1", now - 301, body)
    assert not core.verify_signature("fmss_x", "id1", now - 301, body,
                                     signature, now)
    signature = core.sign("fmss_x", "id1", now, body)
    assert core.verify_signature("fmss_x", "id1", now, body, signature, now)
