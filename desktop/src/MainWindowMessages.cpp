
#include "MainWindow.h"
#include "AgentActivityFeed.h"
#include "MainWindowInternal.h"
#include "DiscordClient.h"
#include "LocalMeshNode.h"
#include "OfficeChannelMirror.h"
#include "SettingsWrite.h"

#include "ChatBlocks.h"
#include "ChatFormat.h"
#include "ChatLinkText.h"
#include "ChatRecipients.h"
#include "ChatVisitorPresence.h"
#include "AppPaths.h"

#include <QApplication>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QTreeWidgetItemIterator>
#include <QSlider>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

// Qt Multimedia is an optional module (plenty of distro Qt6 installs ship
// without it), so an in-app player is a build-time upgrade over handing the
// file to the desktop's own player. See forkmesh_link_multimedia() in
// desktop/CMakeLists.txt.
#ifdef FORKMESH_HAVE_MULTIMEDIA
#include <QAudioOutput>
#include <QMediaPlayer>
#include <QVideoWidget>
#endif

using namespace forkmesh::ui;


static QString chatUserDisplayName(const MemberInfo &member);

static void addMentionProfile(QHash<QString, MemberInfo> &profiles,
                              MemberInfo member)
{
    const QString display = chatUserDisplayName(member).trimmed();
    if (display.isEmpty())
        return;
    member.name = display;
    const QString key = display.toLower();
    if (!profiles.contains(key))
        profiles.insert(key, member);
}

static QHash<QString, MemberInfo> chatMentionProfiles(
    const QHash<QString, MemberInfo> &directory,
    const QList<MemberInfo> &roster)
{
    QHash<QString, MemberInfo> profiles;
    profiles.reserve(directory.size() + roster.size());
    for (const MemberInfo &member : directory)
        addMentionProfile(profiles, member);
    for (const MemberInfo &member : roster)
        addMentionProfile(profiles, member);
    return profiles;
}

QHash<QString, MemberInfo> MainWindow::teammateChatMentionProfiles() const
{
    QHash<QString, MemberInfo> profiles =
        chatMentionProfiles(m_chatDirectoryUsers, m_homeRoster);
    const forkmesh::teammates::LoadResult loaded =
        forkmesh::teammates::Store(teammatesDirPath()).load();
    for (const forkmesh::teammates::Config &teammate : loaded.teammates) {
        const QString handle = forkmesh::teammates::mentionHandle(teammate);
        if (handle.isEmpty())
            continue;
        MemberInfo member;
        member.id = QStringLiteral("teammate:%1").arg(teammate.id);
        member.name = handle;
        member.nodeName = teammate.specialty;
        member.accountKind = QStringLiteral("user");
        member.note = QStringLiteral("AI special agent");
        member.online = teammate.enabled;
        addMentionProfile(profiles, member);
    }
    if (m_currentConversation == QLatin1String("#team") &&
        std::any_of(loaded.teammates.cbegin(), loaded.teammates.cend(),
                    [](const forkmesh::teammates::Config &teammate) {
                        return teammate.enabled;
                    })) {
        MemberInfo all;
        all.id = QStringLiteral("teammate:all");
        all.name = QStringLiteral("all");
        all.accountKind = QStringLiteral("user");
        all.note = QStringLiteral("All enabled AI special agents · one shared agent run");
        all.online = true;
        addMentionProfile(profiles, all);
    }
    return profiles;
}

static bool sameChatProfilePresentation(const MemberInfo &left,
                                        const MemberInfo &right)
{
    return left.id == right.id && left.name == right.name &&
           left.nodeName == right.nodeName &&
           left.ownerUser == right.ownerUser &&
           left.accountKind == right.accountKind && left.note == right.note &&
           left.self == right.self &&
           left.hasUserStatus == right.hasUserStatus &&
           left.userStatus == right.userStatus;
}

static bool sameChatDirectoryPresentation(
    const QHash<QString, MemberInfo> &left,
    const QHash<QString, MemberInfo> &right)
{
    if (left.size() != right.size())
        return false;
    for (auto it = left.cbegin(); it != left.cend(); ++it) {
        const auto candidate = right.constFind(it.key());
        if (candidate == right.cend() ||
            !sameChatProfilePresentation(it.value(), candidate.value())) {
            return false;
        }
    }
    return true;
}

static QUrl chatMentionProfileUrl(const QString &serverInput,
                                  const QString &accountName)
{
    QUrl base(canonicalServerUrl(serverInput));
    QString scheme = base.scheme();
    if (scheme == QLatin1String("wss"))
        scheme = QStringLiteral("https");
    else if (scheme == QLatin1String("ws"))
        scheme = QStringLiteral("http");
    else if (scheme.isEmpty())
        scheme = QStringLiteral("https");

    QUrl url;
    url.setScheme(scheme);
    url.setHost(base.host().compare(
                    QStringLiteral("app.forkmesh.com"),
                    Qt::CaseInsensitive) == 0
                    ? QStringLiteral("forkmesh.com")
                    : base.host());
    if (base.port() >= 0)
        url.setPort(base.port());
    url.setPath(QStringLiteral("/@") + accountName.trimmed().toLower());
    return url;
}

QString MainWindow::senderColor(const QString &sender) const
{
    const uint hash = qHash(sender);
    return Theme::kSenderPalette[hash % Theme::kSenderPaletteSize];
}

QPixmap MainWindow::chatSenderAvatarSource(const QString &senderId,
                                           const QString &senderName) const
{
    if (!senderId.isEmpty()) {
        const QPixmap byId = m_avatars.value(senderId);
        if (!byId.isNull())
            return byId;
    }
    const QString name = senderName.trimmed().toLower();
    if (!name.isEmpty()) {
        const QPixmap byAccount =
            m_avatars.value(QStringLiteral("user:") + name);
        if (!byAccount.isNull())
            return byAccount;
    }
    if (senderId.isEmpty())
        return QPixmap();
    for (const MemberInfo &member : std::as_const(m_homeRoster)) {
        if (member.id != senderId)
            continue;
        const QString owner = member.ownerUser.trimmed().toLower();
        if (!owner.isEmpty() && owner != name)
            return m_avatars.value(QStringLiteral("user:") + owner);
        break;
    }
    return QPixmap();
}

void MainWindow::refreshVisibleRowAvatars()
{
    for (auto it = m_visibleRows.constBegin(); it != m_visibleRows.constEnd();
         ++it) {
        const QPixmap avatar = chatSenderAvatarSource(
            it.value()->senderId(), it.value()->senderName());
        if (!avatar.isNull())
            it.value()->setAvatar(avatar);
    }
    refreshDmList();
}

void MainWindow::refreshVisibleRowStatuses()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto it = m_visibleRows.constBegin(); it != m_visibleRows.constEnd();
         ++it) {
        MessageRow *row = it.value();
        if (!row || row->senderId().startsWith(QLatin1String("teammate:")) ||
            isDiscordConversation(m_currentConversation))
            continue;
        const QString ownId = m_profileIdentity.publicKey();
        const forkmesh::userstatus::UserStatus status = userStatusFor(
            row->senderName(), row->senderId(),
            !ownId.isEmpty() && row->senderId() == ownId);
        row->setSenderStatus(status.emoji,
                             forkmesh::userstatus::tooltip(status, now));
    }
}

forkmesh::userstatus::UserStatus MainWindow::userStatusFor(
    const QString &account, const QString &peerId, bool self) const
{
    using forkmesh::userstatus::visible;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QString key = account.trimmed().toLower();
    const QString ownKey = accountOwner().trimmed().toLower();
    const QString ownId = m_profileIdentity.publicKey();
    if (m_ownUserStatusKnown &&
        (self || (!key.isEmpty() && key == ownKey) ||
         (!peerId.isEmpty() && peerId == ownId)))
        return visible(m_ownUserStatus, now);
    if (!peerId.isEmpty()) {
        for (const MemberInfo &member : std::as_const(m_homeRoster)) {
            if (member.id != peerId)
                continue;
            if (!member.self && member.online && member.hasUserStatus)
                return visible(member.userStatus, now);
            const QString owner = member.ownerUser.trimmed().toLower();
            if (!owner.isEmpty())
                key = owner;
            break;
        }
    }
    if (key.isEmpty())
        return {};
    // Any live device of the account that advertised a status beats the
    // directory, which can lag a change by its cache lifetime.
    for (const MemberInfo &member : std::as_const(m_homeRoster)) {
        if (member.self || !member.online || !member.hasUserStatus)
            continue;
        const QString owner = member.ownerUser.trimmed().toLower();
        if ((owner.isEmpty() ? member.name.trimmed().toLower() : owner) == key)
            return visible(member.userStatus, now);
    }
    const auto directory = m_chatDirectoryUsers.constFind(key);
    if (directory != m_chatDirectoryUsers.constEnd() &&
        directory->hasUserStatus)
        return visible(directory->userStatus, now);
    return {};
}

void MainWindow::refreshUserStatusPresentation()
{
    refreshChatMembers();
    refreshVisibleRowStatuses();
    refreshDmList();
    updateUserSwitcher(); // own avatar's away badge and the status tooltip
    scheduleUserStatusExpiry();
}

void MainWindow::scheduleUserStatusExpiry()
{
    // One timer for every status on screen: it fires when the soonest one
    // lapses, repaints, and re-arms for the next.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    qint64 next = 0;
    const auto consider = [&next, now](const forkmesh::userstatus::UserStatus &s) {
        if (s.expiresAtMs > now && (next == 0 || s.expiresAtMs < next))
            next = s.expiresAtMs;
    };
    if (m_ownUserStatusKnown)
        consider(m_ownUserStatus);
    for (const MemberInfo &member : std::as_const(m_homeRoster))
        if (member.hasUserStatus)
            consider(member.userStatus);
    for (const MemberInfo &member : std::as_const(m_chatDirectoryUsers))
        if (member.hasUserStatus)
            consider(member.userStatus);
    if (next == 0) {
        if (m_userStatusExpiryTimer)
            m_userStatusExpiryTimer->stop();
        return;
    }
    if (!m_userStatusExpiryTimer) {
        m_userStatusExpiryTimer = new QTimer(this);
        m_userStatusExpiryTimer->setSingleShot(true);
        connect(m_userStatusExpiryTimer, &QTimer::timeout, this,
                &MainWindow::refreshUserStatusPresentation);
    }
    // QTimer takes an int; a status days away is simply re-armed on the
    // way. The extra second lands the repaint after the lapse, not on it.
    const qint64 delay = qBound<qint64>(1000, next - now + 1000,
                                        qint64(24) * 60 * 60 * 1000);
    m_userStatusExpiryTimer->start(int(delay));
}

QPixmap MainWindow::chatActorAvatar(const QString &senderId,
                                    const QString &senderName, int side) const
{
    const QString name = senderName.trimmed();
    const QPixmap known = chatSenderAvatarSource(senderId, name);
    if (!known.isNull())
        return roundedAvatar(known, side, 0.5); // round, like every chat face
    if (name.isEmpty() && senderId.trimmed().isEmpty())
        return QPixmap();
    return MessageRow::initialsAvatar(name, senderColor(name), side);
}

QString MainWindow::chatNotificationIcon(const QString &senderId,
                                         const QString &senderName) const
{
    const QPixmap face = chatActorAvatar(senderId, senderName, 128);
    if (face.isNull())
        return QString();
    const QString directory = forkmesh::paths::cacheRoot() +
                              QStringLiteral("/notification-avatars");
    if (!QDir().mkpath(directory))
        return QString();
    const QString key = QString::fromLatin1(
        QCryptographicHash::hash(
            (senderId + QLatin1Char('\n') + senderName.trimmed().toLower())
                .toUtf8(),
            QCryptographicHash::Sha256)
            .toHex()
            .left(32));
    const QString path =
        directory + QLatin1Char('/') + key + QStringLiteral(".png");
    return face.save(path, "PNG") ? path : QString();
}

ThreadReplySummary MainWindow::chatThreadSummary(
    const QString &conversation, const QString &rootMessageId) const
{
    ThreadReplySummary summary;
    if (rootMessageId.isEmpty())
        return summary;
    const QList<ChatMessage> &history = m_history.value(conversation);
    // Walk backwards: the summary leads with whoever replied last, and shows
    // each replier once no matter how often they answered.
    QSet<QString> seen;
    for (int index = history.size() - 1; index >= 0; --index) {
        const ChatMessage &candidate = history.at(index);
        if (candidate.threadRootId != rootMessageId || candidate.deleted)
            continue;
        ++summary.count;
        const QString name = candidate.senderName.trimmed();
        if (summary.latestSender.isEmpty())
            summary.latestSender = name;
        // senderId identifies a person; two accounts can share a display name.
        const QString identity = candidate.senderId.trimmed().isEmpty()
                                     ? name.toLower()
                                     : candidate.senderId.trimmed();
        if (identity.isEmpty() || seen.contains(identity))
            continue;
        seen.insert(identity);
        if (summary.faces.size() >= MessageRow::kThreadSummaryFaces)
            continue;
        const QPixmap face = chatActorAvatar(
            candidate.senderId, name, MessageRow::kThreadSummaryFaceSide);
        if (!face.isNull())
            summary.faces.append(face);
    }
    return summary;
}

MessageRow *MainWindow::createMessageRow(const ChatMessage &message,
                                         bool threadContext,
                                         const QHash<QString, MemberInfo> *profiles)
{
    if (message.deleted)
        return nullptr;

    QHash<QString, MemberInfo> localProfiles;
    QHash<QString, MemberInfo> externalProfiles;
    if (isDiscordConversation(message.conversation)) {
        // A Discord display name must never resolve to a same-named ForkMesh
        // account; those are separate identity domains. Repository teammates
        // are local command handles, though, so keep just those profiles.
        if (!profiles) {
            localProfiles = teammateChatMentionProfiles();
            profiles = &localProfiles;
        }
        for (auto it = profiles->cbegin(); it != profiles->cend(); ++it) {
            if (it.value().id.startsWith(QLatin1String("teammate:")))
                externalProfiles.insert(it.key(), it.value());
        }
        profiles = &externalProfiles;
    } else if (!profiles) {
        localProfiles = teammateChatMentionProfiles();
        profiles = &localProfiles;
    }

    const bool canModerate =
        m_isAdmin && !isOfficeConversation(message.conversation) &&
        !isDiscordConversation(message.conversation);
    const ThreadReplySummary threadSummary =
        threadContext ? ThreadReplySummary()
                      : chatThreadSummary(message.conversation, message.id);
    auto *row = new MessageRow(message, senderColor(message.senderName),
                               *profiles, canModerate, nullptr,
                               threadSummary, threadContext);
    const QPixmap senderAvatar =
        chatSenderAvatarSource(message.senderId, message.senderName);
    if (!senderAvatar.isNull())
        row->setAvatar(senderAvatar);
    if (!isDiscordConversation(message.conversation) &&
        !message.senderId.startsWith(QLatin1String("teammate:"))) {
        const forkmesh::userstatus::UserStatus status = userStatusFor(
            message.senderName, message.senderId, message.self);
        row->setSenderStatus(status.emoji,
                             forkmesh::userstatus::tooltip(
                                 status, QDateTime::currentMSecsSinceEpoch()));
    }
    if (m_reactions.contains(message.id))
        row->setReactions(m_reactions.value(message.id));
    row->setReceiptMark(chatReceiptMark(message));
    if (message.self && isLocalMeshConversation(message.conversation))
        applyLocalMeshQueuedMark(row);
    connect(row, &MessageRow::reactionToggled, this,
            [this](const QString &messageId, const QString &emoji) {
                toggleChatReaction(m_currentConversation, messageId, emoji);
            });
    connect(row, &MessageRow::threadRequested, this,
            &MainWindow::openChatThread);
    connect(row, &MessageRow::editRequested, this, &MainWindow::promptEditMessage);
    connect(row, &MessageRow::createIssueRequested, this,
            &MainWindow::promptIssueFromChatMessage);
    connect(row, &MessageRow::sendToPromptRequested, this,
            &MainWindow::sendMessageToPrompt);
    connect(row, &MessageRow::deleteRequested, this, &MainWindow::confirmDeleteMessage);
    connect(row, &MessageRow::moderateDeleteRequested, this,
            &MainWindow::confirmAdminDeleteMessage);
    connect(row, &MessageRow::saveFileRequested, this,
            &MainWindow::saveIncomingFile);
    connect(row, &MessageRow::imageActivated, this,
            &MainWindow::showChatImageDetail);
    connect(row, &MessageRow::videoActivated, this,
            &MainWindow::showChatVideoDetail);
    connect(row, &MessageRow::senderClicked, this,
            &MainWindow::showChatSenderProfile);
    connect(row, &MessageRow::linkActivated, this,
            [](const QString &url) {
                const QUrl target(url, QUrl::TolerantMode);
                if (target.isValid() && !target.isRelative())
                    QDesktopServices::openUrl(target);
            });
    connect(row, &MessageRow::mentionClicked, this,
            [this](const QString &id, const QString &name) {
                if (id.startsWith(QLatin1String("teammate:"))) {
                    const QString teammateId =
                        id.mid(QStringLiteral("teammate:").size()).trimmed();
                    const bool currentRepoIsWritable =
                        m_repoDetailIndex >= 0 &&
                        m_repoDetailIndex < m_repositories.size() &&
                        !m_repositories.at(m_repoDetailIndex).previewOnly &&
                        !m_repositories.at(m_repoDetailIndex).localPath.isEmpty();
                    const int teammateRepoIndex =
                        currentRepoIsWritable ? m_repoDetailIndex
                                              : activeTeammateRepoIndex();
                    if (teammateId == QLatin1String("all") &&
                        teammateRepoIndex >= 0) {
                        setActiveTeammateRepoIndex(teammateRepoIndex);
                        showSection(kTeamSectionIndex);
                        return;
                    }
                    if (!teammateId.isEmpty() && teammateRepoIndex >= 0) {
                        // A mention belongs to the repository that surfaced
                        // it, even if the Team section was last viewing a
                        // different repository.
                        setActiveTeammateRepoIndex(teammateRepoIndex);
                        showSection(kTeamSectionIndex);
                        QTimer::singleShot(
                            0, this,
                            [this, teammateId] {
                                showTeammateHome(teammateId);
                            });
                    } else {
                        flashMessage(
                            QStringLiteral("Open a repository to view @%1.")
                                .arg(name));
                    }
                    return;
                }
                if (id.startsWith(QStringLiteral("user:"))) {
                    QString account = id.mid(QStringLiteral("user:").size()).trimmed();
                    if (account.isEmpty())
                        account = name.trimmed().toLower();
                    if (m_chatDirectoryUsers.contains(account.toLower())) {
                        showChatSenderProfile(id, name);
                        return;
                    }
                    const QUrl url = chatMentionProfileUrl(
                        m_serverUrlEdit ? m_serverUrlEdit->text() : QString(),
                        account);
                    if (url.isValid() && !url.host().isEmpty()) {
                        QDesktopServices::openUrl(url);
                        return;
                    }
                }
                showSection(0);
                showNodeProfile(id, name);
            });
    return row;
}

QWidget *MainWindow::chatDayDivider(const QDate &day)
{
    auto *divider = new QWidget;
    divider->setObjectName(QStringLiteral("chatDayDivider"));
    auto *label = new QLabel(MessageRow::chatDayLabel(day));
    label->setObjectName(QStringLiteral("chatDayDividerLabel"));
    const auto rule = [] {
        auto *line = new QFrame;
        line->setObjectName(QStringLiteral("chatDayDividerRule"));
        line->setFixedHeight(1);
        return line;
    };
    auto *layout = new QHBoxLayout(divider);
    layout->setContentsMargins(16, 20, 16, 4);
    layout->setSpacing(6);
    layout->addWidget(rule(), 1, Qt::AlignVCenter);
    layout->addWidget(label, 0, Qt::AlignVCenter);
    layout->addWidget(rule(), 1, Qt::AlignVCenter);
    return divider;
}

MessageRow *MainWindow::addMessageRow(
    const ChatMessage &message,
    const QHash<QString, MemberInfo> *mentionProfiles)
{
    // Discord replies remain ordinary channel messages. They can still be
    // opened in ForkMesh's reply drawer, but hiding them from the transcript
    // would not match the Discord channel users are collaborating with.
    if (!message.threadRootId.isEmpty() &&
        !isDiscordConversation(message.conversation)) {
        return nullptr;
    }
    MessageRow *row = createMessageRow(message, false, mentionProfiles);
    if (!row)
        return nullptr;
    MessageRow *previous = nullptr;
    if (m_messageLayout->count() > 1) {
        if (QLayoutItem *last = m_messageLayout->itemAt(m_messageLayout->count() - 2))
            previous = qobject_cast<MessageRow *>(last->widget());
    }
    const QDate day = QDateTime::fromMSecsSinceEpoch(message.timestampMs).date();
    if (Theme::isNext() &&
        (!previous ||
         QDateTime::fromMSecsSinceEpoch(previous->timestampMs()).date() != day)) {
        m_messageLayout->insertWidget(m_messageLayout->count() - 1,
                                      chatDayDivider(day));
        previous = nullptr;
    }
    row->setContinuation(row->continues(previous));
    m_messageLayout->insertWidget(m_messageLayout->count() - 1, row);
    row->show();
    m_visibleRows.insert(message.id, row);
    return row;
}

void MainWindow::openChatThread(const QString &rootMessageId)
{
    if (rootMessageId.trimmed().isEmpty() ||
        isDirectConversation(m_currentConversation))
        return;
    const auto &history = m_history.value(m_currentConversation);
    const bool discord = isDiscordConversation(m_currentConversation);
    const bool rootExists = std::any_of(
        history.cbegin(), history.cend(), [&](const ChatMessage &message) {
            return message.id == rootMessageId &&
                   (message.threadRootId.isEmpty() || discord);
        });
    if (!rootExists)
        return;

    m_activeChatThreadRootId = rootMessageId.left(96);
    if (!m_chatThreadPane || !m_chatThreadRowsLayout || !m_chatThreadInput)
        return;
    if (m_chatThreadTitle) {
        m_chatThreadTitle->setText(
            discord ? QStringLiteral("Discord replies")
                    : QStringLiteral("Thread"));
    }
    m_chatThreadInput->setPlaceholderText(
        discord ? QStringLiteral("Reply on Discord…")
                : QStringLiteral("Reply in thread…"));
    rebuildChatThreadPane();
    m_chatThreadPane->show();
    if (m_chatSplit) {
        QList<int> sizes = m_chatSplit->sizes();
        if (sizes.size() == 3 && sizes.at(2) < kChatThreadPaneMinWidth) {
            sizes[2] = kChatThreadPaneDefaultWidth;
            m_chatSplit->setSizes(sizes);
        }
    }
    m_chatThreadInput->setFocus();
}

void MainWindow::closeChatThread()
{
    m_activeChatThreadRootId.clear();
    if (m_chatThreadPane)
        m_chatThreadPane->hide();
}

void MainWindow::rebuildChatThreadPane()
{
    if (!m_chatThreadPane || !m_chatThreadRowsLayout ||
        m_activeChatThreadRootId.isEmpty())
        return;
    while (QLayoutItem *item = m_chatThreadRowsLayout->takeAt(0)) {
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }
    const QList<ChatMessage> history = m_history.value(m_currentConversation);
    const bool discord = isDiscordConversation(m_currentConversation);
    const auto root = std::find_if(
        history.cbegin(), history.cend(), [&](const ChatMessage &message) {
            return message.id == m_activeChatThreadRootId &&
                   (message.threadRootId.isEmpty() || discord);
        });
    const QHash<QString, MemberInfo> mentionProfiles =
        teammateChatMentionProfiles();
    if (root != history.cend()) {
        if (MessageRow *row = createMessageRow(*root, true, &mentionProfiles))
            m_chatThreadRowsLayout->addWidget(row);
    }
    int replies = 0;
    for (const ChatMessage &message : history) {
        if (message.threadRootId != m_activeChatThreadRootId)
            continue;
        if (MessageRow *row =
                createMessageRow(message, true, &mentionProfiles)) {
            m_chatThreadRowsLayout->addWidget(row);
            ++replies;
        }
    }
    m_chatThreadRowsLayout->addStretch();
    if (m_chatThreadCountLabel) {
        m_chatThreadCountLabel->setText(
            QString::number(replies) +
            (replies == 1 ? " reply" : " replies"));
    }
}

void MainWindow::sendChatThreadReply()
{
    if (!m_chatThreadInput || m_activeChatThreadRootId.isEmpty())
        return;
    const QString text = m_chatThreadInput->toPlainText().trimmed().left(16000);
    if (text.isEmpty())
        return;
    if (isDiscordConversation(m_currentConversation)) {
        if (m_discordClient &&
            m_discordClient->sendReply(m_currentConversation,
                                       m_activeChatThreadRootId, text)) {
            maybeAskForkbot(
                m_currentConversation, text, m_activeChatThreadRootId);
            m_chatThreadInput->clear();
            triggerTeammatesForEvent(
                QStringLiteral("mentions"),
                QStringLiteral("Chat thread in %1\n%2")
                    .arg(m_currentConversation, text), -1,
                m_currentConversation, m_activeChatThreadRootId);
        }
        return;
    }
    if (isOfficeConversation(m_currentConversation)) {
        if (!m_officeChannelMirror ||
            !m_officeChannelMirror->sendThreadReply(
                m_currentConversation, m_activeChatThreadRootId, text)) {
            logSystem(QStringLiteral("Office chat: %1 is not currently writable.")
                          .arg(m_currentConversation));
            return;
        }
        m_chatThreadInput->clear();
        maybeAskForkbot(m_currentConversation, text, m_activeChatThreadRootId);
        triggerTeammatesForEvent(
            QStringLiteral("mentions"),
            QStringLiteral("Chat thread in %1\n%2")
                .arg(m_currentConversation, text), -1,
            m_currentConversation, m_activeChatThreadRootId);
        return;
    }
    if (!m_backend)
        return;
    m_chatThreadInput->clear();
    m_backend->sendThreadReply(m_currentConversation,
                               m_activeChatThreadRootId, text);
    maybeAskForkbot(m_currentConversation, text, m_activeChatThreadRootId);
    triggerTeammatesForEvent(
        QStringLiteral("mentions"),
        QStringLiteral("Chat thread in %1\n%2")
            .arg(m_currentConversation, text), -1,
        m_currentConversation, m_activeChatThreadRootId);
}

#ifdef FORKMESH_WINDOW_TESTS
QStringList MainWindow::testTranscriptRowOrder() const
{
    QStringList texts;
    if (!m_messageLayout)
        return texts;
    const QList<ChatMessage> &history = m_history.value(m_currentConversation);
    for (int index = 0; index < m_messageLayout->count(); ++index) {
        QLayoutItem *item = m_messageLayout->itemAt(index);
        auto *row = item ? qobject_cast<MessageRow *>(item->widget()) : nullptr;
        if (!row)
            continue;
        for (const ChatMessage &message : history) {
            if (message.id == row->messageId()) {
                texts.append(message.text);
                break;
            }
        }
    }
    return texts;
}

QString MainWindow::testChatThreadCountText() const
{
    return m_chatThreadCountLabel ? m_chatThreadCountLabel->text() : QString();
}

QStringList MainWindow::testChatMentionCandidates()
{
    refreshMentionCandidates();
    return m_mentionModel ? m_mentionModel->stringList() : QStringList();
}
#endif

void MainWindow::renderConversationRows()
{
    // Whatever a queued rebuild was going to draw, this call is drawing now —
    // so a conversation switch (which renders synchronously) is not followed by
    // a stale repaint that yanks the reader back to the bottom.
    m_transcriptRebuildPending = false;
    m_transcriptRebuildStickToBottom = false;
    m_visibleRows.clear();
    while (m_messageLayout->count() > 1) {
        QLayoutItem *item = m_messageLayout->takeAt(0);
        if (QWidget *widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
    // MessageRow keeps a copy of the mention directory. Build that directory
    // once per transcript, not once per message: a few hundred messages times
    // a large user roster showed up as QHash rehashing in the stall samples.
    const QHash<QString, MemberInfo> mentionProfiles =
        teammateChatMentionProfiles();
    for (const ChatMessage &message : m_history.value(m_currentConversation)) {
        if (message.threadRootId.isEmpty() ||
            isDiscordConversation(message.conversation)) {
            addMessageRow(message, &mentionProfiles);
        }
    }
}

void MainWindow::rebuildConversationView()
{
    renderConversationRows();
    scrollToBottom();
}

// renderConversationRows() destroys and rebuilds every MessageRow in the
// transcript. Joining a room replays its backlog one message at a time, and
// each replayed thread reply — or any message that lands earlier than the one
// before it — asks for that rebuild, so opening a busy channel used to tear
// the whole transcript down dozens of times in a row. Requests made in the
// same burst collapse into a single repaint here instead.
void MainWindow::scheduleConversationRebuild(bool keepAtBottom)
{
    m_transcriptRebuildStickToBottom =
        m_transcriptRebuildStickToBottom || keepAtBottom;
    if (m_transcriptRebuildPending)
        return;
    m_transcriptRebuildPending = true;
    QTimer::singleShot(0, this, [this] {
        // A direct renderConversationRows() in the meantime already drew it.
        if (!m_transcriptRebuildPending)
            return;
        m_transcriptRebuildPending = false;
        const bool keepBottom = m_transcriptRebuildStickToBottom;
        renderConversationRows();
        if (keepBottom)
            scrollToBottom();
    });
}

void MainWindow::scrollToBottom()
{
    m_stickToBottom = true;
    QTimer::singleShot(0, this, [this] {
        QScrollBar *bar = m_messageScroll->verticalScrollBar();
        bar->setValue(bar->maximum());
    });
}

void MainWindow::onMessage(const ChatMessage &message)
{
    const QString conversation = message.conversation;
    if (conversation.isEmpty())
        return;

    if (message.timestampMs <=
        forkmesh::chatretention::expiryCutoff(
            conversation, QDateTime::currentMSecsSinceEpoch()))
        return;

    if (!message.id.isEmpty()) {
        if (m_historyIds.contains(message.id)) {
            if (message.recipientCount >= 0)
                applyMessageRecipients(conversation, message.id,
                                       message.recipientCount,
                                       message.recipientNames);
            // REST history is also the reconciliation path after ForkMesh was
            // offline. Refresh a cached Discord row in place so edits and
            // attachment metadata do not remain stale merely because its ID
            // was already persisted locally.
            if (isDiscordConversation(conversation)) {
                QList<ChatMessage> &messages = m_history[conversation];
                auto existing = std::find_if(
                    messages.begin(), messages.end(),
                    [&message](const ChatMessage &candidate) {
                        return candidate.id == message.id;
                    });
                if (existing != messages.end()) {
                    const bool changed =
                        existing->conversation != message.conversation ||
                        existing->threadRootId != message.threadRootId ||
                        existing->senderId != message.senderId ||
                        existing->senderName != message.senderName ||
                        existing->text != message.text ||
                        existing->timestampMs != message.timestampMs ||
                        existing->fileName != message.fileName ||
                        existing->fileMime != message.fileMime ||
                        existing->fileData != message.fileData ||
                        existing->self != message.self ||
                        existing->edited != message.edited ||
                        existing->deleted != message.deleted;
                    if (!changed)
                        return;
                    *existing = message;
                    refreshChatListPreview(conversation);
                    if (conversation == m_currentConversation &&
                        !m_discordTranscriptRefreshPending) {
                        m_discordTranscriptRefreshPending = true;
                        QTimer::singleShot(0, this, [this] {
                            m_discordTranscriptRefreshPending = false;
                            if (isDiscordConversation(m_currentConversation)) {
                                rebuildConversationView();
                                if (m_chatThreadPane &&
                                    m_chatThreadPane->isVisible())
                                    rebuildChatThreadPane();
                            }
                        });
                    }
                    scheduleChatSave();
                }
            }
            return;
        }
        m_historyIds.insert(message.id);
    }

    if (isDirectConversation(conversation)) {
        const QString peerId = dmPeerId(conversation);
        if (message.senderId == peerId && !message.senderName.isEmpty())
            m_dmNames.insert(peerId, message.senderName);
        if (!m_openDms.contains(peerId)) {
            m_openDms.append(peerId);
            refreshDmList();
        }
    }

    QList<ChatMessage> &conversationHistory = m_history[conversation];
    int insertAt = conversationHistory.size();
    while (insertAt > 0 && conversationHistory.at(insertAt - 1).timestampMs > message.timestampMs)
        --insertAt;
    conversationHistory.insert(insertAt, message);
    const bool appendedAtEnd = insertAt == conversationHistory.size() - 1;
    refreshChatListPreview(conversation);

    if (conversation == m_currentConversation) {
        const bool wasAtBottom = m_stickToBottom;
        // A reply changes its root's summary row, and a message that landed
        // earlier than the one before it changes the order — both need the
        // whole transcript redrawn, so they queue one coalesced rebuild. The
        // ordinary case, a new message at the end, still appends in place.
        if (!message.threadRootId.isEmpty() || !appendedAtEnd) {
            scheduleConversationRebuild(wasAtBottom);
        } else {
            addMessageRow(message);
            if (wasAtBottom)
                scrollToBottom();
        }
        if (message.threadRootId == m_activeChatThreadRootId)
            rebuildChatThreadPane();
    }
    replayPendingOfficeMutations(message.id);
    const QString ownChatName = chatDisplayName().trimmed();
    const bool ownMessage =
        message.self ||
        (!ownChatName.isEmpty() &&
         message.senderName.trimmed().compare(ownChatName, Qt::CaseInsensitive)
             == 0);
    const bool forkbotMessage =
        message.senderId.compare(QStringLiteral("forkbot"),
                                 Qt::CaseInsensitive) == 0 ||
        message.senderName.compare(QStringLiteral("forkbot"),
                                   Qt::CaseInsensitive) == 0;
    // Browser and guest messages are received here rather than sent through
    // this composer's sendCurrentMessage path. Ask ForkBot for a live public
    // mention too, otherwise it only ever answers the desktop's own author.
    // Excluding its relayed identity prevents its help text from retriggering
    // another request when it contains the word "forkbot".
    if (!message.historical && !ownMessage &&
        !message.senderId.startsWith(QLatin1String("teammate:"))) {
        if (!forkbotMessage)
            maybeAskForkbot(conversation, message.text, message.threadRootId);
        triggerTeammatesForEvent(
            QStringLiteral("mentions"),
            QStringLiteral("Chat message in %1 from %2\n%3")
                .arg(conversation, message.senderName, message.text), -1,
            conversation,
            message.threadRootId.isEmpty() ? message.id
                                           : message.threadRootId);
    }

    if (conversation == m_currentConversation && isChatViewVisible())
        queueReadMark(conversation);
    if (!message.historical && !ownMessage &&
        message.timestampMs > m_readMarks.value(conversation) &&
        !(conversation == m_currentConversation && isChatViewVisible())) {
        m_unread.insert(conversation);
        ++m_unreadCounts[conversation];
        refreshChannelList();
        refreshDmList();
    }
    updateChatButton();

    if (!message.historical && !ownMessage &&
        conversation == kWelcomeChannel &&
        message.senderId != QLatin1String("system:welcome") &&
        message.timestampMs >
            QDateTime::currentMSecsSinceEpoch() - kWelcomePingFreshMs) {
        NotificationLink link;
        link.kind = QStringLiteral("chat");
        link.ref = kWelcomeChannel;
        const QString who = message.senderName.trimmed();
        AppNotification item;
        item.title = QStringLiteral("New user joined");
        item.body = who.isEmpty()
                        ? QStringLiteral("Someone new said hello in #welcome")
                        : who + QStringLiteral(" said hello in #welcome");
        item.link = link;
        item.kind = QStringLiteral("chat");
        item.actor = who;
        item.actorId = message.senderId;
        item.chatMessageId = message.id;
        recordNotification(item);
    }

    if (!message.historical && !ownMessage) {
        const bool privateConversation =
            isPrivateChatConversation(conversation);
        const QString where =
            isDirectConversation(conversation) ||
                    forkmesh::office::isDirectOfficeConversation(conversation)
                ? QStringLiteral("sent you a message")
                : QStringLiteral("in ") + chatConversationLabel(conversation);
        const QString formatted =
            !message.blocks.isEmpty() &&
                    forkmesh::chatblocks::senderMayFormat(message.senderId)
                ? forkmesh::chatblocks::plainSummary(message.blocks)
                : forkmesh::chatformat::toPlainText(message.text);
        const QString preview = forkmesh::chatformat::notificationText(
            message.hasFile() ? QStringLiteral("\U0001F4CE ") + message.fileName +
                                    QLatin1Char('\n') + formatted
                              : formatted);
        constexpr qint64 kChatPingFreshMs = 10 * 60 * 1000;
        if (!privateConversation && conversation != kWelcomeChannel &&
            message.timestampMs >
                QDateTime::currentMSecsSinceEpoch() - kChatPingFreshMs) {
            NotificationLink link;
            link.kind = QStringLiteral("chat");
            link.ref = conversation;
            AppNotification item;
            item.title = message.senderName + QLatin1Char(' ') + where;
            item.body = preview.simplified();
            item.link = link;
            item.kind = QStringLiteral("chat");
            item.actor = message.senderName;
            item.actorId = message.senderId;
            item.chatMessageId = message.id;
            recordNotification(item);
        }
        const QString mentionTarget =
            isDiscordConversation(conversation) && m_discordClient
                ? m_discordClient->botName()
                : m_userName;
        if (textMentionsNodeName(message.text, mentionTarget)) {
            if (notifyEnabled(kMentionAlertSetting)) {
                if (!pingsPaused() && nativeSystemNotificationsEnabled())
                    QApplication::alert(this, 0);
                const QString mentionTitle =
                    where.startsWith(QLatin1String("in "))
                        ? message.senderName + QStringLiteral(" mentioned you ") + where
                        : message.senderName + QStringLiteral(" mentioned you");
                postNotification(mentionTitle, preview, /*warning=*/false,
                                 chatNotificationIcon(message.senderId,
                                                      message.senderName),
                                 QString(), false, !privateConversation);
            }
        } else if (notifyEnabled(kChatMessageAlertSetting) &&
                   !isActiveWindow()) {
            notifyIfInactive(message.senderName + " " + where, preview,
                             QString(),
                             chatNotificationIcon(message.senderId,
                                                  message.senderName),
                             !privateConversation);
        }
    }
    scheduleChatSave();
}

bool MainWindow::canReactInConversation(const QString &conversation) const
{
    if (conversation.isEmpty())
        return false;
    if (isDiscordConversation(conversation))
        return m_discordClient != nullptr;
    if (isOfficeConversation(conversation))
        return m_officeChannelMirror &&
               m_officeChannelMirror->canSend(conversation);
    return m_backend != nullptr;
}

void MainWindow::toggleChatReaction(const QString &conversation,
                                    const QString &messageId,
                                    const QString &reaction)
{
    if (messageId.isEmpty() || reaction.isEmpty() ||
        !canReactInConversation(conversation))
        return;
    if (isDiscordConversation(conversation))
        m_discordClient->toggleReaction(conversation, messageId, reaction);
    else if (isOfficeConversation(conversation))
        m_officeChannelMirror->sendReaction(
            conversation, messageId, reaction,
            !hasOwnChatReaction(conversation, messageId, reaction));
    else
        m_backend->sendReaction(conversation, messageId, reaction);
}

bool MainWindow::hasOwnChatReaction(const QString &conversation,
                                    const QString &messageId,
                                    const QString &reaction) const
{
    const QString self =
        isDiscordConversation(conversation) && m_discordClient
            ? m_discordClient->botName()
            : chatDisplayName();
    return !self.isEmpty() &&
           m_reactions.value(messageId).value(reaction).contains(self);
}

void MainWindow::onReaction(const QString &conversation, const QString &messageId,
                            const QString &emoji, const QString &reactorName,
                            bool added)
{
    Q_UNUSED(conversation);
    QStringList &reactors = m_reactions[messageId][emoji];
    if (added) {
        if (!reactors.contains(reactorName))
            reactors.append(reactorName);
    } else {
        reactors.removeAll(reactorName);
        if (reactors.isEmpty())
            m_reactions[messageId].remove(emoji);
    }
    if (auto *row = m_visibleRows.value(messageId))
        row->setReactions(m_reactions.value(messageId));
}

void MainWindow::onDiscordReactionsLoaded(
    const QString &conversation, const QString &messageId,
    const QMap<QString, QStringList> &reactions)
{
    Q_UNUSED(conversation);
    if (reactions.isEmpty())
        m_reactions.remove(messageId);
    else
        m_reactions.insert(messageId, reactions);
    if (auto *row = m_visibleRows.value(messageId))
        row->setReactions(m_reactions.value(messageId));
}

void MainWindow::onMessageEdited(const QString &conversation, const QString &messageId,
                                 const QString &newText)
{
    QList<ChatMessage> &messages = m_history[conversation];
    for (ChatMessage &message : messages) {
        if (message.id == messageId) {
            message.text = newText.left(16000);
            message.edited = true;
            break;
        }
    }
    refreshChatListPreview(conversation);
    if (conversation == m_currentConversation)
        rebuildConversationView();
    if (conversation == m_currentConversation && m_chatThreadPane &&
        m_chatThreadPane->isVisible())
        rebuildChatThreadPane();
    scheduleChatSave();
}

void MainWindow::applyMessageRecipients(const QString &conversation,
                                        const QString &messageId,
                                        int recipients,
                                        const QStringList &names)
{
    if (recipients < 0 || messageId.isEmpty() || !m_history.contains(conversation))
        return;
    QList<ChatMessage> &messages = m_history[conversation];
    auto existing = std::find_if(messages.begin(), messages.end(),
                                 [&messageId](const ChatMessage &candidate) {
                                     return candidate.id == messageId;
                                 });
    if (existing == messages.end() ||
        (existing->recipientCount == recipients &&
         (names.isEmpty() || existing->recipientNames == names)))
        return;
    existing->recipientCount = recipients;
    if (!names.isEmpty())
        existing->recipientNames = names;
    if (conversation == m_currentConversation) {
        if (auto *row = m_visibleRows.value(messageId)) {
            row->setRecipientCount(recipients, existing->recipientNames);
            row->setReceiptMark(chatReceiptMark(*existing));
        }
        if (m_chatThreadPane && m_chatThreadPane->isVisible() &&
            !m_activeChatThreadRootId.isEmpty() &&
            (existing->id == m_activeChatThreadRootId ||
             existing->threadRootId == m_activeChatThreadRootId))
            rebuildChatThreadPane();
    }
    scheduleChatSave();
}

forkmesh::recipients::ReceiptMark MainWindow::chatReceiptMark(
    const ChatMessage &message) const
{
    const QString &conversation = message.conversation;
    if (!message.self || message.deleted || conversation.isEmpty() ||
        isDiscordConversation(conversation) ||
        isLocalMeshConversation(conversation) ||
        isDirectConversation(conversation) ||
        conversation == QLatin1String("#team") ||
        message.senderId.startsWith(QLatin1String("teammate:")))
        return forkmesh::recipients::ReceiptMark::None;
    return forkmesh::recipients::receiptMark(true, message.recipientCount,
                                             message.receipt,
                                             message.historical);
}

void MainWindow::refreshReceiptRow(const ChatMessage &message)
{
    if (message.conversation != m_currentConversation)
        return;
    if (auto *row = m_visibleRows.value(message.id))
        row->setReceiptMark(chatReceiptMark(message));
    if (m_chatThreadPane && m_chatThreadPane->isVisible() &&
        !m_activeChatThreadRootId.isEmpty() &&
        (message.id == m_activeChatThreadRootId ||
         message.threadRootId == m_activeChatThreadRootId))
        rebuildChatThreadPane();
}

void MainWindow::applyMessagesDelivered(const QString &conversation,
                                        const QStringList &messageIds)
{
    if (messageIds.isEmpty() || !m_history.contains(conversation))
        return;
    bool changed = false;
    for (ChatMessage &message : m_history[conversation]) {
        if (!message.self || message.deleted ||
            message.receipt != forkmesh::recipients::Receipt::None ||
            !messageIds.contains(message.id))
            continue;
        message.receipt = forkmesh::recipients::Receipt::Delivered;
        changed = true;
        refreshReceiptRow(message);
    }
    if (changed)
        scheduleChatSave();
}

void MainWindow::applyConversationRead(const QString &conversation)
{
    if (!m_history.contains(conversation))
        return;
    bool changed = false;
    for (ChatMessage &message : m_history[conversation]) {
        if (!message.self || message.deleted ||
            message.receipt == forkmesh::recipients::Receipt::Read ||
            (message.recipientCount < 0 && !message.historical &&
             message.receipt == forkmesh::recipients::Receipt::None))
            continue;
        message.receipt = forkmesh::recipients::Receipt::Read;
        changed = true;
        refreshReceiptRow(message);
    }
    if (changed)
        scheduleChatSave();
}

bool MainWindow::officeMutationTargetReady(const QString &conversation,
                                           const QString &messageId,
                                           const QJsonObject &mutation)
{
    if (messageId.isEmpty())
        return false;
    const QList<ChatMessage> &messages = m_history.value(conversation);
    const bool present = std::any_of(
        messages.cbegin(), messages.cend(),
        [&messageId](const ChatMessage &message) {
            return message.id == messageId;
        });
    if (present)
        return true;
    if (m_historyIds.contains(messageId))
        return false;
    QList<QJsonObject> &pending = m_pendingOfficeMutations[messageId];
    if (pending.size() < 32)
        pending.append(mutation);
    m_pendingOfficeMutationOrder.removeAll(messageId);
    m_pendingOfficeMutationOrder.append(messageId);
    while (m_pendingOfficeMutationOrder.size() > 200)
        m_pendingOfficeMutations.remove(
            m_pendingOfficeMutationOrder.takeFirst());
    return false;
}

void MainWindow::replayPendingOfficeMutations(const QString &messageId)
{
    const auto it = m_pendingOfficeMutations.find(messageId);
    if (it == m_pendingOfficeMutations.end())
        return;
    const QList<QJsonObject> pending = it.value();
    m_pendingOfficeMutations.erase(it);
    m_pendingOfficeMutationOrder.removeAll(messageId);
    for (const QJsonObject &mutation : pending) {
        const QString type = mutation.value(QStringLiteral("type")).toString();
        const QString conversation =
            mutation.value(QStringLiteral("conversation")).toString();
        if (type == QLatin1String("reaction"))
            applyOfficeReaction(
                conversation, messageId,
                mutation.value(QStringLiteral("emoji")).toString(),
                mutation.value(QStringLiteral("reactorName")).toString(),
                mutation.value(QStringLiteral("added")).toBool());
        else if (type == QLatin1String("edit"))
            applyOfficeEdit(conversation, messageId,
                            mutation.value(QStringLiteral("senderId")).toString(),
                            mutation.value(QStringLiteral("text")).toString());
        else if (type == QLatin1String("delete"))
            applyOfficeDelete(
                conversation, messageId,
                mutation.value(QStringLiteral("senderId")).toString());
    }
}

void MainWindow::applyOfficeReaction(const QString &conversation,
                                     const QString &messageId,
                                     const QString &emoji,
                                     const QString &reactorName, bool added)
{
    if (emoji.isEmpty() || reactorName.isEmpty() ||
        !officeMutationTargetReady(
            conversation, messageId,
            QJsonObject{{"type", "reaction"},
                        {"conversation", conversation},
                        {"emoji", emoji},
                        {"reactorName", reactorName},
                        {"added", added}}))
        return;
    onReaction(conversation, messageId, emoji, reactorName, added);
}

void MainWindow::applyOfficeEdit(const QString &conversation,
                                 const QString &messageId,
                                 const QString &senderId,
                                 const QString &newText)
{
    if (senderId.isEmpty() || newText.trimmed().isEmpty() ||
        !officeMutationTargetReady(
            conversation, messageId,
            QJsonObject{{"type", "edit"},
                        {"conversation", conversation},
                        {"senderId", senderId},
                        {"text", newText}}))
        return;
    const QList<ChatMessage> &messages = m_history.value(conversation);
    const auto target = std::find_if(
        messages.cbegin(), messages.cend(),
        [&messageId](const ChatMessage &message) {
            return message.id == messageId;
        });
    if (target == messages.cend() || target->deleted ||
        target->senderId != senderId)
        return;
    onMessageEdited(conversation, messageId, newText);
}

void MainWindow::applyOfficeDelete(const QString &conversation,
                                   const QString &messageId,
                                   const QString &senderId)
{
    if (senderId.isEmpty() ||
        !officeMutationTargetReady(
            conversation, messageId,
            QJsonObject{{"type", "delete"},
                        {"conversation", conversation},
                        {"senderId", senderId}}))
        return;
    const QList<ChatMessage> &messages = m_history.value(conversation);
    const auto target = std::find_if(
        messages.cbegin(), messages.cend(),
        [&messageId](const ChatMessage &message) {
            return message.id == messageId;
        });
    if (target == messages.cend() || target->deleted ||
        target->senderId != senderId)
        return;
    onMessageDeleted(conversation, messageId);
}

void MainWindow::onMessageDeleted(const QString &conversation, const QString &messageId)
{
    QList<ChatMessage> &messages = m_history[conversation];
    for (ChatMessage &message : messages) {
        if (message.id == messageId) {
            message.text.clear();
            message.fileName.clear();
            message.fileMime.clear();
            message.fileData.clear();
            message.deleted = true;
            break;
        }
    }
    m_reactions.remove(messageId);
    refreshChatListPreview(conversation);
    if (conversation == m_currentConversation)
        rebuildConversationView();
    if (conversation == m_currentConversation && m_chatThreadPane &&
        m_chatThreadPane->isVisible())
        rebuildChatThreadPane();
    scheduleChatSave();
}

void MainWindow::promptEditMessage(const QString &messageId, const QString &currentText)
{
    if (messageId.isEmpty())
        return;
    bool ok = false;
    const QString text = QInputDialog::getMultiLineText(
        this, "Edit message", "Message:", currentText, &ok);
    const QString trimmed = text.trimmed();
    if (!ok || trimmed.isEmpty() || trimmed == currentText)
        return;
    if (isDiscordConversation(m_currentConversation)) {
        if (m_discordClient)
            m_discordClient->editMessage(m_currentConversation, messageId,
                                         trimmed);
    } else if (isOfficeConversation(m_currentConversation)) {
        if (!m_officeChannelMirror ||
            !m_officeChannelMirror->editMessage(m_currentConversation,
                                                messageId, trimmed))
            logSystem(QStringLiteral("Office chat: the edit could not be sent "
                                     "to %1.")
                          .arg(m_currentConversation));
    } else if (m_backend) {
        m_backend->editMessage(m_currentConversation, messageId, trimmed);
    }
}

void MainWindow::confirmDeleteMessage(const QString &messageId)
{
    if (messageId.isEmpty())
        return;
    const int result = QMessageBox::question(
        this, "Delete message", "Delete this message for everyone?");
    if (result != QMessageBox::Yes)
        return;
    if (isDiscordConversation(m_currentConversation)) {
        if (m_discordClient)
            m_discordClient->deleteMessage(m_currentConversation, messageId);
    } else if (isOfficeConversation(m_currentConversation)) {
        if (!m_officeChannelMirror ||
            !m_officeChannelMirror->deleteMessage(m_currentConversation,
                                                  messageId))
            logSystem(QStringLiteral("Office chat: the delete could not be "
                                     "sent to %1.")
                          .arg(m_currentConversation));
    } else if (m_backend) {
        m_backend->deleteMessage(m_currentConversation, messageId);
    }
}

QString MainWindow::adminDeleteCanonical(const QString &conversation,
                                         const QString &messageId,
                                         const QString &adminPubkey,
                                         qint64 ts) const
{
    return QStringLiteral("forkmesh-admin-delete-v1\n") + conversation + "\n" +
           messageId + "\n" + adminPubkey + "\n" + QString::number(ts);
}

void MainWindow::confirmAdminDeleteMessage(const QString &messageId)
{
    if (!m_backend || messageId.isEmpty() || !m_isAdmin ||
        !hasOwnerSigningCapability(accountOwner()) ||
        !m_profileIdentity.isValid())
        return;
    const int result = QMessageBox::question(
        this, "Delete message",
        "Delete this message for everyone as an administrator?");
    if (result != QMessageBox::Yes)
        return;
    const QString conversation = m_currentConversation;
    const QString pubkey = m_profileIdentity.publicKey();
    const qint64 ts = QDateTime::currentMSecsSinceEpoch();
    const QString sig = m_profileIdentity.signData(
        adminDeleteCanonical(conversation, messageId, pubkey, ts).toUtf8());
    if (sig.isEmpty())
        return;
    // Trust ourselves: apply locally now, and broadcast the signed request so
    // every peer can verify and apply it too.
    m_knownAdminPubkeys.insert(pubkey);
    m_backend->applyAdminDelete(conversation, messageId);
    m_backend->sendAdminDelete(conversation, messageId, ts, sig);
}

void MainWindow::onAdminDeleteRequested(const QString &conversation,
                                        const QString &messageId,
                                        const QString &adminId,
                                        const QString &adminName, qint64 ts,
                                        const QString &sig)
{
    if (messageId.isEmpty() || adminId.isEmpty() || sig.isEmpty())
        return;
    // 1) The signature must be valid for the claimed admin key. This stops any
    // room member from forging an admin delete (chat frames are otherwise
    // unsigned), since they can't produce a signature for the admin's key.
    const QByteArray canonical =
        adminDeleteCanonical(conversation, messageId, adminId, ts).toUtf8();
    if (!ForkMeshIdentity::verifySignature(adminId, sig, canonical)) {
        logSystem(QStringLiteral("Ignored an admin delete with a bad signature."));
        return;
    }
    if (m_knownAdminPubkeys.contains(adminId)) {
        m_backend->applyAdminDelete(conversation, messageId);
        return;
    }
    if (adminName.trimmed().isEmpty() || !m_networkAccess)
        return;
    QNetworkRequest request(accountsApiUrl(adminName.trimmed()));
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    QNetworkReply *reply = m_networkAccess->get(request);
    const QString conv = conversation;
    const QString mid = messageId;
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, adminId, conv, mid]() {
                const QByteArray body = reply->readAll();
                const bool ok = reply->error() == QNetworkReply::NoError;
                reply->deleteLater();
                if (!ok || !m_backend)
                    return;
                const QJsonObject rec = QJsonDocument::fromJson(body).object();
                const bool isAdmin = rec.value("isAdmin").toBool();
                const QString pubkey = rec.value("pubkey").toString();
                if (!isAdmin || pubkey != adminId)
                    return; // not an admin, or the key doesn't match: reject
                m_knownAdminPubkeys.insert(adminId);
                m_backend->applyAdminDelete(conv, mid);
            });
}

void MainWindow::onAvatar(const QString &peerId, const QByteArray &pngData)
{
    QPixmap pixmap;
    if (!pixmap.loadFromData(pngData))
        return;
    m_avatars.insert(peerId, pixmap);
    const QString path = avatarCachePath(peerId);
    if (!path.isEmpty()) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(peerId.toUtf8());
            f.write("\n");
            f.write(pngData);
        }
    }
    refreshVisibleRowAvatars();
    refreshChatMembers();
    refreshIssueAvatars();
    refreshOrganizationTaskPeopleFaces();
    if (m_agentTable)
        refreshAgentTable();
}

void MainWindow::onTypingChanged(const QString &conversation, const QString &peerId,
                                 const QString &peerName, bool active)
{
    if (conversation.isEmpty() || peerId.isEmpty())
        return;
    if (active)
        m_typing[conversation].insert(peerId, peerName);
    else if (m_typing.contains(conversation))
        m_typing[conversation].remove(peerId);
    refreshTypingLabel();
}

void MainWindow::setChannels(const QStringList &channels)
{
    // OfficeChannelMirror refreshes the combined sidebar by passing
    // m_channels back into this method. Snapshot the input before clearing the
    // destination so that self-aliasing cannot erase the mesh rooms.
    const QStringList incomingChannels = channels;
    const QString everyoneGeneral =
        forkmesh::office::everyoneGeneralConversation();
    const bool everyoneGeneralArrived =
        m_officeConversations.contains(everyoneGeneral) &&
        !m_channels.contains(everyoneGeneral);
    const QStringList everyone = forkmesh::office::everyoneConversations();
    m_channels.clear();
    for (const QString &conversation : everyone)
        if (m_officeConversations.contains(conversation))
            m_channels.append(conversation);
    for (const QString &channel : incomingChannels)
        if (!forkmesh::office::isOfficeConversation(channel) &&
            !forkmesh::discord::isDiscordConversation(channel) &&
            !forkmesh::office::isRetiredRelayChannel(channel) &&
            !m_channels.contains(channel))
            m_channels.append(channel);
    for (const QString &conversation : std::as_const(m_officeConversations))
        if (!m_channels.contains(conversation))
            m_channels.append(conversation);
    for (const QString &conversation : std::as_const(m_discordConversations))
        if (!m_channels.contains(conversation))
            m_channels.append(conversation);
    // The local mesh room belongs to this device, not to the relay's channel
    // list, so it is re-added on every rebuild for as long as the mesh is up.
    if (m_localMesh && m_localMesh->isRunning() &&
        !m_channels.contains(forkmesh::localmesh::conversation()))
        m_channels.append(forkmesh::localmesh::conversation());
    if (m_currentConversation.startsWith('#') &&
        !m_channels.contains(m_currentConversation))
        m_currentConversation.clear();
    refreshChannelList();
    updateHomeStats();
    if (m_currentConversation.isEmpty() && !m_channels.isEmpty())
        selectFirstChannel(); // the first rendered row; triggers switchConversation
    else if (everyoneGeneralArrived &&
             m_currentConversation == forkmesh::office::worldConversation())
        switchConversation(everyoneGeneral);
}

void MainWindow::setRoster(const QList<MemberInfo> &members)
{
    QSet<QString> freshIds, freshNames;
    for (const MemberInfo &m : members) {
        if (!m.id.isEmpty()) {
            freshIds.insert(m.id);
            m_removedPeerIds.remove(m.id);
        }
        if (!m.name.isEmpty())
            freshNames.insert(m.name);
    }

    const auto peerKey = [](const MemberInfo &m) {
        return m.id.isEmpty() ? m.name.trimmed().toLower() : m.id;
    };
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    for (const MemberInfo &m : members) {
        const QString key = peerKey(m);
        if (!key.isEmpty() && m.online)
            m_peerLastSeenMs.insert(key, nowMs);
    }

    QList<MemberInfo> newRoster = members;
    for (const MemberInfo &prev : std::as_const(m_homeRoster)) {
        if (prev.self)
            continue;
        if (!prev.id.isEmpty() && m_removedPeerIds.contains(prev.id))
            continue;
        const bool stillPresent =
            (!prev.id.isEmpty() && freshIds.contains(prev.id)) ||
            (!prev.name.isEmpty() && freshNames.contains(prev.name));
        if (!stillPresent) {
            if (ChatVisitorPresence::isTransientVisitor(prev.accountKind,
                                                        prev.name)) {
                const QString key = peerKey(prev);
                const qint64 lastSeen = m_peerLastSeenMs.value(key, 0);
                if (lastSeen <= 0)
                    m_peerLastSeenMs.insert(key, nowMs);
                else if (ChatVisitorPresence::visitorIsIdle(lastSeen, nowMs))
                    continue;
            }
            MemberInfo offline = prev;
            offline.online = false;
            newRoster.append(offline);
        }
    }

    for (MemberInfo &member : newRoster) {
        const QString identity = nodeListIdentityKey(member).toLower();
        if (!identity.isEmpty() && m_chatDirectoryUsers.contains(identity))
            member.accountKind = QStringLiteral("user");
    }

    QHash<QString, qint64> keptSightings;
    keptSightings.reserve(newRoster.size());
    for (const MemberInfo &m : std::as_const(newRoster)) {
        const QString key = peerKey(m);
        const auto it = m_peerLastSeenMs.constFind(key);
        if (it != m_peerLastSeenMs.constEnd())
            keptSightings.insert(key, *it);
    }
    m_peerLastSeenMs = keptSightings;

    const bool firstRoster = m_homeRoster.isEmpty();
    const QString ownId = m_profileIdentity.publicKey();
    const QString ownName = accountOwner();
    QSet<QString> previouslyOnline;
    for (const MemberInfo &m : std::as_const(m_homeRoster)) {
        if (m.self)
            continue;
        if (!ownId.isEmpty() && m.id == ownId)
            continue;
        if (!ownName.isEmpty() && m.name.compare(ownName, Qt::CaseInsensitive) == 0)
            continue;
        if (m.online && !m.id.isEmpty())
            previouslyOnline.insert(m.id);
    }
    const bool firstPeerRoster = previouslyOnline.isEmpty();
    if (!firstPeerRoster &&
        QDateTime::currentMSecsSinceEpoch() >= m_nodeAlertGraceUntilMs) {
        // Never notify about our own node coming online. The roster's "self"
        // flag isn't always set (e.g. on reconnect), so also match our own node
        // id (public key) and account name defensively.
        const bool showNodeConnectAlert =
            QSettings().value(kNodeConnectAlertSetting, false).toBool();
        for (const MemberInfo &m : members) {
            if (m.self || m.id.isEmpty() || !m.online)
                continue;
            if (!ownId.isEmpty() && m.id == ownId)
                continue;
            if (!ownName.isEmpty() && m.name.compare(ownName, Qt::CaseInsensitive) == 0)
                continue;
            // A plain user account (accountKind "user") is a person, not a
            // serving node — e.g. a desktop signed in as a user rather than a
            // linked node, or ForkBot's relayed chat identity. Announcing it as
            // "Node connected" is misleading (hit this same mix-up in
            // the node switcher); missing accountKind (older peers) still
            // counts as a node for backward compatibility.
            if (m.accountKind == QLatin1String("user") ||
                m_chatDirectoryUsers.contains(
                    nodeListIdentityKey(m).trimmed().toLower()))
                continue;
            if (isTemporaryChatGuest(m))
                continue;
            if (!previouslyOnline.contains(m.id)) {
                QString displayName = nodeListIdentityKey(m).trimmed();
                if (displayName.isEmpty())
                    displayName = m.id;
                logSystem(QStringLiteral("Node connected: %1 is online").arg(displayName));
                if (!showNodeConnectAlert)
                    continue;
                postNotification(QStringLiteral("Node connected"),
                                 displayName + QStringLiteral(" is online"));
            }
        }
    }

    m_homeRoster = newRoster;
    forkmesh::agentfeed::publishRelayRoster();
    maybeAnnounceWelcome();
    refreshChatMembers();
    // A peer's relay frame changed its status: the member cards above
    // already repainted from their signature, but message authors and the DM
    // list only follow when told.
    QString statusSignature;
    for (const MemberInfo &m : std::as_const(m_homeRoster)) {
        if (!m.hasUserStatus || !m.online)
            continue;
        statusSignature += m.id + QLatin1Char('\x1f') +
                           (m.userStatus.away ? QLatin1Char('a')
                                              : QLatin1Char('-')) +
                           m.userStatus.emoji + QLatin1Char('\x1f') +
                           m.userStatus.text + QLatin1Char('\x1f') +
                           QString::number(m.userStatus.expiresAtMs) +
                           QLatin1Char('\x1e');
    }
    if (statusSignature != m_rosterUserStatusSignature) {
        m_rosterUserStatusSignature = statusSignature;
        refreshVisibleRowStatuses();
        refreshDmList();
        scheduleUserStatusExpiry();
    }
    for (const MemberInfo &member : members) {
        if (m_dmNames.contains(member.id) && m_dmNames.value(member.id) != member.name) {
            m_dmNames.insert(member.id, member.name);
            refreshDmList();
            if (m_currentConversation == dmKey(member.id))
                m_channelTitle->setText(kDmPrefix + member.name);
        }
    }

    refreshRepositoryList();
    updateHomeStats();
    updateConnectionStatus();
    refreshNodeDotMatrix();
    if (mirrorNodesRepoIndex() >= 0) {
        if (!m_mirrorPanelRosterTimer) {
            m_mirrorPanelRosterTimer = new QTimer(this);
            m_mirrorPanelRosterTimer->setSingleShot(true);
            m_mirrorPanelRosterTimer->setInterval(500);
            connect(m_mirrorPanelRosterTimer, &QTimer::timeout, this, [this] {
                if (mirrorNodesRepoIndex() >= 0)
                    loadMirrorNodesPanel();
            });
        }
        if (!m_mirrorPanelRosterTimer->isActive())
            m_mirrorPanelRosterTimer->start();
    }
    if (!firstRoster)
        syncMirrorsBehindRoster();
}

QString MainWindow::welcomeChannelForIdentity() const
{
    if (m_officeConversations.contains(kWelcomeChannel))
        return kWelcomeChannel;
    const QString general = forkmesh::office::everyoneGeneralConversation();
    return m_officeConversations.contains(general)
               ? general
               : forkmesh::office::worldConversation();
}

void MainWindow::maybeAnnounceWelcome()
{
    if (m_welcomeAnnounced || !m_officeChannelMirror ||
        !m_officeConversations.contains(kWelcomeChannel) ||
        !m_officeChannelMirror->canSend(kWelcomeChannel))
        return;
    const QString id = m_profileIdentity.publicKey();
    if (id.isEmpty())
        return;
    if (!m_profileIsUserAccount ||
        !accountEmailVerified(settingsAccountName()))
        return;
    if (m_profileIdentity.hasAnnouncedWelcome()) {
        m_welcomeAnnounced = true; // greeted in an earlier run; don't repeat
        return;
    }
    if (QSettings().value(kLegacyWelcomeAnnouncedSettingPrefix + id, false)
            .toBool()) {
        m_profileIdentity.markWelcomeAnnounced();
        m_welcomeAnnounced = true;
        return;
    }
    if (!m_officeChannelMirror->sendMessage(
            kWelcomeChannel,
            QString::fromUtf8(
                "\xF0\x9F\x91\x8B Just joined ForkMesh \xE2\x80\x94 hello!")))
        return;
    m_profileIdentity.markWelcomeAnnounced();
    m_welcomeAnnounced = true;
}

static QString chatUserDisplayName(const MemberInfo &member)
{
    const QString owner = member.ownerUser.trimmed();
    if (!owner.isEmpty())
        return owner;
    const QString name = member.name.trimmed();
    if (!name.isEmpty())
        return name;
    return member.nodeName.trimmed();
}

static QString chatUserKey(const MemberInfo &member)
{
    if (member.self)
        return QStringLiteral("\x01self");
    return chatUserDisplayName(member).toLower();
}

namespace {
struct ChatNodeBadge {
    QString label;    // node name, shown in the tooltip
    QString platform; // linux/macos/windows/… -> osBadgeIcon glyph
    bool online = false;
};

struct ChatUserGroup {
    MemberInfo primary;         // canonical row (prefers the directory user)
    QString key;                // grouping key (owning account, when known)
    QString anchorId;           // a real node id, for disambiguating look-alikes
    QList<ChatNodeBadge> nodes; // this user's nodes, live and directory-listed
    QSet<QString> nodeKeys;     // lower(nodeName|id) already added, to dedupe
    bool online = false;
};

QString directoryUserKey(const MemberInfo &u)
{
    if (u.self)
        return QStringLiteral("\x01self");
    const QString owner = u.ownerUser.trimmed();
    return (owner.isEmpty() ? u.name.trimmed() : owner).toLower();
}

QString directoryAccountKey(const MemberInfo &u)
{
    const QString owner = u.ownerUser.trimmed();
    return (owner.isEmpty() ? u.name.trimmed() : owner).toLower();
}
} // namespace

void MainWindow::refreshChatUserDirectory()
{
    if (!m_networkAccess || m_chatDirectoryFetchInFlight)
        return;
    // The registered-user directory only moves when somebody signs up. Nothing
    // reads it on a timer: every surface that shows it (Chat, Settings, the
    // issue assignee picker, Users) calls this when it opens, behind the
    // five-minute floor below, and Users has a Refresh button.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_chatDirectoryFetchedMs > 0 &&
        now - m_chatDirectoryFetchedMs < 5 * 60 * 1000)
        return;

    m_chatDirectoryFetchInFlight = true;
    QUrl directoryUrl = accountsApiUrl(QStringLiteral("users"));
    QNetworkRequest request(directoryUrl);
    request.setRawHeader("Accept", "application/json");
    // Chat/World need only the privacy-filtered public roster. The admin-only
    // Users page needs the protected investigation fields, so an is_admin
    // desktop proves the read with its bearer session or the same short-lived
    // account-key signature used by other silent-auth desktop APIs.
    //
    // Ask for that projection only while the Users page is actually showing.
    // It costs the relay four whole-table referral/email scans per request
    // (the third-heaviest D1 read of 2026-09-11), and from every other page
    // this 10-minute backstop only feeds Chat/World, which the edge-cached
    // public roster already serves. refreshUsersPage() forces a refetch when
    // the cached payload lacks the admin columns, so opening the page still
    // shows them.
    const bool wantAdminPayload =
        m_isAdmin && m_sectionStack &&
        m_sectionStack->currentIndex() == kUsersSectionIndex;
    if (wantAdminPayload)
        authorizeAdminUsersDirectory(request);
    QNetworkReply *reply = m_networkAccess->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        m_chatDirectoryFetchInFlight = false;
        m_chatDirectoryFetchedMs = QDateTime::currentMSecsSinceEpoch();
        const QByteArray body = reply->readAll();
        const bool ok = reply->error() == QNetworkReply::NoError;
        reply->deleteLater();
        if (!ok) {
            if (m_usersStatus)
                m_usersStatus->setText(
                    QStringLiteral("Could not refresh the user directory"));
            return;
        }
        const QJsonObject obj = QJsonDocument::fromJson(body).object();
        if (!obj.value(QStringLiteral("ok")).toBool()) {
            if (m_usersStatus)
                m_usersStatus->setText(
                    QStringLiteral("The user directory returned an invalid response"));
            return;
        }
        const bool hasAdminSignupCode =
            m_isAdmin && obj.contains(QStringLiteral("signupReferralCode"));
        renderUsersSignupReferralCode(
            hasAdminSignupCode
                ? obj.value(QStringLiteral("signupReferralCode")).toObject()
                : QJsonObject());
        m_usersQuarantinePayload =
            m_isAdmin
                ? obj.value(QStringLiteral("quarantinedUsers")).toArray()
                : QJsonArray();
        renderUsersQuarantinePage(m_usersQuarantinePayload);
        const QJsonArray users =
            obj.value(QStringLiteral("users")).toArray();
        // Detect the response shape independently of the desktop's current
        // role. An administrator can sign out while this request is in flight;
        // even then, a response carrying the complete projection must feed
        // only `publicUsers` into shared Chat/World state.
        const bool hasCompleteAdminDirectory =
            obj.contains(QStringLiteral("publicUsers"));
        // An admin response contains every users-table record for the guarded
        // Users page. Feed only its separately privacy-filtered projection to
        // shared Chat/World member presentation.
        mergeChatUserDirectory(
            hasCompleteAdminDirectory
                ? obj.value(QStringLiteral("publicUsers")).toArray()
                : users);
        if (hasCompleteAdminDirectory && m_isAdmin) {
            m_usersDirectoryPayload = QJsonArray();
            for (const QJsonValue &value : users) {
                QJsonObject stats = value.toObject();
                stats.remove(QStringLiteral("avatarPng"));
                m_usersDirectoryPayload.append(stats);
            }
            m_usersDirectoryAdminPayload = true;
            renderUsersPage(m_usersDirectoryPayload);
        } else if (hasAdminSignupCode) {
            // Compatibility with a relay from before the complete admin
            // directory split, where protected fields rode in `users`.
            m_usersDirectoryAdminPayload = true;
        }
    });
}

void MainWindow::authorizeAdminUsersDirectory(QNetworkRequest &request) const
{
    QUrl url = request.url();
    const QString token = accountSessionTokenForUrl(url);
    if (!token.isEmpty()) {
        request.setRawHeader(
            "Authorization", QByteArrayLiteral("Bearer ") + token.toUtf8());
        return;
    }
    QString node, ts, sig;
    if (!signOrgTaskProof(kAccountUsersAdminProof, QString(), &node, &ts, &sig))
        return;
    QUrlQuery query(url);
    query.addQueryItem(QStringLiteral("node"), node);
    query.addQueryItem(QStringLiteral("ts"), ts);
    query.addQueryItem(QStringLiteral("sig"), sig);
    url.setQuery(query);
    request.setUrl(url);
}

void MainWindow::fetchNativePackageUserRecords(
    forkmesh::market::NativePackageUserRecordsHandler done)
{
    if (!done)
        return;
    if (!m_networkAccess || !m_isAdmin) {
        done({}, QStringLiteral("only ForkMesh administrators can read the user "
                                "directory's email addresses"));
        return;
    }
    QNetworkRequest request(accountsApiUrl(QStringLiteral("users")));
    request.setRawHeader("Accept", "application/json");
    authorizeAdminUsersDirectory(request);
    QNetworkReply *reply = m_networkAccess->get(request);
    connect(reply, &QNetworkReply::finished, this, [reply, done] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
            return done({}, reply->errorString());
        const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
        if (!obj.value(QStringLiteral("ok")).toBool())
            return done({}, QStringLiteral("the user directory returned an "
                                           "invalid response"));
        if (!obj.contains(QStringLiteral("publicUsers")))
            return done({}, QStringLiteral("the relay did not accept this "
                                           "desktop as an administrator"));
        done(obj.value(QStringLiteral("users")).toArray(), QString());
    });
}

void MainWindow::mergeChatUserDirectory(const QJsonArray &users)
{
    const bool firstDirectoryLoad = !m_chatDirectoryLoaded;
    const QHash<QString, MemberInfo> previousDirectory = m_chatDirectoryUsers;
    QHash<QString, QByteArray> nextAvatarDigests;
    bool avatarChanged = false;
    m_usersDirectoryPayload = QJsonArray();
    m_usersPublicDirectoryPayload = QJsonArray();
    m_usersDirectoryAdminPayload = false;
    const QStringList protectedFields{
        QStringLiteral("email"), QStringLiteral("referralStatus"),
        QStringLiteral("referralRiskFlags"),
        QStringLiteral("referralRiskScore"),
        QStringLiteral("referralClickId"),
        QStringLiteral("referralCreatedAt"),
        QStringLiteral("referralVerifiedAt"),
        QStringLiteral("referralCreditedAt"),
        QStringLiteral("referralClicks"),
        QStringLiteral("referralAuditedUniqueClicks"),
        QStringLiteral("referralDuplicateClickAttempts"),
        QStringLiteral("referralRejectedClickAttempts"),
        QStringLiteral("referralLegacyClickCount"),
        QStringLiteral("creditedReferrals"),
        QStringLiteral("referredUsers"),
        QStringLiteral("legacyUnattributed"),
        QStringLiteral("referralLastAt"), QStringLiteral("signupIp"),
        QStringLiteral("signupIpFingerprint"),
        QStringLiteral("signupUserAgent"),
        QStringLiteral("signupUserAgentFingerprint"),
        QStringLiteral("signupClientFingerprint"),
        QStringLiteral("referralNetworkFingerprint"),
        QStringLiteral("signupCountry"),
        QStringLiteral("signupClientCategory"),
        QStringLiteral("emailOpens"), QStringLiteral("emailClicks"),
        QStringLiteral("lastEmailOpenedAt"),
        QStringLiteral("lastEmailClickedAt"),
        QStringLiteral("discordUsername"),
        QStringLiteral("discordDisplayName"),
        QStringLiteral("discordId"),
        QStringLiteral("discordLinked"),
        QStringLiteral("profilePrivate"), QStringLiteral("isAdmin"),
        QStringLiteral("outreachEnabled"),
        QStringLiteral("emailNotifications"),
        QStringLiteral("userFingerprint"),
        QStringLiteral("emailFingerprint"),
        QStringLiteral("discordFingerprint")};
    for (const QJsonValue &value : users) {
        QJsonObject stats = value.toObject();
        if (stats.contains(QStringLiteral("email")))
            m_usersDirectoryAdminPayload = true;
        stats.remove(QStringLiteral("avatarPng"));
        m_usersDirectoryPayload.append(stats);
        QJsonObject publicStats = stats;
        for (const QString &field : protectedFields)
            publicStats.remove(field);
        m_usersPublicDirectoryPayload.append(publicStats);
    }
    QHash<QString, MemberInfo> next;
    const QString ownName = accountOwner().trimmed();
    for (const QJsonValue &value : users) {
        const QJsonObject obj = value.toObject();
        if (obj.value(QStringLiteral("kind")).toString(QStringLiteral("user")) !=
            QStringLiteral("user"))
            continue;
        const QString name =
            obj.value(QStringLiteral("name"))
                .toString(obj.value(QStringLiteral("nodeName")).toString())
                .trimmed();
        if (name.isEmpty())
            continue;
        const QString key = name.toLower();
        MemberInfo member;
        member.id = QStringLiteral("user:") + key;
        member.name = name;
        member.ownerUser = name;
        member.accountKind = QStringLiteral("user");
        member.self = !ownName.isEmpty() &&
                      ownName.compare(name, Qt::CaseInsensitive) == 0;
        member.online = false;
        member.createdAtMs =
            qint64(obj.value(QStringLiteral("createdAt")).toDouble());
        const QJsonArray nodes = obj.value(QStringLiteral("nodes")).toArray();
        QStringList nodeNames;
        for (const QJsonValue &nodeValue : nodes) {
            const QString node = nodeValue.toString().trimmed();
            if (!node.isEmpty())
                nodeNames.append(node);
        }
        member.nodeName = nodeNames.join(QStringLiteral(", "));
        if (!member.nodeName.isEmpty())
            member.note = QStringLiteral("Nodes: ") + member.nodeName;
        member.hasUserStatus =
            forkmesh::userstatus::readFrom(obj, &member.userStatus);
        // Our own entry: the first copy of our status when the session read
        // has not answered (or cannot, on a key-only sign-in), and later the
        // way a change made on the web or a phone reaches this desktop -
        // unless this desktop wrote one a moment ago that an edge-cached
        // directory may not have caught up with. Guarded on being signed in,
        // like adoptWebAccountAvatar: a local profile that merely shares the
        // name must not advertise that account's status.
        if (member.self && member.hasUserStatus && m_accountAuthenticated &&
            (!m_ownUserStatusKnown ||
             (member.userStatus != m_ownUserStatus &&
              QDateTime::currentMSecsSinceEpoch() - m_ownUserStatusWrittenMs >
                  2 * 60 * 1000)))
            QTimer::singleShot(0, this, [this, status = member.userStatus] {
                applyOwnUserStatus(status);
            });

        const QString avatarPng = obj.value(QStringLiteral("avatarPng")).toString();
        if (!avatarPng.isEmpty()) {
            const QByteArray encoded = avatarPng.toLatin1();
            const QByteArray digest = QCryptographicHash::hash(
                encoded, QCryptographicHash::Sha256);
            nextAvatarDigests.insert(member.id, digest);
            const bool changed =
                m_chatDirectoryAvatarDigests.value(member.id) != digest;
            if (changed) {
                const QByteArray png = QByteArray::fromBase64(encoded);
                QPixmap avatar;
                if (avatar.loadFromData(png)) {
                    avatarChanged = true;
                    m_avatars.insert(member.id, avatar);
                }
                if (member.self)
                    adoptWebAccountAvatar(png);
            }
        }
        next.insert(key, member);
    }
    m_chatDirectoryAvatarDigests = std::move(nextAvatarDigests);
    if (m_chatDirectoryLoaded) {
        for (auto it = next.constBegin(); it != next.constEnd(); ++it) {
            if (!m_chatDirectoryUsers.contains(it.key()) && !it.value().self)
                logSystem(QStringLiteral("New user joined ForkMesh: %1")
                              .arg(it.value().name));
        }
    }
    m_chatDirectoryLoaded = true;
    m_chatDirectoryUsers = next;
    const bool conversationPresentationChanged =
        firstDirectoryLoad || avatarChanged ||
        !sameChatDirectoryPresentation(previousDirectory,
                                       m_chatDirectoryUsers);
    if (conversationPresentationChanged)
        emit chatUserDirectoryChanged();
    if (avatarChanged) {
        refreshVisibleRowAvatars();
        // The issue list and the open issue were painted before this arrived,
        // so they are still wearing the seeded stand-in portraits.
        refreshIssueAvatars();
        refreshOrganizationTaskPeopleFaces();
    }
    renderUsersPage(m_usersDirectoryPayload);
    for (MemberInfo &member : m_homeRoster) {
        const QString identity = nodeListIdentityKey(member).trimmed().toLower();
        if (!identity.isEmpty() && m_chatDirectoryUsers.contains(identity))
            member.accountKind = QStringLiteral("user");
    }
    refreshRepositoryList();
    if (m_networkReposTable && !m_networkReposLastPayload.isEmpty())
        renderNetworkRepos(m_networkReposLastPayload);
    // A periodic directory response is normally byte-for-byte the same user
    // presentation. Rebuilding both the member cards and every transcript row
    // anyway accounted for the chat/roster cluster in the stall log.
    // refreshChatMembers() already refreshes mention candidates.
    if (conversationPresentationChanged) {
        refreshChatMembers();
        refreshDmList();
        scheduleUserStatusExpiry();
    }
    if (conversationPresentationChanged && m_messageLayout &&
        !m_currentConversation.isEmpty()) {
        const bool wasAtBottom = m_stickToBottom;
        renderConversationRows();
        if (wasAtBottom)
            scrollToBottom();
    }
}

void MainWindow::refreshChatMembers()
{
    refreshMentionCandidates();
    if (!m_chatMembersLayout)
        return;

    // Tearing the cards down and rebuilding them is the expensive part (nine
    // widgets and a Markdown-free but rich-text label per card, and a
    // deleteLater storm); it only happens once the new roster is known to
    // differ from what is on screen — see the signature check below.
    const auto tearDownCards = [this] {
        while (m_chatMembersLayout->count() > 1) {
            QLayoutItem *item = m_chatMembersLayout->takeAt(0);
            if (QWidget *w = item->widget()) {
                w->hide();
                w->deleteLater();
            }
            delete item;
        }
    };

    if (isDiscordConversation(m_currentConversation)) {
        const QStringList names =
            m_discordClient
                ? m_discordClient->members(m_currentConversation)
                : QStringList();
        if (m_chatMembersHeading) {
            m_chatMembersHeading->setText(
                QStringLiteral("DISCORD PARTICIPANTS SEEN HERE — %1")
                    .arg(names.size()));
        }
        if (m_chatMembersButton) {
            m_chatMembersButton->setText(QString::number(names.size()));
            m_chatMembersButton->setToolTip(
                names.isEmpty()
                    ? QStringLiteral("No Discord participants have been seen yet")
                    : QStringLiteral("Show %1 Discord participant%2 seen here")
                          .arg(names.size())
                          .arg(names.size() == 1 ? QString()
                                                 : QStringLiteral("s")));
        }
        if (!m_chatMembersMenuOpen)
            return;
        tearDownCards();
        m_chatMembersSignature.clear();
        for (const QString &name : names) {
            auto *card = new QWidget;
            auto *row = new QHBoxLayout(card);
            row->setContentsMargins(4, 4, 4, 4);
            row->setSpacing(10);
            auto *avatar = new QLabel;
            avatar->setFixedSize(28, 28);
            avatar->setPixmap(MessageRow::initialsAvatar(
                name, senderColor(name), 28));
            row->addWidget(avatar);
            auto *label = new QLabel(name);
            label->setTextInteractionFlags(Qt::TextSelectableByMouse);
            row->addWidget(label, 1);
            m_chatMembersLayout->insertWidget(
                m_chatMembersLayout->count() - 1, card);
        }
        return;
    }

    QHash<QString, QString> nodeOwner;      // lower(nodeName) -> user key
    QHash<QString, QStringList> ownedNodes; // user key -> owned node names
    for (const MemberInfo &u : std::as_const(m_chatDirectoryUsers)) {
        const QString key = directoryUserKey(u);
        const QStringList names =
            u.nodeName.split(QStringLiteral(", "), Qt::SkipEmptyParts);
        ownedNodes.insert(key, names);
        for (const QString &n : names) {
            const QString nl = n.trimmed().toLower();
            if (!nl.isEmpty())
                nodeOwner.insert(nl, key);
        }
    }

    auto groupKeyFor = [&](const MemberInfo &m) -> QString {
        const QString owner = m.ownerUser.trimmed().toLower();
        if (!owner.isEmpty())
            return owner;
        const QString nodeL = m.nodeName.trimmed().toLower();
        if (!nodeL.isEmpty() && nodeOwner.contains(nodeL))
            return nodeOwner.value(nodeL);
        const QString nameL = m.name.trimmed().toLower();
        if (!nameL.isEmpty() && nodeOwner.contains(nameL))
            return nodeOwner.value(nameL);
        if (m.self) {
            const QString selfOwner = accountOwner().trimmed().toLower();
            if (!selfOwner.isEmpty())
                return selfOwner;
        }
        return chatUserKey(m);
    };

    QList<ChatUserGroup> groups;
    QHash<QString, int> groupIndex; // key -> index in `groups`
    auto addMember = [&](MemberInfo member) {
        const bool online = member.self ? (m_backend != nullptr) : member.online;
        member.online = online;
        const QString display = chatUserDisplayName(member);
        if (display.isEmpty())
            return;
        member.name = display;
        const QString key = groupKeyFor(member);
        const bool isDirectory = member.id.startsWith(QStringLiteral("user:"));
        const bool isUserAccount =
            isDirectory ||
            member.accountKind.compare(QLatin1String("user"),
                                       Qt::CaseInsensitive) == 0 ||
            m_chatDirectoryUsers.contains(
                nodeListIdentityKey(member).trimmed().toLower());

        int idx;
        const auto it = groupIndex.constFind(key);
        if (it == groupIndex.constEnd()) {
            ChatUserGroup g;
            g.primary = member;
            g.key = key;
            g.online = online;
            idx = groups.size();
            groups.append(g);
            groupIndex.insert(key, idx);
        } else {
            idx = *it;
            ChatUserGroup &g = groups[idx];
            g.online = g.online || online;
            const bool curDirectory =
                g.primary.id.startsWith(QStringLiteral("user:"));
            if (isDirectory && !curDirectory) {
                const bool wasOnline = g.primary.online;
                member.online = wasOnline || online;
                member.self = member.self || g.primary.self;
                g.primary = member;
            }
            const bool curAvatar = !m_avatars.value(g.primary.id).isNull();
            const bool candAvatar = !m_avatars.value(member.id).isNull();
            if (!curAvatar && candAvatar)
                m_avatars.insert(g.primary.id, m_avatars.value(member.id));
        }

        if (!isUserAccount) {
            ChatUserGroup &g = groups[idx];
            if (g.anchorId.isEmpty() && !member.id.isEmpty())
                g.anchorId = member.id;
            ChatNodeBadge badge;
            badge.label = !member.nodeName.trimmed().isEmpty()
                              ? member.nodeName.trimmed()
                              : (!member.name.trimmed().isEmpty()
                                     ? member.name.trimmed()
                                     : member.id);
            badge.platform = member.platform;
            badge.online = online;
            const QString nk = (member.nodeName.trimmed().isEmpty()
                                    ? member.id
                                    : member.nodeName.trimmed())
                                   .toLower();
            if (!nk.isEmpty() && !g.nodeKeys.contains(nk)) {
                g.nodeKeys.insert(nk);
                g.nodes.append(badge);
            }
        }
    };
    QHash<QString, QList<MemberInfo>> liveByUser;
    for (const MemberInfo &member : std::as_const(m_homeRoster)) {
        if (!member.self && !member.online)
            continue;
        const QString key = groupKeyFor(member);
        if (!m_chatDirectoryUsers.contains(key))
            continue;
        liveByUser[key].append(member);
    }

    QSet<QString> roomUserKeys;
    const bool meshRoom = isLocalMeshConversation(m_currentConversation);
    const bool teamRoom = m_currentConversation == QLatin1String("#team");
    const bool privateOfficeRoom =
        m_officeChannelMirror &&
        m_officeChannelMirror->isPrivateConversation(m_currentConversation);
    const bool everyoneRoom =
        m_officeChannelMirror &&
        m_officeChannelMirror->isEveryoneConversation(m_currentConversation);
    const QStringList everyoneMembers =
        everyoneRoom
            ? m_officeChannelMirror->membersForConversation(m_currentConversation)
            : QStringList();
    const int everyoneCount =
        everyoneRoom ? m_officeChannelMirror->memberCountForConversation(
                           m_currentConversation)
                     : 0;
    if (meshRoom) {
        // Nobody here comes from the account directory: this room's membership
        // is whoever this device can see on the network right now.
        if (m_localMesh)
            for (const MemberInfo &member : m_localMesh->members())
                addMember(member);
    } else if (teamRoom) {
        const QString selfKey = accountOwner().trimmed().toLower();
        if (m_chatDirectoryUsers.contains(selfKey))
            roomUserKeys.insert(selfKey);
        // Private mesh rooms do not publish a directory. Show only people who
        // have actually appeared in this local room history (plus the current
        // account), never the public account database.
        QHash<QString, qsizetype> rosterById;
        QHash<QString, qsizetype> rosterByName;
        for (qsizetype i = m_homeRoster.size() - 1; i >= 0; --i) {
            const MemberInfo &member = m_homeRoster.at(i);
            rosterById.insert(member.id, i);
            rosterByName.insert(chatUserDisplayName(member).toCaseFolded(), i);
        }
        QSet<QString> seenSenders;
        for (const ChatMessage &message :
             m_history.value(m_currentConversation)) {
            if (message.senderId.startsWith(QStringLiteral("teammate:")))
                continue;
            const QString sender =
                message.senderId + QLatin1Char('\x1f') + message.senderName;
            if (seenSenders.contains(sender))
                continue;
            seenSenders.insert(sender);
            const QString nameKey = message.senderName.trimmed().toLower();
            if (m_chatDirectoryUsers.contains(nameKey))
                roomUserKeys.insert(nameKey);
            qsizetype match = -1;
            if (!message.senderId.isEmpty())
                match = rosterById.value(message.senderId, -1);
            const qsizetype byName =
                rosterByName.value(message.senderName.toCaseFolded(), -1);
            if (byName >= 0 && (match < 0 || byName < match))
                match = byName;
            if (match >= 0) {
                const QString key = groupKeyFor(m_homeRoster.at(match));
                if (m_chatDirectoryUsers.contains(key))
                    roomUserKeys.insert(key);
            }
        }
    } else if (privateOfficeRoom) {
        for (const QString &username :
             m_officeChannelMirror->membersForConversation(
                 m_currentConversation)) {
            const QString key = username.trimmed().toLower();
            if (!key.isEmpty() && m_chatDirectoryUsers.contains(key))
                roomUserKeys.insert(key);
        }
    } else if (everyoneRoom && !everyoneMembers.isEmpty()) {
        for (const QString &username : everyoneMembers) {
            const QString key = username.trimmed().toLower();
            if (!key.isEmpty() && m_chatDirectoryUsers.contains(key))
                roomUserKeys.insert(key);
        }
        for (auto it = liveByUser.constBegin(); it != liveByUser.constEnd(); ++it)
            roomUserKeys.insert(it.key());
    } else if (isDirectConversation(m_currentConversation)) {
        const QString selfKey = accountOwner().trimmed().toLower();
        if (m_chatDirectoryUsers.contains(selfKey))
            roomUserKeys.insert(selfKey);
        const QString peerId = dmPeerId(m_currentConversation);
        const QString peerName =
            m_dmNames.value(peerId).trimmed().toLower();
        if (m_chatDirectoryUsers.contains(peerName))
            roomUserKeys.insert(peerName);
        for (const MemberInfo &member : std::as_const(m_homeRoster)) {
            if (member.id != peerId)
                continue;
            const QString key = groupKeyFor(member);
            if (m_chatDirectoryUsers.contains(key))
                roomUserKeys.insert(key);
            break;
        }
    } else if (m_privateChannels.contains(m_currentConversation)) {
        const QString selfKey = accountOwner().trimmed().toLower();
        if (m_chatDirectoryUsers.contains(selfKey))
            roomUserKeys.insert(selfKey);
        for (const ChatMessage &message :
             m_history.value(m_currentConversation)) {
            const QString nameKey = message.senderName.trimmed().toLower();
            if (m_chatDirectoryUsers.contains(nameKey))
                roomUserKeys.insert(nameKey);
        }
    } else {
        for (auto it = m_chatDirectoryUsers.constBegin();
             it != m_chatDirectoryUsers.constEnd(); ++it)
            roomUserKeys.insert(it.key());
        for (auto it = liveByUser.constBegin(); it != liveByUser.constEnd(); ++it)
            roomUserKeys.insert(it.key());
    }

    for (const QString &key : std::as_const(roomUserKeys)) {
        const auto directory = m_chatDirectoryUsers.constFind(key);
        if (directory == m_chatDirectoryUsers.constEnd())
            continue;
        addMember(*directory);
        for (const MemberInfo &member : liveByUser.value(key))
            addMember(member);
    }

    if (m_chatDirectoryLoaded && !meshRoom) {
        QSet<QString> accountKeys;
        for (const MemberInfo &u : std::as_const(m_chatDirectoryUsers)) {
            const QString key = directoryAccountKey(u);
            if (!key.isEmpty())
                accountKeys.insert(key);
        }
        const QString selfKey = accountOwner().trimmed().toLower();
        if (!selfKey.isEmpty())
            accountKeys.insert(selfKey);
        groups.erase(std::remove_if(groups.begin(), groups.end(),
                                    [&](const ChatUserGroup &g) {
                                        return !g.primary.self &&
                                               !accountKeys.contains(g.key);
                                    }),
                     groups.end());
        groupIndex.clear(); // indices no longer line up; not used past here
    }

    if (teamRoom) {
        const forkmesh::teammates::LoadResult loaded =
            forkmesh::teammates::Store(teammatesDirPath()).load();
        for (const forkmesh::teammates::Config &teammate : loaded.teammates) {
            const QString handle =
                forkmesh::teammates::mentionHandle(teammate);
            if (teammate.id.trimmed().isEmpty() || handle.isEmpty())
                continue;
            MemberInfo member;
            member.id = QStringLiteral("teammate:%1").arg(teammate.id);
            member.name = handle;
            member.accountKind = QStringLiteral("user");
            QStringList role;
            if (!teammate.specialty.trimmed().isEmpty())
                role << teammate.specialty.trimmed();
            if (!teammate.focus.trimmed().isEmpty())
                role << teammate.focus.trimmed();
            member.note = role.join(QStringLiteral(" · "));
            if (!teammate.enabled)
                member.note = QStringLiteral("Paused") +
                              (member.note.isEmpty()
                                   ? QString()
                                   : QStringLiteral(" · ") + member.note);
            member.online = teammate.enabled;
            const QPixmap portrait = teammateAvatar(teammate, 28);
            if (!portrait.isNull())
                m_avatars.insert(member.id, portrait);
            addMember(member);
        }
    }

    for (ChatUserGroup &g : groups) {
        for (const QString &n : ownedNodes.value(g.key)) {
            const QString nl = n.trimmed().toLower();
            if (nl.isEmpty() || g.nodeKeys.contains(nl))
                continue;
            g.nodeKeys.insert(nl);
            ChatNodeBadge badge;
            badge.label = n.trimmed();
            badge.online = false;
            g.nodes.append(badge);
        }
    }

    std::sort(groups.begin(), groups.end(),
              [](const ChatUserGroup &a, const ChatUserGroup &b) {
                  if (a.online != b.online)
                      return a.online;
                  if (a.primary.self != b.primary.self)
                      return a.primary.self;
                  return a.primary.name.compare(b.primary.name,
                                                Qt::CaseInsensitive) < 0;
              });

    if (m_chatMembersHeading) {
        m_chatMembersHeading->setText(
            teamRoom
                ? QString::fromUtf8("TEAM MEMBERS \xE2\x80\x94 %1")
                      .arg(groups.size())
            : privateOfficeRoom
                ? QString::fromUtf8(
                      "ONLY THESE PEOPLE CAN SEE THIS ROOM \xE2\x80\x94 %1")
                      .arg(groups.size())
            : everyoneRoom
                ? QString::fromUtf8(
                      "EVERY FORKMESH MEMBER IS HERE \xE2\x80\x94 %1")
                      .arg(qMax(everyoneCount, int(groups.size())))
                : QString::fromUtf8(
                      "DATABASE USERS IN THIS ROOM \xE2\x80\x94 %1")
                      .arg(groups.size()));
    }
    if (m_chatMembersButton) {
        const int shownCount =
            everyoneRoom ? qMax(everyoneCount, int(groups.size()))
                         : int(groups.size());
        m_chatMembersButton->setText(QString::number(shownCount));
        m_chatMembersButton->setToolTip(
            everyoneRoom
                ? QStringLiteral("Every ForkMesh member is in this channel: "
                                 "show the %1 members")
                      .arg(shownCount)
            : teamRoom
                ? QStringLiteral("Show %1 special agent%2 and invited participant%3")
                      .arg(groups.size())
                      .arg(groups.size() == 1 ? QString()
                                              : QStringLiteral("s"))
                      .arg(groups.size() == 1 ? QString()
                                              : QStringLiteral("s"))
            : groups.size() == 1
                ? QStringLiteral("Show the 1 database user in this room")
                : QStringLiteral("Show the %1 database users in this room")
                      .arg(groups.size()));
    }
    if (!m_chatMembersMenuOpen)
        return;

    QHash<QString, int> nameCounts;
    for (const ChatUserGroup &g : std::as_const(groups))
        ++nameCounts[g.primary.name.toLower()];

    // Everything a card shows or captures, in render order. Presence frames
    // arrive many times a minute and most of them change nothing here.
    QString signature = m_currentConversation + QLatin1Char('\x1f') +
                        QString::number(m_backend != nullptr) +
                        QLatin1Char('\x1e');
    for (const ChatUserGroup &group : std::as_const(groups)) {
        const MemberInfo &member = group.primary;
        const QPixmap source = m_avatars.value(member.id);
        signature += group.key + QLatin1Char('\x1f') + group.anchorId +
                     QLatin1Char('\x1f') + member.id + QLatin1Char('\x1f') +
                     member.name + QLatin1Char('\x1f') + member.note +
                     QLatin1Char('\x1f') + member.ownerUser + QLatin1Char('\x1f') +
                     member.nodeName + QLatin1Char('\x1f') +
                     (member.self ? QLatin1Char('s') : QLatin1Char('-')) +
                     (group.online ? QLatin1Char('o') : QLatin1Char('-')) +
                     QLatin1Char('\x1f') +
                     QString::number(source.isNull() ? 0 : source.cacheKey()) +
                     QLatin1Char('\x1f') +
                     QString::number(nameCounts.value(member.name.toLower())) +
                     // The click handler captures the whole member for the
                     // profile dialog, so its other fields count too.
                     QLatin1Char('\x1f') + member.solanaAddress +
                     QLatin1Char('\x1f') + member.version + QLatin1Char('\x1f') +
                     member.platform + QLatin1Char('\x1f') + member.accountKind +
                     QLatin1Char('\x1f') + QString::number(member.createdAtMs) +
                     QLatin1Char('\x1f') + member.mirrors.join(QLatin1Char(','));
        if (!member.id.startsWith(QStringLiteral("teammate:"))) {
            const forkmesh::userstatus::UserStatus status =
                userStatusFor(group.key, group.anchorId, member.self);
            signature += QLatin1Char('\x1f');
            signature += (status.away ? QLatin1Char('a') : QLatin1Char('-')) +
                         status.emoji + QLatin1Char('\x1f') + status.text +
                         QLatin1Char('\x1f') +
                         QString::number(status.expiresAtMs);
        }
        for (const ChatNodeBadge &nb : std::as_const(group.nodes))
            signature += QLatin1Char('\x1d') + nb.label + QLatin1Char('\x1f') +
                         nb.platform +
                         (nb.online ? QLatin1Char('o') : QLatin1Char('-'));
        signature += QLatin1Char('\x1e');
    }
    if (signature == m_chatMembersSignature && m_chatMembersLayout->count() > 1)
        return; // the cards on screen already say exactly this
    m_chatMembersSignature = signature;
    tearDownCards();

    // The small grey role, id and "+N" captions share one rule on the list
    // host instead of a sheet parsed per card on every roster rebuild.
    if (QWidget *membersHost = m_chatMembersLayout->parentWidget()) {
        const QString captionSheet = QStringLiteral(
            "QLabel#chatMemberRole, QLabel#chatMemberId, QLabel#chatMemberMore "
            "{ color:#8b949e; font-size:10px; }");
        if (membersHost->styleSheet() != captionSheet)
            membersHost->setStyleSheet(Theme::remapSheet(captionSheet));
    }

    int onlineCount = 0;
    for (const ChatUserGroup &group : std::as_const(groups))
        onlineCount += group.online ? 1 : 0;
    const auto addGroupHeading = [this](const QString &text) {
        auto *heading = new QLabel(text);
        heading->setObjectName(QStringLiteral("chatMembersGroupHeading"));
        m_chatMembersLayout->insertWidget(m_chatMembersLayout->count() - 1,
                                          heading);
    };
    bool headedOnline = false;
    bool headedOffline = false;
    for (const ChatUserGroup &group : std::as_const(groups)) {
        const MemberInfo &member = group.primary;
        if (group.online && !headedOnline) {
            headedOnline = true;
            addGroupHeading(QString::fromUtf8("Online \xE2\x80\x94 %1").arg(onlineCount));
        } else if (!group.online && !headedOffline) {
            headedOffline = true;
            addGroupHeading(QString::fromUtf8("Offline \xE2\x80\x94 %1")
                                .arg(groups.size() - onlineCount));
        }

        auto *card = new ClickableIssueBody;
        card->setObjectName(QStringLiteral("chatMemberCard"));
        card->setProperty("offline", !group.online);
        card->setAttribute(Qt::WA_StyledBackground, true);
        card->setAttribute(Qt::WA_Hover, true);
        card->setCursor(Qt::PointingHandCursor);
        card->setToolTip(QStringLiteral("View profile"));
        {
            QStringList nodeLines;
            for (const ChatNodeBadge &nb : std::as_const(group.nodes)) {
                QString line = nb.label.isEmpty() ? QStringLiteral("node") : nb.label;
                line += nb.online ? QStringLiteral(" · online")
                                  : QStringLiteral(" · offline");
                if (!nb.platform.trimmed().isEmpty())
                    line += QStringLiteral(" · ") + nb.platform.trimmed();
                nodeLines << line;
            }
            MemberInfo profileMember = member;
            profileMember.online = group.online;
            if (profileMember.id.startsWith(QStringLiteral("teammate:"))) {
                const QString teammateId = profileMember.id.mid(9);
                card->onClicked = [this, teammateId] {
                    showSection(kTeamSectionIndex);
                    showTeammateHome(teammateId);
                };
            } else {
                card->onClicked = [this, profileMember, nodeLines] {
                    showChatUserProfile(profileMember, nodeLines);
                };
            }
        }
        auto *row = new QHBoxLayout(card);
        row->setContentsMargins(8, 4, 8, 4);
        row->setSpacing(12);

        const forkmesh::userstatus::UserStatus status =
            member.id.startsWith(QStringLiteral("teammate:"))
                ? forkmesh::userstatus::UserStatus()
                : userStatusFor(group.key, group.anchorId, member.self);
        const bool away = group.online && status.away;

        // Every roster frame rebuilds these cards; the 28 px rounded medallion
        // (a smooth rescale under a clip path, or a painted letter tile) is a
        // pure function of the member's avatar bytes or name, so keep it.
        const QPixmap source = m_avatars.value(member.id);
        const QString avatarKey =
            member.id + QLatin1Char('|') + member.name + QLatin1Char('|') +
            QString::number(source.isNull() ? 0 : source.cacheKey()) +
            QLatin1Char('|') + QString::number(iconDevicePixelRatio()) +
            QLatin1Char('|') +
            (away ? QLatin1Char('a') : group.online ? QLatin1Char('o') : QLatin1Char('-')) +
            Theme::chatColor(QStringLiteral("chatRail")).name();
        QPixmap rounded = m_chatMemberAvatarCache.value(avatarKey);
        if (rounded.isNull()) {
            rounded = source.isNull()
                          ? MessageRow::initialsAvatar(member.name,
                                                       senderColor(member.name),
                                                       kChatMemberAvatarSide)
                          : roundedRectPixmap(source, kChatMemberAvatarSide,
                                              kChatMemberAvatarSide / 2.0);
            if (!group.online) {
                QPixmap faded(rounded.size());
                faded.setDevicePixelRatio(rounded.devicePixelRatio());
                faded.fill(Qt::transparent);
                QPainter painter(&faded);
                painter.setOpacity(0.4);
                painter.drawPixmap(0, 0, rounded);
                painter.end();
                rounded = faded;
            }
            rounded = withPresenceDot(
                rounded,
                Theme::chatColor(away ? QStringLiteral("chatAway")
                                 : group.online ? QStringLiteral("chatOnline")
                                                : QStringLiteral("chatFaint")),
                Theme::chatColor(QStringLiteral("chatRail")));
            if (m_chatMemberAvatarCache.size() > 512)
                m_chatMemberAvatarCache.clear();
            m_chatMemberAvatarCache.insert(avatarKey, rounded);
        }
        auto *icon = new QLabel;
        icon->setPixmap(rounded);
        icon->setFixedSize(kChatMemberAvatarSide, kChatMemberAvatarSide);
        row->addWidget(icon, 0, Qt::AlignVCenter);

        auto *textCol = new QWidget;
        auto *col = new QVBoxLayout(textCol);
        col->setContentsMargins(0, 0, 0, 0);
        col->setSpacing(2);

        const QString state = away           ? QStringLiteral("Away")
                              : group.online ? QStringLiteral("Online")
                                             : QStringLiteral("Offline");
        QString tooltip = state;
        if (!member.note.isEmpty())
            tooltip += QStringLiteral(" · ") + member.note;
        const QString statusSummary = forkmesh::userstatus::summary(status);
        if (!statusSummary.isEmpty()) {
            const QString until = forkmesh::userstatus::untilLabel(
                status.expiresAtMs, QDateTime::currentDateTime());
            tooltip += QLatin1Char('\n') + statusSummary +
                       (until.isEmpty() ? QString()
                                        : QStringLiteral(" — ") + until);
        }
        auto *nameLabel = new QLabel(
            QStringLiteral("<span style='color:%1'>%2</span>%3%4")
                .arg(Theme::chatColor(group.online ? QStringLiteral("chatText")
                                                   : QStringLiteral("chatFaint"))
                         .name(),
                     member.name.toHtmlEscaped(),
                     status.emoji.isEmpty()
                         ? QString()
                         : QStringLiteral(" ") + status.emoji.toHtmlEscaped(),
                     member.self ? " <span style='color:#8b949e'>(you)</span>"
                                 : QString()));
        nameLabel->setObjectName(QStringLiteral("chatMemberName"));
        nameLabel->setTextFormat(Qt::RichText);
        nameLabel->setToolTip(tooltip);
        col->addWidget(nameLabel, 0);

        // The Team room contains named AI colleagues alongside people. Show
        // each teammate's charter and current focus directly in the roster so
        // CEO, COO, and specialist roles are recognizable at a glance.
        if (member.id.startsWith(QStringLiteral("teammate:")) &&
            !member.note.isEmpty()) {
            auto *roleLabel = new QLabel(member.note);
            roleLabel->setObjectName(QStringLiteral("chatMemberRole"));
            roleLabel->setTextFormat(Qt::PlainText);
            roleLabel->setWordWrap(true);
            roleLabel->setToolTip(tooltip);
            col->addWidget(roleLabel, 0);
        }

        if (nameCounts.value(member.name.toLower()) > 1) {
            QString raw = group.anchorId.isEmpty() ? group.key : group.anchorId;
            if (raw.startsWith(QStringLiteral("user:")))
                raw = raw.mid(5);
            const QString shortId = raw.size() > 12
                ? QString::fromUtf8("%1\xE2\x80\xA6%2")
                      .arg(raw.left(4), raw.right(4))
                : raw;
            if (!shortId.isEmpty()) {
                auto *idLabel = new QLabel(
                    QStringLiteral("id %1").arg(shortId.toHtmlEscaped()));
                idLabel->setObjectName(QStringLiteral("chatMemberId"));
                idLabel->setToolTip(raw);
                col->addWidget(idLabel, 0);
            }
        }

        if (!group.nodes.isEmpty()) {
            auto *nodesRow = new QWidget;
            auto *nl = new QHBoxLayout(nodesRow);
            nl->setContentsMargins(0, 0, 0, 0);
            nl->setSpacing(3);
            constexpr int kMaxIcons = 8;
            int shown = 0;
            for (const ChatNodeBadge &nb : std::as_const(group.nodes)) {
                if (shown >= kMaxIcons)
                    break;
                auto *ni = new QLabel;
                ni->setPixmap(osBadgeIcon(nb.platform, nb.online, 14).pixmap(14, 14));
                ni->setFixedSize(14, 14);
                QString tip = nb.label.isEmpty() ? QStringLiteral("node") : nb.label;
                tip += nb.online ? QStringLiteral(" · online")
                                 : QStringLiteral(" · offline");
                if (!nb.platform.trimmed().isEmpty())
                    tip += QStringLiteral(" · ") + nb.platform;
                ni->setToolTip(tip);
                nl->addWidget(ni, 0);
                ++shown;
            }
            if (group.nodes.size() > kMaxIcons) {
                auto *more = new QLabel(
                    QStringLiteral("+%1").arg(group.nodes.size() - kMaxIcons));
                more->setObjectName(QStringLiteral("chatMemberMore"));
                nl->addWidget(more, 0);
            }
            nl->addStretch(1);
            col->addWidget(nodesRow, 0);
        }

        row->addWidget(textCol, 1);

        m_chatMembersLayout->insertWidget(m_chatMembersLayout->count() - 1, card);
    }
}

void MainWindow::showChatSenderProfile(const QString &senderId,
                                       const QString &senderName)
{
    static const QString discordPrefix = QStringLiteral("discord-user:");
    if (senderId.startsWith(discordPrefix)) {
        QMessageBox profile(this);
        profile.setWindowTitle(senderName.isEmpty() ? QStringLiteral("Discord user")
                                                    : senderName);
        profile.setIconPixmap(chatActorAvatar(senderId, senderName, 56));
        profile.setText(senderName.toHtmlEscaped());
        profile.setInformativeText(
            QStringLiteral("Discord user ID: %1\nShown from %2")
                .arg(senderId.mid(discordPrefix.size()),
                     m_discordClient
                         ? m_discordClient->displayName(m_currentConversation)
                         : QStringLiteral("Discord")));
        QPushButton *open = nullptr;
        if (m_discordClient &&
            m_discordClient->browserUrl(m_currentConversation).isValid()) {
            open = profile.addButton(QStringLiteral("Open channel in Discord"),
                                     QMessageBox::ActionRole);
        }
        profile.addButton(QMessageBox::Close);
        profile.exec();
        if (open && profile.clickedButton() == open)
            QDesktopServices::openUrl(
                m_discordClient->browserUrl(m_currentConversation));
        return;
    }

    MemberInfo profileMember;
    bool foundMember = false;
    for (const MemberInfo &member : std::as_const(m_homeRoster)) {
        if (!senderId.trimmed().isEmpty() && member.id == senderId) {
            profileMember = member;
            foundMember = true;
            break;
        }
    }
    if (!foundMember) {
        for (const MemberInfo &member : std::as_const(m_homeRoster)) {
            if (chatUserDisplayName(member).compare(senderName,
                                                    Qt::CaseInsensitive) == 0) {
                profileMember = member;
                foundMember = true;
                break;
            }
        }
    }

    QString account;
    if (senderId.startsWith(QStringLiteral("user:")))
        account = senderId.mid(QStringLiteral("user:").size()).trimmed().toLower();
    if (account.isEmpty() && foundMember)
        account = profileMember.ownerUser.trimmed().toLower();
    if (account.isEmpty())
        account = senderName.trimmed().toLower();

    const auto directoryMember = m_chatDirectoryUsers.constFind(account);
    if (directoryMember != m_chatDirectoryUsers.constEnd()) {
        MemberInfo registeredUser = *directoryMember;
        if (foundMember) {
            registeredUser.online = profileMember.online;
            registeredUser.self = profileMember.self;
            if (m_avatars.value(registeredUser.id).isNull() &&
                !m_avatars.value(profileMember.id).isNull())
                m_avatars.insert(registeredUser.id,
                                 m_avatars.value(profileMember.id));
        }
        profileMember = registeredUser;
    } else if (!foundMember) {
        profileMember.id = senderId;
        profileMember.name = senderName.trimmed();
    }

    if (directoryMember == m_chatDirectoryUsers.constEnd()) {
        const QString displayName =
            foundMember && !profileMember.ownerUser.trimmed().isEmpty()
                ? profileMember.ownerUser.trimmed()
                : senderName.trimmed();
        if (!displayName.isEmpty())
            profileMember.name = displayName;
    }
    if (!account.isEmpty() && profileMember.ownerUser.trimmed().isEmpty())
        profileMember.ownerUser = account;

    showChatUserProfile(profileMember, {});
}

void MainWindow::showChatUserProfile(const MemberInfo &member,
                                     const QStringList &nodeLines)
{
    const QString account =
        (member.ownerUser.trimmed().isEmpty() ? member.name : member.ownerUser)
            .trimmed()
            .toLower();

    auto *dialog = new QDialog(this);
    dialog->setObjectName("chatUserProfileDialog");
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(member.name);
    dialog->setMinimumWidth(380);

    QPixmap avatar = m_avatars.value(member.id);
    if (avatar.isNull())
        avatar = letterFavicon(member.name);
    auto *icon = new QLabel;
    icon->setPixmap(roundedRectPixmap(avatar, 56, 28)); // round
    icon->setFixedSize(56, 56);

    const forkmesh::userstatus::UserStatus userStatus = userStatusFor(
        account,
        member.id.startsWith(QStringLiteral("user:")) ? QString() : member.id,
        member.self);
    const bool away = member.online && userStatus.away;
    auto *nameLabel = new QLabel(
        QStringLiteral("<span style='font-size:16px;font-weight:700'>%1</span>%2%3")
            .arg(member.name.toHtmlEscaped(),
                 userStatus.emoji.isEmpty()
                     ? QString()
                     : QStringLiteral(" <span style='font-size:16px'>%1</span>")
                           .arg(userStatus.emoji.toHtmlEscaped()),
                 member.self
                     ? QStringLiteral(" <span style='color:#8b949e'>(you)</span>")
                     : QString()));
    nameLabel->setTextFormat(Qt::RichText);
    auto *statusLabel = new QLabel(
        QString::fromUtf8("<span style='color:%1'>\xE2\x97\x8F</span> %2")
            .arg(away            ? QStringLiteral("#d29922")
                 : member.online ? QStringLiteral("#3fb950")
                                 : QStringLiteral("#8b949e"),
                 away            ? QStringLiteral("Away")
                 : member.online ? QStringLiteral("Online")
                                 : QStringLiteral("Offline")));
    statusLabel->setTextFormat(Qt::RichText);
    // "🌴 Vacationing — until Mon 5:00 PM" under the presence line.
    auto *customStatusLabel = new QLabel;
    customStatusLabel->setObjectName(QStringLiteral("chatUserProfileStatus"));
    customStatusLabel->setTextFormat(Qt::PlainText);
    customStatusLabel->setWordWrap(true);
    {
        const QString summary = forkmesh::userstatus::summary(userStatus);
        const QString until = forkmesh::userstatus::untilLabel(
            userStatus.expiresAtMs, QDateTime::currentDateTime());
        customStatusLabel->setText(
            until.isEmpty() ? summary
                            : summary + QStringLiteral(" — ") + until);
        customStatusLabel->setVisible(!summary.isEmpty());
    }

    auto *headText = new QVBoxLayout;
    headText->setContentsMargins(0, 0, 0, 0);
    headText->setSpacing(2);
    headText->addWidget(nameLabel);
    headText->addWidget(statusLabel);
    headText->addWidget(customStatusLabel);
    headText->addStretch(1);

    auto *headRow = new QHBoxLayout;
    headRow->setContentsMargins(0, 0, 0, 0);
    headRow->setSpacing(12);
    headRow->addWidget(icon, 0, Qt::AlignTop);
    headRow->addLayout(headText, 1);

    auto *details = new QLabel;
    details->setTextFormat(Qt::RichText);
    details->setWordWrap(true);
    details->setTextInteractionFlags(Qt::TextSelectableByMouse);

    struct ProfileFacts {
        qint64 joinedMs = 0;
        int followers = -1, following = -1, mirrors = -1;
        QString location;
        bool isAdmin = false;
    };
    auto facts = std::make_shared<ProfileFacts>();
    facts->joinedMs = member.createdAtMs;
    if (facts->joinedMs <= 0) {
        const MemberInfo dirUser = m_chatDirectoryUsers.value(account);
        facts->joinedMs = dirUser.createdAtMs;
    }

    const QString accountShown = account;
    auto renderDetails = [details, facts, accountShown, nodeLines] {
        auto detailRow = [](const QString &key, const QString &value) {
            return QStringLiteral(
                       "<tr><td style='color:#8b949e;padding:2px 14px 2px 0;"
                       "white-space:nowrap'>%1</td>"
                       "<td style='padding:2px 0'>%2</td></tr>")
                .arg(key, value);
        };
        QStringList rows;
        if (!accountShown.isEmpty())
            rows << detailRow(QStringLiteral("Account"),
                              QStringLiteral("@%1").arg(accountShown.toHtmlEscaped()));
        if (facts->joinedMs > 0)
            rows << detailRow(
                QStringLiteral("Joined"),
                QStringLiteral("%1 <span style='color:#8b949e'>(%2)</span>")
                    .arg(formatRepoDate(facts->joinedMs),
                         formatIssueRelativeTime(facts->joinedMs)));
        if (!facts->location.isEmpty())
            rows << detailRow(QStringLiteral("Location"),
                              facts->location.toHtmlEscaped());
        if (facts->followers >= 0)
            rows << detailRow(QStringLiteral("Followers"),
                              QString::number(facts->followers));
        if (facts->following >= 0)
            rows << detailRow(QStringLiteral("Following"),
                              QString::number(facts->following));
        if (facts->mirrors > 0)
            rows << detailRow(QStringLiteral("Mirrored repos"),
                              QString::number(facts->mirrors));
        if (facts->isAdmin)
            rows << detailRow(QStringLiteral("Role"),
                              QString::fromUtf8("Administrator \xF0\x9F\x91\x91"));
        if (!nodeLines.isEmpty())
            rows << detailRow(
                QStringLiteral("Nodes (%1)").arg(nodeLines.size()),
                QStringList(nodeLines).replaceInStrings(
                    QStringLiteral("&"), QStringLiteral("&amp;"))
                    .replaceInStrings(QStringLiteral("<"), QStringLiteral("&lt;"))
                    .join(QStringLiteral("<br>")));
        details->setText(
            QStringLiteral("<table cellspacing='0' cellpadding='0'>%1</table>")
                .arg(rows.join(QString())));
    };
    renderDetails();

    auto *bioLabel = new QLabel;
    bioLabel->setWordWrap(true);
    bioLabel->setVisible(false);
    bioLabel->setStyleSheet(Theme::remapSheet(QStringLiteral("color:#c9d1d9;")));
    auto *loadingLabel = new QLabel(QString::fromUtf8("Loading profile\xE2\x80\xA6"));
    loadingLabel->setStyleSheet(Theme::remapSheet(QStringLiteral("color:#8b949e; font-size:11px;")));

    auto *buttonRow = new QHBoxLayout;
    buttonRow->setContentsMargins(0, 0, 0, 0);
    buttonRow->setSpacing(8);
    if (!member.self) {
        QString peerId, peerName;
        if (!member.id.isEmpty() &&
            !member.id.startsWith(QStringLiteral("user:")) && member.online) {
            peerId = member.id;
            peerName = member.name;
        } else {
            for (const MemberInfo &m : std::as_const(m_homeRoster)) {
                if (!m.online || m.id.isEmpty() || m.self)
                    continue;
                const QString owner = m.ownerUser.trimmed().toLower();
                const QString name = m.name.trimmed().toLower();
                if (owner == account || (owner.isEmpty() && name == account)) {
                    peerId = m.id;
                    peerName = member.name;
                    break;
                }
            }
        }
        const bool serverDirect =
            !account.isEmpty() && m_officeChannelMirror &&
            m_officeChannelMirror->isActive();
        if (serverDirect || !peerId.isEmpty()) {
            auto *messageButton = new QPushButton(QStringLiteral("Message"));
            messageButton->setObjectName("primaryButton");
            messageButton->setCursor(Qt::PointingHandCursor);
            messageButton->setToolTip(
                QStringLiteral("Open a private conversation with %1")
                    .arg(member.name));
            connect(messageButton, &QPushButton::clicked, this,
                    [this, dialog, account, peerId, peerName] {
                        dialog->accept();
                        startDirectMessageWith(account, peerId, peerName);
                    });
            buttonRow->addWidget(messageButton);
        }
    }
    if (member.self) {
        auto *editButton = new QPushButton(QStringLiteral("Edit profile"));
        editButton->setObjectName("primaryButton");
        editButton->setCursor(Qt::PointingHandCursor);
        connect(editButton, &QPushButton::clicked, this, [this, dialog] {
            dialog->accept();
            showEditAccountProfileDialog();
        });
        buttonRow->addWidget(editButton);
    }
    if (member.self && hasActiveAccountSession()) {
        auto *statusButton = new QPushButton(QStringLiteral("Set a status…"));
        statusButton->setObjectName("ghostButton");
        statusButton->setCursor(Qt::PointingHandCursor);
        connect(statusButton, &QPushButton::clicked, this, [this, dialog] {
            dialog->accept();
            showSetUserStatusDialog();
        });
        buttonRow->addWidget(statusButton);
    }
    if (!account.isEmpty()) {
        auto *webButton = new QPushButton(QStringLiteral("Web profile"));
        webButton->setObjectName("ghostButton");
        webButton->setCursor(Qt::PointingHandCursor);
        connect(webButton, &QPushButton::clicked, this, [this, account] {
            const QUrl url = chatMentionProfileUrl(
                m_serverUrlEdit ? m_serverUrlEdit->text() : QString(), account);
            if (url.isValid() && !url.host().isEmpty())
                QDesktopServices::openUrl(url);
        });
        buttonRow->addWidget(webButton);
    }
    buttonRow->addStretch(1);
    auto *closeButton = new QPushButton(QStringLiteral("Close"));
    closeButton->setObjectName("ghostButton");
    closeButton->setCursor(Qt::PointingHandCursor);
    connect(closeButton, &QPushButton::clicked, dialog, &QDialog::accept);
    buttonRow->addWidget(closeButton);

    auto *layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(20, 18, 20, 16);
    layout->setSpacing(12);
    layout->addLayout(headRow);
    layout->addWidget(details);
    layout->addWidget(bioLabel);
    layout->addWidget(loadingLabel);
    layout->addStretch(1);
    layout->addLayout(buttonRow);

    if (m_networkAccess && !account.isEmpty()) {
        QNetworkRequest request(accountsApiUrl(account));
        request.setRawHeader("Accept", "application/json");
        QNetworkReply *reply = m_networkAccess->get(request);
        QPointer<QDialog> dialogGuard(dialog);
        QPointer<QLabel> bioGuard(bioLabel);
        QPointer<QLabel> loadingGuard(loadingLabel);
        connect(reply, &QNetworkReply::finished, this,
                [reply, dialogGuard, bioGuard, loadingGuard, facts,
                 renderDetails] {
                    const QByteArray body = reply->readAll();
                    const bool ok = reply->error() == QNetworkReply::NoError;
                    reply->deleteLater();
                    if (!dialogGuard)
                        return;
                    if (loadingGuard)
                        loadingGuard->setVisible(false);
                    if (!ok)
                        return;
                    const QJsonObject rec = QJsonDocument::fromJson(body).object();
                    if (!rec.value(QStringLiteral("exists")).toBool(true))
                        return;
                    const qint64 created =
                        qint64(rec.value(QStringLiteral("createdAt")).toDouble());
                    if (created > 0)
                        facts->joinedMs = created;
                    facts->location =
                        rec.value(QStringLiteral("profileLocation")).toString().trimmed();
                    facts->followers =
                        rec.value(QStringLiteral("followers")).toInt(-1);
                    facts->following =
                        rec.value(QStringLiteral("following")).toInt(-1);
                    facts->mirrors =
                        rec.value(QStringLiteral("mirrorCount")).toInt(-1);
                    facts->isAdmin = rec.value(QStringLiteral("isAdmin")).toBool();
                    renderDetails();
                    const QString bio =
                        rec.value(QStringLiteral("profileBio")).toString().trimmed();
                    if (bioGuard && !bio.isEmpty()) {
                        bioGuard->setText(bio.toHtmlEscaped());
                        bioGuard->setVisible(true);
                    }
                });
    } else {
        loadingLabel->setVisible(false);
    }

    dialog->show();
}

void MainWindow::removeChatMember(const QString &id, const QString &name)
{
    if (id.isEmpty())
        return;

    m_removedPeerIds.insert(id);

    if (m_backend)
        m_backend->forgetMember(id);

    const QString conversation = dmKey(id);
    m_openDms.removeAll(id);
    m_dmNames.remove(id);
    m_avatars.remove(id);
    m_unread.remove(conversation);
    m_unreadCounts.remove(conversation);
    const QList<ChatMessage> removedMessages = m_history.take(conversation);
    for (const ChatMessage &message : removedMessages) {
        m_historyIds.remove(message.id);
        m_reactions.remove(message.id);
    }
    m_typing.remove(conversation);
    for (const QString &key : m_typing.keys()) {
        m_typing[key].remove(id);
        if (m_typing.value(key).isEmpty())
            m_typing.remove(key);
    }
    if (m_typingConversation == conversation)
        sendTypingState(false);
    if (m_currentConversation == conversation) {
        if (m_channels.isEmpty()) {
            m_currentConversation.clear();
            m_channelTitle->setText(QStringLiteral("No conversation"));
            m_messageInput->setPlaceholderText(QStringLiteral("Message"));
            rebuildConversationView();
            refreshTypingLabel();
        } else {
            switchConversation(m_channels.first());
        }
    }

    QList<MemberInfo> remaining;
    for (const MemberInfo &m : std::as_const(m_homeRoster))
        if (m.id != id)
            remaining.append(m);
    refreshDmList();
    setRoster(remaining);
    saveChatHistory();
    logSystem("Removed stale member " + (name.isEmpty() ? id.left(8) : name) + ".");
}

void MainWindow::refreshChannelList()
{
    QSignalBlocker blocker(m_channelList);
    m_channelList->clear();
    // m_channels stays the flat, load-bearing order (unread priority reads
    // it); the tree is just that list folded into the user's groups plus the
    // providers' automatic ones.
    QStringList sidebarChannels = m_channels;
    for (const QString &conversation : unlistedChatConversations()) {
        if (!isDirectConversation(conversation) &&
            !forkmesh::office::isDirectOfficeConversation(conversation))
            sidebarChannels.append(conversation);
    }
    m_chatSidebarRows = ChannelGroups::layout(
        m_channelGroups, sidebarChannels,
        [this](const QString &channel) { return autoGroupForChannel(channel); });

    // Same look as the CHANNELS / DIRECT MESSAGES section labels
    // (#sectionLabel: 11px, bold, 1px letter spacing). Pinned in pixels
    // rather than derived from the widget font, which is not polished yet on
    // the first rebuild.
    QFont headerFont = m_channelList->font();
    headerFont.setBold(true);
    headerFont.setPixelSize(11);
    headerFont.setLetterSpacing(QFont::AbsoluteSpacing, 1);
    const QBrush headerBrush(
        QColor(currentThemeIsDark() ? "#8b949e" : "#656d76"));
    const QIcon settingsIcon(tintedOcticonPixmap(
        QStringLiteral("gear"),
        QColor(currentThemeIsDark() ? "#8b949e" : "#656d76"), 14));

    QTreeWidgetItem *header = nullptr;
    bool headerCollapsed = false;
    QTreeWidgetItem *current = nullptr;
    // A header's expansion is applied once its children exist; expanding an
    // item that has none yet is a no-op in QTreeView.
    const auto finishHeader = [&header, &headerCollapsed] {
        if (header)
            header->setExpanded(!headerCollapsed);
    };
    for (const ChannelGroups::Row &row : std::as_const(m_chatSidebarRows)) {
        if (row.header) {
            finishHeader();
            header = new QTreeWidgetItem(m_channelList);
            header->setFirstColumnSpanned(true);
            headerCollapsed = row.collapsed;
            header->setFlags(Qt::ItemIsEnabled | Qt::ItemIsDropEnabled);
            header->setData(0, ChannelGroupTree::kGroupRole, row.group);
            header->setText(0, channelGroupHeaderText(row.group, row.collapsed));
            header->setFont(0, headerFont);
            header->setForeground(0, headerBrush);
            header->setToolTip(
                0, row.automatic
                       ? QStringLiteral("%1 — channels from this server are "
                                        "grouped automatically. Click to "
                                        "collapse; drag channels in or out.")
                             .arg(row.group)
                       : QStringLiteral("%1 — click to collapse; drag "
                                        "channels here to group them.")
                             .arg(row.group));
            continue;
        }
        const QString &channel = row.channel;
        QString label = channel;
        QString tooltip;
        if (isDiscordConversation(channel) && m_discordClient) {
            // Under its own server's header the guild prefix is redundant:
            // "Discord · Forkmesh / #general" becomes "#general".
            const QString full = m_discordClient->displayName(channel);
            label = row.group == m_discordClient->groupName(channel)
                        ? m_discordClient->shortName(channel)
                        : full;
            tooltip = full;
        }
        if (forkmesh::office::isOfficeConversation(channel) &&
            !forkmesh::office::isDirectOfficeConversation(channel))
            label = officeChannelLabel(channel);
        if (channel == forkmesh::office::worldConversation()) {
            label = forkmesh::office::worldLabel();
            tooltip = QStringLiteral(
                "The World chat: everyone walking the 3D World, guests and "
                "visitors included. ForkMesh members talk in #general.");
        }
        if (isLocalMeshConversation(channel)) {
            tooltip = QStringLiteral(
                "Everyone running ForkMesh on this network, found "
                "automatically. End-to-end encrypted and never sent to the "
                "internet.");
        }
        const QString prefix =
            (m_unread.contains(channel) ? "\xE2\x97\x8F " : "") +
            (m_privateChannels.contains(channel) ||
                     isLocalMeshConversation(channel)
                 ? QString::fromUtf8("\xF0\x9F\x94\x92 ")
                 : QString());
        auto *item = header && !row.group.isEmpty()
                         ? new QTreeWidgetItem(header)
                         : new QTreeWidgetItem(m_channelList);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable |
                       Qt::ItemIsDragEnabled);
        item->setText(0, prefix + label);
        item->setData(0, Qt::UserRole, channel);
        item->setData(0, ChannelGroupTree::kUnreadCountRole,
                      m_unreadCounts.value(channel));
        const forkmesh::chatlist::Preview preview = chatListPreview(channel);
        if (!preview.isEmpty()) {
            item->setData(0, ChannelGroupTree::kPreviewRole, preview.text);
            item->setData(0, ChannelGroupTree::kPreviewTimeRole,
                          preview.timestampMs);
        }
        if (!tooltip.isEmpty())
            item->setToolTip(0, tooltip);
        item->setIcon(ChannelGroupTree::kSettingsColumn, settingsIcon);
        item->setToolTip(ChannelGroupTree::kSettingsColumn,
                         QStringLiteral("Channel settings"));
        if (channel == m_currentConversation)
            current = item;
    }
    finishHeader();
    if (current)
        m_channelList->setCurrentItem(current);
    updateChatButton();
}

void MainWindow::selectFirstChannel()
{
    for (const ChannelGroups::Row &row : std::as_const(m_chatSidebarRows)) {
        if (!row.header) {
            switchConversation(row.channel);
            return;
        }
    }
}

QString MainWindow::autoGroupForChannel(const QString &channel) const
{
    if (m_discordClient && isDiscordConversation(channel))
        return m_discordClient->groupName(channel);
    return QString();
}

QString MainWindow::channelGroupHeaderText(const QString &group,
                                           bool collapsed) const
{
    bool unreadInside = false;
    if (collapsed) {
        for (const ChannelGroups::Row &row : m_chatSidebarRows) {
            if (!row.header && row.group == group && m_unread.contains(row.channel)) {
                unreadInside = true;
                break;
            }
        }
    }
    return QString(QChar(collapsed ? 0x25B8 : 0x25BE)) + QLatin1Char(' ') +
           (unreadInside ? QString(QChar(0x25CF)) + QLatin1Char(' ') : QString()) +
           group.toUpper();
}

void MainWindow::persistChannelGroups()
{
    const QString json = ChannelGroups::toJson(m_channelGroups);
    QSettings settings;
    if (json.isEmpty())
        settings.remove(kChannelGroupsSetting);
    else
        settings.setValue(kChannelGroupsSetting, json);
}

void MainWindow::moveChannelToGroup(const QString &channel, const QString &group,
                                    int index)
{
    const QString target = ChannelGroups::normalizeName(group);
    const QStringList members =
        ChannelGroups::renderedMembersOf(m_chatSidebarRows, target);
    const bool automatic =
        ChannelGroups::renderedGroupIsAutomatic(m_chatSidebarRows, target);
    if (!ChannelGroups::assign(m_channelGroups, channel, target, index, members,
                               automatic))
        return;
    persistChannelGroups();
    refreshChannelList();
}

QString MainWindow::promptChannelGroupName(const QString &title,
                                           const QString &current)
{
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, title, QStringLiteral("Group name"), QLineEdit::Normal, current, &ok);
    return ok ? ChannelGroups::normalizeName(name) : QString();
}

void MainWindow::promptAddChannelGroup()
{
    const QString name =
        promptChannelGroupName(QStringLiteral("New group"), QString());
    if (name.isEmpty())
        return;
    if (!ChannelGroups::createGroup(m_channelGroups, name)) {
        flashMessage(QStringLiteral("A group named %1 already exists.").arg(name),
                     true);
        return;
    }
    persistChannelGroups();
    refreshChannelList();
}

void MainWindow::promptRenameChannelGroup(const QString &group)
{
    const QString name =
        promptChannelGroupName(QStringLiteral("Rename group"), group);
    if (name.isEmpty() || name == group)
        return;
    if (!ChannelGroups::renameGroup(
            m_channelGroups, group, name,
            ChannelGroups::renderedMembersOf(m_chatSidebarRows, group))) {
        flashMessage(QStringLiteral("A group named %1 already exists.").arg(name),
                     true);
        return;
    }
    persistChannelGroups();
    refreshChannelList();
}

void MainWindow::removeChannelGroup(const QString &group)
{
    ChannelGroups::removeGroup(
        m_channelGroups, group,
        ChannelGroups::renderedMembersOf(m_chatSidebarRows, group));
    persistChannelGroups();
    refreshChannelList();
}

void MainWindow::setChannelGroupCollapsed(const QString &group, bool collapsed)
{
    ChannelGroups::setCollapsed(
        m_channelGroups, group, collapsed,
        ChannelGroups::renderedGroupIsAutomatic(m_chatSidebarRows, group));
    for (ChannelGroups::Row &row : m_chatSidebarRows)
        if (row.header && row.group == group)
            row.collapsed = collapsed;
    persistChannelGroups();
    // Update the header in place: rebuilding the tree from inside its own
    // expand/collapse signal would delete the item Qt is still handling.
    for (int i = 0; i < m_channelList->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = m_channelList->topLevelItem(i);
        if (ChannelGroupTree::groupNameOf(item) == group) {
            item->setText(0, channelGroupHeaderText(group, collapsed));
            break;
        }
    }
}

void MainWindow::persistChatSidebarWidth(int width)
{
    m_pendingChatSidebarWidth = width;
    if (!m_chatSidebarWidthTimer) {
        m_chatSidebarWidthTimer = new QTimer(this);
        m_chatSidebarWidthTimer->setSingleShot(true);
        m_chatSidebarWidthTimer->setInterval(600);
        connect(m_chatSidebarWidthTimer, &QTimer::timeout, this, [this] {
            // splitterMoved fires for every pixel of a drag and each
            // setValue() rewrites the whole ini, so persist once it settles.
            QSettings settings;
            if (settings.value(kChatSidebarWidthSetting, -1).toInt() !=
                m_pendingChatSidebarWidth)
                settings.setValue(kChatSidebarWidthSetting,
                                  m_pendingChatSidebarWidth);
        });
    }
    m_chatSidebarWidthTimer->start();
}

#ifdef FORKMESH_WINDOW_TESTS
QStringList MainWindow::testChatSidebarRows() const
{
    QStringList rows;
    for (const ChannelGroups::Row &row : m_chatSidebarRows) {
        if (row.header)
            rows.append(QStringLiteral("[%1]").arg(row.group));
        else if (row.group.isEmpty())
            rows.append(row.channel);
        else
            rows.append(row.group + QStringLiteral(" > ") + row.channel);
    }
    return rows;
}

bool MainWindow::testCreateChannelGroup(const QString &name)
{
    if (!ChannelGroups::createGroup(m_channelGroups, name))
        return false;
    persistChannelGroups();
    refreshChannelList();
    return true;
}
#endif

void MainWindow::refreshDmList()
{
    if (!m_dmList)
        return;
    QSignalBlocker blocker(m_dmList);
    m_dmList->clear();
    m_dmList->setIconSize(QSize(kChatListAvatarSide, kChatListAvatarSide));
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const auto addRow = [this](const QString &key, const QString &peerId,
                               const QString &name,
                               const forkmesh::userstatus::UserStatus &status,
                               const QString &tooltip, bool online) {
        auto *item = new QListWidgetItem(
            (m_unread.contains(key) ? "\xE2\x97\x8F " : "") + kDmPrefix + name +
            (status.emoji.isEmpty() ? QString()
                                    : QStringLiteral(" ") + status.emoji));
        if (!tooltip.isEmpty())
            item->setToolTip(tooltip);
        item->setData(Qt::UserRole, key);
        item->setData(ChannelGroupTree::kUnreadCountRole,
                      m_unreadCounts.value(key));
        item->setData(ChannelGroupTree::kPresenceRole, online);
        item->setData(ChannelGroupTree::kAvatarRole,
                      chatListAvatar(peerId, name, online,
                                     online && status.away));
        const forkmesh::chatlist::Preview preview = chatListPreview(key);
        if (!preview.isEmpty()) {
            item->setData(ChannelGroupTree::kPreviewRole, preview.text);
            item->setData(ChannelGroupTree::kPreviewTimeRole,
                          preview.timestampMs);
        }
        m_dmList->addItem(item);
        if (key == m_currentConversation)
            m_dmList->setCurrentItem(item);
    };
    for (const QString &peerId : std::as_const(m_openDms)) {
        const QString name = m_dmNames.value(peerId, QStringLiteral("unknown"));
        const forkmesh::userstatus::UserStatus status =
            userStatusFor(name, peerId);
        addRow(dmKey(peerId), peerId, name, status,
               forkmesh::userstatus::tooltip(status, now),
               chatPeerOnline(peerId, name));
    }
    for (const QString &key : std::as_const(m_officeDirectConversations)) {
        const QString peer = forkmesh::office::directPeerForConversation(key);
        if (peer.isEmpty())
            continue;
        const forkmesh::userstatus::UserStatus status =
            userStatusFor(peer, QString());
        addRow(key, QString(), peer, status,
               QStringLiteral("Private conversation: only you and @%1 can read it")
                   .arg(peer),
               chatPeerOnline(QString(), peer));
    }
    for (const QString &key : unlistedChatConversations()) {
        if (isDirectConversation(key)) {
            const QString peerId = dmPeerId(key);
            const QString name =
                m_dmNames.value(peerId, QStringLiteral("unknown"));
            addRow(key, peerId, name, {}, QString(),
                   chatPeerOnline(peerId, name));
        } else if (forkmesh::office::isDirectOfficeConversation(key)) {
            const QString peer = forkmesh::office::directPeerForConversation(key);
            if (!peer.isEmpty())
                addRow(key, QString(), peer, {}, QString(),
                       chatPeerOnline(QString(), peer));
        }
    }
    updateHomeStats();
    updateChatButton();
}

forkmesh::chatlist::Preview MainWindow::chatListPreview(
    const QString &conversation) const
{
    const auto it = m_history.constFind(conversation);
    if (it == m_history.constEnd())
        return {};
    return forkmesh::chatlist::preview(
        *it,
        isDirectConversation(conversation) ||
            forkmesh::office::isDirectOfficeConversation(conversation),
        chatDisplayName());
}

void MainWindow::refreshChatListPreview(const QString &conversation)
{
    if (conversation.isEmpty())
        return;
    const forkmesh::chatlist::Preview preview = chatListPreview(conversation);
    const auto apply = [&preview](auto &&read, auto &&write) {
        if (read(ChannelGroupTree::kPreviewRole).toString() != preview.text)
            write(ChannelGroupTree::kPreviewRole, preview.text);
        if (read(ChannelGroupTree::kPreviewTimeRole).toLongLong() !=
            preview.timestampMs)
            write(ChannelGroupTree::kPreviewTimeRole, preview.timestampMs);
    };
    for (int row = 0; m_dmList && row < m_dmList->count(); ++row) {
        QListWidgetItem *item = m_dmList->item(row);
        if (item->data(Qt::UserRole).toString() != conversation)
            continue;
        apply([item](int role) { return item->data(role); },
              [item](int role, const QVariant &value) {
                  item->setData(role, value);
              });
        return;
    }
    if (!m_channelList)
        return;
    for (QTreeWidgetItemIterator it(m_channelList); *it; ++it) {
        QTreeWidgetItem *item = *it;
        if (ChannelGroupTree::isGroupItem(item) ||
            item->data(0, Qt::UserRole).toString() != conversation)
            continue;
        apply([item](int role) { return item->data(0, role); },
              [item](int role, const QVariant &value) {
                  item->setData(0, role, value);
              });
        return;
    }
}

void MainWindow::refreshChatListPreviews()
{
    for (int row = 0; m_dmList && row < m_dmList->count(); ++row)
        refreshChatListPreview(
            m_dmList->item(row)->data(Qt::UserRole).toString());
    if (!m_channelList)
        return;
    QStringList channels;
    for (QTreeWidgetItemIterator it(m_channelList); *it; ++it)
        if (!ChannelGroupTree::isGroupItem(*it))
            channels.append((*it)->data(0, Qt::UserRole).toString());
    for (const QString &channel : std::as_const(channels))
        refreshChatListPreview(channel);
}

QPixmap MainWindow::chatListAvatar(const QString &peerId, const QString &name,
                                   bool online, bool away)
{
    const QPixmap source = chatSenderAvatarSource(peerId, name);
    const QColor faint = Theme::chatColor(QStringLiteral("chatFaint"));
    const QString key =
        QStringLiteral("dm|") + peerId + QLatin1Char('|') + name +
        QLatin1Char('|') +
        QString::number(source.isNull() ? 0 : source.cacheKey()) +
        QLatin1Char('|') + QString::number(iconDevicePixelRatio()) +
        QLatin1Char('|') +
        (away ? QLatin1Char('a') : online ? QLatin1Char('o') : QLatin1Char('-')) +
        faint.name();
    const QPixmap cached = m_chatMemberAvatarCache.value(key);
    if (!cached.isNull())
        return cached;
    QPixmap face = source.isNull()
                       ? MessageRow::initialsAvatar(name, senderColor(name),
                                                    kChatListAvatarSide)
                       : roundedAvatar(source, kChatListAvatarSide, 0.5);
    if (face.isNull())
        return face;
    {
        const qreal side = kChatListAvatarSide;
        const qreal diameter = 10.0;
        const qreal ring = 2.0;
        const QRectF dot(side - diameter, side - diameter, diameter, diameter);
        QPainter painter(&face);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::transparent);
        painter.drawEllipse(dot.adjusted(-ring, -ring, ring, ring));
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        if (online) {
            painter.setBrush(Theme::chatColor(away ? QStringLiteral("chatAway")
                                                   : QStringLiteral("chatOnline")));
            painter.drawEllipse(dot);
        } else {
            painter.setPen(QPen(faint, 1.6));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(dot.adjusted(0.8, 0.8, -0.8, -0.8));
        }
    }
    if (m_chatMemberAvatarCache.size() > 512)
        m_chatMemberAvatarCache.clear();
    m_chatMemberAvatarCache.insert(key, face);
    return face;
}

void MainWindow::updateChatConversationHeading()
{
    if (!m_channelTitle)
        return;
    refreshChatWorkspaceUser();
    const QString title = chatConversationLabel(m_currentConversation);
    QString topic;
    if (isDiscordConversation(m_currentConversation) && m_discordClient) {
        topic =
            m_discordClient->topic(m_currentConversation).simplified().left(240);
    } else if (m_officeChannelMirror) {
        topic = m_officeChannelMirror->topicForConversation(m_currentConversation);
    }
    QString glyph;
    QString shownTitle = title;
    if (shownTitle.startsWith(QLatin1Char('#')) ||
        shownTitle.startsWith(kDmPrefix)) {
        glyph = shownTitle.left(1);
        shownTitle.remove(0, 1);
    }
    m_channelTitle->setText(shownTitle);
    if (QWidget *bar = m_channelTitle->parentWidget()) {
        if (auto *hash = bar->findChild<QLabel *>(QStringLiteral("channelHash"))) {
            hash->setText(glyph);
            hash->setVisible(!glyph.isEmpty());
        }
    }
    if (m_channelTopicLabel) {
        m_channelTopicLabel->setText(topic);
        m_channelTopicLabel->setToolTip(topic);
        m_channelTopicLabel->setVisible(!topic.isEmpty());
        if (QWidget *bar = m_channelTopicLabel->parentWidget()) {
            if (auto *divider = bar->findChild<QFrame *>(
                    QStringLiteral("channelTopicDivider")))
                divider->setVisible(!topic.isEmpty());
        }
    }
    if (m_serverCopiesLabel) {
        const int stored =
            forkmesh::office::isDirectOfficeConversation(m_currentConversation)
                ? m_directServerCopies.value(m_currentConversation, -1)
                : -1;
        const QString peer =
            forkmesh::office::directPeerForConversation(m_currentConversation);
        m_serverCopiesLabel->setText(
            forkmesh::recipients::serverCopiesLabel(stored));
        m_serverCopiesLabel->setToolTip(
            forkmesh::recipients::serverCopiesTooltip(stored, peer));
        m_serverCopiesLabel->setStyleSheet(
            stored > 0 ? QStringLiteral("color: #d29922; font-size: 11px;")
                       : QStringLiteral("color: #8b949e; font-size: 11px;"));
        m_serverCopiesLabel->setVisible(stored >= 0);
    }
    if (m_encryptionLabel && m_officeChannelMirror &&
        m_officeChannelMirror->isServerVisibleConversation(
            m_currentConversation)) {
        m_encryptionLabel->setPixmap(tintedOcticonPixmap(
            QStringLiteral("server"), QColor(QStringLiteral("#8b949e")), 16));
        m_encryptionLabel->setToolTip(
            QStringLiteral("Server-visible notices written by ForkMesh; not "
                           "end-to-end encrypted. Only you can see this "
                           "channel"));
    } else if (m_encryptionLabel) {
        const bool discord = isDiscordConversation(m_currentConversation);
        const bool office = isOfficeConversation(m_currentConversation);
        const bool publicChannel =
            m_currentConversation.startsWith(QLatin1Char('#')) &&
            !m_privateChannels.contains(m_currentConversation) &&
            (!office || (m_officeChannelMirror &&
                         !m_officeChannelMirror->isPrivateConversation(
                             m_currentConversation)));
        m_encryptionLabel->setPixmap(tintedOcticonPixmap(
            discord ? QStringLiteral("server")
                    : publicChannel ? QStringLiteral("unlock")
                                    : QStringLiteral("lock"),
            QColor(QStringLiteral("#8b949e")), 16));
        m_encryptionLabel->setToolTip(
            discord
                ? QStringLiteral("Discord bot connection over TLS; Discord and "
                                 "server moderators can read this channel")
                : publicChannel
                    ? QStringLiteral("Public channel; the controller can read it "
                                     "for ForkBot, MCP, and webhook access")
                    : QStringLiteral("Automatic end-to-end encryption; the "
                                     "controller room key cannot read messages"));
    }
}

void MainWindow::switchConversation(const QString &conversation)
{
    if (conversation.isEmpty() || conversation == m_currentConversation)
        return;
    closeChatThread();
    sendTypingState(false);
    m_currentConversation = conversation;
    m_unread.remove(conversation);
    m_unreadCounts.remove(conversation);
    if (isChatViewVisible())
        queueReadMark(conversation);
    scheduleChatSave();
    refreshMeshDebugBar();

    const QString title = chatConversationLabel(conversation);
    updateChatConversationHeading();
    m_messageInput->setReadOnly(false);
    m_messageInput->setPlaceholderText(
        QStringLiteral("Message %1").arg(title.left(120)));
    refreshChatComposerAccess();
    refreshChannelSettingsButton();
    refreshRoomSealButton();
    rebuildConversationView();
    if (isDiscordConversation(conversation) && m_discordClient)
        m_discordClient->openConversation(conversation);
    if (m_officeChannelMirror &&
        forkmesh::office::isOfficeConversation(conversation))
        m_officeChannelMirror->watchConversation(conversation);
    refreshTypingLabel();
    refreshChatMembers();
    scheduleNavRecord();

    if (isDirectConversation(conversation)) {
        QSignalBlocker blocker(m_channelList);
        m_channelList->clearSelection();
        m_channelList->setCurrentItem(nullptr);
    } else {
        QSignalBlocker blocker(m_dmList);
        m_dmList->clearSelection();
        m_dmList->setCurrentItem(nullptr);
    }
    refreshChannelList();
    refreshDmList();
}

void MainWindow::openDirectChat(const QString &peerId, const QString &peerName)
{
    m_dmNames.insert(peerId, peerName);
    if (!m_openDms.contains(peerId)) {
        m_openDms.append(peerId);
        refreshDmList();
    }
    showChatView(); // chat has no tab now — surface the chat view explicitly
    switchConversation(dmKey(peerId));
    m_messageInput->setFocus();
}

void MainWindow::promptAddChannel()
{
    if (!m_backend)
        return;
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, "Add channel", "Channel name:", QLineEdit::Normal, "#", &ok);
    if (ok && !name.trimmed().isEmpty() && name.trimmed() != "#")
        m_backend->addChannel(name);
}

void MainWindow::promptAddPrivateChannel()
{
    showNewPrivateRoomDialog();
}

QStringList MainWindow::chatPickerAccounts() const
{
    const QString self = accountOwner().trimmed().toLower();
    QStringList accounts;
    for (auto it = m_chatDirectoryUsers.constBegin();
         it != m_chatDirectoryUsers.constEnd(); ++it) {
        const QString key = it.key().trimmed().toLower();
        if (key.isEmpty() || key == self ||
            key == QLatin1String("forkbot") || accounts.contains(key))
            continue;
        accounts.append(key);
    }
    std::sort(accounts.begin(), accounts.end());
    return accounts;
}

QStringList MainWindow::pickChatPeople(const QString &title,
                                       const QString &prompt, bool multiple,
                                       const QStringList &exclude)
{
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("chatPeoplePicker"));
    dialog.setWindowTitle(title);
    dialog.setMinimumWidth(380);
    auto *label = new QLabel(prompt, &dialog);
    label->setWordWrap(true);
    auto *filter = new QLineEdit(&dialog);
    filter->setPlaceholderText(QStringLiteral("Search people or type a username"));
    filter->setClearButtonEnabled(true);
    auto *list = new QListWidget(&dialog);
    list->setMinimumHeight(220);
    list->setSelectionMode(multiple ? QAbstractItemView::NoSelection
                                    : QAbstractItemView::SingleSelection);
    QSet<QString> online;
    for (const MemberInfo &member : std::as_const(m_homeRoster)) {
        if (!member.online || member.self)
            continue;
        const QString owner = member.ownerUser.trimmed().toLower();
        online.insert(owner.isEmpty() ? member.name.trimmed().toLower() : owner);
    }
    for (const QString &account : chatPickerAccounts()) {
        if (exclude.contains(account))
            continue;
        auto *item = new QListWidgetItem(
            QStringLiteral("@") + account +
                (online.contains(account)
                     ? QString::fromUtf8("  \xE2\x97\x8F online")
                     : QString()),
            list);
        item->setData(Qt::UserRole, account);
        if (multiple) {
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(Qt::Unchecked);
        }
    }
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)
        ->setText(multiple ? QStringLiteral("Invite")
                           : QStringLiteral("Message"));
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(filter, &QLineEdit::textChanged, &dialog, [list](const QString &text) {
        const QString needle = text.trimmed().toLower().remove(QLatin1Char('@'));
        for (int row = 0; row < list->count(); ++row) {
            QListWidgetItem *item = list->item(row);
            item->setHidden(!needle.isEmpty() &&
                            !item->data(Qt::UserRole).toString().contains(needle));
        }
    });
    if (!multiple)
        connect(list, &QListWidget::itemDoubleClicked, &dialog,
                [&dialog](QListWidgetItem *) { dialog.accept(); });
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(label);
    layout->addWidget(filter);
    layout->addWidget(list, 1);
    layout->addWidget(buttons);
    filter->setFocus();
    if (dialog.exec() != QDialog::Accepted)
        return {};
    QStringList chosen;
    for (int row = 0; row < list->count(); ++row) {
        QListWidgetItem *item = list->item(row);
        const bool picked = multiple ? item->checkState() == Qt::Checked
                                     : item->isSelected();
        if (picked)
            chosen.append(item->data(Qt::UserRole).toString());
    }
    const QString typed =
        filter->text().trimmed().toLower().remove(QLatin1Char('@'));
    if (chosen.isEmpty() && !typed.isEmpty() && !exclude.contains(typed) &&
        typed != accountOwner().trimmed().toLower())
        chosen.append(typed);
    return chosen;
}

void MainWindow::showNewPrivateRoomDialog()
{
    if (!m_officeChannelMirror || !m_officeChannelMirror->isActive()) {
        QMessageBox::information(
            this, QStringLiteral("New private room"),
            QStringLiteral("Private rooms are kept by the ForkMesh relay so "
                           "only invited people can join them. Sign in with "
                           "your account key to create one."));
        return;
    }
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("newPrivateRoomDialog"));
    dialog.setWindowTitle(QStringLiteral("New private room"));
    dialog.setMinimumWidth(420);
    auto *nameEdit = new QLineEdit(&dialog);
    nameEdit->setMaxLength(40);
    nameEdit->setPlaceholderText(QStringLiteral("room-name"));
    auto *filter = new QLineEdit(&dialog);
    filter->setPlaceholderText(QStringLiteral("Search people to invite"));
    filter->setClearButtonEnabled(true);
    auto *people = new QListWidget(&dialog);
    people->setMinimumHeight(200);
    people->setSelectionMode(QAbstractItemView::NoSelection);
    for (const QString &account : chatPickerAccounts()) {
        auto *item = new QListWidgetItem(QStringLiteral("@") + account, people);
        item->setData(Qt::UserRole, account);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Unchecked);
    }
    auto *audience = new QLabel(&dialog);
    audience->setObjectName(QStringLiteral("newPrivateRoomAudience"));
    audience->setWordWrap(true);
    auto *error = new QLabel(&dialog);
    error->setStyleSheet(Theme::remapSheet(QStringLiteral("color:#f85149;")));
    error->setWordWrap(true);
    error->hide();
    auto invitees = [people] {
        QStringList chosen;
        for (int row = 0; row < people->count(); ++row)
            if (people->item(row)->checkState() == Qt::Checked)
                chosen.append(people->item(row)->data(Qt::UserRole).toString());
        return chosen;
    };
    auto describe = [audience, invitees] {
        const QStringList chosen = invitees();
        QStringList handles;
        for (const QString &name : chosen)
            handles.append(QStringLiteral("@") + name);
        audience->setText(
            chosen.isEmpty()
                ? QStringLiteral("Only you can see this room until you invite "
                                 "someone. Nobody else, including "
                                 "administrators, is added.")
                : QStringLiteral("Only you and %1 can see, join or read this "
                                 "room. Nobody else, including administrators, "
                                 "is added.")
                      .arg(handles.join(QStringLiteral(", "))));
    };
    describe();
    connect(people, &QListWidget::itemChanged, &dialog,
            [describe](QListWidgetItem *) { describe(); });
    connect(filter, &QLineEdit::textChanged, &dialog,
            [people](const QString &text) {
                const QString needle =
                    text.trimmed().toLower().remove(QLatin1Char('@'));
                for (int row = 0; row < people->count(); ++row) {
                    QListWidgetItem *item = people->item(row);
                    item->setHidden(
                        !needle.isEmpty() &&
                        !item->data(Qt::UserRole).toString().contains(needle));
                }
            });
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)
        ->setText(QStringLiteral("Create private room"));
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog,
            [this, &dialog, nameEdit, error, invitees] {
                QString name = nameEdit->text().trimmed().toLower();
                if (name.startsWith(QLatin1Char('#')))
                    name.remove(0, 1);
                static const QRegularExpression valid(QStringLiteral(
                    "\\A[a-z0-9](?:[a-z0-9-]{0,38}[a-z0-9])?\\z"));
                if (!valid.match(name).hasMatch() ||
                    !m_officeChannelMirror ||
                    !m_officeChannelMirror->createChannel(name, true,
                                                          invitees())) {
                    error->setText(QStringLiteral(
                        "Use lowercase letters, numbers and hyphens for the "
                        "room name."));
                    error->show();
                    return;
                }
                dialog.accept();
            });
    auto *form = new QFormLayout;
    form->addRow(QStringLiteral("Room name"), nameEdit);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addLayout(form);
    layout->addWidget(new QLabel(QStringLiteral("Invite people"), &dialog));
    layout->addWidget(filter);
    layout->addWidget(people, 1);
    layout->addWidget(audience);
    layout->addWidget(error);
    layout->addWidget(buttons);
    nameEdit->setFocus();
    dialog.exec();
}

void MainWindow::showNewMessageDialog()
{
    const QStringList chosen = pickChatPeople(
        QStringLiteral("New message"),
        QStringLiteral("Choose who to message. Only the two of you can read "
                       "the conversation."),
        false, {});
    if (!chosen.isEmpty())
        startDirectMessageWith(chosen.first());
}

void MainWindow::startDirectMessageWith(const QString &account,
                                        const QString &peerId,
                                        const QString &peerName)
{
    const QString name = account.trimmed().toLower();
    if (!name.isEmpty() && m_officeChannelMirror &&
        m_officeChannelMirror->isActive() &&
        m_officeChannelMirror->startDirectMessage(name))
        return;
    if (!peerId.isEmpty()) {
        openDirectChat(peerId, peerName.isEmpty() ? name : peerName);
        return;
    }
    QMessageBox::information(
        this, QStringLiteral("New message"),
        QStringLiteral("Direct messages are kept by the ForkMesh relay. Sign "
                       "in with your account key to message @%1.")
            .arg(name));
}

void MainWindow::promptInviteToPrivateChannel()
{
    const QString conversation = m_currentConversation;
    if (!m_officeChannelMirror ||
        !m_officeChannelMirror->isPrivateConversation(conversation) ||
        forkmesh::office::isDirectOfficeConversation(conversation) ||
        !m_officeChannelMirror->canManageConversation(conversation)) {
        QMessageBox::information(
            this, QStringLiteral("Invite"),
            QStringLiteral("Only the creator of a private room can invite "
                           "people to it."));
        return;
    }
    const QStringList members =
        m_officeChannelMirror->membersForConversation(conversation);
    const QStringList chosen = pickChatPeople(
        QStringLiteral("Invite to %1").arg(conversation),
        QStringLiteral("The people you pick can see, join and read this room, "
                       "including its earlier messages."),
        true, members);
    for (const QString &account : chosen)
        m_officeChannelMirror->addMember(conversation, account);
}

void MainWindow::startDirectCallOnWeb(const QString &media)
{
    const QString peer =
        forkmesh::office::directPeerForConversation(m_currentConversation).trimmed();
    if (peer.isEmpty())
        return;
    QUrl url = catalogApiUrl();
    url.setPath(QStringLiteral("/chat"));
    url.setFragment(QString());
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("dm"), peer);
    query.addQueryItem(QStringLiteral("call"), media);
    url.setQuery(query);
    QDesktopServices::openUrl(url);
}

// The chat header only offers this on an office channel this account manages;
// the relay re-checks that on every write regardless.
void MainWindow::refreshChannelSettingsButton()
{
    if (!m_channelSettingsButton)
        return;
    m_channelSettingsButton->setVisible(
        !m_currentConversation.isEmpty() &&
        !isDirectConversation(m_currentConversation) &&
        !forkmesh::office::isDirectOfficeConversation(m_currentConversation));
    const bool directCall =
        forkmesh::office::isDirectOfficeConversation(m_currentConversation) &&
        !forkmesh::office::directPeerForConversation(m_currentConversation)
             .trimmed()
             .isEmpty();
    if (m_callAudioButton)
        m_callAudioButton->setVisible(directCall);
    if (m_callVideoButton)
        m_callVideoButton->setVisible(directCall);
    if (m_inviteButton)
        m_inviteButton->setVisible(
            m_officeChannelMirror &&
            m_officeChannelMirror->isPrivateConversation(m_currentConversation) &&
            !forkmesh::office::isDirectOfficeConversation(m_currentConversation) &&
            m_officeChannelMirror->canManageConversation(m_currentConversation));
    if (!m_channelSettingsDialog ||
        !m_channelSettingsDialog->property("manage").toBool())
        return;
    const QString followed =
        m_channelSettingsDialog->property("conversation").toString();
    if (!m_officeChannelMirror ||
        !m_officeChannelMirror->canManageConversation(followed))
        m_channelSettingsDialog->close();
}

void MainWindow::refreshChatComposerAccess()
{
    if (!m_messageInput)
        return;
    const bool readOnly =
        m_officeChannelMirror &&
        m_officeChannelMirror->isReadOnlyConversation(m_currentConversation);
    if (readOnly) {
        m_messageInput->setReadOnly(true);
        m_messageInput->setPlaceholderText(
            QStringLiteral("Server notices are read-only"));
    } else if (m_messageInput->isReadOnly() &&
               m_messageInput->placeholderText() ==
                   QLatin1String("Server notices are read-only")) {
        m_messageInput->setReadOnly(false);
        m_messageInput->setPlaceholderText(
            QStringLiteral("Message %1")
                .arg(chatConversationLabel(m_currentConversation).left(120)));
    }
}

// Public #channels intentionally use the controller-readable room layer for
// integrations. Private channels, DMs, and private Office conversations add
// the automatic X25519 device layer.
void MainWindow::refreshRoomSealButton()
{
    if (!m_roomSealButton)
        return;
    // Discord conversations never travel through the ForkMesh room, so this
    // badge would claim something it does not do there.
    const bool roomConversation =
        m_backend && !isDiscordConversation(m_currentConversation);
    m_roomSealButton->setVisible(roomConversation);
    if (!roomConversation)
        return;
    if (m_officeChannelMirror &&
        m_officeChannelMirror->isServerVisibleConversation(
            m_currentConversation)) {
        m_roomSealButton->setText(QStringLiteral("Server-visible"));
        setOcticon(m_roomSealButton, "server", 14);
        m_roomSealButton->setToolTip(
            QStringLiteral("ForkMesh writes these notices itself, so the "
                           "server can read them. Only you can see this "
                           "channel"));
        return;
    }
    const bool publicChannel =
        m_currentConversation.startsWith(QLatin1Char('#')) &&
        !m_privateChannels.contains(m_currentConversation) &&
        (!isOfficeConversation(m_currentConversation) ||
         (m_officeChannelMirror &&
          !m_officeChannelMirror->isPrivateConversation(m_currentConversation)));
    if (publicChannel) {
        m_roomSealButton->setText(QStringLiteral("Public"));
        setOcticon(m_roomSealButton, "unlock", 14);
        m_roomSealButton->setToolTip(
            QStringLiteral("Controller-readable public channel; ForkBot, MCP, "
                           "and webhooks can participate"));
        return;
    }
    const QString keyId = m_backend->roomSealKeyId();
    m_roomSealButton->setText(QStringLiteral("End-to-end"));
    setOcticon(m_roomSealButton, "shield-check", 14);
    m_roomSealButton->setToolTip(
        keyId.isEmpty()
            ? QStringLiteral("Automatic end-to-end encryption is unavailable")
            : QStringLiteral("Automatic device-to-device encryption (device %1); "
                             "the controller key cannot unwrap message keys")
                  .arg(keyId));
}

void MainWindow::promptRoomSeal()
{
    if (!m_backend)
        return;
    if (m_officeChannelMirror &&
        m_officeChannelMirror->isServerVisibleConversation(
            m_currentConversation)) {
        QMessageBox::information(
            this, QStringLiteral("Server-visible channel"),
            QStringLiteral(
                "ForkMesh writes these notices itself, so they are not "
                "end-to-end encrypted: the server can read them. Only you can "
                "see this channel, and it is read-only."));
        return;
    }
    const bool publicChannel =
        m_currentConversation.startsWith(QLatin1Char('#')) &&
        !m_privateChannels.contains(m_currentConversation) &&
        (!isOfficeConversation(m_currentConversation) ||
         (m_officeChannelMirror &&
          !m_officeChannelMirror->isPrivateConversation(m_currentConversation)));
    if (publicChannel) {
        QMessageBox::information(
            this, QStringLiteral("Public channel"),
            QStringLiteral(
                "This channel is public and controller-readable. It still uses "
                "authenticated AES-256-GCM transport, while allowing ForkBot, "
                "MCP, and incoming webhooks to read and post messages.\n\n"
                "Private channels and direct messages add automatic "
                "device-to-device encryption beyond the controller key."));
        return;
    }
    QMessageBox::information(
        this, QStringLiteral("End-to-end encryption"),
        QStringLiteral(
            "Encryption is automatic. This device created a private X25519 key "
            "locally and sends only its public key. Each message uses a fresh "
            "key wrapped for known room devices, so the controller's room key "
            "alone can forward and retain it but cannot decrypt it.\n\n"
            "A new device can read messages addressed after its first visit; "
            "it cannot retroactively open older history. ForkBot runs on the "
            "relay and is unavailable in end-to-end encrypted rooms."));
}

// A rename keeps the room, so everything this window filed under the old name
// has to move with it instead of stranding the history (and the unread count)
// on a conversation nothing will ever mention again.
void MainWindow::renameChatConversation(const QString &oldConversation,
                                        const QString &newConversation)
{
    if (oldConversation.isEmpty() || newConversation.isEmpty() ||
        oldConversation == newConversation)
        return;
    if (m_history.contains(oldConversation)) {
        QList<ChatMessage> moved = m_history.take(oldConversation);
        for (ChatMessage &message : moved)
            message.conversation = newConversation;
        m_history[newConversation] += moved;
        std::stable_sort(m_history[newConversation].begin(),
                         m_history[newConversation].end(),
                         [](const ChatMessage &left, const ChatMessage &right) {
                             return left.timestampMs < right.timestampMs;
                         });
    }
    if (m_unread.remove(oldConversation))
        m_unread.insert(newConversation);
    if (m_unreadCounts.contains(oldConversation))
        m_unreadCounts.insert(newConversation,
                              m_unreadCounts.take(oldConversation));
    if (m_typing.contains(oldConversation))
        m_typing.insert(newConversation, m_typing.take(oldConversation));
    if (m_currentConversation == oldConversation) {
        m_currentConversation = newConversation;
        updateChatConversationHeading();
        if (m_messageInput)
            m_messageInput->setPlaceholderText(
                QStringLiteral("Message %1")
                    .arg(chatConversationLabel(newConversation).left(120)));
        rebuildConversationView();
    }
    scheduleChatSave();
    refreshChannelSettingsButton();
}

void MainWindow::openChannelSettings()
{
    openChannelSettingsFor(m_currentConversation);
}

void MainWindow::openChannelSettingsFor(const QString &target)
{
    if (target.isEmpty() || isDirectConversation(target))
        return;
    if (isDiscordConversation(target)) {
        showDiscordConnectionDialog();
        return;
    }
    if (!isOfficeConversation(target)) {
        showMeshChannelSettings(target);
        return;
    }
    if (!m_officeChannelMirror ||
        !m_officeChannelMirror->conversations().contains(target))
        return;
    if (m_channelSettingsDialog) {
        if (m_channelSettingsDialog->property("conversation").toString() ==
            target) {
            m_channelSettingsDialog->raise();
            m_channelSettingsDialog->activateWindow();
            return;
        }
        m_channelSettingsDialog->close();
    }
    const bool manage = m_officeChannelMirror->canManageConversation(target);
    const bool serverVisible =
        m_officeChannelMirror->isServerVisibleConversation(target);
    // The dialog follows one conversation. A rename moves it (the room is the
    // same), a deletion or a lost manager role closes it. The shared string
    // outlives this scope with the lambdas that read it.
    auto conversation = std::make_shared<QString>(target);

    auto *dialog = new QDialog(this);
    m_channelSettingsDialog = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setProperty("conversation", target);
    dialog->setProperty("manage", manage);
    dialog->setWindowTitle(QStringLiteral("Channel settings"));
    dialog->setMinimumWidth(520);

    auto *nameEdit = new QLineEdit(dialog);
    nameEdit->setMaxLength(40);
    nameEdit->setPlaceholderText(QStringLiteral("release-team"));
    auto *topicEdit = new QLineEdit(dialog);
    topicEdit->setMaxLength(200);
    topicEdit->setPlaceholderText(QStringLiteral("What this channel is for"));
    auto *visibilityBox = new QComboBox(dialog);
    visibilityBox->addItem(QStringLiteral("Private — invited, end-to-end"),
                           QStringLiteral("private"));
    visibilityBox->addItem(QStringLiteral("Public — integrations enabled"),
                           QStringLiteral("public"));
    if (serverVisible)
        visibilityBox->addItem(
            QStringLiteral("Server-visible — only you, written by ForkMesh"),
            QStringLiteral("notices"));

    auto *form = new QFormLayout;
    form->addRow(QStringLiteral("Channel name"), nameEdit);
    form->addRow(QStringLiteral("Topic"), topicEdit);
    form->addRow(QStringLiteral("Visibility"), visibilityBox);

    auto *rotateNote = new QLabel(
        QStringLiteral("Closing a public channel rotates its key, so earlier "
                       "messages stay behind in the old room."),
        dialog);
    rotateNote->setWordWrap(true);
    rotateNote->setStyleSheet(Theme::remapSheet(QStringLiteral("color:#8b949e; font-size:11px;")));

    auto *saveButton = new QPushButton(QStringLiteral("Save changes"), dialog);
    saveButton->setObjectName(QStringLiteral("primaryButton"));
    saveButton->setCursor(Qt::PointingHandCursor);

    auto *webhookList = new QListWidget(dialog);
    // Ten is the relay's per-channel limit, so the list never needs to grow
    // past a few rows and swallow the rest of the dialog.
    webhookList->setMinimumHeight(96);
    webhookList->setMaximumHeight(150);
    webhookList->setSelectionMode(QAbstractItemView::SingleSelection);
    auto *webhookUrl = new QLineEdit(dialog);
    webhookUrl->setReadOnly(true);
    webhookUrl->setPlaceholderText(
        QStringLiteral("Select a webhook to see its URL"));
    auto *addWebhook = new QPushButton(QStringLiteral("Add webhook"), dialog);
    auto *rotateWebhook = new QPushButton(QStringLiteral("New URL"), dialog);
    auto *copyWebhook = new QPushButton(QStringLiteral("Copy URL"), dialog);
    auto *removeWebhook = new QPushButton(QStringLiteral("Remove"), dialog);
    for (QPushButton *button :
         {addWebhook, rotateWebhook, copyWebhook, removeWebhook}) {
        button->setObjectName(QStringLiteral("ghostButton"));
        button->setCursor(Qt::PointingHandCursor);
    }
    rotateWebhook->setToolTip(
        QStringLiteral("Replace this webhook's URL and show the new one"));

    auto *deleteButton =
        new QPushButton(QStringLiteral("Delete channel"), dialog);
    deleteButton->setObjectName(QStringLiteral("dangerButton"));
    deleteButton->setCursor(Qt::PointingHandCursor);

    auto *errorLabel = new QLabel(dialog);
    errorLabel->setStyleSheet(Theme::remapSheet(QStringLiteral("color:#f85149;")));
    errorLabel->setWordWrap(true);
    errorLabel->hide();

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);

    auto *webhookButtons = new QHBoxLayout;
    webhookButtons->setContentsMargins(0, 0, 0, 0);
    webhookButtons->addWidget(addWebhook);
    webhookButtons->addWidget(rotateWebhook);
    webhookButtons->addWidget(copyWebhook);
    webhookButtons->addWidget(removeWebhook);
    webhookButtons->addStretch();

    auto *saveRow = new QHBoxLayout;
    saveRow->setContentsMargins(0, 0, 0, 0);
    saveRow->addWidget(saveButton);
    saveRow->addStretch();

    auto *webhookHeading = new QLabel(
        QStringLiteral("<b>Incoming webhooks</b>"), dialog);
    auto *webhookNote = new QLabel(
        QStringLiteral("Public channels can accept controller integrations. "
                       "POST JSON with a text, content, or message field. A "
                       "URL is shown only once when it is issued."),
        dialog);
    webhookNote->setWordWrap(true);
    webhookNote->setStyleSheet(Theme::remapSheet(QStringLiteral("color:#8b949e; font-size:11px;")));

    auto *membersHeading = new QLabel(
        QStringLiteral("<b>Who can see this room</b>"), dialog);
    auto *membersList = new QListWidget(dialog);
    membersList->setObjectName(QStringLiteral("channelMembersList"));
    membersList->setMinimumHeight(72);
    membersList->setMaximumHeight(150);
    membersList->setSelectionMode(QAbstractItemView::SingleSelection);
    auto *inviteMembers =
        new QPushButton(QString::fromUtf8("Invite people\xE2\x80\xA6"), dialog);
    auto *removeMember = new QPushButton(QStringLiteral("Remove"), dialog);
    for (QPushButton *button : {inviteMembers, removeMember}) {
        button->setObjectName(QStringLiteral("ghostButton"));
        button->setCursor(Qt::PointingHandCursor);
    }
    auto *memberButtons = new QHBoxLayout;
    memberButtons->setContentsMargins(0, 0, 0, 0);
    memberButtons->addWidget(inviteMembers);
    memberButtons->addWidget(removeMember);
    memberButtons->addStretch();

    auto *layout = new QVBoxLayout(dialog);
    layout->addLayout(form);
    layout->addWidget(rotateNote);
    layout->addLayout(saveRow);
    layout->addSpacing(6);
    layout->addWidget(membersHeading);
    layout->addWidget(membersList);
    layout->addLayout(memberButtons);
    layout->addSpacing(6);
    layout->addWidget(webhookHeading);
    layout->addWidget(webhookNote);
    layout->addWidget(webhookList);
    layout->addWidget(webhookUrl);
    layout->addLayout(webhookButtons);
    layout->addSpacing(6);
    auto *dangerRow = new QHBoxLayout;
    dangerRow->setContentsMargins(0, 0, 0, 0);
    dangerRow->addWidget(deleteButton);
    dangerRow->addStretch();
    layout->addLayout(dangerRow);
    layout->addWidget(errorLabel);
    layout->addWidget(buttons);

    if (!manage) {
        nameEdit->setReadOnly(true);
        topicEdit->setReadOnly(true);
        visibilityBox->setEnabled(false);
        rotateNote->setText(
            serverVisible
                ? QStringLiteral(
                      "ForkMesh's mail server posts here like an incoming "
                      "webhook: one line for every email it sends, then "
                      "delivered, bounced or rejected as the receiving server "
                      "answers. The server writes these notes and can read "
                      "them; only you can see this channel. It cannot be "
                      "renamed, shared, given webhooks or deleted.")
            : m_officeChannelMirror->isEveryoneConversation(target)
                ? QStringLiteral(
                      "Every ForkMesh member is in this channel, and new "
                      "members join it when they sign up. It cannot be "
                      "renamed, left, made private or deleted.")
                : QStringLiteral("Only the channel creator or an "
                                 "administrator can change this channel."));
        for (QWidget *widget :
             std::initializer_list<QWidget *>{saveButton, webhookHeading,
                                              webhookNote, webhookList,
                                              webhookUrl, addWebhook,
                                              rotateWebhook, copyWebhook,
                                              removeWebhook, deleteButton})
            widget->hide();
    }

    auto showError = [errorLabel](const QString &message) {
        errorLabel->setText(message);
        errorLabel->setVisible(!message.isEmpty());
    };
    auto syncWebhookPolicy = [visibilityBox, addWebhook, rotateWebhook,
                              webhookNote] {
        const bool enabled = visibilityBox->currentData().toString() ==
                             QLatin1String("public");
        addWebhook->setEnabled(enabled);
        rotateWebhook->setEnabled(enabled);
        webhookNote->setText(
            enabled
                ? QStringLiteral("Public channels can accept controller "
                                 "integrations. POST JSON with a text, content, "
                                 "or message field. A URL is shown only once.")
                : QStringLiteral("Incoming webhooks are disabled while this "
                                 "channel is private and end-to-end encrypted."));
    };
    connect(visibilityBox, &QComboBox::currentIndexChanged, dialog,
            [syncWebhookPolicy](int) { syncWebhookPolicy(); });
    auto loadFields = [this, conversation, nameEdit, topicEdit, visibilityBox,
                       dialog, syncWebhookPolicy] {
        if (!m_officeChannelMirror)
            return;
        const QString name =
            m_officeChannelMirror->channelNameForConversation(*conversation);
        if (!name.isEmpty())
            nameEdit->setText(name);
        topicEdit->setText(
            m_officeChannelMirror->topicForConversation(*conversation));
        visibilityBox->setCurrentIndex(
            m_officeChannelMirror->isServerVisibleConversation(*conversation)
                ? visibilityBox->findData(QStringLiteral("notices"))
            : m_officeChannelMirror->isPrivateConversation(*conversation)
                ? 0
                : 1);
        syncWebhookPolicy();
        dialog->setProperty("conversation", *conversation);
        dialog->setWindowTitle(QStringLiteral("Channel settings — %1")
                                   .arg(chatConversationLabel(*conversation)));
    };
    loadFields();

    auto loadMembers = [this, conversation, manage, serverVisible,
                        membersHeading, membersList, inviteMembers,
                        removeMember] {
        const bool privateRoom =
            m_officeChannelMirror && !serverVisible &&
            m_officeChannelMirror->isPrivateConversation(*conversation);
        membersHeading->setVisible(privateRoom);
        membersList->setVisible(privateRoom);
        inviteMembers->setVisible(privateRoom && manage);
        removeMember->setVisible(privateRoom && manage);
        membersList->clear();
        if (!privateRoom)
            return;
        const QStringList members =
            m_officeChannelMirror->membersForConversation(*conversation);
        for (int index = 0; index < members.size(); ++index) {
            auto *item = new QListWidgetItem(
                QStringLiteral("@") + members.at(index) +
                    (index == 0 ? QStringLiteral("  (creator)") : QString()),
                membersList);
            item->setData(Qt::UserRole, members.at(index));
            item->setData(Qt::UserRole + 1, index == 0);
        }
        membersHeading->setText(
            QStringLiteral("<b>Who can see this room</b> — only these %1 "
                           "people; nobody else, including administrators")
                .arg(members.size()));
    };
    loadMembers();
    connect(m_officeChannelMirror, &OfficeChannelMirror::roomMembersChanged,
            dialog, [conversation, loadMembers](const QString &changed,
                                                const QStringList &) {
                if (changed == *conversation)
                    loadMembers();
            });
    connect(inviteMembers, &QPushButton::clicked, dialog,
            [this, conversation] {
                if (!m_officeChannelMirror)
                    return;
                const QStringList chosen = pickChatPeople(
                    QStringLiteral("Invite to %1").arg(*conversation),
                    QStringLiteral("The people you pick can see, join and "
                                   "read this room, including its earlier "
                                   "messages."),
                    true,
                    m_officeChannelMirror->membersForConversation(
                        *conversation));
                for (const QString &account : chosen)
                    m_officeChannelMirror->addMember(*conversation, account);
            });
    connect(removeMember, &QPushButton::clicked, dialog,
            [this, conversation, membersList, dialog] {
                const QListWidgetItem *item = membersList->currentItem();
                if (!item || item->data(Qt::UserRole + 1).toBool() ||
                    !m_officeChannelMirror)
                    return;
                const QString account = item->data(Qt::UserRole).toString();
                if (QMessageBox::question(
                        dialog, QStringLiteral("Remove from room"),
                        QStringLiteral("Remove @%1? They lose access at once "
                                       "and the room's key is rotated.")
                            .arg(account)) != QMessageBox::Yes)
                    return;
                m_officeChannelMirror->removeMember(*conversation, account);
            });

    auto selectedWebhook = [webhookList]() -> QString {
        const QListWidgetItem *item = webhookList->currentItem();
        return item ? item->data(Qt::UserRole).toString() : QString();
    };
    auto showSelectedUrl = [webhookList, webhookUrl, copyWebhook] {
        const QListWidgetItem *item = webhookList->currentItem();
        const QString url =
            item ? item->data(Qt::UserRole + 1).toString() : QString();
        webhookUrl->setText(url);
        // Show the start of the URL, not the tail the cursor lands on.
        webhookUrl->setCursorPosition(0);
        webhookUrl->setPlaceholderText(
            item && url.isEmpty()
                ? QStringLiteral("URL hidden — the relay keeps no copy. Use "
                                 "New URL to issue a working one.")
                : QStringLiteral("Select a webhook to see its URL"));
        copyWebhook->setEnabled(!url.isEmpty());
    };
    connect(webhookList, &QListWidget::currentRowChanged, dialog,
            [showSelectedUrl](int) { showSelectedUrl(); });
    showSelectedUrl();

    connect(saveButton, &QPushButton::clicked, dialog,
            [this, conversation, nameEdit, topicEdit, visibilityBox,
             showError] {
                showError(QString());
                if (!m_officeChannelMirror)
                    return;
                if (!m_officeChannelMirror->updateChannel(
                        *conversation, nameEdit->text(), topicEdit->text(),
                        visibilityBox->currentData().toString() ==
                            QLatin1String("private")))
                    showError(QStringLiteral(
                        "Enter a channel name of lowercase letters, numbers, "
                        "and hyphens."));
            });
    connect(addWebhook, &QPushButton::clicked, dialog,
            [this, conversation, dialog, showError] {
                bool accepted = false;
                const QString name = QInputDialog::getText(
                    dialog, QStringLiteral("Add webhook"),
                    QStringLiteral("Webhook name"), QLineEdit::Normal,
                    QStringLiteral("Deploy bot"), &accepted);
                if (!accepted || !m_officeChannelMirror)
                    return;
                showError(QString());
                if (!m_officeChannelMirror->createWebhook(*conversation, name))
                    showError(QStringLiteral("Give the webhook a name."));
            });
    connect(rotateWebhook, &QPushButton::clicked, dialog,
            [this, conversation, selectedWebhook, showError] {
                const QString id = selectedWebhook();
                if (id.isEmpty() || !m_officeChannelMirror) {
                    showError(QStringLiteral("Select a webhook first."));
                    return;
                }
                showError(QString());
                m_officeChannelMirror->rotateWebhook(*conversation, id);
            });
    connect(copyWebhook, &QPushButton::clicked, dialog, [webhookUrl] {
        if (!webhookUrl->text().isEmpty())
            QGuiApplication::clipboard()->setText(webhookUrl->text());
    });
    connect(removeWebhook, &QPushButton::clicked, dialog,
            [this, conversation, dialog, selectedWebhook, showError] {
                const QString id = selectedWebhook();
                if (id.isEmpty() || !m_officeChannelMirror) {
                    showError(QStringLiteral("Select a webhook first."));
                    return;
                }
                if (QMessageBox::question(
                        dialog, QStringLiteral("Remove webhook"),
                        QStringLiteral("Remove this webhook? Anything posting "
                                       "to its URL stops working.")) !=
                    QMessageBox::Yes)
                    return;
                showError(QString());
                m_officeChannelMirror->deleteWebhook(*conversation, id);
            });
    connect(deleteButton, &QPushButton::clicked, dialog,
            [this, conversation, dialog, showError] {
                if (QMessageBox::question(
                        dialog, QStringLiteral("Delete channel"),
                        QStringLiteral("Delete %1 for everyone? Its "
                                       "memberships, webhooks, and retained "
                                       "history go with it. This cannot be "
                                       "undone.")
                            .arg(*conversation)) != QMessageBox::Yes)
                    return;
                showError(QString());
                if (m_officeChannelMirror)
                    m_officeChannelMirror->deleteChannel(*conversation);
            });

    if (m_officeChannelMirror) {
        connect(m_officeChannelMirror,
                &OfficeChannelMirror::channelWebhooksChanged, dialog,
                [conversation, webhookList, showSelectedUrl](
                    const QString &changed,
                    const QVector<forkmesh::office::ChannelWebhook> &webhooks) {
                    if (changed != *conversation)
                        return;
                    const QString selected =
                        webhookList->currentItem()
                            ? webhookList->currentItem()
                                  ->data(Qt::UserRole)
                                  .toString()
                            : QString();
                    webhookList->clear();
                    for (const forkmesh::office::ChannelWebhook &webhook :
                         webhooks) {
                        const QString used =
                            webhook.lastUsedAt > 0
                                ? QDateTime::fromMSecsSinceEpoch(
                                      webhook.lastUsedAt)
                                      .toString(QStringLiteral("yyyy-MM-dd hh:mm"))
                                : QStringLiteral("not used yet");
                        auto *item = new QListWidgetItem(
                            QStringLiteral("%1 — %2").arg(webhook.name, used),
                            webhookList);
                        item->setData(Qt::UserRole, webhook.id);
                        item->setData(Qt::UserRole + 1, webhook.url);
                        if (webhook.id == selected)
                            webhookList->setCurrentItem(item);
                    }
                    if (!webhookList->currentItem() && webhookList->count())
                        webhookList->setCurrentRow(0);
                    showSelectedUrl();
                });
        connect(m_officeChannelMirror,
                &OfficeChannelMirror::channelSettingsFailed, dialog,
                [conversation, showError](const QString &failed,
                                          const QString &reason) {
                    if (failed == *conversation)
                        showError(reason);
                });
        connect(m_officeChannelMirror,
                &OfficeChannelMirror::channelSettingsSaved, dialog,
                [conversation, loadFields, showError](const QString &saved) {
                    if (saved != *conversation)
                        return;
                    showError(QString());
                    loadFields();
                });
        connect(m_officeChannelMirror,
                &OfficeChannelMirror::conversationRenamed, dialog,
                [conversation, loadFields](const QString &oldConversation,
                                           const QString &newConversation) {
                    if (*conversation != oldConversation)
                        return;
                    *conversation = newConversation;
                    loadFields();
                });
        connect(m_officeChannelMirror, &OfficeChannelMirror::channelDeleted,
                dialog, [conversation, dialog](const QString &deleted) {
                    if (deleted == *conversation)
                        dialog->close();
                });
        if (manage)
            m_officeChannelMirror->refreshWebhooks(*conversation);
    }

    dialog->show();
}

void MainWindow::showMeshChannelSettings(const QString &channel)
{
    if (m_channelSettingsDialog)
        m_channelSettingsDialog->close();
    const bool localMesh = isLocalMeshConversation(channel);
    const bool privateRoom = m_privateChannels.contains(channel);

    auto *dialog = new QDialog(this);
    m_channelSettingsDialog = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setProperty("conversation", channel);
    dialog->setProperty("manage", false);
    dialog->setWindowTitle(QStringLiteral("Channel settings — %1")
                               .arg(chatConversationLabel(channel)));
    dialog->setMinimumWidth(460);

    auto *nameLabel = new QLabel(chatConversationLabel(channel), dialog);
    nameLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *encryptionLabel = new QLabel(
        localMesh ? QStringLiteral("Local network — end-to-end encrypted, "
                                   "never sent to the internet")
        : privateRoom ? QStringLiteral("Private — invited, end-to-end")
                      : QStringLiteral("Public — integrations enabled"),
        dialog);
    encryptionLabel->setWordWrap(true);
    auto *form = new QFormLayout;
    form->addRow(QStringLiteral("Channel"), nameLabel);
    form->addRow(QStringLiteral("Visibility"), encryptionLabel);

    auto *note = new QLabel(
        localMesh ? QStringLiteral("Everyone running ForkMesh on this network "
                                   "joins this room automatically.")
                  : QStringLiteral("This room lives on your mesh devices. "
                                   "Deleting it removes it from this device "
                                   "only; other people keep it."),
        dialog);
    note->setWordWrap(true);
    note->setStyleSheet(Theme::remapSheet(QStringLiteral("color:#8b949e; font-size:11px;")));

    auto *actions = new QHBoxLayout;
    actions->setContentsMargins(0, 0, 0, 0);
    if (privateRoom) {
        auto *invite = new QPushButton(QStringLiteral("Invite…"), dialog);
        invite->setObjectName(QStringLiteral("ghostButton"));
        invite->setCursor(Qt::PointingHandCursor);
        connect(invite, &QPushButton::clicked, this, [this, channel] {
            if (m_currentConversation != channel)
                switchConversation(channel);
            promptInviteToPrivateChannel();
        });
        actions->addWidget(invite);
    }
    if (!localMesh) {
        auto *remove = new QPushButton(QStringLiteral("Delete room"), dialog);
        remove->setObjectName(QStringLiteral("dangerButton"));
        remove->setCursor(Qt::PointingHandCursor);
        connect(remove, &QPushButton::clicked, dialog, [this, dialog, channel] {
            dialog->close();
            promptDeleteRoom(channel);
        });
        actions->addWidget(remove);
    }
    actions->addStretch();

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);

    auto *layout = new QVBoxLayout(dialog);
    layout->addLayout(form);
    layout->addWidget(note);
    layout->addLayout(actions);
    layout->addWidget(buttons);
    dialog->show();
}

void MainWindow::promptDeleteRoom(const QString &channel)
{
    if (!m_backend || channel.isEmpty() || isDirectConversation(channel))
        return;
    const bool priv = m_privateChannels.contains(channel);
    const QString what = priv ? QStringLiteral("private room")
                              : QStringLiteral("channel");
    const auto choice = QMessageBox::question(
        this, QStringLiteral("Delete room"),
        QStringLiteral("Delete the %1 %2?\n\nIt's removed from this device only — "
                       "other people keep it. You won't see new messages in it "
                       "unless you re-create or are re-invited to it.")
            .arg(what, channel),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (choice != QMessageBox::Yes)
        return;

    m_backend->removeChannel(channel);
    m_privateChannels.remove(channel);
    persistPrivateChannels();
    m_unread.remove(channel);
    m_unreadCounts.remove(channel);
    if (m_currentConversation == channel) {
        m_currentConversation.clear();
        const QString fallback = m_channels.value(0);
        if (!fallback.isEmpty())
            switchConversation(fallback);
        else
            m_channelTitle->setText(QString());
    }
    scheduleChatSave();
    logSystem(QStringLiteral("Deleted %1 %2 (this device only).").arg(what, channel));
}

void MainWindow::restorePrivateChannels()
{
    if (!m_backend)
        return;
    QSettings settings;
    const QString key = privateChannelsSettingsKey();
    if (!settings.contains(key) &&
        settings.contains(QStringLiteral("chat/privateChannels"))) {
        settings.setValue(
            key,
            settings.value(QStringLiteral("chat/privateChannels")).toStringList());
        settings.remove(QStringLiteral("chat/privateChannels"));
    }
    const QStringList saved = settings.value(key).toStringList();
    {
        const QScopedValueRollback<bool> restoring(m_restoringPrivateChannels, true);
        for (const QString &name : saved) {
            const QString trimmed = name.trimmed();
            if (!trimmed.isEmpty())
                m_backend->createPrivateChannel(trimmed);
        }
    }
    if (!saved.isEmpty())
        persistPrivateChannels();
}

QSet<QString> MainWindow::savedPrivateChannels() const
{
    QSettings settings;
    QSet<QString> channels;
    const auto addAll = [&channels](const QStringList &names) {
        for (const QString &name : names) {
            const QString trimmed = name.trimmed();
            if (trimmed.isEmpty())
                continue;
            channels.insert(trimmed.startsWith(QLatin1Char('#'))
                                ? trimmed
                                : QLatin1Char('#') + trimmed);
        }
    };
    addAll(settings.value(QStringLiteral("chat/privateChannels")).toStringList());
    settings.beginGroup(QStringLiteral("chat/privateChannelsByInstance"));
    const QStringList instances = settings.childKeys();
    for (const QString &instance : instances)
        addAll(settings.value(instance).toStringList());
    settings.endGroup();
    return channels;
}

QString MainWindow::privateChannelsSettingsKey() const
{
    QString instance = activeServerInstanceId();
    if (instance.isEmpty())
        instance = QStringLiteral("legacy");
    return QStringLiteral("chat/privateChannelsByInstance/") + instance;
}

void MainWindow::persistPrivateChannels()
{
    if (m_restoringPrivateChannels)
        return;
    QStringList list(m_privateChannels.constBegin(), m_privateChannels.constEnd());
    list.sort();
    forkmesh::settings::put(QSettings(), privateChannelsSettingsKey(), list);
}

void MainWindow::ensureTeamRoom()
{
    const bool added = !m_privateChannels.contains(QStringLiteral("#team"));
    m_privateChannels.insert(QStringLiteral("#team"));
    if (added) {
        persistPrivateChannels();
        refreshChannelList();
    }
    if (m_backend)
        m_backend->createPrivateChannel(QStringLiteral("team"));
}

bool MainWindow::isOfficeConversation(const QString &conversation) const
{
    return forkmesh::office::isOfficeConversation(conversation);
}

bool MainWindow::isPrivateChatConversation(const QString &conversation) const
{
    return isDirectConversation(conversation) ||
           conversation.startsWith(QLatin1String("direct:")) ||
           forkmesh::office::isDirectOfficeConversation(conversation) ||
           conversation == QLatin1String("#team") ||
           m_privateChannels.contains(conversation) ||
           (m_officeChannelMirror &&
            m_officeChannelMirror->isPrivateConversation(conversation));
}

void MainWindow::purgePrivateChatPings(const QSet<QString> &savedPrivate)
{
    const qsizetype removed = m_notifications.removeIf(
        [this, &savedPrivate](const AppNotification &item) {
            return item.link.kind == QLatin1String("chat") &&
                   (savedPrivate.contains(item.link.ref) ||
                    isPrivateChatConversation(item.link.ref));
        });
    if (removed == 0)
        return;
    updateNotificationButton();
    saveNotificationJournal();
    if (m_notificationsTable)
        refreshNotificationsTable();
}

void MainWindow::onPrivateChannelJoined(const QString &channel)
{
    m_privateChannels.insert(channel);
    persistPrivateChannels();
    refreshChannelList();
    purgePrivateChatPings();
}

bool MainWindow::isDiscordConversation(const QString &conversation) const
{
    return forkmesh::discord::isDiscordConversation(conversation);
}

void MainWindow::applyLocalMeshQueuedMark(MessageRow *row)
{
    if (!row)
        return;
    if (m_localMesh && m_localMesh->isQueued(row->messageId()))
        row->setDeliveryNote(
            QStringLiteral("queued"),
            QStringLiteral("No device has this yet. It waits on this "
                           "computer and goes to the first device that joins "
                           "#mesh."));
    else
        row->setDeliveryNote(QString(), QString());
}

bool MainWindow::isLocalMeshConversation(const QString &conversation) const
{
    return forkmesh::localmesh::isMeshConversation(conversation);
}

void MainWindow::sendCurrentMessage()
{
    const QString text = m_messageInput->text().trimmed();
    if (text.isEmpty() || m_currentConversation.isEmpty())
        return;
    static const QRegularExpression slashCommand(
        QStringLiteral("\\A/(issue|topic)(?:\\s+([\\s\\S]*))?\\z"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch slash = slashCommand.match(text);
    if (slash.hasMatch()) {
        m_messageInput->clear();
        sendTypingState(false);
        const QString command = slash.captured(1).toLower();
        const QString argument = slash.captured(2).trimmed();
        if (command == QLatin1String("issue")) {
            createIssueFromChatCommand(argument);
            return;
        }
        if (!isOfficeConversation(m_currentConversation) ||
            !m_officeChannelMirror) {
            logSystem(QStringLiteral(
                "Chat: /topic is available in managed Office channels."));
            return;
        }
        if (argument.size() > 200) {
            logSystem(QStringLiteral(
                "Chat: channel topics must be 200 characters or fewer."));
            return;
        }
        if (!m_officeChannelMirror->canManageConversation(
                m_currentConversation)) {
            logSystem(QStringLiteral(
                "Chat: only the channel creator or an administrator can change "
                "this channel topic."));
            return;
        }
        if (!m_officeChannelMirror->setTopic(m_currentConversation, argument))
            logSystem(QStringLiteral("Chat: the channel topic could not be changed."));
        return;
    }
    if (isOfficeConversation(m_currentConversation)) {
        if (!m_officeChannelMirror ||
            !m_officeChannelMirror->sendMessage(m_currentConversation, text)) {
            logSystem(QStringLiteral("Office chat: %1 is not currently writable.")
                          .arg(m_currentConversation));
            return;
        }
        triggerTeammatesForEvent(
            QStringLiteral("mentions"),
            QStringLiteral("Chat message in %1\n%2")
                .arg(m_currentConversation, text));
        maybeAskForkbot(m_currentConversation, text);
        m_messageInput->clear();
        return;
    }
    if (isLocalMeshConversation(m_currentConversation)) {
        if (!m_localMesh || m_localMesh->sendChat(text).isEmpty()) {
            logSystem(QStringLiteral("Local mesh: %1 is not open on this "
                                     "network right now.")
                          .arg(m_currentConversation));
            return;
        }
        sendTypingState(false);
        m_messageInput->clear();
        return;
    }
    if (isDiscordConversation(m_currentConversation)) {
        if (!m_discordClient ||
            !m_discordClient->sendMessage(m_currentConversation, text)) {
            logSystem(QStringLiteral("Discord: %1 is not currently writable.")
                          .arg(m_discordClient
                                   ? m_discordClient->displayName(
                                         m_currentConversation)
                                   : m_currentConversation));
            return;
        }
        triggerTeammatesForEvent(
            QStringLiteral("mentions"),
            QStringLiteral("Chat message in %1\n%2")
                .arg(m_currentConversation, text));
        maybeAskForkbot(m_currentConversation, text);
        sendTypingState(false);
        m_messageInput->clear();
        return;
    }
    if (!m_backend)
        return;
    sendTypingState(false);
    QString chatThreadRootId;
    if (isDirectConversation(m_currentConversation)) {
        m_backend->sendDirect(dmPeerId(m_currentConversation), text);
    } else {
        chatThreadRootId = m_backend->sendChat(m_currentConversation, text);
        maybeAskForkbot(m_currentConversation, text);
    }
    triggerTeammatesForEvent(
        QStringLiteral("mentions"),
        QStringLiteral("Chat message in %1\n%2")
            .arg(m_currentConversation, text), -1,
        m_currentConversation, chatThreadRootId);
    m_messageInput->clear();
}

void MainWindow::insertEmojiIntoComposer(const QString &emoji)
{
    if (!m_messageInput || emoji.isEmpty())
        return;
    m_messageInput->insert(emoji);
    m_messageInput->setFocus();
}

// The composer's link button: wrap the selected words in [label](target) and
// leave the caret on the half still to be written. A URL already on the
// clipboard fills the target in, since pasting the address and then describing
// it is the usual order.
void MainWindow::insertLinkIntoComposer()
{
    if (!m_messageInput)
        return;
    const int start = m_messageInput->selectionStart() < 0
                          ? m_messageInput->cursorPosition()
                          : m_messageInput->selectionStart();
    const int end = start + m_messageInput->selectedText().size();
    const ChatLinkText::Insertion insertion = ChatLinkText::linkInsertion(
        m_messageInput->text(), start, end,
        QApplication::clipboard() ? QApplication::clipboard()->text() : QString());
    if (insertion.text.size() > m_messageInput->maxLength())
        return;
    m_messageInput->setText(insertion.text);
    if (insertion.selectionLength > 0)
        m_messageInput->setSelection(insertion.selectionStart,
                                     insertion.selectionLength);
    else
        m_messageInput->setCursorPosition(insertion.selectionStart);
    m_messageInput->setFocus();
}

void MainWindow::wrapComposerSelection(const QString &before, const QString &after)
{
    if (!m_messageInput)
        return;
    const int start = m_messageInput->selectionStart() < 0
                          ? m_messageInput->cursorPosition()
                          : m_messageInput->selectionStart();
    const int end = start + m_messageInput->selectedText().size();
    const ChatLinkText::Insertion insertion = forkmesh::chatformat::wrapSelection(
        m_messageInput->text(), start, end, before, after);
    if (insertion.text.size() > m_messageInput->maxLength())
        return;
    m_messageInput->setText(insertion.text);
    if (insertion.selectionLength > 0)
        m_messageInput->setSelection(insertion.selectionStart,
                                     insertion.selectionLength);
    else
        m_messageInput->setCursorPosition(insertion.selectionStart);
    m_messageInput->setFocus();
}

void MainWindow::sendMessageToPrompt(const QString &text)
{
    if (text.isEmpty())
        return;
    appendTextToActivePrompt(text);
}

void MainWindow::showEmojiPicker(QWidget *anchor)
{
    if (!m_messageInput)
        return;
    static const char *const kEmoji[] = {
        "\xF0\x9F\x98\x80" /*😀*/, "\xF0\x9F\x98\x81", "\xF0\x9F\x98\x82",
        "\xF0\x9F\xA4\xA3", "\xF0\x9F\x98\x8A", "\xF0\x9F\x98\x8D",
        "\xF0\x9F\x98\x8E", "\xF0\x9F\x98\x89", "\xF0\x9F\x99\x82",
        "\xF0\x9F\x98\xA2", "\xF0\x9F\x98\xAD", "\xF0\x9F\x98\xA1",
        "\xF0\x9F\x98\xB1", "\xF0\x9F\xA4\x94", "\xF0\x9F\x98\xB4",
        "\xF0\x9F\xA4\xAF", "\xF0\x9F\x91\x8D", "\xF0\x9F\x91\x8E",
        "\xF0\x9F\x91\x8F", "\xF0\x9F\x99\x8F", "\xF0\x9F\x92\xAA",
        "\xF0\x9F\x99\x8C", "\xF0\x9F\x91\x8B", "\xF0\x9F\xA4\x9D",
        "\xF0\x9F\x94\xA5", "\xE2\x9C\xA8", "\xF0\x9F\x8E\x89",
        "\xF0\x9F\x92\xAF", "\xE2\x9D\xA4\xEF\xB8\x8F", "\xF0\x9F\x92\x94",
        "\xE2\xAD\x90", "\xE2\x9C\x85", "\xE2\x9D\x8C", "\xF0\x9F\x91\x80",
        "\xF0\x9F\x9A\x80", "\xF0\x9F\x90\x9B", "\xF0\x9F\x92\xA1",
        "\xF0\x9F\x93\x8C", "\xE2\x98\x95", "\xF0\x9F\x8D\x95",
        "\xF0\x9F\x8E\x82", "\xF0\x9F\xA5\xB3",
    };

    QMenu menu(this);
    auto *grid = new QWidget(&menu);
    auto *gridLayout = new QGridLayout(grid);
    gridLayout->setContentsMargins(6, 6, 6, 6);
    gridLayout->setSpacing(2);
    constexpr int kColumns = 7;
    int index = 0;
    for (const char *utf8 : kEmoji) {
        const QString emoji = QString::fromUtf8(utf8);
        auto *button = new QToolButton(grid);
        button->setObjectName("emojiPickerButton");
        button->setText(emoji);
        button->setCursor(Qt::PointingHandCursor);
        button->setAutoRaise(true);
        connect(button, &QToolButton::clicked, &menu, [this, emoji, &menu] {
            insertEmojiIntoComposer(emoji);
            menu.close();
        });
        gridLayout->addWidget(button, index / kColumns, index % kColumns);
        ++index;
    }
    auto *action = new QWidgetAction(&menu);
    action->setDefaultWidget(grid);
    menu.addAction(action);
    if (anchor)
        menu.exec(anchor->mapToGlobal(QPoint(0, -menu.sizeHint().height())));
    else
        menu.exec(QCursor::pos());
}

void MainWindow::maybeAskForkbot(const QString &conversation, const QString &text,
                                 const QString &threadRootId)
{
    static const QRegularExpression forkbotMention(
        QStringLiteral("(?:^|[^A-Za-z0-9_-])@?forkbot\\b"),
        QRegularExpression::CaseInsensitiveOption);
    if (!m_networkAccess ||
        (!m_backend && !m_officeChannelMirror && !m_discordClient))
        return;
    if (!forkbotMention.match(text).hasMatch())
        return;
    const bool office = isOfficeConversation(conversation);
    const bool discord = isDiscordConversation(conversation);
    const bool publicDiscord =
        discord && m_discordClient &&
        m_discordClient->isGuildConversation(conversation);
    const bool publicChannel = publicDiscord ||
                               (!discord &&
                                conversation.startsWith(QLatin1Char('#')) &&
                                !m_privateChannels.contains(conversation) &&
                                (!office ||
                                 (m_officeChannelMirror &&
                                  !m_officeChannelMirror->isPrivateConversation(
                                      conversation))));
    if (!publicChannel) {
        logSystem(QStringLiteral("ForkBot is available only in public, "
                                 "controller-readable channels."));
        return;
    }

    QJsonArray context;
    const QList<ChatMessage> &history = m_history.value(conversation);
    const int end = history.size() - 1; // exclude the triggering message
    for (int i = end - 1; i >= 0 && context.size() < 12; --i) {
        const ChatMessage &entry = history.at(i);
        if (entry.senderId == QLatin1String("forkbot") || entry.text.isEmpty())
            continue;
        // Keep a thread question anchored in its own discussion. The root
        // itself has an empty threadRootId, so include it by id as well.
        if (!threadRootId.isEmpty() && entry.id != threadRootId &&
            entry.threadRootId != threadRootId) {
            continue;
        }
        context.prepend(QJsonObject{
            {QStringLiteral("sender"), entry.senderName.left(32)},
            {QStringLiteral("text"), entry.text.left(600)},
        });
    }

    QUrl url = catalogApiUrl();
    url.setPath(QStringLiteral("/api/forkbot/chat"));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    const QJsonObject payload{{QStringLiteral("message"), text},
                              {QStringLiteral("sender"), chatDisplayName()},
                              {QStringLiteral("room"), QStringLiteral("general")},
                              {QStringLiteral("context"), context}};
    const QString channel = conversation;
    const QString rootId = threadRootId.trimmed().left(96);
    QNetworkReply *reply = m_networkAccess->post(
        request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, channel, rootId] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
            return;
        const QJsonObject resp =
            QJsonDocument::fromJson(reply->readAll()).object();
        const QString botMessage =
            resp.value(QStringLiteral("botMessage")).toString().trimmed();
        if (botMessage.isEmpty())
            return;
        if (isDiscordConversation(channel)) {
            if (m_discordClient) {
                if (rootId.isEmpty())
                    m_discordClient->sendMessage(channel, botMessage);
                else
                    m_discordClient->sendReply(channel, rootId, botMessage);
            }
        } else if (isOfficeConversation(channel)) {
            if (m_officeChannelMirror)
                m_officeChannelMirror->sendBotMessage(channel, botMessage);
        } else if (m_backend) {
            if (rootId.isEmpty())
                m_backend->sendBotChat(channel, botMessage);
            else
                m_backend->sendBotThreadReply(channel, rootId, botMessage);
        }
    });
}

void MainWindow::onComposerEdited(const QString &text)
{
    updateMentionPopup();
    if ((!m_backend && !m_discordClient) || m_currentConversation.isEmpty())
        return;
    if (text.trimmed().isEmpty()) {
        sendTypingState(false);
        return;
    }
    sendTypingState(true);
    m_typingStopTimer->start(2500);
}

static QString mentionTokenAt(const QString &text, int cursor, int *tokenStart)
{
    if (tokenStart)
        *tokenStart = -1;
    int start = cursor - 1;
    while (start >= 0 && !text.at(start).isSpace() &&
           text.at(start) != QLatin1Char('@'))
        --start;
    if (start < 0 || text.at(start) != QLatin1Char('@'))
        return QString();
    if (start > 0 && !text.at(start - 1).isSpace())
        return QString();
    if (tokenStart)
        *tokenStart = start;
    return text.mid(start + 1, cursor - start - 1);
}

void MainWindow::refreshMentionCandidates()
{
    if (!m_mentionModel || m_showingSlashCommands)
        return;
    const forkmesh::teammates::LoadResult team =
        forkmesh::teammates::Store(teammatesDirPath()).load();
    const auto addTeammates = [&team](QStringList *names) {
        if (!names)
            return;
        for (const forkmesh::teammates::Config &teammate : team.teammates) {
            if (!teammate.enabled)
                continue;
            const QString handle =
                forkmesh::teammates::mentionHandle(teammate);
            if (!handle.isEmpty() &&
                !names->contains(handle, Qt::CaseInsensitive)) {
                names->append(handle);
            }
        }
    };
    if (isDiscordConversation(m_currentConversation)) {
        QStringList names =
            m_discordClient
                ? m_discordClient->mentionNames(m_currentConversation)
                : QStringList();
        addTeammates(&names);
        names.sort(Qt::CaseInsensitive);
        m_mentionModel->setStringList(names);
        return;
    }
    QStringList names;
    QSet<QString> seenFolded; // case-insensitive dedup without an O(n^2) scan
    auto addName = [&](const MemberInfo &member) {
        const QString name = chatUserDisplayName(member);
        if (member.self || name.isEmpty())
            return;
        const QString folded = name.toCaseFolded();
        if (seenFolded.contains(folded))
            return;
        seenFolded.insert(folded);
        names.append(name);
    };
    for (const MemberInfo &member : std::as_const(m_homeRoster))
        addName(member);
    for (const MemberInfo &member : std::as_const(m_chatDirectoryUsers))
        addName(member);
    addTeammates(&names);
    if (m_currentConversation == QLatin1String("#team") &&
        std::any_of(team.teammates.cbegin(), team.teammates.cend(),
                    [](const forkmesh::teammates::Config &teammate) {
                        return teammate.enabled;
                    }) &&
        !names.contains(QStringLiteral("all"), Qt::CaseInsensitive)) {
        names.append(QStringLiteral("all"));
    }
    const QString ownName = accountOwner().trimmed();
    if (!ownName.isEmpty()) {
        for (int i = names.size() - 1; i >= 0; --i)
            if (names.at(i).compare(ownName, Qt::CaseInsensitive) == 0)
                names.removeAt(i);
    }
    names.sort(Qt::CaseInsensitive);
    if (m_mentionModel->stringList() != names)
        m_mentionModel->setStringList(names); // a reset the completer re-filters on
}

void MainWindow::updateMentionPopup()
{
    if (!m_mentionCompleter || !m_messageInput)
        return;
    const QString text = m_messageInput->text();
    const int cursor = m_messageInput->cursorPosition();
    static const QRegularExpression slashToken(
        QStringLiteral("\\A/[A-Za-z]*\\z"));
    if (cursor == text.size() && slashToken.match(text).hasMatch()) {
        QStringList commands{
            QStringLiteral("/issue [owner/repo] <title>  —  Create a repository issue")};
        if (isOfficeConversation(m_currentConversation) &&
            m_officeChannelMirror &&
            m_officeChannelMirror->canManageConversation(
                m_currentConversation)) {
            commands.append(QStringLiteral(
                "/topic <text>  —  Change this managed channel topic"));
        }
        m_showingSlashCommands = true;
        m_mentionModel->setStringList(commands);
        m_mentionCompleter->setCompletionPrefix(text);
        if (m_mentionCompleter->completionCount() == 0) {
            m_mentionCompleter->popup()->hide();
            return;
        }
        QRect rect = m_messageInput->rect();
        rect.setWidth(qMin(m_messageInput->width(),
                           m_mentionCompleter->popup()->sizeHintForColumn(0) +
                               24));
        m_mentionCompleter->complete(rect);
        return;
    }
    if (m_showingSlashCommands) {
        m_showingSlashCommands = false;
        refreshMentionCandidates();
    }
    int tokenStart = -1;
    const QString token =
        mentionTokenAt(text, cursor, &tokenStart);
    if (tokenStart < 0 || m_mentionModel->rowCount() == 0) {
        m_mentionCompleter->popup()->hide();
        return;
    }
    m_mentionCompleter->setCompletionPrefix(token);
    if (m_mentionCompleter->completionCount() == 0) {
        m_mentionCompleter->popup()->hide();
        return;
    }
    QRect rect = m_messageInput->rect();
    rect.setWidth(m_mentionCompleter->popup()->sizeHintForColumn(0) + 24);
    m_mentionCompleter->complete(rect);
}

void MainWindow::insertComposerCompletion(const QString &completion)
{
    if (completion.startsWith(QLatin1Char('/'))) {
        const QString command = completion.section(QLatin1Char(' '), 0, 0);
        m_messageInput->setText(command + QLatin1Char(' '));
        m_messageInput->setCursorPosition(m_messageInput->text().size());
        m_mentionCompleter->popup()->hide();
        return;
    }
    insertMention(completion);
}

void MainWindow::insertMention(const QString &name)
{
    if (!m_messageInput || name.isEmpty())
        return;
    QString text = m_messageInput->text();
    int tokenStart = -1;
    mentionTokenAt(text, m_messageInput->cursorPosition(), &tokenStart);
    if (tokenStart < 0)
        return;
    const QString mention = QLatin1Char('@') + name + QLatin1Char(' ');
    text.replace(tokenStart, m_messageInput->cursorPosition() - tokenStart, mention);
    m_messageInput->setText(text);
    m_messageInput->setCursorPosition(tokenStart + mention.length());
}

void MainWindow::sendTypingState(bool active)
{
    if (active && isOfficeConversation(m_currentConversation))
        return;
    if (isLocalMeshConversation(m_currentConversation)) {
        if (m_localMesh)
            m_localMesh->sendTyping(active);
        m_typingConversation = active ? m_currentConversation : QString();
        if (!active)
            m_typingStopTimer->stop();
        return;
    }
    if (isDiscordConversation(m_currentConversation)) {
        if (active && m_discordClient) {
            m_typingConversation = m_currentConversation;
            m_discordClient->sendTyping(m_currentConversation);
        } else {
            m_typingConversation.clear();
        }
        if (!active)
            m_typingStopTimer->stop();
        return;
    }
    if (!m_backend)
        return;
    if (active) {
        if (m_currentConversation.isEmpty())
            return;
        if (m_typingConversation == m_currentConversation &&
            m_typingStopTimer->isActive())
            return;
        m_typingConversation = m_currentConversation;
        m_backend->sendTyping(m_currentConversation, true);
        return;
    }
    if (!m_typingConversation.isEmpty()) {
        m_backend->sendTyping(m_typingConversation, false);
        m_typingConversation.clear();
    }
    m_typingStopTimer->stop();
}

void MainWindow::refreshTypingLabel()
{
    QStringList names;
    const auto active = m_typing.value(m_currentConversation);
    for (const QString &name : active)
        names.append(name);
    names.removeDuplicates();

    if (names.isEmpty()) {
        m_typingLabel->clear();
    } else if (names.size() == 1) {
        m_typingLabel->setText(names.first() + " is typing...");
    } else if (names.size() == 2) {
        m_typingLabel->setText(names.at(0) + " and " + names.at(1) +
                               " are typing...");
    } else {
        m_typingLabel->setText(QString::number(names.size()) + " people are typing...");
    }
}


bool MainWindow::canAttachToOfficeConversation() const
{
    return m_officeChannelMirror &&
           m_officeChannelMirror->canSendFile(m_currentConversation);
}

void MainWindow::attachFile()
{
    if (m_currentConversation.isEmpty())
        return;
    const bool office = isOfficeConversation(m_currentConversation);
    if (office ? !canAttachToOfficeConversation()
               : (!m_backend && !m_discordClient))
        return;
    const QString path =
        QFileDialog::getOpenFileName(this, "Share a file", QString(), "All files (*)");
    if (path.isEmpty())
        return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, "Share a file", "Could not read " + path);
        return;
    }
    const bool discord = isDiscordConversation(m_currentConversation);
    const qint64 maxBytes = office    ? forkmesh::office::kMaxDirectFileBytes
                            : discord ? 10ll * 1024 * 1024
                                      : 64ll * 1024 * 1024;
    if (file.size() > maxBytes) {
        QMessageBox::warning(this, "Share a file",
                             office
                                 ? QStringLiteral(
                                       "Direct messages take files up to 1 MiB. "
                                       "Please share a smaller file.")
                             : discord
                                 ? QStringLiteral(
                                       "That file is larger than Discord's "
                                       "default 10 MiB bot upload limit.")
                                 : QStringLiteral(
                                       "That file is larger than 64 MB. Please "
                                       "share a smaller file."));
        return;
    }
    const QByteArray data = file.readAll();
    const QFileInfo info(path);
    const QString mime = QMimeDatabase().mimeTypeForFileNameAndData(path, data).name();
    if (office) {
        if (!m_officeChannelMirror->sendFile(m_currentConversation,
                                             info.fileName(), mime, data))
            QMessageBox::warning(this, QStringLiteral("Share a file"),
                                 QStringLiteral("Could not share that file in "
                                                "this conversation."));
    } else if (discord) {
        if (m_discordClient)
            m_discordClient->sendFile(m_currentConversation, info.fileName(),
                                      mime, data);
    } else if (isLocalMeshConversation(m_currentConversation)) {
        if (!m_localMesh ||
            m_localMesh->sendFile(info.fileName(), mime, data).isEmpty()) {
            QMessageBox::warning(
                this, QStringLiteral("Share a file"),
                QStringLiteral("The local mesh takes files up to 16 MB."));
        }
    } else if (m_backend) {
        m_backend->sendFile(m_currentConversation, info.fileName(), mime, data);
    }
}

void MainWindow::showChatImageDetail(const QString &fileName,
                                     const QByteArray &data)
{
    QPixmap pixmap;
    if (!pixmap.loadFromData(data)) {
        saveIncomingFile(fileName, data);
        return;
    }

    auto *dialog = new QDialog(this);
    dialog->setObjectName("imageDetailDialog");
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(fileName.isEmpty() ? QStringLiteral("Image")
                                              : fileName);

    QSize maxSize(1200, 800);
    if (QScreen *screen = QGuiApplication::primaryScreen()) {
        const QSize avail = screen->availableSize();
        maxSize = QSize(avail.width() * 9 / 10, avail.height() * 9 / 10);
    }
    QPixmap shown = pixmap;
    if (pixmap.width() > maxSize.width() || pixmap.height() > maxSize.height())
        shown = pixmap.scaled(maxSize, Qt::KeepAspectRatio,
                              Qt::SmoothTransformation);

    auto *imageLabel = new QLabel;
    imageLabel->setAlignment(Qt::AlignCenter);
    imageLabel->setPixmap(shown);

    auto *scroll = new QScrollArea;
    scroll->setObjectName("messageView");
    scroll->setWidgetResizable(true);
    scroll->setAlignment(Qt::AlignCenter);
    scroll->setWidget(imageLabel);

    auto *saveButton = new QPushButton(QString::fromUtf8("Save\xE2\x80\xA6"));
    saveButton->setObjectName("ghostButton");
    saveButton->setCursor(Qt::PointingHandCursor);
    connect(saveButton, &QPushButton::clicked, this,
            [this, fileName, data] { saveIncomingFile(fileName, data); });
    auto *closeButton = new QPushButton(QStringLiteral("Close"));
    closeButton->setObjectName("primaryButton");
    closeButton->setCursor(Qt::PointingHandCursor);
    connect(closeButton, &QPushButton::clicked, dialog, &QDialog::accept);

    auto *buttonRow = new QHBoxLayout;
    buttonRow->setContentsMargins(0, 0, 0, 0);
    buttonRow->addWidget(new QLabel(
        QString::fromUtf8("%1 \xC3\x97 %2").arg(pixmap.width()).arg(pixmap.height())));
    buttonRow->addStretch();
    buttonRow->addWidget(saveButton);
    buttonRow->addWidget(closeButton);

    auto *layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(10);
    layout->addWidget(scroll, 1);
    layout->addLayout(buttonRow);

    dialog->resize(qMin(shown.width() + 48, maxSize.width()),
                   qMin(shown.height() + 96, maxSize.height()));
    dialog->show();
}

#ifdef FORKMESH_HAVE_MULTIMEDIA
namespace {

QString mediaClock(qint64 ms)
{
    const qint64 seconds = qMax(qint64(0), ms) / 1000;
    return QStringLiteral("%1:%2")
        .arg(seconds / 60)
        .arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

} // namespace
#endif

// Writes the attachment into the cache directory and hands it to whatever the
// desktop opens that media type with. This is how a build without Qt Multimedia
// plays a video at all, and it stays available in builds that have it: a
// backend can still be missing the codec this particular file needs.
bool MainWindow::openChatMediaExternally(const QString &fileName,
                                         const QString &fileMime,
                                         const QByteArray &data)
{
    if (data.isEmpty())
        return false;
    const QString root =
        forkmesh::paths::cacheRoot();
    if (root.isEmpty())
        return false;
    const QString dir = root + QStringLiteral("/chat-media");
    if (!QDir().mkpath(dir))
        return false;

    // The extension picks which program the desktop launches, so it is derived
    // from the declared media type rather than from the sender's file name.
    QString suffix = QMimeDatabase().mimeTypeForName(fileMime).preferredSuffix();
    if (suffix.isEmpty())
        suffix = QStringLiteral("mp4");
    static const QRegularExpression unsafe(QStringLiteral("[^A-Za-z0-9 ._-]"));
    QString stem = QFileInfo(fileName).completeBaseName().remove(unsafe).trimmed();
    stem = stem.left(80);
    if (stem.isEmpty())
        stem = QStringLiteral("video");
    // Two senders both calling it "video.mp4" must not overwrite each other's
    // scratch copy while the first one is still playing; the same file opened
    // twice reuses its own.
    stem += QLatin1Char('-') +
            QString::fromLatin1(
                QCryptographicHash::hash(data, QCryptographicHash::Sha1)
                    .toHex()
                    .left(8));

    // These are playback scratch copies, not a library: yesterday's are dropped
    // rather than quietly keeping a second copy of every video ever watched.
    const QDateTime cutoff = QDateTime::currentDateTime().addDays(-1);
    const QFileInfoList previous =
        QDir(dir).entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
    for (const QFileInfo &entry : previous) {
        if (entry.lastModified() < cutoff)
            QFile::remove(entry.absoluteFilePath());
    }

    const QString path = dir + QLatin1Char('/') + stem + QLatin1Char('.') + suffix;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        file.write(data) != data.size()) {
        file.close();
        QFile::remove(path); // never hand a player a half-written video
        return false;
    }
    file.close();
    if (file.error() != QFile::NoError) {
        QFile::remove(path);
        return false;
    }
    return QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void MainWindow::showChatVideoDetail(const QString &fileName,
                                     const QString &fileMime,
                                     const QByteArray &data)
{
    if (data.isEmpty())
        return;
#ifdef FORKMESH_HAVE_MULTIMEDIA
    auto *dialog = new QDialog(this);
    dialog->setObjectName("videoDetailDialog");
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(fileName.isEmpty() ? QStringLiteral("Video") : fileName);

    auto *player = new QMediaPlayer(dialog);
    auto *audio = new QAudioOutput(dialog);
    // Constructed after the player so QObject tears the buffer down last: the
    // player must never outlive the device it is still reading from.
    auto *source = new QBuffer(dialog);
    source->setData(data);
    source->open(QIODevice::ReadOnly);
    auto *surface = new QVideoWidget;
    surface->setObjectName("videoSurface");
    surface->setMinimumSize(480, 270);
    player->setAudioOutput(audio);
    player->setVideoOutput(surface);

    auto *playPause = new QPushButton(QStringLiteral("Pause"));
    playPause->setObjectName("videoPlayButton");
    playPause->setCursor(Qt::PointingHandCursor);
    auto *scrubber = new QSlider(Qt::Horizontal);
    scrubber->setObjectName("videoScrubber");
    scrubber->setRange(0, 0);
    auto *clock = new QLabel(QStringLiteral("0:00 / 0:00"));
    clock->setObjectName("videoClock");
    auto *status = new QLabel;
    status->setObjectName("videoStatus");
    status->setWordWrap(true);
    status->hide();

    connect(playPause, &QPushButton::clicked, player, [player] {
        if (player->playbackState() == QMediaPlayer::PlayingState)
            player->pause();
        else
            player->play();
    });
    connect(player, &QMediaPlayer::playbackStateChanged, playPause,
            [playPause](QMediaPlayer::PlaybackState state) {
                playPause->setText(state == QMediaPlayer::PlayingState
                                       ? QStringLiteral("Pause")
                                       : QStringLiteral("Play"));
            });
    connect(player, &QMediaPlayer::durationChanged, scrubber,
            [scrubber, clock, player](qint64 duration) {
                scrubber->setRange(0, int(qMax(qint64(0), duration)));
                clock->setText(mediaClock(player->position()) +
                               QStringLiteral(" / ") + mediaClock(duration));
            });
    connect(player, &QMediaPlayer::positionChanged, scrubber,
            [scrubber, clock, player](qint64 position) {
                if (!scrubber->isSliderDown())
                    scrubber->setValue(int(position));
                clock->setText(mediaClock(position) + QStringLiteral(" / ") +
                               mediaClock(player->duration()));
            });
    connect(scrubber, &QSlider::sliderMoved, player,
            [player](int value) { player->setPosition(value); });
    connect(player, &QMediaPlayer::errorOccurred, dialog,
            [status, playPause, scrubber](QMediaPlayer::Error error,
                                          const QString &detail) {
                if (error == QMediaPlayer::NoError)
                    return;
                // The file is still saveable and still openable in a system
                // player, so say what failed instead of leaving a black frame.
                status->setText(
                    QStringLiteral("Could not play this video here: %1. Try "
                                   "\"Open in player\".")
                        .arg(detail.isEmpty() ? QStringLiteral("unsupported format")
                                              : detail));
                status->show();
                playPause->setEnabled(false);
                scrubber->setEnabled(false);
            });
    connect(dialog, &QDialog::finished, player, [player] {
        player->stop();
        player->setSourceDevice(nullptr);
    });

    auto *external = new QPushButton(QStringLiteral("Open in player"));
    external->setObjectName("ghostButton");
    external->setCursor(Qt::PointingHandCursor);
    connect(external, &QPushButton::clicked, this,
            [this, fileName, fileMime, data, player] {
                player->pause();
                if (!openChatMediaExternally(fileName, fileMime, data))
                    QMessageBox::warning(
                        this, QStringLiteral("Play video"),
                        QStringLiteral("Could not hand that video to a system "
                                       "player."));
            });
    auto *save = new QPushButton(QStringLiteral("Save\u2026"));
    save->setObjectName("ghostButton");
    save->setCursor(Qt::PointingHandCursor);
    connect(save, &QPushButton::clicked, this,
            [this, fileName, data] { saveIncomingFile(fileName, data); });
    auto *close = new QPushButton(QStringLiteral("Close"));
    close->setObjectName("primaryButton");
    close->setCursor(Qt::PointingHandCursor);
    connect(close, &QPushButton::clicked, dialog, &QDialog::accept);

    auto *controls = new QHBoxLayout;
    controls->setContentsMargins(0, 0, 0, 0);
    controls->setSpacing(10);
    controls->addWidget(playPause);
    controls->addWidget(scrubber, 1);
    controls->addWidget(clock);

    auto *actions = new QHBoxLayout;
    actions->setContentsMargins(0, 0, 0, 0);
    actions->setSpacing(8);
    actions->addWidget(external);
    actions->addStretch();
    actions->addWidget(save);
    actions->addWidget(close);

    auto *layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(10);
    layout->addWidget(surface, 1);
    layout->addWidget(status);
    layout->addLayout(controls);
    layout->addLayout(actions);

    player->setSourceDevice(source);
    dialog->resize(760, 540);
    dialog->show();
    player->play();
#else
    // No in-app decoder in this build: the video still plays, in whatever the
    // desktop already opens that type with. Saving stays the last resort.
    if (!openChatMediaExternally(fileName, fileMime, data))
        saveIncomingFile(fileName, data);
#endif
}

// A composer attachment is already a file on disk, so the player it opens in is
// the chat one — read the bytes back and hand it the same dialog, controls and
// codec fallbacks. A recording past the cap is not held in memory to do that:
// a screen capture can be arbitrarily long, and the desktop opens the original.
void MainWindow::showAttachedVideoDetail(const QString &path)
{
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile())
        return;
#ifdef FORKMESH_HAVE_MULTIMEDIA
    constexpr qint64 kMaxInlineVideoBytes = 64 * 1024 * 1024;
    if (info.size() > 0 && info.size() <= kMaxInlineVideoBytes) {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly)) {
            const QByteArray data = file.readAll();
            if (!data.isEmpty()) {
                showChatVideoDetail(info.fileName(),
                                    QMimeDatabase().mimeTypeForFile(info).name(),
                                    data);
                return;
            }
        }
    }
#endif
    // No in-app decoder in this build, or a recording too long to hold in
    // memory. The attachment is already a file on disk, so the desktop opens
    // the original rather than the cached copy the chat route has to write.
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(info.absoluteFilePath())))
        QMessageBox::warning(
            this, QStringLiteral("Play video"),
            QStringLiteral("Could not open %1 in a player.").arg(info.fileName()));
}

namespace {

struct PastedImage {
    QString fileName;
    QString mime;
    QByteArray data;
};

QByteArray encodedImage(const QImage &image, const char *format, int quality)
{
    QByteArray data;
    QBuffer buffer(&data);
    buffer.open(QIODevice::WriteOnly);
    return image.save(&buffer, format, quality) ? data : QByteArray();
}

PastedImage encodePastedImage(const QImage &image, qsizetype maxBytes)
{
    const QByteArray png = encodedImage(image, "PNG", -1);
    if (!png.isEmpty() && png.size() <= maxBytes)
        return {QStringLiteral("pasted-image.png"), QStringLiteral("image/png"),
                png};
    QImage opaque(image.size(), QImage::Format_RGB32);
    opaque.fill(Qt::white);
    {
        QPainter painter(&opaque);
        painter.drawImage(0, 0, image);
    }
    for (int step = 0; step < 8 && !opaque.isNull(); ++step) {
        for (const int quality : {85, 70}) {
            const QByteArray jpeg = encodedImage(opaque, "JPEG", quality);
            if (!jpeg.isEmpty() && jpeg.size() <= maxBytes)
                return {QStringLiteral("pasted-image.jpg"),
                        QStringLiteral("image/jpeg"), jpeg};
        }
        opaque = opaque.scaled(opaque.size() * 3 / 4, Qt::KeepAspectRatio,
                               Qt::SmoothTransformation);
    }
    return {};
}

} // namespace

bool MainWindow::trySendClipboardImage()
{
    if (m_currentConversation.isEmpty())
        return false;
    const bool office = isOfficeConversation(m_currentConversation);
    if (office ? !canAttachToOfficeConversation() : !m_backend)
        return false;
    const QMimeData *mime = QGuiApplication::clipboard()->mimeData();
    if (!mime || !mime->hasImage())
        return false;
    const QImage image = qvariant_cast<QImage>(mime->imageData());
    if (image.isNull())
        return false;
    if (office) {
        const PastedImage pasted =
            encodePastedImage(image, forkmesh::office::kMaxDirectFileBytes);
        if (pasted.data.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("Paste an image"),
                                 QStringLiteral("That image is too large for a "
                                                "direct message."));
            return true;
        }
        return m_officeChannelMirror->sendFile(m_currentConversation,
                                               pasted.fileName, pasted.mime,
                                               pasted.data);
    }
    QByteArray data;
    QBuffer buffer(&data);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG"))
        return false;
    if (isLocalMeshConversation(m_currentConversation)) {
        if (!m_localMesh)
            return false;
        return !m_localMesh
                    ->sendFile(QStringLiteral("pasted-image.png"),
                               QStringLiteral("image/png"), data)
                    .isEmpty();
    }
    m_backend->sendFile(m_currentConversation,
                        QStringLiteral("pasted-image.png"), QStringLiteral("image/png"),
                        data);
    return true;
}


namespace {

constexpr int kDeferredErrorReportFlushMs = 2 * 60 * 1000;

} // namespace

void MainWindow::reportUserVisibleError(const QString &kind,
                                        const QString &title,
                                        const QString &message,
                                        const QString &surface,
                                        qint64 pingId)
{
    if (!QSettings()
             .value(forkmesh::kReportUserVisibleErrorsSetting, false)
             .toBool())
        return; // the ping was already filed as local-only; see initialPingSync
    const QString area =
        !surface.isEmpty() ? surface : QStringLiteral("app");
    forkmesh::ClientErrorReports::Report report =
        forkmesh::ClientErrorReports::build(
            kind, area, title, message,
            QDateTime::currentMSecsSinceEpoch());
    report.pingId = pingId;
    if (!forkmesh::ClientErrorReports::relayAccepts(report)) {
        setPingSync(pingId, forkmesh::PingSync::LocalOnly,
                    QStringLiteral("the relay only stores a desktop failure "
                                   "that does not quote an earlier report; "
                                   "this copy stayed local"));
        return;
    }
    if (!m_errorReports.accept(report)) {
        setPingSync(pingId, forkmesh::PingSync::LocalOnly,
                    QStringLiteral("an identical failure was already reported "
                                   "to the relay; this copy stayed local"));
        return;
    }
    setPingSync(pingId, forkmesh::PingSync::Pending,
                QStringLiteral("queued for the relay"));
    QTimer::singleShot(0, this,
                       [this, report] { sendUserVisibleErrorReport(report); });
}

void MainWindow::sendUserVisibleErrorReport(
    const forkmesh::ClientErrorReports::Report &report)
{
    // Nothing to sign with means the relay would refuse the report anyway: it
    // only stores reports carrying a signed owner token, never anonymous ones.
    const QString owner = accountOwner();
    if (!m_networkAccess || owner.isEmpty() || !m_profileIdentity.isValid()
        || !hasOwnerSigningCapability(owner)) {
        setPingSync(report.pingId, forkmesh::PingSync::Offline,
                    QStringLiteral("no signed-in account to sign a report "
                                   "with, so nothing was sent"));
        return;
    }
    QUrl url = catalogApiUrl();
    url.setPath(QStringLiteral("/api/desktop-errors"));
    const QUrlQuery query = signedInboxQuery(owner);
    if (query.isEmpty()) {
        setPingSync(report.pingId, forkmesh::PingSync::Offline,
                    QStringLiteral("this account's signing key is locked, so "
                                   "nothing was sent"));
        return;
    }
    url.setQuery(query);

    const auto *network =
        qobject_cast<BackoffNetworkAccessManager *>(m_networkAccess);
    if (network && !url.host().isEmpty() && network->hostInCooldown(url.host())) {
        if (m_errorReports.defer(report)) {
            setPingSync(report.pingId, forkmesh::PingSync::Pending,
                        QStringLiteral("the relay is rate-limited; parked for "
                                       "a retry"));
            scheduleDeferredErrorReportFlush();
        } else {
            setPingSync(report.pingId, forkmesh::PingSync::Failed,
                        QStringLiteral("the relay stayed rate-limited and the "
                                       "report was given up on"));
        }
        return;
    }

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    QNetworkReply *reply = m_networkAccess->post(
        request,
        QJsonDocument(forkmesh::ClientErrorReports::payload(report))
            .toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, report] {
        reply->deleteLater();
        if (reply->error() == QNetworkReply::NoError) {
            setPingSync(report.pingId, forkmesh::PingSync::Synced,
                        QStringLiteral("the relay recorded this failure"));
            return;
        }
        const bool offline =
            reply->error() == QNetworkReply::ConnectionRefusedError ||
            reply->error() == QNetworkReply::RemoteHostClosedError ||
            reply->error() == QNetworkReply::HostNotFoundError ||
            reply->error() == QNetworkReply::TimeoutError ||
            reply->error() == QNetworkReply::TemporaryNetworkFailureError ||
            reply->error() == QNetworkReply::NetworkSessionFailedError ||
            reply->error() == QNetworkReply::UnknownNetworkError;
        const QString detail = reply->errorString().simplified();
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (forkmesh::ClientErrorReports::permanentRefusal(status)) {
            setPingSync(report.pingId, forkmesh::PingSync::Failed,
                        QStringLiteral("the relay refused it (HTTP %1: %2); a "
                                       "retry would be refused the same way")
                            .arg(status)
                            .arg(detail));
            return;
        }
        if (m_errorReports.defer(report)) {
            setPingSync(report.pingId, forkmesh::PingSync::Pending,
                        (offline ? QStringLiteral("this node is offline (%1); "
                                                  "parked for a retry")
                                 : QStringLiteral("the relay didn't take it "
                                                  "(%1); parked for a retry"))
                            .arg(detail));
            scheduleDeferredErrorReportFlush();
        } else {
            setPingSync(report.pingId,
                        offline ? forkmesh::PingSync::Offline
                                : forkmesh::PingSync::Failed,
                        (offline
                             ? QStringLiteral("this node was offline (%1) and "
                                              "the relay never got it")
                             : QStringLiteral("the relay refused it (%1) and "
                                              "the report was given up on"))
                            .arg(detail));
        }
    });
}

void MainWindow::scheduleDeferredErrorReportFlush()
{
    if (m_errorReports.deferredCount() <= 0)
        return;
    if (!m_errorReportFlushTimer) {
        m_errorReportFlushTimer = new QTimer(this);
        m_errorReportFlushTimer->setInterval(kDeferredErrorReportFlushMs);
        connect(m_errorReportFlushTimer, &QTimer::timeout, this,
                &MainWindow::flushDeferredErrorReports);
    }
    if (!m_errorReportFlushTimer->isActive())
        m_errorReportFlushTimer->start();
}

void MainWindow::flushDeferredErrorReports()
{
    if (m_errorReports.deferredCount() <= 0) {
        if (m_errorReportFlushTimer)
            m_errorReportFlushTimer->stop();
        return;
    }
    const auto *network =
        qobject_cast<BackoffNetworkAccessManager *>(m_networkAccess);
    const QString host = catalogApiUrl().host();
    if (network && !host.isEmpty() && network->hostInCooldown(host))
        return; // still refusing; the next tick tries again
    if (m_errorReportFlushTimer)
        m_errorReportFlushTimer->stop();
    QList<forkmesh::ClientErrorReports::Report> expired;
    const QList<forkmesh::ClientErrorReports::Report> ready =
        m_errorReports.takeDeferred(QDateTime::currentMSecsSinceEpoch(),
                                    &expired);
    for (const forkmesh::ClientErrorReports::Report &report : expired)
        setPingSync(report.pingId, forkmesh::PingSync::Failed,
                    QStringLiteral("the relay was unreachable for hours and "
                                   "the report was dropped"));
    for (const forkmesh::ClientErrorReports::Report &report : ready)
        sendUserVisibleErrorReport(report);
    scheduleDeferredErrorReportFlush();
}

void MainWindow::saveIncomingFile(const QString &fileName, const QByteArray &data)
{
    const QString safeName = QFileInfo(fileName).fileName().left(180);
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    const QString suggested =
        (dir.isEmpty() ? QDir::homePath() : dir) + "/" +
        (safeName.isEmpty() ? QStringLiteral("file") : safeName);
    const QString path =
        QFileDialog::getSaveFileName(this, "Save file", suggested);
    if (path.isEmpty())
        return;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size()) {
        QMessageBox::warning(this, "Save file", "Could not save to " + path);
        return;
    }
}
