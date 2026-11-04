#include "ReticulumNode.h"

#include <QDir>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonValue>
#include <QLocalSocket>
#include <QNetworkDatagram>
#include <QRegularExpression>
#include <QTcpSocket>
#include <QTimer>
#include <QUdpSocket>
#include <QUuid>

#include <algorithm>

namespace forkmesh::reticulum {

namespace {

constexpr int kRetryFirstMs = 5000;
constexpr int kRetryMaxMs = 120000;
constexpr int kConnectTimeoutMs = 10000;
constexpr int kKeepaliveMs = 60000;
constexpr int kUdpMtu = 1064;

QString kindKey(InterfaceKind kind)
{
    switch (kind) {
    case InterfaceKind::SharedInstance:
        return QStringLiteral("shared");
    case InterfaceKind::Udp:
        return QStringLiteral("udp");
    case InterfaceKind::TcpClient:
        break;
    }
    return QStringLiteral("tcp");
}

bool truthy(const QString &value)
{
    const QString text = value.trimmed().toLower();
    return text == QStringLiteral("yes") || text == QStringLiteral("true") || text == QStringLiteral("1") ||
           text == QStringLiteral("on");
}

bool validHost(const QString &host)
{
    static const QRegularExpression name(
        QStringLiteral("^(?=.{1,253}$)([A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?)(\\.[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?)*\\.?$"));
    return !QHostAddress(host).isNull() || name.match(host).hasMatch();
}

QString elapsedText(qint64 ms)
{
    if (ms < 1000)
        return QStringLiteral("%1 ms").arg(ms);
    return QStringLiteral("%1 s").arg(double(ms) / 1000.0, 0, 'f', 1);
}

}

QJsonObject InterfaceConfig::toJson() const
{
    QJsonObject object{{QStringLiteral("id"), id},
                       {QStringLiteral("name"), name},
                       {QStringLiteral("kind"), kindKey(kind)},
                       {QStringLiteral("enabled"), enabled}};
    if (usesAccessCode()) {
        object.insert(QStringLiteral("networkName"), networkName);
        object.insert(QStringLiteral("passphrase"), passphrase);
        object.insert(QStringLiteral("accessCodeSize"), accessCodeSize);
    }
    switch (kind) {
    case InterfaceKind::TcpClient:
        object.insert(QStringLiteral("host"), host);
        object.insert(QStringLiteral("port"), port);
        object.insert(QStringLiteral("kiss"), kiss);
        break;
    case InterfaceKind::SharedInstance:
        object.insert(QStringLiteral("instanceName"), instanceName);
        object.insert(QStringLiteral("sharedOverTcp"), sharedOverTcp);
        object.insert(QStringLiteral("port"), port);
        break;
    case InterfaceKind::Udp:
        object.insert(QStringLiteral("listenAddress"), listenAddress);
        object.insert(QStringLiteral("listenPort"), listenPort);
        object.insert(QStringLiteral("forwardAddress"), forwardAddress);
        object.insert(QStringLiteral("forwardPort"), forwardPort);
        break;
    }
    return object;
}

std::optional<InterfaceConfig> InterfaceConfig::fromJson(const QJsonObject &object)
{
    InterfaceConfig config;
    config.id = object.value(QStringLiteral("id")).toString();
    config.name = object.value(QStringLiteral("name")).toString().trimmed();
    const QString kind = object.value(QStringLiteral("kind")).toString();
    if (kind == QStringLiteral("shared"))
        config.kind = InterfaceKind::SharedInstance;
    else if (kind == QStringLiteral("udp"))
        config.kind = InterfaceKind::Udp;
    else if (kind == QStringLiteral("tcp"))
        config.kind = InterfaceKind::TcpClient;
    else
        return std::nullopt;
    config.enabled = object.value(QStringLiteral("enabled")).toBool(true);
    config.host = object.value(QStringLiteral("host")).toString().trimmed();
    config.port = object.value(QStringLiteral("port")).toInt(config.kind == InterfaceKind::SharedInstance ? 37428 : 4242);
    config.kiss = object.value(QStringLiteral("kiss")).toBool(false);
    config.instanceName = object.value(QStringLiteral("instanceName")).toString(QStringLiteral("default")).trimmed();
    config.sharedOverTcp = object.value(QStringLiteral("sharedOverTcp")).toBool(false);
    config.listenAddress = object.value(QStringLiteral("listenAddress")).toString(QStringLiteral("0.0.0.0")).trimmed();
    config.listenPort = object.value(QStringLiteral("listenPort")).toInt(4242);
    config.forwardAddress =
        object.value(QStringLiteral("forwardAddress")).toString(QStringLiteral("255.255.255.255")).trimmed();
    config.forwardPort = object.value(QStringLiteral("forwardPort")).toInt(4242);
    config.networkName = object.value(QStringLiteral("networkName")).toString();
    config.passphrase = object.value(QStringLiteral("passphrase")).toString();
    config.accessCodeSize = object.value(QStringLiteral("accessCodeSize")).toInt(16);
    if (config.id.isEmpty())
        config.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!config.validationError().isEmpty())
        return std::nullopt;
    return config;
}

QString InterfaceConfig::endpoint() const
{
    switch (kind) {
    case InterfaceKind::TcpClient:
        return QStringLiteral("%1:%2").arg(host.contains(QLatin1Char(':')) ? QStringLiteral("[%1]").arg(host) : host)
            .arg(port);
    case InterfaceKind::SharedInstance:
        return sharedOverTcp ? QStringLiteral("127.0.0.1:%1").arg(port)
                             : QStringLiteral("rns/%1").arg(instanceName);
    case InterfaceKind::Udp:
        return QStringLiteral("%1:%2 → %3:%4").arg(listenAddress).arg(listenPort).arg(forwardAddress).arg(forwardPort);
    }
    return {};
}

QString InterfaceConfig::kindLabel() const
{
    switch (kind) {
    case InterfaceKind::TcpClient:
        return kiss ? QStringLiteral("TCP (KISS)") : QStringLiteral("TCP");
    case InterfaceKind::SharedInstance:
        return QStringLiteral("Shared instance");
    case InterfaceKind::Udp:
        return QStringLiteral("UDP");
    }
    return {};
}

QString InterfaceConfig::validationError() const
{
    if (name.isEmpty() || name.size() > 80)
        return QStringLiteral("Give the interface a name of up to 80 characters.");
    const auto badPort = [](int value) { return value < 1 || value > 65535; };
    if (usesAccessCode() && (accessCodeSize < 1 || accessCodeSize > 64))
        return QStringLiteral("The access code size must be between 1 and 64 bytes.");
    if (networkName.size() > 256 || passphrase.size() > 256)
        return QStringLiteral("The network name and passphrase may be up to 256 characters.");
    switch (kind) {
    case InterfaceKind::TcpClient:
        if (!validHost(host))
            return QStringLiteral("Enter a host name or IP address.");
        if (badPort(port))
            return QStringLiteral("The port must be between 1 and 65535.");
        break;
    case InterfaceKind::SharedInstance: {
        static const QRegularExpression instance(QStringLiteral("^[A-Za-z0-9._-]{1,64}$"));
        if (!sharedOverTcp && !instance.match(instanceName).hasMatch())
            return QStringLiteral("The instance name may use letters, digits, dot, dash and underscore.");
        if (sharedOverTcp && badPort(port))
            return QStringLiteral("The port must be between 1 and 65535.");
        break;
    }
    case InterfaceKind::Udp:
        if (QHostAddress(listenAddress).isNull() || QHostAddress(forwardAddress).isNull())
            return QStringLiteral("UDP needs a listen address and a forward address.");
        if (badPort(listenPort) || badPort(forwardPort))
            return QStringLiteral("The ports must be between 1 and 65535.");
        break;
    }
    return {};
}

bool InterfaceConfig::sameTarget(const InterfaceConfig &other) const
{
    return kind == other.kind && endpoint().compare(other.endpoint(), Qt::CaseInsensitive) == 0;
}

QString linkStateLabel(LinkState state)
{
    switch (state) {
    case LinkState::Disabled:
        return QStringLiteral("Off");
    case LinkState::Connecting:
        return QStringLiteral("Connecting");
    case LinkState::Online:
        return QStringLiteral("Online");
    case LinkState::Waiting:
        return QStringLiteral("Retrying");
    case LinkState::Blocked:
        return QStringLiteral("Blocked");
    case LinkState::Failed:
        return QStringLiteral("Failed");
    }
    return {};
}

QList<InterfaceConfig> publicEntryPoints()
{
    InterfaceConfig first;
    first.id = QStringLiteral("public-rmap-1");
    first.name = QStringLiteral("RMAP Backbone 1");
    first.host = QStringLiteral("rmap.world");
    first.port = 4242;
    InterfaceConfig second = first;
    second.id = QStringLiteral("public-rmap-2");
    second.name = QStringLiteral("RMAP Backbone 2");
    second.port = 4343;
    return {first, second};
}

QList<InterfaceConfig> defaultInterfaces()
{
    InterfaceConfig shared;
    shared.id = QStringLiteral("shared-instance");
    shared.name = QStringLiteral("Reticulum on this computer");
    shared.kind = InterfaceKind::SharedInstance;
    shared.port = 37428;
#ifndef Q_OS_LINUX
    shared.sharedOverTcp = true;
#endif
    return {shared};
}

QString reticulumConfigPath()
{
    const QStringList candidates = {QStringLiteral("/etc/reticulum/config"),
                                    QDir::homePath() + QStringLiteral("/.config/reticulum/config"),
                                    QDir::homePath() + QStringLiteral("/.reticulum/config")};
    for (const QString &path : candidates) {
        if (QFileInfo(path).isFile())
            return path;
    }
    return {};
}

QList<InterfaceConfig> parseReticulumConfig(const QString &text, QStringList *skipped)
{
    QList<InterfaceConfig> out;
    QString section;
    QString current;
    QHash<QString, QString> values;
    QHash<QString, QString> reticulum;
    const auto flush = [&] {
        if (current.isEmpty())
            return;
        const QString type = values.value(QStringLiteral("type"));
        InterfaceConfig config;
        config.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        config.name = current.left(80);
        config.enabled = truthy(values.value(QStringLiteral("enabled"))) ||
                         truthy(values.value(QStringLiteral("interface_enabled")));
        bool accepted = false;
        if (type == QStringLiteral("TCPClientInterface")) {
            config.kind = InterfaceKind::TcpClient;
            config.host = values.value(QStringLiteral("target_host"));
            config.port = values.value(QStringLiteral("target_port")).toInt();
            config.kiss = truthy(values.value(QStringLiteral("kiss_framing")));
            accepted = true;
        } else if (type == QStringLiteral("BackboneInterface") && values.contains(QStringLiteral("remote"))) {
            config.kind = InterfaceKind::TcpClient;
            config.host = values.value(QStringLiteral("remote"));
            config.port = values.value(QStringLiteral("target_port"), values.value(QStringLiteral("port"))).toInt();
            accepted = true;
        } else if (type == QStringLiteral("UDPInterface")) {
            config.kind = InterfaceKind::Udp;
            const int both = values.value(QStringLiteral("port")).toInt();
            config.listenAddress = values.value(QStringLiteral("listen_ip"), QStringLiteral("0.0.0.0"));
            config.forwardAddress = values.value(QStringLiteral("forward_ip"), QStringLiteral("255.255.255.255"));
            config.listenPort = values.value(QStringLiteral("listen_port")).toInt();
            config.forwardPort = values.value(QStringLiteral("forward_port")).toInt();
            if (both > 0) {
                config.listenPort = both;
                config.forwardPort = both;
            }
            accepted = true;
        }
        config.networkName = values.value(QStringLiteral("networkname"), values.value(QStringLiteral("network_name")));
        config.passphrase = values.value(QStringLiteral("passphrase"), values.value(QStringLiteral("pass_phrase")));
        if (values.contains(QStringLiteral("ifac_size")))
            config.accessCodeSize = qMax(1, values.value(QStringLiteral("ifac_size")).toInt() / 8);
        if (accepted && config.validationError().isEmpty())
            out.append(config);
        else if (skipped)
            skipped->append(QStringLiteral("%1 (%2)").arg(current, type.isEmpty() ? QStringLiteral("no type") : type));
        current.clear();
        values.clear();
    };
    static const QRegularExpression subsection(QStringLiteral("^\\[\\[\\s*(.+?)\\s*\\]\\]$"));
    static const QRegularExpression top(QStringLiteral("^\\[\\s*([^\\[\\]]+?)\\s*\\]$"));
    static const QRegularExpression pair(QStringLiteral("^([A-Za-z0-9_]+)\\s*=\\s*(.*)$"));
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (QString line : lines) {
        const int hash = line.indexOf(QLatin1Char('#'));
        if (hash >= 0)
            line.truncate(hash);
        line = line.trimmed();
        if (line.isEmpty())
            continue;
        if (const QRegularExpressionMatch match = subsection.match(line); match.hasMatch()) {
            flush();
            if (section == QStringLiteral("interfaces"))
                current = match.captured(1);
            continue;
        }
        if (const QRegularExpressionMatch match = top.match(line); match.hasMatch()) {
            flush();
            section = match.captured(1).toLower();
            continue;
        }
        if (const QRegularExpressionMatch match = pair.match(line); match.hasMatch()) {
            QString value = match.captured(2).trimmed();
            if (value.size() >= 2 && (value.startsWith(QLatin1Char('"')) || value.startsWith(QLatin1Char('\''))) &&
                value.endsWith(value.at(0)))
                value = value.mid(1, value.size() - 2);
            if (!current.isEmpty())
                values.insert(match.captured(1).toLower(), value);
            else if (section == QStringLiteral("reticulum"))
                reticulum.insert(match.captured(1).toLower(), value);
        }
    }
    flush();
    if (!reticulum.contains(QStringLiteral("share_instance")) ||
        truthy(reticulum.value(QStringLiteral("share_instance")))) {
        InterfaceConfig shared = defaultInterfaces().first();
        shared.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (reticulum.contains(QStringLiteral("instance_name")))
            shared.instanceName = reticulum.value(QStringLiteral("instance_name"));
        if (reticulum.value(QStringLiteral("shared_instance_type")).toLower() == QStringLiteral("tcp"))
            shared.sharedOverTcp = true;
        if (reticulum.contains(QStringLiteral("shared_instance_port")))
            shared.port = reticulum.value(QStringLiteral("shared_instance_port")).toInt();
        if (shared.validationError().isEmpty())
            out.prepend(shared);
    }
    return out;
}

InterfaceRuntime::InterfaceRuntime(Node *node, const InterfaceConfig &config)
    : QObject(node)
    , m_node(node)
    , m_config(config)
    , m_decoder(config.kiss ? FrameDecoder::Kiss : FrameDecoder::Hdlc)
    , m_accessCode(config.networkName, config.passphrase, config.accessCodeSize)
    , m_retryTimer(new QTimer(this))
    , m_keepaliveTimer(new QTimer(this))
{
    m_retryTimer->setSingleShot(true);
    connect(m_retryTimer, &QTimer::timeout, this, [this] { start(); });
    m_keepaliveTimer->setInterval(kKeepaliveMs);
    connect(m_keepaliveTimer, &QTimer::timeout, this, [this] {
        if (m_local && m_local->state() == QLocalSocket::ConnectedState)
            m_local->write(hdlcKeepalive());
        else if (m_tcp && m_tcp->state() == QAbstractSocket::ConnectedState &&
                 m_config.kind == InterfaceKind::SharedInstance)
            m_tcp->write(hdlcKeepalive());
    });
    m_status.history.reserve(Node::kHistorySeconds);
}

InterfaceRuntime::~InterfaceRuntime()
{
    dropSockets();
}

void InterfaceRuntime::setConfig(const InterfaceConfig &config)
{
    const bool changed = !m_config.sameTarget(config) || m_config.kiss != config.kiss ||
                         m_config.networkName != config.networkName || m_config.passphrase != config.passphrase ||
                         m_config.accessCodeSize != config.accessCodeSize;
    const bool wasActive = m_active;
    m_config = config;
    if (changed) {
        m_authorized = false;
        m_decoder.setFraming(config.kiss ? FrameDecoder::Kiss : FrameDecoder::Hdlc);
        m_accessCode = InterfaceAccessCode(config.networkName, config.passphrase, config.accessCodeSize);
    }
    if (!config.enabled) {
        stop();
        return;
    }
    if (changed && wasActive) {
        stop();
        start();
    } else if (!wasActive && m_node->running()) {
        start();
    }
}

void InterfaceRuntime::start()
{
    if (!m_config.enabled) {
        setState(LinkState::Disabled);
        return;
    }
    m_active = true;
    m_retryTimer->stop();
    dropSockets();
    m_decoder.reset();
    ++m_status.attempts;
    m_connectStarted = QDateTime::currentMSecsSinceEpoch();
    switch (m_config.kind) {
    case InterfaceKind::TcpClient:
        connectTcp();
        break;
    case InterfaceKind::SharedInstance:
        if (m_config.sharedOverTcp)
            connectTcp();
        else
            connectLocal();
        break;
    case InterfaceKind::Udp:
        bindUdp();
        break;
    }
}

void InterfaceRuntime::stop()
{
    m_active = false;
    m_retryTimer->stop();
    m_keepaliveTimer->stop();
    dropSockets();
    m_status.nextAttempt = {};
    setState(LinkState::Disabled, m_config.enabled ? QStringLiteral("Stopped") : QString());
}

void InterfaceRuntime::reconnectNow()
{
    m_authorized = false;
    m_status.attempts = 0;
    if (!m_config.enabled)
        return;
    stop();
    start();
}

bool InterfaceRuntime::authorize(const QString &method, const QUrl &url)
{
    if (m_authorized)
        return true;
    const Node::Authorizer &authorizer = m_node->m_authorizer;
    if (!authorizer || authorizer(method, url)) {
        m_authorized = true;
        return true;
    }
    return false;
}

void InterfaceRuntime::connectTcp()
{
    const bool shared = m_config.kind == InterfaceKind::SharedInstance;
    const QString host = shared ? QStringLiteral("127.0.0.1") : m_config.host;
    const int port = m_config.port;
    QUrl url;
    url.setScheme(QStringLiteral("tcp"));
    url.setHost(host);
    url.setPort(port);
    if (!authorize(QStringLiteral("Reticulum"), url)) {
        setState(LinkState::Blocked, QStringLiteral("The ForkMesh firewall did not allow %1").arg(m_config.endpoint()));
        m_active = false;
        return;
    }
    auto *socket = new QTcpSocket(this);
    m_tcp = socket;
    socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    socket->setSocketOption(QAbstractSocket::KeepAliveOption, 1);
    setState(LinkState::Connecting, QStringLiteral("Connecting to %1").arg(m_config.endpoint()));
    auto *timeout = new QTimer(socket);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, socket, [this, socket] {
        if (socket->state() != QAbstractSocket::ConnectedState)
            lostConnection(QStringLiteral("No answer from %1 after %2 s")
                               .arg(m_config.endpoint())
                               .arg(kConnectTimeoutMs / 1000));
    });
    timeout->start(kConnectTimeoutMs);
    connect(socket, &QTcpSocket::connected, this, [this, timeout] {
        timeout->stop();
        wentOnline();
    });
    connect(socket, &QTcpSocket::readyRead, this, [this, socket] { receive(socket->readAll()); });
    connect(socket, &QTcpSocket::disconnected, this, [this] {
        lostConnection(QStringLiteral("%1 closed the connection").arg(m_config.endpoint()));
    });
    connect(socket, &QTcpSocket::errorOccurred, this, [this, socket](QAbstractSocket::SocketError) {
        if (socket->state() != QAbstractSocket::ConnectedState)
            lostConnection(socket->errorString());
    });
    socket->connectToHost(host, quint16(port));
}

void InterfaceRuntime::connectLocal()
{
    auto *socket = new QLocalSocket(this);
    m_local = socket;
    socket->setSocketOptions(QLocalSocket::AbstractNamespaceOption);
    setState(LinkState::Connecting, QStringLiteral("Looking for the Reticulum daemon"));
    connect(socket, &QLocalSocket::connected, this, [this] { wentOnline(); });
    connect(socket, &QLocalSocket::readyRead, this, [this, socket] { receive(socket->readAll()); });
    connect(socket, &QLocalSocket::disconnected, this,
            [this] { lostConnection(QStringLiteral("The Reticulum daemon closed the connection")); });
    connect(socket, &QLocalSocket::errorOccurred, this, [this, socket](QLocalSocket::LocalSocketError error) {
        if (socket->state() == QLocalSocket::ConnectedState)
            return;
        if (error == QLocalSocket::ServerNotFoundError || error == QLocalSocket::ConnectionRefusedError)
            lostConnection(QStringLiteral("No Reticulum daemon is running on this computer (rnsd)"));
        else
            lostConnection(socket->errorString());
    });
    socket->connectToServer(QStringLiteral("rns/%1").arg(m_config.instanceName));
}

void InterfaceRuntime::bindUdp()
{
    QUrl url;
    url.setScheme(QStringLiteral("udp"));
    url.setHost(m_config.forwardAddress);
    url.setPort(m_config.forwardPort);
    if (!authorize(QStringLiteral("Reticulum UDP"), url)) {
        setState(LinkState::Blocked, QStringLiteral("The ForkMesh firewall did not allow %1").arg(m_config.forwardAddress));
        m_active = false;
        return;
    }
    auto *socket = new QUdpSocket(this);
    m_udp = socket;
    if (!socket->bind(QHostAddress(m_config.listenAddress), quint16(m_config.listenPort),
                      QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        scheduleRetry(QStringLiteral("Could not listen on %1:%2: %3")
                          .arg(m_config.listenAddress)
                          .arg(m_config.listenPort)
                          .arg(socket->errorString()));
        return;
    }
    connect(socket, &QUdpSocket::readyRead, this, [this, socket] {
        while (socket->hasPendingDatagrams()) {
            const QNetworkDatagram datagram = socket->receiveDatagram(kUdpMtu + 1);
            const QByteArray bytes = datagram.data();
            m_status.rxBytes += bytes.size();
            m_current.rx += bytes.size();
            m_node->m_current.rx += bytes.size();
            if (bytes.size() > kUdpMtu || bytes.size() <= kHeaderMinSize) {
                ++m_status.invalidFrames;
                continue;
            }
            receiveFrame(bytes);
        }
    });
    wentOnline();
}

void InterfaceRuntime::wentOnline()
{
    m_status.connectMs = QDateTime::currentMSecsSinceEpoch() - m_connectStarted;
    m_status.onlineSince = QDateTime::currentDateTimeUtc();
    m_status.attempts = 0;
    m_status.nextAttempt = {};
    ++m_status.sessions;
    if (m_config.kind == InterfaceKind::SharedInstance)
        m_keepaliveTimer->start();
    const QString detail = m_config.kind == InterfaceKind::Udp
                               ? QStringLiteral("Listening on %1:%2").arg(m_config.listenAddress).arg(m_config.listenPort)
                               : QStringLiteral("Connected in %1").arg(elapsedText(m_status.connectMs));
    setState(LinkState::Online, detail);
    m_node->addEvent(QStringLiteral("interface"), QStringLiteral("%1 is online").arg(m_config.name));
}

void InterfaceRuntime::lostConnection(const QString &reason)
{
    if (!m_active)
        return;
    const bool wasOnline = m_status.state == LinkState::Online;
    m_keepaliveTimer->stop();
    if (wasOnline)
        m_node->addEvent(QStringLiteral("interface"), QStringLiteral("%1 went offline: %2").arg(m_config.name, reason));
    scheduleRetry(reason);
}

void InterfaceRuntime::scheduleRetry(const QString &reason)
{
    QPointer<QTcpSocket> tcp = m_tcp;
    QPointer<QLocalSocket> local = m_local;
    QPointer<QUdpSocket> udp = m_udp;
    m_tcp.clear();
    m_local.clear();
    m_udp.clear();
    for (QObject *socket : {static_cast<QObject *>(tcp.data()), static_cast<QObject *>(local.data()),
                            static_cast<QObject *>(udp.data())}) {
        if (socket) {
            socket->disconnect(this);
            socket->deleteLater();
        }
    }
    const int exponent = qBound(0, m_status.attempts - 1, 5);
    const int delay = qMin(kRetryMaxMs, kRetryFirstMs << exponent);
    m_status.nextAttempt = QDateTime::currentDateTimeUtc().addMSecs(delay);
    setState(LinkState::Waiting, reason);
    m_retryTimer->start(delay);
}

void InterfaceRuntime::dropSockets()
{
    if (m_tcp) {
        m_tcp->disconnect(this);
        m_tcp->abort();
        m_tcp->deleteLater();
    }
    if (m_local) {
        m_local->disconnect(this);
        m_local->abort();
        m_local->deleteLater();
    }
    if (m_udp) {
        m_udp->disconnect(this);
        m_udp->close();
        m_udp->deleteLater();
    }
    m_tcp.clear();
    m_local.clear();
    m_udp.clear();
}

void InterfaceRuntime::setState(LinkState state, const QString &detail)
{
    if (state != LinkState::Online)
        m_status.onlineSince = {};
    m_status.state = state;
    m_status.detail = detail;
    m_node->notifyChanged();
}

void InterfaceRuntime::receive(const QByteArray &bytes)
{
    m_status.rxBytes += bytes.size();
    m_current.rx += bytes.size();
    m_node->m_current.rx += bytes.size();
    const qint64 invalidBefore = m_decoder.invalidFrames();
    const QList<QByteArray> frames = m_decoder.feed(bytes);
    m_status.invalidFrames += m_decoder.invalidFrames() - invalidBefore;
    for (const QByteArray &frame : frames)
        receiveFrame(frame);
}

void InterfaceRuntime::receiveFrame(const QByteArray &frame)
{
    ++m_status.rxPackets;
    if (m_accessCode.enabled()) {
        const std::optional<QByteArray> inner = m_accessCode.remove(frame);
        if (!inner) {
            ++m_status.invalidFrames;
            return;
        }
        m_node->ingest(m_config.id, *inner, m_config.kind == InterfaceKind::SharedInstance);
        return;
    }
    if (!frame.isEmpty() && (quint8(frame.at(0)) & 0x80)) {
        ++m_status.invalidFrames;
        return;
    }
    m_node->ingest(m_config.id, frame, m_config.kind == InterfaceKind::SharedInstance);
}

bool InterfaceRuntime::send(const QByteArray &packet)
{
    if (!online() || packet.isEmpty())
        return false;
    const QByteArray raw = m_accessCode.enabled() ? m_accessCode.apply(packet) : packet;
    qint64 written = -1;
    if (m_udp) {
        if (raw.size() > kUdpMtu)
            return false;
        written = m_udp->writeDatagram(raw, QHostAddress(m_config.forwardAddress), quint16(m_config.forwardPort));
    } else {
        const QByteArray framed = m_config.kiss ? kissFrame(raw) : hdlcFrame(raw);
        if (m_tcp)
            written = m_tcp->write(framed);
        else if (m_local)
            written = m_local->write(framed);
    }
    if (written <= 0)
        return false;
    ++m_status.txPackets;
    m_status.txBytes += written;
    m_current.tx += written;
    m_node->m_current.tx += written;
    return true;
}

void InterfaceRuntime::rotateHistory()
{
    m_status.history.append(m_current);
    if (m_status.history.size() > Node::kHistorySeconds)
        m_status.history.remove(0, m_status.history.size() - Node::kHistorySeconds);
    m_current = {};
}

Node::Node(QObject *parent)
    : QObject(parent)
    , m_changeTimer(new QTimer(this))
    , m_tickTimer(new QTimer(this))
{
    m_changeTimer->setSingleShot(true);
    m_changeTimer->setInterval(150);
    connect(m_changeTimer, &QTimer::timeout, this, [this] {
        if (m_changed)
            m_changed();
    });
    m_tickTimer->setInterval(1000);
    connect(m_tickTimer, &QTimer::timeout, this, [this] { tick(); });
    m_history.reserve(kHistorySeconds);
    addLocalDestination(m_aspect, [this] { return m_announceName.toUtf8(); });
}

Node::~Node()
{
    m_changed = nullptr;
    m_announced = nullptr;
    m_packetHandler = nullptr;
    for (InterfaceRuntime *runtime : std::as_const(m_runtimes))
        runtime->stop();
}

void Node::setAuthorizer(Authorizer authorizer)
{
    m_authorizer = std::move(authorizer);
}

void Node::setChangedHandler(std::function<void()> handler)
{
    m_changed = std::move(handler);
}

QList<InterfaceConfig> Node::interfaces() const
{
    QList<InterfaceConfig> out;
    for (const InterfaceRuntime *runtime : m_runtimes)
        out.append(runtime->config());
    return out;
}

void Node::setInterfaces(const QList<InterfaceConfig> &configs)
{
    QSet<QString> keep;
    for (const InterfaceConfig &config : configs)
        keep.insert(config.id);
    for (InterfaceRuntime *runtime : QList<InterfaceRuntime *>(m_runtimes)) {
        if (!keep.contains(runtime->config().id))
            removeInterface(runtime->config().id);
    }
    for (const InterfaceConfig &config : configs)
        upsertInterface(config);
    QList<InterfaceRuntime *> ordered;
    for (const InterfaceConfig &config : configs) {
        if (InterfaceRuntime *found = runtime(config.id))
            ordered.append(found);
    }
    m_runtimes = ordered;
    notifyChanged();
}

void Node::upsertInterface(const InterfaceConfig &config)
{
    if (InterfaceRuntime *existing = runtime(config.id)) {
        existing->setConfig(config);
    } else {
        auto *created = new InterfaceRuntime(this, config);
        m_runtimes.append(created);
        if (m_running && config.enabled)
            created->start();
    }
    updateTicking();
    notifyChanged();
}

void Node::removeInterface(const QString &id)
{
    InterfaceRuntime *found = runtime(id);
    if (!found)
        return;
    m_runtimes.removeAll(found);
    found->stop();
    found->deleteLater();
    updateTicking();
    notifyChanged();
}

void Node::setInterfaceEnabled(const QString &id, bool enabled)
{
    InterfaceRuntime *found = runtime(id);
    if (!found)
        return;
    InterfaceConfig config = found->config();
    config.enabled = enabled;
    found->setConfig(config);
    if (enabled && m_running)
        found->reconnectNow();
    updateTicking();
    notifyChanged();
}

void Node::reconnectInterface(const QString &id)
{
    if (InterfaceRuntime *found = runtime(id))
        found->reconnectNow();
    updateTicking();
}

InterfaceStatus Node::status(const QString &id) const
{
    if (const InterfaceRuntime *found = runtime(id))
        return found->status();
    return {};
}

int Node::onlineCount() const
{
    return int(std::count_if(m_runtimes.cbegin(), m_runtimes.cend(),
                             [](const InterfaceRuntime *runtime) { return runtime->online(); }));
}

void Node::start()
{
    if (m_running)
        return;
    m_running = true;
    m_startedAt = QDateTime::currentDateTimeUtc();
    for (InterfaceRuntime *runtime : std::as_const(m_runtimes)) {
        if (runtime->config().enabled)
            runtime->start();
    }
    updateTicking();
    notifyChanged();
}

void Node::stop()
{
    m_running = false;
    for (InterfaceRuntime *runtime : std::as_const(m_runtimes))
        runtime->stop();
    updateTicking();
    notifyChanged();
}

void Node::setIdentity(const Identity &identity)
{
    m_identity = identity;
    notifyChanged();
}

QByteArray Node::destinationHash() const
{
    return localDestinationHash(m_aspect);
}

void Node::setAnnounceName(const QString &name)
{
    m_announceName = name.trimmed().left(64);
}

void Node::addLocalDestination(const QString &aspect, std::function<QByteArray()> appData)
{
    m_local.insert(aspect, {aspect, std::move(appData)});
}

QByteArray Node::localDestinationHash(const QString &aspect) const
{
    if (!m_identity.isValid() || !m_local.contains(aspect))
        return {};
    return destinationHashForName(aspect, m_identity.hash());
}

QString Node::localAspect(const QByteArray &hash) const
{
    if (!m_identity.isValid() || hash.size() != kHashLength)
        return {};
    for (auto it = m_local.cbegin(); it != m_local.cend(); ++it) {
        if (destinationHashForName(it.key(), m_identity.hash()) == hash)
            return it.key();
    }
    return {};
}

bool Node::announceDestination(const QByteArray &destination, bool pathResponse, const QString &onlyInterface)
{
    const QString aspect = localAspect(destination);
    if (aspect.isEmpty() || !m_identity.hasPrivateKey() || onlineCount() == 0)
        return false;
    const LocalDestination local = m_local.value(aspect);
    const QByteArray raw = buildAnnounce(m_identity, aspect, local.appData ? local.appData() : QByteArray(), pathResponse,
                                         QDateTime::currentSecsSinceEpoch());
    if (raw.isEmpty())
        return false;
    rememberPacket(packetHash(raw));
    transmit(raw, onlyInterface);
    ++m_counters.announcesSent;
    if (!pathResponse) {
        m_lastAnnounce = QDateTime::currentDateTimeUtc();
        addEvent(QStringLiteral("announce-sent"),
                 QStringLiteral("Announced %1 as %2").arg(aspect, prettyHash(destination)), destination);
    }
    notifyChanged();
    return true;
}

bool Node::announce(bool pathResponse, const QString &onlyInterface)
{
    bool sent = false;
    QStringList aspects = m_local.keys();
    std::sort(aspects.begin(), aspects.end());
    for (const QString &aspect : aspects)
        sent = announceDestination(localDestinationHash(aspect), pathResponse, onlyInterface) || sent;
    return sent;
}

std::optional<Identity> Node::identityFor(const QByteArray &destination) const
{
    const auto found = m_destinations.constFind(destination);
    if (found == m_destinations.cend())
        return std::nullopt;
    return Identity::fromPublicKey(found->publicKey);
}

bool Node::interfaceOnline(const QString &id) const
{
    const InterfaceRuntime *found = runtime(id);
    return found && found->online();
}

QByteArray Node::sendRaw(const QByteArray &raw, const QString &onlyInterface)
{
    if (raw.isEmpty())
        return {};
    rememberPacket(packetHash(raw));
    transmit(raw, onlyInterface);
    notifyChanged();
    return raw;
}

QByteArray Node::sendPacket(Packet packet, const QString &onlyInterface)
{
    packet.headerType = Header1;
    packet.transportType = TransportBroadcast;
    packet.transportId.clear();
    const QByteArray raw = packPacket(packet);
    rememberPacket(packetHash(raw));
    const auto found = m_destinations.constFind(packet.destinationHash);
    const bool routable = packet.packetType != PacketAnnounce && packet.destinationType != DestinationPlain &&
                          packet.destinationType != DestinationGroup && found != m_destinations.cend() &&
                          onlyInterface.isEmpty() && interfaceOnline(found->interfaceId);
    if (routable) {
        if (found->hops > 1 || (found->hops == 1 && found->viaSharedInstance)) {
            Packet routed = packet;
            routed.headerType = Header2;
            routed.transportType = TransportForward;
            routed.transportId = found->transportId.size() == kHashLength ? found->transportId : found->hash;
            transmit(packPacket(routed), found->interfaceId);
        } else {
            transmit(raw, found->interfaceId);
        }
    } else {
        transmit(raw, onlyInterface);
    }
    notifyChanged();
    return raw;
}

QByteArray Node::requestPath(const QByteArray &destination)
{
    if (destination.size() != kHashLength || onlineCount() == 0)
        return {};
    const QByteArray tag = truncatedHash(randomBytes(32));
    const QByteArray raw = buildPathRequest(destination, tag);
    rememberPacket(packetHash(raw));
    transmit(raw);
    PathLookup lookup;
    lookup.destination = destination;
    lookup.tag = tag;
    lookup.started = QDateTime::currentDateTimeUtc();
    m_lookups.insert(destination, lookup);
    addEvent(QStringLiteral("path-request"), QStringLiteral("Asked the network for a path to %1").arg(prettyHash(destination)),
             destination);
    updateTicking();
    notifyChanged();
    return tag;
}

void Node::forgetDestination(const QByteArray &hash)
{
    m_destinations.remove(hash);
    m_lookups.remove(hash);
    notifyChanged();
}

void Node::clearDestinations()
{
    m_destinations.clear();
    m_lookups.clear();
    notifyChanged();
}

QVector<TrafficSample> Node::totalHistory() const
{
    return m_history;
}

InterfaceRuntime *Node::runtime(const QString &id) const
{
    for (InterfaceRuntime *runtime : m_runtimes) {
        if (runtime->config().id == id)
            return runtime;
    }
    return nullptr;
}

QString Node::interfaceName(const QString &id) const
{
    if (const InterfaceRuntime *found = runtime(id))
        return found->config().name;
    return id;
}

void Node::transmit(const QByteArray &raw, const QString &onlyInterface)
{
    for (InterfaceRuntime *runtime : std::as_const(m_runtimes)) {
        if (!onlyInterface.isEmpty() && runtime->config().id != onlyInterface)
            continue;
        runtime->send(raw);
    }
}

void Node::notifyChanged()
{
    if (!m_changeTimer->isActive())
        m_changeTimer->start();
}

void Node::addEvent(const QString &kind, const QString &text, const QByteArray &destination)
{
    m_events.prepend({QDateTime::currentDateTimeUtc(), kind, text, destination});
    if (m_events.size() > kMaxEvents)
        m_events.erase(m_events.begin() + kMaxEvents, m_events.end());
    notifyChanged();
}

bool Node::rememberPacket(const QByteArray &hash)
{
    if (hash.isEmpty() || m_seen.contains(hash))
        return false;
    m_seen.insert(hash);
    m_seenOrder.enqueue(hash);
    while (m_seenOrder.size() > kMaxSeenPackets)
        m_seen.remove(m_seenOrder.dequeue());
    return true;
}

void Node::ingest(const QString &interfaceId, const QByteArray &raw, bool fromSharedInstance)
{
    QString error;
    const std::optional<Packet> packet = parsePacket(raw, &error);
    InterfaceRuntime *source = runtime(interfaceId);
    if (!packet) {
        ++m_counters.invalid;
        if (source)
            ++source->m_status.invalidFrames;
        return;
    }
    const int hops = fromSharedInstance ? packet->hops : packet->hops + 1;
    const QByteArray hash = packetHash(raw);
    const bool repeatable = packet->context == ContextKeepalive || packet->context == ContextResourceRequest ||
                            packet->context == ContextResourceProof || packet->context == ContextResource ||
                            packet->context == ContextCacheRequest || packet->context == ContextChannel;
    const bool fresh = rememberPacket(hash) || (repeatable && packet->packetType != PacketAnnounce);
    if (packet->packetType != PacketAnnounce && packet->headerType == Header2 && !fromSharedInstance) {
        ++m_counters.duplicates;
        return;
    }
    switch (packet->packetType) {
    case PacketAnnounce:
        ++m_counters.announces;
        ++m_current.announces;
        if (source) {
            ++source->m_status.announces;
            ++source->m_current.announces;
        }
        if (!fresh) {
            ++m_counters.duplicates;
            auto found = m_destinations.find(packet->destinationHash);
            if (found != m_destinations.end() && hops < found->hops) {
                found->hops = hops;
                found->interfaceId = interfaceId;
                found->transportId = packet->transportId;
                notifyChanged();
            }
            return;
        }
        handleAnnounce(interfaceId, *packet, raw, hops);
        break;
    case PacketData:
        ++m_counters.data;
        if (!fresh) {
            ++m_counters.duplicates;
            return;
        }
        if (packet->destinationType == DestinationPlain && packet->destinationHash == pathRequestDestination())
            handlePathRequest(interfaceId, *packet);
        else if (m_packetHandler)
            m_packetHandler(interfaceId, *packet, raw, hops);
        break;
    case PacketLinkRequest:
        ++m_counters.linkRequests;
        if (fresh && m_packetHandler)
            m_packetHandler(interfaceId, *packet, raw, hops);
        break;
    case PacketProof:
        ++m_counters.proofs;
        if (fresh && m_packetHandler)
            m_packetHandler(interfaceId, *packet, raw, hops);
        break;
    default:
        break;
    }
    notifyChanged();
}

void Node::handleAnnounce(const QString &interfaceId, const Packet &packet, const QByteArray &, int hops)
{
    QString error;
    const std::optional<Announce> announce = parseAnnounce(packet, &error);
    if (!announce || !validateAnnounce(*announce, &error)) {
        ++m_counters.rejectedAnnounces;
        if (InterfaceRuntime *source = runtime(interfaceId))
            ++source->m_status.rejected;
        if (m_counters.rejectedAnnounces <= 20 || m_counters.rejectedAnnounces % 100 == 0)
            addEvent(QStringLiteral("rejected"),
                     QStringLiteral("Dropped an announce for %1 from %2: %3")
                         .arg(prettyHash(packet.destinationHash), interfaceName(interfaceId), error),
                     packet.destinationHash);
        return;
    }
    if (isLocalDestination(announce->destinationHash))
        return;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    auto found = m_destinations.find(announce->destinationHash);
    const bool known = found != m_destinations.end();
    if (known && found->publicKey != announce->publicKey) {
        ++m_counters.rejectedAnnounces;
        addEvent(QStringLiteral("rejected"),
                 QStringLiteral("Ignored an announce for %1 with a different key than before")
                     .arg(prettyHash(announce->destinationHash)),
                 announce->destinationHash);
        return;
    }
    if (known && announce->emittedAt < found->emittedAt && packet.context != ContextPathResponse) {
        ++m_counters.duplicates;
        return;
    }
    DestinationRecord record = known ? *found : DestinationRecord();
    record.hash = announce->destinationHash;
    record.identityHash = announce->identityHash;
    record.publicKey = announce->publicKey;
    record.nameHash = announce->nameHash;
    record.transportId = packet.transportId;
    record.appData = announce->appData.left(1024);
    record.ratchet = !announce->ratchet.isEmpty();
    if (record.ratchet)
        record.ratchetKey = announce->ratchet;
    if (const InterfaceRuntime *source = runtime(interfaceId))
        record.viaSharedInstance = source->config().kind == InterfaceKind::SharedInstance;
    record.emittedAt = qMax(record.emittedAt, announce->emittedAt);
    record.hops = hops;
    record.interfaceId = interfaceId;
    record.lastHeard = now;
    if (!known)
        record.firstHeard = now;
    ++record.announces;
    if (const std::optional<KnownAspect> aspect = knownAspect(announce->nameHash)) {
        record.aspect = aspect->name;
        record.label = aspect->label;
        record.kind = aspect->kind;
    } else {
        record.aspect.clear();
        record.label = QStringLiteral("Unknown application");
        record.kind = QStringLiteral("unknown");
    }
    record.displayName = displayNameFromAppData(record.kind, announce->appData);
    if (record.kind == QStringLiteral("discovery") && (!known || found->appData != record.appData))
        record.discovery = parseDiscoveryAppData(announce->appData);
    if (record.discovery && !record.discovery->name.isEmpty())
        record.displayName = record.discovery->name;
    m_destinations.insert(record.hash, record);
    if (m_announced)
        m_announced(record.hash);
    auto lookup = m_lookups.find(record.hash);
    if (lookup != m_lookups.end() && !lookup->answered.isValid()) {
        lookup->answered = now;
        lookup->hops = hops;
        lookup->interfaceId = interfaceId;
        lookup->timedOut = false;
        addEvent(QStringLiteral("path-found"),
                 QStringLiteral("Found a path to %1: %2 hop%3 via %4 in %5 ms")
                     .arg(record.displayName.isEmpty() ? prettyHash(record.hash) : record.displayName)
                     .arg(hops)
                     .arg(hops == 1 ? QString() : QStringLiteral("s"))
                     .arg(interfaceName(interfaceId))
                     .arg(lookup->started.msecsTo(now)),
                 record.hash);
    } else if (!known) {
        const QString who = record.displayName.isEmpty() ? prettyHash(record.hash)
                                                         : QStringLiteral("%1 %2").arg(record.displayName, prettyHash(record.hash));
        addEvent(QStringLiteral("announce"),
                 QStringLiteral("%1 · %2 · %3 hop%4 via %5")
                     .arg(who, record.label)
                     .arg(hops)
                     .arg(hops == 1 ? QString() : QStringLiteral("s"))
                     .arg(interfaceName(interfaceId)),
                 record.hash);
    }
    if (m_destinations.size() > kMaxDestinations)
        trimDestinations();
}

void Node::handlePathRequest(const QString &interfaceId, const Packet &packet)
{
    QByteArray destination;
    QByteArray tag;
    if (!parsePathRequest(packet, &destination, &tag))
        return;
    ++m_counters.pathRequestsSeen;
    if (!m_answerPathRequests || tag.isEmpty() || !isLocalDestination(destination) || !m_identity.hasPrivateKey())
        return;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QDateTime last = m_lastPathAnswers.value(destination);
    if (last.isValid() && last.secsTo(now) < 10)
        return;
    m_lastPathAnswers.insert(destination, now);
    if (announceDestination(destination, true, interfaceId)) {
        ++m_counters.pathRequestsAnswered;
        addEvent(QStringLiteral("path-answered"),
                 QStringLiteral("Answered a path request for %1 on %2").arg(localAspect(destination), interfaceName(interfaceId)),
                 destination);
    }
}

void Node::trimDestinations()
{
    QList<QPair<QDateTime, QByteArray>> ages;
    ages.reserve(m_destinations.size());
    for (auto it = m_destinations.cbegin(); it != m_destinations.cend(); ++it)
        ages.append({it->lastHeard, it.key()});
    std::sort(ages.begin(), ages.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    const int excess = int(m_destinations.size()) - kMaxDestinations + kMaxDestinations / 10;
    for (int i = 0; i < excess && i < ages.size(); ++i)
        m_destinations.remove(ages.at(i).second);
}

void Node::tick()
{
    m_history.append(m_current);
    if (m_history.size() > kHistorySeconds)
        m_history.remove(0, m_history.size() - kHistorySeconds);
    m_current = {};
    for (InterfaceRuntime *runtime : std::as_const(m_runtimes))
        runtime->rotateHistory();
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (auto it = m_lookups.begin(); it != m_lookups.end(); ++it) {
        if (!it->answered.isValid() && !it->timedOut && it->started.secsTo(now) >= kPathTimeoutSeconds) {
            it->timedOut = true;
            addEvent(QStringLiteral("path-timeout"),
                     QStringLiteral("No path to %1 within %2 s").arg(prettyHash(it->destination)).arg(kPathTimeoutSeconds),
                     it->destination);
        }
    }
    notifyChanged();
}

void Node::updateTicking()
{
    const bool needed = m_running && std::any_of(m_runtimes.cbegin(), m_runtimes.cend(), [](const InterfaceRuntime *r) {
                            return r->config().enabled;
                        });
    if (needed && !m_tickTimer->isActive())
        m_tickTimer->start();
    else if (!needed && m_tickTimer->isActive())
        m_tickTimer->stop();
}

}
