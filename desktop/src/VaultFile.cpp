#include "VaultFile.h"

#include "AppPaths.h"

#include <QByteArrayList>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include <openssl/pem.h>

#include <algorithm>
#include <memory>

namespace forkmesh::vaultfile {

namespace {

constexpr qint64 kMaxFileBytes = 32LL * 1024 * 1024;
constexpr int kKeyBytes = 32;
constexpr int kSaltBytes = 32;
constexpr int kNonceBytes = 12;
constexpr int kTagBytes = 16;
const char *const kKind = "forkmesh.vault";
const char *const kScheme = "ed25519-sig-hkdf-sha256-v1";
const QByteArray kLabelKEK("forkmesh-vault-kek-v1");
const QByteArray kLabelWrap("forkmesh-vault-wrap-v1");
const QByteArray kLabelEntryKey("forkmesh-vault-entry-key-v1");
const QByteArray kLabelEntry("forkmesh-vault-entry-v1");
const QByteArray kLabelMAC("forkmesh-vault-mac-v1");
const QByteArray kLabelKeyID("forkmesh-vault-key-id-v1");

void wipe(QByteArray *bytes)
{
    if (!bytes)
        return;
    bytes->fill('\0');
    bytes->clear();
}

void fail(QString *error, const QString &code)
{
    if (error)
        *error = code;
}

QByteArray fromBase64(const QJsonValue &value, int size)
{
    const auto decoded = QByteArray::fromBase64Encoding(
        value.toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded || (size > 0 && decoded.decoded.size() != size))
        return {};
    return decoded.decoded;
}

QByteArray hkdf(const QByteArray &secret, const QByteArray &salt, const QByteArray &info, int size)
{
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> ctx(
        EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr), EVP_PKEY_CTX_free);
    if (!ctx || EVP_PKEY_derive_init(ctx.get()) != 1 ||
        EVP_PKEY_CTX_set_hkdf_md(ctx.get(), EVP_sha256()) != 1 ||
        EVP_PKEY_CTX_set1_hkdf_key(ctx.get(), reinterpret_cast<const unsigned char *>(secret.constData()),
                                   int(secret.size())) != 1 ||
        EVP_PKEY_CTX_add1_hkdf_info(ctx.get(), reinterpret_cast<const unsigned char *>(info.constData()),
                                    int(info.size())) != 1)
        return {};
    if (!salt.isEmpty() &&
        EVP_PKEY_CTX_set1_hkdf_salt(ctx.get(), reinterpret_cast<const unsigned char *>(salt.constData()),
                                    int(salt.size())) != 1)
        return {};
    QByteArray out(size, '\0');
    size_t length = size_t(size);
    if (EVP_PKEY_derive(ctx.get(), reinterpret_cast<unsigned char *>(out.data()), &length) != 1 ||
        length != size_t(size)) {
        wipe(&out);
        return {};
    }
    return out;
}

QByteArray gcmOpen(const QByteArray &key, const QByteArray &nonce, const QByteArray &sealed,
                   const QByteArray &aad, bool *ok)
{
    *ok = false;
    if (key.size() != kKeyBytes || nonce.size() != kNonceBytes || sealed.size() < kTagBytes)
        return {};
    const int bodySize = int(sealed.size()) - kTagBytes;
    QByteArray tag = sealed.right(kTagBytes);
    QByteArray plain(bodySize, '\0');
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(),
                                                                        EVP_CIPHER_CTX_free);
    int length = 0;
    int total = 0;
    bool good = ctx &&
                EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
                EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, kNonceBytes, nullptr) == 1 &&
                EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr,
                                   reinterpret_cast<const unsigned char *>(key.constData()),
                                   reinterpret_cast<const unsigned char *>(nonce.constData())) == 1 &&
                EVP_DecryptUpdate(ctx.get(), nullptr, &length,
                                  reinterpret_cast<const unsigned char *>(aad.constData()),
                                  int(aad.size())) == 1;
    if (good && bodySize > 0) {
        good = EVP_DecryptUpdate(ctx.get(), reinterpret_cast<unsigned char *>(plain.data()), &length,
                                 reinterpret_cast<const unsigned char *>(sealed.constData()),
                                 bodySize) == 1;
        total = length;
    }
    good = good &&
           EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, kTagBytes, tag.data()) == 1 &&
           EVP_DecryptFinal_ex(ctx.get(), reinterpret_cast<unsigned char *>(plain.data()) + total,
                               &length) == 1;
    if (!good) {
        wipe(&plain);
        return {};
    }
    *ok = true;
    return plain;
}

struct Identity {
    EVP_PKEY *key = nullptr;
    QString publicKey;
    ~Identity()
    {
        if (key)
            EVP_PKEY_free(key);
    }
};

bool loadIdentity(const QString &path, Identity *identity)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    QByteArray pem = file.read(64 * 1024);
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new_mem_buf(pem.constData(), int(pem.size())),
                                                  BIO_free);
    identity->key = bio ? PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr) : nullptr;
    wipe(&pem);
    if (!identity->key || EVP_PKEY_id(identity->key) != EVP_PKEY_ED25519)
        return false;
    unsigned char raw[32];
    size_t length = sizeof(raw);
    if (EVP_PKEY_get_raw_public_key(identity->key, raw, &length) != 1 || length != sizeof(raw))
        return false;
    identity->publicKey = QString::fromLatin1(
        QByteArray(reinterpret_cast<const char *>(raw), int(length))
            .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    return true;
}

QByteArray sign(const Identity &identity, const QByteArray &payload)
{
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    size_t length = 64;
    QByteArray signature(64, '\0');
    if (!ctx || EVP_DigestSignInit(ctx.get(), nullptr, nullptr, nullptr, identity.key) != 1 ||
        EVP_DigestSign(ctx.get(), reinterpret_cast<unsigned char *>(signature.data()), &length,
                       reinterpret_cast<const unsigned char *>(payload.constData()),
                       size_t(payload.size())) != 1 ||
        length != 64) {
        wipe(&signature);
        return {};
    }
    return signature;
}

QByteArray canonical(const QJsonObject &doc, const QJsonObject &entries)
{
    QByteArrayList names;
    for (const QString &name : entries.keys())
        names << name.toUtf8();
    std::sort(names.begin(), names.end());
    QByteArray out = kLabelMAC + '\n';
    out += QByteArray::number(doc.value(QStringLiteral("v")).toInteger()) + '\n';
    out += doc.value(QStringLiteral("keyId")).toString().toUtf8() + '\n';
    out += QByteArray::number(doc.value(QStringLiteral("generation")).toInteger()) + '\n';
    for (const QByteArray &name : names) {
        const QJsonObject entry = entries.value(QString::fromUtf8(name)).toObject();
        out += QByteArray::number(name.size()) + ':' + name + '\n';
        out += entry.value(QStringLiteral("nonce")).toString().toUtf8() + '\n';
        out += entry.value(QStringLiteral("data")).toString().toUtf8() + '\n';
        out += QByteArray::number(entry.value(QStringLiteral("at")).toInteger()) + '\n';
    }
    return out;
}

bool macMatches(const QByteArray &dek, const QJsonObject &doc, const QJsonObject &entries)
{
    QByteArray key = hkdf(dek, {}, kLabelMAC, kKeyBytes);
    const QByteArray want = fromBase64(doc.value(QStringLiteral("mac")), 32);
    if (key.isEmpty() || want.isEmpty())
        return false;
    const QByteArray data = canonical(doc, entries);
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    const bool computed = HMAC(EVP_sha256(), key.constData(), int(key.size()),
                               reinterpret_cast<const unsigned char *>(data.constData()),
                               size_t(data.size()), out, &length) != nullptr;
    wipe(&key);
    return computed && length == 32 && CRYPTO_memcmp(out, want.constData(), 32) == 0;
}

}

QString defaultPath()
{
    return forkmesh::paths::dataRoot() + QStringLiteral("/vault/secrets.json");
}

QString identityPath()
{
    return forkmesh::paths::dataRoot() + QStringLiteral("/identity/ed25519.pem");
}

bool exists(const QString &path)
{
    const QFileInfo info(path);
    return info.exists() && info.isFile() && !info.isSymLink();
}

QString entryName(const QString &ns, const QString &name)
{
    return ns + QLatin1Char('/') +
           QString::fromLatin1(name.toUtf8().toPercentEncoding());
}

bool readNamespace(const QString &vaultPath, const QString &identityPemPath, const QString &ns,
                   QMap<QString, Entry> *out, QString *error)
{
    if (!out)
        return false;
    out->clear();
    const QFileInfo info(vaultPath);
    if (!info.exists()) {
        fail(error, QStringLiteral("vault_missing"));
        return false;
    }
    if (!info.isFile() || info.isSymLink() || info.size() > kMaxFileBytes) {
        fail(error, QStringLiteral("vault_invalid"));
        return false;
    }
    QFile file(vaultPath);
    if (!file.open(QIODevice::ReadOnly)) {
        fail(error, QStringLiteral("vault_io"));
        return false;
    }
    const QJsonObject doc = QJsonDocument::fromJson(file.readAll()).object();
    const QString keyId = doc.value(QStringLiteral("keyId")).toString();
    const QJsonArray wraps = doc.value(QStringLiteral("wraps")).toArray();
    const QJsonObject entries = doc.value(QStringLiteral("entries")).toObject();
    if (doc.value(QStringLiteral("kind")).toString() != QLatin1String(kKind) ||
        doc.value(QStringLiteral("v")).toInt() != 1 || keyId.size() != 16 || wraps.isEmpty()) {
        fail(error, QStringLiteral("vault_invalid"));
        return false;
    }
    Identity identity;
    if (!loadIdentity(identityPemPath, &identity)) {
        fail(error, QStringLiteral("vault_no_identity"));
        return false;
    }
    QByteArray dek;
    bool matched = false;
    for (const QJsonValue &value : wraps) {
        const QJsonObject wrap = value.toObject();
        if (wrap.value(QStringLiteral("scheme")).toString() != QLatin1String(kScheme) ||
            wrap.value(QStringLiteral("identity")).toString() != identity.publicKey)
            continue;
        matched = true;
        const QByteArray salt = fromBase64(wrap.value(QStringLiteral("salt")), kSaltBytes);
        const QByteArray nonce = fromBase64(wrap.value(QStringLiteral("nonce")), kNonceBytes);
        const QByteArray sealed = fromBase64(wrap.value(QStringLiteral("key")), kKeyBytes + kTagBytes);
        if (salt.isEmpty() || nonce.isEmpty() || sealed.isEmpty())
            continue;
        QByteArray signature = sign(identity, kLabelKEK + '\n' + identity.publicKey.toUtf8());
        QByteArray kek = hkdf(signature, salt, kLabelKEK, kKeyBytes);
        wipe(&signature);
        bool ok = false;
        QByteArray candidate = gcmOpen(kek, nonce, sealed,
                                       kLabelWrap + '\0' + identity.publicKey.toUtf8() + '\0' +
                                           keyId.toUtf8(),
                                       &ok);
        wipe(&kek);
        QByteArray check = ok ? hkdf(candidate, {}, kLabelKeyID, 8) : QByteArray();
        if (ok && QString::fromLatin1(check.toHex()) == keyId) {
            dek = candidate;
            wipe(&check);
            break;
        }
        wipe(&check);
        wipe(&candidate);
    }
    if (dek.isEmpty()) {
        fail(error, matched ? QStringLiteral("vault_tampered") : QStringLiteral("vault_locked"));
        return false;
    }
    if (!macMatches(dek, doc, entries)) {
        wipe(&dek);
        fail(error, QStringLiteral("vault_tampered"));
        return false;
    }
    QByteArray entryKey = hkdf(dek, {}, kLabelEntryKey, kKeyBytes);
    wipe(&dek);
    const QString prefix = ns + QLatin1Char('/');
    bool good = !entryKey.isEmpty();
    for (auto it = entries.constBegin(); good && it != entries.constEnd(); ++it) {
        if (!it.key().startsWith(prefix))
            continue;
        const QJsonObject entry = it.value().toObject();
        const QByteArray nonce = fromBase64(entry.value(QStringLiteral("nonce")), kNonceBytes);
        const QByteArray sealed = fromBase64(entry.value(QStringLiteral("data")), 0);
        bool ok = false;
        QByteArray plain = gcmOpen(entryKey, nonce, sealed,
                                   kLabelEntry + '\0' + keyId.toUtf8() + '\0' + it.key().toUtf8(), &ok);
        if (!ok) {
            good = false;
            break;
        }
        const QString name = QString::fromUtf8(QByteArray::fromPercentEncoding(it.key().mid(prefix.size()).toLatin1()));
        out->insert(name, Entry{QString::fromUtf8(plain), entry.value(QStringLiteral("at")).toInteger()});
        wipe(&plain);
    }
    wipe(&entryKey);
    if (!good) {
        out->clear();
        fail(error, QStringLiteral("vault_tampered"));
        return false;
    }
    return true;
}

}
