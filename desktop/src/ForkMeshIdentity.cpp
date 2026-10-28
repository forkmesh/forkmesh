#include "ForkMeshIdentity.h"
#include "AppPaths.h"

#include "CoveCrypto.h"

#include <QByteArray>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QStandardPaths>

#include <openssl/bio.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>

namespace {

constexpr int kPortableSaltBytes = 16;
constexpr int kPortableNonceBytes = 12;
constexpr int kPortableTagBytes = 16;
constexpr int kPortableKeyBytes = 32;
constexpr int kPortablePbkdf2Iterations = 600000;
constexpr qsizetype kPortableMaxPlaintextBytes = 128 * 1024;
const QByteArray kPortableAad("forkmesh.portable-identity/v1");

QString base64Url(const QByteArray &data)
{
    return QString::fromLatin1(
        data.toBase64(QByteArray::Base64UrlEncoding |
                      QByteArray::OmitTrailingEquals));
}

QByteArray base64UrlExact(const QString &value, int expectedSize = -1)
{
    const QByteArray encoded = value.toLatin1();
    if (encoded.isEmpty() || encoded.contains('='))
        return {};
    for (const char ch : encoded) {
        const bool allowed = (ch >= 'A' && ch <= 'Z') ||
                             (ch >= 'a' && ch <= 'z') ||
                             (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
        if (!allowed)
            return {};
    }
    const QByteArray decoded = QByteArray::fromBase64(
        encoded, QByteArray::Base64UrlEncoding |
                     QByteArray::AbortOnBase64DecodingErrors);
    if ((expectedSize >= 0 && decoded.size() != expectedSize) ||
        base64Url(decoded) != value)
        return {};
    return decoded;
}

QByteArray portableRandomBytes(int count)
{
    QByteArray result(count, Qt::Uninitialized);
    if (count <= 0 || RAND_bytes(
            reinterpret_cast<unsigned char *>(result.data()), count) != 1)
        return {};
    return result;
}

QByteArray portableKey(const QString &passphrase, const QByteArray &salt)
{
    if (passphrase.size() < 12 || salt.size() != kPortableSaltBytes)
        return {};
    const QByteArray pass = passphrase.toUtf8();
    QByteArray key(kPortableKeyBytes, Qt::Uninitialized);
    if (PKCS5_PBKDF2_HMAC(
            pass.constData(), pass.size(),
            reinterpret_cast<const unsigned char *>(salt.constData()), salt.size(),
            kPortablePbkdf2Iterations, EVP_sha256(), key.size(),
            reinterpret_cast<unsigned char *>(key.data())) != 1)
        return {};
    return key;
}

bool portableSeal(const QByteArray &key, const QByteArray &plaintext,
                  QByteArray *iv, QByteArray *ciphertext)
{
    if (!iv || !ciphertext || key.size() != kPortableKeyBytes ||
        plaintext.isEmpty() || plaintext.size() > kPortableMaxPlaintextBytes)
        return false;
    *iv = portableRandomBytes(kPortableNonceBytes);
    if (iv->size() != kPortableNonceBytes)
        return false;
    QByteArray body(plaintext.size() + kPortableTagBytes, Qt::Uninitialized);
    QByteArray tag(kPortableTagBytes, Qt::Uninitialized);
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int length = 0;
    int outputLength = 0;
    bool ok = ctx &&
        EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, iv->size(), nullptr) == 1 &&
        EVP_EncryptInit_ex(ctx, nullptr, nullptr,
                           reinterpret_cast<const unsigned char *>(key.constData()),
                           reinterpret_cast<const unsigned char *>(iv->constData())) == 1 &&
        EVP_EncryptUpdate(ctx, nullptr, &length,
                          reinterpret_cast<const unsigned char *>(kPortableAad.constData()),
                          kPortableAad.size()) == 1 &&
        EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char *>(body.data()), &length,
                          reinterpret_cast<const unsigned char *>(plaintext.constData()),
                          plaintext.size()) == 1;
    if (ok) {
        outputLength = length;
        ok = EVP_EncryptFinal_ex(ctx,
                                 reinterpret_cast<unsigned char *>(body.data()) + outputLength,
                                 &length) == 1 &&
             EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, tag.size(), tag.data()) == 1;
        outputLength += length;
    }
    if (ctx)
        EVP_CIPHER_CTX_free(ctx);
    if (!ok)
        return false;
    body.resize(outputLength);
    *ciphertext = body + tag; // WebCrypto's AES-GCM wire representation.
    return true;
}

QByteArray portableOpen(const QByteArray &key, const QByteArray &iv,
                        const QByteArray &ciphertext)
{
    if (key.size() != kPortableKeyBytes || iv.size() != kPortableNonceBytes ||
        ciphertext.size() < kPortableTagBytes + 1 ||
        ciphertext.size() > kPortableMaxPlaintextBytes + kPortableTagBytes)
        return {};
    const QByteArray body = ciphertext.left(ciphertext.size() - kPortableTagBytes);
    const QByteArray tag = ciphertext.right(kPortableTagBytes);
    QByteArray plaintext(body.size(), Qt::Uninitialized);
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int length = 0;
    int outputLength = 0;
    bool ok = ctx &&
        EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, iv.size(), nullptr) == 1 &&
        EVP_DecryptInit_ex(ctx, nullptr, nullptr,
                           reinterpret_cast<const unsigned char *>(key.constData()),
                           reinterpret_cast<const unsigned char *>(iv.constData())) == 1 &&
        EVP_DecryptUpdate(ctx, nullptr, &length,
                          reinterpret_cast<const unsigned char *>(kPortableAad.constData()),
                          kPortableAad.size()) == 1 &&
        EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char *>(plaintext.data()), &length,
                          reinterpret_cast<const unsigned char *>(body.constData()),
                          body.size()) == 1;
    if (ok) {
        outputLength = length;
        ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, tag.size(),
                                 const_cast<char *>(tag.constData())) == 1 &&
             EVP_DecryptFinal_ex(ctx,
                                 reinterpret_cast<unsigned char *>(plaintext.data()) + outputLength,
                                 &length) == 1;
        outputLength += length;
    }
    if (ctx)
        EVP_CIPHER_CTX_free(ctx);
    if (!ok)
        return {};
    plaintext.resize(outputLength);
    return plaintext;
}

} // namespace

ForkMeshIdentity::~ForkMeshIdentity()
{
    if (m_key)
        EVP_PKEY_free(m_key);
}

QString ForkMeshIdentity::defaultKeyDir()
{
    return forkmesh::paths::dataRoot() +
           "/identity";
}

bool ForkMeshIdentity::load()
{
    if (isValid())
        return true;

    const QString dir = defaultKeyDir();
    if (!QDir().mkpath(dir)) {
        m_error = "Could not create identity key directory: " + dir;
        return false;
    }

    const QString keyPath = dir + "/ed25519.pem";
    if (!QFile::exists(keyPath) && !generate(keyPath))
        return false;
    m_keyDir = dir;
    return readKey(keyPath) && refreshPublicKey();
}

bool ForkMeshIdentity::generate(const QString &keyPath)
{
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
    if (!ctx) {
        m_error = "OpenSSL does not support Ed25519 identity keys.";
        return false;
    }

    EVP_PKEY *key = nullptr;
    const bool ok = EVP_PKEY_keygen_init(ctx) == 1 &&
                    EVP_PKEY_keygen(ctx, &key) == 1;
    EVP_PKEY_CTX_free(ctx);
    if (!ok || !key) {
        if (key)
            EVP_PKEY_free(key);
        m_error = "Could not generate the Ed25519 identity key.";
        return false;
    }

    const QByteArray pathBytes = QFile::encodeName(keyPath);
    BIO *bio = BIO_new_file(pathBytes.constData(), "w");
    if (!bio) {
        EVP_PKEY_free(key);
        m_error = "Could not open the identity key for writing: " + keyPath;
        return false;
    }

    const int written =
        PEM_write_bio_PrivateKey(bio, key, nullptr, nullptr, 0, nullptr, nullptr);
    BIO_free(bio);
    EVP_PKEY_free(key);
    if (written != 1) {
        QFile::remove(keyPath);
        m_error = "Could not write the Ed25519 identity key.";
        return false;
    }
    if (!QFile::setPermissions(keyPath, QFile::ReadOwner | QFile::WriteOwner)) {
        // Best-effort hardening: on a filesystem without POSIX permissions this
        // can fail, which would leave the private identity key group/world
        // readable. Warn loudly rather than fail identity creation (which some
        // network-mounted home directories would otherwise trip on).
        qWarning("ForkMesh: could not restrict permissions on identity key %s; "
                 "it may be readable by other users on this machine.",
                 qUtf8Printable(keyPath));
    }
    return true;
}

bool ForkMeshIdentity::readKey(const QString &keyPath)
{
    if (m_key) {
        EVP_PKEY_free(m_key);
        m_key = nullptr;
    }

    const QByteArray pathBytes = QFile::encodeName(keyPath);
    BIO *bio = BIO_new_file(pathBytes.constData(), "r");
    if (!bio) {
        m_error = "Could not read the identity key from " + keyPath;
        return false;
    }

    m_key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!m_key) {
        m_error = "Stored identity key is invalid. Delete " + keyPath +
                  " and restart to regenerate it.";
        return false;
    }
    if (EVP_PKEY_base_id(m_key) != EVP_PKEY_ED25519) {
        m_error = "Stored identity key is not an Ed25519 key.";
        return false;
    }
    return true;
}

bool ForkMeshIdentity::refreshPublicKey()
{
    size_t len = 0;
    if (EVP_PKEY_get_raw_public_key(m_key, nullptr, &len) != 1 || len == 0) {
        m_error = "Could not read the Ed25519 public key.";
        return false;
    }

    QByteArray raw(int(len), Qt::Uninitialized);
    if (EVP_PKEY_get_raw_public_key(
            m_key, reinterpret_cast<unsigned char *>(raw.data()), &len) != 1) {
        m_error = "Could not export the Ed25519 public key.";
        return false;
    }
    raw.resize(int(len));
    m_publicKey = base64Url(raw);
    return true;
}

bool ForkMeshIdentity::hasAnnouncedWelcome() const
{
    return !m_keyDir.isEmpty() &&
           QFile::exists(m_keyDir + "/welcome_announced");
}

void ForkMeshIdentity::markWelcomeAnnounced() const
{
    if (m_keyDir.isEmpty())
        return;
    QFile f(m_keyDir + "/welcome_announced");
    if (!f.open(QIODevice::WriteOnly)) {
        qWarning().noquote()
            << "Could not persist the welcome marker:"
            << f.errorString().simplified().left(240);
        return;
    }
    f.close();
}

QString ForkMeshIdentity::shortPublicKey() const
{
    if (m_publicKey.size() <= 16)
        return m_publicKey;
    return m_publicKey.left(8) + "..." + m_publicKey.right(8);
}

QJsonObject ForkMeshIdentity::profileObject(const QString &name,
                                            const QString &handle,
                                            const QString &solanaAddress,
                                            const QString &bio,
                                            const QString &website) const
{
    return {{"name", name.trimmed()},
            {"handle", handle.trimmed()},
            {"bio", bio.trimmed()},
            {"website", website.trimmed()},
            {"solana", solanaAddress.trimmed()},
            {"pubkey", m_publicKey},
            {"updatedAt", QString::number(QDateTime::currentMSecsSinceEpoch())}};
}

QJsonObject ForkMeshIdentity::signedProfile(const QString &name,
                                            const QString &handle,
                                            const QString &solanaAddress,
                                            const QString &bio,
                                            const QString &website) const
{
    const QJsonObject profile =
        profileObject(name, handle, solanaAddress, bio, website);
    return {{"kind", "forkmesh.identity"},
            {"profile", profile},
            {"signature", signJson(profile)}};
}

QString ForkMeshIdentity::signJson(const QJsonObject &object) const
{
    return signData(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

QString ForkMeshIdentity::signData(const QByteArray &payload) const
{
    if (!m_key)
        return {};

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx)
        return {};

    size_t sigLen = 0;
    bool ok = EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, m_key) == 1 &&
              EVP_DigestSign(ctx, nullptr, &sigLen,
                             reinterpret_cast<const unsigned char *>(payload.constData()),
                             payload.size()) == 1 &&
              sigLen > 0;
    QByteArray signature(int(sigLen), Qt::Uninitialized);
    if (ok) {
        ok = EVP_DigestSign(
                 ctx, reinterpret_cast<unsigned char *>(signature.data()), &sigLen,
                 reinterpret_cast<const unsigned char *>(payload.constData()),
                 payload.size()) == 1;
    }
    EVP_MD_CTX_free(ctx);
    if (!ok)
        return {};

    signature.resize(int(sigLen));
    return base64Url(signature);
}

bool ForkMeshIdentity::verifySignature(const QString &publicKeyB64url,
                                       const QString &signatureB64url,
                                       const QByteArray &payload)
{
    const QByteArray rawKey = QByteArray::fromBase64(
        publicKeyB64url.toLatin1(), QByteArray::Base64UrlEncoding);
    const QByteArray sig = QByteArray::fromBase64(
        signatureB64url.toLatin1(), QByteArray::Base64UrlEncoding);
    // Ed25519 keys are 32 bytes, signatures 64 bytes.
    if (rawKey.size() != 32 || sig.size() != 64)
        return false;

    EVP_PKEY *key = EVP_PKEY_new_raw_public_key(
        EVP_PKEY_ED25519, nullptr,
        reinterpret_cast<const unsigned char *>(rawKey.constData()),
        size_t(rawKey.size()));
    if (!key)
        return false;

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    bool ok = false;
    if (ctx &&
        EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, key) == 1) {
        ok = EVP_DigestVerify(
                 ctx, reinterpret_cast<const unsigned char *>(sig.constData()),
                 size_t(sig.size()),
                 reinterpret_cast<const unsigned char *>(payload.constData()),
                 size_t(payload.size())) == 1;
    }
    if (ctx)
        EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    return ok;
}

QByteArray ForkMeshIdentity::deviceBindCanonical(
    const QString &accountName, const QString &publicKeyB64url,
    const QString &timestamp)
{
    static const QRegularExpression accountPattern(
        QStringLiteral("^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$"));
    static const QRegularExpression publicKeyPattern(
        QStringLiteral("^[A-Za-z0-9_-]{43}$"));
    static const QRegularExpression timestampPattern(
        QStringLiteral("^(?:0|[1-9][0-9]{0,15})$"));
    const QString normalizedAccount = accountName.trimmed().toLower();
    if (!accountPattern.match(normalizedAccount).hasMatch() ||
        !publicKeyPattern.match(publicKeyB64url).hasMatch() ||
        !timestampPattern.match(timestamp).hasMatch()) {
        return {};
    }
    return QByteArrayLiteral("forkmesh-device-bind-v1\n") +
           normalizedAccount.toUtf8() + '\n' + publicKeyB64url.toUtf8() +
           '\n' + timestamp.toUtf8();
}


QByteArray ForkMeshIdentity::rawPrivateSeed() const
{
    if (!m_key)
        return {};
    size_t len = 0;
    if (EVP_PKEY_get_raw_private_key(m_key, nullptr, &len) != 1 || len == 0)
        return {};
    QByteArray raw(int(len), Qt::Uninitialized);
    if (EVP_PKEY_get_raw_private_key(
            m_key, reinterpret_cast<unsigned char *>(raw.data()), &len) != 1)
        return {};
    raw.resize(int(len));
    return raw;
}

QString ForkMeshIdentity::exportEncryptedKeyfile(const QString &passphrase) const
{
    if (!m_key || passphrase.isEmpty())
        return {};
    const QByteArray seed = rawPrivateSeed();
    if (seed.size() != 32) // Ed25519 raw private seed
        return {};

    const QByteArray salt = CoveCrypto::randomSalt();
    const int rounds = CoveCrypto::defaultRounds();
    CoveCrypto crypto(passphrase, salt, rounds);
    if (!crypto.isValid())
        return {};
    const QJsonObject cipher = crypto.encrypt(seed);
    if (cipher.isEmpty())
        return {};

    const QJsonObject envelope{
        {"kind", "forkmesh.identity.keyfile"},
        {"v", 1},
        {"kdf", "pbkdf2-hmac-sha256"},
        {"salt", QString::fromLatin1(salt.toBase64())},
        {"rounds", rounds},
        {"pubkey", m_publicKey}, // clear-text: identifies, never unlocks
        {"createdAt", QString::number(QDateTime::currentMSecsSinceEpoch())},
        {"cipher", cipher}};
    return QString::fromUtf8(
        QJsonDocument(envelope).toJson(QJsonDocument::Indented));
}

QString ForkMeshIdentity::exportPortableIdentity(const QString &passphrase) const
{
    if (!m_key || passphrase.size() < 12)
        return {};
    const QByteArray seed = rawPrivateSeed();
    if (seed.size() != 32)
        return {};
    const QJsonObject payload{
        {"type", "forkmesh.ed25519-seed"},
        {"schemaVersion", 1},
        {"publicKey", m_publicKey},
        {"seed", base64Url(seed)}};
    const QByteArray plaintext = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    const QByteArray salt = portableRandomBytes(kPortableSaltBytes);
    const QByteArray key = portableKey(passphrase, salt);
    QByteArray iv;
    QByteArray ciphertext;
    if (key.isEmpty() || !portableSeal(key, plaintext, &iv, &ciphertext))
        return {};
    return QString::fromUtf8(QJsonDocument(QJsonObject{
        {"type", "forkmesh.portable-identity"},
        {"schemaVersion", 1},
        {"createdAt", QString::number(QDateTime::currentMSecsSinceEpoch())},
        {"kdf", QJsonObject{{"name", "PBKDF2-HMAC-SHA-256"},
                             {"iterations", kPortablePbkdf2Iterations},
                             {"salt", base64Url(salt)}}},
        {"encryption", QJsonObject{{"name", "AES-256-GCM"},
                                    {"iv", base64Url(iv)},
                                    {"ciphertext", base64Url(ciphertext)}}}})
        .toJson(QJsonDocument::Indented));
}

QString ForkMeshIdentity::keyfilePublicKey(const QString &keyfileText)
{
    const QJsonObject env =
        QJsonDocument::fromJson(keyfileText.toUtf8()).object();
    if (env.value("kind").toString() != "forkmesh.identity.keyfile")
        return {};
    return env.value("pubkey").toString();
}

bool ForkMeshIdentity::installRawSeed(const QByteArray &seed)
{
    EVP_PKEY *key = EVP_PKEY_new_raw_private_key(
        EVP_PKEY_ED25519, nullptr,
        reinterpret_cast<const unsigned char *>(seed.constData()),
        size_t(seed.size()));
    if (!key) {
        m_error = "Could not construct an Ed25519 key from the keyfile.";
        return false;
    }

    const QString dir = defaultKeyDir();
    if (!QDir().mkpath(dir)) {
        EVP_PKEY_free(key);
        m_error = "Could not create identity key directory: " + dir;
        return false;
    }
    const QString keyPath = dir + "/ed25519.pem";
    const QByteArray pathBytes = QFile::encodeName(keyPath);
    BIO *bio = BIO_new_file(pathBytes.constData(), "w");
    if (!bio) {
        EVP_PKEY_free(key);
        m_error = "Could not open the identity key for writing: " + keyPath;
        return false;
    }
    const int written =
        PEM_write_bio_PrivateKey(bio, key, nullptr, nullptr, 0, nullptr, nullptr);
    BIO_free(bio);
    EVP_PKEY_free(key);
    if (written != 1) {
        m_error = "Could not write the imported identity key.";
        return false;
    }
    if (!QFile::setPermissions(keyPath, QFile::ReadOwner | QFile::WriteOwner)) {
        // Best-effort hardening: on a filesystem without POSIX permissions this
        // can fail, which would leave the private identity key group/world
        // readable. Warn loudly rather than fail identity creation (which some
        // network-mounted home directories would otherwise trip on).
        qWarning("ForkMesh: could not restrict permissions on identity key %s; "
                 "it may be readable by other users on this machine.",
                 qUtf8Printable(keyPath));
    }
    m_keyDir = dir;
    markBackedUp();
    return readKey(keyPath) && refreshPublicKey();
}

QString ForkMeshIdentity::publicKeyForSeed(const QByteArray &seed)
{
    if (seed.size() != 32)
        return {};
    EVP_PKEY *key = EVP_PKEY_new_raw_private_key(
        EVP_PKEY_ED25519, nullptr,
        reinterpret_cast<const unsigned char *>(seed.constData()), size_t(seed.size()));
    if (!key)
        return {};
    size_t length = 0;
    const bool sizeOk = EVP_PKEY_get_raw_public_key(key, nullptr, &length) == 1 &&
                        length == 32;
    QByteArray raw(sizeOk ? int(length) : 0, Qt::Uninitialized);
    const bool keyOk = sizeOk && EVP_PKEY_get_raw_public_key(
        key, reinterpret_cast<unsigned char *>(raw.data()), &length) == 1;
    EVP_PKEY_free(key);
    if (!keyOk)
        return {};
    raw.resize(int(length));
    return base64Url(raw);
}

bool ForkMeshIdentity::importEncryptedKeyfile(const QString &keyfileText,
                                              const QString &passphrase)
{
    const QJsonObject env =
        QJsonDocument::fromJson(keyfileText.toUtf8()).object();
    if (env.value("kind").toString() != "forkmesh.identity.keyfile") {
        m_error = "This file is not a ForkMesh identity keyfile.";
        return false;
    }
    const QByteArray salt =
        QByteArray::fromBase64(env.value("salt").toString().toLatin1());
    const int rounds = env.value("rounds").toInt();
    if (salt.isEmpty() || rounds <= 0) {
        m_error = "The keyfile is missing its key-derivation parameters.";
        return false;
    }
    CoveCrypto crypto(passphrase, salt, rounds);
    if (!crypto.isValid()) {
        m_error = crypto.errorString();
        return false;
    }
    const QByteArray seed = crypto.decrypt(env.value("cipher").toObject());
    if (seed.size() != 32) {
        m_error = "Wrong passphrase, or the keyfile is corrupt.";
        return false;
    }
    return installRawSeed(seed);
}

bool ForkMeshIdentity::importPortableIdentity(const QString &portableText,
                                              const QString &passphrase)
{
    const QJsonObject env = QJsonDocument::fromJson(portableText.toUtf8()).object();
    const QJsonObject kdf = env.value("kdf").toObject();
    const QJsonObject encryption = env.value("encryption").toObject();
    if (env.value("type").toString() != "forkmesh.portable-identity" ||
        env.value("schemaVersion").toInt() != 1 ||
        kdf.value("name").toString() != "PBKDF2-HMAC-SHA-256" ||
        kdf.value("iterations").toInt() != kPortablePbkdf2Iterations ||
        encryption.value("name").toString() != "AES-256-GCM") {
        m_error = "This file is not a supported portable ForkMesh identity.";
        return false;
    }
    const QByteArray salt = base64UrlExact(kdf.value("salt").toString(),
                                           kPortableSaltBytes);
    const QByteArray iv = base64UrlExact(encryption.value("iv").toString(),
                                         kPortableNonceBytes);
    const QByteArray ciphertext = base64UrlExact(
        encryption.value("ciphertext").toString());
    if (salt.isEmpty() || iv.isEmpty() || ciphertext.size() < kPortableTagBytes + 1 ||
        ciphertext.size() > kPortableMaxPlaintextBytes + kPortableTagBytes) {
        m_error = "The portable identity has invalid encryption parameters.";
        return false;
    }
    const QByteArray key = portableKey(passphrase, salt);
    const QByteArray plaintext = portableOpen(key, iv, ciphertext);
    const QJsonObject payload = QJsonDocument::fromJson(plaintext).object();
    const QByteArray seed = base64UrlExact(payload.value("seed").toString(), 32);
    if (plaintext.isEmpty() || payload.value("type").toString() != "forkmesh.ed25519-seed" ||
        payload.value("schemaVersion").toInt() != 1 || seed.isEmpty()) {
        m_error = "Recovery passphrase is incorrect or the file was changed.";
        return false;
    }
    const QString expectedPublicKey = payload.value("publicKey").toString();
    if (expectedPublicKey.isEmpty() || publicKeyForSeed(seed) != expectedPublicKey) {
        m_error = "Portable identity key material does not match its public key.";
        return false;
    }
    return installRawSeed(seed);
}

bool ForkMeshIdentity::hasBackedUp() const
{
    return !m_keyDir.isEmpty() && QFile::exists(m_keyDir + "/backed_up");
}

void ForkMeshIdentity::markBackedUp() const
{
    if (m_keyDir.isEmpty())
        return;
    QFile f(m_keyDir + "/backed_up");
    if (!f.open(QIODevice::WriteOnly)) {
        qWarning().noquote()
            << "Could not persist the identity backup marker:"
            << f.errorString().simplified().left(240);
        return;
    }
    f.close();
}

QJsonObject ForkMeshIdentity::signRotation(const QString &newPublicKeyB64url) const
{
    if (!m_key || m_publicKey.isEmpty() || newPublicKeyB64url.isEmpty())
        return {};
    const QString ts = QString::number(QDateTime::currentMSecsSinceEpoch());
    const QByteArray canonical = ("forkmesh-rotate-v1\n" + m_publicKey + "\n" +
                                  newPublicKeyB64url + "\n" + ts)
                                     .toUtf8();
    const QString sig = signData(canonical);
    if (sig.isEmpty())
        return {};
    return {{"kind", "forkmesh.rotate"},
            {"oldPubkey", m_publicKey},
            {"newPubkey", newPublicKeyB64url},
            {"ts", ts},
            {"signature", sig}};
}

bool ForkMeshIdentity::verifyRotation(const QJsonObject &record)
{
    if (record.value("kind").toString() != "forkmesh.rotate")
        return false;
    const QString oldPub = record.value("oldPubkey").toString();
    const QString newPub = record.value("newPubkey").toString();
    const QString ts = record.value("ts").toString();
    const QString sig = record.value("signature").toString();
    if (oldPub.isEmpty() || newPub.isEmpty() || ts.isEmpty() || sig.isEmpty())
        return false;
    const QByteArray canonical =
        ("forkmesh-rotate-v1\n" + oldPub + "\n" + newPub + "\n" + ts).toUtf8();
    return verifySignature(oldPub, sig, canonical);
}

static QByteArray rotationAcceptanceCanonical(const QJsonObject &record)
{
    return ("forkmesh-rotate-accept-v1\n" + record.value("nodeName").toString() +
            "\n" + record.value("oldPubkey").toString() + "\n" +
            record.value("newPubkey").toString() + "\n" +
            record.value("ts").toString())
        .toUtf8();
}

QJsonObject ForkMeshIdentity::acceptRotation(const QJsonObject &record,
                                             const QString &nodeName) const
{
    const QString name = nodeName.trimmed().toLower();
    if (!m_key || m_publicKey.isEmpty() || name.isEmpty() ||
        record.value("newPubkey").toString() != m_publicKey ||
        !verifyRotation(record))
        return {};
    QJsonObject accepted = record;
    accepted.insert("nodeName", name);
    const QString sig = signData(rotationAcceptanceCanonical(accepted));
    if (sig.isEmpty())
        return {};
    accepted.insert("newSig", sig);
    return accepted;
}

bool ForkMeshIdentity::verifyRotationAcceptance(const QJsonObject &record)
{
    const QString newPub = record.value("newPubkey").toString();
    const QString sig = record.value("newSig").toString();
    if (!verifyRotation(record) || newPub.isEmpty() || sig.isEmpty() ||
        record.value("nodeName").toString().isEmpty())
        return false;
    return verifySignature(newPub, sig, rotationAcceptanceCanonical(record));
}
