#include "RoomCrypto.h"

#include <QCryptographicHash>
#include <QHash>
#include <QJsonDocument>
#include <QMutex>
#include <QMutexLocker>
#include <QRandomGenerator>

#include <openssl/evp.h>
#include <openssl/rand.h>

#ifdef FORKMESH_SERVER_NODE_TESTS
#include <atomic>
#endif

namespace {

constexpr int kKeyBytes = 32;
constexpr int kNonceBytes = 12;
constexpr int kTagBytes = 16;
constexpr int kPbkdfRounds = 210000;
constexpr int kKeyCacheLimit = 256; // rooms per process; far above real use
// Room frame cap — MUST match the worker's MAX_TEXT_BYTES (4 MB) so both ends
// agree on what they'll relay; we reject oversize frames before encrypting.
constexpr qsizetype kMaxPlainBytes = 4ll * 1024 * 1024;
// Legacy fallback for construction before the authenticated relay-derived
// passphrase arrives. Every build can derive it, as can the relay, so it offers
// no confidentiality from either. Normal startup re-keys through
// ServerNode::setRoomPassphrase once the authenticated fetch completes.
const char kAppRoomKey[] = "forkmesh-shared-room-key-v1";

#ifdef FORKMESH_SERVER_NODE_TESTS
std::atomic<int> g_derivationCount{0};
#endif

QByteArray saltForRoom(const QString &roomName)
{
    return QCryptographicHash::hash("ForkMesh room:" + roomName.toUtf8(),
                                    QCryptographicHash::Sha256)
        .left(16);
}

QByteArray randomBytes(int count)
{
    QByteArray out(count, Qt::Uninitialized);
    if (RAND_bytes(reinterpret_cast<unsigned char *>(out.data()), count) != 1) {
        for (int i = 0; i < count; ++i)
            out[i] = char(QRandomGenerator::global()->bounded(256));
    }
    return out;
}

// 210k PBKDF2 rounds is a fifth of a second of pure CPU, and RoomCrypto is
// constructed per room and re-constructed whenever a room re-keys — Office
// channel history landed several of those on the GUI thread in recorded stall
// stacks. The same (password, room) pair always yields the same key, so keep
// derived keys for the process lifetime. They are no more exposed here than in
// the RoomCrypto objects that already hold them.
QMutex &keyCacheMutex()
{
    static QMutex mutex;
    return mutex;
}

QHash<QByteArray, QByteArray> &keyCache()
{
    static QHash<QByteArray, QByteArray> cache;
    return cache;
}

QByteArray keyCacheId(const QByteArray &pass, const QString &roomName)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData("ForkMesh room key cache v1\n");
    hash.addData(QByteArray::number(pass.size()) + '\n');
    hash.addData(pass);
    hash.addData("\n");
    hash.addData(roomName.trimmed().toUtf8()); // as saltForRoom() sees it
    return hash.result();
}

// Derive the 32-byte AES key from a password and the room-scoped salt.
bool deriveRoomKey(const QByteArray &pass, const QString &roomName, QByteArray &out)
{
#ifdef FORKMESH_SERVER_NODE_TESTS
    // Counts key requests, cached or not, so callers are still held to
    // asking for exactly the keys they need.
    g_derivationCount.fetch_add(1, std::memory_order_relaxed);
#endif
    const QByteArray cacheId = keyCacheId(pass, roomName);
    {
        QMutexLocker locker(&keyCacheMutex());
        const auto cached = keyCache().constFind(cacheId);
        if (cached != keyCache().cend()) {
            out = *cached;
            return !out.isEmpty();
        }
    }
    out.resize(kKeyBytes);
    const QByteArray salt = saltForRoom(roomName.trimmed());
    if (PKCS5_PBKDF2_HMAC(pass.constData(), pass.size(),
                          reinterpret_cast<const unsigned char *>(salt.constData()),
                          salt.size(), kPbkdfRounds, EVP_sha256(), kKeyBytes,
                          reinterpret_cast<unsigned char *>(out.data())) == 1) {
        QMutexLocker locker(&keyCacheMutex());
        if (keyCache().size() >= kKeyCacheLimit)
            keyCache().clear();
        keyCache().insert(cacheId, out);
        return true;
    }
    out.clear();
    return false;
}

} // namespace

#ifdef FORKMESH_SERVER_NODE_TESTS
void RoomCrypto::resetDerivationCountForTests()
{
    g_derivationCount.store(0, std::memory_order_relaxed);
}

int RoomCrypto::derivationCountForTests()
{
    return g_derivationCount.load(std::memory_order_relaxed);
}
#endif

void RoomCrypto::warmKey(const QString &roomName, const QString &passphrase)
{
    if (roomName.trimmed().isEmpty() || passphrase.isEmpty())
        return;
    QByteArray key;
    deriveRoomKey(passphrase.toUtf8(), roomName, key);
}

void RoomCrypto::warmDefaultKey(const QString &roomName)
{
    if (roomName.trimmed().isEmpty())
        return;
    QByteArray key;
    deriveRoomKey(QByteArray(kAppRoomKey), roomName, key);
}

RoomCrypto::RoomCrypto(const QString &roomName)
{
    if (roomName.trimmed().isEmpty()) {
        m_error = "Room name is required.";
        return;
    }
    if (!deriveRoomKey(QByteArray(kAppRoomKey), roomName, m_key))
        m_error = "Could not derive the room encryption key.";
}

RoomCrypto::RoomCrypto(const QString &roomName, const QString &passphrase)
{
    if (roomName.trimmed().isEmpty() || passphrase.isEmpty()) {
        m_error = "Room name and passphrase are required.";
        return;
    }
    if (!deriveRoomKey(passphrase.toUtf8(), roomName, m_key))
        m_error = "Could not derive the room encryption key.";
}

QJsonObject RoomCrypto::encryptObject(const QJsonObject &plain) const
{
    if (m_key.isEmpty())
        return {};

    const QByteArray nonce = randomBytes(kNonceBytes);
    const QByteArray input = QJsonDocument(plain).toJson(QJsonDocument::Compact);
    if (input.size() > kMaxPlainBytes)
        return {};
    QByteArray cipher(input.size() + kTagBytes, Qt::Uninitialized);
    QByteArray tag(kTagBytes, Qt::Uninitialized);

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int len = 0;
    int outLen = 0;
    if (!ctx ||
        EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, nonce.size(), nullptr) != 1 ||
        EVP_EncryptInit_ex(ctx, nullptr, nullptr,
                           reinterpret_cast<const unsigned char *>(m_key.constData()),
                           reinterpret_cast<const unsigned char *>(nonce.constData())) != 1 ||
        EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char *>(cipher.data()), &len,
                          reinterpret_cast<const unsigned char *>(input.constData()),
                          input.size()) != 1) {
        if (ctx)
            EVP_CIPHER_CTX_free(ctx);
        return {};
    }
    outLen = len;
    if (EVP_EncryptFinal_ex(ctx,
                            reinterpret_cast<unsigned char *>(cipher.data()) + outLen,
                            &len) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, tag.size(), tag.data()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return {};
    }
    outLen += len;
    EVP_CIPHER_CTX_free(ctx);
    cipher.resize(outLen);

    return {{"kind", "cipher"},
            {"v", 1},
            {"nonce", QString::fromLatin1(nonce.toBase64())},
            {"tag", QString::fromLatin1(tag.toBase64())},
            {"body", QString::fromLatin1(cipher.toBase64())}};
}

QJsonObject RoomCrypto::decryptObject(const QJsonObject &envelope) const
{
    if (m_key.isEmpty() || envelope.value("kind").toString() != "cipher")
        return {};

    const QByteArray nonce = QByteArray::fromBase64(envelope.value("nonce").toString().toLatin1());
    const QByteArray tag = QByteArray::fromBase64(envelope.value("tag").toString().toLatin1());
    const QByteArray cipher = QByteArray::fromBase64(envelope.value("body").toString().toLatin1());
    if (nonce.size() != kNonceBytes || tag.size() != kTagBytes || cipher.isEmpty() ||
        cipher.size() > kMaxPlainBytes)
        return {};

    QByteArray plain(cipher.size(), Qt::Uninitialized);
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int len = 0;
    int outLen = 0;
    if (!ctx ||
        EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, nonce.size(), nullptr) != 1 ||
        EVP_DecryptInit_ex(ctx, nullptr, nullptr,
                           reinterpret_cast<const unsigned char *>(m_key.constData()),
                           reinterpret_cast<const unsigned char *>(nonce.constData())) != 1 ||
        EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char *>(plain.data()), &len,
                          reinterpret_cast<const unsigned char *>(cipher.constData()),
                          cipher.size()) != 1) {
        if (ctx)
            EVP_CIPHER_CTX_free(ctx);
        return {};
    }
    outLen = len;
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, tag.size(),
                        const_cast<char *>(tag.constData()));
    const int ok = EVP_DecryptFinal_ex(ctx,
                                       reinterpret_cast<unsigned char *>(plain.data()) + outLen,
                                       &len);
    EVP_CIPHER_CTX_free(ctx);
    if (ok != 1)
        return {};
    outLen += len;
    plain.resize(outLen);
    const QJsonDocument doc = QJsonDocument::fromJson(plain);
    return doc.isObject() ? doc.object() : QJsonObject{};
}
