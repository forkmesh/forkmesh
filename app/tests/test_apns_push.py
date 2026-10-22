#!/usr/bin/env python3
"""APNs provider-token and payload contract (mobile push).

A phone cannot hold the ForkMeshNodes event socket the desktop and browser use,
so APNs is the only transport that reaches an app iOS has suspended. The two
things easiest to get silently wrong are the ES256 provider JWT (Apple answers
403 forever and no push ever lands) and the alert payload, which must name its
event in full — there is no generic "New activity" banner. Both are pinned
here.

`app/src/apns.py` runs inside Python Workers, so `js` and `pyodide.ffi` are
stubbed with a WebCrypto shim backed by `cryptography` — which means the JWT
this module produces is verified against the real P-256 key, not just shape
checked.
"""

import ast
import asyncio
import base64
import importlib
import json
import sys
import types
from pathlib import Path

import pytest
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, utils as asym_utils

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"

TEAM_ID = "742ZANCB57"
KEY_ID = "7LYZ4MQFW4"


def _test_key_pem():
    key = ec.generate_private_key(ec.SECP256R1())
    pem = key.private_bytes(
        encoding=serialization.Encoding.PEM,
        format=serialization.PrivateFormat.PKCS8,
        encryption_algorithm=serialization.NoEncryption(),
    ).decode()
    return key, pem


class _Subtle:
    """Minimal WebCrypto shim: importKey('pkcs8') + sign(ECDSA/SHA-256)."""

    async def importKey(self, fmt, data, algorithm, extractable, usages):
        assert fmt == "pkcs8"
        return serialization.load_der_private_key(bytes(data), password=None)

    async def sign(self, algorithm, key, data):
        der = key.sign(bytes(data), ec.ECDSA(hashes.SHA256()))
        r, s = asym_utils.decode_dss_signature(der)
        # WebCrypto returns the raw r||s pair, which is what JWS ES256 wants.
        return r.to_bytes(32, "big") + s.to_bytes(32, "big")


class _Uint8Array:
    def __init__(self, data):
        self._data = bytes(data)

    @staticmethod
    def new(data):
        return _Uint8Array(data)

    def to_py(self):
        return self._data

    def __bytes__(self):
        return self._data

    def __iter__(self):
        return iter(self._data)


@pytest.fixture()
def apns(monkeypatch):
    js = types.ModuleType("js")
    js.Date = types.SimpleNamespace(now=lambda: 1_700_000_000_000)
    js.Uint8Array = _Uint8Array
    js.crypto = types.SimpleNamespace(subtle=_Subtle())
    js.Object = types.SimpleNamespace(fromEntries=dict)

    ffi = types.ModuleType("pyodide.ffi")
    ffi.to_js = lambda value, dict_converter=None: value
    pyodide = types.ModuleType("pyodide")
    pyodide.ffi = ffi

    monkeypatch.setitem(sys.modules, "js", js)
    monkeypatch.setitem(sys.modules, "pyodide", pyodide)
    monkeypatch.setitem(sys.modules, "pyodide.ffi", ffi)
    monkeypatch.syspath_prepend(str(SRC))
    sys.modules.pop("apns", None)
    module = importlib.import_module("apns")
    yield module
    sys.modules.pop("apns", None)


def _env(pem):
    return types.SimpleNamespace(
        APNS_AUTH_KEY=pem, APNS_KEY_ID=KEY_ID, APNS_TEAM_ID=TEAM_ID)


def test_provider_token_is_a_verifiable_es256_jwt(apns):
    key, pem = _test_key_pem()
    apns.reset_provider_token()
    jwt = asyncio.run(apns.provider_token(_env(pem)))

    header_b64, claims_b64, sig_b64 = jwt.split(".")

    def unb64(part):
        return base64.urlsafe_b64decode(part + "=" * ((4 - len(part) % 4) % 4))

    header = json.loads(unb64(header_b64))
    claims = json.loads(unb64(claims_b64))
    # Apple rejects anything but ES256, and routes the token by `kid`/`iss`.
    assert header == {"alg": "ES256", "kid": KEY_ID}
    assert claims["iss"] == TEAM_ID
    assert isinstance(claims["iat"], int)

    signature = unb64(sig_b64)
    assert len(signature) == 64, "JWS ES256 is raw r||s, never DER"
    r = int.from_bytes(signature[:32], "big")
    s = int.from_bytes(signature[32:], "big")
    key.public_key().verify(
        asym_utils.encode_dss_signature(r, s),
        (header_b64 + "." + claims_b64).encode(),
        ec.ECDSA(hashes.SHA256()),
    )


def test_provider_token_is_cached_then_reminted_after_reset(apns):
    _, pem = _test_key_pem()
    apns.reset_provider_token()
    env = _env(pem)
    first = asyncio.run(apns.provider_token(env))
    # Apple rejects a token refreshed too eagerly, so a second call inside the
    # window must reuse the cached one rather than sign again.
    assert asyncio.run(apns.provider_token(env)) == first
    assert apns.APNS_TOKEN_TTL_SECONDS <= 3600, "Apple rejects tokens over 1h"

    apns.reset_provider_token()
    assert apns.provider_token is not None
    assert not apns._PROVIDER_TOKEN, "reset must clear the cached JWT"


_PING = {"title": "#42 New comment", "subtitle": "acme/widgets \xb7 by bob",
         "body": "Looks good to me.", "threadId": "acme/widgets",
         "alertId": "row-1", "href": "/acme/widgets/issues/42"}


def test_an_alert_payload_is_the_event_itself(apns):
    payload = json.loads(apns.alert_payload(_PING, "pings"))
    # The banner says what happened without depending on the extension.
    assert payload["aps"]["alert"] == {
        "title": "#42 New comment", "subtitle": "acme/widgets \xb7 by bob",
        "body": "Looks good to me."}
    assert payload["aps"]["thread-id"] == "acme/widgets"
    assert payload["aps"]["mutable-content"] == 1
    # userInfo for the tap: lands on the record the banner names.
    assert payload["alertId"] == "row-1"
    assert payload["href"] == "/acme/widgets/issues/42"
    assert "topic" not in payload


def test_a_desktop_agent_ping_names_its_category_and_session(apns):
    alert = dict(_PING, category="agent-finished", sessionId="12")
    payload = json.loads(apns.alert_payload(alert, "pings"))
    assert payload["aps"]["category"] == "agent-finished"
    assert payload["sessionId"] == "12"
    assert "sessionId" not in payload["aps"]
    plain = json.loads(apns.alert_payload(_PING, "pings"))
    assert "category" not in plain["aps"]
    assert "sessionId" not in plain


def test_a_desktop_power_alarm_rings_the_alarm_sound_through_focus(apns):
    payload = json.loads(apns.alert_payload(dict(_PING, alarm="power"), "pings"))
    assert payload["aps"]["sound"] == "desktop_power_alarm.wav"
    assert payload["aps"]["interruption-level"] == "time-sensitive"
    assert payload["alarm"] == "power"
    sound = ROOT.parent / "native" / "ios" / "ForkMesh" / "Resources" / \
        "Sounds" / payload["aps"]["sound"]
    assert sound.is_file()
    for alert in (_PING, dict(_PING, alarm=""), dict(_PING, alarm="other")):
        plain = json.loads(apns.alert_payload(alert, "pings"))
        assert plain["aps"]["sound"] == "default"
        assert "interruption-level" not in plain["aps"]
        assert "alarm" not in plain


def test_every_payload_names_the_project_origin_it_came_from(apns):
    # A phone signed in to several projects (app.forkmesh.com,
    # app.forkmesh.com) routes a tap by this origin, for every topic.
    origin = "https://app.forkmesh.com"
    for topic in ("pings", "code-push", "chat", "direct-messages", ""):
        payload = json.loads(apns.alert_payload(_PING, topic, origin))
        assert payload["origin"] == origin
        # Custom keys ride outside aps, beside alertId and href.
        assert "origin" not in payload["aps"]
        assert payload["href"] == _PING["href"]
    # The origin is part of the payload the 4 KB trim measures.
    long_alert = dict(_PING, body="\u00e9\u4e2d" * 3000)
    encoded = apns.alert_payload(long_alert, "pings", origin)
    assert len(encoded.encode("utf-8")) <= apns.APNS_PAYLOAD_MAX_BYTES
    assert json.loads(encoded)["origin"] == origin

    env = types.SimpleNamespace(APP_ORIGIN="https://app.forkmesh.com/")
    assert apns.apns_origin(env) == "https://app.forkmesh.com"
    for bad in ("", "app.forkmesh.com", "javascript:alert(1)",
                "https://app.forkmesh.com/path", "https://a b.com"):
        assert apns.apns_origin(
            types.SimpleNamespace(APP_ORIGIN=bad)) == "", bad
    assert apns.apns_origin(types.SimpleNamespace()) == ""

    # send_one stamps the Worker's own APP_ORIGIN on what it sends.
    fetch_impl = _fetch(_Response(200))
    asyncio.run(apns.send_one(
        env, fetch_impl, "jwt", "ab" * 32, "production", _PING,
        topic="code-push"))
    sent = json.loads(fetch_impl.calls[0][1]["body"])
    assert sent["origin"] == "https://app.forkmesh.com"
    assert sent["topic"] == "code-push"
    assert "apns-collapse-id" not in fetch_impl.calls[0][1]["headers"]


def test_no_push_is_ever_the_ambiguous_new_activity_notice(apns):
    source = (SRC / "apns.py").read_text()
    assert '"New activity"' not in source
    assert "APNS_ALERT_TITLE" not in source
    assert "Open ForkMesh to see what happened" not in source
    # An alert with nothing to say is not sent at all.
    assert apns.alert_payload({}) == ""
    assert apns.alert_payload(None) == ""
    assert apns.alert_payload({"title": "  ", "body": "text"}) == ""
    fetch_impl = _fetch()
    assert asyncio.run(apns.send_one(
        types.SimpleNamespace(), fetch_impl, "jwt", "ab" * 32,
        "production", {})) == (False, False, 0, "no_alert_text")
    assert fetch_impl.calls == []


def test_sealed_conversations_name_neither_sender_nor_channel(apns):
    dm = apns.direct_message_alert()
    assert dm["title"] == "\U0001F4E8 New message"
    assert dm["body"] == "New message"
    assert "encrypted" not in json.dumps(dm).lower()
    assert apns.direct_message_alert(started=True)["title"] == (
        "\U0001F44B New direct conversation")
    room = apns.chat_message_alert(channel_id=_CHANNEL_ID)
    assert room["title"] == "\U0001F4AC New message"
    assert apns.chat_message_alert()["title"] == "\U0001F4AC New message"
    payload = json.loads(apns.alert_payload(room, "chat"))
    assert "conversation" not in payload
    assert payload["aps"]["thread-id"] == "channel:" + _CHANNEL_ID
    for alert in (dm, room, apns.APNS_TEST_ALERT):
        assert "activity" not in json.dumps(alert).lower()


def test_no_chat_or_direct_push_carries_a_sender(apns):
    for alert in (apns.direct_message_alert(conversation_id=_DIRECT_ID,
                                            ref=_REF),
                  apns.direct_message_alert(started=True)):
        assert "sender" not in alert
        payload = json.loads(apns.alert_payload(alert, "direct-messages"))
        assert "sender" not in payload
        assert "alice" not in json.dumps(payload)
    assert "sender" not in json.loads(apns.alert_payload(
        apns.chat_message_alert("#mesh"), "chat"))
    for topic in ("direct-messages", "chat", "pings"):
        assert "sender" not in json.loads(apns.alert_payload(
            {"title": "t", "sender": "alice"}, topic)), topic


def test_a_tap_on_a_sealed_conversation_knows_which_one_to_open(apns):
    direct_id = "0123456789abcdef" * 2
    dm = apns.direct_message_alert(conversation_id=direct_id.upper())
    payload = json.loads(apns.alert_payload(dm, "direct-messages"))
    assert payload["conversation"] == "direct:" + direct_id
    assert "conversation" not in payload["aps"]
    for bad in ("", "../x", "0123", direct_id + "0"):
        assert "conversation" not in apns.direct_message_alert(
            conversation_id=bad), bad

    assert not hasattr(apns, "office_conversation")
    mesh = apns.chat_message_alert(apns.APNS_MESH_CONVERSATION)
    assert json.loads(apns.alert_payload(mesh, "chat"))["conversation"] == (
        "#mesh")
    assert "conversation" not in json.loads(apns.alert_payload(
        apns.chat_message_alert(), "chat"))

    assert "conversation" not in json.loads(apns.alert_payload(
        dict(_PING, conversation="#general"), "pings"))
    for bad in ("general", "#" + "x" * 200, "#a\nb", "@bob"):
        assert "conversation" not in json.loads(apns.alert_payload(
            {"title": "t", "conversation": bad}, "chat")), bad


def test_a_long_body_is_shortened_to_fit_apples_payload_limit(apns):
    alert = dict(_PING, body="\u00e9\u4e2d" * 3000)
    encoded = apns.alert_payload(alert, "pings")
    assert len(encoded.encode("utf-8")) <= apns.APNS_PAYLOAD_MAX_BYTES
    payload = json.loads(encoded)
    # The headline survives whole; only the body is cut, and says so.
    assert payload["aps"]["alert"]["title"] == "#42 New comment"
    assert payload["aps"]["alert"]["body"].endswith("\u2026")
    assert len(payload["aps"]["alert"]["body"]) > 100


def test_push_topics_exclude_the_refresh_hints(apns):
    # "issues" and "catalog" are socket-side refresh hints. Neither names an
    # inbox row, so a banner for one could only read "New activity" — and
    # "issues" fires for the owner's own comments and closes too. Issue events
    # worth a buzz raise a ping, which pushes with the actual text.
    # "code-push" is a ping under its own name (kind code_push), not a hint.
    assert set(apns.APNS_PUSH_TOPICS) == {
        "pings", "direct-messages", "chat", "code-push", "calls"}


def test_only_the_sealed_topics_are_marked(apns):
    # Room chat and direct messages are sealed end to end: the phone has to be
    # told not to go looking for inbox text, and room chat must stay off the
    # sender's own screen. An inbox notification stays unmarked.
    alert = {"title": "New message"}
    for topic in apns.APNS_CONVERSATION_TOPICS:
        marked = json.loads(apns.alert_payload(alert, topic))
        assert marked["topic"] == topic
    assert apns.collapse_id_for("chat") == "chat"
    assert apns.collapse_id_for("direct-messages") == ""
    for topic in ("pings", "issues", "", None):
        assert "topic" not in json.loads(apns.alert_payload(alert, topic))
        assert apns.collapse_id_for(topic) == ""
    # A code push is marked, so the phone can tell it from an inbox ping, but
    # never collapsed: every push banner is its own notification.
    marked = json.loads(apns.alert_payload(alert, "code-push"))
    assert marked["topic"] == "code-push"
    assert apns.collapse_id_for("code-push") == ""


def test_every_ping_and_direct_message_is_its_own_banner(apns):
    # A collapse id makes a new alert replace the previous one. A direct
    # message banner now shows the text the phone decrypted, so collapsing
    # would throw the earlier message's text away (Signal shows each one).
    assert apns.collapse_id_for("pings") == ""
    assert apns.collapse_id_for("issues") == ""
    assert apns.collapse_id_for("direct-messages") == ""
    assert apns.collapse_id_for("chat") == "chat"


def test_the_mesh_wake_shares_the_phones_own_mesh_alert_slot(apns):
    assert apns.collapse_id_for("chat", "#mesh") == "mesh-room"
    assert apns.collapse_id_for("chat", "#general") == "chat"
    assert apns.collapse_id_for("direct-messages", "#mesh") == ""
    alert = apns.chat_message_alert(apns.APNS_MESH_CONVERSATION)
    assert apns.collapse_id_for(
        apns.APNS_CHAT_TOPIC, alert.get("conversation", "")) == "mesh-room"
    swift = (ROOT.parent / "native" / "ios" / "ForkMeshCore" / "Sources"
             / "FMChat" / "Mesh" / "MeshAlert.swift").read_text()
    assert 'meshAlertIdentifier = "mesh-room"' in swift


_REF = "0123456789abcdef0123456789abcdef01234567"
_DIRECT_ID = "0123456789abcdef" * 2
_CHANNEL_ID = "fedcba9876543210" * 2


def test_a_direct_message_push_names_the_ciphertext_to_fetch(apns):
    alert = apns.direct_message_alert(
        conversation_id=_DIRECT_ID, ref=_REF.upper(), key_version=3)
    payload = json.loads(apns.alert_payload(alert, "direct-messages"))
    assert payload["conversation"] == "direct:" + _DIRECT_ID
    assert payload["ref"] == _REF
    assert payload["keyVersion"] == 3
    assert "sender" not in payload
    assert payload["aps"]["alert"]["title"] == "\U0001F4E8 New message"
    assert payload["aps"]["thread-id"] == "direct:" + _DIRECT_ID
    assert payload["aps"]["mutable-content"] == 1
    assert payload["aps"]["alert"]["body"] == "New message"
    for key in ("ref", "keyVersion", "text", "body", "channelId", "frame"):
        assert key not in payload["aps"]
    assert set(payload) == {
        "aps", "conversation", "ref", "keyVersion", "topic"}


_FRAME = {"kind": "cipher", "v": 1, "iv": "a" * 16, "data": "b" * 600,
          "persist": True, "notify": True}


def test_a_direct_message_push_carries_its_sealed_frame(apns):
    alert = apns.direct_message_alert(
        conversation_id=_DIRECT_ID, ref=_REF, key_version=3,
        frame=json.dumps(_FRAME))
    encoded = apns.alert_payload(alert, "direct-messages")
    payload = json.loads(encoded)
    assert payload["frame"] == _FRAME
    assert payload["ref"] == _REF
    relayed = dict(_FRAME, ref=_REF, recipients=1, recipientNames=["bob"],
                   historyReplay=True)
    assert apns.direct_message_alert(
        conversation_id=_DIRECT_ID, ref=_REF,
        frame=json.dumps(relayed))["frame"] == _FRAME
    assert "frame" not in payload["aps"]
    assert len(encoded.encode("utf-8")) <= apns.APNS_PAYLOAD_MAX_BYTES
    assert apns.direct_message_alert(
        conversation_id=_DIRECT_ID, ref=_REF, key_version=3,
        frame=_FRAME)["frame"] == _FRAME
    for bad in ("", "not json", "[1]", json.dumps({"kind": "plain"})):
        assert "frame" not in apns.direct_message_alert(
            conversation_id=_DIRECT_ID, ref=_REF, frame=bad)
    assert "frame" not in apns.direct_message_alert(
        conversation_id=_DIRECT_ID, frame=_FRAME)
    assert "frame" not in apns.direct_message_alert(
        started=True, conversation_id=_DIRECT_ID, ref=_REF,
        frame=_FRAME)
    for topic in ("chat", "pings"):
        assert "frame" not in json.loads(apns.alert_payload(alert, topic))


def test_a_frame_too_big_for_apple_stays_on_the_relay(apns):
    huge = dict(_FRAME, data="b" * 5000)
    alert = apns.direct_message_alert(
        conversation_id=_DIRECT_ID, ref=_REF, key_version=3,
        frame=huge)
    encoded = apns.alert_payload(alert, "direct-messages")
    payload = json.loads(encoded)
    assert "frame" not in payload
    assert payload["ref"] == _REF
    assert payload["aps"]["alert"]["body"] == "New message"
    assert len(encoded.encode("utf-8")) <= apns.APNS_PAYLOAD_MAX_BYTES


def test_a_v3_direct_frame_is_left_for_the_pending_route(apns):
    v3_body = "A" * 6200
    relayed = json.dumps(dict(_FRAME, data=v3_body, ref=_REF, recipients=1))
    alert = apns.direct_message_alert(
        conversation_id=_DIRECT_ID, ref=_REF, key_version=1, frame=relayed)
    assert "frame" not in alert
    encoded = apns.alert_payload(
        dict(alert, frame=json.loads(relayed)), "direct-messages")
    payload = json.loads(encoded)
    assert "frame" not in payload
    assert payload["ref"] == _REF
    assert payload["conversation"] == "direct:" + _DIRECT_ID
    assert payload["aps"]["alert"] == {
        "title": "\U0001F4E8 New message", "body": "New message"}
    assert len(encoded.encode("utf-8")) <= apns.APNS_PAYLOAD_MAX_BYTES


def test_a_push_never_carries_a_malformed_fetch_handle(apns):
    for ref, version in (("", 1), ("xyz", 1), (_REF + "0", 1),
                         (_REF, 0), (_REF, -1), (_REF, True), (_REF, "2x")):
        alert = apns.direct_message_alert(
            conversation_id=_DIRECT_ID, ref=ref, key_version=version)
        payload = json.loads(apns.alert_payload(alert, "direct-messages"))
        if ref != _REF:
            assert "ref" not in payload, ref
        if version is True or version in (0, -1, "2x"):
            assert "keyVersion" not in payload, version
    started = apns.direct_message_alert(
        started=True, conversation_id=_DIRECT_ID, ref=_REF,
        key_version=1)
    assert "ref" not in started and "keyVersion" not in started
    no_conversation = apns.direct_message_alert(ref=_REF)
    assert "ref" not in no_conversation
    for topic in ("pings", "code-push"):
        payload = json.loads(apns.alert_payload(
            dict(_PING, ref=_REF, keyVersion=1, channelId=_CHANNEL_ID),
            topic))
        assert not {"ref", "keyVersion", "channelId"} & set(payload)


def test_a_channel_push_routes_to_the_channel_and_ciphertext(apns):
    alert = apns.chat_message_alert(
        channel_id=_CHANNEL_ID, ref=_REF, key_version=2)
    payload = json.loads(apns.alert_payload(alert, "chat"))
    assert payload["channelId"] == _CHANNEL_ID
    assert payload["ref"] == _REF
    assert payload["keyVersion"] == 2
    assert "conversation" not in payload
    assert payload["aps"]["thread-id"] == "channel:" + _CHANNEL_ID
    world = json.loads(apns.alert_payload(
        apns.chat_message_alert(ref=_REF), "chat"))
    assert not {"channelId", "ref", "keyVersion"} & set(world)
    bare = json.loads(apns.alert_payload(
        apns.chat_message_alert(channel_id=_CHANNEL_ID, ref=_REF), "chat"))
    assert bare["aps"]["thread-id"] == "channel:" + _CHANNEL_ID


def test_an_alert_outlives_a_night_in_flight_mode(apns):
    # Apple keeps only the newest alert per app for an offline phone, so a long
    # window costs one banner on reconnect; a one hour window silently dropped
    # the only ping of the night.
    assert apns.APNS_EXPIRATION_SECONDS >= 12 * 60 * 60
    fetch_impl = _fetch(_Response(200))
    _send(apns, fetch_impl)
    expiration = int(fetch_impl.calls[0][1]["headers"]["apns-expiration"])
    assert expiration == 1_700_000_000 + apns.APNS_EXPIRATION_SECONDS


def test_a_call_push_rings_names_the_caller_and_the_conversation(apns):
    alert = apns.direct_call_alert(conversation_id="A" * 32, caller=" Bob ")
    assert alert["conversation"] == "direct:" + "a" * 32
    assert alert["body"] == "@bob is calling you"
    assert "Incoming call" in alert["title"]
    payload = json.loads(apns.alert_payload(alert, "calls"))
    assert payload["topic"] == "calls"
    assert payload["conversation"] == "direct:" + "a" * 32
    assert payload["aps"]["sound"] == apns.APNS_CALL_SOUND
    assert payload["aps"]["interruption-level"] == "time-sensitive"
    assert "mutable-content" not in payload["aps"]
    assert apns.collapse_id_for("calls", alert["conversation"]) == (
        "call-direct:" + "a" * 32)
    unnamed = apns.direct_call_alert(conversation_id="nothex")
    assert "conversation" not in unnamed
    assert unnamed["body"] == "Someone is calling you in ForkMesh"


def test_a_call_push_expires_with_the_ring(apns):
    fetch_impl = _fetch(_Response(200))
    asyncio.run(apns.send_one(
        types.SimpleNamespace(), fetch_impl, "jwt", "ab" * 32, "development",
        apns.direct_call_alert(conversation_id="a" * 32, caller="bob"),
        topic="calls"))
    expiration = int(fetch_impl.calls[0][1]["headers"]["apns-expiration"])
    assert expiration == 1_700_000_000 + apns.APNS_CALL_EXPIRATION_SECONDS
    assert apns.APNS_CALL_EXPIRATION_SECONDS <= 120


def test_a_call_event_reaches_the_socket_with_its_conversation():
    runtime = _account_event_runtime()
    runtime.notify("alice", "calls", {
        "title": "t", "conversation": "direct:" + "a" * 32})
    assert runtime.frames == ["calls&conversation=direct:" + "a" * 32]
    assert runtime.pushes == [("alice", "calls")]


def test_every_client_rings_with_the_same_topic_and_sound(apns):
    repo = ROOT.parent
    swift_calls = (repo / "native/ios/ForkMeshCore/Sources/FMChat/Direct"
                   / "DirectCalls.swift").read_text()
    assert 'ringTopic = "' + apns.APNS_CALL_TOPIC + '"' in swift_calls
    assert 'ringSound = "' + apns.APNS_CALL_SOUND + '"' in swift_calls
    kotlin_calls = (repo / "native/android/core/src/main/kotlin/com/forkmesh"
                    / "nativeapp/chat/DirectCalls.kt").read_text()
    assert 'RING_TOPIC = "' + apns.APNS_CALL_TOPIC + '"' in kotlin_calls
    swift_direct = (repo / "native/ios/ForkMeshCore/Sources/FMChat/Direct"
                    / "DirectMessages.swift").read_text()
    assert 'envelope["ring"] = true\n                if let call = extra["call"]' in (
        swift_direct)
    kotlin_direct = (repo / "native/android/core/src/main/kotlin/com/forkmesh"
                     / "nativeapp/chat/DirectMessages.kt").read_text()
    assert ('if (type == DirectCallEngine.CALL_INVITE_TYPE) {\n'
            '                envelope["ring"] = true\n'
            '                extra["call"]?.let { envelope["call"] = it }'
            ) in kotlin_direct
    transport = (ROOT / "public" / "chat-room-transport.js").read_text()
    assert ('if (!persist && plain?.type === "call-invite" '
            '&& isDirectRoom(room)) {\n        envelope.ring = true;\n'
            '        if (plain.call) envelope.call = String(plain.call);'
            ) in transport


def test_the_iphone_reports_every_voip_push_to_callkit():
    ios = ROOT.parent / "native" / "ios"
    plist = (ios / "Config" / "ForkMesh-Info.plist").read_text()
    modes = plist[plist.index("<key>UIBackgroundModes</key>"):]
    assert "<string>voip</string>" in modes[:modes.index("</array>")]
    delegate = (ios / "ForkMesh" / "App" / "AppDelegate.swift").read_text()
    assert "DirectCallKit.shared.start()" in delegate
    bridge = (ios / "ForkMesh" / "Features" / "Chat" / "Calls"
              / "DirectCallKit.swift").read_text()
    incoming = bridge[bridge.index("didReceiveIncomingPushWith payload"):]
    assert "received(fields, completion: completion)" in incoming
    received = bridge[bridge.index("private func received("):]
    received = received[:received.index("\n    }\n")]
    assert received.count("apply(") == 2
    assert ".report(id: id" in received
    assert "provider.reportNewIncomingCall(" in bridge


def test_host_follows_the_tokens_build_environment(apns):
    assert apns.apns_host("development") == apns.APNS_HOST_SANDBOX
    assert apns.apns_host("production") == apns.APNS_HOST_PRODUCTION
    # Unknown/absent must never silently mean sandbox in a release build.
    assert apns.apns_host("") == apns.APNS_HOST_PRODUCTION
    assert apns.apns_host(None) == apns.APNS_HOST_PRODUCTION


def test_only_permanent_failures_prune_a_token(apns):
    assert apns._dead_token(410, "Unregistered") is True
    assert apns._dead_token(400, "BadDeviceToken") is True
    assert apns._dead_token(400, "DeviceTokenNotForTopic") is True
    # Transient conditions must never drop a live token: doing so would mute a
    # phone permanently on a blip.
    assert apns._dead_token(429, "TooManyRequests") is False
    assert apns._dead_token(503, "ServiceUnavailable") is False
    assert apns._dead_token(403, "ExpiredProviderToken") is False
    assert apns._dead_token(400, "PayloadTooLarge") is False


def test_configuration_requires_all_three_secrets(apns):
    assert apns.apns_configured(_env("pem")) is True
    assert apns.apns_configured(types.SimpleNamespace(
        APNS_AUTH_KEY="", APNS_KEY_ID=KEY_ID, APNS_TEAM_ID=TEAM_ID)) is False
    assert apns.apns_configured(types.SimpleNamespace(
        APNS_AUTH_KEY="pem", APNS_KEY_ID="", APNS_TEAM_ID=TEAM_ID)) is False
    assert apns.apns_configured(types.SimpleNamespace(
        APNS_AUTH_KEY="pem", APNS_KEY_ID=KEY_ID, APNS_TEAM_ID="")) is False


def test_a_malformed_apple_identifier_is_named_not_silently_used(apns):
    # Apple's Key ID and Team ID are exactly ten characters of A-Z0-9. Any
    # other shape signs a perfectly good looking JWT that Apple then refuses on
    # every push with 403 InvalidProviderToken, so it has to be caught here.
    def env(key_id=KEY_ID, team_id=TEAM_ID, key="pem"):
        return types.SimpleNamespace(
            APNS_AUTH_KEY=key, APNS_KEY_ID=key_id, APNS_TEAM_ID=team_id)

    assert apns.apns_config_problem(env()) == ""
    assert apns.apns_config_problem(env(key="")) == "missing_secrets"
    for bad in ("ABCD_EFG_HI", "abcdefghij", "ABCDEFGHI", "ABCDEFGHIJK",
                "ABCDE-GHIJ"):
        assert apns.apns_config_problem(env(key_id=bad)) == "key_id_malformed"
        assert apns.apns_configured(env(key_id=bad)) is False
        assert apns.apns_config_problem(env(team_id=bad)) == (
            "team_id_malformed")
    # Surrounding whitespace from a pasted secret is not a malformation.
    assert apns.apns_config_problem(env(key_id=" " + KEY_ID + "\n")) == ""


def test_p8_armour_and_escaped_newlines_are_both_accepted(apns):
    _, pem = _test_key_pem()
    direct = apns._decode_p8(pem)
    # `wrangler secret put` round-trips a pasted key with literal \n sequences
    # often enough that both forms have to load.
    escaped = apns._decode_p8(pem.replace("\n", "\\n"))
    assert direct == escaped
    serialization.load_der_private_key(direct, password=None)
    with pytest.raises(ValueError):
        apns._decode_p8("")


def test_token_lifecycle_is_bounded_and_account_scoped():
    source = (SRC / "entry.py").read_text()
    assert "apns.APNS_MAX_TOKENS_PER_ACCOUNT" in source
    assert "WHERE account_bi=? ORDER BY last_seen DESC" in source
    assert "WHERE token_bi=? AND account_bi=?" in source
    assert "unregister_apns_device_token(env, account, token)" in source


# --- gateway round trip ------------------------------------------------------

class _Response:
    def __init__(self, status, body=""):
        self.status = status
        self._body = body

    async def text(self):
        return self._body


def _fetch(*responses):
    """A fetch_impl that answers from a script and records every call."""
    queue = list(responses)
    calls = []

    async def fetch_impl(url, init, timeout_seconds):
        calls.append((url, init, timeout_seconds))
        answer = queue.pop(0)
        if isinstance(answer, Exception):
            raise answer
        return answer

    fetch_impl.calls = calls
    return fetch_impl


def _send(apns, fetch_impl, environment="development", collapse_id=""):
    return asyncio.run(apns.send_one(
        types.SimpleNamespace(), fetch_impl, "jwt", "ab" * 32, environment,
        _PING, collapse_id=collapse_id))


def test_send_one_reports_delivery_death_and_apples_reason(apns):
    assert _send(apns, _fetch(_Response(200))) == (True, False, 200, "")
    # Apple's word travels back so a silent failure has a name. A dead token
    # asks the caller to prune it; a throttle does not.
    assert _send(apns, _fetch(_Response(
        400, '{"reason":"BadDeviceToken"}'))) == (
            False, True, 400, "BadDeviceToken")
    assert _send(apns, _fetch(_Response(
        410, '{"reason":"Unregistered"}'))) == (
            False, True, 410, "Unregistered")
    assert _send(apns, _fetch(_Response(
        429, '{"reason":"TooManyRequests"}'))) == (
            False, False, 429, "TooManyRequests")
    # A body that is not JSON must not turn into an exception on this path.
    assert _send(apns, _fetch(_Response(503, "<html>"))) == (
        False, False, 503, "")


def test_send_one_never_raises_when_the_gateway_is_unreachable(apns):
    # Status 0 is the "never reached Apple" marker the self-test shows; the
    # write that triggered a real push must not see this failure at all.
    assert _send(apns, _fetch(RuntimeError("connect timeout"))) == (
        False, False, 0, "")


def test_send_one_addresses_the_tokens_gateway_with_the_events_alert(apns):
    fetch_impl = _fetch(_Response(200))
    _send(apns, fetch_impl, environment="development", collapse_id="pings")
    (url, init, timeout), = fetch_impl.calls
    # A sandbox-minted token is only valid against the sandbox host.
    assert url == apns.APNS_HOST_SANDBOX + "/3/device/" + "ab" * 32
    assert timeout == apns.APNS_FETCH_TIMEOUT_SECONDS
    assert init["method"] == "POST"
    assert init["body"] == apns.alert_payload(_PING)
    headers = init["headers"]
    assert headers["authorization"] == "bearer jwt"
    assert headers["apns-topic"] == apns.APNS_BUNDLE_ID
    assert headers["apns-push-type"] == "alert"
    assert headers["apns-priority"] == "10"
    assert headers["apns-collapse-id"] == "pings"
    assert int(headers["apns-expiration"]) > 0

    fetch_impl = _fetch(_Response(200))
    _send(apns, fetch_impl, environment="production")
    assert fetch_impl.calls[0][0].startswith(apns.APNS_HOST_PRODUCTION)
    assert "apns-collapse-id" not in fetch_impl.calls[0][1]["headers"]


def test_send_one_drops_the_provider_token_apple_says_is_expired(apns):
    apns._PROVIDER_TOKEN.update({"jwt": "stale", "expires": 4_000_000_000})
    _send(apns, _fetch(_Response(403, '{"reason":"ExpiredProviderToken"}')))
    # Otherwise every later push in this isolate repeats the same rejection
    # until the 40 minute cache window happens to roll over.
    assert not apns._PROVIDER_TOKEN


# --- the phone-facing routes -------------------------------------------------

def _entry_function(name):
    text = (SRC / "entry.py").read_text()
    for node in ast.parse(text).body:
        if (isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
                and node.name == name):
            return ast.get_source_segment(text, node)
    raise AssertionError("no such top-level function: " + name)


def test_token_lookup_normalises_the_owner_like_registration_does():
    # register_apns_device_token lowercases the account before blind-indexing
    # it; a lookup that did not would read an empty set for the same account
    # and the phone would never be woken, with nothing logged anywhere.
    source = _entry_function("_apns_account_tokens")
    assert 'owner = str(owner or "").strip().lower()' in source
    assert "if not valid_node_name(owner):" in source
    assert "await blind_index(env, owner)" in source


def test_registration_tells_the_phone_whether_the_relay_can_push():
    source = _entry_function("register_device_handler")
    # Without this the phone cannot distinguish "registered" from "registered
    # on a relay that has no APNs credentials and will never send anything".
    assert "problem = _apns_module().apns_config_problem(env)" in source
    assert '"pushConfigured": not problem' in source
    assert '"configProblem": problem' in source


def test_push_test_route_is_session_bound_rate_limited_and_fixed_text():
    runtime = (SRC / "app_runtime.py").read_text()
    assert '"/api/accounts/push-test"' in runtime
    assert "return await push_test_handler(self.env, request)" in runtime

    source = _entry_function("push_test_handler")
    # The target is whoever the session proves, never a name from the body.
    assert "await _authed_account_name(env, request, data)" in source
    assert '"unauthorized"}, status=401' in source
    assert 'data.get("account' not in source and 'data.get("node' not in source
    # One real buzz per account per window, since the round trips run inline.
    assert "APNS_PUSH_TEST_MIN_INTERVAL_MS" in source
    assert '"too_many_requests"' in source and "status=429" in source
    # The fixed test text: send_one is the only sender and nothing from the
    # request reaches the alert.
    assert 'apns.APNS_TEST_ALERT, collapse_id="push-test"' in source
    assert "alert_payload" not in source
    assert 'data.get("title' not in source and 'data.get("body' not in source
    # Every outcome the phone needs to explain itself is reported.
    for key in ('"configured"', '"devices"', '"delivered"', '"results"',
                '"apns_provider_key"', '"environment"', '"reason"'):
        assert key in source, key
    # A token Apple declares dead is pruned here too, not only on real events.
    assert "await _apns_prune_token(env, entry[\"token_bi\"])" in source


def test_real_event_fan_out_unpacks_the_reason_it_ignores():
    fan_out = _entry_function("push_account_apns")
    assert "_apns_send_tokens(env, tokens, topic, alert)" in fan_out
    # A ping with no record to describe is dropped, never sent generic.
    assert ('if topic == "pings" and not isinstance(push_alert, dict):'
            in fan_out)
    assert 'if not isinstance(alert, dict) or not alert.get("title"):' in (
        fan_out)
    source = _entry_function("_apns_send_tokens")
    assert "ok, dead, _status, _reason = await apns.send_one(" in source
    assert 'entry["token"], entry["environment"], alert,' in source
    assert ('collapse_id=apns.collapse_id_for(\n'
            '                    topic, alert.get("conversation", "")), '
            'topic=topic') in source


def test_a_ping_banner_is_rendered_from_the_row_just_written():
    source = _entry_function("_apns_ping_alert")
    assert "_push_alert_item(record, repos, {})" in source
    for key in ('"title"', '"subtitle"', '"body"', '"alertId"', '"href"'):
        assert key in source


# --- the handlers, run against fakes -----------------------------------------

def _entry_constant(name):
    import re
    text = (SRC / "entry.py").read_text()
    match = re.search(r"^" + re.escape(name) + r" = (.+)$", text, re.M)
    assert match, name
    return eval(match.group(1))  # noqa: S307 - a literal from our own source


class _Request:
    def __init__(self, method="POST", body=None):
        self.method = method
        self.body = {} if body is None else body


def _push_test_runtime(*, account="alice", configured=True, tokens=(),
                       answers=None, provider_ok=True, lookup_ok=True,
                       now=5_000_000):
    """push_test_handler with every collaborator faked and recorded."""
    responses = []
    sent = []
    bundles = []
    pruned = []
    clock = {"now": now}

    def json_response(payload, status=200, cache_control="",
                      extra_headers=None):
        responses.append((status, payload, extra_headers or {}))
        return responses[-1]

    async def bounded_json_request(request, max_bytes=0):
        if request.body is None:
            raise ValueError("empty body")
        return request.body

    async def _authed_account_name(env, request, data=None):
        return account

    async def provider_token(env):
        if not provider_ok:
            raise ValueError("bad key")
        return "jwt-1"

    async def send_one(env, fetch_impl, jwt, token, environment, alert,
                       collapse_id="", topic="", bundle_id=""):
        assert alert == {"title": "test"}
        sent.append((jwt, token, environment, collapse_id))
        bundles.append(bundle_id)
        return dict(answers or {}).get(token, (True, False, 200, ""))

    async def _apns_account_tokens(env, owner):
        if not lookup_ok:
            raise RuntimeError("d1 down")
        return [dict(t) for t in tokens]

    async def _apns_prune_token(env, token_bi):
        pruned.append(token_bi)

    namespace = {
        "method_name": lambda request: request.method,
        "json_response": json_response,
        "bounded_json_request": bounded_json_request,
        "safe_segment": lambda value: value or None,
        "_authed_account_name": _authed_account_name,
        "Date": SimpleNamespaceNow(clock),
        "_apns_module": lambda: types.SimpleNamespace(
            apns_configured=lambda env: configured,
            apns_config_problem=lambda env: (
                "" if configured else "missing_secrets"),
            provider_token=provider_token,
            APNS_TEST_ALERT={"title": "test"},
            apns_topic_for=lambda bundle_id: (
                bundle_id or "com.forkmesh.forkmesh"),
            send_one=send_one),
        "_apns_account_tokens": _apns_account_tokens,
        "_apns_prune_token": _apns_prune_token,
        "js_fetch_with_timeout": object(),
        "APNS_PUSH_TEST_MIN_INTERVAL_MS": _entry_constant(
            "APNS_PUSH_TEST_MIN_INTERVAL_MS"),
        "_APNS_PUSH_TEST_MEMO": {},
        "_APNS_PUSH_TEST_MEMO_MAX": _entry_constant("_APNS_PUSH_TEST_MEMO_MAX"),
    }
    exec(_entry_function("push_test_handler"), namespace)

    def call(request=None):
        responses.clear()
        asyncio.run(namespace["push_test_handler"](
            types.SimpleNamespace(), request or _Request()))
        return responses[-1]

    return types.SimpleNamespace(
        call=call, sent=sent, bundles=bundles, pruned=pruned, clock=clock,
        memo=namespace["_APNS_PUSH_TEST_MEMO"])


class SimpleNamespaceNow:
    """`Date.now()` reading a mutable clock, so a test can let time pass."""

    def __init__(self, clock):
        self._clock = clock

    def now(self):
        return self._clock["now"]


_TOKENS = (
    {"token_bi": "bi-1", "token": "aa" * 32, "environment": "development"},
    {"token_bi": "bi-2", "token": "bb" * 32, "environment": "production",
     "bundle_id": "com.forkmesh.nativeapp"},
)


def test_push_test_handler_rejects_other_methods_and_missing_sessions():
    runtime = _push_test_runtime()
    status, payload, headers = runtime.call(_Request(method="GET"))
    assert (status, payload["error"]) == (405, "method_not_allowed")
    assert headers["allow"] == "POST"

    runtime = _push_test_runtime(account="")
    status, payload, _ = runtime.call()
    assert (status, payload["error"]) == (401, "unauthorized")
    assert runtime.sent == []


def test_push_test_handler_tolerates_an_empty_body():
    # The session rides in the Authorization header; a bodiless POST from
    # the settings screen must not be a 400.
    runtime = _push_test_runtime(tokens=_TOKENS[:1])
    status, payload, _ = runtime.call(_Request(body=None))
    assert status == 200 and payload["delivered"] == 1


def test_push_test_handler_reports_missing_credentials_without_sending():
    runtime = _push_test_runtime(configured=False, tokens=_TOKENS)
    status, payload, _ = runtime.call()
    assert status == 200
    assert payload["ok"] is True
    assert payload["configured"] is False
    assert payload["configProblem"] == "missing_secrets"
    # The phone still learns it is registered; only the relay side is short.
    assert payload["devices"] == 2
    assert payload["delivered"] == 0 and payload["results"] == []
    assert runtime.sent == []


def test_push_test_handler_reports_an_unregistered_phone():
    runtime = _push_test_runtime(tokens=())
    status, payload, _ = runtime.call()
    assert status == 200
    assert payload["configured"] is True and payload["devices"] == 0
    assert runtime.sent == []


def test_push_test_handler_delivers_per_device_and_prunes_dead_tokens():
    runtime = _push_test_runtime(
        tokens=_TOKENS,
        answers={"bb" * 32: (False, True, 400, "BadDeviceToken")})
    status, payload, _ = runtime.call()
    assert status == 200
    assert payload["ok"] is True and payload["configured"] is True
    assert (payload["devices"], payload["delivered"]) == (2, 1)
    assert payload["results"] == [
        {"environment": "development", "topic": "com.forkmesh.forkmesh",
         "delivered": True, "status": 200, "reason": ""},
        {"environment": "production", "topic": "com.forkmesh.nativeapp",
         "delivered": False, "status": 400, "reason": "BadDeviceToken"},
    ]
    # Each device is sent as the app that registered it; a row from before
    # the field existed is the App Store app's.
    assert runtime.bundles == ["", "com.forkmesh.nativeapp"]
    # Each device is addressed with the minted provider token, its own
    # environment, and the self-test's collapse id, the way a real event is.
    assert runtime.sent == [
        ("jwt-1", "aa" * 32, "development", "push-test"),
        ("jwt-1", "bb" * 32, "production", "push-test"),
    ]
    # Apple declared the second token dead: gone now, not on the next event.
    assert runtime.pruned == ["bi-2"]


def test_push_test_handler_is_rate_limited_per_account():
    runtime = _push_test_runtime(tokens=_TOKENS[:1])
    assert runtime.call()[0] == 200
    status, payload, _ = runtime.call()
    assert (status, payload["error"]) == (429, "too_many_requests")
    assert 0 < payload["retryAfterMs"] <= _entry_constant(
        "APNS_PUSH_TEST_MIN_INTERVAL_MS")
    # Only one real buzz went out.
    assert len(runtime.sent) == 1

    runtime.clock["now"] += _entry_constant("APNS_PUSH_TEST_MIN_INTERVAL_MS")
    assert runtime.call()[0] == 200
    assert len(runtime.sent) == 2


def test_push_test_handler_names_a_key_that_will_not_sign():
    runtime = _push_test_runtime(tokens=_TOKENS[:1], provider_ok=False)
    status, payload, _ = runtime.call()
    assert status == 502
    assert payload["ok"] is False
    assert payload["error"] == "apns_provider_key"
    # Distinguishable from "not configured": the secrets are there.
    assert payload["configured"] is True and payload["devices"] == 1
    assert runtime.sent == []


def test_push_test_handler_names_a_failed_device_lookup():
    runtime = _push_test_runtime(lookup_ok=False)
    status, payload, _ = runtime.call()
    assert (status, payload["error"]) == (502, "device_lookup_failed")


def test_registration_reply_carries_the_relays_push_readiness():
    responses = []
    registered = []

    def json_response(payload, status=200, cache_control="",
                      extra_headers=None):
        responses.append((status, payload))

    async def bounded_json_request(request, max_bytes=0):
        return request.body

    async def _authed_account_name(env, request, data=None):
        return "alice"

    async def register_apns_device_token(env, account, token, environment,
                                         device_bi="", bundle_id="",
                                         voip_token=""):
        registered.append((account, token, environment, device_bi, bundle_id))
        return True

    async def unregister_apns_device_token(env, account, token):
        raise AssertionError("not a DELETE")

    namespace = {
        "method_name": lambda request: request.method,
        "json_response": json_response,
        "bounded_json_request": bounded_json_request,
        "safe_segment": lambda value: value or None,
        "_authed_account_name": _authed_account_name,
        "clean_string": lambda value, n=240: str(value or "")[:n],
        "register_apns_device_token": register_apns_device_token,
        "unregister_apns_device_token": unregister_apns_device_token,
        "_apns_module": lambda: types.SimpleNamespace(
            apns_config_problem=lambda env: "key_id_malformed"),
    }
    exec(_entry_function("register_device_handler"), namespace)
    asyncio.run(namespace["register_device_handler"](
        types.SimpleNamespace(),
        _Request(body={"deviceToken": "cd" * 32, "environment": "development"})))
    assert registered == [("alice", "cd" * 32, "development", "", "")]
    # Registered, and told in the same breath that nothing will be sent.
    assert responses == [
        (200, {"ok": True, "registered": True, "pushConfigured": False,
               "configProblem": "key_id_malformed"})]


def test_token_lookup_lowercases_and_validates_before_touching_d1():
    import re
    queries = []
    indexed = []

    async def d1_all(env, sql, *args):
        queries.append((sql, args))
        return [{"token_bi": "bi", "environment": "development",
                 "bundle_id": "com.forkmesh.nativeapp", "data": "sealed"}]

    async def blind_index(env, value):
        indexed.append(value)
        return "bi:" + value

    async def decrypt_row(env, data):
        return {"token": "ab" * 32}

    namespace = {
        "_apns_module": lambda: types.SimpleNamespace(
            APNS_MAX_DEVICES_PER_PUSH=8),
        "d1_all": d1_all,
        "blind_index": blind_index,
        "decrypt_row": decrypt_row,
        "clean_string": lambda value, n=240: str(value or "")[:n],
        "valid_node_name": lambda v: bool(re.match(r"^[a-z][a-z0-9-]*$", v)),
    }
    exec(_entry_function("_apns_tokens_from_rows"), namespace)
    exec(_entry_function("_apns_account_tokens"), namespace)
    lookup = namespace["_apns_account_tokens"]

    rows = asyncio.run(lookup(types.SimpleNamespace(), " Alice "))
    # The same blind index registration wrote, whatever case the event
    # carried the owner in.
    assert indexed == ["alice"]
    assert queries[0][1] == ("bi:alice", 8)
    assert rows == [{"token_bi": "bi", "account_bi": "", "token": "ab" * 32,
                     "voip_token": "", "environment": "development",
                     "bundle_id": "com.forkmesh.nativeapp"}]
    # The lookup reads the stored app id alongside the token.
    assert "bundle_id" in queries[0][0]

    # Garbage never reaches D1.
    assert asyncio.run(lookup(types.SimpleNamespace(), "../x")) == []
    assert asyncio.run(lookup(types.SimpleNamespace(), "")) == []
    assert len(queries) == 1


# --- which events reach a phone ----------------------------------------------

def _account_event_runtime(now=9_000_000):
    """notify_account_event with the socket and the APNs fan-out recorded."""
    clock = {"now": now}
    frames = []
    pushes = []

    class _NodeObject:
        async def fetch(self, url):
            frames.append(url.rsplit("topic=", 1)[1])

    async def push_account_apns(env, owner, topic, push_alert=None):
        pushes.append((owner, topic))

    namespace = {
        "safe_segment": lambda value: str(value or "").strip().lower(),
        "clean_string": lambda value, n=240: str(value or "")[:n],
        "Date": SimpleNamespaceNow(clock),
        "quote": lambda value: value,
        "_node_events_do_name": lambda owner: "events:" + owner,
        "_cron_subrequest_charge": lambda: None,
        "push_account_apns": push_account_apns,
        "_ACCOUNT_EVENT_PUSH_MEMO": {},
        "re": __import__("re"),
    }
    for name in ("ACCOUNT_EVENT_PUSH_COALESCE_MS",
                 "ACCOUNT_EVENT_PUSH_MEMO_MAX",
                 "ACCOUNT_EVENT_PUSH_EVERY_TOPICS",
                 "ACCOUNT_EVENT_SOCKET_EVERY_TOPICS",
                 "ACCOUNT_EVENT_CONVERSATION_TOPICS"):
        namespace[name] = _entry_constant(name)
    exec(_entry_function("_account_event_query"), namespace)
    exec(_entry_function("notify_account_event"), namespace)
    env = types.SimpleNamespace(FORKMESH_NODES=types.SimpleNamespace(
        idFromName=lambda name: name, get=lambda node_id: _NodeObject()))

    def notify(owner, topic, push_alert=None):
        asyncio.run(namespace["notify_account_event"](
            env, owner, topic, push_alert))

    return types.SimpleNamespace(
        notify=notify, frames=frames, pushes=pushes, clock=clock)


def test_every_ping_reaches_the_phone_even_inside_the_socket_window():
    # The socket frame only means "re-read your inbox", so one frame answers
    # any number of rows. A banner is the notification itself: a second ping
    # inside the window used to produce none, and the phone never heard of it.
    runtime = _account_event_runtime()
    runtime.notify("alice", "pings")
    runtime.notify("alice", "pings")
    runtime.notify("alice", "pings")
    assert runtime.frames == ["pings"]
    assert runtime.pushes == [("alice", "pings")] * 3


def test_other_topics_still_share_the_socket_coalescing_window():
    runtime = _account_event_runtime()
    runtime.notify("alice", "issues")
    runtime.notify("alice", "issues")
    assert runtime.frames == ["issues"]
    assert runtime.pushes == [("alice", "issues")]
    runtime.clock["now"] += _entry_constant("ACCOUNT_EVENT_PUSH_COALESCE_MS")
    runtime.notify("alice", "issues")
    assert runtime.pushes[-1] == ("alice", "issues")
    assert len(runtime.pushes) == 2


def test_every_direct_message_rings_the_socket_and_the_phone():
    # Each message is its own notification, so neither the push nor the
    # socket frame Android reads may be coalesced away.
    runtime = _account_event_runtime()
    first = {"title": "t", "conversation": "direct:" + "a" * 32,
             "ref": "b" * 40, "keyVersion": 2}
    second = dict(first, ref="c" * 40)
    runtime.notify("alice", "direct-messages", first)
    runtime.notify("alice", "direct-messages", second)
    assert runtime.pushes == [("alice", "direct-messages")] * 2
    assert runtime.frames == [
        "direct-messages&conversation=direct:" + "a" * 32
        + "&ref=" + "b" * 40 + "&keyVersion=2",
        "direct-messages&conversation=direct:" + "a" * 32
        + "&ref=" + "c" * 40 + "&keyVersion=2",
    ]


def test_the_direct_message_socket_frame_carries_only_valid_handles():
    runtime = _account_event_runtime()
    runtime.notify("alice", "direct-messages", {"title": "t"})
    runtime.notify("alice", "direct-messages", {
        "title": "t", "conversation": "direct:../x", "ref": "b" * 40})
    runtime.notify("alice", "direct-messages", {
        "title": "t", "conversation": "direct:" + "a" * 32,
        "ref": "nothex", "keyVersion": True})
    runtime.notify("alice", "pings", {
        "title": "t", "conversation": "direct:" + "a" * 32})
    assert runtime.frames == [
        "direct-messages", "direct-messages",
        "direct-messages&conversation=direct:" + "a" * 32, "pings"]


def test_the_event_socket_relays_the_direct_message_handles():
    import re
    from urllib.parse import parse_qs
    namespace = {"re": re}
    exec(_entry_function("_account_event_direct_fields"), namespace)
    fields = namespace["_account_event_direct_fields"]
    assert fields(parse_qs(
        "conversation=direct:" + "a" * 32 + "&ref=" + "b" * 40
        + "&keyVersion=4")) == {
            "conversation": "direct:" + "a" * 32, "ref": "b" * 40,
            "keyVersion": 4}
    assert fields(parse_qs("ref=" + "b" * 40)) == {}
    assert fields(parse_qs(
        "conversation=direct:" + "a" * 32 + "&ref=zz&keyVersion=0")) == {
            "conversation": "direct:" + "a" * 32}
    text = (SRC / "entry.py").read_text()
    notify = text[text.index('            if path.endswith("/notify"):'):]
    notify = notify[:notify.index("frame = json.dumps(frame_data)")]
    assert (
        'if frame_data["topic"] in ACCOUNT_EVENT_CONVERSATION_TOPICS:'
        in notify)
    assert "_account_event_direct_fields(query)" in notify


def test_a_second_call_inside_the_window_still_rings_the_phone():
    runtime = _account_event_runtime()
    first = {"title": "t", "conversation": "direct:" + "a" * 32}
    runtime.notify("alice", "calls", first)
    runtime.notify("alice", "calls", first)
    assert runtime.pushes == [("alice", "calls")] * 2
    assert runtime.frames == [
        "calls&conversation=direct:" + "a" * 32] * 2


def test_every_chat_read_rings_the_socket_and_never_the_phone():
    runtime = _account_event_runtime()
    runtime.notify("alice", "chat-read")
    runtime.notify("alice", "chat-read")
    assert runtime.frames == ["chat-read", "chat-read"]
    assert runtime.pushes == [("alice", "chat-read")] * 2


_DEVICES = (
    {"token_bi": "t-alice", "account_bi": "bi-alice", "token": "aa" * 32,
     "environment": "production"},
    {"token_bi": "t-bob", "account_bi": "bi-bob", "token": "bb" * 32,
     "environment": "production"},
    {"token_bi": "t-carol", "account_bi": "bi-carol", "token": "cc" * 32,
     "environment": "development"},
)


def _chat_push_runtime(*, configured=True, channel=None, members=(),
                       now=9_000_000):
    """push_chat_room_apns over a fixed device table and one channel row."""
    clock = {"now": now}
    sent = []
    reads = []
    titles = []
    conversations = []

    async def d1_first(env, sql, *args):
        reads.append(sql)
        return channel

    async def d1_all(env, sql, *args):
        reads.append(sql)
        if "chat_channel_members" in sql:
            return [{"member_bi": value} for value in members]
        return [dict(device, data="sealed:" + device["token"])
                for device in _DEVICES]

    async def decrypt_row(env, data):
        if str(data).startswith("sealed:"):
            return {"token": str(data)[len("sealed:"):]}
        return data

    alerts = []

    async def _apns_send_tokens(env, tokens, topic, alert):
        sent.append((sorted(t["token_bi"] for t in tokens), topic))
        titles.append(alert["title"])
        conversations.append(alert.get("conversation", ""))
        alerts.append(alert)
        return len(tokens)

    namespace = {
        "_apns_module": lambda: types.SimpleNamespace(
            apns_configured=lambda env: configured,
            APNS_CHAT_TOPIC="chat", APNS_MAX_DEVICES_PER_BROADCAST=32,
            chat_message_alert=lambda conversation="", **fetch: {
                "title": "New message",
                "conversation": conversation, "fetch": fetch}),
        "PUBLIC_WORLD_GENERAL_ROOM_KEY": (
            "repo:mainnode/forkmesh:room:world-general"),
        "Date": SimpleNamespaceNow(clock),
        "d1_first": d1_first,
        "d1_all": d1_all,
        "decrypt_row": decrypt_row,
        "clean_string": lambda value, n=240: str(value or "")[:n],
        "_apns_send_tokens": _apns_send_tokens,
        "_APNS_BROADCAST_MEMO": {},
        "_CHAT_ROOM_PUSH_MEMO": {},
    }
    for name in ("APNS_BROADCAST_MEMO_MS", "CHAT_ROOM_PUSH_COALESCE_MS",
                 "CHAT_ROOM_PUSH_MEMO_MAX"):
        namespace[name] = _entry_constant(name)
    for name in ("_apns_tokens_from_rows", "_apns_broadcast_tokens",
                 "_chat_channel_push_accounts", "push_chat_room_apns"):
        exec(_entry_function(name), namespace)

    def push(room_key, **kwargs):
        return asyncio.run(namespace["push_chat_room_apns"](
            types.SimpleNamespace(), room_key, **kwargs))

    return types.SimpleNamespace(
        push=push, sent=sent, reads=reads, clock=clock, titles=titles,
        conversations=conversations, alerts=alerts)


def test_a_public_room_message_wakes_every_registered_phone():
    # The mobile app's chat is the public world room, whose socket is
    # anonymous: there is no member list and no sender to leave out.
    runtime = _chat_push_runtime()
    assert runtime.push("repo:mainnode/forkmesh:room:world-general") == 3
    assert runtime.sent == [(["t-alice", "t-bob", "t-carol"], "chat")]
    assert runtime.titles == ["New message"]
    assert runtime.conversations == [""]


def test_a_private_channel_wakes_its_members_and_never_the_sender():
    runtime = _chat_push_runtime(
        channel={"data": {"visibility": "private", "name": "design"},
                 "created_by_bi": "bi-alice"},
        members=("bi-bob",))
    runtime.push("chat-channel:c1:v1", channel_id="c1", sender_bi="bi-bob")
    # Carol is not in the channel; Bob sent it.
    assert runtime.sent == [(["t-alice"], "chat")]
    assert runtime.titles == ["New message"]
    assert runtime.conversations == [""]
    assert runtime.alerts[0]["fetch"] == {
        "channel_id": "c1", "ref": "", "key_version": 0}


def test_a_channel_push_hands_the_phone_the_ref_to_fetch():
    runtime = _chat_push_runtime(
        channel={"data": {"visibility": "private", "name": "design"},
                 "created_by_bi": "bi-alice"},
        members=("bi-bob",))
    runtime.push("chat-channel:c1:v3", channel_id="c1", sender_bi="bi-bob",
                 ref="d" * 40, key_version=3)
    assert runtime.alerts[0]["fetch"] == {
        "channel_id": "c1", "ref": "d" * 40, "key_version": 3}


def test_a_public_channel_wakes_everyone_but_the_sender():
    runtime = _chat_push_runtime(
        channel={"data": {"visibility": "public"},
                 "created_by_bi": "bi-alice"})
    runtime.push("chat-channel:c2:v1", channel_id="c2", sender_bi="bi-alice")
    assert runtime.sent == [(["t-bob", "t-carol"], "chat")]


def test_a_channel_that_cannot_be_read_wakes_nobody():
    runtime = _chat_push_runtime(channel=None)
    assert runtime.push("chat-channel:gone:v1", channel_id="gone") == 0
    assert runtime.sent == []


def test_chat_pushes_coalesce_per_room_and_reuse_the_device_table():
    runtime = _chat_push_runtime(
        channel={"data": {"visibility": "public"},
                 "created_by_bi": "bi-alice"})
    world = "repo:mainnode/forkmesh:room:world-general"
    runtime.push(world)
    runtime.push(world)
    runtime.push("chat-channel:c2:v1", channel_id="c2")
    # One buzz per room per window, and one device-table read for all of it.
    assert [topic for _, topic in runtime.sent] == ["chat", "chat"]
    assert sum("apns_device_tokens" in sql for sql in runtime.reads) == 1
    runtime.clock["now"] += _entry_constant("CHAT_ROOM_PUSH_COALESCE_MS")
    runtime.push(world)
    assert len(runtime.sent) == 3


def test_a_legacy_room_wakes_nobody():
    runtime = _chat_push_runtime()
    assert runtime.push("legacy:room:anything") == 0
    assert runtime.sent == [] and runtime.reads == []


def test_a_repository_room_other_than_the_world_room_wakes_nobody():
    runtime = _chat_push_runtime()
    for room in ("repo:someone/project:room:general",
                 "repo:mainnode/forkmesh:room:general",
                 "repo:mainnode/forkmesh:room:world-general-2"):
        assert runtime.push(room) == 0
    assert runtime.sent == [] and runtime.reads == []


def test_chat_pushes_are_a_no_op_without_usable_credentials():
    runtime = _chat_push_runtime(configured=False)
    assert runtime.push("room-a") == 0
    assert runtime.sent == [] and runtime.reads == []


def test_the_relay_only_wakes_phones_for_frames_the_sender_marked():
    # The relay sees ciphertext only, and clients also retain edits, reactions
    # and a key advertisement on every connect. Waking on "persist" alone
    # would buzz every phone each time a desktop came online.
    text = (SRC / "entry.py").read_text()
    retain = text[text.index("    async def _maybe_retain(self, ws, message"):]
    retain = retain[:retain.index("    async def _private_room_current")]
    assert 'elif not direct_id and envelope.get("notify") is True:' in retain
    assert "await push_chat_room_apns(" in retain
    assert "sender_bi=account_bi, ref=msg_id[:40]," in retain
    assert '_ws_attr(ws, "channel_version", 0)' in retain
    assert "ref=msg_id[:40],\n" in retain
    assert '_ws_attr(ws, "direct_version", 0)' in retain

    public = ROOT / "public"
    transport = (public / "chat-room-transport.js").read_text()
    assert 'const NOTIFY_TYPES = new Set(["chat", "thread-reply"]);' in transport
    assert "if (persist && NOTIFY_TYPES.has(plain && plain.type))" in transport
    dashboard = (public / "dashboard-chat.js").read_text()
    assert "if (NOTIFY_TYPES.has(plain && plain.type)) envelope.notify = true;" in dashboard
    # Key advertisements are retained and must stay unmarked.
    for source in (transport, dashboard):
        advert = source[source.index("KeyFrame("):][:400]
        assert "notify" not in advert


# --- the apns-topic is the app that minted the token -------------------------
# Apple answers a token sent under another app's topic with 400
# DeviceTokenNotForTopic, which the fan-out prunes as dead. Every push used to
# go out as the App Store app, so tokens minted under the development id were
# deleted on their first push.

NATIVE = "com.forkmesh.nativeapp"
STORE = "com.forkmesh.forkmesh"


def _send_as(apns, bundle_id, topic=""):
    fetch_impl = _fetch(_Response(200))
    asyncio.run(apns.send_one(
        types.SimpleNamespace(), fetch_impl, "jwt", "ab" * 32, "production",
        _PING, topic=topic, bundle_id=bundle_id))
    return fetch_impl.calls[0][1]


def test_each_token_is_sent_under_the_app_that_registered_it(apns):
    assert apns.APNS_BUNDLE_ID == STORE
    assert apns.APNS_BUNDLE_IDS == frozenset({STORE, NATIVE})
    assert _send_as(apns, NATIVE)["headers"]["apns-topic"] == NATIVE
    assert _send_as(apns, STORE)["headers"]["apns-topic"] == STORE
    # A row registered before the field existed is an App Store install.
    assert _send_as(apns, "")["headers"]["apns-topic"] == STORE


def test_the_native_app_on_the_store_id_is_sent_as_the_app_store_app(apns):
    assert apns.normalize_bundle_id(STORE) == STORE
    assert apns.normalize_bundle_id(STORE + ".NotificationService") == STORE
    assert _send_as(apns, STORE)["headers"]["apns-topic"] == STORE
    assert _send_as(apns, "")["headers"]["apns-topic"] == _send_as(apns, STORE)["headers"]["apns-topic"]


def test_only_our_own_apps_can_be_named_as_a_topic(apns):
    for value in ("com.example.other", "com.forkmesh", "*", "COM.FORKMESH.NATIVEAPP",
                  "com.forkmesh.nativeapp.evil", None, 7):
        assert apns.normalize_bundle_id(value) == ""
        assert apns.apns_topic_for(value) == STORE
    assert apns.normalize_bundle_id(" com.forkmesh.nativeapp ") == NATIVE


def test_the_notification_extension_is_never_the_topic(apns):
    # Apple delivers to the containing app and iOS runs the extension, so an
    # extension id reported by a client means its app.
    assert apns.normalize_bundle_id(NATIVE + ".NotificationService") == NATIVE
    assert apns.normalize_bundle_id(STORE + ".NotificationService") == STORE
    assert apns.apns_topic_for(NATIVE + ".NotificationService") == NATIVE
    # Every alert, whichever app it goes to, wakes that app's extension to add
    # the portrait and badge: mutable-content is not per app.
    init = _send_as(apns, NATIVE, topic="code-push")
    assert init["headers"]["apns-topic"] == NATIVE
    payload = json.loads(init["body"])
    assert payload["aps"]["mutable-content"] == 1
    # A code push is marked so the extension and the app tell it from an
    # inbox ping, and it names the row the extension enriches and the tap
    # opens.
    assert payload["topic"] == "code-push"
    assert payload["alertId"] == "row-1"


def test_a_dead_topic_answer_still_prunes_only_that_token(apns):
    # DeviceTokenNotForTopic stays a dead-token reason: with the per-token
    # topic it only fires for a token that truly belongs to no app of ours.
    assert "DeviceTokenNotForTopic" in apns.APNS_DEAD_REASONS


def _register_runtime(apns):
    runs = []

    async def blind_index(env, value):
        return "bi:" + value

    async def encrypt_row(env, record):
        voip = record.get("voipToken")
        return "sealed:" + record["token"] + ("|" + voip if voip else "")

    async def ensure_schema(env):
        return None

    async def d1_run(env, sql, *args):
        runs.append((sql, args))

    import re
    namespace = {
        "_apns_module": lambda: types.SimpleNamespace(
            APNS_TOKEN_STALE_MS=1, APNS_MAX_TOKENS_PER_ACCOUNT=20,
            normalize_bundle_id=apns.normalize_bundle_id),
        "clean_string": lambda value, n=240: str(value or "")[:n],
        "MAX_NODE_NAME": 64,
        "valid_node_name": lambda v: bool(re.match(r"^[a-z][a-z0-9-]*$", v)),
        "Date": types.SimpleNamespace(now=lambda: 1000),
        "blind_index": blind_index,
        "encrypt_row": encrypt_row,
        "ensure_schema": ensure_schema,
        "d1_run": d1_run,
    }
    exec(_entry_function("register_apns_device_token"), namespace)
    return namespace["register_apns_device_token"], runs


def test_registration_stores_the_allowlisted_app_per_token(apns):
    register, runs = _register_runtime(apns)
    for bundle_id, stored in ((NATIVE, NATIVE),
                              (NATIVE + ".NotificationService", NATIVE),
                              ("", ""), ("com.example.other", "")):
        runs.clear()
        assert asyncio.run(register(
            types.SimpleNamespace(), "alice", "cd" * 32, "production",
            bundle_id=bundle_id))
        sql, args = runs[0]
        assert "bundle_id" in sql
        # Re-registering the same token under another app moves its topic.
        assert "bundle_id=excluded.bundle_id" in sql
        assert args[4] == stored
    # Old callers (no bundle id at all) still register, as the App Store app.
    runs.clear()
    assert asyncio.run(register(
        types.SimpleNamespace(), "alice", "cd" * 32, "production"))
    assert runs[0][1][4] == ""


def test_registration_keeps_the_voip_token_in_the_encrypted_row(apns):
    register, runs = _register_runtime(apns)
    assert asyncio.run(register(
        types.SimpleNamespace(), "alice", "cd" * 32, "production",
        voip_token="EF" * 32))
    assert runs[0][1][5] == "sealed:" + "cd" * 32 + "|" + "ef" * 32
    for bad in ("", "zz" * 32, "ef"):
        runs.clear()
        assert asyncio.run(register(
            types.SimpleNamespace(), "alice", "cd" * 32, "production",
            voip_token=bad))
        assert runs[0][1][5] == "sealed:" + "cd" * 32


def test_the_registration_route_forwards_the_apps_bundle_id():
    registered = []
    voip = []

    async def register_apns_device_token(env, account, token, environment,
                                         device_bi="", bundle_id="",
                                         voip_token=""):
        registered.append(bundle_id)
        voip.append(voip_token)
        return True

    async def _authed(env, request, data=None):
        return "alice"

    async def bounded_json_request(request, max_bytes=0):
        return request.body

    namespace = {
        "method_name": lambda request: request.method,
        "json_response": lambda payload, status=200, cache_control="",
        extra_headers=None: (status, payload),
        "bounded_json_request": bounded_json_request,
        "safe_segment": lambda value: value or None,
        "_authed_account_name": _authed,
        "clean_string": lambda value, n=240: str(value or "")[:n],
        "register_apns_device_token": register_apns_device_token,
        "unregister_apns_device_token": None,
        "_apns_module": lambda: types.SimpleNamespace(
            apns_config_problem=lambda env: ""),
    }
    exec(_entry_function("register_device_handler"), namespace)
    asyncio.run(namespace["register_device_handler"](
        types.SimpleNamespace(),
        _Request(body={"deviceToken": "cd" * 32, "bundleId": NATIVE})))
    assert registered == [NATIVE]
    asyncio.run(namespace["register_device_handler"](
        types.SimpleNamespace(),
        _Request(body={"deviceToken": "cd" * 32, "voipToken": "ef" * 32})))
    assert voip == ["", "ef" * 32]


_CALL_ID = "0b6c1d2e-3f40-4a5b-8c6d-7e8f90a1b2c3"


def test_a_voip_call_push_names_the_call_for_callkit(apns):
    alert = apns.direct_call_alert(
        conversation_id="a" * 32, caller="bob", call_id=_CALL_ID.upper())
    assert alert["callId"] == _CALL_ID
    payload = json.loads(apns.voip_payload(alert, "https://app.example"))
    assert payload == {
        "topic": "calls", "conversation": "direct:" + "a" * 32,
        "caller": "bob", "callId": _CALL_ID, "origin": "https://app.example"}
    assert apns.voip_payload({"callId": _CALL_ID}) == ""
    assert "callId" not in apns.direct_call_alert(
        conversation_id="a" * 32, call_id="not-a-uuid")


def test_a_voip_push_goes_to_the_voip_topic_now_or_never(apns):
    fetch_impl = _fetch(_Response(200))
    result = asyncio.run(apns.send_voip(
        types.SimpleNamespace(), fetch_impl, "jwt", "ab" * 32, "production",
        apns.direct_call_alert(conversation_id="a" * 32, call_id=_CALL_ID),
        bundle_id=NATIVE))
    assert result == (True, False, 200, "")
    url, request = fetch_impl.calls[0][0], fetch_impl.calls[0][1]
    assert url == apns.APNS_HOST_PRODUCTION + "/3/device/" + "ab" * 32
    headers = request["headers"]
    assert headers["apns-topic"] == NATIVE + ".voip"
    assert headers["apns-push-type"] == "voip"
    assert headers["apns-priority"] == "10"
    assert headers["apns-expiration"] == "0"
    assert json.loads(request["body"])["callId"] == _CALL_ID


def _call_fan_out(voip_results):
    sent = []
    forgotten = []

    async def send_one(env, fetch_impl, jwt, token, environment, alert,
                       collapse_id="", topic="", bundle_id=""):
        sent.append(("alert", token, topic, collapse_id))
        return True, False, 200, ""

    async def send_voip(env, fetch_impl, jwt, token, environment, alert,
                        bundle_id=""):
        sent.append(("voip", token, alert.get("callId", "")))
        return voip_results.pop(0) if voip_results else (
            True, False, 200, "")

    async def provider_token(env):
        return "jwt"

    async def forget(env, entry):
        forgotten.append(entry["token_bi"])

    namespace = {
        "_apns_module": lambda: types.SimpleNamespace(
            provider_token=provider_token, send_one=send_one,
            send_voip=send_voip, APNS_PUSH_BUDGET_MS=3000,
            APNS_CALL_TOPIC="calls",
            collapse_id_for=lambda topic, conversation="": "call-" + conversation),
        "Date": types.SimpleNamespace(now=lambda: 0),
        "js_fetch_with_timeout": object(),
        "_apns_prune_token": None,
        "_apns_forget_voip_token": forget,
        "_APNS_BROADCAST_MEMO": {},
        "_APNS_VOIP_CALL_MEMO": {},
        "APNS_VOIP_CALL_MEMO_MS": 120_000,
        "APNS_VOIP_CALL_MEMO_MAX": 256,
    }
    exec(_entry_function("_apns_send_call"), namespace)

    def ring(tokens, alert):
        return asyncio.run(namespace["_apns_send_call"](
            types.SimpleNamespace(), tokens, alert))

    return ring, sent, forgotten


def test_call_rings_use_voip_where_the_phone_has_a_token():
    ring, sent, _forgotten = _call_fan_out([])
    tokens = [
        {"token_bi": "a", "token": "aa", "voip_token": "va",
         "environment": "production"},
        {"token_bi": "b", "token": "bb", "voip_token": "",
         "environment": "production"},
    ]
    alert = {"title": "t", "conversation": "direct:" + "c" * 32,
             "callId": _CALL_ID}
    assert ring(tokens, alert) == 2
    assert sent == [
        ("voip", "va", _CALL_ID),
        ("alert", "bb", "calls", "call-direct:" + "c" * 32)]
    sent.clear()
    assert ring(tokens, alert) == 1
    assert sent == [("alert", "bb", "calls", "call-direct:" + "c" * 32)]
    sent.clear()
    ring(tokens, dict(alert, callId="1" + _CALL_ID[1:]))
    assert sent[0] == ("voip", "va", "1" + _CALL_ID[1:])


def test_a_failed_voip_push_falls_back_to_the_alert():
    ring, sent, forgotten = _call_fan_out([
        (False, True, 400, "BadDeviceToken"), (False, False, 0, "")])
    tokens = [{"token_bi": "a", "token": "aa", "voip_token": "va",
               "environment": "production"}]
    alert = {"title": "t", "conversation": "direct:" + "c" * 32,
             "callId": _CALL_ID}
    ring(tokens, alert)
    assert [entry[0] for entry in sent] == ["voip", "alert"]
    assert forgotten == ["a"]
    sent.clear()
    ring(tokens, dict(alert, callId="2" + _CALL_ID[1:]))
    assert [entry[0] for entry in sent] == ["voip", "alert"]
    assert forgotten == ["a"]


def test_the_fan_out_addresses_every_token_as_its_own_app():
    calls = []

    async def send_one(env, fetch_impl, jwt, token, environment, alert,
                       collapse_id="", topic="", bundle_id=""):
        calls.append((token, topic, bundle_id))
        return True, False, 200, ""

    async def provider_token(env):
        return "jwt"

    namespace = {
        "_apns_module": lambda: types.SimpleNamespace(
            provider_token=provider_token, send_one=send_one,
            APNS_PUSH_BUDGET_MS=3000,
            collapse_id_for=lambda topic, conversation="": ""),
        "Date": types.SimpleNamespace(now=lambda: 0),
        "js_fetch_with_timeout": object(),
        "_apns_prune_token": None,
        "_APNS_BROADCAST_MEMO": {},
    }
    exec(_entry_function("_apns_send_tokens"), namespace)
    tokens = [
        {"token_bi": "a", "token": "aa", "environment": "production",
         "bundle_id": NATIVE},
        {"token_bi": "b", "token": "bb", "environment": "production",
         "bundle_id": ""},
        # A token dict built before the field existed.
        {"token_bi": "c", "token": "cc", "environment": "production"},
    ]
    delivered = asyncio.run(namespace["_apns_send_tokens"](
        types.SimpleNamespace(), tokens, "code-push", {"title": "x"}))
    assert delivered == 3
    assert calls == [("aa", "code-push", NATIVE), ("bb", "code-push", ""),
                     ("cc", "code-push", "")]


def test_the_schema_and_migration_carry_the_bundle_id_column():
    schema_text = (SRC / "schema.py").read_text()
    assert "bundle_id TEXT NOT NULL DEFAULT ''" in schema_text
    migration = (ROOT / "migrations"
                 / "0192_apns_device_token_bundle_id.sql").read_text()
    assert ("ALTER TABLE apns_device_tokens ADD COLUMN bundle_id TEXT NOT NULL "
            "DEFAULT ''") in migration
    assert "ADD COLUMN bundle_id" in (SRC / "entry.py").read_text()
