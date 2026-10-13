#!/usr/bin/env python3
"""Discord messages join the normal ForkMesh Messages timeline safely."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CHAT = (ROOT / "public" / "dashboard-chat.js").read_text(encoding="utf-8")
DISCORD = (ROOT / "src" / "organization_discord.py").read_text(encoding="utf-8")


def _region(start, end):
    return CHAT[CHAT.index(start):CHAT.index(end)]


def test_selected_discord_channels_are_loaded_into_the_normal_timeline():
    loader = _region(
        "async function refreshDiscordMessages()",
        "function startDiscordMessageRefresh()",
    )
    assert 'discordJson("/api/orgs")' in CHAT
    assert "/discord/messages?channelId=" in loader
    assert "appendDiscordMessage(" in loader
    assert "DISCORD_MAX_ORGANIZATIONS" in CHAT
    assert "DISCORD_MAX_CHANNELS" in CHAT
    assert "DISCORD_MAX_INITIAL_MESSAGES" in loader


def test_discord_entries_are_read_only_and_excluded_from_agent_context():
    append = _region(
        "function appendDiscordMessage(",
        "function appendSystem(",
    )
    assert "appendFullMessage(" in append
    assert "appendSideMessage(" in append
    assert "rememberContext" not in append
    materialize = _region(
        "function materializeFullMessage(record)",
        "function renderHistoryWindow(",
    )
    assert "record.external" in materialize
    assert "chat-message-source" in materialize
    assert "if (record.id && !record.external)" in materialize


def test_discord_refresh_is_foreground_only_and_rate_bounded():
    refresh = _region(
        "function startDiscordMessageRefresh()",
        "async function fetchMentionProfile(",
    )
    assert "document.visibilityState" in refresh
    assert "DISCORD_REFRESH_MS" in CHAT
    assert "DISCORD_REFRESH_JITTER_MS" in CHAT
    assert "discordBackoffUntil" in CHAT
    assert "scheduleDiscordMessageRefresh()" in refresh
    assert 'document.addEventListener("visibilitychange"' in refresh
    assert "stopDiscordMessageRefresh()" in refresh
    assert "discordBackoffAttempts" in CHAT
    assert "response.status === 429" in CHAT
    assert "response.status >= 500" in CHAT
    assert "5 * 60_000" in CHAT


def test_discord_discovery_stops_fanout_after_provider_throttling():
    discovery = _region(
        "async function discoverDiscordSources()",
        "async function refreshDiscordMessages()",
    )
    assert "for (const record of organizations)" in discovery
    assert "Promise.allSettled(organizations.map" not in discovery
    assert "error?.status === 429 || error?.status >= 500" in discovery


def test_discord_message_refresh_stops_channel_fanout_on_rate_limit():
    refresh = _region(
        "async function refreshDiscordMessages()",
        "function stopDiscordMessageRefresh()",
    )
    assert "for (const source of sources)" in refresh
    assert "Promise.allSettled(sources.map" not in refresh
    assert "if (error?.status === 429 || error?.status >= 500) throw error;" in refresh
    assert "const orderedMessages = messages" in refresh


def test_provider_bodies_remain_out_of_d1_and_channel_label_is_projected():
    assert '"contentStored": False' in DISCORD
    assert '"channel": {' in DISCORD
    assert '"name": _text(channel.get("name"), 100)' in DISCORD


def test_discord_messages_append_in_paint_separated_slices():
    refresh = _region(
        "async function refreshDiscordMessages()",
        "function stopDiscordMessageRefresh()",
    )
    loop = refresh[refresh.index("for (const message of visibleMessages)"):]
    loop = loop[: loop.index("discordBackoffAttempts = 0;")]
    assert "performance.now() - sliceStartedAt >= DISCORD_APPEND_SLICE_MS" in loop
    assert loop.index("await nextChatSlice();") < loop.index("appendDiscordMessage(message)")
    assert "const DISCORD_APPEND_SLICE_MS = 6;" in CHAT
    slice_helper = _region("function nextChatSlice()", "function pinFullLogToNewest()")
    assert "afterNextPaint(finish);" in slice_helper
    assert "setTimeout(finish, 100);" in slice_helper
