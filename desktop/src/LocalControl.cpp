#include "LocalControl.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMessageAuthenticationCode>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTcpSocket>
#include <QTimer>
#include <QUuid>
#include <QtEndian>

#include <openssl/evp.h>
#include <openssl/rand.h>

namespace forkmesh::localcontrol {

namespace {

constexpr int kKeyBytes = 32;
constexpr int kNonceBytes = 12;
constexpr int kTagBytes = 16;
constexpr int kClientNonceBytes = 16;
constexpr qsizetype kMaxFrameBytes = 4 * 1024 * 1024;
constexpr int kFileChunkBytes = 256 * 1024;
constexpr int kMaxNameChars = 80;
// A relayed protocol frame is reassembled from chunks; nothing the phone asks
// for over the relay needs more than this, and a larger one closes the channel.
constexpr qsizetype kMaxRelayFrameBytes = 1024 * 1024;
// Base64 characters per relayed chunk: app/src/node_control.py accepts up to
// 12000, and the whole frame has to fit a headless node's 16 KiB socket limit.
constexpr int kRelayChunkChars = 9000;

const QByteArray &protocolLabel()
{
    static const QByteArray value("forkmesh-local-control-v1");
    return value;
}

QByteArray sha256(const QByteArray &input)
{
    return QCryptographicHash::hash(input, QCryptographicHash::Sha256);
}

QByteArray hmac(const QByteArray &key, const QByteArray &message)
{
    return QMessageAuthenticationCode::hash(message, key,
                                            QCryptographicHash::Sha256);
}

QByteArray nonceFor(quint64 counter)
{
    QByteArray nonce(kNonceBytes, '\0');
    qToBigEndian(counter, reinterpret_cast<uchar *>(nonce.data()) + 4);
    return nonce;
}

} // namespace

QString helloTag() { return QStringLiteral("fm-control-hello"); }
QString welcomeTag() { return QStringLiteral("fm-control-welcome"); }
QString authTag() { return QStringLiteral("fm-control-auth"); }

bool isControlHello(const QByteArray &payload)
{
    // Cheap test first: every mesh frame is a RoomCrypto envelope, which never
    // carries this tag in the clear.
    if (!payload.contains("fm-control-hello"))
        return false;
    const QJsonObject object = QJsonDocument::fromJson(payload).object();
    return object.value(QStringLiteral("t")).toString() == helloTag() &&
           object.value(QStringLiteral("v")).toInt() == kProtocolVersion;
}

QByteArray transcript(const QString &clientKey, const QString &clientEph,
                      const QString &clientNonce, const QString &serverKey,
                      const QString &serverEph)
{
    return protocolLabel() + '\n' + clientKey.toLatin1() + '\n' +
           clientEph.toLatin1() + '\n' + clientNonce.toLatin1() + '\n' +
           serverKey.toLatin1() + '\n' + serverEph.toLatin1();
}

QByteArray serverSignedBytes(const QByteArray &transcriptBytes)
{
    return protocolLabel() + " server\n" + transcriptBytes;
}

QByteArray clientSignedBytes(const QByteArray &transcriptBytes)
{
    return protocolLabel() + " client\n" + transcriptBytes;
}

SessionKeys deriveKeys(const QByteArray &sharedSecret,
                       const QByteArray &transcriptBytes)
{
    SessionKeys keys;
    if (sharedSecret.size() != kKeyBytes || transcriptBytes.isEmpty())
        return keys;
    // HKDF-SHA256 (RFC 5869): extract with the transcript hash as salt, then
    // one 32-byte expand block per direction.
    const QByteArray prk = hmac(sha256(transcriptBytes), sharedSecret);
    keys.clientToServer = hmac(prk, protocolLabel() + " c2s" + '\x01');
    keys.serverToClient = hmac(prk, protocolLabel() + " s2c" + '\x01');
    return keys;
}

QString pairingCode(const QByteArray &transcriptBytes)
{
    const QByteArray digest = sha256(protocolLabel() + " pair\n" + transcriptBytes);
    const quint32 value =
        qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(digest.constData()));
    return QStringLiteral("%1").arg(value % 1000000u, 6, 10, QLatin1Char('0'));
}

QByteArray sealFrame(const QByteArray &key, quint64 counter,
                     const QByteArray &plain)
{
    if (key.size() != kKeyBytes)
        return {};
    const QByteArray nonce = nonceFor(counter);
    QByteArray out(plain.size() + kTagBytes, Qt::Uninitialized);
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int length = 0;
    int total = 0;
    bool ok =
        ctx &&
        EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kNonceBytes, nullptr) == 1 &&
        EVP_EncryptInit_ex(ctx, nullptr, nullptr,
                           reinterpret_cast<const uchar *>(key.constData()),
                           reinterpret_cast<const uchar *>(nonce.constData())) == 1 &&
        EVP_EncryptUpdate(ctx, nullptr, &length,
                          reinterpret_cast<const uchar *>(protocolLabel().constData()),
                          int(protocolLabel().size())) == 1 &&
        EVP_EncryptUpdate(ctx, reinterpret_cast<uchar *>(out.data()), &length,
                          reinterpret_cast<const uchar *>(plain.constData()),
                          int(plain.size())) == 1;
    if (ok) {
        total = length;
        ok = EVP_EncryptFinal_ex(ctx, reinterpret_cast<uchar *>(out.data()) + total,
                                 &length) == 1;
        total += length;
    }
    if (ok)
        ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, kTagBytes,
                                 out.data() + total) == 1;
    if (ctx)
        EVP_CIPHER_CTX_free(ctx);
    if (!ok)
        return {};
    out.resize(total + kTagBytes);
    return out;
}

QByteArray openFrame(const QByteArray &key, quint64 counter,
                     const QByteArray &frame)
{
    if (key.size() != kKeyBytes || frame.size() < kTagBytes)
        return {};
    const QByteArray nonce = nonceFor(counter);
    const qsizetype cipherBytes = frame.size() - kTagBytes;
    QByteArray tag = frame.right(kTagBytes);
    QByteArray plain(cipherBytes, Qt::Uninitialized);
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int length = 0;
    int total = 0;
    bool ok =
        ctx &&
        EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kNonceBytes, nullptr) == 1 &&
        EVP_DecryptInit_ex(ctx, nullptr, nullptr,
                           reinterpret_cast<const uchar *>(key.constData()),
                           reinterpret_cast<const uchar *>(nonce.constData())) == 1 &&
        EVP_DecryptUpdate(ctx, nullptr, &length,
                          reinterpret_cast<const uchar *>(protocolLabel().constData()),
                          int(protocolLabel().size())) == 1 &&
        EVP_DecryptUpdate(ctx, reinterpret_cast<uchar *>(plain.data()), &length,
                          reinterpret_cast<const uchar *>(frame.constData()),
                          int(cipherBytes)) == 1;
    if (ok) {
        total = length;
        ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, kTagBytes,
                                 tag.data()) == 1 &&
             EVP_DecryptFinal_ex(ctx, reinterpret_cast<uchar *>(plain.data()) + total,
                                 &length) == 1;
        total += length;
    }
    if (ctx)
        EVP_CIPHER_CTX_free(ctx);
    if (!ok)
        return {};
    plain.resize(total);
    // An empty body is never sent, so empty always means "did not open".
    return plain.isEmpty() ? QByteArray() : plain;
}

Ephemeral newEphemeral()
{
    Ephemeral out;
    EVP_PKEY *key = nullptr;
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr);
    if (ctx && EVP_PKEY_keygen_init(ctx) == 1 && EVP_PKEY_keygen(ctx, &key) == 1) {
        QByteArray priv(kKeyBytes, Qt::Uninitialized);
        QByteArray pub(kKeyBytes, Qt::Uninitialized);
        size_t privLength = kKeyBytes;
        size_t pubLength = kKeyBytes;
        if (EVP_PKEY_get_raw_private_key(key, reinterpret_cast<uchar *>(priv.data()),
                                         &privLength) == 1 &&
            EVP_PKEY_get_raw_public_key(key, reinterpret_cast<uchar *>(pub.data()),
                                        &pubLength) == 1 &&
            privLength == kKeyBytes && pubLength == kKeyBytes) {
            out.privateKey = priv;
            out.publicKey = pub;
        }
    }
    if (key)
        EVP_PKEY_free(key);
    if (ctx)
        EVP_PKEY_CTX_free(ctx);
    return out;
}

QByteArray x25519(const QByteArray &privateKey, const QByteArray &peerPublic)
{
    if (privateKey.size() != kKeyBytes || peerPublic.size() != kKeyBytes)
        return {};
    EVP_PKEY *mine = EVP_PKEY_new_raw_private_key(
        EVP_PKEY_X25519, nullptr,
        reinterpret_cast<const uchar *>(privateKey.constData()), kKeyBytes);
    EVP_PKEY *peer = EVP_PKEY_new_raw_public_key(
        EVP_PKEY_X25519, nullptr,
        reinterpret_cast<const uchar *>(peerPublic.constData()), kKeyBytes);
    EVP_PKEY_CTX *ctx = mine ? EVP_PKEY_CTX_new(mine, nullptr) : nullptr;
    QByteArray shared(kKeyBytes, Qt::Uninitialized);
    size_t length = kKeyBytes;
    const bool ok = mine && peer && ctx && EVP_PKEY_derive_init(ctx) == 1 &&
                    EVP_PKEY_derive_set_peer(ctx, peer) == 1 &&
                    EVP_PKEY_derive(ctx, reinterpret_cast<uchar *>(shared.data()),
                                    &length) == 1 &&
                    length == kKeyBytes;
    if (ctx)
        EVP_PKEY_CTX_free(ctx);
    if (peer)
        EVP_PKEY_free(peer);
    if (mine)
        EVP_PKEY_free(mine);
    if (!ok || shared == QByteArray(kKeyBytes, '\0'))
        return {};
    return shared;
}

bool verifyEd25519(const QString &publicKeyB64url,
                   const QString &signatureB64url, const QByteArray &data)
{
    const QByteArray key = fromBase64Url(publicKeyB64url, kKeyBytes);
    const QByteArray sig = fromBase64Url(signatureB64url, 64);
    if (key.isEmpty() || sig.isEmpty())
        return false;
    EVP_PKEY *pkey = EVP_PKEY_new_raw_public_key(
        EVP_PKEY_ED25519, nullptr,
        reinterpret_cast<const uchar *>(key.constData()), kKeyBytes);
    if (!pkey)
        return false;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    const bool ok =
        ctx && EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, pkey) == 1 &&
        EVP_DigestVerify(ctx, reinterpret_cast<const uchar *>(sig.constData()),
                         size_t(sig.size()),
                         reinterpret_cast<const uchar *>(data.constData()),
                         size_t(data.size())) == 1;
    if (ctx)
        EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return ok;
}

QString toBase64Url(const QByteArray &bytes)
{
    return QString::fromLatin1(bytes.toBase64(QByteArray::Base64UrlEncoding |
                                              QByteArray::OmitTrailingEquals));
}

QByteArray fromBase64Url(const QString &text, int bytes)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_-]+$"));
    if (text.isEmpty() || text.size() > 200 || !pattern.match(text).hasMatch())
        return {};
    const QByteArray decoded =
        QByteArray::fromBase64(text.toLatin1(), QByteArray::Base64UrlEncoding);
    return decoded.size() == bytes ? decoded : QByteArray();
}

QString safeFileName(const QString &name)
{
    QString base = QFileInfo(QString(name).replace(QLatin1Char('\\'),
                                                   QLatin1Char('/')))
                       .fileName();
    QString out;
    out.reserve(base.size());
    for (const QChar ch : std::as_const(base)) {
        if (ch.isLetterOrNumber() || ch == QLatin1Char('.') ||
            ch == QLatin1Char('-') || ch == QLatin1Char('_') ||
            ch == QLatin1Char(' ') || ch == QLatin1Char('(') ||
            ch == QLatin1Char(')'))
            out.append(ch);
        else
            out.append(QLatin1Char('_'));
    }
    out = out.trimmed();
    while (out.startsWith(QLatin1Char('.')))
        out.remove(0, 1);
    if (out.size() > 120)
        out = out.right(120);
    return out.isEmpty() ? QStringLiteral("file") : out;
}

qsizetype maxFrameBytes() { return kMaxFrameBytes; }
int fileChunkBytes() { return kFileChunkBytes; }
qsizetype maxRelayFrameBytes() { return kMaxRelayFrameBytes; }
int relayChunkChars() { return kRelayChunkChars; }

} // namespace forkmesh::localcontrol

using namespace forkmesh::localcontrol;

namespace {

constexpr int kMaxConnections = 8;
constexpr int kHandshakeTimeoutMs = 15000;
constexpr int kMaintenanceIntervalMs = 60 * 1000;
constexpr qint64 kRelayIdleMs = 15 * 60 * 1000;
constexpr qint64 kRelayEvictIdleMs = 60 * 1000;
constexpr qint64 kRelayOpeningTimeoutMs = 30 * 1000;
constexpr int kMaxRelayOpenings = 16;
constexpr int kMaxRelayNotes = 64;
constexpr qint64 kAccountRecheckMs = 10 * 60 * 1000;
constexpr qint64 kAccountRetryMs = 2 * 60 * 1000;
constexpr qint64 kAccountGraceMs = 24ll * 60 * 60 * 1000;

const QString &accountApproval()
{
    static const QString value = QStringLiteral("account");
    return value;
}

const QString &codeApproval()
{
    static const QString value = QStringLiteral("code");
    return value;
}

void noteBounded(QStringList &list, const QString &value)
{
    if (list.contains(value))
        return;
    list.append(value);
    while (list.size() > kMaxRelayNotes)
        list.removeFirst();
}

constexpr qint64 kMaxFileBytes = 2ll * 1024 * 1024 * 1024;
constexpr qint64 kOutgoingHighWater = 1024 * 1024;

QByteArray frame(const QByteArray &payload)
{
    QByteArray out(4, '\0');
    qToBigEndian(quint32(payload.size()), reinterpret_cast<uchar *>(out.data()));
    return out + payload;
}

// Returns 1 with *payload for a complete frame, 0 for "not yet", -1 for an
// impossible length.
int takeFrame(QByteArray &buffer, QByteArray *payload)
{
    if (buffer.size() < 4)
        return 0;
    const quint32 length =
        qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(buffer.constData()));
    if (length == 0 || qsizetype(length) > maxFrameBytes())
        return -1;
    if (buffer.size() < qsizetype(length) + 4)
        return 0;
    *payload = buffer.mid(4, length);
    buffer.remove(0, qsizetype(length) + 4);
    return 1;
}

QString boundedText(const QJsonObject &object, const char *key,
                    int max = kMaxNameChars)
{
    return object.value(QLatin1String(key)).toString().trimmed().left(max);
}

QString uniquePath(const QString &dir, const QString &name)
{
    const QFileInfo info(name);
    const QString stem = info.completeBaseName();
    const QString suffix = info.suffix().isEmpty()
                               ? QString()
                               : QStringLiteral(".") + info.suffix();
    QString candidate = QDir(dir).filePath(name);
    for (int n = 2; QFileInfo::exists(candidate) ||
                    QFileInfo::exists(candidate + QStringLiteral(".part"));
         ++n)
        candidate = QDir(dir).filePath(
            QStringLiteral("%1 (%2)%3").arg(stem).arg(n).arg(suffix));
    return candidate;
}

} // namespace

LocalControlServer::LocalControlServer(
    const QString &publicKey, std::function<QString(const QByteArray &)> signer,
    QObject *parent)
    : QObject(parent), m_publicKey(publicKey), m_signer(std::move(signer))
{
    m_maintenanceTimer = new QTimer(this);
    m_maintenanceTimer->setInterval(kMaintenanceIntervalMs);
    connect(m_maintenanceTimer, &QTimer::timeout, this,
            [this] { maintain(QDateTime::currentMSecsSinceEpoch()); });
    m_maintenanceTimer->start();
}

LocalControlServer::~LocalControlServer()
{
    onPairingRequested = nullptr;
    onPairingClosed = nullptr;
    onDevicesChanged = nullptr;
    onConnectionClosed = nullptr;
    const QList<Connection *> connections = m_connections;
    for (Connection *conn : connections)
        closeConnection(conn->id, QString());
}

void LocalControlServer::setDesktopInfo(const QString &name, const QString &user)
{
    m_name = name.left(kMaxNameChars);
    m_user = user.left(kMaxNameChars);
}

void LocalControlServer::setDeviceStorePath(const QString &path)
{
    m_storePath = path;
    loadDevices();
}

void LocalControlServer::setReceiveDirectory(const QString &path)
{
    m_receiveDir = path;
}

QList<LocalControlServer::Device> LocalControlServer::pairedDevices() const
{
    QList<Device> out = m_devices.values();
    std::sort(out.begin(), out.end(), [](const Device &a, const Device &b) {
        return a.pairedAtMs < b.pairedAtMs;
    });
    return out;
}

bool LocalControlServer::isPaired(const QString &key) const
{
    return m_devices.contains(key);
}

QList<LocalControlServer::Device> LocalControlServer::connectedDevices() const
{
    QList<Device> out;
    for (const Connection *conn : m_connections)
        if (conn->state != State::AwaitAuth)
            out.append(conn->device);
    return out;
}

bool LocalControlServer::isConnected(const QString &key) const
{
    for (const Connection *conn : m_connections)
        if (conn->state == State::Paired && conn->device.key == key)
            return true;
    return false;
}

void LocalControlServer::forget(const QString &key)
{
    if (!m_devices.remove(key))
        return;
    saveDevices();
    const QList<Connection *> connections = m_connections;
    for (Connection *conn : connections)
        if (conn->device.key == key)
            closeConnection(conn->id, QStringLiteral("forgotten"));
    if (onDevicesChanged)
        onDevicesChanged();
}

LocalControlServer::Connection *LocalControlServer::connection(int id)
{
    for (Connection *conn : std::as_const(m_connections))
        if (conn->id == id)
            return conn;
    return nullptr;
}

const LocalControlServer::Connection *LocalControlServer::connection(int id) const
{
    for (const Connection *conn : m_connections)
        if (conn->id == id)
            return conn;
    return nullptr;
}

LocalControlServer::Connection *
LocalControlServer::connectionForRelay(const QString &session)
{
    for (Connection *conn : std::as_const(m_connections))
        if (conn->relaySession == session)
            return conn;
    return nullptr;
}

bool LocalControlServer::isRelayed(int connectionId) const
{
    const Connection *conn = connection(connectionId);
    return conn && conn->relayed();
}

LocalControlServer::Device LocalControlServer::deviceFor(int connectionId) const
{
    const Connection *conn = connection(connectionId);
    return conn && conn->state != State::AwaitAuth ? conn->device : Device();
}

bool LocalControlServer::linkOpen(const Connection &conn) const
{
    if (conn.relayed())
        return bool(m_relaySink);
    return conn.socket &&
           conn.socket->state() == QAbstractSocket::ConnectedState;
}

bool LocalControlServer::sendRelay(const QJsonObject &frame)
{
    return m_relaySink && m_relaySink(frame);
}

bool LocalControlServer::writeFrame(Connection &conn, const QByteArray &payload)
{
    if (!conn.relayed()) {
        if (!conn.socket)
            return false;
        conn.socket->write(frame(payload));
        return true;
    }
    if (!m_relaySink || payload.size() > kMaxRelayFrameBytes)
        return false;
    // One protocol frame, split into chunks the relay accepts; "more" joins
    // them back together on the phone.
    const QByteArray encoded = payload.toBase64();
    for (qsizetype at = 0; at < encoded.size(); at += kRelayChunkChars) {
        const bool more = at + kRelayChunkChars < encoded.size();
        const QJsonObject chunk{
            {QStringLiteral("type"), QStringLiteral("control")},
            {QStringLiteral("action"), QStringLiteral("data")},
            {QStringLiteral("session"), conn.relaySession},
            {QStringLiteral("node"), m_relayNode},
            {QStringLiteral("seq"), qint64(conn.relaySendSeq++)},
            {QStringLiteral("data"),
             QString::fromLatin1(encoded.mid(at, kRelayChunkChars))},
            {QStringLiteral("more"), more},
        };
        if (!sendRelay(chunk))
            return false;
    }
    return true;
}

void LocalControlServer::startHandshakeTimer(Connection &conn)
{
    const int id = conn.id;
    conn.handshakeTimer = new QTimer(this);
    conn.handshakeTimer->setSingleShot(true);
    connect(conn.handshakeTimer, &QTimer::timeout, this, [this, id] {
        Connection *c = connection(id);
        if (c && c->state == State::AwaitAuth)
            closeConnection(id, QStringLiteral("handshake timed out"));
    });
    conn.handshakeTimer->start(kHandshakeTimeoutMs);
}

void LocalControlServer::setRelaySink(
    std::function<bool(const QJsonObject &frame)> sink, const QString &node)
{
    m_relaySink = std::move(sink);
    m_relayNode = node.trimmed().toLower();
}

void LocalControlServer::relayFrame(const QJsonObject &frameObject)
{
    if (!m_relaySink || m_relayNode.isEmpty() || m_publicKey.isEmpty() ||
        !m_signer)
        return;
    if (frameObject.value(QStringLiteral("from")).toString() !=
            QLatin1String("browser") ||
        frameObject.value(QStringLiteral("node")).toString().trimmed().toLower() !=
            m_relayNode)
        return;
    const QString session =
        frameObject.value(QStringLiteral("session")).toString();
    const QString action = frameObject.value(QStringLiteral("action")).toString();
    if (session.size() < 16 || session.size() > 64)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const auto refuse = [this, &session](const QString &reason) {
        m_relayOpening.remove(session);
        m_relayOpeningSeq.remove(session);
        m_relayOpeningSince.remove(session);
        noteBounded(m_relayToldGone, session);
        sendRelay({{QStringLiteral("type"), QStringLiteral("control")},
                   {QStringLiteral("action"), QStringLiteral("error")},
                   {QStringLiteral("session"), session},
                   {QStringLiteral("node"), m_relayNode},
                   {QStringLiteral("reason"), reason}});
    };
    Connection *conn = connectionForRelay(session);
    if (action == QLatin1String("close")) {
        m_relayOpening.remove(session);
        m_relayOpeningSeq.remove(session);
        m_relayOpeningSince.remove(session);
        m_relayUndelivered.removeAll(session);
        noteBounded(m_relayToldGone, session);
        if (conn) {
            // The phone left; nothing to tell it.
            conn->relaySession.clear();
            closeConnection(conn->id, QString());
        }
        return;
    }
    const QByteArray chunk = QByteArray::fromBase64(
        frameObject.value(QStringLiteral("data")).toString().toLatin1(),
        QByteArray::AbortOnBase64DecodingErrors);
    const bool more = frameObject.value(QStringLiteral("more")).toBool();
    if (chunk.isEmpty())
        return;
    // Chunk sequence numbers run per direction over the whole channel, the
    // hello's own chunks included, starting at 0.
    const qint64 seqValue =
        qint64(frameObject.value(QStringLiteral("seq")).toDouble(-1));
    if (action == QLatin1String("open")) {
        if (conn)
            return; // a duplicate open of a live channel
        if (!m_relayOpening.contains(session)) {
            if (m_relayOpening.size() >= kMaxRelayOpenings) {
                refuse(QStringLiteral("This desktop has too many channels opening."));
                return;
            }
            m_relayOpeningSince.insert(session, now);
        }
        QByteArray &pending = m_relayOpening[session];
        if (seqValue != qint64(m_relayOpeningSeq.value(session, 0))) {
            refuse(QStringLiteral("The hello arrived out of order."));
            m_relayOpeningSeq.remove(session);
            return;
        }
        m_relayOpeningSeq[session] = quint32(seqValue + 1);
        pending.append(chunk);
        if (pending.size() > 64 * 1024) {
            refuse(QStringLiteral("The hello is too large."));
            return;
        }
        if (more)
            return;
        const QByteArray hello = m_relayOpening.take(session);
        const quint32 nextSeq = m_relayOpeningSeq.take(session);
        m_relayOpeningSince.remove(session);
        if (!isControlHello(hello)) {
            refuse(QStringLiteral("That is not a ForkMesh control hello."));
            return;
        }
        if (m_connections.size() >= kMaxConnections && !evictIdleRelayed(now)) {
            refuse(QStringLiteral("This desktop has too many open channels."));
            return;
        }
        auto *created = new Connection;
        created->id = m_nextId++;
        created->relaySession = session;
        created->relayReceiveSeq = nextSeq;
        created->lastHeardMs = now;
        m_connections.append(created);
        if (!acceptHello(*created, hello)) {
            refuse(QStringLiteral("The hello did not verify."));
            created->relaySession.clear();
            closeConnection(created->id, QStringLiteral("bad hello"));
            return;
        }
        startHandshakeTimer(*created);
        return;
    }
    if (action != QLatin1String("data"))
        return;
    if (!conn) {
        m_relayOpening.remove(session);
        m_relayOpeningSeq.remove(session);
        m_relayOpeningSince.remove(session);
        tellRelayGone(session, QStringLiteral("channel_gone"));
        return;
    }
    conn->lastHeardMs = now;
    if (seqValue != qint64(conn->relayReceiveSeq)) {
        // A lost or reordered chunk breaks the frame counter the keys depend
        // on, so the channel cannot continue; the phone reconnects.
        closeConnection(conn->id, QStringLiteral("relay chunk out of order"));
        return;
    }
    ++conn->relayReceiveSeq;
    conn->relayPending.append(chunk);
    if (conn->relayPending.size() > kMaxRelayFrameBytes) {
        closeConnection(conn->id, QStringLiteral("oversized frame"));
        return;
    }
    if (more)
        return;
    const QByteArray payload = conn->relayPending;
    conn->relayPending.clear();
    handleFrame(*conn, payload);
}

void LocalControlServer::tellRelayGone(const QString &session,
                                       const QString &reason)
{
    if (m_relayToldGone.contains(session))
        return;
    noteBounded(m_relayToldGone, session);
    sendRelay({{QStringLiteral("type"), QStringLiteral("control")},
               {QStringLiteral("action"), QStringLiteral("close")},
               {QStringLiteral("session"), session},
               {QStringLiteral("node"), m_relayNode},
               {QStringLiteral("reason"), reason}});
}

void LocalControlServer::relayRestored()
{
    const QStringList sessions = m_relayUndelivered;
    m_relayUndelivered.clear();
    for (const QString &session : sessions) {
        m_relayToldGone.removeAll(session);
        tellRelayGone(session, QStringLiteral("relay_reconnected"));
    }
}

bool LocalControlServer::evictIdleRelayed(qint64 nowMs)
{
    Connection *oldest = nullptr;
    for (Connection *conn : std::as_const(m_connections))
        if (conn->relayed() && nowMs - conn->lastHeardMs >= kRelayEvictIdleMs &&
            (!oldest || conn->lastHeardMs < oldest->lastHeardMs))
            oldest = conn;
    if (!oldest)
        return false;
    closeConnection(oldest->id, QStringLiteral("idle"));
    return true;
}

void LocalControlServer::maintain(qint64 nowMs)
{
    for (auto it = m_relayOpeningSince.begin(); it != m_relayOpeningSince.end();) {
        if (nowMs - it.value() >= kRelayOpeningTimeoutMs) {
            m_relayOpening.remove(it.key());
            m_relayOpeningSeq.remove(it.key());
            it = m_relayOpeningSince.erase(it);
        } else {
            ++it;
        }
    }
    QList<int> idle;
    QList<int> recheck;
    for (const Connection *conn : std::as_const(m_connections)) {
        if (conn->state != State::Paired)
            continue;
        if (conn->relayed() && nowMs - conn->lastHeardMs >= kRelayIdleMs)
            idle.append(conn->id);
        else if (!conn->accountCheckInFlight && needsAccountCheck(*conn) &&
                 nowMs - conn->accountCheckedMs >= kAccountRecheckMs)
            recheck.append(conn->id);
    }
    for (int id : std::as_const(idle))
        closeConnection(id, QStringLiteral("idle"));
    for (int id : std::as_const(recheck))
        recheckAccount(id);
}

bool LocalControlServer::needsAccountCheck(const Connection &conn) const
{
    if (!accountDeviceCheck)
        return false;
    if (conn.device.platform == QLatin1String("web"))
        return true;
    const auto it = m_devices.constFind(conn.device.key);
    if (it == m_devices.cend() || it->approval == accountApproval())
        return true;
    return it->approval.isEmpty() && conn.relayed();
}

void LocalControlServer::recheckAccount(int connectionId)
{
    Connection *conn = connection(connectionId);
    if (!conn || !accountDeviceCheck)
        return;
    conn->accountCheckInFlight = true;
    const QString key = conn->device.key;
    QPointer<LocalControlServer> self(this);
    accountDeviceCheck(conn->device, [self, connectionId, key](AccountCheck verdict) {
        if (!self)
            return;
        Connection *c = self->connection(connectionId);
        if (!c || c->device.key != key)
            return;
        c->accountCheckInFlight = false;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (verdict == AccountCheck::Unreachable) {
            c->accountCheckedMs = now - kAccountRecheckMs + kAccountRetryMs;
            return;
        }
        if (verdict == AccountCheck::Owned) {
            c->accountCheckedMs = now;
            const auto it = self->m_devices.find(key);
            if (it != self->m_devices.end()) {
                it->accountCheckedMs = now;
                self->saveDevices();
            }
            return;
        }
        const auto it = self->m_devices.find(key);
        if (it != self->m_devices.end() && it->approval == accountApproval()) {
            self->m_devices.erase(it);
            self->saveDevices();
        }
        self->closeConnection(connectionId, QStringLiteral("revoked"));
    });
}

LocalControlServer::Connection *
LocalControlServer::connectionForSocket(QTcpSocket *socket)
{
    for (Connection *conn : std::as_const(m_connections))
        if (conn->socket == socket)
            return conn;
    return nullptr;
}

void LocalControlServer::adopt(QTcpSocket *socket, const QByteArray &hello,
                               const QByteArray &rest)
{
    if (!socket)
        return;
    socket->setParent(this);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if ((m_connections.size() >= kMaxConnections && !evictIdleRelayed(now)) ||
        m_publicKey.isEmpty() || !m_signer) {
        socket->abort();
        socket->deleteLater();
        return;
    }
    socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    auto *conn = new Connection;
    conn->id = m_nextId++;
    conn->socket = socket;
    conn->lastHeardMs = now;
    m_connections.append(conn);
    const int id = conn->id;
    if (!acceptHello(*conn, hello)) {
        closeConnection(id, QStringLiteral("bad hello"));
        return;
    }
    startHandshakeTimer(*conn);
    connect(socket, &QTcpSocket::readyRead, this,
            [this, socket] { onReadyRead(socket); });
    connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
        if (Connection *c = connectionForSocket(socket))
            closeConnection(c->id, QString());
    });
    connect(socket, &QTcpSocket::errorOccurred, this,
            [this, socket](QAbstractSocket::SocketError) {
                if (Connection *c = connectionForSocket(socket))
                    closeConnection(c->id, socket->errorString());
            });
    connect(socket, &QTcpSocket::bytesWritten, this, [this, socket](qint64) {
        if (Connection *c = connectionForSocket(socket))
            pumpOutgoing(*c);
    });
    conn->buffer = rest + socket->readAll();
    if (!conn->buffer.isEmpty())
        QTimer::singleShot(0, this, [this, socket] { onReadyRead(socket); });
}

bool LocalControlServer::acceptHello(Connection &conn, const QByteArray &hello)
{
    const QJsonObject object = QJsonDocument::fromJson(hello).object();
    if (object.value(QStringLiteral("t")).toString() != helloTag() ||
        object.value(QStringLiteral("v")).toInt() != kProtocolVersion)
        return false;
    const QString clientKey = object.value(QStringLiteral("key")).toString();
    const QString clientEph = object.value(QStringLiteral("eph")).toString();
    const QString clientNonce = object.value(QStringLiteral("nonce")).toString();
    const QByteArray clientEphRaw = fromBase64Url(clientEph, 32);
    if (fromBase64Url(clientKey, 32).isEmpty() || clientEphRaw.isEmpty() ||
        fromBase64Url(clientNonce, kClientNonceBytes).isEmpty() ||
        clientKey == m_publicKey)
        return false;

    const Ephemeral eph = newEphemeral();
    const QByteArray shared = x25519(eph.privateKey, clientEphRaw);
    if (shared.isEmpty())
        return false;
    const QString serverEph = toBase64Url(eph.publicKey);
    conn.transcript =
        transcript(clientKey, clientEph, clientNonce, m_publicKey, serverEph);
    const SessionKeys keys = deriveKeys(shared, conn.transcript);
    if (!keys.isValid())
        return false;
    conn.sendKey = keys.serverToClient;
    conn.receiveKey = keys.clientToServer;
    conn.device.key = clientKey;
    conn.device.name = boundedText(object, "name");
    conn.device.user = boundedText(object, "user");
    conn.device.platform = boundedText(object, "platform", 16);

    const QString sig = m_signer(serverSignedBytes(conn.transcript));
    if (sig.isEmpty())
        return false;
    const QJsonObject welcome{
        {QStringLiteral("t"), welcomeTag()},
        {QStringLiteral("v"), kProtocolVersion},
        {QStringLiteral("key"), m_publicKey},
        {QStringLiteral("eph"), serverEph},
        {QStringLiteral("name"), m_name},
        {QStringLiteral("user"), m_user},
        {QStringLiteral("sig"), sig},
    };
    return writeFrame(conn, QJsonDocument(welcome).toJson(QJsonDocument::Compact));
}

void LocalControlServer::onReadyRead(QTcpSocket *socket)
{
    Connection *conn = connectionForSocket(socket);
    if (!conn)
        return;
    const int id = conn->id;
    conn->buffer.append(socket->readAll());
    conn->lastHeardMs = QDateTime::currentMSecsSinceEpoch();
    while (true) {
        conn = connection(id);
        if (!conn)
            return;
        QByteArray payload;
        const int taken = takeFrame(conn->buffer, &payload);
        if (taken == 0)
            return;
        if (taken < 0) {
            closeConnection(id, QStringLiteral("oversized frame"));
            return;
        }
        handleFrame(*conn, payload);
    }
}

void LocalControlServer::handleFrame(Connection &conn, const QByteArray &payload)
{
    if (conn.state == State::AwaitAuth) {
        handleAuth(conn, payload);
        return;
    }
    const QByteArray plain =
        openFrame(conn.receiveKey, conn.receiveCounter, payload);
    if (plain.isEmpty()) {
        closeConnection(conn.id, QStringLiteral("frame did not authenticate"));
        return;
    }
    ++conn.receiveCounter;
    const QJsonDocument document = QJsonDocument::fromJson(plain);
    if (!document.isObject()) {
        closeConnection(conn.id, QStringLiteral("malformed message"));
        return;
    }
    handleMessage(conn, document.object());
}

void LocalControlServer::handleAuth(Connection &conn, const QByteArray &payload)
{
    const QJsonObject object = QJsonDocument::fromJson(payload).object();
    const QString sig = object.value(QStringLiteral("sig")).toString();
    if (object.value(QStringLiteral("t")).toString() != authTag() ||
        !verifyEd25519(conn.device.key, sig, clientSignedBytes(conn.transcript))) {
        closeConnection(conn.id, QStringLiteral("authentication failed"));
        return;
    }
    if (conn.handshakeTimer) {
        conn.handshakeTimer->deleteLater();
        conn.handshakeTimer = nullptr;
    }
    // One live channel per phone: a reconnect replaces the one it left behind.
    const QList<Connection *> connections = m_connections;
    for (Connection *other : connections)
        if (other != &conn && other->state != State::AwaitAuth &&
            other->device.key == conn.device.key)
            closeConnection(other->id, QStringLiteral("replaced"));

    const bool browser = conn.device.platform == QLatin1String("web");
    if (browser && m_devices.remove(conn.device.key))
        saveDevices();
    if (!browser && m_devices.contains(conn.device.key) &&
        !needsAccountCheck(conn)) {
        admitStored(conn);
    } else if (accountDeviceCheck) {
        conn.state = State::Checking;
        conn.accountCheckInFlight = true;
        const int id = conn.id;
        const QString key = conn.device.key;
        const Device device = conn.device;
        QPointer<LocalControlServer> self(this);
        if (onDevicesChanged)
            onDevicesChanged();
        accountDeviceCheck(device, [self, id, key](AccountCheck verdict) {
            if (!self)
                return;
            Connection *c = self->connection(id);
            if (!c || c->state != State::Checking || c->device.key != key)
                return;
            c->accountCheckInFlight = false;
            self->settleAccountCheck(*c, verdict);
            if (self->onDevicesChanged)
                self->onDevicesChanged();
        });
        return;
    } else {
        const int id = conn.id;
        requestPairing(conn);
        if (!connection(id))
            return;
    }
    if (onDevicesChanged)
        onDevicesChanged();
}

void LocalControlServer::admitStored(Connection &conn)
{
    Device stored = m_devices.value(conn.device.key);
    stored.name = conn.device.name.isEmpty() ? stored.name : conn.device.name;
    stored.user = conn.device.user.isEmpty() ? stored.user : conn.device.user;
    stored.platform = conn.device.platform;
    stored.lastSeenMs = QDateTime::currentMSecsSinceEpoch();
    conn.device = stored;
    rememberDevice(stored);
    conn.state = State::Paired;
    sendEvent(conn, QStringLiteral("session"), sessionInfo(conn));
}

void LocalControlServer::settleAccountCheck(Connection &conn, AccountCheck verdict)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool browser = conn.device.platform == QLatin1String("web");
    auto it = m_devices.find(conn.device.key);
    bool stored = !browser && it != m_devices.end();
    if (verdict == AccountCheck::Owned) {
        conn.accountCheckedMs = now;
        if (!stored) {
            pairDevice(conn, accountApproval());
            return;
        }
        if (it->approval.isEmpty())
            it->approval = accountApproval();
        it->accountCheckedMs = now;
        admitStored(conn);
        return;
    }
    if (verdict == AccountCheck::NotOwned) {
        if (stored && it->approval == accountApproval()) {
            m_devices.erase(it);
            saveDevices();
            stored = false;
        }
        if (stored && !conn.relayed())
            admitStored(conn);
        else
            requestPairing(conn);
        return;
    }
    if (stored && (!conn.relayed() || now - it->accountCheckedMs < kAccountGraceMs)) {
        admitStored(conn);
        return;
    }
    if (conn.relayed()) {
        refuseRelayed(conn, QStringLiteral("account_check_failed"));
        return;
    }
    requestPairing(conn);
}

void LocalControlServer::refuseRelayed(Connection &conn, const QString &reason)
{
    conn.state = State::Pending;
    sendEvent(conn, QStringLiteral("pairing-denied"),
              {{QStringLiteral("reason"), reason}});
    closeConnection(conn.id, reason);
}

void LocalControlServer::requestPairing(Connection &conn)
{
    conn.state = State::Pending;
    if (conn.relayed()) {
        sendEvent(conn, QStringLiteral("pairing-denied"),
                  {{QStringLiteral("reason"), QStringLiteral("pair_on_lan")}});
        closeConnection(conn.id, QStringLiteral(
            "Approve this phone on the desktop's own network first, or sign "
            "both in to the same ForkMesh account."));
        return;
    }
    conn.code = pairingCode(conn.transcript);
    sendEvent(conn, QStringLiteral("session"), sessionInfo(conn));
    if (onPairingRequested)
        onPairingRequested(conn.id, conn.device, conn.code);
}

void LocalControlServer::pairDevice(Connection &conn, const QString &approval)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    conn.device.pairedAtMs = now;
    conn.device.lastSeenMs = now;
    conn.device.approval = approval;
    conn.device.accountCheckedMs = approval == accountApproval() ? now : 0;
    if (conn.device.platform != QLatin1String("web"))
        rememberDevice(conn.device);
    conn.state = State::Paired;
    conn.code.clear();
    sendEvent(conn, QStringLiteral("session"), sessionInfo(conn));
}

QJsonObject LocalControlServer::sessionInfo(const Connection &conn) const
{
    QJsonObject info{
        {QStringLiteral("paired"), conn.state == State::Paired},
        {QStringLiteral("name"), m_name},
        {QStringLiteral("user"), m_user},
        {QStringLiteral("key"), m_publicKey},
        {QStringLiteral("protocol"), kProtocolVersion},
        {QStringLiteral("transport"),
         conn.relayed() ? QStringLiteral("relay") : QStringLiteral("lan")},
    };
    if (conn.state == State::Pending)
        info.insert(QStringLiteral("code"), conn.code);
    return info;
}

void LocalControlServer::approvePairing(int connectionId)
{
    Connection *conn = connection(connectionId);
    if (!conn || conn->state != State::Pending)
        return;
    pairDevice(*conn, codeApproval());
    if (onPairingClosed)
        onPairingClosed(connectionId);
    if (onDevicesChanged)
        onDevicesChanged();
}

void LocalControlServer::denyPairing(int connectionId)
{
    Connection *conn = connection(connectionId);
    if (!conn || conn->state != State::Pending)
        return;
    sendEvent(*conn, QStringLiteral("pairing-denied"));
    if (conn->socket)
        conn->socket->flush();
    closeConnection(connectionId, QStringLiteral("pairing denied"));
}

void LocalControlServer::handleMessage(Connection &conn, const QJsonObject &message)
{
    const int requestId = message.value(QStringLiteral("id")).toInt();
    const QString method = message.value(QStringLiteral("method")).toString();
    if (requestId <= 0 || method.isEmpty())
        return;
    const QJsonObject params = message.value(QStringLiteral("params")).toObject();
    if (method == QLatin1String("ping")) {
        respond(conn.id, requestId, {});
        return;
    }
    if (conn.state != State::Paired) {
        respondError(conn.id, requestId, QStringLiteral("not_paired"));
        return;
    }
    if (const QString family = familyFor(method); !family.isEmpty()) {
        const FamilyHandler handler = m_families.value(family);
        handler(conn.id, conn.device, requestId, method, params);
        return;
    }
    if (method.startsWith(QLatin1String("files."))) {
        if (conn.relayed()) {
            // Files stay a same-network convenience: the relay is sized for
            // requests and log lines, not for moving gigabytes.
            respondError(conn.id, requestId, QStringLiteral("lan_only"));
            return;
        }
        handleFileRequest(conn, requestId, method, params);
        return;
    }
    if (!onRequest) {
        respondError(conn.id, requestId, QStringLiteral("unsupported"));
        return;
    }
    onRequest(conn.id, conn.device, requestId, method, params);
}

bool LocalControlServer::registerFamily(const QString &prefix, FamilyHandler handler)
{
    if (prefix.size() < 2 || !prefix.endsWith(QLatin1Char('.')) || !handler)
        return false;
    m_families.insert(prefix, std::move(handler));
    return true;
}

QString LocalControlServer::familyFor(const QString &method) const
{
    QString best;
    for (auto it = m_families.constBegin(); it != m_families.constEnd(); ++it) {
        if (method.startsWith(it.key()) && method.size() > it.key().size() &&
            it.key().size() > best.size())
            best = it.key();
    }
    return best;
}

void LocalControlServer::handleFileRequest(Connection &conn, int requestId,
                                           const QString &method,
                                           const QJsonObject &params)
{
    const QString transfer = params.value(QStringLiteral("transfer")).toString();
    if (method == QLatin1String("files.begin")) {
        const qint64 size = qint64(params.value(QStringLiteral("size")).toDouble());
        if (m_receiveDir.isEmpty() || size < 0 || size > kMaxFileBytes ||
            conn.incoming.size() >= 4) {
            respondError(conn.id, requestId, QStringLiteral("refused"));
            return;
        }
        if (!QDir().mkpath(m_receiveDir)) {
            respondError(conn.id, requestId, QStringLiteral("no_space"));
            return;
        }
        Incoming in;
        in.name = safeFileName(params.value(QStringLiteral("name")).toString());
        in.finalPath = uniquePath(m_receiveDir, in.name);
        in.size = size;
        in.file = new QFile(in.finalPath + QStringLiteral(".part"));
        if (!in.file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            delete in.file;
            respondError(conn.id, requestId, QStringLiteral("cannot_write"));
            return;
        }
        const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        conn.incoming.insert(id, in);
        respond(conn.id, requestId, {{QStringLiteral("transfer"), id}});
        return;
    }
    auto it = conn.incoming.find(transfer);
    if (it == conn.incoming.end()) {
        respondError(conn.id, requestId, QStringLiteral("unknown_transfer"));
        return;
    }
    if (method == QLatin1String("files.chunk")) {
        const QByteArray data = QByteArray::fromBase64(
            params.value(QStringLiteral("data")).toString().toLatin1());
        if (data.isEmpty() || it->received + data.size() > it->size ||
            it->file->write(data) != data.size()) {
            it->file->close();
            it->file->remove();
            delete it->file;
            conn.incoming.erase(it);
            respondError(conn.id, requestId, QStringLiteral("write_failed"));
            return;
        }
        it->received += data.size();
        respond(conn.id, requestId,
                {{QStringLiteral("received"), double(it->received)}});
        return;
    }
    if (method == QLatin1String("files.end")) {
        Incoming in = it.value();
        conn.incoming.erase(it);
        in.file->close();
        const bool complete = in.received == in.size;
        const bool renamed = complete && in.file->rename(in.finalPath);
        if (!renamed)
            in.file->remove();
        delete in.file;
        if (!renamed) {
            respondError(conn.id, requestId, complete
                                                 ? QStringLiteral("cannot_write")
                                                 : QStringLiteral("incomplete"));
            return;
        }
        respond(conn.id, requestId,
                {{QStringLiteral("path"), in.finalPath},
                 {QStringLiteral("name"), QFileInfo(in.finalPath).fileName()}});
        if (onFileReceived)
            onFileReceived(conn.device, in.finalPath);
        return;
    }
    if (method == QLatin1String("files.cancel")) {
        it->file->close();
        it->file->remove();
        delete it->file;
        conn.incoming.erase(it);
        respond(conn.id, requestId, {});
        return;
    }
    respondError(conn.id, requestId, QStringLiteral("unsupported"));
}

bool LocalControlServer::writeJson(Connection &conn, const QJsonObject &object)
{
    if (conn.state == State::AwaitAuth || !linkOpen(conn))
        return false;
    const QByteArray sealed = sealFrame(
        conn.sendKey, conn.sendCounter,
        QJsonDocument(object).toJson(QJsonDocument::Compact));
    if (sealed.isEmpty() || sealed.size() > maxFrameBytes() ||
        (conn.relayed() && sealed.size() > kMaxRelayFrameBytes))
        return false;
    ++conn.sendCounter;
    if (!writeFrame(conn, sealed)) {
        // The counter already moved; a relay that dropped a chunk has broken
        // the channel for good.
        if (conn.relayed()) {
            const int id = conn.id;
            QTimer::singleShot(0, this, [this, id] {
                closeConnection(id, QStringLiteral("relay unavailable"));
            });
        }
        return false;
    }
    return true;
}

void LocalControlServer::sendEvent(Connection &conn, const QString &event,
                                   const QJsonObject &data)
{
    writeJson(conn, {{QStringLiteral("event"), event},
                     {QStringLiteral("data"), data}});
}

void LocalControlServer::respond(int connectionId, int requestId,
                                 const QJsonObject &result)
{
    if (Connection *conn = connection(connectionId))
        writeJson(*conn, {{QStringLiteral("id"), requestId},
                          {QStringLiteral("ok"), true},
                          {QStringLiteral("result"), result}});
}

void LocalControlServer::respondError(int connectionId, int requestId,
                                      const QString &error)
{
    if (Connection *conn = connection(connectionId))
        writeJson(*conn, {{QStringLiteral("id"), requestId},
                          {QStringLiteral("ok"), false},
                          {QStringLiteral("error"), error}});
}

void LocalControlServer::respondError(int connectionId, int requestId,
                                      const QString &error, const QString &message)
{
    QJsonObject reply{{QStringLiteral("id"), requestId},
                      {QStringLiteral("ok"), false},
                      {QStringLiteral("error"), error}};
    if (!message.trimmed().isEmpty())
        reply.insert(QStringLiteral("message"), message.trimmed().left(500));
    if (Connection *conn = connection(connectionId))
        writeJson(*conn, reply);
}

void LocalControlServer::broadcast(const QString &event, const QJsonObject &data)
{
    for (Connection *conn : std::as_const(m_connections))
        if (conn->state == State::Paired)
            sendEvent(*conn, event, data);
}

bool LocalControlServer::sendEventTo(int connectionId, const QString &event,
                                     const QJsonObject &data)
{
    Connection *conn = connection(connectionId);
    if (!conn || conn->state != State::Paired)
        return false;
    return writeJson(*conn, {{QStringLiteral("event"), event},
                             {QStringLiteral("data"), data}});
}

bool LocalControlServer::sendFile(const QString &deviceKey, const QString &path,
                                  QString *error)
{
    Connection *target = nullptr;
    for (Connection *conn : std::as_const(m_connections))
        if (conn->state == State::Paired && conn->device.key == deviceKey &&
            !conn->relayed())
            target = conn;
    if (!target) {
        if (error)
            *error = QStringLiteral("That phone is not connected.");
        return false;
    }
    auto *file = new QFile(path);
    if (!file->open(QIODevice::ReadOnly) || file->size() > kMaxFileBytes) {
        if (error)
            *error = file->isOpen() ? QStringLiteral("The file is too large.")
                                    : file->errorString();
        delete file;
        return false;
    }
    Outgoing out;
    out.file = file;
    out.size = file->size();
    out.transferId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    sendEvent(*target, QStringLiteral("file.begin"),
              {{QStringLiteral("transfer"), out.transferId},
               {QStringLiteral("name"), QFileInfo(path).fileName()},
               {QStringLiteral("size"), double(out.size)}});
    target->outgoing.append(out);
    pumpOutgoing(*target);
    return true;
}

void LocalControlServer::pumpOutgoing(Connection &conn)
{
    while (!conn.outgoing.isEmpty() && conn.socket && !conn.relayed() &&
           conn.socket->bytesToWrite() < kOutgoingHighWater) {
        Outgoing &out = conn.outgoing.first();
        const QByteArray chunk = out.file->read(fileChunkBytes());
        if (!chunk.isEmpty())
            sendEvent(conn, QStringLiteral("file.chunk"),
                      {{QStringLiteral("transfer"), out.transferId},
                       {QStringLiteral("data"),
                        QString::fromLatin1(chunk.toBase64())}});
        if (chunk.isEmpty() || out.file->atEnd()) {
            const bool ok = out.file->pos() == out.size;
            sendEvent(conn, ok ? QStringLiteral("file.end")
                               : QStringLiteral("file.cancel"),
                      {{QStringLiteral("transfer"), out.transferId}});
            delete out.file;
            conn.outgoing.removeFirst();
        }
    }
}

void LocalControlServer::closeConnection(int id, const QString &reason)
{
    Connection *conn = connection(id);
    if (!conn)
        return;
    m_connections.removeOne(conn);
    if (conn->relayed()) {
        noteBounded(m_relayToldGone, conn->relaySession);
        if (!sendRelay({{QStringLiteral("type"), QStringLiteral("control")},
                        {QStringLiteral("action"), QStringLiteral("close")},
                        {QStringLiteral("session"), conn->relaySession},
                        {QStringLiteral("node"), m_relayNode},
                        {QStringLiteral("reason"), reason.left(160)}}))
            noteBounded(m_relayUndelivered, conn->relaySession);
    }
    const bool wasPending = conn->state == State::Pending;
    const bool wasVisible = conn->state != State::AwaitAuth;
    for (Incoming &in : conn->incoming) {
        in.file->close();
        in.file->remove();
        delete in.file;
    }
    for (Outgoing &out : conn->outgoing)
        delete out.file;
    if (conn->handshakeTimer)
        conn->handshakeTimer->deleteLater();
    if (conn->socket) {
        conn->socket->disconnect(this);
        conn->socket->close();
        conn->socket->deleteLater();
    }
    delete conn;
    if (wasVisible && onConnectionClosed)
        onConnectionClosed(id);
    if (wasPending && onPairingClosed)
        onPairingClosed(id);
    if (wasVisible && onDevicesChanged)
        onDevicesChanged();
}

void LocalControlServer::rememberDevice(const Device &device)
{
    m_devices.insert(device.key, device);
    saveDevices();
}

void LocalControlServer::loadDevices()
{
    m_devices.clear();
    if (m_storePath.isEmpty())
        return;
    QFile file(m_storePath);
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonArray devices = QJsonDocument::fromJson(file.readAll())
                                   .object()
                                   .value(QStringLiteral("devices"))
                                   .toArray();
    for (const QJsonValue &value : devices) {
        const QJsonObject object = value.toObject();
        Device device;
        device.key = object.value(QStringLiteral("key")).toString();
        if (fromBase64Url(device.key, 32).isEmpty())
            continue;
        device.name = boundedText(object, "name");
        device.user = boundedText(object, "user");
        device.platform = boundedText(object, "platform", 16);
        device.pairedAtMs = qint64(object.value(QStringLiteral("pairedAtMs")).toDouble());
        device.lastSeenMs = qint64(object.value(QStringLiteral("lastSeenMs")).toDouble());
        device.approval = boundedText(object, "approval", 16);
        device.accountCheckedMs =
            qint64(object.value(QStringLiteral("accountCheckedMs")).toDouble());
        m_devices.insert(device.key, device);
    }
}

void LocalControlServer::saveDevices() const
{
    if (m_storePath.isEmpty())
        return;
    QJsonArray devices;
    for (const Device &device : pairedDevices()) {
        QJsonObject object{
            {QStringLiteral("key"), device.key},
            {QStringLiteral("name"), device.name},
            {QStringLiteral("user"), device.user},
            {QStringLiteral("platform"), device.platform},
            {QStringLiteral("pairedAtMs"), double(device.pairedAtMs)},
            {QStringLiteral("lastSeenMs"), double(device.lastSeenMs)},
        };
        if (!device.approval.isEmpty())
            object.insert(QStringLiteral("approval"), device.approval);
        if (device.accountCheckedMs > 0)
            object.insert(QStringLiteral("accountCheckedMs"),
                          double(device.accountCheckedMs));
        devices.append(object);
    }
    QDir().mkpath(QFileInfo(m_storePath).absolutePath());
    QSaveFile file(m_storePath);
    if (!file.open(QIODevice::WriteOnly))
        return;
    file.write(QJsonDocument(QJsonObject{{QStringLiteral("devices"), devices}})
                   .toJson(QJsonDocument::Indented));
    file.commit();
}
