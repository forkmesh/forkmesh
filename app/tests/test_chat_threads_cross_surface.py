"""Threaded room chat stays interoperable across web, World, and Qt."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PUBLIC = ROOT / "app" / "public"
WEB_CHAT = (PUBLIC / "chat.js").read_text(encoding="utf-8")
DASHBOARD_CHAT = (PUBLIC / "dashboard-chat.js").read_text(encoding="utf-8")
WORLD = (ROOT / "world" / "public" / "world" / "world.js").read_text(encoding="utf-8")
QT_BACKEND = (ROOT / "desktop" / "src" / "ChatBackend.h").read_text(
    encoding="utf-8"
)
QT_NODE = (ROOT / "desktop" / "src" / "ServerNode.cpp").read_text(
    encoding="utf-8"
)
QT_MESSAGES = (ROOT / "desktop" / "src" / "MainWindowMessages.cpp").read_text(
    encoding="utf-8"
)
QT_ROW = (ROOT / "desktop" / "src" / "MessageRow.cpp").read_text(
    encoding="utf-8"
)
QT_ROW_HEADER = (ROOT / "desktop" / "src" / "MessageRow.h").read_text(
    encoding="utf-8"
)


def test_every_client_sends_the_same_thread_reply_envelope():
    for source in (WEB_CHAT, DASHBOARD_CHAT):
        assert 'makePlain("thread-reply"' in source
        assert "rootId:" in source
        assert '"thread-reply"' in source[source.index("DURABLE_TYPES") :]

    assert "virtual void sendThreadReply" in QT_BACKEND
    send_reply = QT_NODE[
        QT_NODE.index("void ServerNode::sendThreadReply") :
        QT_NODE.index("void ServerNode::setAccountKind")
    ]
    assert 'makeMessage("thread-reply")' in send_reply
    assert 'message.insert("rootId", rootId)' in send_reply
    assert 'message.insert("channel", channel)' in send_reply
    assert "storeHistory(message)" in send_reply
    assert "sendEncrypted(message, true)" in send_reply


def test_qt_keeps_replies_out_of_the_room_timeline_and_opens_a_thread_view():
    assert 'type == "chat" || type == "thread-reply"' in QT_NODE
    assert 'out.threadRootId = message.value("rootId")' in QT_NODE
    # A reply never becomes a transcript row of its own (outside Discord, where
    # replies are ordinary channel messages) — it only lands in the drawer.
    add_row = QT_MESSAGES[
        QT_MESSAGES.index("MessageRow *MainWindow::addMessageRow("):
        QT_MESSAGES.index("void MainWindow::openChatThread(")
    ]
    assert "if (!message.threadRootId.isEmpty() &&" in add_row
    assert "!isDiscordConversation(message.conversation)) {" in add_row
    assert "return nullptr;" in add_row
    assert "void MainWindow::openChatThread" in QT_MESSAGES
    assert "void MainWindow::sendChatThreadReply" in QT_MESSAGES
    assert "m_backend->sendThreadReply" in QT_MESSAGES
    # The action reads "Reply on Discord" on a mirrored Discord message and
    # "Reply in thread" everywhere else.
    assert 'QStringLiteral("Reply in thread")' in QT_ROW
    assert 'thread->setObjectName("threadSummary")' in QT_ROW


def test_qt_chat_thread_uses_a_full_right_hand_splitter_pane():
    issues = (ROOT / "desktop" / "src" / "MainWindowIssues.cpp").read_text(
        encoding="utf-8"
    )
    messages = QT_MESSAGES

    assert 'm_chatThreadPane->setObjectName(QStringLiteral("chatThreadPane"))' in issues
    assert "m_chatSplit->addWidget(mainColumnHost);" in issues
    assert "m_chatSplit->addWidget(m_chatThreadPane);" in issues
    assert "m_chatThreadPane->hide();" in issues
    assert "m_chatThreadPane->show();" in messages
    assert "void MainWindow::closeChatThread()" in messages
    assert "new QDialog(this)" not in messages[
        messages.index("void MainWindow::openChatThread("):
        messages.index("void MainWindow::rebuildChatThreadPane()")
    ]


def test_qt_thread_summary_shows_who_replied():
    # The summary row under a message leads with the repliers' faces and the
    # name on the newest reply, the same as the web surfaces.
    assert "struct ThreadReplySummary" in QT_ROW_HEADER
    assert "QList<QPixmap> faces;" in QT_ROW_HEADER
    assert "QString latestSender;" in QT_ROW_HEADER
    assert "kThreadSummaryFaces" in QT_ROW_HEADER
    summary = QT_ROW[
        QT_ROW.index("if (!threadContext && !threadSummary.isEmpty())"):
        QT_ROW.index("m_reactionsBar = nullptr;")
    ]
    assert "threadFacePile(threadSummary.faces" in summary
    assert "thread->setIcon(" in summary
    assert "threadSummary.latestSender" in summary
    assert "threadCountLabel(threadSummary.count)" in summary
    # MainWindow owns the avatar cache, so it is what fills the summary in.
    assert "ThreadReplySummary MainWindow::chatThreadSummary(" in QT_MESSAGES
    assert "candidate.senderId, name, MessageRow::kThreadSummaryFaceSide" \
        in QT_MESSAGES
    assert "MessageRow::kThreadSummaryFaces" in QT_MESSAGES


def test_qt_chat_avatars_are_round():
    # Every face in the desktop transcript, its people pane and the profile
    # dialog is a circle, matching /chat and the World.
    assert "painter.drawEllipse(0, 0, side, side);" in QT_ROW
    assert "clip.addEllipse(0, 0, kAvatarSize, kAvatarSize);" in QT_ROW
    assert "roundedAvatar(known, side, 0.5)" in QT_MESSAGES
    assert "roundedRectPixmap(source, kChatMemberAvatarSide,\n                                              kChatMemberAvatarSide / 2.0)" in QT_MESSAGES
    assert "roundedRectPixmap(avatar, 56, 28)" in QT_MESSAGES
    # No chat face is left on the old squircle radius.
    assert "0.25" not in QT_MESSAGES[
        QT_MESSAGES.index("QPixmap MainWindow::chatActorAvatar("):
        QT_MESSAGES.index("ThreadReplySummary MainWindow::chatThreadSummary(")
    ]


def test_qt_replayed_backlog_repaints_the_transcript_once():
    # renderConversationRows() destroys and rebuilds every MessageRow. A room's
    # backlog arrives a message at a time, and each replayed thread reply or
    # out-of-order arrival asks for that rebuild, so the requests are coalesced
    # into a single repaint instead of tearing the transcript down per message.
    assert "void MainWindow::scheduleConversationRebuild(bool keepAtBottom)" \
        in QT_MESSAGES
    assert "m_transcriptRebuildPending" in QT_MESSAGES
    on_message = QT_MESSAGES[
        QT_MESSAGES.index("void MainWindow::onMessage("):
        QT_MESSAGES.index("void MainWindow::onReaction(")
    ]
    assert "scheduleConversationRebuild(wasAtBottom)" in on_message
    # The ordinary case — a new message at the end — still appends in place.
    assert "addMessageRow(message);" in on_message
    assert "renderConversationRows();" not in on_message


def test_world_embedded_chat_renders_and_sends_threads_without_top_level_bubbles():
    # World opens /dashboard/chat in its in-world panel, so this controller is
    # the actual World chat implementation as well as the dashboard one.
    assert 'this.openWorldChat("/dashboard/chat")' in WORLD
    assert "function ensureThreadDialog()" in DASHBOARD_CHAT
    assert "function renderThreadEntry(" in DASHBOARD_CHAT
    assert 'messageActionButton("Reply", () => openThread(record)' in DASHBOARD_CHAT
    assert "threadRepliesById.set(record.id, record)" in DASHBOARD_CHAT
    assert "renderThreadEntry(plain, \"peer\", !historyReplay)" in DASHBOARD_CHAT
    handler = DASHBOARD_CHAT[
        DASHBOARD_CHAT.index('} else if (type === "thread-reply")') :
        DASHBOARD_CHAT.index('} else if (type === "history")')
    ]
    assert "renderChatEntry" not in handler
    assert "emitWorldChatBubble" not in handler
