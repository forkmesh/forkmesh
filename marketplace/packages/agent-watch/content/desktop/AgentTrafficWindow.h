#pragma once

#include "AgentActivityModel.h"

#include <QHash>
#include <QString>
#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

class AgentTrafficWindow : public QWidget
{
public:
    enum ConnectionColumn {
        ConnectionProcess,
        ConnectionAgent,
        ConnectionRemote,
        ConnectionPort,
        ConnectionReceived,
        ConnectionSent,
        ConnectionDownRate,
        ConnectionUpRate,
        ConnectionLast,
        ConnectionColumns
    };
    enum TrafficColumn {
        TrafficTime,
        TrafficDirection,
        TrafficProcess,
        TrafficAgent,
        TrafficRemote,
        TrafficPort,
        TrafficBytes,
        TrafficColumns
    };
    static constexpr int kMaxRows = 2000;
    static constexpr int kRefreshMs = 250;

    AgentTrafficWindow(forkmesh::agentvis::Scene *scene, int sessionId, const QString &host,
                       QWidget *parent = nullptr);

    static QString keyFor(int sessionId, const QString &host);
    QString key() const { return keyFor(m_sessionId, m_host); }
    int sessionId() const { return m_sessionId; }
    QString host() const { return m_host; }

    void refresh();
    void setPaused(bool paused);
    bool paused() const { return m_paused; }
    void setFilter(const QString &text);
    void clearTraffic();

    QTreeWidget *connectionList() const { return m_connections; }
    QTreeWidget *trafficList() const { return m_traffic; }
    QString heading() const;
    QString summary() const;

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    QString sessionName(int sessionId) const;
    QString remoteText(const QString &host, const QString &address) const;
    void refreshConnections();
    void appendTraffic();
    void applyFilter();
    bool matches(const QTreeWidgetItem *item, int columns) const;

    forkmesh::agentvis::Scene *m_scene = nullptr;
    int m_sessionId = forkmesh::agentvis::Scene::kAnySession;
    QString m_host;
    quint64 m_after = 0;
    bool m_paused = false;
    QString m_filter;
    QString m_selected;
    QHash<QString, QTreeWidgetItem *> m_connectionItems;
    QLabel *m_heading = nullptr;
    QLabel *m_summary = nullptr;
    QLabel *m_details = nullptr;
    QLineEdit *m_filterEdit = nullptr;
    QPushButton *m_pauseButton = nullptr;
    QTreeWidget *m_connections = nullptr;
    QTreeWidget *m_traffic = nullptr;
    QTimer *m_timer = nullptr;
};
