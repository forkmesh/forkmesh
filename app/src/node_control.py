"""Frame grammar for the owner control relay carried on the node socket.

The phone drives its owner's desktop over the local control channel
(desktop/src/LocalControl.h) while both are on one network. Away from home the
same channel rides the per-owner ForkMeshNodes Durable Object instead: the
phone holds a browser-role socket (an account event ticket), the desktop its
node-role socket (the signed drain token), and the object hands each
``{"type":"control"}`` frame to the sockets of the opposite role, exactly as it
does for the provider sign-in terminal (node_terminal.py).

Unlike the terminal, the relay cannot read what it carries. ``data`` is one
chunk of the local control protocol's own frames: the plain hello and welcome
of its handshake, then AES-256-GCM frames under keys only the two devices
derive. The desktop accepts a relayed channel only from a phone it has already
approved in person on its local network, and both ends sign the handshake
transcript with their device keys, so the relay (or anyone holding the account
session) can neither read, forge nor replay a request. This module therefore
only bounds the envelope: a session id, the machine it is addressed to, a
sequence number, and a base64 chunk small enough for every node client's own
socket limit.

Like node_terminal.py it stays free of ``js``/``workers`` imports and out of
entry.py's global scope (Cloudflare startup-memory validation, error 10021).
"""

# Opaque correlation id minted by the phone for one channel. Several phones
# (and tabs) of one account may hold sockets, so a desktop's reply names the
# channel it belongs to.
MIN_SESSION_ID = 16
MAX_SESSION_ID = 64
MAX_NODE_NAME = 63
# Base64 characters of one chunk. The whole frame must fit the Go node's own
# 16 KiB socket limit (server/nodeevents.go eventMaxFrameBytes), which drops
# the socket on an oversize frame, because the relay fans a phone's frame out
# to every node socket of the account, headless nodes included.
MAX_DATA_CHARS = 12000
MAX_REASON_CHARS = 160
MAX_SEQ = 2 ** 31 - 1

# phone -> desktop. "open" carries the handshake hello, "data" every later
# chunk, "close" ends the channel.
BROWSER_ACTIONS = ("open", "data", "close")
# desktop -> phone. "error" is a refusal the phone shows (for example a phone
# the desktop has never approved), "close" ends the channel.
NODE_ACTIONS = ("data", "close", "error")

_SESSION_CHARS = set(
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")
_NODE_CHARS = set("abcdefghijklmnopqrstuvwxyz0123456789._:-")
_BASE64_CHARS = set(
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+/=")


def _bounded(value, limit):
    text = value if isinstance(value, str) else ""
    return text[:limit].strip()


def _valid_session(value):
    return (
        MIN_SESSION_ID <= len(value) <= MAX_SESSION_ID
        and set(value) <= _SESSION_CHARS
    )


def _valid_node(value):
    return bool(value) and len(value) <= MAX_NODE_NAME and \
        set(value) <= _NODE_CHARS


def _valid_data(value):
    return bool(value) and len(value) <= MAX_DATA_CHARS and \
        set(value) <= _BASE64_CHARS


def _sequence(value):
    if isinstance(value, bool):
        return None
    try:
        number = int(value)
    except (TypeError, ValueError, OverflowError):
        return None
    if number < 0 or number > MAX_SEQ:
        return None
    return number


def normalize_control_frame(frame, role):
    """Rebuild one control frame from `role`, or None to refuse it.

    The returned dict is assembled field by field from validated values, so a
    key a client invents never rides the relay to the other side.
    """
    if not isinstance(frame, dict) or role not in ("browser", "node"):
        return None
    action = _bounded(frame.get("action"), 20).lower()
    allowed = BROWSER_ACTIONS if role == "browser" else NODE_ACTIONS
    if action not in allowed:
        return None
    session = _bounded(frame.get("session"), MAX_SESSION_ID + 1)
    node = _bounded(frame.get("node"), MAX_NODE_NAME + 1).lower()
    if not _valid_session(session) or not _valid_node(node):
        return None
    out = {
        "type": "control",
        "action": action,
        "session": session,
        "node": node,
        "from": role,
    }
    if action in ("open", "data"):
        data = _bounded(frame.get("data"), MAX_DATA_CHARS + 1)
        seq = _sequence(frame.get("seq"))
        if not _valid_data(data) or seq is None:
            return None
        out["seq"] = seq
        out["data"] = data
        # A sealed frame larger than one chunk is split; "more" says another
        # chunk of the same frame follows.
        out["more"] = frame.get("more") is True
    if action in ("close", "error"):
        reason = _bounded(frame.get("reason"), MAX_REASON_CHARS)
        if action == "error":
            reason = reason or "The desktop refused this control channel."
        if reason:
            out["reason"] = reason
    return out
