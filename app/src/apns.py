"""Apple Push Notification service transport for account events.

This is the phone half of the payload-free account event channel. A desktop or
browser client holds a WebSocket to the per-owner ForkMeshNodes Durable Object
and answers a `{"type":"event","topic"}` frame with its own authenticated read
(see `notify_account_event` in entry.py). A phone cannot: iOS suspends the
process, so there is no socket left to receive the frame. APNs is the only
mechanism that reaches a device whose app is not running. Room chat has no
account event of its own and reaches phones through push_chat_room_apns in
entry.py, which shares this transport.

Every alert names its event in full. A ping carries the same headline, byline
and body the inbox row shows ("#42 New comment" / "acme/widgets \xb7 by bob" /
the comment text), rendered by _push_alert_item in entry.py from the row that
was just written; the push test says it is a test. A push with nothing
specific to say is not sent at all (see alert_payload). Chat and direct
messages are sealed end to end and unreadable to this Worker too, and their
alerts name neither the sender nor the channel: the title is "New message"
and the phone opens the sealed frame and fills in the sender and text itself
(docs/app/chat-protocol-v3.md, docs/app/chat-push-previews.md).

This deliberately hands Apple the text of pings. Every alert also sets
`mutable-content`, so the bundled notification service extension
(`native/ios/NotificationService`) can still add the actor's portrait and the
unread badge, read over the account's own session; the text no longer depends
on that read succeeding.

Device tokens are stored exactly like every other sensitive value here: the row
key is a blind index of the token and the token itself lives encrypted in
`data`, so the table discloses neither the account nor the device.
"""

import base64
import json
import re
import time

from js import Date
from js import Uint8Array
from js import crypto as js_crypto
from pyodide.ffi import to_js as _to_js

from push_icons import PUSH_CALL_ICON
from push_icons import PUSH_CHAT_ICON
from push_icons import PUSH_DEFAULT_ICON
from push_icons import PUSH_DIRECT_MESSAGE_ICON
from push_icons import PUSH_DIRECT_START_ICON
from push_icons import with_icon


# --- Alert copy -------------------------------------------------------------
# Every push says what happened; there is no generic fallback line. These are
# the only fixed strings, and each belongs to one kind of event.
APNS_TEST_ALERT = {
    "title": with_icon("ForkMesh test notification", PUSH_DEFAULT_ICON),
    "body": ("Push notifications reach this phone. Pings, direct messages "
             "and chat messages will arrive here with their details."),
}
# Chat and direct messages are end-to-end encrypted: neither this Worker nor
# Apple can read the text, so the body says so instead of pretending.
APNS_SEALED_TITLE = "New message"
APNS_SEALED_BODY = "New message"
APNS_DIRECT_STARTED_TITLE = "New direct conversation"
APNS_DIRECT_CONVERSATION_PREFIX = "direct:"
APNS_DIRECT_ID_RE = re.compile(r"[0-9a-f]{32}")
APNS_CHANNEL_ID_RE = re.compile(r"[0-9a-f]{32}")
APNS_REF_RE = re.compile(r"[0-9a-f]{40}")
APNS_KEY_VERSION_MAX = 1_000_000_000
APNS_FRAME_RELAY_KEYS = frozenset({
    "ref", "recipients", "recipientNames", "historyReplay"})
APNS_MESH_CONVERSATION = "#mesh"
APNS_MESH_COLLAPSE_ID = "mesh-room"
APNS_CONVERSATION_MAX_CHARS = 120
# APNs refuses a payload over 4 KB; stay under it with room for the envelope.
APNS_PAYLOAD_MAX_BYTES = 4000
APNS_ALARM_SOUNDS = {"power": "desktop_power_alarm.wav"}

# Account event topics worth waking a phone for. "catalog" is only a UI
# freshness hint that accompanies a "pings" event. "issues" is the same kind of
# hint: it fires for every comment or close on the owner's repositories —
# including the owner's own — and names no inbox row, so a banner for it would
# have nothing specific to say. Issue events the user should hear about
# raise a ping of their own, and that ping is what buzzes the phone, with the
# actual text. "chat" is a message retained in a channel or public room (see
# push_chat_room_apns in entry.py); it has no socket-side account event of its
# own.
APNS_CHAT_TOPIC = "chat"
# "code-push" is a ping (kind code_push, see notify_code_push in entry.py)
# pushed under its own topic so the phone can tell a push banner from an inbox
# one. It is a ping in every other respect: rendered from the row just written,
# no collapse id, every one its own banner (the Worker's storm guard, not
# Apple, keeps a busy repository from flooding the phone).
APNS_CODE_PUSH_TOPIC = "code-push"
APNS_CALL_TOPIC = "calls"
APNS_CALL_TITLE = "Incoming call"
APNS_CALL_SOUND = "forkmesh-call-ring.wav"
APNS_CALL_COLLAPSE_PREFIX = "call-"
APNS_CALL_EXPIRATION_SECONDS = 60
APNS_PUSH_TOPICS = ("pings", "direct-messages", APNS_CHAT_TOPIC,
                    APNS_CODE_PUSH_TOPIC, APNS_CALL_TOPIC)

APNS_CONVERSATION_TOPICS = ("direct-messages", APNS_CHAT_TOPIC,
                            APNS_CALL_TOPIC)
APNS_COLLAPSE_TOPICS = (APNS_CHAT_TOPIC,)
APNS_MARKED_TOPICS = APNS_CONVERSATION_TOPICS + (APNS_CODE_PUSH_TOPIC,)

# How long Apple holds an alert for a phone it cannot reach. Apple keeps only
# the newest alert per app for an offline device, so a long window costs one
# banner on reconnect, not a burst; a short one silently drops the night's
# only ping for a phone that was in flight mode.
APNS_EXPIRATION_SECONDS = 24 * 60 * 60

# The apns-topic is the bundle id of the app that minted the device token, and
# Apple answers any other topic with 400 DeviceTokenNotForTopic, which the
# fan-out treats as a dead token and prunes. Two app ids register here: the
# App Store app (com.forkmesh.forkmesh) and the native app's development id
# (com.forkmesh.nativeapp), so the topic is stored per token
# (apns_device_tokens.bundle_id, migration 0192). A row with no bundle id was
# registered by an older build that never sent one, and keeps the App Store id.
APNS_BUNDLE_ID = "com.forkmesh.forkmesh"
APNS_NATIVE_BUNDLE_ID = "com.forkmesh.nativeapp"
# Only our own apps: the topic is chosen by the phone, and a free-form value
# would let a registration aim the provider key at somebody else's app.
APNS_BUNDLE_IDS = frozenset({APNS_BUNDLE_ID, APNS_NATIVE_BUNDLE_ID})
# The notification service extension has its own bundle id
# (<app>.NotificationService) but never receives a push under it: Apple
# delivers to the containing app's topic and iOS runs the extension. A client
# that reports the extension's id (it is the process that knows the id most
# directly) is registering its containing app.
APNS_EXTENSION_SUFFIXES = (".NotificationService",)
APNS_HOST_PRODUCTION = "https://api.push.apple.com"
APNS_HOST_SANDBOX = "https://api.sandbox.push.apple.com"

# Apple rejects a provider token older than one hour and also rejects tokens
# refreshed too eagerly, so mint at most one per isolate per 40 minutes.
APNS_TOKEN_TTL_SECONDS = 40 * 60
APNS_FETCH_TIMEOUT_SECONDS = 5
# A phone that has not checked in for this long is almost certainly a stale
# install; APNs would answer 410 for it eventually anyway.
APNS_TOKEN_STALE_MS = 180 * 24 * 60 * 60 * 1000
APNS_MAX_TOKENS_PER_ACCOUNT = 20
# This Worker has no waitUntil (the no-concurrent-Pyodide-task rule), so the
# fan-out runs inline on the request that triggered it and must stay bounded.
# One person has one or two phones; these caps only bite on a pathological
# account and are far cheaper than wedging a write behind a slow gateway.
APNS_MAX_DEVICES_PER_PUSH = 8
APNS_PUSH_BUDGET_MS = 3000
# A public room has no membership rows, so its audience is every registered
# phone. Bounded so one chat frame stays inside a Durable Object invocation's
# subrequest allowance.
APNS_MAX_DEVICES_PER_BROADCAST = 32

# Per-isolate provider-token cache: {"jwt": str, "expires": epoch_seconds}.
_PROVIDER_TOKEN = {}
# Per-isolate imported signing key, keyed by the key id it was imported for.
_SIGNING_KEY = {}


def _to_js_bytes(data):
    # Copy into a JS-owned buffer. A bare _to_js(bytes) is a view into Python's
    # WASM memory and WebCrypto may read it after the GIL is released.
    return Uint8Array.new(_to_js(bytes(data)))


def _b64url(data):
    return base64.urlsafe_b64encode(bytes(data)).decode().rstrip("=")


def _apple_identifier(value):
    # Apple's Key IDs and Team IDs are both exactly ten characters of A-Z0-9.
    value = str(value or "").strip()
    return len(value) == 10 and all(
        c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789" for c in value)


def apns_config_problem(env):
    """Why this Worker cannot push, or "" when its credentials look usable.

    A Key ID or Team ID of the wrong shape can never work: Apple answers every
    single push 403 InvalidProviderToken, and nothing else in the chain says
    so. Naming it here lets the self-test and the settings card point at the
    secret instead of at the phone.
    """
    key = str(getattr(env, "APNS_AUTH_KEY", "") or "").strip()
    key_id = str(getattr(env, "APNS_KEY_ID", "") or "").strip()
    team_id = str(getattr(env, "APNS_TEAM_ID", "") or "").strip()
    if not (key and key_id and team_id):
        return "missing_secrets"
    if not _apple_identifier(key_id):
        return "key_id_malformed"
    if not _apple_identifier(team_id):
        return "team_id_malformed"
    return ""


def apns_configured(env):
    """True when all three provider secrets are present and well-formed."""
    return not apns_config_problem(env)


def _decode_p8(pem):
    """Raw PKCS#8 DER bytes from a .p8 private key, PEM armour stripped."""
    body = []
    for line in str(pem or "").replace("\\n", "\n").splitlines():
        line = line.strip()
        if not line or line.startswith("-----"):
            continue
        body.append(line)
    joined = "".join(body)
    if not joined:
        raise ValueError("empty APNS_AUTH_KEY")
    padded = joined + "=" * ((4 - len(joined) % 4) % 4)
    return base64.b64decode(padded)


async def _signing_key(env):
    key_id = str(getattr(env, "APNS_KEY_ID", "") or "").strip()
    cached = _SIGNING_KEY.get(key_id)
    if cached is not None:
        return cached
    der = _decode_p8(getattr(env, "APNS_AUTH_KEY", ""))
    key = await js_crypto.subtle.importKey(
        "pkcs8",
        _to_js_bytes(der),
        _to_js({"name": "ECDSA", "namedCurve": "P-256"},
               dict_converter=_dict_converter()),
        False,
        _to_js(["sign"]),
    )
    _SIGNING_KEY.clear()
    _SIGNING_KEY[key_id] = key
    return key


def _dict_converter():
    from js import Object
    return Object.fromEntries


def _js_obj(value):
    return _to_js(value, dict_converter=_dict_converter())


async def provider_token(env):
    """Cached ES256 provider JWT for APNs.

    Apple refuses a token older than an hour, so this is refreshed on a 40
    minute cadence. WebCrypto's ECDSA signature is already the raw r||s pair
    JWS ES256 wants, so no DER unwrapping is needed.
    """
    now = int(time.time())
    cached = _PROVIDER_TOKEN.get("jwt")
    if cached and int(_PROVIDER_TOKEN.get("expires", 0)) > now:
        return cached

    key_id = str(getattr(env, "APNS_KEY_ID", "") or "").strip()
    team_id = str(getattr(env, "APNS_TEAM_ID", "") or "").strip()
    header = _b64url(json.dumps(
        {"alg": "ES256", "kid": key_id}, separators=(",", ":")).encode())
    claims = _b64url(json.dumps(
        {"iss": team_id, "iat": now}, separators=(",", ":")).encode())
    signing_input = (header + "." + claims).encode()

    key = await _signing_key(env)
    signature = await js_crypto.subtle.sign(
        _js_obj({"name": "ECDSA", "hash": {"name": "SHA-256"}}),
        key,
        _to_js_bytes(signing_input),
    )
    jwt = (header + "." + claims + "."
           + _b64url(bytes(Uint8Array.new(signature).to_py())))
    _PROVIDER_TOKEN["jwt"] = jwt
    _PROVIDER_TOKEN["expires"] = now + APNS_TOKEN_TTL_SECONDS
    return jwt


def reset_provider_token():
    """Drop the cached JWT so the next send mints a fresh one."""
    _PROVIDER_TOKEN.clear()
    _SIGNING_KEY.clear()


def apns_host(environment):
    """Sandbox for development builds, production otherwise.

    A token minted by a development build is only valid against the sandbox
    gateway and vice versa; sending to the wrong one returns 400
    BadDeviceToken, which is why the environment travels with the token.
    """
    return (APNS_HOST_SANDBOX
            if str(environment or "").strip().lower() == "development"
            else APNS_HOST_PRODUCTION)


def _key_version(value):
    if isinstance(value, bool):
        return 0
    try:
        value = int(value or 0)
    except (TypeError, ValueError, OverflowError):
        return 0
    return value if 0 < value <= APNS_KEY_VERSION_MAX else 0


def _fetch_fields(alert, ref, key_version):
    ref = str(ref or "").strip().lower()
    if APNS_REF_RE.fullmatch(ref):
        alert["ref"] = ref
    key_version = _key_version(key_version)
    if key_version:
        alert["keyVersion"] = key_version
    return alert


def direct_message_alert(started=False, conversation_id="", ref="",
                         key_version=0, frame=""):
    """The banner for a direct message, which the relay cannot read."""
    if started:
        alert = {
            "title": with_icon(APNS_DIRECT_STARTED_TITLE,
                               PUSH_DIRECT_START_ICON),
            "body": "Open Chat in ForkMesh to reply.",
        }
    else:
        alert = {
            "title": with_icon(APNS_SEALED_TITLE, PUSH_DIRECT_MESSAGE_ICON),
            "body": APNS_SEALED_BODY,
        }
    conversation_id = str(conversation_id or "").strip().lower()
    if APNS_DIRECT_ID_RE.fullmatch(conversation_id):
        alert["conversation"] = APNS_DIRECT_CONVERSATION_PREFIX + conversation_id
        if not started:
            _fetch_fields(alert, ref, key_version)
            sealed_frame = _sealed_frame(frame)
            if sealed_frame and "ref" in alert:
                alert["frame"] = sealed_frame
    return alert


def direct_call_alert(conversation_id="", caller=""):
    caller = str(caller or "").strip().lower()
    alert = {
        "title": with_icon(APNS_CALL_TITLE, PUSH_CALL_ICON),
        "body": ("@" + caller + " is calling you" if caller
                 else "Someone is calling you in ForkMesh"),
    }
    conversation_id = str(conversation_id or "").strip().lower()
    if APNS_DIRECT_ID_RE.fullmatch(conversation_id):
        alert["conversation"] = APNS_DIRECT_CONVERSATION_PREFIX + conversation_id
    return alert


def _sealed_frame(frame):
    if isinstance(frame, dict):
        envelope = frame
    else:
        if len(str(frame or "")) > APNS_PAYLOAD_MAX_BYTES:
            return None
        try:
            envelope = json.loads(str(frame or ""))
        except (TypeError, ValueError):
            return None
    if not isinstance(envelope, dict) or envelope.get("kind") != "cipher":
        return None
    return {key: value for key, value in envelope.items()
            if key not in APNS_FRAME_RELAY_KEYS}


def chat_message_alert(conversation="", channel_id="", ref="",
                       key_version=0):
    """The banner for a channel or room message, which the relay cannot read."""
    alert = {
        "title": with_icon(APNS_SEALED_TITLE, PUSH_CHAT_ICON),
        "body": APNS_SEALED_BODY,
    }
    conversation = str(conversation or "").strip()
    if conversation:
        alert["conversation"] = conversation
    channel_id = str(channel_id or "").strip().lower()
    if APNS_CHANNEL_ID_RE.fullmatch(channel_id):
        alert["channelId"] = channel_id
        _fetch_fields(alert, ref, key_version)
    return alert


def apns_origin(env):
    """The app's public origin (env APP_ORIGIN), or "" when unset or unusable.

    Every payload carries it so a phone signed in to several ForkMesh
    projects (app.forkmesh.com, app.forkmesh.com, ...) routes a tap to
    the project that sent it. Only a bare http(s) origin is ever sent.
    """
    value = str(getattr(env, "APP_ORIGIN", "") or "").strip().rstrip("/")
    scheme, sep, rest = value.partition("://")
    if (not sep or scheme.lower() not in ("https", "http") or not rest
            or len(value) > 200
            or any(c in rest for c in "/?#@ \t\r\n")):
        return ""
    return value


def alert_payload(alert, topic="", origin=""):
    """The push body for one event, or "" when there is nothing to say.

    `alert` is {title, subtitle?, body?, alertId?, href?, threadId?}. The title
    is required: an alert without one would be exactly the ambiguous
    "something happened" banner this refuses to send, so the caller skips the
    push instead. alertId and href ride outside `aps`, where iOS hands them to
    the app as userInfo, so a tap lands on the record the banner names;
    `origin` (apns_origin) rides beside them so the tap lands in the right
    project.

    The two conversation topics are also marked outside `aps`: room chat must
    stay off the screen of the person who just typed it (a public room's
    socket is anonymous, so the relay cannot leave the sender's own phone out
    of the fan-out), and the extension must not go looking for inbox text for
    a message that is sealed end to end. They also carry the relay ref and key
    version of the stored ciphertext so the extension can fetch and decrypt
    it on the phone (docs/app/chat-push-previews.md).
    """
    alert = alert if isinstance(alert, dict) else {}
    title = str(alert.get("title") or "").strip()
    if not title:
        return ""
    body = str(alert.get("body") or "").strip()
    subtitle = str(alert.get("subtitle") or "").strip()
    payload = {
        "aps": {
            "alert": {"title": title},
            "sound": "default",
            "mutable-content": 1,
        }
    }
    if subtitle:
        payload["aps"]["alert"]["subtitle"] = subtitle
    thread = str(alert.get("threadId") or "").strip()
    if thread:
        payload["aps"]["thread-id"] = thread[:64]
    for key in ("alertId", "href", "sessionId", "runId"):
        value = str(alert.get(key) or "").strip()
        if value:
            payload[key] = value
    conversation = str(alert.get("conversation") or "").strip()
    sealed = str(topic or "") in APNS_CONVERSATION_TOPICS
    if (sealed and conversation
            and len(conversation) <= APNS_CONVERSATION_MAX_CHARS
            and conversation.startswith(("#", APNS_DIRECT_CONVERSATION_PREFIX))
            and all(c.isprintable() for c in conversation)):
        payload["conversation"] = conversation
        if not thread:
            payload["aps"]["thread-id"] = conversation[:64]
    if sealed:
        channel_id = str(alert.get("channelId") or "").strip().lower()
        if APNS_CHANNEL_ID_RE.fullmatch(channel_id):
            payload["channelId"] = channel_id
            payload["aps"].setdefault("thread-id", "channel:" + channel_id)
        ref = str(alert.get("ref") or "").strip().lower()
        if APNS_REF_RE.fullmatch(ref):
            payload["ref"] = ref
        key_version = _key_version(alert.get("keyVersion"))
        if key_version:
            payload["keyVersion"] = key_version
        if "ref" in payload and str(topic or "") == "direct-messages":
            sealed_frame = _sealed_frame(alert.get("frame"))
            if sealed_frame:
                payload["frame"] = sealed_frame
    category = str(alert.get("category") or "").strip()
    if category:
        payload["aps"]["category"] = category[:40]
    alarm = str(alert.get("alarm") or "").strip()
    if alarm in APNS_ALARM_SOUNDS:
        payload["aps"]["sound"] = APNS_ALARM_SOUNDS[alarm]
        payload["aps"]["interruption-level"] = "time-sensitive"
        payload["alarm"] = alarm
    topic = str(topic or "")
    if topic in APNS_MARKED_TOPICS:
        payload["topic"] = topic
    if topic == APNS_CALL_TOPIC:
        payload["aps"]["sound"] = APNS_CALL_SOUND
        payload["aps"]["interruption-level"] = "time-sensitive"
        payload["aps"]["relevance-score"] = 1
        payload["aps"].pop("mutable-content", None)
    origin = str(origin or "").strip()
    if origin:
        payload["origin"] = origin[:200]

    def encode(text):
        if text:
            payload["aps"]["alert"]["body"] = text
        else:
            payload["aps"]["alert"].pop("body", None)
        return json.dumps(payload, separators=(",", ":"))

    def fits(text):
        return len(encode(text).encode("utf-8")) <= APNS_PAYLOAD_MAX_BYTES

    if fits(body):
        return encode(body)
    if payload.pop("frame", None) is not None and fits(body):
        return encode(body)
    # A long comment (or one in a script JSON escapes to six bytes a
    # character) can push the payload past Apple's limit; keep the longest
    # prefix of the body that fits. The headline is never cut.
    low, high = 0, len(body)
    while low < high:
        mid = (low + high + 1) // 2
        if fits(body[:mid].rstrip() + "\u2026"):
            low = mid
        else:
            high = mid - 1
    kept = body[:low].rstrip()
    return encode(kept + "\u2026" if kept else "")


def collapse_id_for(topic, conversation=""):
    """The apns-collapse-id for a topic, or "" when its alerts must stack."""
    if topic == APNS_CHAT_TOPIC and conversation == APNS_MESH_CONVERSATION:
        return APNS_MESH_COLLAPSE_ID
    if topic == APNS_CALL_TOPIC and conversation:
        return (APNS_CALL_COLLAPSE_PREFIX + str(conversation))[:64]
    return str(topic) if topic in APNS_COLLAPSE_TOPICS else ""


# APNs `reason` values that mean the token is permanently dead and its row must
# go. Anything else (429, 5xx, timeouts) is transient: the next event retries,
# and dropping a live token on a blip would silently mute a phone forever.
APNS_DEAD_REASONS = frozenset({
    "BadDeviceToken",
    "DeviceTokenNotForTopic",
    "Unregistered",
})


def normalize_bundle_id(value):
    """The allowlisted app bundle id a registration named, or "".

    "" means "the App Store app" (APNS_BUNDLE_ID) wherever a topic is chosen, so
    an unknown or missing id behaves exactly as registration did before the
    field existed rather than failing the registration.
    """
    value = str(value or "").strip()
    for suffix in APNS_EXTENSION_SUFFIXES:
        if value.endswith(suffix):
            value = value[:-len(suffix)]
    return value if value in APNS_BUNDLE_IDS else ""


def apns_topic_for(bundle_id):
    """The apns-topic header for a token registered with `bundle_id`."""
    return normalize_bundle_id(bundle_id) or APNS_BUNDLE_ID


def _dead_token(status, reason):
    # 410 Unregistered is the documented "the app was uninstalled" signal. A
    # 400 BadDeviceToken means the token never belonged to this topic or
    # environment; either way it can never succeed again.
    if int(status) == 410:
        return True
    return int(status) == 400 and str(reason or "") in APNS_DEAD_REASONS


async def send_one(env, fetch_impl, jwt, token, environment, alert,
                   collapse_id="", topic="", bundle_id=""):
    """Deliver one alert. Returns (delivered, dead, status, reason).

    `topic` is the account event (it may be marked in the payload); the
    apns-topic header is the app that minted the token, `bundle_id` as stored
    at registration (apns_topic_for).

    `alert` is the event's own text (see alert_payload). Without a title
    nothing is sent and the reason is "no_alert_text".

    `dead` asks the caller to prune the token row. `reason` is Apple's own
    word for a refusal ("BadDeviceToken", "TooManyProviderTokenUpdates", ...)
    and is empty on success or when the gateway could not be reached at all
    (status 0); the self-test route hands it back to the phone so a silent
    failure has a name. Never raises: a push is best-effort decoration on a
    write that has already succeeded, so a failure here must never surface to
    the request that triggered it.
    """
    body = alert_payload(alert, topic, apns_origin(env))
    if not body:
        return False, False, 0, "no_alert_text"
    url = apns_host(environment) + "/3/device/" + str(token)
    expiration = (APNS_CALL_EXPIRATION_SECONDS if topic == APNS_CALL_TOPIC
                  else APNS_EXPIRATION_SECONDS)
    headers = {
        "authorization": "bearer " + jwt,
        "apns-topic": apns_topic_for(bundle_id),
        "apns-push-type": "alert",
        # 10 = deliver immediately. The alert is visible, so this is the
        # correct priority and is not subject to the silent-push throttle.
        "apns-priority": "10",
        # See APNS_EXPIRATION_SECONDS: held for a phone that is offline now.
        "apns-expiration": str(
            int(int(Date.now()) / 1000) + expiration),
    }
    if collapse_id:
        headers["apns-collapse-id"] = collapse_id[:64]
    try:
        response = await fetch_impl(
            url,
            {"method": "POST", "headers": headers,
             "body": body},
            APNS_FETCH_TIMEOUT_SECONDS,
        )
    except Exception:
        return False, False, 0, ""
    status = int(getattr(response, "status", 0) or 0)
    if status == 200:
        return True, False, status, ""
    reason = ""
    try:
        body = await response.text()
        if body:
            parsed = json.loads(body)
            if isinstance(parsed, dict):
                reason = str(parsed.get("reason") or "")
    except Exception:
        reason = ""
    if status == 403 or reason == "ExpiredProviderToken":
        # The provider JWT is stale or was signed by a rotated key. Drop it so
        # the next send mints a new one rather than repeating the rejection.
        reset_provider_token()
    return False, _dead_token(status, reason), status, reason
