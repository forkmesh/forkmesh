from pathlib import Path


# MainWindowRepos.cpp was split into feature files; read them in the original
# source order so section slices that span functions still line up.
MAIN_WINDOW_REPOS_SOURCES = (
    "MainWindowRepos.cpp",
    "MainWindowReposInternal.h",
    "MainWindowRepoSettings.cpp",
    "MainWindowRepoSync.cpp",
    "MainWindowMirrorPush.cpp",
    "MainWindowRepoFetch.cpp",
    "MainWindowAccounts.cpp",
)

# MainWindowChat.cpp was split into feature files; read them in the original
# source order so section slices that span functions still line up.
MAIN_WINDOW_CHAT_SOURCES = (
    "MainWindowChat.cpp",
    "MainWindowNetworkInternal.h",
    "MainWindowDiagnostics.cpp",
    "MainWindowLogs.cpp",
    "MainWindowNavigation.cpp",
    "MainWindowNetworkRepos.cpp",
    "MainWindowHosts.cpp",
    "MainWindowNodes.cpp",
    "MainWindowNetworkDiagnostics.cpp",
    "MainWindowUsers.cpp",
    "MainWindowVultrMirrors.cpp",
    "MainWindowHostInstall.cpp",
)

ROOT = Path(__file__).resolve().parents[2]
QT_SRC = ROOT / "desktop" / "src"
HEADER = QT_SRC / "MainWindow.h"
SETUP = QT_SRC / "MainWindowSetup.cpp"
SETTINGS = QT_SRC / "MainWindowSettings.cpp"
REPOS = QT_SRC / "MainWindowRepos.cpp"
MESSAGES = QT_SRC / "MainWindowMessages.cpp"
ISSUES = QT_SRC / "MainWindowIssues.cpp"
MESSAGE_ROW_HEADER = QT_SRC / "MessageRow.h"
MESSAGE_ROW = QT_SRC / "MessageRow.cpp"


def _body(source: str, start: str, end: str) -> str:
    i = source.index(start)
    j = source.index(end, i)
    return source[i:j]


def test_chat_backend_starts_with_user_display_name_not_node_name():
    header = HEADER.read_text(encoding="utf-8")
    setup = SETUP.read_text(encoding="utf-8")

    assert "QString chatDisplayName() const;" in header
    assert "QString nodeOwnerDisplayName() const;" in header
    assert "void updateChatIdentity();" in header
    assert "new ServerNode(chatDisplayName(), sharedNodeName()," in setup
    assert "nodeOwnerDisplayName()," in setup
    assert "m_profileIdentity.publicKey()" in setup


def test_chat_identity_pushes_user_name_and_user_avatar():
    setup = SETUP.read_text(encoding="utf-8")
    body = _body(
        setup,
        "void MainWindow::updateChatIdentity()",
        "bool MainWindow::accountEmailVerified",
    )

    assert "const QString name = chatDisplayName();" in body
    assert "m_backend->setUserName(name);" in body
    assert "m_backend->setNodeIdentity(sharedNodeName(), nodeOwnerDisplayName());" in body
    assert "const QByteArray avatar = effectiveUserAvatar();" in body
    assert "m_backend->setAvatar(avatar);" in body
    assert "effectiveAvatar()" not in body


def test_profile_name_and_avatar_edits_refresh_chat_identity():
    repos = "".join(
        (REPOS.parent / name).read_text(encoding="utf-8")
        for name in MAIN_WINDOW_REPOS_SOURCES
    )
    name_body = _body(
        repos,
        "void MainWindow::onProfileNameChanged",
        "void MainWindow::onAvatarChosen",
    )
    avatar_body = _body(
        repos,
        "void MainWindow::onAvatarChosen",
        "void MainWindow::logout",
    )

    assert "m_backend->setUserName(trimmed)" not in name_body
    assert "m_backend->setAvatar(pngData)" not in avatar_body
    assert "updateChatIdentity();" in name_body
    assert "updateChatIdentity();" in avatar_body


def test_user_avatar_prefers_chosen_avatar_before_generated_identicon():
    settings = SETTINGS.read_text(encoding="utf-8")
    body = _body(
        settings,
        "QByteArray MainWindow::effectiveUserAvatar()",
        "void MainWindow::updateAvatarButton()",
    )

    assert "if (!m_userAvatar.isEmpty())" in body
    assert "return m_userAvatar;" in body
    assert body.index("return m_userAvatar;") < body.index("forkMeshAvatarPng(seed)")


def test_chat_member_column_is_user_directory_not_online_nodes():
    header = HEADER.read_text(encoding="utf-8")
    messages = MESSAGES.read_text(encoding="utf-8")
    roster_body = _body(
        messages,
        "void MainWindow::refreshChatMembers()",
        "void MainWindow::removeChatMember",
    )
    directory_body = _body(
        messages,
        "void MainWindow::refreshChatUserDirectory()",
        "void MainWindow::mergeChatUserDirectory",
    )

    assert "QHash<QString, MemberInfo> m_chatDirectoryUsers" in header
    assert 'accountsApiUrl(QStringLiteral("users"))' in directory_body
    assert "for (const MemberInfo &u : std::as_const(m_chatDirectoryUsers))" in roster_body
    assert "if (!online)" not in roster_body
    assert "DATABASE USERS IN THIS ROOM \\xE2\\x80\\x94 %1" in roster_body
    assert "chatUserDisplayName(member)" in roster_body


def test_self_row_folds_into_its_own_account_group():
    # "You" must group under your OWN account (its directory entry + owned nodes)
    # instead of a separate "\x01self" island, or the local user appears as a
    # duplicate participant next to their own account (a second "jett").
    messages = MESSAGES.read_text(encoding="utf-8")
    roster_body = _body(
        messages,
        "void MainWindow::refreshChatMembers()",
        "void MainWindow::removeChatMember",
    )
    # groupKeyFor no longer short-circuits self to a self-only key first; instead
    # a self row resolves to the account owner key so it merges with the account.
    assert 'if (m.self)\n            return QStringLiteral("\\x01self");' not in roster_body
    assert "const QString selfOwner = accountOwner().trimmed().toLower();" in roster_body
    # The merged canonical (directory) row keeps the self marker so it still
    # sorts first and renders as you.
    assert "member.self = member.self || g.primary.self;" in roster_body


def test_chat_mentions_highlight_and_open_user_profiles():
    header = MESSAGE_ROW_HEADER.read_text(encoding="utf-8")
    row = MESSAGE_ROW.read_text(encoding="utf-8")
    messages = MESSAGES.read_text(encoding="utf-8")
    create_row_body = _body(
        messages,
        "MessageRow *MainWindow::createMessageRow",
        "MessageRow *MainWindow::addMessageRow",
    )
    mention_profiles_body = _body(
        messages,
        "static QHash<QString, MemberInfo> chatMentionProfiles",
        "static bool sameChatProfilePresentation",
    )
    directory_body = _body(
        messages,
        "void MainWindow::mergeChatUserDirectory",
        "void MainWindow::refreshChatMembers",
    )

    assert "const QHash<QString, MemberInfo> &mentionProfiles" in header
    assert "void mentionClicked(const QString &id, const QString &name);" in header
    assert "renderMentionedText(message.text, m_mentionProfiles)" in row
    assert "forkmesh-mention:" in row
    assert "QLabel::linkHovered" in row
    assert "QToolTip::showText" in row
    assert "QLabel::linkActivated" in row

    # Directory users carry the canonical /@name profile target; live roster
    # entries are a fallback when the directory has not seen that user yet.
    assert "teammateChatMentionProfiles()" in create_row_body
    assert "chatMentionProfiles(m_chatDirectoryUsers, m_homeRoster)" in messages
    assert mention_profiles_body.index("member : directory") < (
        mention_profiles_body.index("member : roster")
    )
    open_profile_body = _body(
        messages,
        "bool MainWindow::openUserProfile",
        "void MainWindow::showChatSenderProfile",
    )
    assert "connect(row, &MessageRow::mentionClicked" in create_row_body
    assert "openUserProfile(account, name)" in create_row_body
    assert "m_chatDirectoryUsers.contains(key)" in open_profile_body
    assert "chatMentionProfileUrl" in open_profile_body
    assert "QDesktopServices::openUrl(url)" in open_profile_body
    assert 'url.setPath(QStringLiteral("/@")' in messages
    assert 'QStringLiteral("app.forkmesh.com")' in messages
    assert 'QStringLiteral("forkmesh.com")' in messages

    # Existing visible rows are rerendered after the async user directory lands,
    # otherwise old messages would stay plain text until the next full refresh.
    assert "renderConversationRows();" in directory_body


def test_chat_message_times_use_compact_12_hour_clock():
    row = MESSAGE_ROW.read_text(encoding="utf-8")

    assert 'sent.toString("h:mm ap")' in row


def test_chat_composer_stays_visible_and_rooms_scroll_independently():
    issues = ISSUES.read_text(encoding="utf-8")
    body = _body(
        issues,
        "QWidget *MainWindow::buildChatSection()",
        "void MainWindow::updateHomeStats()",
    )

    assert "auto *roomsScroll = new QScrollArea;" in body
    assert "roomsScroll->setWidgetResizable(true);" in body
    assert "roomsScroll->setMinimumHeight(0);" in body
    assert "roomsScroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);" in body
    assert "sidebarLayout->addWidget(roomsScroll, 1);" in body

    for list_name in ("m_channelList", "m_dmList"):
        assert f"{list_name}->setMinimumHeight(0);" in body
        assert f"{list_name}->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);" in body
        assert f"fitToRows({list_name});" in body
        assert f"{list_name}->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);" in body

    assert "m_messageScroll->setMinimumHeight(0);" in body
    assert "m_messageScroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);" in body
    assert "composer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);" in body
    assert "m_messageInput->setMinimumWidth(0);" in body
    assert "auto *mainColumnHost = new QWidget;" in body
    assert "mainColumnHost->setMinimumHeight(0);" in body
    assert "m_chatSplit->addWidget(mainColumnHost);" in body
    assert "m_chatSplit->addWidget(m_chatThreadPane);" in body
    assert "layout->addWidget(m_chatSplit);" in body


def test_desktop_frames_carry_account_kind_for_web_display():
    # Web/dashboard chat surfaces only render frames stamped accountKind
    # "user". The desktop never sent the field at all, so a desktop user's
    # messages relayed fine but were invisible on the website — "web and Qt
    # aren't syncing". The backend stamps every outgoing frame, hydrated from
    # the same user-vs-node rule as welcomeChannelForIdentity().
    server_node = (QT_SRC / "ServerNode.cpp").read_text(encoding="utf-8")
    server_node_h = (QT_SRC / "ServerNode.h").read_text(encoding="utf-8")
    chat_backend = (QT_SRC / "ChatBackend.h").read_text(encoding="utf-8")
    chat_win = "".join(
        (QT_SRC / name).read_text(encoding="utf-8")
        for name in MAIN_WINDOW_CHAT_SOURCES
    )
    setup = SETUP.read_text(encoding="utf-8")

    assert "virtual void setAccountKind(const QString &kind)" in chat_backend
    assert "void setAccountKind(const QString &kind) override;" in server_node_h
    assert 'message.insert("accountKind", m_accountKind);' in server_node
    switcher = _body(
        chat_win,
        "void MainWindow::updateUserSwitcher()",
        "const QString user = topBarUserName();",
    )
    assert "m_profileIsUserAccount || !m_profileLinkedNodes.isEmpty()" in switcher
    assert "m_backend->setAccountKind(userLike" in switcher
    assert "server->setAccountKind(" in setup


def test_desktop_composer_bridges_forkbot_like_the_web_chat():
    # ForkBot has no room connection: whichever client SENDS a mention asks
    # /api/forkbot/chat and relays the reply. Only web clients did this, so
    # ForkBot never answered desktop users. The desktop bridge mirrors the
    # web one: author-side only, public channels only, recent conversation
    # context attached, reply broadcast as forkbot/accountKind user.
    header = HEADER.read_text(encoding="utf-8")
    messages = MESSAGES.read_text(encoding="utf-8")
    server_node = (QT_SRC / "ServerNode.cpp").read_text(encoding="utf-8")
    chat_backend = (QT_SRC / "ChatBackend.h").read_text(encoding="utf-8")

    assert "void maybeAskForkbot(const QString &conversation, const QString &text," in header
    assert "sendChatText(m_currentConversation, text)" in messages
    send_text = _body(
        messages,
        "bool MainWindow::sendChatText(",
        "void MainWindow::insertEmojiIntoComposer(",
    )
    assert "maybeAskForkbot(conversation, text);" in send_text
    assert "maybeAskForkbot(m_currentConversation, text, m_activeChatThreadRootId);" in messages
    bridge = _body(
        messages,
        "void MainWindow::maybeAskForkbot",
        "void MainWindow::onComposerEdited",
    )
    assert '"(?:^|[^A-Za-z0-9_-])@?forkbot\\\\b"' in bridge
    assert "m_privateChannels.contains(conversation)" in bridge
    assert '/api/forkbot/chat' in bridge
    assert '{QStringLiteral("context"), context}' in bridge
    assert "m_backend->sendBotChat(channel, botMessage);" in bridge
    assert "m_backend->sendBotThreadReply(channel, rootId, botMessage);" in bridge
    assert "m_discordClient->isGuildConversation(conversation)" in bridge
    assert "m_discordClient->sendReply(channel, rootId, botMessage);" in bridge
    # A live message received from someone else takes the same mention path;
    # otherwise ForkBot only answered the desktop client that sent a line.
    receive = _body(messages, "void MainWindow::onMessage", "void MainWindow::onReaction")
    assert "!message.historical && !ownMessage &&" in receive
    assert "if (!forkbotMessage)" in receive
    assert "maybeAskForkbot(conversation, message.text, message.threadRootId);" in receive

    assert "virtual void sendBotChat(const QString &channel, const QString &text)" in chat_backend
    bot_chat = _body(
        server_node,
        "void ServerNode::sendBotChat",
        "void ServerNode::sendDirect",
    )
    assert '{"senderId", QStringLiteral("forkbot")}' in bot_chat
    assert '{"accountKind", QStringLiteral("user")}' in bot_chat
    # Never into a private room — the bot round-trip would leak the reply
    # path to the relay.
    assert "m_privateChannels.contains(channel)" in bot_chat
    assert "void sendBotThreadReply(const QString &channel" in chat_backend
    thread_bot_chat = _body(
        server_node,
        "void ServerNode::sendBotThreadReply",
        "void ServerNode::sendTeammateChat",
    )
    assert '{"type", QStringLiteral("thread-reply")}' in thread_bot_chat
    assert '{"rootId", rootId}' in thread_bot_chat
    assert 'm_privateChannels.contains(channel)' in thread_bot_chat


def test_own_account_messages_never_mark_unread_or_notify():
    # adhoc #426: ChatMessage.self is a node-id match, so a line this user
    # typed on the website or in the World came back as somebody else's and lit
    # the desktop's red unread badge (and raised a notification) for something
    # they had just written. chatDisplayName() is the name this account stamps
    # on its own outgoing frames, so incoming senders are matched against it.
    messages = MESSAGES.read_text(encoding="utf-8")
    body = _body(
        messages,
        "void MainWindow::onMessage(const ChatMessage &message)",
        "void MainWindow::onReaction(",
    )
    assert "const QString ownChatName = chatDisplayName().trimmed();" in body
    assert "const bool ownMessage =" in body
    assert "message.self ||" in body
    assert "Qt::CaseInsensitive" in body
    # Both the unread tally and the notification path use the wider check.
    # Each also skips backfill, so the guard reads "!message.historical &&
    # !ownMessage"; what matters here is that no path badges your own message.
    assert "!message.historical && !ownMessage &&" in body
    assert "!message.historical && !ownMessage) {" in body
    assert body.count("!ownMessage") == 4
    assert "if (!message.self &&" not in body
    assert "if (!message.self) {" not in body
