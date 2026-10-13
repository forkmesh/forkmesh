#pragma once

#include "AgentActivityFeed.h"
#include "NativePackageExtension.h"

#include <QHash>
#include <QJsonArray>
#include <QList>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

class AgentActivityView;
class AgentTrafficWindow;
class QPushButton;
class QTimer;
class QToolButton;
class QVBoxLayout;

namespace forkmesh::agentnet {
class Ledger;
}

namespace forkmesh::agentvis {
class Scene;
struct BlockRule;
}

namespace forkmesh::agentwatch {

class AgentWatchFeature final : public forkmesh::market::NativePackageWidget,
                                public forkmesh::agentfeed::Listener
{
public:
    explicit AgentWatchFeature(const forkmesh::market::NativePackageHost &host,
                               QWidget *parent = nullptr);
    ~AgentWatchFeature() override;

    void refresh() override;
    void prepareSearch() override {}
    QList<forkmesh::market::NativePackageSearchHit>
    searchHits(const QString &query, int limit) const override;
    void activateSearchHit(const QString &id) override;

    void agentEvent(int sessionId, const QJsonObject &event) override;
    void noteScheduled(int sessionId, forkmesh::sonify::Motion motion, double delaySeconds,
                       double gain, quint32 seed) override;
    void logLine(const QString &storedLine) override;
    void backgroundTask(const forkmesh::agentfeed::TaskEvent &task) override;
    void relayRosterChanged() override;
    void relayFrame(bool outgoing, const QString &type, const QString &peerId,
                    qint64 bytes) override;
    void webEvent(const forkmesh::control::CloudflareTailEvent &event) override;
    void replySpeechChanged(bool on) override;

    forkmesh::agentvis::Scene *scene() const { return m_scene.get(); }
    AgentActivityView *view() const { return m_view; }
    bool watching() const;

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void buildPage();
    void updateWatching();
    void beginWatching();
    void endWatching();
    void syncRoster();
    void syncRelaySocket();
    void refreshState(int sessionId);
    void noteLog(const QString &storedLine, double ageSeconds);
    void seedLogs();
    void seedTasks();
    void noteClients(bool announce);
    void noteCue(int sessionId, forkmesh::sonify::Motion motion, const QString &text,
                 quint32 seed);
    void setHearAll(bool on);
    void setFullScreen(bool on);
    void applyMutedKinds();

    void sampleNetwork();
    QString hostName(const QString &peer);
    void lookupWhois(const QString &peer);
    void seedNetworkNames();
    void refreshMirrors();
    void openTraffic(int sessionId, const QString &host);
    int trafficWindows() const;

    QJsonArray visitorBlocksStored() const;
    void refreshVisitorBlockButton();
    void resolveVisitorZone(const QString &token,
                            std::function<void(QString, QString)> onDone);
    void blockVisitors(const QList<forkmesh::agentvis::BlockRule> &rules);
    void unblockVisitor(const QJsonObject &stored);
    void offerVisitorBlock(const QString &clientId, const QPoint &globalPos);
    void openVisitorBlocks();

    std::unique_ptr<forkmesh::agentvis::Scene> m_scene;
    std::unique_ptr<forkmesh::agentnet::Ledger> m_ledger;
    AgentActivityView *m_view = nullptr;
    QVBoxLayout *m_layout = nullptr;
    QPointer<QWidget> m_fullWindow;
    QList<QPointer<AgentTrafficWindow>> m_trafficWindows;
    QPushButton *m_fullScreenButton = nullptr;
    QPushButton *m_hearAllButton = nullptr;
    QPushButton *m_speakButton = nullptr;
    QPushButton *m_visitorsButton = nullptr;
    QToolButton *m_kindsButton = nullptr;
    QTimer *m_rosterTimer = nullptr;
    QTimer *m_networkTimer = nullptr;
    bool m_pageShown = false;
    bool m_watching = false;
    bool m_hearAll = false;
    bool m_speechRestored = false;
    bool m_sampleInFlight = false;
    qint64 m_socketCueMs = 0;
    qint64 m_mirrorsReadMs = 0;
    QHash<QString, QString> m_networkNames;
    QSet<QString> m_networkLookups;
    QHash<QString, QStringList> m_mirrorResolved;
    QSet<QString> m_mirrorLookups;
    QString m_visitorZoneId;
    QString m_visitorZoneToken;
    QString m_siteHost;
    std::function<void()> m_searchIndexChanged;
};

} // namespace forkmesh::agentwatch
