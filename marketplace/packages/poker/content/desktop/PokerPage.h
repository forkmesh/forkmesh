#pragma once

#include "NativePackageExtension.h"
#include "PokerModel.h"

#include <QJsonObject>
#include <QString>

#include <functional>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSlider;
class QSpinBox;
class QStackedWidget;
class QTabWidget;
class QTableWidget;
class QTextBrowser;
class QTimer;
class QToolButton;

namespace forkmesh::poker {

class PokerSound;
class PokerTableView;

class PokerPage final : public forkmesh::market::NativePackageWidget
{
public:
    using ReplyHandler = std::function<void(bool, const QJsonObject &, const QString &)>;
    using Request = std::function<void(const QByteArray &, const QString &, const QJsonObject &, ReplyHandler)>;

    explicit PokerPage(const forkmesh::market::NativePackageHost &host, QWidget *parent = nullptr);
    ~PokerPage() override;

    void refresh() override;
    QList<forkmesh::market::NativePackageSearchHit> searchHits(const QString &query, int limit) const override;
    void activateSearchHit(const QString &id) override;
    bool exportData(forkmesh::market::NativePackageReplyHandler done) override;
    void packageChanged() override;
    int navigationBadge() const override;
    void accountPicturesChanged() override;

    static QString apiPath(const QString &suffix = QString());

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void buildLobby();
    void buildTable();
    void arrange(bool controlTower);
    void placeOverlays();
    void setDrawerOpen(bool open);
    void request(const QByteArray &method, const QString &path, const QJsonObject &body, ReplyHandler handler);
    void loadLobby();
    void renderLobby();
    void refreshClaimLabel();
    void claimChips();
    void createTable();
    void joinByCode();
    void openTable(const QString &id);
    void showLobby();
    void loadTable();
    void applyTable(const QJsonObject &payload);
    void tableFailed(const QJsonObject &payload, const QString &error);
    void renderTable();
    void renderActions();
    void renderChat();
    void renderLog();
    void renderLastHand();
    void armWake();
    void sendTick();
    void act(const QString &action, qint64 amount = 0);
    void foldHand();
    void checkOrCall();
    void betOrRaise();
    void allIn();
    void setRaiseAmount(qint64 amount);
    void sitDown(int seat);
    void setAway(bool away);
    void rebuy();
    void leaveTable();
    void addBot();
    void removeBot(int seat);
    void inviteFriends();
    void sendChat();
    void closeTable();
    void tableWrite(const QString &suffix, const QJsonObject &body, const QString &failure,
                    std::function<void(const QJsonObject &)> done = {});
    void setStatus(const QString &text, bool error = false);
    void setTableStatus(const QString &text, bool error = false);
    void setMuted(bool muted);
    void updateBadge();

    forkmesh::market::NativePackageHost m_host;
    Request m_request;
    std::function<void()> m_searchIndexChanged;
    PokerSound *m_sound = nullptr;

    QStackedWidget *m_stack = nullptr;
    QWidget *m_lobbyPage = nullptr;
    QWidget *m_tablePage = nullptr;
    QLabel *m_status = nullptr;
    QToolButton *m_mute = nullptr;
    QLabel *m_chips = nullptr;
    QLabel *m_onTables = nullptr;
    QPushButton *m_claim = nullptr;
    QLabel *m_claimNote = nullptr;
    QLabel *m_stats = nullptr;
    QTableWidget *m_tables = nullptr;
    QLabel *m_tablesEmpty = nullptr;
    QPushButton *m_create = nullptr;
    QLineEdit *m_code = nullptr;
    QPushButton *m_join = nullptr;
    QTimer *m_claimTimer = nullptr;

    QLabel *m_tableTitle = nullptr;
    QLabel *m_tableInfo = nullptr;
    QLabel *m_tableStatus = nullptr;
    QLabel *m_wallet = nullptr;
    QPushButton *m_back = nullptr;
    QPushButton *m_sit = nullptr;
    QPushButton *m_away = nullptr;
    QPushButton *m_rebuy = nullptr;
    QPushButton *m_leave = nullptr;
    QPushButton *m_bots = nullptr;
    QPushButton *m_invite = nullptr;
    QPushButton *m_more = nullptr;
    QPushButton *m_drawerButton = nullptr;
    QWidget *m_stage = nullptr;
    QWidget *m_infoCard = nullptr;
    QWidget *m_optionsCard = nullptr;
    QLabel *m_dealerLine = nullptr;
    PokerTableView *m_view = nullptr;
    QWidget *m_actionBar = nullptr;
    QLabel *m_actionNote = nullptr;
    QPushButton *m_fold = nullptr;
    QPushButton *m_call = nullptr;
    QPushButton *m_raise = nullptr;
    QPushButton *m_half = nullptr;
    QPushButton *m_threeQuarters = nullptr;
    QPushButton *m_potButton = nullptr;
    QPushButton *m_maxButton = nullptr;
    QPushButton *m_raiseDown = nullptr;
    QPushButton *m_raiseUp = nullptr;
    QSlider *m_slider = nullptr;
    QSpinBox *m_amount = nullptr;
    QWidget *m_side = nullptr;
    QTabWidget *m_sideTabs = nullptr;
    QListWidget *m_chat = nullptr;
    QLineEdit *m_chatInput = nullptr;
    QPushButton *m_chatSend = nullptr;
    QListWidget *m_log = nullptr;
    QTextBrowser *m_lastHand = nullptr;
    QTimer *m_wake = nullptr;

    PokerLobby m_lobby;
    qint64 m_lobbyOffset = 0;
    PokerTable m_table;
    QString m_openTableId;
    qint64 m_lastSeq = -1;
    int m_chatSeen = 0;
    int m_unread = 0;
    bool m_lobbyLoading = false;
    bool m_lobbyAgain = false;
    bool m_lobbyDirty = false;
    bool m_tableLoading = false;
    bool m_tableAgain = false;
    bool m_acting = false;
    bool m_wasMyTurn = false;
    bool m_controlTower = false;
    bool m_arranged = false;
    bool m_drawerOpen = false;
    bool m_placing = false;
    bool m_syncingAmount = false;
    int m_badge = 0;
};

}
