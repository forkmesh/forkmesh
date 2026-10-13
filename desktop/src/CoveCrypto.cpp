#include "CoveCrypto.h"

#include <QRandomGenerator>

#include <openssl/evp.h>
#include <openssl/rand.h>

namespace {

constexpr int kKeyBytes = 32;
constexpr int kNonceBytes = 12;
constexpr int kTagBytes = 16;
constexpr int kSaltBytes = 16;
// Hardened key-stretch for a human-chosen password. Higher than RoomCrypto's
// 210k because a cove password guards a long-lived vault, not an ephemeral room.
constexpr int kDefaultRounds = 600000;
constexpr qsizetype kMaxPlainBytes = 4ll * 1024 * 1024;

} // namespace

int CoveCrypto::defaultRounds()
{
    return kDefaultRounds;
}

QByteArray CoveCrypto::randomBytes(int count)
{
    QByteArray out(count, Qt::Uninitialized);
    if (RAND_bytes(reinterpret_cast<unsigned char *>(out.data()), count) != 1) {
        for (int i = 0; i < count; ++i)
            out[i] = char(QRandomGenerator::global()->bounded(256));
    }
    return out;
}

QByteArray CoveCrypto::randomSalt()
{
    return randomBytes(kSaltBytes);
}

QByteArray CoveCrypto::randomKey()
{
    return randomBytes(kKeyBytes);
}

CoveCrypto CoveCrypto::withKey(const QByteArray &key)
{
    CoveCrypto crypto;
    if (key.size() == kKeyBytes)
        crypto.m_key = key;
    else
        crypto.m_error = "A cove content key must be exactly 32 bytes.";
    return crypto;
}

CoveCrypto::CoveCrypto(const QString &password, const QByteArray &salt, int rounds)
{
    if (password.isEmpty()) {
        m_error = "A password is required.";
        return;
    }
    if (salt.isEmpty() || rounds <= 0) {
        m_error = "The cove is missing its key-derivation parameters.";
        return;
    }
    const QByteArray pass = password.toUtf8();
    QByteArray key(kKeyBytes, Qt::Uninitialized);
    if (PKCS5_PBKDF2_HMAC(pass.constData(), pass.size(),
                          reinterpret_cast<const unsigned char *>(salt.constData()),
                          salt.size(), rounds, EVP_sha256(), kKeyBytes,
                          reinterpret_cast<unsigned char *>(key.data())) != 1) {
        m_error = "Could not derive the cove encryption key.";
        return;
    }
    m_key = key;
}

QJsonObject CoveCrypto::encrypt(const QByteArray &plaintext) const
{
    if (m_key.isEmpty() || plaintext.size() > kMaxPlainBytes)
        return {};

    const QByteArray nonce = randomBytes(kNonceBytes);
    QByteArray cipher(plaintext.size() + kTagBytes, Qt::Uninitialized);
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
                          reinterpret_cast<const unsigned char *>(plaintext.constData()),
                          plaintext.size()) != 1) {
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

    return {{"nonce", QString::fromLatin1(nonce.toBase64())},
            {"tag", QString::fromLatin1(tag.toBase64())},
            {"body", QString::fromLatin1(cipher.toBase64())}};
}

QByteArray CoveCrypto::decrypt(const QJsonObject &cipherObj) const
{
    if (m_key.isEmpty())
        return {};

    const QByteArray nonce = QByteArray::fromBase64(cipherObj.value("nonce").toString().toLatin1());
    const QByteArray tag = QByteArray::fromBase64(cipherObj.value("tag").toString().toLatin1());
    const QByteArray cipher = QByteArray::fromBase64(cipherObj.value("body").toString().toLatin1());
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
        return {}; // wrong password (GCM tag mismatch) or corrupt ciphertext
    outLen += len;
    plain.resize(outLen);
    return plain;
}
