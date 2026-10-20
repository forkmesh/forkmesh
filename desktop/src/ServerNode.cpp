#include "ForkMeshVersion.h"
#include "AppPaths.h"
#include "ChatHistoryLimits.h"
#include "ChatFormat.h"
#include "ChatRecipients.h"
#include "ChatVisitorPresence.h"
#include "NodeDiagnostics.h"
#include "OfficeChannelMirror.h"
#include "ServerNode.h"
#include "StickerCatalog.h"
#include "SystemStats.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QSslSocket>
#include <QThread>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>

#include <atomic>
#include <utility>

#ifndef FORKMESH_VERSION
#define FORKMESH_VERSION "dev"
#endif

namespace {

constexpr int kSeenCacheLimit = 4096;
constexpr int kMessageIndexLimit = 4096;
constexpr qint64 kPeerReapMs = 3600000; // 1 hour
constexpr qint64 kOutboxRetainMs = 7ll * 24 * 60 * 60 * 1000;
const QString kKnownRosterGroup = QStringLiteral("mainnode/knownRoster");
constexpr int kPresenceIntervalMs = 60000;   // 60s broadcast
constexpr qint64 kPeerStaleMs = 180000;       // 3 missed beats -> offline
constexpr qint64 kStaleRxMs = 70000; // ~2.8x the 25s ping interval
// A server that accepts TCP/TLS but never answers the WebSocket upgrade would
// otherwise hang the node forever: the ping timer only starts after the 101.
constexpr qint64 kHelloAdvertiseMinIntervalMs = 30000;
constexpr qint64 kHelloReplyMinIntervalMs = 120000;
constexpr quint64 kMaxWsPayload = 96ull * 1024 * 1024;
constexpr int kMaxDisplayNameChars = 32;
constexpr int kMaxSolanaAddressChars = 64;
constexpr int kMaxVersionChars = 32;
constexpr int kMaxTextChars = 16000;
constexpr int kMaxFileNameChars = 180;
constexpr int kMaxMimeChars = 100;
constexpr qsizetype kMaxFileBytes = 64ll * 1024 * 1024;
constexpr qsizetype kMaxBase64FileChars = 90ll * 1024 * 1024;
constexpr qsizetype kMaxAvatarBytes = 256 * 1024;

const QSet<QByteArray> &blockedChatTermHashes()
{
    static const QSet<QByteArray> hashes = {
        QByteArrayLiteral("08a841e996781e9e77d30a4e4420a8f501a280b00624e6d1224bf54aaff73eba"),
        QByteArrayLiteral("0f28c4960d96647e77e7ab6d13b85bd16c7ca56f45df802cdc763a5e5c0c7863"),
        QByteArrayLiteral("120f6e5b4ea32f65bda68452fcfaaef06b0136e1d0e4a6f60bc3771fa0936dd6"),
        QByteArrayLiteral("158869a97379229b7681efae9d7f9c9214134e836d649ba53477c0c111414d59"),
        QByteArrayLiteral("16ea09fc78ca83ca502cbcf2377acdf280bf18f61e259153f0868405eedab5ef"),
        QByteArrayLiteral("2189c0ed714f0c54ea91fc1d8355e3b1d68723a4fb580e99b088651c80a50f26"),
        QByteArrayLiteral("2f5f6ce5ae30b54aa5d7ced1ba566982bab34ba2814a51ce1865d2c2d8815cd4"),
        QByteArrayLiteral("566f532d486c947709d3d0e6b7575af8380248db66dada211d58eb00ad585297"),
        QByteArrayLiteral("6ac3c336e4094835293a3fed8a4b5fedde1b5e2626d9838fed50693bba00af0e"),
        QByteArrayLiteral("796e43a5a8cdb73b92b5f59eb50610cea3efa8ce229cd7f0557983091b2b4552"),
        QByteArrayLiteral("7bc671151cbfaee7f32cd56e86a87b0be30fde8dc72c7f236d3ab2ce42cddbd5"),
        QByteArrayLiteral("85fc17f7069acd39a5c636cd0a6530651096128da447959f5e250824857dc559"),
        QByteArrayLiteral("886d51e97ad7931d0d2af8439ca6d9e4887e3c2b469ed247cbd68ceb3649ccde"),
        QByteArrayLiteral("8f5083e3e5c7dc8932f2bf58212f963f3a44752618c96297f82623f736c52738"),
        QByteArrayLiteral("98b52c4b6b7d1f48e7477a5ccc10955dd195d0ac5a38c8281bfeb08762634909"),
        QByteArrayLiteral("9ae315a94e428a7ee3b5e48adae6541965d93b86acf10ffa1c45b93b6fe577b4"),
        QByteArrayLiteral("ad505b0be8a49b89273e307106fa42133cbd804456724c5e7635bd953215d92a"),
        QByteArrayLiteral("c2c3b68b48832afd9a4dbdd474c1b6c81c8baecdb71446f9947dac72dd0fe93d"),
        QByteArrayLiteral("c3de533e9b7fe63b79f648687a30d2861edd92fe7c3cd1f2c485e0a605367624"),
        QByteArrayLiteral("d75a838dc758ba17f28bd8dbac605cb70c35465263d5733164521de2f7ef7926"),
        QByteArrayLiteral("dd92623b0a4b255f87cc4aaee7990ee182d91db49189df6229ce65b5e9d960da"),
        QByteArrayLiteral("e512a05583448f44790783f986b1f36925c8cfc42338ca0e1caa637755bd15ae"),
        QByteArrayLiteral("e7b98c6aa5b944e0b315d350d423f895ac9e44fb84f1534b18c2572370a67b9e"),
        QByteArrayLiteral("eef3bd091670c3447022d619c06ad15de96da72b5a66f28bb8b75d1b1c12a05f"),
        QByteArrayLiteral("f50c51ed2315dcf3fa88181cf033f8029cac64f7dea4048327ca032ec102ea74"),
        QByteArrayLiteral("f9d0d9b18ae9033a5ea36df19bf279b059e887a9ae785db81117bceaecc95933"),
    };
    return hashes;
}

bool isBlockedChatRun(const QString &value)
{
    static const QSet<qsizetype> blockedLengths = {4, 5, 6, 7, 12};
    static const QRegularExpression marks(QStringLiteral("\\p{M}+"));
    static const QRegularExpression nonLetters(QStringLiteral("[^a-z]+"));
    static const QRegularExpression repeats(QStringLiteral("(.)\\1+"));
    QString normalized = value.normalized(QString::NormalizationForm_D).toLower();
    normalized.remove(marks);
    normalized.replace(QLatin1Char('0'), QLatin1Char('o'));
    normalized.replace(QLatin1Char('1'), QLatin1Char('i'));
    normalized.replace(QLatin1Char('3'), QLatin1Char('e'));
    normalized.replace(QLatin1Char('4'), QLatin1Char('a'));
    normalized.replace(QLatin1Char('5'), QLatin1Char('s'));
    normalized.replace(QLatin1Char('7'), QLatin1Char('t'));
    normalized.replace(QLatin1Char('8'), QLatin1Char('b'));
    normalized.replace(QLatin1Char('9'), QLatin1Char('g'));
    normalized.remove(nonLetters);
    QSet<QString> forms{normalized};
    forms.insert(QString(normalized).replace(repeats, QStringLiteral("\\1")));
    for (const QString &form : std::as_const(forms)) {
        if (!blockedLengths.contains(form.size()))
            continue;
        const QByteArray digest = QCryptographicHash::hash(
            form.toUtf8(), QCryptographicHash::Sha256).toHex();
        if (blockedChatTermHashes().contains(digest))
            return true;
    }
    return false;
}

QString filteredChatText(const QString &source)
{
    static const QRegularExpression wordRun(
        QStringLiteral(R"([\p{L}\p{N}](?:[\p{L}\p{N}]|[._-](?=[\p{L}\p{N}]))*)"),
        QRegularExpression::UseUnicodePropertiesOption);
    QString output;
    qsizetype cursor = 0;
    auto matches = wordRun.globalMatch(source);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        output += source.mid(cursor, match.capturedStart() - cursor);
        output += isBlockedChatRun(match.captured())
            ? QStringLiteral("***") : match.captured();
        cursor = match.capturedEnd();
    }
    output += source.mid(cursor);
    return output;
}

QJsonObject moderatedChatObject(const QJsonObject &source)
{
    QJsonObject result = source;
    const QString type = result.value(QStringLiteral("type")).toString();
    if ((type == QLatin1String("chat") || type == QLatin1String("thread-reply") ||
         type == QLatin1String("edit") || type == QLatin1String("dm")) &&
        result.value(QStringLiteral("text")).isString()) {
        result.insert(QStringLiteral("text"),
                      filteredChatText(result.value(QStringLiteral("text")).toString()));
    } else if (type == QLatin1String("history")) {
        QJsonArray entries;
        for (const QJsonValue &entry : result.value(QStringLiteral("entries")).toArray())
            entries.append(moderatedChatObject(entry.toObject()));
        result.insert(QStringLiteral("entries"), entries);
    }
    return result;
}

QString frameConversation(const QJsonObject &frame)
{
    const QString type = frame.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("chat") ||
        type == QLatin1String("thread-reply")) {
        return frame.value(QStringLiteral("channel")).toString();
    }
    if (type == QLatin1String("reaction") || type == QLatin1String("edit") ||
        type == QLatin1String("delete") ||
        type == QLatin1String("admin-delete") ||
        type == QLatin1String("typing")) {
        return frame.value(QStringLiteral("conversation")).toString();
    }
    return QString();
}

QString currentPlatform()
{
#if defined(Q_OS_ANDROID)
    return QStringLiteral("android");
#elif defined(Q_OS_IOS)
    return QStringLiteral("ios");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("macos");
#elif defined(Q_OS_WIN)
    return QStringLiteral("windows");
#elif defined(Q_OS_LINUX)
    return QStringLiteral("linux");
#else
    return QStringLiteral("unknown");
#endif
}

QByteArray randomKey()
{
    QByteArray key(16, Qt::Uninitialized);
    for (int i = 0; i < key.size(); ++i)
        key[i] = char(QRandomGenerator::global()->bounded(256));
    return key.toBase64();
}

QString wsPath(const QUrl &url)
{
    QString path = url.path().isEmpty() ? "/" : url.path();
    if (url.hasQuery())
        path += "?" + url.query();
    return path;
}

QString endpointDisplay(const QUrl &url)
{
    return url.toString(QUrl::RemoveUserInfo);
}

bool isDurableMainnodeType(const QString &type)
{
    static const QSet<QString> kDurableTypes = {
        QStringLiteral("chat"), QStringLiteral("thread-reply"),
        QStringLiteral("edit"),
        QStringLiteral("delete"), QStringLiteral("reaction"),
        QStringLiteral("admin-delete")};
    return kDurableTypes.contains(type);
}

QString mainnodeScopeFor(const QJsonObject &plain, bool durable)
{
    if (durable)
        return QStringLiteral("durable");
    const QString type = plain.value("type").toString();
    if (type == QLatin1String("dm") || plain.contains(QStringLiteral("to")) ||
        plain.value("private").toBool())
        return QStringLiteral("targeted");
    return QStringLiteral("ephemeral");
}

QString boundedText(const QJsonObject &object, const char *key, int maxChars)
{
    QString value = object.value(key).toString();
    if (value.size() > maxChars)
        value = value.left(maxChars);
    return value;
}

QString chatBodyText(const QJsonObject &message, int maxChars)
{
    return forkmesh::chatformat::bodySource(message, maxChars);
}

constexpr int kMaxMirrorRepos = 500;
constexpr int kMaxRepoNameChars = 160;

QList<MirrorAdvert> readMirrors(const QJsonObject &message)
{
    const QJsonObject heads = message.value("mirrorHeads").toObject();
    QList<MirrorAdvert> mirrors;
    for (const auto &value : message.value("mirrors").toArray()) {
        const QString repo = value.toString().left(kMaxRepoNameChars);
        if (repo.isEmpty() || mirrors.size() >= kMaxMirrorRepos)
            continue;
        MirrorAdvert advert;
        advert.ownerName = repo;
        const QJsonObject head = heads.value(repo).toObject();
        advert.commit = head.value("c").toString().left(64);
        advert.branch = head.value("b").toString().left(kMaxRepoNameChars);
        advert.refsFingerprint = head.value("r").toString().left(64);
        advert.updatedMs = qint64(head.value("t").toDouble());
        advert.source = head.value("s").toString().left(kMaxRepoNameChars);
        advert.sizeBytes = qMax(qint64(0), qint64(head.value("z").toDouble()));
        advert.issueCount = head.contains("i") ? head.value("i").toInt(-1) : -1;
        advert.commitCount = head.contains("k") ? head.value("k").toInt(-1) : -1;
        advert.branchCount = head.contains("h") ? head.value("h").toInt(-1) : -1;
        advert.pullCount = head.contains("p") ? head.value("p").toInt(-1) : -1;
        advert.discussionCount =
            head.contains("d") ? head.value("d").toInt(-1) : -1;
        advert.worktreeCount = head.contains("w") ? head.value("w").toInt(-1) : -1;
        advert.artifactCount = head.contains("a") ? head.value("a").toInt(-1) : -1;
        advert.commitIdentity.subject =
            head.value("m").toString().left(kMaxCommitSubjectChars);
        advert.commitIdentity.author =
            head.value("n").toString().left(kMaxCommitAuthorChars);
        advert.commitIdentity.committedAtMs =
            qMax(qint64(0), qint64(head.value("e").toDouble()));
        mirrors.append(advert);
    }
    return mirrors;
}

QStringList mirrorNames(const QList<MirrorAdvert> &mirrors)
{
    QStringList names;
    names.reserve(mirrors.size());
    for (const MirrorAdvert &mirror : mirrors)
        names.append(mirror.ownerName);
    return names;
}

QString safeFileName(const QString &name)
{
    QString cleaned = name;
    cleaned.replace('\\', '/');
    cleaned = cleaned.section('/', -1).trimmed();
    cleaned.remove('\0');
    if (cleaned.isEmpty())
        cleaned = "file";
    return cleaned.left(kMaxFileNameChars);
}

QByteArray boundedBase64(const QJsonObject &object, const char *key,
                         qsizetype maxBase64Chars, qsizetype maxDecodedBytes)
{
    const QString encoded = object.value(key).toString();
    if (encoded.isEmpty() || encoded.size() > maxBase64Chars)
        return {};
    const QByteArray decoded = QByteArray::fromBase64(encoded.toLatin1());
    if (decoded.size() > maxDecodedBytes)
        return {};
    return decoded;
}

bool messageHasSafePayload(const QJsonObject &message)
{
    return message.value("text").toString().size() <= kMaxTextChars &&
           message.value("richText").toObject().value("source").toString().size() <=
               kMaxTextChars &&
           message.value("sender").toString().size() <= kMaxDisplayNameChars &&
           message.value("solana").toString().size() <= kMaxSolanaAddressChars &&
           message.value("version").toString().size() <= kMaxVersionChars &&
           message.value("fileName").toString().size() <= kMaxFileNameChars &&
           message.value("fileMime").toString().size() <= kMaxMimeChars &&
           message.value("rootId").toString().size() <= 96 &&
           message.value("file").toString().size() <= kMaxBase64FileChars &&
           message.value("png").toString().size() <= 4 * kMaxAvatarBytes / 3 + 8;
}

QString stableOrRandomNodeId(const QString &stableNodeId)
{
    const QString trimmed = stableNodeId.trimmed();
    if (!trimmed.isEmpty())
        return trimmed.left(160);
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QByteArray diagnosticToken(const QString &value)
{
    return QUrl::toPercentEncoding(value.trimmed().left(64), "-._~");
}

} // namespace

ServerNode::ServerNode(const QString &userName, const QString &nodeName,
                       const QString &ownerUser, const QString &stableNodeId,
                       const QUrl &serverUrl,
                       const QString &roomName,
                       const QString &solanaAddress,
                       const QString &roomPassphrase,
                       QObject *parent)
    : ChatBackend(parent),
      m_userName(userName),
      m_nodeName(nodeName.trimmed().left(kMaxDisplayNameChars)),
      m_ownerUser(ownerUser.trimmed().left(kMaxDisplayNameChars)),
      m_url(serverUrl),
      m_roomName(roomName.trimmed()),
      m_solanaAddress(solanaAddress.trimmed().left(kMaxSolanaAddressChars)),
      m_platform(currentPlatform()),
      m_version(QStringLiteral(FORKMESH_VERSION).left(kMaxVersionChars)),
      m_nodeId(stableOrRandomNodeId(stableNodeId)),
      // A relay passphrase supersedes the legacy fallback, so derive exactly
      // one key.  Constructing the fallback first doubled the 210k-round
      // PBKDF2 work and blocked Office-channel setup on the GUI thread.
      m_crypto(roomPassphrase.isEmpty()
                   ? RoomCrypto(m_roomName)
                   : RoomCrypto(m_roomName, roomPassphrase)),
      m_seal(m_roomName, m_roomName, RoomSeal::Policy::Open, m_nodeId, m_nodeId)
{
    m_chatAccount = m_nodeId;
    if (const auto device = m_seal.device())
        m_deviceToken = device->subscribe(
            [this](forkmesh::chatv3::ChatDevice::Event event, const QString &scope,
                   const QString &) { onDeviceEvent(event, scope); });
    m_presenceIntervalMs = kPresenceIntervalMs;
    const QByteArray material = m_url.toString().toUtf8() + '\n' +
                                m_roomName.toUtf8();
    m_rosterStorageKey =
        QString::fromLatin1(QCryptographicHash::hash(material, QCryptographicHash::Sha256)
                                .toHex());
    m_endpoints = {m_url}; // default to the single URL; setEndpoints() adds failovers
    loadKnownPeers();
    loadHiddenChannels();
}

ServerNode::~ServerNode()
{
    if (const auto device = m_seal.device())
        device->unsubscribe(m_deviceToken);
}

void ServerNode::onDeviceEvent(forkmesh::chatv3::ChatDevice::Event event,
                               const QString &scope)
{
    using Event = forkmesh::chatv3::ChatDevice::Event;
    if (event == Event::PrekeyRotated) {
        if (m_rotationAdvertQueued)
            return;
        m_rotationAdvertQueued = true;
        QMetaObject::invokeMethod(
            this,
            [this] {
                m_rotationAdvertQueued = false;
                sendKeyAdvertisement(true);
            },
            Qt::QueuedConnection);
        return;
    }
    if (scope != m_seal.trustScope() || m_trustRefreshQueued)
        return;
    m_trustRefreshQueued = true;
    QMetaObject::invokeMethod(
        this,
        [this] {
            m_trustRefreshQueued = false;
            reopenHeldFrames();
            if (m_directRoom)
                resealOutbox();
            emit roomSealPeersChanged();
        },
        Qt::QueuedConnection);
}

void ServerNode::rebuildSeal()
{
    const auto device = m_seal.device();
    if (!m_directRoom) {
        m_seal = RoomSeal(m_roomName, m_roomName, RoomSeal::Policy::Open, m_chatAccount,
                          m_nodeId, device);
        return;
    }
    static const QRegularExpression directRoom(
        QStringLiteral("\\Achat-direct:([0-9a-f]{32}):v[0-9]+\\z"));
    const QRegularExpressionMatch match = directRoom.match(m_roomName);
    const QString scope = match.hasMatch()
                              ? QStringLiteral("direct:") + match.captured(1)
                              : QStringLiteral("direct-room:") + m_roomName;
    m_seal = RoomSeal(m_roomName, scope, RoomSeal::Policy::Strict, m_chatAccount, m_nodeId,
                      device);
    m_seal.setParticipants(m_directParticipants);
}

void ServerNode::setChatAccount(const QString &account)
{
    const QString normalized = account.trimmed().toLower();
    const QString value = normalized.isEmpty() ? m_nodeId : normalized;
    if (value == m_chatAccount)
        return;
    m_chatAccount = value;
    rebuildSeal();
}

void ServerNode::setDirectParticipants(const QStringList &participants)
{
    m_directParticipants = participants;
    m_seal.setParticipants(participants);
}

void ServerNode::setChatDevice(std::shared_ptr<forkmesh::chatv3::ChatDevice> device)
{
    if (!device || device == m_seal.device())
        return;
    if (const auto previous = m_seal.device())
        previous->unsubscribe(m_deviceToken);
    m_seal = RoomSeal(m_roomName, m_roomName, RoomSeal::Policy::Open, m_chatAccount,
                      m_nodeId, device);
    m_deviceToken = device->subscribe(
        [this](forkmesh::chatv3::ChatDevice::Event event, const QString &scope,
               const QString &) { onDeviceEvent(event, scope); });
    rebuildSeal();
}

void ServerNode::approveSealDevice(const QString &kid)
{
    m_seal.approve(kid);
}

void ServerNode::blockSealDevice(const QString &kid)
{
    m_seal.block(kid);
}

void ServerNode::reopenHeldFrames()
{
    const QList<QJsonObject> released = m_seal.takeReleasedHeld();
    for (const QJsonObject &frame : released)
        handleSealedFrame(frame, QJsonObject());
}

QJsonObject ServerNode::sealForRoom(const QJsonObject &plain) const
{
    if (plain.value(QStringLiteral("type")).toString() != QLatin1String("dm"))
        return m_seal.sealObject(plain);
    const QString to = plain.value(QStringLiteral("to")).toString();
    QStringList recipients = m_seal.keyIdsForNode(to);
    for (const QString &kid : m_seal.keyIdsForAcct(m_chatAccount))
        if (!recipients.contains(kid))
            recipients.append(kid);
    return m_seal.sealObjectFor(plain, recipients);
}

void ServerNode::setEndpoints(const QList<QUrl> &endpoints)
{
    QList<QUrl> valid;
    for (const QUrl &u : endpoints) {
        if (u.isValid() && (u.scheme() == "ws" || u.scheme() == "wss") &&
            !valid.contains(u))
            valid.append(u);
    }
    if (valid.isEmpty())
        return;
    m_endpoints = valid;
    m_endpointIndex = 0;
    m_url = m_endpoints.first();
}

void ServerNode::setConnectionAuthorizer(std::function<bool(const QUrl &)> authorizer)
{
    m_connectionAuthorizer = std::move(authorizer);
}

#ifdef FORKMESH_SERVER_NODE_TESTS
void ServerNode::setTransportLimitsForTests(int connectTimeoutMs,
                                            int reconnectBaseDelayMs,
                                            qint64 maxPendingWriteBytes)
{
    m_connectTimeoutMs = qMax(1, connectTimeoutMs);
    m_reconnectBaseDelayMs = qMax(1, reconnectBaseDelayMs);
    m_maxPendingWriteBytes = qMax<qint64>(64, maxPendingWriteBytes);
    m_reconnectJitter = false;
}

void ServerNode::setPresenceIntervalForTests(int presenceIntervalMs)
{
    m_presenceIntervalMs = qMax(1, presenceIntervalMs);
    if (m_presenceTimer)
        m_presenceTimer->setInterval(m_presenceIntervalMs);
}
#endif

void ServerNode::setNetworkAvailable(bool available)
{
    if (m_networkAvailable == available)
        return;
    m_networkAvailable = available;

    if (!available) {
        if (m_reconnectTimer)
            m_reconnectTimer->stop();
        if (m_connectTimeoutTimer)
            m_connectTimeoutTimer->stop();
        if (m_socket || m_attemptActive || m_wsReady) {
            discardCurrentSocket();
            handleLinkLost();
        }
        emitStatus(QString::fromUtf8("Offline \xE2\x80\x94 waiting for network\xE2\x80\xA6"));
        return;
    }

    if (m_userStopped || m_wsReady || m_attemptActive)
        return;
    if (m_reconnectTimer)
        m_reconnectTimer->stop();
    m_reconnectAttempts = 0;
    emitStatus(QString::fromUtf8("Network restored \xE2\x80\x94 reconnecting\xE2\x80\xA6"));
    openConnection();
}

void ServerNode::advanceEndpoint()
{
    if (m_endpoints.size() <= 1)
        return;
    m_endpointIndex = (m_endpointIndex + 1) % m_endpoints.size();
    m_url = m_endpoints.at(m_endpointIndex);
}

bool ServerNode::start()
{
    if (!m_crypto.isValid()) {
        emit fatalError(m_crypto.errorString());
        return false;
    }
    if (!m_url.isValid() || (m_url.scheme() != "ws" && m_url.scheme() != "wss")) {
        emit fatalError("Enter a ws:// or wss:// mainnode URL.");
        return false;
    }
    if (m_roomName.isEmpty()) {
        emit fatalError("Enter a repository room name.");
        return false;
    }

    if (!m_pingTimer) {
        // A periodic WebSocket ping keeps the relay (and Cloudflare's edge)
        // from dropping the connection while the node is idle. The pong's
        // round trip doubles as a free latency sample (latencySampled) so the
        // relay radar doesn't need its own HTTP probe while we're connected.
        m_pingTimer = new QTimer(this);
        m_pingTimer->setInterval(25000);
        connect(m_pingTimer, &QTimer::timeout, this, [this] {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            const qint64 lastAlive = qMax(m_lastRxMs, m_wsConnectedAtMs);
            if (m_wsReady && lastAlive > 0 && now - lastAlive > kStaleRxMs) {
                emit systemMessage(
                    QString::fromUtf8("Mainnode link went stale (nothing "
                                      "received for %1s); reconnecting\xE2\x80\xA6")
                        .arg((now - lastAlive) / 1000));
                failCurrentConnection();
                return;
            }
            m_pingSentMs = now;
            sendControlFrame(0x9);
        });
    }

    if (!m_presenceTimer) {
        m_presenceTimer = new QTimer(this);
        m_presenceTimer->setInterval(m_presenceIntervalMs);
        connect(m_presenceTimer, &QTimer::timeout, this, [this] {
            sendPresence();
            reapStalePeers();
            updateRosterAndStatus(); // re-evaluate staleness even with no traffic
        });
    }

    m_userStopped = false;
    if (!m_endpoints.isEmpty()) {
        m_endpointIndex = 0;
        m_url = m_endpoints.first();
    }
    emitStatus("Connecting to " + m_url.host() + "...");
    m_reconnectAttempts = 0;
    if (m_networkAvailable)
        openConnection();
    return true;
}

void ServerNode::discardCurrentSocket()
{
    QTcpSocket *socket = m_socket;
    m_socket = nullptr;
    if (!socket)
        return;
    socket->disconnect(this);
    socket->abort();
    m_socketRetirement.retire(socket);
}

void ServerNode::closeCurrentSocket()
{
    QTcpSocket *socket = m_socket;
    if (!socket)
        return;

    // A TCP FIN/abort without a WebSocket close frame is reported by the
    // Durable Object as code 1006 (unclean). Put the normal-closure code on
    // the wire first; disconnectFromHost() drains queued bytes, unlike abort().
    if (m_wsReady && socket->state() == QAbstractSocket::ConnectedState) {
        QByteArray payload;
        payload.append(char(0x03));
        payload.append(char(0xE8)); // RFC 6455 status 1000, network byte order
        sendControlFrame(0x8, payload);
        socket->flush();
    }

    m_socket = nullptr;
    socket->disconnect(this);
    if (socket->state() == QAbstractSocket::ConnectedState)
        socket->disconnectFromHost();
    else
        socket->abort();
    m_socketRetirement.retire(socket);
}

void ServerNode::failCurrentConnection()
{
    discardCurrentSocket();
    handleLinkLost();
}

void ServerNode::openConnection()
{
    discardCurrentSocket();
    m_attemptActive = false;
    m_wsReady = false;
    m_wsConnectedAtMs = 0;
    m_readBuffer.clear();
    emit networkDiagnosticsChanged();

    if (m_userStopped || !m_networkAvailable)
        return;

    if (m_connectionAuthorizer && !m_connectionAuthorizer(m_url)) {
        emit systemMessage(QStringLiteral("Firewall blocked mainnode socket to %1.")
                               .arg(m_url.host()));
        scheduleReconnect();
        return;
    }

    // Bound the whole connect + TLS + WebSocket-upgrade sequence. A server
    // that accepts the TCP connection but never answers the upgrade leaves no
    // timer running (ping/presence only start after the 101), so without this
    // the node would hang in "Connecting…" forever.
    if (!m_connectTimeoutTimer) {
        m_connectTimeoutTimer = new QTimer(this);
        m_connectTimeoutTimer->setSingleShot(true);
        connect(m_connectTimeoutTimer, &QTimer::timeout, this, [this] {
            if (m_wsReady || m_userStopped || !m_networkAvailable ||
                !m_attemptActive)
                return;
            emit systemMessage(
                QString::fromUtf8("Mainnode connect to %1 timed out; "
                                  "retrying\xE2\x80\xA6")
                    .arg(m_url.host()));
            failCurrentConnection();
        });
    }
    m_connectTimeoutTimer->start(m_connectTimeoutMs);

    m_socket = m_url.scheme() == "wss" ? new QSslSocket(this) : new QTcpSocket(this);
    m_attemptActive = true;
    connectSocketSignals();
    const int port = m_url.port(m_url.scheme() == "wss" ? 443 : 80);
    if (auto *ssl = qobject_cast<QSslSocket *>(m_socket))
        ssl->connectToHostEncrypted(m_url.host(), port);
    else
        m_socket->connectToHost(m_url.host(), port);
}

void ServerNode::scheduleReconnect()
{
    if (m_connectTimeoutTimer)
        m_connectTimeoutTimer->stop();
    if (m_userStopped || !m_networkAvailable)
        return; // the user left the node; don't keep retrying
    if (m_reconnectTimer && m_reconnectTimer->isActive())
        return; // a retry is already pending

    if (!m_reconnectTimer) {
        m_reconnectTimer = new QTimer(this);
        m_reconnectTimer->setSingleShot(true);
        connect(m_reconnectTimer, &QTimer::timeout, this, [this] {
            if (m_userStopped || !m_networkAvailable)
                return;
            advanceEndpoint();
            emitStatus("Reconnecting to " + m_url.host() + "...");
            openConnection();
        });
    }

    const int endpointCount = qMax(1, int(m_endpoints.size()));
    const int shift = qMin(m_reconnectAttempts / endpointCount, 9);
    ++m_reconnectAttempts;
    int delay = int(qMin<qint64>(
        qint64(m_reconnectBaseDelayMs) << shift, 300000));
    if (m_reconnectJitter)
        delay += int(QRandomGenerator::global()->bounded(delay / 4 + 250));
    if (delay >= 5000)
        emitStatus(
            QString::fromUtf8("Disconnected \xE2\x80\x94 reconnecting in %1s\xE2\x80\xA6")
                .arg((delay + 500) / 1000));
    else
        emitStatus(
            QString::fromUtf8("Disconnected \xE2\x80\x94 reconnecting\xE2\x80\xA6"));
    m_reconnectTimer->start(delay);
}

void ServerNode::connectSocketSignals()
{
    QTcpSocket *socket = m_socket;
    connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
        const forkmesh::DeferredSocketRetirement::Scope dispatching(
            m_socketRetirement);
        if (socket == m_socket)
            onSocketReadyRead();
    });
    connect(socket, &QTcpSocket::connected, this, [this, socket] {
        const forkmesh::DeferredSocketRetirement::Scope dispatching(
            m_socketRetirement);
        if (socket == m_socket && !qobject_cast<QSslSocket *>(socket))
            onConnectedTransport();
    });
    if (auto *ssl = qobject_cast<QSslSocket *>(socket))
        connect(ssl, &QSslSocket::encrypted, this, [this, socket] {
            const forkmesh::DeferredSocketRetirement::Scope dispatching(
                m_socketRetirement);
            if (socket == m_socket)
                onConnectedTransport();
        });
    connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
        const forkmesh::DeferredSocketRetirement::Scope dispatching(
            m_socketRetirement);
        if (socket != m_socket)
            return;
        discardCurrentSocket();
        handleLinkLost();
    });
    connect(socket, &QTcpSocket::errorOccurred, this, [this, socket] {
        const forkmesh::DeferredSocketRetirement::Scope dispatching(
            m_socketRetirement);
        if (socket != m_socket)
            return;
        // A TLS transport being replaced during a Worker deploy is expected
        // and this backend reconnects automatically.  Name the affected local
        // client and endpoint, but do not label every recoverable disconnect
        // as an application error.
        emit systemMessage(
            QString::fromUtf8(
                "Mainnode link interrupted (user %1 \xC2\xB7 node %2 \xC2\xB7 owner %3 "
                "\xC2\xB7 room %4 \xC2\xB7 endpoint %5): %6; reconnecting\xE2\x80\xA6")
                .arg(m_userName.isEmpty() ? QStringLiteral("(unknown)")
                                          : m_userName,
                     m_nodeName.isEmpty() ? QStringLiteral("(unknown)")
                                          : m_nodeName,
                     m_ownerUser.isEmpty() ? QStringLiteral("(unknown)")
                                           : m_ownerUser,
                     m_roomName.isEmpty() ? QStringLiteral("(unknown)")
                                          : m_roomName,
                     m_url.host().isEmpty() ? QStringLiteral("(unknown)")
                                            : m_url.host(),
                     socket->errorString()));
        failCurrentConnection();
    });
}

void ServerNode::handleLinkLost()
{
    if (!m_attemptActive && !m_wsReady)
        return;
    if (m_wsReady && m_networkAvailable)
        NodeDiagnostics::hostCollector().noteRelayDrop();
    m_attemptActive = false;
    if (m_pingTimer)
        m_pingTimer->stop();
    if (m_presenceTimer)
        m_presenceTimer->stop();
    if (m_helloAdvertiseTimer)
        m_helloAdvertiseTimer->stop();
    if (m_connectTimeoutTimer)
        m_connectTimeoutTimer->stop();
    const bool wasConnected = m_wsReady;
    m_wsReady = false;
    m_wsConnectedAtMs = 0;
    m_peers.clear();
    m_lastHelloReplyMs.clear();
    emit networkDiagnosticsChanged();
    if (wasConnected)
        emit connectionChanged(false);
    updateRosterAndStatus();
    scheduleReconnect();
}

void ServerNode::onConnectedTransport()
{
    sendHandshake();
}

void ServerNode::sendHandshake()
{
    m_wsKey = randomKey();
    QByteArray host = m_url.host().toUtf8();
    if (m_url.port() > 0)
        host += ":" + QByteArray::number(m_url.port());
    const QByteArray request =
        "GET " + wsPath(m_url).toUtf8() + " HTTP/1.1\r\n"
        "Host: " + host + "\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: " + m_wsKey + "\r\n"
        "User-Agent: ForkMeshDesktop/" FORKMESH_VERSION " (user=" +
        diagnosticToken(m_userName) + "; node=" +
        diagnosticToken(m_nodeName) + "; owner=" +
        diagnosticToken(m_ownerUser) + "; room=" +
        diagnosticToken(m_roomName) + ")\r\n\r\n";
    m_socket->write(request);
}

void ServerNode::onSocketReadyRead()
{
    if (!m_socket)
        return;
    const QPointer<QTcpSocket> socket(m_socket);
    m_readBuffer += m_socket->readAll();
    if (!m_wsReady) {
        const int headerEnd = m_readBuffer.indexOf("\r\n\r\n");
        if (headerEnd < 0)
            return;
        const QByteArray header = m_readBuffer.left(headerEnd);
        m_readBuffer.remove(0, headerEnd + 4);
        if (!header.startsWith("HTTP/1.1 101") && !header.startsWith("HTTP/1.0 101")) {
            const int lineEnd = header.indexOf("\r\n");
            const QString statusLine = QString::fromLatin1(
                (lineEnd < 0 ? header : header.left(lineEnd)).left(80)).trimmed();
            emit systemMessage(
                "Mainnode did not accept the WebSocket upgrade ("
                + (statusLine.isEmpty() ? QStringLiteral("no status") : statusLine)
                + "); reconnecting\xE2\x80\xA6");
            m_wsReady = false;
            failCurrentConnection();
            return;
        }
        m_wsReady = true;
        m_attemptActive = true;
        m_wsConnectedAtMs = QDateTime::currentMSecsSinceEpoch();
        m_reconnectAttempts = 0; // link is healthy again; retry fast next drop
        if (m_reconnectTimer)
            m_reconnectTimer->stop();
        if (m_connectTimeoutTimer)
            m_connectTimeoutTimer->stop();
        if (m_pingTimer)
            m_pingTimer->start();
        if (m_presenceTimer)
            m_presenceTimer->start();
        emit channelsChanged(m_channels);
        updateRosterAndStatus();
        emitStatus("Connected to encrypted mainnode room " + m_roomName);
        emit connectionChanged(true);
        emit networkDiagnosticsChanged();
        m_mismatchAdvertised = false;
        m_historyEnded = false;
        sendKeyAdvertisement(m_seal.shouldPersistAdvertisement());
        sendHello(true, true);
        if (!m_avatarPng.isEmpty())
            setAvatar(m_avatarPng);
    }

    while (socket && m_socket == socket && m_readBuffer.size() >= 2) {
        const uchar b0 = uchar(m_readBuffer.at(0));
        const uchar b1 = uchar(m_readBuffer.at(1));
        const int opcode = b0 & 0x0f;
        quint64 len = b1 & 0x7f;
        int pos = 2;
        if (len == 126) {
            if (m_readBuffer.size() < pos + 2)
                return;
            len = (uchar(m_readBuffer.at(pos)) << 8) | uchar(m_readBuffer.at(pos + 1));
            pos += 2;
        } else if (len == 127) {
            if (m_readBuffer.size() < pos + 8)
                return;
            len = 0;
            for (int i = 0; i < 8; ++i)
                len = (len << 8) | uchar(m_readBuffer.at(pos + i));
            pos += 8;
        }
        if (len > kMaxWsPayload) {
            emit systemMessage("Mainnode frame exceeded the safe size limit; disconnecting.");
            failCurrentConnection();
            return;
        }
        const bool masked = b1 & 0x80;
        QByteArray mask;
        if (masked) {
            if (m_readBuffer.size() < pos + 4)
                return;
            mask = m_readBuffer.mid(pos, 4);
            pos += 4;
        }
        if (m_readBuffer.size() < pos + int(len))
            return;
        QByteArray payload = m_readBuffer.mid(pos, int(len));
        m_readBuffer.remove(0, pos + int(len));
        if (masked) {
            for (int i = 0; i < payload.size(); ++i)
                payload[i] = payload.at(i) ^ mask.at(i % 4);
        }
        if (opcode == 0x9) { // ping from server -> reply with pong
            ++m_rxControlFrames;
            m_lastRxMs = QDateTime::currentMSecsSinceEpoch();
            emit networkDiagnosticsChanged();
            sendControlFrame(0xA, payload);
            continue;
        }
        if (opcode == 0xA) { // pong: keepalive acknowledged
            ++m_rxControlFrames;
            m_lastRxMs = QDateTime::currentMSecsSinceEpoch();
            if (m_pingSentMs > 0) {
                emit latencySampled(int(qMin<qint64>(
                    m_lastRxMs - m_pingSentMs, 60 * 1000)));
                m_pingSentMs = 0;
            }
            emit networkDiagnosticsChanged();
            continue;
        }
        if (opcode == 0x8) {
            ++m_rxControlFrames;
            m_lastRxMs = QDateTime::currentMSecsSinceEpoch();
            emit networkDiagnosticsChanged();
            // Complete the peer's close handshake by echoing its status and
            // reason before closing TCP. Without this reply the peer records
            // an otherwise intentional shutdown as code 1006.
            sendControlFrame(0x8, payload);
            if (socket && m_socket == socket)
                socket->flush();
            if (socket && m_socket == socket)
                socket->disconnectFromHost();
            return;
        }
        if (opcode == 0x1) {
            ++m_rxFrames;
            m_rxBytes += payload.size();
            m_lastRxBytes = payload.size();
            m_lastRxMs = QDateTime::currentMSecsSinceEpoch();
            m_lastRxType = QStringLiteral("text");
            m_lastRxScope.clear();
            m_lastRxSender.clear();
            processFrame(payload);
            // The relay echoing this node's own frame back is not traffic
            // from anybody.
            if (m_lastRxSender.isEmpty() || m_lastRxSender != m_nodeId)
                emit socketFrame(false, m_lastRxType, m_lastRxSender, payload.size());
            emit networkDiagnosticsChanged();
        }
    }
}

void ServerNode::processFrame(const QByteArray &payload)
{
    if (quint64(payload.size()) > kMaxWsPayload)
        return;
    const QJsonDocument doc = QJsonDocument::fromJson(payload);
    if (!doc.isObject())
        return;
    const QJsonObject envelope = doc.object();
    // The relay's own plaintext announcement that an account was just created.
    // It is the one frame the room originates itself: peers may only send
    // "cipher" envelopes, so nobody can forge a welcome for a name that never
    // signed up. Validate the handle the same way the Worker does before it
    // reaches the UI.
    if (envelope.value(QStringLiteral("kind")).toString() ==
        QStringLiteral("forkmesh-new-member")) {
        static const QRegularExpression handle(
            QStringLiteral("^[a-z0-9](?:[a-z0-9-]{0,38}[a-z0-9])?$"));
        const QString name =
            envelope.value(QStringLiteral("name")).toString().trimmed().toLower();
        m_lastRxType = QStringLiteral("new-member");
        if (handle.match(name).hasMatch())
            emit newMemberJoined(name);
        return;
    }
    if (forkmesh::recipients::isAckFrame(envelope)) {
        m_lastRxType = QStringLiteral("recipients");
        handleRecipientsAck(envelope);
        return;
    }
    if (forkmesh::recipients::isDeliveredFrame(envelope)) {
        m_lastRxType = QStringLiteral("delivered");
        handleDelivered(envelope);
        return;
    }
    if (forkmesh::recipients::isReadFrame(envelope)) {
        m_lastRxType = QStringLiteral("read");
        if (m_directRoom)
            emit conversationRead();
        return;
    }
    if (forkmesh::recipients::isServerCopiesFrame(envelope)) {
        m_lastRxType = QStringLiteral("server-copies");
        const int stored =
            forkmesh::recipients::serverCopiesFromEnvelope(envelope);
        if (stored == 0)
            m_outbox.clear();
        if (stored >= 0)
            emit serverCopiesKnown(stored);
        return;
    }
    if (forkmesh::recipients::isHistoryEndFrame(envelope)) {
        m_lastRxType = QStringLiteral("history-end");
        m_historyEnded = true;
        if (m_directRoom)
            resealOutbox();
        return;
    }
    QJsonObject plain = m_crypto.decryptObject(envelope);
    if (plain.isEmpty())
        return;
    m_lastRxSender = plain.value(QStringLiteral("senderId")).toString();
    if (RoomSeal::isLegacyFrame(plain)) {
        m_lastRxType = QStringLiteral("legacy-seal");
        return;
    }
    if (RoomSeal::isKeyFrame(plain)) {
        m_lastRxType = QStringLiteral("e2ee-key");
        m_lastRxSender.clear();
        bool changed = false;
        m_seal.acceptKeyFrame(plain, &changed);
        if (changed) {
            scheduleKeyAdvertisement();
            if (m_directRoom)
                resealOutbox();
            emit roomSealPeersChanged();
            reopenHeldFrames();
        }
        return;
    }
    if (RoomSeal::isSealedFrame(plain)) {
        handleSealedFrame(plain, envelope);
        return;
    }
    if (!isControllerReadableFrame(plain)) {
        noteSealMismatch(QString());
        return;
    }
    dispatchPlain(plain, envelope, QJsonObject(), QString());
}

void ServerNode::handleSealedFrame(const QJsonObject &sealed,
                                   const QJsonObject &envelope)
{
    const bool replayed =
        !m_historyEnded && envelope.value(QStringLiteral("historyReplay")).toBool();
    const RoomSeal::Opened opened = m_seal.open(sealed, replayed);
    switch (opened.status) {
    case RoomSeal::Status::Opened:
        break;
    case RoomSeal::Status::Held: {
        m_lastRxType = QStringLiteral("held");
        const bool unknown = m_seal.acctForKeyId(opened.senderKid).isEmpty();
        if (unknown) {
            noteSealMismatch(opened.senderKid, true);
            if (!m_mismatchAdvertised) {
                m_mismatchAdvertised = true;
                sendKeyAdvertisement(true);
            }
        }
        return;
    }
    case RoomSeal::Status::NotForMe:
        m_lastRxType = QStringLiteral("not-for-me");
        noteSealMismatch(opened.senderKid);
        if (m_directRoom && !m_mismatchAdvertised) {
            m_mismatchAdvertised = true;
            sendKeyAdvertisement(true);
        }
        return;
    case RoomSeal::Status::Replay:
        m_lastRxType = QStringLiteral("replay");
        return;
    case RoomSeal::Status::Invalid:
        m_lastRxType = QStringLiteral("invalid");
        return;
    case RoomSeal::Status::Own:
        m_lastRxType = QStringLiteral("own");
        noteOwnEcho(envelope);
        return;
    case RoomSeal::Status::NoSession:
        m_lastRxType = QStringLiteral("no-session");
        recoverSession(opened.senderKid, opened.ctr);
        return;
    }
    if (m_seal.usesRatchet() && RoomSeal::isResetPlain(opened.plain)) {
        m_lastRxType = QStringLiteral("e2ee-reset");
        if (!envelope.isEmpty())
            noteReceived(envelope);
        resealOutboxTo(opened.senderKid);
        return;
    }
    QJsonObject plain = opened.plain;
    if (!plain.contains(QStringLiteral("senderId")) && !opened.senderNode.isEmpty())
        plain.insert(QStringLiteral("senderId"), opened.senderNode);
    RoomSeal::useSignedTime(plain, opened);
    dispatchPlain(plain, envelope, sealed, opened.senderAcct);
}

void ServerNode::dispatchPlain(QJsonObject plain, const QJsonObject &envelope,
                               const QJsonObject &sealed, const QString &senderAcct)
{
    if (m_directRoom && !sealed.isEmpty() &&
        plain.value(QStringLiteral("senderId")).toString() == m_nodeId &&
        envelope.value(QStringLiteral("persist")).toBool())
        rememberOutbox(forkmesh::recipients::relayRef(envelope), plain, sealed);
    const QString type = plain.value("type").toString();
    if (!type.isEmpty()) {
        m_lastRxType = type;
        m_lastRxScope = mainnodeScopeFor(plain, envelope.value("persist").toBool());
    }
    if (const QString sender = plain.value(QStringLiteral("senderId")).toString();
        !sender.isEmpty())
        m_lastRxSender = sender;
    const int recipients = forkmesh::recipients::countFromEnvelope(envelope);
    const QStringList recipientNames =
        forkmesh::recipients::namesFromEnvelope(envelope);
    if (plain.value("senderId").toString() == m_nodeId) {
        const QString ownId = plain.value(QStringLiteral("id")).toString();
        if (!ownId.isEmpty() && (type == QLatin1String("chat") ||
                                 type == QLatin1String("thread-reply"))) {
            rememberSentRef(forkmesh::recipients::relayRef(envelope),
                            ownId.left(96));
            if (recipients >= 0)
                emit messageRecipientsKnown(ownId.left(96), recipients,
                                            recipientNames);
        }
        return;
    }
    if (!senderAcct.isEmpty() && senderAcct == m_chatAccount) {
        const QString ownId = plain.value(QStringLiteral("id")).toString();
        if (!ownId.isEmpty() && (type == QLatin1String("chat") ||
                                 type == QLatin1String("thread-reply")))
            rememberSentRef(forkmesh::recipients::relayRef(envelope), ownId.left(96));
    }
    if (!envelope.isEmpty())
        noteReceived(envelope);
    handlePlain(plain, recipients, recipientNames);
}

void ServerNode::noteReceived(const QJsonObject &envelope)
{
    const QString ref = forkmesh::recipients::relayRef(envelope);
    if (ref.isEmpty() || m_receivedRefs.contains(ref))
        return;
    m_receivedRefs.append(ref);
    if (m_receivedFlushPending)
        return;
    m_receivedFlushPending = true;
    QTimer::singleShot(150, this, [this] { flushReceived(); });
}

void ServerNode::flushReceived()
{
    m_receivedFlushPending = false;
    const QStringList refs = m_receivedRefs;
    m_receivedRefs.clear();
    if (!m_wsReady)
        return;
    for (qsizetype start = 0; start < refs.size();
         start += forkmesh::recipients::kReceivedRefsPerFrame)
        sendTextFrame(forkmesh::recipients::receivedFrame(refs.mid(
                          start, forkmesh::recipients::kReceivedRefsPerFrame)),
                      QStringLiteral("received"), QString(), QString());
}

void ServerNode::handleRecipientsAck(const QJsonObject &envelope)
{
    const int count = forkmesh::recipients::countFromEnvelope(envelope);
    const QString ref = envelope.value(QStringLiteral("ref")).toString();
    if (count < 0 || ref.size() != 40)
        return;
    const auto it = m_recipientRefs.find(ref);
    if (it == m_recipientRefs.end())
        return;
    const QString messageId = it.value();
    m_recipientRefs.erase(it);
    m_recipientRefOrder.removeOne(ref);
    emit messageRecipientsKnown(messageId, count,
                                forkmesh::recipients::namesFromEnvelope(envelope));
}

void ServerNode::rememberSentRef(const QString &ref, const QString &messageId)
{
    if (ref.isEmpty() || messageId.isEmpty())
        return;
    if (!m_sentRefs.contains(ref))
        m_sentRefOrder.enqueue(ref);
    m_sentRefs.insert(ref, messageId);
    m_sentRefIds.insert(messageId, ref);
    while (m_sentRefOrder.size() > forkmesh::recipients::kRememberedRefs) {
        const QString dropped = m_sentRefOrder.dequeue();
        const QString id = m_sentRefs.take(dropped);
        if (m_sentRefIds.value(id) == dropped)
            m_sentRefIds.remove(id);
    }
}

void ServerNode::handleDelivered(const QJsonObject &envelope)
{
    if (!m_directRoom)
        return;
    QStringList ids;
    for (const QString &ref : forkmesh::recipients::refsFromEnvelope(envelope)) {
        const QString id = m_sentRefs.value(ref);
        if (!id.isEmpty() && !ids.contains(id))
            ids.append(id);
    }
    if (!ids.isEmpty())
        emit messagesDelivered(ids);
}

void ServerNode::rememberRecipientRef(const QByteArray &frame,
                                      const QString &messageId)
{
    if (messageId.isEmpty())
        return;
    const QString ref = forkmesh::recipients::frameRef(frame);
    if (!m_recipientRefs.contains(ref))
        m_recipientRefOrder.enqueue(ref);
    m_recipientRefs.insert(ref, messageId);
    while (m_recipientRefOrder.size() > forkmesh::recipients::kRememberedRefs)
        m_recipientRefs.remove(m_recipientRefOrder.dequeue());
}

void ServerNode::sendTextFrame(const QByteArray &payload, const QString &type,
                               const QString &scope, const QString &peer)
{
    if (!m_wsReady || !m_socket)
        return;
    if (quint64(payload.size()) > kMaxWsPayload) {
        emit systemMessage("Message exceeded the safe mainnode frame limit and was not sent.");
        return;
    }
    const qint64 frameBytes = qint64(payload.size()) + 14;
    const qint64 buffered = m_socket->bytesToWrite();
    if (frameBytes > m_maxPendingWriteBytes ||
        buffered > m_maxPendingWriteBytes - frameBytes) {
        ++m_backpressureDrops;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - m_lastBackpressureNoticeMs >= 10000) {
            m_lastBackpressureNoticeMs = now;
            emit systemMessage(
                QStringLiteral("Mainnode send queue is full; dropped one "
                               "outgoing frame instead of growing memory."));
        }
        emit networkDiagnosticsChanged();
        return;
    }
    QByteArray frame;
    frame.append(char(0x81));
    if (payload.size() < 126) {
        frame.append(char(0x80 | payload.size()));
    } else if (payload.size() <= 0xffff) {
        frame.append(char(0x80 | 126));
        frame.append(char((payload.size() >> 8) & 0xff));
        frame.append(char(payload.size() & 0xff));
    } else {
        frame.append(char(0x80 | 127));
        quint64 len = payload.size();
        for (int i = 7; i >= 0; --i)
            frame.append(char((len >> (8 * i)) & 0xff));
    }
    QByteArray mask(4, Qt::Uninitialized);
    for (int i = 0; i < 4; ++i)
        mask[i] = char(QRandomGenerator::global()->bounded(256));
    frame.append(mask);
    QByteArray masked = payload;
    for (int i = 0; i < masked.size(); ++i)
        masked[i] = masked.at(i) ^ mask.at(i % 4);
    frame.append(masked);
    m_socket->write(frame);
    ++m_txFrames;
    m_txBytes += payload.size();
    m_lastTxBytes = payload.size();
    m_lastTxMs = QDateTime::currentMSecsSinceEpoch();
    m_lastTxType = type.isEmpty() ? QStringLiteral("text") : type;
    m_lastTxScope = scope;
    emit socketFrame(true, m_lastTxType, peer, payload.size());
    emit networkDiagnosticsChanged();
}

void ServerNode::sendControlFrame(int opcode, const QByteArray &payload)
{
    // Ping/pong control frames (payload <= 125 bytes), client-masked per RFC 6455.
    if (!m_socket || !m_wsReady || payload.size() > 125)
        return;
    if (m_socket->bytesToWrite() >
        m_maxPendingWriteBytes - (qint64(payload.size()) + 6)) {
        ++m_backpressureDrops;
        emit networkDiagnosticsChanged();
        return;
    }
    QByteArray frame;
    frame.append(char(0x80 | (opcode & 0x0f)));
    frame.append(char(0x80 | payload.size()));
    QByteArray mask(4, Qt::Uninitialized);
    for (int i = 0; i < 4; ++i)
        mask[i] = char(QRandomGenerator::global()->bounded(256));
    frame.append(mask);
    QByteArray masked = payload;
    for (int i = 0; i < masked.size(); ++i)
        masked[i] = masked.at(i) ^ mask.at(i % 4);
    frame.append(masked);
    m_socket->write(frame);
    ++m_txControlFrames;
    m_lastTxMs = QDateTime::currentMSecsSinceEpoch();
    emit networkDiagnosticsChanged();
}

QList<QJsonObject> ServerNode::networkDiagnostics() const
{
    QJsonObject row;
    row.insert(QStringLiteral("connection"), QStringLiteral("Mainnode room"));
    row.insert(QStringLiteral("kind"), QStringLiteral("mainnode"));
    row.insert(QStringLiteral("durableObject"),
               QStringLiteral("Room DO: %1").arg(m_roomName));
    row.insert(QStringLiteral("endpoint"), endpointDisplay(m_url));
    row.insert(QStringLiteral("state"),
               m_wsReady ? QStringLiteral("Connected")
                         : (m_socket ? QStringLiteral("Connecting")
                                     : QStringLiteral("Disconnected")));
    row.insert(QStringLiteral("connected"), m_wsReady);
    row.insert(QStringLiteral("connectedAtMs"), double(m_wsConnectedAtMs));
    row.insert(QStringLiteral("lastRxMs"), double(m_lastRxMs));
    row.insert(QStringLiteral("lastTxMs"), double(m_lastTxMs));
    row.insert(QStringLiteral("rxFrames"), double(m_rxFrames));
    row.insert(QStringLiteral("txFrames"), double(m_txFrames));
    row.insert(QStringLiteral("rxBytes"), double(m_rxBytes));
    row.insert(QStringLiteral("txBytes"), double(m_txBytes));
    row.insert(QStringLiteral("rxControlFrames"), double(m_rxControlFrames));
    row.insert(QStringLiteral("txControlFrames"), double(m_txControlFrames));
    row.insert(QStringLiteral("bufferedBytes"),
               double(m_socket ? m_socket->bytesToWrite() : 0));
    row.insert(QStringLiteral("backpressureDrops"),
               double(m_backpressureDrops));
    row.insert(QStringLiteral("networkAvailable"), m_networkAvailable);
    row.insert(QStringLiteral("lastRxBytes"), m_lastRxBytes);
    row.insert(QStringLiteral("lastTxBytes"), m_lastTxBytes);
    row.insert(QStringLiteral("lastRxType"), m_lastRxType);
    row.insert(QStringLiteral("lastRxScope"), m_lastRxScope);
    row.insert(QStringLiteral("lastTxType"), m_lastTxType);
    row.insert(QStringLiteral("lastTxScope"), m_lastTxScope);
    row.insert(QStringLiteral("peers"), m_peers.size());
    row.insert(QStringLiteral("channels"), m_channels.size());
    row.insert(QStringLiteral("data"),
               QStringLiteral("Encrypted room frames: chat/edit/delete/reaction/admin-delete "
                              "are durable history; hello/presence/typing/avatar/history/"
                              "mirror/mirror-refresh/cove frames are ephemeral; direct/"
                              "private frames are targeted."));
    return {row};
}

QJsonObject ServerNode::makeMessage(const QString &type) const
{
    QJsonObject message{{"type", type},
                        {"id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                        {"senderId", m_nodeId},
                        {"sender", m_userName.left(kMaxDisplayNameChars)},
                        {"ts", double(QDateTime::currentMSecsSinceEpoch())}};
    if (!m_accountKind.isEmpty())
        message.insert("accountKind", m_accountKind);
    if (!m_nodeName.isEmpty())
        message.insert("nodeName", m_nodeName);
    if (!m_ownerUser.isEmpty())
        message.insert("ownerUser", m_ownerUser);
    if (!m_solanaAddress.isEmpty())
        message.insert("solana", m_solanaAddress);
    if (!m_platform.isEmpty())
        message.insert("platform", m_platform);
    if (!m_version.isEmpty())
        message.insert("version", m_version);
    if (m_memTotalBytes > 0 || m_diskTotalBytes > 0 || m_cpuPercent >= 0.0) {
        QJsonObject sys;
        if (m_memTotalBytes > 0) {
            sys.insert("mu", double(m_memUsedBytes));
            sys.insert("mt", double(m_memTotalBytes));
        }
        if (m_diskTotalBytes > 0) {
            sys.insert("du", double(m_diskUsedBytes));
            sys.insert("dt", double(m_diskTotalBytes));
        }
        if (m_cpuPercent >= 0.0)
            sys.insert("cpu", m_cpuPercent);
        message.insert("sys", sys);
    }
    if ((type == QLatin1String("hello") || type == QLatin1String("presence")) &&
        m_diagnosticsMs > 0)
        message.insert("dg", NodeDiagnostics::toJson(m_diagnostics));
    // The sender's away flag and custom status ride the same two frames the
    // web and phone clients stamp it on. Older receivers ignore the key.
    if ((type == QLatin1String("hello") || type == QLatin1String("presence")) &&
        !m_userStatus.isEmpty())
        message.insert("userStatus", m_userStatus);
    return message;
}

bool ServerNode::isControllerReadableFrame(const QJsonObject &plain) const
{
    const QString type = plain.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("chat") ||
        type == QLatin1String("thread-reply")) {
        if (plain.value(QStringLiteral("private")).toBool())
            return false;
    } else if (type != QLatin1String("reaction") &&
               type != QLatin1String("edit") &&
               type != QLatin1String("delete") &&
               type != QLatin1String("admin-delete") &&
               type != QLatin1String("typing")) {
        return false;
    }
    const QString conversation = frameConversation(plain);
    if (!conversation.startsWith(QLatin1Char('#')) ||
        m_privateChannels.contains(conversation)) {
        return false;
    }
    if (type != QLatin1String("typing") &&
        type != QLatin1String("chat") &&
        type != QLatin1String("thread-reply")) {
        const QString target = plain.value(QStringLiteral("target")).toString();
        // A public controller frame must not mutate a protected message merely
        // by claiming that its opaque id belongs to a public conversation.
        // Bind mutations to the conversation recorded with the target.
        if (target.isEmpty() ||
            m_messageConversation.value(target) != conversation) {
            return false;
        }
    }
    return true;
}

void ServerNode::setDirectRoom(bool direct)
{
    if (m_directRoom == direct)
        return;
    m_directRoom = direct;
    rebuildSeal();
}

void ServerNode::rememberOutbox(const QString &ref, const QJsonObject &plain,
                                const QJsonObject &sealed)
{
    if (ref.isEmpty())
        return;
    OutboxEntry entry;
    entry.ref = ref;
    entry.plain = plain;
    for (const QJsonValue &value : sealed.value(QStringLiteral("keys")).toArray())
        entry.keyIds.insert(value.toObject().value(QStringLiteral("kid")).toString());
    const qint64 ts = qint64(plain.value(QStringLiteral("ts")).toDouble());
    entry.ts = ts > 0 ? ts : QDateTime::currentMSecsSinceEpoch();
    const QString id = plain.value(QStringLiteral("id")).toString();
    for (qsizetype i = m_outbox.size() - 1; i >= 0; --i) {
        if (m_outbox.at(i).ref == ref ||
            (!id.isEmpty() &&
             m_outbox.at(i).plain.value(QStringLiteral("id")).toString() == id))
            m_outbox.removeAt(i);
    }
    m_outbox.append(entry);
    while (m_outbox.size() > forkmesh::recipients::kReceivedRefsPerFrame)
        m_outbox.removeFirst();
}

void ServerNode::resealOutbox()
{
    if (!m_wsReady || m_outbox.isEmpty())
        return;
    const qint64 cutoff = QDateTime::currentMSecsSinceEpoch() - kOutboxRetainMs;
    const QStringList known = m_seal.peerKeyIds();
    QList<OutboxEntry> stale;
    QList<OutboxEntry> kept;
    for (const OutboxEntry &entry : std::as_const(m_outbox)) {
        if (entry.ts < cutoff)
            continue;
        bool missing = false;
        for (const QString &keyId : known)
            missing = missing || !entry.keyIds.contains(keyId);
        (missing ? stale : kept).append(entry);
    }
    m_outbox = kept;
    if (stale.isEmpty())
        return;
    QStringList superseded;
    for (const OutboxEntry &entry : std::as_const(stale)) {
        superseded.append(entry.ref);
        sendEncrypted(entry.plain, false, true);
    }
    for (qsizetype start = 0; start < superseded.size();
         start += forkmesh::recipients::kReceivedRefsPerFrame)
        sendTextFrame(forkmesh::recipients::supersededFrame(superseded.mid(
                          start, forkmesh::recipients::kReceivedRefsPerFrame)),
                      QStringLiteral("superseded"), QString(), QString());
}

void ServerNode::recoverSession(const QString &kid, qint64 ctr)
{
    if (!m_directRoom || !m_wsReady || kid.isEmpty() || !m_seal.usesRatchet() ||
        kid == m_seal.keyId() || m_seal.openedLaterThan(kid, ctr))
        return;
    if (!m_seal.claimSessionReset(kid))
        return;
    m_seal.resetSessions(kid);
    sendSealedTo(QJsonObject{{QStringLiteral("type"), forkmesh::chatv4::resetType()},
                             {QStringLiteral("senderId"), m_nodeId}},
                 kid, false);
    sendKeyAdvertisement(true);
}

void ServerNode::resealOutboxTo(const QString &kid)
{
    if (!m_wsReady || kid.isEmpty() || kid == m_seal.keyId())
        return;
    const qint64 cutoff = QDateTime::currentMSecsSinceEpoch() - kOutboxRetainMs;
    const QList<OutboxEntry> entries = m_outbox;
    for (const OutboxEntry &entry : entries)
        if (entry.ts >= cutoff)
            sendSealedTo(entry.plain, kid, true);
}

bool ServerNode::sendSealedTo(const QJsonObject &plain, const QString &kid, bool resend)
{
    if (!m_wsReady)
        return false;
    const QJsonObject framed = m_seal.sealObjectFor(plain, {kid});
    if (framed.isEmpty())
        return false;
    QJsonObject envelope = m_crypto.encryptObject(framed);
    if (envelope.isEmpty())
        return false;
    envelope.insert(QStringLiteral("persist"), true);
    const QString type = plain.value(QStringLiteral("type")).toString();
    const bool message =
        type == QLatin1String("chat") || type == QLatin1String("thread-reply");
    if (message)
        envelope.insert(QStringLiteral("notify"), true);
    if (resend)
        envelope.insert(QStringLiteral("resend"), true);
    const QByteArray payload = QJsonDocument(envelope).toJson(QJsonDocument::Compact);
    if (message)
        rememberSentRef(forkmesh::recipients::frameRef(payload),
                        plain.value(QStringLiteral("id")).toString().left(96));
    sendTextFrame(payload, type, mainnodeScopeFor(plain, true),
                  plain.value(QStringLiteral("to")).toString());
    return true;
}

void ServerNode::noteOwnEcho(const QJsonObject &envelope)
{
    const QString id = m_sentRefs.value(forkmesh::recipients::relayRef(envelope));
    const int recipients = forkmesh::recipients::countFromEnvelope(envelope);
    if (!id.isEmpty() && recipients >= 0)
        emit messageRecipientsKnown(id, recipients,
                                    forkmesh::recipients::namesFromEnvelope(envelope));
}

void ServerNode::sendEncrypted(const QJsonObject &plain, bool showActivity,
                               bool resend)
{
    const QJsonObject moderated = moderatedChatObject(plain);
    // #team is an internal bridge between the person at this desktop and the
    // configured local teammates. Keeping the whole conversation off the wire
    // is stronger than advertising a well-known private-room name and relying
    // on every other peer to honour UI membership. The normal send methods have
    // already stored and emitted the local event before reaching this boundary.
    if (frameConversation(moderated) == QLatin1String("#team") &&
        m_privateChannels.contains(QStringLiteral("#team"))) {
        if (showActivity) {
            emit systemMessage(
                "Special Agents: kept the private special agent conversation on this device.");
        }
        return;
    }
    // Public channels deliberately use only the controller-readable room
    // layer. Everything else gets the automatic device-to-device layer first.
    const QJsonObject framed = isControllerReadableFrame(moderated)
                                   ? moderated
                                   : sealForRoom(moderated);
    if (framed.isEmpty()) {
        emit systemMessage("Mainnode room seal failed; message was not sent.");
        return;
    }
    QJsonObject envelope = m_crypto.encryptObject(framed);
    if (envelope.isEmpty()) {
        emit systemMessage("Mainnode encryption failed; message was not sent.");
        return;
    }
    // Tag durable conversation messages so the relay can retain and replay
    // them to nodes that join later. Public content is readable by controller
    // integrations; retained private content remains recipient-sealed.
    const QString type = moderated.value("type").toString();
    const bool durable = isDurableMainnodeType(type);
    if (durable)
        envelope.insert("persist", true);
    // The relay only sees ciphertext and also retains edits, reactions and key
    // advertisements, so a message says in the clear that it is one: that is
    // what lets the relay wake the room's phones for it and for nothing else.
    if (type == QLatin1String("chat") || type == QLatin1String("thread-reply"))
        envelope.insert("notify", true);
    if (resend)
        envelope.insert(QStringLiteral("resend"), true);
    const QByteArray payload = QJsonDocument(envelope).toJson(QJsonDocument::Compact);
    if (type == QLatin1String("chat") || type == QLatin1String("thread-reply")) {
        const QString id = moderated.value(QStringLiteral("id")).toString();
        rememberRecipientRef(payload, id);
        rememberSentRef(forkmesh::recipients::frameRef(payload), id.left(96));
    }
    if (m_directRoom && durable && framed != moderated && m_wsReady)
        rememberOutbox(forkmesh::recipients::frameRef(payload), moderated, framed);
    // A direct message, or history sent to one peer, is addressed to it.
    sendTextFrame(payload,
                  type, mainnodeScopeFor(moderated, durable),
                  moderated.value(QStringLiteral("to")).toString());
    if (showActivity)
        emit systemMessage("Network: sent encrypted " + type +
                           " through mainnode room " + m_roomName + ".");
}

void ServerNode::sendKeyAdvertisement(bool persist)
{
    if (!m_wsReady || !m_seal.isValid())
        return;
    const QJsonObject bundle = m_seal.keyFrame();
    if (bundle.isEmpty())
        return;
    QJsonObject envelope = m_crypto.encryptObject(bundle);
    if (envelope.isEmpty())
        return;
    if (persist)
        envelope.insert(QStringLiteral("persist"), true);
    sendTextFrame(QJsonDocument(envelope).toJson(QJsonDocument::Compact),
                  QStringLiteral("e2ee-key"),
                  persist ? QStringLiteral("durable")
                          : QStringLiteral("ephemeral"));
}

void ServerNode::scheduleKeyAdvertisement()
{
    if (!m_keyAdvertiseTimer) {
        m_keyAdvertiseTimer = new QTimer(this);
        m_keyAdvertiseTimer->setSingleShot(true);
        connect(m_keyAdvertiseTimer, &QTimer::timeout, this,
                [this] { sendKeyAdvertisement(false); });
    }
    if (!m_keyAdvertiseTimer->isActive())
        m_keyAdvertiseTimer->start(25);
}

void ServerNode::sampleSystemStats()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_lastStatsSampleMs != 0 && now - m_lastStatsSampleMs < 10000)
        return;
    m_lastStatsSampleMs = now;

    QSettings settings;
    const bool showCpu = settings.value(TelemetrySettings::kReportCpu, false).toBool();
    const bool showMem = settings.value(TelemetrySettings::kReportMemory, false).toBool();
    const bool showDisk = settings.value(TelemetrySettings::kReportDisk, false).toBool();

    if (showMem) {
        const qint64 total = SystemStats::totalMemoryBytes();
        const qint64 avail = SystemStats::availableMemoryBytes();
        m_memTotalBytes = total;
        m_memUsedBytes = (total > 0 && avail > 0) ? qMax(qint64(0), total - avail) : 0;
    } else {
        m_memTotalBytes = 0;
        m_memUsedBytes = 0;
    }

    if (showDisk) {
        QString diskPath =
            forkmesh::paths::dataRoot();
        if (diskPath.isEmpty() || !QDir(diskPath).exists())
            diskPath = QDir::homePath();
        const qint64 diskTotal = SystemStats::diskTotalBytes(diskPath);
        const qint64 diskFree = SystemStats::diskFreeBytes(diskPath);
        m_diskTotalBytes = diskTotal;
        m_diskUsedBytes =
            (diskTotal > 0 && diskFree >= 0) ? qMax(qint64(0), diskTotal - diskFree) : 0;
    } else {
        m_diskTotalBytes = 0;
        m_diskUsedBytes = 0;
    }

    m_cpuPercent = showCpu ? SystemStats::hostCpuPercent() : -1.0;

    sampleDiagnostics();
}

// adhoc #2104: the collector stats filesystems, counts inodes and file
// descriptors and tails every log file. On a loaded box one pass blocked the
// GUI thread for 5.2 s (stalls.log 2026-09-11). Nothing here has to finish
// before the frame this was called from is sent — the result is memoised for a
// minute either way — so a due run goes to a worker and this heartbeat carries
// the previous findings. The collector is a process-wide singleton and locks
// its own state, so the worker needs no handle on any node.
void ServerNode::sampleDiagnostics()
{
    if (!QSettings()
             .value(TelemetrySettings::kReportDiagnostics, true)
             .toBool()) {
        m_diagnostics.clear();
        m_diagnosticsMs = 0;
        return;
    }
    NodeDiagnostics::Collector &collector = NodeDiagnostics::hostCollector();
    QString dataDir = forkmesh::paths::dataRoot();
    if (dataDir.isEmpty() || !QDir(dataDir).exists())
        dataDir = QDir::homePath();
    collector.setDataDir(dataDir);
    collector.noteBackpressureDrops(m_backpressureDrops);
    static std::atomic_bool sampling{false};
    if (collector.runIsDue() && !sampling.exchange(true)) {
        QThread *worker = QThread::create([] {
            NodeDiagnostics::hostCollector().run();
            sampling.store(false);
        });
        connect(worker, &QThread::finished, worker, &QObject::deleteLater);
        worker->start();
    }
    m_diagnostics = collector.findings();
    m_diagnosticsMs = collector.lastRunMs();
}

void ServerNode::sendHello(bool force, bool showActivity)
{
    if (!m_wsReady)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!force && m_lastHelloSentMs > 0) {
        const qint64 remaining =
            kHelloAdvertiseMinIntervalMs - (now - m_lastHelloSentMs);
        if (remaining > 0) {
            if (!m_helloAdvertiseTimer) {
                m_helloAdvertiseTimer = new QTimer(this);
                m_helloAdvertiseTimer->setSingleShot(true);
                connect(m_helloAdvertiseTimer, &QTimer::timeout, this,
                        [this] { sendHello(true, false); });
            }
            if (!m_helloAdvertiseTimer->isActive())
                m_helloAdvertiseTimer->start(int(qMin<qint64>(
                    remaining, kHelloAdvertiseMinIntervalMs)));
            return;
        }
    }
    m_lastHelloSentMs = now;
    sampleSystemStats();
    QJsonArray channels;
    for (const QString &channel : std::as_const(m_channels))
        if (!m_privateChannels.contains(channel))
            channels.append(channel);
    QJsonObject hello = makeMessage("hello");
    hello.insert("channels", channels);
    sendEncrypted(hello, showActivity);
}

void ServerNode::sendPresence()
{
    if (!m_wsReady)
        return;
    sampleSystemStats();
    sendEncrypted(makeMessage("presence"), false);
}

void ServerNode::notifyCoveOpened(const QString &creatorKey, const QString &coveId,
                                  const QString &coveName, const QString &openerKey,
                                  const QString &openerName, qint64 ts,
                                  const QString &signature)
{
    if (creatorKey.trimmed().isEmpty() || !m_wsReady)
        return;
    QJsonObject message = makeMessage("cove-open");
    message.insert("creator", creatorKey.left(120));
    message.insert("coveId", coveId.left(80));
    message.insert("coveName", coveName.left(160));
    message.insert("openerKey", openerKey.left(120));
    message.insert("openerName", openerName.left(80));
    message.insert("coveTs", double(ts));
    message.insert("sig", signature.left(200));
    markSeen(message.value("id").toString());
    sendEncrypted(message, false);
}

void ServerNode::notifyCoveInvited(const QString &inviteeAccount, const QString &coveId,
                                   const QString &coveName, const QString &inviterName,
                                   const QString &shareUrl, qint64 ts)
{
    if (inviteeAccount.trimmed().isEmpty() || !m_wsReady)
        return;
    QJsonObject message = makeMessage("cove-invite");
    message.insert("invitee", inviteeAccount.left(120));
    message.insert("coveId", coveId.left(80));
    message.insert("coveName", coveName.left(160));
    message.insert("inviterName", inviterName.left(80));
    message.insert("shareUrl", shareUrl.left(512));
    message.insert("coveTs", double(ts));
    markSeen(message.value("id").toString());
    sendEncrypted(message, false);
}

QString ServerNode::moderatedChatText(const QString &text)
{
    return filteredChatText(text).left(kMaxTextChars);
}

QString ServerNode::sendChat(const QString &channel, const QString &text)
{
    return sendChatWithId(channel, text, QString());
}

QString ServerNode::sendChatWithId(const QString &channel, const QString &text,
                                   const QString &messageId)
{
    const QString moderated = moderatedChatText(text);
    if (moderated.trimmed().isEmpty())
        return QString();
    QJsonObject message = makeMessage("chat");
    if (!messageId.trimmed().isEmpty())
        message.insert("id", messageId.trimmed().left(96));
    message.insert("channel", channel);
    message.insert("text", moderated);
    if (m_privateChannels.contains(channel))
        message.insert("private", true);
    markSeen(message.value("id").toString());
    storeHistory(message);
    sendEncrypted(message, true);
    emitChat(message);
    return message.value(QStringLiteral("id")).toString();
}

void ServerNode::sendThreadReply(const QString &channel,
                                 const QString &rootMessageId,
                                 const QString &text)
{
    const QString rootId = rootMessageId.trimmed().left(96);
    const QString moderated = filteredChatText(text).left(kMaxTextChars);
    if (channel.trimmed().isEmpty() || rootId.isEmpty() ||
        moderated.trimmed().isEmpty())
        return;
    QJsonObject message = makeMessage("thread-reply");
    message.insert("channel", channel);
    message.insert("rootId", rootId);
    message.insert("text", moderated);
    if (m_privateChannels.contains(channel))
        message.insert("private", true);
    markSeen(message.value("id").toString());
    storeHistory(message);
    sendEncrypted(message, true);
    emitChat(message);
}

QString ServerNode::sendThreadReplyWithId(const QString &channel,
                                          const QString &rootMessageId,
                                          const QString &text,
                                          const QString &messageId)
{
    const QString rootId = rootMessageId.trimmed().left(96);
    const QString moderated = filteredChatText(text).left(kMaxTextChars);
    if (channel.trimmed().isEmpty() || rootId.isEmpty() ||
        moderated.trimmed().isEmpty())
        return QString();
    QJsonObject message = makeMessage("thread-reply");
    if (!messageId.trimmed().isEmpty())
        message.insert("id", messageId.trimmed().left(96));
    message.insert("channel", channel);
    message.insert("rootId", rootId);
    message.insert("text", moderated);
    if (m_privateChannels.contains(channel))
        message.insert("private", true);
    markSeen(message.value("id").toString());
    storeHistory(message);
    sendEncrypted(message, true);
    emitChat(message);
    return message.value(QStringLiteral("id")).toString();
}

void ServerNode::sendRoomReaction(const QString &conversation,
                                  const QString &messageId,
                                  const QString &emoji, bool added,
                                  const QString &frameId)
{
    if (conversation.isEmpty() || messageId.isEmpty() || emoji.isEmpty())
        return;
    QJsonObject message = makeMessage("reaction");
    if (!frameId.trimmed().isEmpty())
        message.insert("id", frameId.trimmed().left(96));
    message.insert("conversation", conversation);
    message.insert("target", messageId.left(96));
    stickers::writeWire(message, emoji);
    message.insert("reactorId", m_nodeId);
    message.insert("reactorName", m_userName);
    message.insert("added", added);
    markSeen(message.value("id").toString());
    if (m_messageConversation.contains(messageId)) {
        if (added)
            m_reactions[messageId][emoji].insert(m_nodeId, m_userName);
        else
            m_reactions[messageId][emoji].remove(m_nodeId);
    }
    sendEncrypted(message, false);
}

void ServerNode::sendRoomEdit(const QString &conversation,
                              const QString &messageId, const QString &newText,
                              const QString &frameId)
{
    const QString text = filteredChatText(newText.trimmed()).left(kMaxTextChars);
    if (conversation.isEmpty() || messageId.isEmpty() || text.trimmed().isEmpty())
        return;
    QJsonObject message = makeMessage("edit");
    if (!frameId.trimmed().isEmpty())
        message.insert("id", frameId.trimmed().left(96));
    message.insert("conversation", conversation);
    message.insert("target", messageId.left(96));
    message.insert("text", text);
    message.insert("editedAt", message.value("ts"));
    markSeen(message.value("id").toString());
    updateStoredMessage(messageId, text, false);
    sendEncrypted(message, false);
}

void ServerNode::sendRoomDelete(const QString &conversation,
                                const QString &messageId, const QString &frameId)
{
    if (conversation.isEmpty() || messageId.isEmpty())
        return;
    QJsonObject message = makeMessage("delete");
    if (!frameId.trimmed().isEmpty())
        message.insert("id", frameId.trimmed().left(96));
    message.insert("conversation", conversation);
    message.insert("target", messageId.left(96));
    markSeen(message.value("id").toString());
    updateStoredMessage(messageId, QString(), true);
    m_reactions.remove(messageId);
    m_outbox.removeIf([&messageId](const OutboxEntry &entry) {
        return entry.plain.value(QStringLiteral("id")).toString() == messageId;
    });
    sendEncrypted(message, false);
    const QString ref = m_sentRefIds.value(messageId);
    if (!m_directRoom || ref.isEmpty())
        return;
    sendTextFrame(forkmesh::recipients::supersededFrame({ref}),
                  QStringLiteral("superseded"), QString(), QString());
}

void ServerNode::setAccountKind(const QString &kind)
{
    m_accountKind = kind.trimmed().left(16);
}

void ServerNode::setRoomPassphrase(const QString &passphrase)
{
    // The passphrase fetch is async and races the initial connect (it's kicked
    // off from the first heartbeat, which only fires once start() has already
    // returned), so this backend is commonly still on the constructor's
    // fallback key by the time the real one shows up. Re-key so this session
    // matches the server-derived key every other client (web included) uses —
    // without this, Qt and web would encrypt with different keys and silently
    // fail to decrypt each other's messages.
    if (passphrase.isEmpty())
        return;
    m_crypto = RoomCrypto(m_roomName, passphrase);
    if (m_wsReady) {
        // The first advertisement may have raced out under the constructor's
        // fallback room key. Re-announce durably under the controller-issued
        // key so live peers can answer and offline peers can discover this
        // device later. Application frames remain recipient-sealed either way.
        sendKeyAdvertisement(true);
        sendHello(true, false);
    }
}

QString ServerNode::roomSealKeyId() const
{
    return m_seal.keyId();
}

void ServerNode::noteSealMismatch(const QString &keyId, bool unknownSender)
{
    if (m_reportedSealKeyIds.contains(keyId))
        return;
    m_reportedSealKeyIds.insert(keyId);
    emit roomSealMismatch(keyId);
    if (unknownSender) {
        emit systemMessage(
            QStringLiteral("An end-to-end encrypted message from device %1 is "
                           "waiting for that device's key and opens when it "
                           "arrives.")
                .arg(keyId));
        return;
    }
    if (keyId.isEmpty()) {
        emit systemMessage(
            QStringLiteral("Automatic end-to-end encryption refused an "
                           "unencrypted frame in %1.")
                .arg(m_roomName));
        return;
    }
    emit systemMessage(
        QStringLiteral("An end-to-end encrypted message from device %1 was not "
                       "addressed to this device and cannot be read.")
            .arg(keyId));
}

void ServerNode::sendBotChat(const QString &channel, const QString &text)
{
    // A ForkBot reply relayed on behalf of this client (the bot has no room
    // connection of its own). Mirrors the web clients' makeForkbotPlain frame:
    // sender/senderId "forkbot", accountKind "user" so every surface — web
    // and desktop — renders it. Never inject one into a private channel or DM:
    // those are recipient-sealed, while ForkBot intentionally operates only on
    // controller-readable public channels.
    const QString trimmed = filteredChatText(text).trimmed();
    if (trimmed.isEmpty() || !channel.startsWith(QLatin1Char('#')) ||
        m_privateChannels.contains(channel))
        return;
    QJsonObject message{{"type", QStringLiteral("chat")},
                        {"id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                        {"senderId", QStringLiteral("forkbot")},
                        {"sender", QStringLiteral("forkbot")},
                        {"accountKind", QStringLiteral("user")},
                        {"ts", double(QDateTime::currentMSecsSinceEpoch())},
                        {"channel", channel},
                        {"text", trimmed.left(kMaxTextChars)}};
    markSeen(message.value("id").toString());
    storeHistory(message);
    sendEncrypted(message, false);
    emitChat(message);
}

void ServerNode::sendBotThreadReply(const QString &channel,
                                    const QString &rootMessageId,
                                    const QString &text)
{
    const QString trimmed = filteredChatText(text).trimmed();
    const QString rootId = rootMessageId.trimmed().left(96);
    // ForkBot has no identity or private-room key of its own. Only relay an
    // answer in the public thread that asked for it.
    if (trimmed.isEmpty() || rootId.isEmpty() ||
        !channel.startsWith(QLatin1Char('#')) ||
        m_privateChannels.contains(channel)) {
        return;
    }
    QJsonObject message{{"type", QStringLiteral("thread-reply")},
                        {"id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                        {"senderId", QStringLiteral("forkbot")},
                        {"sender", QStringLiteral("forkbot")},
                        {"accountKind", QStringLiteral("user")},
                        {"ts", double(QDateTime::currentMSecsSinceEpoch())},
                        {"channel", channel},
                        {"rootId", rootId},
                        {"text", trimmed.left(kMaxTextChars)}};
    markSeen(message.value("id").toString());
    storeHistory(message);
    sendEncrypted(message, false);
    emitChat(message);
}

void ServerNode::sendTeammateChat(const QString &channel,
                                  const QString &teammateId,
                                  const QString &teammateName,
                                  const QString &text)
{
    const QString cleanChannel = channel.trimmed().left(120);
    const QString cleanId = teammateId.trimmed().left(64);
    const QString cleanName = teammateName.trimmed().left(80);
    const QString moderated = filteredChatText(text).trimmed();
    const bool privateTeam =
        cleanChannel == QLatin1String("#team") &&
        m_privateChannels.contains(cleanChannel);
    // General teammate relays remain public-only. #team is the narrow private,
    // device-local exception for configured teammates on this desktop.
    if (!cleanChannel.startsWith(QLatin1Char('#')) ||
        (m_privateChannels.contains(cleanChannel) && !privateTeam) ||
        cleanId.isEmpty() ||
        cleanName.isEmpty() || moderated.isEmpty()) {
        return;
    }
    QJsonObject message{{"type", QStringLiteral("chat")},
                        {"id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                        {"senderId", QStringLiteral("teammate:%1").arg(cleanId)},
                        {"sender", cleanName},
                        {"accountKind", QStringLiteral("user")},
                        {"ts", double(QDateTime::currentMSecsSinceEpoch())},
                        {"channel", cleanChannel},
                        {"text", moderated.left(kMaxTextChars)}};
    if (privateTeam)
        message.insert(QStringLiteral("private"), true);
    markSeen(message.value("id").toString());
    storeHistory(message);
    sendEncrypted(message, privateTeam);
    emitChat(message);
}

void ServerNode::sendTeammateThreadReply(const QString &channel,
                                         const QString &rootMessageId,
                                         const QString &teammateId,
                                         const QString &teammateName,
                                         const QString &text)
{
    const QString cleanChannel = channel.trimmed().left(120);
    const QString rootId = rootMessageId.trimmed().left(96);
    const QString cleanId = teammateId.trimmed().left(64);
    const QString cleanName = teammateName.trimmed().left(80);
    const QString moderated = filteredChatText(text).trimmed();
    if (cleanChannel != QLatin1String("#team") ||
        !m_privateChannels.contains(cleanChannel) || rootId.isEmpty() ||
        cleanId.isEmpty() || cleanName.isEmpty() || moderated.isEmpty()) {
        return;
    }
    QJsonObject message{
        {"type", QStringLiteral("thread-reply")},
        {"id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {"senderId", QStringLiteral("teammate:%1").arg(cleanId)},
        {"sender", cleanName},
        {"accountKind", QStringLiteral("user")},
        {"ts", double(QDateTime::currentMSecsSinceEpoch())},
        {"channel", cleanChannel},
        {"rootId", rootId},
        {"text", moderated.left(kMaxTextChars)},
        {"private", true},
    };
    markSeen(message.value(QStringLiteral("id")).toString());
    storeHistory(message);
    sendEncrypted(message, true);
    emitChat(message);
}

void ServerNode::sendTeamNotice(const QString &channel,
                                const QString &rootMessageId,
                                const QString &text)
{
    const QString cleanChannel = channel.trimmed().left(120);
    const QString rootId = rootMessageId.trimmed().left(96);
    const QString moderated = filteredChatText(text).trimmed();
    if (cleanChannel != QLatin1String("#team") ||
        !m_privateChannels.contains(cleanChannel) || rootId.isEmpty() ||
        moderated.isEmpty()) {
        return;
    }
    QJsonObject message{
        {"type", QStringLiteral("thread-reply")},
        {"id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {"senderId", QStringLiteral("team-system")},
        {"sender", QStringLiteral("Special Agents")},
        {"accountKind", QStringLiteral("system")},
        {"ts", double(QDateTime::currentMSecsSinceEpoch())},
        {"channel", cleanChannel},
        {"rootId", rootId},
        {"text", moderated.left(kMaxTextChars)},
        {"private", true},
    };
    markSeen(message.value(QStringLiteral("id")).toString());
    storeHistory(message);
    // #team never leaves this desktop; the notice names a local account
    // requirement and must not reveal it to the mesh either.
    sendEncrypted(message, true);
    emitChat(message);
}

void ServerNode::sendDirect(const QString &targetId, const QString &text)
{
    const QString moderated = filteredChatText(text).left(kMaxTextChars);
    if (moderated.trimmed().isEmpty() || targetId == m_nodeId)
        return;
    QJsonObject message = makeMessage("dm");
    message.insert("to", targetId);
    message.insert("text", moderated);
    markSeen(message.value("id").toString());
    sendEncrypted(message, true);
    emitDm(message, targetId);
}

void ServerNode::sendFile(const QString &conversation, const QString &fileName,
                          const QString &mimeType, const QByteArray &data)
{
    if (fileName.isEmpty() || data.isEmpty() || data.size() > kMaxFileBytes)
        return;
    if (conversation.startsWith('@')) {
        QJsonObject message = makeMessage("dm");
        message.insert("to", conversation.mid(1));
        message.insert("fileName", safeFileName(fileName));
        message.insert("fileMime", mimeType.left(kMaxMimeChars));
        message.insert("file", QString::fromLatin1(data.toBase64()));
        markSeen(message.value("id").toString());
        sendEncrypted(message, true);
        emitDm(message, conversation.mid(1));
    } else {
        sendFileWithId(conversation, fileName, mimeType, data, QString());
    }
}

QString ServerNode::sendFileWithId(const QString &channel, const QString &fileName,
                                   const QString &mimeType, const QByteArray &data,
                                   const QString &messageId)
{
    if (channel.trimmed().isEmpty() || fileName.isEmpty() || data.isEmpty() ||
        data.size() > kMaxFileBytes)
        return QString();
    QJsonObject message = makeMessage("chat");
    if (!messageId.trimmed().isEmpty())
        message.insert("id", messageId.trimmed().left(96));
    message.insert("channel", channel);
    message.insert("fileName", safeFileName(fileName));
    message.insert("fileMime", mimeType.left(kMaxMimeChars));
    message.insert("file", QString::fromLatin1(data.toBase64()));
    if (m_privateChannels.contains(channel))
        message.insert("private", true);
    markSeen(message.value("id").toString());
    storeHistory(message);
    sendEncrypted(message, true);
    emitChat(message);
    return message.value(QStringLiteral("id")).toString();
}

void ServerNode::sendReaction(const QString &conversation, const QString &messageId,
                              const QString &emoji)
{
    if (messageId.isEmpty() || emoji.isEmpty())
        return;
    const bool added =
        !m_reactions.value(messageId).value(emoji).contains(m_nodeId);
    QJsonObject message = makeMessage("reaction");
    message.insert("conversation", conversation);
    message.insert("target", messageId);
    stickers::writeWire(message, emoji);
    message.insert("reactorId", m_nodeId);
    message.insert("reactorName", m_userName);
    message.insert("added", added);
    handlePlain(message);
    sendEncrypted(message, false);
}

void ServerNode::editMessage(const QString &conversation, const QString &messageId,
                             const QString &newText)
{
    const QString text = newText.trimmed().left(kMaxTextChars);
    if (messageId.isEmpty() || text.isEmpty() ||
        !messageIsAuthoredBy(messageId, m_nodeId))
        return;
    QJsonObject message = makeMessage("edit");
    message.insert("conversation", conversation);
    message.insert("target", messageId);
    message.insert("text", text);
    handlePlain(message);
    sendEncrypted(message, false);
}

void ServerNode::deleteMessage(const QString &conversation, const QString &messageId)
{
    if (messageId.isEmpty() || !messageIsAuthoredBy(messageId, m_nodeId))
        return;
    QJsonObject message = makeMessage("delete");
    message.insert("conversation", conversation);
    message.insert("target", messageId);
    handlePlain(message);
    sendEncrypted(message, false);
}

void ServerNode::sendAdminDelete(const QString &conversation, const QString &messageId,
                                 qint64 ts, const QString &signature)
{
    if (messageId.isEmpty() || signature.isEmpty())
        return;
    // The UI signed a canonical string that binds this exact ts/messageId/
    // conversation to the admin's key; carry the same ts (overriding the one
    // makeMessage stamps) so every receiver reconstructs the identical string.
    QJsonObject message = makeMessage("admin-delete");
    message.insert("ts", double(ts));
    message.insert("conversation", conversation);
    message.insert("target", messageId);
    message.insert("sig", signature);
    sendEncrypted(message, false);
}

void ServerNode::applyAdminDelete(const QString &conversation,
                                  const QString &messageId)
{
    // Called by the UI only after it verified the signature and admin status.
    applyDeleteUnchecked(conversation, messageId);
}

void ServerNode::setAvatar(const QByteArray &pngData)
{
    if (pngData.size() > kMaxAvatarBytes)
        return;
    m_avatarPng = pngData;
    QJsonObject message = makeMessage("avatar");
    message.insert("png", QString::fromLatin1(pngData.toBase64()));
    markSeen(message.value("id").toString());
    emit avatarChanged(m_nodeId, pngData);
    sendEncrypted(message, false);
}

void ServerNode::setUserName(const QString &name)
{
    const QString trimmed = name.trimmed().left(kMaxDisplayNameChars);
    if (trimmed.isEmpty() || trimmed == m_userName)
        return;
    m_userName = trimmed;
    updateRosterAndStatus();
    if (m_wsReady)
        sendHello();
}

void ServerNode::setNodeIdentity(const QString &nodeName, const QString &ownerUser)
{
    const QString node = nodeName.trimmed().left(kMaxDisplayNameChars);
    const QString owner = ownerUser.trimmed().left(kMaxDisplayNameChars);
    if (node == m_nodeName && owner == m_ownerUser)
        return;
    m_nodeName = node;
    m_ownerUser = owner;
    updateRosterAndStatus();
    if (m_wsReady)
        sendHello();
}

void ServerNode::setUserStatus(const QJsonObject &status)
{
    const QJsonObject cleaned =
        status.isEmpty() ? QJsonObject()
                         : forkmesh::userstatus::toJson(
                               forkmesh::userstatus::fromJson(status));
    if (cleaned == m_userStatus)
        return;
    m_userStatus = cleaned;
    updateRosterAndStatus();
    // Peers repaint the name from this frame; the 60s presence beat would
    // otherwise leave them showing the old status for up to a minute.
    sendPresence();
}

void ServerNode::forgetMember(const QString &peerId)
{
    if (peerId.isEmpty() || peerId == m_nodeId)
        return;
    if (m_peers.remove(peerId) <= 0)
        return;
    persistKnownPeers();
    updateRosterAndStatus();
    emit systemMessage("Removed member from the live roster.");
}

void ServerNode::sendTyping(const QString &conversation, bool active)
{
    QJsonObject message = makeMessage("typing");
    message.insert("conversation", conversation);
    message.insert("active", active);
    sendEncrypted(message, false);
}

QString ServerNode::hiddenChannelsSettingKey() const
{
    return QStringLiteral("chat/hiddenChannels/") + m_rosterStorageKey;
}

void ServerNode::loadHiddenChannels()
{
    const QStringList hidden =
        QSettings().value(hiddenChannelsSettingKey()).toStringList();
    m_hiddenChannels = QSet<QString>(hidden.begin(), hidden.end());
    for (const QString &name : hidden)
        m_channels.removeAll(name);
}

void ServerNode::saveHiddenChannels()
{
    QSettings().setValue(hiddenChannelsSettingKey(),
                         QStringList(m_hiddenChannels.begin(),
                                     m_hiddenChannels.end()));
}

void ServerNode::addChannel(const QString &channel)
{
    QString name = channel.trimmed();
    if (name.isEmpty())
        return;
    if (!name.startsWith('#'))
        name.prepend('#');
    if (m_hiddenChannels.contains(name) ||
        forkmesh::office::isRetiredRelayChannel(name))
        return;
    if (!m_channels.contains(name)) {
        m_channels.append(name);
        emit channelsChanged(m_channels);
    }
    QJsonObject message = makeMessage("channel");
    message.insert("name", name);
    markSeen(message.value("id").toString());
    sendEncrypted(message, true);
}

void ServerNode::removeChannel(const QString &channel)
{
    QString name = channel.trimmed();
    if (name.isEmpty())
        return;
    if (!name.startsWith('#'))
        name.prepend('#');
    m_hiddenChannels.insert(name);
    saveHiddenChannels();
    m_privateChannels.remove(name);
    if (m_channels.removeAll(name) > 0)
        emit channelsChanged(m_channels);
    m_channelHistory.remove(name);
    m_channelHistoryChars.remove(name);
}

void ServerNode::createPrivateChannel(const QString &channel)
{
    QString name = channel.trimmed();
    if (name.isEmpty())
        return;
    if (!name.startsWith('#'))
        name.prepend('#');
    if (m_hiddenChannels.remove(name))
        saveHiddenChannels();
    m_privateChannels.insert(name);
    if (!m_channels.contains(name)) {
        m_channels.append(name);
        emit channelsChanged(m_channels);
    }
    emit privateChannelJoined(name);
}

void ServerNode::shutdown()
{
    m_userStopped = true; // intentional leave: stop the auto-reconnect loop
    if (m_reconnectTimer)
        m_reconnectTimer->stop();
    if (m_pingTimer)
        m_pingTimer->stop();
    if (m_presenceTimer)
        m_presenceTimer->stop();
    if (m_connectTimeoutTimer)
        m_connectTimeoutTimer->stop();
    QJsonObject bye = makeMessage("bye");
    sendEncrypted(bye, true);
    const bool wasConnected = m_wsReady;
    closeCurrentSocket();
    m_attemptActive = false;
    m_wsReady = false;
    m_wsConnectedAtMs = 0;
    if (wasConnected)
        emit connectionChanged(false);
    emit networkDiagnosticsChanged();
}

void ServerNode::handlePlain(const QJsonObject &message, int envelopeRecipients,
                             const QStringList &envelopeRecipientNames)
{
    if (!messageHasSafePayload(message)) {
        emit systemMessage("Dropped oversized or unsafe mainnode payload.");
        return;
    }
    const QString type = message.value("type").toString();
    if (type != "typing" && !markSeen(message.value("id").toString()))
        return;
    // No remote frame can join the device-local Team conversation, including
    // frames from older clients that treated the well-known name as a room.
    if (frameConversation(message) == QLatin1String("#team"))
        return;
    const QString senderId = message.value("senderId").toString();
    const QString sender = message.value("sender").toString();
    const QString nodeName = boundedText(message, "nodeName", kMaxDisplayNameChars);
    const QString ownerUser = boundedText(message, "ownerUser", kMaxDisplayNameChars);
    const QString accountKind = message.value("accountKind").toString().left(16);
    const QString solanaAddress = boundedText(message, "solana", kMaxSolanaAddressChars);
    const QString platform = message.value("platform").toString().left(16);
    const QString version = boundedText(message, "version", kMaxVersionChars);
    if (!senderId.isEmpty())
        rememberPeer(senderId, sender, nodeName, ownerUser, accountKind,
                     solanaAddress, platform, version);

    if (!senderId.isEmpty() && message.contains("sys") &&
        m_peers.contains(senderId)) {
        const QJsonObject sys = message.value("sys").toObject();
        Peer &peer = m_peers[senderId];
        if (sys.contains("mt")) {
            peer.memTotalBytes = qMax(qint64(0), qint64(sys.value("mt").toDouble()));
            peer.memUsedBytes = qMax(qint64(0), qint64(sys.value("mu").toDouble()));
        }
        if (sys.contains("dt")) {
            peer.diskTotalBytes = qMax(qint64(0), qint64(sys.value("dt").toDouble()));
            peer.diskUsedBytes = qMax(qint64(0), qint64(sys.value("du").toDouble()));
        }
        if (sys.contains("cpu"))
            peer.cpuPercent = qBound(0.0, sys.value("cpu").toDouble(), 100.0);
    }

    // Only a live hello/presence frame speaks for the sender's status now;
    // a stale one (older than the roster's own staleness window) could undo
    // a newer change. A frame without the key (an older client, or one that
    // has not loaded its own status yet) keeps whatever the peer last
    // advertised, or the directory's copy.
    const qint64 frameTs = qint64(message.value("ts").toDouble());
    if (!senderId.isEmpty() && m_peers.contains(senderId) &&
        (type == QLatin1String("hello") || type == QLatin1String("presence")) &&
        frameTs > 0 &&
        QDateTime::currentMSecsSinceEpoch() - frameTs <= kPeerStaleMs) {
        forkmesh::userstatus::UserStatus status;
        if (forkmesh::userstatus::readFrom(message, &status)) {
            Peer &peer = m_peers[senderId];
            if (!peer.hasUserStatus || peer.userStatus != status) {
                peer.userStatus = status;
                peer.hasUserStatus = true;
                updateRosterAndStatus();
            }
        }
    }

    if (!senderId.isEmpty() && message.contains("dg") &&
        m_peers.contains(senderId)) {
        Peer &peer = m_peers[senderId];
        peer.diagnostics =
            NodeDiagnostics::fromJson(message.value("dg").toArray());
        peer.diagnosticsMs = QDateTime::currentMSecsSinceEpoch();
    }
    // Heartbeats are clock samples: our time minus the sender's. A drifting
    // clock gets caught here rather than when signed requests start being
    // rejected. Only hello/presence — durable chat frames are replayed from the
    // relay's retained history with their original timestamps, which would read
    // as hours of skew.
    if (!senderId.isEmpty() && senderId != m_nodeId &&
        (type == QLatin1String("hello") || type == QLatin1String("presence"))) {
        const qint64 peerMs = qint64(message.value("ts").toDouble());
        if (peerMs > 0)
            NodeDiagnostics::hostCollector().notePeerTimestamp(
                peerMs, QDateTime::currentMSecsSinceEpoch());
    }

    if (type == "hello") {
        bool changed = false;
        for (const auto &value : message.value("channels").toArray()) {
            const QString channel = value.toString();
            if (!channel.isEmpty() && !m_channels.contains(channel) &&
                !m_hiddenChannels.contains(channel) &&
                !forkmesh::office::isRetiredRelayChannel(channel)) {
                m_channels.append(channel);
                changed = true;
            }
        }
        if (changed)
            emit channelsChanged(m_channels);
        if (m_peers.contains(senderId)) {
            const QList<MirrorAdvert> mirrors = readMirrors(message);
            m_peers[senderId].mirrorDetails = mirrors;
            m_peers[senderId].mirrors = mirrorNames(mirrors);
            updateRosterAndStatus();
        }
        if (message.value("to").toString().isEmpty()) {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            const qint64 last = m_lastHelloReplyMs.value(senderId, 0);
            if (last <= 0 || now - last >= kHelloReplyMinIntervalMs) {
                m_lastHelloReplyMs.insert(senderId, now);
                sendHistoryTo(senderId);
                QJsonArray channels;
                for (const QString &channel : std::as_const(m_channels))
                    if (!m_privateChannels.contains(channel))
                        channels.append(channel);
                QJsonObject reply = makeMessage("hello");
                reply.insert("channels", channels);
                reply.insert("to", senderId);
                sendEncrypted(reply, false);
            }
        }
    } else if (type == "chat" || type == "thread-reply") {
        const QJsonObject moderated = moderatedChatObject(message);
        const QString channel = moderated.value("channel").toString();
        if (type == "thread-reply" &&
            moderated.value("rootId").toString().trimmed().isEmpty())
            return;
        if (moderated.value("private").toBool() && !m_channels.contains(channel))
            return;
        if (m_hiddenChannels.contains(channel) ||
            forkmesh::office::isRetiredRelayChannel(channel))
            return;
        if (!m_channels.contains(channel))
            m_channels.append(channel);
        storeHistory(moderated);
        emitChat(moderated, envelopeRecipients, envelopeRecipientNames);
    } else if (type == "dm") {
        const QString to = message.value("to").toString();
        if (to == m_nodeId)
            emitDm(moderatedChatObject(message), senderId, envelopeRecipients,
                   envelopeRecipientNames);
    } else if (type == "channel") {
        const QString name = message.value("name").toString();
        if (!m_channels.contains(name) && !m_hiddenChannels.contains(name) &&
            !forkmesh::office::isRetiredRelayChannel(name)) {
            m_channels.append(name);
            emit channelsChanged(m_channels);
        }
    } else if (type == "mirror-update") {
        const QString repo = message.value("repo").toString().left(160);
        if (!repo.isEmpty())
            emit mirrorUpdated(repo, sender,
                               message.value("commit").toString().left(64));
    } else if (type == "mirror-synced") {
        const QString repo = message.value("repo").toString().left(160);
        if (!repo.isEmpty())
            emit mirrorSynced(repo, sender,
                              message.value("commit").toString().left(64));
    } else if (type == "cove-open") {
        const QString creator = message.value("creator").toString().left(120);
        if (!creator.isEmpty())
            emit coveOpened(creator, message.value("coveId").toString().left(80),
                            message.value("coveName").toString().left(160),
                            message.value("openerKey").toString().left(120),
                            message.value("openerName").toString().left(80),
                            qint64(message.value("coveTs").toDouble()),
                            message.value("sig").toString().left(200));
    } else if (type == "cove-invite") {
        const QString invitee = message.value("invitee").toString().left(120);
        if (!invitee.isEmpty())
            emit coveInvited(invitee, message.value("coveId").toString().left(80),
                             message.value("coveName").toString().left(160),
                             message.value("inviterName").toString().left(80),
                             message.value("shareUrl").toString().left(512),
                             qint64(message.value("coveTs").toDouble()));
    } else if (type == "reaction" || type == "edit" || type == "delete") {
        emit roomMutationArrived(message);
    }
    if (type == "reaction") {
        const QString target = message.value("target").toString();
        const QString emoji = stickers::valueFromWire(message);
        const QString reactorId = message.value("reactorId").toString();
        if (!reactorId.isEmpty() && reactorId != senderId)
            return;
        const QString reactorName = message.value("reactorName").toString();
        if (m_messageConversation.contains(target)) {
            if (message.value("added").toBool())
                m_reactions[target][emoji].insert(reactorId, reactorName);
            else
                m_reactions[target][emoji].remove(reactorId);
        }
        emit reactionChanged(message.value("conversation").toString(), target,
                             emoji, reactorName, message.value("added").toBool());
    } else if (type == "edit") {
        const QString target = message.value("target").toString();
        if (messageIsAuthoredBy(target, senderId))
            applyEdit(message.value("conversation").toString(), target, senderId,
                      chatBodyText(message, kMaxTextChars));
    } else if (type == "delete") {
        const QString target = message.value("target").toString();
        if (messageIsAuthoredBy(target, senderId))
            applyDelete(message.value("conversation").toString(), target, senderId);
    } else if (type == "admin-delete") {
        // A signed moderation delete. We can't authorise it here (admin status
        // lives server-side); hand it to the UI to verify the signature against
        // senderId and confirm that node is an admin, then apply.
        const QString target = message.value("target").toString();
        const QString sig = message.value("sig").toString();
        const qint64 ts = qint64(message.value("ts").toDouble());
        if (!target.isEmpty() && !sig.isEmpty())
            emit adminDeleteRequested(message.value("conversation").toString(),
                                      target, senderId, sender, ts, sig);
    } else if (type == "avatar") {
        const QByteArray avatar =
            boundedBase64(message, "png", 4 * kMaxAvatarBytes / 3 + 8,
                          kMaxAvatarBytes);
        if (!avatar.isEmpty())
            emit avatarChanged(senderId, avatar);
    } else if (type == "typing") {
        emit typingChanged(message.value("conversation").toString(), senderId,
                           sender, message.value("active").toBool());
    } else if (type == "history") {
        if (!message.value("to").toString().isEmpty() &&
            message.value("to").toString() != m_nodeId)
            return;
        for (const auto &value : message.value("entries").toArray()) {
            const QJsonObject entry = moderatedChatObject(value.toObject());
            if (!messageHasSafePayload(entry))
                continue;
            if (frameConversation(entry) == QLatin1String("#team") ||
                forkmesh::office::isRetiredRelayChannel(
                    frameConversation(entry)))
                continue;
            if (entry.value("private").toBool() &&
                !m_channels.contains(entry.value("channel").toString()))
                continue;
            if (markSeen(entry.value("id").toString())) {
                storeHistory(entry);
                emitChat(entry);
            }
        }
    } else if (type == "bye") {
        if (m_peers.contains(senderId)) {
            m_peers.remove(senderId);
            emit systemMessage(sender + " left");
            updateRosterAndStatus();
        }
    }
}

void ServerNode::sendHistoryTo(const QString &peerId)
{
    if (peerId.isEmpty() || m_channelHistory.isEmpty())
        return;
    QJsonArray entries;
    for (auto it = m_channelHistory.constBegin(); it != m_channelHistory.constEnd();
         ++it) {
        if (m_privateChannels.contains(it.key()))
            continue;
        for (const QJsonObject &entry : it.value())
            entries.append(entry);
    }
    QJsonObject message = makeMessage("history");
    message.insert("to", peerId);
    message.insert("entries", entries);
    sendEncrypted(message, false);
}

void ServerNode::emitChat(const QJsonObject &message, int recipients,
                          const QStringList &recipientNames)
{
    ChatMessage out;
    out.recipientCount = recipients;
    out.recipientNames = recipientNames;
    out.id = message.value("id").toString();
    out.conversation = message.value("channel").toString();
    out.threadRootId = message.value("rootId").toString().left(96);
    out.senderId = message.value("senderId").toString();
    out.senderName = boundedText(message, "sender", kMaxDisplayNameChars);
    out.text = filteredChatText(chatBodyText(message, kMaxTextChars));
    out.timestampMs = qint64(message.value("ts").toDouble());
    out.fileName = safeFileName(message.value("fileName").toString());
    if (out.fileName == "file" && !message.contains("fileName"))
        out.fileName.clear();
    out.fileMime = boundedText(message, "fileMime", kMaxMimeChars);
    if (message.contains("file"))
        out.fileData = boundedBase64(message, "file", kMaxBase64FileChars, kMaxFileBytes);
    out.self = out.senderId == m_nodeId;
    out.edited = message.value("edited").toBool();
    out.deleted = message.value("deleted").toBool();
    indexMessage(out.id, out.conversation, out.senderId);
    emit messageArrived(out);
}

void ServerNode::emitDm(const QJsonObject &message, const QString &conversationPeer,
                        int recipients, const QStringList &recipientNames)
{
    ChatMessage out;
    out.recipientCount = recipients;
    out.recipientNames = recipientNames;
    out.id = message.value("id").toString();
    out.conversation = "@" + conversationPeer;
    out.senderId = message.value("senderId").toString();
    out.senderName = boundedText(message, "sender", kMaxDisplayNameChars);
    out.text = filteredChatText(chatBodyText(message, kMaxTextChars));
    out.timestampMs = qint64(message.value("ts").toDouble());
    out.fileName = safeFileName(message.value("fileName").toString());
    if (out.fileName == "file" && !message.contains("fileName"))
        out.fileName.clear();
    out.fileMime = boundedText(message, "fileMime", kMaxMimeChars);
    if (message.contains("file"))
        out.fileData = boundedBase64(message, "file", kMaxBase64FileChars, kMaxFileBytes);
    out.self = out.senderId == m_nodeId;
    out.edited = message.value("edited").toBool();
    out.deleted = message.value("deleted").toBool();
    indexMessage(out.id, out.conversation, out.senderId);
    emit messageArrived(out);
}

void ServerNode::rememberPeer(const QString &peerId, const QString &name,
                              const QString &nodeName, const QString &ownerUser,
                              const QString &accountKind,
                              const QString &solanaAddress,
                              const QString &platform,
                              const QString &version, bool online)
{
    if (peerId.isEmpty() || peerId == m_nodeId)
        return;
    Peer &peer = m_peers[peerId];
    peer.name = name.isEmpty() ? peer.name : name;
    if (!nodeName.trimmed().isEmpty())
        peer.nodeName = nodeName.trimmed().left(kMaxDisplayNameChars);
    if (!ownerUser.trimmed().isEmpty())
        peer.ownerUser = ownerUser.trimmed().left(kMaxDisplayNameChars);
    if (!accountKind.trimmed().isEmpty())
        peer.accountKind = accountKind.trimmed().left(16);
    if (!solanaAddress.trimmed().isEmpty())
        peer.solanaAddress = solanaAddress.trimmed().left(kMaxSolanaAddressChars);
    if (!platform.isEmpty())
        peer.platform = platform;
    if (!version.isEmpty())
        peer.version = version;
    peer.online = online;
    peer.lastSeenMs = QDateTime::currentMSecsSinceEpoch();
    updateRosterAndStatus();
}

void ServerNode::updateRosterAndStatus()
{
    if (!m_rosterEmitTimer) {
        m_rosterEmitTimer = new QTimer(this);
        m_rosterEmitTimer->setSingleShot(true);
        m_rosterEmitTimer->setInterval(200);
        connect(m_rosterEmitTimer, &QTimer::timeout, this,
                &ServerNode::flushRosterAndStatus);
    }
    if (!m_rosterEmitTimer->isActive())
        m_rosterEmitTimer->start();
}

void ServerNode::flushRosterAndStatus()
{
    sampleSystemStats();
    MemberInfo self;
    self.id = m_nodeId;
    self.name = m_userName;
    self.nodeName = m_nodeName.isEmpty() ? m_userName : m_nodeName;
    self.ownerUser = m_ownerUser;
    self.accountKind = m_accountKind;
    self.self = true;
    self.online = m_wsReady;
    self.solanaAddress = m_solanaAddress;
    self.platform = m_platform;
    self.version = m_version;
    self.memUsedBytes = m_memUsedBytes;
    self.memTotalBytes = m_memTotalBytes;
    self.diskUsedBytes = m_diskUsedBytes;
    self.diskTotalBytes = m_diskTotalBytes;
    self.cpuPercent = m_cpuPercent;
    self.diagnostics = m_diagnostics;
    self.diagnosticsMs = m_diagnosticsMs;
    self.hasUserStatus = !m_userStatus.isEmpty();
    if (self.hasUserStatus)
        self.userStatus = forkmesh::userstatus::fromJson(m_userStatus);
    QList<MemberInfo> members{self};
    int onlineCount = 0;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto it = m_peers.constBegin(); it != m_peers.constEnd(); ++it) {
        if (!it->online)
            continue;
        if (now - it->lastSeenMs > kPeerStaleMs)
            continue;
        MemberInfo member;
        member.id = it.key();
        member.name = it->name;
        member.nodeName = it->nodeName;
        member.ownerUser = it->ownerUser;
        member.accountKind = it->accountKind;
        member.note = QString();
        member.online = it->online;
        member.solanaAddress = it->solanaAddress;
        member.platform = it->platform;
        member.version = it->version;
        member.mirrors = it->mirrors;
        member.mirrorDetails = it->mirrorDetails;
        member.memUsedBytes = it->memUsedBytes;
        member.memTotalBytes = it->memTotalBytes;
        member.diskUsedBytes = it->diskUsedBytes;
        member.diskTotalBytes = it->diskTotalBytes;
        member.cpuPercent = it->cpuPercent;
        member.diagnostics = it->diagnostics;
        member.diagnosticsMs = it->diagnosticsMs;
        member.userStatus = it->userStatus;
        member.hasUserStatus = it->hasUserStatus;
        members.append(member);
        ++onlineCount;
    }
    emit rosterChanged(members);
    // Presence-derived status is event driven: the 60s presence beat re-runs
    // this flush so peer staleness is re-evaluated even without traffic, but
    // the line is only published when the peer count or room actually moved.
    emitStatus(QString::number(onlineCount) +
               " peer(s) online · encrypted room " + m_roomName);
}

void ServerNode::emitStatus(const QString &status)
{
    if (status == m_lastStatusText)
        return;
    m_lastStatusText = status;
    emit statusChanged(status);
}

void ServerNode::loadKnownPeers()
{
    QSettings settings;
    settings.remove(kKnownRosterGroup + "/" + m_rosterStorageKey);
}

void ServerNode::persistKnownPeers() const
{
    QSettings settings;
    settings.remove(kKnownRosterGroup + "/" + m_rosterStorageKey);
}

void ServerNode::storeHistory(const QJsonObject &message)
{
    const QString channel = message.value("channel").toString();
    if (channel.isEmpty())
        return;
    const QStringList evicted = ChatHistoryLimits::appendBounded(
        m_channelHistory[channel], message, &m_channelHistoryChars[channel]);
    for (const QString &id : evicted)
        dropMessageIndex(id);
}

void ServerNode::indexMessage(const QString &id, const QString &conversation,
                              const QString &senderId)
{
    if (id.isEmpty())
        return;
    if (!m_messageConversation.contains(id))
        m_messageIndexOrder.enqueue(id);
    m_messageConversation.insert(id, conversation);
    m_messageSender.insert(id, senderId);
    while (m_messageIndexOrder.size() > kMessageIndexLimit)
        dropMessageIndex(m_messageIndexOrder.dequeue());
}

void ServerNode::dropMessageIndex(const QString &id)
{
    m_messageConversation.remove(id);
    m_messageSender.remove(id);
    m_reactions.remove(id);
}

void ServerNode::reapStalePeers()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto it = m_peers.begin(); it != m_peers.end();) {
        const qint64 reapAfter =
            ChatVisitorPresence::isTransientVisitor(it->accountKind, it->name)
                ? ChatVisitorPresence::kVisitorIdleMs
                : kPeerReapMs;
        if (now - it->lastSeenMs > reapAfter) {
            m_lastHelloReplyMs.remove(it.key());
            it = m_peers.erase(it);
        } else {
            ++it;
        }
    }
}

bool ServerNode::messageIsAuthoredBy(const QString &messageId, const QString &senderId) const
{
    return !messageId.isEmpty() && !senderId.isEmpty() &&
           m_messageSender.value(messageId) == senderId;
}

void ServerNode::updateStoredMessage(const QString &messageId, const QString &text,
                                     bool deleted)
{
    for (auto it = m_channelHistory.begin(); it != m_channelHistory.end(); ++it) {
        QList<QJsonObject> &messages = it.value();
        for (QJsonObject &message : messages) {
            if (message.value("id").toString() != messageId)
                continue;
            const qsizetype costBefore = ChatHistoryLimits::entryCost(message);
            // The rewritten body is authoritative, so the rich source the
            // original arrived with goes too: leaving it behind would make a
            // later replay of this entry render the text that was edited away.
            message.remove("richText");
            if (deleted) {
                message.insert("deleted", true);
                message.insert("edited", false);
                message.insert("text", QString());
                message.remove("fileName");
                message.remove("fileMime");
                message.remove("file");
            } else {
                message.insert("text", text.left(kMaxTextChars));
                message.insert("edited", true);
            }
            const auto total = m_channelHistoryChars.find(it.key());
            if (total != m_channelHistoryChars.end())
                *total += ChatHistoryLimits::entryCost(message) - costBefore;
            return;
        }
    }
}

void ServerNode::applyEdit(const QString &conversation, const QString &target,
                           const QString &senderId, const QString &text)
{
    const QString moderated = filteredChatText(text).left(kMaxTextChars);
    if (!messageIsAuthoredBy(target, senderId) || moderated.trimmed().isEmpty())
        return;
    const QString conv =
        conversation.isEmpty() ? m_messageConversation.value(target) : conversation;
    updateStoredMessage(target, moderated, false);
    emit messageEdited(conv, target, moderated);
}

void ServerNode::applyDelete(const QString &conversation, const QString &target,
                             const QString &senderId)
{
    if (!messageIsAuthoredBy(target, senderId))
        return;
    applyDeleteUnchecked(conversation, target);
}

void ServerNode::applyDeleteUnchecked(const QString &conversation,
                                      const QString &target)
{
    if (target.isEmpty())
        return;
    const QString conv =
        conversation.isEmpty() ? m_messageConversation.value(target) : conversation;
    updateStoredMessage(target, QString(), true);
    m_reactions.remove(target);
    emit messageDeleted(conv, target);
}

bool ServerNode::markSeen(const QString &messageId)
{
    if (messageId.isEmpty() || m_seenIds.contains(messageId))
        return false;
    m_seenIds.insert(messageId);
    m_seenOrder.enqueue(messageId);
    while (m_seenOrder.size() > kSeenCacheLimit)
        m_seenIds.remove(m_seenOrder.dequeue());
    return true;
}
