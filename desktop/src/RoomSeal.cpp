#include "RoomSeal.h"
#include "AppPaths.h"
#include "OwnerOnlyFile.h"
#include "SettingsWrite.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMessageAuthenticationCode>
#include <QMap>
#include <QMutex>
#include <QRegularExpression>
#include <QSettings>
#include <QTimer>

#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/params.h>
#include <openssl/rand.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>

namespace forkmesh::chatv3 {

namespace {

constexpr int kKeyBytes = 32;
constexpr int kNonceBytes = 12;
constexpr int kTagBytes = 16;
constexpr int kSignatureBytes = 64;
constexpr qsizetype kMaxPlainBytes = 96ll * 1024 * 1024;
constexpr int kMaxOpenRecords = 512;
constexpr int kMaxSeenSenders = 256;
constexpr int kCounterReserve = 64;
constexpr int kWriteDelayMs = 2000;
constexpr qint64 kAnnounceEveryMs = kDayMs;

using PkeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using PkeyCtxPtr = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;

const unsigned char *bytes(const QByteArray &data)
{
    return reinterpret_cast<const unsigned char *>(data.constData());
}

unsigned char *bytes(QByteArray &data)
{
    return reinterpret_cast<unsigned char *>(data.data());
}

const QRegularExpression &b64uPattern()
{
    static const QRegularExpression value(QStringLiteral("\\A[A-Za-z0-9_-]*\\z"));
    return value;
}

const QRegularExpression &keyIdPattern()
{
    static const QRegularExpression value(QStringLiteral("\\A[A-Za-z0-9_-]{16}\\z"));
    return value;
}

bool jsonInteger(const QJsonValue &value, qint64 *out)
{
    if (!value.isDouble())
        return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number ||
        std::fabs(number) > 9007199254740991.0)
        return false;
    if (out)
        *out = qint64(number);
    return true;
}

QString settingKey(const char *prefix, const QString &name)
{
    return QLatin1String(prefix) + b64u(name.toUtf8());
}

struct PendingWrites {
    QMutex mutex;
    QHash<QString, QVariant> values;
    bool scheduled = false;
    bool quitHooked = false;
};

PendingWrites &pendingWrites()
{
    static PendingWrites pending;
    return pending;
}

bool storedStateKey(const QString &key)
{
    return key.startsWith(QLatin1String("chat/e2ee/trustV3/")) ||
           key.startsWith(QLatin1String("chat/e2ee/ratchetV4/")) ||
           key.startsWith(QLatin1String("chat/e2ee/seenV3/"));
}

QString storedStatePath()
{
    if (forkmesh::paths::dataRoot().isEmpty())
        return QString();
    return forkmesh::paths::dataPath(QStringLiteral("chat-seal")) +
           QStringLiteral("/state.ini");
}

std::unique_ptr<QSettings> openStoredState()
{
    const QString path = storedStatePath();
    if (path.isEmpty() || !QDir().mkpath(QFileInfo(path).absolutePath()))
        return nullptr;
    if (QFileInfo::exists(path))
        forkmesh::ownerfile::restrictToOwner(path);
    else
        forkmesh::ownerfile::write(path, QByteArray());
    return std::make_unique<QSettings>(path, QSettings::IniFormat);
}

QString storedStateKeyPath(const QString &key)
{
    if (!storedStateKey(key))
        return QString();
    const QString state = storedStatePath();
    if (state.isEmpty())
        return QString();
    const QStringList parts = key.split(QLatin1Char('/'));
    if (parts.size() != 4 || parts.at(3).isEmpty() || parts.at(3).size() > 200 ||
        !b64uPattern().match(parts.at(3)).hasMatch())
        return QString();
    return QFileInfo(state).absolutePath() + QLatin1Char('/') + parts.at(2) +
           QLatin1Char('/') + parts.at(3);
}

bool writeStateFile(const QString &path, const QVariant &value)
{
    if (!value.isValid())
        return !QFileInfo::exists(path) || QFile::remove(path);
    return forkmesh::ownerfile::write(path, value.toByteArray());
}

bool readStateFile(const QString &path, QByteArray *out)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    *out = file.readAll();
    return file.error() == QFileDevice::NoError;
}

std::atomic_bool &legacyStateCleared()
{
    static std::atomic_bool cleared{false};
    return cleared;
}

void putValue(QSettings &settings, const QString &key, const QVariant &value)
{
    if (value.isValid())
        forkmesh::settings::put(settings, key, value);
    else
        settings.remove(key);
}

void storeValues(const QHash<QString, QVariant> &values)
{
    QHash<QString, QVariant> mainValues;
    QHash<QString, QVariant> iniValues;
    QStringList fileKeys;
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        if (!storedStateKey(it.key())) {
            mainValues.insert(it.key(), it.value());
            continue;
        }
        const QString path = storedStateKeyPath(it.key());
        if (!path.isEmpty() && writeStateFile(path, it.value())) {
            fileKeys.append(it.key());
            continue;
        }
        if (!path.isEmpty())
            QFile::remove(path);
        iniValues.insert(it.key(), it.value());
    }
    std::unique_ptr<QSettings> state =
        iniValues.isEmpty() && fileKeys.isEmpty() ? nullptr : openStoredState();
    bool iniSaved = false;
    if (state) {
        bool changed = !iniValues.isEmpty();
        for (const QString &key : std::as_const(fileKeys)) {
            if (state->contains(key)) {
                state->remove(key);
                changed = true;
            }
        }
        for (auto it = iniValues.cbegin(); it != iniValues.cend(); ++it)
            putValue(*state, it.key(), it.value());
        if (changed)
            state->sync();
        iniSaved = state->status() == QSettings::NoError;
    }
    QStringList movedKeys = fileKeys;
    if (iniSaved) {
        movedKeys += iniValues.keys();
    } else if (!iniValues.isEmpty()) {
        mainValues.insert(iniValues);
        legacyStateCleared().store(false);
    }
    const bool dropLegacy = !movedKeys.isEmpty() && !legacyStateCleared().load();
    if (mainValues.isEmpty() && !dropLegacy)
        return;
    QSettings settings;
    for (auto it = mainValues.cbegin(); it != mainValues.cend(); ++it)
        putValue(settings, it.key(), it.value());
    if (dropLegacy) {
        for (const QString &key : std::as_const(movedKeys))
            forkmesh::settings::drop(settings, key);
    }
}

void flushPendingWrites()
{
    QHash<QString, QVariant> values;
    {
        PendingWrites &pending = pendingWrites();
        const QMutexLocker lock(&pending.mutex);
        values.swap(pending.values);
        pending.scheduled = false;
    }
    if (values.isEmpty())
        return;
    storeValues(values);
}

void writeNow(const QString &key, const QVariant &value)
{
    {
        PendingWrites &pending = pendingWrites();
        const QMutexLocker lock(&pending.mutex);
        pending.values.remove(key);
    }
    storeValues({{key, value}});
}

void writeLater(const QString &key, const QVariant &value)
{
    QCoreApplication *app = QCoreApplication::instance();
    if (!app) {
        writeNow(key, value);
        return;
    }
    PendingWrites &pending = pendingWrites();
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
            PendingWrites &state = pendingWrites();
            bool hook = false;
            {
                const QMutexLocker lock(&state.mutex);
                hook = !state.quitHooked;
                state.quitHooked = true;
            }
            if (hook)
                QObject::connect(app, &QCoreApplication::aboutToQuit, app,
                                 &flushPendingWrites);
            QTimer::singleShot(kWriteDelayMs, app, &flushPendingWrites);
        },
        Qt::QueuedConnection);
}

QVariant readSetting(const QString &key)
{
    {
        PendingWrites &pending = pendingWrites();
        const QMutexLocker lock(&pending.mutex);
        const auto it = pending.values.constFind(key);
        if (it != pending.values.cend())
            return it.value();
    }
    if (storedStateKey(key)) {
        if (const QString path = storedStateKeyPath(key); !path.isEmpty()) {
            QByteArray data;
            if (readStateFile(path, &data))
                return data;
        }
        if (std::unique_ptr<QSettings> state = openStoredState();
            state && state->contains(key))
            return state->value(key);
    }
    return QSettings().value(key);
}

const char *const kStoredStateFamilies[] = {
    "chat/e2ee/trustV3", "chat/e2ee/ratchetV4", "chat/e2ee/seenV3"};

QByteArray compactJson(const QJsonObject &object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

bool trimHeld(QList<QJsonObject> *frames)
{
    qsizetype bytes = 0;
    qsizetype first = frames->size();
    while (first > 0 && frames->size() - first < kMaxHeldFrames) {
        const qsizetype size = compactJson(frames->at(first - 1)).size();
        if (first < frames->size() && bytes + size > kMaxHeldBytes)
            break;
        bytes += size;
        --first;
    }
    if (first == 0)
        return false;
    frames->remove(0, first);
    return true;
}

TrustState stateFromName(const QString &name)
{
    if (name == QLatin1String("approved"))
        return TrustState::Approved;
    if (name == QLatin1String("blocked"))
        return TrustState::Blocked;
    return TrustState::Pending;
}

QJsonObject recordJson(const TrustRecord &record)
{
    return {{QStringLiteral("kid"), record.kid},
            {QStringLiteral("idk"), record.idk},
            {QStringLiteral("acct"), record.acct},
            {QStringLiteral("node"), record.node},
            {QStringLiteral("spk"), record.spk},
            {QStringLiteral("state"), trustStateName(record.state)},
            {QStringLiteral("firstSeen"), double(record.firstSeen)},
            {QStringLiteral("lastSeen"), double(record.lastSeen)},
            {QStringLiteral("verified"), record.verified}};
}

TrustRecord recordFromJson(const QJsonObject &object)
{
    TrustRecord record;
    record.kid = object.value(QStringLiteral("kid")).toString();
    record.idk = object.value(QStringLiteral("idk")).toString();
    record.acct = object.value(QStringLiteral("acct")).toString();
    record.node = object.value(QStringLiteral("node")).toString();
    record.spk = object.value(QStringLiteral("spk")).toObject();
    record.state = stateFromName(object.value(QStringLiteral("state")).toString());
    record.firstSeen = qint64(object.value(QStringLiteral("firstSeen")).toDouble());
    record.lastSeen = qint64(object.value(QStringLiteral("lastSeen")).toDouble());
    record.verified = object.value(QStringLiteral("verified")).toBool();
    return record;
}

qint64 spkTs(const QJsonObject &spk)
{
    qint64 value = 0;
    jsonInteger(spk.value(QStringLiteral("ts")), &value);
    return value;
}

qint64 spkExp(const QJsonObject &spk)
{
    qint64 value = 0;
    jsonInteger(spk.value(QStringLiteral("exp")), &value);
    return value;
}

QHash<QByteArray, std::shared_ptr<ChatDevice>> &testDevices()
{
    static QHash<QByteArray, std::shared_ptr<ChatDevice>> devices;
    return devices;
}

} // namespace

QString b64u(const QByteArray &data)
{
    return QString::fromLatin1(
        data.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

QByteArray fromB64u(const QString &text, bool *ok)
{
    if (ok)
        *ok = false;
    if (!b64uPattern().match(text).hasMatch() || text.size() % 4 == 1)
        return {};
    auto result = QByteArray::fromBase64Encoding(
        text.toLatin1(), QByteArray::Base64UrlEncoding |
                             QByteArray::AbortOnBase64DecodingErrors);
    if (!result)
        return {};
    if (ok)
        *ok = true;
    return result.decoded;
}

QString b64(const QByteArray &data)
{
    return QString::fromLatin1(data.toBase64());
}

QByteArray fromB64(const QString &text, bool *ok)
{
    if (ok)
        *ok = false;
    if (text.size() % 4 != 0)
        return {};
    auto result = QByteArray::fromBase64Encoding(
        text.toLatin1(),
        QByteArray::Base64Encoding | QByteArray::AbortOnBase64DecodingErrors);
    if (!result)
        return {};
    if (ok)
        *ok = true;
    return result.decoded;
}

QByteArray sha256(const QByteArray &data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256);
}

QString id16(const QByteArray &data)
{
    return b64u(sha256(data)).left(16);
}

QByteArray randomBytes(int count)
{
    QByteArray output(count, Qt::Uninitialized);
    if (count > 0 && RAND_bytes(bytes(output), count) != 1)
        output.clear();
    return output;
}

void wipe(QByteArray &data)
{
    if (!data.isEmpty())
        data.fill('\0');
    data.clear();
}

QByteArray hkdfSha256(const QByteArray &ikm, const QByteArray &salt,
                      const QByteArray &info, int length)
{
    if (ikm.isEmpty() || length <= 0)
        return {};
    PkeyCtxPtr ctx(EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr), &EVP_PKEY_CTX_free);
    if (!ctx || EVP_PKEY_derive_init(ctx.get()) != 1 ||
        EVP_PKEY_CTX_set_hkdf_md(ctx.get(), EVP_sha256()) != 1 ||
        EVP_PKEY_CTX_set1_hkdf_salt(ctx.get(), bytes(salt), int(salt.size())) != 1 ||
        EVP_PKEY_CTX_set1_hkdf_key(ctx.get(), bytes(ikm), int(ikm.size())) != 1 ||
        EVP_PKEY_CTX_add1_hkdf_info(ctx.get(), bytes(info), int(info.size())) != 1)
        return {};
    QByteArray output(length, Qt::Uninitialized);
    size_t outputLength = size_t(length);
    if (EVP_PKEY_derive(ctx.get(), bytes(output), &outputLength) != 1 ||
        outputLength != size_t(length))
        return {};
    return output;
}

bool aesGcmSeal(const QByteArray &key, const QByteArray &nonce,
                const QByteArray &plain, const QByteArray &aad,
                QByteArray *cipher, QByteArray *tag)
{
    if (!cipher || !tag || key.size() != kKeyBytes || nonce.size() != kNonceBytes)
        return false;
    QByteArray output(plain.size() + kTagBytes, Qt::Uninitialized);
    QByteArray outputTag(kTagBytes, Qt::Uninitialized);
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int length = 0;
    int total = 0;
    bool ok = ctx &&
              EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
              EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kNonceBytes, nullptr) == 1 &&
              EVP_EncryptInit_ex(ctx, nullptr, nullptr, bytes(key), bytes(nonce)) == 1 &&
              EVP_EncryptUpdate(ctx, nullptr, &length, bytes(aad), int(aad.size())) == 1;
    if (ok && !plain.isEmpty()) {
        ok = EVP_EncryptUpdate(ctx, bytes(output), &length, bytes(plain),
                               int(plain.size())) == 1;
        total = length;
    }
    if (ok) {
        ok = EVP_EncryptFinal_ex(ctx, bytes(output) + total, &length) == 1 &&
             EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, kTagBytes,
                                 outputTag.data()) == 1;
        total += length;
    }
    if (ctx)
        EVP_CIPHER_CTX_free(ctx);
    if (!ok)
        return false;
    output.resize(total);
    *cipher = output;
    *tag = outputTag;
    return true;
}

bool aesGcmOpen(const QByteArray &key, const QByteArray &nonce,
                const QByteArray &cipher, const QByteArray &tag,
                const QByteArray &aad, QByteArray *plain)
{
    if (!plain || key.size() != kKeyBytes || nonce.size() != kNonceBytes ||
        tag.size() != kTagBytes)
        return false;
    QByteArray output(cipher.size() + kTagBytes, Qt::Uninitialized);
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int length = 0;
    int total = 0;
    bool ok = ctx &&
              EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
              EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kNonceBytes, nullptr) == 1 &&
              EVP_DecryptInit_ex(ctx, nullptr, nullptr, bytes(key), bytes(nonce)) == 1 &&
              EVP_DecryptUpdate(ctx, nullptr, &length, bytes(aad), int(aad.size())) == 1;
    if (ok && !cipher.isEmpty()) {
        ok = EVP_DecryptUpdate(ctx, bytes(output), &length, bytes(cipher),
                               int(cipher.size())) == 1;
        total = length;
    }
    if (ok) {
        ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, kTagBytes,
                                 const_cast<char *>(tag.constData())) == 1 &&
             EVP_DecryptFinal_ex(ctx, bytes(output) + total, &length) == 1;
        total += length;
    }
    if (ctx)
        EVP_CIPHER_CTX_free(ctx);
    if (!ok)
        return false;
    output.resize(total);
    *plain = output;
    return true;
}

QByteArray x25519Public(const QByteArray &privateKey)
{
    if (privateKey.size() != kKeyBytes)
        return {};
    PkeyPtr key(EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr,
                                             bytes(privateKey), kKeyBytes),
                &EVP_PKEY_free);
    if (!key)
        return {};
    QByteArray output(kKeyBytes, Qt::Uninitialized);
    size_t length = kKeyBytes;
    if (EVP_PKEY_get_raw_public_key(key.get(), bytes(output), &length) != 1 ||
        length != size_t(kKeyBytes))
        return {};
    return output;
}

QByteArray x25519Shared(const QByteArray &privateKey, const QByteArray &publicKey)
{
    if (privateKey.size() != kKeyBytes || publicKey.size() != kKeyBytes)
        return {};
    PkeyPtr mine(EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr,
                                              bytes(privateKey), kKeyBytes),
                 &EVP_PKEY_free);
    PkeyPtr peer(EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr,
                                             bytes(publicKey), kKeyBytes),
                 &EVP_PKEY_free);
    if (!mine || !peer)
        return {};
    PkeyCtxPtr ctx(EVP_PKEY_CTX_new(mine.get(), nullptr), &EVP_PKEY_CTX_free);
    QByteArray shared(kKeyBytes, Qt::Uninitialized);
    size_t length = kKeyBytes;
    if (!ctx || EVP_PKEY_derive_init(ctx.get()) != 1 ||
        EVP_PKEY_derive_set_peer(ctx.get(), peer.get()) != 1 ||
        EVP_PKEY_derive(ctx.get(), bytes(shared), &length) != 1 ||
        length != size_t(kKeyBytes))
        return {};
    if (shared == QByteArray(kKeyBytes, '\0'))
        return {};
    return shared;
}

QByteArray ed25519Public(const QByteArray &seed)
{
    if (seed.size() != kKeyBytes)
        return {};
    PkeyPtr key(EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, bytes(seed),
                                             kKeyBytes),
                &EVP_PKEY_free);
    if (!key)
        return {};
    QByteArray output(kKeyBytes, Qt::Uninitialized);
    size_t length = kKeyBytes;
    if (EVP_PKEY_get_raw_public_key(key.get(), bytes(output), &length) != 1 ||
        length != size_t(kKeyBytes))
        return {};
    return output;
}

QByteArray ed25519Sign(const QByteArray &seed, const QByteArray &message)
{
    if (seed.size() != kKeyBytes)
        return {};
    PkeyPtr key(EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, bytes(seed),
                                             kKeyBytes),
                &EVP_PKEY_free);
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    QByteArray signature(kSignatureBytes, Qt::Uninitialized);
    size_t length = kSignatureBytes;
    const bool ok = key && ctx &&
                    EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, key.get()) == 1 &&
                    EVP_DigestSign(ctx, bytes(signature), &length, bytes(message),
                                   size_t(message.size())) == 1 &&
                    length == size_t(kSignatureBytes);
    if (ctx)
        EVP_MD_CTX_free(ctx);
    return ok ? signature : QByteArray();
}

bool ed25519Verify(const QByteArray &publicKey, const QByteArray &signature,
                   const QByteArray &message)
{
    if (publicKey.size() != kKeyBytes || signature.size() != kSignatureBytes)
        return false;
    PkeyPtr key(EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
                                            bytes(publicKey), kKeyBytes),
                &EVP_PKEY_free);
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    const bool ok = key && ctx &&
                    EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, key.get()) == 1 &&
                    EVP_DigestVerify(ctx, bytes(signature), size_t(signature.size()),
                                     bytes(message), size_t(message.size())) == 1;
    if (ctx)
        EVP_MD_CTX_free(ctx);
    return ok;
}

namespace {

PkeyPtr mlkemFromSeed(const QByteArray &seed)
{
    PkeyPtr none(nullptr, &EVP_PKEY_free);
    if (seed.size() != kMlkemSeedBytes)
        return none;
    PkeyCtxPtr ctx(EVP_PKEY_CTX_new_from_name(nullptr, "ML-KEM-768", nullptr),
                   &EVP_PKEY_CTX_free);
    if (!ctx || EVP_PKEY_keygen_init(ctx.get()) != 1)
        return none;
    QByteArray copy = seed;
    OSSL_PARAM params[2] = {
        OSSL_PARAM_construct_octet_string(OSSL_PKEY_PARAM_ML_KEM_SEED, copy.data(),
                                          size_t(copy.size())),
        OSSL_PARAM_construct_end()};
    EVP_PKEY *key = nullptr;
    const bool ok = EVP_PKEY_CTX_set_params(ctx.get(), params) == 1 &&
                    EVP_PKEY_generate(ctx.get(), &key) == 1;
    wipe(copy);
    if (!ok) {
        if (key)
            EVP_PKEY_free(key);
        return none;
    }
    return PkeyPtr(key, &EVP_PKEY_free);
}

} // namespace

QByteArray mlkemPublic(const QByteArray &seed)
{
    PkeyPtr key = mlkemFromSeed(seed);
    if (!key)
        return {};
    QByteArray output(kMlkemPublicBytes, Qt::Uninitialized);
    size_t length = kMlkemPublicBytes;
    if (EVP_PKEY_get_raw_public_key(key.get(), bytes(output), &length) != 1 ||
        length != size_t(kMlkemPublicBytes))
        return {};
    return output;
}

bool mlkemEncapsulate(const QByteArray &publicKey, QByteArray *ciphertext,
                      QByteArray *shared)
{
    if (!ciphertext || !shared || publicKey.size() != kMlkemPublicBytes)
        return false;
    PkeyPtr peer(EVP_PKEY_new_raw_public_key_ex(nullptr, "ML-KEM-768", nullptr,
                                                bytes(publicKey),
                                                size_t(publicKey.size())),
                 &EVP_PKEY_free);
    if (!peer)
        return false;
    PkeyCtxPtr ctx(EVP_PKEY_CTX_new(peer.get(), nullptr), &EVP_PKEY_CTX_free);
    size_t ctLength = 0;
    size_t ssLength = 0;
    if (!ctx || EVP_PKEY_encapsulate_init(ctx.get(), nullptr) != 1 ||
        EVP_PKEY_encapsulate(ctx.get(), nullptr, &ctLength, nullptr, &ssLength) != 1)
        return false;
    QByteArray ct(qsizetype(ctLength), Qt::Uninitialized);
    QByteArray ss(qsizetype(ssLength), Qt::Uninitialized);
    if (EVP_PKEY_encapsulate(ctx.get(), bytes(ct), &ctLength, bytes(ss), &ssLength) != 1 ||
        ctLength != size_t(kMlkemCiphertextBytes) || ssLength != size_t(kKeyBytes))
        return false;
    *ciphertext = ct;
    *shared = ss;
    return true;
}

QByteArray mlkemDecapsulate(const QByteArray &seed, const QByteArray &ciphertext)
{
    if (ciphertext.size() != kMlkemCiphertextBytes)
        return {};
    PkeyPtr key = mlkemFromSeed(seed);
    if (!key)
        return {};
    PkeyCtxPtr ctx(EVP_PKEY_CTX_new(key.get(), nullptr), &EVP_PKEY_CTX_free);
    size_t length = 0;
    if (!ctx || EVP_PKEY_decapsulate_init(ctx.get(), nullptr) != 1 ||
        EVP_PKEY_decapsulate(ctx.get(), nullptr, &length, bytes(ciphertext),
                             size_t(ciphertext.size())) != 1)
        return {};
    QByteArray shared(qsizetype(length), Qt::Uninitialized);
    if (EVP_PKEY_decapsulate(ctx.get(), bytes(shared), &length, bytes(ciphertext),
                             size_t(ciphertext.size())) != 1 ||
        length != size_t(kKeyBytes))
        return {};
    return shared;
}

Prekey makePrekey(const QByteArray &xPriv, const QByteArray &pqSeed, qint64 ts)
{
    Prekey prekey;
    prekey.x = x25519Public(xPriv);
    prekey.pq = mlkemPublic(pqSeed);
    if (prekey.x.size() != kKeyBytes || prekey.pq.size() != kMlkemPublicBytes)
        return {};
    prekey.id = id16(prekey.x + prekey.pq);
    prekey.ts = ts;
    prekey.exp = ts + kPrekeyLifeMs;
    prekey.xPriv = xPriv;
    prekey.pqSeed = pqSeed;
    return prekey;
}

QJsonObject publicPrekey(const Prekey &prekey)
{
    return {{QStringLiteral("id"), prekey.id},
            {QStringLiteral("x"), b64u(prekey.x)},
            {QStringLiteral("pq"), b64u(prekey.pq)},
            {QStringLiteral("ts"), double(prekey.ts)},
            {QStringLiteral("exp"), double(prekey.exp)}};
}

bool validAcct(const QString &acct)
{
    const QList<uint> points = acct.toUcs4();
    if (points.isEmpty() || points.size() > 128)
        return false;
    for (const uint point : points) {
        if (point == ' ')
            continue;
        switch (QChar::category(char32_t(point))) {
        case QChar::Other_Control:
        case QChar::Other_Format:
        case QChar::Other_Surrogate:
        case QChar::Other_PrivateUse:
        case QChar::Other_NotAssigned:
        case QChar::Separator_Space:
        case QChar::Separator_Line:
        case QChar::Separator_Paragraph:
            return false;
        default:
            break;
        }
    }
    return true;
}

QByteArray bundleMessage(const QString &room, const QString &kid,
                         const QString &idk, const QString &acct,
                         const QString &node, const QJsonObject &spk)
{
    qint64 ts = 0;
    qint64 exp = 0;
    jsonInteger(spk.value(QStringLiteral("ts")), &ts);
    jsonInteger(spk.value(QStringLiteral("exp")), &exp);
    const QStringList lines{QStringLiteral("forkmesh-chat-e2ee-bundle-v3"),
                            room,
                            kid,
                            idk,
                            acct,
                            node,
                            spk.value(QStringLiteral("id")).toString(),
                            spk.value(QStringLiteral("x")).toString(),
                            spk.value(QStringLiteral("pq")).toString(),
                            QString::number(ts),
                            QString::number(exp)};
    return lines.join(QLatin1Char('\n')).toUtf8();
}

bool verifyBundle(const QJsonObject &bundle, const QString &room, qint64 now)
{
    if (bundle.value(QStringLiteral("type")).toString() != QLatin1String("e2ee-key"))
        return false;
    qint64 version = 0;
    if (!jsonInteger(bundle.value(QStringLiteral("v")), &version) || version != 3)
        return false;
    const QJsonValue kidValue = bundle.value(QStringLiteral("kid"));
    const QJsonValue idkValue = bundle.value(QStringLiteral("idk"));
    const QJsonValue acctValue = bundle.value(QStringLiteral("acct"));
    const QJsonValue nodeValue = bundle.value(QStringLiteral("node"));
    const QJsonValue spkValue = bundle.value(QStringLiteral("spk"));
    const QJsonValue sigValue = bundle.value(QStringLiteral("sig"));
    if (!kidValue.isString() || !idkValue.isString() || !acctValue.isString() ||
        !nodeValue.isString() || !spkValue.isObject() || !sigValue.isString())
        return false;
    const QJsonObject spk = spkValue.toObject();
    bool ok1 = false;
    bool ok2 = false;
    bool ok3 = false;
    bool ok4 = false;
    const QByteArray idk = fromB64u(idkValue.toString(), &ok1);
    const QByteArray x = fromB64u(spk.value(QStringLiteral("x")).toString(), &ok2);
    const QByteArray pq = fromB64u(spk.value(QStringLiteral("pq")).toString(), &ok3);
    const QByteArray sig = fromB64u(sigValue.toString(), &ok4);
    if (!ok1 || !ok2 || !ok3 || !ok4 || idk.size() != kKeyBytes ||
        x.size() != kKeyBytes || pq.size() != kMlkemPublicBytes ||
        sig.size() != kSignatureBytes)
        return false;
    const QString kid = kidValue.toString();
    const QString acct = acctValue.toString();
    const QString node = nodeValue.toString();
    if (kid != id16(idk) ||
        spk.value(QStringLiteral("id")).toString() != id16(x + pq) || !validAcct(acct) ||
        !validAcct(node))
        return false;
    qint64 ts = 0;
    qint64 exp = 0;
    if (!jsonInteger(spk.value(QStringLiteral("ts")), &ts) ||
        !jsonInteger(spk.value(QStringLiteral("exp")), &exp))
        return false;
    if (!(exp - ts > 0 && exp - ts <= 9 * kDayMs))
        return false;
    if (ts > now + kDayMs || exp <= now)
        return false;
    return ed25519Verify(idk, sig,
                         bundleMessage(room, kid, idkValue.toString(), acct, node, spk));
}

QByteArray nodeBindingMessage(const QString &room, const QString &kid,
                              const QString &idk, const QString &acct)
{
    return QStringList{QStringLiteral("forkmesh-chat-e2ee-node-v1"), room, kid, idk, acct}
        .join(QLatin1Char('\n'))
        .toUtf8();
}

bool isNodeKey(const QString &node)
{
    bool ok = false;
    const QByteArray raw = fromB64u(node, &ok);
    return ok && raw.size() == kKeyBytes && b64u(raw) == node;
}

bool verifyNodeBinding(const QJsonObject &bundle, const QString &room)
{
    const QString node = bundle.value(QStringLiteral("node")).toString();
    if (!isNodeKey(node))
        return false;
    bool ok = false;
    const QByteArray nsig = fromB64u(bundle.value(QStringLiteral("nsig")).toString(), &ok);
    if (!ok || nsig.size() != kSignatureBytes)
        return false;
    return ed25519Verify(fromB64u(node), nsig,
                         nodeBindingMessage(room,
                                            bundle.value(QStringLiteral("kid")).toString(),
                                            bundle.value(QStringLiteral("idk")).toString(),
                                            bundle.value(QStringLiteral("acct")).toString()));
}

QByteArray kek(const QString &room, const QString &sender,
               const QString &recipient, const QString &spkId,
               const QByteArray &ephPub, const QByteArray &ciphertext,
               const QByteArray &ss1, const QByteArray &ss2)
{
    if (ss1.isEmpty() || ss2.isEmpty())
        return {};
    const QByteArray salt =
        sha256(QByteArrayLiteral("ForkMesh chat e2ee salt v3:") + room.toUtf8());
    const QStringList info{QStringLiteral("forkmesh-chat-e2ee-kek-v3"),
                           room,
                           sender,
                           recipient,
                           spkId,
                           b64u(ephPub),
                           b64u(sha256(ciphertext))};
    QByteArray ikm = ss1 + ss2;
    const QByteArray key = hkdfSha256(ikm, salt, info.join(QLatin1Char('\n')).toUtf8());
    wipe(ikm);
    return key;
}

QByteArray contentAad(const QString &room, const QString &sender, qint64 ctr,
                      qint64 ts)
{
    return QStringList{QStringLiteral("forkmesh-chat-e2ee-content-v3"), room, sender,
                       QString::number(ctr), QString::number(ts)}
        .join(QLatin1Char('\n'))
        .toUtf8();
}

QByteArray wrapAad(const QString &room, const QString &sender,
                   const QString &recipient, const QString &nonce,
                   const QString &tag)
{
    return QStringList{QStringLiteral("forkmesh-chat-e2ee-wrap-v3"), room, sender,
                       recipient, nonce, tag}
        .join(QLatin1Char('\n'))
        .toUtf8();
}

QByteArray sealedMessage(const QString &room, const QJsonObject &frame)
{
    bool ok = false;
    const QByteArray body = fromB64(frame.value(QStringLiteral("body")).toString(), &ok);
    if (!ok)
        return {};
    qint64 ctr = 0;
    qint64 ts = 0;
    jsonInteger(frame.value(QStringLiteral("ctr")), &ctr);
    jsonInteger(frame.value(QStringLiteral("ts")), &ts);
    QString text = QStringList{QStringLiteral("forkmesh-chat-e2ee-sealed-v3"),
                               room,
                               frame.value(QStringLiteral("kid")).toString(),
                               frame.value(QStringLiteral("idk")).toString(),
                               QString::number(ctr),
                               QString::number(ts),
                               frame.value(QStringLiteral("nonce")).toString(),
                               frame.value(QStringLiteral("tag")).toString(),
                               b64u(sha256(body))}
                       .join(QLatin1Char('\n'));
    const QJsonArray keys = frame.value(QStringLiteral("keys")).toArray();
    for (const QJsonValue &value : keys) {
        const QJsonObject entry = value.toObject();
        const QByteArray ct = fromB64(entry.value(QStringLiteral("ct")).toString(), &ok);
        if (!ok)
            return {};
        text += QLatin1Char('\n') +
                QStringList{entry.value(QStringLiteral("kid")).toString(),
                            entry.value(QStringLiteral("spk")).toString(),
                            entry.value(QStringLiteral("eph")).toString(),
                            b64u(sha256(ct)),
                            entry.value(QStringLiteral("nonce")).toString(),
                            entry.value(QStringLiteral("tag")).toString(),
                            entry.value(QStringLiteral("key")).toString()}
                    .join(QLatin1Char('|'));
    }
    return text.toUtf8();
}

SafetyNumber safetyNumber(const QString &scope, QList<QByteArray> idks)
{
    std::sort(idks.begin(), idks.end());
    QByteArray keys;
    for (const QByteArray &idk : std::as_const(idks))
        keys += idk;
    QByteArray h = QCryptographicHash::hash(
        QByteArrayLiteral("forkmesh-safety-v3\n") + scope.toUtf8() + "\n" + keys,
        QCryptographicHash::Sha512);
    for (int round = 0; round < 5199; ++round)
        h = QCryptographicHash::hash(h + keys, QCryptographicHash::Sha512);
    SafetyNumber out;
    for (int offset = 0; offset < 60; offset += 5) {
        quint64 value = 0;
        for (int i = 0; i < 5; ++i)
            value = (value << 8) | quint8(h.at(offset + i));
        out.digits += QStringLiteral("%1").arg(value % 100000, 5, 10, QLatin1Char('0'));
    }
    out.qr = QStringLiteral("forkmesh-safety:3:%1:%2").arg(scope, b64u(h.left(32)));
    return out;
}

QStringList safetyGroups(const QString &digits)
{
    QStringList groups;
    for (qsizetype i = 0; i + 5 <= digits.size(); i += 5)
        groups.append(digits.mid(i, 5));
    return groups;
}

QJsonObject linkHello(const QByteArray &idk, const QByteArray &ephPub,
                      const QByteArray &nonce)
{
    return {{QStringLiteral("t"), QStringLiteral("fm-link-hello")},
            {QStringLiteral("v"), 3},
            {QStringLiteral("idk"), b64u(idk)},
            {QStringLiteral("eph"), b64u(ephPub)},
            {QStringLiteral("nonce"), b64u(nonce)}};
}

bool isLinkHello(const QJsonObject &frame)
{
    if (frame.value(QStringLiteral("t")).toString() != QLatin1String("fm-link-hello"))
        return false;
    qint64 version = 0;
    if (!jsonInteger(frame.value(QStringLiteral("v")), &version) || version != 3)
        return false;
    bool ok1 = false;
    bool ok2 = false;
    bool ok3 = false;
    const QByteArray idk = fromB64u(frame.value(QStringLiteral("idk")).toString(), &ok1);
    const QByteArray eph = fromB64u(frame.value(QStringLiteral("eph")).toString(), &ok2);
    const QByteArray nonce =
        fromB64u(frame.value(QStringLiteral("nonce")).toString(), &ok3);
    return ok1 && ok2 && ok3 && idk.size() == kKeyBytes && eph.size() == kKeyBytes &&
           nonce.size() == 16;
}

QByteArray linkTranscript(const QJsonObject &initiatorHello,
                          const QJsonObject &responderHello)
{
    const QStringList parts{
        QStringLiteral("forkmesh-mesh-link-v3"),
        initiatorHello.value(QStringLiteral("idk")).toString(),
        initiatorHello.value(QStringLiteral("eph")).toString(),
        initiatorHello.value(QStringLiteral("nonce")).toString(),
        responderHello.value(QStringLiteral("idk")).toString(),
        responderHello.value(QStringLiteral("eph")).toString(),
        responderHello.value(QStringLiteral("nonce")).toString()};
    return sha256(parts.join(QLatin1Char('\n')).toUtf8());
}

QByteArray linkAuthMessage(const QString &role, const QByteArray &transcript)
{
    return (QStringLiteral("forkmesh-mesh-link-auth-v3\n") + role +
            QLatin1Char('\n') + b64u(transcript))
        .toUtf8();
}

bool linkKeys(const QByteArray &ownEphPriv, const QByteArray &peerEphPub,
              const QByteArray &transcript, QByteArray *i2r, QByteArray *r2i)
{
    if (!i2r || !r2i)
        return false;
    QByteArray shared = x25519Shared(ownEphPriv, peerEphPub);
    if (shared.isEmpty())
        return false;
    *i2r = hkdfSha256(shared, transcript, QByteArrayLiteral("forkmesh-mesh-link-v3 i2r"));
    *r2i = hkdfSha256(shared, transcript, QByteArrayLiteral("forkmesh-mesh-link-v3 r2i"));
    wipe(shared);
    return i2r->size() == kKeyBytes && r2i->size() == kKeyBytes;
}

QByteArray linkNonce(quint64 counter)
{
    QByteArray nonce(kNonceBytes, '\0');
    for (int i = 0; i < 8; ++i)
        nonce[4 + i] = char((counter >> (8 * (7 - i))) & 0xff);
    return nonce;
}

QByteArray linkSeal(const QByteArray &key, quint64 counter, const QByteArray &plain)
{
    QByteArray cipher;
    QByteArray tag;
    if (!aesGcmSeal(key, linkNonce(counter), plain,
                    QByteArrayLiteral("forkmesh-mesh-link-v3"), &cipher, &tag))
        return {};
    return cipher + tag;
}

bool linkOpen(const QByteArray &key, quint64 counter, const QByteArray &data,
              QByteArray *plain)
{
    if (data.size() < kTagBytes)
        return false;
    return aesGcmOpen(key, linkNonce(counter), data.left(data.size() - kTagBytes),
                      data.right(kTagBytes), QByteArrayLiteral("forkmesh-mesh-link-v3"),
                      plain);
}

QString trustStateName(TrustState state)
{
    switch (state) {
    case TrustState::Approved:
        return QStringLiteral("approved");
    case TrustState::Blocked:
        return QStringLiteral("blocked");
    case TrustState::Pending:
        break;
    }
    return QStringLiteral("pending");
}

ChatDevice::ChatDevice(bool persistent)
    : m_persistent(persistent)
{
}

ChatDevice::~ChatDevice()
{
    wipe(m_seed);
    for (Prekey &prekey : m_prekeys) {
        wipe(prekey.xPriv);
        wipe(prekey.pqSeed);
    }
}

QString storedStateFile()
{
    return storedStatePath();
}

QString storedStateKeyFile(const QString &key)
{
    return storedStateKeyPath(key);
}

static bool moveStoredStateToFiles(QSettings &state)
{
    QStringList keys;
    for (const char *family : kStoredStateFamilies) {
        state.beginGroup(QLatin1String(family));
        for (const QString &child : state.childKeys())
            keys.append(QLatin1String(family) + QLatin1Char('/') + child);
        state.endGroup();
    }
    if (keys.isEmpty())
        return true;
    bool moved = true;
    QStringList done;
    for (const QString &key : std::as_const(keys)) {
        const QString path = storedStateKeyPath(key);
        if (path.isEmpty())
            continue;
        const QByteArray value = state.value(key).toByteArray();
        QByteArray written;
        if (!writeStateFile(path, value) || !readStateFile(path, &written) ||
            written != value) {
            QFile::remove(path);
            moved = false;
            continue;
        }
        done.append(key);
    }
    for (const QString &key : std::as_const(done))
        state.remove(key);
    state.sync();
    return moved && state.status() == QSettings::NoError;
}

bool migrateStoredState()
{
    flushPendingWrites();
    std::unique_ptr<QSettings> state = openStoredState();
    if (!state)
        return false;
    QSettings settings;
    QStringList keys;
    for (const char *family : kStoredStateFamilies) {
        settings.beginGroup(QLatin1String(family));
        for (const QString &child : settings.childKeys())
            keys.append(QLatin1String(family) + QLatin1Char('/') + child);
        settings.endGroup();
    }
    QSet<QString> copied;
    for (const QString &key : std::as_const(keys)) {
        if (state->contains(key))
            continue;
        state->setValue(key, settings.value(key));
        copied.insert(key);
    }
    state->sync();
    if (state->status() != QSettings::NoError)
        return false;
    const QSettings written(state->fileName(), QSettings::IniFormat);
    for (const QString &key : std::as_const(keys)) {
        if (!written.contains(key) ||
            (copied.contains(key) && written.value(key) != settings.value(key)))
            return false;
    }
    for (const QString &key : std::as_const(keys))
        settings.remove(key);
    settings.sync();
    legacyStateCleared().store(settings.status() == QSettings::NoError);
    const bool filesMoved = moveStoredStateToFiles(*state);
    return legacyStateCleared().load() && filesMoved;
}

QVariant storedSetting(const QString &key)
{
    return readSetting(key);
}

void clearStoredState()
{
    {
        PendingWrites &pending = pendingWrites();
        const QMutexLocker lock(&pending.mutex);
        pending.values.clear();
    }
    QSettings().remove(QStringLiteral("chat/e2ee"));
    if (std::unique_ptr<QSettings> state = openStoredState())
        state->remove(QStringLiteral("chat/e2ee"));
    const QString state = storedStatePath();
    if (state.isEmpty())
        return;
    for (const char *family : kStoredStateFamilies) {
        const QDir dir(QFileInfo(state).absolutePath() + QLatin1Char('/') +
                       QString::fromLatin1(family).section(QLatin1Char('/'), -1));
        for (const QString &name : dir.entryList(QDir::Files | QDir::Hidden))
            QFile::remove(dir.filePath(name));
    }
}

std::shared_ptr<ChatDevice> ChatDevice::local()
{
    static std::shared_ptr<ChatDevice> device;
    if (!device) {
        migrateStoredState();
        device.reset(new ChatDevice(true));
        if (device->loadIdentity()) {
            device->loadPrekeys();
            device->maintainPrekeys();
        }
    }
    return device;
}

std::shared_ptr<ChatDevice> ChatDevice::inMemory(const QByteArray &identitySeed)
{
    std::shared_ptr<ChatDevice> device(new ChatDevice(false));
    device->setIdentity(identitySeed.size() == kKeyBytes ? identitySeed
                                                          : randomBytes(kKeyBytes));
    return device;
}

std::shared_ptr<ChatDevice> ChatDevice::sharedForTests(const QByteArray &identitySeed)
{
    auto &devices = testDevices();
    auto it = devices.find(identitySeed);
    if (it == devices.end())
        it = devices.insert(identitySeed, inMemory(identitySeed));
    return it.value();
}

bool ChatDevice::loadIdentity()
{
    const QString key = QStringLiteral("chat/e2ee/ed25519IdentityV3");
    bool ok = false;
    QByteArray seed = fromB64(QSettings().value(key).toString(), &ok);
    if (!ok || seed.size() != kKeyBytes || ed25519Public(seed).isEmpty()) {
        seed = randomBytes(kKeyBytes);
        if (seed.size() != kKeyBytes)
            return false;
        writeNow(key, b64(seed));
    }
    setIdentity(seed);
    wipe(seed);
    return isValid();
}

void ChatDevice::setIdentity(const QByteArray &seed)
{
    m_seed = seed;
    m_idk = ed25519Public(seed);
    m_kid = m_idk.size() == kKeyBytes ? id16(m_idk) : QString();
}

QByteArray ChatDevice::sign(const QByteArray &message) const
{
    return ed25519Sign(m_seed, message);
}

qint64 ChatDevice::now() const
{
    return m_clock ? m_clock() : QDateTime::currentMSecsSinceEpoch();
}

void ChatDevice::setClock(std::function<qint64()> clock)
{
    m_clock = std::move(clock);
}

void ChatDevice::loadPrekeys()
{
    const QJsonArray entries =
        QJsonDocument::fromJson(
            readSetting(QStringLiteral("chat/e2ee/prekeysV3")).toByteArray())
            .array();
    for (const QJsonValue &value : entries) {
        const QJsonObject entry = value.toObject();
        Prekey prekey;
        prekey.id = entry.value(QStringLiteral("id")).toString();
        prekey.x = fromB64u(entry.value(QStringLiteral("x")).toString());
        prekey.pq = fromB64u(entry.value(QStringLiteral("pq")).toString());
        prekey.ts = qint64(entry.value(QStringLiteral("ts")).toDouble());
        prekey.exp = qint64(entry.value(QStringLiteral("exp")).toDouble());
        prekey.xPriv = fromB64(entry.value(QStringLiteral("xPriv")).toString());
        prekey.pqSeed = fromB64(entry.value(QStringLiteral("pqSeed")).toString());
        if (prekey.xPriv.size() != kKeyBytes || prekey.pqSeed.size() != kMlkemSeedBytes ||
            prekey.x.size() != kKeyBytes || prekey.pq.size() != kMlkemPublicBytes ||
            prekey.id != id16(prekey.x + prekey.pq))
            continue;
        m_prekeys.append(prekey);
    }
}

void ChatDevice::savePrekeys()
{
    if (!m_persistent)
        return;
    QJsonArray entries;
    for (const Prekey &prekey : std::as_const(m_prekeys))
        entries.append(QJsonObject{{QStringLiteral("id"), prekey.id},
                                   {QStringLiteral("x"), b64u(prekey.x)},
                                   {QStringLiteral("pq"), b64u(prekey.pq)},
                                   {QStringLiteral("ts"), double(prekey.ts)},
                                   {QStringLiteral("exp"), double(prekey.exp)},
                                   {QStringLiteral("xPriv"), b64(prekey.xPriv)},
                                   {QStringLiteral("pqSeed"), b64(prekey.pqSeed)}});
    writeNow(QStringLiteral("chat/e2ee/prekeysV3"),
             QJsonDocument(entries).toJson(QJsonDocument::Compact));
}

void ChatDevice::addPrekey(const Prekey &prekey)
{
    if (prekey.id.isEmpty())
        return;
    m_prekeys.append(prekey);
    std::stable_sort(m_prekeys.begin(), m_prekeys.end(),
                     [](const Prekey &a, const Prekey &b) { return a.ts < b.ts; });
    while (m_prekeys.size() > kMaxPrekeys) {
        Prekey dropped = m_prekeys.takeFirst();
        wipe(dropped.xPriv);
        wipe(dropped.pqSeed);
    }
}

const Prekey *ChatDevice::newestPrekey() const
{
    const Prekey *newest = nullptr;
    for (const Prekey &prekey : m_prekeys)
        if (!newest || prekey.ts > newest->ts)
            newest = &prekey;
    return newest;
}

const Prekey *ChatDevice::findPrekey(const QString &id) const
{
    for (const Prekey &prekey : m_prekeys)
        if (prekey.id == id)
            return &prekey;
    return nullptr;
}

void ChatDevice::wipeExpired(qint64 now)
{
    for (qsizetype i = m_prekeys.size() - 1; i >= 0; --i) {
        if (now <= m_prekeys.at(i).exp)
            continue;
        Prekey dropped = m_prekeys.takeAt(i);
        wipe(dropped.xPriv);
        wipe(dropped.pqSeed);
    }
}

bool ChatDevice::maintainPrekeys()
{
    if (!isValid())
        return false;
    const qint64 current = now();
    const qsizetype before = m_prekeys.size();
    wipeExpired(current);
    const Prekey *newest = newestPrekey();
    bool rotated = false;
    if (!newest || current - newest->ts >= kPrekeyRotateMs) {
        QByteArray xPriv = randomBytes(kKeyBytes);
        QByteArray pqSeed = randomBytes(kMlkemSeedBytes);
        const Prekey made = makePrekey(xPriv, pqSeed, current);
        wipe(xPriv);
        wipe(pqSeed);
        if (!made.id.isEmpty()) {
            addPrekey(made);
            rotated = true;
        }
    }
    if (rotated || m_prekeys.size() != before)
        savePrekeys();
    if (rotated)
        notify(Event::PrekeyRotated, QString(), m_kid);
    return rotated;
}

ChatDevice::Scope &ChatDevice::scopeState(const QString &scope)
{
    Scope &state = m_scopes[scope];
    if (state.loaded)
        return state;
    state.loaded = true;
    if (!m_persistent)
        return state;
    const QJsonObject root =
        QJsonDocument::fromJson(
            readSetting(settingKey("chat/e2ee/trustV3/", scope)).toByteArray())
            .object();
    if (root.contains(QStringLiteral("policy"))) {
        state.policyKnown = true;
        state.policy = root.value(QStringLiteral("policy")).toString() ==
                               QLatin1String("strict")
                           ? Policy::Strict
                           : Policy::Open;
    }
    for (const QJsonValue &value : root.value(QStringLiteral("records")).toArray()) {
        const TrustRecord record = recordFromJson(value.toObject());
        if (keyIdPattern().match(record.kid).hasMatch())
            state.records.insert(record.kid, record);
    }
    const QJsonObject held = root.value(QStringLiteral("held")).toObject();
    bool trimmed = false;
    for (auto it = held.constBegin(); it != held.constEnd(); ++it) {
        QList<QJsonObject> frames;
        for (const QJsonValue &frame : it.value().toArray())
            frames.append(frame.toObject());
        trimmed = trimHeld(&frames) || trimmed;
        state.held.insert(it.key(), frames);
    }
    state.safety = root.value(QStringLiteral("safety")).toString();
    if (trimmed)
        saveScope(scope, false);
    return state;
}

void ChatDevice::saveScope(const QString &scope, bool immediately)
{
    if (!m_persistent)
        return;
    const Scope &state = m_scopes[scope];
    QJsonArray records;
    for (const TrustRecord &record : state.records)
        records.append(recordJson(record));
    QJsonObject held;
    for (auto it = state.held.constBegin(); it != state.held.constEnd(); ++it) {
        if (it.value().isEmpty())
            continue;
        QJsonArray frames;
        for (const QJsonObject &frame : it.value())
            frames.append(frame);
        held.insert(it.key(), frames);
    }
    const QJsonObject root{
        {QStringLiteral("policy"),
         state.policy == Policy::Strict ? QStringLiteral("strict") : QStringLiteral("open")},
        {QStringLiteral("records"), records},
        {QStringLiteral("held"), held},
        {QStringLiteral("safety"), state.safety}};
    const QString key = settingKey("chat/e2ee/trustV3/", scope);
    const QByteArray value = QJsonDocument(root).toJson(QJsonDocument::Compact);
    if (immediately)
        writeNow(key, value);
    else
        writeLater(key, value);
}

Policy ChatDevice::policy(const QString &scope)
{
    return scopeState(scope).policy;
}

void ChatDevice::setPolicy(const QString &scope, Policy policy)
{
    Scope &state = scopeState(scope);
    if (state.policyKnown && state.policy == policy)
        return;
    state.policy = policy;
    state.policyKnown = true;
    saveScope(scope, false);
}

QList<TrustRecord> ChatDevice::records(const QString &scope)
{
    QList<TrustRecord> out = scopeState(scope).records.values();
    std::sort(out.begin(), out.end(), [](const TrustRecord &a, const TrustRecord &b) {
        return a.kid < b.kid;
    });
    return out;
}

const TrustRecord *ChatDevice::record(const QString &scope, const QString &kid)
{
    Scope &state = scopeState(scope);
    const auto it = state.records.constFind(kid);
    return it == state.records.constEnd() ? nullptr : &it.value();
}

void ChatDevice::ensureOwnRecord(const QString &scope, const QString &acct,
                                 const QString &node)
{
    const Prekey *newest = newestPrekey();
    if (!isValid() || !newest || !validAcct(acct) || !validAcct(node))
        return;
    Scope &state = scopeState(scope);
    const QJsonObject spk = publicPrekey(*newest);
    auto it = state.records.find(m_kid);
    if (it != state.records.end() && it->acct == acct && it->node == node &&
        it->idk == idkText() &&
        it->state == TrustState::Approved && spkTs(it->spk) >= newest->ts)
        return;
    const bool fresh = it == state.records.end();
    TrustRecord record = fresh ? TrustRecord() : it.value();
    record.kid = m_kid;
    record.idk = idkText();
    record.acct = acct;
    record.node = node;
    record.spk = spk;
    record.state = TrustState::Approved;
    if (fresh)
        record.firstSeen = now();
    state.records.insert(m_kid, record);
    saveScope(scope, false);
    if (fresh)
        refreshSafety(scope);
}

ChatDevice::BundleResult ChatDevice::acceptBundle(const QString &scope, Policy policy,
                                                  const QString &room,
                                                  const QJsonObject &bundle,
                                                  bool *changed,
                                                  const QStringList &participants)
{
    if (changed)
        *changed = false;
    const qint64 current = now();
    if (!verifyBundle(bundle, room, current))
        return BundleResult::Rejected;
    const QString acct = bundle.value(QStringLiteral("acct")).toString();
    if (!participants.isEmpty() && !participants.contains(acct))
        return BundleResult::Rejected;
    Scope &state = scopeState(scope);
    const QString kid = bundle.value(QStringLiteral("kid")).toString();
    const QString idk = bundle.value(QStringLiteral("idk")).toString();
    const QString node = bundle.value(QStringLiteral("node")).toString();
    const QJsonObject spk = bundle.value(QStringLiteral("spk")).toObject();
    auto known = state.records.find(kid);
    if (known != state.records.end()) {
        if (known->idk != idk || known->acct != acct || known->node != node)
            return BundleResult::Rejected;
        known->lastSeen = current;
        if (spkTs(spk) > spkTs(known->spk)) {
            known->spk = spk;
            if (changed)
                *changed = true;
            saveScope(scope, false);
        }
        return BundleResult::Known;
    }
    TrustState trust = TrustState::Approved;
    if (policy == Policy::Strict && kid != m_kid) {
        for (const TrustRecord &record : std::as_const(state.records)) {
            if (record.acct == acct && record.state == TrustState::Approved) {
                trust = TrustState::Pending;
                break;
            }
        }
    }
    TrustRecord record;
    record.kid = kid;
    record.idk = idk;
    record.acct = acct;
    record.node = node;
    record.spk = spk;
    record.state = trust;
    record.firstSeen = current;
    record.lastSeen = current;
    state.records.insert(kid, record);
    if (policy == Policy::Open && state.records.size() > kMaxOpenRecords) {
        QList<TrustRecord> ordered = state.records.values();
        std::sort(ordered.begin(), ordered.end(),
                  [](const TrustRecord &a, const TrustRecord &b) {
                      return a.lastSeen < b.lastSeen;
                  });
        for (const TrustRecord &old : std::as_const(ordered)) {
            if (state.records.size() <= kMaxOpenRecords)
                break;
            if (old.kid != m_kid && old.kid != kid)
                state.records.remove(old.kid);
        }
    }
    if (changed)
        *changed = true;
    saveScope(scope, false);
    if (trust == TrustState::Approved)
        refreshSafety(scope);
    notify(trust == TrustState::Pending ? Event::DevicePending : Event::TrustChanged,
           scope, kid);
    if (trust == TrustState::Pending)
        notify(Event::TrustChanged, scope, kid);
    return trust == TrustState::Pending ? BundleResult::Pending : BundleResult::Approved;
}

void ChatDevice::touch(const QString &scope, const QString &kid)
{
    Scope &state = scopeState(scope);
    auto it = state.records.find(kid);
    if (it != state.records.end())
        it->lastSeen = now();
}

void ChatDevice::setState(const QString &scope, const QString &kid, TrustState trust)
{
    Scope &state = scopeState(scope);
    auto it = state.records.find(kid);
    if (it == state.records.end() || it->state == trust || kid == m_kid)
        return;
    it->state = trust;
    saveScope(scope, true);
    refreshSafety(scope);
    notify(Event::TrustChanged, scope, kid);
}

void ChatDevice::refreshSafety(const QString &scope)
{
    Scope &state = scopeState(scope);
    if (state.policy != Policy::Strict)
        return;
    QList<QByteArray> idks;
    QSet<QString> accounts;
    for (const TrustRecord &record : std::as_const(state.records)) {
        if (record.state != TrustState::Approved)
            continue;
        idks.append(fromB64u(record.idk));
        accounts.insert(record.acct);
    }
    const QString digits =
        accounts.size() >= 2 ? safetyNumber(scope, idks).digits : QString();
    if (digits == state.safety)
        return;
    const bool changed = !state.safety.isEmpty() && !digits.isEmpty();
    state.safety = digits;
    if (changed)
        for (TrustRecord &record : state.records)
            record.verified = false;
    saveScope(scope, false);
    if (changed)
        notify(Event::SafetyNumberChanged, scope, QString());
}

void ChatDevice::markVerified(const QString &scope)
{
    Scope &state = scopeState(scope);
    for (TrustRecord &record : state.records)
        if (record.state == TrustState::Approved)
            record.verified = true;
    saveScope(scope, true);
    notify(Event::TrustChanged, scope, QString());
}

bool ChatDevice::isVerified(const QString &scope)
{
    Scope &state = scopeState(scope);
    bool any = false;
    for (const TrustRecord &record : std::as_const(state.records)) {
        if (record.state != TrustState::Approved || record.kid == m_kid)
            continue;
        if (!record.verified)
            return false;
        any = true;
    }
    return any;
}

bool ChatDevice::isBlockedAnywhere(const QString &kid)
{
    for (auto it = m_scopes.begin(); it != m_scopes.end(); ++it) {
        const auto record = it->records.constFind(kid);
        if (record != it->records.constEnd() && record->state == TrustState::Blocked)
            return true;
    }
    return false;
}

SafetyNumber ChatDevice::safetyNumberFor(const QString &scope)
{
    QList<QByteArray> idks;
    for (const TrustRecord &record : records(scope))
        if (record.state == TrustState::Approved)
            idks.append(fromB64u(record.idk));
    return safetyNumber(scope, idks);
}

ChatDevice::Room &ChatDevice::roomState(const QString &room)
{
    Room &state = m_rooms[room];
    if (state.loaded)
        return state;
    state.loaded = true;
    if (m_persistent) {
        state.reserved = readSetting(settingKey("chat/e2ee/ctrV3/", room)).toLongLong();
        state.counter = state.reserved;
    }
    return state;
}

qint64 ChatDevice::nextCounter(const QString &room)
{
    Room &state = roomState(room);
    ++state.counter;
    if (m_persistent && state.counter > state.reserved) {
        state.reserved = state.counter + kCounterReserve - 1;
        writeNow(settingKey("chat/e2ee/ctrV3/", room), state.reserved);
    }
    return state.counter;
}

void ChatDevice::loadSeen(const QString &room, Room &state)
{
    if (state.seenLoaded)
        return;
    state.seenLoaded = true;
    if (!m_persistent)
        return;
    const QJsonObject root =
        QJsonDocument::fromJson(
            readSetting(settingKey("chat/e2ee/seenV3/", room)).toByteArray())
            .object();
    for (auto it = root.constBegin(); it != root.constEnd(); ++it) {
        const QJsonObject entry = it.value().toObject();
        Seen seen;
        seen.max = qint64(entry.value(QStringLiteral("max")).toDouble());
        seen.at = qint64(entry.value(QStringLiteral("at")).toDouble());
        for (const QJsonValue &value : entry.value(QStringLiteral("set")).toArray())
            seen.set.insert(qint64(value.toDouble()));
        state.seen.insert(it.key(), seen);
    }
}

void ChatDevice::saveSeen(const QString &room)
{
    if (!m_persistent)
        return;
    const Room &state = m_rooms[room];
    QJsonObject root;
    for (auto it = state.seen.constBegin(); it != state.seen.constEnd(); ++it) {
        QList<qint64> counters = it->set.values();
        std::sort(counters.begin(), counters.end());
        QJsonArray set;
        for (const qint64 counter : std::as_const(counters))
            set.append(double(counter));
        root.insert(it.key(), QJsonObject{{QStringLiteral("max"), double(it->max)},
                                          {QStringLiteral("at"), double(it->at)},
                                          {QStringLiteral("set"), set}});
    }
    writeLater(settingKey("chat/e2ee/seenV3/", room),
               QJsonDocument(root).toJson(QJsonDocument::Compact));
}

bool ChatDevice::isReplay(const QString &room, const QString &kid, qint64 ctr)
{
    Room &state = roomState(room);
    loadSeen(room, state);
    const auto it = state.seen.constFind(kid);
    if (it == state.seen.constEnd())
        return false;
    return ctr <= it->max - kReplayWindow || it->set.contains(ctr);
}

qint64 ChatDevice::seenMax(const QString &room, const QString &kid)
{
    Room &state = roomState(room);
    loadSeen(room, state);
    const auto it = state.seen.constFind(kid);
    return it == state.seen.constEnd() ? 0 : it->max;
}

void ChatDevice::markSeen(const QString &room, const QString &kid, qint64 ctr)
{
    Room &state = roomState(room);
    loadSeen(room, state);
    Seen &seen = state.seen[kid];
    seen.set.insert(ctr);
    seen.max = qMax(seen.max, ctr);
    seen.at = now();
    for (auto it = seen.set.begin(); it != seen.set.end();) {
        if (*it <= seen.max - kReplayWindow)
            it = seen.set.erase(it);
        else
            ++it;
    }
    if (state.seen.size() > kMaxSeenSenders) {
        QString oldest;
        qint64 oldestAt = 0;
        for (auto it = state.seen.constBegin(); it != state.seen.constEnd(); ++it) {
            if (it.key() == kid)
                continue;
            if (oldest.isEmpty() || it->at < oldestAt) {
                oldest = it.key();
                oldestAt = it->at;
            }
        }
        state.seen.remove(oldest);
    }
    saveSeen(room);
}

void ChatDevice::clearSeen()
{
    for (auto it = m_rooms.begin(); it != m_rooms.end(); ++it) {
        loadSeen(it.key(), it.value());
        if (it->seen.isEmpty())
            continue;
        it->seen.clear();
        saveSeen(it.key());
    }
}

void ChatDevice::loadRatchets(const QString &room, Room &state)
{
    if (state.ratchetsLoaded)
        return;
    state.ratchetsLoaded = true;
    if (!m_persistent)
        return;
    const QJsonObject root =
        QJsonDocument::fromJson(
            readSetting(settingKey("chat/e2ee/ratchetV4/", room)).toByteArray())
            .object();
    if (root.value(QStringLiteral("v")).toInt() != forkmesh::chatv4::kVersion)
        return;
    const QJsonObject peers = root.value(QStringLiteral("peers")).toObject();
    for (auto it = peers.constBegin(); it != peers.constEnd(); ++it) {
        if (!keyIdPattern().match(it.key()).hasMatch())
            continue;
        const QJsonObject entry = it.value().toObject();
        RatchetPeer peer;
        peer.current = entry.value(QStringLiteral("current")).toString();
        for (const QJsonValue &value : entry.value(QStringLiteral("sessions")).toArray()) {
            RatchetSession session;
            if (forkmesh::chatv4::sessionFromJson(value.toObject(), &session))
                peer.sessions.append(session);
        }
        if (!peer.sessions.isEmpty())
            state.ratchets.insert(it.key(), peer);
    }
}

QHash<QString, RatchetPeer> ChatDevice::ratchetPeers(const QString &room)
{
    Room &state = roomState(room);
    loadRatchets(room, state);
    return state.ratchets;
}

void ChatDevice::storeRatchetPeer(const QString &room, const QString &kid, RatchetPeer peer,
                                  qint64 now)
{
    Room &state = roomState(room);
    loadRatchets(room, state);
    std::stable_sort(peer.sessions.begin(), peer.sessions.end(),
                     [](const RatchetSession &a, const RatchetSession &b) {
                         return a.used > b.used;
                     });
    QList<RatchetSession> kept;
    for (const RatchetSession &session : std::as_const(peer.sessions))
        if (session.sid == peer.current) {
            kept.append(session);
            break;
        }
    for (const RatchetSession &session : std::as_const(peer.sessions)) {
        if (kept.size() >= forkmesh::chatv4::kSessionsKept)
            break;
        if (session.sid != peer.current)
            kept.append(session);
    }
    for (RatchetSession &session : kept) {
        QList<RatchetSkipped> fresh;
        for (const RatchetSkipped &skipped : std::as_const(session.skipped))
            if (skipped.ts >= now - forkmesh::chatv4::kSkippedLifeMs)
                fresh.append(skipped);
        session.skipped = fresh.mid(qMax<qsizetype>(0, fresh.size() -
                                                           forkmesh::chatv4::kSkippedKept));
    }
    peer.sessions = kept;
    state.ratchets.insert(kid, peer);
}

void ChatDevice::saveRatchets(const QString &room, bool immediately)
{
    if (!m_persistent)
        return;
    const QString key = settingKey("chat/e2ee/ratchetV4/", room);
    const QByteArray value =
        QJsonDocument(exportRatchets(room)).toJson(QJsonDocument::Compact);
    if (immediately)
        writeNow(key, value);
    else
        writeLater(key, value);
}

void ChatDevice::resetRatchet(const QString &room, const QString &kid)
{
    Room &state = roomState(room);
    loadRatchets(room, state);
    if (state.ratchets.remove(kid))
        saveRatchets(room, true);
}

bool ChatDevice::claimSessionReset(const QString &room, const QString &kid, qint64 now)
{
    const QString key = room + QLatin1Char('\n') + kid;
    const auto last = m_sessionResets.constFind(key);
    if (last != m_sessionResets.cend() && now - last.value() < forkmesh::chatv4::kResetEveryMs &&
        now >= last.value())
        return false;
    m_sessionResets.insert(key, now);
    return true;
}

QJsonObject ChatDevice::exportRatchets(const QString &room)
{
    Room &state = roomState(room);
    loadRatchets(room, state);
    QJsonObject peers;
    for (auto it = state.ratchets.constBegin(); it != state.ratchets.constEnd(); ++it) {
        QJsonArray sessions;
        for (const RatchetSession &session : it->sessions)
            sessions.append(forkmesh::chatv4::sessionJson(session));
        peers.insert(it.key(),
                     QJsonObject{{QStringLiteral("current"),
                                  it->current.isEmpty() ? QJsonValue(QJsonValue::Null)
                                                        : QJsonValue(it->current)},
                                 {QStringLiteral("sessions"), sessions}});
    }
    return {{QStringLiteral("v"), forkmesh::chatv4::kVersion},
            {QStringLiteral("peers"), peers}};
}

void ChatDevice::importRatchets(const QString &room, const QJsonObject &stored)
{
    Room &state = roomState(room);
    state.ratchetsLoaded = true;
    state.ratchets.clear();
    const QJsonObject peers = stored.value(QStringLiteral("peers")).toObject();
    for (auto it = peers.constBegin(); it != peers.constEnd(); ++it) {
        const QJsonObject entry = it.value().toObject();
        RatchetPeer peer;
        peer.current = entry.value(QStringLiteral("current")).toString();
        for (const QJsonValue &value : entry.value(QStringLiteral("sessions")).toArray()) {
            RatchetSession session;
            if (forkmesh::chatv4::sessionFromJson(value.toObject(), &session))
                peer.sessions.append(session);
        }
        state.ratchets.insert(it.key(), peer);
    }
    saveRatchets(room, true);
}

void ChatDevice::hold(const QString &scope, const QString &room,
                      const QJsonObject &frame)
{
    Scope &state = scopeState(scope);
    QList<QJsonObject> &frames = state.held[room];
    const QString sig = frame.value(QStringLiteral("sig")).toString();
    for (const QJsonObject &existing : std::as_const(frames))
        if (existing.value(QStringLiteral("sig")).toString() == sig)
            return;
    frames.append(frame);
    trimHeld(&frames);
    saveScope(scope, false);
}

QList<QJsonObject> ChatDevice::held(const QString &scope, const QString &room)
{
    return scopeState(scope).held.value(room);
}

void ChatDevice::setHeld(const QString &scope, const QString &room,
                         const QList<QJsonObject> &frames)
{
    Scope &state = scopeState(scope);
    if (state.held.value(room) == frames)
        return;
    if (frames.isEmpty())
        state.held.remove(room);
    else
        state.held.insert(room, frames);
    saveScope(scope, false);
}

int ChatDevice::subscribe(Listener listener)
{
    const int token = m_nextToken++;
    m_listeners.insert(token, std::move(listener));
    return token;
}

void ChatDevice::unsubscribe(int token)
{
    m_listeners.remove(token);
}

void ChatDevice::notify(Event event, const QString &scope, const QString &kid)
{
    const QList<int> tokens = m_listeners.keys();
    for (const int token : tokens) {
        const auto it = m_listeners.constFind(token);
        if (it == m_listeners.constEnd())
            continue;
        const Listener listener = it.value();
        listener(event, scope, kid);
    }
}

void ChatDevice::flush()
{
    flushPendingWrites();
}

} // namespace forkmesh::chatv3

namespace forkmesh::chatv4 {

using forkmesh::chatv3::RatchetSession;
using forkmesh::chatv3::RatchetSkipped;

namespace {

const QRegularExpression &key32Pattern()
{
    static const QRegularExpression value(QStringLiteral("\\A[A-Za-z0-9_-]{43}\\z"));
    return value;
}

bool count(const QJsonValue &value, qint64 *out)
{
    qint64 number = 0;
    if (!chatv3::jsonInteger(value, &number) || number < 0 || number >= (qint64(1) << 31))
        return false;
    if (out)
        *out = number;
    return true;
}

KeyPair split64(const QByteArray &out)
{
    if (out.size() != 64)
        return {};
    return {out.left(32), out.mid(32)};
}

QJsonValue nullable(const QString &text)
{
    return text.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(text);
}

QJsonValue nullableB64(const QByteArray &bytes)
{
    return bytes.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(chatv3::b64(bytes));
}

bool optionalB64(const QJsonValue &value, int size, QByteArray *out)
{
    if (value.isNull() || value.isUndefined()) {
        out->clear();
        return true;
    }
    bool ok = false;
    *out = chatv3::fromB64(value.toString(), &ok);
    return ok && out->size() == size;
}

} // namespace

QByteArray hmacSha256(const QByteArray &key, const QByteArray &data)
{
    return QMessageAuthenticationCode::hash(data, key, QCryptographicHash::Sha256);
}

KeyPair kdfInit(const QString &room, const QString &sender, const QString &recipient,
                const QString &spkId, const QString &sid, const QByteArray &ss1,
                const QByteArray &ss2)
{
    if (ss1.isEmpty() || ss2.isEmpty())
        return {};
    const QByteArray salt =
        chatv3::sha256(QByteArrayLiteral("ForkMesh chat e2ee salt v4:") + room.toUtf8());
    const QByteArray info = QStringList{QStringLiteral("forkmesh-chat-e2ee-init-v4"), room,
                                        sender, recipient, spkId, sid}
                                .join(QLatin1Char('\n'))
                                .toUtf8();
    QByteArray ikm = ss1 + ss2;
    const KeyPair out = split64(chatv3::hkdfSha256(ikm, salt, info, 64));
    chatv3::wipe(ikm);
    return out;
}

KeyPair kdfRk(const QByteArray &rk, const QByteArray &dhOut, const QByteArray &ss)
{
    if (rk.isEmpty() || dhOut.isEmpty() || ss.isEmpty())
        return {};
    QByteArray ikm = dhOut + ss;
    const KeyPair out = split64(chatv3::hkdfSha256(
        ikm, rk, QByteArrayLiteral("forkmesh-chat-e2ee-ratchet-v4"), 64));
    chatv3::wipe(ikm);
    return out;
}

KeyPair kdfCk(const QByteArray &ck)
{
    if (ck.isEmpty())
        return {};
    return {hmacSha256(ck, QByteArray(1, '\x01')), hmacSha256(ck, QByteArray(1, '\x02'))};
}

QString sessionId(const QByteArray &dhPub, const QByteArray &ct)
{
    return chatv3::id16(dhPub + ct);
}

QByteArray contentAad(const QString &room, const QString &sender, qint64 ctr, qint64 ts)
{
    return QStringList{QStringLiteral("forkmesh-chat-e2ee-content-v4"), room, sender,
                       QString::number(ctr), QString::number(ts)}
        .join(QLatin1Char('\n'))
        .toUtf8();
}

namespace {

QString ctHash(const QJsonObject &entry)
{
    return chatv3::b64u(
        chatv3::sha256(chatv3::fromB64(entry.value(QStringLiteral("ct")).toString())));
}

QString integerText(const QJsonObject &entry, const char *key)
{
    qint64 value = 0;
    chatv3::jsonInteger(entry.value(QLatin1String(key)), &value);
    return QString::number(value);
}

} // namespace

QByteArray wrapAad(const QString &room, const QString &sender, const QJsonObject &entry,
                   const QString &bodyNonce, const QString &bodyTag)
{
    return QStringList{QStringLiteral("forkmesh-chat-e2ee-wrap-v4"),
                       room,
                       sender,
                       entry.value(QStringLiteral("kid")).toString(),
                       entry.value(QStringLiteral("sid")).toString(),
                       entry.value(QStringLiteral("spk")).toString(),
                       ctHash(entry),
                       entry.value(QStringLiteral("dh")).toString(),
                       integerText(entry, "pn"),
                       integerText(entry, "n"),
                       bodyNonce,
                       bodyTag}
        .join(QLatin1Char('\n'))
        .toUtf8();
}

QByteArray sealedMessage(const QString &room, const QJsonObject &frame)
{
    bool ok = false;
    const QByteArray body =
        chatv3::fromB64(frame.value(QStringLiteral("body")).toString(), &ok);
    if (!ok)
        return {};
    QString text = QStringList{QStringLiteral("forkmesh-chat-e2ee-sealed-v4"),
                               room,
                               frame.value(QStringLiteral("kid")).toString(),
                               frame.value(QStringLiteral("idk")).toString(),
                               integerText(frame, "ctr"),
                               integerText(frame, "ts"),
                               frame.value(QStringLiteral("nonce")).toString(),
                               frame.value(QStringLiteral("tag")).toString(),
                               chatv3::b64u(chatv3::sha256(body))}
                       .join(QLatin1Char('\n'));
    for (const QJsonValue &value : frame.value(QStringLiteral("keys")).toArray()) {
        const QJsonObject entry = value.toObject();
        text += QLatin1Char('\n') +
                QStringList{entry.value(QStringLiteral("kid")).toString(),
                            entry.value(QStringLiteral("sid")).toString(),
                            entry.value(QStringLiteral("spk")).toString(),
                            ctHash(entry),
                            entry.value(QStringLiteral("dh")).toString(),
                            integerText(entry, "pn"),
                            integerText(entry, "n"),
                            entry.value(QStringLiteral("nonce")).toString(),
                            entry.value(QStringLiteral("tag")).toString(),
                            entry.value(QStringLiteral("key")).toString()}
                    .join(QLatin1Char('|'));
    }
    return text.toUtf8();
}

bool validEntry(const QJsonObject &entry)
{
    for (const char *key : {"kid", "sid", "spk", "ct", "dh", "nonce", "tag", "key"})
        if (!entry.value(QLatin1String(key)).isString())
            return false;
    const auto &id = chatv3::keyIdPattern();
    if (!id.match(entry.value(QStringLiteral("kid")).toString()).hasMatch() ||
        !id.match(entry.value(QStringLiteral("sid")).toString()).hasMatch() ||
        !id.match(entry.value(QStringLiteral("spk")).toString()).hasMatch() ||
        !key32Pattern().match(entry.value(QStringLiteral("dh")).toString()).hasMatch() ||
        !count(entry.value(QStringLiteral("pn")), nullptr) ||
        !count(entry.value(QStringLiteral("n")), nullptr))
        return false;
    bool okCt = false;
    bool okNonce = false;
    bool okTag = false;
    bool okKey = false;
    return chatv3::fromB64(entry.value(QStringLiteral("ct")).toString(), &okCt).size() ==
               chatv3::kMlkemCiphertextBytes &&
           chatv3::fromB64(entry.value(QStringLiteral("nonce")).toString(), &okNonce)
                   .size() == 12 &&
           chatv3::fromB64(entry.value(QStringLiteral("tag")).toString(), &okTag).size() ==
               16 &&
           chatv3::fromB64(entry.value(QStringLiteral("key")).toString(), &okKey).size() ==
               32 &&
           okCt && okNonce && okTag && okKey;
}

QJsonObject sessionJson(const RatchetSession &session)
{
    QJsonArray skipped;
    for (const RatchetSkipped &key : session.skipped)
        skipped.append(QJsonObject{{QStringLiteral("dh"), key.dh},
                                   {QStringLiteral("n"), key.n},
                                   {QStringLiteral("mk"), chatv3::b64(key.mk)},
                                   {QStringLiteral("ts"), key.ts}});
    const QJsonValue dhs =
        session.dhsPriv.isEmpty()
            ? QJsonValue(QJsonValue::Null)
            : QJsonValue(QJsonObject{{QStringLiteral("priv"), chatv3::b64(session.dhsPriv)},
                                     {QStringLiteral("pub"), session.dhsPub}});
    const QJsonValue sq =
        session.sqSpk.isEmpty()
            ? QJsonValue(QJsonValue::Null)
            : QJsonValue(QJsonObject{{QStringLiteral("spk"), session.sqSpk},
                                     {QStringLiteral("ct"), chatv3::b64(session.sqCt)}});
    return {{QStringLiteral("sid"), session.sid},
            {QStringLiteral("rk"), chatv3::b64(session.rk)},
            {QStringLiteral("dhs"), dhs},
            {QStringLiteral("dhr"), nullable(session.dhr)},
            {QStringLiteral("cks"), nullableB64(session.cks)},
            {QStringLiteral("ns"), session.ns},
            {QStringLiteral("pn"), session.pn},
            {QStringLiteral("sq"), sq},
            {QStringLiteral("ckr"), nullableB64(session.ckr)},
            {QStringLiteral("nr"), session.nr},
            {QStringLiteral("skipped"), skipped},
            {QStringLiteral("created"), session.created},
            {QStringLiteral("used"), session.used}};
}

bool sessionFromJson(const QJsonObject &object, RatchetSession *session)
{
    RatchetSession out;
    out.sid = object.value(QStringLiteral("sid")).toString();
    bool ok = false;
    out.rk = chatv3::fromB64(object.value(QStringLiteral("rk")).toString(), &ok);
    if (!chatv3::keyIdPattern().match(out.sid).hasMatch() || !ok || out.rk.size() != 32)
        return false;
    const QJsonValue dhs = object.value(QStringLiteral("dhs"));
    if (dhs.isObject()) {
        out.dhsPriv = chatv3::fromB64(dhs.toObject().value(QStringLiteral("priv")).toString(),
                                      &ok);
        out.dhsPub = dhs.toObject().value(QStringLiteral("pub")).toString();
        if (!ok || out.dhsPriv.size() != 32 || !key32Pattern().match(out.dhsPub).hasMatch())
            return false;
    }
    const QJsonValue dhr = object.value(QStringLiteral("dhr"));
    if (dhr.isString()) {
        out.dhr = dhr.toString();
        if (!key32Pattern().match(out.dhr).hasMatch())
            return false;
    }
    if (!optionalB64(object.value(QStringLiteral("cks")), 32, &out.cks) ||
        !optionalB64(object.value(QStringLiteral("ckr")), 32, &out.ckr))
        return false;
    const QJsonValue sq = object.value(QStringLiteral("sq"));
    if (sq.isObject()) {
        out.sqSpk = sq.toObject().value(QStringLiteral("spk")).toString();
        out.sqCt = chatv3::fromB64(sq.toObject().value(QStringLiteral("ct")).toString(), &ok);
        if (!ok || !chatv3::keyIdPattern().match(out.sqSpk).hasMatch() ||
            out.sqCt.size() != chatv3::kMlkemCiphertextBytes)
            return false;
    }
    if (!count(object.value(QStringLiteral("ns")), &out.ns) ||
        !count(object.value(QStringLiteral("pn")), &out.pn) ||
        !count(object.value(QStringLiteral("nr")), &out.nr))
        return false;
    chatv3::jsonInteger(object.value(QStringLiteral("created")), &out.created);
    chatv3::jsonInteger(object.value(QStringLiteral("used")), &out.used);
    for (const QJsonValue &value : object.value(QStringLiteral("skipped")).toArray()) {
        const QJsonObject entry = value.toObject();
        RatchetSkipped key;
        key.dh = entry.value(QStringLiteral("dh")).toString();
        key.mk = chatv3::fromB64(entry.value(QStringLiteral("mk")).toString(), &ok);
        if (!ok || key.mk.size() != 32 || !key32Pattern().match(key.dh).hasMatch() ||
            !count(entry.value(QStringLiteral("n")), &key.n))
            continue;
        chatv3::jsonInteger(entry.value(QStringLiteral("ts")), &key.ts);
        out.skipped.append(key);
    }
    *session = out;
    return true;
}

QString resetType()
{
    return QStringLiteral("e2ee-reset");
}

} // namespace forkmesh::chatv4

using namespace forkmesh::chatv3;

RoomSeal::RoomSeal(const QString &roomName, const QString &trustScope, Policy policy,
                   const QString &acct, const QString &node,
                   std::shared_ptr<ChatDevice> device)
    : m_room(roomName.trimmed()),
      m_scope(trustScope.trimmed().isEmpty() ? roomName.trimmed() : trustScope.trimmed()),
      m_acct(acct),
      m_node(node),
      m_policy(policy),
      m_ratchet(policy == Policy::Strict),
      m_device(std::move(device))
{
    if (m_room.isEmpty()) {
        m_error = QStringLiteral("A chat room is required for end-to-end encryption.");
        return;
    }
    if (!m_device || !m_device->isValid()) {
        m_error = QStringLiteral("This device has no chat identity key.");
        return;
    }
    m_device->setPolicy(m_scope, m_policy);
    m_device->maintainPrekeys();
    m_device->ensureOwnRecord(m_scope, m_acct, m_node);
}

void RoomSeal::setParticipants(const QStringList &participants)
{
    m_participants.clear();
    for (const QString &participant : participants) {
        const QString name = participant.trimmed().toLower();
        if (!name.isEmpty() && !m_participants.contains(name))
            m_participants.append(name);
    }
}

bool RoomSeal::isKeyFrame(const QJsonObject &frame)
{
    return frame.value(QStringLiteral("type")).toString() == QLatin1String("e2ee-key") &&
           frame.value(QStringLiteral("v")).toInt() == 3 &&
           keyIdPattern().match(frame.value(QStringLiteral("kid")).toString()).hasMatch() &&
           frame.value(QStringLiteral("idk")).isString() &&
           frame.value(QStringLiteral("spk")).isObject();
}

bool RoomSeal::isSealedFrame(const QJsonObject &frame)
{
    const QJsonValue keys = frame.value(QStringLiteral("keys"));
    const int version = frame.value(QStringLiteral("v")).toInt();
    return frame.value(QStringLiteral("type")).toString() == QLatin1String("sealed") &&
           (version == 3 || version == forkmesh::chatv4::kVersion) &&
           keyIdPattern().match(frame.value(QStringLiteral("kid")).toString()).hasMatch() &&
           keys.isArray() && (version != 3 || !keys.toArray().isEmpty()) &&
           keys.toArray().size() <= kMaxRecipients;
}

bool RoomSeal::isLegacyFrame(const QJsonObject &frame)
{
    const QString type = frame.value(QStringLiteral("type")).toString();
    const int version = frame.value(QStringLiteral("v")).toInt();
    if (type == QLatin1String("sealed"))
        return version != 3 && version != forkmesh::chatv4::kVersion;
    return type == QLatin1String("e2ee-key") && version != 3;
}

QString RoomSeal::frameKeyId(const QJsonObject &frame)
{
    return isSealedFrame(frame) ? frame.value(QStringLiteral("kid")).toString()
                                : QString();
}

QJsonObject RoomSeal::keyFrame() const
{
    if (!isValid() || !validAcct(m_acct) || !validAcct(m_node))
        return {};
    m_device->maintainPrekeys();
    const Prekey *newest = m_device->newestPrekey();
    if (!newest)
        return {};
    m_device->ensureOwnRecord(m_scope, m_acct, m_node);
    const QJsonObject spk = publicPrekey(*newest);
    const QString kid = m_device->kid();
    const QString idk = m_device->idkText();
    const QByteArray sig =
        m_device->sign(bundleMessage(m_room, kid, idk, m_acct, m_node, spk));
    if (sig.isEmpty())
        return {};
    QJsonObject bundle{{QStringLiteral("type"), QStringLiteral("e2ee-key")},
                       {QStringLiteral("v"), 3},
                       {QStringLiteral("kid"), kid},
                       {QStringLiteral("idk"), idk},
                       {QStringLiteral("acct"), m_acct},
                       {QStringLiteral("node"), m_node},
                       {QStringLiteral("spk"), spk},
                       {QStringLiteral("sig"), b64u(sig)}};
    if (m_policy == Policy::Open && m_nodeSigner) {
        const QByteArray nsig = m_nodeSigner(nodeBindingMessage(m_room, kid, idk, m_acct));
        if (nsig.size() == kSignatureBytes)
            bundle.insert(QStringLiteral("nsig"), b64u(nsig));
    }
    return bundle;
}

RoomSeal::BundleResult RoomSeal::acceptKeyFrame(const QJsonObject &frame,
                                                bool *changed) const
{
    if (changed)
        *changed = false;
    if (!isValid())
        return BundleResult::Rejected;
    if (frame.value(QStringLiteral("kid")).toString() == m_device->kid())
        return verifyBundle(frame, m_room, m_device->now()) ? BundleResult::Known
                                                           : BundleResult::Rejected;
    if (m_policy == Policy::Open && !verifyNodeBinding(frame, m_room))
        return BundleResult::Rejected;
    return m_device->acceptBundle(m_scope, m_policy, m_room, frame, changed,
                                  m_policy == Policy::Strict ? m_participants
                                                             : QStringList());
}

QJsonObject RoomSeal::sealObject(const QJsonObject &plain) const
{
    return seal(plain, nullptr);
}

QJsonObject RoomSeal::sealObjectFor(const QJsonObject &plain,
                                    const QStringList &keyIds) const
{
    return seal(plain, &keyIds);
}

QJsonObject RoomSeal::seal(const QJsonObject &plain, const QStringList *only) const
{
    if (!isValid())
        return {};
    m_device->maintainPrekeys();
    if (usesRatchet())
        return sealV4(plain, only);
    const Prekey *own = m_device->newestPrekey();
    if (!own)
        return {};
    const QByteArray input = compactJson(plain);
    if (input.isEmpty() || input.size() > kMaxPlainBytes)
        return {};
    const qint64 now = m_device->now();
    const QString self = m_device->kid();

    QList<TrustRecord> candidates;
    for (const TrustRecord &record : m_device->records(m_scope)) {
        if (record.state != TrustState::Approved || record.kid == self)
            continue;
        if (only && !only->contains(record.kid))
            continue;
        if (spkExp(record.spk) <= now)
            continue;
        candidates.append(record);
    }
    if (candidates.size() > kMaxRecipients - 1) {
        std::sort(candidates.begin(), candidates.end(),
                  [](const TrustRecord &a, const TrustRecord &b) {
                      return a.lastSeen > b.lastSeen;
                  });
        candidates = candidates.mid(0, kMaxRecipients - 1);
    }
    QMap<QString, QJsonObject> targets;
    targets.insert(self, publicPrekey(*own));
    for (const TrustRecord &record : std::as_const(candidates))
        targets.insert(record.kid, record.spk);

    const qint64 ctr = m_device->nextCounter(m_room);
    QByteArray contentKey = randomBytes(kKeyBytes);
    const QByteArray nonce = randomBytes(kNonceBytes);
    QByteArray body;
    QByteArray tag;
    if (contentKey.size() != kKeyBytes || nonce.size() != kNonceBytes ||
        !aesGcmSeal(contentKey, nonce, input, contentAad(m_room, self, ctr, now), &body,
                    &tag)) {
        wipe(contentKey);
        return {};
    }
    const QString nonceText = b64(nonce);
    const QString tagText = b64(tag);
    QJsonArray keys;
    for (auto it = targets.cbegin(); it != targets.cend(); ++it) {
        const QJsonObject spk = it.value();
        const QByteArray x = fromB64u(spk.value(QStringLiteral("x")).toString());
        const QByteArray pq = fromB64u(spk.value(QStringLiteral("pq")).toString());
        QByteArray ephPriv = randomBytes(kKeyBytes);
        const QByteArray ephPub = x25519Public(ephPriv);
        QByteArray ss1 = x25519Shared(ephPriv, x);
        wipe(ephPriv);
        QByteArray ct;
        QByteArray ss2;
        if (ss1.isEmpty() || ephPub.isEmpty() || !mlkemEncapsulate(pq, &ct, &ss2)) {
            wipe(ss1);
            continue;
        }
        const QString spkId = spk.value(QStringLiteral("id")).toString();
        QByteArray wrapKey = kek(m_room, self, it.key(), spkId, ephPub, ct, ss1, ss2);
        wipe(ss1);
        wipe(ss2);
        const QByteArray wrapNonce = randomBytes(kNonceBytes);
        QByteArray wrapped;
        QByteArray wrapTag;
        const bool ok = aesGcmSeal(wrapKey, wrapNonce, contentKey,
                                   wrapAad(m_room, self, it.key(), nonceText, tagText),
                                   &wrapped, &wrapTag);
        wipe(wrapKey);
        if (!ok)
            continue;
        keys.append(QJsonObject{{QStringLiteral("kid"), it.key()},
                                {QStringLiteral("spk"), spkId},
                                {QStringLiteral("eph"), b64u(ephPub)},
                                {QStringLiteral("ct"), b64(ct)},
                                {QStringLiteral("nonce"), b64(wrapNonce)},
                                {QStringLiteral("tag"), b64(wrapTag)},
                                {QStringLiteral("key"), b64(wrapped)}});
    }
    wipe(contentKey);
    if (keys.isEmpty())
        return {};
    QJsonObject frame{{QStringLiteral("type"), QStringLiteral("sealed")},
                      {QStringLiteral("v"), 3},
                      {QStringLiteral("kid"), self},
                      {QStringLiteral("idk"), m_device->idkText()},
                      {QStringLiteral("ctr"), double(ctr)},
                      {QStringLiteral("ts"), double(now)},
                      {QStringLiteral("nonce"), nonceText},
                      {QStringLiteral("tag"), tagText},
                      {QStringLiteral("body"), b64(body)},
                      {QStringLiteral("keys"), keys}};
    const QByteArray sig = m_device->sign(sealedMessage(m_room, frame));
    if (sig.isEmpty())
        return {};
    frame.insert(QStringLiteral("sig"), b64u(sig));
    return frame;
}

RoomSeal::Opened RoomSeal::open(const QJsonObject &frame, bool historyReplay,
                                bool commit) const
{
    Opened result;
    if (!isValid())
        return result;
    if (frame.value(QStringLiteral("type")).toString() != QLatin1String("sealed"))
        return result;
    qint64 version = 0;
    if (!jsonInteger(frame.value(QStringLiteral("v")), &version))
        return result;
    if (usesRatchet())
        return version == forkmesh::chatv4::kVersion ? openV4(frame, commit) : result;
    if (version != 3)
        return result;
    const QJsonValue kidValue = frame.value(QStringLiteral("kid"));
    const QJsonValue idkValue = frame.value(QStringLiteral("idk"));
    const QJsonValue keysValue = frame.value(QStringLiteral("keys"));
    if (!kidValue.isString() || !idkValue.isString() || !keysValue.isArray())
        return result;
    const QString kid = kidValue.toString();
    bool ok = false;
    const QByteArray idk = fromB64u(idkValue.toString(), &ok);
    if (!ok || idk.size() != kKeyBytes || kid != id16(idk))
        return result;
    const QJsonArray keys = keysValue.toArray();
    if (keys.isEmpty() || keys.size() > kMaxRecipients)
        return result;
    QString previous;
    for (qsizetype i = 0; i < keys.size(); ++i) {
        if (!keys.at(i).isObject())
            return result;
        const QJsonObject entry = keys.at(i).toObject();
        const QString entryKid = entry.value(QStringLiteral("kid")).toString();
        if (!keyIdPattern().match(entryKid).hasMatch() ||
            (i > 0 && !(previous < entryKid)))
            return result;
        previous = entryKid;
        bool okEph = false;
        bool okCt = false;
        bool okNonce = false;
        bool okTag = false;
        bool okKey = false;
        if (!keyIdPattern()
                 .match(entry.value(QStringLiteral("spk")).toString())
                 .hasMatch() ||
            fromB64u(entry.value(QStringLiteral("eph")).toString(), &okEph).size() !=
                kKeyBytes ||
            fromB64(entry.value(QStringLiteral("ct")).toString(), &okCt).size() !=
                kMlkemCiphertextBytes ||
            fromB64(entry.value(QStringLiteral("nonce")).toString(), &okNonce).size() !=
                kNonceBytes ||
            fromB64(entry.value(QStringLiteral("tag")).toString(), &okTag).size() !=
                kTagBytes ||
            fromB64(entry.value(QStringLiteral("key")).toString(), &okKey).size() !=
                kKeyBytes ||
            !okEph || !okCt || !okNonce || !okTag || !okKey)
            return result;
    }
    qint64 ctr = 0;
    qint64 ts = 0;
    if (!jsonInteger(frame.value(QStringLiteral("ctr")), &ctr) || ctr < 1 ||
        !jsonInteger(frame.value(QStringLiteral("ts")), &ts))
        return result;
    bool okNonce = false;
    bool okTag = false;
    bool okBody = false;
    bool okSig = false;
    const QByteArray nonce = fromB64(frame.value(QStringLiteral("nonce")).toString(), &okNonce);
    const QByteArray tag = fromB64(frame.value(QStringLiteral("tag")).toString(), &okTag);
    const QByteArray body = fromB64(frame.value(QStringLiteral("body")).toString(), &okBody);
    const QByteArray sig = fromB64u(frame.value(QStringLiteral("sig")).toString(), &okSig);
    if (!okNonce || !okTag || !okBody || !okSig || nonce.size() != kNonceBytes ||
        tag.size() != kTagBytes || sig.size() != kSignatureBytes ||
        body.size() > kMaxPlainBytes)
        return result;

    const QString self = m_device->kid();
    const bool fromSelf = kid == self;
    QString acct;
    QString node;
    result.senderKid = kid;
    if (fromSelf) {
        acct = m_acct;
        node = m_node;
    } else {
        const TrustRecord *record = m_device->record(m_scope, kid);
        if (!record || record->state != TrustState::Approved) {
            if (commit && (!record || record->state == TrustState::Pending))
                m_device->hold(m_scope, m_room, frame);
            result.status = Status::Held;
            return result;
        }
        if (record->idk != idkValue.toString())
            return result;
        acct = record->acct;
        node = record->node;
    }
    if (!ed25519Verify(idk, sig, sealedMessage(m_room, frame)))
        return result;
    if (!historyReplay && m_device->isReplay(m_room, kid, ctr)) {
        result.status = Status::Replay;
        return result;
    }
    const qint64 now = m_device->now();
    if (ts < now - kPrekeyLifeMs || ts > now + kDayMs)
        return result;
    QJsonObject entry;
    for (const QJsonValue &value : keys) {
        if (value.toObject().value(QStringLiteral("kid")).toString() == self) {
            entry = value.toObject();
            break;
        }
    }
    if (entry.isEmpty()) {
        result.status = Status::NotForMe;
        return result;
    }
    const Prekey *prekey =
        m_device->findPrekey(entry.value(QStringLiteral("spk")).toString());
    if (!prekey) {
        result.status = Status::NotForMe;
        return result;
    }
    const QByteArray ct = fromB64(entry.value(QStringLiteral("ct")).toString());
    const QByteArray ephPub = fromB64u(entry.value(QStringLiteral("eph")).toString());
    QByteArray ss1 = x25519Shared(prekey->xPriv, ephPub);
    QByteArray ss2 = mlkemDecapsulate(prekey->pqSeed, ct);
    QByteArray wrapKey = kek(m_room, kid, self, prekey->id, ephPub, ct, ss1, ss2);
    wipe(ss1);
    wipe(ss2);
    if (wrapKey.isEmpty())
        return result;
    QByteArray contentKey;
    const bool unwrapped = aesGcmOpen(
        wrapKey, fromB64(entry.value(QStringLiteral("nonce")).toString()),
        fromB64(entry.value(QStringLiteral("key")).toString()),
        fromB64(entry.value(QStringLiteral("tag")).toString()),
        wrapAad(m_room, kid, self, frame.value(QStringLiteral("nonce")).toString(),
                frame.value(QStringLiteral("tag")).toString()),
        &contentKey);
    wipe(wrapKey);
    if (!unwrapped || contentKey.size() != kKeyBytes) {
        wipe(contentKey);
        return result;
    }
    QByteArray clear;
    const bool decrypted =
        aesGcmOpen(contentKey, nonce, body, tag, contentAad(m_room, kid, ctr, ts), &clear);
    wipe(contentKey);
    if (!decrypted)
        return result;
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(clear, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return result;
    const QJsonObject plain = document.object();
    if (!fromSelf && plain.contains(QStringLiteral("senderId")) &&
        plain.value(QStringLiteral("senderId")).toString() != node)
        return result;
    if (commit) {
        m_device->markSeen(m_room, kid, ctr);
        if (!fromSelf)
            m_device->touch(m_scope, kid);
    }
    result.status = Status::Opened;
    result.plain = plain;
    result.senderAcct = acct;
    result.senderNode = node;
    result.ts = ts;
    result.self = fromSelf;
    return result;
}

namespace {

enum class Step { Ok, NoSession, Replay };

qint64 entryInteger(const QJsonObject &entry, const char *key)
{
    return qint64(entry.value(QLatin1String(key)).toDouble());
}

RatchetSession *findSession(RatchetPeer &peer, const QString &sid)
{
    for (RatchetSession &session : peer.sessions)
        if (session.sid == sid)
            return &session;
    return nullptr;
}

Step skipTo(RatchetSession &session, qint64 until, qint64 now)
{
    if (until < session.nr)
        return Step::Replay;
    if (until - session.nr > forkmesh::chatv4::kMaxSkip)
        return Step::NoSession;
    QByteArray ck = session.ckr;
    while (session.nr < until) {
        const forkmesh::chatv4::KeyPair chain = forkmesh::chatv4::kdfCk(ck);
        if (chain.first.isEmpty())
            return Step::NoSession;
        session.skipped.append({session.dhr, session.nr, chain.first, now});
        ck = chain.second;
        ++session.nr;
    }
    session.ckr = ck;
    return Step::Ok;
}

Step receiveStep(const ChatDevice &device, RatchetSession &session, const QJsonObject &entry,
                 qint64 now, QByteArray *mk)
{
    const QString dh = entry.value(QStringLiteral("dh")).toString();
    const qint64 n = entryInteger(entry, "n");
    for (qsizetype i = 0; i < session.skipped.size(); ++i) {
        if (session.skipped.at(i).dh == dh && session.skipped.at(i).n == n) {
            *mk = session.skipped.at(i).mk;
            session.skipped.removeAt(i);
            return Step::Ok;
        }
    }
    if (dh != session.dhr) {
        if (session.dhsPriv.isEmpty())
            return Step::NoSession;
        if (!session.ckr.isEmpty()) {
            const Step step = skipTo(session, entryInteger(entry, "pn"), now);
            if (step != Step::Ok)
                return step;
        }
        const Prekey *spk = device.findPrekey(entry.value(QStringLiteral("spk")).toString());
        if (!spk)
            return Step::NoSession;
        QByteArray ss = mlkemDecapsulate(spk->pqSeed,
                                         fromB64(entry.value(QStringLiteral("ct")).toString()));
        QByteArray dhOut = x25519Shared(session.dhsPriv, fromB64u(dh));
        const forkmesh::chatv4::KeyPair root = forkmesh::chatv4::kdfRk(session.rk, dhOut, ss);
        wipe(ss);
        wipe(dhOut);
        if (root.first.isEmpty())
            return Step::NoSession;
        session.rk = root.first;
        session.ckr = root.second;
        session.nr = 0;
        session.dhr = dh;
        session.pn = session.ns;
        session.ns = 0;
        session.cks.clear();
        session.sqSpk.clear();
        session.sqCt.clear();
    }
    if (session.ckr.isEmpty())
        return Step::NoSession;
    const Step step = skipTo(session, n, now);
    if (step != Step::Ok)
        return step;
    const forkmesh::chatv4::KeyPair chain = forkmesh::chatv4::kdfCk(session.ckr);
    session.ckr = chain.second;
    ++session.nr;
    *mk = chain.first;
    return Step::Ok;
}

Step openEntry(const ChatDevice &device, const QString &room, const QString &sender,
               const QJsonObject &entry, qint64 now, RatchetPeer &peer, QByteArray *mk)
{
    const QString sid = entry.value(QStringLiteral("sid")).toString();
    RatchetSession *session = findSession(peer, sid);
    if (!session) {
        const QByteArray dh = fromB64u(entry.value(QStringLiteral("dh")).toString());
        const QByteArray ct = fromB64(entry.value(QStringLiteral("ct")).toString());
        if (sid != forkmesh::chatv4::sessionId(dh, ct) || entryInteger(entry, "pn") != 0)
            return Step::NoSession;
        const Prekey *spk = device.findPrekey(entry.value(QStringLiteral("spk")).toString());
        if (!spk)
            return Step::NoSession;
        QByteArray ss1 = x25519Shared(spk->xPriv, dh);
        QByteArray ss2 = mlkemDecapsulate(spk->pqSeed, ct);
        const forkmesh::chatv4::KeyPair init =
            forkmesh::chatv4::kdfInit(room, sender, device.kid(), spk->id, sid, ss1, ss2);
        wipe(ss1);
        wipe(ss2);
        if (init.first.isEmpty())
            return Step::NoSession;
        RatchetSession fresh;
        fresh.sid = sid;
        fresh.rk = init.first;
        fresh.dhr = entry.value(QStringLiteral("dh")).toString();
        fresh.ckr = init.second;
        fresh.created = now;
        fresh.used = now;
        peer.sessions.append(fresh);
        session = &peer.sessions.last();
    }
    const Step step = receiveStep(device, *session, entry, now, mk);
    if (step != Step::Ok)
        return step;
    session->used = now;
    peer.current = sid;
    return Step::Ok;
}

bool sendEntry(const QString &room, const QString &self, const QString &recipient,
               const QJsonObject &spk, qint64 now, RatchetPeer &peer, QJsonObject *entry,
               QByteArray *mk)
{
    RatchetSession *session = findSession(peer, peer.current);
    if (session && session->cks.isEmpty() && session->dhr.isEmpty())
        session = nullptr;
    const QString spkId = spk.value(QStringLiteral("id")).toString();
    const QByteArray pq = fromB64u(spk.value(QStringLiteral("pq")).toString());
    if (!session) {
        const QByteArray x = fromB64u(spk.value(QStringLiteral("x")).toString());
        QByteArray ephPriv = randomBytes(kKeyBytes);
        const QByteArray ephPub = x25519Public(ephPriv);
        QByteArray ss1 = x25519Shared(ephPriv, x);
        QByteArray ct;
        QByteArray ss2;
        if (ss1.isEmpty() || ephPub.isEmpty() || !mlkemEncapsulate(pq, &ct, &ss2)) {
            wipe(ephPriv);
            wipe(ss1);
            return false;
        }
        const QString sid = forkmesh::chatv4::sessionId(ephPub, ct);
        const forkmesh::chatv4::KeyPair init =
            forkmesh::chatv4::kdfInit(room, self, recipient, spkId, sid, ss1, ss2);
        wipe(ss1);
        wipe(ss2);
        if (init.first.isEmpty()) {
            wipe(ephPriv);
            return false;
        }
        RatchetSession fresh;
        fresh.sid = sid;
        fresh.rk = init.first;
        fresh.dhsPriv = ephPriv;
        fresh.dhsPub = b64u(ephPub);
        fresh.cks = init.second;
        fresh.sqSpk = spkId;
        fresh.sqCt = ct;
        fresh.created = now;
        fresh.used = now;
        wipe(ephPriv);
        peer.sessions.append(fresh);
        peer.current = sid;
        session = &peer.sessions.last();
    } else if (session->cks.isEmpty()) {
        QByteArray dhsPriv = randomBytes(kKeyBytes);
        const QByteArray dhsPub = x25519Public(dhsPriv);
        QByteArray dhOut = x25519Shared(dhsPriv, fromB64u(session->dhr));
        QByteArray ct;
        QByteArray ss;
        if (dhOut.isEmpty() || dhsPub.isEmpty() || !mlkemEncapsulate(pq, &ct, &ss)) {
            wipe(dhsPriv);
            wipe(dhOut);
            return false;
        }
        const forkmesh::chatv4::KeyPair root = forkmesh::chatv4::kdfRk(session->rk, dhOut, ss);
        wipe(dhOut);
        wipe(ss);
        if (root.first.isEmpty()) {
            wipe(dhsPriv);
            return false;
        }
        session->rk = root.first;
        session->cks = root.second;
        session->dhsPriv = dhsPriv;
        session->dhsPub = b64u(dhsPub);
        session->sqSpk = spkId;
        session->sqCt = ct;
        wipe(dhsPriv);
    }
    if (session->sqSpk.isEmpty() || session->dhsPub.isEmpty())
        return false;
    const forkmesh::chatv4::KeyPair chain = forkmesh::chatv4::kdfCk(session->cks);
    *entry = QJsonObject{{QStringLiteral("kid"), recipient},
                         {QStringLiteral("sid"), session->sid},
                         {QStringLiteral("spk"), session->sqSpk},
                         {QStringLiteral("ct"), b64(session->sqCt)},
                         {QStringLiteral("dh"), session->dhsPub},
                         {QStringLiteral("pn"), session->pn},
                         {QStringLiteral("n"), session->ns}};
    session->cks = chain.second;
    ++session->ns;
    session->used = now;
    *mk = chain.first;
    return true;
}

} // namespace

QJsonObject RoomSeal::sealV4(const QJsonObject &plain, const QStringList *only) const
{
    const QByteArray input = compactJson(plain);
    if (input.isEmpty() || input.size() > kMaxPlainBytes)
        return {};
    const qint64 now = m_device->now();
    const QString self = m_device->kid();
    QList<TrustRecord> candidates;
    for (const TrustRecord &record : m_device->records(m_scope)) {
        if (record.state != TrustState::Approved || record.kid == self)
            continue;
        if (only && !only->contains(record.kid))
            continue;
        if (spkExp(record.spk) <= now)
            continue;
        candidates.append(record);
    }
    if (candidates.size() > kMaxRecipients) {
        std::sort(candidates.begin(), candidates.end(),
                  [](const TrustRecord &a, const TrustRecord &b) {
                      return a.lastSeen > b.lastSeen;
                  });
        candidates = candidates.mid(0, kMaxRecipients);
    }
    QMap<QString, QJsonObject> targets;
    for (const TrustRecord &record : std::as_const(candidates))
        targets.insert(record.kid, record.spk);

    const qint64 ctr = m_device->nextCounter(m_room);
    QByteArray contentKey = randomBytes(kKeyBytes);
    const QByteArray nonce = randomBytes(kNonceBytes);
    QByteArray body;
    QByteArray tag;
    if (contentKey.size() != kKeyBytes || nonce.size() != kNonceBytes ||
        !aesGcmSeal(contentKey, nonce, input,
                    forkmesh::chatv4::contentAad(m_room, self, ctr, now), &body, &tag)) {
        wipe(contentKey);
        return {};
    }
    const QString nonceText = b64(nonce);
    const QString tagText = b64(tag);
    const QHash<QString, RatchetPeer> stored = m_device->ratchetPeers(m_room);
    QHash<QString, RatchetPeer> changed;
    QJsonArray keys;
    for (auto it = targets.cbegin(); it != targets.cend(); ++it) {
        RatchetPeer peer = stored.value(it.key());
        QJsonObject entry;
        QByteArray mk;
        if (!sendEntry(m_room, self, it.key(), it.value(), now, peer, &entry, &mk))
            continue;
        const QByteArray wrapNonce = randomBytes(kNonceBytes);
        QByteArray wrapped;
        QByteArray wrapTag;
        const bool ok = aesGcmSeal(
            mk, wrapNonce, contentKey,
            forkmesh::chatv4::wrapAad(m_room, self, entry, nonceText, tagText), &wrapped,
            &wrapTag);
        wipe(mk);
        if (!ok)
            continue;
        entry.insert(QStringLiteral("nonce"), b64(wrapNonce));
        entry.insert(QStringLiteral("tag"), b64(wrapTag));
        entry.insert(QStringLiteral("key"), b64(wrapped));
        keys.append(entry);
        changed.insert(it.key(), peer);
    }
    wipe(contentKey);
    for (auto it = changed.cbegin(); it != changed.cend(); ++it)
        m_device->storeRatchetPeer(m_room, it.key(), it.value(), now);
    m_device->saveRatchets(m_room, true);
    QJsonObject frame{{QStringLiteral("type"), QStringLiteral("sealed")},
                      {QStringLiteral("v"), forkmesh::chatv4::kVersion},
                      {QStringLiteral("kid"), self},
                      {QStringLiteral("idk"), m_device->idkText()},
                      {QStringLiteral("ctr"), ctr},
                      {QStringLiteral("ts"), now},
                      {QStringLiteral("nonce"), nonceText},
                      {QStringLiteral("tag"), tagText},
                      {QStringLiteral("body"), b64(body)},
                      {QStringLiteral("keys"), keys}};
    const QByteArray sig = m_device->sign(forkmesh::chatv4::sealedMessage(m_room, frame));
    if (sig.isEmpty())
        return {};
    frame.insert(QStringLiteral("sig"), b64u(sig));
    return frame;
}

RoomSeal::Opened RoomSeal::openV4(const QJsonObject &frame, bool commit) const
{
    Opened result;
    const QJsonValue kidValue = frame.value(QStringLiteral("kid"));
    const QJsonValue idkValue = frame.value(QStringLiteral("idk"));
    const QJsonValue keysValue = frame.value(QStringLiteral("keys"));
    if (!kidValue.isString() || !idkValue.isString() || !keysValue.isArray())
        return result;
    const QString kid = kidValue.toString();
    bool ok = false;
    const QByteArray idk = fromB64u(idkValue.toString(), &ok);
    if (!ok || idk.size() != kKeyBytes || kid != id16(idk))
        return result;
    const QJsonArray keys = keysValue.toArray();
    if (keys.size() > kMaxRecipients)
        return result;
    QString previous;
    for (qsizetype i = 0; i < keys.size(); ++i) {
        if (!keys.at(i).isObject() || !forkmesh::chatv4::validEntry(keys.at(i).toObject()))
            return result;
        const QString entryKid = keys.at(i).toObject().value(QStringLiteral("kid")).toString();
        if ((i > 0 && !(previous < entryKid)) || entryKid == kid)
            return result;
        previous = entryKid;
    }
    qint64 ctr = 0;
    qint64 ts = 0;
    if (!jsonInteger(frame.value(QStringLiteral("ctr")), &ctr) || ctr < 1 ||
        ctr >= (qint64(1) << 31) || !jsonInteger(frame.value(QStringLiteral("ts")), &ts))
        return result;
    bool okNonce = false;
    bool okTag = false;
    bool okBody = false;
    bool okSig = false;
    const QByteArray nonce = fromB64(frame.value(QStringLiteral("nonce")).toString(), &okNonce);
    const QByteArray tag = fromB64(frame.value(QStringLiteral("tag")).toString(), &okTag);
    const QByteArray body = fromB64(frame.value(QStringLiteral("body")).toString(), &okBody);
    const QByteArray sig = fromB64u(frame.value(QStringLiteral("sig")).toString(), &okSig);
    if (!okNonce || !okTag || !okBody || !okSig || nonce.size() != kNonceBytes ||
        tag.size() != kTagBytes || sig.size() != kSignatureBytes ||
        body.size() > kMaxPlainBytes)
        return result;

    const QString self = m_device->kid();
    result.senderKid = kid;
    result.ctr = ctr;
    if (kid == self) {
        result.status = Status::Own;
        result.self = true;
        return result;
    }
    const TrustRecord *record = m_device->record(m_scope, kid);
    if (!record || record->state != TrustState::Approved) {
        if (commit && (!record || record->state == TrustState::Pending))
            m_device->hold(m_scope, m_room, frame);
        result.status = Status::Held;
        return result;
    }
    if (record->idk != idkValue.toString())
        return result;
    const QString acct = record->acct;
    const QString node = record->node;
    if (!ed25519Verify(idk, sig, forkmesh::chatv4::sealedMessage(m_room, frame)))
        return result;
    if (m_device->isReplay(m_room, kid, ctr)) {
        result.status = Status::Replay;
        return result;
    }
    const qint64 now = m_device->now();
    if (ts < now - kPrekeyLifeMs || ts > now + kDayMs)
        return result;
    QJsonObject entry;
    for (const QJsonValue &value : keys) {
        if (value.toObject().value(QStringLiteral("kid")).toString() == self) {
            entry = value.toObject();
            break;
        }
    }
    if (entry.isEmpty()) {
        result.status = Status::NotForMe;
        return result;
    }
    RatchetPeer peer = m_device->ratchetPeers(m_room).value(kid);
    QByteArray mk;
    const Step step = openEntry(*m_device, m_room, kid, entry, now, peer, &mk);
    if (step == Step::Replay) {
        result.status = Status::Replay;
        return result;
    }
    QByteArray contentKey;
    const bool unwrapped =
        step == Step::Ok &&
        aesGcmOpen(mk, fromB64(entry.value(QStringLiteral("nonce")).toString()),
                   fromB64(entry.value(QStringLiteral("key")).toString()),
                   fromB64(entry.value(QStringLiteral("tag")).toString()),
                   forkmesh::chatv4::wrapAad(m_room, kid, entry,
                                             frame.value(QStringLiteral("nonce")).toString(),
                                             frame.value(QStringLiteral("tag")).toString()),
                   &contentKey);
    wipe(mk);
    if (!unwrapped || contentKey.size() != kKeyBytes) {
        wipe(contentKey);
        result.status = Status::NoSession;
        return result;
    }
    QByteArray clear;
    const bool decrypted =
        aesGcmOpen(contentKey, nonce, body, tag,
                   forkmesh::chatv4::contentAad(m_room, kid, ctr, ts), &clear);
    wipe(contentKey);
    if (!decrypted)
        return result;
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(clear, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return result;
    const QJsonObject plain = document.object();
    if (plain.contains(QStringLiteral("senderId")) &&
        plain.value(QStringLiteral("senderId")).toString() != node)
        return result;
    if (commit) {
        m_device->storeRatchetPeer(m_room, kid, peer, now);
        m_device->saveRatchets(m_room, false);
        m_device->markSeen(m_room, kid, ctr);
        m_device->touch(m_scope, kid);
    }
    result.status = Status::Opened;
    result.plain = plain;
    result.senderAcct = acct;
    result.senderNode = node;
    result.ts = ts;
    return result;
}

bool RoomSeal::openedLaterThan(const QString &kid, qint64 ctr) const
{
    return isValid() && !kid.isEmpty() && ctr > 0 && ctr < m_device->seenMax(m_room, kid);
}

bool RoomSeal::claimSessionReset(const QString &kid) const
{
    return isValid() && !kid.isEmpty() &&
           m_device->claimSessionReset(m_room, kid, m_device->now());
}

void RoomSeal::resetSessions(const QString &kid) const
{
    if (isValid() && !kid.isEmpty())
        m_device->resetRatchet(m_room, kid);
}

bool RoomSeal::isResetPlain(const QJsonObject &plain)
{
    return plain.value(QStringLiteral("type")).toString() == forkmesh::chatv4::resetType();
}

void RoomSeal::useSignedTime(QJsonObject &plain, const Opened &opened)
{
    const QString type = plain.value(QStringLiteral("type")).toString();
    if (opened.ts > 0 &&
        (type == QLatin1String("chat") || type == QLatin1String("thread-reply")))
        plain.insert(QStringLiteral("ts"), double(opened.ts));
}

QJsonObject RoomSeal::unsealObject(const QJsonObject &frame) const
{
    const Opened opened = open(frame);
    return opened.status == Status::Opened ? opened.plain : QJsonObject();
}

QList<QJsonObject> RoomSeal::takeReleasedHeld() const
{
    if (!isValid())
        return {};
    const QList<QJsonObject> frames = m_device->held(m_scope, m_room);
    if (frames.isEmpty())
        return {};
    QList<QJsonObject> released;
    QList<QJsonObject> kept;
    for (const QJsonObject &frame : frames) {
        const QString kid = frame.value(QStringLiteral("kid")).toString();
        const TrustRecord *record = m_device->record(m_scope, kid);
        if (record && record->state == TrustState::Approved)
            released.append(frame);
        else if (!record || record->state == TrustState::Pending)
            kept.append(frame);
    }
    m_device->setHeld(m_scope, m_room, kept);
    return released;
}

void RoomSeal::approve(const QString &kid) const
{
    if (isValid())
        m_device->setState(m_scope, kid, TrustState::Approved);
}

void RoomSeal::block(const QString &kid) const
{
    if (isValid())
        m_device->setState(m_scope, kid, TrustState::Blocked);
}

QList<TrustRecord> RoomSeal::records() const
{
    return isValid() ? m_device->records(m_scope) : QList<TrustRecord>();
}

QStringList RoomSeal::approvedKeyIds() const
{
    QStringList out;
    for (const TrustRecord &record : records())
        if (record.state == TrustState::Approved)
            out.append(record.kid);
    return out;
}

QStringList RoomSeal::pendingKeyIds() const
{
    QStringList out;
    for (const TrustRecord &record : records())
        if (record.state == TrustState::Pending)
            out.append(record.kid);
    return out;
}

QStringList RoomSeal::keyIdsForAcct(const QString &acct) const
{
    QStringList out;
    for (const TrustRecord &record : records())
        if (record.state == TrustState::Approved && record.acct == acct)
            out.append(record.kid);
    return out;
}

QStringList RoomSeal::keyIdsForNode(const QString &node) const
{
    QStringList out;
    for (const TrustRecord &record : records())
        if (record.state == TrustState::Approved && record.node == node)
            out.append(record.kid);
    return out;
}

QString RoomSeal::nodeForKeyId(const QString &kid) const
{
    if (!isValid())
        return {};
    const TrustRecord *record = m_device->record(m_scope, kid);
    return record ? record->node : QString();
}

QString RoomSeal::acctForKeyId(const QString &kid) const
{
    if (!isValid())
        return {};
    const TrustRecord *record = m_device->record(m_scope, kid);
    return record ? record->acct : QString();
}

bool RoomSeal::isApproved(const QString &kid) const
{
    if (!isValid())
        return false;
    const TrustRecord *record = m_device->record(m_scope, kid);
    return record && record->state == TrustState::Approved;
}

QStringList RoomSeal::peerKeyIds() const
{
    QStringList out;
    const QString self = keyId();
    for (const TrustRecord &record : records())
        if (record.state == TrustState::Approved && record.kid != self)
            out.append(record.kid);
    return out;
}

SafetyNumber RoomSeal::safetyNumber() const
{
    return isValid() ? m_device->safetyNumberFor(m_scope) : SafetyNumber();
}

bool RoomSeal::isVerified() const
{
    return isValid() && m_device->isVerified(m_scope);
}

void RoomSeal::markVerified() const
{
    if (isValid())
        m_device->markVerified(m_scope);
}

bool RoomSeal::shouldPersistAdvertisement() const
{
    if (!isValid())
        return false;
    QSettings settings;
    const QString key = settingKey("chat/e2ee/announcedV3/", m_room);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 previous = settings.value(key).toLongLong();
    if (now - previous < kAnnounceEveryMs)
        return false;
    settings.setValue(key, now);
    return true;
}
