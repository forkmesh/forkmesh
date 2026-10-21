#!/usr/bin/env python3
"""The website chat shares the same retained history as the desktop nodes.

The relay only retains (and later replays to joining clients) a room frame whose
envelope carries `persist: true` — see ForkMeshRoom._maybe_retain. The desktop
node tags its durable frames via kDurableTypes (ServerNode::sendEncrypted). If
the website's dashboard chat omits the flag, a message typed on the site is
relayed live but never becomes part of the shared history, so nodes and later
web visitors never see it — leaving the website out of the shared chat. This
contract pins the website tagging the same durable set the node does.
"""

import json
from pathlib import Path
import re
import subprocess


PUBLIC = Path(__file__).resolve().parents[1] / "public"
WWW_PUBLIC = PUBLIC.parents[1] / "www" / "public"
CHAT = (PUBLIC / "dashboard-chat.js").read_text(encoding="utf-8")
PUBLIC_CHAT = (PUBLIC / "chat.js").read_text(encoding="utf-8")
DASHBOARD_HTML = (PUBLIC / "dashboard" / "chat" / "index.html").read_text(encoding="utf-8")
PUBLIC_CHAT_HTML = (PUBLIC / "chat.html").read_text(encoding="utf-8")
PUBLIC_CHAT_CSS = (PUBLIC / "chat.css").read_text(encoding="utf-8")
CHAT_VIEW = (
    PUBLIC / "dashboard" / "partials" / "views" / "chat.html"
).read_text(encoding="utf-8")
STYLES = (WWW_PUBLIC / "styles.css").read_text(encoding="utf-8")


def test_send_marks_durable_frames_for_relay_retention():
    send = CHAT[CHAT.index("function send(") : CHAT.index("async function connect(")]
    # The durable frame gets the persist flag the relay checks before retaining.
    assert "envelope.persist = true" in send
    assert "DURABLE_TYPES.has(" in send


def test_public_chat_marks_durable_frames_for_relay_retention():
    send = PUBLIC_CHAT[
        PUBLIC_CHAT.index("function send("):PUBLIC_CHAT.index("function makeForkbotPlain(")
    ]
    assert "envelope.persist = true" in send
    assert "DURABLE_TYPES.has(" in send


def test_replayed_self_messages_are_rendered_after_refresh():
    for source in (CHAT, PUBLIC_CHAT):
        on_frame = source[
            source.index("async function onFrame("):
            source.index("const DURABLE_TYPES", source.index("async function onFrame("))
        ]
        assert "plain.senderId === selfId" not in on_frame

        render_entry = source[
            source.index("function renderChatEntry("):
            source.index("async function verifyAdminDelete(")
        ]
        assert 'entry.senderId === selfId ? "self" : kind' in render_entry


def test_dashboard_history_starts_at_five_and_reveals_viewport_sized_batches():
    assert "const HISTORY_INITIAL_MESSAGES = 5;" in CHAT
    assert "const HISTORY_MIN_BATCH_MESSAGES = 5;" in CHAT
    assert "function historyBatchMessages()" in CHAT
    assert "function renderHistoryWindow(" in CHAT
    assert "function revealOlderHistory(" in CHAT
    assert "row.dataset.chatHistory" in CHAT
    assert "function materializeFullMessage(record)" in CHAT
    assert "if (deferHistory && id)" in CHAT
    assert "historyRowIds.push(id);" in CHAT
    assert "historyReplayEnvelopes.push(envelope);" in CHAT
    assert 'envelope?.kind === "forkmesh-history-end"' in CHAT
    assert "envelopes.map((envelope) => decryptObject(envelope, true))" in CHAT
    assert "handlePlain(plain, true)" in CHAT
    assert 'if (plain.type === "chat") handlePlain(plain, true);' in CHAT
    assert 'if (plain.type !== "chat") handlePlain(plain, true);' in CHAT
    assert "scroll up to load ${nextBatch}" in CHAT
    assert "Beginning of conversation" in CHAT
    assert "fullLog.scrollTop += Math.max(" in CHAT
    assert "function hasHiddenHistory()" in CHAT
    assert "function scheduleHistoryWheelReset()" in CHAT
    assert "!historyWheelLatched" in CHAT
    assert "!historyTouchRevealed" in CHAT
    history = CHAT[
        CHAT.index('} else if (type === "history")'):
        CHAT.index('} else if (type === "reaction")')
    ]
    assert "finishHistoryReplay();" in history


def test_world_embed_chat_reads_oldest_first_newest_last():
    # adhoc #93: the world embed rendered its feed newest-first (history sorted
    # descending, indicator appended, scroll parked at the top), so the latest
    # line sat above older ones. Both modes now read like every other chat.
    assert "records.sort((left, right) => left.tsMs - right.tsMs)" in CHAT
    assert "fullLog.prepend(historyIndicator);" in CHAT
    assert "Number(candidate.dataset.chatTimestamp || 0) > record.tsMs" in CHAT
    assert "simpleWorldComposer ? 0 : fullLog.scrollHeight" not in CHAT
    assert "index < visibleCount" not in CHAT


def test_durable_type_set_matches_the_node():
    # Same set as the desktop node's kDurableTypes (ServerNode.cpp).
    for kind in (
        "chat", "thread-reply", "edit", "delete", "reaction", "admin-delete"
    ):
        assert f'"{kind}"' in CHAT[CHAT.index("DURABLE_TYPES") : CHAT.index("function send(")]
        assert f'"{kind}"' in PUBLIC_CHAT[
            PUBLIC_CHAT.index("DURABLE_TYPES"):PUBLIC_CHAT.index("function send(")
        ]


def test_dashboard_chat_allows_guests_only_in_public_world_general():
    assert "function userSession()" in CHAT
    assert "function isUserLikeSession(session)" in CHAT
    assert 'session.kind === "user"' in CHAT
    assert 'session.kind === "node"' in CHAT
    assert "return Boolean(session.email)" in CHAT
    assert "async function hydrateUserSession()" in CHAT
    assert 'fetch("/api/accounts/" + encodeURIComponent(session.nodeName)' in CHAT
    assert 'PUBLIC_WORLD_GENERAL_ROOM = "world-general"' in CHAT
    assert (
        "PUBLIC_WORLD_GENERAL = !ACTIVE_SPACE && !requestedOrganization"
    ) in CHAT
    assert "return PUBLIC_WORLD_GENERAL || Boolean(userSession())" in CHAT
    assert 'return PUBLIC_WORLD_GENERAL && !userSession() ? "guest" : "user"' in CHAT
    assert 'value === "user" || (PUBLIC_WORLD_GENERAL && value === "guest")' in CHAT
    assert "function normalizedPublicWorldFrame(entry)" in CHAT
    assert 'accountKind: entry.accountKind === "user" ? "user" : "guest"' in CHAT
    assert "sender: publicWorldName(entry.sender)" in CHAT
    # Public-room names are shown as asserted - no "World visitor - jett".
    assert "World visitor · ${" not in CHAT
    assert 'function publicWorldName(value)' in CHAT
    assert "Guests can use only public World #general" in CHAT
    assert "`&room=${encodeURIComponent(ACTIVE_ROOM)}`" in CHAT
    assert "`/${encodeURIComponent(ROOM_REPO)}/rooms/${encodeURIComponent(ACTIVE_ROOM)}/ws`" in CHAT


def test_public_chat_splits_guest_general_from_authenticated_channels():
    assert "function userSession()" in PUBLIC_CHAT
    assert "function isUserLikeSession(session)" in PUBLIC_CHAT
    assert 'session.kind === "user"' in PUBLIC_CHAT
    assert 'session.kind === "node"' in PUBLIC_CHAT
    assert "return Boolean(session.email)" in PUBLIC_CHAT
    assert "async function hydrateUserSession()" in PUBLIC_CHAT
    assert 'fetch("/api/accounts/" + encodeURIComponent(session.nodeName)' in PUBLIC_CHAT
    assert 'PUBLIC_WORLD_GENERAL_ROOM = "world-general"' in PUBLIC_CHAT
    assert 'channel === "#general"' in PUBLIC_CHAT
    assert '"public-world-general"' in PUBLIC_CHAT
    assert 'normalized !== "#general"' in PUBLIC_CHAT
    assert (
        'roomScopeForChannel() === "public-world-general" && !userSession()'
        in PUBLIC_CHAT
    )
    assert 'scope === "public-world-general" && value === "guest"' in PUBLIC_CHAT
    assert "function normalizedPublicWorldFrame(" in PUBLIC_CHAT
    assert 'accountKind: plain.accountKind === "user" ? "user" : "guest"' in PUBLIC_CHAT
    assert "sender: publicWorldName(plain.sender)" in PUBLIC_CHAT
    # Public-room names are shown as asserted - no "World visitor - jett".
    assert "World visitor · ${" not in PUBLIC_CHAT
    assert "function publicWorldName(value)" in PUBLIC_CHAT
    assert "PUBLIC_WORLD_ROOM_KEY_ENDPOINT" in PUBLIC_CHAT
    assert "PRIVATE_CHANNELS_ENDPOINT" in PUBLIC_CHAT
    assert "fetchRoomAccess" in PUBLIC_CHAT
    assert "PUBLIC_WORLD_CHAT_WS_PATH" in PUBLIC_CHAT
    assert "access.webSocketUrl" in PUBLIC_CHAT
    assert "switchChatRoom" in PUBLIC_CHAT
    assert "frameMatchesScope" in PUBLIC_CHAT
    assert "Guests can chat only in World" in PUBLIC_CHAT
    assert "lockChatForNonUser" in PUBLIC_CHAT


def test_public_chat_uses_shared_site_theme_keys():
    assert '"forkmesh.dashboard.theme", "forkmesh.theme"' in PUBLIC_CHAT_HTML
    assert 'meta name="color-scheme" content="light dark"' in PUBLIC_CHAT_HTML
    assert "html:not(.dark)" in PUBLIC_CHAT_CSS
    assert 'theme === "light" ? "#f6f8fa" : "#090909"' in PUBLIC_CHAT_HTML
    assert "--bg: #080d12;" in PUBLIC_CHAT_CSS
    assert "--surface: #0c1117;" in PUBLIC_CHAT_CSS
    assert "--fg: #e6edf3;" in PUBLIC_CHAT_CSS


def test_web_chat_mentions_link_to_public_profiles_with_hover_cards():
    for source in (CHAT, PUBLIC_CHAT):
        assert "const CHAT_MENTION_RE" in source
        assert "function renderMessageText" in source
        # The login snapshot is already the signed-in user's complete profile;
        # rendering their avatar or mention must not look that account up again.
        assert "Number(session.accountSnapshotVersion) >= 1" in source
        assert "const ownProfile = Promise.resolve(session);" in source
        assert 'fetch("/api/accounts/" + encodeURIComponent(key)' in source
        assert 'anchor.href = mentionProfilePath(name)' in source
        assert "anchor.dataset.chatMention = name" in source
        assert 'mentionCardEl.className = "chat-mention-card"' in source
        assert "showMentionCard(anchor, name)" in source

    assert "function appendMentionText" in CHAT
    assert "function makeMentionAnchor" in PUBLIC_CHAT
    assert "renderRichText(container, record" in PUBLIC_CHAT
    assert "renderRecordBody(rec)" in PUBLIC_CHAT
    # An edited message is re-rendered through the mention renderer (the record
    # keeps the new text so a later edit starts from it), never set as raw
    # textContent, so mentions stay linked after an edit.
    assert "rec.text = plain.text || \"\"" in CHAT
    assert "renderMessageText(rec.textEl, rec.text)" in CHAT
    assert "sideEntry.text = plain.text || \"\"" in CHAT


def test_dashboard_chat_composer_offers_mention_autocomplete():
    # Typing "@partial" in the dashboard composer pops a suggestion list built
    # from people seen in the room plus the registered-user directory; Tab (or
    # Enter / click) accepts, arrows navigate, Escape dismisses. The token
    # charset matches CHAT_MENTION_RE so an accepted mention renders linked.
    assert "function mentionTokenAtCaret(" in CHAT
    assert "function mentionCandidates(" in CHAT
    assert "function acceptMentionSuggest(" in CHAT
    assert "function rememberMentionPerson(" in CHAT
    assert 'const MENTION_DIRECTORY_ENDPOINT = "/api/accounts/users"' in CHAT
    assert 'event.key === "Tab" || event.key === "Enter"' in CHAT
    assert "acceptMentionSuggest()" in CHAT
    assert 'inputEl.addEventListener("input", () => updateMentionSuggest(inputEl))' in CHAT
    # ForkBot answers a "@forkbot" mention, so it is always suggestable.
    assert "byName.set(FORKBOT_SENDER_ID" in CHAT
    # The mention list owns Enter while it is open, so accepting a name must not
    # also send the half-typed message.
    wire = CHAT[CHAT.index("function wireInput("):CHAT.index("async function initChat(")]
    assert wire.index("acceptMentionSuggest()") < wire.index(
        "sendFrom(inputEl, attachmentControl);\n      }"
    )


def test_suggestion_rows_are_pointer_selectable_and_hover_never_rerenders():
    # Clicking a suggestion has to insert the name on every surface, not just
    # Tab. Two things broke that. The hover handler re-rendered the whole list,
    # which detached the row under the cursor; the browser then re-fired
    # mouseenter on its replacement on the next frame, so the row was rebuilt
    # continuously and a press landed on a node that was already gone. And
    # selection listened for "mousedown" alone, which leaves pen and touch to
    # whatever synthetic mouse events the engine happens to emit.
    for source in (PUBLIC_CHAT, CHAT):
        # Hover moves the highlight in place; only a changed item list rebuilds.
        assert "function highlightMentionSuggest(" in source
        assert "function highlightSlashSuggest(" in source
        assert "highlightMentionSuggest(index, false)" in source
        assert "highlightSlashSuggest(index, false)" in source
        for handler in re.findall(
            r'addEventListener\("mouseenter",(.{0,160})', source, re.S
        ):
            assert "renderMentionSuggest" not in handler
            assert "renderSlashSuggest" not in handler
        # Arrow keys reuse the same in-place highlight, and scroll to follow.
        assert "highlightMentionSuggest(mentionSuggest.index + step, true)" in source
        assert "highlightSlashSuggest(slashSuggest.index + step, true)" in source
        # One pick path, wired for pointer, mouse, and click.
        assert "function wireSuggestPick(item, accept)" in source
        assert 'item.addEventListener("pointerdown", pick);' in source
        assert 'item.addEventListener("mousedown", pick);' in source
        assert 'item.addEventListener("click", pick);' in source
        assert "wireSuggestPick(item, () => acceptMentionSuggest(index));" in source
        assert "wireSuggestPick(item, () => acceptSlashSuggest(index));" in source
        # preventDefault keeps focus in the composer, so no blur closes the
        # popup out from under the press.
        assert "if (event.cancelable) event.preventDefault();" in source
    # Accepting is a no-op once the popup has closed, so the mousedown and click
    # that follow a pointerdown cannot insert a second name.
    assert "function acceptMentionSuggest(index) {\n  if (!mentionSuggest) return;" \
        in PUBLIC_CHAT
    assert "function acceptMentionSuggest(index) {\n    if (!mentionSuggest) return;" \
        in CHAT
    # An accepted mention grows the composer like every other insertion does.
    accept = PUBLIC_CHAT[
        PUBLIC_CHAT.index("function acceptMentionSuggest("):
        PUBLIC_CHAT.index("function highlightMentionSuggest(")
    ]
    assert "resizeComposer();" in accept


def test_dashboard_enter_sends_without_blocking_shift_enter_newlines():
    wire = CHAT[CHAT.index("function wireInput("):CHAT.index("async function initChat(")]
    assert 'event.key === "Enter" && !event.shiftKey' in wire


def test_dashboard_picker_and_paste_attachments_send_without_an_extra_click():
    picker = CHAT[
        CHAT.index('fileInput.addEventListener("change"'):
        CHAT.index("control.button.disabled", CHAT.index('fileInput.addEventListener("change"'))
    ]
    paste = CHAT[
        CHAT.index('inputEl.addEventListener("paste"'):
        CHAT.index('inputEl.addEventListener("keydown"')
    ]
    # Both surfaces send inside the staging promise chain — staging is async,
    # so an unchained send would read an empty draft — with no second user
    # action needed.
    assert "return sendDashboardDraft(control);" in picker
    assert "return sendDashboardDraft(attachmentControl);" in paste


def test_chat_mention_styles_are_available_on_all_chat_surfaces():
    for source in (DASHBOARD_HTML, PUBLIC_CHAT_CSS, STYLES):
        assert ".chat-mention {" in source
        assert ".chat-mention-card {" in source
        assert ".chat-mention-card[hidden]" in source


# --- /chat three-pane layout (rooms | conversation | people) -----------------

def test_public_chat_has_rooms_conversation_and_people_panes():
    assert 'id="chat-rooms"' in PUBLIC_CHAT_HTML
    assert 'id="chat-people"' in PUBLIC_CHAT_HTML
    assert 'id="chat-channel-title"' in PUBLIC_CHAT_HTML
    assert ".chat-rooms-pane" in PUBLIC_CHAT_CSS
    assert ".chat-people-pane" in PUBLIC_CHAT_CSS
    # Sends carry the selected display label while private buffers and room
    # access stay keyed by the server-provided opaque channel id.
    assert "channel: channelWireLabel(activeChannel)" in PUBLIC_CHAT
    assert "channel: channelDisplayLabel(activeChannel)" not in PUBLIC_CHAT
    assert "function setActiveChannel(" in PUBLIC_CHAT
    assert 'const PRIVATE_CHANNELS_ENDPOINT = "/api/chat/channels"' in PUBLIC_CHAT
    assert 'DEFAULT_CHANNELS = ["#general", "#welcome", "#random"]' not in PUBLIC_CHAT


def test_dashboard_public_room_links_to_private_channel_directory():
    assert "function mountPrivateChannelsLink(" in CHAT
    assert 'link.href = "/chat"' in CHAT
    assert 'link.textContent = "Open private channels"' in CHAT


def test_public_chat_sends_presence_keepalive_at_desktop_cadence():
    # The room DO reaps sockets with no frames for 3 minutes; the presence beat
    # (same 60s cadence as ServerNode's kPresenceIntervalMs) keeps the web
    # client connected AND keeps its roster entry fresh for peers. The old web
    # client sent nothing while idle and kept getting disconnected.
    assert "PRESENCE_INTERVAL_MS = 60000" in PUBLIC_CHAT
    assert "PEER_STALE_MS = 180000" in PUBLIC_CHAT  # ServerNode kPeerStaleMs
    assert 'send(makePlain("presence"))' in PUBLIC_CHAT
    assert "function scheduleReconnect(" in PUBLIC_CHAT


def test_public_chat_reactions_speak_the_desktop_protocol():
    # Same frame shape as ServerNode::sendReaction so toggles converge across
    # web and desktop clients, and reactions persist for late joiners (the
    # "reaction" kind is already in DURABLE_TYPES).
    reaction = PUBLIC_CHAT[
        PUBLIC_CHAT.index("function toggleReaction("):
        PUBLIC_CHAT.index("function ensureEmojiPicker(")
    ]
    assert 'makePlain("reaction"' in reaction
    assert "conversation:" in reaction
    assert "target: messageId" in reaction
    assert "reactorId: selfId" in reaction
    assert "reactorName: displayName()" in reaction
    assert "added: !mine" in reaction
    assert ".chat-reaction-chip" in PUBLIC_CHAT_CSS
    assert ".chat-emoji-picker" in PUBLIC_CHAT_CSS


def test_public_chat_people_pane_tracks_online_status_from_frame_ts():
    # Presence derives from each frame's own timestamp so replayed history
    # can't paint a days-old author as online, and an old frame never demotes
    # a peer heard from more recently.
    assert "function noteRoster(" in PUBLIC_CHAT
    assert "Math.min(Number(plain.ts) || Date.now(), Date.now())" in PUBLIC_CHAT
    assert "function personIsOnline(" in PUBLIC_CHAT
    assert "chat-presence-dot" in PUBLIC_CHAT
    assert ".chat-presence-dot.is-online" in PUBLIC_CHAT_CSS


def test_public_chat_orders_messages_by_ts_with_avatars_and_time():
    assert "function insertMessage(" in PUBLIC_CHAT
    assert "function buildRow(" in PUBLIC_CHAT
    assert "function makeAvatar(" in PUBLIC_CHAT
    assert "function fmtTime(" in PUBLIC_CHAT
    assert "GROUP_WINDOW_MS" in PUBLIC_CHAT
    assert ".chat-avatar" in PUBLIC_CHAT_CSS
    assert ".chat-time" in PUBLIC_CHAT_CSS


def test_chat_times_use_compact_12_hour_clock():
    for source in (CHAT, PUBLIC_CHAT):
        assert 'toLocaleTimeString("en-US", {' in source
        assert 'hour: "numeric", minute: "2-digit", hour12: true,' in source
        assert '.toLowerCase().replace(/\\s+/g, "")' in source


def test_public_chat_mention_autocomplete_accepts_with_tab():
    # Typing "@partial" pops a roster-backed suggestion list; Tab (or Enter /
    # click) accepts, arrows navigate, Escape dismisses. The token charset
    # matches CHAT_MENTION_RE so an accepted mention always renders linked.
    assert "function mentionTokenAtCaret(" in PUBLIC_CHAT
    assert "function mentionCandidates(" in PUBLIC_CHAT
    assert "function acceptMentionSuggest(" in PUBLIC_CHAT
    assert 'event.key === "Tab" || event.key === "Enter"' in PUBLIC_CHAT
    assert "acceptMentionSuggest()" in PUBLIC_CHAT
    assert 'input.addEventListener("input", () => {' in PUBLIC_CHAT
    assert "resizeComposer();\n    updateMentionSuggest();" in PUBLIC_CHAT
    assert ".chat-mention-suggest" in PUBLIC_CHAT_CSS
    assert ".chat-mention-suggest-item.is-active" in PUBLIC_CHAT_CSS


def test_public_chat_usernames_link_to_relay_profile_pages():
    # Clicking a message author, their avatar, or a people-pane row opens
    # the canonical public origin. Official App/WWW/World surfaces converge on
    # forkmesh.com; a self-hosted relay retains its own origin. ForkBot and
    # unverified World guests stay unlinked.
    assert 'const origin = official ? "https://forkmesh.com" : location.origin;' \
        in PUBLIC_CHAT
    assert 'return origin + "/@" + encodeURIComponent(key);' in PUBLIC_CHAT
    assert "author.href = mentionProfilePath(record.sender)" in PUBLIC_CHAT
    assert "avatarLink.href = mentionProfilePath(record.sender)" in PUBLIC_CHAT
    assert "row.href = mentionProfilePath(person.name)" in PUBLIC_CHAT
    assert 'document.createElement(isBot ? "span" : "a")' in PUBLIC_CHAT
    assert 'const hasProfile = person.kind === "user"' in PUBLIC_CHAT
    assert 'document.createElement(hasProfile ? "a" : "div")' in PUBLIC_CHAT
    assert "a.chat-author:hover" in PUBLIC_CHAT_CSS
    assert "a.chat-person:hover" in PUBLIC_CHAT_CSS


def test_public_chat_thread_authors_link_to_profile_pages():
    header = PUBLIC_CHAT[
        PUBLIC_CHAT.index("function threadMessageHeader(record) {"):
        PUBLIC_CHAT.index("function renderThreadRoot(record) {")
    ]
    assert 'const author = document.createElement("strong");' in header
    assert "if (record.senderId === FORKBOT_SENDER_ID) {" in header
    assert 'authorLink.className = "chat-author";' in header
    assert "authorLink.href = mentionProfilePath(record.sender);" in header
    assert "showMentionCard(authorLink, record.sender)" in header
    assert "author.append(authorLink);" in header


def test_dashboard_chat_authors_link_to_profile_pages():
    helper = CHAT[
        CHAT.index("function authorProfileName(record) {"):
        CHAT.index("async function fetchMentionProfile(name) {")
    ]
    assert 'record.external || record.accountKind !== "user"' in helper
    assert "/^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$/" in helper
    assert "FORKBOT_SENDER_ID, CLAUDE_SENDER_ID, CODEX_SENDER_ID, ORG_BOT_SENDER_ID" in helper
    assert 'replace(/^@/, "")' in helper
    assert 'href="${escapeHtml(mentionProfilePath(name))}"' in helper
    assert "hover:underline" in helper
    assert "data-chat-author-profile" in helper
    assert "showMentionCard(element, name)" in helper
    assert (
        '${authorElementHtml(record, `chat-message-author text-xs font-semibold '
        in CHAT
    )
    assert 'bindAuthorProfileCard(row.querySelector(".chat-message-author"));' in CHAT
    assert "chat-side-message-author" in CHAT
    assert 'bindAuthorProfileCard(row.querySelector(".chat-side-message-author"));' in CHAT
    card = CHAT[CHAT.index("function threadCard(record, root = false) {"):]
    card = card[:card.index("\n  }\n")]
    assert 'document.createElement(profileName ? "a" : "strong")' in card
    assert "author.href = mentionProfilePath(profileName);" in card
    assert 'accountKind: String(entry.accountKind || ""),' in CHAT
    assert 'accountKind: String(metadata?.accountKind || (self ? chatAccountKind() : "")),' in CHAT
    assert 'String(entry.accountKind || ""),\n    );' in CHAT


def test_dashboard_chat_author_links_only_cover_user_accounts():
    helpers = (
        CHAT[CHAT.index("  function escapeHtml(value) {"):CHAT.index("  function mentionName(value) {")]
        + CHAT[CHAT.index("  function mentionName(value) {"):CHAT.index("  async function fetchMentionProfile(name) {")]
    )
    script = (
        "globalThis.location = { hostname: 'world.forkmesh.com', origin: 'https://world.forkmesh.com' };\n"
        "const FORKBOT_SENDER_ID = 'forkbot', CLAUDE_SENDER_ID = 'claude', "
        "CODEX_SENDER_ID = 'codex', ORG_BOT_SENDER_ID = 'agent';\n"
        + helpers
        + "\nconst cases = {"
        "user: { who: 'Jett', senderId: 'p1', accountKind: 'user' },"
        "at: { who: '@dana', senderId: 'p2', accountKind: 'user' },"
        "guest: { who: 'jett', senderId: 'p3', accountKind: 'guest' },"
        "discord: { who: 'jett', senderId: 'discord:1', accountKind: 'user', external: true },"
        "bot: { who: 'forkbot', senderId: 'forkbot', accountKind: 'user' },"
        "agent: { who: 'jett', senderId: 'agent', accountKind: 'user' },"
        "suffixed: { who: 'helper[bot]', senderId: 'p4', accountKind: 'user' },"
        "spaced: { who: 'Jett Smith', senderId: 'p5', accountKind: 'user' },"
        "hostile: { who: '<b>x</b>', senderId: 'p6', accountKind: 'user' },"
        "};\n"
        "const out = {};\n"
        "for (const [key, record] of Object.entries(cases)) "
        "out[key] = authorElementHtml(record, 'chat-message-author');\n"
        "console.log(JSON.stringify(out));\n"
    )
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True, capture_output=True, text=True, timeout=30)
    out = json.loads(result.stdout)
    assert out["user"] == (
        '<a class="chat-message-author hover:underline" '
        'href="https://forkmesh.com/@jett" data-chat-author-profile="jett" '
        'data-user-status-name="Jett">Jett</a>')
    assert 'href="https://forkmesh.com/@dana"' in out["at"]
    for key in ("guest", "discord", "bot", "agent", "suffixed", "spaced", "hostile"):
        assert out[key].startswith('<span class="chat-message-author"'), key
        assert "href=" not in out[key], key
    assert "&lt;b&gt;x&lt;/b&gt;" in out["hostile"]


def test_dashboard_side_chat_orders_by_ts_with_avatar_and_time():
    # The rail's mini chat renders like the full chat — avatar + name + time —
    # and inserts each message by its own timestamp so replayed history and
    # live traffic interleave with the newest at the bottom.
    assert "function fmtChatTime(" in CHAT
    assert "tsMs: Number(tsMs) || Date.now()" in CHAT
    assert "sideEntries.splice(index, 0, entry)" in CHAT
    assert "fmtChatTime(message.tsMs)" in CHAT
    assert "hydrateChatAvatar(row.querySelector" in CHAT
    # Call sites hand the epoch timestamp through (formatting happens at
    # render), so ordering never depends on arrival order.
    render_entry = CHAT[
        CHAT.index("function renderChatEntry("):
        CHAT.index("async function verifyAdminDelete(")
    ]
    for marker in (
        "appendMessage(",
        "entry.id,",
        "entry.senderId,",
        "Number(entry.ts) || Date.now(),",
        "attachment,",
    ):
        assert marker in render_entry


def test_dashboard_side_chat_keeps_its_socket_alive_and_reconnects():
    # The room DO reaps sockets that send nothing for 3 minutes; the dashboard
    # chat used to go silently stale on idle tabs (no keepalive, no reconnect)
    # so new messages just stopped arriving. Mirrors the /chat page fix: 60s
    # presence beat (desktop cadence) + reconnect with backoff.
    assert 'send(makePlain("presence"))' in CHAT
    assert "}, 60000);" in CHAT
    assert "function scheduleReconnect(" in CHAT
    assert "reconnectDelayMs = Math.min(reconnectDelayMs * 2, 30000);" in CHAT
    close_handler = CHAT[
        CHAT.index('socket.addEventListener("close"'):
        CHAT.index('socket.addEventListener("error"')
    ]
    assert "scheduleReconnect();" in close_handler


def test_world_embedded_dashboard_chat_removes_redundant_dashboard_chrome():
    assert 'params.get("worldEmbed") !== "1"' in CHAT_VIEW
    assert 'document.documentElement.dataset.worldEmbed = "1"' in CHAT_VIEW
    assert 'html[data-world-embed="1"] [data-app-header]' in CHAT_VIEW
    assert 'html[data-world-embed="1"] [data-dashboard-sidebar]' in CHAT_VIEW
    assert 'html[data-world-embed="1"] [data-mobile-sidebar-backdrop]' in CHAT_VIEW
    assert "display: none !important" in CHAT_VIEW
    assert "data-dashboard-chat-view" in CHAT_VIEW
    assert "data-dashboard-chat-composer" in CHAT_VIEW


def test_world_embedded_dashboard_chat_tracks_mobile_keyboard_viewport():
    viewport_sync = CHAT_VIEW[
        CHAT_VIEW.index("const syncWorldEmbedViewport = () => {"):
        CHAT_VIEW.index("syncWorldEmbedViewport();")
    ]
    assert "const layoutHeight = window.innerHeight;" in viewport_sync
    assert "const visualHeight = window.visualViewport?.height;" in viewport_sync
    assert (
        "window.frameElement?.getBoundingClientRect?.().height"
        in viewport_sync
    )
    assert "const candidates = [" in viewport_sync
    assert "layoutHeight," in viewport_sync
    assert "visualHeight," in viewport_sync
    assert "frameHeight," in viewport_sync
    assert "Math.min(...candidates)" in viewport_sync
    assert '"--forkmesh-chat-viewport-height"' in viewport_sync
    viewport_listeners = CHAT_VIEW[
        CHAT_VIEW.index("syncWorldEmbedViewport();"):
        CHAT_VIEW.index('window.addEventListener("resize"')
    ]
    assert viewport_listeners.count(
        "window.visualViewport?.addEventListener("
    ) == 2
    assert '"resize",' in viewport_listeners
    assert '"scroll",' in viewport_listeners
    assert "height: 100%;" in CHAT_VIEW
    assert "min-height: 0 !important;" in CHAT_VIEW
    assert "overflow-y: auto;" in CHAT_VIEW


def test_world_embedded_dashboard_chat_keeps_mobile_composer_usable():
    assert "font-size: 16px;" in CHAT_VIEW
    assert "min-height: 2.75rem;" in CHAT_VIEW
    assert 'meta[name="viewport"]' in CHAT_VIEW
    assert 'viewportMeta.content += ", viewport-fit=cover"' in CHAT_VIEW
    assert "env(safe-area-inset-bottom, 0px)" in CHAT_VIEW
    assert "env(safe-area-inset-left, 0px)" in CHAT_VIEW
    assert "env(safe-area-inset-right, 0px)" in CHAT_VIEW
    assert 'aria-label="Message #general"' in CHAT_VIEW
    assert 'enterkeyhint="send"' in CHAT_VIEW


def test_world_chat_has_a_primer_multiline_repository_action_composer():
    for marker in (
        'id="fullChatChannel"',
        'id="fullChatRepo"',
        'id="fullChatAction"',
        '<option value="chat">Send to chat</option>',
        '<option value="task">Create team task</option>',
        '<option value="issue">Create repository issue</option>',
        '<option value="agent" selected>Send to bot</option>',
        'id="fullChatInput"',
        'rows="2"',
        "border-border",
        "bg-card",
        "focus:ring-1",
    ):
        assert marker in CHAT_VIEW
    for contract in (
        'fetch("/api/repositories"',
        "selectedComposerRepository()",
        "runFullComposerAction(inputEl, attachmentControl)",
        "ForkMeshDashboardActions?.submitWebIssue",
        "return await queueOrgAgent(",
        '"agent",',
        'fullAction.value !== "chat"',
        "inputEl.style.height",
        'destination.searchParams.set("worldEmbed", "1")',
    ):
        assert contract in CHAT


def test_chat_prompt_input_expands_from_its_top_resize_handle():
    # The grip is deliberately separate from the composer shell: pulling it up
    # changes only the prompt textarea's inline height, and pulling it down to
    # the minimum clears that override so the prompt snaps back to its resting
    # size.
    assert "data-dashboard-chat-input-resize-handle" in CHAT_VIEW
    assert 'aria-label="Resize message input"' in CHAT_VIEW
    assert "wireFullInputResizeHandle()" in CHAT
    assert "drag.startHeight + drag.startY - event.clientY" in CHAT
    assert 'fullInput.style.height = ""' in CHAT
    assert "fullInputManualHeight" in CHAT


def test_world_chat_is_a_transparent_bubble_hud_with_context_and_emotes():
    for marker in (
        "data-dashboard-chat-context-rail",
        "data-dashboard-chat-context-channel",
        "data-dashboard-chat-context-source",
        "data-dashboard-chat-scroll-rail",
        "chat-message-bubble",
        "background: transparent !important",
        "world-chat-composer-unfurl",
        'data-dashboard-chat-emote="wave"',
        'data-dashboard-chat-emote="jump"',
        'data-dashboard-chat-emote="spin"',
        'data-dashboard-chat-emote="backflip"',
        'data-dashboard-chat-emote="dance"',
        'data-dashboard-chat-emote="float"',
        'data-dashboard-chat-emote="wobble"',
        'data-dashboard-chat-emote="sparkle"',
    ):
        assert marker in CHAT_VIEW
    assert 'type: "forkmesh:world-emote"' in CHAT
    assert "fullLog.scrollTo({" in CHAT


def test_world_todo_actions_replay_as_chat_notification_bubbles():
    assert 'type: "forkmesh:chat-ready"' in CHAT
    assert 'data.type === "forkmesh:chat-notification"' in CHAT
    assert "seenParentNotifications" in CHAT
    assert "appendSystem(text);" in CHAT
    assert "emitWorldActivity" not in CHAT


def test_world_embed_uses_one_dark_primer_header_and_pinned_grid_composer():
    for marker in (
        "color-scheme: dark",
        "--background: #0d1117",
        "[data-dashboard-chat-channel-header]",
        "display: none;",
        "[data-dashboard-chat-composer-toolbar]",
        "grid-template-columns:",
        "[data-dashboard-chat-action]",
        "grid-column: 1 / -1",
    ):
        assert marker in CHAT_VIEW
    assert "Public World <strong" not in CHAT_VIEW
