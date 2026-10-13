"""Privacy-safe protocol helpers for the transient multiplayer world.

This module is deliberately pure Python: the Durable Object in ``entry.py``
uses it to turn untrusted WebSocket JSON into a very small, allowlisted public
presence state.  Unknown fields are discarded rather than echoed, so repository
paths, wallet addresses, URLs, form contents, and other private client data
cannot accidentally become multiplayer payloads.

Nothing here identifies a network connection.  Country is an approximate
Cloudflare-provided two-letter code, while peer ids are random per connection.
"""

import math
import re
import unicodedata


# One shared virtual day lasts four real hours.  Clients advance the returned
# worldTimeMs locally and wrap at WORLD_DAY_LENGTH_MS, so every visitor sees the
# same cycle without polling.
WORLD_DAY_LENGTH_MS = 4 * 60 * 60 * 1000

# The socket is intentionally low-bandwidth: a client should send movement only
# a few times per second and a heartbeat while idle.
WORLD_MESSAGE_MAX_BYTES = 1024
WORLD_RATE_WINDOW_MS = 1000
WORLD_RATE_MAX_PER_WINDOW = 4
# Ordinary browser bursts can legitimately cross the soft budget while a
# connection publishes its profile and initial position.  The Durable Object
# drops those disposable excess frames, but a sender that keeps flooding past
# this bounded ceiling is still disconnected within the same one-second
# window.
WORLD_RATE_HARD_MAX_PER_WINDOW = 12
WORLD_BROADCAST_WINDOW_MS = 1000
WORLD_BROADCAST_MAX_PER_WINDOW = 24
WORLD_CONNECT_WINDOW_MS = 10 * 1000
WORLD_CONNECT_MAX_PER_WINDOW = 20
WORLD_CLIENT_STALE_MS = 90 * 1000
WORLD_MAX_CONNECTIONS = 64
# Status listeners share the hibernating World relay but are not participants
# in the room. Keep a separate ceiling so a busy public status page can never
# consume avatar capacity.
STATUS_SUBSCRIBER_MAX_CONNECTIONS = 2048
# Town and the original Office remain compact, but marketplace district
# packages may place their islands thousands of units out (the aquarium Sky
# Office package sits 14,400 units down its hyperloop axis). Presence still
# needs one finite envelope so visitors remain visible to each other after
# arrival without accepting unbounded client coordinates.
WORLD_COORD_LIMIT = 16_000.0
# One animated walking step spans about 1.6 World coordinate units at the
# avatar's normal gait. The relay converts accepted horizontal movement into
# this aggregate, but never stores the coordinates or a route. A generous
# speed ceiling keeps sprint/dash movement valid while rejecting browser-made
# teleports; a stale sample starts a fresh baseline instead of crediting the
# whole gap at once.
WORLD_STEP_LENGTH_UNITS = 1.6
WORLD_STEP_MAX_SPEED_UNITS_PER_SECOND = 60.0
WORLD_STEP_DISTANCE_ALLOWANCE_UNITS = 2.5
WORLD_STEP_MAX_SAMPLE_MS = 5 * 1000
WORLD_STEP_FLUSH_COUNT = 50
WORLD_STEP_FLUSH_MS = 30 * 1000
WORLD_STEP_MAX_TOTAL = 9_000_000_000_000
WORLD_ARRIVAL_COLUMNS = 10
WORLD_ARRIVAL_X = -8.1
WORLD_ARRIVAL_Z = 30.0
WORLD_ARRIVAL_COLUMN_GAP = 1.8
WORLD_ARRIVAL_ROW_GAP = 2.1
WORLD_ARRIVAL_CLEARANCE = 0.9

WORLD_BROWSER_VALUES = frozenset({
    "chrome", "edge", "firefox", "safari", "other", "hidden",
})
WORLD_OS_VALUES = frozenset({
    "android", "chromeos", "ios", "linux", "macos", "windows", "other",
    "hidden",
})
WORLD_STATUS_VALUES = frozenset({
    "available", "away", "busy", "exploring", "hidden", "idle",
})
WORLD_ACTIVITY_VALUES = frozenset({
    "browsing-code-visualization", "exploring-town-square", "hidden",
    "reading-documentation", "viewing-repository", "visiting-office",
    "visiting-organization",
})
WORLD_FIRST_VISIT_AGE_VALUES = frozenset({
    "this-session", "today", "this-week", "this-month", "this-year",
    "over-a-year", "hidden",
})
WORLD_DOOR_VALUES = frozenset({"closed", "knock", "open"})
# Text-free consent frames addressed at exactly one live peer. A handshake is
# the two-person greeting: one visitor offers, the other shakes back (or
# declines, which says nothing beyond "not now"). Only an accepted handshake
# becomes public, and then only as the pair of peer ids that shook.
WORLD_TARGETED_INTERACTION_KINDS = (
    "knock",
    "home-grant",
    "home-decline",
    "handshake-offer",
    "handshake-accept",
    "handshake-decline",
)
WORLD_EMOTE_VALUES = frozenset({
    "backflip",
    "celebrate",
    "dance",
    "float",
    "idea",
    "jump",
    "superjump",
    "sparkle",
    "spin",
    "wave",
    "wobble",
})
WORLD_ACCOUNT_STATUS_VALUES = frozenset({
    "Guest", "Registered", "Supporting member", "Mirror operator",
    "Organization admin", "Verified bot",
})
# A Supporting member perk: an alternate avatar outfit color that replaces the
# default country-flag shirt. "" means "use the default flag shirt".
WORLD_OUTFIT_COLOR_VALUES = frozenset({
    "", "aurora", "ember", "violet", "gold", "slate",
})
# A Supporting member perk: pin one of the tailored outfit cuts instead of the
# cut the visitor's public name seeds. "" means "wear the name-seeded cut".
# The ids must stay in lockstep with OUTFIT_STYLE_OPTIONS in world-data.js.
WORLD_OUTFIT_STYLE_VALUES = frozenset({
    "", "sash", "racer", "chevron", "argyle", "circuit", "pixel",
    "waves", "starfield", "hex", "bolt", "tartan", "binary",
})
WORLD_SPACE_VALUES = frozenset({
    "town-square", "east", "central", "west", "sky-campus",
    "space-station", "code-planet", "organization-region", "planet-atlas",
    "office-campus", "office-lobby", "office-marketing",
    "office-engineering", "office-infrastructure", "office-rooftop",
})
# Spaces owned by marketplace district packages are namespaced
# "pkg-<package-id>-<floor>". The relay never learns which packages exist, so
# it accepts the shape (bounded length and alphabet) rather than a roster;
# the client shows a peer in an uninstalled district far away, not in Town.
WORLD_PACKAGE_SPACE_RE = re.compile(r"pkg-[a-z0-9]+(?:-[a-z0-9]+){1,8}")
WORLD_PACKAGE_SPACE_MAX = 64


def world_space(value, fallback="town-square"):
    """Return a core or package-namespaced space label, else ``fallback``."""
    space = str(value or "")
    if space in WORLD_SPACE_VALUES:
        return space
    if len(space) <= WORLD_PACKAGE_SPACE_MAX and WORLD_PACKAGE_SPACE_RE.fullmatch(space):
        return space
    return fallback
WORLD_INACTIVITY_VALUES = frozenset({
    "away", "inactive", "recent", "offline-operator", "returning",
})
WORLD_NODE_BADGE_MAX = 6
WORLD_STATUS_NOTE_MAX = 20
# "First seen 12 minutes ago" on the chest badge. The client derives this from
# its own local first-visit marker; only whole minutes travel, bounded to ten
# years so a spoofed value cannot become an unbounded number on a peer canvas.
WORLD_FIRST_SEEN_MAX_MINUTES = 10 * 365 * 24 * 60
# "Joined 3 months ago" — the account creation timestamp, which is already
# public on /api/accounts/{name}. Guests never carry one.
WORLD_JOINED_AT_MIN_MS = 1577836800000  # 2020-01-01T00:00:00Z

# Office meetings use a separate authorized socket from the global World.
# This protocol carries only ephemeral room-local avatar and chair state. Chat
# messages, channel identifiers, attachments, and encryption material are not
# valid fields here.
OFFICE_MESSAGE_MAX_BYTES = 4096
OFFICE_RATE_WINDOW_MS = 1000
OFFICE_RATE_MAX_PER_WINDOW = 8
OFFICE_BROADCAST_WINDOW_MS = 1000
OFFICE_BROADCAST_MAX_PER_WINDOW = 64
OFFICE_CLIENT_STALE_MS = 90 * 1000
OFFICE_MAX_CONNECTIONS = 16
OFFICE_COORD_LIMIT = 32.0
OFFICE_CHAIR_IDS = frozenset(
    "chair-%d" % index for index in range(1, 9))
OFFICE_POSES = frozenset({"standing", "seated"})
OFFICE_PUBLIC_FIELDS = (
    "id", "name", "accountStatus", "x", "y", "z", "yaw", "moving",
    "pose", "chairId", "bindingKey", "updatedAt",
)

# This is the complete state that may leave the Durable Object.  Keeping the
# list explicit is the privacy boundary for snapshots and presence frames.
WORLD_PUBLIC_FIELDS = (
    "id", "name", "countryCode", "browser", "os", "status", "localTime",
    "activityCategory", "inputActive", "visitCount", "firstVisitAge",
    "firstSeenMinutes", "joinedAt",
    "accountStatus", "nodeCount", "space",
    "publicDoor", "statusEmoji", "statusNote", "outfitColor",
    "outfitStyle", "faceImage", "solana",
    "x", "y", "z", "yaw", "lookYaw", "lookPitch", "moving", "updatedAt",
)

WORLD_DRONE_RACE_MAX_RINGS = 64
WORLD_DRONE_RACE_MAX_LAP_MS = 3_600_000

_COUNTRY_RE = re.compile(r"^[A-Z]{2}$")
_LOCAL_TIME_RE = re.compile(r"^(?:[01]\d|2[0-3]):[0-5]\d$")
_PEER_ID_RE = re.compile(r"^[A-Za-z0-9_-]{1,32}$")
_P256_COORDINATE_RE = re.compile(r"^[A-Za-z0-9_-]{43}$")
_SOLANA_ADDRESS_RE = re.compile(r"^[1-9A-HJ-NP-Za-km-z]{32,44}$")


def approximate_country_code(value):
    """Return an approximate two-letter code or an empty string.

    Cloudflare uses ``XX`` when the country is unknown and may use non-country
    sentinels such as ``T1``.  Neither is suitable for a public flag, so they
    collapse to no country rather than exposing lower-level connection data.
    """
    code = str(value or "").strip().upper()
    if not _COUNTRY_RE.fullmatch(code) or code == "XX":
        return ""
    return code


def clean_client_profile(country="", browser="", operating_system=""):
    """Normalize the coarse client profile stored on an account record.

    An account keeps the same three coarse values a live presence frame
    publishes — approximate country, browser family, operating-system family —
    so the member's campfire bench figure still wears their flag and client
    badge while they are away and have no live presence.  Nothing finer ever
    enters the record: no address, user-agent string, or version.  ``hidden``
    (the privacy sentinel of a live frame) and any unknown value store as "".
    """
    browser = str(browser or "").strip().lower()
    operating_system = str(operating_system or "").strip().lower()
    return {
        "countryCode": approximate_country_code(country),
        "browser": browser if browser in WORLD_BROWSER_VALUES
                   and browser != "hidden" else "",
        "os": operating_system if operating_system in WORLD_OS_VALUES
              and operating_system != "hidden" else "",
    }


def clean_display_name(value, fallback="Guest"):
    """Bound a public display name and remove markup/control punctuation."""
    raw = str(value or "")[:128]
    # Unicode letters and digits are welcome.  A deliberately tiny punctuation
    # set keeps names readable while excluding HTML/URL syntax and controls.
    clean = "".join(
        ch for ch in raw if ch.isalnum() or ch in (" ", ".", "_", "-")
    )
    clean = " ".join(clean.split()).strip(" ._-")[:32].strip()
    if clean:
        return clean
    safe_fallback = "".join(
        ch for ch in str(fallback or "Guest")
        if ch.isalnum() or ch in (" ", ".", "_", "-")
    )
    return (" ".join(safe_fallback.split()).strip(" ._-")[:32] or "Guest")


def movement_step_credit(previous, current, observed_at, now, carry=0.0):
    """Convert one plausible horizontal movement sample into whole steps.

    ``carry`` is the uncredited fraction of a step held only in the socket's
    hibernation attachment. The returned tuple is ``(steps, carry, observed)``.
    No coordinate, distance sample, or timestamp enters the account aggregate.
    """
    previous = previous if isinstance(previous, dict) else {}
    current = current if isinstance(current, dict) else {}
    try:
        now = int(now)
        observed_at = int(observed_at or 0)
        carry = float(carry or 0.0)
    except (TypeError, ValueError):
        return 0, 0.0, 0
    carry = carry if math.isfinite(carry) else 0.0
    carry = max(0.0, min(WORLD_STEP_LENGTH_UNITS - 0.001, carry))
    elapsed = now - observed_at
    if (
        observed_at <= 0
        or elapsed <= 0
        or elapsed > WORLD_STEP_MAX_SAMPLE_MS
        or current.get("pose") == "seated"
    ):
        return 0, 0.0 if elapsed > WORLD_STEP_MAX_SAMPLE_MS else carry, now
    previous_space = str(previous.get("space") or "")
    current_space = str(current.get("space") or "")
    if previous_space and current_space and previous_space != current_space:
        return 0, 0.0, now
    if not (
        previous.get("moving") is True or current.get("moving") is True
    ):
        return 0, carry, now
    try:
        dx = float(current.get("x")) - float(previous.get("x"))
        dz = float(current.get("z")) - float(previous.get("z"))
    except (TypeError, ValueError):
        return 0, carry, now
    if not math.isfinite(dx) or not math.isfinite(dz):
        return 0, carry, now
    distance = math.hypot(dx, dz)
    maximum = (
        WORLD_STEP_MAX_SPEED_UNITS_PER_SECOND * elapsed / 1000.0
        + WORLD_STEP_DISTANCE_ALLOWANCE_UNITS
    )
    if distance > maximum:
        return 0, 0.0, now
    accumulated = round(carry + distance, 4)
    steps = int(accumulated // WORLD_STEP_LENGTH_UNITS)
    return steps, round(
        accumulated - steps * WORLD_STEP_LENGTH_UNITS, 4), now


def account_presence_key(name):
    """Fold one ticket-verified account name into a single presence identity.

    Two sockets share an identity only when the relay itself verified the same
    account behind both. An empty value means "no verified account" — guests,
    and anyone whose ticket did not check out, are never combined.
    """
    raw = str(name or "").strip()
    clean = clean_display_name(raw, "")
    # Anything that did not survive sanitizing unchanged collapsed onto a
    # generic fallback label; folding on that would combine unrelated people.
    return clean.casefold() if raw and clean == raw else ""


def _emoji_base(character):
    codepoint = ord(character)
    return (
        codepoint in {
            0x00A9, 0x00AE, 0x203C, 0x2049, 0x2122, 0x2139,
            0x24C2, 0x3030, 0x303D, 0x3297, 0x3299,
        }
        or 0x2194 <= codepoint <= 0x21FF
        or codepoint in {0x231A, 0x231B, 0x2328, 0x23CF}
        or 0x23E9 <= codepoint <= 0x23F3
        or 0x23F8 <= codepoint <= 0x23FA
        or 0x25AA <= codepoint <= 0x25AB
        or codepoint in {0x25B6, 0x25C0}
        or 0x25FB <= codepoint <= 0x25FE
        or 0x2600 <= codepoint <= 0x27BF
        or 0x2934 <= codepoint <= 0x2935
        or 0x2B05 <= codepoint <= 0x2B07
        or 0x2B1B <= codepoint <= 0x2B1C
        or codepoint in {0x2B50, 0x2B55}
        or (
            0x1F000 <= codepoint <= 0x1FAFF
            and not 0x1F1E6 <= codepoint <= 0x1F1FF
        )
    )


def clean_status_emoji(value):
    """Return one bounded Unicode emoji grapheme or an empty string.

    This parser accepts pictographs, flags, keycaps, skin-tone modifiers,
    subdivision-flag tags, and zero-width-joiner sequences. It intentionally
    rejects arbitrary text and multiple adjacent emoji, keeping the public
    presence field both useful and mechanically bounded.
    """
    emoji = unicodedata.normalize("NFC", str(value or "").strip())
    if not emoji or len(emoji) > 24 or len(emoji.encode("utf-8")) > 96:
        return ""
    codepoints = [ord(character) for character in emoji]
    if (
        len(codepoints) == 2
        and all(0x1F1E6 <= codepoint <= 0x1F1FF for codepoint in codepoints)
    ):
        return emoji
    if (
        len(codepoints) in (2, 3)
        and emoji[0] in "#*0123456789"
        and codepoints[-1] == 0x20E3
        and (len(codepoints) == 2 or codepoints[1] == 0xFE0F)
    ):
        return emoji

    index = 0

    def consume_pictograph(offset):
        if offset >= len(emoji) or not _emoji_base(emoji[offset]):
            return -1
        base = ord(emoji[offset])
        offset += 1
        if offset < len(emoji) and ord(emoji[offset]) in (0xFE0E, 0xFE0F):
            offset += 1
        if offset < len(emoji) and 0x1F3FB <= ord(emoji[offset]) <= 0x1F3FF:
            offset += 1
        if offset < len(emoji) and 0xE0020 <= ord(emoji[offset]) <= 0xE007E:
            if base != 0x1F3F4:
                return -1
            while (
                offset < len(emoji)
                and 0xE0020 <= ord(emoji[offset]) <= 0xE007E
            ):
                offset += 1
            if offset >= len(emoji) or ord(emoji[offset]) != 0xE007F:
                return -1
            offset += 1
        return offset

    index = consume_pictograph(index)
    if index < 0:
        return ""
    while index < len(emoji):
        if ord(emoji[index]) != 0x200D:
            return ""
        index = consume_pictograph(index + 1)
        if index < 0:
            return ""
    return emoji


def clean_status_note(value):
    """Return one Unicode word of at most 20 code points."""
    note = unicodedata.normalize("NFKC", str(value or "").strip())
    if not note or len(note) > WORLD_STATUS_NOTE_MAX:
        return ""
    first_category = unicodedata.category(note[0])
    if first_category[:1] not in {"L", "N"}:
        return ""
    for character in note[1:]:
        if (
            unicodedata.category(character)[:1] not in {"L", "M", "N"}
            and character not in {"'", "\u2019", "-"}
        ):
            return ""
    return note


def _choice(value, allowed, fallback):
    choice = str(value or "").strip().lower()
    return choice if choice in allowed else fallback


def _bounded_number(value, fallback):
    if isinstance(value, bool):
        return fallback
    try:
        number = float(value)
    except (TypeError, ValueError):
        return fallback
    if not math.isfinite(number):
        return fallback
    number = max(-WORLD_COORD_LIMIT, min(WORLD_COORD_LIMIT, number))
    return round(number, 2)


def _bounded_yaw(value, fallback):
    if isinstance(value, bool):
        return fallback
    try:
        number = float(value)
    except (TypeError, ValueError):
        return fallback
    if not math.isfinite(number):
        return fallback
    return round(max(-math.pi, min(math.pi, number)), 3)


def _bounded_look_pitch(value, fallback):
    if isinstance(value, bool):
        return fallback
    try:
        number = float(value)
    except (TypeError, ValueError):
        return fallback
    if not math.isfinite(number):
        return fallback
    # The camera stops just short of vertical. Match that finite envelope at
    # the relay boundary so malformed view frames cannot distort an avatar.
    return round(max(-math.pi / 2 + 0.01, min(math.pi / 2 - 0.01, number)), 3)


def _bounded_visit_count(value, fallback):
    """Return an integer visit count without coercing strings or booleans."""
    if isinstance(value, bool) or not isinstance(value, int):
        return fallback
    return max(0, min(999, value))


def _bounded_first_seen_minutes(value, fallback):
    """Return whole minutes since the visitor's own first visit."""
    if isinstance(value, bool) or not isinstance(value, int):
        return fallback
    return max(0, min(WORLD_FIRST_SEEN_MAX_MINUTES, value))


def _bounded_joined_at(value, now, fallback):
    """Return a public account-creation timestamp, or 0 when implausible."""
    if isinstance(value, bool) or not isinstance(value, int):
        return fallback
    if value < WORLD_JOINED_AT_MIN_MS or value > int(now):
        return 0
    return value


def arrival_position(slot):
    """Return one deterministic, non-overlapping Town Square arrival slot.

    The 64-person room fits into seven shallow rows.  Ten people fill a row
    before the next row begins, and yaw zero faces everyone toward the square
    instead of toward one another.  The slot itself stays private to the live
    Durable Object attachment; only the ordinary bounded coordinates leave it.
    """
    try:
        slot = int(slot)
    except (TypeError, ValueError):
        slot = 0
    slot = max(0, min(WORLD_MAX_CONNECTIONS - 1, slot))
    column = slot % WORLD_ARRIVAL_COLUMNS
    row = slot // WORLD_ARRIVAL_COLUMNS
    return {
        "x": round(WORLD_ARRIVAL_X + column * WORLD_ARRIVAL_COLUMN_GAP, 2),
        "y": 0.38,
        "z": round(WORLD_ARRIVAL_Z - row * WORLD_ARRIVAL_ROW_GAP, 2),
        "yaw": 0.0,
    }


def arrival_slot_near_position(x, z):
    """Return the arrival slot whose cell (x, z) is standing in, else -1.

    A visitor who restored a saved position on top of an arrival cell keeps
    blocking that cell even when their own reservation points at a different
    slot, so a newcomer is never dropped onto a person already standing there.
    """
    try:
        x = float(x)
        z = float(z)
    except (TypeError, ValueError):
        return -1
    if not (math.isfinite(x) and math.isfinite(z)):
        return -1
    column = int(round((x - WORLD_ARRIVAL_X) / WORLD_ARRIVAL_COLUMN_GAP))
    row = int(round((WORLD_ARRIVAL_Z - z) / WORLD_ARRIVAL_ROW_GAP))
    if column < 0 or column >= WORLD_ARRIVAL_COLUMNS or row < 0:
        return -1
    slot = row * WORLD_ARRIVAL_COLUMNS + column
    if slot >= WORLD_MAX_CONNECTIONS:
        return -1
    cell = arrival_position(slot)
    if (abs(cell["x"] - x) > WORLD_ARRIVAL_CLEARANCE
            or abs(cell["z"] - z) > WORLD_ARRIVAL_CLEARANCE):
        return -1
    return slot


def first_available_arrival_slot(used_slots):
    """Choose the first free room slot without consulting persistent storage."""
    used = set()
    for value in used_slots or ():
        if isinstance(value, bool):
            continue
        try:
            slot = int(value)
        except (TypeError, ValueError):
            continue
        if 0 <= slot < WORLD_MAX_CONNECTIONS:
            used.add(slot)
    for slot in range(WORLD_MAX_CONNECTIONS):
        if slot not in used:
            return slot
    return WORLD_MAX_CONNECTIONS - 1


def default_presence(peer_id, now):
    """Create a non-identifying, privacy-default per-connection state."""
    peer_id = str(peer_id or "")[:32]
    suffix = "".join(ch for ch in peer_id[:4] if ch.isalnum())
    return {
        "id": peer_id,
        "name": clean_display_name("", "Guest " + suffix if suffix else "Guest"),
        # The server keeps the coarse edge country privately on the live socket.
        # A client must explicitly send shareCountry=true before it is public.
        "countryCode": "",
        "browser": "hidden",
        "os": "hidden",
        "status": "hidden",
        "localTime": "",
        "activityCategory": "hidden",
        # These coarse indicators are meaningful only while generalized
        # activity sharing is enabled. They never contain event coordinates,
        # visited URLs, query strings, or timestamps.
        "inputActive": False,
        "visitCount": 0,
        "firstVisitAge": "hidden",
        "firstSeenMinutes": 0,
        # Public account age, shown as "joined … ago" on the chest badge. It
        # stays 0 until a validated world ticket has made this a named account.
        "joinedAt": 0,
        # Account status and operator-belt count are supplied by the routing
        # Worker after it validates a short-lived world ticket. They are never
        # accepted from arbitrary socket JSON.
        "accountStatus": "Guest",
        "nodeCount": 0,
        "space": "town-square",
        "publicDoor": "closed",
        "statusEmoji": "",
        "statusNote": "",
        "outfitColor": "",
        "outfitStyle": "",
        "faceImage": None,
        # The chest wallet-QR address. A self-claim like the display name,
        # but only a ticket-authenticated account may wear one.
        "solana": "",
        "x": 0.0,
        "y": 0.0,
        "z": 0.0,
        "yaw": 0.0,
        "lookYaw": 0.0,
        "lookPitch": 0.0,
        "moving": False,
        "updatedAt": int(now),
    }


def default_office_presence(participant_id, now):
    """Create one privacy-minimal live Office participant state."""
    participant_id = str(participant_id or "")[:32]
    if not _PEER_ID_RE.fullmatch(participant_id):
        participant_id = "participant"
    suffix = "".join(ch for ch in participant_id[:4] if ch.isalnum())
    return {
        "id": participant_id,
        "name": clean_display_name(
            "", "Guest " + suffix if suffix else "Guest"),
        "accountStatus": "Guest",
        "x": 0.0,
        "y": 0.38,
        "z": 0.0,
        "yaw": 0.0,
        "moving": False,
        "pose": "standing",
        "chairId": "",
        "bindingKey": None,
        "updatedAt": int(now),
    }


def public_office_presence(state):
    """Return exactly the fields authorized Office participants may receive."""
    state = state if isinstance(state, dict) else {}
    return {field: state.get(field) for field in OFFICE_PUBLIC_FIELDS}


def _office_binding_key(value):
    """Return a bounded public P-256 JWK or ``None``.

    Exact keys prevent extensions such as a private coordinate from being
    reflected to meeting peers.
    """
    if not isinstance(value, dict) or set(value) != {"kty", "crv", "x", "y"}:
        return None
    if value.get("kty") != "EC" or value.get("crv") != "P-256":
        return None
    x = str(value.get("x") or "")
    y = str(value.get("y") or "")
    if not _P256_COORDINATE_RE.fullmatch(x):
        return None
    if not _P256_COORDINATE_RE.fullmatch(y):
        return None
    return {"kty": "EC", "crv": "P-256", "x": x, "y": y}


def _bounded_office_number(value, fallback):
    if isinstance(value, bool):
        return fallback
    try:
        number = float(value)
    except (TypeError, ValueError):
        return fallback
    if not math.isfinite(number):
        return fallback
    number = max(-OFFICE_COORD_LIMIT, min(OFFICE_COORD_LIMIT, number))
    return round(number, 2)


def sanitize_office_message(payload, current, now, trusted_name="",
                            trusted_account_status="Guest"):
    """Apply an allowlisted Office avatar frame.

    Seat requests are intentionally handled separately so the Durable Object
    can decide ownership from its complete live-socket snapshot.
    """
    if not isinstance(payload, dict) or not isinstance(current, dict):
        return None
    kind = str(payload.get("type") or "").strip().lower()
    state = dict(current)

    if kind == "presence":
        if trusted_name:
            state["name"] = clean_display_name(
                trusted_name, state.get("name") or "Contributor")
        elif "name" in payload:
            state["name"] = clean_display_name(
                payload.get("name"), state.get("name") or "Guest")
        status = str(trusted_account_status or "Guest").strip()
        state["accountStatus"] = (
            status if status in WORLD_ACCOUNT_STATUS_VALUES else "Guest")
        if "bindingKey" in payload:
            state["bindingKey"] = _office_binding_key(
                payload.get("bindingKey"))
        state["updatedAt"] = int(now)
        return kind, state

    if kind == "move":
        if state.get("pose") == "seated":
            state["moving"] = False
            state["updatedAt"] = int(now)
            return kind, state
        for field in ("x", "y", "z"):
            if field in payload:
                state[field] = _bounded_office_number(
                    payload.get(field), state.get(field, 0.0))
        if "yaw" in payload:
            state["yaw"] = _bounded_yaw(
                payload.get("yaw"), state.get("yaw", 0.0))
        if isinstance(payload.get("moving"), bool):
            state["moving"] = payload["moving"]
        state["updatedAt"] = int(now)
        return kind, state

    if kind == "ping":
        return kind, state

    return None


def allocate_office_seat(current, requested_chair, occupied):
    """Grant, release, or reject one server-authoritative chair request."""
    if not isinstance(current, dict):
        return "invalid", current
    state = dict(current)
    chair_id = str(requested_chair or "").strip()
    if not chair_id:
        state["chairId"] = ""
        state["pose"] = "standing"
        state["moving"] = False
        return "released", state
    if chair_id not in OFFICE_CHAIR_IDS:
        return "invalid", state
    occupied = occupied if isinstance(occupied, dict) else {}
    owner = str(occupied.get(chair_id) or "")
    if owner and owner != str(state.get("id") or ""):
        return "denied", state
    state["chairId"] = chair_id
    state["pose"] = "seated"
    state["moving"] = False
    return "granted", state


def office_movement_delta(state):
    """Return one bounded Office movement frame without identity secrets."""
    state = state if isinstance(state, dict) else {}
    return {
        "type": "move",
        "id": state.get("id"),
        "x": state.get("x"),
        "y": state.get("y"),
        "z": state.get("z"),
        "yaw": state.get("yaw"),
        "moving": bool(state.get("moving")),
        "updatedAt": state.get("updatedAt"),
    }


def public_presence(state):
    """Return exactly the allowlisted public fields from a socket state."""
    state = state if isinstance(state, dict) else {}
    return {field: state.get(field) for field in WORLD_PUBLIC_FIELDS}


def sanitize_message(payload, current, now, country_source="",
                     trusted_name="", trusted_node_count=0):
    """Apply one allowlisted client frame.

    Returns ``(kind, new_state)`` for ``presence``, ``move``, and ``ping`` or
    ``None`` for every other shape.  Client-supplied ids/country codes and all
    unknown fields are ignored.
    """
    if not isinstance(payload, dict) or not isinstance(current, dict):
        return None
    kind = str(payload.get("type") or "").strip().lower()
    state = dict(current)

    if kind == "presence":
        if payload.get("shareName") is False:
            suffix = "".join(
                ch for ch in str(state.get("id") or "")[:4] if ch.isalnum())
            state["name"] = clean_display_name(
                "", "Guest " + suffix if suffix else "Guest")
        elif trusted_name:
            state["name"] = clean_display_name(
                trusted_name, state.get("name") or "Contributor")
        # An anonymous socket never chooses an account-looking display name.
        # Its server-generated Guest suffix is bound to this random peer id,
        # so a second client cannot visually impersonate a signed-in avatar
        # simply by copying the victim's name into a presence frame.
        if "browser" in payload:
            state["browser"] = _choice(
                payload.get("browser"), WORLD_BROWSER_VALUES,
                state.get("browser") or "hidden")
        if "os" in payload:
            state["os"] = _choice(
                payload.get("os"), WORLD_OS_VALUES,
                state.get("os") or "hidden")
        if "status" in payload:
            state["status"] = _choice(
                payload.get("status"), WORLD_STATUS_VALUES,
                state.get("status") or "exploring")
        if "localTime" in payload:
            local_time = str(payload.get("localTime") or "").strip()
            state["localTime"] = (
                local_time if _LOCAL_TIME_RE.fullmatch(local_time) else "")
        if "activityCategory" in payload:
            state["activityCategory"] = _choice(
                payload.get("activityCategory"), WORLD_ACTIVITY_VALUES,
                "hidden")
        if "inputActive" in payload:
            state["inputActive"] = (
                payload.get("inputActive")
                if isinstance(payload.get("inputActive"), bool)
                else False)
        if "visitCount" in payload:
            state["visitCount"] = _bounded_visit_count(
                payload.get("visitCount"), 0)
        if "firstVisitAge" in payload:
            state["firstVisitAge"] = _choice(
                payload.get("firstVisitAge"),
                WORLD_FIRST_VISIT_AGE_VALUES,
                "hidden")
        if "firstSeenMinutes" in payload:
            state["firstSeenMinutes"] = _bounded_first_seen_minutes(
                payload.get("firstSeenMinutes"), 0)
        if "joinedAt" in payload:
            state["joinedAt"] = _bounded_joined_at(
                payload.get("joinedAt"), now, 0)
        # Activity privacy is the parent control for all four derived
        # indicators. Explicitly clear prior values so turning sharing off
        # cannot leave stale metadata visible in a live socket attachment.
        if state.get("activityCategory") == "hidden":
            state["inputActive"] = False
            state["visitCount"] = 0
            state["firstVisitAge"] = "hidden"
            state["firstSeenMinutes"] = 0
        # A joined date belongs to an account. A connection the routing Worker
        # never authenticated cannot publish one.
        if state.get("accountStatus") == "Guest":
            state["joinedAt"] = 0
        if "publicDoor" in payload:
            state["publicDoor"] = _choice(
                payload.get("publicDoor"), WORLD_DOOR_VALUES, "closed")
        if "statusEmoji" in payload:
            state["statusEmoji"] = clean_status_emoji(
                payload.get("statusEmoji"))
        if "statusNote" in payload:
            state["statusNote"] = clean_status_note(
                payload.get("statusNote"))
        if not state.get("statusEmoji"):
            state["statusNote"] = ""
        # The outfit color perk is validated against the connection's own
        # trusted accountStatus, never the client-claimed one: a Guest or
        # Registered payload requesting a color is silently reset to "".
        if "outfitColor" in payload:
            state["outfitColor"] = (
                _choice(payload.get("outfitColor"), WORLD_OUTFIT_COLOR_VALUES, "")
                if state.get("accountStatus") == "Supporting member" else "")
        # The pinned outfit cut follows the same trust rule as the color.
        if "outfitStyle" in payload:
            state["outfitStyle"] = (
                _choice(payload.get("outfitStyle"), WORLD_OUTFIT_STYLE_VALUES, "")
                if state.get("accountStatus") == "Supporting member" else "")
        # Wearing the account's public avatar image as the 3D face is opt-in
        # and only a boolean travels over presence: peers resolve the actual
        # image from the already-public /api/accounts/{name} lookup, so no
        # image bytes ever enter a presence frame.
        if "faceImage" in payload:
            state["faceImage"] = (
                payload.get("faceImage") is True
                and state.get("accountStatus") != "Guest")
        # The chest wallet QR shows an address its owner already chose to
        # publish on their account. It is validated as base58 and gated to
        # authenticated accounts so an anonymous socket cannot dress itself
        # in an arbitrary wallet.
        if "solana" in payload:
            raw_solana = str(payload.get("solana") or "")
            state["solana"] = (
                raw_solana
                if state.get("accountStatus") != "Guest"
                and _SOLANA_ADDRESS_RE.fullmatch(raw_solana)
                else "")
        if "space" in payload:
            state["space"] = world_space(payload.get("space"))
        if isinstance(payload.get("shareNodes"), bool):
            try:
                trusted_count = int(trusted_node_count or 0)
            except (TypeError, ValueError):
                trusted_count = 0
            state["nodeCount"] = (
                max(0, min(WORLD_NODE_BADGE_MAX, trusted_count))
                if payload["shareNodes"] else 0)
        # The connection's approximate country came from the edge, not the
        # client. A privacy toggle may hide or restore that coarse value without
        # accepting a spoofed country code from arbitrary JSON.
        if isinstance(payload.get("shareCountry"), bool):
            state["countryCode"] = (
                approximate_country_code(country_source)
                if payload["shareCountry"] else "")
        state["updatedAt"] = int(now)
        return kind, state

    if kind == "move":
        for field in ("x", "y", "z"):
            if field in payload:
                state[field] = _bounded_number(
                    payload.get(field), state.get(field, 0.0))
        if "yaw" in payload:
            state["yaw"] = _bounded_yaw(
                payload.get("yaw"), state.get("yaw", 0.0))
        if "lookYaw" in payload:
            state["lookYaw"] = _bounded_yaw(
                payload.get("lookYaw"), state.get("lookYaw", state.get("yaw", 0.0)))
        if "lookPitch" in payload:
            state["lookPitch"] = _bounded_look_pitch(
                payload.get("lookPitch"), state.get("lookPitch", 0.0))
        if isinstance(payload.get("moving"), bool):
            state["moving"] = payload["moving"]
        state["race"] = drone_race_state(payload.get("race"))
        state["updatedAt"] = int(now)
        return kind, state

    if kind == "ping":
        # Heartbeats refresh only the private socket liveness timestamp.  They
        # do not fabricate public movement/activity.
        return kind, state

    return None


def trusted_presence_claim(name="", account_status="Guest", node_count=0):
    """Sanitize the public parts of a server-validated account claim.

    The name and full node list remain private inputs to the live attachment.
    Only the status and bounded count enter the initial public state; the name
    and belt appear later if the owner explicitly enables those presence
    fields.
    """
    status = str(account_status or "Guest").strip()
    if status not in WORLD_ACCOUNT_STATUS_VALUES:
        status = "Guest"
    try:
        count = int(node_count or 0)
    except (TypeError, ValueError):
        count = 0
    return {
        "name": clean_display_name(name, "Contributor") if name else "",
        "accountStatus": status,
        "nodeCount": max(0, min(WORLD_NODE_BADGE_MAX, count)),
    }


def sanitize_inactivity_record(payload):
    """Return one consented generalized inactivity choice or ``None``."""
    if not isinstance(payload, dict):
        return None
    status = str(payload.get("status") or "").strip().lower()
    if status not in WORLD_INACTIVITY_VALUES:
        return None
    if payload.get("shareInactivity") is not True:
        return None
    return {
        "status": status,
        "shareName": payload.get("shareName") is True,
        "shareNodes": payload.get("shareNodes") is True,
    }


def sanitize_interaction(payload, sender_state):
    """Return one tiny targeted social gesture or ``None``.

    Interactions never carry user text, room names, repository data, or URLs.
    The Durable Object supplies the sender id and delivers the gesture only to
    the requested live peer, so clients cannot impersonate another visitor.
    """
    if not isinstance(payload, dict) or not isinstance(sender_state, dict):
        return None
    if payload.get("type") != "interaction":
        return None
    sender = str(sender_state.get("id") or "").strip()
    if not _PEER_ID_RE.fullmatch(sender):
        return None
    kind = str(payload.get("kind") or "").strip().lower()
    if kind in WORLD_TARGETED_INTERACTION_KINDS:
        target = str(payload.get("target") or "").strip()
        if not _PEER_ID_RE.fullmatch(target) or target == sender:
            return None
        return {"type": "interaction", "kind": kind, "target": target}
    if kind == "emote":
        emote = str(payload.get("emote") or "").strip().lower()
        if emote not in WORLD_EMOTE_VALUES:
            return None
        return {"type": "interaction", "kind": "emote", "emote": emote}
    return None


def _bounded_race_int(value, limit):
    if isinstance(value, bool):
        return 0
    try:
        number = float(value)
    except (TypeError, ValueError):
        return 0
    if not math.isfinite(number):
        return 0
    return int(max(0, min(limit, round(number))))


def drone_race_state(value):
    if not isinstance(value, dict):
        return None
    return {
        "ring": _bounded_race_int(value.get("ring"), WORLD_DRONE_RACE_MAX_RINGS),
        "lap": _bounded_race_int(value.get("lap"), WORLD_DRONE_RACE_MAX_LAP_MS),
        "best": _bounded_race_int(value.get("best"), WORLD_DRONE_RACE_MAX_LAP_MS),
    }


def movement_delta(state):
    """Small outgoing movement frame; no profile or connection metadata."""
    state = state if isinstance(state, dict) else {}
    delta = {
        "type": "move",
        "id": state.get("id"),
        "x": state.get("x"),
        "y": state.get("y"),
        "z": state.get("z"),
        "yaw": state.get("yaw"),
        "lookYaw": state.get("lookYaw"),
        "lookPitch": state.get("lookPitch"),
        "moving": bool(state.get("moving")),
        "updatedAt": state.get("updatedAt"),
    }
    if state.get("race"):
        delta["race"] = dict(state["race"])
    return delta


def advance_rate_window(start, count, now):
    """Advance the per-socket fixed window and return allowed/start/count."""
    try:
        start = int(start or 0)
        count = int(count or 0)
    except (TypeError, ValueError):
        start, count = 0, 0
    now = int(now)
    if start <= 0 or now < start or now - start >= WORLD_RATE_WINDOW_MS:
        start, count = now, 0
    count += 1
    return count <= WORLD_RATE_MAX_PER_WINDOW, start, count


def advance_broadcast_window(start, count, now):
    """Room-wide frame budget that caps fan-out amplification."""
    try:
        start = int(start or 0)
        count = int(count or 0)
    except (TypeError, ValueError):
        start, count = 0, 0
    now = int(now)
    if (start <= 0 or now < start
            or now - start >= WORLD_BROADCAST_WINDOW_MS):
        start, count = now, 0
    count += 1
    return count <= WORLD_BROADCAST_MAX_PER_WINDOW, start, count


def advance_connection_window(start, count, now):
    """Room-wide upgrade admission budget to blunt connection churn."""
    try:
        start = int(start or 0)
        count = int(count or 0)
    except (TypeError, ValueError):
        start, count = 0, 0
    now = int(now)
    if (start <= 0 or now < start
            or now - start >= WORLD_CONNECT_WINDOW_MS):
        start, count = now, 0
    count += 1
    return count <= WORLD_CONNECT_MAX_PER_WINDOW, start, count


def advance_office_rate_window(start, count, now):
    """Advance the per-participant Office frame rate window."""
    try:
        start = int(start or 0)
        count = int(count or 0)
    except (TypeError, ValueError):
        start, count = 0, 0
    now = int(now)
    if (
        start <= 0
        or now < start
        or now - start >= OFFICE_RATE_WINDOW_MS
    ):
        start, count = now, 0
    count += 1
    return count <= OFFICE_RATE_MAX_PER_WINDOW, start, count


def advance_office_broadcast_window(start, count, now):
    """Advance the room-wide Office broadcast amplification budget."""
    try:
        start = int(start or 0)
        count = int(count or 0)
    except (TypeError, ValueError):
        start, count = 0, 0
    now = int(now)
    if (
        start <= 0
        or now < start
        or now - start >= OFFICE_BROADCAST_WINDOW_MS
    ):
        start, count = now, 0
    count += 1
    return count <= OFFICE_BROADCAST_MAX_PER_WINDOW, start, count


def presence_is_stale(last_seen, now):
    """Whether a socket should be removed from live snapshots/broadcasts."""
    try:
        last_seen = int(last_seen or 0)
        now = int(now)
    except (TypeError, ValueError):
        return True
    return last_seen <= 0 or now - last_seen > WORLD_CLIENT_STALE_MS


def office_presence_is_stale(last_seen, now):
    """Whether a live Office participant should be removed and its seat freed."""
    try:
        last_seen = int(last_seen or 0)
        now = int(now)
    except (TypeError, ValueError):
        return True
    return last_seen <= 0 or now - last_seen > OFFICE_CLIENT_STALE_MS


def context_payload(country_code, now, chat_connections=0):
    """Public context with no address, user-agent, or precise location data."""
    now = int(now)
    return {
        "ok": True,
        "countryCode": approximate_country_code(country_code),
        "serverTimeMs": now,
        "worldTimeMs": now % WORLD_DAY_LENGTH_MS,
        "worldDayLengthMs": WORLD_DAY_LENGTH_MS,
        "worldConnections": WORLD_MAX_CONNECTIONS,
        "worldMessagesPerSecond": WORLD_RATE_MAX_PER_WINDOW,
        "chatConnections": max(0, int(chat_connections or 0)),
    }
