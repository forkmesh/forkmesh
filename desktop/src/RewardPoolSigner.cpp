#include "RewardPoolSigner.h"
#include "AppPaths.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QVector>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace forkmesh::rewards {
namespace {

constexpr int kSeedBytes = 32;
constexpr int kPublicKeyBytes = 32;
constexpr int kKeypairBytes = 64;
constexpr int kSignatureBytes = 64;
constexpr int kSaltBytes = 16;
constexpr int kNonceBytes = 12;
constexpr int kTagBytes = 16;
constexpr quint64 kScryptN = 32768;
constexpr quint64 kScryptR = 8;
constexpr quint64 kScryptP = 1;
constexpr quint64 kScryptMaxMemory = 64ull * 1024 * 1024;
constexpr int kMinimumPassphraseLength = 12;
constexpr int kMaximumPassphraseLength = 1024;
constexpr qint64 kMaximumVaultBytes = 64 * 1024;
constexpr int kMaximumTransactionBytes = 1232;
constexpr quint64 kMaximumJsonInteger = 9007199254740991ull;
constexpr quint64 kLamportsPerSol = 1000000000ull;
constexpr int kMaximumWorkerTransfersPerIntent = 8;

const QString kVaultKind = QStringLiteral("forkmesh.solana.pool-key-vault");
const QString kTransferType =
    QStringLiteral("forkmesh.solana.reward-transfer-intent");
const QString kClaimType =
    QStringLiteral("forkmesh.solana.pending-claim-intent");
const QString kSystemProgram =
    QStringLiteral("11111111111111111111111111111111");

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

QByteArray randomBytes(int count)
{
    QByteArray bytes(count, Qt::Uninitialized);
    if (count <= 0 ||
        RAND_bytes(reinterpret_cast<unsigned char *>(bytes.data()), count) != 1) {
        secureErase(bytes);
        return {};
    }
    return bytes;
}

bool deriveVaultKey(const QString &passphrase, const QByteArray &salt,
                    QByteArray *key, QString *error)
{
    if (!key)
        return false;
    if (passphrase.size() < kMinimumPassphraseLength ||
        passphrase.size() > kMaximumPassphraseLength) {
        setError(error, QStringLiteral("Use a vault passphrase between %1 and "
                                       "%2 characters.")
                            .arg(kMinimumPassphraseLength)
                            .arg(kMaximumPassphraseLength));
        return false;
    }
    if (salt.size() != kSaltBytes) {
        setError(error, QStringLiteral("The encrypted vault has an invalid salt."));
        return false;
    }
    QByteArray password = passphrase.toUtf8();
    QByteArray derived(32, Qt::Uninitialized);
    const int ok = EVP_PBE_scrypt(
        password.constData(), size_t(password.size()),
        reinterpret_cast<const unsigned char *>(salt.constData()),
        size_t(salt.size()), kScryptN, kScryptR, kScryptP, kScryptMaxMemory,
        reinterpret_cast<unsigned char *>(derived.data()), size_t(derived.size()));
    secureErase(password);
    if (ok != 1) {
        secureErase(derived);
        setError(error,
                 QStringLiteral("Secure local key derivation is unavailable."));
        return false;
    }
    *key = derived;
    secureErase(derived);
    return true;
}

QByteArray vaultAad(const QString &publicAddress)
{
    return QByteArrayLiteral("forkmesh-solana-pool-vault-v1\n") +
           publicAddress.toUtf8();
}

bool encryptSeed(const QByteArray &seed, const QByteArray &key,
                 const QByteArray &nonce, const QByteArray &aad,
                 QByteArray *ciphertext, QByteArray *tag)
{
    if (!ciphertext || !tag || seed.size() != kSeedBytes || key.size() != 32 ||
        nonce.size() != kNonceBytes)
        return false;
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    QByteArray cipher(seed.size(), Qt::Uninitialized);
    QByteArray authTag(kTagBytes, Qt::Uninitialized);
    int length = 0;
    int total = 0;
    bool ok =
        ctx &&
        EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, nonce.size(), nullptr) == 1 &&
        EVP_EncryptInit_ex(
            ctx, nullptr, nullptr,
            reinterpret_cast<const unsigned char *>(key.constData()),
            reinterpret_cast<const unsigned char *>(nonce.constData())) == 1 &&
        EVP_EncryptUpdate(
            ctx, nullptr, &length,
            reinterpret_cast<const unsigned char *>(aad.constData()),
            aad.size()) == 1 &&
        EVP_EncryptUpdate(
            ctx, reinterpret_cast<unsigned char *>(cipher.data()), &length,
            reinterpret_cast<const unsigned char *>(seed.constData()),
            seed.size()) == 1;
    if (ok) {
        total = length;
        ok = EVP_EncryptFinal_ex(
                 ctx, reinterpret_cast<unsigned char *>(cipher.data()) + total,
                 &length) == 1;
        total += length;
    }
    if (ok) {
        ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, authTag.size(),
                                 authTag.data()) == 1;
    }
    if (ctx)
        EVP_CIPHER_CTX_free(ctx);
    if (!ok) {
        secureErase(cipher);
        secureErase(authTag);
        return false;
    }
    cipher.resize(total);
    *ciphertext = cipher;
    *tag = authTag;
    secureErase(cipher);
    secureErase(authTag);
    return true;
}

bool decryptSeed(const QByteArray &ciphertext, const QByteArray &tag,
                 const QByteArray &key, const QByteArray &nonce,
                 const QByteArray &aad, QByteArray *seed)
{
    if (!seed || ciphertext.size() != kSeedBytes || tag.size() != kTagBytes ||
        key.size() != 32 || nonce.size() != kNonceBytes)
        return false;
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    QByteArray plain(ciphertext.size(), Qt::Uninitialized);
    int length = 0;
    int total = 0;
    bool ok =
        ctx &&
        EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, nonce.size(), nullptr) == 1 &&
        EVP_DecryptInit_ex(
            ctx, nullptr, nullptr,
            reinterpret_cast<const unsigned char *>(key.constData()),
            reinterpret_cast<const unsigned char *>(nonce.constData())) == 1 &&
        EVP_DecryptUpdate(
            ctx, nullptr, &length,
            reinterpret_cast<const unsigned char *>(aad.constData()),
            aad.size()) == 1 &&
        EVP_DecryptUpdate(
            ctx, reinterpret_cast<unsigned char *>(plain.data()), &length,
            reinterpret_cast<const unsigned char *>(ciphertext.constData()),
            ciphertext.size()) == 1;
    if (ok) {
        total = length;
        ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, tag.size(),
                                 const_cast<char *>(tag.constData())) == 1 &&
             EVP_DecryptFinal_ex(
                 ctx, reinterpret_cast<unsigned char *>(plain.data()) + total,
                 &length) == 1;
        total += length;
    }
    if (ctx)
        EVP_CIPHER_CTX_free(ctx);
    if (!ok) {
        secureErase(plain);
        return false;
    }
    plain.resize(total);
    *seed = plain;
    secureErase(plain);
    return true;
}

QByteArray publicKeyForSeed(const QByteArray &seed)
{
    if (seed.size() != kSeedBytes)
        return {};
    EVP_PKEY *key = EVP_PKEY_new_raw_private_key(
        EVP_PKEY_ED25519, nullptr,
        reinterpret_cast<const unsigned char *>(seed.constData()), seed.size());
    if (!key)
        return {};
    QByteArray publicKey(kPublicKeyBytes, Qt::Uninitialized);
    size_t length = size_t(publicKey.size());
    const bool ok =
        EVP_PKEY_get_raw_public_key(
            key, reinterpret_cast<unsigned char *>(publicKey.data()), &length) == 1 &&
        length == kPublicKeyBytes;
    EVP_PKEY_free(key);
    if (!ok) {
        secureErase(publicKey);
        return {};
    }
    return publicKey;
}

bool parseKeyMaterial(const QByteArray &input, QByteArray *seed,
                      QByteArray *publicKey, QString *error)
{
    if (!seed || !publicKey)
        return false;
    QByteArray material = input.trimmed();
    if (material.isEmpty() || material.size() > 4096) {
        setError(error,
                 QStringLiteral("Paste one existing Solana 64-byte keypair."));
        secureErase(material);
        return false;
    }

    QByteArray keypair;
    if (material.startsWith('[')) {
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(material, &parseError);
        if (parseError.error != QJsonParseError::NoError ||
            !document.isArray() || document.array().size() != kKeypairBytes) {
            setError(error,
                     QStringLiteral("The Solana keypair JSON must contain exactly 64 bytes."));
            secureErase(material);
            return false;
        }
        keypair.resize(kKeypairBytes);
        const QJsonArray array = document.array();
        for (int index = 0; index < array.size(); ++index) {
            const QJsonValue value = array.at(index);
            const double number = value.toDouble(-1);
            if (!value.isDouble() || number < 0 || number > 255 ||
                std::floor(number) != number) {
                setError(error,
                         QStringLiteral("The Solana keypair JSON contains an invalid byte."));
                secureErase(material);
                secureErase(keypair);
                return false;
            }
            keypair[index] = char(int(number));
        }
    } else {
        keypair = decodeBase58(QString::fromLatin1(material), kKeypairBytes);
        if (keypair.size() != kKeypairBytes) {
            setError(error,
                     QStringLiteral("The Solana private key must decode to 64 bytes."));
            secureErase(material);
            secureErase(keypair);
            return false;
        }
    }
    secureErase(material);

    QByteArray parsedSeed = keypair.left(kSeedBytes);
    const QByteArray embeddedPublic = keypair.mid(kSeedBytes, kPublicKeyBytes);
    QByteArray derivedPublic = publicKeyForSeed(parsedSeed);
    const bool matches =
        derivedPublic.size() == kPublicKeyBytes &&
        CRYPTO_memcmp(derivedPublic.constData(), embeddedPublic.constData(),
                      kPublicKeyBytes) == 0;
    secureErase(keypair);
    if (!matches) {
        secureErase(parsedSeed);
        secureErase(derivedPublic);
        setError(error,
                 QStringLiteral("The Solana keypair's public key does not match its private seed."));
        return false;
    }
    *seed = parsedSeed;
    *publicKey = derivedPublic;
    secureErase(parsedSeed);
    secureErase(derivedPublic);
    return true;
}

bool ownerOnlyPermissions(const QString &path, bool directory, QString *error)
{
    QFileDevice::Permissions wanted = QFileDevice::ReadOwner |
                                      QFileDevice::WriteOwner;
    if (directory)
        wanted |= QFileDevice::ExeOwner;
    if (!QFile::setPermissions(path, wanted)) {
        setError(error,
                 QStringLiteral("Secure owner-only local storage is unavailable."));
        return false;
    }
    const QFileDevice::Permissions actual = QFileInfo(path).permissions();
    const QFileDevice::Permissions forbidden =
        QFileDevice::ReadGroup | QFileDevice::WriteGroup |
        QFileDevice::ExeGroup | QFileDevice::ReadOther |
        QFileDevice::WriteOther | QFileDevice::ExeOther;
    if ((actual & forbidden) != 0 || (actual & QFileDevice::ReadOwner) == 0 ||
        (actual & QFileDevice::WriteOwner) == 0 ||
        (directory && (actual & QFileDevice::ExeOwner) == 0)) {
        setError(error,
                 QStringLiteral("Secure owner-only local storage could not be enforced."));
        return false;
    }
    return true;
}

QJsonObject readVaultEnvelope(const QString &vaultPath, QString *error)
{
    const QFileInfo info(vaultPath);
    if (!info.exists()) {
        setError(error, QStringLiteral("No local reward-pool key vault is configured."));
        return {};
    }
    if (!info.isFile() || info.isSymLink() || info.size() <= 0 ||
        info.size() > kMaximumVaultBytes) {
        setError(error,
                 QStringLiteral("The local reward-pool key vault is unsafe or malformed."));
        return {};
    }
    if (!ownerOnlyPermissions(vaultPath, false, error))
        return {};
    QFile file(vaultPath);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error,
                 QStringLiteral("The local reward-pool key vault could not be opened."));
        return {};
    }
    const QByteArray bytes = file.read(kMaximumVaultBytes + 1);
    file.close();
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        setError(error,
                 QStringLiteral("The local reward-pool key vault is malformed."));
        return {};
    }
    return document.object();
}

bool validateVaultEnvelope(const QJsonObject &envelope, QString *error)
{
    const QJsonObject kdf = envelope.value(QStringLiteral("kdf")).toObject();
    const QJsonObject cipher =
        envelope.value(QStringLiteral("cipher")).toObject();
    const QString address =
        envelope.value(QStringLiteral("publicAddress")).toString();
    if (envelope.value(QStringLiteral("kind")).toString() != kVaultKind ||
        envelope.value(QStringLiteral("version")).toInt() != 1 ||
        kdf.value(QStringLiteral("name")).toString() != QLatin1String("scrypt") ||
        kdf.value(QStringLiteral("n")).toString() != QString::number(kScryptN) ||
        kdf.value(QStringLiteral("r")).toInt() != int(kScryptR) ||
        kdf.value(QStringLiteral("p")).toInt() != int(kScryptP) ||
        cipher.value(QStringLiteral("name")).toString() !=
            QLatin1String("aes-256-gcm") ||
        decodeBase58(address, kPublicKeyBytes).size() != kPublicKeyBytes) {
        setError(error,
                 QStringLiteral("The local reward-pool key vault has an unsupported format."));
        return false;
    }
    return true;
}

bool decryptVault(const QString &vaultPath, const QString &passphrase,
                  QByteArray *seed, QString *publicAddress, QString *error)
{
    if (!seed)
        return false;
    const QJsonObject envelope = readVaultEnvelope(vaultPath, error);
    if (envelope.isEmpty() || !validateVaultEnvelope(envelope, error))
        return false;
    const QJsonObject kdf = envelope.value(QStringLiteral("kdf")).toObject();
    const QJsonObject cipher =
        envelope.value(QStringLiteral("cipher")).toObject();
    const QString address =
        envelope.value(QStringLiteral("publicAddress")).toString();
    const QByteArray salt =
        QByteArray::fromBase64(kdf.value(QStringLiteral("salt")).toString().toLatin1());
    const QByteArray nonce =
        QByteArray::fromBase64(cipher.value(QStringLiteral("nonce")).toString().toLatin1());
    const QByteArray tag =
        QByteArray::fromBase64(cipher.value(QStringLiteral("tag")).toString().toLatin1());
    const QByteArray ciphertext =
        QByteArray::fromBase64(cipher.value(QStringLiteral("body")).toString().toLatin1());
    if (salt.size() != kSaltBytes || nonce.size() != kNonceBytes ||
        tag.size() != kTagBytes || ciphertext.size() != kSeedBytes) {
        setError(error,
                 QStringLiteral("The local reward-pool key vault is malformed."));
        return false;
    }

    QByteArray key;
    if (!deriveVaultKey(passphrase, salt, &key, error))
        return false;
    QByteArray plainSeed;
    const bool decrypted =
        decryptSeed(ciphertext, tag, key, nonce, vaultAad(address), &plainSeed);
    secureErase(key);
    if (!decrypted || plainSeed.size() != kSeedBytes) {
        secureErase(plainSeed);
        setError(error,
                 QStringLiteral("Wrong vault passphrase, or the local vault is corrupt."));
        return false;
    }
    QByteArray publicKey = publicKeyForSeed(plainSeed);
    const QString derivedAddress = encodeBase58(publicKey);
    secureErase(publicKey);
    if (derivedAddress != address) {
        secureErase(plainSeed);
        setError(error,
                 QStringLiteral("The encrypted vault failed its public-key integrity check."));
        return false;
    }
    *seed = plainSeed;
    secureErase(plainSeed);
    if (publicAddress)
        *publicAddress = address;
    return true;
}

bool containsSensitiveField(const QJsonValue &value)
{
    static const QRegularExpression prohibited(
        QStringLiteral("(?:private|secret|seed|mnemonic|keypair)"),
        QRegularExpression::CaseInsensitiveOption);
    if (value.isObject()) {
        const QJsonObject object = value.toObject();
        for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
            if (prohibited.match(it.key()).hasMatch() ||
                containsSensitiveField(it.value()))
                return true;
        }
    } else if (value.isArray()) {
        for (const QJsonValue &entry : value.toArray())
            if (containsSensitiveField(entry))
                return true;
    }
    return false;
}

bool parseUnsignedInteger(const QJsonValue &value, quint64 *number)
{
    if (!number)
        return false;
    bool ok = false;
    quint64 parsed = 0;
    if (value.isString()) {
        const QString text = value.toString();
        static const QRegularExpression digits(
            QStringLiteral("^(?:0|[1-9][0-9]{0,15})$"));
        if (!digits.match(text).hasMatch())
            return false;
        parsed = text.toULongLong(&ok);
    } else if (value.isDouble()) {
        const double raw = value.toDouble();
        ok = std::isfinite(raw) && raw >= 0 &&
             raw <= double(kMaximumJsonInteger) && std::floor(raw) == raw;
        if (ok)
            parsed = quint64(raw);
    }
    if (!ok || parsed > kMaximumJsonInteger)
        return false;
    *number = parsed;
    return true;
}

bool parseUtcTime(const QString &text, QDateTime *value)
{
    if (!value || !text.endsWith(QLatin1Char('Z')))
        return false;
    QDateTime parsed = QDateTime::fromString(text, Qt::ISODateWithMs);
    if (!parsed.isValid())
        parsed = QDateTime::fromString(text, Qt::ISODate);
    if (!parsed.isValid())
        return false;
    *value = parsed.toUTC();
    return true;
}

bool readShortVector(const QByteArray &bytes, int *offset, quint32 *value)
{
    if (!offset || !value)
        return false;
    const int start = *offset;
    quint32 result = 0;
    int shift = 0;
    for (int count = 0; count < 3; ++count) {
        if (*offset >= bytes.size())
            return false;
        const quint8 byte = quint8(bytes.at((*offset)++));
        result |= quint32(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0) {
            if (result > 65535 || (count > 0 && (byte & 0x7f) == 0)) {
                *offset = start;
                return false;
            }
            *value = result;
            return true;
        }
        shift += 7;
    }
    *offset = start;
    return false;
}

quint32 little32(const QByteArray &bytes, int offset)
{
    return quint32(quint8(bytes.at(offset))) |
           (quint32(quint8(bytes.at(offset + 1))) << 8) |
           (quint32(quint8(bytes.at(offset + 2))) << 16) |
           (quint32(quint8(bytes.at(offset + 3))) << 24);
}

quint64 little64(const QByteArray &bytes, int offset)
{
    quint64 value = 0;
    for (int index = 0; index < 8; ++index)
        value |= quint64(quint8(bytes.at(offset + index))) << (index * 8);
    return value;
}

bool parseLegacySystemTransfer(const QByteArray &transaction,
                               RewardTransferIntent *intent, QString *error)
{
    if (!intent || transaction.size() < 64 + 3 + 32 ||
        transaction.size() > kMaximumTransactionBytes) {
        setError(error, QStringLiteral("The unsigned Solana transaction has an invalid size."));
        return false;
    }
    int offset = 0;
    quint32 signatureCount = 0;
    if (!readShortVector(transaction, &offset, &signatureCount) ||
        signatureCount != 1 || offset + kSignatureBytes > transaction.size()) {
        setError(error,
                 QStringLiteral("Only one-signature Solana transfers are supported."));
        return false;
    }
    const int signatureOffset = offset;
    for (int index = 0; index < kSignatureBytes; ++index) {
        if (transaction.at(signatureOffset + index) != '\0') {
            setError(error,
                     QStringLiteral("The Worker transaction already contains a signature."));
            return false;
        }
    }
    offset += kSignatureBytes;
    const int messageOffset = offset;
    if (offset + 3 > transaction.size() ||
        (quint8(transaction.at(offset)) & 0x80) != 0) {
        setError(error,
                 QStringLiteral("Only a legacy Solana transfer message is supported."));
        return false;
    }
    const quint8 requiredSignatures = quint8(transaction.at(offset++));
    const quint8 readonlySigned = quint8(transaction.at(offset++));
    const quint8 readonlyUnsigned = quint8(transaction.at(offset++));
    if (requiredSignatures != 1 || readonlySigned != 0) {
        setError(error,
                 QStringLiteral("The transfer must require only the local pool signer."));
        return false;
    }

    quint32 accountCount = 0;
    if (!readShortVector(transaction, &offset, &accountCount) ||
        accountCount < 3 || accountCount > 32 ||
        readonlyUnsigned == 0 || readonlyUnsigned >= accountCount ||
        offset + int(accountCount * 32) + 32 > transaction.size()) {
        setError(error,
                 QStringLiteral("The Solana transfer has an invalid account table."));
        return false;
    }
    QList<QByteArray> accounts;
    accounts.reserve(int(accountCount));
    for (quint32 index = 0; index < accountCount; ++index) {
        accounts.append(transaction.mid(offset, 32));
        offset += 32;
    }
    const QByteArray blockhash = transaction.mid(offset, 32);
    offset += 32;
    if (blockhash == QByteArray(32, '\0')) {
        setError(error,
                 QStringLiteral("The Solana transfer has no recent blockhash."));
        return false;
    }

    quint32 instructionCount = 0;
    if (!readShortVector(transaction, &offset, &instructionCount) ||
        instructionCount == 0 ||
        instructionCount != quint32(intent->transfers.size()) ||
        offset >= transaction.size()) {
        setError(error,
                 QStringLiteral(
                     "The transaction instruction count does not match the reviewed plan."));
        return false;
    }
    const QString source = encodeBase58(accounts.at(0));
    if (source != intent->sourceWallet) {
        setError(error,
                 QStringLiteral(
                     "The serialized transaction has the wrong pool source."));
        return false;
    }
    for (quint32 instruction = 0; instruction < instructionCount;
         ++instruction) {
        if (offset >= transaction.size()) {
            setError(error,
                     QStringLiteral("A transfer instruction is truncated."));
            return false;
        }
        const quint8 programIndex = quint8(transaction.at(offset++));
        quint32 instructionAccountCount = 0;
        if (!readShortVector(transaction, &offset,
                             &instructionAccountCount) ||
            instructionAccountCount != 2 ||
            offset + 2 > transaction.size()) {
            setError(
                error,
                QStringLiteral(
                    "A transfer instruction has an invalid account list."));
            return false;
        }
        const quint8 sourceIndex = quint8(transaction.at(offset++));
        const quint8 destinationIndex = quint8(transaction.at(offset++));
        quint32 dataLength = 0;
        if (!readShortVector(transaction, &offset, &dataLength) ||
            dataLength != 12 ||
            offset + int(dataLength) > transaction.size()) {
            setError(
                error,
                QStringLiteral(
                    "A transfer instruction has unsupported data."));
            return false;
        }
        const QByteArray data = transaction.mid(offset, int(dataLength));
        offset += int(dataLength);
        if (programIndex >= accountCount || sourceIndex != 0 ||
            destinationIndex >= accountCount ||
            destinationIndex == sourceIndex ||
            encodeBase58(accounts.at(programIndex)) != kSystemProgram ||
            programIndex < accountCount - readonlyUnsigned ||
            destinationIndex < requiredSignatures ||
            destinationIndex >= accountCount - readonlyUnsigned ||
            little32(data, 0) != 2) {
            setError(
                error,
                QStringLiteral(
                    "The transaction contains a non-transfer instruction."));
            return false;
        }
        const RewardTransfer &expected =
            intent->transfers.at(int(instruction));
        const QString destination =
            encodeBase58(accounts.at(destinationIndex));
        const quint64 lamports = little64(data, 4);
        if (destination != expected.destinationWallet ||
            lamports != expected.lamports || lamports == 0) {
            setError(
                error,
                QStringLiteral(
                    "A serialized transfer does not match the reviewed plan."));
            return false;
        }
    }
    if (offset != transaction.size()) {
        setError(error,
                 QStringLiteral(
                     "The transaction contains trailing unreviewed data."));
        return false;
    }
    intent->recentBlockhash = encodeBase58(blockhash);
    intent->message = transaction.mid(messageOffset);
    intent->signatureOffset = signatureOffset;
    return true;
}

QByteArray decodeRequiredBase64(const QString &encoded)
{
    if (encoded.isEmpty() || encoded.size() > kMaximumTransactionBytes * 2 ||
        encoded.contains(QRegularExpression(QStringLiteral("\\s"))))
        return {};
    const QByteArray result =
        QByteArray::fromBase64(encoded.toLatin1(),
                               QByteArray::AbortOnBase64DecodingErrors);
    if (result.isEmpty())
        return {};
    QByteArray normalized = result.toBase64();
    QByteArray provided = encoded.toLatin1();
    while (provided.endsWith('='))
        provided.chop(1);
    while (normalized.endsWith('='))
        normalized.chop(1);
    return provided == normalized ? result : QByteArray();
}

bool signEd25519(const QByteArray &seed, const QByteArray &message,
                 QByteArray *signature)
{
    if (!signature || seed.size() != kSeedBytes || message.isEmpty())
        return false;
    EVP_PKEY *key = EVP_PKEY_new_raw_private_key(
        EVP_PKEY_ED25519, nullptr,
        reinterpret_cast<const unsigned char *>(seed.constData()), seed.size());
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    QByteArray output(kSignatureBytes, Qt::Uninitialized);
    size_t outputLength = size_t(output.size());
    const bool ok =
        key && context &&
        EVP_DigestSignInit(context, nullptr, nullptr, nullptr, key) == 1 &&
        EVP_DigestSign(
            context, reinterpret_cast<unsigned char *>(output.data()),
            &outputLength,
            reinterpret_cast<const unsigned char *>(message.constData()),
            size_t(message.size())) == 1 &&
        outputLength == kSignatureBytes;
    if (context)
        EVP_MD_CTX_free(context);
    if (key)
        EVP_PKEY_free(key);
    if (!ok) {
        secureErase(output);
        return false;
    }
    *signature = output;
    secureErase(output);
    return true;
}

} // namespace

void secureErase(QByteArray &bytes)
{
    if (!bytes.isEmpty()) {
        bytes.detach();
        OPENSSL_cleanse(bytes.data(), size_t(bytes.size()));
    }
    bytes.clear();
    bytes.squeeze();
}

void secureErase(QString &text)
{
    if (!text.isEmpty()) {
        text.detach();
        std::fill(text.begin(), text.end(), QChar(u'\0'));
    }
    text.clear();
    text.squeeze();
}

QString encodeBase58(const QByteArray &bytes)
{
    if (bytes.isEmpty())
        return {};
    int zeroes = 0;
    while (zeroes < bytes.size() && bytes.at(zeroes) == '\0')
        ++zeroes;
    QVector<quint8> digits((bytes.size() - zeroes) * 138 / 100 + 1);
    int length = 0;
    for (int index = zeroes; index < bytes.size(); ++index) {
        int carry = quint8(bytes.at(index));
        int used = 0;
        for (int pos = digits.size() - 1;
             (carry != 0 || used < length) && pos >= 0; --pos, ++used) {
            carry += 256 * digits[pos];
            digits[pos] = quint8(carry % 58);
            carry /= 58;
        }
        length = used;
    }
    int start = digits.size() - length;
    while (start < digits.size() && digits[start] == 0)
        ++start;
    static const char alphabet[] =
        "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    QByteArray encoded(zeroes, '1');
    for (; start < digits.size(); ++start)
        encoded.append(alphabet[digits[start]]);
    return QString::fromLatin1(encoded);
}

QByteArray decodeBase58(const QString &text, int expectedBytes)
{
    const QByteArray input = text.toLatin1();
    if (input.isEmpty() || input.size() > 128)
        return {};
    static const QByteArray alphabet(
        "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz");
    int zeroes = 0;
    while (zeroes < input.size() && input.at(zeroes) == '1')
        ++zeroes;
    QVector<quint8> bytes((input.size() - zeroes) * 733 / 1000 + 1);
    int length = 0;
    for (int index = zeroes; index < input.size(); ++index) {
        const int alphabetIndex = alphabet.indexOf(input.at(index));
        if (alphabetIndex < 0)
            return {};
        int carry = alphabetIndex;
        int used = 0;
        for (int pos = bytes.size() - 1;
             (carry != 0 || used < length) && pos >= 0; --pos, ++used) {
            carry += 58 * bytes[pos];
            bytes[pos] = quint8(carry % 256);
            carry /= 256;
        }
        if (carry != 0)
            return {};
        length = used;
    }
    int start = bytes.size() - length;
    while (start < bytes.size() && bytes[start] == 0)
        ++start;
    QByteArray decoded(zeroes, '\0');
    for (; start < bytes.size(); ++start)
        decoded.append(char(bytes[start]));
    if (expectedBytes >= 0 && decoded.size() != expectedBytes)
        return {};
    return decoded;
}

QString defaultPoolVaultPath()
{
    const QString root =
        forkmesh::paths::dataRoot();
    return root.isEmpty()
               ? QString()
               : QDir(root).filePath(QStringLiteral("reward-pool/pool-key.vault"));
}

QString poolAddressForKeyMaterial(const QByteArray &keyMaterial, QString *error)
{
    QByteArray seed;
    QByteArray publicKey;
    if (!parseKeyMaterial(keyMaterial, &seed, &publicKey, error))
        return {};
    const QString address = encodeBase58(publicKey);
    secureErase(seed);
    secureErase(publicKey);
    return address;
}

namespace {

// Encrypt one seed into a fresh owner-only vault file. Shared by the import
// path and the generate path so both produce byte-identical envelopes and
// inherit the same permission and atomic-write guarantees. The seed is taken by
// value and wiped on every path out, but QByteArray is copy-on-write: that
// wipes this function's own copy, so the caller must still erase its own.
bool writeSeedVault(QByteArray seed, const QString &address,
                    const QString &passphrase, const QString &vaultPath,
                    QString *error)
{
    const QFileInfo targetInfo(vaultPath);
    if (targetInfo.exists() &&
        (!targetInfo.isFile() || targetInfo.isSymLink())) {
        secureErase(seed);
        setError(error,
                 QStringLiteral("Refusing an unsafe reward-pool vault path."));
        return false;
    }
    const QString directoryPath = targetInfo.absolutePath();
    const QFileInfo directoryInfo(directoryPath);
    if ((directoryInfo.exists() && directoryInfo.isSymLink()) ||
        !QDir().mkpath(directoryPath) ||
        !ownerOnlyPermissions(directoryPath, true, error)) {
        secureErase(seed);
        if (error && error->isEmpty())
            *error = QStringLiteral("Secure local vault storage is unavailable.");
        return false;
    }

    QByteArray salt = randomBytes(kSaltBytes);
    QByteArray nonce = randomBytes(kNonceBytes);
    QByteArray key;
    if (salt.size() != kSaltBytes || nonce.size() != kNonceBytes ||
        !deriveVaultKey(passphrase, salt, &key, error)) {
        secureErase(seed);
        secureErase(salt);
        secureErase(nonce);
        secureErase(key);
        return false;
    }
    QByteArray ciphertext;
    QByteArray tag;
    const bool encrypted =
        encryptSeed(seed, key, nonce, vaultAad(address), &ciphertext, &tag);
    secureErase(seed);
    secureErase(key);
    if (!encrypted) {
        secureErase(salt);
        secureErase(nonce);
        setError(error,
                 QStringLiteral("Could not encrypt the local reward-pool key."));
        return false;
    }

    const QJsonObject envelope{
        {QStringLiteral("kind"), kVaultKind},
        {QStringLiteral("version"), 1},
        {QStringLiteral("publicAddress"), address},
        {QStringLiteral("createdAt"),
         QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("kdf"),
         QJsonObject{{QStringLiteral("name"), QStringLiteral("scrypt")},
                     {QStringLiteral("salt"),
                      QString::fromLatin1(salt.toBase64())},
                     {QStringLiteral("n"), QString::number(kScryptN)},
                     {QStringLiteral("r"), int(kScryptR)},
                     {QStringLiteral("p"), int(kScryptP)}}},
        {QStringLiteral("cipher"),
         QJsonObject{{QStringLiteral("name"),
                      QStringLiteral("aes-256-gcm")},
                     {QStringLiteral("nonce"),
                      QString::fromLatin1(nonce.toBase64())},
                     {QStringLiteral("tag"),
                      QString::fromLatin1(tag.toBase64())},
                     {QStringLiteral("body"),
                      QString::fromLatin1(ciphertext.toBase64())}}}};
    secureErase(salt);
    secureErase(nonce);
    secureErase(ciphertext);
    secureErase(tag);

    QSaveFile file(vaultPath);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) ||
        !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        file.cancelWriting();
        setError(error,
                 QStringLiteral("Secure local reward-pool vault creation failed."));
        return false;
    }
    const QByteArray serialized =
        QJsonDocument(envelope).toJson(QJsonDocument::Compact);
    if (file.write(serialized) != serialized.size() || !file.commit() ||
        !ownerOnlyPermissions(vaultPath, false, error)) {
        file.cancelWriting();
        QFile::remove(vaultPath);
        if (error && error->isEmpty())
            *error = QStringLiteral("Secure local reward-pool vault write failed.");
        return false;
    }
    return true;
}

} // namespace

int minimumVaultPassphraseLength()
{
    return kMinimumPassphraseLength;
}

bool importPoolKey(const QByteArray &keyMaterial, const QString &passphrase,
                   const QString &vaultPath, QString *publicAddress,
                   QString *error)
{
    if (vaultPath.trimmed().isEmpty()) {
        setError(error,
                 QStringLiteral("A secure local vault location is unavailable."));
        return false;
    }
    QByteArray seed;
    QByteArray publicKey;
    if (!parseKeyMaterial(keyMaterial, &seed, &publicKey, error))
        return false;
    const QString address = encodeBase58(publicKey);
    secureErase(publicKey);
    const bool stored = writeSeedVault(seed, address, passphrase, vaultPath,
                                       error);
    secureErase(seed);
    if (stored && publicAddress)
        *publicAddress = address;
    return stored;
}

bool generateSolanaKeypair(QByteArray *keypair, QString *error)
{
    if (!keypair)
        return false;
    QByteArray seed = randomBytes(kSeedBytes);
    if (seed.size() != kSeedBytes) {
        secureErase(seed);
        setError(error,
                 QStringLiteral("This machine's secure random source is "
                                "unavailable, so no key was generated."));
        return false;
    }
    QByteArray publicKey = publicKeyForSeed(seed);
    if (publicKey.size() != kPublicKeyBytes) {
        secureErase(seed);
        secureErase(publicKey);
        setError(error,
                 QStringLiteral("The local Ed25519 key generator is unavailable."));
        return false;
    }
    QByteArray material = seed + publicKey;
    secureErase(seed);
    secureErase(publicKey);
    *keypair = material;
    secureErase(material);
    return true;
}

bool exportVaultKeypair(const QString &vaultPath, const QString &passphrase,
                        QByteArray *keypair, QString *publicAddress,
                        QString *error)
{
    if (!keypair)
        return false;
    QByteArray seed;
    QString address;
    if (!decryptVault(vaultPath, passphrase, &seed, &address, error))
        return false;
    // decryptVault has already proved this seed derives `address`, so the
    // exported halves cannot disagree.
    QByteArray publicKey = publicKeyForSeed(seed);
    if (publicKey.size() != kPublicKeyBytes) {
        secureErase(seed);
        secureErase(publicKey);
        setError(error,
                 QStringLiteral("The local Ed25519 signer is unavailable, so "
                                "the key could not be exported."));
        return false;
    }
    QByteArray material = seed + publicKey;
    secureErase(seed);
    secureErase(publicKey);
    *keypair = material;
    secureErase(material);
    if (publicAddress)
        *publicAddress = address;
    return true;
}

QByteArray solanaKeypairJson(const QByteArray &keypair)
{
    if (keypair.size() != kKeypairBytes)
        return {};
    // Built by hand rather than through QJsonDocument: this buffer holds a
    // private key, and hand-assembly keeps the only copy one the caller can
    // wipe instead of leaving it in a JSON container's internal allocations.
    QByteArray json;
    json.reserve(4 * kKeypairBytes + 2);
    json.append('[');
    for (int index = 0; index < keypair.size(); ++index) {
        if (index)
            json.append(',');
        json.append(QByteArray::number(int(quint8(keypair.at(index)))));
    }
    json.append(']');
    return json;
}

QString solanaKeypairBase58(const QByteArray &keypair)
{
    return keypair.size() == kKeypairBytes ? encodeBase58(keypair) : QString();
}

QString addressForKeypair(const QByteArray &keypair)
{
    if (keypair.size() != kKeypairBytes)
        return {};
    QByteArray seed = keypair.left(kSeedBytes);
    QByteArray derived = publicKeyForSeed(seed);
    secureErase(seed);
    const bool matches =
        derived.size() == kPublicKeyBytes &&
        CRYPTO_memcmp(derived.constData(),
                      keypair.constData() + kSeedBytes, kPublicKeyBytes) == 0;
    const QString address = matches ? encodeBase58(derived) : QString();
    secureErase(derived);
    return address;
}

QString poolVaultPublicAddress(const QString &vaultPath, QString *error)
{
    const QJsonObject envelope = readVaultEnvelope(vaultPath, error);
    if (envelope.isEmpty() || !validateVaultEnvelope(envelope, error))
        return {};
    return envelope.value(QStringLiteral("publicAddress")).toString();
}

bool signSolanaMessageWithVault(const QString &vaultPath,
                                const QString &passphrase,
                                const QByteArray &message,
                                QString *signerAddress,
                                QByteArray *signature, QString *error)
{
    if (!signature || message.isEmpty() ||
        message.size() > kMaximumTransactionBytes) {
        setError(error,
                 QStringLiteral("There is no valid Solana message to sign."));
        return false;
    }
    QByteArray seed;
    QString address;
    if (!decryptVault(vaultPath, passphrase, &seed, &address, error))
        return false;
    QByteArray produced;
    const bool signedOk = signEd25519(seed, message, &produced);
    secureErase(seed);
    if (!signedOk || produced.size() != kSignatureBytes) {
        secureErase(produced);
        setError(error,
                 QStringLiteral("The local Solana signer could not sign the message."));
        return false;
    }
    *signature = produced;
    secureErase(produced);
    if (signerAddress)
        *signerAddress = address;
    return true;
}

bool parseRewardSigningJob(const QJsonObject &job,
                           const QString &expectedPoolAddress,
                           const QString &expectedNetwork,
                           const QDateTime &now,
                           RewardSigningJob *parsedJob, QString *error)
{
    if (!parsedJob || containsSensitiveField(job)) {
        setError(error,
                 QStringLiteral(
                     "The reward signing job contains prohibited private-key fields."));
        return false;
    }
    RewardSigningJob parsed;
    parsed.intentId = job.value(QStringLiteral("intentId")).toString();
    parsed.kind = job.value(QStringLiteral("kind")).toString();
    parsed.status = job.value(QStringLiteral("status")).toString();
    parsed.sourceWallet =
        job.value(QStringLiteral("sourceAddress")).toString();
    parsed.transactionSignature =
        job.value(QStringLiteral("transactionSignature")).toString();
    const QJsonObject plan = job.value(QStringLiteral("plan")).toObject();
    const QJsonObject custody =
        plan.value(QStringLiteral("custody")).toObject();
    const QJsonObject signing =
        plan.value(QStringLiteral("signing")).toObject();
    parsed.network = plan.value(QStringLiteral("network")).toString();
    parsed.policy = plan.value(QStringLiteral("policy")).toString();

    static const QRegularExpression intentPattern(
        QStringLiteral("^(?:[0-9a-f]{32}|[0-9a-f]{64})$"));
    if (!intentPattern.match(parsed.intentId).hasMatch() ||
        (parsed.kind != QLatin1String("community_reward_random") &&
         parsed.kind !=
             QLatin1String("community_reward_instant_distribution") &&
         parsed.kind != QLatin1String("pending_reward") &&
         parsed.kind != QLatin1String("microjob_payout")) ||
        (parsed.status != QLatin1String("pending_signature") &&
         parsed.status != QLatin1String("submitted")) ||
        job.value(QStringLiteral("custody")).toString() !=
            QLatin1String("external-local-signer") ||
        plan.value(QStringLiteral("v")).toInt() != 2 ||
        custody.value(QStringLiteral("forkMeshHoldsUserKeys")).toBool(true) ||
        !custody.value(
                    QStringLiteral("fundsRemainInSourceWalletUntilSigned"))
             .toBool(false) ||
        !signing.value(QStringLiteral("required")).toBool(false) ||
        signing.value(QStringLiteral("performed")).toBool(true) ||
        signing.value(QStringLiteral("method")).toString() !=
            QLatin1String("external-local-qt") ||
        parsed.network != expectedNetwork ||
        (parsed.network != QLatin1String("devnet") &&
         parsed.network != QLatin1String("testnet") &&
         parsed.network != QLatin1String("mainnet-beta")) ||
        parsed.sourceWallet != expectedPoolAddress ||
        plan.value(QStringLiteral("sourceAddress")).toString() !=
            parsed.sourceWallet ||
        decodeBase58(parsed.sourceWallet, kPublicKeyBytes).size() !=
            kPublicKeyBytes ||
        parsed.policy.isEmpty() || parsed.policy.size() > 100) {
        setError(error,
                 QStringLiteral(
                     "The reward job identity, custody, network, or source is invalid."));
        return false;
    }

    quint64 created = 0;
    quint64 expires = 0;
    if (!parseUnsignedInteger(job.value(QStringLiteral("createdAt")),
                              &created) ||
        !parseUnsignedInteger(job.value(QStringLiteral("expiresAt")),
                              &expires) ||
        created > quint64(std::numeric_limits<qint64>::max()) ||
        expires > quint64(std::numeric_limits<qint64>::max())) {
        setError(error,
                 QStringLiteral("The reward job has invalid timestamps."));
        return false;
    }
    parsed.createdAtMs = qint64(created);
    parsed.expiresAtMs = qint64(expires);
    const qint64 nowMs = now.toUTC().toMSecsSinceEpoch();
    if (parsed.createdAtMs <= 0 || parsed.expiresAtMs <= nowMs ||
        parsed.createdAtMs > nowMs + 5 * 60 * 1000 ||
        parsed.expiresAtMs <= parsed.createdAtMs ||
        parsed.expiresAtMs - parsed.createdAtMs >
            4 * 60 * 60 * 1000 + 5 * 60 * 1000) {
        setError(error,
                 QStringLiteral("The reward job is expired or outside its allowed window."));
        return false;
    }

    const QJsonArray transfers =
        plan.value(QStringLiteral("transfers")).toArray();
    if (transfers.isEmpty() || transfers.size() > 200) {
        setError(error,
                 QStringLiteral("The reward job has an invalid transfer count."));
        return false;
    }
    QSet<QString> destinations;
    quint64 total = 0;
    for (const QJsonValue &value : transfers) {
        if (!value.isObject()) {
            setError(error,
                     QStringLiteral("The reward job contains an invalid transfer."));
            return false;
        }
        const QJsonObject transfer = value.toObject();
        RewardTransfer parsedTransfer;
        parsedTransfer.destinationWallet =
            transfer.value(QStringLiteral("address")).toString();
        parsedTransfer.nodeId =
            transfer.value(QStringLiteral("nodeId")).toString();
        if (decodeBase58(parsedTransfer.destinationWallet,
                         kPublicKeyBytes)
                    .size() != kPublicKeyBytes ||
            parsedTransfer.destinationWallet == parsed.sourceWallet ||
            destinations.contains(parsedTransfer.destinationWallet) ||
            parsedTransfer.nodeId.size() > 160 ||
            !parseUnsignedInteger(
                transfer.value(QStringLiteral("lamports")),
                &parsedTransfer.lamports) ||
            parsedTransfer.lamports == 0 ||
            total > std::numeric_limits<quint64>::max() -
                        parsedTransfer.lamports) {
            setError(error,
                     QStringLiteral(
                         "The reward job contains an invalid, duplicate, or overflowing transfer."));
            return false;
        }
        destinations.insert(parsedTransfer.destinationWallet);
        total += parsedTransfer.lamports;
        parsed.transfers.append(parsedTransfer);
    }
    parsed.totalLamports = total;
    quint64 maximumTotal = 0;
    bool transferShapeAllowed = false;
    if (parsed.kind == QLatin1String("community_reward_random") &&
        parsed.policy == QLatin1String("randomized-eligible-mirror-v2")) {
        maximumTotal = 10 * kLamportsPerSol;
        transferShapeAllowed = parsed.transfers.size() == 1;
    } else if (
        parsed.kind ==
            QLatin1String("community_reward_instant_distribution") &&
        parsed.policy ==
            QLatin1String("instant-all-eligible-mirrors-v1")) {
        maximumTotal = 100 * kLamportsPerSol;
        transferShapeAllowed =
            parsed.transfers.size() <= kMaximumWorkerTransfersPerIntent;
    } else if (
        parsed.kind == QLatin1String("pending_reward") &&
        parsed.policy == QLatin1String("pending-reward-claim-v1")) {
        maximumTotal = 10 * kLamportsPerSol;
        transferShapeAllowed = parsed.transfers.size() == 1;
    } else if (
        parsed.kind == QLatin1String("microjob_payout") &&
        parsed.policy == QLatin1String("microjob-payout-v1")) {
        // Microjobs are fixed, tiny thank-you amounts batched at most eight
        // to a plan. The cap is deliberately far below the pool's other
        // policies: a whole batch of them should still be small, so a Worker
        // that authored a wrong plan cannot spend meaningfully before the
        // owner notices.
        maximumTotal = kLamportsPerSol / 2;
        transferShapeAllowed =
            parsed.transfers.size() <= kMaximumWorkerTransfersPerIntent;
    }
    if (!transferShapeAllowed || total == 0 || total > maximumTotal) {
        setError(
            error,
            QStringLiteral(
                "The reward job policy, transfer shape, or amount exceeds the "
                "locally supported safety contract."));
        return false;
    }
    if (parsed.status == QLatin1String("pending_signature") &&
        !parsed.transactionSignature.isEmpty()) {
        setError(error,
                 QStringLiteral(
                     "An unsigned reward job unexpectedly contains a transaction signature."));
        return false;
    }
    if (parsed.status == QLatin1String("submitted") &&
        !isValidSolanaSignature(parsed.transactionSignature)) {
        setError(error,
                 QStringLiteral(
                     "A submitted reward job has an invalid public transaction signature."));
        return false;
    }
    *parsedJob = parsed;
    return true;
}

bool buildRewardTransactionIntent(const QJsonObject &job,
                                  const QString &expectedPoolAddress,
                                  const QString &expectedNetwork,
                                  const QString &recentBlockhash,
                                  quint64 lastValidBlockHeight,
                                  const QDateTime &now,
                                  QJsonObject *intent, QString *error)
{
    if (!intent || lastValidBlockHeight == 0 ||
        lastValidBlockHeight > kMaximumJsonInteger) {
        setError(error,
                 QStringLiteral("The Solana block-height response is invalid."));
        return false;
    }
    RewardSigningJob parsed;
    if (!parseRewardSigningJob(job, expectedPoolAddress, expectedNetwork,
                               now, &parsed, error))
        return false;
    if (parsed.status != QLatin1String("pending_signature")) {
        setError(error,
                 QStringLiteral("Only a pending unsigned reward job can be signed."));
        return false;
    }
    if (parsed.transfers.size() > 20) {
        setError(
            error,
            QStringLiteral(
                "This all-node plan is too large for one safe Solana transaction; "
                "the Worker must split it into smaller signing jobs."));
        return false;
    }
    const QByteArray blockhash = decodeBase58(recentBlockhash, 32);
    if (blockhash.size() != 32 || blockhash == QByteArray(32, '\0')) {
        setError(error,
                 QStringLiteral("The Solana RPC returned an invalid recent blockhash."));
        return false;
    }
    QList<QByteArray> accounts;
    accounts.append(decodeBase58(parsed.sourceWallet, 32));
    for (const RewardTransfer &transfer : parsed.transfers)
        accounts.append(decodeBase58(transfer.destinationWallet, 32));
    accounts.append(QByteArray(32, '\0')); // System Program

    auto appendShortVector = [](QByteArray *bytes, quint32 value) {
        do {
            quint8 byte = quint8(value & 0x7f);
            value >>= 7;
            if (value)
                byte |= 0x80;
            bytes->append(char(byte));
        } while (value);
    };
    auto appendLittle = [](QByteArray *bytes, quint64 value, int count) {
        for (int index = 0; index < count; ++index)
            bytes->append(char((value >> (index * 8)) & 0xff));
    };

    QByteArray message;
    message.append(char(1)); // pool/fee payer is the only signer
    message.append(char(0)); // signer is writable
    message.append(char(1)); // System Program is the only read-only unsigned key
    appendShortVector(&message, quint32(accounts.size()));
    for (const QByteArray &account : std::as_const(accounts))
        message.append(account);
    message.append(blockhash);
    appendShortVector(&message, quint32(parsed.transfers.size()));
    const quint8 programIndex = quint8(accounts.size() - 1);
    for (int index = 0; index < parsed.transfers.size(); ++index) {
        message.append(char(programIndex));
        appendShortVector(&message, 2);
        message.append(char(0));
        message.append(char(index + 1));
        appendShortVector(&message, 12);
        appendLittle(&message, 2, 4); // SystemInstruction::Transfer
        appendLittle(&message, parsed.transfers.at(index).lamports, 8);
    }
    QByteArray transaction;
    appendShortVector(&transaction, 1);
    transaction.append(QByteArray(kSignatureBytes, '\0'));
    transaction.append(message);
    if (transaction.size() > kMaximumTransactionBytes) {
        setError(
            error,
            QStringLiteral(
                "This reward plan exceeds Solana's serialized transaction limit; "
                "the Worker must split it into smaller signing jobs."));
        return false;
    }

    QJsonArray transferArray;
    for (const RewardTransfer &transfer : std::as_const(parsed.transfers)) {
        transferArray.append(
            QJsonObject{
                {QStringLiteral("destinationWallet"),
                 transfer.destinationWallet},
                {QStringLiteral("lamports"),
                 QString::number(transfer.lamports)},
                {QStringLiteral("nodeId"), transfer.nodeId}});
    }
    const QJsonObject plan = job.value(QStringLiteral("plan")).toObject();
    QJsonObject selection =
        plan.value(QStringLiteral("selection")).toObject();
    if (selection.isEmpty() && parsed.transfers.size() == 1) {
        selection.insert(QStringLiteral("selectedNodeId"),
                         parsed.transfers.first().nodeId);
    }
    const QDateTime expiry = std::min(
        QDateTime::fromMSecsSinceEpoch(parsed.expiresAtMs).toUTC(),
        now.toUTC().addSecs(5 * 60));
    *intent = QJsonObject{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("type"), kTransferType},
        {QStringLiteral("intentId"), parsed.intentId},
        {QStringLiteral("kind"), parsed.kind},
        {QStringLiteral("policy"), parsed.policy},
        {QStringLiteral("createdAt"),
         now.toUTC().toString(Qt::ISODateWithMs)},
        {QStringLiteral("expiresAt"),
         expiry.toUTC().toString(Qt::ISODateWithMs)},
        {QStringLiteral("network"), parsed.network},
        {QStringLiteral("sourceWallet"), parsed.sourceWallet},
        {QStringLiteral("destinationWallet"),
         parsed.transfers.first().destinationWallet},
        {QStringLiteral("lamports"),
         QString::number(parsed.totalLamports)},
        {QStringLiteral("transfers"), transferArray},
        {QStringLiteral("recentBlockhash"), recentBlockhash},
        {QStringLiteral("lastValidBlockHeight"),
         QString::number(lastValidBlockHeight)},
        {QStringLiteral("unsignedTransaction"),
         QString::fromLatin1(transaction.toBase64())},
        {QStringLiteral("messageSha256"),
         QString::fromLatin1(
             QCryptographicHash::hash(message, QCryptographicHash::Sha256)
                 .toHex())},
        {QStringLiteral("selection"), selection},
        {QStringLiteral("custody"),
         QJsonObject{
             {QStringLiteral("forkMeshHoldsUserKeys"), false},
             {QStringLiteral("fundsRemainInSourceWalletUntilSigned"), true}}},
        {QStringLiteral("signing"),
         QJsonObject{{QStringLiteral("required"), true},
                     {QStringLiteral("performed"), false},
                     {QStringLiteral("method"),
                      QStringLiteral("local-encrypted-vault")}}}};
    return true;
}

bool parseRewardTransferIntent(const QJsonObject &object,
                               const QString &expectedPoolAddress,
                               const QString &expectedNetwork,
                               const QDateTime &now,
                               RewardTransferIntent *intent, QString *error)
{
    if (!intent || containsSensitiveField(object)) {
        setError(error,
                 QStringLiteral("The reward intent contains prohibited private-key fields."));
        return false;
    }
    RewardTransferIntent parsed;
    parsed.type = object.value(QStringLiteral("type")).toString();
    parsed.intentId = object.value(QStringLiteral("intentId")).toString();
    parsed.network = object.value(QStringLiteral("network")).toString();
    parsed.sourceWallet =
        object.value(QStringLiteral("sourceWallet")).toString();
    parsed.destinationWallet =
        object.value(QStringLiteral("destinationWallet")).toString();
    const QJsonObject selection =
        object.value(QStringLiteral("selection")).toObject();
    parsed.roundId = selection.value(QStringLiteral("roundId")).toString();
    parsed.selectedNodeId =
        selection.value(QStringLiteral("selectedNodeId")).toString();

    static const QRegularExpression digestPattern(
        QStringLiteral("^(?:[0-9a-f]{32}|[0-9a-f]{64})$"));
    if (object.value(QStringLiteral("schemaVersion")).toInt() != 1 ||
        (parsed.type != kTransferType && parsed.type != kClaimType) ||
        !digestPattern.match(parsed.intentId).hasMatch() ||
        (parsed.network != QLatin1String("devnet") &&
         parsed.network != QLatin1String("testnet") &&
         parsed.network != QLatin1String("mainnet-beta")) ||
        parsed.network != expectedNetwork ||
        decodeBase58(parsed.sourceWallet, kPublicKeyBytes).size() !=
            kPublicKeyBytes ||
        parsed.sourceWallet != expectedPoolAddress) {
        setError(error,
                 QStringLiteral("The reward intent identity, network, or wallet fields are invalid."));
        return false;
    }
    quint64 declaredTotal = 0;
    if (!parseUnsignedInteger(object.value(QStringLiteral("lamports")),
                              &declaredTotal) ||
        declaredTotal == 0 ||
        !parseUnsignedInteger(
            object.value(QStringLiteral("lastValidBlockHeight")),
            &parsed.lastValidBlockHeight) ||
        parsed.lastValidBlockHeight == 0) {
        setError(error,
                 QStringLiteral("The reward intent has an invalid amount or block height."));
        return false;
    }
    const QJsonArray transferValues =
        object.value(QStringLiteral("transfers")).toArray();
    if (transferValues.isEmpty()) {
        if (decodeBase58(parsed.destinationWallet, kPublicKeyBytes).size() !=
                kPublicKeyBytes ||
            parsed.destinationWallet == parsed.sourceWallet) {
            setError(error,
                     QStringLiteral("The reward intent destination is invalid."));
            return false;
        }
        parsed.transfers.append(
            RewardTransfer{parsed.destinationWallet, parsed.selectedNodeId,
                           declaredTotal});
    } else {
        if (transferValues.size() > 20) {
            setError(
                error,
                QStringLiteral(
                    "The reward intent contains too many transfers for one safe transaction."));
            return false;
        }
        QSet<QString> destinations;
        quint64 computedTotal = 0;
        for (const QJsonValue &value : transferValues) {
            if (!value.isObject()) {
                setError(error,
                         QStringLiteral("The reward intent has an invalid transfer."));
                return false;
            }
            const QJsonObject transferObject = value.toObject();
            RewardTransfer transfer;
            transfer.destinationWallet =
                transferObject.value(
                                  QStringLiteral("destinationWallet"))
                    .toString();
            transfer.nodeId =
                transferObject.value(QStringLiteral("nodeId")).toString();
            if (decodeBase58(transfer.destinationWallet,
                             kPublicKeyBytes)
                        .size() != kPublicKeyBytes ||
                transfer.destinationWallet == parsed.sourceWallet ||
                destinations.contains(transfer.destinationWallet) ||
                !parseUnsignedInteger(
                    transferObject.value(QStringLiteral("lamports")),
                    &transfer.lamports) ||
                transfer.lamports == 0 ||
                computedTotal > std::numeric_limits<quint64>::max() -
                                    transfer.lamports) {
                setError(
                    error,
                    QStringLiteral(
                        "The reward intent has an invalid, duplicate, or overflowing transfer."));
                return false;
            }
            destinations.insert(transfer.destinationWallet);
            computedTotal += transfer.lamports;
            parsed.transfers.append(transfer);
        }
        if (computedTotal != declaredTotal) {
            setError(
                error,
                QStringLiteral(
                    "The reward intent total does not match its reviewed transfers."));
            return false;
        }
        parsed.destinationWallet =
            parsed.transfers.first().destinationWallet;
    }
    parsed.lamports = declaredTotal;
    parsed.totalLamports = declaredTotal;
    if (!parseUtcTime(object.value(QStringLiteral("createdAt")).toString(),
                      &parsed.createdAt) ||
        !parseUtcTime(object.value(QStringLiteral("expiresAt")).toString(),
                      &parsed.expiresAt) ||
        parsed.expiresAt <= now.toUTC() ||
        parsed.createdAt > now.toUTC().addSecs(5 * 60) ||
        parsed.expiresAt <= parsed.createdAt ||
        parsed.createdAt.secsTo(parsed.expiresAt) > 15 * 60 ||
        parsed.createdAt < now.toUTC().addSecs(-30 * 60)) {
        setError(error,
                 QStringLiteral("The reward intent is expired or outside its signing window."));
        return false;
    }
    const QJsonObject custody =
        object.value(QStringLiteral("custody")).toObject();
    const QJsonObject signing =
        object.value(QStringLiteral("signing")).toObject();
    if (custody.value(QStringLiteral("forkMeshHoldsUserKeys")).toBool(true) ||
        !custody.value(
                    QStringLiteral("fundsRemainInSourceWalletUntilSigned"))
             .toBool(false) ||
        !signing.value(QStringLiteral("required")).toBool(false) ||
        signing.value(QStringLiteral("performed")).toBool(true)) {
        setError(error,
                 QStringLiteral("The reward intent has an invalid custody or signing declaration."));
        return false;
    }

    parsed.unsignedTransaction = decodeRequiredBase64(
        object.value(QStringLiteral("unsignedTransaction")).toString());
    if (parsed.unsignedTransaction.isEmpty() ||
        !parseLegacySystemTransfer(parsed.unsignedTransaction, &parsed,
                                   error)) {
        if (error && error->isEmpty())
            *error = QStringLiteral("The unsigned Solana transaction is invalid.");
        return false;
    }
    if (parsed.recentBlockhash !=
        object.value(QStringLiteral("recentBlockhash")).toString()) {
        setError(error,
                 QStringLiteral("The reward intent blockhash does not match its transaction."));
        return false;
    }
    const QString messageDigest =
        QString::fromLatin1(
            QCryptographicHash::hash(parsed.message,
                                     QCryptographicHash::Sha256)
                .toHex());
    if (messageDigest !=
        object.value(QStringLiteral("messageSha256")).toString()) {
        setError(error,
                 QStringLiteral("The reward intent message checksum does not match."));
        return false;
    }
    *intent = parsed;
    return true;
}

bool signRewardTransferIntent(const QJsonObject &object,
                              const QString &expectedNetwork,
                              const QString &passphrase,
                              const QString &vaultPath,
                              const QDateTime &now,
                              SignedRewardTransaction *signedTransaction,
                              QString *error)
{
    if (!signedTransaction)
        return false;
    QByteArray seed;
    QString poolAddress;
    if (!decryptVault(vaultPath, passphrase, &seed, &poolAddress, error))
        return false;
    RewardTransferIntent intent;
    if (!parseRewardTransferIntent(object, poolAddress, expectedNetwork, now,
                                   &intent, error)) {
        secureErase(seed);
        return false;
    }
    QByteArray signature;
    const bool signedOk = signEd25519(seed, intent.message, &signature);
    secureErase(seed);
    if (!signedOk || signature.size() != kSignatureBytes) {
        secureErase(signature);
        setError(error,
                 QStringLiteral("The local Solana signer could not sign the transaction."));
        return false;
    }
    QByteArray transaction = intent.unsignedTransaction;
    std::copy(signature.constBegin(), signature.constEnd(),
              transaction.begin() + intent.signatureOffset);
    SignedRewardTransaction result;
    result.intentId = intent.intentId;
    result.signerPublicKey = poolAddress;
    result.chainSignature = encodeBase58(signature);
    result.signedTransactionBase64 =
        QString::fromLatin1(transaction.toBase64());
    result.transactionSha256 =
        QString::fromLatin1(
            QCryptographicHash::hash(transaction, QCryptographicHash::Sha256)
                .toHex());
    secureErase(signature);
    secureErase(transaction);
    *signedTransaction = result;
    return true;
}

bool validatePublicRpcUrl(const QString &configured, QUrl *normalized,
                          QString *error)
{
    QUrl url(configured.trimmed());
    if (!url.isValid() || url.scheme() != QLatin1String("https") ||
        url.host().isEmpty() || !url.userInfo().isEmpty() || url.hasQuery() ||
        url.hasFragment() ||
        (!url.path().isEmpty() && url.path() != QLatin1String("/"))) {
        setError(
            error,
            QStringLiteral(
                "Configure one public HTTPS Solana RPC origin without credentials, "
                "query parameters, fragments, or API-key paths."));
        return false;
    }
    url.setPath(QStringLiteral("/"));
    if (normalized)
        *normalized = url.adjusted(QUrl::StripTrailingSlash);
    return true;
}

QByteArray signingJobsFetchCanonical(const QString &signerAccount,
                                     const QString &timestampMs)
{
    static const QRegularExpression accountPattern(
        QStringLiteral("^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$"));
    static const QRegularExpression timestampPattern(
        QStringLiteral("^(?:0|[1-9][0-9]{0,15})$"));
    const QString signer = signerAccount.trimmed().toLower();
    if (!accountPattern.match(signer).hasMatch() ||
        !timestampPattern.match(timestampMs).hasMatch())
        return {};
    return QByteArrayLiteral("forkmesh-reward-jobs-v1\n") +
           signer.toUtf8() + '\n' + timestampMs.toUtf8();
}

QByteArray signingJobSubmitCanonical(const QString &signerAccount,
                                     const QString &intentId,
                                     const QString &chainSignature,
                                     const QString &timestampMs)
{
    static const QRegularExpression accountPattern(
        QStringLiteral("^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$"));
    static const QRegularExpression intentPattern(
        QStringLiteral("^(?:[0-9a-f]{32}|[0-9a-f]{64})$"));
    static const QRegularExpression timestampPattern(
        QStringLiteral("^(?:0|[1-9][0-9]{0,15})$"));
    const QString signer = signerAccount.trimmed().toLower();
    if (!accountPattern.match(signer).hasMatch() ||
        !intentPattern.match(intentId).hasMatch() ||
        !isValidSolanaSignature(chainSignature) ||
        !timestampPattern.match(timestampMs).hasMatch())
        return {};
    return QByteArrayLiteral("forkmesh-reward-submit-v1\n") +
           signer.toUtf8() + '\n' + intentId.toUtf8() + '\n' +
           chainSignature.toUtf8() + '\n' + timestampMs.toUtf8();
}

bool isValidSolanaSignature(const QString &signature)
{
    return decodeBase58(signature, kSignatureBytes).size() == kSignatureBytes;
}

QString redactRewardText(const QString &text, const QStringList &exactSecrets)
{
    QString redacted = text.left(4096);
    redacted.remove(QChar(u'\0'));
    static const QRegularExpression controls(
        QStringLiteral("[\\x00-\\x08\\x0b\\x0c\\x0e-\\x1f\\x7f]"));
    redacted.remove(controls);
    for (const QString &secret : exactSecrets) {
        if (!secret.isEmpty())
            redacted.replace(secret, QStringLiteral("[REDACTED]"),
                             Qt::CaseSensitive);
    }
    static const QRegularExpression bearer(
        QStringLiteral("(?i)Bearer\\s+[A-Za-z0-9._~+\\-/=]+"));
    static const QRegularExpression secretJson(
        QStringLiteral(
            "(?i)(\"?(?:privateKey|secretKey|seed|mnemonic|keypair)\"?\\s*[:=]\\s*)"
            "(\"[^\"]*\"|'[^']*'|[^,}\\s]+)"));
    redacted.replace(bearer, QStringLiteral("Bearer [REDACTED]"));
    redacted.replace(secretJson, QStringLiteral("\\1[REDACTED]"));
    if (text.size() > redacted.size())
        redacted += QStringLiteral(" …");
    return redacted;
}

} // namespace forkmesh::rewards
