#!/usr/bin/env python3
"""Contracts for the compact ForkMesh Office encrypted chat embed."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HTML = (ROOT / "public" / "chat.html").read_text(encoding="utf-8")
CSS = (ROOT / "public" / "chat.css").read_text(encoding="utf-8")
CHAT = (ROOT / "public" / "chat.js").read_text(encoding="utf-8")


def test_office_embed_is_selected_before_first_paint():
    assert 'if (chatEmbed === "office" || chatEmbed === "world")' in HTML
    assert "document.documentElement.dataset.chatEmbed = chatEmbed;" in HTML
    assert 'chatQuery.get("embed") === "office"' in CHAT
    assert 'html[data-chat-embed="office"]' in CSS


def test_office_embed_keeps_chat_tools_and_hides_full_page_chrome():
    for selector in (
        ".chat-site-header",
        ".chat-people-pane",
        ".chat-e2e-note",
        ".chat-admin-action",
        "#chat-channel-dialog",
    ):
        assert f'html[data-chat-embed="office"] {selector}' in CSS
    for required in (
        'class="chat-rooms-pane"',
        'class="chat-main-pane"',
        'id="chat-status"',
        'id="chat-log"',
        'id="chat-input"',
        'id="chat-attachment-input"',
        'id="chat-send"',
        'input.addEventListener("paste"',
    ):
        assert required in HTML or required in CHAT


def test_office_embed_suspends_hidden_socket_and_notifies_parent():
    assert 'type: "office-chat-ready"' in CHAT
    assert 'message.type !== "office-chat-suspend"' in CHAT
    assert "chatSuspended" in CHAT
    assert "if (chatSuspended) return" in CHAT
    assert "event.origin !== window.location.origin" in CHAT
    assert "event.source !== window.parent" in CHAT


def test_office_embed_exposes_full_management_only_through_top_page():
    assert 'id="chat-office-manage"' in HTML
    assert 'target="_top"' in HTML
    assert "Manage channels in full chat" in HTML
    # Member management, webhooks and topics stay behind the top-page link;
    # the embed itself only ever offers channel creation.
    assert "channelManageBtn.hidden = !(session && channel?.canManage" in CHAT
    assert "&& !isOfficeEmbed)" in CHAT
    assert "officeManageLink.hidden = !(isOfficeEmbed && authorsChannels)" in (
        CHAT
    )


def test_office_embed_channel_creation_follows_organization_membership():
    assert "function canAuthorChannelsHere(" in CHAT
    assert "!isOfficeEmbed || officeOrganizationMember" in CHAT
    assert "async function loadOfficeOrganizationMembership(" in CHAT
    assert 'const OFFICE_FLOOR_ACCESS_ENDPOINT = "/api/world/office/floors"' in (
        CHAT
    )


def test_office_embed_has_actionable_session_and_relay_failures():
    assert (
        "Your session expired. Log in again to use authorized channels."
        in CHAT
    )
    assert "Chat relay unavailable. Try again." in CHAT
    assert 'id="chat-office-login"' in HTML
    assert 'id="chat-office-retry"' in HTML
    request = CHAT[
        CHAT.index("async function privateChannelRequest("):
        CHAT.index("async function fetchRoomAccess(")
    ]
    assert 'response.status === 401 ? "auth"' in request


def test_world_embed_shows_one_conversation_without_the_room_list():
    for selector in (".chat-site-header", ".chat-rooms-pane", ".chat-people-pane"):
        assert f'html[data-chat-embed="world"] {selector}' in HTML
    assert 'html[data-chat-embed="world"] .chat-panes' in HTML
    assert "const CHAT_DIRECT_HASH_RE = /^#direct=([0-9a-f]{32})$/;" in CHAT
