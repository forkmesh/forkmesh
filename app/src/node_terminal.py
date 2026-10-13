"""Frame grammar for the owner terminal relay carried on the node socket.

The website's "Sign in to Claude Code on <node>" button used to be a
``forkmesh://agent-login/...`` link, which is a dead end on any machine that
has no ForkMesh desktop installed (the browser hands the URL to the OS and the
OS answers "No Apps Available"). The web now streams the node's own login
terminal instead, over the per-owner ForkMeshNodes Durable Object that the
browser and the node both hold a WebSocket to. Which object that is comes from
the machine, not the tab: a managed mirror is its own node account and sits on
its own object, a desktop sits on the person's, and the browser is joined to
the right one when its socket is authorized (_node_events_authorization).

This module is the whole policy for that transport, kept out of entry.py's
global scope (Cloudflare startup-memory validation, error 10021) and free of
any ``js``/``workers`` import so the test suite can exercise it directly.

What the relay does with a terminal frame is deliberately tiny: validate the
shape, then hand the *same* frame to the sockets of the opposite role on the
same owner object. It keeps nothing — no storage key, no D1 row, no history —
and it never synthesises a frame of its own. Authority is not granted by the
socket either: the browser can ask only for a provider login on a node the
same account owns, and the node decides for itself whether to run one.
"""

# Frames are addressed by an opaque session id minted by the browser. It is
# only a correlation key: several tabs of one account may hold sockets, so a
# node's output has to name the request it belongs to.
MIN_SESSION_ID = 8
MAX_SESSION_ID = 64
MAX_NODE_NAME = 63
# Base64 characters per frame, so 9 KiB of raw bytes. Output is coalesced by
# the sender, making this a bound on a redraw burst rather than a keystroke.
# The ceiling that matters is not NODE_EVENT_MAX_FRAME_BYTES but the node
# clients' own 16 KiB frame limit (server/nodeevents.go eventMaxFrameBytes),
# which drops the whole socket on an oversize frame rather than one message.
MAX_DATA_CHARS = 12000
MAX_MESSAGE_CHARS = 240
MIN_COLS, MAX_COLS = 20, 500
MIN_ROWS, MAX_ROWS = 5, 200

TERMINAL_PROVIDERS = ("claude-code", "codex")
# browser -> node. "open" asks the node to start (or re-attach to) a provider
# login on its own machine; the rest drive that one session.
BROWSER_ACTIONS = ("open", "input", "resize", "close")
# node -> browser. "ready" acknowledges the open, "output" carries pty bytes,
# "error" is a refusal the browser must show, "exit" ends the session.
NODE_ACTIONS = ("ready", "output", "error", "exit")

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
    return len(value) <= MAX_DATA_CHARS and set(value) <= _BASE64_CHARS


def _clamped_int(value, low, high):
    try:
        number = int(value)
    except (TypeError, ValueError, OverflowError):
        return 0
    return max(low, min(high, number))


def normalize_terminal_frame(frame, role):
    """Rebuild one terminal frame from `role`, or None to refuse it.

    Never forwards the caller's own object: the returned dict is assembled
    field by field from validated values, so an unknown key a client invents
    cannot ride the relay to the other side.
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
        "type": "terminal",
        "action": action,
        "session": session,
        "node": node,
        "from": role,
    }
    if action == "open":
        provider = _bounded(frame.get("provider"), 20).lower()
        if provider not in TERMINAL_PROVIDERS:
            return None
        out["provider"] = provider
    if action in ("open", "resize"):
        cols = _clamped_int(frame.get("cols"), MIN_COLS, MAX_COLS)
        rows = _clamped_int(frame.get("rows"), MIN_ROWS, MAX_ROWS)
        if not cols or not rows:
            return None
        out["cols"] = cols
        out["rows"] = rows
    if action in ("input", "output"):
        data = _bounded(frame.get("data"), MAX_DATA_CHARS + 1)
        if not data or not _valid_data(data):
            return None
        out["data"] = data
    if action == "error":
        message = _bounded(frame.get("message"), MAX_MESSAGE_CHARS)
        out["message"] = message or "The node refused this terminal session."
    if action == "exit":
        out["code"] = _clamped_int(frame.get("code"), -1, 255)
    return out


def terminal_rate_state(now, start, count, window_ms, maximum):
    """Rolling window for terminal frames, separate from the event budget.

    An interactive terminal is orders of magnitude chattier than the signed
    node writes NODE_EVENT_MSG_MAX_PER_WINDOW was sized for, and a single
    redraw would otherwise trip that limit and take the socket's event
    channel down with it. Returns ``(start, count, allowed)``.
    """
    try:
        start = int(start or 0)
        count = int(count or 0)
    except (TypeError, ValueError, OverflowError):
        start, count = 0, 0
    if start <= 0 or now - start >= window_ms:
        start, count = now, 0
    count += 1
    return start, count, count <= maximum
