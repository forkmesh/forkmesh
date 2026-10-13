// Member-to-member tips: local key vault, amount parsing, exact transaction
// layout, and the fail-closed guards around signing. No Worker, no network.

#include "RewardPoolSigner.h"
#include "TipSigner.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <openssl/evp.h>

#include <iostream>

namespace tips = forkmesh::tips;

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition) {
        std::cout << "PASS: " << message << '\n';
    } else {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

QByteArray publicForSeed(const QByteArray &seed)
{
    EVP_PKEY *key = EVP_PKEY_new_raw_private_key(
        EVP_PKEY_ED25519, nullptr,
        reinterpret_cast<const unsigned char *>(seed.constData()), seed.size());
    QByteArray publicKey(32, Qt::Uninitialized);
    size_t size = 32;
    const bool ok =
        key && EVP_PKEY_get_raw_public_key(
                   key, reinterpret_cast<unsigned char *>(publicKey.data()),
                   &size) == 1 &&
        size == 32;
    if (key)
        EVP_PKEY_free(key);
    return ok ? publicKey : QByteArray();
}

// solana-keygen's on-disk format: a 64-element JSON byte array holding the
// 32-byte seed followed by the 32-byte public key.
QByteArray keypairJson(const QByteArray &seed, const QByteArray &publicKey)
{
    QJsonArray array;
    for (char byte : seed)
        array.append(int(quint8(byte)));
    for (char byte : publicKey)
        array.append(int(quint8(byte)));
    return QJsonDocument(array).toJson(QJsonDocument::Compact);
}

bool ed25519Verify(const QByteArray &publicKey, const QByteArray &message,
                   const QByteArray &signature)
{
    EVP_PKEY *key = EVP_PKEY_new_raw_public_key(
        EVP_PKEY_ED25519, nullptr,
        reinterpret_cast<const unsigned char *>(publicKey.constData()),
        size_t(publicKey.size()));
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    const bool ok =
        key && context &&
        EVP_DigestVerifyInit(context, nullptr, nullptr, nullptr, key) == 1 &&
        EVP_DigestVerify(
            context,
            reinterpret_cast<const unsigned char *>(signature.constData()),
            size_t(signature.size()),
            reinterpret_cast<const unsigned char *>(message.constData()),
            size_t(message.size())) == 1;
    if (context)
        EVP_MD_CTX_free(context);
    if (key)
        EVP_PKEY_free(key);
    return ok;
}

quint32 readShortVector(const QByteArray &bytes, int *offset)
{
    quint32 value = 0;
    int shift = 0;
    while (*offset < bytes.size()) {
        const quint8 byte = quint8(bytes.at((*offset)++));
        value |= quint32(byte & 0x7f) << shift;
        if (!(byte & 0x80))
            break;
        shift += 7;
    }
    return value;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    // --- amount parsing ------------------------------------------------
    quint64 lamports = 0;
    QString error;
    check(tips::parseSolAmount(QStringLiteral("0.05"), &lamports) &&
              lamports == 50000000ull,
          "0.05 SOL parses to exactly 50000000 lamports");
    check(tips::parseSolAmount(QStringLiteral("1"), &lamports) &&
              lamports == 1000000000ull,
          "a whole-number amount parses without a decimal point");
    check(tips::parseSolAmount(QStringLiteral("1.000000001"), &lamports) &&
              lamports == 1000000001ull,
          "the ninth decimal place survives (no binary float rounding)");
    check(tips::parseSolAmount(QStringLiteral(" 0.010 "), &lamports) &&
              lamports == 10000000ull,
          "surrounding whitespace and trailing zeros are tolerated");
    check(!tips::parseSolAmount(QStringLiteral("0.0000000001"), &lamports,
                                &error),
          "a tenth decimal place is rejected rather than truncated");
    check(!tips::parseSolAmount(QStringLiteral("-1"), &lamports, &error),
          "a negative amount is rejected");
    check(!tips::parseSolAmount(QStringLiteral("1e9"), &lamports, &error),
          "scientific notation is rejected");
    check(!tips::parseSolAmount(QString(), &lamports, &error),
          "an empty amount is rejected");
    check(!tips::parseSolAmount(QStringLiteral("0.0001"), &lamports, &error),
          "an amount below the dust floor is rejected");
    check(!tips::parseSolAmount(QStringLiteral("11"), &lamports, &error),
          "an amount above the ceiling is rejected");
    check(tips::formatLamports(50000000ull) == QStringLiteral("0.05 SOL") &&
              tips::formatLamports(1000000000ull) == QStringLiteral("1.0 SOL"),
          "lamports format back to a readable SOL figure");
    check(tips::minimumContributorPayoutLamports() == 1000000ull &&
              tips::maximumContributorPayoutLamports() == 25000000ull &&
              tips::contributorPayoutDailyCapLamports() == 50000000ull,
          "CFO payouts have narrow per-payment and daily limits");
    check(tips::contributorPayoutSpacingMs() >= 60000 &&
              tips::contributorPayoutFinalityPollMs() >= 20000 &&
              tips::contributorPayoutRetryDelayMs() >= 60000,
          "CFO network operations are deliberately paced");
    check(!tips::noticeResponseConfirmsFinality(true, 202, true, true),
          "an accepted signature awaiting finality is never reported as paid");
    check(tips::noticeResponseConfirmsFinality(true, 200, true, true),
          "a completed successful notice confirms finality");
    check(!tips::noticeResponseConfirmsFinality(false, 200, true, true) &&
              !tips::noticeResponseConfirmsFinality(true, 500, true, true),
          "network and relay failures never confirm payment");

    // --- vault: link, read, unlink -------------------------------------
    QTemporaryDir temporary;
    check(temporary.isValid(), "a temporary vault directory is available");
    const QString vaultPath = temporary.filePath(QStringLiteral("tip.vault"));

    QByteArray seed(32, char(0x11));
    const QByteArray publicKey = publicForSeed(seed);
    check(publicKey.size() == 32, "the test keypair derives a public key");
    const QString senderAddress = forkmesh::rewards::encodeBase58(publicKey);

    check(tips::linkedTipAddress(vaultPath).isEmpty(),
          "no wallet is linked before an import");
    check(!tips::linkTipWallet(keypairJson(seed, publicKey),
                               QStringLiteral("short"), vaultPath, nullptr,
                               &error),
          "a too-short vault passphrase is refused before anything is written");
    check(!QFileInfo::exists(vaultPath),
          "the refused import left no vault file behind");

    QString imported;
    check(tips::linkTipWallet(keypairJson(seed, publicKey),
                              QStringLiteral("correct horse battery"),
                              vaultPath, &imported, &error),
          "a solana-keygen JSON keypair imports into the local vault");
    check(imported == senderAddress,
          "the import reports the keypair's own public address");
    check(tips::linkedTipAddress(vaultPath) == senderAddress,
          "the linked address is readable without the passphrase");
    QString verifiedAddress;
    check(!tips::verifyTipWalletPassphrase(
              QStringLiteral("wrong passphrase"), vaultPath,
              &verifiedAddress, &error) && verifiedAddress.isEmpty(),
          "a wrong passphrase cannot arm CFO payouts");
    check(tips::verifyTipWalletPassphrase(
              QStringLiteral("correct horse battery"), vaultPath,
              &verifiedAddress, &error) && verifiedAddress == senderAddress,
          "the linked wallet passphrase can be verified without a transfer");

    const QByteArray vaultBytes = [&] {
        QFile file(vaultPath);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }();
    check(!vaultBytes.isEmpty() && !vaultBytes.contains(seed) &&
              !vaultBytes.contains(seed.toBase64()) &&
              !vaultBytes.contains(seed.toHex()),
          "the private seed appears nowhere in the vault file");
    // Qt reports the owner bits twice on POSIX (ReadOwner and ReadUser), so
    // assert on what must be absent: any group or world access.
    const QFileDevice::Permissions vaultPermissions =
        QFileInfo(vaultPath).permissions();
    check(vaultPermissions.testFlag(QFileDevice::ReadOwner) &&
              !(vaultPermissions &
                (QFileDevice::ReadGroup | QFileDevice::WriteGroup |
                 QFileDevice::ExeGroup | QFileDevice::ReadOther |
                 QFileDevice::WriteOther | QFileDevice::ExeOther)),
          "the vault file grants no group or world access");

    // --- transaction layout --------------------------------------------
    QByteArray recipientSeed(32, char(0x22));
    const QByteArray recipientKey = publicForSeed(recipientSeed);
    const QString recipientAddress =
        forkmesh::rewards::encodeBase58(recipientKey);
    const QByteArray blockhashBytes(32, char(0x33));
    const QString blockhash = forkmesh::rewards::encodeBase58(blockhashBytes);

    // --- reviewed contributor payout input ----------------------------
    QJsonObject reward{
        {QStringLiteral("rewardId"), QStringLiteral("reward-12345678")},
        {QStringLiteral("recipient"), QStringLiteral("alice")},
        {QStringLiteral("recipientAddress"), recipientAddress},
        {QStringLiteral("repository"), QStringLiteral("acme/widgets")},
        {QStringLiteral("sourceKind"), QStringLiteral("issue")},
        {QStringLiteral("subjectNumber"), 42},
        {QStringLiteral("amountLamports"), 3000000},
        {QStringLiteral("condition"), QStringLiteral("accepted_report")},
        {QStringLiteral("status"), QStringLiteral("ready")},
        {QStringLiteral("createdAt"), 1787000000000.0},
        {QStringLiteral("transactionSignature"), QString()},
    };
    tips::ContributorPayout payout;
    check(tips::parseContributorPayout(reward, &payout, &error) &&
              payout.rewardId == QLatin1String("reward-12345678") &&
              payout.recipientAddress == recipientAddress &&
              payout.lamports == 3000000ull && payout.subjectNumber == 42,
          "a bounded owner-reviewed contributor payout parses exactly");
    QJsonObject invalidReward = reward;
    invalidReward.insert(QStringLiteral("amountLamports"), 5000001);
    check(!tips::parseContributorPayout(invalidReward, &payout, &error),
          "an issue payout above the reporting-reward policy is rejected");
    QJsonObject pullReward = reward;
    pullReward.insert(QStringLiteral("sourceKind"), QStringLiteral("pull"));
    pullReward.insert(QStringLiteral("condition"), QStringLiteral("merged"));
    pullReward.insert(QStringLiteral("amountLamports"), 20000000);
    check(tips::parseContributorPayout(pullReward, &payout, &error) &&
              payout.lamports == 20000000ull,
          "a reviewed 0.02 SOL pull reward reaches the payout signer");
    pullReward.insert(QStringLiteral("amountLamports"), 25000001);
    check(!tips::parseContributorPayout(pullReward, &payout, &error),
          "a pull payout above the merge-reward policy is rejected");
    invalidReward = reward;
    invalidReward.insert(QStringLiteral("status"), QStringLiteral("submitted"));
    check(!tips::parseContributorPayout(invalidReward, &payout, &error),
          "a payout already submitted to the chain is never re-signed");
    invalidReward = reward;
    invalidReward.insert(QStringLiteral("condition"), QStringLiteral("merged"));
    check(!tips::parseContributorPayout(invalidReward, &payout, &error),
          "a reward condition must agree with its issue or pull source");
    invalidReward = reward;
    invalidReward.insert(QStringLiteral("recipient"), QStringLiteral("Alice!"));
    check(!tips::parseContributorPayout(invalidReward, &payout, &error),
          "an invalid contributor account cannot reach the signer");

    tips::TipTransfer transfer;
    check(tips::buildTipTransfer(senderAddress, recipientAddress, 50000000ull,
                                 blockhash, 12345, &transfer, &error),
          "a well-formed tip transfer builds");

    int offset = 0;
    check(readShortVector(transfer.unsignedTransaction, &offset) == 1 &&
              offset == 1,
          "the transaction declares exactly one signature");
    check(transfer.signatureOffset == 1 &&
              transfer.unsignedTransaction.mid(1, 64) == QByteArray(64, '\0'),
          "the signature slot is zeroed and sits right after the count");
    check(transfer.message ==
              transfer.unsignedTransaction.mid(1 + 64),
          "the signed message is the transaction body after the signature");

    const QByteArray &message = transfer.message;
    check(message.at(0) == char(1) && message.at(1) == char(0) &&
              message.at(2) == char(1),
          "the header requires one signer and one read-only unsigned key");
    offset = 3;
    check(readShortVector(message, &offset) == 3,
          "the message carries exactly three account keys");
    check(message.mid(offset, 32) == publicKey,
          "account 0 is the sender");
    check(message.mid(offset + 32, 32) == recipientKey,
          "account 1 is the recipient");
    check(message.mid(offset + 64, 32) == QByteArray(32, '\0'),
          "account 2 is the System Program");
    check(message.mid(offset + 96, 32) == blockhashBytes,
          "the recent blockhash follows the account keys");
    offset += 128;
    check(readShortVector(message, &offset) == 1,
          "there is exactly one instruction — no hidden extras");
    check(message.at(offset) == char(2),
          "the instruction's program is the System Program");
    ++offset;
    check(readShortVector(message, &offset) == 2 &&
              message.at(offset) == char(0) &&
              message.at(offset + 1) == char(1),
          "the instruction touches only the sender and the recipient");
    offset += 2;
    check(readShortVector(message, &offset) == 12,
          "the instruction data is the 12-byte System transfer payload");
    QByteArray expectedData;
    for (int index = 0; index < 4; ++index)
        expectedData.append(char((2u >> (index * 8)) & 0xff));
    for (int index = 0; index < 8; ++index)
        expectedData.append(char((50000000ull >> (index * 8)) & 0xff));
    check(message.mid(offset, 12) == expectedData,
          "the payload is SystemInstruction::Transfer for the exact amount");
    check(offset + 12 == message.size(),
          "nothing trails the single transfer instruction");

    // --- build-time guards ---------------------------------------------
    tips::TipTransfer selfTransfer;
    check(tips::buildTipTransfer(senderAddress, senderAddress, 50000000ull,
                                 blockhash, 12345, &selfTransfer, &error),
          "tipping your own address builds without duplicate account keys");
    int selfOffset = 3;
    check(readShortVector(selfTransfer.message, &selfOffset) == 2,
          "a self-tip has one wallet account and the System Program");
    check(selfTransfer.message.mid(selfOffset, 32) == publicKey,
          "the self-tip wallet account is the linked signer");
    selfOffset += 32 + 32 + 32;
    const quint32 selfInstructionCount =
        readShortVector(selfTransfer.message, &selfOffset);
    const quint8 selfProgramIndex =
        quint8(selfTransfer.message.at(selfOffset++));
    const quint32 selfAccountIndexCount =
        readShortVector(selfTransfer.message, &selfOffset);
    const quint8 selfFromIndex = quint8(selfTransfer.message.at(selfOffset++));
    const quint8 selfToIndex = quint8(selfTransfer.message.at(selfOffset++));
    check(selfInstructionCount == 1 && selfProgramIndex == 1 &&
              selfAccountIndexCount == 2 && selfFromIndex == 0 &&
              selfToIndex == 0,
          "a self-tip transfer references the same wallet on both sides");
    tips::TipTransfer rejected;
    check(!tips::buildTipTransfer(senderAddress, QStringLiteral("not-base58!"),
                                  50000000ull, blockhash, 12345, &rejected,
                                  &error),
          "a malformed recipient address is refused");
    check(!tips::buildTipTransfer(
              senderAddress,
              forkmesh::rewards::encodeBase58(QByteArray(32, '\0')),
              50000000ull, blockhash, 12345, &rejected, &error),
          "a transfer to the all-zero System Program address is refused");
    check(!tips::buildTipTransfer(senderAddress, recipientAddress, 1ull,
                                  blockhash, 12345, &rejected, &error),
          "a dust amount is refused at build time, not just at parse time");
    check(!tips::buildTipTransfer(senderAddress, recipientAddress, 50000000ull,
                                  forkmesh::rewards::encodeBase58(
                                      QByteArray(32, '\0')),
                                  12345, &rejected, &error),
          "an all-zero blockhash is refused");
    check(!tips::buildTipTransfer(senderAddress, recipientAddress, 50000000ull,
                                  blockhash, 0, &rejected, &error),
          "a zero last-valid block height is refused");

    // --- signing --------------------------------------------------------
    tips::SignedTip signedTip;
    check(!tips::signTipTransfer(transfer, QStringLiteral("wrong passphrase"),
                                 vaultPath, &signedTip, &error),
          "a wrong vault passphrase cannot sign");
    check(tips::signTipTransfer(transfer,
                                QStringLiteral("correct horse battery"),
                                vaultPath, &signedTip, &error),
          "the correct passphrase signs the reviewed transfer");
    check(signedTip.signerAddress == senderAddress,
          "the signer is the linked wallet");

    const QByteArray wire =
        QByteArray::fromBase64(signedTip.signedTransactionBase64.toLatin1());
    const QByteArray signature = wire.mid(1, 64);
    check(wire.mid(1 + 64) == transfer.message,
          "the broadcast transaction carries the reviewed message unchanged");
    check(ed25519Verify(publicKey, transfer.message, signature),
          "the signature verifies against the sender's public key");
    check(forkmesh::rewards::decodeBase58(signedTip.chainSignature, 64) ==
              signature,
          "the reported chain signature is the signature in the transaction");
    check(!signedTip.signedTransactionBase64.isEmpty() &&
              !wire.contains(seed),
          "the broadcast bytes contain no private seed");

    // A transfer whose reviewed fields disagree with its serialized bytes is
    // never signed: the signer rebuilds from the fields and compares.
    tips::TipTransfer tampered = transfer;
    tampered.lamports = 9000000000ull; // fields say 9 SOL, bytes still say 0.05
    check(!tips::signTipTransfer(tampered,
                                 QStringLiteral("correct horse battery"),
                                 vaultPath, &signedTip, &error),
          "a transfer tampered with after review is refused");

    // The vault of a different wallet cannot sign this sender's transfer.
    const QString otherVault = temporary.filePath(QStringLiteral("other.vault"));
    check(tips::linkTipWallet(keypairJson(recipientSeed, recipientKey),
                              QStringLiteral("another passphrase"), otherVault,
                              nullptr, &error),
          "a second wallet links to its own vault");
    check(!tips::signTipTransfer(transfer, QStringLiteral("another passphrase"),
                                 otherVault, &signedTip, &error),
          "a vault holding a different key cannot sign this transfer");

    // --- generate ---------------------------------------------------------
    QByteArray generatedJson;
    QString generatedAddress;
    check(tips::generateTipKeypair(&generatedJson, &generatedAddress, &error),
          "a fresh tip wallet generates on this machine");
    check(tips::isValidTipAddress(generatedAddress),
          "the generated wallet has a usable Solana address");
    check(forkmesh::rewards::poolAddressForKeyMaterial(generatedJson) ==
              generatedAddress,
          "the generated material parses back to the reported address");

    QByteArray secondJson;
    QString secondAddress;
    check(tips::generateTipKeypair(&secondJson, &secondAddress, &error) &&
              secondAddress != generatedAddress && secondJson != generatedJson,
          "each generated wallet is a different key");

    const QString generatedVault =
        temporary.filePath(QStringLiteral("generated.vault"));
    check(!tips::linkTipWallet(generatedJson, QStringLiteral("short"),
                               generatedVault, nullptr, &error),
          "a generated key still has to clear the passphrase gate");
    QString generatedLinked;
    check(tips::linkTipWallet(generatedJson, QStringLiteral("generated vault pass"),
                              generatedVault, &generatedLinked, &error) &&
              generatedLinked == generatedAddress,
          "a generated key links into a vault like any imported one");

    // The passphrase the link gate accepts must be one the vault itself
    // accepts, or a wallet vanishes between the dialog and the disk.
    const QString gateVault = temporary.filePath(QStringLiteral("gate.vault"));
    const QString shortestAllowed =
        QString(tips::minimumVaultPassphraseLength(), QLatin1Char('p'));
    check(tips::linkTipWallet(secondJson, shortestAllowed, gateVault, nullptr,
                              &error),
          "the shortest passphrase the gate allows is one the vault accepts");

    // --- export -----------------------------------------------------------
    QByteArray exportedJson;
    QString exportedBase58;
    QString exportedAddress;
    check(!tips::exportTipWalletKey(QStringLiteral("wrong passphrase"),
                                    generatedVault, &exportedJson,
                                    &exportedBase58, &exportedAddress, &error),
          "a wrong passphrase cannot export the secret key");
    check(exportedJson.isEmpty() && exportedBase58.isEmpty(),
          "the refused export handed back no private material");
    check(tips::exportTipWalletKey(QStringLiteral("generated vault pass"),
                                   generatedVault, &exportedJson,
                                   &exportedBase58, &exportedAddress, &error),
          "the vault passphrase exports the secret key");
    check(exportedAddress == generatedAddress,
          "the export reports the wallet it came from");
    check(exportedJson == generatedJson,
          "the exported JSON is byte-for-byte the generated keypair");
    check(forkmesh::rewards::decodeBase58(exportedBase58, 64).size() == 64,
          "the exported base58 form decodes to a 64-byte keypair");
    check(forkmesh::rewards::poolAddressForKeyMaterial(
              exportedBase58.toLatin1()) == generatedAddress,
          "the exported base58 key re-imports as the same wallet");

    // Round-trip: an exported key relinks into a new vault and still signs.
    const QString relinkVault =
        temporary.filePath(QStringLiteral("relink.vault"));
    QString relinked;
    check(tips::linkTipWallet(exportedJson, QStringLiteral("relinked vault pass"),
                              relinkVault, &relinked, &error) &&
              relinked == generatedAddress,
          "an exported key links back into a fresh vault as the same wallet");

    tips::TipTransfer generatedTransfer;
    tips::SignedTip generatedSigned;
    check(tips::buildTipTransfer(generatedAddress, recipientAddress, 50000000ull,
                                 blockhash, 12345, &generatedTransfer, &error) &&
              tips::signTipTransfer(generatedTransfer,
                                    QStringLiteral("relinked vault pass"),
                                    relinkVault, &generatedSigned, &error),
          "the round-tripped wallet signs a transfer");
    check(ed25519Verify(
              forkmesh::rewards::decodeBase58(generatedAddress, 32),
              generatedTransfer.message,
              forkmesh::rewards::decodeBase58(generatedSigned.chainSignature, 64)),
          "the round-tripped wallet's signature verifies");

    const QString missingVault =
        temporary.filePath(QStringLiteral("absent.vault"));
    check(!tips::exportTipWalletKey(QStringLiteral("generated vault pass"),
                                    missingVault, &exportedJson,
                                    &exportedBase58, nullptr, &error),
          "there is nothing to export when no wallet is linked");

    // --- unlink ---------------------------------------------------------
    check(tips::unlinkTipWallet(vaultPath, &error) &&
              !QFileInfo::exists(vaultPath),
          "unlinking removes the local vault file");
    check(tips::unlinkTipWallet(vaultPath, &error),
          "unlinking an already-absent vault succeeds");
    check(tips::linkedTipAddress(vaultPath).isEmpty(),
          "no wallet is linked after unlinking");

    std::cout << (failures == 0 ? "tip signer tests passed\n"
                                : "tip signer tests failed\n");
    return failures == 0 ? 0 : 1;
}
