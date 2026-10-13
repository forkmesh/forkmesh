"""The owner control relay: the phone's local control channel, carried
through the per-owner node object when the phone is away from home.

The relay forwards opaque, end-to-end sealed chunks between an account's own
browser-role and node-role sockets (node_control.py). These tests pin the
envelope it accepts, the budget that keeps the frames inside every node
client's own socket limit, and the desktop and phone halves that must agree
with it.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPOSITORY = ROOT.parent
SRC = ROOT / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import node_control  # noqa: E402

ENTRY = (SRC / "entry.py").read_text(encoding="utf-8")
GO_EVENTS = (REPOSITORY / "server" / "nodeevents.go").read_text(
    encoding="utf-8")
DESKTOP_SOCKET = (
    REPOSITORY / "desktop" / "src" / "NodeEventSocket.cpp"
).read_text(encoding="utf-8")
DESKTOP_CONTROL = (
    REPOSITORY / "desktop" / "src" / "LocalControl.cpp"
).read_text(encoding="utf-8")
# The phone half lives with the native app, which may be carried in another
# repository (ForkMesh Next); its contract is checked when it is here.
PHONE_CHANNEL_PATH = (
    REPOSITORY / "native" / "ios" / "ForkMeshCore" / "Sources" / "FMControl"
    / "RelayControlConnection.swift"
)


def _frame(**overrides):
    frame = {
        "action": "data",
        "session": "s" * 32,
        "node": "Desk-1",
        "seq": 0,
        "data": "AAAA",
    }
    frame.update(overrides)
    return frame


def test_a_phone_frame_is_rebuilt_from_validated_fields_only():
    out = node_control.normalize_control_frame(
        _frame(more=True, smuggled={"x": 1}), "browser")
    assert out == {
        "type": "control", "action": "data", "session": "s" * 32,
        "node": "desk-1", "from": "browser", "seq": 0, "data": "AAAA",
        "more": True,
    }
    opened = node_control.normalize_control_frame(
        _frame(action="open"), "browser")
    assert opened["action"] == "open" and opened["more"] is False


def test_each_side_speaks_only_its_own_actions():
    assert node_control.normalize_control_frame(
        _frame(action="error"), "browser") is None
    assert node_control.normalize_control_frame(
        _frame(action="open"), "node") is None
    assert node_control.normalize_control_frame(_frame(), "relay") is None
    refused = node_control.normalize_control_frame(
        {"action": "error", "session": "s" * 32, "node": "desk"}, "node")
    assert refused["reason"] == "The desktop refused this control channel."
    closed = node_control.normalize_control_frame(
        {"action": "close", "session": "s" * 32, "node": "desk",
         "reason": "x" * 999}, "browser")
    assert len(closed["reason"]) == node_control.MAX_REASON_CHARS


def test_the_envelope_is_bounded():
    bad = [
        _frame(session="short"),
        _frame(session="s" * 65),
        _frame(session="s" * 31 + "/"),
        _frame(node=""),
        _frame(node="desk one"),
        _frame(data=""),
        _frame(data="A" * (node_control.MAX_DATA_CHARS + 1)),
        _frame(data="not-base64"),
        _frame(seq=-1),
        _frame(seq=2 ** 31),
        _frame(seq=True),
        _frame(seq="x"),
        "not a dict",
    ]
    for frame in bad:
        assert node_control.normalize_control_frame(frame, "browser") is None


def test_a_chunk_fits_every_node_clients_own_socket_limit():
    # The relay fans a phone's frame out to EVERY node socket of the account,
    # and the Go node drops its whole socket on an oversize frame.
    assert "eventMaxFrameBytes  = 16 << 10" in GO_EVENTS
    assert node_control.MAX_DATA_CHARS + 512 < 16 * 1024
    # Go ignores frame types it does not speak, so control frames cost a
    # headless node nothing.
    assert 'if frame.Type != "event" {' in GO_EVENTS


def test_the_relay_advertises_and_meters_control_frames():
    assert 'NODE_FRAME_RELAYS = ("terminal", "control")' in ENTRY
    relay = ENTRY.split("def _relay_control(self, ws, frame):", 1)[1][:1600]
    assert "normalize_control_frame(frame, role)" in relay
    assert 'target = "node" if role == "browser" else "browser"' in relay
    for persistence in ("storage", "d1_run", "encrypt_row"):
        assert persistence not in relay
    assert '"cl_start": control_start, "cl_count": control_count,' in ENTRY


def test_desktop_and_phone_chunk_inside_the_relay_budget():
    # Both halves split a sealed frame into chunks the relay accepts.
    assert "kRelayChunkChars = 9000" in DESKTOP_CONTROL
    if PHONE_CHANNEL_PATH.exists():
        phone = PHONE_CHANNEL_PATH.read_text(encoding="utf-8")
        assert "static let chunkChars = 9000" in phone
    assert 9000 <= node_control.MAX_DATA_CHARS
    # The desktop hears only frames the relay labelled as the browser half.
    assert 'type == QLatin1String("control")' in DESKTOP_SOCKET
