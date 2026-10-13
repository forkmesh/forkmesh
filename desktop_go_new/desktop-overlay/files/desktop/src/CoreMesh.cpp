#include "CoreMesh.h"

#include "CoreClient.h"

#include <QDateTime>
#include <QHostAddress>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonValue>
#include <QSettings>
#include <QTimer>
#include <QUuid>

#include <utility>

namespace {

constexpr int kCoreWaitMs = 4000;
constexpr int kReconnectGraceMs = 8000;
constexpr int kStartTimeoutMs = 15000;
constexpr int kBluetoothBeatMs = 15000;
constexpr qint64 kBluetoothQuietMs = 600000;
constexpr int kBluetoothRedialMs = 5000;
constexpr qint64 kDiagnosticsStaleMs = 1000;
constexpr int kMaxTextChars = 16000;
constexpr qsizetype kMaxFileBytes = 16ll * 1024 * 1024;

const QString &startMethod()
{
    static const QString value = QStringLiteral("core.mesh.start");
    return value;
}

const QString &sealKeySetting()
{
    static const QString value = QStringLiteral("chat/e2ee/x25519PrivateV2");
    return value;
}

MemberInfo memberFrom(const QJsonObject &object)
{
    MemberInfo member;
    member.id = object.value(QStringLiteral("id")).toString();
    member.name = object.value(QStringLiteral("name")).toString();
    member.nodeName = object.value(QStringLiteral("nodeName")).toString();
    member.ownerUser = object.value(QStringLiteral("ownerUser")).toString();
    member.accountKind = object.value(QStringLiteral("accountKind")).toString();
    member.platform = object.value(QStringLiteral("platform")).toString();
    member.version = object.value(QStringLiteral("version")).toString();
    member.note = object.value(QStringLiteral("note")).toString();
    member.self = object.value(QStringLiteral("self")).toBool();
    member.online = object.value(QStringLiteral("online")).toBool();
    return member;
}

ChatMessage messageFrom(const QJsonObject &object)
{
    ChatMessage message;
    message.id = object.value(QStringLiteral("id")).toString();
    message.conversation = object.value(QStringLiteral("conversation")).toString();
    message.senderId = object.value(QStringLiteral("senderId")).toString();
    message.senderName = object.value(QStringLiteral("senderName")).toString();
    message.text = object.value(QStringLiteral("text")).toString();
    message.timestampMs = object.value(QStringLiteral("timestampMs")).toInteger();
    message.fileName = object.value(QStringLiteral("fileName")).toString();
    message.fileMime = object.value(QStringLiteral("fileMime")).toString();
    message.fileData =
        QByteArray::fromBase64(object.value(QStringLiteral("file")).toString().toLatin1());
    if (message.fileData.isEmpty())
        message.fileName.clear();
    message.self = object.value(QStringLiteral("self")).toBool();
    message.historical = object.value(QStringLiteral("historical")).toBool();
    return message;
}

QStringList stringsFrom(const QJsonValue &value)
{
    QStringList out;
    const QJsonArray array = value.toArray();
    for (const QJsonValue &item : array)
        out.append(item.toString());
    return out;
}

} // namespace

void CoreMesh::attach(LocalMeshNode *node, CoreClient *client)
{
    if (node && client)
        node->setBackend(new CoreMesh(node, client));
}

CoreMesh::CoreMesh(LocalMeshNode *node, CoreClient *client)
    : m_node(node), m_client(client)
{
    m_grace = new QTimer(this);
    m_grace->setSingleShot(true);
    connect(m_grace, &QTimer::timeout, this, [this] {
        if (m_wanted && m_mode == Mode::Waiting && !m_starting)
            fallBackToNative();
    });
    m_bluetoothBeat = new QTimer(this);
    m_bluetoothBeat->setInterval(kBluetoothBeatMs);
    connect(m_bluetoothBeat, &QTimer::timeout, this, [this] {
        if (m_mode == Mode::Core && !m_node->bluetoothActive())
            m_node->openBluetooth();
    });
    connect(client, &CoreClient::connectedChanged, this, &CoreMesh::onConnectedChanged);
    connect(client, &CoreClient::coreEvent, this, &CoreMesh::onCoreEvent);
}

CoreMesh::~CoreMesh()
{
    closeBridges(false);
}

bool CoreMesh::coreReady() const
{
    return m_client && m_client->isConnected() && m_client->hasNativeRoute(startMethod());
}

bool CoreMesh::routed() const
{
    return m_mode == Mode::Waiting || m_mode == Mode::Core;
}

bool CoreMesh::start()
{
    if (!m_client)
        return false;
    m_wanted = true;
    if (coreReady()) {
        beginCore();
        return true;
    }
    if (m_client->isConnected()) {
        m_mode = Mode::Native;
        return false;
    }
    m_mode = Mode::Waiting;
    m_node->m_running = true;
    m_grace->start(kCoreWaitMs);
    return true;
}

bool CoreMesh::stop()
{
    const bool routedNow = routed();
    m_wanted = false;
    m_grace->stop();
    m_pending.clear();
    if (!routedNow) {
        m_mode = Mode::Idle;
        return false;
    }
    leaveCore();
    m_mode = Mode::Idle;
    m_node->m_running = false;
    return true;
}

QJsonObject CoreMesh::startParams() const
{
    return {{QStringLiteral("nodeId"), m_node->m_nodeId},
            {QStringLiteral("userName"), m_node->m_userName},
            {QStringLiteral("nodeName"), m_node->m_nodeName},
            {QStringLiteral("ownerUser"), m_node->m_ownerUser},
            {QStringLiteral("accountKind"), m_node->m_accountKind},
            {QStringLiteral("version"), m_node->m_version},
            {QStringLiteral("platform"), m_node->m_platform},
            {QStringLiteral("sealKey"),
             QString::fromLatin1(QSettings().value(sealKeySetting()).toByteArray())},
            {QStringLiteral("historyFile"), m_node->m_historyPath},
            {QStringLiteral("control"), bool(m_node->m_controlHandler)}};
}

void CoreMesh::beginCore()
{
    m_grace->stop();
    m_mode = Mode::Waiting;
    m_starting = true;
    m_node->m_running = true;
    QPointer<CoreMesh> self(this);
    m_client->request(
        startMethod(), startParams(),
        [self](bool ok, const QJsonValue &result, const QString &error) {
            if (self)
                self->onStarted(ok, result.toObject(), error);
        },
        kStartTimeoutMs);
}

void CoreMesh::onStarted(bool ok, const QJsonObject &result, const QString &error)
{
    m_starting = false;
    if (!m_wanted || m_mode != Mode::Waiting)
        return;
    if (!ok) {
        if (error == QLatin1String("core_gone") || error == QLatin1String("core_unavailable")) {
            m_grace->start(kReconnectGraceMs);
            return;
        }
        emit m_node->systemMessage(
            QStringLiteral("Local mesh: forkmesh-core could not open %1 (%2); this window runs it.")
                .arg(forkmesh::localmesh::conversation(), error));
        fallBackToNative();
        return;
    }
    m_mode = Mode::Core;
    applyState(result.value(QStringLiteral("state")).toObject());
    const QJsonArray history = result.value(QStringLiteral("history")).toArray();
    for (const QJsonValue &entry : history)
        emit m_node->messageArrived(messageFrom(entry.toObject()));
    const QString note = result.value(QStringLiteral("note")).toString();
    if (!note.isEmpty())
        emit m_node->systemMessage(note);
    emit m_node->peersChanged();
    const QSet<QString> queued = m_node->m_queued;
    for (const QString &id : queued)
        emit m_node->queuedChanged(id, true);
    flushPending();
    if (m_mode != Mode::Core)
        return;
    m_node->openBluetooth();
    m_bluetoothBeat->start();
}

void CoreMesh::leaveCore()
{
    m_bluetoothBeat->stop();
    closeBridges(m_client && m_client->isConnected());
    m_node->closeBluetooth();
    m_diagLine.clear();
    m_diagDetail.clear();
    m_diagAtMs = 0;
}

void CoreMesh::fallBackToNative()
{
    leaveCore();
    m_mode = Mode::Native;
    m_node->m_running = false;
    m_node->m_linksBlocked = false;
    m_node->m_queued.clear();
    m_node->m_historyLoaded = false;
    m_node->m_history.clear();
    m_node->m_historyChars = 0;
    m_node->m_seenIds.clear();
    m_node->m_seenOrder.clear();
    m_node->m_wakeTokens.clear();
    m_node->m_wakeOrder.clear();
    m_members.clear();
    m_peerCount = 0;
    m_listenPort = 0;
    m_wakeTargets.clear();
    m_node->startNative();
    flushPending();
    emit m_node->peersChanged();
}

void CoreMesh::onConnectedChanged(bool connected)
{
    if (!m_wanted)
        return;
    if (connected) {
        if (!coreReady() || m_mode == Mode::Core || m_starting)
            return;
        if (m_mode == Mode::Native)
            m_node->stopNative();
        beginCore();
        return;
    }
    if (m_mode != Mode::Core)
        return;
    leaveCore();
    m_mode = Mode::Waiting;
    m_node->m_running = true;
    m_grace->start(kReconnectGraceMs);
}

void CoreMesh::applyState(const QJsonObject &state)
{
    if (state.isEmpty())
        return;
    m_members.clear();
    const QJsonArray members = state.value(QStringLiteral("members")).toArray();
    for (const QJsonValue &member : members)
        m_members.append(memberFrom(member.toObject()));
    m_peerCount = state.value(QStringLiteral("peerCount")).toInt();
    m_listenPort = quint16(state.value(QStringLiteral("listenPort")).toInt());
    m_wakeTargets = stringsFrom(state.value(QStringLiteral("wakeTargets")));
    m_node->m_linksBlocked = state.value(QStringLiteral("linksBlocked")).toBool();
    const QStringList queued = stringsFrom(state.value(QStringLiteral("queued")));
    m_node->m_queued = QSet<QString>(queued.cbegin(), queued.cend());
}

void CoreMesh::onCoreEvent(const QString &event, const QJsonObject &data)
{
    if (m_mode != Mode::Core || !event.startsWith(QLatin1String("core.mesh.")))
        return;
    const QString name = event.mid(10);
    const QJsonObject state = data.value(QStringLiteral("state")).toObject();
    if (name == QLatin1String("message")) {
        applyState(state);
        emit m_node->messageArrived(messageFrom(data.value(QStringLiteral("message")).toObject()));
    } else if (name == QLatin1String("queued")) {
        applyState(state);
        emit m_node->queuedChanged(data.value(QStringLiteral("id")).toString(),
                                   data.value(QStringLiteral("queued")).toBool());
    } else if (name == QLatin1String("peers")) {
        applyState(state);
        emit m_node->peersChanged();
    } else if (name == QLatin1String("blocked")) {
        applyState(state);
        emit m_node->linksBlockedDetected();
    } else if (name == QLatin1String("typing")) {
        emit m_node->typingChanged(data.value(QStringLiteral("conversation")).toString(),
                                   data.value(QStringLiteral("peerId")).toString(),
                                   data.value(QStringLiteral("peerName")).toString(),
                                   data.value(QStringLiteral("active")).toBool());
    } else if (name == QLatin1String("wake")) {
        emit m_node->wakeRequested(stringsFrom(data.value(QStringLiteral("tokens"))));
    } else if (name == QLatin1String("system")) {
        emit m_node->systemMessage(data.value(QStringLiteral("text")).toString());
    } else if (name == QLatin1String("inbound")) {
        emit m_node->inboundLinkAccepted();
    } else if (name == QLatin1String("link.write")) {
        const Bridge bridge = m_bridges.value(data.value(QStringLiteral("link")).toInt());
        if (bridge.socket)
            bridge.socket->write(
                QByteArray::fromBase64(data.value(QStringLiteral("data")).toString().toLatin1()));
    } else if (name == QLatin1String("link.close")) {
        const int id = data.value(QStringLiteral("link")).toInt();
        const Bridge bridge = m_bridges.value(id);
        forgetBridge(id);
        if (!bridge.key.isEmpty() &&
            data.value(QStringLiteral("reason")).toString() == QLatin1String("duplicate link"))
            m_node->m_btQuietUntil.insert(bridge.key,
                                          QDateTime::currentMSecsSinceEpoch() + kBluetoothQuietMs);
        if (bridge.socket) {
            bridge.socket->disconnect(m_node);
            bridge.socket->close();
            bridge.socket->deleteLater();
        }
    } else if (name == QLatin1String("link.dropped")) {
        const bool superseded = data.value(QStringLiteral("superseded")).toBool();
        if (!data.value(QStringLiteral("bluetooth")).toBool() && !superseded)
            m_node->m_btQuietUntil.clear();
        if (!superseded && data.value(QStringLiteral("settled")).toBool()) {
            LocalMeshNode *node = m_node;
            QTimer::singleShot(kBluetoothRedialMs, node,
                               [node] { node->restartBluetoothDiscovery(); });
        }
    }
}

void CoreMesh::runWhenReady(std::function<void()> core, std::function<void()> native)
{
    switch (m_mode) {
    case Mode::Core:
        core();
        break;
    case Mode::Native:
        native();
        break;
    case Mode::Waiting:
        m_pending.append({std::move(core), std::move(native)});
        break;
    case Mode::Idle:
        break;
    }
}

void CoreMesh::flushPending()
{
    const auto pending = std::exchange(m_pending, {});
    for (const auto &action : pending) {
        if (m_mode == Mode::Core)
            action.first();
        else if (m_mode == Mode::Native)
            action.second();
    }
}

void CoreMesh::notify(const QString &method, const QJsonObject &params)
{
    if (m_client)
        m_client->notify(method, params);
}

QList<MemberInfo> CoreMesh::members() const
{
    if (m_mode == Mode::Core && !m_members.isEmpty())
        return m_members;
    MemberInfo self;
    self.id = m_node->m_nodeId;
    self.name = m_node->m_userName;
    self.nodeName = m_node->m_nodeName;
    self.ownerUser = m_node->m_ownerUser;
    self.accountKind = m_node->m_accountKind;
    self.platform = m_node->m_platform;
    self.version = m_node->m_version;
    self.self = true;
    self.online = m_node->m_running;
    return {self};
}

int CoreMesh::peerCount() const
{
    return m_mode == Mode::Core ? m_peerCount : 0;
}

quint16 CoreMesh::listenPort() const
{
    return m_mode == Mode::Core ? m_listenPort : 0;
}

QStringList CoreMesh::wakeTargets() const
{
    return m_mode == Mode::Core ? m_wakeTargets : QStringList();
}

QJsonObject CoreMesh::bluetoothInfo() const
{
    return {{QStringLiteral("active"), m_node->bluetoothActive()},
            {QStringLiteral("seen"), m_node->m_diag.bleSeen},
            {QStringLiteral("dials"), m_node->m_diag.bleDials},
            {QStringLiteral("opened"), m_node->m_diag.bleOpened},
            {QStringLiteral("lastError"), m_node->m_diag.lastBleError}};
}

void CoreMesh::refreshDiagnostics()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_mode != Mode::Core || m_diagInFlight || now - m_diagAtMs < kDiagnosticsStaleMs ||
        !m_client)
        return;
    m_diagInFlight = true;
    QPointer<CoreMesh> self(this);
    m_client->request(
        QStringLiteral("core.mesh.diagnostics"), {{QStringLiteral("bluetooth"), bluetoothInfo()}},
        [self](bool ok, const QJsonValue &result, const QString &) {
            if (!self)
                return;
            self->m_diagInFlight = false;
            self->m_diagAtMs = QDateTime::currentMSecsSinceEpoch();
            if (!ok)
                return;
            const QJsonObject object = result.toObject();
            self->m_diagLine = object.value(QStringLiteral("line")).toString();
            self->m_diagDetail = object.value(QStringLiteral("detail")).toString();
        },
        5000);
}

QString CoreMesh::diagnosticsLine()
{
    refreshDiagnostics();
    if (m_mode != Mode::Core || m_diagLine.isEmpty())
        return QStringLiteral("mesh starting · forkmesh-core");
    return m_diagLine;
}

QString CoreMesh::diagnosticsDetail()
{
    refreshDiagnostics();
    return m_diagDetail;
}

QString CoreMesh::sendChat(const QString &text)
{
    const QString trimmed = text.trimmed().left(kMaxTextChars);
    if (!m_node->m_running || trimmed.isEmpty())
        return QString();
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    LocalMeshNode *node = m_node;
    runWhenReady(
        [this, id, trimmed] {
            notify(QStringLiteral("core.mesh.sendChat"),
                   {{QStringLiteral("id"), id}, {QStringLiteral("text"), trimmed}});
        },
        [node, trimmed] { node->sendChat(trimmed); });
    return id;
}

QString CoreMesh::sendFile(const QString &fileName, const QString &mimeType,
                           const QByteArray &data)
{
    if (!m_node->m_running || data.isEmpty() || data.size() > kMaxFileBytes)
        return QString();
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    LocalMeshNode *node = m_node;
    runWhenReady(
        [this, id, fileName, mimeType, data] {
            notify(QStringLiteral("core.mesh.sendFile"),
                   {{QStringLiteral("id"), id},
                    {QStringLiteral("fileName"), fileName},
                    {QStringLiteral("mimeType"), mimeType},
                    {QStringLiteral("data"), QString::fromLatin1(data.toBase64())}});
        },
        [node, fileName, mimeType, data] { node->sendFile(fileName, mimeType, data); });
    return id;
}

void CoreMesh::sendTyping(bool active)
{
    if (m_mode == Mode::Core)
        notify(QStringLiteral("core.mesh.typing"), {{QStringLiteral("active"), active}});
}

bool CoreMesh::connectToPeer(const QHostAddress &address, quint16 port)
{
    if (!m_node->m_running || port == 0 ||
        !forkmesh::localmesh::isLocalNetworkAddress(address))
        return false;
    QHostAddress plain(address);
    plain.setScopeId(QString());
    LocalMeshNode *node = m_node;
    runWhenReady(
        [this, plain, port] {
            notify(QStringLiteral("core.mesh.connect"),
                   {{QStringLiteral("address"), plain.toString()},
                    {QStringLiteral("port"), int(port)}});
        },
        [node, address, port] { node->connectToPeer(address, port); });
    return true;
}

void CoreMesh::announceNow()
{
    LocalMeshNode *node = m_node;
    runWhenReady([this] { notify(QStringLiteral("core.mesh.announce"), {}); },
                 [node] { node->announceNow(); });
}

void CoreMesh::identityChanged()
{
    if (m_mode != Mode::Core)
        return;
    notify(QStringLiteral("core.mesh.identity"),
           {{QStringLiteral("userName"), m_node->m_userName},
            {QStringLiteral("nodeName"), m_node->m_nodeName},
            {QStringLiteral("ownerUser"), m_node->m_ownerUser},
            {QStringLiteral("accountKind"), m_node->m_accountKind}});
}

void CoreMesh::controlChanged()
{
    if (m_mode == Mode::Core)
        notify(QStringLiteral("core.mesh.control"),
               {{QStringLiteral("enabled"), bool(m_node->m_controlHandler)}});
}

int CoreMesh::bridgeFor(const QIODevice *socket) const
{
    if (!socket)
        return 0;
    for (auto it = m_bridges.cbegin(); it != m_bridges.cend(); ++it)
        if (it->socket == socket)
            return it.key();
    return 0;
}

void CoreMesh::forgetBridge(int id)
{
    const Bridge bridge = m_bridges.take(id);
    if (bridge.socket)
        QObject::disconnect(bridge.socket, nullptr, this, nullptr);
}

bool CoreMesh::attachBluetooth(QIODevice *socket, bool outbound)
{
    if (m_mode != Mode::Core) {
        socket->disconnect(m_node);
        socket->close();
        socket->deleteLater();
        return true;
    }
    const int id = ++m_nextBridge;
    m_bridges.insert(id, Bridge{socket, QString()});
    const auto forward = [this, id] {
        const Bridge bridge = m_bridges.value(id);
        if (!bridge.socket)
            return;
        const QByteArray data = bridge.socket->readAll();
        if (!data.isEmpty())
            notify(QStringLiteral("core.mesh.link.data"),
                   {{QStringLiteral("link"), id},
                    {QStringLiteral("data"), QString::fromLatin1(data.toBase64())}});
    };
    connect(socket, &QIODevice::readyRead, this, forward);
    connect(socket, &QObject::destroyed, this, [this, id] {
        if (m_bridges.remove(id) > 0)
            notify(QStringLiteral("core.mesh.link.closed"),
                   {{QStringLiteral("link"), id}, {QStringLiteral("reason"), QString()}});
    });
    notify(QStringLiteral("core.mesh.link.open"),
           {{QStringLiteral("link"), id}, {QStringLiteral("outbound"), outbound}});
    if (socket->bytesAvailable() > 0)
        forward();
    return true;
}

bool CoreMesh::dropBluetooth(QIODevice *socket, const QString &reason)
{
    const int id = bridgeFor(socket);
    if (!id)
        return false;
    forgetBridge(id);
    notify(QStringLiteral("core.mesh.link.closed"),
           {{QStringLiteral("link"), id}, {QStringLiteral("reason"), reason}});
    socket->disconnect(m_node);
    socket->close();
    socket->deleteLater();
    return true;
}

void CoreMesh::setBluetoothKey(QIODevice *socket, const QString &key)
{
    const int id = bridgeFor(socket);
    if (id)
        m_bridges[id].key = key;
}

bool CoreMesh::hasBluetoothKey(const QString &key) const
{
    for (const Bridge &bridge : m_bridges)
        if (bridge.socket && bridge.key == key)
            return true;
    return false;
}

void CoreMesh::closeBridges(bool tellCore)
{
    const QList<int> ids = m_bridges.keys();
    for (int id : ids) {
        const Bridge bridge = m_bridges.value(id);
        forgetBridge(id);
        if (tellCore)
            notify(QStringLiteral("core.mesh.link.closed"),
                   {{QStringLiteral("link"), id}, {QStringLiteral("reason"), QString()}});
        if (bridge.socket) {
            bridge.socket->disconnect(m_node);
            bridge.socket->close();
            bridge.socket->deleteLater();
        }
    }
}
