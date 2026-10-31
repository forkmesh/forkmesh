
#include "PackageModules.h"
#include "PingSettingsPanel.h"
#include "ProcessReaper.h"
#include "ChatBlocks.h"
#include "ChatRecipients.h"
#include "ControlNode.h"
#include "DesktopPingForward.h"
#include "DiscordClient.h"
#include "ForkMeshPackageRegistry.h"
#include "ForkMeshVersion.h"
#include "MainWindow.h"
#include "MessageBoxClicks.h"
#include "MirrorNodeColumns.h"
#include "MainWindowInternal.h"
#include "MainWindowNextAccess.h"
#include "NetworkReplyError.h"
#include "NodeEventSocket.h"
#include "OwnerOnlyFile.h"
#include "SafeDelete.h"
#include "LocalMeshNode.h"
#include "OfficeChannelMirror.h"
#include "PrivateMirrorStore.h"
#include "ReleaseTrust.h"
#include "RestartTiming.h"
#include "SourceTreeWatch.h"
#include "UserAvatarStore.h"
#include "SettingsWrite.h"

#include <QFutureWatcher>
#include <QElapsedTimer>
#include <QItemSelectionModel>
#include <QNetworkInformation>
#include <QProcessEnvironment>
#include <QThreadPool>
#include <QtConcurrent/QtConcurrentRun>

#ifndef Q_OS_WIN
#include <csignal>
#include <unistd.h>
#endif

using namespace forkmesh::ui;


QString MainWindow::chatHistoryKey() const
{
    if (m_activeServer < 0 || m_activeServer >= m_servers.size())
        return {};
    const ServerConfig &s = m_servers.at(m_activeServer);
    const QString instanceKey = s.id.trimmed().isEmpty() ? s.url : s.id;
    return QString::fromLatin1(
        QCryptographicHash::hash(
            (instanceKey + "\n" + s.room).toUtf8(), QCryptographicHash::Sha256)
            .toHex());
}

QString MainWindow::chatHistoryPathForServerUrl(const QString &serverUrl,
                                                const QString &room) const
{
    const QString url = serverUrl.trimmed();
    if (url.isEmpty())
        return {};
    const QString key = QString::fromLatin1(
        QCryptographicHash::hash((url + "\n" + room).toUtf8(),
                                 QCryptographicHash::Sha256)
            .toHex());
    return forkmesh::paths::dataPath(QStringLiteral("chat_history")) +
           "/" + key + ".json";
}

QStringList MainWindow::chatHistoryCompatibilityPaths() const
{
    if (m_activeServer < 0 || m_activeServer >= m_servers.size())
        return {};
    const ServerConfig &server = m_servers.at(m_activeServer);
    QStringList paths{chatHistoryPathForServerUrl(server.url, server.room)};
    if (m_activeServer < m_legacyServerUrls.size() &&
        m_activeServer < m_legacyServerRooms.size()) {
        const QString legacyUrl =
            m_legacyServerUrls.at(m_activeServer).trimmed();
        if (!legacyUrl.isEmpty())
            paths.append(chatHistoryPathForServerUrl(
                legacyUrl, m_legacyServerRooms.at(m_activeServer)));
    }
    paths.removeAll(chatHistoryPath());
    paths.removeDuplicates();
    return paths;
}

QString MainWindow::chatHistoryPath() const
{
    const QString key = chatHistoryKey();
    if (key.isEmpty())
        return {};
    return forkmesh::paths::dataPath(QStringLiteral("chat_history")) +
           "/" + key + ".json";
}

QString MainWindow::avatarCacheDirectory() const
{
    QString instance = activeServerInstanceId();
    if (instance.isEmpty())
        instance = activeServerInstanceUrl();
    const QString directory = QString::fromLatin1(
        QCryptographicHash::hash(instance.toUtf8(), QCryptographicHash::Sha256)
            .toHex());
    return forkmesh::paths::dataPath(QStringLiteral("avatars")) +
           "/" + directory;
}

QString MainWindow::avatarCachePath(const QString &peerId) const
{
    if (peerId.isEmpty())
        return {};
    const QString file = QString::fromLatin1(
        QCryptographicHash::hash(peerId.toUtf8(), QCryptographicHash::Sha256)
            .toHex());
    return avatarCacheDirectory() + "/" + file + ".png";
}

void MainWindow::loadCachedAvatars()
{
    const QString dir = avatarCacheDirectory();
    if (!QDir(dir).exists())
        return;
    auto *watcher = new QFutureWatcher<QList<QPair<QString, QImage>>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher] {
        watcher->deleteLater();
        const QList<QPair<QString, QImage>> decoded = watcher->result();
        for (const auto &entry : decoded) {
            if (!m_avatars.contains(entry.first))
                m_avatars.insert(entry.first, QPixmap::fromImage(entry.second));
        }
        refreshVisibleRowAvatars();
        refreshChatMembers();
        refreshIssueAvatars();
        notifyPackagesAccountPicturesChanged();
    });
    watcher->setFuture(QtConcurrent::run([dir] {
        const forkmesh::BackgroundScope activity(
            QStringLiteral("avatars"),
            QStringLiteral("Decoding cached avatar image(s)"));
        QList<QPair<QString, QImage>> decoded;
        const QStringList names =
            QDir(dir).entryList({QStringLiteral("*.png")}, QDir::Files);
        for (const QString &name : names) {
            QFile f(QDir(dir).filePath(name));
            if (!f.open(QIODevice::ReadOnly))
                continue;
            const QByteArray data = f.readAll();
            const int nl = data.indexOf('\n');
            if (nl <= 0)
                continue;
            const QString peerId = QString::fromUtf8(data.left(nl));
            QImage image;
            if (peerId.isEmpty() || !image.loadFromData(data.mid(nl + 1)))
                continue;
            decoded.append({peerId, image});
        }
        return decoded;
    }));
}

namespace {

QThreadPool &chatHistoryWriter()
{
    static QThreadPool *pool = [] {
        auto *made = new QThreadPool;
        made->setMaxThreadCount(1);
        if (QCoreApplication *app = QCoreApplication::instance())
            QObject::connect(app, &QCoreApplication::aboutToQuit, app,
                             [made] { made->waitForDone(); });
        return made;
    }();
    return *pool;
}

QByteArray chatHistoryBytes(const QHash<QString, QList<ChatMessage>> &history,
                            const QStringList &openDmPeers,
                            const QHash<QString, QString> &dmNameMap,
                            const QHash<QString, int> &unreadCounts,
                            const QString &current)
{
    QJsonObject conversations;
    for (auto it = history.constBegin(); it != history.constEnd(); ++it) {
        QJsonArray arr;
        const QList<ChatMessage> &msgs = it.value();
        const int first = std::max(0, int(msgs.size()) - 1000);
        for (int i = first; i < msgs.size(); ++i) {
            const ChatMessage &m = msgs.at(i);
            QJsonObject obj{{"id", m.id},
                            {"threadRootId", m.threadRootId},
                            {"senderId", m.senderId},
                            {"senderName", m.senderName},
                            {"text", m.text},
                            {"ts", m.timestampMs},
                            {"self", m.self},
                            {"edited", m.edited},
                            {"deleted", m.deleted}};
            if (m.recipientCount >= 0)
                obj.insert("recipients", m.recipientCount);
            if (m.recipientCount >= 0 && !m.recipientNames.isEmpty())
                obj.insert("recipientNames",
                           QJsonArray::fromStringList(m.recipientNames));
            if (m.receipt != forkmesh::recipients::Receipt::None)
                obj.insert("receipt", forkmesh::recipients::receiptName(m.receipt));
            if (!m.blocks.isEmpty())
                obj.insert("blocks", m.blocks);
            if (m.hasFile()) {
                obj.insert("fileName", m.fileName);
                obj.insert("fileMime", m.fileMime);
                if (m.fileData.size() <= 512 * 1024)
                    obj.insert("fileData",
                               QString::fromLatin1(m.fileData.toBase64()));
            }
            arr.append(obj);
        }
        if (!arr.isEmpty())
            conversations.insert(it.key(), arr);
    }

    QJsonArray openDms;
    for (const QString &peer : openDmPeers)
        openDms.append(peer);
    QJsonObject dmNames;
    for (auto it = dmNameMap.constBegin(); it != dmNameMap.constEnd(); ++it)
        dmNames.insert(it.key(), it.value());

    QJsonObject unreadObj;
    for (auto it = unreadCounts.constBegin(); it != unreadCounts.constEnd(); ++it)
        unreadObj.insert(it.key(), it.value());

    const QJsonObject root{{"current", current},
                           {"openDms", openDms},
                           {"dmNames", dmNames},
                           {"unread", unreadObj},
                           {"conversations", conversations}};
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

} // namespace

void MainWindow::saveChatHistory()
{
    const QString path = chatHistoryPath();
    if (path.isEmpty())
        return;
    chatHistoryWriter().start(
        [path, history = m_history, openDms = m_openDms, dmNames = m_dmNames,
         unread = m_unreadCounts, current = m_currentConversation] {
            forkmesh::ownerfile::write(
                path, chatHistoryBytes(history, openDms, dmNames, unread, current));
        });
    if (m_closingDown)
        chatHistoryWriter().waitForDone();
}

void MainWindow::loadChatHistory()
{
    chatHistoryWriter().waitForDone();
    QString path = chatHistoryPath();
    bool migrated = false;
    if (!path.isEmpty() && !QFileInfo::exists(path)) {
        for (const QString &legacyPath : chatHistoryCompatibilityPaths()) {
            if (QFileInfo::exists(legacyPath)) {
                path = legacyPath;
                migrated = true;
                break;
            }
        }
    }
    if (path.isEmpty() || !QFileInfo::exists(path))
        return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return;
    int purged = 0;
    const QJsonObject root = forkmesh::relayrules::purgedChatHistory(
        QJsonDocument::fromJson(file.readAll()).object(), m_relayPrivatePurged, &purged);
    if (purged > 0)
        migrated = true;

    const qint64 loadedAtMs = QDateTime::currentMSecsSinceEpoch();

    const QJsonObject conversations = root.value("conversations").toObject();
    for (auto it = conversations.constBegin(); it != conversations.constEnd(); ++it) {
        const QString conversation = it.key();
        const qint64 expiryCutoff =
            forkmesh::chatretention::expiryCutoff(conversation, loadedAtMs);
        QList<ChatMessage> &dest = m_history[conversation];
        for (const QJsonValue &v : it.value().toArray()) {
            const QJsonObject obj = v.toObject();
            ChatMessage m;
            m.id = obj.value("id").toString();
            m.conversation = conversation;
            m.threadRootId = obj.value("threadRootId").toString().left(96);
            m.senderId = obj.value("senderId").toString();
            m.senderName = obj.value("senderName").toString();
            m.text = obj.value("text").toString();
            m.timestampMs = obj.value("ts").toVariant().toLongLong();
            m.self = obj.value("self").toBool();
            m.edited = obj.value("edited").toBool();
            m.deleted = obj.value("deleted").toBool();
            m.recipientCount = forkmesh::recipients::countFromValue(
                obj.value("recipients"));
            m.recipientNames = forkmesh::recipients::namesFromValue(
                obj.value("recipientNames"));
            m.receipt = forkmesh::recipients::receiptFromName(
                obj.value("receipt").toString());
            m.historical = true;
            if (forkmesh::chatblocks::senderMayFormat(m.senderId))
                m.blocks = forkmesh::chatblocks::sanitized(obj.value("blocks"));
            m.fileName = obj.value("fileName").toString();
            m.fileMime = obj.value("fileMime").toString();
            if (obj.contains("fileData"))
                m.fileData = QByteArray::fromBase64(
                    obj.value("fileData").toString().toLatin1());
            if (m.timestampMs <= expiryCutoff) {
                if (!m.id.isEmpty())
                    m_historyIds.insert(m.id);
                continue;
            }
            if (!m.id.isEmpty()) {
                if (m_historyIds.contains(m.id))
                    continue;
                m_historyIds.insert(m.id);
            }
            dest.append(m);
        }
    }

    const QJsonObject dmNames = root.value("dmNames").toObject();
    for (auto it = dmNames.constBegin(); it != dmNames.constEnd(); ++it)
        m_dmNames.insert(it.key(), it.value().toString());
    for (const QJsonValue &v : root.value("openDms").toArray()) {
        const QString peer = v.toString();
        if (!peer.isEmpty() && !m_openDms.contains(peer))
            m_openDms.append(peer);
    }
    refreshDmList();

    const QJsonObject unreadObj = root.value("unread").toObject();
    for (auto it = unreadObj.constBegin(); it != unreadObj.constEnd(); ++it) {
        const QString conv = it.key();
        const int count = it.value().toInt();
        if (!conv.isEmpty() && count > 0) {
            m_unread.insert(conv);
            m_unreadCounts[conv] = count;
        }
    }
    // A counter restored from disk can outlive the messages it counted: the
    // retention sweep above drops anything older than a week, and the save cap
    // keeps only the tail of a long conversation. Left alone, that is the badge
    // that greets a relaunch with unread messages no conversation can show.
    reconcileUnreadWithHistory();
    if (!m_unread.isEmpty()) {
        refreshChannelList();
        refreshDmList();
    }
    refreshChatListPreviews();
    updateChatButton();

    const QString current = root.value("current").toString();
    if (!current.isEmpty() && current != m_currentConversation &&
        m_history.contains(current)) {
        switchConversation(current);
    } else if (!m_currentConversation.isEmpty()) {
        rebuildConversationView();
    }

    if (migrated)
        saveChatHistory();
}

void MainWindow::scheduleChatSave()
{
    if (!m_chatSaveTimer) {
        m_chatSaveTimer = new QTimer(this);
        m_chatSaveTimer->setSingleShot(true);
        connect(m_chatSaveTimer, &QTimer::timeout, this,
                &MainWindow::saveChatHistory);
    }
    m_chatSaveTimer->start(1500);
}

void MainWindow::pruneExpiredChatHistory()
{
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    bool changedCurrent = false;
    bool changedAny = false;
    for (auto it = m_history.begin(); it != m_history.end(); ++it) {
        QList<ChatMessage> &messages = it.value();
        const qint64 cutoff =
            forkmesh::chatretention::expiryCutoff(it.key(), nowMs);
        bool changed = false;
        while (!messages.isEmpty() && messages.first().timestampMs <= cutoff) {
            messages.removeFirst();
            changed = true;
        }
        if (changed) {
            changedAny = true;
            if (it.key() == m_currentConversation)
                changedCurrent = true;
        }
    }
    const bool unreadChanged = reconcileUnreadWithHistory();
    if (!changedAny && !unreadChanged)
        return;
    if (unreadChanged) {
        refreshChannelList();
        refreshDmList();
    }
    updateChatButton();
    if (changedCurrent)
        rebuildConversationView();
    scheduleChatSave();
}

// An unread count only means something while the messages it counted are still
// here to read. Expiry and the save cap both drop messages without touching the
// counter, so clamp each one to the messages the conversation can still show
// from somebody else, and drop the counters left pointing at nothing.
bool MainWindow::reconcileUnreadWithHistory()
{
    bool changed = false;
    const QStringList counted = m_unreadCounts.keys();
    for (const QString &conversation : counted) {
        int readable = 0;
        const auto messages = m_history.constFind(conversation);
        if (messages != m_history.constEnd()) {
            for (const ChatMessage &message : *messages) {
                if (!message.self)
                    ++readable;
            }
        }
        if (readable <= 0) {
            m_unreadCounts.remove(conversation);
            m_unread.remove(conversation);
            changed = true;
        } else if (m_unreadCounts.value(conversation) > readable) {
            m_unreadCounts[conversation] = readable;
            changed = true;
        }
    }
    // The sidebar dot is the same phantom seen from the other side: a marker
    // with no counter behind it marks a conversation with nothing unread.
    const QSet<QString> marked = m_unread;
    for (const QString &conversation : marked) {
        if (m_unreadCounts.value(conversation) <= 0) {
            m_unread.remove(conversation);
            m_unreadCounts.remove(conversation);
            changed = true;
        }
    }
    return changed;
}


QWidget *MainWindow::buildSetupPage()
{
    auto *page = new QWidget;

    auto *card = new QWidget;
    card->setObjectName("setupCard");
    card->setFixedWidth(420);

    auto *title = new QLabel("<span style='color:#22c55e'>Fork</span>Mesh");
    title->setObjectName("appTitle");
    title->setAlignment(Qt::AlignHCenter);
    auto *subtitle = new QLabel(
        "Pick a name and join the mesh \xE2\x80\x94 preserve code, mirror repos, and chat.");
    subtitle->setObjectName("appSubtitle");
    subtitle->setAlignment(Qt::AlignHCenter);
    subtitle->setWordWrap(true);
    auto *versionLabel = new QLabel("v" FORKMESH_VERSION);
    versionLabel->setObjectName("versionLabel");
    versionLabel->setAlignment(Qt::AlignHCenter);

    m_nameEdit = new QLineEdit;
    m_nameEdit->setPlaceholderText("Pick a username (e.g. ada-lovelace)");
    m_nameEdit->setMaxLength(63);
    m_nameEdit->setText(savedProfileName());
    m_nameEdit->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("[A-Za-z0-9-]*")), m_nameEdit));
    auto *nameHint = new QLabel(
        "Your username on the mesh \xE2\x80\x94 letters, numbers and hyphens, no spaces.");
    nameHint->setObjectName("modeHint");
    nameHint->setWordWrap(true);
    m_solanaEdit = new QLineEdit(card);
    m_solanaEdit->setMaxLength(64);
    const QString storedSolana = savedSolanaAddress();
    if (!storedSolana.isEmpty() &&
        !forkmesh::control::isValidSolanaPublicAddress(storedSolana)) {
        saveSolanaAddress(QString());
        m_solanaEdit->clear();
    } else {
        m_solanaEdit->setText(storedSolana);
    }
    m_solanaEdit->hide();
    // The node's public key is no longer surfaced on the welcome screen (it kept
    // the first run feeling technical). The label is kept as a hidden data-holder
    // so the code that fills it after identity load keeps working unchanged.
    m_pubkeyLabel = new QLabel("Ed25519 public key: generating...");
    m_pubkeyLabel->setObjectName("modeHint");
    m_pubkeyLabel->setWordWrap(true);
    m_pubkeyLabel->hide();

    auto *serverLabel = new QLabel("Relay server");
    serverLabel->setObjectName("modeHint");
    m_serverUrlEdit = new QLineEdit;
    m_serverUrlEdit->setPlaceholderText(serverHostDisplay(kDefaultServerUrl));
    m_serverUrlEdit->setMaxLength(2048);
    m_serverUrlEdit->setObjectName("relayHostEdit");
    const QString savedServerUrl = QSettings().value(kServerUrlSetting).toString().trimmed();
    const QString effectiveServerUrl =
        savedServerUrl.isEmpty() || savedServerUrl == kLocalServerUrl
            ? kDefaultServerUrl
            : savedServerUrl;
    m_serverUrlEdit->setText(serverHostDisplay(effectiveServerUrl));

    m_roomNameEdit = new QLineEdit(card);
    m_roomNameEdit->setMaxLength(80);
    m_roomNameEdit->setText(kDefaultRoomName);
    m_roomNameEdit->setReadOnly(true);
    m_roomNameEdit->hide();

    auto *mainnodeHint = new QLabel(
        "Mainnodes relay encrypted repository-room ciphertext only.");
    mainnodeHint->setObjectName("modeHint");

    m_setupError = new QLabel;
    m_setupError->setWordWrap(true);
    m_setupError->setStyleSheet(Theme::remapSheet("color:#ff6b6b; background:transparent;"));
    m_setupError->hide();

    auto *startButton = new QPushButton("Start ForkMesh node");
    startButton->setObjectName("primaryButton");
    startButton->setMinimumHeight(40);

    m_updateButton = new QPushButton(card);
    m_updateButton->hide();
    m_updateStatus = new QLabel(card);
    m_updateStatus->setObjectName("modeHint");
    m_updateStatus->setWordWrap(true);
    m_updateStatus->setAlignment(Qt::AlignHCenter);
    m_updateStatus->hide();

    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(28, 28, 28, 28);
    cardLayout->setSpacing(10);
    cardLayout->addWidget(title);
    cardLayout->addWidget(subtitle);
    cardLayout->addWidget(versionLabel);
    cardLayout->addSpacing(14);
    cardLayout->addWidget(m_nameEdit);
    cardLayout->addWidget(nameHint);
    cardLayout->addSpacing(8);
    cardLayout->addWidget(serverLabel);
    cardLayout->addWidget(m_serverUrlEdit);
    cardLayout->addWidget(mainnodeHint);
    cardLayout->addSpacing(8);
    cardLayout->addWidget(m_setupError);
    cardLayout->addSpacing(10);
    cardLayout->addWidget(startButton);

    auto *setupContent = new QWidget;
    auto *setupContentLayout = new QVBoxLayout(setupContent);
    setupContentLayout->addStretch();
    auto *cardRow = new QHBoxLayout;
    cardRow->addStretch();
    cardRow->addWidget(card);
    cardRow->addStretch();
    setupContentLayout->addLayout(cardRow);
    setupContentLayout->addStretch();

    auto *setupScroll = new QScrollArea;
    setupScroll->setWidgetResizable(true);
    setupScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setupScroll->setFrameShape(QFrame::NoFrame);
    setupScroll->setMinimumHeight(0);
    setupScroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    setupScroll->setWidget(setupContent);

    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *chrome = new WindowChromeBar;
    auto *chromeRow = new QHBoxLayout(chrome);
    chromeRow->setContentsMargins(14, 0, 8, 0);
    chromeRow->setSpacing(8);
    auto *chromeTitle = new QLabel(QStringLiteral("ForkMesh v" FORKMESH_VERSION));
    chromeTitle->setObjectName("appVersionLabel");
    chromeRow->addWidget(chromeTitle);
    chromeRow->addStretch();
    auto makeWindowButton = [this](QStyle::StandardPixmap icon, const QString &tip) {
        auto *button = new QPushButton;
        button->setObjectName(QStringLiteral("windowChromeButton"));
        button->setCursor(Qt::PointingHandCursor);
        button->setFixedSize(32, 30);
        button->setToolTip(tip);
        button->setIcon(style()->standardIcon(icon));
        button->setIconSize(QSize(14, 14));
        return button;
    };
    auto *minimizeButton = makeWindowButton(QStyle::SP_TitleBarMinButton,
                                            QStringLiteral("Minimize"));
    connect(minimizeButton, &QPushButton::clicked, this, &MainWindow::showMinimized);
    auto *maximizeButton = makeWindowButton(QStyle::SP_TitleBarMaxButton,
                                            QStringLiteral("Maximize / restore"));
    connect(maximizeButton, &QPushButton::clicked, this, [this] {
        if (isMaximized())
            showNormal();
        else
            showMaximized();
    });
    auto *closeButton = makeWindowButton(QStyle::SP_TitleBarCloseButton,
                                         QStringLiteral("Close"));
    closeButton->setObjectName(QStringLiteral("windowChromeCloseButton"));
    connect(closeButton, &QPushButton::clicked, this, &MainWindow::close);
    chromeRow->addWidget(minimizeButton);
    chromeRow->addWidget(maximizeButton);
    chromeRow->addWidget(closeButton);
    layout->addWidget(chrome);
    layout->addWidget(setupScroll);

    connect(startButton, &QPushButton::clicked, this, &MainWindow::startSession);
    connect(m_nameEdit, &QLineEdit::returnPressed, this, &MainWindow::startSession);
    connect(m_nameEdit, &QLineEdit::textEdited, this, [this](const QString &name) {
        const QString lower = name.toLower();
        if (lower != name) {
            const int pos = m_nameEdit->cursorPosition();
            QSignalBlocker block(m_nameEdit);
            m_nameEdit->setText(lower);
            m_nameEdit->setCursorPosition(pos);
        }
        if (m_setupError)
            m_setupError->hide();
        saveProfileName(lower);
    });
    connect(m_serverUrlEdit, &QLineEdit::textEdited, this, [](const QString &host) {
        QSettings().setValue(kServerUrlSetting, canonicalServerUrl(host));
        forgetConfiguredRelayHost();
    });

    return page;
}

#ifdef FORKMESH_WINDOW_TESTS
void MainWindow::testSetSetupInputs(const QString &name, const QString &solana)
{
    if (m_nameEdit)
        m_nameEdit->setText(name);
    if (m_solanaEdit)
        m_solanaEdit->setText(solana);
    QSettings().setValue(kSolanaSetting, solana.trimmed());
}

int MainWindow::testStackIndex() const
{
    return m_stack ? m_stack->currentIndex() : -1;
}

bool MainWindow::testSignInButtonVisible() const
{
    return m_navSignInButton && !m_navSignInButton->isHidden();
}

QString MainWindow::testSavedSolanaAddress() const
{
    return QSettings().value(kSolanaSetting).toString().trimmed();
}

bool MainWindow::testColumnsBecomeResizable()
{
    QTableWidget table(0, 4);
    QHeaderView *header = table.horizontalHeader();
    header->setSectionResizeMode(0, QHeaderView::Stretch);
    header->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(3, QHeaderView::Fixed);
    makeColumnsResizable(&table);

    if (header->sectionResizeMode(1) != QHeaderView::ResizeToContents ||
        header->sectionResizeMode(2) != QHeaderView::ResizeToContents)
        return false;

    table.insertRow(0);
    table.setItem(0, 0, new QTableWidgetItem(QStringLiteral("flex")));
    table.setItem(0, 1, new QTableWidgetItem(QStringLiteral("short")));
    table.setItem(0, 2,
                  new QTableWidgetItem(QStringLiteral("a much wider cell value")));
    table.setItem(0, 3, new QTableWidgetItem(QStringLiteral("x")));
    QApplication::processEvents();
    QApplication::processEvents();

    const bool flexDraggable = header->sectionResizeMode(0) == QHeaderView::Interactive;
    const bool fixedUntouched = header->sectionResizeMode(3) == QHeaderView::Fixed;
    const bool col1Draggable = header->sectionResizeMode(1) == QHeaderView::Interactive;
    const bool col2Draggable = header->sectionResizeMode(2) == QHeaderView::Interactive;
    const bool widthsPreserved = header->sectionSize(2) > header->sectionSize(1);
    return flexDraggable && fixedUntouched && col1Draggable && col2Draggable &&
           widthsPreserved;
}

bool MainWindow::testSpreadsheetResize()
{
    QTableWidget table(0, 5);
    QHeaderView *header = table.horizontalHeader();
    header->setSectionResizeMode(0, QHeaderView::ResizeToContents); // #
    header->setSectionResizeMode(1, QHeaderView::Stretch);          // Title (flex)
    for (int i = 2; i < 5; ++i)
        header->setSectionResizeMode(i, QHeaderView::ResizeToContents);
    makeColumnsResizable(&table);
    table.resize(900, 200);

    table.insertRow(0);
    table.setItem(0, 0, new QTableWidgetItem(QStringLiteral("1")));
    table.setItem(0, 1, new QTableWidgetItem(QStringLiteral("a flexible title")));
    table.setItem(0, 2, new QTableWidgetItem(QStringLiteral("status value")));
    table.setItem(0, 3,
                  new QTableWidgetItem(QStringLiteral("a wider neighbour cell")));
    table.setItem(0, 4, new QTableWidgetItem(QStringLiteral("tail value")));
    QApplication::processEvents();
    QApplication::processEvents();

    const bool flexDraggable =
        header->sectionResizeMode(1) == QHeaderView::Interactive;

    const int before2 = header->sectionSize(2);
    const int before3 = header->sectionSize(3);
    const int before4 = header->sectionSize(4);
    const int delta = 24;
    header->resizeSection(2, before2 + delta);
    QApplication::processEvents();

    const bool draggedGrew = header->sectionSize(2) == before2 + delta;
    const bool neighborUntouched = header->sectionSize(3) == before3;
    const bool tailUntouched = header->sectionSize(4) == before4;
    return flexDraggable && draggedGrew && neighborUntouched && tailUntouched;
}

bool MainWindow::testSpreadsheetResizeAfterMove()
{
    QTableWidget table(0, 3);
    QHeaderView *header = table.horizontalHeader();
    header->setSectionsMovable(true);
    for (int i = 0; i < 3; ++i)
        header->setSectionResizeMode(i, QHeaderView::ResizeToContents);
    makeColumnsResizable(&table);
    table.resize(600, 200);

    table.insertRow(0);
    table.setItem(0, 0, new QTableWidgetItem(QStringLiteral("alpha value")));
    table.setItem(0, 1, new QTableWidgetItem(QStringLiteral("beta value")));
    table.setItem(0, 2, new QTableWidgetItem(QStringLiteral("gamma value")));
    QApplication::processEvents();
    QApplication::processEvents();

    header->moveSection(header->visualIndex(0), 2);

    const int before1 = header->sectionSize(1);
    const int before2 = header->sectionSize(2);
    const int before0 = header->sectionSize(0);
    const int delta = 20;
    header->resizeSection(1, before1 + delta);
    QApplication::processEvents();

    const bool draggedGrew = header->sectionSize(1) == before1 + delta;
    const bool neighborUntouched = header->sectionSize(2) == before2;
    const bool movedColumnUntouched = header->sectionSize(0) == before0;
    return draggedGrew && neighborUntouched && movedColumnUntouched;
}

bool MainWindow::testAgentListChromeHidden() const
{
    return m_agentTable && m_agentTable->horizontalHeader()->isHidden() &&
           m_agentTable->frameShape() == QFrame::NoFrame;
}

QString MainWindow::testAgentColumnLayout() const
{
    if (!m_agentTable)
        return QString();
    QHeaderView *header = m_agentTable->horizontalHeader();
    QStringList labels;
    for (int c = 0; c < m_agentTable->columnCount(); ++c)
        labels << m_agentTable->horizontalHeaderItem(c)->text();
    return QStringLiteral("%1|%2/%3")
        .arg(labels.join(QLatin1Char(',')))
        .arg(header->length())
        .arg(m_agentTable->viewport()->width());
}

QString MainWindow::testQuickAddAgentProvider() const
{
    return m_quickAddAgentProvider ? m_quickAddAgentProvider->currentData().toString()
                                   : QString();
}

QString MainWindow::testIssueAgentProvider() const
{
    return m_issueAgentProvider ? m_issueAgentProvider->currentData().toString()
                                : QString();
}

QString MainWindow::testIssueAgentModel() const
{
    return selectedModelComboValue(m_issueAgentModel);
}

void MainWindow::testSetQuickAddAgentProvider(const QString &provider)
{
    if (!m_quickAddAgentProvider)
        return;
    const int index = m_quickAddAgentProvider->findData(provider);
    m_quickAddAgentProvider->setCurrentIndex(index >= 0 ? index : 0);
}

QStringList MainWindow::testQuickAddModelLabels() const
{
    QStringList labels;
    if (!m_quickAddClaudeModel)
        return labels;
    for (int i = 0; i < m_quickAddClaudeModel->count(); ++i)
        labels << m_quickAddClaudeModel->itemText(i);
    return labels;
}

bool MainWindow::testQuickAddModelVisible() const
{
    return m_quickAddClaudeModel && m_quickAddClaudeModel->isVisible();
}

bool MainWindow::testQuickAddModelEditable() const
{
    return m_quickAddClaudeModel && m_quickAddClaudeModel->isEditable();
}

QString MainWindow::testWorktreeAheadBehindText(const QString &branch) const
{
    if (!m_worktreesTable)
        return QString();
    for (int row = 0; row < m_worktreesTable->rowCount(); ++row) {
        QTableWidgetItem *b = m_worktreesTable->item(row, 0);
        if (b && b->data(Qt::UserRole).toString() == branch) {
            if (QTableWidgetItem *ab = m_worktreesTable->item(row, 3))
                return ab->text();
        }
    }
    return QString();
}

QStringList MainWindow::testWorktreeBranches() const
{
    QStringList branches;
    if (!m_worktreesTable)
        return branches;
    for (int row = 0; row < m_worktreesTable->rowCount(); ++row) {
        if (QTableWidgetItem *item = m_worktreesTable->item(row, 0))
            branches << item->data(Qt::UserRole).toString();
    }
    return branches;
}

QString MainWindow::testWorktreeBranchLabel() const
{
    return m_worktreeBranchLabel ? m_worktreeBranchLabel->text() : QString();
}

int MainWindow::testOverviewBodyPage() const
{
    return m_overviewBodyStack ? m_overviewBodyStack->currentIndex() : -1;
}

QStringList MainWindow::testOverviewRowNames() const
{
    QStringList names;
    for (int row = 0; m_overviewList && row < m_overviewList->topLevelItemCount();
         ++row) {
        // An entry row draws its name in a cell widget and keeps the path in
        // the row data; a message row ("Could not read files: …") is cell text.
        const QTreeWidgetItem *item = m_overviewList->topLevelItem(row);
        const QString path = item->data(0, Qt::UserRole).toString();
        names.append(path.isEmpty() ? item->text(0) : path);
    }
    return names;
}

QStringList MainWindow::testSourceControlPaths() const
{
    QStringList paths;
    if (!m_scmTree)
        return paths;
    for (int g = 0; g < m_scmTree->topLevelItemCount(); ++g) {
        QTreeWidgetItem *group = m_scmTree->topLevelItem(g);
        for (int i = 0; group && i < group->childCount(); ++i) {
            const QString path =
                group->child(i)->data(0, Qt::UserRole).toString();
            if (!path.isEmpty())
                paths.append(path);
        }
    }
    return paths;
}

QString MainWindow::testSourceControlDiffText() const
{
    return m_scmDiff ? m_scmDiff->toPlainText() : QString();
}

QString MainWindow::testScmCommitControlsState() const
{
    QStringList parts;
    const auto describe = [&parts](const QString &name, QPushButton *button) {
        parts << QStringLiteral("%1=%2").arg(
            name,
            !button ? QStringLiteral("missing")
                    : !button->isVisibleTo(button->window())
                          ? QStringLiteral("hidden")
                          : button->isEnabled() ? QStringLiteral("enabled")
                                                : QStringLiteral("disabled"));
    };
    describe(QStringLiteral("commit"), m_scmCommitButton);
    describe(QStringLiteral("commitPush"), m_scmCommitPushButton);
    describe(QStringLiteral("stagePush"), m_scmStageCommitPushButton);
    describe(QStringLiteral("sync"), m_scmSyncButton);
    return parts.join(QLatin1Char(' '));
}

bool MainWindow::testClickSourceControlPath(const QString &path)
{
    if (!m_scmTree)
        return false;
    m_lastSourceControlDiffPath.clear();
    for (int g = 0; g < m_scmTree->topLevelItemCount(); ++g) {
        QTreeWidgetItem *group = m_scmTree->topLevelItem(g);
        for (int i = 0; group && i < group->childCount(); ++i) {
            QTreeWidgetItem *item = group->child(i);
            if (item->data(0, Qt::UserRole).toString() != path)
                continue;
            m_scmTree->setCurrentItem(nullptr);
            m_scmTree->setCurrentItem(item);
            // Production queues the click while the working-tree patch is read
            // off-thread. The window test still promises its callers that a
            // successful return means the section has landed and been pinned.
            QElapsedTimer timer;
            timer.start();
            while (timer.elapsed() < 5000 &&
                   (!m_scmSectionPaths.contains(path) ||
                    !m_scmPendingScrollKey.isEmpty())) {
                QApplication::processEvents(QEventLoop::AllEvents, 20);
            }
            return m_lastSourceControlDiffPath == path;
        }
    }
    return false;
}

QString MainWindow::testArrowOnSourceControl(bool down)
{
    if (!m_scmTree)
        return QString();
    m_scmTree->setFocus();
    const Qt::Key key = down ? Qt::Key_Down : Qt::Key_Up;
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
    QApplication::sendEvent(m_scmTree, &press);
    QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
    QApplication::sendEvent(m_scmTree, &release);
    QApplication::processEvents();
    QTreeWidgetItem *item = m_scmTree->currentItem();
    return item ? item->data(0, Qt::UserRole).toString() : QString();
}

QString MainWindow::testBranchStickyHeaderText() const
{
    return m_branchStickyPath ? m_branchStickyPath->text() : QString();
}

QString MainWindow::testArrowOnWorktrees(bool down)
{
    if (!m_worktreesTable)
        return QString();
    m_worktreesTable->setFocus();
    const Qt::Key key = down ? Qt::Key_Down : Qt::Key_Up;
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
    QApplication::sendEvent(m_worktreesTable, &press);
    QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
    QApplication::sendEvent(m_worktreesTable, &release);
    QApplication::processEvents();
    const int row = m_worktreesTable->currentRow();
    QTableWidgetItem *b = row >= 0 ? m_worktreesTable->item(row, 0) : nullptr;
    return b ? b->data(Qt::UserRole).toString() : QString();
}

void MainWindow::testClickRepoDetailTab(int id)
{
    if (id == 1) {
        if (m_railGitButton)
            m_railGitButton->click();
        return;
    }
    if (id == 3) {
        showSection(0);
        ensureRepoDetailTabBuilt(id);
        if (m_repoDetailStack)
            m_repoDetailStack->setCurrentIndex(id);
        if (m_agentsNavButton)
            m_agentsNavButton->setChecked(true);
        return;
    }
    if (id == m_worktreesTabIndex) {
        showOverviewWorktrees();
        loadWorktreesPanel();
        focusRepoDetailTable(id);
        return;
    }
    if (id == m_branchesTabIndex) {
        showOverviewBranches();
        loadBranchesPanel();
        focusRepoDetailTable(id);
        return;
    }
    if (!m_repoDetailTabs)
        return;
    if (QAbstractButton *b = m_repoDetailTabs->button(id))
        b->click(); // emits idClicked(id) -> the same path a real click takes
}

bool MainWindow::testRepoDetailTabDeferred(int id) const
{
    if (!m_repoDetailStack || id < 0 || id >= m_repoDetailStack->count())
        return false;
    QWidget *page = m_repoDetailStack->widget(id);
    return page && page->property("forkmeshDeferredRepoTab").toBool();
}

bool MainWindow::testOpenMostRecentCommit()
{
    if (!m_commitsTable)
        return false;
    if (m_commitsTable->rowCount() == 0) {
        if (!m_commitsLoading)
            loadCommits();
        return false;
    }
    openMostRecentCommit();
    return true;
}

bool MainWindow::testWorktreesTableHasKeyboardFocus() const
{
    return m_worktreesTable && m_worktreesTable->window() &&
           m_worktreesTable->window()->focusWidget() == m_worktreesTable;
}

bool MainWindow::testReleasesTableHasKeyboardFocus() const
{
    return m_releasesTable && m_releasesTable->window() &&
           m_releasesTable->window()->focusWidget() == m_releasesTable;
}

bool MainWindow::testMirrorNodesTableHasKeyboardFocus() const
{
    return m_nodesTable && m_nodesTable->window() &&
           m_nodesTable->window()->focusWidget() == m_nodesTable;
}

QString MainWindow::testBranchWorktreePath(const QString &branch) const
{
    if (!m_branchesTable)
        return QString();
    for (int row = 0; row < m_branchesTable->rowCount(); ++row) {
        QTableWidgetItem *name =
            m_branchesTable->item(row, kBranchesNameColumn);
        if (name && name->text() == branch) {
            if (QTableWidgetItem *wt =
                    m_branchesTable->item(row, kBranchesWorktreeColumn))
                return wt->text();
        }
    }
    return QString();
}

bool MainWindow::testBranchAttachmentHasIcon(const QString &branch) const
{
    if (!m_branchesTable)
        return false;
    for (int row = 0; row < m_branchesTable->rowCount(); ++row) {
        QTableWidgetItem *name =
            m_branchesTable->item(row, kBranchesNameColumn);
        if (name && name->text() == branch)
            return !name->icon().isNull();
    }
    return false;
}

QStringList MainWindow::testBranchRowOrder() const
{
    QStringList names;
    if (!m_branchesTable)
        return names;
    for (int row = 0; row < m_branchesTable->rowCount(); ++row) {
        if (QTableWidgetItem *name =
                m_branchesTable->item(row, kBranchesNameColumn))
            names << name->text();
    }
    return names;
}

void MainWindow::testSetDefaultAgentProvider(const QString &provider)
{
    if (!m_defaultAgentProviderCombo)
        return;
    const int index = m_defaultAgentProviderCombo->findData(provider);
    m_defaultAgentProviderCombo->setCurrentIndex(index >= 0 ? index : 0);
}

int MainWindow::testAddPublishedRepository(const QString &owner, const QString &name,
                                           const QString &mirrorPath)
{
    RepositoryRecord repo;
    repo.owner = owner;
    repo.name = name;
    repo.mirrorPath = mirrorPath;
    repo.publishToNetwork = true;
    stampRepositoryRelay(repo);
    m_repositories.append(repo);
    return m_repositories.size() - 1;
}

// A private repository is never advertised to the relay, so it can only ever
// reach the Repos page through the private/local table.
int MainWindow::testAddPrivateLocalRepository(const QString &owner,
                                              const QString &name,
                                              const QString &localPath)
{
    RepositoryRecord repo;
    repo.owner = owner;
    repo.name = name;
    repo.localPath = localPath;
    repo.isPrivate = true;
    stampRepositoryRelay(repo);
    stampRepositorySyncIdentity(repo);
    m_repositories.append(repo);
    return m_repositories.size() - 1;
}

int MainWindow::testAddLocalRepository(const QString &owner, const QString &name,
                                       const QString &localPath)
{
    RepositoryRecord repo;
    repo.owner = owner;
    repo.name = name;
    repo.localPath = localPath;
    stampRepositoryRelay(repo);
    stampRepositorySyncIdentity(repo);
    m_repositories.append(repo);
    return m_repositories.size() - 1;
}

bool MainWindow::testOpenRepository(int index)
{
    if (index < 0 || index >= m_repositories.size())
        return false;
    m_selectedNode = m_repositories.at(index).owner;
    openRepoDetail(index);
    return true;
}

QString MainWindow::testRepoDefaultBranch() const
{
    return repoDefaultBranch(repoBranches());
}

QString MainWindow::testRepoActionsTabText() const
{
    if (!m_repoActionsTab)
        return QString();
    const auto *badged = dynamic_cast<VerticalIconButton *>(m_repoActionsTab);
    return QStringLiteral("%1 (%2)")
        .arg(m_repoActionsTab->text(),
             formatCount(badged ? badged->badgeCount() : 0));
}

QString MainWindow::testRepoBranchesButtonText() const
{
    if (!m_branchesButton)
        return QString();
    const auto *badged = dynamic_cast<VerticalIconButton *>(m_branchesButton);
    const qint64 count = badged ? badged->badgeCount() : 0;
    return QStringLiteral("%1 %2")
        .arg(formatCount(count),
             count == 1 ? QStringLiteral("branch") : QStringLiteral("branches"));
}

bool MainWindow::testShowRepoIssuesTab()
{
    if (!m_repoDetailStack || m_repoDetailStack->count() <= 2 ||
        !m_repoDetailTabs)
        return false;
    QAbstractButton *button = m_repoDetailTabs->button(2);
    if (!button)
        return false;
    button->click();
    return m_repoDetailStack->currentIndex() == 2;
}

int MainWindow::testRepoTabContentTop()
{
    return m_repoDetailStack ? m_repoDetailStack->mapTo(this, QPoint(0, 0)).y() : -1;
}

int MainWindow::testRepoTabGapAroundIssues() const
{
    if (!m_repoProjectsTab || !m_repoIssuesTab || !m_repoPullsTab)
        return -1;
    const QRect projectsRect(
        m_repoProjectsTab->mapTo(const_cast<MainWindow *>(this), QPoint(0, 0)),
        m_repoProjectsTab->size());
    const QRect issuesRect(m_repoIssuesTab->mapTo(const_cast<MainWindow *>(this),
                                                  QPoint(0, 0)),
                           m_repoIssuesTab->size());
    const QRect pullsRect(m_repoPullsTab->mapTo(const_cast<MainWindow *>(this),
                                                QPoint(0, 0)),
                          m_repoPullsTab->size());
    return qMin(projectsRect.left() - issuesRect.right() - 1,
                pullsRect.left() - projectsRect.right() - 1);
}

int MainWindow::testRailTabIconLineSkew() const
{
    if (!m_railCodeButton || !m_repoIssuesTab)
        return -100000;
    auto *self = const_cast<MainWindow *>(this);
    if (!m_railCodeButton->isVisibleTo(self) || !m_repoIssuesTab->isVisibleTo(self))
        return -100000;
    return m_railCodeButton->mapTo(self, QPoint(0, 0)).y() -
           m_repoIssuesTab->mapTo(self, QPoint(0, 0)).y();
}

int MainWindow::testIssueLooperGapFromNewIssueButton() const
{
    if (!m_looperToggle || !m_issueListNewButton ||
        !m_looperToggle->isVisible())
        return -100000;
    const QRect looperRect(m_looperToggle->mapTo(const_cast<MainWindow *>(this),
                                                 QPoint(0, 0)),
                           m_looperToggle->size());
    const QRect newIssueRect(
        m_issueListNewButton->mapTo(const_cast<MainWindow *>(this), QPoint(0, 0)),
        m_issueListNewButton->size());
    return looperRect.left() - newIssueRect.right() - 1;
}

bool MainWindow::testIssueLooperRowAligned() const
{
    if (!m_looperToggle || !m_issueListNewButton ||
        !m_looperToggle->isVisible())
        return false;
    const QRect looperRect(m_looperToggle->mapTo(const_cast<MainWindow *>(this),
                                                 QPoint(0, 0)),
                           m_looperToggle->size());
    const QRect newIssueRect(
        m_issueListNewButton->mapTo(const_cast<MainWindow *>(this), QPoint(0, 0)),
        m_issueListNewButton->size());
    return qAbs(looperRect.center().y() - newIssueRect.center().y()) <= 8;
}

int MainWindow::testTopNavTrailingGap() const
{
    if (QWidget *rail =
            findChild<QWidget *>(QStringLiteral("appNavigationRail"))) {
        const QRect rect(rail->mapTo(const_cast<MainWindow *>(this), QPoint(0, 0)),
                         rail->size());
        return rect.left();
    }
    int rightEdge = -1;
    for (QWidget *widget :
         {static_cast<QWidget *>(m_navScreenshotButton),
          static_cast<QWidget *>(m_navResizeButton),
          static_cast<QWidget *>(m_navRebuildButton)}) {
        if (!widget || !widget->isVisibleTo(const_cast<MainWindow *>(this)))
            continue;
        const QRect rect(widget->mapTo(const_cast<MainWindow *>(this), QPoint(0, 0)),
                         widget->size());
        rightEdge = qMax(rightEdge, rect.right());
    }
    return rightEdge >= 0 ? width() - rightEdge - 1 : -1;
}

void MainWindow::testSetDirectoryUserNodes(const QString &user,
                                           const QStringList &nodes)
{
    MemberInfo member;
    member.id = QStringLiteral("user:") + user.toLower();
    member.name = user;
    member.ownerUser = user;
    member.accountKind = QStringLiteral("user");
    member.nodeName = nodes.join(QStringLiteral(", "));
    m_chatDirectoryUsers.insert(user.toLower(), member);
    m_chatDirectoryLoaded = true;
}

void MainWindow::testShowNodesSection()
{
    showSection(kNodesSectionIndex);
    refreshNodesTable();
}

QStringList MainWindow::testNodeDirectoryNames() const
{
    QStringList names;
    if (!m_nodesTable)
        return names;
    for (int row = 0; row < m_nodesTable->rowCount(); ++row) {
        QTableWidgetItem *item = m_nodesTable->item(row, 0);
        if (item && !item->data(kNodeGroupChildRole).toBool())
            names.append(item->data(Qt::UserRole).toString());
    }
    names.sort(Qt::CaseInsensitive);
    return names;
}

void MainWindow::testAddMirrorNodeRow(const QString &node,
                                      const QString &commit)
{
    if (!m_mirrorNodesTable)
        return;
    const int row = m_mirrorNodesTable->rowCount();
    m_mirrorNodesTable->insertRow(row);
    auto *name = new QTableWidgetItem(node);
    name->setData(kMirrorNodeKeysRole, QStringList{node.trimmed().toLower()});
    name->setData(kMirrorNodeOnlineRole, true);
    m_mirrorNodesTable->setItem(row, MirrorNodeColNode, name);
    m_mirrorNodesTable->setItem(row, MirrorNodeColCommit,
                                new QTableWidgetItem(commit));
}

QStringList MainWindow::testNodeDirectoryRowTexts() const
{
    QStringList texts;
    if (!m_nodesTable)
        return texts;
    for (int row = 0; row < m_nodesTable->rowCount(); ++row) {
        const QTableWidgetItem *item = m_nodesTable->item(row, 0);
        texts.append(item ? item->text() : QString());
    }
    return texts;
}

void MainWindow::testSortNodeDirectory(int column, Qt::SortOrder order)
{
    if (m_nodesTable)
        m_nodesTable->horizontalHeader()->setSortIndicator(column, order);
}

// Read one node directory cell by its header caption, so a test names the
// column the way the user reads it instead of tracking a column index.
QString MainWindow::testNodeDirectoryCell(const QString &node,
                                          const QString &columnCaption) const
{
    if (!m_nodesTable)
        return QString();
    int column = -1;
    for (int c = 0; c < m_nodesTable->columnCount(); ++c) {
        QTableWidgetItem *header = m_nodesTable->horizontalHeaderItem(c);
        if (header && header->text().compare(columnCaption,
                                             Qt::CaseInsensitive) == 0) {
            column = c;
            break;
        }
    }
    if (column < 0)
        return QString();
    for (int row = 0; row < m_nodesTable->rowCount(); ++row) {
        QTableWidgetItem *nameItem = m_nodesTable->item(row, 0);
        if (!nameItem ||
            nameItem->data(Qt::UserRole).toString().compare(
                node, Qt::CaseInsensitive) != 0)
            continue;
        QTableWidgetItem *cell = m_nodesTable->item(row, column);
        return cell ? cell->text() : QString();
    }
    return QString();
}

bool MainWindow::testRepoDetailTabButtonExists(int id) const
{
    return m_repoDetailTabs && m_repoDetailTabs->button(id);
}

bool MainWindow::testNetworkNodesTabShowing() const
{
    return m_sectionStack &&
           m_sectionStack->currentIndex() == kNetworkDiagnosticsSectionIndex &&
           m_networkTabs && m_networkTabs->currentIndex() == kNetworkNodesTab;
}

QStringList MainWindow::testNodeDirectoryHeaderCaptions() const
{
    QStringList captions;
    if (!m_nodesTable)
        return captions;
    for (int c = 0; c < m_nodesTable->columnCount(); ++c)
        if (QTableWidgetItem *header = m_nodesTable->horizontalHeaderItem(c))
            captions.append(header->text());
    return captions;
}

void MainWindow::testSetNodesRoleFilter(const QString &roleKey)
{
    if (!m_nodesRoleFilter)
        return;
    const int index = m_nodesRoleFilter->findData(roleKey);
    m_nodesRoleFilter->setCurrentIndex(qMax(0, index));
    applyNodeRoleFilter();
}

QString MainWindow::testNodesRoleFilterKey() const
{
    return m_nodesRoleFilter ? m_nodesRoleFilter->currentData().toString()
                             : QString();
}

QStringList MainWindow::testNodesRoleFilterKeys() const
{
    QStringList keys;
    if (m_nodesRoleFilter)
        for (int i = 0; i < m_nodesRoleFilter->count(); ++i)
            keys.append(m_nodesRoleFilter->itemData(i).toString());
    return keys;
}

void MainWindow::testSetRelayOnlineNodes(const QStringList &nodes)
{
    m_relayOnlineNodes.clear();
    for (const QString &node : nodes)
        m_relayOnlineNodes.insert(node.trimmed().toLower());
    m_relayOnlineNodesFetched = true;
    m_relayOnlineNodesFetchedMs = QDateTime::currentMSecsSinceEpoch();
}

void MainWindow::testSetVultrFleetJson(const QByteArray &instancesJson)
{
    m_vultrFleetNodes = forkmesh::corelogs::parseVultrInstances(instancesJson);
}

void MainWindow::testSetRunnersPayload(const QJsonObject &payload)
{
    m_runners = forkmesh::runners::parseRunners(payload);
    m_runnersLoaded = true;
    setRunnerAttention(forkmesh::runners::runnerAttentionFromRunners(m_runners));
}

void MainWindow::testSetDirectoryInstances(const QJsonArray &instances)
{
    m_directoryInstances = instances;
    m_directoryInstancesFetchedMs = QDateTime::currentMSecsSinceEpoch();
}

QStringList MainWindow::testNodeDirectoryVisualCaptions() const
{
    QStringList captions;
    if (!m_nodesTable)
        return captions;
    const QHeaderView *header = m_nodesTable->horizontalHeader();
    for (int visual = 0; visual < header->count(); ++visual)
        if (QTableWidgetItem *item = m_nodesTable->horizontalHeaderItem(header->logicalIndex(visual)))
            captions.append(item->text());
    return captions;
}

QStringList MainWindow::testRelayRowNames() const
{
    QStringList names;
    if (!m_nodesTable)
        return names;
    for (int row = 0; row < m_nodesTable->rowCount(); ++row) {
        const QTableWidgetItem *item = m_nodesTable->item(row, 0);
        if (item && item->data(Qt::UserRole + 5).toString() == QLatin1String("relay"))
            names.append(item->data(Qt::UserRole).toString());
    }
    return names;
}

void MainWindow::testSetRelayProbe(const QString &relayName, const QString &state,
                                   int latencyMs, const QString &version,
                                   const QString &failure)
{
    for (const ServerConfig &server : std::as_const(m_servers)) {
        const QString host = serverHost(server.url);
        const QString name = server.name.trimmed().isEmpty()
                                 ? (host.isEmpty() ? server.url : host)
                                 : server.name.trimmed();
        if (name != relayName)
            continue;
        forkmesh::nodedir::RelayProbe probe;
        probe.state = state;
        probe.latencyMs = latencyMs;
        probe.version = version;
        probe.failure = failure;
        probe.updatedMs = QDateTime::currentMSecsSinceEpoch();
        m_relayProbes.insert(host, probe);
    }
    m_relayProbesInFlight = 0;
    m_relayProbesUpdatedAt = QTime::currentTime().toString(QStringLiteral("HH:mm:ss"));
    updateRelayNodeRows();
}

QString MainWindow::testNodeDirectoryCellToolTip(const QString &node,
                                                 const QString &columnCaption) const
{
    if (!m_nodesTable)
        return QString();
    for (int c = 0; c < m_nodesTable->columnCount(); ++c) {
        const QTableWidgetItem *header = m_nodesTable->horizontalHeaderItem(c);
        if (!header || header->text().compare(columnCaption, Qt::CaseInsensitive) != 0)
            continue;
        for (int row = 0; row < m_nodesTable->rowCount(); ++row) {
            const QTableWidgetItem *name = m_nodesTable->item(row, 0);
            if (!name || name->data(Qt::UserRole).toString().compare(
                             node, Qt::CaseInsensitive) != 0)
                continue;
            const QTableWidgetItem *cell = m_nodesTable->item(row, c);
            return cell ? cell->toolTip() : QString();
        }
    }
    return QString();
}

void MainWindow::testActivateNodeDirectoryAction(const QString &node)
{
    if (!m_nodesTable)
        return;
    for (int row = 0; row < m_nodesTable->rowCount(); ++row) {
        const QTableWidgetItem *name = m_nodesTable->item(row, 0);
        if (name && name->data(Qt::UserRole).toString().compare(
                        node, Qt::CaseInsensitive) == 0) {
            activateNodeDirectoryRow(row);
            return;
        }
    }
}

QStringList MainWindow::testNodeLogFilterNodes() const
{
    QStringList nodes(m_nodeLogFilter.nodes.cbegin(), m_nodeLogFilter.nodes.cend());
    nodes.sort();
    return nodes;
}

void MainWindow::testClearNodeLogFilter()
{
    m_nodeLogFilter = {};
    updateNodeLogFilterButton();
}

bool MainWindow::testLogSectionShowing() const
{
    return m_sectionStack && m_sectionStack->currentIndex() == 4;
}

QString MainWindow::testNodesStatusText() const
{
    return m_nodesStatus ? m_nodesStatus->text() : QString();
}

int MainWindow::testVisibleNodeDirectoryRows() const
{
    int visible = 0;
    if (!m_nodesTable)
        return visible;
    for (int row = 0; row < m_nodesTable->rowCount(); ++row)
        if (!m_nodesTable->isRowHidden(row))
            ++visible;
    return visible;
}

QStringList MainWindow::testNodeDirectoryHeadersWithoutIcons() const
{
    QStringList captions;
    if (!m_nodesTable)
        return captions;
    for (int c = 0; c < m_nodesTable->columnCount(); ++c) {
        QTableWidgetItem *header = m_nodesTable->horizontalHeaderItem(c);
        if (!header || header->icon().isNull())
            captions.append(header ? header->text()
                                   : QStringLiteral("(column %1)").arg(c));
    }
    return captions;
}

bool MainWindow::testUsersNavButtonVisible() const
{
    return m_usersNavButton && !m_usersNavButton->isHidden();
}

QStringList MainWindow::testUsersColumns() const
{
    QStringList labels;
    if (!m_usersTable)
        return labels;
    for (int column = 0; column < m_usersTable->columnCount(); ++column) {
        if (QTableWidgetItem *item =
                m_usersTable->horizontalHeaderItem(column))
            labels.append(item->text());
    }
    return labels;
}

QString MainWindow::testUsersCellText(int row, const QString &header) const
{
    if (!m_usersTable || row < 0 || row >= m_usersTable->rowCount())
        return QString();
    for (int column = 0; column < m_usersTable->columnCount(); ++column) {
        const QTableWidgetItem *label =
            m_usersTable->horizontalHeaderItem(column);
        if (!label || label->text() != header)
            continue;
        const QTableWidgetItem *item = m_usersTable->item(row, column);
        return item ? item->text() : QString();
    }
    return QString();
}

QStringList MainWindow::testUsersTabLabels() const
{
    QStringList labels;
    if (!m_usersTabs)
        return labels;
    for (int index = 0; index < m_usersTabs->count(); ++index)
        labels.append(m_usersTabs->tabText(index));
    return labels;
}

void MainWindow::testShowUsersTab(int index)
{
    if (!m_usersTabs || index < 0 || index >= m_usersTabs->count())
        return;
    m_usersTabs->setCurrentIndex(index);
}

QStringList MainWindow::testUsersQuarantineColumns() const
{
    QStringList labels;
    if (!m_usersQuarantineTable)
        return labels;
    for (int column = 0; column < m_usersQuarantineTable->columnCount();
         ++column) {
        if (QTableWidgetItem *item =
                m_usersQuarantineTable->horizontalHeaderItem(column))
            labels.append(item->text());
    }
    return labels;
}

QString MainWindow::testUsersQuarantineCellText(
    int row, const QString &header) const
{
    if (!m_usersQuarantineTable || row < 0 ||
        row >= m_usersQuarantineTable->rowCount())
        return QString();
    for (int column = 0; column < m_usersQuarantineTable->columnCount();
         ++column) {
        const QTableWidgetItem *label =
            m_usersQuarantineTable->horizontalHeaderItem(column);
        if (!label || label->text() != header)
            continue;
        const QTableWidgetItem *item =
            m_usersQuarantineTable->item(row, column);
        return item ? item->text() : QString();
    }
    return QString();
}

QString MainWindow::testUsersSignupReferralCode() const
{
    return m_usersSignupReferralCodeEdit
               ? m_usersSignupReferralCodeEdit->text()
               : QString();
}

QString MainWindow::testUsersSignupReferralCodeStatus() const
{
    return m_usersSignupReferralCodeStatus
               ? m_usersSignupReferralCodeStatus->text()
               : QString();
}

bool MainWindow::testUsersSignupReferralCodeCopyEnabled() const
{
    return m_usersSignupReferralCodeCopyButton &&
           m_usersSignupReferralCodeCopyButton->isEnabled();
}

void MainWindow::testSelectUsersRow(int row)
{
    if (!m_usersTable || row < 0 || row >= m_usersTable->rowCount())
        return;
    m_usersTable->setCurrentCell(
        row, 0,
        QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    refreshUsersTipButton();
    refreshUsersQuarantineButton();
    refreshUsersDeleteButton();
}

void MainWindow::testSelectUsersQuarantineRow(int row)
{
    if (!m_usersQuarantineTable || row < 0 ||
        row >= m_usersQuarantineTable->rowCount())
        return;
    m_usersQuarantineTable->setCurrentCell(
        row, 0,
        QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    refreshUsersUnquarantineButton();
}

bool MainWindow::testUsersTipButtonEnabled() const
{
    return m_usersTipButton && m_usersTipButton->isEnabled();
}

QString MainWindow::testUsersTipButtonToolTip() const
{
    return m_usersTipButton ? m_usersTipButton->toolTip() : QString();
}

bool MainWindow::testUsersQuarantineButtonEnabled() const
{
    return m_usersQuarantineButton && m_usersQuarantineButton->isEnabled();
}

QString MainWindow::testUsersQuarantineButtonText() const
{
    return m_usersQuarantineButton ? m_usersQuarantineButton->text()
                                   : QString();
}

QString MainWindow::testUsersQuarantineButtonToolTip() const
{
    return m_usersQuarantineButton
               ? m_usersQuarantineButton->toolTip()
               : QString();
}

bool MainWindow::testUsersUnquarantineButtonEnabled() const
{
    return m_usersUnquarantineButton &&
           m_usersUnquarantineButton->isEnabled();
}

QString MainWindow::testUsersUnquarantineButtonToolTip() const
{
    return m_usersUnquarantineButton
               ? m_usersUnquarantineButton->toolTip()
               : QString();
}

bool MainWindow::testUsersDeleteButtonEnabled() const
{
    return m_usersDeleteButton && m_usersDeleteButton->isEnabled();
}

QString MainWindow::testUsersDeleteButtonText() const
{
    return m_usersDeleteButton ? m_usersDeleteButton->text() : QString();
}

QString MainWindow::testUsersDeleteButtonToolTip() const
{
    return m_usersDeleteButton ? m_usersDeleteButton->toolTip() : QString();
}

QStringList MainWindow::testSortUsersBy(const QString &header,
                                        Qt::SortOrder order)
{
    QStringList names;
    if (!m_usersTable)
        return names;
    int sortColumn = -1;
    for (int column = 0; column < m_usersTable->columnCount(); ++column) {
        const QTableWidgetItem *item =
            m_usersTable->horizontalHeaderItem(column);
        if (item && item->text() == header) {
            sortColumn = column;
            break;
        }
    }
    if (sortColumn < 0)
        return names;
    m_usersTable->sortItems(sortColumn, order);
    for (int row = 0; row < m_usersTable->rowCount(); ++row) {
        if (const QTableWidgetItem *item = m_usersTable->item(row, 0))
            names.append(item->text());
    }
    return names;
}

QStringList MainWindow::testChatMemberNames(const QString &conversation)
{
    const QString saved = m_currentConversation;
    const bool menuWasOpen = m_chatMembersMenuOpen;
    m_currentConversation = conversation;
    m_chatMembersMenuOpen = true;
    refreshChatMembers();
    QStringList names;
    if (m_chatMembersLayout) {
        static const QRegularExpression tag(QStringLiteral("<[^>]*>"));
        for (int i = 0; i < m_chatMembersLayout->count(); ++i) {
            QWidget *card = m_chatMembersLayout->itemAt(i)->widget();
            if (!card)
                continue;
            if (auto *label = card->findChild<QLabel *>(
                    QStringLiteral("chatMemberName"))) {
                names.append(label->text()
                                 .remove(tag)
                                 .remove(QStringLiteral("(you)"))
                                 .trimmed());
            }
        }
    }
    m_currentConversation = saved;
    m_chatMembersMenuOpen = menuWasOpen;
    refreshChatMembers();
    names.sort(Qt::CaseInsensitive);
    return names;
}

QStringList MainWindow::testTeamRoomMembersWith(const QList<MemberInfo> &roster,
                                                const QList<ChatMessage> &messages,
                                                qint64 *elapsedMs)
{
    const QString team = QStringLiteral("#team");
    const QList<MemberInfo> savedRoster = m_homeRoster;
    const QList<ChatMessage> savedHistory = m_history.value(team);
    m_homeRoster = roster;
    m_history[team] = messages;
    QElapsedTimer clock;
    clock.start();
    const QStringList names = testChatMemberNames(team);
    if (elapsedMs)
        *elapsedMs = clock.elapsed();
    m_homeRoster = savedRoster;
    m_history[team] = savedHistory;
    return names;
}

void MainWindow::testRenderNetworkRepos(const QJsonArray &repos)
{
    showSection(kNetworkReposSectionIndex);
    ++m_networkReposLoadGen;
    renderNetworkRepos(repos);
}

QStringList MainWindow::testNetworkRepoNames() const
{
    QStringList names;
    if (!m_networkReposTable)
        return names;
    for (int row = 0; row < m_networkReposTable->rowCount(); ++row) {
        if (QTableWidgetItem *item = m_networkReposTable->item(row, 0))
            names.append(item->data(Qt::UserRole).toString());
    }
    return names;
}

QString MainWindow::testNetworkRepoActionText(int row) const
{
    if (!m_networkReposTable || row < 0 ||
        row >= m_networkReposTable->rowCount())
        return QString();
    // Found by shape rather than by position: the buttons are free to move to
    // whichever column the layout puts them in.
    for (int column = 0; column < m_networkReposTable->columnCount(); ++column) {
        QWidget *cell = m_networkReposTable->cellWidget(row, column);
        if (!cell)
            continue;
        const QList<QPushButton *> buttons = cell->findChildren<QPushButton *>(
            QString(), Qt::FindDirectChildrenOnly);
        if (!buttons.isEmpty())
            return buttons.first()->text();
    }
    return QString();
}

QString MainWindow::testNetworkRepoMirrorHeader() const
{
    if (!m_networkReposTable)
        return QString();
    // The mirror-count cell marks itself, so the column is found by what it
    // carries rather than by where it sits.
    for (int column = 0; column < m_networkReposTable->columnCount(); ++column) {
        QTableWidgetItem *item = m_networkReposTable->item(0, column);
        if (!item || !item->data(kRepoMirrorCellRole).toBool())
            continue;
        QTableWidgetItem *head =
            m_networkReposTable->horizontalHeaderItem(column);
        return head ? head->text() : QString();
    }
    return QString();
}

QStringList MainWindow::testNetworkTabLabels() const
{
    QStringList labels;
    if (m_networkTabs)
        for (int index = 0; index < m_networkTabs->count(); ++index)
            labels.append(m_networkTabs->tabText(index));
    return labels;
}

void MainWindow::testRenderNetworkWebRequests(const QJsonObject &payload)
{
    showSection(kNetworkDiagnosticsSectionIndex);
    if (m_networkTabs)
        m_networkTabs->setCurrentIndex(m_networkWebRequestsTabIndex);
    renderNetworkWebRequests(payload);
}

QStringList MainWindow::testNetworkWebRequestsGroups() const
{
    QStringList groups;
    if (!m_networkWebRequestsTable)
        return groups;
    for (int row = 0; row < m_networkWebRequestsTable->rowCount(); ++row) {
        if (QTableWidgetItem *item = m_networkWebRequestsTable->item(row, 0))
            groups.append(item->text());
    }
    return groups;
}

QString MainWindow::testNetworkWebRequestsCellText(int row,
                                                   const QString &header) const
{
    if (!m_networkWebRequestsTable)
        return QString();
    for (int column = 0; column < m_networkWebRequestsTable->columnCount();
         ++column) {
        QTableWidgetItem *head =
            m_networkWebRequestsTable->horizontalHeaderItem(column);
        if (!head || head->text() != header)
            continue;
        QTableWidgetItem *item = m_networkWebRequestsTable->item(row, column);
        return item ? item->text() : QString();
    }
    return QString();
}

QString MainWindow::testLogSearchEditText() const
{
    return m_logSearchEdit ? m_logSearchEdit->text() : QString();
}

QString MainWindow::testClickWebRequestsLogButton(int row)
{
    if (!m_networkWebRequestsTable)
        return QString();
    QTableWidgetItem *logItem = m_networkWebRequestsTable->item(row, 1);
    QTableWidgetItem *groupItem = m_networkWebRequestsTable->item(row, 0);
    if (!logItem || !groupItem || logItem->icon().isNull())
        return QString();
    emit m_networkWebRequestsTable->cellClicked(row, 1);
    return groupItem->text();
}

QString MainWindow::testClickWebRequestsGroupLabel(int row)
{
    if (!m_networkWebRequestsTable)
        return QString();
    QTableWidgetItem *groupItem = m_networkWebRequestsTable->item(row, 0);
    if (!groupItem)
        return QString();
    emit m_networkWebRequestsTable->cellClicked(row, 0);
    return groupItem->text();
}

QString MainWindow::testNetworkWebRequestsStatusText() const
{
    return m_networkWebRequestsStatus ? m_networkWebRequestsStatus->text()
                                      : QString();
}

int MainWindow::testNetworkWebRequestsChartHeight() const
{
    return m_networkWebRequestsChart
               ? m_networkWebRequestsChart->minimumHeight()
               : 0;
}

QStringList MainWindow::testNetworkRepoColumns() const
{
    QStringList labels;
    if (!m_networkReposTable)
        return labels;
    for (int column = 0; column < m_networkReposTable->columnCount(); ++column) {
        if (QTableWidgetItem *item =
                m_networkReposTable->horizontalHeaderItem(column))
            labels.append(item->text());
    }
    return labels;
}

QString MainWindow::testNetworkRepoCellText(int row,
                                            const QString &header) const
{
    if (!m_networkReposTable || row < 0 ||
        row >= m_networkReposTable->rowCount())
        return QString();
    for (int column = 0; column < m_networkReposTable->columnCount(); ++column) {
        QTableWidgetItem *label =
            m_networkReposTable->horizontalHeaderItem(column);
        if (!label || label->text() != header)
            continue;
        QTableWidgetItem *item = m_networkReposTable->item(row, column);
        return item ? item->text() : QString();
    }
    return QString();
}

bool MainWindow::testNetworkRepoHasCommitSparkline(int row) const
{
    if (!m_networkReposTable || row < 0 ||
        row >= m_networkReposTable->rowCount())
        return false;
    for (int column = 0; column < m_networkReposTable->columnCount(); ++column) {
        QTableWidgetItem *label =
            m_networkReposTable->horizontalHeaderItem(column);
        if (!label || label->text() != QLatin1String("Commit activity"))
            continue;
        QWidget *cell = m_networkReposTable->cellWidget(row, column);
        return cell && cell->objectName() ==
            QLatin1String("networkRepoCommitActivityChart");
    }
    return false;
}

QString MainWindow::testNetworkRepoCommitActivitySummary(int row) const
{
    if (!m_networkReposTable || row < 0 ||
        row >= m_networkReposTable->rowCount())
        return QString();
    for (int column = 0; column < m_networkReposTable->columnCount(); ++column) {
        QTableWidgetItem *label =
            m_networkReposTable->horizontalHeaderItem(column);
        if (!label || label->text() != QLatin1String("Commit activity"))
            continue;
        QWidget *cell = m_networkReposTable->cellWidget(row, column);
        return cell ? cell->accessibleName() : QString();
    }
    return QString();
}

bool MainWindow::testNetworkRepoHasMergeChart(int row) const
{
    if (!m_networkReposTable || row < 0 ||
        row >= m_networkReposTable->rowCount())
        return false;
    for (int column = 0; column < m_networkReposTable->columnCount(); ++column) {
        QTableWidgetItem *label =
            m_networkReposTable->horizontalHeaderItem(column);
        if (!label || label->text() != QLatin1String("Merge activity"))
            continue;
        QWidget *cell = m_networkReposTable->cellWidget(row, column);
        return cell && cell->objectName() ==
            QLatin1String("networkRepoMergeActivityChart");
    }
    return false;
}

QString MainWindow::testNetworkRepoMergeActivitySummary(int row) const
{
    if (!m_networkReposTable || row < 0 ||
        row >= m_networkReposTable->rowCount())
        return QString();
    for (int column = 0; column < m_networkReposTable->columnCount(); ++column) {
        QTableWidgetItem *label =
            m_networkReposTable->horizontalHeaderItem(column);
        if (!label || label->text() != QLatin1String("Merge activity"))
            continue;
        QWidget *cell = m_networkReposTable->cellWidget(row, column);
        return cell ? cell->accessibleName() : QString();
    }
    return QString();
}

QStringList MainWindow::testLocalRepoNames() const
{
    QStringList names;
    // isHidden(), not isVisible(): the page itself is not on screen in a test.
    if (!m_localReposTable || m_localReposTable->isHidden())
        return names;
    for (int row = 0; row < m_localReposTable->rowCount(); ++row) {
        if (QTableWidgetItem *item = m_localReposTable->item(row, 0))
            names.append(item->data(Qt::UserRole).toString());
    }
    return names;
}

QString MainWindow::testLocalRepoCellText(int row, const QString &header) const
{
    if (!m_localReposTable || row < 0 || row >= m_localReposTable->rowCount())
        return QString();
    for (int column = 0; column < m_localReposTable->columnCount(); ++column) {
        QTableWidgetItem *label =
            m_localReposTable->horizontalHeaderItem(column);
        if (!label || label->text() != header)
            continue;
        QTableWidgetItem *item = m_localReposTable->item(row, column);
        return item ? item->text() : QString();
    }
    return QString();
}

QString MainWindow::testLocalReposStatusText() const
{
    return m_localReposStatus ? m_localReposStatus->text() : QString();
}

int MainWindow::testReposNavBadgeCount() const
{
    auto *railButton = dynamic_cast<ActivityRailButton *>(m_reposNavButton);
    return railButton ? railButton->badgeCount() : -1;
}

namespace {
int firewallRuleColumn(const QTableWidget *table, const QString &header)
{
    if (!table)
        return -1;
    for (int column = 0; column < table->columnCount(); ++column) {
        const QTableWidgetItem *head = table->horizontalHeaderItem(column);
        if (head && head->text() == header)
            return column;
    }
    return -1;
}
} // namespace

QStringList MainWindow::testFirewallRuleColumns() const
{
    QStringList labels;
    if (!m_requestFirewallRulesTable)
        return labels;
    for (int column = 0; column < m_requestFirewallRulesTable->columnCount();
         ++column) {
        const QTableWidgetItem *head =
            m_requestFirewallRulesTable->horizontalHeaderItem(column);
        labels.append(head ? head->text() : QString());
    }
    return labels;
}

int MainWindow::testFirewallRuleRow(const QString &storedRule) const
{
    const int column =
        firewallRuleColumn(m_requestFirewallRulesTable, QStringLiteral("Stored as"));
    if (column < 0)
        return -1;
    for (int row = 0; row < m_requestFirewallRulesTable->rowCount(); ++row) {
        const QTableWidgetItem *item =
            m_requestFirewallRulesTable->item(row, column);
        if (item && item->text() == storedRule)
            return row;
    }
    return -1;
}

QString MainWindow::testFirewallRuleCellText(int row,
                                             const QString &header) const
{
    const int column = firewallRuleColumn(m_requestFirewallRulesTable, header);
    if (column < 0)
        return QString();
    const QTableWidgetItem *item = m_requestFirewallRulesTable->item(row, column);
    return item ? item->text() : QString();
}

QString MainWindow::testFirewallRuleDraft() const
{
    return m_requestFirewallRuleEdit ? m_requestFirewallRuleEdit->text()
                                     : QString();
}

QString MainWindow::testCurrentNetworkTabText() const
{
    return m_networkTabs ? m_networkTabs->tabText(m_networkTabs->currentIndex())
                         : QString();
}

QString MainWindow::testFirewallStatusText() const
{
    return m_requestFirewallStatus ? m_requestFirewallStatus->text() : QString();
}

QStringList MainWindow::testFirewallRules(bool deny) const
{
    auto *manager = qobject_cast<BackoffNetworkAccessManager *>(m_networkAccess);
    if (!manager)
        return {};
    return deny ? manager->firewallDenyRules() : manager->firewallRules();
}

void MainWindow::testSetFirewallRules(const QStringList &allow,
                                      const QStringList &deny)
{
    auto *manager = qobject_cast<BackoffNetworkAccessManager *>(m_networkAccess);
    if (!manager)
        return;
    manager->setFirewallRules(allow);
    manager->setFirewallDenyRules(deny);
    refreshFirewallTables();
}

void MainWindow::testRecordFirewallMatch(const QUrl &url)
{
    auto *manager = qobject_cast<BackoffNetworkAccessManager *>(m_networkAccess);
    if (!manager)
        return;
    manager->recordFirewallMatch(url);
    updateFirewallRuleCounts();
}

QString MainWindow::testClickFirewallRuleLogCell(int row)
{
    const int column =
        firewallRuleColumn(m_requestFirewallRulesTable, QStringLiteral("Log"));
    if (column < 0)
        return QString();
    const QTableWidgetItem *item = m_requestFirewallRulesTable->item(row, column);
    if (!item)
        return QString();
    emit m_requestFirewallRulesTable->cellClicked(row, column);
    return item->data(Qt::UserRole).toString();
}
#endif

void MainWindow::startSession()
{
    // Session restore is delayed until after first paint, while Chat itself is
    // an on-demand page. A restored identity still needs Chat's channel and
    // transcript models before startSession clears/repopulates them, even when
    // the user intends to remain on Home. Build that one dependency here; do
    // not assume a prior Chat navigation has already created its widgets.
    ensureSectionBuilt(2);
    if (!m_channelList || !m_dmList || !m_typingStopTimer) {
        qWarning("Could not start session: Chat models are not initialized");
        return;
    }

    const QString raw = m_nameEdit->text().trimmed().toLower();
    if (raw.isEmpty()) {
        m_setupError->setText("Pick a username to get started.");
        m_setupError->show();
        m_nameEdit->setFocus();
        return;
    }
    if (!isValidNodeName(raw)) {
        m_setupError->setText(
            "That username won't work — use lowercase letters, numbers and hyphens "
            "(no spaces), starting with a letter.");
        m_setupError->show();
        m_nameEdit->setFocus();
        return;
    }
    const QString name = raw; // validated; no rewriting needed
    m_nameEdit->setText(name);
    saveProfileName(name);
    if (!m_profileIdentity.isValid() && !m_profileIdentity.load()) {
        m_setupError->setText(m_profileIdentity.errorString());
        m_setupError->show();
        return;
    }
    if (m_accountAuthenticated &&
        AccountCapability::normalizedAccount(m_accountName) != name) {
        setDesktopCapability(m_accountName, false);
        m_accountAuthenticated = false;
        m_accountEmail.clear();
        m_accountTier = QStringLiteral("free");
        m_accountSolanaVerified = false;
        m_isAdmin = false;
        updateAdminCrownBadge();
        forkmesh::settings::drop(QSettings(), kAuthedAccountSetting);
    }

// No wallet, no signup: the core flow (clone, mirror, issues, PRs, chat)
// needs only a node name. Crypto is strictly opt-in in the node profile's
// reward settings — we never gate entry on an account or Solana address
// here. We still attempt a silent, dialog-free
// auth so a returning node that already owns an active account keeps its
// hosting/payout privileges; a brand-new node simply starts unauthenticated.
#ifdef FORKMESH_WINDOW_TESTS
    if (!m_testBypassServerStart)
        authenticateSilently(name);
#else
    authenticateSilently(name);
#endif

    if (m_serverUrlEdit->text().trimmed().isEmpty())
        m_serverUrlEdit->setText(serverHostDisplay(kDefaultServerUrl));

    QString payoutAddress = m_solanaEdit->text().trimmed();
    if (!payoutAddress.isEmpty() &&
        !forkmesh::control::isValidSolanaPublicAddress(payoutAddress)) {
        payoutAddress.clear();
        m_solanaEdit->clear();
    }
    saveSolanaAddress(payoutAddress);
    if (m_accountAuthenticated &&
        AccountCapability::normalizedAccount(m_accountName) !=
            AccountCapability::normalizedAccount(name)) {
        setDesktopCapability(m_accountName, false);
        m_accountAuthenticated = false;
        m_accountTier = QStringLiteral("free");
        m_accountSolanaVerified = false;
        m_isAdmin = false;
        updateAdminCrownBadge();
        forkmesh::settings::drop(QSettings(), kAuthedAccountSetting);
    }
    m_accountName = name;
    forkmesh::settings::put(QSettings(), kAccountNameSetting, name);
    refreshSettingsEmailVerifiedBadge();

    m_roomNameEdit->setText(kDefaultRoomName); // fixed shared room (read-only)
    m_setupError->hide();
    m_userName = name;
#ifdef FORKMESH_WINDOW_TESTS
    if (m_testBypassServerStart) {
        m_stack->setCurrentIndex(1);
        return;
    }
#endif
    persistProfile();
    m_userAvatar = UserAvatarStore::load();

    if (m_settingsNameEdit)
        m_settingsNameEdit->setText(m_userName);
    if (m_settingsMachineNodeEdit)
        m_settingsMachineNodeEdit->setText(machineNodeName());
    setSettingsAvatar(m_userAvatar);
    updateUserSwitcher();
    updateAvatarButton();

    m_channels.clear();
    m_privateChannels.clear(); // repopulated from QSettings by restorePrivateChannels()
    m_welcomeAnnounced = false; // re-evaluate the one-time greeting for this identity
    m_currentConversation.clear();
    m_history.clear();
    m_historyIds.clear();
    m_visibleRows.clear();
    m_transcriptRenderBatches = 1;
    m_reactions.clear();
    m_avatars.clear();
    m_chatDirectoryAvatarDigests.clear();
    m_dmNames.clear();
    m_openDms.clear();
    m_unread.clear();
    m_unreadCounts.clear();
    m_typing.clear();
    m_typingConversation.clear();
    m_typingStopTimer->stop();
    m_channelList->clear();
    m_dmList->clear();
    rebuildConversationView();

    const QString fullServerUrl = canonicalServerUrl(m_serverUrlEdit->text());
    forkmesh::settings::put(QSettings(), kServerUrlSetting, fullServerUrl);
    forgetConfiguredRelayHost();
    forkmesh::settings::put(QSettings(), kRoomNameSetting, kDefaultRoomName);
    persistEditsToActiveServer();
    const QUrl url(fullServerUrl);
    auto *server = new ServerNode(chatDisplayName(), sharedNodeName(),
                                  nodeOwnerDisplayName(),
                                  m_profileIdentity.publicKey(), url,
                                  kDefaultRoomName,
                                  sharedTipAddress(),
                                  m_roomPassphrase, this);
    server->setNodeSigner([this](const QByteArray &message) {
        return forkmesh::chatv3::fromB64u(m_profileIdentity.signData(message));
    });
    ensureChatTrust();
    server->setOwnerVerifier(m_chatNodeOwners);
    server->setConnectionAuthorizer([this](const QUrl &endpoint) {
        return authorizeFirewallConnection(QStringLiteral("WebSocket"), endpoint);
    });
    server->setChatAccount(chatTrustAccount());
    const auto *networkInfo = QNetworkInformation::instance();
    if (m_offlineMode || projectInstancePending() ||
        (networkInfo && networkInfo->reachability() ==
                            QNetworkInformation::Reachability::Disconnected)) {
        server->setNetworkAvailable(false);
    }
    // A selected relay is an instance/security boundary, not merely the first
    // address in a transport failover list. Crossing to another configured
    // relay would attach this identity, private Team room and repository work
    // to the wrong instance without an explicit user switch.
    server->setEndpoints({url});
    attachBackend(server);
    server->setAccountKind(
        (m_profileIsUserAccount || !m_profileLinkedNodes.isEmpty())
            ? QStringLiteral("user")
            : chatIdentityIsGuest() ? QStringLiteral("guest")
                                    : QStringLiteral("node"));
    // Before the relay, and regardless of what it does: the local mesh room
    // needs no account, no sign-in and no internet, so nothing about it waits
    // on a server handshake.
    startLocalMesh();
    if (!server->start())
        return;

    if (m_backend) {
        if (m_connectedAtMs <= 0)
            m_connectedAtMs = QDateTime::currentMSecsSinceEpoch();
        m_nodeAlertGraceUntilMs =
            QDateTime::currentMSecsSinceEpoch() + 12000;
        updateChatIdentity();
        m_backend->addChannel(QStringLiteral("general"));
        restorePrivateChannels();
        // Team coordination is private by construction. Configured local
        // teammates are admitted by the desktop relay, while other people
        // still need an explicit private-room invitation.
        ensureTeamRoom();
        logSystem("Encryption: client-side AES-256-GCM mainnode room encryption.");
        const QJsonObject signedProfile =
            m_profileIdentity.signedProfile(m_userName,
                                            m_userName,
                                            payoutAddress);
        const QString profileBytes = QString::fromUtf8(
            QJsonDocument(signedProfile).toJson(QJsonDocument::Compact));
        logSystem("Identity: signed profile for " +
                  m_profileIdentity.shortPublicKey() + " (" +
                  QString::number(profileBytes.toUtf8().size()) + " bytes).");
        m_stack->setCurrentIndex(1);
        showSection(startupLandingSection());
        updateBreadcrumb();
        updateSolanaNotice();
        loadCachedAvatars();
        loadChatHistory();
        if (m_discordPackageActive) // the Discord package is installed
            startDiscordClient();
        // Two separate switches land here, and they are not the same scope.
        // Node offline (the Node page toggle) keeps chat and every API call
        // running and only stops serving repos and publishing reward
        // eligibility. Offline mode (the footer switch) pauses this app's
        // internet requests altogether -- that is what it is for, and a reader
        // who reads one comment as describing the other will expect the wrong
        // thing of both (issue #712).
        if (m_nodeOffline || m_offlineMode) {
            if (m_connectedAtMs > 0) {
                m_totalConnectionMs +=
                    QDateTime::currentMSecsSinceEpoch() - m_connectedAtMs;
                m_connectedAtMs = 0;
                QSettings().setValue(kConnectionTotalSetting, m_totalConnectionMs);
            }
        } else if (hasOwnerSigningCapability(name)) {
            startNodePresence();
        }
        updateNodeOnlineControls();
    }
}

// ---- Account / node registration and signed reward heartbeat ----------------

QJsonObject MainWindow::emailNotificationPreferencesPayload() const
{
    // Every email switch the relay knows (PingSettings.h), read from this
    // node's copy. The first thirteen keep their historical settings keys.
    QJsonObject prefs;
    for (const QString &kind : forkmesh::pings::emailKinds())
        prefs.insert(kind, forkmesh::pings::emailKindEnabled(kind));
    return prefs;
}

QString MainWindow::emailNotificationDelivery() const
{
    // Anything the relay wouldn't accept reads as the digest default rather
    // than being sent through, so a hand-edited ini can't opt somebody into
    // instant mail by accident.
    const QString mode = QSettings()
                             .value(kEmailNotifyDeliverySetting,
                                    QStringLiteral("digest"))
                             .toString()
                             .trimmed()
                             .toLower();
    return mode == QStringLiteral("instant") ? mode : QStringLiteral("digest");
}

void MainWindow::sendNodeHeartbeat()
{
    if (m_nodeOffline || m_offlineMode)
        return;
    if (m_nodeHeartbeatDeliveryActive) {
        // Settings and refill events can ask for another beat while the prior
        // socket write is awaiting its verdict. Coalesce them, then rebuild a
        // fresh signed body once this delivery finishes.
        m_nodeHeartbeatSendPending = true;
        return;
    }
    const QString name = m_accountName.isEmpty()
                             ? QSettings().value(kAccountNameSetting).toString().trimmed()
                             : m_accountName;
    if (name.isEmpty() || !hasOwnerSigningCapability(name) ||
        !m_profileIdentity.isValid())
        return;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (!m_pollBackoff.ready(QStringLiteral("heartbeat"), nowMs))
        return;
    const QString ts = QString::number(nowMs);
    const QByteArray canonical =
        ("forkmesh-heartbeat-v1\n" + name + "\n" + ts).toUtf8();
    const QString solana = savedSolanaAddress();
    // an opt-in refill notification piggybacks this already-signed
    // channel instead of a dedicated endpoint. 5h takes priority when both
    // flags are set in the same tick; the other stays pending for the next beat.
    const QString creditsRefilled = m_pendingCreditsRefilled5h
        ? QStringLiteral("5h")
        : (m_pendingCreditsRefilledWeekly ? QStringLiteral("weekly") : QString());
    QJsonObject body{{"nodeName", name}, {"solana", solana}, {"ts", ts},
                     {"sig", m_profileIdentity.signData(canonical)}};
    // Ping settings (#347) ride only the beat after a local change, like the
    // pause below. They used to be restated every minute, which quietly undid
    // any switch flipped on the website or the phone a minute later; the
    // relay's copy is now adopted from each reply instead.
    if (m_pendingPingSettingsPush ||
        QSettings()
            .value(forkmesh::pings::kSettingsPushPendingSetting, false)
            .toBool()) {
        m_pendingPingSettingsPush = true;
        const QJsonObject snapshot =
            forkmesh::pings::localSnapshot(emailNotificationDelivery());
        body.insert(QStringLiteral("notificationPreferences"),
                    emailNotificationPreferencesPayload());
        body.insert(QStringLiteral("notificationDelivery"),
                    emailNotificationDelivery());
        for (const QString &key :
             {QStringLiteral("emailNotifications"),
              QStringLiteral("pingMutedKinds"),
              QStringLiteral("notificationQuietHours")})
            body.insert(key, snapshot.value(key));
    }
    // Only on the beat after a local change: a pause started in the browser
    // must not be stomped by this node's stale copy a minute later.
    if (m_pendingPingPausePush ||
        QSettings().value(kPingPausePushPendingSetting, false).toBool()) {
        m_pendingPingPausePush = true;
        body.insert(QStringLiteral("notificationPauseUntil"),
                    QSettings().value(kPingPauseUntilSetting, 0).toLongLong());
    }
    if (!creditsRefilled.isEmpty())
        body.insert(QStringLiteral("creditsRefilled"), creditsRefilled);
    // acknowledge the accepted mirror requests we've already acted
    // on so the relay stops redelivering them on every beat. Rides the same
    // signed heartbeat as creditsRefilled above.
    QStringList mirrorAcks;
    if (!m_pendingMirrorRequestAcks.isEmpty()) {
        QJsonArray acks;
        for (const QString &id : m_pendingMirrorRequestAcks)
            acks.append(id);
        body.insert(QStringLiteral("mirrorRequestsAck"), acks);
        mirrorAcks = m_pendingMirrorRequestAcks;
    }

    m_nodeHeartbeatDeliveryActive = true;
    m_nodeHeartbeatRequestId = ts;
    m_nodeHeartbeatCreditsRefilled = creditsRefilled;
    m_nodeHeartbeatMirrorAcks = mirrorAcks;
    m_nodeHeartbeatBody = body;

    // The account event socket is already open for relay pushes. Carry the
    // same independently signed heartbeat back up it so steady-state presence
    // creates no HTTP request. Older relays do not advertise this frame and
    // automatically stay on the endpoint below.
    QJsonObject frame = body;
    frame.insert(QStringLiteral("type"), QStringLiteral("heartbeat"));
    // A credits-refilled heartbeat creates a pings notification, whose fan-out
    // targets this same Durable Object. Keep that rare one-shot event on HTTPS
    // so the hibernating socket handler never has to call back into itself.
    if (creditsRefilled.isEmpty() && !m_nodeHeartbeatSocketFailed &&
        m_nodeEventSocket &&
        m_nodeEventSocket->sendSignedFrame(frame)) {
        m_nodeHeartbeatViaSocket = true;
        if (!m_nodeHeartbeatAckTimer) {
            m_nodeHeartbeatAckTimer = new QTimer(this);
            m_nodeHeartbeatAckTimer->setSingleShot(true);
            connect(m_nodeHeartbeatAckTimer, &QTimer::timeout, this,
                    &MainWindow::retryNodeHeartbeatOverHttp);
        }
        m_nodeHeartbeatAckTimer->start(kNodeHeartbeatAckTimeoutMs);
        return;
    }
    postNodeHeartbeatFallback();
}

void MainWindow::postNodeHeartbeatFallback()
{
    if (!m_nodeHeartbeatDeliveryActive)
        return;
    if (m_offlineMode) {
        finishNodeHeartbeatDelivery();
        return;
    }
    m_nodeHeartbeatViaSocket = false;
    if (m_nodeHeartbeatAckTimer)
        m_nodeHeartbeatAckTimer->stop();
    if (!m_networkAccess) {
        applyNodeHeartbeatResult(0, QJsonObject());
        return;
    }
    QNetworkRequest request(accountsApiUrl("heartbeat"));
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    request.setTransferTimeout(kNodeHeartbeatAckTimeoutMs);
    QNetworkReply *reply = m_networkAccess->post(
        request,
        QJsonDocument(m_nodeHeartbeatBody).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonObject response =
            QJsonDocument::fromJson(reply->readAll()).object();
        reply->deleteLater();
        applyNodeHeartbeatResult(status, response);
    });
}

void MainWindow::retryNodeHeartbeatOverHttp()
{
    if (!m_nodeHeartbeatDeliveryActive || !m_nodeHeartbeatViaSocket)
        return;
    if (m_nodeOffline || m_offlineMode || !m_heartbeatTimer ||
        !m_heartbeatTimer->isActive()) {
        finishNodeHeartbeatDelivery();
        return;
    }
    // A connected socket that does not answer its advertised write is not a
    // safe presence transport. Keep the original signed body (still within
    // the timestamp window), recover it once over HTTPS, and leave later beats
    // on HTTPS until the socket reconnects and negotiates capabilities again.
    m_nodeHeartbeatSocketFailed = true;
    postNodeHeartbeatFallback();
}

void MainWindow::onNodeHeartbeatFrame(const QString &requestId, int status,
                                      const QJsonObject &response)
{
    if (!m_nodeHeartbeatDeliveryActive || !m_nodeHeartbeatViaSocket ||
        requestId != m_nodeHeartbeatRequestId)
        return;
    if (m_nodeHeartbeatAckTimer)
        m_nodeHeartbeatAckTimer->stop();
    if (status == 409 &&
        response.value(QStringLiteral("error")).toString() ==
            QLatin1String("http_transport_required")) {
        postNodeHeartbeatFallback();
        return;
    }
    m_nodeHeartbeatSocketFailed = false;
    applyNodeHeartbeatResult(status, response);
}

void MainWindow::applyNodeHeartbeatResult(int status,
                                          const QJsonObject &resp)
{
    if (status < 200 || status >= 300) {
        m_pollBackoff.noteFailure(QStringLiteral("heartbeat"),
                                  QDateTime::currentMSecsSinceEpoch());
        finishNodeHeartbeatDelivery();
        return;
    }

    if (m_nodeHeartbeatCreditsRefilled == QLatin1String("5h"))
        m_pendingCreditsRefilled5h = false;
    else if (m_nodeHeartbeatCreditsRefilled == QLatin1String("weekly"))
        m_pendingCreditsRefilledWeekly = false;
    acknowledgePingPauseDelivery();
    acknowledgePingSettingsDelivery();
    // Same rule for the ping settings: adopt only with nothing of ours queued.
    if (!m_pendingPingSettingsPush &&
        forkmesh::pings::adoptRelaySettings(resp, kEmailNotifyDeliverySetting))
        refreshPingSettingsSurfaces();
    // The relay is authoritative once our own change has landed — it clamps an
    // over-long deadline and it holds a pause set in the browser. While a push
    // is still queued this reply predates it, and adopting would hand back
    // exactly the value the user just replaced.
    if (!m_pendingPingPausePush &&
        resp.contains(QStringLiteral("notificationPauseUntil"))) {
        const qint64 relayPause =
            qint64(resp.value(QStringLiteral("notificationPauseUntil"))
                       .toDouble());
        if (relayPause !=
            QSettings().value(kPingPauseUntilSetting, 0).toLongLong()) {
            if (relayPause > 0)
                QSettings().setValue(kPingPauseUntilSetting, relayPause);
            else
                QSettings().remove(kPingPauseUntilSetting);
            refreshPingPauseSurfaces();
        }
    }
    for (const QString &id : std::as_const(m_nodeHeartbeatMirrorAcks))
        m_pendingMirrorRequestAcks.removeAll(id);
    m_pollBackoff.noteSuccess(QStringLiteral("heartbeat"));
    const qint64 heartbeatOkMs = QDateTime::currentMSecsSinceEpoch();
    if (forkmesh::pingforward::cameOnline(m_lastHeartbeatOkMs, heartbeatOkMs))
        announceDesktopOnline(heartbeatOkMs);
    m_lastHeartbeatOkMs = heartbeatOkMs;

    const bool wasAdmin = m_isAdmin;
    m_isAdmin = resp.value("isAdmin").toBool();
    if (m_isAdmin != wasAdmin) {
        updateAdminCrownBadge();
        applyDebugBarStartupPreference();
        refreshPingSettingsSurfaces();
    }
    // A freshly launched instance's request to join rides the same signed
    // heartbeat reply for admins: light the red dot over the relay favicon
    // and show the Approve button beside it.
    setPendingRelayJoins(
        m_isAdmin ? resp.value(QStringLiteral("pendingRelays")).toInt() : 0);
    fetchRoomPassphrase();
    startOfficeChannelMirror();
    refreshWebAlerts();
    refreshRunnerAttention();
    // Checked once per signed-in admin, on the first heartbeat that confirms
    // the crown — a pending email confirmation is rare and never urgent, so
    // re-asking every five minutes only spent a request per node forever.
    // The Email confirmations dialog re-fetches on demand.
    if (m_isAdmin && !m_adminPendingPolled) {
        m_adminPendingPolled = true;
        pollPendingUsers();
    }
    if (resp.value(QStringLiteral("balanceIncreased")).toBool()) {
        updateNavSolanaBalance();
        refreshNavSolanaBalance(true);
    }
    const QJsonObject claim = resp.value(QStringLiteral("claim")).toObject();
    const QString claimCode = claim.value(QStringLiteral("code")).toString();
    if (!claimCode.isEmpty() && claimCode != m_lastClaimCodeShown) {
        m_lastClaimCodeShown = claimCode;
        showNodeClaimCode(claim.value(QStringLiteral("user")).toString(),
                          claimCode);
    }
    // An admin requested ownership of THIS node: the reply carries who's
    // asking, and only this node's own signed decision can approve or deny it.
    const QJsonObject transfer =
        resp.value(QStringLiteral("ownershipTransfer")).toObject();
    const QString transferAdmin =
        transfer.value(QStringLiteral("admin")).toString();
    if (!transferAdmin.isEmpty() &&
        transferAdmin != m_lastOwnershipTransferAdminShown) {
        m_lastOwnershipTransferAdminShown = transferAdmin;
        showOwnershipTransferPrompt(transferAdmin);
    } else if (transferAdmin.isEmpty()) {
        m_lastOwnershipTransferAdminShown.clear();
    }
    const QJsonArray mirrorRequests =
        resp.value(QStringLiteral("mirrorRequests")).toArray();
    for (const QJsonValue &value : mirrorRequests) {
        const QJsonObject req = value.toObject();
        const QString id =
            req.value(QStringLiteral("id")).toString().trimmed();
        const QString owner =
            req.value(QStringLiteral("owner")).toString().trimmed();
        const QString repo =
            req.value(QStringLiteral("repo")).toString().trimmed();
        if (id.isEmpty() || owner.isEmpty() || repo.isEmpty())
            continue;
        if (!m_pendingMirrorRequestAcks.contains(id))
            m_pendingMirrorRequestAcks.append(id);
        if (m_handledMirrorRequests.contains(id))
            continue; // already mirrored this session; just keep acking
        m_handledMirrorRequests.insert(id);
        logSystem("Accepted mirror request: mirroring " + owner + "/" + repo +
                  " for a peer.");
        mirrorNetworkRepo(owner, repo, QString(), false);
    }
    finishNodeHeartbeatDelivery();
}

void MainWindow::acknowledgePingPauseDelivery()
{
    // Clear only when THIS delivery carried the pause value that is still
    // current. If the user changed the deadline while an older pause heartbeat
    // was in flight, that older reply must not discard the newer pending push.
    const qint64 desiredPause =
        QSettings().value(kPingPauseUntilSetting, 0).toLongLong();
    const bool deliveredCurrentPause =
        m_nodeHeartbeatBody.contains(QStringLiteral("notificationPauseUntil")) &&
        m_nodeHeartbeatBody.value(QStringLiteral("notificationPauseUntil"))
                .toVariant()
                .toLongLong() == desiredPause;
    if (!deliveredCurrentPause)
        return;
    m_pendingPingPausePush = false;
    QSettings().remove(kPingPausePushPendingSetting);
}

void MainWindow::acknowledgePingSettingsDelivery()
{
    // As with the pause: clear only when THIS delivery carried the settings
    // that are still current, so a switch flipped while an older push was in
    // flight stays pending for the next beat.
    if (!m_nodeHeartbeatBody.contains(QStringLiteral("pingMutedKinds")))
        return;
    const QJsonObject current =
        forkmesh::pings::localSnapshot(emailNotificationDelivery());
    for (auto it = current.begin(); it != current.end(); ++it) {
        if (m_nodeHeartbeatBody.value(it.key()) != it.value())
            return;
    }
    m_pendingPingSettingsPush = false;
    QSettings().remove(forkmesh::pings::kSettingsPushPendingSetting);
}

void MainWindow::markPingSettingsChanged()
{
    m_pendingPingSettingsPush = true;
    QSettings().setValue(forkmesh::pings::kSettingsPushPendingSetting, true);
    // Quiet hours can start or end a mute on the spot.
    refreshPingPauseSurfaces();
    sendNodeHeartbeat();
}

void MainWindow::refreshPingSettingsSurfaces()
{
    if (m_pingSettingsPanel) {
        m_pingSettingsPanel->setAdmin(m_isAdmin);
        m_pingSettingsPanel->refresh();
    }
    if (m_pingAdminCard)
        m_pingAdminCard->setVisible(m_isAdmin);
    refreshPingPauseSurfaces();
}

#ifdef FORKMESH_WINDOW_TESTS
void MainWindow::testApplyPingSettingsReply(const QJsonObject &reply)
{
    if (!m_pendingPingSettingsPush &&
        forkmesh::pings::adoptRelaySettings(reply, kEmailNotifyDeliverySetting))
        refreshPingSettingsSurfaces();
}

bool MainWindow::testAcknowledgePingPauseDelivery(qint64 deliveredPause,
                                                  qint64 currentPause)
{
    QSettings settings;
    if (currentPause > 0)
        settings.setValue(kPingPauseUntilSetting, currentPause);
    else
        settings.remove(kPingPauseUntilSetting);
    settings.setValue(kPingPausePushPendingSetting, true);
    m_pendingPingPausePush = true;
    m_nodeHeartbeatBody = {
        {QStringLiteral("notificationPauseUntil"), deliveredPause}};
    acknowledgePingPauseDelivery();
    const bool pending =
        m_pendingPingPausePush &&
        settings.value(kPingPausePushPendingSetting, false).toBool();
    m_pendingPingPausePush = false;
    m_nodeHeartbeatBody = QJsonObject();
    settings.remove(kPingPausePushPendingSetting);
    settings.remove(kPingPauseUntilSetting);
    return pending;
}
#endif

void MainWindow::finishNodeHeartbeatDelivery()
{
    if (m_nodeHeartbeatAckTimer)
        m_nodeHeartbeatAckTimer->stop();
    m_nodeHeartbeatDeliveryActive = false;
    m_nodeHeartbeatViaSocket = false;
    m_nodeHeartbeatRequestId.clear();
    m_nodeHeartbeatCreditsRefilled.clear();
    m_nodeHeartbeatMirrorAcks.clear();
    m_nodeHeartbeatBody = QJsonObject();
    const bool sendAgain = m_nodeHeartbeatSendPending && !m_nodeOffline &&
                           !m_offlineMode &&
                           m_heartbeatTimer && m_heartbeatTimer->isActive();
    m_nodeHeartbeatSendPending = false;
    if (sendAgain) {
        QTimer::singleShot(0, this, &MainWindow::sendNodeHeartbeat);
    }
}

void MainWindow::showNodeClaimCode(const QString &user, const QString &code)
{
    const QString who = user.isEmpty() ? QStringLiteral("A user") : user;
    logSystem("Account: user \"" + who + "\" is claiming this node on the "
              "website. Confirmation code: " + code + " — enter it there to "
              "link this node to that account (expires in 10 minutes).");
    auto *box = new QMessageBox(this);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setWindowTitle(QStringLiteral("Link this machine?"));
    box->setIcon(QMessageBox::Information);
    box->setText(
        QStringLiteral("<b>%1</b> is claiming this machine's node on "
                       "forkmesh.com.<br><br>"
                       "Confirmation code:"
                       "<div style='font-size:28px;letter-spacing:6px'><b>%2</b></div>"
                       "Enter this code on the website to link this machine to "
                       "that account. If this isn't you, just close this window — "
                       "the code expires in 10 minutes and is never sent anywhere "
                       "else.")
            .arg(who.toHtmlEscaped(), code.toHtmlEscaped()));
    box->setStandardButtons(QMessageBox::Close);
    box->setModal(false);
    box->show();
    box->raise();
    box->activateWindow();
}

void MainWindow::showOwnershipTransferPrompt(const QString &admin)
{
    const QString who = admin.isEmpty() ? QStringLiteral("An admin") : admin;
    logSystem("Account: admin \"" + who + "\" has requested ownership of this "
              "node. Approve or deny from the node's profile" +
              QStringLiteral("."));
    auto *box = new QMessageBox(this);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setWindowTitle(QStringLiteral("Ownership transfer request"));
    box->setIcon(QMessageBox::Warning);
    box->setText(
        QStringLiteral("<b>%1</b> (admin) is requesting ownership of this "
                       "node.<br><br>Approving hands this node over to that "
                       "account and removes it from its current owner's "
                       "fleet. If you didn't expect this, choose Deny.")
            .arg(who.toHtmlEscaped()));
    QPushButton *approve = box->addButton("Approve", QMessageBox::AcceptRole);
    QPushButton *deny = box->addButton("Deny", QMessageBox::RejectRole);
    box->setDefaultButton(deny);
    box->setModal(false);
    forkmesh::ui::onMessageBoxClicked(box, this,
            [this, approve](QAbstractButton *clicked) {
                submitOwnershipTransferDecision(clicked == approve);
            });
    box->show();
    box->raise();
    box->activateWindow();
}

void MainWindow::submitOwnershipTransferDecision(bool approve)
{
    const QString node = accountOwner();
    if (node.isEmpty() || !hasOwnerSigningCapability(node) ||
        !m_profileIdentity.isValid())
        return;
    const QString action = approve ? QStringLiteral("approve")
                                   : QStringLiteral("deny");
    const QString ts = QString::number(QDateTime::currentMSecsSinceEpoch());
    const QByteArray canonical =
        ("forkmesh-ownership-transfer-confirm-v1\n" + node + "\n" + action +
         "\n" + ts)
            .toUtf8();
    int status = 0;
    const QJsonObject resp = postAccountSync(
        "ownership-transfer-confirm",
        QJsonObject{{"nodeName", node}, {"action", action}, {"ts", ts},
                    {"sig", m_profileIdentity.signData(canonical)}},
        &status);
    m_lastOwnershipTransferAdminShown.clear();
    if (status == 200 && resp.value("ok").toBool()) {
        logSystem(approve ? "Account: ownership transfer approved."
                          : "Account: ownership transfer denied.");
        if (approve)
            refreshProfileAccountStatus(true);
    } else {
        logSystem("Account: ownership transfer decision failed to submit.");
    }
}

void MainWindow::requestNodeOwnership()
{
    if (!m_isAdmin)
        return;
    const QString target = m_profileNodeName;
    const QString node = accountOwner();
    if (target.isEmpty() || node.isEmpty() ||
        !hasOwnerSigningCapability(node) || !m_profileIdentity.isValid())
        return;
    if (QMessageBox::question(
            this, "Take ownership",
            QStringLiteral("Request ownership of <b>%1</b>?<br><br>This only "
                           "sends a request — it takes effect once that "
                           "node's current owner approves the confirmation "
                           "prompt it receives.")
                .arg(target.toHtmlEscaped()),
            QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
        return;
    const QString ts = QString::number(QDateTime::currentMSecsSinceEpoch());
    const QByteArray canonical =
        ("forkmesh-admin-request-ownership-v1\n" + node + "\n" + target +
         "\n" + ts)
            .toUtf8();
    int status = 0;
    const QJsonObject resp = postAccountSync(
        "admin-request-ownership",
        QJsonObject{{"node", node}, {"target", target}, {"ts", ts},
                    {"sig", m_profileIdentity.signData(canonical)}},
        &status);
    if ((status == 200 || status == 201) && resp.value("ok").toBool()) {
        logSystem("Admin: requested ownership of " + target +
                  " — awaiting that node's approval.");
        QMessageBox::information(
            this, "Request sent",
            QStringLiteral("Ownership request sent. It completes once %1's "
                           "current owner approves it.")
                .arg(target.toHtmlEscaped()));
    } else {
        QMessageBox::warning(
            this, "Request failed",
            QStringLiteral("Could not request ownership: %1")
                .arg(resp.value("error").toString(
                    QStringLiteral("unknown error"))));
    }
}

void MainWindow::pollPendingUsers()
{
    if (!m_isAdmin)
        return;
    const QString node = accountOwner();
    if (node.isEmpty() || !hasOwnerSigningCapability(node) ||
        !m_profileIdentity.isValid())
        return;
    const QString ts = QString::number(QDateTime::currentMSecsSinceEpoch());
    const QByteArray canonical =
        ("forkmesh-admin-pending-v1\n" + node + "\n" + ts).toUtf8();
    QUrl url = accountsApiUrl("admin-pending");
    QUrlQuery query;
    query.addQueryItem("node", node);
    query.addQueryItem("ts", ts);
    query.addQueryItem("sig", m_profileIdentity.signData(canonical));
    url.setQuery(query);
    QNetworkReply *reply = m_networkAccess->get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const QJsonObject resp = QJsonDocument::fromJson(reply->readAll()).object();
        reply->deleteLater();
        if (!resp.value("ok").toBool())
            return;
        QStringList current, fresh;
        for (const QJsonValue &v : resp.value("pending").toArray()) {
            const QString name = v.toObject().value("name").toString();
            if (name.isEmpty())
                continue;
            current.append(name);
            if (!m_seenPendingUsers.contains(name))
                fresh.append(name);
        }
        m_seenPendingUsers = current;
        if (fresh.isEmpty())
            return;
        const QString msg =
            QStringLiteral("%1 new user(s) joined and need email verification:\n\n%2")
                .arg(fresh.size())
                .arg(fresh.join(", "));
        if (notifyEnabled(kNewUserAlertSetting))
            showPingTrayMessage(QStringLiteral("ForkMesh — new user"), msg,
                                /*warning=*/false, /*timeoutMs=*/8000);
        QMessageBox box(QMessageBox::Information,
                        QStringLiteral("New user joined"),
                        msg + QStringLiteral(
                                  "\n\nReview and verify now, or quarantine "
                                  "all new users?"),
                        QMessageBox::No | QMessageBox::Yes, this);
        QPushButton *quarantineAll =
            box.addButton(QStringLiteral("Quarantine all"),
                          QMessageBox::DestructiveRole);
        box.setDefaultButton(QMessageBox::No);
        box.exec();
        if (box.clickedButton() == quarantineAll) {
            // Sweep the queue at the Worker rather than issuing one request
            // for the notification's snapshot.  The Worker removes every
            // quarantined row from pending_verifications in the same batch,
            // so this poll cannot surface those names again.
            adminQuarantinePendingUsers(this);
        }
        else if (box.standardButton(box.clickedButton()) == QMessageBox::Yes)
            showAdminVerifyDialog();
    });
}

void MainWindow::fetchRoomPassphrase()
{
    // Fetch once: the shared room key is stable per relay. Signed with the node's
    // own Ed25519 key (the same proof a heartbeat carries), so the relay hands
    // the key only to a registered account — it is no longer a public constant.
    if (!m_roomPassphrase.isEmpty())
        return;
    const QString node = accountOwner();
    if (node.isEmpty() || !hasOwnerSigningCapability(node) ||
        !m_profileIdentity.isValid())
        return;
    const QString ts = QString::number(QDateTime::currentMSecsSinceEpoch());
    const QString roomOwner = QStringLiteral("mainnode");
    const QString roomRepo = QStringLiteral("forkmesh");
    const QByteArray canonical =
        ("forkmesh-room-key-v2\n" + node + "\n" + roomOwner + "\n" +
         roomRepo + "\n" + ts).toUtf8();
    QUrl url = catalogApiUrl();
    url.setPath(QStringLiteral("/api/chat/room-key"));
    QUrlQuery query;
    query.addQueryItem("owner", roomOwner);
    query.addQueryItem("repo", roomRepo);
    query.addQueryItem("room", kDefaultRoomName);
    query.addQueryItem("node", node);
    query.addQueryItem("ts", ts);
    query.addQueryItem("sig", m_profileIdentity.signData(canonical));
    url.setQuery(query);
    QNetworkReply *reply = m_networkAccess->get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const QJsonObject resp = QJsonDocument::fromJson(reply->readAll()).object();
        reply->deleteLater();
        const QString pass = resp.value("passphrase").toString();
        const bool matchingScope =
            resp.value("ok").toBool() &&
            resp.value("scope").toString() ==
                QStringLiteral("mainnode/forkmesh") &&
            resp.value("room").toString() == kDefaultRoomName;
        if (matchingScope && !pass.isEmpty()) {
            // Swapping the room key re-derives it (setRoomPassphrase builds a
            // RoomCrypto); do the PBKDF2 work on a worker first so the swap
            // on the GUI thread is a cache hit.
            QThread *warm = QThread::create([pass] {
                RoomCrypto::warmKey(kDefaultRoomName, pass);
            });
            connect(warm, &QThread::finished, warm, &QObject::deleteLater);
            connect(warm, &QThread::finished, this, [this, pass] {
                m_roomPassphrase = pass;
                if (m_backend)
                    m_backend->setRoomPassphrase(pass);
            });
            warm->start();
        }
    });
}

void MainWindow::startOfficeChannelMirror()
{
    const QString node = accountOwner();
    if (node.isEmpty() || !m_profileIdentity.isValid() ||
        (!hasOwnerSigningCapability(node) &&
         accountSessionTokenForUrl(catalogApiUrl()).isEmpty()))
        return;
    if (!m_officeChannelMirror) {
        m_officeChannelMirror = new OfficeChannelMirror(m_networkAccess, this);
        m_officeChannelMirror->setSigner([this](const QByteArray &canonical) {
            return m_profileIdentity.signData(canonical);
        });
        m_officeChannelMirror->setSessionProvider([this](const QUrl &url) {
            return accountSessionTokenForUrl(url);
        });
        m_officeChannelMirror->setConnectionAuthorizer(
            [this](const QUrl &endpoint) {
                return authorizeFirewallConnection(QStringLiteral("WebSocket"),
                                                   endpoint);
            });
        connect(m_officeChannelMirror, &OfficeChannelMirror::messageArrived,
                this, &MainWindow::onMessage);
        connect(m_officeChannelMirror, &OfficeChannelMirror::directDevicePending, this,
                &MainWindow::noteDirectDevicePending);
        connect(m_officeChannelMirror,
                &OfficeChannelMirror::directSafetyNumberChanged, this,
                &MainWindow::noteDirectSafetyChanged);
        connect(m_officeChannelMirror, &OfficeChannelMirror::directRouteChanged, this,
                [this](const QString &conversation) {
                    if (conversation == m_currentConversation) {
                        refreshDirectRoute();
                        refreshDevicePendingBanner();
                    }
                });
        connect(m_officeChannelMirror, &OfficeChannelMirror::typingChanged, this,
                &MainWindow::onTypingChanged);
        connect(m_officeChannelMirror, &OfficeChannelMirror::readMarksFetched,
                this, &MainWindow::applyReadMarks);
        connect(m_officeChannelMirror,
                &OfficeChannelMirror::messageRecipientsKnown, this,
                &MainWindow::applyMessageRecipients);
        connect(m_officeChannelMirror, &OfficeChannelMirror::messagesDelivered,
                this, &MainWindow::applyMessagesDelivered);
        connect(m_officeChannelMirror, &OfficeChannelMirror::conversationRead,
                this, &MainWindow::applyConversationRead);
        connect(m_officeChannelMirror, &OfficeChannelMirror::reactionChanged,
                this, &MainWindow::applyOfficeReaction);
        connect(m_officeChannelMirror, &OfficeChannelMirror::messageEdited, this,
                &MainWindow::applyOfficeEdit);
        connect(m_officeChannelMirror, &OfficeChannelMirror::messageDeleted,
                this, &MainWindow::applyOfficeDelete);
        connect(m_officeChannelMirror,
                &OfficeChannelMirror::directServerCopiesKnown, this,
                [this](const QString &conversation, int stored) {
                    m_directServerCopies.insert(conversation, stored);
                    if (conversation == m_currentConversation)
                        updateChatConversationHeading();
                });
        connect(m_officeChannelMirror, &OfficeChannelMirror::sendActivity, this,
                [this](const QString &text) { logSystem(text); });
        connect(m_officeChannelMirror, &OfficeChannelMirror::messageSendFailed,
                this,
                [this](const QString &conversation, const QString &messageId,
                       const QString &text, const QString &reason) {
                    logSystem(QStringLiteral("Office chat: could not send to %1 "
                                             "(%2).")
                                  .arg(conversation, reason));
                    if (!messageId.isEmpty() &&
                        m_history[conversation].removeIf(
                            [&messageId](const ChatMessage &message) {
                                return message.id == messageId;
                            }) > 0) {
                        m_historyIds.remove(messageId);
                        refreshChatListPreview(conversation);
                        if (conversation == m_currentConversation)
                            scheduleConversationRebuild(m_stickToBottom);
                        scheduleChatSave();
                    }
                    if (conversation == m_currentConversation &&
                        m_messageInput && m_messageInput->text().isEmpty()) {
                        m_messageInput->setText(text);
                        m_messageInput->setFocus();
                    }
                });
        connect(m_officeChannelMirror, &OfficeChannelMirror::conversationsChanged,
                this, [this](const QStringList &conversations) {
                    m_officeConversations = conversations;
                    purgePrivateChatPings();
                    setChannels(m_channels);
                    refreshChatMembers();
                    refreshChannelSettingsButton();
                    refreshChatComposerAccess();
                    refreshRoomSealButton();
                    maybeAnnounceWelcome();
                    if (!m_pendingOfficeConversation.isEmpty() &&
                        conversations.contains(m_pendingOfficeConversation)) {
                        const QString created = m_pendingOfficeConversation;
                        m_pendingOfficeConversation.clear();
                        showChatView();
                        switchConversation(created);
                    }
                });
        connect(m_officeChannelMirror, &OfficeChannelMirror::channelCreated,
                this, [this](const QString &conversation) {
                    m_pendingOfficeConversation = conversation;
                    if (m_officeConversations.contains(conversation)) {
                        m_pendingOfficeConversation.clear();
                        showChatView();
                        switchConversation(conversation);
                    }
                });
        connect(m_officeChannelMirror,
                &OfficeChannelMirror::channelCreateFailed, this,
                [this](const QString &reason) {
                    QMessageBox::warning(
                        this, QStringLiteral("New private room"),
                        QStringLiteral("The room was not created: %1")
                            .arg(reason));
                });
        connect(m_officeChannelMirror,
                &OfficeChannelMirror::directConversationsChanged, this,
                [this](const QStringList &conversations) {
                    m_officeDirectConversations = conversations;
                    refreshDmList();
                    if (m_officeDirectConversations.contains(
                            m_currentConversation)) {
                        refreshChatMembers();
                        refreshChatComposerAccess();
                        refreshRoomSealButton();
                    }
                });
        connect(m_officeChannelMirror,
                &OfficeChannelMirror::directConversationReady, this,
                [this](const QString &conversation) {
                    if (!m_officeDirectConversations.contains(conversation)) {
                        m_officeDirectConversations.append(conversation);
                        refreshDmList();
                    }
                    const QString invite =
                        m_pendingDirectInvites.take(conversation);
                    if (!invite.isEmpty()) {
                        const QString peer =
                            forkmesh::office::directPeerForConversation(
                                conversation);
                        const bool sent = m_officeChannelMirror->sendMessage(
                            conversation, invite);
                        const QString note =
                            sent ? QStringLiteral("Invitation sent to @%1. "
                                                  "It is in your direct "
                                                  "messages.")
                                       .arg(peer)
                                 : QStringLiteral("Could not send the "
                                                  "invitation to @%1.")
                                       .arg(peer);
                        if (m_projectInviteStatus)
                            m_projectInviteStatus->setText(note);
                        else
                            flashMessage(note, !sent);
                        return;
                    }
                    showChatView();
                    switchConversation(conversation);
                    if (m_messageInput)
                        m_messageInput->setFocus();
                });
        connect(m_officeChannelMirror,
                &OfficeChannelMirror::directMessageFailed, this,
                [this](const QString &username, const QString &reason) {
                    if (m_pendingDirectInvites.remove(
                            forkmesh::office::conversationForDirect(username)) &&
                        m_projectInviteStatus) {
                        m_projectInviteStatus->setText(
                            QStringLiteral("Could not invite @%1: %2")
                                .arg(username, reason));
                        return;
                    }
                    QMessageBox::warning(
                        this, QStringLiteral("New message"),
                        QStringLiteral("Could not open a conversation with "
                                       "@%1: %2")
                            .arg(username, reason));
                });
        // A rename keeps the room, so carry this window's history, unread
        // count and open view across to the new name.
        connect(m_officeChannelMirror,
                &OfficeChannelMirror::conversationRenamed, this,
                &MainWindow::renameChatConversation);
        connect(m_officeChannelMirror, &OfficeChannelMirror::channelSettingsSaved,
                this, [this](const QString &conversation) {
                    logSystem(QStringLiteral("Office chat: saved settings for "
                                             "%1.")
                                  .arg(conversation));
                    if (conversation == m_currentConversation) {
                        updateChatConversationHeading();
                        refreshRoomSealButton();
                    }
                });
        connect(m_officeChannelMirror, &OfficeChannelMirror::channelDeleted,
                this, [this](const QString &conversation) {
                    logSystem(QStringLiteral("Office chat: deleted %1.")
                                  .arg(conversation));
                    m_unread.remove(conversation);
                    m_unreadCounts.remove(conversation);
                    m_history.remove(conversation);
                    if (m_currentConversation == conversation) {
                        const QString fallback = m_channels.value(0);
                        m_currentConversation.clear();
                        if (!fallback.isEmpty())
                            switchConversation(fallback);
                    }
                    refreshChannelSettingsButton();
                    scheduleChatSave();
                });
        connect(m_officeChannelMirror, &OfficeChannelMirror::roomMembersChanged,
                this, [this](const QString &conversation, const QStringList &) {
                    if (conversation == m_currentConversation) {
                        refreshChatMembers();
                        updateChatConversationHeading();
                        refreshRoomSealButton();
                    }
                });
        connect(m_officeChannelMirror, &OfficeChannelMirror::topicChanged,
                this, [this](const QString &conversation, const QString &) {
                    if (conversation == m_currentConversation)
                        updateChatConversationHeading();
                });
        connect(m_officeChannelMirror,
                &OfficeChannelMirror::channelSettingsFailed, this,
                [this](const QString &conversation, const QString &reason) {
                    logSystem(QStringLiteral("Office chat: %1 (%2).")
                                  .arg(conversation, reason));
                });
    }
    m_officeChannelMirror->setApiBase(catalogApiUrl());
    ensureChatTrust();
    m_officeChannelMirror->setOwnerVerifier(m_chatNodeOwners);
    m_officeChannelMirror->setIdentity(node, m_profileIdentity.publicKey(),
                                       chatDisplayName());
    m_officeChannelMirror->start();
    syncChatBlocks();
    wireDirectMesh();
    refreshDirectFocus();
}

void MainWindow::stopLocalMesh()
{
    if (!m_localMesh)
        return;
    LocalMeshNode *mesh = m_localMesh;
    m_localMesh = nullptr;
    QObject::disconnect(mesh, nullptr, this, nullptr);
    mesh->stop();
    mesh->deleteLater();
    setChannels(m_channels);
    refreshMeshDebugBar();
    refreshLocalControlTab();
}

void MainWindow::startLocalMesh()
{
    if (!corePackageServiceRunning(QStringLiteral("local-mesh")))
        return;
    if (m_localMesh) {
        m_localMesh->setUserName(chatDisplayName());
        m_localMesh->setNodeIdentity(sharedNodeName(), nodeOwnerDisplayName());
        m_localMesh->announceNow();
        startLocalControl();
        return;
    }
    if (m_localMeshKeyState != 2) {
        if (m_localMeshKeyState == 1)
            return;
        m_localMeshKeyState = 1;
        QThread *warm = QThread::create([room = forkmesh::localmesh::roomName()] {
            RoomCrypto::warmDefaultKey(room);
        });
        connect(warm, &QThread::finished, warm, &QObject::deleteLater);
        connect(warm, &QThread::finished, this, [this] {
            m_localMeshKeyState = 2;
            if (!m_closingDown)
                startLocalMesh();
        });
        warm->start();
        return;
    }
    // Deliberately not gated on offline mode, an account, or a sign-in: the
    // mesh is local-only, and offline mode is switched on automatically when
    // the internet drops — which is exactly when this room matters.
    m_localMesh = new LocalMeshNode(chatDisplayName(), sharedNodeName(),
                                    nodeOwnerDisplayName(),
                                    m_profileIdentity.publicKey(), this);
    m_localMesh->setNodeSigner([this](const QByteArray &message) {
        return forkmesh::chatv3::fromB64u(m_profileIdentity.signData(message));
    });
    // The room's history, and the messages still waiting for a device, live
    // with this identity, so a restart neither loses nor strands them.
    m_localMesh->setHistoryFile(
        forkmesh::paths::dataPath(QStringLiteral("local_mesh")) +
        QStringLiteral("/history-") +
        QString::fromLatin1(
            QCryptographicHash::hash(m_profileIdentity.publicKey().toUtf8(),
                                     QCryptographicHash::Sha256)
                .toHex()
                .left(16)) +
        QStringLiteral(".json"));
    connect(m_localMesh, &LocalMeshNode::messageArrived, this,
            &MainWindow::onMessage);
    connect(m_localMesh, &LocalMeshNode::queuedChanged, this,
            [this](const QString &messageId, bool) {
                if (isLocalMeshConversation(m_currentConversation))
                    applyLocalMeshQueuedMark(m_visibleRows.value(messageId));
            });
    connect(m_localMesh, &LocalMeshNode::typingChanged, this,
            &MainWindow::onTypingChanged);
    connect(m_localMesh, &LocalMeshNode::wakeRequested, this,
            &MainWindow::wakeLocalMeshPhones);
    connect(m_localMesh, &LocalMeshNode::systemMessage, this,
            [this](const QString &text) {
                m_localMeshLastNote = text;
                logSystem(text);
            });
    connect(m_localMesh, &LocalMeshNode::peersChanged, this, [this] {
        if (isLocalMeshConversation(m_currentConversation)) {
            refreshChatMembers();
            updateChatConversationHeading();
        }
    });
    connect(m_localMesh, &LocalMeshNode::linksBlockedDetected, this,
            &MainWindow::checkLocalMeshFirewall);
    connect(m_localMesh, &LocalMeshNode::inboundLinkAccepted, this,
            &MainWindow::noteLocalMeshReachable);
    // Set before start() so the very first beacon already offers the phone
    // its control channel.
    startLocalControl();
    if (!m_localMesh->start()) {
        m_localMesh->deleteLater();
        m_localMesh = nullptr;
        refreshMeshDebugBar();
        refreshLocalControlTab();
        return;
    }
    setChannels(m_channels); // folds the mesh room into the sidebar
    wireDirectMesh();
    checkLocalMeshFirewall();
    if (m_offlineMode)
        startOfficeChannelMirror();
    if (isLocalMeshConversation(m_currentConversation))
        for (MessageRow *row : std::as_const(m_visibleRows))
            applyLocalMeshQueuedMark(row);
}

void MainWindow::startDiscordClient()
{
    if (!m_discordClient) {
        m_discordClient = new DiscordClient(m_networkAccess, this);
        m_discordClient->setConnectionAuthorizer(
            [this](const QUrl &endpoint) {
                return authorizeFirewallConnection(QStringLiteral("WebSocket"),
                                                    endpoint);
            });
        connect(m_discordClient, &DiscordClient::messageArrived, this,
                &MainWindow::onMessage);
        connect(m_discordClient, &DiscordClient::reactionChanged, this,
                &MainWindow::onReaction);
        connect(m_discordClient, &DiscordClient::reactionsLoaded, this,
                &MainWindow::onDiscordReactionsLoaded);
        connect(m_discordClient, &DiscordClient::messageEdited, this,
                &MainWindow::onMessageEdited);
        connect(m_discordClient, &DiscordClient::messageDeleted, this,
                &MainWindow::onMessageDeleted);
        connect(m_discordClient, &DiscordClient::typingChanged, this,
                &MainWindow::onTypingChanged);
        connect(m_discordClient, &DiscordClient::avatarChanged, this,
                &MainWindow::onAvatar);
        connect(m_discordClient, &DiscordClient::activity, this,
                [this](const QString &text) { logSystem(text); });
        connect(m_discordClient, &DiscordClient::sendFailed, this,
                [this](const QString &conversation, const QString &text,
                       const QString &reason) {
                    logSystem(QStringLiteral("Discord: could not send to %1 (%2).")
                                  .arg(conversation, reason));
                    if (!text.isEmpty() &&
                        conversation == m_currentConversation) {
                        if (m_chatThreadInput &&
                            !m_activeChatThreadRootId.isEmpty() &&
                            m_chatThreadInput->toPlainText().isEmpty()) {
                            m_chatThreadInput->setPlainText(text);
                            m_chatThreadInput->setFocus();
                        } else if (m_messageInput &&
                                   m_messageInput->text().isEmpty()) {
                            m_messageInput->setText(text);
                            m_messageInput->setFocus();
                        }
                    }
                });
        connect(m_discordClient, &DiscordClient::conversationsChanged, this,
                [this](const QStringList &conversations) {
                    m_discordConversations = conversations;
                    setChannels(m_channels);
                    refreshChatMembers();
                });
        connect(m_discordClient, &DiscordClient::membersChanged, this,
                [this](const QString &conversation) {
                    if (conversation == m_currentConversation)
                        refreshChatMembers();
                });
        connect(m_discordClient, &DiscordClient::conversationMetadataChanged,
                this, [this](const QString &conversation) {
                    refreshChannelList();
                    if (conversation == m_currentConversation)
                        updateChatConversationHeading();
                });
        connect(m_discordClient, &DiscordClient::topicChanged, this,
                [this](const QString &conversation, const QString &) {
                    if (conversation == m_currentConversation)
                        updateChatConversationHeading();
                });
        connect(m_discordClient, &DiscordClient::statusChanged, this,
                &MainWindow::updateDiscordConnectButton);
    }

    QSettings settings;
    QString stored = settings.value(forkmesh::discord::kBotTokenSetting).toString();
    if (stored.isEmpty() &&
        settings.contains(forkmesh::discord::kLegacyBotTokenSetting)) {
        // Builds before the Discord package kept the token outside the
        // package's data prefix; move it so removing the package wipes it.
        stored = settings.value(forkmesh::discord::kLegacyBotTokenSetting)
                     .toString();
        if (!forkmesh::discord::normalizeBotToken(stored).isEmpty())
            settings.setValue(forkmesh::discord::kBotTokenSetting, stored);
        settings.remove(forkmesh::discord::kLegacyBotTokenSetting);
    }
    const QString token = forkmesh::discord::normalizeBotToken(stored);
    m_discordClient->setBotToken(token);
    m_discordConversations = m_discordClient->conversations();
    setChannels(m_channels);
    if (token.isEmpty()) {
        updateDiscordConnectButton();
        return;
    }
    m_discordClient->start();
}

void MainWindow::updateDiscordConnectButton(const QString &status,
                                            bool connected, bool error)
{
    if (!m_discordConnectButton)
        return;
    const bool configured =
        m_discordClient && m_discordClient->isConfigured();
    if (!configured) {
        m_discordConnectButton->setText(QStringLiteral("Connect Discord"));
        m_discordConnectButton->setToolTip(QStringLiteral(
            "Connect a Discord bot and show its channels in this chat area"));
    } else if (connected) {
        const QString name = m_discordClient->botName().trimmed();
        m_discordConnectButton->setText(
            name.isEmpty() ? QStringLiteral("Discord · connected")
                           : QStringLiteral("Discord · %1").arg(name));
        m_discordConnectButton->setToolTip(
            status.isEmpty() ? QStringLiteral("Discord is connected live") : status);
    } else {
        m_discordConnectButton->setText(
            error ? QStringLiteral("Discord · fix connection")
                  : QStringLiteral("Discord · reconnecting"));
        m_discordConnectButton->setToolTip(
            status.isEmpty() ? QStringLiteral("Open Discord connection settings")
                             : status);
    }
    m_discordConnectButton->setProperty("connected", connected);
    m_discordConnectButton->setProperty("bad", error);
    m_discordConnectButton->style()->unpolish(m_discordConnectButton);
    m_discordConnectButton->style()->polish(m_discordConnectButton);
}

void MainWindow::showDiscordConnectionDialog()
{
    if (!m_discordPackageActive) {
        flashMessage(QStringLiteral("Discord is not installed. Install it from "
                                    "Market → Extensions."),
                     true);
        return;
    }
    startDiscordClient();

    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QStringLiteral("Connect Discord"));
    dialog->setMinimumWidth(560);
    auto *layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(20, 20, 20, 18);
    layout->setSpacing(10);

    auto *title = new QLabel(QStringLiteral("DISCORD IN FORKMESH CHAT"));
    title->setObjectName(QStringLiteral("sectionLabel"));
    layout->addWidget(title);

    auto *explanation = new QLabel(QStringLiteral(
        "ForkMesh connects as a dedicated Discord bot and places every server "
        "text channel the bot can access directly in the Chat sidebar. Messages "
        "you send here appear under that bot's identity. Normal Discord user "
        "tokens are intentionally unsupported because Discord prohibits "
        "self-bots."));
    explanation->setWordWrap(true);
    explanation->setObjectName(QStringLiteral("statusLine"));
    layout->addWidget(explanation);

    auto *steps = new QLabel(QStringLiteral(
        "1. Create or open an app in the <a href='https://discord.com/developers/"
        "applications'>Discord Developer Portal</a>.<br>"
        "2. On <b>Bot</b>, enable <b>Message Content Intent</b> and copy the bot "
        "token.<br>3. Install the bot in each server where it should read and "
        "send messages."));
    steps->setWordWrap(true);
    steps->setOpenExternalLinks(true);
    steps->setTextFormat(Qt::RichText);
    layout->addWidget(steps);

    auto *tokenEdit = new QLineEdit;
    tokenEdit->setEchoMode(QLineEdit::Password);
    tokenEdit->setPlaceholderText(QStringLiteral("Discord bot token"));
    tokenEdit->setMaxLength(512);
    tokenEdit->setText(
        QSettings().value(forkmesh::discord::kBotTokenSetting).toString());
    tokenEdit->setToolTip(QStringLiteral(
        "Stored only in this app's device-local settings; never placed in logs, "
        "action variables, command lines, or chat history"));
    layout->addWidget(tokenEdit);

    auto *privacy = new QLabel(QStringLiteral(
        "The token stays in device-local app settings. ForkMesh requests only "
        "message/channel Gateway intents; server permissions still control exactly "
        "what the bot can see and do."));
    privacy->setWordWrap(true);
    privacy->setObjectName(QStringLiteral("modeHint"));
    layout->addWidget(privacy);

    auto *statusLabel = new QLabel;
    statusLabel->setObjectName(QStringLiteral("modeHint"));
    statusLabel->setWordWrap(true);
    if (m_discordClient && m_discordClient->isConfigured()) {
        statusLabel->setText(
            m_discordClient->isConnected()
                ? QStringLiteral("Connected live as %1.")
                      .arg(m_discordClient->botName())
                : QStringLiteral("Configured; reconnecting or waiting for setup."));
    } else {
        statusLabel->setText(QStringLiteral("Not connected."));
    }
    layout->addWidget(statusLabel);

    auto *buttons = new QHBoxLayout;
    auto *portal = new QPushButton(QStringLiteral("Open Developer Portal"));
    portal->setObjectName(QStringLiteral("ghostButton"));
    portal->setCursor(Qt::PointingHandCursor);
    connect(portal, &QPushButton::clicked, dialog, [] {
        QDesktopServices::openUrl(
            QUrl(QStringLiteral("https://discord.com/developers/applications")));
    });
    buttons->addWidget(portal);

    if (m_discordClient && !m_discordClient->botId().isEmpty()) {
        auto *invite = new QPushButton(QStringLiteral("Install bot in a server"));
        invite->setObjectName(QStringLiteral("ghostButton"));
        invite->setCursor(Qt::PointingHandCursor);
        const QString clientId = m_discordClient->botId();
        connect(invite, &QPushButton::clicked, dialog, [clientId] {
            // View Channels, Send Messages, Embed Links, Attach Files,
            // Read Message History, Add Reactions, Use External Emoji, and
            // Send Messages in Threads. Server admins can grant less.
            constexpr qulonglong permissions =
                (1ull << 6) | (1ull << 10) | (1ull << 11) | (1ull << 14) |
                (1ull << 15) | (1ull << 16) | (1ull << 18) | (1ull << 38);
            QUrl url(QStringLiteral("https://discord.com/oauth2/authorize"));
            QUrlQuery query;
            query.addQueryItem(QStringLiteral("client_id"), clientId);
            query.addQueryItem(QStringLiteral("scope"), QStringLiteral("bot"));
            query.addQueryItem(QStringLiteral("permissions"),
                               QString::number(permissions));
            url.setQuery(query);
            QDesktopServices::openUrl(url);
        });
        buttons->addWidget(invite);
    }
    buttons->addStretch();

    if (m_discordClient && m_discordClient->isConfigured()) {
        auto *disconnect = new QPushButton(QStringLiteral("Disconnect"));
        disconnect->setObjectName(QStringLiteral("dangerButton"));
        disconnect->setCursor(Qt::PointingHandCursor);
        connect(disconnect, &QPushButton::clicked, dialog, [this, dialog] {
            disconnectDiscord();
            if (!m_discordClient || !m_discordClient->isConfigured())
                dialog->accept();
        });
        buttons->addWidget(disconnect);
    }

    auto *cancel = new QPushButton(QStringLiteral("Cancel"));
    cancel->setObjectName(QStringLiteral("ghostButton"));
    connect(cancel, &QPushButton::clicked, dialog, &QDialog::reject);
    buttons->addWidget(cancel);
    auto *connectButton = new QPushButton(QStringLiteral("Connect"));
    connectButton->setObjectName(QStringLiteral("primaryButton"));
    connectButton->setCursor(Qt::PointingHandCursor);
    connect(connectButton, &QPushButton::clicked, dialog,
            [this, dialog, tokenEdit, statusLabel] {
                const QString token = forkmesh::discord::normalizeBotToken(
                    tokenEdit->text());
                if (token.isEmpty()) {
                    statusLabel->setText(QStringLiteral(
                        "Enter a valid bot token without spaces or line breaks."));
                    statusLabel->setProperty("bad", true);
                    statusLabel->style()->unpolish(statusLabel);
                    statusLabel->style()->polish(statusLabel);
                    return;
                }
                if (!m_discordPackageActive || !m_discordClient) {
                    // Removed from Market while this dialog was open: the
                    // token must not be written back after the wipe.
                    statusLabel->setText(QStringLiteral(
                        "The Discord package was removed. Reinstall it from "
                        "Market → Extensions to connect again."));
                    return;
                }
                QSettings settings;
                const QString previousToken =
                    forkmesh::discord::normalizeBotToken(
                        settings
                            .value(forkmesh::discord::kBotTokenSetting)
                            .toString());
                if (!previousToken.isEmpty() && previousToken != token) {
                    const int replace = QMessageBox::question(
                        dialog, QStringLiteral("Replace Discord bot"),
                        QStringLiteral(
                            "Replace the connected Discord bot? Cached Discord "
                            "history from the previous bot will be removed from "
                            "this device."),
                        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
                    if (replace != QMessageBox::Yes)
                        return;
                    if (isDiscordConversation(m_currentConversation))
                        sendTypingState(false);
                    clearDiscordLocalHistory();
                    logSystem(QStringLiteral(
                        "Discord: removed cached history from the previous bot."));
                }
                settings.setValue(forkmesh::discord::kBotTokenSetting, token);
                settings.sync();
                m_discordClient->stop();
                m_discordClient->setBotToken(token);
                m_discordClient->start();
                updateDiscordConnectButton(QStringLiteral("Discord · connecting"));
                dialog->accept();
            });
    buttons->addWidget(connectButton);
    layout->addLayout(buttons);

    dialog->show();
}

void MainWindow::disconnectDiscord()
{
    const int answer = QMessageBox::question(
        this, QStringLiteral("Disconnect Discord"),
        QStringLiteral("Disconnect Discord and remove its local token and cached "
                       "chat history from this device?"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;

    const bool wasDiscord = isDiscordConversation(m_currentConversation);
    if (wasDiscord)
        sendTypingState(false);
    QSettings settings;
    settings.remove(forkmesh::discord::kBotTokenSetting);
    settings.remove(forkmesh::discord::kLegacyBotTokenSetting);
    settings.sync();
    if (m_discordClient) {
        m_discordClient->stop();
        m_discordClient->setBotToken(QString());
    }

    clearDiscordLocalHistory();
    updateDiscordConnectButton();
    logSystem(QStringLiteral(
        "Discord: disconnected and removed the device-local token/history."));
}

void MainWindow::clearDiscordLocalHistory()
{
    const QStringList historyKeys = m_history.keys();
    for (const QString &conversation : historyKeys) {
        if (!forkmesh::discord::isDiscordConversation(conversation))
            continue;
        const QList<ChatMessage> messages = m_history.take(conversation);
        for (const ChatMessage &message : messages) {
            m_historyIds.remove(message.id);
            m_reactions.remove(message.id);
        }
        m_unread.remove(conversation);
        m_unreadCounts.remove(conversation);
        m_typing.remove(conversation);
    }
    const QSet<QString> historyIds = m_historyIds;
    for (const QString &messageId : historyIds) {
        if (messageId.startsWith(QStringLiteral("discord-message:")))
            m_historyIds.remove(messageId);
    }
    for (const QString &messageId : m_reactions.keys()) {
        if (messageId.startsWith(QStringLiteral("discord-message:")))
            m_reactions.remove(messageId);
    }
    const QSet<QString> unread = m_unread;
    for (const QString &conversation : unread) {
        if (forkmesh::discord::isDiscordConversation(conversation))
            m_unread.remove(conversation);
    }
    for (const QString &conversation : m_unreadCounts.keys()) {
        if (forkmesh::discord::isDiscordConversation(conversation))
            m_unreadCounts.remove(conversation);
    }
    for (const QString &conversation : m_typing.keys()) {
        if (forkmesh::discord::isDiscordConversation(conversation))
            m_typing.remove(conversation);
    }
    if (isDiscordConversation(m_currentConversation))
        m_currentConversation.clear();
    m_discordConversations.clear();
    setChannels(m_channels);
    if (m_currentConversation.isEmpty()) {
        m_channelTitle->setText(QStringLiteral("No conversation"));
        rebuildConversationView();
    }
    saveChatHistory();
}

bool MainWindow::discordPackageInstalled() const
{
    return m_packageRegistry && m_packageRegistry->isReady() &&
           m_packageRegistry->isInstalledHere(forkmesh::discord::kPackageId);
}

void MainWindow::attachDiscordPackage()
{
    if (m_discordPackageActive)
        return;
    m_discordPackageActive = true;
    if (m_discordSettingsCard)
        m_discordSettingsCard->show();
    if (m_discordConnectButton)
        m_discordConnectButton->show();
    // Before the node has connected, the post-connect boot path starts the
    // client; an install from Market after that starts it right away.
    if (m_backend)
        startDiscordClient();
    else
        updateDiscordConnectButton();
}

// Removing the package must leave no trace: the runtime, its token (current
// and pre-package key), the cached Discord transcript and unread state, the
// Discord avatars on disk, and the automatic sidebar group. The Market
// registry separately drops market/packages/discord/data, which is where the
// token lives, so even a crash between the two leaves nothing secret behind.
void MainWindow::detachDiscordPackage()
{
    m_discordPackageActive = false;
    if (isDiscordConversation(m_currentConversation))
        sendTypingState(false);
    if (m_discordClient) {
        // Replies still in flight must not re-add channels through
        // conversationsChanged after the package is gone.
        disconnect(m_discordClient, nullptr, this, nullptr);
        m_discordClient->stop();
        m_discordClient->deleteLater();
        m_discordClient = nullptr;
    }
    m_discordTranscriptRefreshPending = false;
    {
        QSettings settings;
        settings.remove(forkmesh::discord::kBotTokenSetting);
        settings.remove(forkmesh::discord::kLegacyBotTokenSetting);
        settings.sync();
    }
    if (ChannelGroups::purge(
            m_channelGroups,
            [](const QString &channel) {
                return forkmesh::discord::isDiscordConversation(channel);
            },
            [](const QString &group) {
                return forkmesh::discord::isDiscordGroupName(group);
            }))
        persistChannelGroups();
    clearDiscordLocalHistory(); // also re-merges the sidebar and saves
    purgeDiscordAvatarCache();
    if (m_discordSettingsCard)
        m_discordSettingsCard->hide();
    if (m_discordConnectButton)
        m_discordConnectButton->hide();
    updateDiscordConnectButton();
    logSystem(QStringLiteral(
        "Discord: package removed; its token, cached history, avatars, and "
        "sidebar group were cleared from this device."));
}

void MainWindow::purgeDiscordAvatarCache()
{
    const QString prefix = QStringLiteral("discord-user:");
    for (auto it = m_avatars.begin(); it != m_avatars.end();) {
        if (it.key().startsWith(prefix))
            it = m_avatars.erase(it);
        else
            ++it;
    }
    const QDir dir(forkmesh::paths::dataPath(QStringLiteral("avatars")));
    if (!dir.exists())
        return;
    // Each cached avatar starts with its peer id on the first line (see
    // onAvatar), so the Discord ones are identifiable without an index.
    const QByteArray marker = prefix.toUtf8();
    const QStringList names =
        dir.entryList({QStringLiteral("*.png")}, QDir::Files);
    for (const QString &name : names) {
        QFile file(dir.filePath(name));
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const QByteArray head = file.read(marker.size());
        file.close();
        if (head == marker)
            QFile::remove(file.fileName());
    }
}

void MainWindow::showAdminVerifyDialog()
{
    const QString node = accountOwner();
    if (node.isEmpty() || !hasOwnerSigningCapability(node) ||
        !m_profileIdentity.isValid())
        return;

    const auto fetchPending = [this]() -> QJsonArray {
        const QString owner = accountOwner();
        const QString ts =
            QString::number(QDateTime::currentMSecsSinceEpoch());
        const QByteArray canonical =
            ("forkmesh-admin-pending-v1\n" + owner + "\n" + ts).toUtf8();
        QUrl url = accountsApiUrl("admin-pending");
        QUrlQuery query;
        query.addQueryItem("node", owner);
        query.addQueryItem("ts", ts);
        query.addQueryItem("sig", m_profileIdentity.signData(canonical));
        url.setQuery(query);
        QNetworkReply *reply = m_networkAccess->get(QNetworkRequest(url));
        QEventLoop loop;
        connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();
        const QJsonObject resp =
            QJsonDocument::fromJson(reply->readAll()).object();
        reply->deleteLater();
        return resp.value("pending").toArray();
    };

    QDialog dialog(this);
    dialog.setWindowTitle("Email confirmations");
    dialog.resize(560, 460);
    auto *layout = new QVBoxLayout(&dialog);
    auto *intro = new QLabel(
        "These users still need to confirm the link sent to their mailbox. "
        "Mark one verified to vouch for the address yourself — the account "
        "records that as an administrator override, never as a recipient "
        "confirmation. A queue that is only unconfirmed signup noise can go "
        "into quarantine in one press.");
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    layout->addWidget(scroll, 1);

    // A row button only ever edits its own row. Rebuilding the page from
    // inside one would have setWidget() delete that button while its clicked
    // signal is still being delivered; only the sweep below, which lives in
    // the button bar, replaces the page.
    const auto pageFor = [this, &dialog](const QJsonArray &pending) {
        auto *inner = new QWidget;
        auto *rows = new QVBoxLayout(inner);
        int shown = 0;
        for (const QJsonValue &v : pending) {
            const QJsonObject obj = v.toObject();
            const QString name = obj.value("name").toString();
            const QString email = obj.value("email").toString();
            if (name.isEmpty())
                continue;
            ++shown;
            auto *row = new QHBoxLayout;
            auto *label = new QLabel(
                QStringLiteral(
                    "<b>%1</b><br><span style='color:#8b949e'>%2</span>")
                    .arg(name.toHtmlEscaped(), email.toHtmlEscaped()));
            label->setTextFormat(Qt::RichText);
            row->addWidget(label, 1);
            auto *btn = new QPushButton("Mark verified");
            btn->setObjectName("primaryButton");
            btn->setCursor(Qt::PointingHandCursor);
            btn->setToolTip(
                QStringLiteral(
                    "Vouch for %1 yourself. The account stores this as an "
                    "administrator override, so it stays distinguishable "
                    "from an address its owner confirmed.")
                    .arg(email.isEmpty() ? name : email));
            row->addWidget(btn);
            rows->addLayout(row);
            connect(btn, &QPushButton::clicked, &dialog,
                    [this, name, btn, &dialog]() {
                        btn->setEnabled(false);
                        QString detail;
                        if (adminMarkEmailVerified(name, &detail)) {
                            btn->setText("Verified \xE2\x9C\x93");
                            return;
                        }
                        btn->setText("Mark verified");
                        btn->setEnabled(true);
                        QMessageBox::warning(
                            &dialog, QStringLiteral("Mark verified"), detail);
                    });
        }
        if (!shown)
            rows->addWidget(
                new QLabel("<i>No users awaiting verification.</i>"));
        rows->addStretch();
        return inner;
    };
    scroll->setWidget(pageFor(fetchPending()));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    auto *sweep = new QPushButton("Quarantine all");
    sweep->setObjectName("dangerButton");
    sweep->setCursor(Qt::PointingHandCursor);
    sweep->setToolTip(QStringLiteral(
        "Move every account still waiting on a confirmation out of the "
        "active directory, revoke its sessions and devices, and quarantine "
        "its linked nodes. Administrators are left where they are."));
    buttons->addButton(sweep, QDialogButtonBox::DestructiveRole);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(sweep, &QPushButton::clicked, &dialog,
            [this, sweep, scroll, &dialog, pageFor, fetchPending]() {
                sweep->setEnabled(false);
                // The sweep pumps the event loop, so repaint from what the
                // queue reports afterwards rather than from the stale page.
                if (adminQuarantinePendingUsers(&dialog))
                    scroll->setWidget(pageFor(fetchPending()));
                sweep->setEnabled(true);
            });
    dialog.exec();
}

// Delivery is not always possible — an unconfigured provider, a bounced
// address, a link that simply never arrived — and until this existed those
// signups stayed stranded with nothing an administrator could do. The Worker
// stores the result as email_verification_method=admin_override and names the
// administrator, so a vouched address is never mistaken for a confirmed one.
bool MainWindow::adminMarkEmailVerified(const QString &target, QString *detail)
{
    if (detail)
        detail->clear();
    const QString node = accountOwner();
    if (node.isEmpty() || target.isEmpty() ||
        !hasOwnerSigningCapability(node) || !m_profileIdentity.isValid()) {
        if (detail)
            *detail = QStringLiteral(
                "This desktop cannot authenticate the administrator request.");
        return false;
    }
    const QString ts = QString::number(QDateTime::currentMSecsSinceEpoch());
    const QByteArray canonical =
        ("forkmesh-admin-verify-email-v1\n" + node + "\n" + target + "\n" + ts)
            .toUtf8();
    int status = 0;
    const QJsonObject resp = postAccountSync(
        "admin-verify-email",
        QJsonObject{{"node", node}, {"ts", ts},
                    {"sig", m_profileIdentity.signData(canonical)},
                    {"target", target}},
        &status);
    if (status == 200 && resp.value("ok").toBool()) {
        logSystem(
            resp.value("alreadyVerified").toBool()
                ? QStringLiteral("%1 was already verified; cleared it from "
                                 "the confirmation queue.")
                      .arg(target)
                : QStringLiteral("Marked %1's email verified by administrator "
                                 "override — the recipient never confirmed "
                                 "the link.")
                      .arg(target));
        return true;
    }
    if (detail) {
        const QString code = resp.value("error").toString();
        if (code == QLatin1String("account_has_no_email"))
            *detail = QStringLiteral(
                "%1 has no email address on file, so there is nothing to "
                "verify.").arg(target);
        else if (code == QLatin1String("no_such_user"))
            *detail = QStringLiteral("%1 is no longer an active account.")
                          .arg(target);
        else if (status == 401)
            *detail = QStringLiteral(
                "The administrator authorization was rejected.");
        else
            *detail = QStringLiteral(
                "ForkMesh could not mark %1's email verified.").arg(target);
    }
    return false;
}

// A signup wave that never confirms is the shape abuse takes on this queue,
// so the whole thing goes in one press. The Worker caps how many accounts it
// moves per call — each is its own D1 batch — and reports what is left, so
// this presses on its behalf until the queue drains rather than asking the
// administrator to click through the batches.
bool MainWindow::adminQuarantinePendingUsers(QWidget *parent)
{
    const QString node = accountOwner();
    if (node.isEmpty() || !hasOwnerSigningCapability(node) ||
        !m_profileIdentity.isValid()) {
        QMessageBox::warning(
            parent, QStringLiteral("Quarantine all"),
            QStringLiteral(
                "This desktop cannot authenticate the administrator "
                "request."));
        return false;
    }
    if (QMessageBox::warning(
            parent, QStringLiteral("Quarantine all"),
            QStringLiteral(
                "Quarantine every account still waiting on an email "
                "confirmation?\n\nEach one leaves the active directory, its "
                "sessions and devices are revoked, and its linked nodes go "
                "with it. The records are kept for review, and administrator "
                "accounts are left alone."),
            QMessageBox::Cancel | QMessageBox::Ok,
            QMessageBox::Cancel) != QMessageBox::Ok)
        return false;

    constexpr int kMaxSweepPasses = 25;
    int quarantined = 0;
    int cleared = 0;
    int held = 0;
    int remaining = 0;
    bool ran = false;
    QString failure;
    for (int pass = 0; pass < kMaxSweepPasses; ++pass) {
        QUrl url = accountsApiUrl(QStringLiteral("admin-quarantine-pending"));
        QNetworkRequest request;
        if (!authenticateOrgTaskRequest(url, request,
                                        kAccountPendingQuarantineProof,
                                        kAccountPendingQuarantineResource)) {
            failure = QStringLiteral(
                "This desktop cannot authenticate the administrator "
                "request.");
            break;
        }
        QNetworkReply *reply =
            m_networkAccess->post(request, QByteArrayLiteral("{}"));
        QEventLoop loop;
        connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool networkOk = reply->error() == QNetworkReply::NoError;
        const QJsonObject resp =
            QJsonDocument::fromJson(reply->readAll()).object();
        reply->deleteLater();
        if (!networkOk || !resp.value(QStringLiteral("ok")).toBool()) {
            failure = status == 401
                          ? QStringLiteral("The administrator authorization "
                                           "was rejected.")
                          : QStringLiteral("ForkMesh could not reach the "
                                           "quarantine endpoint.");
            break;
        }
        ran = true;
        quarantined +=
            resp.value(QStringLiteral("quarantined")).toArray().size();
        cleared += resp.value(QStringLiteral("cleared")).toInt();
        // Held rows are re-counted every pass, so only the pass that drained
        // the queue reports the number that is actually left behind.
        held = resp.value(QStringLiteral("held")).toInt();
        remaining = resp.value(QStringLiteral("remaining")).toInt();
        if (remaining <= 0)
            break;
    }

    QString summary;
    if (quarantined > 0) {
        summary = QStringLiteral("Quarantined %1 %2.")
                      .arg(quarantined)
                      .arg(quarantined == 1 ? QStringLiteral("account")
                                            : QStringLiteral("accounts"));
    } else if (cleared > 0) {
        summary = QStringLiteral("Cleared %1 stale confirmation %2.")
                      .arg(cleared)
                      .arg(cleared == 1 ? QStringLiteral("entry")
                                        : QStringLiteral("entries"));
    } else if (ran) {
        summary = held > 0
                      ? QStringLiteral(
                            "No non-administrator accounts were waiting on "
                            "an email confirmation.")
                      : QStringLiteral(
                            "No accounts were waiting on an email "
                            "confirmation.");
    }
    if (quarantined > 0 && cleared > 0) {
        summary += QStringLiteral(" Cleared %1 stale confirmation %2.")
                       .arg(cleared)
                       .arg(cleared == 1 ? QStringLiteral("entry")
                                         : QStringLiteral("entries"));
    }
    if (held > 0)
        summary += QStringLiteral(
                       " %1 administrator %2 left in the queue.")
                       .arg(held)
                       .arg(held == 1 ? QStringLiteral("account was")
                                      : QStringLiteral("accounts were"));
    if (remaining > 0)
        summary += QStringLiteral(
                       " %1 more still queued — press Quarantine all again "
                       "to finish.")
                       .arg(remaining);
    if (!failure.isEmpty()) {
        QMessageBox::warning(
            parent, QStringLiteral("Quarantine all"),
            summary.isEmpty() ? failure
                              : summary + QStringLiteral(" Then it stopped: ") +
                                    failure);
        return ran;
    }
    if (!summary.isEmpty()) {
        logSystem(QStringLiteral("Email confirmations: ") + summary);
        QMessageBox::information(parent, QStringLiteral("Quarantine all"),
                                 summary);
    }
    return ran;
}

void MainWindow::setPendingRelayJoins(int count)
{
    count = qMax(0, count);
    const int previous = m_pendingRelayJoins;
    m_pendingRelayJoins = count;
    if (m_relayJoinDot && m_relayMenuButton) {
        if (count > 0) {
            m_relayJoinDot->move(
                qMax(0, m_relayMenuButton->width() - m_relayJoinDot->width() -
                            2),
                2);
            m_relayJoinDot->show();
            m_relayJoinDot->raise();
        } else {
            m_relayJoinDot->hide();
        }
    }
    if (m_relayJoinApproveButton) {
        m_relayJoinApproveButton->setText(
            count > 1 ? QStringLiteral("Approve (%1)").arg(count)
                      : QStringLiteral("Approve"));
        m_relayJoinApproveButton->setToolTip(
            count > 1
                ? QStringLiteral(
                      "%1 newly launched ForkMesh instances pinged this relay "
                      "asking to join. Approve to link them — each joins the "
                      "Worlds and starts the firework show.")
                      .arg(count)
                : QStringLiteral(
                      "A newly launched ForkMesh instance pinged this relay "
                      "asking to join. Approve to link it — it joins the "
                      "Worlds and the firework show starts."));
        m_relayJoinApproveButton->setVisible(count > 0);
    }
    if (count > previous)
        logSystem(QStringLiteral(
            "A new ForkMesh instance is ready to be linked — click Approve "
            "beside the relay favicon to let it join."));
}

void MainWindow::showRelayJoinApprovalDialog()
{
    const QString node = accountOwner();
    if (node.isEmpty() || !hasOwnerSigningCapability(node) ||
        !m_profileIdentity.isValid())
        return;
    const QString ts = QString::number(QDateTime::currentMSecsSinceEpoch());
    const QByteArray canonical =
        ("forkmesh-admin-relays-v1\n" + node + "\n" + ts).toUtf8();
    QUrl url = accountsApiUrl("admin-relays");
    QUrlQuery query;
    query.addQueryItem("node", node);
    query.addQueryItem("ts", ts);
    query.addQueryItem("sig", m_profileIdentity.signData(canonical));
    url.setQuery(query);
    QNetworkReply *reply = m_networkAccess->get(QNetworkRequest(url));
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    const QJsonObject resp = QJsonDocument::fromJson(reply->readAll()).object();
    reply->deleteLater();

    QDialog dialog(this);
    dialog.setWindowTitle("Link new instances");
    dialog.resize(560, 420);
    auto *layout = new QVBoxLayout(&dialog);
    auto *intro = new QLabel(
        "Newly launched ForkMesh instances that pinged this relay asking to "
        "join the federation. Approving links an instance into the Worlds — "
        "and the firework show starts.");
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    auto *inner = new QWidget;
    auto *rows = new QVBoxLayout(inner);
    int pendingShown = 0;
    for (const QJsonValue &v : resp.value("relays").toArray()) {
        const QJsonObject obj = v.toObject();
        if (obj.value("status").toString() != QStringLiteral("pending"))
            continue;
        const QString pubkey = obj.value("pubkey").toString();
        if (pubkey.isEmpty())
            continue;
        ++pendingShown;
        const QString label = obj.value("label").toString();
        const QString baseUrl = obj.value("baseUrl").toString();
        auto *row = new QHBoxLayout;
        auto *info = new QLabel(
            QStringLiteral("<b>%1</b><br><span style='color:#8b949e'>%2</span>")
                .arg((label.isEmpty() ? QStringLiteral("Unnamed instance")
                                      : label)
                         .toHtmlEscaped(),
                     baseUrl.toHtmlEscaped()));
        info->setTextFormat(Qt::RichText);
        row->addWidget(info, 1);
        auto *blockBtn = new QPushButton("Block");
        blockBtn->setObjectName("ghostButton");
        blockBtn->setCursor(Qt::PointingHandCursor);
        row->addWidget(blockBtn);
        auto *approveBtn = new QPushButton("Approve && link");
        approveBtn->setObjectName("primaryButton");
        approveBtn->setCursor(Qt::PointingHandCursor);
        row->addWidget(approveBtn);
        rows->addLayout(row);
        connect(approveBtn, &QPushButton::clicked, &dialog,
                [this, pubkey, approveBtn, blockBtn]() {
                    approveBtn->setEnabled(false);
                    blockBtn->setEnabled(false);
                    if (adminRelayApprove(pubkey, QStringLiteral("approve"))) {
                        approveBtn->setText("Linked \xE2\x9C\x93");
                    } else {
                        approveBtn->setText("Failed");
                        approveBtn->setEnabled(true);
                        blockBtn->setEnabled(true);
                    }
                });
        connect(blockBtn, &QPushButton::clicked, &dialog,
                [this, pubkey, approveBtn, blockBtn]() {
                    approveBtn->setEnabled(false);
                    blockBtn->setEnabled(false);
                    if (adminRelayApprove(pubkey, QStringLiteral("block"))) {
                        blockBtn->setText("Blocked");
                    } else {
                        blockBtn->setText("Failed");
                        approveBtn->setEnabled(true);
                        blockBtn->setEnabled(true);
                    }
                });
    }
    if (!pendingShown)
        rows->addWidget(
            new QLabel("<i>No instances waiting to be linked.</i>"));
    rows->addStretch();
    scroll->setWidget(inner);
    layout->addWidget(scroll, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    dialog.exec();
}

bool MainWindow::adminRelayApprove(const QString &pubkey, const QString &action)
{
    const QString node = accountOwner();
    if (node.isEmpty() || pubkey.isEmpty() ||
        !hasOwnerSigningCapability(node) || !m_profileIdentity.isValid())
        return false;
    const QString ts = QString::number(QDateTime::currentMSecsSinceEpoch());
    const QByteArray canonical =
        ("forkmesh-admin-relay-approve-v1\n" + node + "\n" + pubkey + "\n" +
         action + "\n" + ts)
            .toUtf8();
    int status = 0;
    const QJsonObject resp = postAccountSync(
        "admin-relay-approve",
        QJsonObject{{"node", node}, {"ts", ts},
                    {"sig", m_profileIdentity.signData(canonical)},
                    {"pubkey", pubkey}, {"action", action}},
        &status);
    if (status != 200 || !resp.value("ok").toBool())
        return false;
    setPendingRelayJoins(m_pendingRelayJoins - 1);
    logSystem(action == QStringLiteral("approve")
                  ? QString::fromUtf8(
                        "\xF0\x9F\x8E\x86 Instance linked: it joined the "
                        "federation and the World is running its firework "
                        "show.")
                  : QStringLiteral("Instance join request blocked."));
    return true;
}

QString MainWindow::accountOwner() const
{
    if (!m_accountName.isEmpty())
        return m_accountName;
    const QString stored = QSettings().value(kAccountNameSetting).toString();
    if (!stored.isEmpty())
        return stored;
    return accountNameFromInput(m_userName, QStringLiteral("owner"));
}

QString MainWindow::hostLinkUserName()
{
    if (!hasOwnerSigningCapability(accountOwner()))
        return QString();
    const QString linkedOwner = m_nodeOwnerUser.trimmed().toLower();
    if (!linkedOwner.isEmpty())
        return linkedOwner;

    const QString signer = accountOwner().trimmed().toLower();
    if (signer.isEmpty())
        return QString();

    if (m_profileIsUserAccount || !m_profileLinkedNodes.isEmpty())
        return signer;

    if (!m_networkAccess)
        return QString();

    // Reuse the record login already read: this runs from repo screens that
    // repaint often, and it blocks the GUI thread while it waits.
    QJsonObject lookup = cachedAccountRecord(signer);
    if (lookup.isEmpty()) {
        int status = 0;
        lookup = getAccountSync(signer, &status);
        if (status != 200 || !lookup.value(QStringLiteral("exists")).toBool())
            return QString();
        cacheAccountRecord(signer, lookup);
    }

    m_nodeOwnerUser = lookup.value(QStringLiteral("owner")).toString().trimmed().toLower();
    m_profileIsUserAccount =
        lookup.value(QStringLiteral("kind")).toString() == QStringLiteral("user");
    if (!m_nodeOwnerUser.isEmpty())
        return m_nodeOwnerUser;
    return m_profileIsUserAccount ? signer : QString();
}

QString MainWindow::hostLinkSigningAccountName(QString *userName)
{
    if (userName)
        userName->clear();
    const QString signer = accountOwner().trimmed().toLower();
    if (signer.isEmpty() || !hasOwnerSigningCapability(signer))
        return QString();
    if (!m_profileIdentity.isValid() && !m_profileIdentity.load())
        return QString();
    const QString user = hostLinkUserName();
    if (user.isEmpty())
        return QString();
    if (userName)
        *userName = user;
    return signer;
}

QString MainWindow::settingsAccountName() const
{
    if (!m_accountName.trimmed().isEmpty())
        return m_accountName.trimmed().toLower();
    const QString authed =
        QSettings().value(kAuthedAccountSetting).toString().trimmed().toLower();
    if (!authed.isEmpty())
        return authed;
    return QSettings().value(kAccountNameSetting).toString().trimmed().toLower();
}

QString MainWindow::topBarUserName() const
{
    const QString linkedOwner = m_nodeOwnerUser.trimmed().toLower();
    if (!linkedOwner.isEmpty())
        return linkedOwner;

    const QString account = settingsAccountName();
    if (!account.isEmpty())
        return account;

    return accountNameFromInput(m_userName, QString());
}

QString MainWindow::nodeOwnerDisplayName() const
{
    const QString linkedOwner = m_nodeOwnerUser.trimmed().toLower();
    if (!linkedOwner.isEmpty())
        return linkedOwner;

    const QString account = accountOwner().trimmed().toLower();
    if (account.isEmpty())
        return QString();

    if (m_profileIsUserAccount || !m_profileLinkedNodes.isEmpty())
        return account;

    return QString();
}

QString MainWindow::chatDisplayName() const
{
    if (chatIdentityIsGuest())
        return guestChatName();
    const QString user = topBarUserName().trimmed();
    if (!user.isEmpty())
        return user.left(80);
    return accountNameFromInput(m_userName, QString()).left(80);
}

bool MainWindow::chatIdentityIsGuest() const
{
    if (m_profileIsUserAccount || !m_profileLinkedNodes.isEmpty() ||
        !m_nodeOwnerUser.trimmed().isEmpty())
        return false;
    // A guest is an install still running under the name a first run handed
    // out. Typing a username (setup screen or Settings) or logging in / signing
    // up replaces account/nodeName, so the two diverge and guest mode ends.
    const QString generated = accountNameFromInput(
        QSettings().value(kGeneratedNodeNameSetting).toString(), QString());
    return !generated.isEmpty() &&
           savedProfileName().compare(generated, Qt::CaseInsensitive) == 0;
}

QString MainWindow::guestChatName() const
{
    const QString generated =
        QSettings().value(kGeneratedNodeNameSetting).toString();
    static const QRegularExpression tailDigits(QStringLiteral("(\\d{4})$"));
    const QRegularExpressionMatch match = tailDigits.match(generated);
    QString digits = match.hasMatch() ? match.captured(1) : QString();
    if (digits.isEmpty()) {
        const QByteArray key = m_profileIdentity.publicKey().toUtf8();
        if (!key.isEmpty()) {
            const QByteArray digest =
                QCryptographicHash::hash(key, QCryptographicHash::Sha256);
            quint32 value = 0;
            for (int i = 0; i < 4; ++i)
                value = (value << 8) | quint8(digest.at(i));
            digits = QString::number(1000 + value % 9000);
        }
    }
    return digits.isEmpty() ? QStringLiteral("Guest")
                            : QStringLiteral("Guest %1").arg(digits);
}

QString MainWindow::machineNodeName() const
{
    const QString saved = accountNameFromInput(
        QSettings().value(kMachineNodeNameSetting).toString(), QString());
    if (!saved.isEmpty())
        return saved;
    if (m_profileIsUserAccount || !m_profileLinkedNodes.isEmpty()) {
        const QString host =
            accountNameFromInput(QSysInfo::machineHostName(), QString());
        if (!host.isEmpty())
            return host;
    }
    return accountOwner();
}

QString MainWindow::sharedNodeName() const
{
    return accountNameFromInput(QSettings().value(kMachineNodeNameSetting).toString(),
                                QString());
}

QString MainWindow::sharedTipAddress() const
{
    if (!QSettings().value(kShareTipAddressSetting, false).toBool())
        return QString();
    const QString address = savedSolanaAddress().trimmed();
    return forkmesh::control::isValidSolanaPublicAddress(address) ? address : QString();
}

void MainWindow::saveMachineNodeName(const QString &name)
{
    const QString clean = accountNameFromInput(name, QString());
    if (clean.isEmpty())
        QSettings().remove(kMachineNodeNameSetting); // back to the default
    else
        QSettings().setValue(kMachineNodeNameSetting, clean);
    updateChatIdentity();
    updateNavSolanaBalance();
    if (m_profileIsSelf)
        renderProfileAccountStatus();
    logSystem("This machine's node name is now \"" + machineNodeName() + "\".");
}

void MainWindow::updateChatIdentity()
{
    if (!m_backend)
        return;
    const QString name = chatDisplayName();
    if (!name.isEmpty() && name != m_lastChatDisplayName) {
        m_backend->setUserName(name);
        m_lastChatDisplayName = name;
    }
    m_backend->setNodeIdentity(sharedNodeName(), nodeOwnerDisplayName());
    if (auto *server = qobject_cast<ServerNode *>(m_backend))
        server->setTipAddress(sharedTipAddress());
    const QByteArray avatar = effectiveUserAvatar();
    if (avatar != m_lastChatAvatar) {
        m_backend->setAvatar(avatar);
        m_lastChatAvatar = avatar;
    }
}

bool MainWindow::accountEmailVerified(const QString &accountName) const
{
    const QString normalized = accountName.trimmed().toLower();
    return !normalized.isEmpty() &&
           QSettings().value(emailVerifiedSettingKey(normalized), false).toBool();
}

void MainWindow::applyAccountEmailVerified(const QString &accountName, bool verified)
{
    const QString normalized = accountName.trimmed().toLower();
    if (!normalized.isEmpty() && verified)
        QSettings().setValue(emailVerifiedSettingKey(normalized), true);
    refreshSettingsEmailVerifiedBadge();
    if (verified && !normalized.isEmpty() &&
        normalized == settingsAccountName())
        maybeAnnounceWelcome();
}

void MainWindow::refreshSettingsEmailVerifiedBadge()
{
    updateUserSwitcher();
    if (!m_settingsEmailVerifiedBadge)
        return;
    const bool verified = accountEmailVerified(settingsAccountName());
    m_settingsEmailVerifiedBadge->setText(QStringLiteral("Email is verified"));
    m_settingsEmailVerifiedBadge->setToolTip(
        QStringLiteral("This account's email has been verified."));
    m_settingsEmailVerifiedBadge->setVisible(verified);
    if (m_settingsEmailLabel)
        m_settingsEmailLabel->setVisible(verified);
}

bool MainWindow::hasActiveAccountSession() const
{
    return m_accountAuthenticated && m_accountTier == QStringLiteral("active");
}

bool MainWindow::hasOwnerSigningCapability(
    const QString &signerAccount) const
{
    if (!AccountCapability::ownerSigningAllowed(
            hasActiveAccountSession(), m_accountDesktopCapable,
            m_accountName, signerAccount)) {
        return false;
    }
    const QSettings settings;
    return AccountCapability::persistedMarkerMatches(
        settings.value(kDesktopCapableAccountSetting).toString(),
        settings.value(kDesktopCapablePublicKeySetting).toString(),
        m_accountName, m_profileIdentity.publicKey());
}

void MainWindow::setDesktopCapability(const QString &accountName,
                                      bool capable)
{
    const QString normalized =
        AccountCapability::normalizedAccount(accountName);
    const QString publicKey = m_profileIdentity.publicKey().trimmed();
    m_accountDesktopCapable =
        capable && !normalized.isEmpty() && !publicKey.isEmpty();
    QSettings settings;
    if (m_accountDesktopCapable) {
        settings.setValue(kDesktopCapableAccountSetting, normalized);
        settings.setValue(kDesktopCapablePublicKeySetting, publicKey);
        return;
    }
    settings.remove(kDesktopCapableAccountSetting);
    settings.remove(kDesktopCapablePublicKeySetting);
    if (m_heartbeatTimer)
        m_heartbeatTimer->stop();
    stopNodeEventSocket();
}

bool MainWindow::restoreDesktopCapability(const QString &accountName)
{
    const QString stored =
        QSettings().value(kDesktopCapableAccountSetting).toString();
    const QString storedPublicKey =
        QSettings().value(kDesktopCapablePublicKeySetting).toString();
    m_accountDesktopCapable =
        AccountCapability::persistedMarkerMatches(
            stored, storedPublicKey, accountName,
            m_profileIdentity.publicKey());
    return m_accountDesktopCapable;
}

QString MainWindow::catalogOwner(const RepositoryRecord &repo) const
{
    const QString account = accountOwner();
    return account.isEmpty() ? repoSegment(repo.owner, QStringLiteral("owner"))
                             : account;
}

QUrl MainWindow::accountsApiUrl(const QString &leaf) const
{
    QUrl url = catalogApiUrl(); // same host, http(s) scheme
    url.setPath(QStringLiteral("/api/accounts/") + leaf);
    url.setQuery(QString());
    return url;
}

QJsonObject MainWindow::postAccountSync(const QString &leaf,
                                        const QJsonObject &body, int *status)
{
    QNetworkRequest request(accountsApiUrl(leaf));
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    QNetworkReply *reply = m_networkAccess->post(
        request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    if (status)
        *status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray data = reply->readAll();
    reply->deleteLater();
    return QJsonDocument::fromJson(data).object();
}

QJsonObject MainWindow::postAccountSyncForInstance(
    const QString &instanceUrl, const QString &leaf,
    const QJsonObject &body, int *status, QString *transportError)
{
    if (status)
        *status = 0;
    if (transportError)
        transportError->clear();
    QNetworkRequest request(accountApiUrlForInstance(instanceUrl, leaf));
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    request.setTransferTimeout(15000);
    QNetworkReply *reply = m_networkAccess->post(
        request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    if (status)
        *status = reply->attribute(
                           QNetworkRequest::HttpStatusCodeAttribute)
                          .toInt();
    if (transportError && reply->error() != QNetworkReply::NoError)
        *transportError = forkmesh::networkReplyFailureSummary(reply);
    const QByteArray data = reply->readAll();
    reply->deleteLater();
    return QJsonDocument::fromJson(data).object();
}

QJsonObject MainWindow::getAccountSync(const QString &leaf, int *status)
{
    QNetworkReply *reply = m_networkAccess->get(QNetworkRequest(accountsApiUrl(leaf)));
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    if (status)
        *status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray data = reply->readAll();
    reply->deleteLater();
    return QJsonDocument::fromJson(data).object();
}

// The account record is read once per account per session. It carries the whole
// public record (including the base64 avatar PNG), it changes only when its
// owner edits it, and before this cache the app re-read it on a 60s timer and
// on every profile repaint — enough to make one URL a measurable slice of a
// session's network log. Login primes the cache; screens read it from here.
void MainWindow::cacheAccountRecord(const QString &account,
                                    const QJsonObject &record)
{
    const QString normalized = account.trimmed().toLower();
    if (normalized.isEmpty() ||
        !record.value(QStringLiteral("exists")).toBool())
        return;
    if (m_accountRecords.size() >= kAccountRecordCacheMax &&
        !m_accountRecords.contains(normalized))
        m_accountRecords.clear();
    m_accountRecords.insert(normalized, record);
    cacheWebUserSolanaProfile(normalized, record);
}

QJsonObject MainWindow::cachedAccountRecord(const QString &account) const
{
    return m_accountRecords.value(account.trimmed().toLower());
}

// Drop the cached copy so the next reader goes back to the relay. Called by the
// writes that change the record (avatar push, node linking, ownership transfer)
// — everything else is expected to reuse the copy read at login.
void MainWindow::invalidateAccountRecord(const QString &account)
{
    const QString normalized = account.trimmed().toLower();
    if (normalized.isEmpty())
        m_accountRecords.clear();
    else
        m_accountRecords.remove(normalized);
    // The payout address the top bar shows is a field of the record, so a
    // dropped record has to let refreshWebUserSolanaAddress() ask again too.
    // Only the "already asked" stamp is cleared: the address itself stays on
    // screen until the fresh read lands, rather than flickering back to the
    // machine-local one for the length of a request.
    if (normalized.isEmpty() || normalized == m_webSolanaAccount)
        m_webSolanaFetchedMs = 0;
}

#ifdef FORKMESH_WINDOW_TESTS
int MainWindow::testAccountRecordGets(const QString &account) const
{
    auto *manager = qobject_cast<BackoffNetworkAccessManager *>(m_networkAccess);
    if (!manager)
        return -1;
    // Blocked requests are counted before they are blocked, so this is what the
    // app asked for, not what the sandbox let through.
    const QString leaf = QStringLiteral("/api/accounts/") + account.toLower();
    int calls = 0;
    const QList<BackoffNetworkAccessManager::EndpointStats> stats =
        manager->endpointStats();
    for (const BackoffNetworkAccessManager::EndpointStats &row : stats) {
        if (row.method == QLatin1String("GET") && row.endpoint.endsWith(leaf))
            calls += row.calls;
    }
    return calls;
}
#endif

QUrl MainWindow::catalogListUrl()
{
    QUrl url = catalogApiUrl();
    url.setPath(QStringLiteral("/api/repositories"));
    url.setQuery(QString());
    const QString viewer = accountOwner();
    if (m_profileIdentity.isValid() && !viewer.isEmpty() &&
        hasOwnerSigningCapability(viewer)) {
        const QString ts = QString::number(QDateTime::currentMSecsSinceEpoch());
        const QByteArray canonical =
            ("forkmesh-catalog-view-v1\n" + viewer + "\n" + ts).toUtf8();
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("viewer"), viewer);
        query.addQueryItem(QStringLiteral("ts"), ts);
        query.addQueryItem(QStringLiteral("sig"),
                           m_profileIdentity.signData(canonical));
        url.setQuery(query);
    }
    return url;
}

QJsonArray MainWindow::fetchCatalogRepos()
{
    QNetworkReply *reply = m_networkAccess->get(QNetworkRequest(catalogListUrl()));
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
    reply->deleteLater();
    return obj.value("repositories").toArray();
}

int MainWindow::fetchNodesOnline()
{
    QUrl url = catalogApiUrl();
    url.setPath(QStringLiteral("/api/network/stats"));
    url.setQuery(QString());
    QNetworkReply *reply = m_networkAccess->get(QNetworkRequest(url));
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
    reply->deleteLater();
    return obj.value("hosts").toInt();
}

void MainWindow::mirrorCatalogRepo(const QString &owner, const QString &name,
                                   const QString &cloneUrl, bool isPrivate)
{
    if (owner.isEmpty() || name.isEmpty() || cloneUrl.isEmpty())
        return;
    for (int i = 0; i < m_repositories.size(); ++i) {
        const RepositoryRecord &r = m_repositories.at(i);
        if (!repositoryBelongsToActiveRelay(r) || r.owner != owner ||
            r.name != name)
            continue;
        if (r.previewOnly) {
            mirrorPreviewRepository(i);
            return;
        }
        return; // already mirroring this repo
    }
    RepositoryRecord repo;
    repo.owner = owner;
    repo.name = name;
    repo.cloneUrl = cloneUrl;
    repo.publishToNetwork = true;
    // A private repo discovered via the authenticated catalog — one we own, or
    // one shared with us — must keep its private flag so clone/fetch
    // attaches a view token (the owner's, or our grantee share token).
    repo.isPrivate = isPrivate;
    if (isPrivate) {
        const QString accessId =
            m_privateCatalogAccessIds.value(owner + "/" + name);
        if (PrivateMirrorStore::isOpaqueId(accessId))
            repo.privateReplicaId = accessId;
    }
    repo.actionsEnabled = false;
    repo.hostedSinceMs = QDateTime::currentMSecsSinceEpoch();
    repo.mirrorPath = repositoryMirrorRoot() + "/" +
                      repoSegment(owner, QStringLiteral("owner")) + "-" +
                      repoSegment(name, QStringLiteral("repository")) + ".git";
    stampRepositoryRelay(repo);
    m_repositories.append(repo);
    saveRepositories();
    if (m_backend)
        m_backend->addChannel(repositoryChannel(repo));
    refreshRepositoryList();
    logSystem("Mirroring " + owner + "/" + name + " from " + cloneUrl);
    syncRepository(m_repositories.size() - 1);
}

QString MainWindow::hostedCloneUrl(const QString &owner, const QString &name) const
{
    const QString safeOwner = repoSegment(owner, QStringLiteral("owner"));
    const QString safeName = repoSegment(name, QStringLiteral("repository"));
    if (safeOwner.isEmpty() || safeName.isEmpty())
        return QString();
    QUrl url = catalogApiUrl(); // http(s) on the mainnode host
    url.setPath(QStringLiteral("/") + safeOwner + QLatin1Char('/') + safeName);
    url.setQuery(QString());
    url.setFragment(QString());
    return url.toString();
}

bool MainWindow::ensureNodeAccount(const QString &accountName, const QString &solana)
{
#ifdef FORKMESH_WINDOW_TESTS
    if (m_testUseAccountFlowResult) {
        ++m_testEnsureNodeAccountCalls;
        if (m_testAccountFlowResult) {
            m_accountAuthenticated = true;
            m_accountName = accountName;
            m_accountTier = QStringLiteral("active");
            m_accountSolanaVerified = true;
            setDesktopCapability(accountName,
                                 m_testAccountFlowDesktopCapable);
        }
        Q_UNUSED(solana);
        return m_testAccountFlowResult;
    }
#endif
    Q_UNUSED(solana);
    if (hasOwnerSigningCapability(accountName))
        return true;
    if (!isValidNodeName(accountName)) {
        QMessageBox::warning(this, "Join the network",
                             "Choose a valid username first (lowercase letters, "
                             "numbers and hyphens; start with a letter).");
        return false;
    }

    if (authenticateSilently(accountName))
        return true;

    // authenticateSilently() just read this account; reuse its copy rather than
    // asking the relay for the same record twice in a row.
    QJsonObject lookup = cachedAccountRecord(accountName);
    if (lookup.isEmpty()) {
        int status = 0;
        lookup = getAccountSync(accountName, &status);
    }
    if (lookup.value("exists").toBool() &&
        lookup.value("status").toString() == "active")
        return runLoginFlow(accountName);

    // New (or unfinished) account: run the staged join (reserve → donate →
    // set credentials).
    return runSignupFlow(accountName, solana);
}

// Silent (no dialogs, no signup) authentication: the app is authenticated when
// this node already holds the key registered to an active account. The node key —
// not a password — is what the relay verifies for hosting/publishing, so this can
// run on launch without prompting. Returns false (quietly) when it can't confirm.
bool MainWindow::authenticateSilently(const QString &accountName)
{
    if (hasOwnerSigningCapability(accountName))
        return true;
    if (!isValidNodeName(accountName))
        return false;
    m_accountEmail.clear();
    if (m_accountAuthenticated &&
        AccountCapability::normalizedAccount(m_accountName) !=
            AccountCapability::normalizedAccount(accountName)) {
        setDesktopCapability(m_accountName, false);
        m_accountAuthenticated = false;
        m_accountEmail.clear();
        m_accountTier = QStringLiteral("free");
        m_accountSolanaVerified = false;
        m_isAdmin = false;
        updateAdminCrownBadge();
        QSettings().remove(kAuthedAccountSetting);
    }
    int status = 0;
    const QJsonObject lookup = getAccountSync(accountName, &status);
    // This is the once-per-session read of the account record: everything that
    // needs it later (payout address, linked nodes, admin flag) reads it back
    // out of the cache instead of re-fetching it.
    cacheAccountRecord(accountName, lookup);
    if (lookup.value("exists").toBool() &&
        lookup.value("status").toString() == "active" &&
        lookup.value("pubkey").toString() == m_profileIdentity.publicKey()) {
        m_accountAuthenticated = true;
        m_accountName = accountName;
        m_accountEmail = lookup.value("email").toString().trimmed();
        m_accountTier = QStringLiteral("active");
        m_accountSolanaVerified = true;
        setDesktopCapability(accountName, true);
        m_nodeOwnerUser = lookup.value("owner").toString();
        m_profileIsUserAccount =
            lookup.value(QStringLiteral("kind")).toString() ==
            QStringLiteral("user");
        QSettings().setValue(kAuthedAccountSetting, accountName);
        applyAccountEmailVerified(accountName,
                                  lookup.value("emailVerified").toBool());
        updateChatIdentity();
        if (m_settingsNameEdit && m_settingsNameEdit->text().trimmed().isEmpty())
            m_settingsNameEdit->setText(m_accountName);
        return true;
    }
    // The relay says the account is active but bound to another desktop key
    // (e.g. a password login from a second device). That mismatch only blocks
    // signed hosting auth from here — the account and its payout wallet are
    // already verified network members, so reflect that instead of nagging
    // "verify your payout wallet" on every launch.
    if (lookup.value("exists").toBool() &&
        lookup.value("status").toString() == "active") {
        m_accountSolanaVerified = true;
        m_accountEmail = lookup.value("email").toString().trimmed();
        setDesktopCapability(accountName, false);
    }
    // Trust a previously authenticated marker whenever the relay gave no
    // authoritative answer — unreachable (status 0), rate-limited or erroring
    // (429/5xx) — so a transient lookup failure doesn't demote a returning user
    // to unverified for the whole session. When the lookup DID answer, do not
    // treat a password-login cache as signed hosting auth unless the server
    // pubkey matched above. Publishing and heartbeat require this desktop's
    // Ed25519 key, not just an email/password session.
    const bool cachedHere =
        AccountCapability::normalizedAccount(
            QSettings().value(kAuthedAccountSetting).toString()) ==
        AccountCapability::normalizedAccount(accountName);
    if (cachedHere && status != 200 &&
        restoreDesktopCapability(accountName)) {
        m_accountAuthenticated = true;
        m_accountName = accountName;
        m_accountTier = QStringLiteral("active");
        m_accountSolanaVerified = true;
        m_accountEmail.clear();
        refreshSettingsEmailVerifiedBadge();
        return true;
    }
    if (status == 200)
        setDesktopCapability(accountName, false);
    return false;
}

namespace {

bool accountLoginResponseAccepted(const QString &requestedAccount,
                                  const QJsonObject &payload)
{
    return payload.value(QStringLiteral("ok")).toBool() &&
           payload.value(QStringLiteral("status"))
                   .toString() == QLatin1String("active") &&
           !payload.value(QStringLiteral("sessionToken"))
                .toString()
                .trimmed()
                .isEmpty() &&
           AccountCapability::loginResponseMatchesRequestedAccount(
               requestedAccount,
               payload.value(QStringLiteral("nodeName")).toString());
}

} // namespace

#ifdef FORKMESH_WINDOW_TESTS
bool MainWindow::testAccountLoginResponseAccepted(
    const QString &requestedAccount, const QJsonObject &payload) const
{
    return accountLoginResponseAccepted(requestedAccount, payload);
}
#endif

void MainWindow::activateAccountSession(const QJsonObject &payload,
                                        const QString &expectedAccount,
                                        bool ownsDesktopKey,
                                        const QString &instanceUrl)
{
    const QString returnedAccount =
        payload.value(QStringLiteral("nodeName")).toString();
    if (!AccountCapability::loginResponseMatchesRequestedAccount(
            expectedAccount, returnedAccount)) {
        return;
    }
    m_accountAuthenticated = true;
    m_accountName = AccountCapability::normalizedAccount(returnedAccount);
    m_accountEmail =
        payload.value(QStringLiteral("email")).toString().trimmed();
    m_accountTier = QStringLiteral("active");
    m_accountSolanaVerified = true;
    setDesktopCapability(m_accountName, ownsDesktopKey);
    QSettings().setValue(kAuthedAccountSetting, m_accountName);
    applyAccountEmailVerified(
        m_accountName,
        payload.value(QStringLiteral("emailVerified")).toBool());
    m_accountSessionToken =
        payload.value(QStringLiteral("sessionToken")).toString().trimmed();
    m_accountSessionInstanceUrl = m_accountSessionToken.isEmpty()
                                      ? QString()
                                      : instanceUrl.trimmed();
    m_profileIsUserAccount =
        payload.value(QStringLiteral("kind")).toString() ==
        QStringLiteral("user");
    m_isAdmin = payload.value(QStringLiteral("isAdmin")).toBool(false);
    m_seenPendingUsers.clear();
    m_adminPendingPolled = false;
    m_nodeOwnerUser =
        payload.value(QStringLiteral("owner")).toString().trimmed();
    m_profileLinkedNodes.clear();
    for (const QJsonValue &nodeValue :
         payload.value(QStringLiteral("nodes")).toArray()) {
        const QString node = nodeValue.toString().trimmed();
        if (!node.isEmpty() &&
            !m_profileLinkedNodes.contains(node, Qt::CaseInsensitive)) {
            m_profileLinkedNodes.append(node);
        }
    }
    updateAdminCrownBadge();
    rememberUserAccount(m_accountName);
    cacheWebUserSolanaProfile(m_accountName, payload);
    cacheActiveAccountSession(instanceUrl);
    if (ownsDesktopKey)
        ensurePrivateMirrorRecipientIdentityRegistered();
    const QString serverAvatar =
        payload.value(QStringLiteral("avatarPng")).toString();
    if (!serverAvatar.isEmpty()) {
        const QByteArray png =
            QByteArray::fromBase64(serverAvatar.toLatin1());
        if (!png.isEmpty() && png != m_userAvatar) {
            m_userAvatar = png;
            UserAvatarStore::store(png);
            updateAvatarButton();
            updateUserAvatarButton();
            updateChatIdentity();
        }
    } else {
        pushAccountAvatar();
    }
    updateUserSwitcher();
    // Away and the custom status: take the copy the sign-in answer carries,
    // else ask for it (an empty POST /api/accounts/user-status is a read).
    forkmesh::userstatus::UserStatus signedInStatus;
    if (forkmesh::userstatus::readFrom(payload, &signedInStatus))
        applyOwnUserStatus(signedInStatus);
    else
        QTimer::singleShot(0, this, [this] { fetchOwnUserStatus(); });
    if (payload.value(QStringLiteral("chatRetention")).isObject())
        applyChatRetention(
            payload.value(QStringLiteral("chatRetention")).toObject());
    else
        QTimer::singleShot(0, this, [this] { fetchChatRetention(); });
    startUserActivitySync();
    // A payout settled from public chain state while this machine was signed
    // out still owes its recipient an email. Signing in is the one event that
    // makes that possible, so it is the event that retries it — queued, since
    // this runs inside the sign-in that just produced the session.
    QTimer::singleShot(0, this,
                       [this] { flushSettledContributorNotices(); });
    m_accountPackagesReadMs = 0;
    QTimer::singleShot(0, this, [this] { refreshAccountPackages(true); });
    QTimer::singleShot(0, this, [this] { preparePackagePages(); });
    QTimer::singleShot(0, this, [this] { startOfficeChannelMirror(); });
}

bool MainWindow::verifyTotpLogin(const QString &email,
                                 const QString &password, const QString &totp,
                                 const QString &accountName, bool *fatal)
{
    if (fatal)
        *fatal = false;
    const QString requestedAccount =
        AccountCapability::normalizedAccount(accountName);
    QJsonObject loginRequest =
        AccountCapability::passwordLoginRequest(email, password, totp);
    const QString publicKey = m_profileIdentity.publicKey();
    if (!publicKey.isEmpty()) {
        const QString deviceTs =
            QString::number(QDateTime::currentMSecsSinceEpoch());
        const QByteArray deviceCanonical =
            ForkMeshIdentity::deviceBindCanonical(
                accountName, publicKey, deviceTs);
        const QString deviceSig =
            deviceCanonical.isEmpty()
                ? QString()
                : m_profileIdentity.signData(deviceCanonical);
        if (!deviceSig.isEmpty()) {
            loginRequest.insert(QStringLiteral("pubkey"), publicKey);
            loginRequest.insert(QStringLiteral("deviceTs"), deviceTs);
            loginRequest.insert(QStringLiteral("deviceSig"), deviceSig);
            loginRequest.insert(QStringLiteral("deviceLabel"),
                                machineNodeName());
        }
    }
    const QString loginInstance = currentAccountInstanceUrl();
    int status = 0;
    QString transportError;
    const QJsonObject resp = postAccountSyncForInstance(
        loginInstance, QStringLiteral("login"), loginRequest, &status,
        &transportError);

    auto rejectUnexpectedSession =
        [this, &loginInstance, &requestedAccount, fatal](
            const QJsonObject &payload) {
            const QString returnedAccount =
                AccountCapability::normalizedAccount(
                    payload.value(QStringLiteral("nodeName")).toString());
            const QString token =
                payload.value(QStringLiteral("sessionToken"))
                    .toString()
                    .trimmed();
            // A successful response also installs a website cookie. Revoke the
            // unexpected session on the exact origin that minted it before
            // returning control to the account chooser.
            if (!token.isEmpty()) {
                int logoutStatus = 0;
                postAccountSyncForInstance(
                    loginInstance, QStringLiteral("logout"),
                    QJsonObject{{QStringLiteral("sessionToken"), token}},
                    &logoutStatus);
            }
            if (fatal)
                *fatal = true;
            const QString rejectedText =
                returnedAccount.isEmpty()
                    ? QStringLiteral(
                          "The selected instance returned an invalid account "
                          "session. No account was switched.")
                    : QStringLiteral(
                          "That email and password are correct, but they "
                          "belong to the account @%1, not @%2. Go back, "
                          "enter %1 as your ForkMesh username, and log in "
                          "again.")
                          .arg(returnedAccount, requestedAccount);
            if (!forkmesh::ui::NextAccess::accountNotice(
                    *this, QStringLiteral("Log in"), rejectedText, true)) {
                QMessageBox::warning(this, QStringLiteral("Log in"),
                                     rejectedText);
            }
        };

    if (status == 200 && resp.value("ok").toBool()) {
        if (!accountLoginResponseAccepted(requestedAccount, resp)) {
            rejectUnexpectedSession(resp);
            return false;
        }
        const bool ownsDesktopKey =
            resp.value(QStringLiteral("deviceKeyMatched")).toBool(false) &&
            resp.value(QStringLiteral("desktopCapable")).toBool(false);
        activateAccountSession(resp, requestedAccount, ownsDesktopKey,
                               loginInstance);
        return true;
    }
    const QString err = resp.value("error").toString();

    if (err == QLatin1String("totp_setup_required")) {
        QMessageBox box(
            QMessageBox::Information,
            QStringLiteral("Set up two-factor authentication"),
            QStringLiteral(
                "Administrator accounts need two-factor authentication. Add "
                "this key to your authenticator app, then enter the 6-digit "
                "code it shows in the 2FA code field and log in again.\n\n%1")
                .arg(resp.value(QStringLiteral("totpSecret")).toString()),
            QMessageBox::Ok, this);
        box.setTextInteractionFlags(Qt::TextSelectableByMouse);
        box.exec();
        return false;
    }

    if (AccountCapability::allowsPasswordOnlyFallback(err)) {
        int webStatus = 0;
        const QJsonObject webResp = postAccountSyncForInstance(
            loginInstance, QStringLiteral("login"),
            AccountCapability::passwordLoginRequest(email, password, totp),
            &webStatus);
        if (webStatus == 200 && webResp.value("ok").toBool()) {
            if (!accountLoginResponseAccepted(requestedAccount, webResp)) {
                rejectUnexpectedSession(webResp);
                return false;
            }
            activateAccountSession(webResp, requestedAccount, false,
                                   loginInstance);
            const QString loggedInText =
                err == QLatin1String("device_proof_required")
                    ? "You are signed in with email/password for browsing and "
                      "account features. This device's identity proof was not "
                      "accepted, so publishing, hosting, and owner-signed "
                      "actions stay disabled. Check this computer's clock and "
                      "identity, then sign in again to restore them."
                : err == QLatin1String("device_key_conflict")
                    ? "You are signed in with email/password for browsing and "
                      "account features. The relay could not find an enabled, "
                      "verified device entry for this key on the selected account "
                      "and found conflicting ownership records, so publishing, "
                      "hosting, and owner-signed actions are paused. Do not revoke "
                      "an enabled entry for this computer. Import the selected "
                      "account's identity backup or ask an administrator to "
                      "reconcile the duplicate registration, then sign in again. "
                      "Your local identity key was not changed."
                    : "You are signed in with email/password, but this account "
                      "is already bound to a different desktop key. Browsing and "
                      "account features will work from this device; publishing, "
                      "hosting, and owner-signed actions require the original "
                      "desktop key or an explicit account-key rotation/import.";
            if (!forkmesh::ui::NextAccess::accountNotice(
                    *this, "Logged in", loggedInText, false)) {
                QMessageBox::information(this, "Logged in", loggedInText);
            }
            return true;
        }
    }

    if (fatal)
        *fatal = false;
    const QString failureText =
        err == "bad_totp"
            ? "Incorrect authenticator code."
        : err == "invalid_credentials"
            ? AccountCapability::incorrectPasswordMessage(
                  accountApiUrlForInstance(loginInstance,
                                           QStringLiteral("login"))
                      .host())
        : err == "too_many_attempts"
            ? AccountCapability::loginLockoutMessage(
                  qint64(resp.value(QStringLiteral("retryAfterMs"))
                             .toDouble()))
        : err == "pubkey_mismatch"
            ? "This account is already bound to another "
              "desktop key, and password-only fallback also "
              "failed. Use the original device, import its "
              "identity backup, or rotate the account key."
        : err == "device_proof_required"
            ? "This desktop could not prove possession of "
              "its identity key. Reload the identity or "
              "restart ForkMesh, then try again."
        : err == "device_key_conflict"
            ? "The relay could not find an enabled, verified "
              "device entry for this key on the selected "
              "account and found conflicting ownership "
              "records. Do not revoke an enabled entry for "
              "this computer. Import the selected account's "
              "identity backup or ask an administrator to "
              "reconcile the duplicate registration, then "
              "sign in again. ForkMesh has not changed your "
              "local identity key."
            : AccountCapability::loginFailureFallbackMessage(
                  status, err,
                  accountApiUrlForInstance(loginInstance,
                                           QStringLiteral("login"))
                      .host(),
                  transportError);
    if (!forkmesh::ui::NextAccess::accountNotice(*this, "Log in", failureText,
                                                 true)) {
        QMessageBox::warning(this, "Log in", failureText);
    }
    return false;
}

bool MainWindow::runLoginFlow(const QString &accountName)
{
    if (Theme::isNext())
        return forkmesh::ui::NextAccess::runLoginPanel(*this, accountName);
    QDialog dialog(this);
    dialog.setWindowTitle("Log in to " + accountName);
    auto *form = new QFormLayout(&dialog);
    auto *intro = new QLabel("Use the email and password you use on the "
                             "ForkMesh website. There is no separate desktop "
                             "password.");
    intro->setWordWrap(true);
    form->addRow(intro);
    auto *emailEdit = new QLineEdit;
    emailEdit->setPlaceholderText("you@example.com");
    auto *passEdit = new QLineEdit;
    passEdit->setEchoMode(QLineEdit::Password);
    forkmesh::ui::kit::addPasswordReveal(passEdit);
    auto *totpEdit = new QLineEdit;
    totpEdit->setPlaceholderText("6-digit code (only if you enabled 2FA)");
    totpEdit->setMaxLength(6);
    form->addRow("Email", emailEdit);
    form->addRow("Password", passEdit);
    form->addRow("2FA code", totpEdit);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText("Log in");
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    while (dialog.exec() == QDialog::Accepted) {
        const QString email = emailEdit->text().trimmed();
        if (!email.contains('@')) {
            QMessageBox::warning(this, "Log in", "Enter a valid email.");
            continue;
        }
        bool fatal = false;
        if (verifyTotpLogin(email, passEdit->text(), totpEdit->text().trimmed(),
                            accountName, &fatal))
            return true;
        if (fatal)
            break;
    }
    if (m_setupError) {
        m_setupError->setText("Account login is required to join the network.");
        m_setupError->show();
    }
    return false;
}

bool MainWindow::runSignupFlow(const QString &accountName, const QString &solana)
{
    Q_UNUSED(solana);
    if (!m_profileIdentity.isValid() && !m_profileIdentity.load()) {
        QMessageBox::warning(this, "Join the network", m_profileIdentity.errorString());
        return false;
    }

    QDialog dialog(this);
    dialog.setObjectName("signupWizard");
    dialog.setWindowTitle("Join ForkMesh");
    dialog.setModal(true);
    dialog.setMinimumWidth(440);

    auto *outer = new QVBoxLayout(&dialog);
    outer->setContentsMargins(28, 24, 28, 24);
    outer->setSpacing(8);

    auto *titleLabel = new QLabel("Choose your username");
    titleLabel->setObjectName("wizardTitle");
    titleLabel->setWordWrap(true);
    outer->addWidget(titleLabel);
    outer->addSpacing(4);

    auto *nameEdit = new QLineEdit;
    nameEdit->setMaxLength(63);
    nameEdit->setPlaceholderText("ada-lovelace");
    nameEdit->setText(accountName);
    auto *hint = new QLabel;
    hint->setObjectName("modeHint");
    hint->setWordWrap(true);
    auto *joinBtn = new QPushButton("Join ForkMesh");
    joinBtn->setObjectName("primaryButton");
    joinBtn->setMinimumHeight(38);
    outer->addWidget(nameEdit);
    outer->addWidget(hint);
    outer->addSpacing(4);
    outer->addWidget(joinBtn);

    bool joined = false;

    auto styleHint = [](QLabel *h, const QString &text, const char *color) {
        h->setText(text);
        h->setStyleSheet(Theme::remapSheet(color ? QStringLiteral("color:%1; background:transparent;")
                                     .arg(QString::fromUtf8(color))
                               : QStringLiteral("background:transparent;")));
    };
    styleHint(hint, "Lowercase letters, numbers and hyphens. Start with a letter. "
                    "Your username is public, and joining is free.", nullptr);

    auto *availTimer = new QTimer(&dialog);
    availTimer->setSingleShot(true);
    availTimer->setInterval(350);
    connect(nameEdit, &QLineEdit::textChanged, this, [=]() {
        const QString v = nameEdit->text().trimmed().toLower();
        joinBtn->setEnabled(false);
        if (v.isEmpty()) {
            styleHint(hint, "Your username is public, and joining is free.", nullptr);
            return;
        }
        if (!isValidNodeName(v)) {
            styleHint(hint, "Use lowercase letters, numbers and hyphens; start "
                            "with a letter.", "#f85149");
            return;
        }
        styleHint(hint, "Checking availability…", nullptr);
        availTimer->start();
    });
    connect(availTimer, &QTimer::timeout, this, [=]() {
        const QString v = nameEdit->text().trimmed().toLower();
        if (!isValidNodeName(v))
            return;
        int status = 0;
        const QJsonObject look = getAccountSync(v, &status);
        if (status == 0) { // relay unreachable: allow continuing
            styleHint(hint, "Couldn't check availability — you can still continue.",
                      nullptr);
            joinBtn->setEnabled(true);
            return;
        }
        if (look.value("exists").toBool() && !look.value("available").toBool()) {
            styleHint(hint, "That username is already taken — try another.", "#f85149");
            joinBtn->setEnabled(false);
        } else {
            styleHint(hint, QString::fromUtf8("\xE2\x80\x9C%1\xE2\x80\x9D is available.")
                                .arg(v), "#3fb950");
            joinBtn->setEnabled(true);
        }
    });

    // Reserve the name (binding it to this device key), then activate it for free
    // by finalizing with no email/password. Both calls are signed with the local
    // identity; the finalize signature covers an empty email segment, matching the
    // relay's canonical string for a key-bound, credential-less join.
    auto doJoin = [&]() {
        const QString name = nameEdit->text().trimmed().toLower();
        if (!isValidNodeName(name))
            return;
        joinBtn->setEnabled(false);
        joinBtn->setText("Joining…");

        const QString rts = QString::number(QDateTime::currentMSecsSinceEpoch());
        const QByteArray rcanon =
            ("forkmesh-reserve-v1\n" + name + "\n" + rts).toUtf8();
        int rstatus = 0;
        const QJsonObject rresp = postAccountSync(
            "reserve",
            QJsonObject{{"nodeName", name},
                        {"pubkey", m_profileIdentity.publicKey()},
                        {"ts", rts},
                        {"sig", m_profileIdentity.signData(rcanon)}},
            &rstatus);
        if (!rresp.value("ok").toBool()) {
            joinBtn->setText("Join ForkMesh");
            joinBtn->setEnabled(true);
            styleHint(hint, "Could not reserve that username — it may be taken. "
                            "Try another.", "#f85149");
            return;
        }

        const QString fts = QString::number(QDateTime::currentMSecsSinceEpoch());
        const QByteArray fcanon =
            ("forkmesh-finalize-v1\n" + name + "\n\n" + fts).toUtf8();
        int fstatus = 0;
        const QJsonObject fresp = postAccountSync(
            "finalize",
            QJsonObject{{"nodeName", name},
                        {"email", QString()},
                        {"password", QString()},
                        {"pubkey", m_profileIdentity.publicKey()},
                        {"ts", fts},
                        {"sig", m_profileIdentity.signData(fcanon)}},
            &fstatus);
        joinBtn->setText("Join ForkMesh");
        joinBtn->setEnabled(true);
        if (fstatus != 201 || !fresp.value("ok").toBool()) {
            styleHint(hint, "Could not join right now. Please try again.", "#f85149");
            return;
        }
        m_accountAuthenticated = true;
        m_accountName = name;
        m_accountEmail.clear();
        m_accountTier = QStringLiteral("active");
        m_accountSolanaVerified = true; // joined = active network member
        setDesktopCapability(name, true);
        QSettings().setValue(kAuthedAccountSetting, name);
        joined = true;
        dialog.accept();
    };
    connect(joinBtn, &QPushButton::clicked, this, doJoin);
    connect(nameEdit, &QLineEdit::returnPressed, this, [&]() {
        if (joinBtn->isEnabled())
            doJoin();
    });

    joinBtn->setEnabled(isValidNodeName(accountName));

    dialog.exec();
    if (joined) {
        QTimer::singleShot(0, this, [this] {
            flashMessage(QString::fromUtf8(
                "You\xE2\x80\x99re in \xE2\x80\x94 your username is registered."));
        });
        return true;
    }
    if (m_setupError) {
        m_setupError->setText("Joining was cancelled. You can keep using ForkMesh "
                              "for free, or join again any time.");
        m_setupError->show();
    }
    return false;
}

void MainWindow::verifyWallet()
{
    const QString address = savedSolanaAddress().trimmed();
    if (address.isEmpty() ||
        !forkmesh::control::isValidSolanaPublicAddress(address)) {
        QMessageBox::information(
            this, QStringLiteral("Reward settings"),
            QStringLiteral(
                "Add one valid public self-custodial Solana payout address. "
                "Never enter a private key, seed, mnemonic, or recovery phrase."));
        return;
    }
    const QString name = accountOwner();
    if (name.isEmpty() || !hasOwnerSigningCapability(name)) {
        QMessageBox::information(
            this, QStringLiteral("Reward settings"),
            QStringLiteral(
                "Register or sign in to this node from Account settings first. "
                "Checking reward settings does not create a wallet or request a deposit, "
                "and it does not register an account."));
        return;
    }
    sendNodeHeartbeat();
    if (m_profileEligibility)
        m_profileEligibility->setText(
            QString::fromUtf8(
                "<span style='color:#3fb950'>Public payout address configured "
                "\xC2\xB7 may be eligible</span>"));
    updateSolanaNotice();
    flashMessage(QStringLiteral(
        "Reward settings checked. Selection and payment are not guaranteed."));
}

void MainWindow::persistProfile()
{
    const QString name = accountNameFromInput(m_nameEdit->text(), m_userName);
    m_nameEdit->setText(name);
    saveProfileName(name);
    const QString address = m_solanaEdit->text().trimmed();
    if (address.isEmpty() ||
        forkmesh::control::isValidSolanaPublicAddress(address)) {
        saveSolanaAddress(address);
    } else {
        m_solanaEdit->clear();
        saveSolanaAddress(QString());
        flashMessage(
            QStringLiteral(
                "Invalid payout-address text was removed. ForkMesh accepts only "
                "a public Solana address, never a private key or recovery phrase."),
            true);
    }
    updateNavSolanaBalance();
}


void MainWindow::showUpdateLog()
{
    if (!m_updateLogDialog) {
        m_updateLogDialog = new QDialog(this);
        m_updateLogDialog->setWindowTitle(QStringLiteral("Updating ForkMesh"));
        m_updateLogDialog->resize(780, 480);
        auto *intro = new QLabel(QStringLiteral(
            "Live update log. The app relaunches automatically once the rebuild "
            "finishes; if a step fails this window stays open so you can read "
            "exactly what went wrong."));
        intro->setObjectName("statusLine");
        intro->setWordWrap(true);
        m_updateLog = new QPlainTextEdit;
        m_updateLog->setReadOnly(true);
        m_updateLog->setObjectName("actionLog");
        m_updateLog->setLineWrapMode(QPlainTextEdit::NoWrap);
        m_updateLog->setMaximumBlockCount(50000);
        applyLogFont(m_updateLog);
        new AgentLogHighlighter(m_updateLog->document());
        auto *copyBtn = new QPushButton(QStringLiteral("Copy log"));
        copyBtn->setObjectName("ghostButton");
        copyBtn->setCursor(Qt::PointingHandCursor);
        auto *copyStatus = new QLabel;
        copyStatus->setObjectName("statusLine");
        copyStatus->setWordWrap(true);
        copyStatus->hide();
        connect(copyBtn, &QPushButton::clicked, this, [this, copyStatus] {
            if (!m_updateLog)
                return;
            QApplication::clipboard()->setText(m_updateLog->toPlainText());
            showLocalNotice(copyStatus, QStringLiteral("Update log copied to clipboard."));
        });
        auto *closeBtn = new QPushButton(QStringLiteral("Close"));
        closeBtn->setObjectName("ghostButton");
        closeBtn->setCursor(Qt::PointingHandCursor);
        connect(closeBtn, &QPushButton::clicked, m_updateLogDialog, &QDialog::hide);
        auto *row = new QHBoxLayout;
        row->addStretch();
        row->addWidget(copyBtn);
        row->addWidget(closeBtn);
        auto *lay = new QVBoxLayout(m_updateLogDialog);
        lay->addWidget(intro);
        lay->addWidget(m_updateLog, 1);
        lay->addLayout(row);
        lay->addWidget(copyStatus);
    }
    m_updateFeed.reset();
    m_updateLog->clear();
    appendUpdateLog(QStringLiteral("==> ForkMesh update started\n"));
}

void MainWindow::appendUpdateLog(const QString &text)
{
    m_updateFeed.append(text);
    if (!m_updateLog || text.isEmpty())
        return;
    m_updateLog->moveCursor(QTextCursor::End);
    m_updateLog->insertPlainText(text);
    m_updateLog->moveCursor(QTextCursor::End);
    for (QTextBlock b = m_updateLog->document()->lastBlock(); b.isValid();
         b = b.previous()) {
        const QString line = b.text().trimmed();
        if (!line.isEmpty()) {
            setFooterUpdateLine(line);
            break;
        }
    }
}

// Both log views paint the same line HTML, which carries its own colours, so
// they take the same light chrome under it whatever the app theme is. The
// debug tail keeps a hairline on top to stay parted from the debug bar.
static QString logViewStyleSheet(const QString &objectName, bool topBorder)
{
    const QString selector = QStringLiteral("QTextEdit#") + objectName;
    return QStringLiteral(
               "%1{color:#1f2328;border:none;%2"
               "background:#ffffff;font-family:monospace;font-size:11px;"
               "padding:3px 1px 3px 12px;}"
               "%1 QScrollBar:vertical{"
               "background:#ffffff;width:9px;margin:0;}"
               "%1 QScrollBar::handle:vertical{"
               "background:#d0d7de;border-radius:4px;min-height:24px;}"
               "%1 QScrollBar::handle:vertical:"
               "hover{background:#afb8c1;}"
               "%1 QScrollBar::add-line:vertical,"
               "%1 QScrollBar::sub-line:vertical{"
               "height:0;}"
               "%1 QScrollBar::add-page:vertical,"
               "%1 QScrollBar::sub-page:vertical{"
               "background:#ffffff;}")
        .arg(selector,
             topBorder ? QStringLiteral("border-top:1px solid #d0d7de;")
                       : QString());
}

void MainWindow::styleFooterUpdateLog()
{
    if (m_footerUpdateLog)
        m_footerUpdateLog->setStyleSheet(Theme::remapSheet(logViewStyleSheet(
            QStringLiteral("footerUpdateLog"), /*topBorder=*/false)));
    if (m_debugLogTail)
        m_debugLogTail->setStyleSheet(Theme::remapSheet(logViewStyleSheet(
            QStringLiteral("debugLogTail"), /*topBorder=*/true)));
}

QString MainWindow::footerLogLineHtml(const QString &clean, QTextEdit *view)
{
    QString time, message = clean;
    if (clean.size() >= 21 && clean.at(10) == QLatin1Char(' ')) {
        time = clean.mid(11, 8);
        message = clean.mid(21);
    }
    QString sourcePath;
    int sourceLine = 0;
    {
        QString body;
        if (forkmesh::splitLogSource(message, &body, &sourcePath, &sourceLine))
            message = body;
    }
    const QString badge = logBadgeFor(clean);
    const QString accent = logAccentFor(clean);
    const QString nodeHtml = forkmesh::corelogs::nodeLineHtml(
        message, false,
        forkmesh::ui::LogActivityLights::accentForBadge(
            forkmesh::corelogs::nodeLineBadge(message, false)));
    QString html;
    html += logPromptIconTag(view, clean);
    html += logFaviconTag(message, view);
    if (!time.isEmpty())
        html += QStringLiteral("<span style='color:#656d76'>%1</span>&nbsp;&nbsp;")
                    .arg(time);
    html += QStringLiteral(
                "<span style='color:%1; font-weight:700'>%2</span>&nbsp;&nbsp;"
                "<span style='color:#1f2328'>%3</span>")
                .arg(accent,
                     badge.toHtmlEscaped() +
                         QStringLiteral("&nbsp;")
                             .repeated(qMax(0, 8 - badge.size())),
                     nodeHtml.isEmpty()
                         ? forkmesh::colorizeBackgroundMarker(message.toHtmlEscaped())
                         : nodeHtml);
    html += logSourceAnchorHtml(sourcePath, sourceLine, /*dark=*/false);
    return html;
}

void MainWindow::pulseLogActivityLights(const QString &badge)
{
    ++m_debugAllLogCount;
    if (auto *allButton = dynamic_cast<ActivityRailButton *>(m_debugLogAllButton))
        allButton->setBadgeCount(m_debugAllLogCount);
    if (m_logActivityLights)
        m_logActivityLights->pulse(badge);
    if (m_logActivityHeader)
        m_logActivityHeader->pulse(badge);
}

// The five-line tail that drops out from under the debug bar. It is seeded
// from the whole buffer, so it has always shown CLOUD lines on open;
// logSystemFrom() feeds it those directly because they are routed around the
// footer tail and would otherwise stop arriving the moment the tail went live.
// Lines render exactly as they do in the footer log — prompt icon, background
// task mark, favicon, badge colour and source anchor — and the raw line rides
// along on the block so clicks and tooltips can read it back.
void MainWindow::appendDebugLogTailLine(const QString &line)
{
    if (!m_debugLogTail || !m_debugLogTailShown)
        return;
    const QString clean = line.trimmed();
    if (clean.isEmpty())
        return;
    m_debugLogTailPending.append(clean);
    if (m_debugLogTailPending.size() > kDebugLogTailLines)
        m_debugLogTailPending.removeFirst();
    armFooterLogFlush();
}

namespace {

constexpr int kFooterLogTrimSlack = 64;

}

void MainWindow::setFooterUpdateLine(const QString &line)
{
    const QString badge = logBadgeFor(line);
    pulseLogActivityLights(badge);
    const QString clean = line.trimmed();
    if (clean.isEmpty())
        return;
    appendDebugLogTailLine(clean);
    if (!m_footerUpdateLog)
        return;
    m_footerLogPending.append(clean);
    if (m_footerLogPending.size() > kFooterLogSeedLines + kFooterLogTrimSlack)
        m_footerLogPending.removeFirst();
    armFooterLogFlush();
}

void MainWindow::armFooterLogFlush()
{
    if (!m_footerLogFlushTimer) {
        m_footerLogFlushTimer = new QTimer(this);
        m_footerLogFlushTimer->setSingleShot(true);
        m_footerLogFlushTimer->setInterval(50);
        connect(m_footerLogFlushTimer, &QTimer::timeout, this,
                &MainWindow::flushFooterLogLines);
    }
    if (!m_footerLogFlushTimer->isActive())
        m_footerLogFlushTimer->start();
}

void MainWindow::flushFooterLogLines()
{
    if (m_footerLogFlushTimer)
        m_footerLogFlushTimer->stop();
    const auto appendLines = [this](QTextEdit *view, QStringList lines,
                                    int keep, int slack) {
        QTextDocument *doc = view->document();
        QTextCursor batch(doc);
        batch.beginEditBlock();
        if (keep > 0 && lines.size() >= keep) {
            lines = lines.mid(lines.size() - keep);
            batch.select(QTextCursor::Document);
            batch.removeSelectedText();
        }
        for (const QString &clean : std::as_const(lines)) {
            view->append(footerLogLineHtml(clean, view));
            if (QTextBlock last = doc->lastBlock(); last.isValid())
                last.setUserData(new FooterLogLineData(clean));
        }
        if (const int excess = doc->blockCount() - keep - slack; excess > 0) {
            QTextCursor trim(doc);
            trim.movePosition(QTextCursor::Start);
            trim.movePosition(QTextCursor::NextBlock, QTextCursor::KeepAnchor,
                              excess + slack);
            trim.removeSelectedText();
        }
        batch.endEditBlock();
    };
    if (const QStringList lines = std::exchange(m_debugLogTailPending, {});
        !lines.isEmpty() && m_debugLogTail) {
        // QTextEdit has no maximumBlockCount, so the cap is kept by hand.
        appendLines(m_debugLogTail, lines, kDebugLogTailLines, 0);
        if (QScrollBar *tailBar = m_debugLogTail->verticalScrollBar())
            tailBar->setValue(tailBar->maximum());
    }
    if (const QStringList lines = std::exchange(m_footerLogPending, {});
        !lines.isEmpty() && m_footerUpdateLog) {
        QScrollBar *bar = m_footerUpdateLog->verticalScrollBar();
        const bool follow = !m_footerLogScrollPaused ||
                            (bar && bar->value() >= bar->maximum());
        // Trim in batches. Every removal relaid the document out — a second edit on
        // top of the append, once per logged line, and stalls were sampled inside
        // QTextDocument::finishEdit() from here. Letting the tail run a little long
        // between trims costs a few off-screen blocks and one relayout per batch.
        appendLines(m_footerUpdateLog, lines, kFooterLogSeedLines,
                    kFooterLogTrimSlack);
        if (follow && bar)
            bar->setValue(bar->maximum());
    }
}

void MainWindow::setUpdateStatus(const QString &status, bool isError)
{
    logRestart(isError ? QStringLiteral("ERROR: %1").arg(status) : status);
    appendUpdateLog((isError ? QStringLiteral("!! ") : QStringLiteral("==> ")) + status +
                    QStringLiteral("\n"));
    m_updateFeed.noteStatus(status, isError);
    flushLocalControlUpdate();
    if (isError && m_updateLogDialog) {
        m_updateLogDialog->show();
        m_updateLogDialog->raise();
        m_updateLogDialog->activateWindow();
    }
    QLabel *label = m_buildStatusLabel ? m_buildStatusLabel : m_updateStatus;
    if (!label)
        return;
    label->setStyleSheet(Theme::remapSheet(isError ? "color:#ff6b6b; background:transparent;"
                                  : "color:#9ca3af; background:transparent;"));
    label->setText(status);
    label->show();
}

#ifdef FORKMESH_WINDOW_TESTS
QStringList MainWindow::testQuickUpdatePullArguments(const QString &clientDir) const
{
    return quickUpdatePullArguments(clientDir);
}

QString MainWindow::testWorkingClientDir() const
{
    return workingClientDir();
}

QString MainWindow::testRunningClientDir() const
{
    return runningClientDir();
}

QString MainWindow::testRunningClientExecutable() const
{
    return runningClientExecutable();
}

QStringList MainWindow::testBuildAndPreviewSteps(const QString &gitDir,
                                                 const QString &previewDir,
                                                 const QString &clientDir,
                                                 const QString &buildDir,
                                                 const QString &commit,
                                                 bool haveWorktree) const
{
    QStringList lines;
    for (const PullPreviewStep &step :
         pullPreviewSteps(gitDir, previewDir, clientDir, buildDir, commit,
                          haveWorktree, /*jobs=*/4))
        lines << (QStringList{step.program} + step.args).join(QLatin1Char(' '));
    return lines;
}
#endif

void MainWindow::runUpdateStep(const QString &program, const QStringList &arguments,
                               const QString &workingDir,
                               std::function<void()> onSuccess,
                               std::function<void()> onFailure,
                               const QMap<QString, QString> &extraEnv)
{
    QStringList envPrefix;
    for (auto it = extraEnv.cbegin(); it != extraEnv.cend(); ++it)
        envPrefix << (it.key() + QLatin1Char('=') + it.value());
    const QString commandLine =
        (envPrefix + QStringList{program} + arguments).join(QLatin1Char(' '));
    logRestart(QStringLiteral("run: %1").arg(commandLine));
    appendUpdateLog(QStringLiteral("\n$ %1\n  (in %2)\n").arg(commandLine, workingDir));
    QElapsedTimer stepTimer;
    stepTimer.start();
    auto *process = new QProcess(this);
    process->setWorkingDirectory(workingDir);
    // Own process group, so cancelling can signal the whole build tree (cmake
    // hands the real work to ninja/make children) and not just the direct child.
#ifndef Q_OS_WIN
    process->setChildProcessModifier([] { ::setsid(); });
#endif
    m_updateSteps.append(QPointer<QProcess>(process));
    const int token = m_updateRunToken;
    if (!extraEnv.isEmpty()) {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        for (auto it = extraEnv.cbegin(); it != extraEnv.cend(); ++it)
            env.insert(it.key(), it.value());
        process->setProcessEnvironment(env);
    }
    process->setProcessChannelMode(QProcess::MergedChannels);
    auto output = std::make_shared<QString>();
    connect(process, &QProcess::readyReadStandardOutput, this,
            [this, process, output, token] {
        const QString chunk = QString::fromUtf8(process->readAllStandardOutput());
        if (token != m_updateRunToken)
            return; // cancelled; whatever it still prints is not our business
        output->append(chunk);
        if (output->size() > 200000)
            *output = output->right(200000);
        appendUpdateLog(chunk);
        noteRestartStepOutput(chunk);
    });
    connect(process, &QProcess::finished, this,
            [this, process, stepTimer, commandLine, onSuccess, onFailure, output,
             token](int exitCode, QProcess::ExitStatus) {
                m_updateSteps.removeAll(QPointer<QProcess>(process));
                if (token != m_updateRunToken) {
                    process->deleteLater();
                    return; // cancelled: no status, no next step
                }
                const QString tail = QString::fromUtf8(process->readAllStandardOutput());
                if (!tail.isEmpty()) {
                    output->append(tail);
                    appendUpdateLog(tail);
                }
                process->deleteLater();
                logRestart(QStringLiteral("done in %1ms (exit %2): %3")
                               .arg(stepTimer.elapsed())
                               .arg(exitCode)
                               .arg(commandLine));
                appendUpdateLog(QString::fromUtf8("\xE2\x80\x94 finished in %1ms (exit %2)\n")
                                    .arg(stepTimer.elapsed())
                                    .arg(exitCode));
                if (exitCode != 0) {
                    if (onFailure) {
                        onFailure();
                        return;
                    }
                    stopRefreshSpin();
                    stopRestartSpin();
                    setUpdateStatus("Update failed: " + updateFailureSummary(*output),
                                    true);
                    if (m_buildButton)
                        m_buildButton->setEnabled(true);
                    return;
                }
                onSuccess();
            });
    connect(process, &QProcess::errorOccurred, this,
            [this, process, stepTimer, commandLine, token] {
        m_updateSteps.removeAll(QPointer<QProcess>(process));
        if (token != m_updateRunToken) {
            process->deleteLater();
            return; // cancelled: the failure is ours, not the user's to see
        }
        stopRefreshSpin();
        stopRestartSpin();
        logRestart(QStringLiteral("failed to start after %1ms: %2")
                       .arg(stepTimer.elapsed())
                       .arg(commandLine));
        setUpdateStatus("Update failed: could not run " + process->program(), true);
        process->deleteLater();
        if (m_buildButton)
            m_buildButton->setEnabled(true);
    });
    process->start(program, arguments);
}

void MainWindow::runQuickUpdate()
{
    startRestartCautionFlash();
    beginRestartLog();
    showUpdateLog();
    logRestart(QStringLiteral("quick update started"));
    logSystem(QStringLiteral("=== Quick update started (will rebuild & restart) ==="));
    saveProfileName(m_nameEdit->text());
    m_buildButton = m_updateButton;
    m_buildStatusLabel = m_updateStatus;
    m_updateButton->setEnabled(false);
    const QString clientDir = runningClientDir();
    const QString relaunchPath = runningClientExecutable();

    if (QDir(clientDir).exists("CMakeLists.txt")) {
        setUpdateStatus("Pulling the latest version...");
        runUpdateStep("git", quickUpdatePullArguments(clientDir), clientDir,
                      [this, clientDir, relaunchPath] {
            buildAndRelaunch(clientDir, QString(), relaunchPath);
        });
    } else {
        const QString repoDir = QFileInfo(clientDir).absolutePath(); // .../src
        QDir().mkpath(QFileInfo(repoDir).absolutePath());
        setUpdateStatus("Downloading the latest version...");
        runUpdateStep("git", {"clone", "--depth", "1", kRepoUrl, repoDir},
                      QFileInfo(repoDir).absolutePath(),
                      [this, clientDir, relaunchPath] {
            buildAndRelaunch(clientDir, QString(), relaunchPath);
        });
    }
}

void MainWindow::runUpdateStepUser(const QString &program,
                                   const QStringList &arguments,
                                   const QString &workingDir,
                                   std::function<void()> onSuccess,
                                   std::function<void()> onFailure,
                                   const QMap<QString, QString> &extraEnv)
{
    if (m_updateAsUser.isEmpty()) {
        runUpdateStep(program, arguments, workingDir, std::move(onSuccess),
                      std::move(onFailure), extraEnv);
        return;
    }
    QStringList wrapped{"-u", m_updateAsUser, "-H"};
    if (!extraEnv.isEmpty()) {
        wrapped << "env";
        for (auto it = extraEnv.cbegin(); it != extraEnv.cend(); ++it)
            wrapped << (it.key() + QLatin1Char('=') + it.value());
    }
    wrapped << program;
    wrapped += arguments;
    runUpdateStep("sudo", wrapped, workingDir, std::move(onSuccess),
                  std::move(onFailure));
}

bool MainWindow::restartInProgress() const
{
    // The spin button is set the moment a restart button is clicked and cleared
    // by every finish/failure path, so it is the one honest "still going" flag.
    if (m_restartSpinButton || m_rebuildRestartQueued)
        return true;
    // An auto-update rebuild has no spin button; cancelling it beats letting a
    // manual click start a second build in the same build directory.
    for (const QPointer<QProcess> &step : m_updateSteps)
        if (step && step->state() != QProcess::NotRunning)
            return true;
    return false;
}

bool MainWindow::cancelRestartIfRunning()
{
    if (!restartInProgress())
        return false;
    cancelRestart();
    return true;
}

void MainWindow::killUpdateSteps()
{
    const QList<QPointer<QProcess>> steps = m_updateSteps;
    m_updateSteps.clear();
    for (const QPointer<QProcess> &step : steps) {
        if (!step || step->state() == QProcess::NotRunning)
            continue;
#ifndef Q_OS_WIN
        // Every step got its own session, so the negative pid reaches the whole
        // build tree; terminating cmake alone would leave ninja compiling.
        if (const qint64 pid = step->processId(); pid > 0)
            ::kill(static_cast<pid_t>(-pid), SIGTERM);
#endif
        step->terminate();
        QPointer<QProcess> pending = step;
        QTimer::singleShot(1500, this, [pending] {
            if (!pending || pending->state() == QProcess::NotRunning)
                return;
#ifndef Q_OS_WIN
            if (const qint64 pid = pending->processId(); pid > 0)
                ::kill(static_cast<pid_t>(-pid), SIGKILL);
#endif
            pending->kill();
        });
    }
}

void MainWindow::cancelRestart()
{
    ++m_updateRunToken; // orphans every in-flight step callback
    m_rebuildRestartQueued = false;
    if (m_rebuildQueuePollTimer)
        m_rebuildQueuePollTimer->stop();
    killUpdateSteps();
    m_updateAsUser.clear();
    stopRefreshSpin();
    stopRestartSpin(); // also stops the caution flash
    logSystem(QStringLiteral("=== Restart cancelled ==="));
    setUpdateStatus(QString::fromUtf8(
        "Restart cancelled. Nothing was installed \xE2\x80\x94 this is still "
        "the build you were already running."));
    if (m_buildButton)
        m_buildButton->setEnabled(true);
    if (m_rebuildButton)
        m_rebuildButton->setEnabled(true);
    if (m_updateButton)
        m_updateButton->setEnabled(true);
    flashMessage(QStringLiteral("Restart cancelled."));
}

void MainWindow::buildAndRelaunch(const QString &clientDir, const QString &asUser,
                                  const QString &relaunchPath, const QString &buildType)
{
    m_updateAsUser = asUser;
    const QString appPath = relaunchPath.isEmpty()
                                ? QCoreApplication::applicationFilePath()
                                : relaunchPath;
    const QString buildDir = clientDir + "/build";

    const qint64 freeBytes = freeBytesForPath(buildDir);
    const qint64 requiredBytes = rebuildFreeBytesRequired(buildDir);
    if (freeBytes >= 0 && freeBytes < requiredBytes) {
        stopRefreshSpin();
        stopRestartSpin();
        setUpdateStatus(QStringLiteral("Update cancelled: only %1 free on the "
                                       "volume holding %2, and the rebuild "
                                       "needs at least %3. Free some space and "
                                       "try again — nothing was changed.")
                            .arg(formatByteSize(freeBytes), buildDir,
                                 formatByteSize(requiredBytes)),
                        true);
        if (m_buildButton)
            m_buildButton->setEnabled(true);
        return;
    }

    const QString scratchDir = buildScratchDir(buildDir);
    setRestartPhase(QStringLiteral("configuring"));
    setRestartSpinProgress(35);
    setUpdateStatus("Configuring...");
    const QString generator = cmakeGeneratorForRebuild();
    // A tree already configured for this source, build type and generator is
    // built directly: cmake --build re-runs the generator itself when a
    // CMakeLists.txt changed, so the explicit reconfigure only ever cost time.
    // A tree left by another generator drops that generator's entries first
    // (never the objects or the built binary), because CMake will not switch
    // an existing cache in place.
    const bool configured =
        cmakeCacheMatches(clientDir, buildDir, buildType, generator);
    const QStringList staleEntries =
        cmakeGeneratorEntriesToDrop(buildDir, generator);
    for (const QString &entry : staleEntries + QStringList{scratchDir}) {
        const QString refused = forkmesh::safedelete::refusal(entry, buildDir);
        if (refused.isEmpty())
            continue;
        stopRefreshSpin();
        stopRestartSpin();
        setUpdateStatus(QStringLiteral("Update cancelled: refusing to delete %1 (%2). "
                                       "Nothing was changed.")
                            .arg(entry, refused),
                        true);
        if (m_buildButton)
            m_buildButton->setEnabled(true);
        return;
    }
    const QString cacheHome =
        QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
    QMap<QString, QString> buildEnv =
        asUser.isEmpty()
            ? forkmesh::restarttiming::ccacheEnvironment(
                  qEnvironmentVariable("CCACHE_DIR"),
                  forkmesh::fastdisk::ccacheDir(forkmesh::fastdisk::activeRoot()),
                  cacheHome, freeBytesForPath(cacheHome))
            : QMap<QString, QString>();
    if (buildEnv.contains(QStringLiteral("CCACHE_DIR"))) {
        QDir().mkpath(buildEnv.value(QStringLiteral("CCACHE_DIR")));
        logRestart(QStringLiteral("compile cache: %1 (up to %2)")
                       .arg(buildEnv.value(QStringLiteral("CCACHE_DIR")),
                            buildEnv.value(QStringLiteral("CCACHE_MAXSIZE"))));
    }
    const QString cacheStats = scratchDir + QStringLiteral("/ccache-stats.log");
    buildEnv.insert(QStringLiteral("TMPDIR"), scratchDir);
    buildEnv.insert(QStringLiteral("CCACHE_STATSLOG"), cacheStats);
    // The build reads the checkout this app keeps merging into. A session
    // that finishes while the compiler runs lands its branch here, and a
    // header changed between the precompiled header and a source that uses it
    // fails the whole build ("redefinition of ..."), or worse, links objects
    // from two different trees. So the build step is a pass: when anything
    // under the sources changed while it ran, the step runs again — an
    // incremental build, only the touched files recompile — and only a pass
    // that finished on an unchanged tree installs. Bounded, so a tree that
    // will not sit still cannot keep the restart spinning forever.
    auto build = std::make_shared<std::function<void(int)>>();
    auto compiled = std::make_shared<QStringList>();
    // Held weakly by its own body: the in-flight step callbacks own the strong
    // references, so the chain frees itself once the restart ends either way.
    std::weak_ptr<std::function<void(int)>> weakBuild = build;
    *build = [this, clientDir, buildDir, scratchDir, appPath, weakBuild,
              compiled, buildEnv, cacheStats](int pass) {
        constexpr int kMaxBuildPasses = 3;
        auto build = weakBuild.lock();
        if (!build)
            return;
        setRestartPhase(pass == 1 ? QStringLiteral("compiling")
                                  : QStringLiteral("compiling again (pass %1)")
                                        .arg(pass),
                        pass);
        setRestartSpinProgress(60);
        setUpdateStatus(pass == 1
                            ? QStringLiteral("Rebuilding...")
                            : QStringLiteral("Rebuilding again (pass %1 of %2): "
                                             "the sources changed during the "
                                             "build...")
                                  .arg(pass)
                                  .arg(kMaxBuildPasses));
        const int jobs = ramCappedBuildJobs();
        runOffThread<QString>(
            [clientDir] {
                QString head;
                gitOutput(clientDir, {"rev-parse", "--short", "HEAD"}, &head);
                return head;
            },
            [this, clientDir, buildDir, scratchDir, appPath, build, pass,
             jobs, compiled, buildEnv, cacheStats](QString headBefore) {
                logRestart(QStringLiteral("build pass %1: %2 parallel jobs, HEAD %3")
                               .arg(pass)
                               .arg(jobs)
                               .arg(headBefore.trimmed()));
                const QDateTime started = QDateTime::currentDateTime();
                const int logStart =
                    m_updateLog ? m_updateLog->document()->characterCount() : 0;
                auto settle = [this, clientDir, buildDir, appPath, build, pass,
                               headBefore, logStart, compiled,
                               cacheStats](bool ok, const QString &changed,
                                         const QString &headAfter) {
                    const QString output =
                        m_updateLog ? m_updateLog->toPlainText().mid(logStart)
                                    : QString();
                    for (const QString &source :
                         forkmesh::restarttiming::compiledSources(output))
                        if (!compiled->contains(source))
                            compiled->append(source);
                    if (!changed.isEmpty()) {
                        const QString note =
                            QStringLiteral("the sources changed while build pass %1 ran "
                                           "(%2; HEAD %3 -> %4)")
                                .arg(pass)
                                .arg(QDir(clientDir).relativeFilePath(changed),
                                     headBefore.trimmed(), headAfter.trimmed());
                        if (pass < kMaxBuildPasses) {
                            logRestart(note + QStringLiteral("; building again"));
                            appendUpdateLog(QStringLiteral("\n!! %1; building again so the "
                                                           "binary comes from one "
                                                           "consistent tree.\n")
                                                .arg(note));
                            (*build)(pass + 1);
                            return;
                        }
                        logRestart(note + (ok ? QStringLiteral("; out of passes, installing "
                                                               "this build as it is")
                                              : QStringLiteral("; out of passes")));
                    }
                    if (!ok) {
                        stopRefreshSpin();
                        stopRestartSpin();
                        // Out of passes on a tree that kept moving: the error is most
                        // likely the merge landing mid-pass (ninja reads build.ninja only
                        // at startup, so a source added to CMakeLists.txt then shows up
                        // as undefined symbols at link), not a real break on main.
                        const QString moved =
                            changed.isEmpty()
                                ? QString()
                                : QStringLiteral("the sources changed during all %1 build "
                                                 "passes (last: %2), so this is most "
                                                 "likely a merge landing mid-build "
                                                 "— rebuild again. ")
                                      .arg(kMaxBuildPasses)
                                      .arg(QDir(clientDir).relativeFilePath(changed));
                        setUpdateStatus("Update failed: " + moved +
                                            updateFailureSummary(output),
                                        true);
                        if (m_buildButton)
                            m_buildButton->setEnabled(true);
                        return;
                    }
                    QFile statsFile(cacheStats);
                    const QString stats =
                        statsFile.open(QIODevice::ReadOnly | QIODevice::Text)
                            ? QString::fromUtf8(statsFile.readAll())
                            : QString();
                    const QString summary =
                        forkmesh::restarttiming::withCcacheSummary(
                            forkmesh::restarttiming::rebuiltSummary(*compiled),
                            forkmesh::restarttiming::ccacheCounts(stats));
                    logRestart(summary);
                    appendUpdateLog(QStringLiteral("\n==> %1\n").arg(summary));
                    if (compiled->size() > 1)
                        appendUpdateLog(QStringLiteral("    %1\n").arg(
                            compiled->join(QStringLiteral("\n    "))));
                    logSystem(QStringLiteral("Rebuild & restart: %1").arg(summary));
                    QSettings().setValue(QStringLiteral("restartTiming/pendingSummary"),
                                         summary);
                    buildMirrorNodeCompanion(clientDir, buildDir,
                                             [this, buildDir, appPath] {
                        const QString built = builtExecutablePath(buildDir);
                        installAndRelaunch(built, appPath);
                    });
                };
                auto finished = [this, clientDir, started, settle](bool ok) {
                    runOffThread<QStringList>(
                        [clientDir, started] {
                            QStringList scan{sourceChangedSince(clientDir, started),
                                             QString()};
                            if (!scan.first().isEmpty())
                                gitOutput(clientDir, {"rev-parse", "--short", "HEAD"},
                                          &scan[1]);
                            return scan;
                        },
                        [settle, ok](QStringList scan) {
                            settle(ok, scan.value(0), scan.value(1));
                        });
                };
                runUpdateStepUser("cmake",
                                  {"--build", buildDir, "-j", QString::number(jobs)},
                                  buildDir, [finished] { finished(true); },
                                  [finished] { finished(false); }, buildEnv);
            });
    };
    auto configure = [this, clientDir, buildDir, buildType, configured,
                      staleEntries, build] {
        if (configured) {
            logRestart(QStringLiteral("build tree already configured; "
                                      "skipping the configure step"));
            (*build)(1);
            return;
        }
        auto run = [this, clientDir, buildDir, buildType, build] {
            runUpdateStepUser("cmake",
                              cmakeConfigureArgs(clientDir, buildDir, buildType),
                              clientDir, [build] { (*build)(1); });
        };
        if (staleEntries.isEmpty()) {
            run();
            return;
        }
        logRestart(QStringLiteral("switching the build tree's generator; "
                                  "dropping %1").arg(staleEntries.join(", ")));
        runUpdateStepUser("rm", QStringList{"-rf"} + staleEntries, buildDir,
                          run);
    };
    runUpdateStepUser("rm", {"-rf", scratchDir}, clientDir,
                      [this, clientDir, scratchDir, configure] {
        runUpdateStepUser("mkdir", {"-p", scratchDir}, clientDir, configure);
    });
}

void MainWindow::buildMirrorNodeCompanion(const QString &clientDir,
                                          const QString &buildDir,
                                          std::function<void()> onDone)
{
    const QString mirrorSource =
        QDir(clientDir).filePath(QStringLiteral("../server"));
    const QString go = QStandardPaths::findExecutable(QStringLiteral("go"));
    if (go.isEmpty() ||
        !QFileInfo::exists(
            QDir(mirrorSource).filePath(QStringLiteral("go.mod")))) {
        appendUpdateLog(QStringLiteral(
            "\nSkipping the Go mirror-node companion (Go toolchain or "
            "server sources not available); host deploys need "
            "forkmesh-mirror-node installed beside the client.\n"));
        onDone();
        return;
    }
    setRestartPhase(QStringLiteral("building the mirror-node companion"));
    setUpdateStatus("Building the Go mirror-node companion...");
    runUpdateStepUser(
        go,
        {QStringLiteral("build"), QStringLiteral("-trimpath"),
         QStringLiteral("-ldflags=-s -w"), QStringLiteral("-o"),
         QDir(buildDir).filePath(QStringLiteral("forkmesh-mirror-node")),
         QStringLiteral("./cmd/forkmesh-mirror-node")},
        mirrorSource, onDone, [this, onDone] {
            appendUpdateLog(QStringLiteral(
                "\nThe Go mirror-node companion failed to build; continuing "
                "the client update without refreshing it.\n"));
            onDone();
        });
}

static bool binaryPassesStartCheck(const QString &binary, const QString &asUser)
{
    QProcess probe;
    if (asUser.isEmpty()) {
        probe.start(binary, {QStringLiteral("--version")});
    } else {
        probe.start(QStringLiteral("sudo"),
                    {QStringLiteral("-u"), asUser, QStringLiteral("-H"), binary,
                     QStringLiteral("--version")});
    }
    if (!probe.waitForStarted(5000))
        return false;
    if (!probe.waitForFinished(15000)) {
        probe.kill();
        probe.waitForFinished(2000);
        return false;
    }
    return probe.exitStatus() == QProcess::NormalExit && probe.exitCode() == 0;
}

static QString builtMirrorNodeCompanionPath(const QString &built)
{
    QDir dir = QFileInfo(built).dir();
    QString candidate = dir.filePath(QStringLiteral("forkmesh-mirror-node"));
    if (QFileInfo(candidate).isExecutable())
        return candidate;
#ifdef Q_OS_MACOS
    if (dir.cdUp() && dir.cdUp() && dir.cdUp()) {
        candidate = dir.filePath(QStringLiteral("forkmesh-mirror-node"));
        if (QFileInfo(candidate).isExecutable())
            return candidate;
    }
#endif
    return QString();
}

static bool installMirrorNodeCompanionFile(const QString &source,
                                           const QString &binDir)
{
    const QString target =
        QDir(binDir).filePath(QStringLiteral("forkmesh-mirror-node"));
    if (!QFileInfo(source).canonicalFilePath().isEmpty() &&
        QFileInfo(source).canonicalFilePath() ==
            QFileInfo(target).canonicalFilePath())
        return true;
    const QString bak = target + QStringLiteral(".bak-update");
    QFile::remove(bak);
    if (QFileInfo::exists(target) && !QFile::rename(target, bak))
        return false;
    if (!QFile::copy(source, target)) {
        QFile::rename(bak, target);
        return false;
    }
    QFile::setPermissions(target, QFile::ReadOwner | QFile::WriteOwner |
                                      QFile::ExeOwner | QFile::ReadGroup |
                                      QFile::ExeGroup | QFile::ReadOther |
                                      QFile::ExeOther);
    return true;
}

void MainWindow::installAndRelaunch(const QString &built, const QString &appPath)
{
    const QStringList relaunchArgs = QCoreApplication::arguments().mid(1);
    setRestartPhase(QStringLiteral("installing"));
    setRestartSpinProgress(80);

    if (!m_updateAsUser.isEmpty()) {
        const QString binDir = QFileInfo(appPath).absolutePath();
        QString script =
            QStringLiteral("mkdir -p %1 && { [ ! -e %3 ] || cp -f %3 %3.bak-update; }"
                           " && cp -f %2 %3 && chmod 0755 %3")
                .arg(shellSingleQuote(binDir), shellSingleQuote(built),
                     shellSingleQuote(appPath));
        const QString companion = builtMirrorNodeCompanionPath(built);
        if (!companion.isEmpty()) {
            const QString companionTarget = QDir(binDir).filePath(
                QStringLiteral("forkmesh-mirror-node"));
            script += QStringLiteral(
                          " && { { [ ! -e %2 ] || mv -f %2 %2.bak-update; } && "
                          "cp -f %1 %2 && chmod 0755 %2 || true; }")
                          .arg(shellSingleQuote(companion),
                               shellSingleQuote(companionTarget));
        }
        const QString builtModules =
            QFileInfo(built).absolutePath() + QStringLiteral("/packages");
        if (QFileInfo(builtModules).isDir()) {
            const QString moduleTarget =
                forkmesh::market::modules::installedDirectoryFor(appPath);
            script += QStringLiteral(
                          " && { mkdir -p %2 && for f in %1/*.json %1/*.so; do "
                          "[ ! -f \"$f\" ] || cp -f \"$f\" %2/; done || true; }")
                          .arg(shellSingleQuote(builtModules), shellSingleQuote(moduleTarget));
        }
        setUpdateStatus("Installing for " + m_updateAsUser + "...");
        const QString probeUser = m_updateAsUser;
        if (!binaryPassesStartCheck(built, probeUser)) {
            setUpdateStatus("Update failed: the new build did not pass its "
                            "start check; keeping the current version.",
                            true);
            stopRestartSpin();
            if (m_buildButton)
                m_buildButton->setEnabled(true);
            return;
        }
        runUpdateStep("sudo", {"-u", m_updateAsUser, "-H", "sh", "-c", script},
                      QDir::tempPath(), [this, appPath, relaunchArgs] {
            if (m_closingDown)
                return;
            setRestartSpinProgress(100);
            setUpdateStatus("Relaunching...");
            const QString user = m_updateAsUser;
            forkmesh::releaseSingleInstance();
            QStringList args{QStringLiteral("-u"), user, QStringLiteral("-H"),
                             appPath};
            args += relaunchArgs;
            if (forkmesh::processreaper::startReleased("sudo", args))
                recordRestartDuration();
            logRestart(QStringLiteral("relaunched %1; quitting").arg(appPath));
            logSystem(QStringLiteral("=== Restarting now (rebuild & restart) ==="));
            QCoreApplication::quit();
        });
        return;
    }

    if (!binaryPassesStartCheck(built, QString())) {
        setUpdateStatus("Update failed: the new build did not pass its start "
                        "check; keeping the current version.",
                        true);
        stopRestartSpin();
        if (m_buildButton)
            m_buildButton->setEnabled(true);
        return;
    }
    if (QFileInfo(built).canonicalFilePath() !=
        QFileInfo(appPath).canonicalFilePath()) {
        const QString bak = appPath + QStringLiteral(".bak-update");
        QFile::remove(bak);
        const bool hadInstalledCopy = QFileInfo::exists(appPath);
        if (!QDir().mkpath(QFileInfo(appPath).absolutePath())) {
            setUpdateStatus("Update failed: could not create " +
                                QFileInfo(appPath).absolutePath(),
                            true);
            stopRestartSpin();
            if (m_buildButton)
                m_buildButton->setEnabled(true);
            return;
        }
        if (hadInstalledCopy && !QFile::rename(appPath, bak)) {
            setUpdateStatus("Update failed: could not move the current binary "
                            "aside at " + appPath, true);
            stopRestartSpin();
            if (m_buildButton)
                m_buildButton->setEnabled(true);
            return;
        }
        if (!QFile::copy(built, appPath)) {
            if (hadInstalledCopy)
                QFile::rename(bak, appPath); // never leave an old install empty
            setUpdateStatus("Update failed: could not replace " + appPath, true);
            stopRestartSpin();
            if (m_buildButton)
                m_buildButton->setEnabled(true);
            return;
        }
        QFile::setPermissions(appPath,
                              QFile::ReadOwner | QFile::WriteOwner |
                              QFile::ExeOwner | QFile::ReadGroup |
                              QFile::ExeGroup | QFile::ReadOther |
                              QFile::ExeOther);
    }
    const QString companion = builtMirrorNodeCompanionPath(built);
    if (!companion.isEmpty()) {
        if (installMirrorNodeCompanionFile(companion,
                                           QFileInfo(appPath).absolutePath()))
            logRestart(QStringLiteral(
                "installed the Go mirror-node companion beside %1").arg(appPath));
        else
            logRestart(QStringLiteral(
                "could not install the Go mirror-node companion beside %1")
                           .arg(appPath));
    }
    if (QFileInfo(built).canonicalFilePath() != QFileInfo(appPath).canonicalFilePath()) {
        QString moduleError;
        const int modules =
            forkmesh::market::modules::installBeside(built, appPath, &moduleError);
        if (!moduleError.isEmpty())
            logRestart(QStringLiteral("package modules: %1").arg(moduleError));
        else if (modules > 0)
            logRestart(QStringLiteral("installed %1 package module files in %2")
                           .arg(modules)
                           .arg(forkmesh::market::modules::installedDirectoryFor(appPath)));
    }
    if (m_closingDown)
        return;
    setRestartSpinProgress(100);
    setUpdateStatus("Relaunching...");
    if (requestSupervisingUserServiceRestart(appPath)) {
        recordRestartDuration();
        logRestart(QStringLiteral("%1 runs under a systemd user service; "
                                  "restarting the unit instead of starting a "
                                  "child it would stop")
                       .arg(appPath));
        logSystem(QStringLiteral("=== Restarting now (rebuild & restart) ==="));
        QCoreApplication::quit();
        return;
    }
    forkmesh::releaseSingleInstance();
    if (!forkmesh::processreaper::startReleased(appPath, relaunchArgs)) {
        setUpdateStatus("Update installed but the relaunch failed; still "
                        "running the previous version in memory.",
                        true);
        stopRestartSpin();
        if (m_buildButton)
            m_buildButton->setEnabled(true);
        return;
    }
    recordRestartDuration();
    logRestart(QStringLiteral("relaunched %1; quitting").arg(appPath));
    logSystem(QStringLiteral("=== Restarting now (rebuild & restart) ==="));
    QCoreApplication::quit();
}

QString MainWindow::resolveInstallCloneUrl()
{
    if (!m_networkAccess)
        return QString();
    QUrl url = catalogApiUrl(); // http(s) on the mainnode host
    url.setPath(QStringLiteral("/api/install-source"));
    url.setQuery(QString());
    QNetworkReply *reply = m_networkAccess->get(QNetworkRequest(url));
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
    reply->deleteLater();
    const QString node = obj.value("node").toString().trimmed();
    const QString repo = obj.value("repo").toString(QStringLiteral("forkmesh")).trimmed();
    if (node.isEmpty() || repo.isEmpty())
        return QString();
    QUrl clone = catalogApiUrl();
    clone.setPath(QStringLiteral("/forkmesh/") + repo);
    clone.setQuery(QString());
    clone.setFragment(QString());
    return clone.toString();
}


static bool fileSha256Matches(const QString &path, const QString &expected)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    QCryptographicHash hasher(QCryptographicHash::Sha256);
    if (!hasher.addData(&file))
        return false;
    return QString::fromLatin1(hasher.result().toHex()) ==
           expected.toLower();
}

bool MainWindow::tryPrebuiltAutoUpdate(const QString &clientDir,
                                       const QString &tag,
                                       const QString &tagCommit)
{
    if (!invokingNonRootUser().isEmpty())
        return false;

#if defined(Q_OS_LINUX)
    const QString wantOs = QStringLiteral("linux");
#elif defined(Q_OS_MACOS)
    const QString wantOs = QStringLiteral("macos");
#elif defined(Q_OS_WIN)
    const QString wantOs = QStringLiteral("windows");
#else
    return false;
#endif
    const QString wantArch = QSysInfo::currentCpuArchitecture();

    // The Actions release job tags the version-bump commit and lands the signed
    // metadata in a later commit, so `latest` at the tag commit can still
    // describe the PREVIOUS release; installing its binary "updated" a node to
    // the version it already ran, again on every check. Accept only a manifest
    // for this exact tag whose tag commit the tag contains: the tag commit's own
    // tree first (a tag moved onto its metadata commit), then the tracked
    // upstream branch, where promoted metadata lands.
    const QString wantedTagCommit = tagCommit.trimmed().toLower();
    bool publishesManifests = false;
    QString rejectedBecause;
    QString verifiedTagCommit;
    auto manifestForTag = [&](const QString &revision) -> QJsonObject {
        QByteArray out;
        if (!runGitCapture(clientDir,
                           {QStringLiteral("show"),
                            revision + QStringLiteral(
                                           ":.forkmesh/releases/latest/release.json")},
                           &out, nullptr))
            return {};
        publishesManifests = true;
        const QJsonObject candidate = QJsonDocument::fromJson(out).object();
        if (candidate.value(QStringLiteral("tag")).toString().trimmed() != tag)
            return {};
        const QString releasedCommit =
            candidate.value(QStringLiteral("tag_commit"))
                .toString().trimmed().toLower();
        if (releasedCommit.isEmpty())
            return {};
        if (releasedCommit != wantedTagCommit &&
            !runGitCapture(clientDir,
                           {QStringLiteral("merge-base"),
                            QStringLiteral("--is-ancestor"), releasedCommit,
                            wantedTagCommit},
                           nullptr, nullptr))
            return {};
        QByteArray signature;
        runGitCapture(clientDir,
                      {QStringLiteral("show"),
                       revision + QStringLiteral(
                                      ":.forkmesh/releases/latest/release.json.sig")},
                      &signature, nullptr);
        const forkmesh::releasetrust::Verdict verdict =
            forkmesh::releasetrust::checkManifest(
                out, signature, tag, QStringLiteral(FORKMESH_VERSION));
        if (!verdict.ok) {
            rejectedBecause = verdict.reason;
            return {};
        }
        verifiedTagCommit = verdict.tagCommit;
        return verdict.manifest;
    };
    QJsonObject manifest = manifestForTag(wantedTagCommit);
    if (manifest.isEmpty())
        manifest = manifestForTag(QStringLiteral("@{upstream}"));
    if (manifest.isEmpty() && !rejectedBecause.isEmpty()) {
        static QString rejectedLoggedFor;
        if (rejectedLoggedFor != tag + rejectedBecause) {
            rejectedLoggedFor = tag + rejectedBecause;
            logSystem(QStringLiteral("Auto-update: release %1 was not "
                                     "installed because %2. Nothing was "
                                     "changed.")
                          .arg(tag, rejectedBecause));
        }
        return true;
    }
    if (manifest.isEmpty()) {
        if (publishesManifests) {
            // This repository ships signed prebuilts; the release's own set is
            // still being completed (for example its Android APK). Wait for it
            // rather than compiling on a machine that installed a prebuilt.
            static QString waitingLoggedFor;
            if (waitingLoggedFor != tag) {
                waitingLoggedFor = tag;
                logSystem(QStringLiteral("Auto-update: release %1 is tagged but "
                                         "its signed artifacts are not "
                                         "published yet; waiting.")
                              .arg(tag));
            }
            return true;
        }
        static QString unsignedLoggedFor;
        if (unsignedLoggedFor != tag) {
            unsignedLoggedFor = tag;
            logSystem(QStringLiteral("Auto-update: release %1 publishes no "
                                     "signed release manifest, so it is not "
                                     "installed automatically. Use Update to "
                                     "build it by hand.")
                          .arg(tag));
        }
        return true;
    }
    static const QRegularExpression sha256Re(
        QStringLiteral("\\A[0-9a-f]{64}\\z"));
    QString assetHash;
    QString assetName;
    QString companionHash;
    const QJsonArray assets =
        manifest.value(QStringLiteral("assets")).toArray();
    for (const QJsonValue &value : assets) {
        const QJsonObject asset = value.toObject();
        if (asset.value(QStringLiteral("os")).toString().trimmed() != wantOs)
            continue;
        if (asset.value(QStringLiteral("arch")).toString().trimmed() !=
            wantArch)
            continue;
        const QString hash = asset.value(QStringLiteral("blob_sha256"))
                                 .toString()
                                 .trimmed()
                                 .toLower();
        if (!sha256Re.match(hash).hasMatch())
            continue;
        const QString name = asset.value(QStringLiteral("name")).toString();
        if (name.startsWith(QStringLiteral("forkmesh-mirror-node"))) {
            if (companionHash.isEmpty())
                companionHash = hash;
            continue;
        }
        if (assetHash.isEmpty()) {
            assetHash = hash;
            assetName = name;
        }
    }
    const QString manifestRepo =
        manifest.value(QStringLiteral("repo")).toString().trimmed();
    const int slash = manifestRepo.indexOf(QLatin1Char('/'));
    if (assetHash.isEmpty() || slash <= 0) {
        logSystem(QStringLiteral("Auto-update: release %1 has no prebuilt "
                                 "artifact for %2/%3; building its signed "
                                 "commit from source.")
                      .arg(tag, wantOs, wantArch));
        autoUpdateFromVerifiedSource(tag, verifiedTagCommit);
        return true;
    }
    const QString owner = manifestRepo.left(slash);
    const QString name = manifestRepo.mid(slash + 1);

    for (const RepositoryRecord &repo : std::as_const(m_repositories)) {
        if (repo.previewOnly || repo.mirrorPath.trimmed().isEmpty())
            continue;
        const QString identity =
            repoSegment(repo.owner, QStringLiteral("owner")) +
            QLatin1Char('/') +
            repoSegment(repo.name, QStringLiteral("repository"));
        if (identity.compare(manifestRepo, Qt::CaseInsensitive) != 0)
            continue;
        const QString blob = mirrorReleaseBlobPath(repo.mirrorPath, assetHash);
        if (QFile::exists(blob) && fileSha256Matches(blob, assetHash)) {
            logSystem(QStringLiteral("Auto-update: installing release %1 from "
                                     "this node's own artifact store (%2).")
                          .arg(tag, assetName));
            installMirrorCompanionThenRelaunch(blob, tag, owner, name,
                                               companionHash);
            return true;
        }
    }

    if (!m_networkAccess)
        return false;
    const QString staging =
        QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
            .filePath(QStringLiteral("forkmesh-update-") + assetHash.left(12));
    auto tmp = std::make_shared<QFile>(staging + QStringLiteral(".part"));
    if (!tmp->open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    auto hasher =
        std::make_shared<QCryptographicHash>(QCryptographicHash::Sha256);
    QUrl url = catalogApiUrl();
    url.setPath(QStringLiteral("/api/repo/%1/%2/releases/blob/sha256/%3")
                    .arg(QString::fromUtf8(QUrl::toPercentEncoding(owner)),
                         QString::fromUtf8(QUrl::toPercentEncoding(name)),
                         assetHash));
    logSystem(QStringLiteral(
                  "Auto-update: downloading the release %1 artifact (%2)...")
                  .arg(tag, assetName));
    QNetworkReply *reply = m_networkAccess->get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::readyRead, this, [reply, tmp, hasher] {
        const QByteArray chunk = reply->readAll();
        tmp->write(chunk);
        hasher->addData(chunk);
    });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, tmp, hasher, staging, assetHash, tag, owner, name,
             companionHash, verifiedTagCommit] {
                const QByteArray rest = reply->readAll();
                tmp->write(rest);
                hasher->addData(rest);
                const bool ok = reply->error() == QNetworkReply::NoError;
                const QString netError = reply->errorString();
                reply->deleteLater();
                tmp->close();
                const QString actual =
                    QString::fromLatin1(hasher->result().toHex());
                if (!ok || actual != assetHash) {
                    QFile::remove(tmp->fileName());
                    logSystem(
                        QStringLiteral(
                            "Auto-update: artifact download for %1 failed "
                            "(%2); building its signed commit from source.")
                            .arg(tag, ok ? QStringLiteral("checksum mismatch")
                                         : netError));
                    autoUpdateFromVerifiedSource(tag, verifiedTagCommit);
                    return;
                }
                QFile::remove(staging);
                if (!QFile::rename(tmp->fileName(), staging)) {
                    QFile::remove(tmp->fileName());
                    autoUpdateFromVerifiedSource(tag, verifiedTagCommit);
                    return;
                }
                installMirrorCompanionThenRelaunch(staging, tag, owner, name,
                                                   companionHash);
            });
    return true;
}

void MainWindow::installMirrorCompanionThenRelaunch(const QString &artifactPath,
                                                    const QString &tag,
                                                    const QString &owner,
                                                    const QString &name,
                                                    const QString &companionHash)
{
    const QString binDir =
        QFileInfo(runningClientExecutable()).absolutePath();
    const QString installed =
        QDir(binDir).filePath(QStringLiteral("forkmesh-mirror-node"));
    if (companionHash.isEmpty()) {
        logSystem(QStringLiteral(
                      "Auto-update: release %1 publishes no Go mirror-node "
                      "companion; keeping the current one.")
                      .arg(tag));
        installPrebuiltAndRelaunch(artifactPath, tag);
        return;
    }
    if (fileSha256Matches(installed, companionHash)) {
        installPrebuiltAndRelaunch(artifactPath, tag);
        return;
    }
    for (const RepositoryRecord &repo : std::as_const(m_repositories)) {
        if (repo.previewOnly || repo.mirrorPath.trimmed().isEmpty())
            continue;
        const QString blob =
            mirrorReleaseBlobPath(repo.mirrorPath, companionHash);
        if (QFile::exists(blob) && fileSha256Matches(blob, companionHash)) {
            if (installMirrorNodeCompanionFile(blob, binDir))
                logSystem(QStringLiteral(
                              "Auto-update: installed the release %1 Go "
                              "mirror-node companion from this node's own "
                              "artifact store.")
                              .arg(tag));
            else
                logSystem(QStringLiteral(
                              "Auto-update: could not refresh the Go "
                              "mirror-node companion for %1; continuing the "
                              "client update.")
                              .arg(tag));
            installPrebuiltAndRelaunch(artifactPath, tag);
            return;
        }
    }
    if (!m_networkAccess) {
        installPrebuiltAndRelaunch(artifactPath, tag);
        return;
    }
    auto tmp = std::make_shared<QFile>(
        QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
            .filePath(QStringLiteral("forkmesh-mirror-node-update-") +
                      companionHash.left(12) + QStringLiteral(".part")));
    if (!tmp->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        installPrebuiltAndRelaunch(artifactPath, tag);
        return;
    }
    auto hasher =
        std::make_shared<QCryptographicHash>(QCryptographicHash::Sha256);
    QUrl url = catalogApiUrl();
    url.setPath(QStringLiteral("/api/repo/%1/%2/releases/blob/sha256/%3")
                    .arg(QString::fromUtf8(QUrl::toPercentEncoding(owner)),
                         QString::fromUtf8(QUrl::toPercentEncoding(name)),
                         companionHash));
    logSystem(QStringLiteral("Auto-update: downloading the release %1 Go "
                             "mirror-node companion...")
                  .arg(tag));
    QNetworkReply *reply = m_networkAccess->get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::readyRead, this, [reply, tmp, hasher] {
        const QByteArray chunk = reply->readAll();
        tmp->write(chunk);
        hasher->addData(chunk);
    });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, tmp, hasher, artifactPath, tag, companionHash,
             binDir] {
                const QByteArray rest = reply->readAll();
                tmp->write(rest);
                hasher->addData(rest);
                const bool ok = reply->error() == QNetworkReply::NoError;
                reply->deleteLater();
                tmp->close();
                const QString actual =
                    QString::fromLatin1(hasher->result().toHex());
                if (ok && actual == companionHash &&
                    installMirrorNodeCompanionFile(tmp->fileName(), binDir))
                    logSystem(QStringLiteral(
                                  "Auto-update: installed the release %1 Go "
                                  "mirror-node companion.")
                                  .arg(tag));
                else
                    logSystem(QStringLiteral(
                                  "Auto-update: could not refresh the Go "
                                  "mirror-node companion for %1; continuing "
                                  "the client update.")
                                  .arg(tag));
                QFile::remove(tmp->fileName());
                installPrebuiltAndRelaunch(artifactPath, tag);
            });
}

void MainWindow::installPrebuiltAndRelaunch(const QString &artifactPath,
                                            const QString &tag)
{
    startRestartCautionFlash();
    const QString staged =
        QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
            .filePath(QStringLiteral("forkmesh-update-staged"));
    if (staged != artifactPath) {
        QFile::remove(staged);
        if (!QFile::copy(artifactPath, staged)) {
            logSystem(QStringLiteral("Auto-update: could not stage the %1 "
                                     "artifact; nothing was changed and the "
                                     "next check tries again.")
                          .arg(tag));
            stopRestartCautionFlash();
            return;
        }
    }
    QFile::setPermissions(staged, QFile::ReadOwner | QFile::WriteOwner |
                                      QFile::ExeOwner | QFile::ReadGroup |
                                      QFile::ExeGroup | QFile::ReadOther |
                                      QFile::ExeOther);
    logRestart(QStringLiteral("prebuilt update to %1 staged").arg(tag));
    m_updateAsUser.clear();
    installAndRelaunch(staged, runningClientExecutable());
}

void MainWindow::autoUpdateFromVerifiedSource(const QString &tag,
                                              const QString &verifiedCommit)
{
    const QString clientDir = runningClientDir();
    const QString repoDir = QFileInfo(clientDir).absolutePath();
    if (!forkmesh::releasetrust::isCommitId(verifiedCommit)
        || !QDir(repoDir).exists(QStringLiteral(".git"))
        || !QDir(clientDir).exists(QStringLiteral("CMakeLists.txt"))) {
        logSystem(QStringLiteral("Auto-update: release %1 cannot be built "
                                 "from its signed commit here; nothing was "
                                 "changed. Use Update to update by hand.")
                      .arg(tag));
        return;
    }
    startRestartCautionFlash();
    beginRestartLog();
    showUpdateLog();
    logRestart(QStringLiteral("auto-update to %1 from signed commit %2 started")
                   .arg(tag, verifiedCommit));
    m_buildButton = m_rebuildButton;
    m_buildStatusLabel = m_rebuildStatus;
    m_updateAsUser.clear();
    if (m_rebuildButton)
        m_rebuildButton->setEnabled(false);
    setRestartPhase(QStringLiteral("moving to the signed release commit"));
    setRestartSpinProgress(20);
    setUpdateStatus(QStringLiteral("Moving the checkout to release %1 (%2)...")
                        .arg(tag, verifiedCommit.left(12)));
    runUpdateStepUser(
        QStringLiteral("git"),
        {QStringLiteral("-C"), repoDir, QStringLiteral("merge"),
         QStringLiteral("--ff-only"), verifiedCommit},
        repoDir,
        [this, clientDir, repoDir, tag, verifiedCommit] {
            QString head;
            if (!gitOutput(repoDir, {QStringLiteral("rev-parse"),
                                     QStringLiteral("HEAD")},
                           &head)
                || head.trimmed().toLower() != verifiedCommit) {
                logRestart(QStringLiteral("auto-update stopped: HEAD is not "
                                          "the signed commit of %1")
                               .arg(tag));
                setUpdateStatus(QStringLiteral("Auto-update stopped: the "
                                               "checkout is not at the signed "
                                               "commit of %1.")
                                    .arg(tag),
                                true);
                stopRestartSpin();
                stopRestartCautionFlash();
                if (m_rebuildButton)
                    m_rebuildButton->setEnabled(true);
                return;
            }
            buildAndRelaunch(clientDir);
        },
        [this, repoDir, tag] {
            logRestart(QStringLiteral("auto-update stopped; %1 left untouched")
                           .arg(repoDir));
            setUpdateStatus(QStringLiteral("Auto-update to %1 stopped: the "
                                           "checkout cannot fast-forward to "
                                           "its signed commit. Nothing in %2 "
                                           "was changed.")
                                .arg(tag, repoDir),
                            true);
            stopRestartSpin();
            stopRestartCautionFlash();
            if (m_rebuildButton)
                m_rebuildButton->setEnabled(true);
        });
}

void MainWindow::updateRebuildRestart()
{
    startRestartCautionFlash();
    beginRestartLog();
    showUpdateLog();
    logRestart(QStringLiteral("update, rebuild & restart started"));
    logSystem(QStringLiteral("=== Update, rebuild & restart started ==="));
    m_buildButton = m_rebuildButton;
    m_buildStatusLabel = m_rebuildStatus;
    setRestartPhase(QStringLiteral("finding a mirror"));
    setRestartSpinProgress(5);

    const QString user = invokingNonRootUser();
    const QString home = user.isEmpty() ? QString() : homeForUser(user);
    const QString clientDir = user.isEmpty()
                                  ? runningClientDir()
                                  : runningClientDirUnderHome(home);
    const QString relaunchPath =
        runningClientExecutableUnderHome(user.isEmpty() ? QDir::homePath() : home);
    m_updateAsUser = user;

    if (m_rebuildButton)
        m_rebuildButton->setEnabled(false);
    flashMessage(QStringLiteral("Updating ForkMesh from the install mirror..."));
    setUpdateStatus(user.isEmpty()
                        ? QStringLiteral("Finding an online ForkMesh mirror...")
                        : QStringLiteral("Finding an online ForkMesh mirror "
                                         "(installing for %1)...").arg(user));

    const QString installUrl = resolveInstallCloneUrl();
    if (installUrl.isEmpty()) {
        m_updateAsUser.clear();
        setUpdateStatus("No online ForkMesh mirror is available right now. "
                        "Try again shortly.",
                        true);
        flashMessage("No online ForkMesh mirror is available right now.", true);
        stopRestartSpin();
        if (m_rebuildButton)
            m_rebuildButton->setEnabled(true);
        return;
    }

    const QString repoDir = QFileInfo(clientDir).absolutePath(); // .../src
    const bool haveCheckout = QDir(repoDir).exists(QStringLiteral(".git"));

    auto stopUntouched = [this, repoDir](const QString &reason) {
        m_updateAsUser.clear();
        logRestart(QStringLiteral("update stopped; %1 left untouched: %2")
                       .arg(repoDir, reason));
        setUpdateStatus(reason + QStringLiteral(" Nothing in ") + repoDir +
                            QStringLiteral(" was changed or removed."),
                        true);
        flashMessage(reason, true);
        stopRestartSpin();
        if (m_rebuildButton)
            m_rebuildButton->setEnabled(true);
    };

    if (haveCheckout) {
        setRestartPhase(QStringLiteral("pulling the source"));
        setRestartSpinProgress(20);
        setUpdateStatus("Pulling a fresh copy from " + installUrl + "...");
        runUpdateStepUser("git", {"-C", repoDir, "remote", "set-url", "origin",
                                  installUrl},
                          repoDir, [this, repoDir, clientDir, user, relaunchPath,
                                    installUrl, stopUntouched] {
            runUpdateStepUser("git", {"-C", repoDir, "pull", "--ff-only"}, repoDir,
                              [this, clientDir, user, relaunchPath] {
                buildAndRelaunch(clientDir, user, relaunchPath);
            }, [installUrl, stopUntouched] {
                stopUntouched(QStringLiteral("Could not fast-forward from ") +
                              installUrl +
                              QStringLiteral(" (the mirror may be busy, or this "
                                             "checkout has its own commits)."));
            });
        }, [stopUntouched] {
            stopUntouched(QStringLiteral("Could not point the checkout at the "
                                         "install mirror."));
        });
        return;
    }

    const QDir existing(repoDir);
    if (existing.exists() &&
        !existing.entryList(QDir::AllEntries | QDir::Hidden | QDir::System |
                            QDir::NoDotAndDotDot)
             .isEmpty()) {
        stopUntouched(QStringLiteral("The source folder is not empty but has no "
                                     "git checkout."));
        return;
    }
    setRestartPhase(QStringLiteral("downloading the source"));
    setRestartSpinProgress(20);
    setUpdateStatus("Downloading a fresh copy from " + installUrl + "...");
    const QString parent = QFileInfo(repoDir).absolutePath();
    runUpdateStepUser("mkdir", {"-p", parent}, QDir::tempPath(),
                      [this, installUrl, repoDir, parent, clientDir, user,
                       relaunchPath] {
        runUpdateStepUser("git", {"clone", "--depth", "1", installUrl, repoDir},
                          parent, [this, clientDir, user, relaunchPath] {
            buildAndRelaunch(clientDir, user, relaunchPath);
        });
    });
}
