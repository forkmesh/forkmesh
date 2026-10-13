#include "TipSigner.h"
#include "AppPaths.h"

#include "RewardPoolSigner.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>

#include <cmath>

namespace forkmesh::tips {
namespace {

using forkmesh::rewards::decodeBase58;
using forkmesh::rewards::encodeBase58;

constexpr int kAddressBytes = 32;
constexpr int kSignatureBytes = 64;
constexpr int kMaximumTransactionBytes = 1232;
constexpr quint64 kLamportsPerSol = 1000000000ull;
// A tip is a gratuity, not a treasury transfer. The floor keeps a fat-fingered
// "0.0000000001" from burning a network fee to move dust; the ceiling is the
// last line of defence against a misplaced decimal point, and is deliberately
// low enough that it can never be mistaken for a general payments feature.
constexpr quint64 kMinimumTipLamports = 1000000ull;         // 0.001 SOL
constexpr quint64 kMaximumTipLamports = 10ull * kLamportsPerSol;
constexpr quint64 kMinimumContributorPayoutLamports = 1000000ull;
constexpr quint64 kMaximumIssueContributorPayoutLamports = 5000000ull;
constexpr quint64 kMaximumContributorPayoutLamports = 25000000ull;
constexpr quint64 kContributorPayoutDailyCapLamports = 50000000ull;
constexpr int kContributorPayoutSpacingMs = 60 * 1000;
constexpr int kContributorPayoutFinalityPollMs = 20 * 1000;
constexpr int kContributorPayoutRetryDelayMs = 60 * 1000;

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

} // namespace

quint64 minimumTipLamports()
{
    return kMinimumTipLamports;
}

quint64 maximumTipLamports()
{
    return kMaximumTipLamports;
}

quint64 minimumContributorPayoutLamports()
{
    return kMinimumContributorPayoutLamports;
}

quint64 maximumContributorPayoutLamports()
{
    return kMaximumContributorPayoutLamports;
}

quint64 contributorPayoutDailyCapLamports()
{
    return kContributorPayoutDailyCapLamports;
}

int contributorPayoutSpacingMs()
{
    return kContributorPayoutSpacingMs;
}

int contributorPayoutFinalityPollMs()
{
    return kContributorPayoutFinalityPollMs;
}

int contributorPayoutRetryDelayMs()
{
    return kContributorPayoutRetryDelayMs;
}

bool noticeResponseConfirmsFinality(bool networkOk, int httpStatus,
                                    bool responseOk, bool rewardPaid)
{
    // The relay deliberately returns {ok:true} with 202 while a signature is
    // still pending finality. Only a completed 2xx response may advance UI or
    // accounting state to paid.
    return networkOk && httpStatus >= 200 && httpStatus < 300 &&
           httpStatus != 202 && responseOk && rewardPaid;
}

bool isValidTipAddress(const QString &address)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[1-9A-HJ-NP-Za-km-z]{32,44}$"));
    const QString trimmed = address.trimmed();
    if (!pattern.match(trimmed).hasMatch())
        return false;
    const QByteArray raw = decodeBase58(trimmed, kAddressBytes);
    // The all-zero key is the System Program itself; nobody can spend from it
    // and a transfer to it is a burn.
    return raw.size() == kAddressBytes && raw != QByteArray(kAddressBytes, '\0');
}

bool parseContributorPayout(const QJsonObject &object,
                            ContributorPayout *payout, QString *error)
{
    if (!payout)
        return false;
    static const QRegularExpression rewardIdPattern(
        QStringLiteral("^reward-[a-z0-9-]{8,88}$"));
    static const QRegularExpression accountPattern(
        QStringLiteral("^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$"));
    static const QRegularExpression repositorySegmentPattern(
        QStringLiteral("^[A-Za-z0-9][A-Za-z0-9_.-]{0,99}$"));

    ContributorPayout parsed;
    parsed.rewardId =
        object.value(QStringLiteral("rewardId")).toString().trimmed();
    parsed.recipientName =
        object.value(QStringLiteral("recipient")).toString().trimmed();
    parsed.recipientAddress =
        object.value(QStringLiteral("recipientAddress")).toString().trimmed();
    parsed.repository =
        object.value(QStringLiteral("repository")).toString().trimmed();
    parsed.sourceKind =
        object.value(QStringLiteral("sourceKind")).toString().trimmed();

    if (object.value(QStringLiteral("status")).toString() !=
            QLatin1String("ready") ||
        !object.value(QStringLiteral("transactionSignature"))
             .toString()
             .trimmed()
             .isEmpty()) {
        setError(error, QStringLiteral(
                            "The contributor reward is not ready for payment."));
        return false;
    }
    if (!rewardIdPattern.match(parsed.rewardId).hasMatch() ||
        !accountPattern.match(parsed.recipientName).hasMatch()) {
        setError(error,
                 QStringLiteral("The contributor reward identity is invalid."));
        return false;
    }
    if (!isValidTipAddress(parsed.recipientAddress)) {
        setError(error, QStringLiteral(
                            "The contributor payout address is invalid."));
        return false;
    }

    const QStringList repositoryParts =
        parsed.repository.split(QLatin1Char('/'), Qt::KeepEmptyParts);
    if (repositoryParts.size() != 2 ||
        !repositorySegmentPattern.match(repositoryParts.at(0)).hasMatch() ||
        !repositorySegmentPattern.match(repositoryParts.at(1)).hasMatch()) {
        setError(error,
                 QStringLiteral("The contributor reward repository is invalid."));
        return false;
    }

    const double subjectNumber =
        object.value(QStringLiteral("subjectNumber")).toDouble(-1);
    const double amount =
        object.value(QStringLiteral("amountLamports")).toDouble(-1);
    const double createdAt =
        object.value(QStringLiteral("createdAt")).toDouble(-1);
    const quint64 maximumAmount =
        parsed.sourceKind == QLatin1String("pull")
            ? kMaximumContributorPayoutLamports
            : kMaximumIssueContributorPayoutLamports;
    if (!std::isfinite(subjectNumber) || subjectNumber < 1 ||
        subjectNumber > 999999999 || std::floor(subjectNumber) != subjectNumber ||
        !std::isfinite(amount) || std::floor(amount) != amount ||
        amount < double(kMinimumContributorPayoutLamports) ||
        amount > double(maximumAmount) ||
        !std::isfinite(createdAt) || createdAt < 0 ||
        createdAt > 9007199254740991.0 || std::floor(createdAt) != createdAt) {
        setError(error,
                 QStringLiteral("The contributor reward amount or source is invalid."));
        return false;
    }
    parsed.subjectNumber = int(subjectNumber);
    parsed.lamports = quint64(amount);
    parsed.createdAtMs = qint64(createdAt);

    const QString condition =
        object.value(QStringLiteral("condition")).toString().trimmed();
    const bool sourceMatches =
        (parsed.sourceKind == QLatin1String("issue") &&
         condition == QLatin1String("accepted_report")) ||
        (parsed.sourceKind == QLatin1String("pull") &&
         condition == QLatin1String("merged"));
    if (!sourceMatches) {
        setError(error, QStringLiteral(
                            "The contributor reward condition is invalid."));
        return false;
    }

    *payout = parsed;
    if (error)
        error->clear();
    return true;
}

bool verifyTipWalletPassphrase(const QString &passphrase,
                               const QString &vaultPath,
                               QString *publicAddress, QString *error)
{
    if (publicAddress)
        publicAddress->clear();
    if (passphrase.isEmpty()) {
        setError(error, QStringLiteral("Enter the linked wallet passphrase."));
        return false;
    }
    static const QByteArray authorizationMessage =
        QByteArrayLiteral("ForkMesh contributor payout session authorization v1");
    QString address;
    QByteArray signature;
    const bool signedOk = forkmesh::rewards::signSolanaMessageWithVault(
        vaultPath, passphrase, authorizationMessage, &address, &signature,
        error);
    const bool valid = signedOk && isValidTipAddress(address) &&
                       signature.size() == kSignatureBytes;
    forkmesh::rewards::secureErase(signature);
    if (!valid) {
        if (signedOk)
            setError(error, QStringLiteral(
                                "The linked wallet could not be verified."));
        return false;
    }
    if (publicAddress)
        *publicAddress = address;
    if (error)
        error->clear();
    return true;
}

QString defaultTipVaultPath()
{
    const QString root =
        forkmesh::paths::dataRoot();
    return root.isEmpty()
               ? QString()
               : QDir(root).filePath(QStringLiteral("tips/tip-key.vault"));
}

QString linkedTipAddress(const QString &vaultPath, QString *error)
{
    if (vaultPath.trimmed().isEmpty() || !QFileInfo::exists(vaultPath))
        return {};
    return forkmesh::rewards::poolVaultPublicAddress(vaultPath, error);
}

int minimumVaultPassphraseLength()
{
    return forkmesh::rewards::minimumVaultPassphraseLength();
}

bool linkTipWallet(const QByteArray &keyMaterial, const QString &passphrase,
                   const QString &vaultPath, QString *publicAddress,
                   QString *error)
{
    if (passphrase.size() < minimumVaultPassphraseLength()) {
        setError(error,
                 QStringLiteral(
                     "Choose a vault passphrase of at least %1 characters. It "
                     "is the only thing protecting this key on disk.")
                     .arg(minimumVaultPassphraseLength()));
        return false;
    }
    return forkmesh::rewards::importPoolKey(keyMaterial, passphrase, vaultPath,
                                            publicAddress, error);
}

bool generateTipKeypair(QByteArray *keypairJson, QString *publicAddress,
                        QString *error)
{
    if (!keypairJson)
        return false;
    QByteArray keypair;
    if (!forkmesh::rewards::generateSolanaKeypair(&keypair, error))
        return false;
    QByteArray json = forkmesh::rewards::solanaKeypairJson(keypair);
    const QString address = forkmesh::rewards::addressForKeypair(keypair);
    forkmesh::rewards::secureErase(keypair);
    if (json.isEmpty() || !isValidTipAddress(address)) {
        forkmesh::rewards::secureErase(json);
        setError(error,
                 QStringLiteral("The generated wallet failed its own key check "
                                "and was discarded."));
        return false;
    }
    *keypairJson = json;
    forkmesh::rewards::secureErase(json);
    if (publicAddress)
        *publicAddress = address;
    return true;
}

bool exportTipWalletKey(const QString &passphrase, const QString &vaultPath,
                        QByteArray *keypairJson, QString *base58SecretKey,
                        QString *publicAddress, QString *error)
{
    if (!keypairJson && !base58SecretKey)
        return false;
    if (vaultPath.trimmed().isEmpty() || !QFileInfo::exists(vaultPath)) {
        setError(error,
                 QStringLiteral("No tip wallet is linked on this machine, so "
                                "there is no key to export."));
        return false;
    }
    QByteArray keypair;
    QString address;
    if (!forkmesh::rewards::exportVaultKeypair(vaultPath, passphrase, &keypair,
                                               &address, error))
        return false;
    QByteArray json = forkmesh::rewards::solanaKeypairJson(keypair);
    QString base58 = forkmesh::rewards::solanaKeypairBase58(keypair);
    forkmesh::rewards::secureErase(keypair);
    if (json.isEmpty() || base58.isEmpty()) {
        forkmesh::rewards::secureErase(json);
        forkmesh::rewards::secureErase(base58);
        setError(error,
                 QStringLiteral("The linked wallet could not be encoded for "
                                "export."));
        return false;
    }
    if (keypairJson)
        *keypairJson = json;
    if (base58SecretKey)
        *base58SecretKey = base58;
    forkmesh::rewards::secureErase(json);
    forkmesh::rewards::secureErase(base58);
    if (publicAddress)
        *publicAddress = address;
    return true;
}

bool unlinkTipWallet(const QString &vaultPath, QString *error)
{
    if (vaultPath.trimmed().isEmpty() || !QFileInfo::exists(vaultPath))
        return true;
    if (QFile::remove(vaultPath))
        return true;
    setError(error,
             QStringLiteral("Could not remove the local tip-wallet vault."));
    return false;
}

QString formatLamports(quint64 lamports)
{
    const quint64 whole = lamports / kLamportsPerSol;
    QString fraction = QString::number(lamports % kLamportsPerSol)
                           .rightJustified(9, QLatin1Char('0'));
    while (fraction.endsWith(QLatin1Char('0')) && fraction.size() > 1)
        fraction.chop(1);
    return QStringLiteral("%1.%2 SOL").arg(whole).arg(fraction);
}

bool parseSolAmount(const QString &text, quint64 *lamports, QString *error)
{
    if (!lamports)
        return false;
    // Parsed by hand rather than through a double: 0.1 SOL is not exactly
    // representable in binary floating point, and a tip is denominated in
    // integer lamports.
    static const QRegularExpression pattern(
        QStringLiteral("^([0-9]{1,6})(?:\\.([0-9]{1,9}))?$"));
    const auto match = pattern.match(text.trimmed());
    if (!match.hasMatch()) {
        setError(error,
                 QStringLiteral(
                     "Enter an amount in SOL, with at most nine decimal places "
                     "(for example 0.05)."));
        return false;
    }
    // The regex bounds both halves (<= 6 whole digits, <= 9 fractional), so
    // this arithmetic cannot overflow and the range checks below own every
    // rejection message.
    const quint64 whole = match.captured(1).toULongLong();
    const quint64 fraction =
        match.captured(2).leftJustified(9, QLatin1Char('0')).toULongLong();
    const quint64 total = whole * kLamportsPerSol + fraction;
    if (total < kMinimumTipLamports) {
        setError(error, QStringLiteral("The smallest tip is %1.")
                            .arg(formatLamports(kMinimumTipLamports)));
        return false;
    }
    if (total > kMaximumTipLamports) {
        setError(error, QStringLiteral("The largest tip is %1.")
                            .arg(formatLamports(kMaximumTipLamports)));
        return false;
    }
    *lamports = total;
    return true;
}

bool buildTipTransfer(const QString &fromAddress, const QString &toAddress,
                      quint64 lamports, const QString &recentBlockhash,
                      quint64 lastValidBlockHeight, TipTransfer *transfer,
                      QString *error)
{
    if (!transfer)
        return false;
    const QString from = fromAddress.trimmed();
    const QString to = toAddress.trimmed();
    if (!isValidTipAddress(from)) {
        setError(error,
                 QStringLiteral("The linked tip wallet's address is not usable."));
        return false;
    }
    if (!isValidTipAddress(to)) {
        setError(error,
                 QStringLiteral(
                     "That member's payout address is not a valid Solana address."));
        return false;
    }
    if (lamports < kMinimumTipLamports || lamports > kMaximumTipLamports) {
        setError(error, QStringLiteral("A tip must be between %1 and %2.")
                            .arg(formatLamports(kMinimumTipLamports),
                                 formatLamports(kMaximumTipLamports)));
        return false;
    }
    if (lastValidBlockHeight == 0) {
        setError(error,
                 QStringLiteral("The Solana RPC returned an invalid block height."));
        return false;
    }
    const QByteArray blockhash = decodeBase58(recentBlockhash.trimmed(), 32);
    if (blockhash.size() != 32 || blockhash == QByteArray(32, '\0')) {
        setError(error,
                 QStringLiteral("The Solana RPC returned an invalid recent blockhash."));
        return false;
    }

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

    // account[0] = sender/recipient (signer, writable) for a self-tip, or
    // sender (signer, writable) and recipient (writable) otherwise. The
    // System Program is the final read-only, unsigned account in either case.
    const bool selfTransfer = from == to;
    QByteArray message;
    message.append(char(1)); // one required signature: the sender/fee payer
    message.append(char(0)); // no read-only signed accounts
    message.append(char(1)); // System Program is the only read-only unsigned key
    appendShortVector(&message, selfTransfer ? 2 : 3);
    message.append(decodeBase58(from, 32));
    if (!selfTransfer)
        message.append(decodeBase58(to, 32));
    message.append(QByteArray(32, '\0')); // System Program
    message.append(blockhash);
    appendShortVector(&message, 1); // exactly one instruction
    message.append(char(selfTransfer ? 1 : 2)); // program id index -> System Program
    appendShortVector(&message, 2); // account indexes: sender, recipient
    message.append(char(0));
    message.append(char(selfTransfer ? 0 : 1));
    appendShortVector(&message, 12); // instruction data length
    appendLittle(&message, 2, 4);    // SystemInstruction::Transfer
    appendLittle(&message, lamports, 8);

    QByteArray transaction;
    appendShortVector(&transaction, 1);
    const int signatureOffset = transaction.size();
    transaction.append(QByteArray(kSignatureBytes, '\0'));
    transaction.append(message);
    if (transaction.size() > kMaximumTransactionBytes) {
        setError(error,
                 QStringLiteral("The transaction exceeded Solana's size limit."));
        return false;
    }

    transfer->fromAddress = from;
    transfer->toAddress = to;
    transfer->recentBlockhash = recentBlockhash.trimmed();
    transfer->lamports = lamports;
    transfer->lastValidBlockHeight = lastValidBlockHeight;
    transfer->message = message;
    transfer->unsignedTransaction = transaction;
    transfer->signatureOffset = signatureOffset;
    return true;
}

bool signTipTransfer(const TipTransfer &transfer, const QString &passphrase,
                     const QString &vaultPath, SignedTip *signedTip,
                     QString *error)
{
    if (!signedTip)
        return false;
    // Re-derive the transaction from the reviewed fields rather than trusting
    // the caller's buffers: whatever is signed is exactly what was described.
    TipTransfer rebuilt;
    if (!buildTipTransfer(transfer.fromAddress, transfer.toAddress,
                          transfer.lamports, transfer.recentBlockhash,
                          transfer.lastValidBlockHeight, &rebuilt, error))
        return false;
    if (rebuilt.message != transfer.message ||
        rebuilt.unsignedTransaction != transfer.unsignedTransaction ||
        rebuilt.signatureOffset != transfer.signatureOffset) {
        setError(error,
                 QStringLiteral(
                     "The transfer changed after it was reviewed; nothing was signed."));
        return false;
    }

    QString signerAddress;
    QByteArray signature;
    if (!forkmesh::rewards::signSolanaMessageWithVault(
            vaultPath, passphrase, rebuilt.message, &signerAddress, &signature,
            error))
        return false;
    if (signerAddress != rebuilt.fromAddress) {
        forkmesh::rewards::secureErase(signature);
        setError(error,
                 QStringLiteral(
                     "The linked tip wallet is not the sender of this transfer."));
        return false;
    }

    QByteArray wire = rebuilt.unsignedTransaction;
    std::copy(signature.constBegin(), signature.constEnd(),
              wire.begin() + rebuilt.signatureOffset);
    signedTip->signerAddress = signerAddress;
    signedTip->chainSignature = encodeBase58(signature);
    signedTip->signedTransactionBase64 = QString::fromLatin1(wire.toBase64());
    forkmesh::rewards::secureErase(signature);
    forkmesh::rewards::secureErase(wire);
    return true;
}

} // namespace forkmesh::tips
