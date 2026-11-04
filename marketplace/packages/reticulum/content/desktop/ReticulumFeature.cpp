#include "ReticulumFeature.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QFrame>
#include <QScrollBar>
#include <QPlainTextEdit>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QShowEvent>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyle>
#include <QTabWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>

namespace forkmesh::reticulum {

namespace {

const QString kInterfacesSetting = QStringLiteral("interfaces");
const QString kIdentitySetting = QStringLiteral("identity");
const QString kAnnounceNameSetting = QStringLiteral("announceName");
const QString kAnnounceIntervalSetting = QStringLiteral("announceMinutes");
const QString kAnswerPathsSetting = QStringLiteral("answerPathRequests");
const QString kRunningSetting = QStringLiteral("networkOn");
const QString kMessagesSetting = QStringLiteral("messages");

enum TileIndex { StatusTile, DestinationsTile, AnnouncesTile, TrafficTile, MessagesTile, PathsTile };
constexpr int kHashRole = Qt::UserRole + 1;
constexpr int kSortRole = Qt::UserRole + 2;

class SortableItem : public QTreeWidgetItem
{
public:
    using QTreeWidgetItem::QTreeWidgetItem;
    bool operator<(const QTreeWidgetItem &other) const override
    {
        const int column = treeWidget() ? treeWidget()->sortColumn() : 0;
        const QVariant left = data(column, kSortRole);
        const QVariant right = other.data(column, kSortRole);
        if (left.isValid() && right.isValid())
            return left.toDouble() < right.toDouble();
        return text(column).compare(other.text(column), Qt::CaseInsensitive) < 0;
    }
};

QLabel *mutedLabel(const QString &text = {})
{
    auto *label = new QLabel(text);
    label->setObjectName(QStringLiteral("mutedLabel"));
    label->setWordWrap(true);
    return label;
}

QLabel *sectionTitle(const QString &text)
{
    auto *label = new QLabel(text);
    label->setObjectName(QStringLiteral("sectionTitle"));
    return label;
}

QPushButton *makeButton(const QString &text, const QString &style = QStringLiteral("ghostButton"))
{
    auto *button = new QPushButton(text);
    button->setObjectName(style);
    button->setCursor(Qt::PointingHandCursor);
    return button;
}

QTreeWidget *makeTree(const QString &name, const QStringList &headers, const QString &accessible)
{
    auto *tree = new QTreeWidget;
    tree->setObjectName(name);
    tree->setAccessibleName(accessible);
    tree->setColumnCount(headers.size());
    tree->setHeaderLabels(headers);
    tree->setRootIsDecorated(false);
    tree->setAlternatingRowColors(true);
    tree->setUniformRowHeights(true);
    tree->setSelectionMode(QAbstractItemView::SingleSelection);
    tree->header()->setStretchLastSection(true);
    for (int column = 0; column + 1 < headers.size(); ++column)
        tree->header()->setSectionResizeMode(column, QHeaderView::Interactive);
    return tree;
}

QString hopsText(int hops)
{
    return hops == 1 ? QStringLiteral("1 hop") : QStringLiteral("%1 hops").arg(hops);
}

QString stateTone(LinkState state)
{
    switch (state) {
    case LinkState::Online:
        return QStringLiteral("●");
    case LinkState::Connecting:
    case LinkState::Waiting:
        return QStringLiteral("◐");
    case LinkState::Blocked:
    case LinkState::Failed:
        return QStringLiteral("✕");
    case LinkState::Disabled:
        break;
    }
    return QStringLiteral("○");
}

QString shortHash(const QByteArray &hash)
{
    const QString hex = hexHash(hash);
    return hex.size() > 12 ? QStringLiteral("<%1…%2>").arg(hex.left(6), hex.right(4)) : prettyHash(hash);
}

}

ReticulumFeature::ReticulumFeature(const forkmesh::market::NativePackageHost &host, QWidget *parent)
    : forkmesh::market::NativePackageWidget(parent)
    , m_host(host)
    , m_node(new Node(this))
    , m_messenger(new Messenger(m_node, this))
    , m_viewTimer(new QTimer(this))
    , m_announceTimer(new QTimer(this))
{
    setObjectName(QStringLiteral("reticulumPage"));
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(20, 18, 20, 20);
    outer->setSpacing(10);

    auto *headingRow = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("Reticulum"));
    title->setObjectName(QStringLiteral("pageTitle"));
    headingRow->addWidget(title);
    m_summary = mutedLabel();
    m_summary->setWordWrap(false);
    m_summary->setTextFormat(Qt::PlainText);
    headingRow->addWidget(m_summary);
    headingRow->addStretch();
    m_announceButton = makeButton(QStringLiteral("Announce"));
    m_announceButton->setToolTip(QStringLiteral("Tell the network this desktop is here"));
    m_powerButton = makeButton(QStringLiteral("Disconnect"));
    headingRow->addWidget(m_announceButton);
    headingRow->addWidget(m_powerButton);
    outer->addLayout(headingRow);

    m_statusLine = mutedLabel();
    m_statusLine->setObjectName(QStringLiteral("statusLine"));
    m_statusLine->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_statusLine->setTextFormat(Qt::PlainText);
    m_statusLine->hide();
    outer->addWidget(m_statusLine);

    m_onboarding = new QWidget;
    m_onboarding->setObjectName(QStringLiteral("inlineNotice"));
    auto *onboardingRow = new QHBoxLayout(m_onboarding);
    onboardingRow->setContentsMargins(12, 8, 12, 8);
    auto *onboardingText =
        mutedLabel(QStringLiteral("Reticulum is an encrypted mesh network that runs over the internet, LoRa radios, "
                                  "packet radio and local networks. Join a public entry point to see it, or connect to "
                                  "the Reticulum daemon (rnsd) on this computer."));
    onboardingRow->addWidget(onboardingText, 1);
    auto *joinButton = makeButton(QStringLiteral("Join the public network"), QStringLiteral("primaryButton"));
    auto *importButton = makeButton(QStringLiteral("Import my Reticulum config"));
    onboardingRow->addWidget(importButton);
    onboardingRow->addWidget(joinButton);
    outer->addWidget(m_onboarding);
    connect(joinButton, &QPushButton::clicked, this, [this] { joinPublicNetwork(); });
    connect(importButton, &QPushButton::clicked, this, [this] {
        const QString path = reticulumConfigPath();
        if (path.isEmpty()) {
            showStatus(QStringLiteral("No Reticulum config was found in /etc/reticulum, ~/.config/reticulum or ~/.reticulum."),
                       true);
            return;
        }
        showStatus(importReticulumConfig(path));
    });

    auto *tiles = new QHBoxLayout;
    tiles->setSpacing(10);
    for (const QString &label : {QStringLiteral("Connection"), QStringLiteral("Destinations"),
                                 QStringLiteral("Announces"), QStringLiteral("Traffic"), QStringLiteral("Messages"),
                                 QStringLiteral("Path lookups")}) {
        auto *tile = new StatTile(label);
        m_tiles.append(tile);
        tiles->addWidget(tile);
    }
    outer->addLayout(tiles);

    m_tabs = new QTabWidget;
    m_tabs->setObjectName(QStringLiteral("reticulumTabs"));
    m_tabs->setDocumentMode(true);
    m_tabs->addTab(buildNetworkTab(), QStringLiteral("Network"));
    m_tabs->addTab(buildMessagesTab(), QStringLiteral("Messages"));
    m_tabs->addTab(buildInterfacesTab(), QStringLiteral("Interfaces"));
    m_tabs->addTab(buildDestinationsTab(), QStringLiteral("Destinations"));
    m_tabs->addTab(buildActivityTab(), QStringLiteral("Activity"));
    m_tabs->addTab(buildIdentityTab(), QStringLiteral("Identity"));
    outer->addWidget(m_tabs, 1);
    connect(m_tabs, &QTabWidget::currentChanged, this, [this] {
        updateViewsNow();
        if (m_host.promptTargetChanged)
            m_host.promptTargetChanged();
    });
    if (m_host.promptTargetChanged) {
        m_composer->hide();
        m_sendButton->hide();
    }

    m_viewTimer->setSingleShot(true);
    m_viewTimer->setInterval(400);
    connect(m_viewTimer, &QTimer::timeout, this, [this] { updateViewsNow(); });
    connect(m_announceTimer, &QTimer::timeout, this, [this] {
        if (m_node->onlineCount() > 0)
            m_node->announce();
    });

    connect(m_announceButton, &QPushButton::clicked, this, [this] { announceNow(); });
    connect(m_powerButton, &QPushButton::clicked, this, [this] {
        if (m_node->running()) {
            m_node->stop();
            if (m_host.setSetting)
                m_host.setSetting(kRunningSetting, false);
            showStatus(QStringLiteral("Disconnected from every interface. Nothing is sent until you connect again."));
        } else {
            m_node->start();
            if (m_host.setSetting)
                m_host.setSetting(kRunningSetting, true);
            showStatus(QString());
        }
        updateViewsNow();
    });

    if (m_host.authorizeConnection)
        m_node->setAuthorizer(m_host.authorizeConnection);
    m_node->setChangedHandler([this] {
        const int online = m_node->onlineCount();
        if (online > 0 && m_previousOnline == 0 && m_announceInterval->currentData().toInt() > 0)
            QTimer::singleShot(3000, this, [this] { m_node->announce(); });
        m_previousOnline = online;
        scheduleViews();
    });
    m_node->setAnnounceHandler([this](const QByteArray &hash) {
        if (m_map)
            m_map->pulse(hash);
        m_messenger->destinationAnnounced(hash);
    });
    m_node->setPacketHandler([this](const QString &id, const Packet &packet, const QByteArray &raw, int hops) {
        m_messenger->handlePacket(id, packet, raw, hops);
    });
    m_messenger->setChangedHandler([this] {
        m_chatDirty = true;
        if (m_host.navigationBadgeChanged)
            m_host.navigationBadgeChanged();
        scheduleViews();
    });
    m_messenger->setSaveHandler([this](const QString &json) {
        if (m_host.setSetting)
            m_host.setSetting(kMessagesSetting, json);
    });
    m_messenger->setIncomingHandler([this](const ChatMessage &message) {
        const bool watching = isVisible() && m_tabs->currentIndex() == MessagesTab && m_currentPeer == message.peer;
        if (watching) {
            m_messenger->markRead(message.peer);
            return;
        }
        if (m_host.notify)
            m_host.notify(QStringLiteral("Reticulum message from %1").arg(m_messenger->peerName(message.peer)),
                          message.content.left(200), true);
    });
    loadSettings();
}

ReticulumFeature::~ReticulumFeature()
{
    m_node->setChangedHandler(nullptr);
    m_node->setAnnounceHandler(nullptr);
    m_node->setPacketHandler(nullptr);
    m_messenger->setChangedHandler(nullptr);
    m_messenger->setIncomingHandler(nullptr);
    m_node->stop();
}

QWidget *ReticulumFeature::buildNetworkTab()
{
    auto *page = new QWidget;
    auto *layout = new QHBoxLayout(page);
    layout->setContentsMargins(0, 10, 0, 0);
    auto *splitter = new QSplitter(Qt::Horizontal);
    splitter->setChildrenCollapsible(false);

    auto *mapColumn = new QWidget;
    auto *mapLayout = new QVBoxLayout(mapColumn);
    mapLayout->setContentsMargins(0, 0, 0, 0);
    auto *mapHeader = new QHBoxLayout;
    mapHeader->addWidget(sectionTitle(QStringLiteral("Network map")));
    mapHeader->addStretch();
    m_mapFilter = new QLineEdit;
    m_mapFilter->setPlaceholderText(QStringLiteral("Highlight by name or hash"));
    m_mapFilter->setClearButtonEnabled(true);
    m_mapFilter->setMaximumWidth(240);
    mapHeader->addWidget(m_mapFilter);
    mapLayout->addLayout(mapHeader);
    m_map = new NetworkMap;
    mapLayout->addWidget(m_map, 1);
    splitter->addWidget(mapColumn);
    connect(m_mapFilter, &QLineEdit::textChanged, m_map, [this](const QString &text) { m_map->setFilter(text); });
    m_map->setActivated([this](const QByteArray &hash) { selectDestination(hash); });

    auto *side = new QWidget;
    auto *sideLayout = new QVBoxLayout(side);
    sideLayout->setContentsMargins(12, 0, 0, 0);
    sideLayout->setSpacing(8);
    m_traffic = new TrafficChart;
    sideLayout->addWidget(m_traffic);
    sideLayout->addWidget(sectionTitle(QStringLiteral("Selected destination")));
    m_detailTitle = new QLabel(QStringLiteral("Nothing selected"));
    m_detailTitle->setObjectName(QStringLiteral("sectionTitle"));
    m_detailTitle->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_detailTitle->setTextFormat(Qt::PlainText);
    sideLayout->addWidget(m_detailTitle);
    m_detailBody = mutedLabel(QStringLiteral("Click a point on the map or a row in Destinations."));
    m_detailBody->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_detailBody->setTextFormat(Qt::RichText);
    sideLayout->addWidget(m_detailBody);
    auto *detailButtons = new QHBoxLayout;
    m_detailPath = makeButton(QStringLiteral("Find path"));
    m_detailCopy = makeButton(QStringLiteral("Copy hash"));
    m_detailForget = makeButton(QStringLiteral("Forget"));
    m_detailMessage = makeButton(QStringLiteral("Message"), QStringLiteral("primaryButton"));
    detailButtons->addWidget(m_detailMessage);
    connect(m_detailMessage, &QPushButton::clicked, this, [this] { openConversation(m_selected); });
    detailButtons->addWidget(m_detailPath);
    detailButtons->addWidget(m_detailCopy);
    detailButtons->addWidget(m_detailForget);
    detailButtons->addStretch();
    sideLayout->addLayout(detailButtons);
    sideLayout->addStretch();
    splitter->addWidget(side);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    layout->addWidget(splitter);

    connect(m_detailPath, &QPushButton::clicked, this, [this] { requestPathFor(m_selected); });
    connect(m_detailCopy, &QPushButton::clicked, this,
            [this] { copyText(hexHash(m_selected), QStringLiteral("Destination hash")); });
    connect(m_detailForget, &QPushButton::clicked, this, [this] {
        m_node->forgetDestination(m_selected);
        m_selected.clear();
        updateViewsNow();
    });
    return page;
}

QWidget *ReticulumFeature::buildMessagesTab()
{
    auto *page = new QWidget;
    auto *layout = new QHBoxLayout(page);
    layout->setContentsMargins(0, 10, 0, 0);
    auto *splitter = new QSplitter(Qt::Horizontal);
    splitter->setChildrenCollapsible(false);

    auto *left = new QWidget;
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 8, 0);
    auto *newButton = makeButton(QStringLiteral("New message"), QStringLiteral("primaryButton"));
    leftLayout->addWidget(newButton);
    m_conversationList = new QListWidget;
    m_conversationList->setObjectName(QStringLiteral("reticulumConversations"));
    m_conversationList->setAccessibleName(QStringLiteral("Reticulum conversations"));
    m_conversationList->setTextElideMode(Qt::ElideRight);
    m_conversationList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_conversationList->setWordWrap(false);
    leftLayout->addWidget(m_conversationList, 1);
    leftLayout->addWidget(mutedLabel(QStringLiteral("Messages use LXMF, so Sideband, MeshChat and Nomad Network "
                                                    "users can write to you. Announce so they can find you.")));
    splitter->addWidget(left);

    m_chatPane = new QWidget;
    auto *chatLayout = new QVBoxLayout(m_chatPane);
    chatLayout->setContentsMargins(8, 0, 0, 0);
    auto *chatHeader = new QHBoxLayout;
    auto *titles = new QVBoxLayout;
    m_chatTitle = new QLabel;
    m_chatTitle->setObjectName(QStringLiteral("sectionTitle"));
    m_chatTitle->setTextFormat(Qt::PlainText);
    m_chatSubtitle = mutedLabel();
    m_chatSubtitle->setTextFormat(Qt::PlainText);
    m_chatSubtitle->setTextInteractionFlags(Qt::TextSelectableByMouse);
    titles->addWidget(m_chatTitle);
    titles->addWidget(m_chatSubtitle);
    chatHeader->addLayout(titles, 1);
    m_chatPathButton = makeButton(QStringLiteral("Find path"));
    auto *clearButton = makeButton(QStringLiteral("Delete conversation"));
    chatHeader->addWidget(m_chatPathButton);
    chatHeader->addWidget(clearButton);
    chatLayout->addLayout(chatHeader);
    m_chatList = new QListWidget;
    m_chatList->setObjectName(QStringLiteral("reticulumChat"));
    m_chatList->setAccessibleName(QStringLiteral("Messages"));
    m_chatList->setSelectionMode(QAbstractItemView::NoSelection);
    m_chatList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_chatList->setFrameShape(QFrame::NoFrame);
    m_chatList->viewport()->installEventFilter(this);
    chatLayout->addWidget(m_chatList, 1);
    m_chatEmpty = mutedLabel(QStringLiteral("Pick a conversation, or start one with New message."));
    m_chatEmpty->setAlignment(Qt::AlignCenter);
    chatLayout->addWidget(m_chatEmpty, 1);
    m_composer = new QPlainTextEdit;
    m_composer->setPlaceholderText(QStringLiteral("Write a message. Ctrl+Enter sends."));
    m_composer->setMaximumHeight(96);
    m_composer->installEventFilter(this);
    chatLayout->addWidget(m_composer);
    auto *composerRow = new QHBoxLayout;
    m_composerHint = mutedLabel();
    m_composerHint->setWordWrap(false);
    composerRow->addWidget(m_composerHint, 1);
    m_sendButton = makeButton(QStringLiteral("Send"), QStringLiteral("primaryButton"));
    composerRow->addWidget(m_sendButton);
    chatLayout->addLayout(composerRow);
    splitter->addWidget(m_chatPane);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 3);
    layout->addWidget(splitter);

    connect(newButton, &QPushButton::clicked, this, [this] { startConversation(); });
    connect(m_conversationList, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *item) {
        if (!item)
            return;
        const QByteArray peer = item->data(kHashRole).toByteArray();
        if (peer != m_currentPeer)
            openConversation(peer);
    });
    connect(m_sendButton, &QPushButton::clicked, this, [this] { sendMessage(m_composer->toPlainText()); });
    connect(m_composer, &QPlainTextEdit::textChanged, this, [this] { updateComposerHint(); });
    connect(m_chatPathButton, &QPushButton::clicked, this, [this] { requestPathFor(m_currentPeer); });
    connect(clearButton, &QPushButton::clicked, this, [this] {
        if (m_currentPeer.isEmpty())
            return;
        if (QMessageBox::question(this, QStringLiteral("Delete conversation"),
                                  QStringLiteral("Delete every message with %1 from this desktop?")
                                      .arg(m_messenger->peerName(m_currentPeer)),
                                  QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
            return;
        m_messenger->clearConversation(m_currentPeer);
        m_currentPeer.clear();
        m_chatDirty = true;
        updateMessages();
    });
    return page;
}

void ReticulumFeature::openConversation(const QByteArray &peer)
{
    if (peer.size() != kHashLength)
        return;
    m_currentPeer = peer;
    m_chatDirty = true;
    if (m_tabs->currentIndex() != MessagesTab)
        m_tabs->setCurrentIndex(MessagesTab);
    m_messenger->markRead(peer);
    if (m_host.promptTargetChanged)
        m_host.promptTargetChanged();
    if (!m_node->identityFor(peer) && m_node->onlineCount() > 0 && !m_node->lookups().contains(peer))
        m_node->requestPath(peer);
    updateMessages();
    m_composer->setFocus();
}

QString ReticulumFeature::sendMessage(const QString &text)
{
    if (m_currentPeer.isEmpty() || text.trimmed().isEmpty())
        return {};
    if (text.toUtf8().size() > Messenger::kMaxContentBytes) {
        showStatus(QStringLiteral("Messages can be up to %1 KB.").arg(Messenger::kMaxContentBytes / 1024), true);
        return {};
    }
    const QString id = m_messenger->send(m_currentPeer, text);
    if (id.isEmpty()) {
        showStatus(QStringLiteral("That message could not be queued."), true);
        return {};
    }
    m_composer->clear();
    m_chatDirty = true;
    updateMessages();
    return id;
}

void ReticulumFeature::startConversation()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("New message"));
    dialog.resize(460, 420);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(mutedLabel(QStringLiteral("Pick someone who has announced on the network, or paste an LXMF address.")));
    auto *filter = new QLineEdit;
    filter->setPlaceholderText(QStringLiteral("Filter by name"));
    filter->setClearButtonEnabled(true);
    layout->addWidget(filter);
    auto *list = new QListWidget;
    QList<DestinationRecord> peers;
    for (const DestinationRecord &record : m_node->destinations()) {
        if (record.aspect == QStringLiteral("lxmf.delivery"))
            peers.append(record);
    }
    std::sort(peers.begin(), peers.end(),
              [](const DestinationRecord &a, const DestinationRecord &b) { return a.lastHeard > b.lastHeard; });
    for (const DestinationRecord &record : std::as_const(peers)) {
        auto *item = new QListWidgetItem(QStringLiteral("%1 · %2 · heard %3")
                                             .arg(record.displayName.isEmpty() ? shortHash(record.hash) : record.displayName,
                                                  hopsText(record.hops), formatAge(record.lastHeard)));
        item->setData(kHashRole, record.hash);
        list->addItem(item);
    }
    layout->addWidget(list, 1);
    auto *address = new QLineEdit;
    address->setPlaceholderText(QStringLiteral("LXMF address, 32 hex characters"));
    layout->addWidget(address);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Open conversation"));
    layout->addWidget(buttons);
    connect(filter, &QLineEdit::textChanged, &dialog, [list](const QString &text) {
        for (int i = 0; i < list->count(); ++i)
            list->item(i)->setHidden(!list->item(i)->text().contains(text, Qt::CaseInsensitive));
    });
    connect(list, &QListWidget::currentItemChanged, &dialog, [address](QListWidgetItem *item) {
        if (item)
            address->setText(hexHash(item->data(kHashRole).toByteArray()));
    });
    connect(list, &QListWidget::itemDoubleClicked, &dialog, [&dialog] { dialog.accept(); });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&dialog, address] {
        if (!parseHash(address->text()).isEmpty())
            dialog.accept();
    });
    if (dialog.exec() != QDialog::Accepted)
        return;
    openConversation(parseHash(address->text()));
}

QWidget *ReticulumFeature::messageBubble(const ChatMessage &message, int width)
{
    const ChartPalette colors = ChartPalette::forWidget(this);
    auto *row = new QWidget;
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(4, 4, 4, 4);
    auto *bubble = new QFrame;
    bubble->setObjectName(QStringLiteral("reticulumBubble"));
    QColor fill = message.outgoing ? colors.accent : colors.raised;
    if (message.outgoing)
        fill.setAlphaF(colors.dark ? 0.22 : 0.12);
    bubble->setStyleSheet(QStringLiteral("QFrame#reticulumBubble { background: %1; border: 1px solid %2; border-radius: 10px; }"
                                         "QFrame#reticulumBubble QLabel { background: transparent; border: none; }")
                              .arg(fill.name(QColor::HexArgb), colors.border.name()));
    auto *bubbleLayout = new QVBoxLayout(bubble);
    bubbleLayout->setContentsMargins(12, 8, 12, 8);
    bubbleLayout->setSpacing(4);
    const int textWidth = qMax(120, width - 40);
    if (!message.title.isEmpty()) {
        auto *title = new QLabel(message.title);
        title->setTextFormat(Qt::PlainText);
        title->setWordWrap(true);
        QFont bold = title->font();
        bold.setBold(true);
        title->setFont(bold);
        title->setMaximumWidth(textWidth);
        bubbleLayout->addWidget(title);
    }
    auto *text = new QLabel(message.content.left(20000));
    text->setTextFormat(Qt::PlainText);
    text->setWordWrap(true);
    text->setTextInteractionFlags(Qt::TextSelectableByMouse);
    const QRect bounds = text->fontMetrics().boundingRect(QRect(0, 0, textWidth, 100000), Qt::TextWordWrap,
                                                          message.content.left(20000));
    text->setFixedWidth(qMin(textWidth, bounds.width() + 4));
    text->setMinimumHeight(bounds.height());
    bubbleLayout->addWidget(text);
    for (const LxmfAttachment &attachment : message.attachments) {
        auto *save = makeButton(QStringLiteral("Save %1 (%2)").arg(attachment.name, formatBytes(double(attachment.data.size()))));
        save->setEnabled(!attachment.data.isEmpty());
        connect(save, &QPushButton::clicked, this, [this, attachment] {
            const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save attachment"), attachment.name);
            if (path.isEmpty())
                return;
            QFile file(path);
            if (file.open(QIODevice::WriteOnly) && file.write(attachment.data) == attachment.data.size())
                showStatus(QStringLiteral("Saved %1.").arg(path));
            else
                showStatus(QStringLiteral("Could not save %1.").arg(path), true);
        });
        bubbleLayout->addWidget(save);
    }
    QStringList meta;
    meta << QDateTime::fromMSecsSinceEpoch(qint64(message.timestamp * 1000)).toLocalTime().toString(QStringLiteral("d MMM HH:mm"));
    if (message.outgoing) {
        QString state = messageStateLabel(message.state);
        if (message.state == MessageState::Delivered)
            state = QStringLiteral("✓✓ Delivered");
        else if (message.state == MessageState::Sent)
            state = QStringLiteral("✓ Sent");
        else if (message.state == MessageState::Sending && message.progress > 0)
            state = QStringLiteral("Sending %1%").arg(message.progress);
        meta << state;
    } else {
        meta << (message.verified ? QStringLiteral("Signature checked") : QStringLiteral("Signature not checked"));
    }
    if (!message.method.isEmpty())
        meta << (message.method == QStringLiteral("opportunistic") ? QStringLiteral("single packet")
                 : message.method == QStringLiteral("link")        ? QStringLiteral("over a link")
                                                                   : QStringLiteral("as a resource"));
    auto *metaLabel = new QLabel(meta.join(QStringLiteral(" · ")));
    metaLabel->setObjectName(QStringLiteral("mutedLabel"));
    metaLabel->setTextFormat(Qt::PlainText);
    bubbleLayout->addWidget(metaLabel);
    if (!message.note.isEmpty() && message.state != MessageState::Delivered) {
        auto *note = new QLabel(message.note);
        note->setObjectName(message.state == MessageState::Failed ? QStringLiteral("errorLabel") : QStringLiteral("mutedLabel"));
        note->setTextFormat(Qt::PlainText);
        note->setWordWrap(true);
        note->setMaximumWidth(textWidth);
        bubbleLayout->addWidget(note);
    }
    if (message.outgoing && (message.state == MessageState::Failed || message.state == MessageState::Sent)) {
        auto *actions = new QHBoxLayout;
        auto *retry = makeButton(message.state == MessageState::Failed ? QStringLiteral("Retry") : QStringLiteral("Send again"));
        auto *remove = makeButton(QStringLiteral("Delete"));
        const QString id = message.id;
        connect(retry, &QPushButton::clicked, this, [this, id] { m_messenger->retry(id); });
        connect(remove, &QPushButton::clicked, this, [this, id] { m_messenger->forget(id); });
        actions->addWidget(retry);
        actions->addWidget(remove);
        actions->addStretch();
        bubbleLayout->addLayout(actions);
    }
    if (message.outgoing) {
        rowLayout->addStretch();
        rowLayout->addWidget(bubble);
    } else {
        rowLayout->addWidget(bubble);
        rowLayout->addStretch();
    }
    return row;
}

void ReticulumFeature::updateMessages()
{
    const QList<Conversation> conversations = m_messenger->conversations();
    m_conversationList->blockSignals(true);
    m_conversationList->clear();
    bool currentListed = false;
    for (const Conversation &conversation : conversations) {
        QString preview = conversation.last.content.simplified().left(80);
        if (conversation.last.outgoing)
            preview = QStringLiteral("You: ") + preview;
        auto *item = new QListWidgetItem(QStringLiteral("%1%2\n%3")
                                             .arg(conversation.name,
                                                  conversation.unread > 0 ? QStringLiteral("  (%1 new)").arg(conversation.unread)
                                                                          : QString(),
                                                  preview));
        item->setData(kHashRole, conversation.peer);
        if (conversation.unread > 0) {
            QFont bold = item->font();
            bold.setBold(true);
            item->setFont(bold);
        }
        m_conversationList->addItem(item);
        if (conversation.peer == m_currentPeer) {
            m_conversationList->setCurrentItem(item);
            currentListed = true;
        }
    }
    if (!m_currentPeer.isEmpty() && !currentListed) {
        auto *item = new QListWidgetItem(QStringLiteral("%1\nNew conversation").arg(m_messenger->peerName(m_currentPeer)));
        item->setData(kHashRole, m_currentPeer);
        m_conversationList->insertItem(0, item);
        m_conversationList->setCurrentItem(item);
    }
    m_conversationList->blockSignals(false);
    if (m_chatDirty)
        updateChat();
    updateComposerHint();
}

void ReticulumFeature::updateChat()
{
    m_chatDirty = false;
    const bool has = !m_currentPeer.isEmpty();
    m_chatList->setVisible(has);
    m_chatEmpty->setVisible(!has);
    m_composer->setEnabled(has);
    m_sendButton->setEnabled(has);
    m_chatPathButton->setEnabled(has && m_node->onlineCount() > 0);
    if (!has) {
        m_chatTitle->setText(QStringLiteral("Messages"));
        m_chatSubtitle->setText(QStringLiteral("Your address: %1").arg(hexHash(m_messenger->address())));
        m_chatList->clear();
        return;
    }
    if (isVisible() && m_tabs->currentIndex() == MessagesTab)
        m_messenger->markRead(m_currentPeer);
    m_chatTitle->setText(m_messenger->peerName(m_currentPeer));
    const auto record = m_node->destinations().constFind(m_currentPeer);
    QString subtitle = hexHash(m_currentPeer);
    if (record != m_node->destinations().cend())
        subtitle += QStringLiteral(" · %1 via %2 · heard %3")
                        .arg(hopsText(record->hops), interfaceName(record->interfaceId), formatAge(record->lastHeard));
    else
        subtitle += QStringLiteral(" · not heard yet");
    const int cost = m_messenger->peerStampCost(m_currentPeer);
    if (cost > 0)
        subtitle += QStringLiteral(" · asks for %1 bits of proof of work").arg(cost);
    m_chatSubtitle->setText(subtitle);
    const QList<ChatMessage> messages = m_messenger->messages(m_currentPeer);
    const bool atBottom = m_chatList->verticalScrollBar()->value() >= m_chatList->verticalScrollBar()->maximum() - 4;
    m_chatList->setUpdatesEnabled(false);
    m_chatList->clear();
    const int width = qMin(560, int(m_chatList->viewport()->width() * 0.72));
    const int start = qMax(0, int(messages.size()) - 300);
    for (int i = start; i < messages.size(); ++i) {
        QWidget *bubble = messageBubble(messages.at(i), width);
        auto *item = new QListWidgetItem;
        item->setFlags(Qt::ItemIsEnabled);
        bubble->adjustSize();
        item->setSizeHint(bubble->sizeHint());
        m_chatList->addItem(item);
        m_chatList->setItemWidget(item, bubble);
    }
    m_chatList->setUpdatesEnabled(true);
    if (atBottom || m_chatList->count() <= 1)
        m_chatList->scrollToBottom();
}

void ReticulumFeature::updateComposerHint()
{
    const QByteArray text = m_composer->toPlainText().toUtf8();
    if (m_currentPeer.isEmpty()) {
        m_composerHint->clear();
        return;
    }
    if (text.isEmpty()) {
        m_composerHint->setText(m_node->onlineCount() > 0 ? QStringLiteral("End-to-end encrypted with the recipient's key")
                                                          : QStringLiteral("Connect an interface to send"));
        return;
    }
    const int size = int(text.size()) + 16;
    QString route = size <= kLxmfOpportunisticMaxContent ? QStringLiteral("fits in a single packet")
                    : size <= kLxmfLinkPacketMaxContent  ? QStringLiteral("goes over an encrypted link")
                                                         : QStringLiteral("goes over a link as a resource");
    m_composerHint->setText(QStringLiteral("%1 bytes · %2").arg(text.size()).arg(route));
}

bool ReticulumFeature::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_composer && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) && (key->modifiers() & Qt::ControlModifier)) {
            sendMessage(m_composer->toPlainText());
            return true;
        }
    }
    if (m_chatList && watched == m_chatList->viewport() && event->type() == QEvent::Resize) {
        m_chatDirty = true;
        QTimer::singleShot(0, this, [this] {
            if (m_chatDirty && m_tabs->currentIndex() == MessagesTab)
                updateChat();
        });
    }
    return forkmesh::market::NativePackageWidget::eventFilter(watched, event);
}

forkmesh::market::NativePackagePromptTarget ReticulumFeature::hostPromptTarget() const
{
    if (m_tabs->currentIndex() != MessagesTab || m_currentPeer.isEmpty())
        return {};
    const QString name = m_messenger->peerName(m_currentPeer);
    return {QStringLiteral("Message %1, end-to-end encrypted over Reticulum").arg(name),
            QStringLiteral("Send to %1").arg(name)};
}

bool ReticulumFeature::submitHostPrompt(const QString &text, const QStringList &attachments)
{
    if (!attachments.isEmpty() || m_tabs->currentIndex() != MessagesTab || m_currentPeer.isEmpty())
        return false;
    return !sendMessage(text).isEmpty();
}

int ReticulumFeature::navigationBadge() const
{
    return m_messenger ? m_messenger->unreadCount() : 0;
}

QWidget *ReticulumFeature::buildInterfacesTab()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 10, 0, 0);
    auto *buttons = new QHBoxLayout;
    auto *addButton = makeButton(QStringLiteral("Add interface"), QStringLiteral("primaryButton"));
    auto *publicButton = makeButton(QStringLiteral("Add public entry point"));
    auto *publicMenu = new QMenu(publicButton);
    for (const InterfaceConfig &entry : publicEntryPoints()) {
        publicMenu->addAction(QStringLiteral("%1 (%2)").arg(entry.name, entry.endpoint()), this, [this, entry] {
            InterfaceConfig copy = entry;
            copy.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            addInterface(copy);
        });
    }
    publicButton->setMenu(publicMenu);
    auto *importButton = makeButton(QStringLiteral("Import Reticulum config…"));
    m_editButton = makeButton(QStringLiteral("Edit"));
    m_toggleButton = makeButton(QStringLiteral("Turn off"));
    m_reconnectButton = makeButton(QStringLiteral("Reconnect"));
    m_removeButton = makeButton(QStringLiteral("Remove"), QStringLiteral("dangerButton"));
    for (QPushButton *button : {addButton, publicButton, importButton})
        buttons->addWidget(button);
    buttons->addStretch();
    for (QPushButton *button : {m_reconnectButton, m_toggleButton, m_editButton, m_removeButton})
        buttons->addWidget(button);
    layout->addLayout(buttons);

    auto *splitter = new QSplitter(Qt::Vertical);
    splitter->setChildrenCollapsible(false);
    m_interfaceTree = makeTree(QStringLiteral("reticulumInterfaces"),
                               {QStringLiteral("Interface"), QStringLiteral("Type"), QStringLiteral("Endpoint"),
                                QStringLiteral("Status"), QStringLiteral("Online for"), QStringLiteral("Received"),
                                QStringLiteral("Sent"), QStringLiteral("Packets in / out"), QStringLiteral("Announces")},
                               QStringLiteral("Reticulum interfaces"));
    m_interfaceTree->header()->resizeSection(0, 200);
    m_interfaceTree->header()->resizeSection(2, 230);
    m_interfaceTree->header()->resizeSection(3, 110);
    splitter->addWidget(m_interfaceTree);

    auto *lower = new QWidget;
    auto *lowerLayout = new QHBoxLayout(lower);
    lowerLayout->setContentsMargins(0, 6, 0, 0);
    auto *detailColumn = new QVBoxLayout;
    m_interfaceDetail = mutedLabel(QStringLiteral("Pick an interface to see its details."));
    m_interfaceDetail->setTextFormat(Qt::RichText);
    m_interfaceDetail->setTextInteractionFlags(Qt::TextSelectableByMouse);
    detailColumn->addWidget(m_interfaceDetail);
    detailColumn->addStretch();
    lowerLayout->addLayout(detailColumn, 2);
    m_interfaceTraffic = new TrafficChart;
    m_interfaceTraffic->setTitle(QStringLiteral("This interface, last 3 minutes"));
    lowerLayout->addWidget(m_interfaceTraffic, 3);
    splitter->addWidget(lower);

    auto *discovered = new QWidget;
    auto *discoveredLayout = new QVBoxLayout(discovered);
    discoveredLayout->setContentsMargins(0, 6, 0, 0);
    auto *discoveredHeader = new QHBoxLayout;
    discoveredHeader->addWidget(sectionTitle(QStringLiteral("Entry points announced on the network")));
    discoveredHeader->addStretch();
    m_useDiscovered = makeButton(QStringLiteral("Connect to this one"));
    discoveredHeader->addWidget(m_useDiscovered);
    discoveredLayout->addLayout(discoveredHeader);
    m_discoveredTree = makeTree(QStringLiteral("reticulumDiscovered"),
                                {QStringLiteral("Name"), QStringLiteral("Type"), QStringLiteral("Address"),
                                 QStringLiteral("Transport"), QStringLiteral("Proof of work"), QStringLiteral("Location"),
                                 QStringLiteral("Heard")},
                                QStringLiteral("Discovered Reticulum entry points"));
    m_discoveredTree->header()->resizeSection(0, 200);
    m_discoveredTree->header()->resizeSection(2, 230);
    discoveredLayout->addWidget(m_discoveredTree);
    splitter->addWidget(discovered);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    splitter->setStretchFactor(2, 2);
    layout->addWidget(splitter, 1);

    connect(m_interfaceTree, &QTreeWidget::itemSelectionChanged, this, [this] { updateInterfaces(); });
    connect(m_interfaceTree, &QTreeWidget::itemDoubleClicked, this, [this] { m_editButton->click(); });
    connect(addButton, &QPushButton::clicked, this, [this] {
        InterfaceConfig config;
        config.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        config.name = QStringLiteral("New interface");
        if (editInterface(&config, true))
            addInterface(config);
    });
    connect(importButton, &QPushButton::clicked, this, [this] {
        QString start = reticulumConfigPath();
        const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Import a Reticulum config"), start);
        if (!path.isEmpty())
            showStatus(importReticulumConfig(path));
    });
    connect(m_editButton, &QPushButton::clicked, this, [this] {
        const QString id = selectedInterfaceId();
        for (InterfaceConfig config : m_node->interfaces()) {
            if (config.id != id)
                continue;
            if (editInterface(&config, false)) {
                m_node->upsertInterface(config);
                saveInterfaces();
            }
            break;
        }
    });
    connect(m_toggleButton, &QPushButton::clicked, this, [this] {
        const QString id = selectedInterfaceId();
        for (const InterfaceConfig &config : m_node->interfaces()) {
            if (config.id == id) {
                m_node->setInterfaceEnabled(id, !config.enabled);
                saveInterfaces();
                break;
            }
        }
        updateViewsNow();
    });
    connect(m_reconnectButton, &QPushButton::clicked, this, [this] {
        m_node->reconnectInterface(selectedInterfaceId());
        updateViewsNow();
    });
    connect(m_removeButton, &QPushButton::clicked, this, [this] {
        const QString id = selectedInterfaceId();
        const QString name = interfaceName(id);
        if (id.isEmpty())
            return;
        if (QMessageBox::question(this, QStringLiteral("Remove interface"),
                                  QStringLiteral("Remove %1? Destinations heard through it stay in the list.").arg(name),
                                  QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
            return;
        m_node->removeInterface(id);
        saveInterfaces();
        updateViewsNow();
    });
    connect(m_discoveredTree, &QTreeWidget::itemSelectionChanged, this,
            [this] { m_useDiscovered->setEnabled(!m_discoveredTree->selectedItems().isEmpty()); });
    connect(m_useDiscovered, &QPushButton::clicked, this, [this] {
        const QList<QTreeWidgetItem *> items = m_discoveredTree->selectedItems();
        if (items.isEmpty())
            return;
        const QByteArray hash = items.first()->data(0, kHashRole).toByteArray();
        const auto found = m_node->destinations().constFind(hash);
        if (found == m_node->destinations().cend() || !found->discovery)
            return;
        InterfaceConfig config;
        config.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        config.name = found->discovery->name.isEmpty() ? found->discovery->reachableOn : found->discovery->name;
        config.host = found->discovery->reachableOn;
        config.port = found->discovery->port;
        config.kiss = found->discovery->type == QStringLiteral("KISSInterface");
        if (editInterface(&config, true))
            addInterface(config);
    });
    m_useDiscovered->setEnabled(false);
    return page;
}

QWidget *ReticulumFeature::buildDestinationsTab()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 10, 0, 0);

    auto *lookupRow = new QHBoxLayout;
    lookupRow->addWidget(new QLabel(QStringLiteral("Find a path to")));
    m_lookupEdit = new QLineEdit;
    m_lookupEdit->setPlaceholderText(QStringLiteral("Destination hash, 32 hex characters"));
    m_lookupEdit->setClearButtonEnabled(true);
    lookupRow->addWidget(m_lookupEdit, 1);
    auto *lookupButton = makeButton(QStringLiteral("Find path"), QStringLiteral("primaryButton"));
    lookupRow->addWidget(lookupButton);
    layout->addLayout(lookupRow);
    m_lookupResult = mutedLabel();
    m_lookupResult->setTextFormat(Qt::PlainText);
    m_lookupResult->hide();
    layout->addWidget(m_lookupResult);

    auto *filterRow = new QHBoxLayout;
    m_destinationFilter = new QLineEdit;
    m_destinationFilter->setPlaceholderText(QStringLiteral("Filter by name, application or hash"));
    m_destinationFilter->setClearButtonEnabled(true);
    filterRow->addWidget(m_destinationFilter, 1);
    m_kindFilter = new QComboBox;
    m_kindFilter->addItem(QStringLiteral("Every kind"), -1);
    for (MapGroup group : {MapGroup::Peer, MapGroup::Node, MapGroup::Propagation, MapGroup::Transport, MapGroup::Other})
        m_kindFilter->addItem(mapGroupLabel(group), int(group));
    filterRow->addWidget(m_kindFilter);
    m_destinationCount = mutedLabel();
    m_destinationCount->setWordWrap(false);
    m_destinationCount->setTextFormat(Qt::PlainText);
    m_destinationCount->setMinimumWidth(130);
    filterRow->addWidget(m_destinationCount);
    auto *clearButton = makeButton(QStringLiteral("Clear list"));
    filterRow->addWidget(clearButton);
    layout->addLayout(filterRow);

    m_destinationTree = makeTree(QStringLiteral("reticulumDestinations"),
                                 {QStringLiteral("Name"), QStringLiteral("Application"), QStringLiteral("Hops"),
                                  QStringLiteral("Heard via"), QStringLiteral("Last heard"), QStringLiteral("Announces"),
                                  QStringLiteral("Destination")},
                                 QStringLiteral("Reticulum destinations"));
    m_destinationTree->header()->resizeSection(0, 220);
    m_destinationTree->header()->resizeSection(1, 170);
    m_destinationTree->header()->resizeSection(2, 60);
    m_destinationTree->header()->resizeSection(3, 170);
    m_destinationTree->header()->resizeSection(4, 110);
    m_destinationTree->setSortingEnabled(true);
    m_destinationTree->sortByColumn(4, Qt::DescendingOrder);
    m_destinationTree->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(m_destinationTree, 1);

    const auto lookup = [this] {
        const QByteArray hash = parseHash(m_lookupEdit->text());
        if (hash.isEmpty()) {
            m_lookupResult->setText(QStringLiteral("A destination hash is 32 hexadecimal characters, like the ones in the list below."));
            m_lookupResult->show();
            return;
        }
        requestPathFor(hash);
    };
    connect(lookupButton, &QPushButton::clicked, this, lookup);
    connect(m_lookupEdit, &QLineEdit::returnPressed, this, lookup);
    connect(m_destinationFilter, &QLineEdit::textChanged, this, [this] {
        m_lastTableRefresh = 0;
        updateDestinations();
    });
    connect(m_kindFilter, &QComboBox::currentIndexChanged, this, [this] {
        m_lastTableRefresh = 0;
        updateDestinations();
    });
    connect(clearButton, &QPushButton::clicked, this, [this] {
        m_node->clearDestinations();
        m_selected.clear();
        updateViewsNow();
    });
    connect(m_destinationTree, &QTreeWidget::itemSelectionChanged, this, [this] {
        const QList<QTreeWidgetItem *> items = m_destinationTree->selectedItems();
        if (!items.isEmpty()) {
            m_selected = items.first()->data(0, kHashRole).toByteArray();
            if (m_map)
                m_map->setSelected(m_selected);
            updateDetail();
        }
    });
    connect(m_destinationTree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item) {
        selectDestination(item->data(0, kHashRole).toByteArray());
        m_tabs->setCurrentIndex(NetworkTab);
    });
    connect(m_destinationTree, &QWidget::customContextMenuRequested, this, [this](const QPoint &point) {
        QTreeWidgetItem *item = m_destinationTree->itemAt(point);
        if (!item)
            return;
        const QByteArray hash = item->data(0, kHashRole).toByteArray();
        QMenu menu(this);
        menu.addAction(QStringLiteral("Show on map"), this, [this, hash] {
            selectDestination(hash);
            m_tabs->setCurrentIndex(NetworkTab);
        });
        menu.addAction(QStringLiteral("Find path"), this, [this, hash] { requestPathFor(hash); });
        menu.addAction(QStringLiteral("Copy destination hash"), this,
                       [this, hash] { copyText(hexHash(hash), QStringLiteral("Destination hash")); });
        const auto found = m_node->destinations().constFind(hash);
        if (found != m_node->destinations().cend())
            menu.addAction(QStringLiteral("Copy identity hash"), this, [this, identity = found->identityHash] {
                copyText(hexHash(identity), QStringLiteral("Identity hash"));
            });
        menu.addSeparator();
        menu.addAction(QStringLiteral("Forget"), this, [this, hash] {
            m_node->forgetDestination(hash);
            if (m_selected == hash)
                m_selected.clear();
            updateViewsNow();
        });
        menu.exec(m_destinationTree->viewport()->mapToGlobal(point));
    });
    return page;
}

QWidget *ReticulumFeature::buildActivityTab()
{
    auto *page = new QWidget;
    auto *layout = new QHBoxLayout(page);
    layout->setContentsMargins(0, 10, 0, 0);
    m_activity = new QListWidget;
    m_activity->setObjectName(QStringLiteral("reticulumActivity"));
    m_activity->setAccessibleName(QStringLiteral("Reticulum activity"));
    m_activity->setAlternatingRowColors(true);
    m_activity->setUniformItemSizes(true);
    layout->addWidget(m_activity, 3);
    auto *side = new QVBoxLayout;
    side->addWidget(sectionTitle(QStringLiteral("Packets since you connected")));
    m_counters = mutedLabel();
    m_counters->setTextFormat(Qt::RichText);
    side->addWidget(m_counters);
    side->addStretch();
    layout->addLayout(side, 1);
    connect(m_activity, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        const QByteArray hash = item->data(kHashRole).toByteArray();
        if (!hash.isEmpty() && m_node->destinations().contains(hash)) {
            selectDestination(hash);
            m_tabs->setCurrentIndex(NetworkTab);
        }
    });
    return page;
}

QWidget *ReticulumFeature::buildIdentityTab()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 14, 0, 0);
    layout->setSpacing(10);
    layout->addWidget(sectionTitle(QStringLiteral("This desktop on Reticulum")));
    layout->addWidget(mutedLabel(
        QStringLiteral("ForkMesh keeps a Reticulum identity for this desktop: an X25519 key for encryption and an "
                       "Ed25519 key for signing. Announcing publishes its public key and the name below under the "
                       "forkmesh.node application, so others can find a path to it. Nothing is announced until you "
                       "choose to.")));
    auto *form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight);
    const auto hashRow = [this, form](const QString &label, QLabel **target, const QString &what) {
        auto *row = new QHBoxLayout;
        *target = new QLabel;
        (*target)->setTextInteractionFlags(Qt::TextSelectableByMouse);
        QFont mono = (*target)->font();
        mono.setFamily(QStringLiteral("monospace"));
        mono.setStyleHint(QFont::Monospace);
        (*target)->setFont(mono);
        row->addWidget(*target);
        auto *copy = makeButton(QStringLiteral("Copy"));
        connect(copy, &QPushButton::clicked, this, [this, target, what] {
            copyText((*target)->text().remove(QLatin1Char('<')).remove(QLatin1Char('>')), what);
        });
        row->addWidget(copy);
        row->addStretch();
        form->addRow(label, row);
    };
    hashRow(QStringLiteral("Destination"), &m_destinationHash, QStringLiteral("Destination hash"));
    hashRow(QStringLiteral("Identity"), &m_identityHash, QStringLiteral("Identity hash"));
    hashRow(QStringLiteral("Messaging address"), &m_messagingAddress, QStringLiteral("Messaging address"));
    m_announceName = new QLineEdit;
    m_announceName->setMaxLength(64);
    m_announceName->setPlaceholderText(QStringLiteral("Name shown to others, for example your handle"));
    form->addRow(QStringLiteral("Name in announces"), m_announceName);
    m_announceInterval = new QComboBox;
    m_announceInterval->addItem(QStringLiteral("Only when I click Announce"), 0);
    m_announceInterval->addItem(QStringLiteral("Every 15 minutes"), 15);
    m_announceInterval->addItem(QStringLiteral("Every 30 minutes"), 30);
    m_announceInterval->addItem(QStringLiteral("Every hour"), 60);
    m_announceInterval->addItem(QStringLiteral("Every 3 hours"), 180);
    form->addRow(QStringLiteral("Announce"), m_announceInterval);
    m_answerPaths = new QCheckBox(QStringLiteral("Answer path requests for this desktop"));
    form->addRow(QString(), m_answerPaths);
    m_lastAnnounce = mutedLabel();
    form->addRow(QStringLiteral("Last announce"), m_lastAnnounce);
    layout->addLayout(form);
    auto *buttons = new QHBoxLayout;
    auto *announce = makeButton(QStringLiteral("Announce now"), QStringLiteral("primaryButton"));
    auto *renew = makeButton(QStringLiteral("Make a new identity"), QStringLiteral("dangerButton"));
    buttons->addWidget(announce);
    buttons->addWidget(renew);
    buttons->addStretch();
    layout->addLayout(buttons);
    layout->addStretch();

    connect(announce, &QPushButton::clicked, this, [this] { announceNow(); });
    connect(m_announceName, &QLineEdit::editingFinished, this, [this] {
        m_node->setAnnounceName(m_announceName->text());
        m_messenger->setDisplayName(m_announceName->text());
        if (m_host.setSetting)
            m_host.setSetting(kAnnounceNameSetting, m_node->announceName());
    });
    connect(m_announceInterval, &QComboBox::currentIndexChanged, this, [this] {
        if (m_host.setSetting)
            m_host.setSetting(kAnnounceIntervalSetting, m_announceInterval->currentData().toInt());
        updateAnnounceTimer();
    });
    connect(m_answerPaths, &QCheckBox::toggled, this, [this](bool on) {
        m_node->setAnswerPathRequests(on);
        if (m_host.setSetting)
            m_host.setSetting(kAnswerPathsSetting, on);
    });
    connect(renew, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, QStringLiteral("Make a new identity"),
                                  QStringLiteral("This desktop gets a new destination hash. Anyone who knew the old "
                                                 "one can no longer reach this desktop through it. Continue?"),
                                  QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
            return;
        m_node->setIdentity(Identity::generate());
        saveIdentity();
        updateViewsNow();
        showStatus(QStringLiteral("New identity %1. Announce it so others can find this desktop.")
                       .arg(prettyHash(m_node->destinationHash())));
    });
    return page;
}

void ReticulumFeature::loadSettings()
{
    QList<InterfaceConfig> configs;
    const QString stored = m_host.setting ? m_host.setting(kInterfacesSetting, QString()).toString() : QString();
    if (stored.isEmpty()) {
        configs = defaultInterfaces();
    } else {
        const QJsonArray array = QJsonDocument::fromJson(stored.toUtf8()).array();
        for (const QJsonValue &value : array) {
            if (const std::optional<InterfaceConfig> config = InterfaceConfig::fromJson(value.toObject()))
                configs.append(*config);
        }
    }
    m_node->setInterfaces(configs);
    ensureIdentity();
    const QString name = m_host.setting ? m_host.setting(kAnnounceNameSetting, QString()).toString() : QString();
    m_node->setAnnounceName(name.isEmpty() && m_host.accountName ? m_host.accountName() : name);
    m_announceName->setText(m_node->announceName());
    m_messenger->setDisplayName(m_node->announceName());
    m_messenger->load(m_host.setting ? m_host.setting(kMessagesSetting, QString()).toString() : QString());
    const int minutes = m_host.setting ? m_host.setting(kAnnounceIntervalSetting, 0).toInt() : 0;
    const int index = m_announceInterval->findData(minutes);
    m_announceInterval->blockSignals(true);
    m_announceInterval->setCurrentIndex(index < 0 ? 0 : index);
    m_announceInterval->blockSignals(false);
    const bool answer = m_host.setting ? m_host.setting(kAnswerPathsSetting, true).toBool() : true;
    m_answerPaths->blockSignals(true);
    m_answerPaths->setChecked(answer);
    m_answerPaths->blockSignals(false);
    m_node->setAnswerPathRequests(answer);
    updateAnnounceTimer();
}

void ReticulumFeature::saveInterfaces()
{
    QJsonArray array;
    for (const InterfaceConfig &config : m_node->interfaces())
        array.append(config.toJson());
    if (m_host.setSetting)
        m_host.setSetting(kInterfacesSetting, QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact)));
}

void ReticulumFeature::ensureIdentity()
{
    const QString stored = m_host.setting ? m_host.setting(kIdentitySetting, QString()).toString() : QString();
    if (std::optional<Identity> identity = Identity::fromPrivateKey(QByteArray::fromHex(stored.toLatin1()))) {
        m_node->setIdentity(*identity);
        return;
    }
    m_node->setIdentity(Identity::generate());
    saveIdentity();
}

void ReticulumFeature::saveIdentity()
{
    if (m_host.setSetting && m_node->identity().hasPrivateKey())
        m_host.setSetting(kIdentitySetting, QString::fromLatin1(m_node->identity().privateKey().toHex()));
}

void ReticulumFeature::refresh()
{
    if (!m_started) {
        m_started = true;
        const bool on = m_host.setting ? m_host.setting(kRunningSetting, true).toBool() : true;
        if (on)
            m_node->start();
    } else {
        for (const InterfaceConfig &config : m_node->interfaces()) {
            if (config.enabled && m_node->status(config.id).state != LinkState::Online && m_node->running())
                m_node->reconnectInterface(config.id);
        }
    }
    updateViewsNow();
}

void ReticulumFeature::showEvent(QShowEvent *event)
{
    forkmesh::market::NativePackageWidget::showEvent(event);
    updateViewsNow();
}

void ReticulumFeature::themeChanged()
{
    update();
    for (QWidget *child : findChildren<QWidget *>())
        child->update();
}

void ReticulumFeature::scheduleViews()
{
    if (!isVisible())
        return;
    if (!m_viewTimer->isActive())
        m_viewTimer->start();
}

void ReticulumFeature::updateViewsNow()
{
    updateHeader();
    updateTiles();
    switch (m_tabs->currentIndex()) {
    case NetworkTab:
        updateMap();
        updateDetail();
        break;
    case MessagesTab:
        updateMessages();
        break;
    case InterfacesTab:
        updateInterfaces();
        updateDiscovered();
        break;
    case DestinationsTab:
        updateDestinations();
        break;
    case ActivityTab:
        updateActivity();
        break;
    case IdentityTab:
        updateIdentity();
        break;
    default:
        break;
    }
}

void ReticulumFeature::updateHeader()
{
    const int online = m_node->onlineCount();
    const QList<InterfaceConfig> configs = m_node->interfaces();
    const int enabled = int(std::count_if(configs.cbegin(), configs.cend(), [](const auto &c) { return c.enabled; }));
    m_summary->setText(m_node->running()
                           ? QStringLiteral("%1 of %2 interfaces online · you are %3")
                                 .arg(online)
                                 .arg(enabled)
                                 .arg(shortHash(m_node->destinationHash()))
                           : QStringLiteral("Disconnected"));
    m_powerButton->setText(m_node->running() ? QStringLiteral("Disconnect") : QStringLiteral("Connect"));
    m_announceButton->setEnabled(online > 0);
    const bool hasRemote = std::any_of(configs.cbegin(), configs.cend(), [](const InterfaceConfig &c) {
        return c.kind != InterfaceKind::SharedInstance;
    });
    m_onboarding->setVisible(!hasRemote && online == 0);
}

void ReticulumFeature::updateTiles()
{
    const int online = m_node->onlineCount();
    const QList<InterfaceConfig> configs = m_node->interfaces();
    const int enabled = int(std::count_if(configs.cbegin(), configs.cend(), [](const auto &c) { return c.enabled; }));
    StatTile *status = m_tiles.at(StatusTile);
    if (!m_node->running()) {
        status->setValue(QStringLiteral("Off"), QStringLiteral("Disconnected by you"));
        status->setTone(0);
    } else if (online > 0) {
        status->setValue(QStringLiteral("Online"), QStringLiteral("%1 of %2 interfaces up").arg(online).arg(enabled));
        status->setTone(1);
    } else if (enabled > 0) {
        status->setValue(QStringLiteral("Connecting"), QStringLiteral("No interface is up yet"));
        status->setTone(2);
    } else {
        status->setValue(QStringLiteral("No interfaces"), QStringLiteral("Add one in Interfaces"));
        status->setTone(0);
    }

    const QDateTime now = QDateTime::currentDateTimeUtc();
    const auto &destinations = m_node->destinations();
    int recent = 0;
    int nearest = 0;
    for (const DestinationRecord &record : destinations) {
        if (record.firstHeard.secsTo(now) < 600)
            ++recent;
        if (record.hops <= 1)
            ++nearest;
    }
    m_tiles.at(DestinationsTile)->setValue(QString::number(destinations.size()),
                                           QStringLiteral("%1 new in 10 min · %2 one hop away").arg(recent).arg(nearest));

    const QVector<TrafficSample> history = m_node->totalHistory();
    QVector<double> announces;
    QVector<double> bytes;
    int lastMinute = 0;
    qint64 rx = 0;
    qint64 tx = 0;
    for (int i = 0; i < history.size(); ++i) {
        announces.append(history.at(i).announces);
        bytes.append(double(history.at(i).rx + history.at(i).tx));
        if (i >= history.size() - 60)
            lastMinute += history.at(i).announces;
        if (i >= history.size() - 10) {
            rx += history.at(i).rx;
            tx += history.at(i).tx;
        }
    }
    const int window = int(qMin<qsizetype>(10, qMax<qsizetype>(1, history.size())));
    m_tiles.at(AnnouncesTile)->setValue(QStringLiteral("%1/min").arg(lastMinute),
                                        QStringLiteral("%1 heard · %2 rejected")
                                            .arg(m_node->counters().announces)
                                            .arg(m_node->counters().rejectedAnnounces));
    m_tiles.at(AnnouncesTile)->setSpark(announces);
    m_tiles.at(TrafficTile)->setValue(formatRate(double(rx + tx) / window),
                                      QStringLiteral("↓ %1 · ↑ %2").arg(formatRate(double(rx) / window),
                                                                      formatRate(double(tx) / window)));
    m_tiles.at(TrafficTile)->setSpark(bytes);

    int answered = 0;
    int pending = 0;
    qint64 fastest = -1;
    for (const PathLookup &lookup : m_node->lookups()) {
        if (lookup.answered.isValid()) {
            ++answered;
            const qint64 ms = lookup.started.msecsTo(lookup.answered);
            fastest = fastest < 0 ? ms : qMin(fastest, ms);
        } else if (!lookup.timedOut) {
            ++pending;
        }
    }
    const int unread = m_messenger->unreadCount();
    const QList<Conversation> conversations = m_messenger->conversations();
    m_tiles.at(MessagesTile)->setValue(unread > 0 ? QStringLiteral("%1 unread").arg(unread)
                                                  : QStringLiteral("%1").arg(conversations.size()),
                                       QStringLiteral("%1 conversation%2 · %3 link%4 open")
                                           .arg(conversations.size())
                                           .arg(conversations.size() == 1 ? QString() : QStringLiteral("s"))
                                           .arg(m_messenger->activeLinks())
                                           .arg(m_messenger->activeLinks() == 1 ? QString() : QStringLiteral("s")));
    m_tiles.at(MessagesTile)->setTone(unread > 0 ? 2 : 0);
    m_tiles.at(PathsTile)->setValue(QStringLiteral("%1 found").arg(answered),
                                    pending > 0     ? QStringLiteral("%1 waiting for an answer").arg(pending)
                                    : fastest >= 0 ? QStringLiteral("Fastest answer %1 ms").arg(fastest)
                                                   : QStringLiteral("Ask for one in Destinations"));
}

void ReticulumFeature::updateMap()
{
    if (!m_map)
        return;
    QList<NetworkMap::Spoke> spokes;
    for (const InterfaceConfig &config : m_node->interfaces())
        spokes.append({config.id, config.name, m_node->status(config.id).state == LinkState::Online});
    const QList<DestinationRecord> records = m_node->destinations().values();
    m_map->setData(spokes, records, m_node->destinationHash(), m_node->onlineCount() > 0);
    m_map->setSelected(m_selected);
    m_traffic->setSamples(m_node->totalHistory());
}

void ReticulumFeature::updateDetail()
{
    const auto found = m_node->destinations().constFind(m_selected);
    const bool has = found != m_node->destinations().cend();
    m_detailPath->setEnabled(has && m_node->onlineCount() > 0);
    m_detailCopy->setEnabled(has);
    m_detailForget->setEnabled(has);
    m_detailMessage->setVisible(has && found->aspect == QStringLiteral("lxmf.delivery"));
    if (!has) {
        m_detailTitle->setText(QStringLiteral("Nothing selected"));
        m_detailBody->setText(QStringLiteral("Click a point on the map or a row in Destinations."));
        return;
    }
    const DestinationRecord &record = *found;
    m_detailTitle->setText(record.displayName.isEmpty() ? record.label : record.displayName);
    QStringList rows;
    const auto row = [&rows](const QString &label, const QString &value) {
        rows.append(QStringLiteral("<tr><td style='padding-right:10px'>%1</td><td>%2</td></tr>")
                        .arg(label.toHtmlEscaped(), value));
    };
    row(QStringLiteral("Application"), (record.aspect.isEmpty() ? record.label : QStringLiteral("%1 (%2)").arg(record.label, record.aspect))
                                           .toHtmlEscaped());
    row(QStringLiteral("Destination"), QStringLiteral("<code>%1</code>").arg(hexHash(record.hash)));
    row(QStringLiteral("Identity"), QStringLiteral("<code>%1</code>").arg(hexHash(record.identityHash)));
    row(QStringLiteral("Distance"), hopsText(record.hops).toHtmlEscaped());
    row(QStringLiteral("Heard via"), interfaceName(record.interfaceId).toHtmlEscaped());
    if (!record.transportId.isEmpty())
        row(QStringLiteral("Next hop"), QStringLiteral("<code>%1</code>").arg(hexHash(record.transportId)));
    row(QStringLiteral("First heard"), formatAge(record.firstHeard).toHtmlEscaped());
    row(QStringLiteral("Last heard"), formatAge(record.lastHeard).toHtmlEscaped());
    if (record.emittedAt > 0)
        row(QStringLiteral("Announced at"),
            QDateTime::fromSecsSinceEpoch(record.emittedAt).toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
    row(QStringLiteral("Announces"), QString::number(record.announces));
    row(QStringLiteral("Forward secrecy"), record.ratchet ? QStringLiteral("Ratchet key announced") : QStringLiteral("No ratchet"));
    if (record.discovery) {
        const DiscoveredInterface &found = *record.discovery;
        row(QStringLiteral("Offers"), QStringLiteral("%1 %2%3")
                                          .arg(found.type.toHtmlEscaped(), found.reachableOn.toHtmlEscaped(),
                                               found.port > 0 ? QStringLiteral(":%1").arg(found.port) : QString()));
        row(QStringLiteral("Proof of work"), QStringLiteral("%1 bits%2").arg(found.stampValue).arg(found.stampValid ? QString() : QStringLiteral(" (below 16, not trusted)")));
    }
    const auto lookup = m_node->lookups().constFind(record.hash);
    if (lookup != m_node->lookups().cend()) {
        row(QStringLiteral("Path lookup"),
            lookup->answered.isValid() ? QStringLiteral("Answered in %1 ms").arg(lookup->started.msecsTo(lookup->answered))
            : lookup->timedOut         ? QStringLiteral("No answer")
                                       : QStringLiteral("Waiting…"));
    }
    m_detailBody->setText(QStringLiteral("<table>%1</table>").arg(rows.join(QString())));
}

void ReticulumFeature::updateInterfaces()
{
    const QString selected = selectedInterfaceId();
    const QList<InterfaceConfig> configs = m_node->interfaces();
    m_interfaceTree->setUpdatesEnabled(false);
    while (m_interfaceTree->topLevelItemCount() > configs.size())
        delete m_interfaceTree->takeTopLevelItem(m_interfaceTree->topLevelItemCount() - 1);
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (int i = 0; i < configs.size(); ++i) {
        const InterfaceConfig &config = configs.at(i);
        const InterfaceStatus status = m_node->status(config.id);
        QTreeWidgetItem *item = m_interfaceTree->topLevelItem(i);
        if (!item) {
            item = new QTreeWidgetItem;
            m_interfaceTree->addTopLevelItem(item);
        }
        item->setData(0, kHashRole, config.id);
        item->setText(0, config.name);
        item->setText(1, config.kindLabel());
        item->setText(2, config.endpoint());
        item->setText(3, QStringLiteral("%1 %2").arg(stateTone(status.state), linkStateLabel(status.state)));
        item->setToolTip(3, status.detail);
        item->setText(4, status.onlineSince.isValid() ? formatDuration(status.onlineSince.secsTo(now)) : QStringLiteral("—"));
        item->setText(5, formatBytes(double(status.rxBytes)));
        item->setText(6, formatBytes(double(status.txBytes)));
        item->setText(7, QStringLiteral("%1 / %2").arg(status.rxPackets).arg(status.txPackets));
        item->setText(8, QString::number(status.announces));
        if (config.id == selected)
            item->setSelected(true);
    }
    m_interfaceTree->setUpdatesEnabled(true);

    const QString id = selectedInterfaceId();
    const bool has = !id.isEmpty();
    m_editButton->setEnabled(has);
    m_removeButton->setEnabled(has);
    m_toggleButton->setEnabled(has);
    m_reconnectButton->setEnabled(has && m_node->running());
    if (!has) {
        m_interfaceDetail->setText(QStringLiteral("Pick an interface to see its details."));
        m_interfaceTraffic->setSamples({});
        return;
    }
    InterfaceConfig config;
    for (const InterfaceConfig &candidate : configs) {
        if (candidate.id == id)
            config = candidate;
    }
    const InterfaceStatus status = m_node->status(id);
    m_toggleButton->setText(config.enabled ? QStringLiteral("Turn off") : QStringLiteral("Turn on"));
    QStringList lines;
    lines << QStringLiteral("<b>%1</b>").arg(config.name.toHtmlEscaped());
    lines << QStringLiteral("%1 · %2").arg(config.kindLabel().toHtmlEscaped(), config.endpoint().toHtmlEscaped());
    lines << QStringLiteral("%1%2").arg(linkStateLabel(status.state).toHtmlEscaped(),
                                        status.detail.isEmpty() ? QString() : QStringLiteral(": ") + status.detail.toHtmlEscaped());
    if (status.state == LinkState::Waiting && status.nextAttempt.isValid())
        lines << QStringLiteral("Next try in %1").arg(formatDuration(qMax<qint64>(0, now.secsTo(status.nextAttempt))));
    if (status.connectMs >= 0)
        lines << QStringLiteral("Last connect took %1 ms").arg(status.connectMs);
    lines << QStringLiteral("Sessions %1 · invalid frames %2 · rejected announces %3")
                 .arg(status.sessions)
                 .arg(status.invalidFrames)
                 .arg(status.rejected);
    int heard = 0;
    for (const DestinationRecord &record : m_node->destinations()) {
        if (record.interfaceId == id)
            ++heard;
    }
    lines << QStringLiteral("%1 destinations best reached through it").arg(heard);
    m_interfaceDetail->setText(lines.join(QStringLiteral("<br>")));
    m_interfaceTraffic->setSamples(status.history);
}

void ReticulumFeature::updateDiscovered()
{
    const QByteArray selected = m_discoveredTree->selectedItems().isEmpty()
                                    ? QByteArray()
                                    : m_discoveredTree->selectedItems().first()->data(0, kHashRole).toByteArray();
    m_discoveredTree->setUpdatesEnabled(false);
    m_discoveredTree->clear();
    for (const DestinationRecord &record : m_node->destinations()) {
        if (!record.discovery)
            continue;
        const DiscoveredInterface &found = *record.discovery;
        const bool connectable = !found.reachableOn.isEmpty() && found.port > 0 &&
                                 (found.type == QStringLiteral("TCPServerInterface") ||
                                  found.type == QStringLiteral("BackboneInterface"));
        auto *item = new QTreeWidgetItem;
        item->setData(0, kHashRole, record.hash);
        item->setText(0, found.name.isEmpty() ? QStringLiteral("Unnamed") : found.name);
        item->setText(1, found.type);
        item->setText(2, connectable ? QStringLiteral("%1:%2").arg(found.reachableOn).arg(found.port)
                                     : found.frequency > 0 ? QStringLiteral("%1 MHz").arg(found.frequency / 1e6, 0, 'f', 3)
                                                           : QStringLiteral("Not reachable over IP"));
        item->setText(3, found.transport ? QStringLiteral("Yes") : QStringLiteral("No"));
        item->setText(4, QStringLiteral("%1 bits%2").arg(found.stampValue).arg(found.stampValid ? QString() : QStringLiteral(" (low)")));
        item->setText(5, found.hasLocation ? QStringLiteral("%1, %2").arg(found.latitude, 0, 'f', 3).arg(found.longitude, 0, 'f', 3)
                                           : QStringLiteral("—"));
        item->setText(6, formatAge(record.lastHeard));
        item->setDisabled(!connectable || !found.stampValid);
        if (!found.networkName.isEmpty())
            item->setToolTip(0, QStringLiteral("Needs the network name %1, which this connector does not support yet").arg(found.networkName));
        m_discoveredTree->addTopLevelItem(item);
        if (record.hash == selected)
            item->setSelected(true);
    }
    m_discoveredTree->setUpdatesEnabled(true);
    m_useDiscovered->setEnabled(!m_discoveredTree->selectedItems().isEmpty() &&
                                !m_discoveredTree->selectedItems().first()->isDisabled());
}

void ReticulumFeature::updateDestinations()
{
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (m_lastTableRefresh > 0 && nowMs - m_lastTableRefresh < 1500) {
        if (!m_viewTimer->isActive())
            m_viewTimer->start();
        return;
    }
    m_lastTableRefresh = nowMs;
    const QString filter = m_destinationFilter->text().trimmed();
    const int group = m_kindFilter->currentData().toInt();
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const auto &destinations = m_node->destinations();
    m_destinationTree->setUpdatesEnabled(false);
    m_destinationTree->setSortingEnabled(false);
    for (auto it = m_destinationItems.begin(); it != m_destinationItems.end();) {
        if (!destinations.contains(it.key())) {
            delete it.value();
            it = m_destinationItems.erase(it);
        } else {
            ++it;
        }
    }
    int shown = 0;
    for (const DestinationRecord &record : destinations) {
        QTreeWidgetItem *item = m_destinationItems.value(record.hash);
        if (!item) {
            item = new SortableItem;
            item->setData(0, kHashRole, record.hash);
            m_destinationTree->addTopLevelItem(item);
            m_destinationItems.insert(record.hash, item);
        }
        const bool visible = (group < 0 || int(mapGroup(record.kind)) == group) &&
                             (filter.isEmpty() || record.displayName.contains(filter, Qt::CaseInsensitive) ||
                              record.label.contains(filter, Qt::CaseInsensitive) ||
                              record.aspect.contains(filter, Qt::CaseInsensitive) ||
                              hexHash(record.hash).contains(filter, Qt::CaseInsensitive));
        item->setHidden(!visible);
        if (!visible)
            continue;
        ++shown;
        item->setText(0, record.displayName.isEmpty() ? QStringLiteral("—") : record.displayName);
        item->setText(1, record.label);
        item->setToolTip(1, record.aspect);
        item->setText(2, QString::number(record.hops));
        item->setData(2, kSortRole, record.hops);
        item->setText(3, interfaceName(record.interfaceId));
        item->setText(4, formatAge(record.lastHeard, now));
        item->setData(4, kSortRole, double(record.lastHeard.toMSecsSinceEpoch()));
        item->setText(5, QString::number(record.announces));
        item->setData(5, kSortRole, record.announces);
        item->setText(6, hexHash(record.hash));
    }
    m_destinationTree->setSortingEnabled(true);
    m_destinationTree->setUpdatesEnabled(true);
    m_destinationCount->setText(shown == destinations.size()
                                    ? QStringLiteral("%1 destinations").arg(destinations.size())
                                    : QStringLiteral("%1 of %2").arg(shown).arg(destinations.size()));
    const auto lookup = m_node->lookups().constFind(parseHash(m_lookupEdit->text()));
    if (lookup != m_node->lookups().cend()) {
        m_lookupResult->show();
        if (lookup->answered.isValid())
            m_lookupResult->setText(QStringLiteral("Path found: %1 via %2, answered in %3 ms.")
                                        .arg(hopsText(lookup->hops), interfaceName(lookup->interfaceId))
                                        .arg(lookup->started.msecsTo(lookup->answered)));
        else if (lookup->timedOut)
            m_lookupResult->setText(QStringLiteral("No path within %1 seconds. The destination may be offline, or no "
                                                   "transport node you reach knows it.")
                                        .arg(Node::kPathTimeoutSeconds));
        else
            m_lookupResult->setText(QStringLiteral("Asking the network…"));
    }
}

void ReticulumFeature::updateActivity()
{
    const QList<NetworkEvent> &events = m_node->events();
    m_activity->setUpdatesEnabled(false);
    m_activity->clear();
    for (const NetworkEvent &event : events) {
        auto *item = new QListWidgetItem(QStringLiteral("%1  %2").arg(
            event.at.toLocalTime().toString(QStringLiteral("HH:mm:ss")), event.text));
        item->setData(kHashRole, event.destination);
        m_activity->addItem(item);
    }
    m_activity->setUpdatesEnabled(true);
    const PacketCounters &counters = m_node->counters();
    QStringList rows;
    const auto row = [&rows](const QString &label, qint64 value) {
        rows.append(QStringLiteral("<tr><td style='padding-right:12px'>%1</td><td align='right'>%2</td></tr>")
                        .arg(label.toHtmlEscaped())
                        .arg(value));
    };
    row(QStringLiteral("Announces"), counters.announces);
    row(QStringLiteral("Rejected announces"), counters.rejectedAnnounces);
    row(QStringLiteral("Duplicates"), counters.duplicates);
    row(QStringLiteral("Data packets"), counters.data);
    row(QStringLiteral("Link requests"), counters.linkRequests);
    row(QStringLiteral("Proofs"), counters.proofs);
    row(QStringLiteral("Unreadable frames"), counters.invalid);
    row(QStringLiteral("Path requests heard"), counters.pathRequestsSeen);
    row(QStringLiteral("Path requests answered"), counters.pathRequestsAnswered);
    row(QStringLiteral("Announces sent"), counters.announcesSent);
    m_counters->setText(QStringLiteral("<table>%1</table>").arg(rows.join(QString())));
}

void ReticulumFeature::updateIdentity()
{
    m_destinationHash->setText(prettyHash(m_node->destinationHash()));
    m_identityHash->setText(prettyHash(m_node->identity().hash()));
    m_messagingAddress->setText(prettyHash(m_messenger->address()));
    m_lastAnnounce->setText(m_node->lastAnnounce().isValid() ? formatAge(m_node->lastAnnounce())
                                                             : QStringLiteral("Not announced since ForkMesh started"));
}

void ReticulumFeature::updateAnnounceTimer()
{
    const int minutes = m_announceInterval->currentData().toInt();
    if (minutes > 0)
        m_announceTimer->start(minutes * 60 * 1000);
    else
        m_announceTimer->stop();
}

void ReticulumFeature::showStatus(const QString &text, bool error)
{
    m_statusLine->setObjectName(error ? QStringLiteral("errorLabel") : QStringLiteral("statusLine"));
    m_statusLine->style()->unpolish(m_statusLine);
    m_statusLine->style()->polish(m_statusLine);
    m_statusLine->setText(text);
    m_statusLine->setVisible(!text.isEmpty());
}

bool ReticulumFeature::editInterface(InterfaceConfig *config, bool creating)
{
    QDialog dialog(this);
    dialog.setWindowTitle(creating ? QStringLiteral("Add a Reticulum interface") : QStringLiteral("Edit interface"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    auto *name = new QLineEdit(config->name);
    name->setMaxLength(80);
    form->addRow(QStringLiteral("Name"), name);
    auto *kind = new QComboBox;
    kind->addItem(QStringLiteral("TCP connection to a Reticulum node"), int(InterfaceKind::TcpClient));
    kind->addItem(QStringLiteral("Reticulum daemon on this computer"), int(InterfaceKind::SharedInstance));
    kind->addItem(QStringLiteral("UDP on the local network"), int(InterfaceKind::Udp));
    kind->setCurrentIndex(kind->findData(int(config->kind)));
    form->addRow(QStringLiteral("Type"), kind);
    layout->addLayout(form);

    auto *pages = new QStackedWidget;
    auto *tcpPage = new QWidget;
    auto *tcpForm = new QFormLayout(tcpPage);
    tcpForm->setContentsMargins(0, 0, 0, 0);
    auto *host = new QLineEdit(config->host);
    host->setPlaceholderText(QStringLiteral("rmap.world"));
    auto *port = new QSpinBox;
    port->setRange(1, 65535);
    port->setValue(config->kind == InterfaceKind::TcpClient ? config->port : 4242);
    auto *kiss = new QCheckBox(QStringLiteral("KISS framing (for a TNC or modem reached over TCP)"));
    kiss->setChecked(config->kiss);
    tcpForm->addRow(QStringLiteral("Host"), host);
    tcpForm->addRow(QStringLiteral("Port"), port);
    tcpForm->addRow(QString(), kiss);
    pages->addWidget(tcpPage);

    auto *sharedPage = new QWidget;
    auto *sharedForm = new QFormLayout(sharedPage);
    sharedForm->setContentsMargins(0, 0, 0, 0);
    auto *instance = new QLineEdit(config->instanceName);
    auto *overTcp = new QCheckBox(QStringLiteral("The daemon shares over TCP (shared_instance_type = tcp)"));
    overTcp->setChecked(config->sharedOverTcp);
    auto *sharedPort = new QSpinBox;
    sharedPort->setRange(1, 65535);
    sharedPort->setValue(config->kind == InterfaceKind::SharedInstance ? config->port : 37428);
    sharedForm->addRow(QStringLiteral("Instance name"), instance);
    sharedForm->addRow(QString(), overTcp);
    sharedForm->addRow(QStringLiteral("Port"), sharedPort);
    sharedForm->addRow(QString(), mutedLabel(QStringLiteral("Start the daemon with rnsd. ForkMesh then shares its "
                                                            "interfaces, radios included, and its paths.")));
    pages->addWidget(sharedPage);

    auto *udpPage = new QWidget;
    auto *udpForm = new QFormLayout(udpPage);
    udpForm->setContentsMargins(0, 0, 0, 0);
    auto *listenAddress = new QLineEdit(config->listenAddress);
    auto *listenPort = new QSpinBox;
    listenPort->setRange(1, 65535);
    listenPort->setValue(config->listenPort);
    auto *forwardAddress = new QLineEdit(config->forwardAddress);
    auto *forwardPort = new QSpinBox;
    forwardPort->setRange(1, 65535);
    forwardPort->setValue(config->forwardPort);
    udpForm->addRow(QStringLiteral("Listen on"), listenAddress);
    udpForm->addRow(QStringLiteral("Listen port"), listenPort);
    udpForm->addRow(QStringLiteral("Send to"), forwardAddress);
    udpForm->addRow(QStringLiteral("Send port"), forwardPort);
    pages->addWidget(udpPage);
    layout->addWidget(pages);

    auto *accessForm = new QFormLayout;
    auto *networkName = new QLineEdit(config->networkName);
    networkName->setPlaceholderText(QStringLiteral("Leave empty for an open network"));
    auto *passphrase = new QLineEdit(config->passphrase);
    passphrase->setEchoMode(QLineEdit::Password);
    passphrase->setPlaceholderText(QStringLiteral("Leave empty for an open network"));
    auto *accessSize = new QSpinBox;
    accessSize->setRange(1, 64);
    accessSize->setSuffix(QStringLiteral(" bytes"));
    accessSize->setValue(config->accessCodeSize);
    accessForm->addRow(QStringLiteral("Network name"), networkName);
    accessForm->addRow(QStringLiteral("Passphrase"), passphrase);
    accessForm->addRow(QStringLiteral("Access code size"), accessSize);
    accessForm->addRow(QString(), mutedLabel(QStringLiteral("A virtual network on a shared medium (IFAC): only nodes with "
                                                            "the same name and passphrase see each other's packets.")));
    layout->addLayout(accessForm);

    auto *error = new QLabel;
    error->setObjectName(QStringLiteral("errorLabel"));
    error->setWordWrap(true);
    error->hide();
    layout->addWidget(error);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    const auto syncPage = [kind, pages, sharedPort, overTcp] {
        pages->setCurrentIndex(kind->currentIndex());
        sharedPort->setEnabled(overTcp->isChecked());
    };
    connect(kind, &QComboBox::currentIndexChanged, &dialog, syncPage);
    connect(overTcp, &QCheckBox::toggled, &dialog, syncPage);
    syncPage();
    InterfaceConfig result = *config;
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        result.name = name->text().trimmed();
        result.kind = InterfaceKind(kind->currentData().toInt());
        result.host = host->text().trimmed();
        result.kiss = kiss->isChecked();
        result.instanceName = instance->text().trimmed();
        result.sharedOverTcp = overTcp->isChecked();
        result.port = result.kind == InterfaceKind::SharedInstance ? sharedPort->value() : port->value();
        result.listenAddress = listenAddress->text().trimmed();
        result.listenPort = listenPort->value();
        result.forwardAddress = forwardAddress->text().trimmed();
        result.forwardPort = forwardPort->value();
        result.networkName = networkName->text().trimmed();
        result.passphrase = passphrase->text();
        result.accessCodeSize = accessSize->value();
        const QString problem = result.validationError();
        if (!problem.isEmpty()) {
            error->setText(problem);
            error->show();
            return;
        }
        dialog.accept();
    });
    if (dialog.exec() != QDialog::Accepted)
        return false;
    *config = result;
    return true;
}

QString ReticulumFeature::selectedInterfaceId() const
{
    const QList<QTreeWidgetItem *> items = m_interfaceTree->selectedItems();
    return items.isEmpty() ? QString() : items.first()->data(0, kHashRole).toString();
}

QString ReticulumFeature::interfaceName(const QString &id) const
{
    for (const InterfaceConfig &config : m_node->interfaces()) {
        if (config.id == id)
            return config.name;
    }
    return id.isEmpty() ? QStringLiteral("—") : QStringLiteral("Removed interface");
}

void ReticulumFeature::addInterface(const InterfaceConfig &config)
{
    for (const InterfaceConfig &existing : m_node->interfaces()) {
        if (existing.sameTarget(config)) {
            showStatus(QStringLiteral("%1 is already in your interfaces as %2.").arg(config.endpoint(), existing.name));
            if (!existing.enabled)
                m_node->setInterfaceEnabled(existing.id, true);
            saveInterfaces();
            return;
        }
    }
    m_node->upsertInterface(config);
    if (!m_node->running())
        m_node->start();
    saveInterfaces();
    showStatus(QStringLiteral("Added %1 (%2).").arg(config.name, config.endpoint()));
    updateViewsNow();
}

void ReticulumFeature::joinPublicNetwork()
{
    const QList<InterfaceConfig> entries = publicEntryPoints();
    if (entries.isEmpty())
        return;
    InterfaceConfig entry = entries.first();
    entry.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    addInterface(entry);
    if (m_host.setSetting)
        m_host.setSetting(kRunningSetting, true);
    m_tabs->setCurrentIndex(NetworkTab);
}

QString ReticulumFeature::importReticulumConfig(const QString &path)
{
    QFile file(path);
    if (file.size() > 1024 * 1024 || !file.open(QIODevice::ReadOnly))
        return QStringLiteral("Could not read %1.").arg(path);
    QStringList skipped;
    const QList<InterfaceConfig> found = parseReticulumConfig(QString::fromUtf8(file.readAll()), &skipped);
    int added = 0;
    for (const InterfaceConfig &config : found) {
        bool duplicate = false;
        for (const InterfaceConfig &existing : m_node->interfaces())
            duplicate = duplicate || existing.sameTarget(config);
        if (duplicate)
            continue;
        m_node->upsertInterface(config);
        ++added;
    }
    saveInterfaces();
    updateViewsNow();
    QString text = QStringLiteral("Imported %1 interface%2 from %3.").arg(added).arg(added == 1 ? QString() : QStringLiteral("s")).arg(path);
    if (!skipped.isEmpty())
        text += QStringLiteral(" Skipped (not supported here, or protected by a network name): %1.").arg(skipped.join(QStringLiteral(", ")));
    return text;
}

void ReticulumFeature::announceNow()
{
    m_node->setAnnounceName(m_announceName->text());
    m_messenger->setDisplayName(m_announceName->text());
    if (m_host.setSetting)
        m_host.setSetting(kAnnounceNameSetting, m_node->announceName());
    if (m_node->announce())
        showStatus(QStringLiteral("Announced %1 on %2 interface%3.")
                       .arg(prettyHash(m_node->destinationHash()))
                       .arg(m_node->onlineCount())
                       .arg(m_node->onlineCount() == 1 ? QString() : QStringLiteral("s")));
    else
        showStatus(QStringLiteral("Connect an interface first."), true);
    updateViewsNow();
}

void ReticulumFeature::requestPathFor(const QByteArray &hash)
{
    if (hash.size() != kHashLength)
        return;
    if (m_node->requestPath(hash).isEmpty()) {
        showStatus(QStringLiteral("Connect an interface first."), true);
        return;
    }
    m_lookupEdit->setText(hexHash(hash));
    m_lookupResult->setText(QStringLiteral("Asking the network…"));
    m_lookupResult->show();
    m_lastTableRefresh = 0;
    updateViewsNow();
}

void ReticulumFeature::copyText(const QString &text, const QString &what)
{
    if (text.isEmpty())
        return;
    QApplication::clipboard()->setText(text);
    if (m_host.flashMessage)
        m_host.flashMessage(QStringLiteral("%1 copied").arg(what), false);
    else
        showStatus(QStringLiteral("%1 copied.").arg(what));
}

void ReticulumFeature::selectDestination(const QByteArray &hash)
{
    m_selected = hash;
    if (m_map)
        m_map->setSelected(hash);
    if (QTreeWidgetItem *item = m_destinationItems.value(hash)) {
        m_destinationTree->blockSignals(true);
        m_destinationTree->setCurrentItem(item);
        m_destinationTree->blockSignals(false);
    }
    updateDetail();
}

void ReticulumFeature::setHostSearchQuery(const QString &query)
{
    m_query = query;
    m_destinationFilter->setText(query);
    if (m_map)
        m_map->setFilter(query);
}

QList<forkmesh::market::NativePackageSearchHit> ReticulumFeature::searchHits(const QString &query, int limit) const
{
    QList<forkmesh::market::NativePackageSearchHit> hits;
    const QString needle = query.trimmed();
    if (needle.size() < 2)
        return hits;
    for (const InterfaceConfig &config : m_node->interfaces()) {
        if (hits.size() >= limit)
            return hits;
        if (config.name.contains(needle, Qt::CaseInsensitive) || config.endpoint().contains(needle, Qt::CaseInsensitive))
            hits.append({QStringLiteral("interface:") + config.id, config.name,
                         QStringLiteral("Reticulum interface · %1").arg(config.endpoint())});
    }
    for (const DestinationRecord &record : m_node->destinations()) {
        if (hits.size() >= limit)
            break;
        if (record.displayName.contains(needle, Qt::CaseInsensitive) || hexHash(record.hash).startsWith(needle.toLower()))
            hits.append({QStringLiteral("destination:") + hexHash(record.hash),
                         record.displayName.isEmpty() ? prettyHash(record.hash) : record.displayName,
                         QStringLiteral("%1 · %2").arg(record.label, hopsText(record.hops))});
    }
    return hits;
}

void ReticulumFeature::activateSearchHit(const QString &id)
{
    if (id.startsWith(QStringLiteral("interface:"))) {
        m_tabs->setCurrentIndex(InterfacesTab);
        const QString interfaceId = id.mid(10);
        for (int i = 0; i < m_interfaceTree->topLevelItemCount(); ++i) {
            QTreeWidgetItem *item = m_interfaceTree->topLevelItem(i);
            if (item->data(0, kHashRole).toString() == interfaceId)
                m_interfaceTree->setCurrentItem(item);
        }
    } else if (id.startsWith(QStringLiteral("destination:"))) {
        m_tabs->setCurrentIndex(NetworkTab);
        selectDestination(QByteArray::fromHex(id.mid(12).toLatin1()));
    }
}

}

namespace {
const forkmesh::market::NativePackageRegistration reticulumRegistration({
    QStringLiteral("builtin:reticulum"),
    {QStringLiteral("extension.native")},
    QStringLiteral("Reticulum"),
    QStringLiteral("pulse"),
    QString(),
    QString(),
    [](const forkmesh::market::NativePackageHost &host, QWidget *parent) {
        return new forkmesh::reticulum::ReticulumFeature(host, parent);
    },
});
}
