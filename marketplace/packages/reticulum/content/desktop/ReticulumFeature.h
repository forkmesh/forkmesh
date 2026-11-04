#pragma once

#include "NativePackageExtension.h"
#include "ReticulumGraphics.h"
#include "ReticulumMessenger.h"
#include "ReticulumNode.h"

#include <QHash>
#include <QPointer>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QSpinBox;
class QPushButton;
class QTabWidget;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;
class QWidget;

namespace forkmesh::reticulum {

class ReticulumFeature final : public forkmesh::market::NativePackageWidget
{
public:
    enum Tab { NetworkTab, MessagesTab, InterfacesTab, DestinationsTab, ActivityTab, IdentityTab };

    explicit ReticulumFeature(const forkmesh::market::NativePackageHost &host, QWidget *parent = nullptr);
    ~ReticulumFeature() override;

    void refresh() override;
    void prepareSearch() override {}
    void setHostSearchQuery(const QString &query) override;
    QList<forkmesh::market::NativePackageSearchHit> searchHits(const QString &query, int limit) const override;
    void activateSearchHit(const QString &id) override;
    void themeChanged() override;
    int navigationBadge() const override;
    forkmesh::market::NativePackagePromptTarget hostPromptTarget() const override;
    bool submitHostPrompt(const QString &text, const QStringList &attachments) override;
    void prepareNavigationBadge() override {}

    Node *node() const { return m_node; }
    Messenger *messenger() const { return m_messenger; }
    QListWidget *conversationList() const { return m_conversationList; }
    QListWidget *chatList() const { return m_chatList; }
    void openConversation(const QByteArray &peer);
    QString sendMessage(const QString &text);
    QTabWidget *tabs() const { return m_tabs; }
    QTreeWidget *interfaceTree() const { return m_interfaceTree; }
    QTreeWidget *destinationTree() const { return m_destinationTree; }
    NetworkMap *networkMap() const { return m_map; }
    StatTile *tile(int index) const { return m_tiles.value(index); }
    QByteArray selectedDestination() const { return m_selected; }
    void selectDestination(const QByteArray &hash);
    void addInterface(const InterfaceConfig &config);
    void joinPublicNetwork();
    QString importReticulumConfig(const QString &path);
    void updateViewsNow();

protected:
    void showEvent(QShowEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QWidget *buildNetworkTab();
    QWidget *buildMessagesTab();
    void updateMessages();
    void updateChat();
    void updateComposerHint();
    void startConversation();
    QWidget *messageBubble(const ChatMessage &message, int width);
    QWidget *buildInterfacesTab();
    QWidget *buildDestinationsTab();
    QWidget *buildActivityTab();
    QWidget *buildIdentityTab();

    void loadSettings();
    void saveInterfaces();
    void ensureIdentity();
    void saveIdentity();
    void scheduleViews();
    void updateHeader();
    void updateTiles();
    void updateMap();
    void updateInterfaces();
    void updateDiscovered();
    void updateDestinations();
    void updateDetail();
    void updateActivity();
    void updateIdentity();
    void updateAnnounceTimer();
    void showStatus(const QString &text, bool error = false);
    bool editInterface(InterfaceConfig *config, bool creating);
    QString selectedInterfaceId() const;
    void announceNow();
    void requestPathFor(const QByteArray &hash);
    void copyText(const QString &text, const QString &what);
    QString interfaceName(const QString &id) const;

    forkmesh::market::NativePackageHost m_host;
    Node *m_node = nullptr;
    Messenger *m_messenger = nullptr;
    QByteArray m_currentPeer;
    QListWidget *m_conversationList = nullptr;
    QLabel *m_chatTitle = nullptr;
    QLabel *m_chatSubtitle = nullptr;
    QListWidget *m_chatList = nullptr;
    QPlainTextEdit *m_composer = nullptr;
    QPushButton *m_sendButton = nullptr;
    QLabel *m_composerHint = nullptr;
    QPushButton *m_chatPathButton = nullptr;
    QWidget *m_chatPane = nullptr;
    QLabel *m_chatEmpty = nullptr;
    QPushButton *m_detailMessage = nullptr;
    QLabel *m_messagingAddress = nullptr;
    bool m_chatDirty = true;
    bool m_started = false;
    QByteArray m_selected;
    QString m_query;
    int m_previousOnline = 0;

    QLabel *m_statusLine = nullptr;
    QLabel *m_summary = nullptr;
    QPushButton *m_announceButton = nullptr;
    QPushButton *m_powerButton = nullptr;
    QWidget *m_onboarding = nullptr;
    QList<StatTile *> m_tiles;
    QTabWidget *m_tabs = nullptr;

    NetworkMap *m_map = nullptr;
    TrafficChart *m_traffic = nullptr;
    QLabel *m_detailTitle = nullptr;
    QLabel *m_detailBody = nullptr;
    QPushButton *m_detailPath = nullptr;
    QPushButton *m_detailCopy = nullptr;
    QPushButton *m_detailForget = nullptr;
    QLineEdit *m_mapFilter = nullptr;

    QTreeWidget *m_interfaceTree = nullptr;
    TrafficChart *m_interfaceTraffic = nullptr;
    QLabel *m_interfaceDetail = nullptr;
    QPushButton *m_editButton = nullptr;
    QPushButton *m_removeButton = nullptr;
    QPushButton *m_toggleButton = nullptr;
    QPushButton *m_reconnectButton = nullptr;
    QTreeWidget *m_discoveredTree = nullptr;
    QPushButton *m_useDiscovered = nullptr;

    QLineEdit *m_destinationFilter = nullptr;
    QComboBox *m_kindFilter = nullptr;
    QTreeWidget *m_destinationTree = nullptr;
    QHash<QByteArray, QTreeWidgetItem *> m_destinationItems;
    QLineEdit *m_lookupEdit = nullptr;
    QLabel *m_lookupResult = nullptr;
    QLabel *m_destinationCount = nullptr;

    QListWidget *m_activity = nullptr;
    QLabel *m_counters = nullptr;

    QLabel *m_identityHash = nullptr;
    QLabel *m_destinationHash = nullptr;
    QLineEdit *m_announceName = nullptr;
    QComboBox *m_announceInterval = nullptr;
    QCheckBox *m_answerPaths = nullptr;
    QLabel *m_lastAnnounce = nullptr;

    QTimer *m_viewTimer = nullptr;
    QTimer *m_announceTimer = nullptr;
    qint64 m_lastTableRefresh = 0;
};

}
