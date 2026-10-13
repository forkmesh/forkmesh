#include "VaultCrypto.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QUrl>

#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/params.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>

#include <memory>

#ifndef OSSL_KDF_PARAM_ARGON2_LANES
#define OSSL_KDF_PARAM_ARGON2_LANES "lanes"
#endif
#ifndef OSSL_KDF_PARAM_ARGON2_MEMCOST
#define OSSL_KDF_PARAM_ARGON2_MEMCOST "memcost"
#endif
#ifndef OSSL_KDF_PARAM_THREADS
#define OSSL_KDF_PARAM_THREADS "threads"
#endif
#ifndef OSSL_PKEY_PARAM_ML_KEM_SEED
#define OSSL_PKEY_PARAM_ML_KEM_SEED "seed"
#endif

namespace forkmesh::vault {
namespace {

using PkeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using PkeyCtxPtr = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
using CipherCtxPtr = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;
using MdCtxPtr = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
using KdfPtr = std::unique_ptr<EVP_KDF, decltype(&EVP_KDF_free)>;
using KdfCtxPtr = std::unique_ptr<EVP_KDF_CTX, decltype(&EVP_KDF_CTX_free)>;

constexpr int kFloorMemoryKiB = 19456;
constexpr int kFloorPasses = 2;
constexpr int kMlkemPublicBytes = 1184;
constexpr int kMlkemCiphertextBytes = 1088;
// XWingLabel, the six bytes \.//^\ that close the X-Wing combiner input.
const QByteArray kXWingLabel = QByteArray::fromHex("5c2e2f2f5e5c");

bool fail(Failure *failure, const char *code)
{
    if (failure)
        failure->code = QString::fromLatin1(code);
    return false;
}

QByteArray context(const QString &suffix)
{
    return QByteArrayLiteral("forkmesh-vault/fmv1/") + suffix.toUtf8();
}

unsigned char *raw(QByteArray &bytes) { return reinterpret_cast<unsigned char *>(bytes.data()); }
const unsigned char *raw(const QByteArray &bytes)
{
    return reinterpret_cast<const unsigned char *>(bytes.constData());
}

bool validAccount(const QString &account)
{
    static const QRegularExpression pattern(QStringLiteral("^[a-z0-9](?:[a-z0-9-]{0,62})$"));
    return pattern.match(account).hasMatch();
}

bool validItemId(const QString &itemId)
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{32}$"));
    return pattern.match(itemId).hasMatch();
}

bool hkdf(const QByteArray &secret, const QString &info, QByteArray *out, int length = 32)
{
    KdfPtr kdf(EVP_KDF_fetch(nullptr, "HKDF", nullptr), &EVP_KDF_free);
    KdfCtxPtr ctx(kdf ? EVP_KDF_CTX_new(kdf.get()) : nullptr, &EVP_KDF_CTX_free);
    if (!ctx)
        return false;
    QByteArray salt(32, '\0');
    QByteArray label = context(info);
    QByteArray key = secret;
    char digest[] = "SHA256";
    OSSL_PARAM params[] = {
        OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST, digest, 0),
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_KEY, key.data(), size_t(key.size())),
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT, salt.data(), size_t(salt.size())),
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_INFO, label.data(), size_t(label.size())),
        OSSL_PARAM_construct_end()};
    out->resize(length);
    const bool ok = EVP_KDF_derive(ctx.get(), raw(*out), size_t(length), params) == 1;
    wipe(key);
    return ok;
}

// iv || ciphertext || tag, base64url. `label` is bound as associated data, so
// a blob moved to another row, item, or account fails to open.
bool seal(const QByteArray &key, const QString &label, const QByteArray &plaintext,
          QString *blob, Failure *failure)
{
    if (key.size() != 32)
        return fail(failure, "invalid_key");
    const QByteArray iv = randomBytes(12);
    const QByteArray aad = context(label);
    CipherCtxPtr ctx(EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
    QByteArray sealed(plaintext.size() + 16, '\0');
    int written = 0, tail = 0;
    if (!ctx || EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1
        || EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) != 1
        || EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr, raw(key), raw(iv)) != 1
        || EVP_EncryptUpdate(ctx.get(), nullptr, &written, raw(aad), aad.size()) != 1
        || EVP_EncryptUpdate(ctx.get(), raw(sealed), &written, raw(plaintext), plaintext.size()) != 1
        || EVP_EncryptFinal_ex(ctx.get(), raw(sealed) + written, &tail) != 1
        || EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, 16, raw(sealed) + plaintext.size()) != 1)
        return fail(failure, "encryption_failed");
    *blob = toBase64Url(iv + sealed);
    return true;
}

bool open(const QByteArray &key, const QString &label, const QString &blob,
          QByteArray *plaintext, Failure *failure)
{
    QByteArray bytes;
    if (!fromBase64Url(blob, &bytes))
        return fail(failure, "invalid_encoding");
    if (key.size() != 32)
        return fail(failure, "invalid_key");
    if (bytes.size() < 12 + 16)
        return fail(failure, "invalid_ciphertext");
    const QByteArray aad = context(label);
    const int length = bytes.size() - 12 - 16;
    QByteArray tag = bytes.right(16);
    CipherCtxPtr ctx(EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
    plaintext->resize(length);
    int written = 0, tail = 0;
    if (!ctx || EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1
        || EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) != 1
        || EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr, raw(key), raw(bytes)) != 1
        || EVP_DecryptUpdate(ctx.get(), nullptr, &written, raw(aad), aad.size()) != 1
        || EVP_DecryptUpdate(ctx.get(), raw(*plaintext), &written, raw(bytes) + 12, length) != 1
        || EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, 16, tag.data()) != 1
        || EVP_DecryptFinal_ex(ctx.get(), raw(*plaintext) + written, &tail) != 1) {
        wipe(*plaintext);
        plaintext->clear();
        return fail(failure, "decryption_failed");
    }
    return true;
}

PkeyPtr privateKey(int type, const QByteArray &bytes)
{
    return PkeyPtr(EVP_PKEY_new_raw_private_key(type, nullptr, raw(bytes), size_t(bytes.size())),
                   &EVP_PKEY_free);
}

PkeyPtr publicKey(int type, const QByteArray &bytes)
{
    return PkeyPtr(EVP_PKEY_new_raw_public_key(type, nullptr, raw(bytes), size_t(bytes.size())),
                   &EVP_PKEY_free);
}

bool rawPublic(EVP_PKEY *key, QByteArray *out)
{
    size_t length = 0;
    if (!key || EVP_PKEY_get_raw_public_key(key, nullptr, &length) != 1)
        return false;
    out->resize(int(length));
    return EVP_PKEY_get_raw_public_key(key, raw(*out), &length) == 1;
}

// The ML-KEM-768 key pair a 64-byte seed (d || z) expands to: the form
// FIPS 203, OpenSSL and the browser suite all store.
PkeyPtr mlkemFromSeed(const QByteArray &seed)
{
    PkeyCtxPtr ctx(EVP_PKEY_CTX_new_from_name(nullptr, "ML-KEM-768", nullptr), &EVP_PKEY_CTX_free);
    if (!ctx || seed.size() != 64 || EVP_PKEY_fromdata_init(ctx.get()) != 1)
        return PkeyPtr(nullptr, &EVP_PKEY_free);
    QByteArray copy = seed;
    OSSL_PARAM params[] = {
        OSSL_PARAM_construct_octet_string(OSSL_PKEY_PARAM_ML_KEM_SEED, copy.data(), size_t(copy.size())),
        OSSL_PARAM_construct_end()};
    EVP_PKEY *key = nullptr;
    const bool ok = EVP_PKEY_fromdata(ctx.get(), &key, EVP_PKEY_KEYPAIR, params) == 1;
    wipe(copy);
    return PkeyPtr(ok ? key : nullptr, &EVP_PKEY_free);
}

PkeyPtr mlkemPublic(const QByteArray &bytes)
{
    return PkeyPtr(bytes.size() == kMlkemPublicBytes
                       ? EVP_PKEY_new_raw_public_key_ex(nullptr, "ML-KEM-768", nullptr, raw(bytes),
                                                        size_t(bytes.size()))
                       : nullptr,
                   &EVP_PKEY_free);
}

bool x25519(EVP_PKEY *mine, EVP_PKEY *theirs, QByteArray *shared)
{
    PkeyCtxPtr ctx(mine && theirs ? EVP_PKEY_CTX_new(mine, nullptr) : nullptr, &EVP_PKEY_CTX_free);
    size_t length = 32;
    shared->resize(32);
    if (!ctx || EVP_PKEY_derive_init(ctx.get()) != 1
        || EVP_PKEY_derive_set_peer(ctx.get(), theirs) != 1
        || EVP_PKEY_derive(ctx.get(), raw(*shared), &length) != 1 || length != 32)
        return false;
    // An all-zero result means a low-order public key was supplied.
    return *shared != QByteArray(32, '\0');
}

bool sha3(const QByteArray &input, QByteArray *out)
{
    unsigned int length = 0;
    out->resize(32);
    return EVP_Digest(raw(input), size_t(input.size()), raw(*out), &length, EVP_sha3_256(), nullptr) == 1
           && length == 32;
}

bool sign(const QByteArray &ed25519, const QByteArray &message, QByteArray *signature)
{
    PkeyPtr key = privateKey(EVP_PKEY_ED25519, ed25519);
    MdCtxPtr ctx(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    size_t length = 64;
    signature->resize(64);
    return key && ctx && EVP_DigestSignInit(ctx.get(), nullptr, nullptr, nullptr, key.get()) == 1
           && EVP_DigestSign(ctx.get(), raw(*signature), &length, raw(message), size_t(message.size())) == 1
           && length == 64;
}

bool verify(const QByteArray &ed25519, const QByteArray &message, const QByteArray &signature)
{
    PkeyPtr key = publicKey(EVP_PKEY_ED25519, ed25519);
    MdCtxPtr ctx(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    return key && ctx && signature.size() == 64
           && EVP_DigestVerifyInit(ctx.get(), nullptr, nullptr, nullptr, key.get()) == 1
           && EVP_DigestVerify(ctx.get(), raw(signature), size_t(signature.size()), raw(message),
                               size_t(message.size())) == 1;
}

QByteArray bundleMessage(const QString &account, const QByteArray &x, const QByteArray &m,
                         const QByteArray &ed)
{
    const QStringList lines{QString::fromLatin1(context(QStringLiteral("identity"))), account,
                            toBase64Url(x), toBase64Url(m), toBase64Url(ed)};
    return lines.join(QLatin1Char('\n')).toUtf8();
}

QString grantContext(const QString &itemId, const QString &sender, const QString &recipient,
                     const QString &permission)
{
    return QStringList{QStringLiteral("grant"), itemId, sender, recipient, permission}
        .join(QLatin1Char('|'));
}

QByteArray grantMessage(const QString &label, const QByteArray &recipientEd, const QByteArray &ctX,
                        const QByteArray &ctM, const QByteArray &wrapped)
{
    const QByteArray digest = QCryptographicHash::hash(ctX + ctM + wrapped, QCryptographicHash::Sha256);
    const QStringList lines{QString::fromLatin1(context(label)), toBase64Url(recipientEd),
                            toBase64Url(digest)};
    return lines.join(QLatin1Char('\n')).toUtf8();
}

bool validPermission(const QString &permission)
{
    return permission == QLatin1String("read") || permission == QLatin1String("write");
}

QByteArray compactJson(const QJsonObject &value)
{
    return QJsonDocument(value).toJson(QJsonDocument::Compact);
}

bool parseObject(const QByteArray &bytes, QJsonObject *value)
{
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return false;
    *value = document.object();
    return true;
}

const char kBase32[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

// Uniform over [0, bound): bytes that would bias the result are thrown away.
int randomBelow(int bound)
{
    const int limit = 256 - (256 % bound);
    for (;;) {
        const int byte = static_cast<unsigned char>(randomBytes(1).at(0));
        if (byte < limit)
            return byte % bound;
    }
}

QUrl loginUrl(const QString &text)
{
    static const QRegularExpression scheme(QStringLiteral("^[a-z][a-z0-9+.-]*://"),
                                           QRegularExpression::CaseInsensitiveOption);
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return {};
    const QUrl url(scheme.match(trimmed).hasMatch() ? trimmed : QStringLiteral("https://") + trimmed,
                   QUrl::StrictMode);
    if (!url.isValid() || url.host().isEmpty())
        return {};
    const QString host = url.host().toLower();
    const bool local = host == QLatin1String("localhost") || host == QLatin1String("127.0.0.1")
                       || host == QLatin1String("::1");
    if (url.scheme() != QLatin1String("https") && !(url.scheme() == QLatin1String("http") && local))
        return {};
    return url;
}

}  // namespace

// ---- bytes ---------------------------------------------------------------------

QString toBase64Url(const QByteArray &bytes)
{
    return QString::fromLatin1(
        bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

bool fromBase64Url(const QString &text, QByteArray *bytes)
{
    static const QRegularExpression alphabet(QStringLiteral("^[A-Za-z0-9_-]*$"));
    if (!alphabet.match(text).hasMatch())
        return false;
    const auto decoded = QByteArray::fromBase64Encoding(
        text.toLatin1(), QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded)
        return false;
    *bytes = *decoded;
    return true;
}

void wipe(QByteArray &bytes)
{
    if (!bytes.isEmpty())
        OPENSSL_cleanse(bytes.data(), size_t(bytes.size()));
}

QByteArray randomBytes(int length)
{
    QByteArray out(length, '\0');
    if (RAND_bytes(raw(out), length) != 1)
        qFatal("The system random number generator failed.");
    return out;
}

// ---- master password -----------------------------------------------------------

QJsonObject Kdf::toJson() const
{
    return {{QStringLiteral("algorithm"), QStringLiteral("argon2id")},
            {QStringLiteral("version"), 19},
            {QStringLiteral("memoryKiB"), memoryKiB},
            {QStringLiteral("passes"), passes},
            {QStringLiteral("parallelism"), parallelism},
            {QStringLiteral("salt"), toBase64Url(salt)}};
}

bool Kdf::fromJson(const QJsonObject &json, Kdf *kdf, Failure *failure)
{
    Kdf parsed;
    const auto whole = [&json](const char *name, int *out) {
        const QJsonValue value = json.value(QLatin1String(name));
        const double number = value.toDouble(-1);
        *out = int(number);
        return value.isDouble() && number == double(*out);
    };
    if (json.value(QStringLiteral("algorithm")).toString() != QLatin1String("argon2id")
        || json.value(QStringLiteral("version")).toInt() != 19
        || !whole("memoryKiB", &parsed.memoryKiB) || !whole("passes", &parsed.passes)
        || !whole("parallelism", &parsed.parallelism)
        || !fromBase64Url(json.value(QStringLiteral("salt")).toString(), &parsed.salt)
        || parsed.salt.size() < 16 || parsed.salt.size() > 64
        || parsed.memoryKiB < kFloorMemoryKiB || parsed.memoryKiB > 1048576
        || parsed.passes < kFloorPasses || parsed.passes > 16
        || parsed.parallelism < 1 || parsed.parallelism > 16)
        return fail(failure, "unsafe_kdf_parameters");
    *kdf = parsed;
    return true;
}

Kdf Kdf::fresh()
{
    Kdf kdf;
    kdf.salt = randomBytes(16);
    return kdf;
}

bool cryptoAvailable(Failure *failure)
{
    KdfPtr argon2(EVP_KDF_fetch(nullptr, "ARGON2ID", nullptr), &EVP_KDF_free);
    if (!argon2)
        return fail(failure, "argon2id_unavailable");
    EVP_KEM *kem = EVP_KEM_fetch(nullptr, "ML-KEM-768", nullptr);
    EVP_KEYMGMT *keys = EVP_KEYMGMT_fetch(nullptr, "ML-KEM-768", nullptr);
    const bool mlkem = kem && keys;
    EVP_KEM_free(kem);
    EVP_KEYMGMT_free(keys);
    return mlkem ? true : fail(failure, "mlkem_unavailable");
}

bool stretch(const QString &password, const Kdf &kdf, QByteArray *masterKey, Failure *failure)
{
    Failure checked;
    Kdf verified;
    if (!Kdf::fromJson(kdf.toJson(), &verified, &checked))
        return fail(failure, "unsafe_kdf_parameters");
    if (!cryptoAvailable(failure))
        return false;
    KdfPtr algorithm(EVP_KDF_fetch(nullptr, "ARGON2ID", nullptr), &EVP_KDF_free);
    KdfCtxPtr ctx(algorithm ? EVP_KDF_CTX_new(algorithm.get()) : nullptr, &EVP_KDF_CTX_free);
    if (!ctx)
        return fail(failure, "argon2id_unavailable");
    QByteArray secret = password.normalized(QString::NormalizationForm_KC).toUtf8();
    QByteArray salt = kdf.salt;
    uint32_t passes = uint32_t(kdf.passes), memory = uint32_t(kdf.memoryKiB),
             lanes = uint32_t(kdf.parallelism), threads = 1;
    OSSL_PARAM params[] = {
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_PASSWORD, secret.data(), size_t(secret.size())),
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT, salt.data(), size_t(salt.size())),
        OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ITER, &passes),
        OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_MEMCOST, &memory),
        OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_LANES, &lanes),
        OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_THREADS, &threads),
        OSSL_PARAM_construct_end()};
    masterKey->resize(32);
    const bool ok = EVP_KDF_derive(ctx.get(), raw(*masterKey), 32, params) == 1;
    wipe(secret);
    return ok ? true : fail(failure, "kdf_failed");
}

// ---- keyset --------------------------------------------------------------------

Keyset::~Keyset() { clear(); }

void Keyset::clear()
{
    for (QByteArray *part : {&vaultKey, &x25519, &mlkemSeed, &ed25519}) {
        // Only the last owner of the bytes may zero them: a copy shares them.
        if (part->isDetached())
            wipe(*part);
        part->clear();
    }
}

bool Keyset::isValid() const
{
    return vaultKey.size() == 32 && x25519.size() == 32 && mlkemSeed.size() == 64
           && ed25519.size() == 32;
}

QJsonObject Keyset::toJson() const
{
    return {{QStringLiteral("v"), 1},
            {QStringLiteral("vaultKey"), toBase64Url(vaultKey)},
            {QStringLiteral("x25519"), toBase64Url(x25519)},
            {QStringLiteral("mlkemSeed"), toBase64Url(mlkemSeed)},
            {QStringLiteral("ed25519"), toBase64Url(ed25519)}};
}

bool Keyset::fromJson(const QJsonObject &json, Keyset *keyset, Failure *failure)
{
    if (json.value(QStringLiteral("v")).toInt() != 1)
        return fail(failure, "unsupported_keyset");
    Keyset parsed;
    if (!fromBase64Url(json.value(QStringLiteral("vaultKey")).toString(), &parsed.vaultKey)
        || !fromBase64Url(json.value(QStringLiteral("x25519")).toString(), &parsed.x25519)
        || !fromBase64Url(json.value(QStringLiteral("mlkemSeed")).toString(), &parsed.mlkemSeed)
        || !fromBase64Url(json.value(QStringLiteral("ed25519")).toString(), &parsed.ed25519)
        || !parsed.isValid())
        return fail(failure, "invalid_keyset");
    *keyset = parsed;
    return true;
}

bool Keyset::generate(Keyset *keyset, Failure *failure)
{
    if (!cryptoAvailable(failure))
        return false;
    Keyset fresh;
    fresh.vaultKey = randomBytes(32);
    fresh.x25519 = randomBytes(32);       // RFC 7748 clamps on use
    fresh.mlkemSeed = randomBytes(64);
    fresh.ed25519 = randomBytes(32);      // RFC 8032: any 32 bytes are a seed
    if (!privateKey(EVP_PKEY_X25519, fresh.x25519) || !privateKey(EVP_PKEY_ED25519, fresh.ed25519)
        || !mlkemFromSeed(fresh.mlkemSeed))
        return fail(failure, "key_generation_failed");
    *keyset = fresh;
    return true;
}

bool wrapKeyset(const QByteArray &secret, const QString &purpose, const QString &account,
                const Keyset &keyset, QString *blob, Failure *failure)
{
    if (!keyset.isValid())
        return fail(failure, "invalid_keyset");
    if (!validAccount(account))
        return fail(failure, "invalid_account");
    QByteArray kek;
    if (!hkdf(secret, QStringLiteral("keyset-kek|") + purpose, &kek))
        return fail(failure, "kdf_failed");
    QByteArray plain = compactJson(keyset.toJson());
    const bool ok = seal(kek, QStringLiteral("keyset|%1|%2").arg(purpose, account), plain, blob, failure);
    wipe(kek);
    wipe(plain);
    return ok;
}

bool unwrapKeyset(const QByteArray &secret, const QString &purpose, const QString &account,
                  const QString &blob, Keyset *keyset, Failure *failure)
{
    if (!validAccount(account))
        return fail(failure, "invalid_account");
    QByteArray kek, plain;
    if (!hkdf(secret, QStringLiteral("keyset-kek|") + purpose, &kek))
        return fail(failure, "kdf_failed");
    Failure inner;
    const bool opened = open(kek, QStringLiteral("keyset|%1|%2").arg(purpose, account), blob, &plain, &inner);
    wipe(kek);
    // A wrong master password and a corrupted blob look the same on purpose.
    if (!opened)
        return fail(failure, "wrong_password");
    QJsonObject json;
    const bool parsed = parseObject(plain, &json);
    wipe(plain);
    if (!parsed)
        return fail(failure, "wrong_password");
    return Keyset::fromJson(json, keyset, failure);
}

// ---- recovery code ---------------------------------------------------------------
// 256 random bits as 52 Crockford base32 symbols (the last carries 4 padding
// bits), in groups of four.

QString newRecoveryCode()
{
    QByteArray bytes = randomBytes(32);
    bytes.append('\0');                                 // room for the 4 padding bits
    QString out;
    for (int symbol = 0; symbol < 52; ++symbol) {
        const int bit = symbol * 5;
        const int word = (static_cast<unsigned char>(bytes.at(bit / 8)) << 8)
                         | static_cast<unsigned char>(bytes.at(bit / 8 + 1 < 33 ? bit / 8 + 1 : 32));
        out.append(QLatin1Char(kBase32[(word >> (11 - bit % 8)) & 31]));
        if (symbol % 4 == 3 && symbol != 51)
            out.append(QLatin1Char('-'));
    }
    wipe(bytes);
    return out;
}

bool recoverySecret(const QString &code, QByteArray *secret, Failure *failure)
{
    QString clean = code.toUpper();
    clean.remove(QRegularExpression(QStringLiteral("[\\s-]")));
    clean.replace(QLatin1Char('O'), QLatin1Char('0'));
    clean.replace(QLatin1Char('I'), QLatin1Char('1'));
    clean.replace(QLatin1Char('L'), QLatin1Char('1'));
    if (clean.size() != 52)
        return fail(failure, "invalid_recovery_code");
    QByteArray out(33, '\0');
    for (int symbol = 0; symbol < 52; ++symbol) {
        const char *found = strchr(kBase32, clean.at(symbol).toLatin1());
        if (!found || !*found)
            return fail(failure, "invalid_recovery_code");
        const int value = int(found - kBase32), bit = symbol * 5;
        const int shifted = value << (11 - bit % 8);
        out[bit / 8] = char(static_cast<unsigned char>(out.at(bit / 8)) | (shifted >> 8));
        if (bit / 8 + 1 < 33)
            out[bit / 8 + 1] = char(static_cast<unsigned char>(out.at(bit / 8 + 1)) | (shifted & 0xff));
    }
    *secret = out.left(32);
    wipe(out);
    return true;
}

// ---- public keys -----------------------------------------------------------------

QJsonObject Bundle::toJson() const
{
    return {{QStringLiteral("v"), 1},
            {QStringLiteral("suite"), QLatin1String(kSuite)},
            {QStringLiteral("account"), account},
            {QStringLiteral("x25519"), toBase64Url(x25519)},
            {QStringLiteral("mlkem768"), toBase64Url(mlkem768)},
            {QStringLiteral("ed25519"), toBase64Url(ed25519)},
            {QStringLiteral("signature"), toBase64Url(signature)}};
}

bool Bundle::fromJson(const QJsonObject &json, const QString &account, Bundle *bundle,
                      Failure *failure)
{
    Bundle parsed;
    parsed.account = account;
    // The name inside the bundle is part of what its signature covers: one
    // account's bundle served under another name never verifies.
    if (!validAccount(account) || json.value(QStringLiteral("v")).toInt() != 1
        || json.value(QStringLiteral("suite")).toString() != QLatin1String(kSuite)
        || json.value(QStringLiteral("account")).toString() != account
        || !fromBase64Url(json.value(QStringLiteral("x25519")).toString(), &parsed.x25519)
        || !fromBase64Url(json.value(QStringLiteral("mlkem768")).toString(), &parsed.mlkem768)
        || !fromBase64Url(json.value(QStringLiteral("ed25519")).toString(), &parsed.ed25519)
        || !fromBase64Url(json.value(QStringLiteral("signature")).toString(), &parsed.signature)
        || parsed.x25519.size() != 32 || parsed.ed25519.size() != 32
        || parsed.mlkem768.size() != kMlkemPublicBytes
        // OpenSSL runs the FIPS 203 modulus check when it imports the key.
        || !mlkemPublic(parsed.mlkem768)
        || !verify(parsed.ed25519,
                   bundleMessage(account, parsed.x25519, parsed.mlkem768, parsed.ed25519),
                   parsed.signature))
        return fail(failure, "invalid_public_keys");
    *bundle = parsed;
    return true;
}

bool publicBundle(const Keyset &keyset, const QString &account, Bundle *bundle, Failure *failure)
{
    if (!keyset.isValid())
        return fail(failure, "invalid_keyset");
    if (!validAccount(account))
        return fail(failure, "invalid_account");
    Bundle made;
    made.account = account;
    PkeyPtr x = privateKey(EVP_PKEY_X25519, keyset.x25519);
    PkeyPtr ed = privateKey(EVP_PKEY_ED25519, keyset.ed25519);
    PkeyPtr kem = mlkemFromSeed(keyset.mlkemSeed);
    size_t length = 0;
    made.mlkem768.resize(kMlkemPublicBytes);
    if (!rawPublic(x.get(), &made.x25519) || !rawPublic(ed.get(), &made.ed25519) || !kem
        || EVP_PKEY_get_octet_string_param(kem.get(), OSSL_PKEY_PARAM_PUB_KEY, raw(made.mlkem768),
                                           size_t(made.mlkem768.size()), &length) != 1
        || length != size_t(kMlkemPublicBytes)
        || !sign(keyset.ed25519, bundleMessage(account, made.x25519, made.mlkem768, made.ed25519),
                 &made.signature))
        return fail(failure, "key_generation_failed");
    *bundle = made;
    return true;
}

bool fingerprint(const Bundle &bundle, QString *text, Failure *failure)
{
    if (bundle.x25519.size() != 32 || bundle.ed25519.size() != 32
        || bundle.mlkem768.size() != kMlkemPublicBytes)
        return fail(failure, "invalid_public_keys");
    const QByteArray digest = QCryptographicHash::hash(
        context(QStringLiteral("fingerprint\n%1\n").arg(bundle.account)) + bundle.x25519
            + bundle.mlkem768 + bundle.ed25519,
        QCryptographicHash::Sha256);
    const QString hex = QString::fromLatin1(digest.toHex());
    QStringList groups;
    for (int at = 0; at < hex.size(); at += 4)
        groups.append(hex.mid(at, 4));
    *text = groups.join(QLatin1Char(' '));
    return true;
}

// ---- items -----------------------------------------------------------------------

QString newItemId() { return QString::fromLatin1(randomBytes(16).toHex()); }

bool sealItem(const QByteArray &itemKey, const QString &itemId, const QJsonObject &fields,
              QString *blob, Failure *failure)
{
    if (!validItemId(itemId))
        return fail(failure, "invalid_item_id");
    QByteArray body = compactJson(fields);
    // Padded to a 256-byte step so a ciphertext's size says little about the
    // password inside it. JSON ignores the trailing spaces.
    QByteArray padded(((body.size() + 1 + 255) / 256) * 256, ' ');
    padded.replace(0, body.size(), body);
    const bool ok = seal(itemKey, QStringLiteral("item|") + itemId, padded, blob, failure);
    wipe(body);
    wipe(padded);
    return ok;
}

bool openItem(const QByteArray &itemKey, const QString &itemId, const QString &blob,
              QJsonObject *fields, Failure *failure)
{
    if (!validItemId(itemId))
        return fail(failure, "invalid_item_id");
    QByteArray plain;
    if (!open(itemKey, QStringLiteral("item|") + itemId, blob, &plain, failure))
        return false;
    const bool parsed = parseObject(plain.trimmed(), fields);
    wipe(plain);
    return parsed ? true : fail(failure, "invalid_item");
}

bool wrapItemKey(const Keyset &keyset, const QString &itemId, const QByteArray &itemKey,
                 QString *blob, Failure *failure)
{
    if (!validItemId(itemId))
        return fail(failure, "invalid_item_id");
    return seal(keyset.vaultKey, QStringLiteral("itemkey|") + itemId, itemKey, blob, failure);
}

bool unwrapItemKey(const Keyset &keyset, const QString &itemId, const QString &blob,
                   QByteArray *itemKey, Failure *failure)
{
    if (!validItemId(itemId))
        return fail(failure, "invalid_item_id");
    return open(keyset.vaultKey, QStringLiteral("itemkey|") + itemId, blob, itemKey, failure);
}

bool sealPrivate(const Keyset &keyset, const QString &label, const QJsonObject &value,
                 QString *blob, Failure *failure)
{
    return seal(keyset.vaultKey, QStringLiteral("private|") + label, compactJson(value), blob, failure);
}

bool openPrivate(const Keyset &keyset, const QString &label, const QString &blob,
                 QJsonObject *value, Failure *failure)
{
    QByteArray plain;
    if (!open(keyset.vaultKey, QStringLiteral("private|") + label, blob, &plain, failure))
        return false;
    return parseObject(plain, value) ? true : fail(failure, "invalid_item");
}

// ---- shares ----------------------------------------------------------------------

bool sealGrant(const Keyset &sender, const QString &senderAccount, const Bundle &recipient,
               const QString &itemId, const QByteArray &itemKey, const QString &permission,
               QJsonObject *grant, Failure *failure)
{
    if (!validPermission(permission))
        return fail(failure, "invalid_permission");
    if (!sender.isValid() || !validAccount(senderAccount) || !validItemId(itemId)
        || itemKey.size() != 32)
        return fail(failure, "invalid_share");
    const QString label = grantContext(itemId, senderAccount, recipient.account, permission);

    // Classical half: a fresh X25519 key, whose public half is the ciphertext.
    PkeyPtr ephemeral(EVP_PKEY_Q_keygen(nullptr, nullptr, "X25519"), &EVP_PKEY_free);
    PkeyPtr theirs = publicKey(EVP_PKEY_X25519, recipient.x25519);
    QByteArray ctX, ssX;
    if (!rawPublic(ephemeral.get(), &ctX) || !x25519(ephemeral.get(), theirs.get(), &ssX))
        return fail(failure, "invalid_public_keys");

    // Post-quantum half.
    PkeyPtr kem = mlkemPublic(recipient.mlkem768);
    PkeyCtxPtr ctx(kem ? EVP_PKEY_CTX_new_from_pkey(nullptr, kem.get(), nullptr) : nullptr,
                   &EVP_PKEY_CTX_free);
    size_t ctLength = 0, ssLength = 0;
    if (!ctx || EVP_PKEY_encapsulate_init(ctx.get(), nullptr) != 1
        || EVP_PKEY_encapsulate(ctx.get(), nullptr, &ctLength, nullptr, &ssLength) != 1
        || ctLength != size_t(kMlkemCiphertextBytes) || ssLength != 32)
        return fail(failure, "invalid_public_keys");
    QByteArray ctM(int(ctLength), '\0'), ssM(int(ssLength), '\0');
    if (EVP_PKEY_encapsulate(ctx.get(), raw(ctM), &ctLength, raw(ssM), &ssLength) != 1)
        return fail(failure, "invalid_public_keys");

    QByteArray shared, wrapKey;
    QString wrapped;
    const bool sealed = sha3(ssM + ssX + ctX + recipient.x25519 + kXWingLabel, &shared)
                        && hkdf(shared, label, &wrapKey)
                        && seal(wrapKey, label, itemKey, &wrapped, failure);
    wipe(ssM);
    wipe(ssX);
    wipe(shared);
    wipe(wrapKey);
    if (!sealed)
        return failure && failure->failed() ? false : fail(failure, "encryption_failed");

    QByteArray wrappedBytes, signature;
    fromBase64Url(wrapped, &wrappedBytes);
    if (!sign(sender.ed25519, grantMessage(label, recipient.ed25519, ctX, ctM, wrappedBytes), &signature))
        return fail(failure, "signing_failed");
    *grant = {{QStringLiteral("v"), 1},
              {QStringLiteral("suite"), QLatin1String(kSuite)},
              {QStringLiteral("x25519"), toBase64Url(ctX)},
              {QStringLiteral("mlkem768"), toBase64Url(ctM)},
              {QStringLiteral("wrapped"), wrapped},
              {QStringLiteral("signature"), toBase64Url(signature)}};
    return true;
}

bool openGrant(const Keyset &recipient, const QString &recipientAccount, const Bundle &sender,
               const QString &itemId, const QJsonObject &grant, const QString &permission,
               QByteArray *itemKey, Failure *failure)
{
    QByteArray ctX, ctM, wrappedBytes, signature;
    const QString wrapped = grant.value(QStringLiteral("wrapped")).toString();
    if (grant.value(QStringLiteral("v")).toInt() != 1
        || grant.value(QStringLiteral("suite")).toString() != QLatin1String(kSuite)
        || !validPermission(permission) || !recipient.isValid() || !validAccount(recipientAccount)
        || !validItemId(itemId)
        || !fromBase64Url(grant.value(QStringLiteral("x25519")).toString(), &ctX)
        || !fromBase64Url(grant.value(QStringLiteral("mlkem768")).toString(), &ctM)
        || !fromBase64Url(wrapped, &wrappedBytes)
        || !fromBase64Url(grant.value(QStringLiteral("signature")).toString(), &signature)
        || ctX.size() != 32 || ctM.size() != kMlkemCiphertextBytes)
        return fail(failure, "invalid_share");
    const QString label = grantContext(itemId, sender.account, recipientAccount, permission);

    // Nothing is decapsulated until the sender's signature holds.
    PkeyPtr myEd = privateKey(EVP_PKEY_ED25519, recipient.ed25519);
    QByteArray myEdPublic;
    if (!rawPublic(myEd.get(), &myEdPublic)
        || !verify(sender.ed25519, grantMessage(label, myEdPublic, ctX, ctM, wrappedBytes), signature))
        return fail(failure, "invalid_share_signature");

    PkeyPtr mine = privateKey(EVP_PKEY_X25519, recipient.x25519);
    PkeyPtr theirs = publicKey(EVP_PKEY_X25519, ctX);
    QByteArray myX, ssX;
    if (!rawPublic(mine.get(), &myX) || !x25519(mine.get(), theirs.get(), &ssX))
        return fail(failure, "invalid_share");

    PkeyPtr kem = mlkemFromSeed(recipient.mlkemSeed);
    PkeyCtxPtr ctx(kem ? EVP_PKEY_CTX_new_from_pkey(nullptr, kem.get(), nullptr) : nullptr,
                   &EVP_PKEY_CTX_free);
    size_t ssLength = 32;
    QByteArray ssM(32, '\0');
    if (!ctx || EVP_PKEY_decapsulate_init(ctx.get(), nullptr) != 1
        || EVP_PKEY_decapsulate(ctx.get(), raw(ssM), &ssLength, raw(ctM), size_t(ctM.size())) != 1
        || ssLength != 32)
        return fail(failure, "invalid_share");

    QByteArray shared, wrapKey;
    const bool derived = sha3(ssM + ssX + ctX + myX + kXWingLabel, &shared) && hkdf(shared, label, &wrapKey);
    wipe(ssM);
    wipe(ssX);
    wipe(shared);
    const bool opened = derived && open(wrapKey, label, wrapped, itemKey, failure);
    wipe(wrapKey);
    if (!opened)
        return failure && failure->failed() ? false : fail(failure, "invalid_share");
    return itemKey->size() == 32 ? true : fail(failure, "invalid_share");
}

// ---- password generator ------------------------------------------------------------

QString generatePassword(const PasswordOptions &options)
{
    QStringList classes;
    if (options.lower) classes << QStringLiteral("abcdefghijkmnopqrstuvwxyz");      // no l
    if (options.upper) classes << QStringLiteral("ABCDEFGHJKLMNPQRSTUVWXYZ");       // no I, O
    if (options.digits) classes << QStringLiteral("23456789");                      // no 0, 1
    if (options.symbols) classes << QStringLiteral("!@#$%^&*-_=+?");
    if (classes.isEmpty())
        return {};
    const QString alphabet = classes.join(QString());
    const int length = qBound(8, options.length, 128);
    for (;;) {
        QString out;
        for (int i = 0; i < length; ++i)
            out.append(alphabet.at(randomBelow(int(alphabet.size()))));
        // Redraw the whole password rather than patch a class in: every
        // accepted password is then equally likely.
        bool complete = true;
        for (const QString &set : classes) {
            bool present = false;
            for (const QChar c : out)
                present = present || set.contains(c);
            complete = complete && present;
        }
        if (complete)
            return out;
    }
}

// ---- sites -----------------------------------------------------------------------

QString loginHost(const QString &url)
{
    QString host = loginUrl(url).host().toLower();
    if (host.startsWith(QLatin1String("www.")))
        host.remove(0, 4);
    return host;
}

QString loginHref(const QString &url)
{
    const QUrl parsed = loginUrl(url);
    return parsed.isValid() ? parsed.toString(QUrl::FullyEncoded) : QString();
}

}  // namespace forkmesh::vault
