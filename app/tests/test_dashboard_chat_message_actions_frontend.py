#!/usr/bin/env python3
"""Authors can edit and delete their own dashboard chat messages.

The dashboard chat has always *honoured* inbound "edit"/"delete" frames — from
the desktop client (MainWindowMessages) and the full chat page — but shipped no
way to produce them, so a typo on the dashboard was permanent. These contracts
pin the author-side controls and the frames they broadcast:

* both frames name the message via `target` and are members of DURABLE_TYPES,
  so the relay retains them and later joiners replay the edit/delete rather
  than the superseded original;
* every peer drops an edit/delete whose senderId differs from the original
  message's, so the controls are built only for the author's own rows.
"""

from pathlib import Path


PUBLIC = Path(__file__).resolve().parents[1] / "public"
CHAT = (PUBLIC / "dashboard-chat.js").read_text(encoding="utf-8")
SHELL = (PUBLIC / "dashboard" / "shell.html").read_text(encoding="utf-8")


def _region(start, end):
    return CHAT[CHAT.index(start):CHAT.index(end)]


def test_reactions_are_on_every_message_but_edits_stay_author_only():
    materialize = _region(
        "function materializeFullMessage(record)",
        "function renderHistoryWindow(",
    )
    assert "content?.append(buildMessageActions(record));" in materialize
    actions = _region("function buildMessageActions(", "// An \"(edited)\" marker")
    assert 'messageActionButton("☺"' in actions
    assert "if (record.self && record.senderId === selfId)" in actions
    assert 'messageActionButton("Edit"' in actions
    assert 'messageActionButton("Delete"' in actions


def test_message_avatars_use_uploaded_faces_with_generated_face_fallbacks():
    assert "function generatedChatFace(handle)" in CHAT
    assert "function publicChatAvatar(handle)" in CHAT
    assert "function hydrateChatAvatar(avatar, handle)" in CHAT
    avatar = _region("function publicChatAvatar(handle)", "function hydrateChatAvatar(")
    assert "Number(session.accountSnapshotVersion) >= 1" in avatar
    assert "return Promise.resolve(String(session.avatarPng));" in avatar
    assert 'fetch(`/api/accounts/${encodeURIComponent(name)}`' in CHAT
    assert 'image.src = `data:image/png;base64,${avatarPng}`' in CHAT
    assert 'class="chat-message-avatar avatar' in CHAT
    assert "hydrateChatAvatar(avatarEl, record.who)" in CHAT


def test_dashboard_reactions_use_the_shared_durable_protocol():
    reaction = _region("function reactionKey(", "// An \"(edited)\" marker")
    assert "function applyReaction(plain)" in reaction
    assert "function renderReactions(messageId)" in reaction
    assert "function toggleReaction(messageId, emoji)" in reaction
    assert 'makePlain("reaction"' in reaction
    assert "conversation: CHANNEL_LABEL" in reaction
    assert "reactorId: selfId" in reaction
    assert "reactorName: displayName()" in reaction
    assert "added: !mine" in reaction
    assert '"reaction"' in CHAT[CHAT.index("const DURABLE_TYPES"):][:200]
    assert ".chat-reactions {" in SHELL
    assert ".chat-reaction-chip" in SHELL
    assert ".chat-reaction-picker" in SHELL


def test_edit_frame_targets_the_message_and_is_durable():
    save = _region("function saveMessageEdit(", "function requestMessageDelete(")
    assert 'makePlain("edit"' in save
    assert "target: record.id" in save
    assert "runWhenConnected(() => send(plain))" in save
    # Guard mirrors the receiver's: never broadcast an edit for another author.
    assert "record.senderId !== selfId" in save
    assert '"edit"' in CHAT[CHAT.index("const DURABLE_TYPES"):][:200]


def test_delete_frame_targets_the_message_and_is_durable():
    confirm = _region("function confirmMessageDelete(", "// The rail's mini chat")
    assert 'makePlain("delete"' in confirm
    assert "target: record.id" in confirm
    assert "removeMessage(record.id)" in confirm
    assert "record.senderId !== selfId" in confirm
    assert '"delete"' in CHAT[CHAT.index("const DURABLE_TYPES"):][:200]


def test_delete_is_confirmed_without_blocking_the_world_embed():
    # Deleting is irreversible for every reader, so it takes a second click —
    # inline, because this chat also runs inside the World's same-origin
    # iframe, where a blocking window.confirm() would freeze the host frame.
    request = _region("function requestMessageDelete(", "function confirmMessageDelete(")
    code = "\n".join(
        line for line in request.splitlines() if not line.strip().startswith("//")
    )
    assert "window.confirm" not in code
    assert "chat-delete-confirm" in code
    assert 'messageActionButton("Cancel"' in code
    assert "confirmMessageDelete(record)" in code


def test_edited_messages_are_marked_for_readers():
    assert "chat-edited" in CHAT
    edit_branch = _region('} else if (type === "edit") {', '} else if (type === "delete") {')
    # An inbound edit re-renders the text and stamps the marker, so a rewritten
    # message never silently replaces what a reader already saw.
    assert "renderMessageText(rec.textEl, rec.text)" in edit_branch
    assert "markEdited(rec," in edit_branch


def test_actions_are_hidden_until_the_row_is_hovered_or_focused():
    # The pre-built dashboard/tailwind.css carries no group-hover variant, so
    # the reveal has to be hand-written CSS in the shell.
    assert "group-hover:opacity-100" not in CHAT
    assert ".chat-message-actions {" in SHELL
    assert ".group:hover > .chat-message-actions," in SHELL
    assert ".chat-message-actions:focus-within {" in SHELL


def test_hidden_controls_toggle_display_not_the_hidden_attribute():
    # Tailwind's `.flex` utility outranks the [hidden] preflight rule in the
    # pre-built stylesheet, so `el.hidden = true` would leave the row visible.
    actions = _region("function messageActionButton(", "// The rail's mini chat")
    assert ".hidden = true" not in actions
    assert 'el.style.display = visible ? "" : "none"' in actions
