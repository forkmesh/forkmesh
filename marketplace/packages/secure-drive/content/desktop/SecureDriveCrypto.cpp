#include "SecureDriveCrypto.h"

#include <QRegularExpression>
#include <QStringList>

#include <cstring>

#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include <openssl/params.h>
#include <openssl/rand.h>

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

namespace forkmesh::securedrive {
namespace {

using PkeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using PkeyCtxPtr = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
using CipherCtxPtr = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;
using MdCtxPtr = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
using KdfPtr = std::unique_ptr<EVP_KDF, decltype(&EVP_KDF_free)>;
using KdfCtxPtr = std::unique_ptr<EVP_KDF_CTX, decltype(&EVP_KDF_CTX_free)>;

constexpr int kFloorMemoryKiB = 19456;
constexpr int kFloorPasses = 2;
const QByteArray kXWingLabel = QByteArray::fromHex("5c2e2f2f5e5c");
const char kBase32[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

QByteArray context(const QString &suffix)
{
    return QByteArrayLiteral("forkmesh-secure-drive/fmsd1/") + suffix.toUtf8();
}

unsigned char *raw(QByteArray &bytes) { return reinterpret_cast<unsigned char *>(bytes.data()); }
const unsigned char *raw(const QByteArray &bytes)
{
    return reinterpret_cast<const unsigned char *>(bytes.constData());
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
    return *shared != QByteArray(32, '\0');
}

bool sha3(const QByteArray &input, QByteArray *out)
{
    unsigned int length = 0;
    out->resize(32);
    return EVP_Digest(raw(input), size_t(input.size()), raw(*out), &length, EVP_sha3_256(), nullptr) == 1
           && length == 32;
}

QString base32(const QByteArray &bytes)
{
    QString out;
    quint32 buffer = 0;
    int bits = 0;
    for (const char byte : bytes) {
        buffer = (buffer << 8) | quint8(byte);
        bits += 8;
        while (bits >= 5) {
            out.append(QLatin1Char(kBase32[(buffer >> (bits - 5)) & 31]));
            bits -= 5;
        }
    }
    if (bits > 0)
        out.append(QLatin1Char(kBase32[(buffer << (5 - bits)) & 31]));
    return out;
}

}  // namespace

bool fail(Failure *failure, const char *code)
{
    if (failure)
        failure->code = QString::fromLatin1(code);
    return false;
}

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
    if (length > 0 && RAND_bytes(raw(out), length) != 1)
        qFatal("The system random number generator failed.");
    return out;
}

QString randomId()
{
    return QString::fromLatin1(randomBytes(16).toHex());
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

QByteArray sha256(const QByteArray &bytes)
{
    QByteArray out(32, '\0');
    unsigned int length = 0;
    if (EVP_Digest(raw(bytes), size_t(bytes.size()), raw(out), &length, EVP_sha256(), nullptr) != 1)
        return {};
    return out;
}

bool hkdf(const QByteArray &secret, const QString &info, QByteArray *out, int length)
{
    KdfPtr kdf(EVP_KDF_fetch(nullptr, "HKDF", nullptr), &EVP_KDF_free);
    KdfCtxPtr ctx(kdf ? EVP_KDF_CTX_new(kdf.get()) : nullptr, &EVP_KDF_CTX_free);
    if (!ctx || secret.isEmpty())
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

QByteArray hmac(const QByteArray &key, const QByteArray &message)
{
    QByteArray out(32, '\0');
    unsigned int length = 0;
    if (!HMAC(EVP_sha256(), key.constData(), key.size(), raw(message), size_t(message.size()),
              raw(out), &length)
        || length != 32)
        return {};
    return out;
}

bool constantTimeEqual(const QByteArray &left, const QByteArray &right)
{
    return left.size() == right.size() && !left.isEmpty()
           && CRYPTO_memcmp(left.constData(), right.constData(), size_t(left.size())) == 0;
}

bool aeadSeal(const QByteArray &key, const QByteArray &nonce, const QByteArray &aad,
              const QByteArray &plaintext, QByteArray *sealed)
{
    if (key.size() != kKeyBytes || nonce.size() != 12)
        return false;
    CipherCtxPtr ctx(EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
    sealed->resize(plaintext.size() + 16);
    int written = 0, tail = 0;
    return ctx && EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1
           && EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) == 1
           && EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr, raw(key), raw(nonce)) == 1
           && EVP_EncryptUpdate(ctx.get(), nullptr, &written, raw(aad), int(aad.size())) == 1
           && EVP_EncryptUpdate(ctx.get(), raw(*sealed), &written, raw(plaintext),
                                int(plaintext.size())) == 1
           && EVP_EncryptFinal_ex(ctx.get(), raw(*sealed) + written, &tail) == 1
           && EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, 16,
                                  raw(*sealed) + plaintext.size()) == 1;
}

bool aeadOpen(const QByteArray &key, const QByteArray &nonce, const QByteArray &aad,
              const QByteArray &sealed, QByteArray *plaintext)
{
    if (key.size() != kKeyBytes || nonce.size() != 12 || sealed.size() < 16)
        return false;
    const int length = int(sealed.size()) - 16;
    QByteArray tag = sealed.right(16);
    CipherCtxPtr ctx(EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
    plaintext->resize(length);
    int written = 0, tail = 0;
    if (!ctx || EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1
        || EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) != 1
        || EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr, raw(key), raw(nonce)) != 1
        || EVP_DecryptUpdate(ctx.get(), nullptr, &written, raw(aad), int(aad.size())) != 1
        || EVP_DecryptUpdate(ctx.get(), raw(*plaintext), &written, raw(sealed), length) != 1
        || EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, 16, tag.data()) != 1
        || EVP_DecryptFinal_ex(ctx.get(), raw(*plaintext) + written, &tail) != 1) {
        wipe(*plaintext);
        plaintext->clear();
        return false;
    }
    return true;
}

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

bool stretch(const QString &passphrase, const Kdf &kdf, QByteArray *key, Failure *failure)
{
    Kdf verified;
    if (!Kdf::fromJson(kdf.toJson(), &verified, failure))
        return false;
    if (!cryptoAvailable(failure))
        return false;
    KdfPtr algorithm(EVP_KDF_fetch(nullptr, "ARGON2ID", nullptr), &EVP_KDF_free);
    KdfCtxPtr ctx(algorithm ? EVP_KDF_CTX_new(algorithm.get()) : nullptr, &EVP_KDF_CTX_free);
    if (!ctx)
        return fail(failure, "argon2id_unavailable");
    QByteArray secret = passphrase.normalized(QString::NormalizationForm_KC).toUtf8();
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
    key->resize(kKeyBytes);
    const bool ok = EVP_KDF_derive(ctx.get(), raw(*key), kKeyBytes, params) == 1;
    wipe(secret);
    return ok ? true : fail(failure, "kdf_failed");
}

DeviceKeys::~DeviceKeys() { clear(); }

void DeviceKeys::clear()
{
    for (QByteArray *part : {&x25519, &mlkemSeed, &ed25519}) {
        wipe(*part);
        part->clear();
    }
}

bool DeviceKeys::isValid() const
{
    return x25519.size() == 32 && mlkemSeed.size() == 64 && ed25519.size() == 32;
}

QJsonObject DeviceKeys::toJson() const
{
    return {{QStringLiteral("x25519"), toBase64Url(x25519)},
            {QStringLiteral("mlkemSeed"), toBase64Url(mlkemSeed)},
            {QStringLiteral("ed25519"), toBase64Url(ed25519)}};
}

bool DeviceKeys::fromJson(const QJsonObject &json, DeviceKeys *keys, Failure *failure)
{
    DeviceKeys parsed;
    if (!fromBase64Url(json.value(QStringLiteral("x25519")).toString(), &parsed.x25519)
        || !fromBase64Url(json.value(QStringLiteral("mlkemSeed")).toString(), &parsed.mlkemSeed)
        || !fromBase64Url(json.value(QStringLiteral("ed25519")).toString(), &parsed.ed25519)
        || !parsed.isValid())
        return fail(failure, "invalid_device_keys");
    *keys = parsed;
    return true;
}

bool DeviceKeys::generate(DeviceKeys *keys, Failure *failure)
{
    if (!cryptoAvailable(failure))
        return false;
    DeviceKeys fresh;
    fresh.x25519 = randomBytes(32);
    fresh.mlkemSeed = randomBytes(64);
    fresh.ed25519 = randomBytes(32);
    if (!privateKey(EVP_PKEY_X25519, fresh.x25519) || !privateKey(EVP_PKEY_ED25519, fresh.ed25519)
        || !mlkemFromSeed(fresh.mlkemSeed))
        return fail(failure, "key_generation_failed");
    *keys = fresh;
    return true;
}

bool PublicKeys::isValid() const
{
    return x25519.size() == 32 && mlkem768.size() == kMlkemPublicBytes && ed25519.size() == 32;
}

QJsonObject PublicKeys::toJson() const
{
    return {{QStringLiteral("x25519"), toBase64Url(x25519)},
            {QStringLiteral("mlkem768"), toBase64Url(mlkem768)},
            {QStringLiteral("ed25519"), toBase64Url(ed25519)}};
}

bool PublicKeys::fromJson(const QJsonObject &json, PublicKeys *keys)
{
    PublicKeys parsed;
    if (!fromBase64Url(json.value(QStringLiteral("x25519")).toString(), &parsed.x25519)
        || !fromBase64Url(json.value(QStringLiteral("mlkem768")).toString(), &parsed.mlkem768)
        || !fromBase64Url(json.value(QStringLiteral("ed25519")).toString(), &parsed.ed25519)
        || !parsed.isValid() || !mlkemPublic(parsed.mlkem768)
        || !publicKey(EVP_PKEY_X25519, parsed.x25519) || !publicKey(EVP_PKEY_ED25519, parsed.ed25519))
        return false;
    *keys = parsed;
    return true;
}

bool publicKeys(const DeviceKeys &keys, PublicKeys *out, Failure *failure)
{
    if (!keys.isValid())
        return fail(failure, "invalid_device_keys");
    PkeyPtr x = privateKey(EVP_PKEY_X25519, keys.x25519);
    PkeyPtr ed = privateKey(EVP_PKEY_ED25519, keys.ed25519);
    PkeyPtr kem = mlkemFromSeed(keys.mlkemSeed);
    PublicKeys made;
    made.mlkem768.resize(kMlkemPublicBytes);
    size_t length = 0;
    if (!rawPublic(x.get(), &made.x25519) || !rawPublic(ed.get(), &made.ed25519) || !kem
        || EVP_PKEY_get_octet_string_param(kem.get(), OSSL_PKEY_PARAM_PUB_KEY, raw(made.mlkem768),
                                           size_t(made.mlkem768.size()), &length) != 1
        || length != size_t(kMlkemPublicBytes))
        return fail(failure, "invalid_device_keys");
    *out = made;
    return true;
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

bool verify(const QByteArray &ed25519Public, const QByteArray &message, const QByteArray &signature)
{
    PkeyPtr key = publicKey(EVP_PKEY_ED25519, ed25519Public);
    MdCtxPtr ctx(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    return key && ctx && signature.size() == 64
           && EVP_DigestVerifyInit(ctx.get(), nullptr, nullptr, nullptr, key.get()) == 1
           && EVP_DigestVerify(ctx.get(), raw(signature), size_t(signature.size()), raw(message),
                               size_t(message.size())) == 1;
}

bool sealToDevice(const PublicKeys &recipient, const QByteArray &label, const QByteArray &plaintext,
                  QByteArray *sealed, Failure *failure)
{
    if (!recipient.isValid())
        return fail(failure, "invalid_public_keys");
    PkeyPtr ephemeral(EVP_PKEY_Q_keygen(nullptr, nullptr, "X25519"), &EVP_PKEY_free);
    PkeyPtr theirs = publicKey(EVP_PKEY_X25519, recipient.x25519);
    QByteArray ctX, ssX;
    if (!rawPublic(ephemeral.get(), &ctX) || !x25519(ephemeral.get(), theirs.get(), &ssX))
        return fail(failure, "invalid_public_keys");
    PkeyPtr kem = mlkemPublic(recipient.mlkem768);
    PkeyCtxPtr ctx(kem ? EVP_PKEY_CTX_new_from_pkey(nullptr, kem.get(), nullptr) : nullptr,
                   &EVP_PKEY_CTX_free);
    size_t ctLength = 0, ssLength = 0;
    if (!ctx || EVP_PKEY_encapsulate_init(ctx.get(), nullptr) != 1
        || EVP_PKEY_encapsulate(ctx.get(), nullptr, &ctLength, nullptr, &ssLength) != 1
        || ctLength != size_t(kMlkemCiphertextBytes) || ssLength != 32)
        return fail(failure, "mlkem_unavailable");
    QByteArray ctM(int(ctLength), '\0'), ssM(int(ssLength), '\0');
    if (EVP_PKEY_encapsulate(ctx.get(), raw(ctM), &ctLength, raw(ssM), &ssLength) != 1)
        return fail(failure, "invalid_public_keys");
    QByteArray shared, key, body;
    const QByteArray nonce = randomBytes(12);
    const QByteArray aad = context(QStringLiteral("seal/")) + label + ctX + ctM;
    const bool ok = sha3(ssM + ssX + ctX + recipient.x25519 + kXWingLabel, &shared)
                    && hkdf(shared, QStringLiteral("seal/") + QString::fromUtf8(label), &key)
                    && aeadSeal(key, nonce, aad, plaintext, &body);
    wipe(ssM);
    wipe(ssX);
    wipe(shared);
    wipe(key);
    if (!ok)
        return fail(failure, "encryption_failed");
    *sealed = ctX + ctM + nonce + body;
    return true;
}

bool openFromDevice(const DeviceKeys &recipient, const QByteArray &label, const QByteArray &sealed,
                    QByteArray *plaintext, Failure *failure)
{
    if (!recipient.isValid())
        return fail(failure, "invalid_device_keys");
    if (sealed.size() < 32 + kMlkemCiphertextBytes + 12 + 16)
        return fail(failure, "invalid_ciphertext");
    const QByteArray ctX = sealed.left(32);
    const QByteArray ctM = sealed.mid(32, kMlkemCiphertextBytes);
    const QByteArray nonce = sealed.mid(32 + kMlkemCiphertextBytes, 12);
    const QByteArray body = sealed.mid(32 + kMlkemCiphertextBytes + 12);
    PkeyPtr mine = privateKey(EVP_PKEY_X25519, recipient.x25519);
    PkeyPtr theirs = publicKey(EVP_PKEY_X25519, ctX);
    QByteArray myX, ssX;
    if (!rawPublic(mine.get(), &myX) || !x25519(mine.get(), theirs.get(), &ssX))
        return fail(failure, "invalid_ciphertext");
    PkeyPtr kem = mlkemFromSeed(recipient.mlkemSeed);
    PkeyCtxPtr ctx(kem ? EVP_PKEY_CTX_new_from_pkey(nullptr, kem.get(), nullptr) : nullptr,
                   &EVP_PKEY_CTX_free);
    size_t ssLength = 32;
    QByteArray ssM(32, '\0');
    if (!ctx || EVP_PKEY_decapsulate_init(ctx.get(), nullptr) != 1
        || EVP_PKEY_decapsulate(ctx.get(), raw(ssM), &ssLength, raw(ctM), size_t(ctM.size())) != 1
        || ssLength != 32)
        return fail(failure, "invalid_ciphertext");
    QByteArray shared, key;
    const QByteArray aad = context(QStringLiteral("seal/")) + label + ctX + ctM;
    const bool derived = sha3(ssM + ssX + ctX + myX + kXWingLabel, &shared)
                         && hkdf(shared, QStringLiteral("seal/") + QString::fromUtf8(label), &key);
    wipe(ssM);
    wipe(ssX);
    wipe(shared);
    const bool opened = derived && aeadOpen(key, nonce, aad, body, plaintext);
    wipe(key);
    return opened ? true : fail(failure, "decryption_failed");
}

QString safetyNumber(const QByteArray &contextBytes, const PublicKeys &keys)
{
    const QByteArray digest = sha256(context(QStringLiteral("safety")) + contextBytes + keys.ed25519
                                     + keys.x25519 + keys.mlkem768);
    QStringList groups;
    for (int i = 0; i < 4; ++i) {
        quint32 value = 0;
        for (int j = 0; j < 4; ++j)
            value = (value << 8) | quint8(digest.at(i * 4 + j));
        groups << QStringLiteral("%1").arg(value % 100000, 5, 10, QLatin1Char('0'));
    }
    return groups.join(QLatin1Char(' '));
}

QString newInviteCode(QByteArray *secret)
{
    *secret = randomBytes(16);
    const QByteArray check = sha256(context(QStringLiteral("invite-check")) + *secret).left(1);
    const QString text = base32(*secret + check).left(27);
    QStringList groups;
    for (int i = 0; i < text.size(); i += 3)
        groups << text.mid(i, 3);
    return groups.join(QLatin1Char('-'));
}

bool parseInviteCode(const QString &code, QByteArray *secret)
{
    QString cleaned;
    for (const QChar c : code.toUpper()) {
        if (c == QLatin1Char('-') || c.isSpace())
            continue;
        if (c == QLatin1Char('O'))
            cleaned.append(QLatin1Char('0'));
        else if (c == QLatin1Char('I') || c == QLatin1Char('L'))
            cleaned.append(QLatin1Char('1'));
        else
            cleaned.append(c);
    }
    if (cleaned.size() != 27)
        return false;
    quint64 buffer = 0;
    int bits = 0;
    QByteArray bytes;
    for (const QChar c : cleaned) {
        const char latin = c.toLatin1();
        const char *found = latin ? strchr(kBase32, latin) : nullptr;
        if (!found)
            return false;
        buffer = (buffer << 5) | quint64(found - kBase32);
        bits += 5;
        if (bits >= 8) {
            bytes.append(char((buffer >> (bits - 8)) & 0xff));
            bits -= 8;
        }
    }
    if (bytes.size() != 16)
        return false;
    const QByteArray check = sha256(context(QStringLiteral("invite-check")) + bytes).left(1);
    const quint8 expected = quint8(check.at(0)) >> (8 - bits);
    if (quint8(buffer & ((1u << bits) - 1)) != expected)
        return false;
    *secret = bytes;
    return true;
}

qint64 paddedSize(qint64 length)
{
    const qint64 floor = 4096;
    if (length <= floor)
        return floor;
    int exponent = 0;
    while ((qint64(1) << (exponent + 1)) <= length)
        ++exponent;
    int bitsOfExponent = 0;
    while ((1 << bitsOfExponent) <= exponent)
        ++bitsOfExponent;
    const int lastBits = exponent - bitsOfExponent;
    const qint64 mask = (qint64(1) << lastBits) - 1;
    return (length + mask) & ~mask;
}

}  // namespace forkmesh::securedrive
