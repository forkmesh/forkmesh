"""Full web-chat contracts for one-to-one direct messages."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CHAT = (ROOT / "public" / "chat.js").read_text(encoding="utf-8")
HTML = (ROOT / "public" / "chat.html").read_text(encoding="utf-8")
PROTOCOL = (
    ROOT.parent / "www" / "public" / "docs" / "protocol" / "index.html"
).read_text(encoding="utf-8")


def _function_source(name):
    marker = f"function {name}("
    async_marker = f"async function {name}("
    start = CHAT.find(async_marker)
    if start < 0:
        start = CHAT.index(marker)
    next_function = CHAT.find("\nfunction ", start + 1)
    next_async = CHAT.find("\nasync function ", start + 1)
    ends = [value for value in (next_function, next_async) if value >= 0]
    return CHAT[start:min(ends) if ends else len(CHAT)]


def test_chat_has_accessible_direct_message_sidebar_and_picker():
    for marker in (
        'id="chat-direct-section"',
        'aria-labelledby="chat-direct-title"',
        'id="chat-direct-create"',
        'aria-label="New direct message"',
        'id="chat-direct-list"',
        'id="chat-direct-dialog"',
        'aria-labelledby="chat-direct-dialog-title"',
        'id="chat-direct-search"',
        'aria-label="Search people to message"',
        'id="chat-direct-options"',
        'id="chat-direct-error"',
    ):
        assert marker in HTML


def test_chat_loads_and_reconciles_authorized_direct_messages():
    assert (
        'const DIRECT_MESSAGES_ENDPOINT = "/api/chat/direct-messages"'
        in CHAT
    )
    assert "const directMessages = new Map()" in CHAT
    assert "function directMessageForKey(" in CHAT
    assert "function reconcileDirectMessages(" in CHAT
    assert "async function refreshDirectMessages(" in CHAT
    assert "data.conversations || []" in CHAT


def test_direct_room_access_uses_ephemeral_server_credentials():
    source = _function_source("fetchRoomAccess")
    assert 'DIRECT_MESSAGES_ENDPOINT + "/"' in source
    assert '"/room-access"' in source
    assert "access.webSocketUrl" in CHAT
    assert "access.passphrase" in CHAT
    assert "localStorage" not in source


def test_starting_a_direct_message_reuses_the_server_conversation():
    source = _function_source("startDirectMessage")
    assert "DIRECT_MESSAGES_ENDPOINT" in source
    assert 'method: "POST"' in source
    assert "JSON.stringify({ username" in source
    assert "conversation.id" in source
    assert "directMessageKey(conversation.id)" in source


def test_direct_picker_searches_authenticated_active_users():
    source = _function_source("refreshDirectMessageSearch")
    assert 'DIRECT_MESSAGES_ENDPOINT + "/users?query="' in source
    assert "privateChannelRequest" in source
    assert "directSearchResults" in source


def test_people_action_failures_have_visible_page_feedback():
    source = _function_source("startDirectMessage")
    assert "directDialog?.open" in source
    assert "setStatus(message)" in source


def test_direct_sidebar_uses_server_unread_counts_and_pagination():
    reconcile = _function_source("reconcileDirectMessages")
    refresh = _function_source("refreshDirectMessages")
    assert "value.unreadCount" in reconcile
    assert "nextCursor" in refresh
    assert "chat-direct-more" in HTML


def test_people_keep_profile_links_and_add_separate_message_actions():
    source = _function_source("renderPeople")
    assert 'document.createElement(hasProfile ? "a" : "div")' in source
    assert "mentionProfilePath(person.name)" in source
    assert 'message.className = "chat-person-message"' in source
    assert "startDirectMessage(person.name)" in source
    assert "currentName" in source


def test_only_opaque_direct_id_is_saved_as_a_preference():
    source = _function_source("setActiveChannel")
    assert "ACTIVE_DIRECT_KEY" in source
    assert "direct.id" in source
    assert "passphrase" not in source
    assert "webSocketUrl" not in source


def test_direct_message_rendering_uses_distinct_labels_and_no_forkbot():
    assert 'return "@" + record.otherUser' in CHAT
    assert 'channelVisibilityBadge.textContent = direct ? "direct"' in CHAT
    assert "!controllerReadableScope()" in _function_source(
        "maybeAskForkbot"
    )
    assert "isDirectMessageKey(scope)" in _function_source(
        "frameMatchesScope"
    )


def test_direct_message_controls_are_wired_before_async_chat_hydration():
    init = _function_source("initChat")
    assert "function wireChatControls()" in CHAT
    assert init.index("wireChatControls();") < init.index("await hydrateUserSession();")
    assert "function wireChatComposer()" in CHAT
    assert init.index("wireChatComposer();") < init.index("await hydrateUserSession();")
    controls = _function_source("wireChatControls")
    assert "directCreateBtn?.addEventListener(\"click" in controls
    assert "openDirectMessageDialog" in controls
    assert "directSearch?.addEventListener(\"input" in controls
    assert "scheduleDirectMessageSearch" in controls
    assert 'sendBtn.addEventListener("click", sendCurrentMessage)' in controls
    assert 'attachmentInput.addEventListener("change"' in controls
    assert "void sendAttachmentDraft();" in controls
    composer = _function_source("wireChatComposer")
    assert 'input.addEventListener("input"' in composer
    assert "updateMentionSuggest();" in composer
    assert 'input.addEventListener("keydown"' in composer
    assert 'event.key === "Enter" && !event.shiftKey' in composer
    refresh = _function_source("refreshDirectMessages")
    start = _function_source("startDirectMessage")
    assert "let directMessagesRevision = 0" in CHAT
    assert "const requestRevision = directMessagesRevision" in refresh
    assert "requestRevision !== directMessagesRevision" in refresh
    assert "directMessagesRevision += 1" in start
    assert start.index("if (!conversation?.id)") < start.index(
        "directMessagesRevision += 1"
    ) < start.index("reconcileDirectMessages(")


def test_thread_reply_controls_are_wired_before_async_chat_hydration():
    init = _function_source("initChat")
    assert "function wireThreadControls()" in CHAT
    assert init.index("wireThreadControls();") < init.index(
        "await hydrateUserSession();"
    )
    controls = _function_source("wireThreadControls")
    assert 'threadSendBtn?.addEventListener("click", sendThreadReply)' in controls
    assert 'threadInput?.addEventListener("keydown"' in controls
    assert 'event.key === "Enter" && !event.shiftKey' in controls
    assert "sendThreadReply();" in controls


def test_delete_controls_are_wired_early_and_use_canonical_room_labels():
    init = _function_source("initChat")
    assert "function wireMessageActionControls()" in CHAT
    assert init.index("wireMessageActionControls();") < init.index(
        "await hydrateUserSession();"
    )
    controls = _function_source("wireMessageActionControls")
    assert 'deleteConfirmBtn?.addEventListener("click", confirmMessageDelete)' in controls
    confirm = _function_source("confirmMessageDelete")
    edit = _function_source("saveMessageEdit")
    assert "conversation: channelWireLabel(record.channel)" in confirm
    assert "conversation: channelWireLabel(record.channel)" in edit


def test_protocol_documents_participant_only_direct_messages():
    for marker in (
        'id="personal-direct-messages"',
        "/api/chat/direct-messages",
        "/room-access",
        "/users?query=",
        "/read",
        "cursor-paginated",
        "/ws?ticket=",
        "exactly two active registered users",
        "Administrators have no implicit access",
        "automatically add the X25519 recipient layer",
    ):
        assert marker in PROTOCOL
