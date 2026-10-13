#!/usr/bin/env python3
"""New signups reach chat instantly + site-wide chat icon with unread badge.

adhoc #208: a user who signed up on the website was invisible in the chat
people pane until they happened to speak in the room, and nothing outside
/chat hinted there was anyone to welcome. Contracts pinned here:

  * The Worker exposes a cheap public /api/chat/activity counter digest
    (retained #general message count + registered user count) that never
    decrypts anything, and edge-caches it.
  * The public user directory (/api/accounts/users) is edge-cached too — the
    chat page now polls it — and a successful signup invalidates both caches
    so the new account is visible on the very next poll. That cache is
    edge-only: the response the browser receives is no-store (adhoc #434).
  * chat.js seeds/refreshes its roster from the directory and announces a
    freshly-created account in the log so the room can welcome them.
  * site-header.js renders a chat icon with an unread badge on every page
    that mounts the shared header, keyed off the same localStorage "seen"
    baseline the chat page maintains.
"""

import ast
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
WWW_PUBLIC = ROOT.parent / "www" / "public"
ENTRY = (
    (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
)
ACCOUNT_USERS = (ROOT / "src" / "account_users.py").read_text(
    encoding="utf-8")
CHAT = (ROOT / "public" / "chat.js").read_text(encoding="utf-8")
HEADER_JS = (WWW_PUBLIC / "site-header.js").read_text(encoding="utf-8")
HEADER_CSS = (WWW_PUBLIC / "site-header.css").read_text(encoding="utf-8")
DASHBOARD_CHAT = (ROOT / "public" / "dashboard-chat.js").read_text(
    encoding="utf-8")


def _function_source(name: str) -> str:
    for text in (ENTRY, ACCOUNT_USERS):
        tree = ast.parse(text)
        for node in tree.body:
            if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) and \
                    node.name == name:
                return ast.get_source_segment(text, node)
    raise AssertionError(f"Worker source must define {name}")


# --- Worker: /api/chat/activity ---------------------------------------------

def test_chat_activity_route_registered():
    assert '"/api/chat/activity"' in ENTRY
    assert "await chat_activity_handler(self.env, request)" in ENTRY


def test_chat_activity_counts_messages_without_decrypting():
    source = _function_source("chat_activity_handler")
    # The message count is still a bare tally: chat bodies stay ciphertext.
    assert "COUNT(*)" in source
    assert "FROM chat_history" in source
    assert "FLAGSHIP_ROOM_KEY" in source


def test_chat_activity_member_count_matches_world_roster():
    # adhoc #1617: the badge and the World campfire HUD used to run two
    # different filters over `users` (one raw "has an email" count, one a
    # decrypted active/user/non-private roster) and could disagree by a few
    # accounts. Both now go through the one shared counter/predicate.
    source = _function_source("chat_activity_handler")
    assert "_public_member_count(env)" in source
    assert "email_bi IS NOT NULL" not in source

    count_source = _function_source("_public_member_count")
    assert "_is_public_roster_member(rec)" in count_source

    directory_source = _function_source("_account_users_directory")
    assert "_is_public_roster_member(rec)" in directory_source


def test_chat_activity_is_edge_cached():
    source = _function_source("chat_activity_handler")
    assert "edge_cache_match(CHAT_ACTIVITY_CACHE_KEY)" in source
    assert "edge_cache_put(CHAT_ACTIVITY_CACHE_KEY" in source
    assert re.search(r"CHAT_ACTIVITY_CACHE_KEY\s*=", ENTRY)


def test_chat_activity_includes_deduplicated_membership_counts():
    source = _function_source("chat_activity_handler")
    assert "_public_membership_counts(env)" in source
    assert '"membership": membership' in source

    count_source = _function_source("_public_membership_counts")
    assert "supporting_member" in count_source
    assert "pro_member" in count_source
    assert "builder_member" in count_source
    assert "studio_member" in count_source
    # One provider-neutral source: the role grants a membership leaves.
    assert "role_grants" in count_source
    assert "pro - builder" in count_source
    assert "builder - studio" in count_source


# --- Worker: users directory caching + signup invalidation --------------------

def test_users_directory_is_edge_cached():
    source = _function_source("_account_users_directory")
    assert "_users_directory_cache_get()" in source
    assert "edge_cache_put(" in source
    assert "USERS_DIRECTORY_CACHE_KEY" in source


def test_users_directory_is_never_browser_cached():
    # The World campfire paints its member count from this body, so a browser
    # copy left the fire stale until a hard refresh (adhoc #434). The edge
    # copy still collapses origin work; the client bytes are no-store on both
    # the cache-hit and the rebuild path.
    assert re.search(
        r'USERS_DIRECTORY_CLIENT_CACHE_CONTROL\s*=\s*"no-store,', ENTRY)
    hit = _function_source("_users_directory_cache_get")
    assert '"cache-control": USERS_DIRECTORY_CLIENT_CACHE_CONTROL' in hit
    source = _function_source("_account_users_directory")
    assert "cache_control=USERS_DIRECTORY_CLIENT_CACHE_CONTROL" in source


def test_signup_invalidates_chat_caches():
    source = _function_source("_account_signup")
    assert "await _users_directory_cache_forget()" in source
    assert "edge_cache_delete(CHAT_ACTIVITY_CACHE_KEY)" in source


# --- chat.js: directory-fed roster + welcome line -----------------------------

def test_chat_page_seeds_roster_from_directory():
    assert '"/api/accounts/users"' in CHAT
    assert "refreshUsersDirectory" in CHAT
    # Directory entries are namespaced so they can never collide with (or
    # shadow) a live senderId roster entry for the same account.
    assert '"account:" + name' in CHAT


def test_chat_page_polls_and_announces_new_signups():
    assert "USERS_DIRECTORY_REFRESH_MS" in CHAT
    assert "just joined ForkMesh" in CHAT
    # Only genuinely fresh accounts are announced; a name merely missing from
    # the previous directory page is not news.
    assert "NEW_USER_ANNOUNCE_WINDOW_MS" in CHAT


def test_chat_page_maintains_seen_baseline():
    assert '"/api/chat/activity"' in CHAT or "CHAT_ACTIVITY_ENDPOINT" in CHAT
    assert "forkmesh.chat.activitySeen" in CHAT


# --- site-header.js: chat icon + unread badge ---------------------------------

def test_header_renders_chat_icon_linking_to_chat():
    assert 'class="fm-header-chat" href="/chat"' in HEADER_JS
    assert "fm-header-chat-badge" in HEADER_JS


def test_header_badge_uses_activity_counters_and_shared_baseline():
    assert '"/api/chat/activity"' in HEADER_JS
    # Same localStorage key the chat page writes, so opening /chat clears the
    # badge everywhere.
    assert "forkmesh.chat.activitySeen" in HEADER_JS
    assert "renderChatBadge" in HEADER_JS
    # First visit (no baseline) and the chat page itself seed silently rather
    # than badging the entire room history.
    assert "writeChatSeen(activity)" in HEADER_JS
    assert '"999+"' in HEADER_JS


def test_header_badge_clears_when_general_was_read_on_another_device():
    assert 'latestMessageTs: Number(data.latestMessageTs) || 0' in HEADER_JS
    assert 'const CHAT_READS_ENDPOINT = "/api/chat/reads";' in HEADER_JS
    assert 'const CHAT_GENERAL_READ_KEY = "#general";' in HEADER_JS
    assert 'credentials: "include"' in HEADER_JS
    assert "mark.at >= activity.latestMessageTs" in HEADER_JS
    assert ("mark.ts >= activity.latestMessageTs - CHAT_READ_CLOCK_SLACK_MS"
            in HEADER_JS)
    cleared = HEADER_JS.index(
        "if (unread > 0 && readCoversActivity(await readGeneralReadMark(), "
        "activity)) {")
    assert "writeChatSeen(activity);" in HEADER_JS[cleared:cleared + 200]
    assert 'document.addEventListener("visibilitychange"' in HEADER_JS


def test_header_css_styles_icon_and_badge():
    assert ".fm-header-chat {" in HEADER_CSS
    assert ".fm-header-chat-badge {" in HEADER_CSS


# --- own messages never count as unread --------------------------------------

def test_own_messages_advance_the_header_baseline():
    # adhoc #426: the badge is a delta against the stored baseline, so a line
    # this browser contributed to #general showed up as unread on every other
    # page of the site. Sending advances the baseline by one instead.
    assert "function noteSeenChatActivity()" in CHAT
    assert "noteSeenChatActivity();" in CHAT
    # Same baseline bump for a line that ARRIVES while the page is open — that
    # is what let the 60s re-read of /api/chat/activity go away. Replayed
    # history is excluded: the read at page open already counted it.
    assert 'type !== "history" &&' in CHAT
    assert "DURABLE_TYPES.has(type) &&" in CHAT
    # Only retained public-world frames reach the counter the badge reads; a
    # private channel, a DM, or an unretained oversized file frame must not
    # move the baseline.
    assert (
        'if (envelope.persist && scope === "public-world-general" '
        "&& !plain.file) {"
    ) in CHAT
    # Without an existing baseline the header still seeds one silently.
    assert "if (!raw) return;" in CHAT
    # The World and dashboard talk to the same room through their own client,
    # so it keeps the same baseline (this is where the World's CHAT bar sends).
    assert "function noteOwnChatActivity()" in DASHBOARD_CHAT
    assert 'const CHAT_ACTIVITY_SEEN_KEY = "forkmesh.chat.activitySeen";' in (
        DASHBOARD_CHAT
    )
    assert (
        "if (envelope.persist && PUBLIC_WORLD_GENERAL && !plain.file) {"
    ) in DASHBOARD_CHAT


def test_own_account_messages_never_bump_a_room_badge():
    # selfId is per-browser, so the same account talking from another tab,
    # phone, or the desktop client used to light an unread badge for a message
    # the user had just typed. The account name on the frame settles it.
    assert "function isOwnChatMessage(record)" in CHAT
    assert "} else if (!isOwnChatMessage(record)) {" in CHAT
    assert 'String(userSession()?.nodeName || "").trim().toLowerCase()' in CHAT
    # Weaker identity check, so it gates unread bookkeeping only — editing and
    # deleting still require the senderId match.
    assert "record.senderId !== selfId" in CHAT
