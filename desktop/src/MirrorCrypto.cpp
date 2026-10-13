#include "MirrorCrypto.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QSet>

#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <memory>

namespace {

constexpr int kKeyBytes = 32;   // AES-256 content key / KEK
constexpr int kNonceBytes = 12;
constexpr int kTagBytes = 16;
constexpr int kX25519Bytes = 32;
constexpr int kMlkemPubBytes = 1184;
constexpr int kMlkemPrivBytes = 2400;
constexpr int kMlkemCtBytes = 1088;

const char kHkdfSalt[] = "forkmesh-mirror-kem-v1";

using PkeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using PkeyCtxPtr = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;

class ByteArrayCleanser
{
public:
    explicit ByteArrayCleanser(QByteArray *value) : m_value(value) {}
    ~ByteArrayCleanser()
    {
        if (m_value && !m_value->isEmpty())
            OPENSSL_cleanse(m_value->data(), size_t(m_value->size()));
    }

private:
    QByteArray *m_value;
};

QByteArray randomBytes(int count)
{
    QByteArray out(count, Qt::Uninitialized);
    // Mirror keys and nonces must never fall back to a non-cryptographic PRNG.
    // A system RNG failure is a hard encryption failure.
    if (RAND_bytes(reinterpret_cast<unsigned char *>(out.data()), count) != 1)
        return {};
    return out;
}

QString toB64(const QByteArray &raw)
{
    return QString::fromLatin1(raw.toBase64());
}

QString toB64Url(const QByteArray &raw)
{
    return QString::fromLatin1(
        raw.toBase64(QByteArray::Base64UrlEncoding |
                     QByteArray::OmitTrailingEquals));
}

QByteArray fromB64(const QJsonValue &v)
{
    return QByteArray::fromBase64(v.toString().toLatin1());
}

QByteArray fromPublicB64(const QJsonValue &v)
{
    return QByteArray::fromBase64(
        v.toString().toLatin1(), QByteArray::Base64UrlEncoding);
}

QByteArray fromB64UrlExact(const QJsonValue &value, int exactSize)
{
    const QByteArray encoded = value.toString().toLatin1();
    if (encoded.isEmpty() || encoded.contains('='))
        return {};
    for (const char ch : encoded) {
        const bool allowed =
            (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
        if (!allowed)
            return {};
    }
    const QByteArray decoded = QByteArray::fromBase64(
        encoded, QByteArray::Base64UrlEncoding |
                     QByteArray::AbortOnBase64DecodingErrors);
    return decoded.size() == exactSize ? decoded : QByteArray{};
}

QByteArray fromB64UrlBounded(const QJsonValue &value, int maximumSize)
{
    const QByteArray encoded = value.toString().toLatin1();
    if (encoded.isEmpty() || encoded.contains('='))
        return {};
    for (const char ch : encoded) {
        const bool allowed =
            (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
        if (!allowed)
            return {};
    }
    const QByteArray decoded = QByteArray::fromBase64(
        encoded, QByteArray::Base64UrlEncoding |
                     QByteArray::AbortOnBase64DecodingErrors);
    return decoded.size() <= maximumSize ? decoded : QByteArray{};
}

// AES-256-GCM seal: writes nonce/tag/body. Returns false on any OpenSSL error.
bool gcmSeal(const QByteArray &key, const QByteArray &plaintext,
             QByteArray &nonce, QByteArray &tag, QByteArray &body)
{
    if (key.size() != kKeyBytes)
        return false;
    nonce = randomBytes(kNonceBytes);
    if (nonce.size() != kNonceBytes)
        return false;
    body.resize(plaintext.size());
    tag.resize(kTagBytes);

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return false;
    int len = 0;
    int outLen = 0;
    bool ok =
        EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, nonce.size(), nullptr) == 1 &&
        EVP_EncryptInit_ex(ctx, nullptr, nullptr,
                           reinterpret_cast<const unsigned char *>(key.constData()),
                           reinterpret_cast<const unsigned char *>(nonce.constData())) == 1 &&
        EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char *>(body.data()), &len,
                          reinterpret_cast<const unsigned char *>(plaintext.constData()),
                          plaintext.size()) == 1;
    if (ok) {
        outLen = len;
        ok = EVP_EncryptFinal_ex(
                 ctx, reinterpret_cast<unsigned char *>(body.data()) + outLen, &len) == 1 &&
             EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, tag.size(), tag.data()) == 1;
        outLen += len;
    }
    EVP_CIPHER_CTX_free(ctx);
    if (!ok)
        return false;
    body.resize(outLen);
    return true;
}

// AES-256-GCM open. Returns the plaintext, or {} on tag mismatch / bad input.
QByteArray gcmOpen(const QByteArray &key, const QByteArray &nonce,
                   const QByteArray &tag, const QByteArray &body)
{
    if (key.size() != kKeyBytes || nonce.size() != kNonceBytes ||
        tag.size() != kTagBytes)
        return {};

    QByteArray plain(body.size(), Qt::Uninitialized);
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return {};
    int len = 0;
    int outLen = 0;
    bool ok =
        EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, nonce.size(), nullptr) == 1 &&
        EVP_DecryptInit_ex(ctx, nullptr, nullptr,
                           reinterpret_cast<const unsigned char *>(key.constData()),
                           reinterpret_cast<const unsigned char *>(nonce.constData())) == 1 &&
        EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char *>(plain.data()), &len,
                          reinterpret_cast<const unsigned char *>(body.constData()),
                          body.size()) == 1;
    if (ok) {
        outLen = len;
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, tag.size(),
                            const_cast<char *>(tag.constData()));
        ok = EVP_DecryptFinal_ex(
                 ctx, reinterpret_cast<unsigned char *>(plain.data()) + outLen, &len) == 1;
        outLen += len;
    }
    EVP_CIPHER_CTX_free(ctx);
    if (!ok)
        return {}; // wrong KEK (GCM tag mismatch) or corrupt ciphertext
    plain.resize(outLen);
    return plain;
}

// X25519 ECDH: derive the shared secret from a raw private and a raw peer public.
QByteArray x25519Derive(const QByteArray &rawPriv, const QByteArray &rawPeerPub)
{
    if (rawPriv.size() != kX25519Bytes || rawPeerPub.size() != kX25519Bytes)
        return {};
    PkeyPtr me(EVP_PKEY_new_raw_private_key_ex(
                   nullptr, "X25519", nullptr,
                   reinterpret_cast<const unsigned char *>(rawPriv.constData()),
                   rawPriv.size()),
               &EVP_PKEY_free);
    PkeyPtr peer(EVP_PKEY_new_raw_public_key_ex(
                     nullptr, "X25519", nullptr,
                     reinterpret_cast<const unsigned char *>(rawPeerPub.constData()),
                     rawPeerPub.size()),
                 &EVP_PKEY_free);
    if (!me || !peer)
        return {};
    PkeyCtxPtr ctx(EVP_PKEY_CTX_new(me.get(), nullptr), &EVP_PKEY_CTX_free);
    size_t sl = 0;
    if (!ctx || EVP_PKEY_derive_init(ctx.get()) != 1 ||
        EVP_PKEY_derive_set_peer(ctx.get(), peer.get()) != 1 ||
        EVP_PKEY_derive(ctx.get(), nullptr, &sl) != 1)
        return {};
    QByteArray ss(sl, Qt::Uninitialized);
    if (EVP_PKEY_derive(ctx.get(), reinterpret_cast<unsigned char *>(ss.data()), &sl) != 1)
        return {};
    ss.resize(sl);
    return ss;
}

// ML-KEM-768 encapsulate to a raw public key: fills ct and shared secret.
bool mlkemEncapsulate(const QByteArray &rawPub, QByteArray &ct, QByteArray &ss)
{
    if (rawPub.size() != kMlkemPubBytes)
        return false;
    PkeyPtr peer(EVP_PKEY_new_raw_public_key_ex(
                     nullptr, "ML-KEM-768", nullptr,
                     reinterpret_cast<const unsigned char *>(rawPub.constData()),
                     rawPub.size()),
                 &EVP_PKEY_free);
    if (!peer)
        return false;
    PkeyCtxPtr ctx(EVP_PKEY_CTX_new(peer.get(), nullptr), &EVP_PKEY_CTX_free);
    size_t ctl = 0, ssl = 0;
    if (!ctx || EVP_PKEY_encapsulate_init(ctx.get(), nullptr) != 1 ||
        EVP_PKEY_encapsulate(ctx.get(), nullptr, &ctl, nullptr, &ssl) != 1)
        return false;
    ct.resize(ctl);
    ss.resize(ssl);
    if (EVP_PKEY_encapsulate(ctx.get(), reinterpret_cast<unsigned char *>(ct.data()), &ctl,
                             reinterpret_cast<unsigned char *>(ss.data()), &ssl) != 1)
        return false;
    ct.resize(ctl);
    ss.resize(ssl);
    return true;
}

// ML-KEM-768 decapsulate with a raw private key: recovers the shared secret.
QByteArray mlkemDecapsulate(const QByteArray &rawPriv, const QByteArray &ct)
{
    if (rawPriv.size() != kMlkemPrivBytes || ct.size() != kMlkemCtBytes)
        return {};
    PkeyPtr me(EVP_PKEY_new_raw_private_key_ex(
                   nullptr, "ML-KEM-768", nullptr,
                   reinterpret_cast<const unsigned char *>(rawPriv.constData()),
                   rawPriv.size()),
               &EVP_PKEY_free);
    if (!me)
        return {};
    PkeyCtxPtr ctx(EVP_PKEY_CTX_new(me.get(), nullptr), &EVP_PKEY_CTX_free);
    size_t ssl = 0;
    if (!ctx || EVP_PKEY_decapsulate_init(ctx.get(), nullptr) != 1 ||
        EVP_PKEY_decapsulate(ctx.get(), nullptr, &ssl,
                             reinterpret_cast<const unsigned char *>(ct.constData()),
                             ct.size()) != 1)
        return {};
    QByteArray ss(ssl, Qt::Uninitialized);
    if (EVP_PKEY_decapsulate(ctx.get(), reinterpret_cast<unsigned char *>(ss.data()), &ssl,
                             reinterpret_cast<const unsigned char *>(ct.constData()),
                             ct.size()) != 1)
        return {};
    ss.resize(ssl);
    return ss;
}

// HKDF-SHA256 combine the two KEM shared secrets into the 32-byte AES KEK. The
// info string binds the KEK to the exact ephemeral X25519 pub + ML-KEM ct that
// produced it, so a mix-and-match of wrap fields cannot yield a valid key.
QByteArray deriveKek(const QByteArray &ssX, const QByteArray &ssM,
                     const QByteArray &info)
{
    const QByteArray ikm = ssX + ssM;
    if (ssX.isEmpty() || ssM.isEmpty())
        return {};
    PkeyCtxPtr ctx(EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr), &EVP_PKEY_CTX_free);
    if (!ctx || EVP_PKEY_derive_init(ctx.get()) != 1 ||
        EVP_PKEY_CTX_set_hkdf_md(ctx.get(), EVP_sha256()) != 1 ||
        EVP_PKEY_CTX_set1_hkdf_salt(
            ctx.get(), reinterpret_cast<const unsigned char *>(kHkdfSalt),
            int(sizeof(kHkdfSalt) - 1)) != 1 ||
        EVP_PKEY_CTX_set1_hkdf_key(
            ctx.get(), reinterpret_cast<const unsigned char *>(ikm.constData()),
            ikm.size()) != 1 ||
        EVP_PKEY_CTX_add1_hkdf_info(
            ctx.get(), reinterpret_cast<const unsigned char *>(info.constData()),
            info.size()) != 1)
        return {};
    QByteArray kek(kKeyBytes, Qt::Uninitialized);
    size_t kl = kKeyBytes;
    if (EVP_PKEY_derive(ctx.get(), reinterpret_cast<unsigned char *>(kek.data()), &kl) != 1 ||
        kl != kKeyBytes)
        return {};
    return kek;
}

QString keyIdFor(const QByteArray &x25519Pub, const QByteArray &mlkemPub)
{
    const QByteArray digest = QCryptographicHash::hash(
        x25519Pub + mlkemPub, QCryptographicHash::Sha256);
    return QString::fromLatin1(digest.toBase64(QByteArray::Base64UrlEncoding |
                                               QByteArray::OmitTrailingEquals));
}

} // namespace

bool MirrorCrypto::Identity::isValid() const
{
    return x25519Pub.size() == kX25519Bytes && x25519Priv.size() == kX25519Bytes &&
           mlkemPub.size() == kMlkemPubBytes && mlkemPriv.size() == kMlkemPrivBytes;
}

QJsonObject MirrorCrypto::Identity::publicBundle() const
{
    if (x25519Pub.size() != kX25519Bytes || mlkemPub.size() != kMlkemPubBytes)
        return {};
    return {{"v", 1},
            {"x25519", toB64Url(x25519Pub)},
            {"mlkem768", toB64Url(mlkemPub)}};
}

QString MirrorCrypto::Identity::keyId() const
{
    return keyIdFor(x25519Pub, mlkemPub);
}

bool MirrorCrypto::postQuantumAvailable(QString *reason)
{
    EVP_KEM *kem = EVP_KEM_fetch(nullptr, "ML-KEM-768", nullptr);
    EVP_KEYMGMT *keys = EVP_KEYMGMT_fetch(nullptr, "ML-KEM-768", nullptr);
    const bool available = kem && keys;
    EVP_KEM_free(kem);
    EVP_KEYMGMT_free(keys);
    if (reason) {
        if (available)
            reason->clear();
        else
            *reason = QStringLiteral(
                          "Private mirrors need ML-KEM-768, which OpenSSL 3.5 or newer "
                          "provides; this process loaded %1.")
                          .arg(QString::fromLatin1(OpenSSL_version(OPENSSL_VERSION)));
    }
    return available;
}

MirrorCrypto::Identity MirrorCrypto::generateIdentity()
{
    Identity id;

    PkeyPtr xk(EVP_PKEY_Q_keygen(nullptr, nullptr, "X25519"), &EVP_PKEY_free);
    PkeyPtr mk(EVP_PKEY_Q_keygen(nullptr, nullptr, "ML-KEM-768"), &EVP_PKEY_free);
    if (!xk || !mk)
        return {};

    auto rawOf = [](EVP_PKEY *k, bool pub, int expect) -> QByteArray {
        size_t len = 0;
        int rc = pub ? EVP_PKEY_get_raw_public_key(k, nullptr, &len)
                     : EVP_PKEY_get_raw_private_key(k, nullptr, &len);
        if (rc != 1)
            return {};
        QByteArray out(len, Qt::Uninitialized);
        rc = pub ? EVP_PKEY_get_raw_public_key(
                       k, reinterpret_cast<unsigned char *>(out.data()), &len)
                 : EVP_PKEY_get_raw_private_key(
                       k, reinterpret_cast<unsigned char *>(out.data()), &len);
        if (rc != 1)
            return {};
        out.resize(len);
        return out.size() == expect ? out : QByteArray{};
    };

    id.x25519Pub = rawOf(xk.get(), true, kX25519Bytes);
    id.x25519Priv = rawOf(xk.get(), false, kX25519Bytes);
    id.mlkemPub = rawOf(mk.get(), true, kMlkemPubBytes);
    id.mlkemPriv = rawOf(mk.get(), false, kMlkemPrivBytes);
    return id.isValid() ? id : Identity{};
}

QString MirrorCrypto::publicKeyId(const QJsonObject &publicBundle)
{
    const QByteArray x25519 =
        fromPublicB64(publicBundle.value("x25519"));
    const QByteArray mlkem =
        fromPublicB64(publicBundle.value("mlkem768"));
    if (publicBundle.value("v").toInt() != 1 ||
        x25519.size() != kX25519Bytes || mlkem.size() != kMlkemPubBytes)
        return {};
    return keyIdFor(x25519, mlkem);
}

QJsonObject MirrorCrypto::sealArchive(const QByteArray &plaintext,
                                      const QList<QJsonObject> &recipientBundles,
                                      QString *error)
{
    auto fail = [&](const QString &msg) -> QJsonObject {
        if (error)
            *error = msg;
        return {};
    };
    QString unavailable;
    if (!postQuantumAvailable(&unavailable))
        return fail(unavailable);
    if (recipientBundles.isEmpty() || recipientBundles.size() > 256)
        return fail("At least one recipient is required.");

    // One random content key encrypts the whole archive; only this key is wrapped
    // per recipient, so an N-collaborator repo stores the ciphertext once.
    QByteArray contentKey = randomBytes(kKeyBytes);
    ByteArrayCleanser contentKeyCleanser(&contentKey);
    QByteArray nonce, tag, body;
    if (contentKey.size() != kKeyBytes ||
        !gcmSeal(contentKey, plaintext, nonce, tag, body))
        return fail("Could not encrypt the mirror archive.");

    QJsonArray recipients;
    QSet<QString> recipientIds;
    for (const QJsonObject &bundle : recipientBundles) {
        const QByteArray xPub = fromPublicB64(bundle.value("x25519"));
        const QByteArray mPub =
            fromPublicB64(bundle.value("mlkem768"));
        if (bundle.value("v").toInt() != 1 ||
            xPub.size() != kX25519Bytes || mPub.size() != kMlkemPubBytes)
            return fail("A recipient public bundle is malformed.");
        const QString recipientId = keyIdFor(xPub, mPub);
        if (recipientIds.contains(recipientId))
            return fail("A recipient public bundle is duplicated.");
        recipientIds.insert(recipientId);

        // X25519: ephemeral keypair, ECDH with the recipient's static public key.
        PkeyPtr eph(EVP_PKEY_Q_keygen(nullptr, nullptr, "X25519"), &EVP_PKEY_free);
        QByteArray ephPub(kX25519Bytes, Qt::Uninitialized), ephPriv(kX25519Bytes, Qt::Uninitialized);
        size_t epl = kX25519Bytes, eprl = kX25519Bytes;
        if (!eph ||
            EVP_PKEY_get_raw_public_key(
                eph.get(), reinterpret_cast<unsigned char *>(ephPub.data()), &epl) != 1 ||
            EVP_PKEY_get_raw_private_key(
                eph.get(), reinterpret_cast<unsigned char *>(ephPriv.data()), &eprl) != 1)
            return fail("Could not generate an ephemeral X25519 key.");
        const QByteArray ssX = x25519Derive(ephPriv, xPub);

        // ML-KEM-768: encapsulate to the recipient's static public key.
        QByteArray ct, ssM;
        if (!mlkemEncapsulate(mPub, ct, ssM))
            return fail("Could not encapsulate to the recipient's ML-KEM key.");

        const QByteArray kek = deriveKek(ssX, ssM, ephPub + ct);
        QByteArray wNonce, wTag, wBody;
        if (kek.isEmpty() || !gcmSeal(kek, contentKey, wNonce, wTag, wBody))
            return fail("Could not wrap the content key to a recipient.");

        recipients.append(QJsonObject{
            {"kid", recipientId},
            {"x25519", toB64(ephPub)},
            {"mlkem768", toB64(ct)},
            {"nonce", toB64(wNonce)},
            {"tag", toB64(wTag)},
            {"key", toB64(wBody)}});
    }

    if (error)
        error->clear();
    return {{"kind", "forkmesh.mirror"},
            {"v", 1},
            {"alg", "x25519+mlkem768/aes256gcm"},
            {"nonce", toB64(nonce)},
            {"tag", toB64(tag)},
            {"body", toB64(body)},
            {"recipients", recipients}};
}

QByteArray MirrorCrypto::openArchive(const QJsonObject &envelope,
                                     const Identity &me, QString *error)
{
    auto fail = [&](const QString &msg) -> QByteArray {
        if (error)
            *error = msg;
        return {};
    };
    QString unavailable;
    if (!postQuantumAvailable(&unavailable))
        return fail(unavailable);
    if (!me.isValid())
        return fail("This identity is incomplete.");
    if (envelope.value("kind").toString() != "forkmesh.mirror" ||
        envelope.value("v").toInt() != 1 ||
        envelope.value("alg").toString() !=
            "x25519+mlkem768/aes256gcm")
        return fail("Not a ForkMesh mirror envelope.");

    const QString myKid = me.keyId();
    QByteArray contentKey;
    ByteArrayCleanser contentKeyCleanser(&contentKey);
    const QJsonArray recipients =
        envelope.value("recipients").toArray();
    if (recipients.isEmpty() || recipients.size() > 256)
        return fail("The mirror recipient list is invalid.");
    QSet<QString> seenRecipients;
    for (const QJsonValue &rv : recipients) {
        const QJsonObject r = rv.toObject();
        const QString recipientId = r.value("kid").toString();
        if (recipientId.isEmpty() || seenRecipients.contains(recipientId))
            return fail("The mirror recipient list is invalid.");
        seenRecipients.insert(recipientId);
        if (r.value("kid").toString() != myKid)
            continue;

        const QByteArray ephPub = fromB64(r.value("x25519"));
        const QByteArray ct = fromB64(r.value("mlkem768"));
        const QByteArray ssX = x25519Derive(me.x25519Priv, ephPub);
        const QByteArray ssM = mlkemDecapsulate(me.mlkemPriv, ct);
        const QByteArray kek = deriveKek(ssX, ssM, ephPub + ct);
        if (kek.isEmpty())
            return fail("Could not recover the wrapping key.");
        contentKey = gcmOpen(kek, fromB64(r.value("nonce")), fromB64(r.value("tag")),
                             fromB64(r.value("key")));
        if (contentKey.size() != kKeyBytes)
            return fail("The wrapped content key failed authentication.");
        break;
    }
    if (contentKey.isEmpty())
        return fail("This identity is not a recipient of the archive.");

    const QByteArray plain = gcmOpen(
        contentKey, fromB64(envelope.value("nonce")),
        fromB64(envelope.value("tag")), fromB64(envelope.value("body")));
    if (plain.isEmpty() && !envelope.value("body").toString().isEmpty())
        return fail("The mirror archive failed authentication.");
    if (error)
        error->clear();
    return plain;
}

QJsonObject MirrorCrypto::sealOwnerPayload(
    const QByteArray &plaintext, const QJsonObject &recipientBundle,
    QString *error)
{
    return sealOwnerPayload(plaintext, QList<QJsonObject>{recipientBundle},
                            error);
}

QJsonObject MirrorCrypto::sealOwnerPayload(
    const QByteArray &plaintext, const QList<QJsonObject> &recipientBundles,
    QString *error)
{
    // Matches the Worker's MAX_OWNER_ENVELOPE_RECIPIENTS: one wrap entry per
    // enrolled owner device, never an open-ended broadcast list.
    if (recipientBundles.isEmpty() || recipientBundles.size() > 6) {
        if (error)
            *error = QStringLiteral(
                "An owner envelope takes between one and six owner devices.");
        return {};
    }
    QString archiveError;
    const QJsonObject archive =
        sealArchive(plaintext, recipientBundles, &archiveError);
    if (archive.isEmpty()) {
        if (error)
            *error = archiveError;
        return {};
    }
    const QJsonArray archiveRecipients =
        archive.value(QStringLiteral("recipients")).toArray();
    if (archiveRecipients.size() != recipientBundles.size()) {
        if (error)
            *error = QStringLiteral(
                "The owner envelope lost a recipient while sealing.");
        return {};
    }
    auto asUrl = [](const QJsonValue &value) {
        return toB64Url(fromB64(value));
    };
    QJsonArray sealedRecipients;
    for (const QJsonValue &value : archiveRecipients) {
        const QJsonObject recipient = value.toObject();
        sealedRecipients.append(QJsonObject{
            {QStringLiteral("kid"), recipient.value(QStringLiteral("kid"))},
            {QStringLiteral("x25519"),
             asUrl(recipient.value(QStringLiteral("x25519")))},
            {QStringLiteral("mlkem768"),
             asUrl(recipient.value(QStringLiteral("mlkem768")))},
            {QStringLiteral("nonce"),
             asUrl(recipient.value(QStringLiteral("nonce")))},
            {QStringLiteral("tag"),
             asUrl(recipient.value(QStringLiteral("tag")))},
            {QStringLiteral("key"),
             asUrl(recipient.value(QStringLiteral("key")))},
        });
    }
    if (error)
        error->clear();
    return {
        {QStringLiteral("kind"), QStringLiteral("forkmesh.owner-sealed")},
        {QStringLiteral("v"), 1},
        {QStringLiteral("alg"),
         QStringLiteral("x25519+mlkem768/aes256gcm")},
        {QStringLiteral("nonce"),
         asUrl(archive.value(QStringLiteral("nonce")))},
        {QStringLiteral("tag"),
         asUrl(archive.value(QStringLiteral("tag")))},
        {QStringLiteral("body"),
         asUrl(archive.value(QStringLiteral("body")))},
        {QStringLiteral("recipients"), sealedRecipients},
    };
}

QString MirrorCrypto::ownerPayloadKeyId(const QJsonObject &envelope)
{
    const QStringList keyIds = ownerPayloadKeyIds(envelope);
    return keyIds.isEmpty() ? QString() : keyIds.first();
}

QStringList MirrorCrypto::ownerPayloadKeyIds(const QJsonObject &envelope)
{
    if (envelope.value(QStringLiteral("kind")).toString() !=
            QLatin1String("forkmesh.owner-sealed") ||
        envelope.value(QStringLiteral("v")).toInt() != 1 ||
        envelope.value(QStringLiteral("alg")).toString() !=
            QLatin1String("x25519+mlkem768/aes256gcm"))
        return {};
    const QJsonArray recipients =
        envelope.value(QStringLiteral("recipients")).toArray();
    if (recipients.isEmpty() || recipients.size() > 6)
        return {};
    QStringList keyIds;
    QSet<QString> seen;
    for (const QJsonValue &value : recipients) {
        const QString keyId =
            value.toObject().value(QStringLiteral("kid")).toString();
        if (keyId.size() < 16 || keyId.size() > 128 || seen.contains(keyId))
            return {};
        for (const QChar ch : keyId) {
            if (!(ch.isLetterOrNumber() || ch == QLatin1Char('-') ||
                  ch == QLatin1Char('_')))
                return {};
        }
        seen.insert(keyId);
        keyIds.append(keyId);
    }
    return keyIds;
}

QByteArray MirrorCrypto::openOwnerPayload(
    const QJsonObject &envelope, const Identity &me, QString *error)
{
    auto fail = [&](const QString &message) -> QByteArray {
        if (error)
            *error = message;
        return {};
    };
    const QStringList keyIds = ownerPayloadKeyIds(envelope);
    if (keyIds.isEmpty())
        return fail(QStringLiteral("Not a ForkMesh owner envelope."));
    const int myEntry = me.isValid() ? int(keyIds.indexOf(me.keyId())) : -1;
    if (myEntry < 0)
        return fail(QStringLiteral(
            "This device is not an owner-envelope recipient."));
    const QString keyId = keyIds.at(myEntry);

    const QByteArray payloadNonce =
        fromB64UrlExact(envelope.value(QStringLiteral("nonce")), kNonceBytes);
    const QByteArray payloadTag =
        fromB64UrlExact(envelope.value(QStringLiteral("tag")), kTagBytes);
    const QByteArray payloadBody =
        fromB64UrlBounded(envelope.value(QStringLiteral("body")),
                          1024 * 1024);
    const QJsonObject recipient = envelope.value(QStringLiteral("recipients"))
                                      .toArray()
                                      .at(myEntry)
                                      .toObject();
    const QByteArray ephemeral =
        fromB64UrlExact(recipient.value(QStringLiteral("x25519")),
                        kX25519Bytes);
    const QByteArray kemCiphertext =
        fromB64UrlExact(recipient.value(QStringLiteral("mlkem768")),
                        kMlkemCtBytes);
    const QByteArray wrapNonce =
        fromB64UrlExact(recipient.value(QStringLiteral("nonce")), kNonceBytes);
    const QByteArray wrapTag =
        fromB64UrlExact(recipient.value(QStringLiteral("tag")), kTagBytes);
    const QByteArray wrappedKey =
        fromB64UrlExact(recipient.value(QStringLiteral("key")), kKeyBytes);
    if (payloadNonce.isEmpty() || payloadTag.isEmpty() ||
        payloadBody.isEmpty() || ephemeral.isEmpty() ||
        kemCiphertext.isEmpty() || wrapNonce.isEmpty() ||
        wrapTag.isEmpty() || wrappedKey.isEmpty())
        return fail(QStringLiteral("The owner envelope framing is malformed."));

    const QJsonObject archiveRecipient{
        {QStringLiteral("kid"), keyId},
        {QStringLiteral("x25519"), toB64(ephemeral)},
        {QStringLiteral("mlkem768"), toB64(kemCiphertext)},
        {QStringLiteral("nonce"), toB64(wrapNonce)},
        {QStringLiteral("tag"), toB64(wrapTag)},
        {QStringLiteral("key"), toB64(wrappedKey)},
    };
    const QJsonObject archive{
        {QStringLiteral("kind"), QStringLiteral("forkmesh.mirror")},
        {QStringLiteral("v"), 1},
        {QStringLiteral("alg"),
         QStringLiteral("x25519+mlkem768/aes256gcm")},
        {QStringLiteral("nonce"), toB64(payloadNonce)},
        {QStringLiteral("tag"), toB64(payloadTag)},
        {QStringLiteral("body"), toB64(payloadBody)},
        {QStringLiteral("recipients"), QJsonArray{archiveRecipient}},
    };
    return openArchive(archive, me, error);
}
