import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CHAT = (ROOT / "app" / "public" / "chat.js").read_text(encoding="utf-8")
HTML = (ROOT / "app" / "public" / "chat.html").read_text(encoding="utf-8")
ACCOUNT = (
    ROOT / "app" / "public" / "dashboard" / "js" / "04-account.js"
).read_text(encoding="utf-8")
DESKTOP = ROOT / "desktop" / "src"
MESSAGES = (DESKTOP / "MainWindowMessages.cpp").read_text(encoding="utf-8")
ISSUES = (DESKTOP / "MainWindowIssues.cpp").read_text(encoding="utf-8")
SERVER_NODE = (DESKTOP / "ServerNode.cpp").read_text(encoding="utf-8")
CHAT_BACKEND = (DESKTOP / "ChatBackend.h").read_text(encoding="utf-8")
MIRROR = (DESKTOP / "OfficeChannelMirror.cpp").read_text(encoding="utf-8")
IOS_SHEET = (
    ROOT / "native" / "ios" / "ForkMesh" / "Features" / "Chat" / "Channels"
    / "ChatChannelSidebar.swift"
).read_text(encoding="utf-8")
ANDROID_SHEET = (
    ROOT / "native" / "android" / "app" / "src" / "main" / "kotlin" / "com"
    / "forkmesh" / "nativeapp" / "feature" / "chat" / "ChatSheets.kt"
).read_text(encoding="utf-8")


def _js_function(name):
    match = re.search(r"(?:async )?function " + re.escape(name) + r"\(", CHAT)
    assert match, name
    start = match.start()
    end = CHAT.find("\nfunction ", start + 1)
    async_end = CHAT.find("\nasync function ", start + 1)
    ends = [value for value in (end, async_end) if value != -1]
    return CHAT[start:min(ends) if ends else len(CHAT)]


def _cpp_function(source, signature):
    start = source.index(signature)
    end = source.find("\nvoid MainWindow::", start + 1)
    return source[start:end if end != -1 else len(source)]


def test_web_people_pane_lists_only_the_rooms_members():
    members = _js_function("activeRoomMembers")
    assert "directMessageForKey(channel)" in members
    assert 'record.visibility !== "public"' in members
    people = _js_function("renderPeople")
    assert "const roomMembers = activeRoomMembers();" in people
    assert "allowed.has(key)" in people
    assert "bySection.guest = [];" in people
    assert "bySection.node = [];" in people
    assert "`Members · ${bySection.user.length}`" in people
    reconcile = _js_function("reconcilePrivateChannels")
    assert "members: Array.isArray(value.members)" in reconcile
    assert "schedulePeopleRender();" in reconcile
    assert "schedulePeopleRender();" in _js_function("setActiveChannel")


def test_web_create_dialog_says_exactly_who_can_see_the_room():
    audience = _js_function("describeChannelAudience")
    assert "Only you can see this room until you invite someone." in audience
    assert "Nobody else, including administrators, is added." in audience
    assert "Every signed-in ForkMesh member can find, read and join" in audience
    assert "channelInitialMembersInput.hidden = isPublic;" in audience
    assert 'id="chat-channel-audience"' in HTML
    assert "Private — only you and the people you invite" in HTML
    assert "Public — every signed-in member can join" in HTML
    assert 'channelVisibilityInput?.addEventListener("change", describeChannelAudience)' in CHAT


def test_web_private_frame_flag_only_drops_public_world_frames():
    entry = _js_function("renderChatEntry")
    assert 'if (entry.private && scope === "public-world-general") return;' in entry
    assert "if (entry.private) return;" not in entry
    thread = _js_function("renderThreadEntry")
    assert '(entry.private && scope === "public-world-general")' in thread


def test_web_offers_message_from_mentions_profiles_and_links():
    card = _js_function("renderMentionCard")
    assert "canMessagePerson(name)" in card
    assert "startDirectMessage(name)" in card
    assert "Open profile" in card
    can = _js_function("canMessagePerson")
    assert "isOfficeEmbed" in can and "key !== self" in can
    assert 'chatQuery.get("dm")' in CHAT
    assert "await startDirectMessage(requestedDirect);" in CHAT
    assert 'message.setAttribute("data-profile-message", name);' in ACCOUNT
    assert '"/chat?dm=" + encodeURIComponent(name)' in ACCOUNT


def test_desktop_private_room_is_a_server_room_with_explicit_invites():
    prompt = _cpp_function(MESSAGES, "void MainWindow::promptAddPrivateChannel()")
    assert "showNewPrivateRoomDialog();" in prompt
    assert "createPrivateChannel" not in prompt
    dialog = _cpp_function(MESSAGES, "void MainWindow::showNewPrivateRoomDialog()")
    assert "m_officeChannelMirror->createChannel(name, true," in dialog
    assert "Nobody else, including" in dialog
    assert "administrators, is added." in dialog
    invite = _cpp_function(MESSAGES, "void MainWindow::promptInviteToPrivateChannel()")
    assert "m_officeChannelMirror->addMember(conversation, account);" in invite
    assert "inviteToChannel" not in MESSAGES
    assert "inviteToChannel" not in CHAT_BACKEND
    assert 'type == "invite"' not in SERVER_NODE
    assert "New private room (only people you invite)" in ISSUES


def test_desktop_member_list_never_falls_back_to_everyone_for_private_rooms():
    members = _cpp_function(MESSAGES, "void MainWindow::refreshChatMembers()")
    legacy = members.index("} else if (m_privateChannels.contains(m_currentConversation)) {")
    everyone = members.index("for (auto it = m_chatDirectoryUsers.constBegin();\n             it != m_chatDirectoryUsers.constEnd(); ++it)\n            roomUserKeys.insert(it.key());")
    assert members.index("} else if (privateOfficeRoom) {") < legacy < everyone
    assert "ONLY THESE PEOPLE CAN SEE THIS ROOM" in members


def test_desktop_message_actions_open_or_reuse_one_server_dm():
    start = _cpp_function(MESSAGES, "void MainWindow::startDirectMessageWith(")
    assert "m_officeChannelMirror->startDirectMessage(name)" in start
    assert "openDirectChat(" not in start
    assert "startDirectMessageWith(account, QString(), QString());" in MESSAGES
    assert "openUserProfile(account, name)" in MESSAGES
    assert 'showChatSenderProfile(QStringLiteral("user:") + key,' in MESSAGES
    assert "&MainWindow::showNewMessageDialog" in ISSUES
    reuse = MIRROR[MIRROR.index("bool OfficeChannelMirror::startDirectMessage("):]
    reuse = reuse[:reuse.index("const QString ts")]
    assert "emit directConversationReady(existing);" in reuse
    assert 'QStringLiteral("direct:") + room.id' in MIRROR


def test_native_private_option_is_a_server_room_with_explicit_invites():
    assert "hub.extras.createPrivateChannel(name)" not in IOS_SHEET
    assert "office.create(name: name, isPrivate: true, members: list)" in IOS_SHEET
    assert "isPrivate: !list.isEmpty" not in IOS_SHEET
    assert "chat.extras.createPrivateChannel(name)" not in ANDROID_SHEET
    assert "create(name, isPrivate = true, members = invited)" in ANDROID_SHEET
    assert "isPrivate = list.isNotEmpty()" not in ANDROID_SHEET
