#!/usr/bin/env python3
"""Guard the face beside a chat message.

Chat messages are signed by the sender's node, so senderId is a node key, while
a website account's uploaded avatar is cached under user:<account>. The rows
read the cache with senderId alone, which drew the letter circle for every
account whose picture came from the website instead of a peer broadcast.

The rule: every transcript surface resolves the picture through
chatSenderAvatarSource(), which falls back from the sender id to the account.
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MESSAGES = (ROOT / "src/MainWindowMessages.cpp").read_text(encoding="utf-8")
SETUP = (ROOT / "src/MainWindowSetup.cpp").read_text(encoding="utf-8")


def between(source: str, start: str, end: str) -> str:
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


def main() -> None:
    resolver = between(
        MESSAGES,
        "QPixmap MainWindow::chatSenderAvatarSource(",
        "\nvoid MainWindow::refreshVisibleRowAvatars()",
    )
    assert "m_avatars.value(senderId)" in resolver
    assert 'QStringLiteral("user:") + name' in resolver
    assert "member.ownerUser" in resolver

    row = between(
        MESSAGES,
        "MessageRow *MainWindow::createMessageRow(",
        "connect(row, &MessageRow::reactionToggled",
    )
    assert "chatSenderAvatarSource(message.senderId, message.senderName)" in row
    assert "m_avatars.value(message.senderId)" not in row

    actor = between(
        MESSAGES,
        "QPixmap MainWindow::chatActorAvatar(",
        "\nThreadReplySummary MainWindow::chatThreadSummary(",
    )
    assert "chatSenderAvatarSource(senderId, name)" in actor

    on_avatar = between(
        MESSAGES, "void MainWindow::onAvatar(", "\nvoid MainWindow::onTypingChanged("
    )
    assert "refreshVisibleRowAvatars();" in on_avatar
    directory = between(
        MESSAGES,
        "void MainWindow::mergeChatUserDirectory(",
        "refreshRepositoryList();",
    )
    assert "if (avatarChanged) {\n        refreshVisibleRowAvatars();" in directory
    cached = between(
        SETUP, "void MainWindow::loadCachedAvatars()", "watcher->setFuture("
    )
    assert "refreshVisibleRowAvatars();" in cached
    for name, region in (("onAvatar", on_avatar), ("cache", cached)):
        assert "setAvatar(m_avatars.value(" not in region, name


if __name__ == "__main__":
    main()
    print("chat sender avatar contract ok")
