#include "RoomSeal.h"
#include "SettingsWrite.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QHash>
#include <QMap>
#include <QMessageAuthenticationCode>
#include <QRegularExpression>
#include <QMutex>
#include <QSettings>

#include "SettingsSecretBackend.h"
#include <QTimer>
#include <QUrl>

#include <openssl/evp.h>
#include <openssl/rand.h>

namespace {

constexpr int kSealVersion = 2;
constexpr int kKeyBytes = 32;
constexpr int kNonceBytes = 12;
constexpr int kTagBytes = 16;
constexpr int kKeyIdChars = 16;
constexpr int kMaxRecipients = 64;
constexpr qsizetype kMaxPlainBytes = 4ll * 1024 * 1024;
constexpr qint64 kDurableAnnouncementMs = 24ll * 60 * 60 * 1000;

const QString &sealedType()
{
    static const QString value = QStringLiteral("sealed");
    return value;
}

const QString &keyType()
{
    static const QString value = QStringLiteral("e2ee-key");
    return value;
}

const QRegularExpression &keyIdPattern()
{
    static const QRegularExpression value(QStringLiteral("^[A-Za-z0-9_-]{16}$"));
    return value;
}

const QRegularExpression &publicKeyPattern()
{
    static const QRegularExpression value(QStringLiteral("^[A-Za-z0-9_-]{43}$"));
    return value;
}

QByteArray sha256(const QByteArray &input)
{
    return QCryptographicHash::hash(input, QCryptographicHash::Sha256);
}

QString toBase64Url(const QByteArray &input)
{
    return QString::fromLatin1(input.toBase64(
        QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

QByteArray fromBase64Url(const QString &input)
{
    if (!publicKeyPattern().match(input).hasMatch())
        return {};
    const QByteArray decoded = QByteArray::fromBase64(
        input.toLatin1(), QByteArray::Base64UrlEncoding);
    return decoded.size() == kKeyBytes ? decoded : QByteArray{};
}

QString keyIdFor(const QByteArray &publicKey)
{
    return publicKey.size() == kKeyBytes
        ? toBase64Url(sha256(publicKey)).left(kKeyIdChars)
        : QString();
}

QByteArray randomBytes(int count)
{
    QByteArray output(count, Qt::Uninitialized);
    if (RAND_bytes(reinterpret_cast<unsigned char *>(output.data()), count) != 1)
        output.clear();
    return output;
}

QByteArray publicForPrivate(const QByteArray &privateKey)
{
    if (privateKey.size() != kKeyBytes)
        return {};
    EVP_PKEY *key = EVP_PKEY_new_raw_private_key(
        EVP_PKEY_X25519, nullptr,
        reinterpret_cast<const unsigned char *>(privateKey.constData()),
        size_t(privateKey.size()));
    if (!key)
        return {};
    QByteArray publicKey(kKeyBytes, Qt::Uninitialized);
    size_t length = size_t(publicKey.size());
    const bool ok = EVP_PKEY_get_raw_public_key(
                        key,
                        reinterpret_cast<unsigned char *>(publicKey.data()),
                        &length) == 1 &&
                    length == size_t(kKeyBytes);
    EVP_PKEY_free(key);
    if (!ok)
        return {};
    return publicKey;
}

QByteArray deriveShared(const QByteArray &privateKey,
                        const QByteArray &peerPublicKey)
{
    if (privateKey.size() != kKeyBytes || peerPublicKey.size() != kKeyBytes)
        return {};
    EVP_PKEY *mine = EVP_PKEY_new_raw_private_key(
        EVP_PKEY_X25519, nullptr,
        reinterpret_cast<const unsigned char *>(privateKey.constData()),
        size_t(privateKey.size()));
    EVP_PKEY *peer = EVP_PKEY_new_raw_public_key(
        EVP_PKEY_X25519, nullptr,
        reinterpret_cast<const unsigned char *>(peerPublicKey.constData()),
        size_t(peerPublicKey.size()));
    EVP_PKEY_CTX *context = mine ? EVP_PKEY_CTX_new(mine, nullptr) : nullptr;
    QByteArray shared(kKeyBytes, Qt::Uninitialized);
    size_t length = size_t(shared.size());
    const bool ok = mine && peer && context &&
                    EVP_PKEY_derive_init(context) == 1 &&
                    EVP_PKEY_derive_set_peer(context, peer) == 1 &&
                    EVP_PKEY_derive(context,
                                    reinterpret_cast<unsigned char *>(shared.data()),
                                    &length) == 1 &&
                    length == size_t(kKeyBytes);
    if (context)
        EVP_PKEY_CTX_free(context);
    if (peer)
        EVP_PKEY_free(peer);
    if (mine)
        EVP_PKEY_free(mine);
    if (!ok || shared == QByteArray(kKeyBytes, '\0'))
        return {};
    return shared;
}

QByteArray pairwiseKey(const QByteArray &privateKey,
                       const QByteArray &peerPublicKey,
                       const QString &roomName,
                       const QString &senderKeyId,
                       const QString &recipientKeyId)
{
    QByteArray shared = deriveShared(privateKey, peerPublicKey);
    if (shared.isEmpty())
        return {};
    const QByteArray salt = sha256("ForkMesh chat e2ee salt:" + roomName.toUtf8());
    const QByteArray extracted = QMessageAuthenticationCode::hash(
        shared, salt, QCryptographicHash::Sha256);
    shared.fill('\0');
    const QByteArray info =
        "forkmesh-chat-e2ee-kek-v2\n" + roomName.toUtf8() + "\n" +
        senderKeyId.toUtf8() + "\n" + recipientKeyId.toUtf8();
    // HKDF-SHA256 expand for one 32-byte block (RFC 5869, T(1)).
    return QMessageAuthenticationCode::hash(
        info + QByteArray(1, '\x01'), extracted, QCryptographicHash::Sha256);
}

QByteArray contentAad(const QString &roomName, const QString &senderKeyId)
{
    return "forkmesh-chat-e2ee-content-v2\n" + roomName.toUtf8() + "\n" +
           senderKeyId.toUtf8();
}

QByteArray wrapAad(const QString &roomName, const QString &senderKeyId,
                   const QString &recipientKeyId, const QByteArray &bodyNonce)
{
    return "forkmesh-chat-e2ee-wrap-v2\n" + roomName.toUtf8() + "\n" +
           senderKeyId.toUtf8() + "\n" + recipientKeyId.toUtf8() + "\n" +
           toBase64Url(bodyNonce).toUtf8();
}

bool aesEncrypt(const QByteArray &plain, const QByteArray &key,
                const QByteArray &aad, const QByteArray &nonce,
                QByteArray *cipher, QByteArray *tag)
{
    if (!cipher || !tag || key.size() != kKeyBytes ||
        nonce.size() != kNonceBytes)
        return false;
    cipher->resize(plain.size() + kTagBytes);
    tag->resize(kTagBytes);
    EVP_CIPHER_CTX *context = EVP_CIPHER_CTX_new();
    int length = 0;
    int outputLength = 0;
    bool ok = context &&
              EVP_EncryptInit_ex(context, EVP_aes_256_gcm(), nullptr,
                                 nullptr, nullptr) == 1 &&
              EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN,
                                  nonce.size(), nullptr) == 1 &&
              EVP_EncryptInit_ex(
                  context, nullptr, nullptr,
                  reinterpret_cast<const unsigned char *>(key.constData()),
                  reinterpret_cast<const unsigned char *>(nonce.constData())) == 1 &&
              EVP_EncryptUpdate(
                  context, nullptr, &length,
                  reinterpret_cast<const unsigned char *>(aad.constData()),
                  aad.size()) == 1 &&
              EVP_EncryptUpdate(
                  context,
                  reinterpret_cast<unsigned char *>(cipher->data()), &length,
                  reinterpret_cast<const unsigned char *>(plain.constData()),
                  plain.size()) == 1;
    if (ok) {
        outputLength = length;
        ok = EVP_EncryptFinal_ex(
                 context,
                 reinterpret_cast<unsigned char *>(cipher->data()) + outputLength,
                 &length) == 1 &&
             EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_GET_TAG, tag->size(),
                                 tag->data()) == 1;
        outputLength += length;
    }
    if (context)
        EVP_CIPHER_CTX_free(context);
    if (!ok) {
        cipher->clear();
        tag->clear();
        return false;
    }
    cipher->resize(outputLength);
    return true;
}

QByteArray aesDecrypt(const QByteArray &cipher, const QByteArray &tag,
                      const QByteArray &key, const QByteArray &aad,
                      const QByteArray &nonce)
{
    if (cipher.isEmpty() || tag.size() != kTagBytes ||
        key.size() != kKeyBytes || nonce.size() != kNonceBytes)
        return {};
    QByteArray plain(cipher.size(), Qt::Uninitialized);
    EVP_CIPHER_CTX *context = EVP_CIPHER_CTX_new();
    int length = 0;
    int outputLength = 0;
    bool ok = context &&
              EVP_DecryptInit_ex(context, EVP_aes_256_gcm(), nullptr,
                                 nullptr, nullptr) == 1 &&
              EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN,
                                  nonce.size(), nullptr) == 1 &&
              EVP_DecryptInit_ex(
                  context, nullptr, nullptr,
                  reinterpret_cast<const unsigned char *>(key.constData()),
                  reinterpret_cast<const unsigned char *>(nonce.constData())) == 1 &&
              EVP_DecryptUpdate(
                  context, nullptr, &length,
                  reinterpret_cast<const unsigned char *>(aad.constData()),
                  aad.size()) == 1 &&
              EVP_DecryptUpdate(
                  context,
                  reinterpret_cast<unsigned char *>(plain.data()), &length,
                  reinterpret_cast<const unsigned char *>(cipher.constData()),
                  cipher.size()) == 1;
    if (ok) {
        outputLength = length;
        ok = EVP_CIPHER_CTX_ctrl(
                 context, EVP_CTRL_GCM_SET_TAG, tag.size(),
                 const_cast<char *>(tag.constData())) == 1 &&
             EVP_DecryptFinal_ex(
                 context,
                 reinterpret_cast<unsigned char *>(plain.data()) + outputLength,
                 &length) == 1;
        outputLength += length;
    }
    if (context)
        EVP_CIPHER_CTX_free(context);
    if (!ok)
        return {};
    plain.resize(outputLength);
    return plain;
}

QString scopedSetting(const QString &prefix, const QString &storageScope)
{
    return prefix + QString::fromLatin1(
                        QUrl::toPercentEncoding(storageScope.trimmed(), "-._~"));
}

QString privateKeySetting()
{
    return QStringLiteral("chat/e2ee/x25519PrivateV2");
}

QString legacyPhraseSetting(const QString &roomName)
{
    return scopedSetting(QStringLiteral("chat/roomSeals/"), roomName);
}

QString peersSetting(const QString &storageScope)
{
    return scopedSetting(QStringLiteral("chat/e2ee/peersV2/"), storageScope);
}

constexpr int kPeerFlushDelayMs = 2000;

struct PendingPeerWrites {
    QMutex mutex;
    QHash<QString, QByteArray> values;
    bool scheduled = false;
    bool quitHooked = false;
};

PendingPeerWrites &pendingPeerWrites()
{
    static PendingPeerWrites pending;
    return pending;
}

void flushPendingPeerWrites()
{
    QHash<QString, QByteArray> values;
    {
        PendingPeerWrites &pending = pendingPeerWrites();
        const QMutexLocker lock(&pending.mutex);
        values.swap(pending.values);
        pending.scheduled = false;
    }
    if (values.isEmpty())
        return;
    QSettings settings;
    for (auto it = values.cbegin(); it != values.cend(); ++it)
        forkmesh::settings::put(settings, it.key(), it.value());
}

bool pendingPeerWrite(const QString &key, QByteArray *value)
{
    PendingPeerWrites &pending = pendingPeerWrites();
    const QMutexLocker lock(&pending.mutex);
    const auto it = pending.values.constFind(key);
    if (it == pending.values.cend())
        return false;
    *value = it.value();
    return true;
}

void queuePeerWrite(const QString &key, const QByteArray &value)
{
    QCoreApplication *app = QCoreApplication::instance();
    if (!app) {
        forkmesh::settings::put(QSettings(), key, value);
        return;
    }
    PendingPeerWrites &pending = pendingPeerWrites();
    bool schedule = false;
    {
        const QMutexLocker lock(&pending.mutex);
        pending.values.insert(key, value);
        schedule = !pending.scheduled;
        pending.scheduled = true;
    }
    if (!schedule)
        return;
    QMetaObject::invokeMethod(
        app,
        [app] {
            PendingPeerWrites &pending = pendingPeerWrites();
            bool hook = false;
            {
                const QMutexLocker lock(&pending.mutex);
                hook = !pending.quitHooked;
                pending.quitHooked = true;
            }
            if (hook)
                QObject::connect(app, &QCoreApplication::aboutToQuit, app,
                                 &flushPendingPeerWrites);
            QTimer::singleShot(kPeerFlushDelayMs, app, &flushPendingPeerWrites);
        },
        Qt::QueuedConnection);
}

QString announcedSetting(const QString &storageScope)
{
    return scopedSetting(QStringLiteral("chat/e2ee/announcedV2/"), storageScope);
}

QByteArray standardBase64(const QJsonObject &object, const char *name)
{
    return QByteArray::fromBase64(object.value(QLatin1String(name))
                                      .toString().toLatin1());
}

} // namespace

RoomSeal::RoomSeal(const QString &roomName)
    : RoomSeal(roomName, roomName)
{
}

RoomSeal::RoomSeal(const QString &roomName, const QString &storageScope)
    : m_roomName(roomName.trimmed()),
      m_storageScope(storageScope.trimmed())
{
    if (m_roomName.isEmpty()) {
        m_error = QStringLiteral("A chat room is required for end-to-end encryption.");
        return;
    }
    if (m_storageScope.isEmpty())
        m_storageScope = m_roomName;
    QSettings settings;
    // v1 asked the user for a shared phrase. It is obsolete in v2 and should
    // not remain in settings after this room upgrades.
    forkmesh::settings::drop(settings, legacyPhraseSetting(m_roomName));
    QByteArray privateKey = QByteArray::fromBase64(
        forkmesh::settingsecrets::value(settings, privateKeySetting()).toLatin1());
    if (privateKey.size() != kKeyBytes) {
        privateKey = randomBytes(kKeyBytes);
        forkmesh::settingsecrets::store(settings, privateKeySetting(),
                                        QString::fromLatin1(privateKey.toBase64()),
                                        privateKey.toBase64());
    }
    if (!initialize(privateKey, true)) {
        // A corrupt/unsupported stored key is replaced once. Failure after
        // that leaves the room closed rather than falling back to plaintext.
        privateKey = randomBytes(kKeyBytes);
        forkmesh::settingsecrets::store(settings, privateKeySetting(),
                                        QString::fromLatin1(privateKey.toBase64()),
                                        privateKey.toBase64());
        initialize(privateKey, true);
    }
}

RoomSeal::RoomSeal(const QString &roomName, const QByteArray &privateKey)
    : m_roomName(roomName.trimmed()),
      m_storageScope(m_roomName)
{
    initialize(privateKey, false);
}

bool RoomSeal::initialize(const QByteArray &privateKey, bool loadStoredPeers)
{
    m_privateKey.clear();
    m_publicKey.clear();
    m_keyId.clear();
    if (m_roomName.isEmpty() || privateKey.size() != kKeyBytes) {
        m_error = QStringLiteral("A room and 32-byte device key are required.");
        return false;
    }
    const QByteArray publicKey = publicForPrivate(privateKey);
    if (publicKey.size() != kKeyBytes) {
        m_error = QStringLiteral("Could not create the X25519 chat identity.");
        return false;
    }
    m_privateKey = privateKey;
    m_publicKey = publicKey;
    m_keyId = keyIdFor(publicKey);
    m_error.clear();
    if (loadStoredPeers)
        loadPeers();
    return true;
}

bool RoomSeal::isKeyFrame(const QJsonObject &frame)
{
    return frame.value(QStringLiteral("type")).toString() == keyType() &&
           frame.value(QStringLiteral("v")).toInt() == kSealVersion &&
           keyIdPattern().match(frame.value(QStringLiteral("kid")).toString())
               .hasMatch() &&
           publicKeyPattern().match(frame.value(QStringLiteral("pub")).toString())
               .hasMatch();
}

bool RoomSeal::isSealedFrame(const QJsonObject &frame)
{
    const QJsonValue keys = frame.value(QStringLiteral("keys"));
    return frame.value(QStringLiteral("type")).toString() == sealedType() &&
           frame.value(QStringLiteral("v")).toInt() == kSealVersion &&
           keyIdPattern().match(frame.value(QStringLiteral("kid")).toString())
               .hasMatch() &&
           publicKeyPattern().match(frame.value(QStringLiteral("pub")).toString())
               .hasMatch() &&
           keys.isArray() && !keys.toArray().isEmpty() &&
           keys.toArray().size() <= kMaxRecipients;
}

QString RoomSeal::frameKeyId(const QJsonObject &frame)
{
    return isSealedFrame(frame) ? frame.value(QStringLiteral("kid")).toString()
                                : QString();
}

QJsonObject RoomSeal::keyFrame() const
{
    if (!isValid())
        return {};
    return {{QStringLiteral("type"), keyType()},
            {QStringLiteral("v"), kSealVersion},
            {QStringLiteral("kid"), m_keyId},
            {QStringLiteral("pub"), toBase64Url(m_publicKey)}};
}

bool RoomSeal::rememberPeer(const QString &keyId, const QByteArray &publicKey)
{
    if (!isValid() || keyId == m_keyId || publicKey.size() != kKeyBytes ||
        keyIdFor(publicKey) != keyId)
        return false;
    const bool isNew = !m_peers.contains(keyId);
    m_peerOrder.removeAll(keyId);
    m_peerOrder.append(keyId);
    m_peers.insert(keyId, publicKey);
    while (m_peerOrder.size() >= kMaxRecipients) {
        const QString removed = m_peerOrder.takeFirst();
        m_peers.remove(removed);
    }
    return isNew;
}

bool RoomSeal::acceptKeyFrame(const QJsonObject &frame)
{
    if (!isKeyFrame(frame))
        return false;
    const QString keyId = frame.value(QStringLiteral("kid")).toString();
    const QByteArray publicKey =
        fromBase64Url(frame.value(QStringLiteral("pub")).toString());
    const bool isNew = rememberPeer(keyId, publicKey);
    if (isNew)
        persistPeers();
    return isNew;
}

void RoomSeal::loadPeers()
{
    QByteArray stored;
    if (!pendingPeerWrite(peersSetting(m_storageScope), &stored))
        stored = QSettings().value(peersSetting(m_storageScope)).toByteArray();
    const QJsonDocument document = QJsonDocument::fromJson(stored);
    if (!document.isArray())
        return;
    const QJsonArray entries = document.array();
    const int first = qMax(0, entries.size() - (kMaxRecipients - 1));
    for (int index = first; index < entries.size(); ++index) {
        const QJsonObject entry = entries.at(index).toObject();
        const QString keyId = entry.value(QStringLiteral("kid")).toString();
        const QByteArray publicKey =
            fromBase64Url(entry.value(QStringLiteral("pub")).toString());
        rememberPeer(keyId, publicKey);
    }
}

void RoomSeal::persistPeers() const
{
    QJsonArray entries;
    for (const QString &keyId : m_peerOrder) {
        const QByteArray publicKey = m_peers.value(keyId);
        if (publicKey.size() == kKeyBytes) {
            entries.append(QJsonObject{{QStringLiteral("kid"), keyId},
                                       {QStringLiteral("pub"),
                                        toBase64Url(publicKey)}});
        }
    }
    queuePeerWrite(peersSetting(m_storageScope),
                   QJsonDocument(entries).toJson(QJsonDocument::Compact));
}

bool RoomSeal::shouldPersistAdvertisement() const
{
    if (!isValid())
        return false;
    QSettings settings;
    const QString key = announcedSetting(m_storageScope);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 previous = settings.value(key).toLongLong();
    if (now - previous < kDurableAnnouncementMs)
        return false;
    settings.setValue(key, now);
    return true;
}

QJsonObject RoomSeal::sealObject(const QJsonObject &plain) const
{
    return sealObjectFor(plain, m_peerOrder);
}

QJsonObject RoomSeal::sealObjectFor(const QJsonObject &plain,
                                    const QStringList &keyIds) const
{
    if (!isValid())
        return {};
    const QByteArray input = QJsonDocument(plain).toJson(QJsonDocument::Compact);
    if (input.isEmpty() || input.size() > kMaxPlainBytes)
        return {};

    QByteArray contentKey = randomBytes(kKeyBytes);
    const QByteArray bodyNonce = randomBytes(kNonceBytes);
    QByteArray body;
    QByteArray bodyTag;
    if (!aesEncrypt(input, contentKey, contentAad(m_roomName, m_keyId),
                    bodyNonce, &body, &bodyTag)) {
        contentKey.fill('\0');
        return {};
    }

    QMap<QString, QByteArray> recipients;
    for (const QString &keyId : keyIds)
        if (m_peers.contains(keyId))
            recipients.insert(keyId, m_peers.value(keyId));
    recipients.insert(m_keyId, m_publicKey);

    QJsonArray wrappedKeys;
    for (auto iterator = recipients.cbegin(); iterator != recipients.cend();
         ++iterator) {
        const QByteArray kek = pairwiseKey(
            m_privateKey, iterator.value(), m_roomName, m_keyId, iterator.key());
        if (kek.size() != kKeyBytes)
            continue;
        const QByteArray nonce = randomBytes(kNonceBytes);
        QByteArray wrapped;
        QByteArray tag;
        if (!aesEncrypt(contentKey, kek,
                        wrapAad(m_roomName, m_keyId, iterator.key(), bodyNonce),
                        nonce, &wrapped, &tag))
            continue;
        wrappedKeys.append(
            QJsonObject{{QStringLiteral("kid"), iterator.key()},
                        {QStringLiteral("nonce"),
                         QString::fromLatin1(nonce.toBase64())},
                        {QStringLiteral("tag"),
                         QString::fromLatin1(tag.toBase64())},
                        {QStringLiteral("key"),
                         QString::fromLatin1(wrapped.toBase64())}});
    }
    contentKey.fill('\0');
    if (wrappedKeys.isEmpty())
        return {};
    return {{QStringLiteral("type"), sealedType()},
            {QStringLiteral("v"), kSealVersion},
            {QStringLiteral("kid"), m_keyId},
            {QStringLiteral("pub"), toBase64Url(m_publicKey)},
            {QStringLiteral("nonce"),
             QString::fromLatin1(bodyNonce.toBase64())},
            {QStringLiteral("tag"), QString::fromLatin1(bodyTag.toBase64())},
            {QStringLiteral("body"), QString::fromLatin1(body.toBase64())},
            {QStringLiteral("keys"), wrappedKeys}};
}

QJsonObject RoomSeal::unsealObject(const QJsonObject &frame)
{
    if (!isValid() || !isSealedFrame(frame))
        return {};
    const QString senderKeyId = frame.value(QStringLiteral("kid")).toString();
    const QByteArray senderPublic =
        fromBase64Url(frame.value(QStringLiteral("pub")).toString());
    if (senderPublic.size() != kKeyBytes ||
        keyIdFor(senderPublic) != senderKeyId)
        return {};

    QJsonObject wrapped;
    const QJsonArray keys = frame.value(QStringLiteral("keys")).toArray();
    for (const QJsonValue &value : keys) {
        const QJsonObject candidate = value.toObject();
        if (candidate.value(QStringLiteral("kid")).toString() == m_keyId) {
            wrapped = candidate;
            break;
        }
    }
    if (wrapped.isEmpty())
        return {};

    const QByteArray bodyNonce = standardBase64(frame, "nonce");
    const QByteArray bodyTag = standardBase64(frame, "tag");
    const QByteArray body = standardBase64(frame, "body");
    const QByteArray keyNonce = standardBase64(wrapped, "nonce");
    const QByteArray keyTag = standardBase64(wrapped, "tag");
    const QByteArray wrappedKey = standardBase64(wrapped, "key");
    if (bodyNonce.size() != kNonceBytes || bodyTag.size() != kTagBytes ||
        body.isEmpty() || body.size() > kMaxPlainBytes ||
        keyNonce.size() != kNonceBytes || keyTag.size() != kTagBytes ||
        wrappedKey.size() != kKeyBytes)
        return {};

    const QByteArray kek = pairwiseKey(
        m_privateKey, senderPublic, m_roomName, senderKeyId, m_keyId);
    QByteArray contentKey = aesDecrypt(
        wrappedKey, keyTag, kek,
        wrapAad(m_roomName, senderKeyId, m_keyId, bodyNonce), keyNonce);
    if (contentKey.size() != kKeyBytes)
        return {};
    const QByteArray clear = aesDecrypt(
        body, bodyTag, contentKey, contentAad(m_roomName, senderKeyId),
        bodyNonce);
    contentKey.fill('\0');
    if (clear.isEmpty())
        return {};
    const QJsonDocument document = QJsonDocument::fromJson(clear);
    if (!document.isObject())
        return {};
    if (senderKeyId != m_keyId && rememberPeer(senderKeyId, senderPublic))
        persistPeers();
    return document.object();
}
