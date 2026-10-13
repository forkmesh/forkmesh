import json
from urllib.parse import urlparse

from apps_fakes import World, call, create_app, install
import apps_core as core  # noqa: E402

CHANNEL = "c" * 32
PRIVATE = "d" * 32
COMMAND_URL = "https://hello.example.org/commands"
INTERACTIVE_URL = "https://hello.example.org/interactive"
BLOCKS = [{"type": "section", "text": "Hello"},
          {"type": "actions", "elements": [
              {"type": "button", "action_id": "wave", "text": "Wave",
               "value": "wave"},
              {"type": "static_select", "action_id": "mood",
               "options": [{"text": "Great", "value": "great"}]}]}]


def _world(reply=None, interaction=None):
    world = World()
    world.channels[CHANNEL] = {"public": True, "name": "general",
                               "key_version": 2}
    world.channels[PRIVATE] = {"public": False, "name": "secret",
                               "key_version": 1}

    def handler(url, headers, body):
        if url == COMMAND_URL:
            return 200, json.dumps(reply or {"text": "hi"})
        if url == INTERACTIVE_URL:
            return 200, json.dumps(interaction or {"text": "ok"})
        return 200, ""
    world.http_handler = handler
    return world


def _app(world, **overrides):
    body = dict(scopes=["commands", "chat:write", "chat:read"],
                commands=[{"command": "hello", "url": COMMAND_URL,
                           "description": "Say hello"}],
                interactivityUrl=INTERACTIVE_URL, bot={"enabled": True})
    body.update(overrides)
    created = create_app(world, **body)
    install(world, created)
    return created


def _run(world, text, user="alice", channel=CHANNEL):
    return call(world, "POST", "/api/apps/commands/run",
                {"text": text, "channelId": channel}, user=user)


def test_commands_are_listed_only_for_installers():
    world = _world()
    _app(world)
    listed = call(world, "GET", "/api/apps/commands")["data"]["commands"]
    assert [c["command"] for c in listed] == ["hello"]
    assert call(world, "GET", "/api/apps/commands")["data"][
        "mentionable"] == []
    assert listed[0]["app"]["slug"] == "hello-app"
    assert "url" not in listed[0]
    assert call(world, "GET", "/api/apps/commands",
                user="bob")["data"]["commands"] == []
    assert _run(world, "/hello", user="bob")["status"] == 404


def test_slash_command_round_trip_is_signed_and_ephemeral_by_default():
    world = _world(reply={"text": "Hello, @alice!", "blocks": BLOCKS})
    created = _app(world)
    result = _run(world, "/hello  there friend")
    assert result["status"] == 200, result["data"]
    data = result["data"]
    assert data["responseType"] == "ephemeral"
    assert data["text"] == "Hello, @alice!"
    assert data["app"]["botName"] == "Hello App (app)"
    assert data["blocks"][1]["elements"][0]["action_id"] == "wave"
    assert world.chat == []
    [request] = world.http
    headers = request["headers"]
    assert core.verify_signature(
        created["signingSecret"], headers["ForkMesh-Webhook-Id"],
        headers["ForkMesh-Webhook-Timestamp"], request["body"].encode(),
        headers["ForkMesh-Webhook-Signature"], world.now // 1000)
    payload = json.loads(request["body"])
    assert payload["command"] == "hello"
    assert payload["text"] == "there friend"
    assert payload["user"] == {"name": "alice"}
    assert payload["channel"] == {"id": CHANNEL, "name": "general"}
    token = urlparse(payload["responseUrl"]).path.rsplit("/", 1)[1]
    assert token.startswith("fmru_")
    assert token not in "\n".join(
        repr(tuple(r)) for r in world.db.execute(
            "SELECT * FROM app_response_urls"))


def test_in_channel_replies_post_as_the_bot_in_public_channels_only():
    world = _world(reply={"responseType": "in_channel", "text": "Hi all"})
    _app(world)
    result = _run(world, "/hello")
    assert result["data"]["responseType"] == "in_channel"
    [(channel, plain)] = world.chat
    assert channel == CHANNEL and plain["sender"] == "Hello App (app)"
    private = _run(world, "/hello", channel=PRIVATE)
    assert private["data"]["responseType"] == "ephemeral"
    assert len(world.chat) == 1


def test_in_channel_needs_chat_write():
    world = _world(reply={"responseType": "in_channel", "text": "Hi"})
    created = create_app(world, scopes=["commands", "chat:write"],
                         commands=[{"command": "hello", "url": COMMAND_URL}],
                         bot={"enabled": True})
    install(world, created, scope="commands")
    assert _run(world, "/hello")["data"]["responseType"] == "ephemeral"
    assert world.chat == []


def test_response_url_is_limited_and_single_install():
    world = _world(reply={"text": "working on it"})
    _app(world)
    _run(world, "/hello")
    token = urlparse(json.loads(world.http[-1]["body"])[
        "responseUrl"]).path.rsplit("/", 1)[1]
    path = "/api/apps/respond/" + token
    later = call(world, "POST", path, {"responseType": "in_channel",
                                       "text": "Done!"}, user="")
    assert later["data"]["responseType"] == "in_channel"
    assert world.chat[-1][1]["text"] == "Done!"
    private = call(world, "POST", path, {"text": "Just for you"}, user="")
    assert private["data"]["responseType"] == "ephemeral"
    assert world.notices[-1][0] == "alice"
    for _ in range(core.RESPONSE_URL_MAX_USES - 2):
        assert call(world, "POST", path, {"text": "x"}, user="")["status"] \
            == 200
    assert call(world, "POST", path, {"text": "x"}, user="")["status"] == 404
    assert call(world, "POST", "/api/apps/respond/fmru_" + "0" * 64,
                {"text": "x"}, user="")["status"] == 404


def test_interactive_callbacks_accept_only_offered_actions():
    world = _world(reply={"responseType": "in_channel", "text": "Pick",
                          "blocks": BLOCKS},
                   interaction={"responseType": "in_channel",
                                "text": "bob waved"})
    created = _app(world)
    posted = _run(world, "/hello")
    message_id = posted["data"]["messageId"]
    stored = world.chat[-1][1]
    assert stored["appMessageId"] == message_id
    for body in ({"actionId": "nope", "value": "wave"},
                 {"actionId": "wave", "value": "other"},
                 {"actionId": "mood", "value": "evil"}):
        bad = call(world, "POST", "/api/apps/interact", dict(
            body, appMessageId=message_id, channelId=CHANNEL), user="bob")
        assert bad["data"]["error"] == "invalid_action"
    wrong_channel = call(world, "POST", "/api/apps/interact", {
        "appMessageId": message_id, "channelId": PRIVATE,
        "actionId": "wave", "value": "wave"}, user="bob")
    assert wrong_channel["status"] == 403
    world.http = []
    ok = call(world, "POST", "/api/apps/interact", {
        "appMessageId": message_id, "channelId": CHANNEL,
        "actionId": "wave", "value": "wave"}, user="bob")
    assert ok["data"]["responseType"] == "in_channel"
    request = world.http[0]
    assert request["url"] == INTERACTIVE_URL
    payload = json.loads(request["body"])
    assert payload["action"] == {"actionId": "wave", "value": "wave"}
    assert payload["user"] == {"name": "bob"}
    assert core.verify_signature(
        created["signingSecret"], request["headers"]["ForkMesh-Webhook-Id"],
        request["headers"]["ForkMesh-Webhook-Timestamp"],
        request["body"].encode(),
        request["headers"]["ForkMesh-Webhook-Signature"], world.now // 1000)
    select = call(world, "POST", "/api/apps/interact", {
        "appMessageId": message_id, "channelId": CHANNEL,
        "actionId": "mood", "value": "great"}, user="bob")
    assert select["status"] == 200
    world.now += core.INTERACTIVE_MESSAGE_TTL_MS + 1
    expired = call(world, "POST", "/api/apps/interact", {
        "appMessageId": message_id, "channelId": CHANNEL,
        "actionId": "wave", "value": "wave"}, user="bob")
    assert expired["data"]["error"] == "message_expired"


def test_ephemeral_block_messages_belong_to_one_person():
    world = _world(reply={"text": "Choose", "blocks": BLOCKS})
    _app(world)
    message_id = _run(world, "/hello")["data"]["appMessageId"]
    other = call(world, "POST", "/api/apps/interact", {
        "appMessageId": message_id, "actionId": "wave", "value": "wave"},
        user="bob")
    assert other["status"] == 403
    mine = call(world, "POST", "/api/apps/interact", {
        "appMessageId": message_id, "actionId": "wave", "value": "wave"})
    assert mine["status"] == 200


def test_failed_app_reports_unavailable_and_logs():
    world = _world()
    created = _app(world)
    world.http_handler = lambda url, headers, body: (500, "")
    result = _run(world, "/hello")
    assert result["status"] == 502
    log = call(world, "GET", "/api/apps/%s/deliveries"
               % created["app"]["id"])["data"]["deliveries"]
    assert log[0]["kind"] == "command" and log[0]["outcome"] == "failed"


def test_mentions_are_forwarded_only_to_apps_the_sender_installed():
    world = _world()
    created = _app(world, events=["app.mention"],
                   eventUrl="https://hello.example.org/events")

    def handler(url, headers, body):
        payload = json.loads(body)
        if payload.get("type") == "url_verification":
            return 200, json.dumps({"challenge": payload["challenge"]})
        return 200, ""
    world.http_handler = handler
    call(world, "POST", "/api/apps/%s/verify" % created["app"]["id"])
    world.http = []
    sent = call(world, "POST", "/api/apps/mentions", {
        "channelId": CHANNEL, "messageId": "m1",
        "text": "hey @hello-app and @other-app"})
    assert sent["data"]["forwarded"] == 1
    event = json.loads(world.http[0]["body"])["event"]
    assert event["type"] == "app.mention"
    assert event["sender"] == {"name": "alice"}
    assert call(world, "POST", "/api/apps/mentions", {
        "channelId": CHANNEL, "text": "@hello-app"},
        user="bob")["data"]["forwarded"] == 0
    assert call(world, "POST", "/api/apps/mentions", {
        "channelId": PRIVATE, "text": "@hello-app"})["status"] == 409
