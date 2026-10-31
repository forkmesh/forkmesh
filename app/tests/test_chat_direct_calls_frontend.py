"""Contracts for DM calls, the DM "add person" flow, and the dashboard strip."""

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CHAT = (ROOT / "public" / "chat.js").read_text(encoding="utf-8")
CALLS = (ROOT / "public" / "chat-calls.js").read_text(encoding="utf-8")
HTML = (ROOT / "public" / "chat.html").read_text(encoding="utf-8")
CSS = (ROOT / "public" / "chat.css").read_text(encoding="utf-8")
DASHBOARD_CHAT = (
    ROOT / "public" / "dashboard-chat.js").read_text(encoding="utf-8")
DASHBOARD = (ROOT / "public" / "dashboard.js").read_text(encoding="utf-8")
HEADERS = (
    ROOT.parent / "www" / "public" / "_headers").read_text(encoding="utf-8")


def _function_source(text, name):
    for marker in (f"async function {name}(", f"function {name}("):
        start = text.find(marker)
        if start >= 0:
            break
    assert start >= 0, name
    next_function = text.find("\nfunction ", start + 1)
    next_async = text.find("\nasync function ", start + 1)
    ends = [value for value in (next_function, next_async) if value >= 0]
    return text[start:min(ends) if ends else len(text)]


def test_chat_has_call_controls_and_panels():
    for marker in (
        'id="chat-call-audio"',
        'id="chat-call-video"',
        'id="chat-call-panel"',
        'id="chat-call-peer"',
        'id="chat-call-state"',
        'id="chat-call-remote"',
        'id="chat-call-local"',
        'id="chat-call-mute"',
        'id="chat-call-camera"',
        'id="chat-call-hangup"',
        'id="chat-call-incoming"',
        'id="chat-call-accept"',
        'id="chat-call-decline"',
        'id="chat-direct-add"',
        'id="chat-direct-dialog-hint"',
    ):
        assert marker in HTML
    assert ".chat-call-panel" in CSS
    assert ".chat-call-incoming" in CSS


def test_call_signaling_stays_off_the_relay_history():
    for frame in (
        "call-invite",
        "call-accept",
        "call-decline",
        "call-offer",
        "call-answer",
        "call-ice",
        "call-bye",
    ):
        assert f'"{frame}"' in CALLS
    durable = _function_source(CHAT, "recordOwnChatBadgeActivity")
    durable_block = CHAT[
        CHAT.index("const DURABLE_TYPES"):CHAT.index("]);", CHAT.index(
            "const DURABLE_TYPES"))]
    assert "call-" not in durable_block
    assert "persist" not in CALLS
    assert durable is not None


def test_call_signals_are_bound_to_the_direct_room():
    source = _function_source(CHAT, "sendCallSignal")
    assert "isDirectMessageKey(channelKey)" in source
    assert "roomScopeForChannel() !== channelKey" in source
    assert "channelWireLabel(channelKey)" in source
    assert "CALL_FRAME_TYPES.has(type)" in CHAT
    assert "isDirectMessageKey(scope)) void chatCalls.handleFrame" in CHAT


def test_calls_use_stun_only_peer_to_peer_ice():
    assert 'stun:stun.cloudflare.com:3478' in CHAT
    assert "turn:" not in CHAT
    assert "RTCPeerConnectionImpl({ iceServers })" in CALLS
    assert "getUserMedia" in CALLS


def test_calls_require_a_secure_context():
    assert "isSecureContext" in CALLS
    assert "callsSupported" in CHAT


def test_caller_posts_a_missed_call_line():
    source = _function_source(CHAT, "sendMissedCallLine")
    assert 'makePlain("chat"' in source
    assert "Missed ${media} call" in source


def test_incoming_call_frames_are_filtered_by_device_and_call_id():
    source = _function_source(CALLS, "handleFrame")
    assert 'typeof selfId === "function" ? selfId() : selfId' in source
    assert "from === self" in source
    assert "plain.to && plain.to !== self" in source
    assert 'call.callId !== callId' in source


def test_ring_times_out_and_resends_invites():
    assert "RING_TIMEOUT_MS = 45000" in CALLS
    assert "INVITE_RESEND_MS = 3000" in CALLS
    start = _function_source(CALLS, "start")
    assert 'timer("invite", ring, INVITE_RESEND_MS, true)' in start
    assert 'timer("ring", hangUp, RING_TIMEOUT_MS)' in start


def test_hanging_up_an_unanswered_call_posts_a_missed_call_line():
    source = _function_source(CALLS, "hangUp")
    assert 'call?.role === "caller" && call.phase === "ringing-out"' in source
    assert 'reason: "missed"' in source
    assert "onMissed(missed.channelKey, missed.media)" in source
    assert 'hangupBtn?.addEventListener("click", hangUp)' in CALLS


def test_add_person_starts_a_private_group_channel():
    source = _function_source(CHAT, "addPersonToDirectMessage")
    assert "PRIVATE_CHANNELS_ENDPOINT" in source
    assert '"private"' in source
    assert "members: [direct.otherUser, normalized]" in source
    assert "setActiveChannel(privateChannelKey(id))" in source


def test_group_channel_names_fit_the_channel_name_rule():
    source = _function_source(CHAT, "directGroupChannelName")
    assert ".slice(0, 10)" in source
    assert ".slice(0, 34)" in source
    name_re = re.compile(r"^[a-z0-9](?:[a-z0-9-]{0,38}[a-z0-9])?$")
    assert name_re.fullmatch("alice-bob-carol-3f2a")


def test_direct_dialog_supports_add_mode():
    source = _function_source(CHAT, "openDirectMessageDialog")
    assert 'mode === "add"' in source
    assert "Add someone to this conversation" in source
    picker = _function_source(CHAT, "renderDirectMessagePicker")
    assert "user.name !== partner" in picker
    assert "addPersonToDirectMessage(user.name)" in picker


def test_empty_dm_query_opens_the_picker():
    assert 'chatQuery.has("dm") && !requestedDirect' in CHAT


def test_dashboard_shows_a_direct_message_strip():
    assert "function mountDirectMessageStrip()" in DASHBOARD_CHAT
    assert '"/api/chat/direct-messages?limit=20"' in DASHBOARD_CHAT
    assert "WORLD_EMBED_BUBBLES" in _function_source(
        DASHBOARD_CHAT, "mountDirectMessageStrip")
    assert '"/chat?dm=" + encodeURIComponent(name)' in DASHBOARD_CHAT
    assert '"forkmesh:direct-messages"' in DASHBOARD_CHAT
    assert "unreadCount" in DASHBOARD_CHAT


def test_dashboard_rebroadcasts_the_direct_messages_topic():
    assert 'topic === "direct-messages"' in DASHBOARD
    assert '"forkmesh:direct-messages"' in DASHBOARD


def test_chat_pages_allow_the_camera_for_calls():
    chat_section = HEADERS[HEADERS.index("\n/chat\n"):HEADERS.index(
        "\n/chat.html\n")]
    html_section = HEADERS[HEADERS.index("\n/chat.html\n"):]
    html_section = html_section[:html_section.index("\n\n", 2)]
    for section in (chat_section, html_section):
        assert "camera=(self)" in section
        assert "microphone=(self)" in section
        assert "mediastream:" in section
        assert "! Permissions-Policy" in section
