#pragma once

#include "LocalMeshNode.h"

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>

class CoreClient;
class QTimer;

class CoreMesh : public QObject, public LocalMeshBackend
{
public:
    static void attach(LocalMeshNode *node, CoreClient *client);

    CoreMesh(LocalMeshNode *node, CoreClient *client);
    ~CoreMesh() override;

    bool start() override;
    bool stop() override;
    bool routed() const override;
    QList<MemberInfo> members() const override;
    int peerCount() const override;
    quint16 listenPort() const override;
    QStringList wakeTargets() const override;
    QString diagnosticsLine() override;
    QString diagnosticsDetail() override;
    QString sendChat(const QString &text) override;
    QString sendFile(const QString &fileName, const QString &mimeType,
                     const QByteArray &data) override;
    void sendTyping(bool active) override;
    bool connectToPeer(const QHostAddress &address, quint16 port) override;
    void announceNow() override;
    void identityChanged() override;
    void controlChanged() override;
    bool attachBluetooth(QIODevice *socket, bool outbound) override;
    bool dropBluetooth(QIODevice *socket, const QString &reason) override;
    void setBluetoothKey(QIODevice *socket, const QString &key) override;
    bool hasBluetoothKey(const QString &key) const override;

private:
    enum class Mode { Idle, Waiting, Core, Native };

    struct Bridge {
        QPointer<QIODevice> socket;
        QString key;
    };

    bool coreReady() const;
    void beginCore();
    void onStarted(bool ok, const QJsonObject &result, const QString &error);
    void fallBackToNative();
    void leaveCore();
    void onConnectedChanged(bool connected);
    void onCoreEvent(const QString &event, const QJsonObject &data);
    void applyState(const QJsonObject &state);
    void runWhenReady(std::function<void()> core, std::function<void()> native);
    void flushPending();
    void notify(const QString &method, const QJsonObject &params);
    void closeBridges(bool tellCore);
    int bridgeFor(const QIODevice *socket) const;
    void forgetBridge(int id);
    void refreshDiagnostics();
    QJsonObject startParams() const;
    QJsonObject bluetoothInfo() const;

    LocalMeshNode *m_node = nullptr;
    QPointer<CoreClient> m_client;
    Mode m_mode = Mode::Idle;
    bool m_wanted = false;
    bool m_starting = false;
    QTimer *m_grace = nullptr;
    QTimer *m_bluetoothBeat = nullptr;
    QList<std::pair<std::function<void()>, std::function<void()>>> m_pending;
    QList<MemberInfo> m_members;
    int m_peerCount = 0;
    quint16 m_listenPort = 0;
    QStringList m_wakeTargets;
    QHash<int, Bridge> m_bridges;
    int m_nextBridge = 0;
    QString m_diagLine;
    QString m_diagDetail;
    qint64 m_diagAtMs = 0;
    bool m_diagInFlight = false;
};
