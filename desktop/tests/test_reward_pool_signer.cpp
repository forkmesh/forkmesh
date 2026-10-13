#include "RewardPoolSigner.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <openssl/evp.h>

#include <cmath>
#include <iostream>

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
                   key,
                   reinterpret_cast<unsigned char *>(publicKey.data()),
                   &size) == 1 &&
        size == 32;
    if (key)
        EVP_PKEY_free(key);
    return ok ? publicKey : QByteArray();
}

QByteArray shortVector(quint32 value)
{
    QByteArray encoded;
    do {
        quint8 byte = quint8(value & 0x7f);
        value >>= 7;
        if (value)
            byte |= 0x80;
        encoded.append(char(byte));
    } while (value);
    return encoded;
}

void appendLittle32(QByteArray &bytes, quint32 value)
{
    for (int index = 0; index < 4; ++index)
        bytes.append(char((value >> (index * 8)) & 0xff));
}

void appendLittle64(QByteArray &bytes, quint64 value)
{
    for (int index = 0; index < 8; ++index)
        bytes.append(char((value >> (index * 8)) & 0xff));
}

struct Fixture {
    QByteArray seed;
    QByteArray publicKey;
    QByteArray keypair;
    QString source;
    QString destination;
    QString blockhash;
    QByteArray transaction;
    QByteArray message;
    int instructionCountOffset = -1;
    quint64 lamports = 12500000;
};

Fixture makeFixture()
{
    Fixture fixture;
    fixture.seed.resize(32);
    for (int index = 0; index < fixture.seed.size(); ++index)
        fixture.seed[index] = char(index + 1);
    fixture.publicKey = publicForSeed(fixture.seed);
    fixture.keypair = fixture.seed + fixture.publicKey;
    fixture.source = forkmesh::rewards::encodeBase58(fixture.publicKey);

    QByteArray destinationBytes(32, Qt::Uninitialized);
    QByteArray blockhashBytes(32, Qt::Uninitialized);
    for (int index = 0; index < 32; ++index) {
        destinationBytes[index] = char(0xa0 + index);
        blockhashBytes[index] = char(0x40 + index);
    }
    fixture.destination =
        forkmesh::rewards::encodeBase58(destinationBytes);
    fixture.blockhash = forkmesh::rewards::encodeBase58(blockhashBytes);

    QByteArray message;
    message.append(char(1)); // one required signature
    message.append(char(0)); // no read-only signed accounts
    message.append(char(1)); // System Program is read-only
    message += shortVector(3);
    message += fixture.publicKey;
    message += destinationBytes;
    message += QByteArray(32, '\0'); // System Program address
    message += blockhashBytes;
    fixture.instructionCountOffset = 1 + 64 + message.size();
    message += shortVector(1);
    message.append(char(2)); // System Program account index
    message += shortVector(2);
    message.append(char(0)); // pool/source
    message.append(char(1)); // recipient
    message += shortVector(12);
    appendLittle32(message, 2); // SystemInstruction::Transfer
    appendLittle64(message, fixture.lamports);
    fixture.message = message;
    fixture.transaction = shortVector(1) + QByteArray(64, '\0') + message;
    return fixture;
}

QJsonObject makeIntent(const Fixture &fixture, const QDateTime &now)
{
    return {
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("type"),
         QStringLiteral("forkmesh.solana.reward-transfer-intent")},
        {QStringLiteral("intentId"), QString(64, QLatin1Char('a'))},
        {QStringLiteral("createdAt"),
         now.addSecs(-5).toUTC().toString(Qt::ISODateWithMs)},
        {QStringLiteral("expiresAt"),
         now.addSecs(300).toUTC().toString(Qt::ISODateWithMs)},
        {QStringLiteral("network"), QStringLiteral("devnet")},
        {QStringLiteral("sourceWallet"), fixture.source},
        {QStringLiteral("destinationWallet"), fixture.destination},
        {QStringLiteral("lamports"), QString::number(fixture.lamports)},
        {QStringLiteral("recentBlockhash"), fixture.blockhash},
        {QStringLiteral("lastValidBlockHeight"), QStringLiteral("123456")},
        {QStringLiteral("unsignedTransaction"),
         QString::fromLatin1(fixture.transaction.toBase64())},
        {QStringLiteral("messageSha256"),
         QString::fromLatin1(
             QCryptographicHash::hash(fixture.message,
                                      QCryptographicHash::Sha256)
                 .toHex())},
        {QStringLiteral("selection"),
         QJsonObject{{QStringLiteral("roundId"), QStringLiteral("round-7")},
                     {QStringLiteral("selectedNodeId"),
                      QStringLiteral("node-4")}}},
        {QStringLiteral("custody"),
         QJsonObject{
             {QStringLiteral("forkMeshHoldsUserKeys"), false},
             {QStringLiteral("fundsRemainInSourceWalletUntilSigned"), true}}},
        {QStringLiteral("signing"),
         QJsonObject{{QStringLiteral("required"), true},
                     {QStringLiteral("performed"), false},
                     {QStringLiteral("method"), QStringLiteral("external")}}},
    };
}

QJsonObject makeSigningJob(const Fixture &fixture, const QDateTime &now,
                           int transferCount = 1)
{
    QJsonArray transfers;
    transfers.append(
        QJsonObject{{QStringLiteral("address"), fixture.destination},
                    {QStringLiteral("lamports"),
                     QString::number(fixture.lamports)},
                    {QStringLiteral("nodeId"), QStringLiteral("node-4")}});
    if (transferCount > 1) {
        QByteArray secondBytes(32, Qt::Uninitialized);
        for (int index = 0; index < 32; ++index)
            secondBytes[index] = char(0x20 + index);
        transfers.append(
            QJsonObject{
                {QStringLiteral("address"),
                 forkmesh::rewards::encodeBase58(secondBytes)},
                {QStringLiteral("lamports"), QStringLiteral("5000")},
                {QStringLiteral("nodeId"), QStringLiteral("node-5")}});
    }
    return {
        {QStringLiteral("intentId"), QString(32, QLatin1Char('c'))},
        {QStringLiteral("kind"),
         QStringLiteral("community_reward_random")},
        {QStringLiteral("sourceAddress"), fixture.source},
        {QStringLiteral("status"), QStringLiteral("pending_signature")},
        {QStringLiteral("createdAt"),
         QString::number(now.addSecs(-5).toMSecsSinceEpoch())},
        {QStringLiteral("expiresAt"),
         QString::number(now.addSecs(3600).toMSecsSinceEpoch())},
        {QStringLiteral("transactionSignature"), QString()},
        {QStringLiteral("completedAt"), 0},
        {QStringLiteral("custody"),
         QStringLiteral("external-local-signer")},
        {QStringLiteral("plan"),
         QJsonObject{
             {QStringLiteral("v"), 2},
             {QStringLiteral("network"), QStringLiteral("devnet")},
             {QStringLiteral("sourceAddress"), fixture.source},
             {QStringLiteral("policy"),
              QStringLiteral("randomized-eligible-mirror-v2")},
             {QStringLiteral("custody"),
              QJsonObject{
                  {QStringLiteral("forkMeshHoldsUserKeys"), false},
                  {QStringLiteral("fundsRemainInSourceWalletUntilSigned"),
                   true}}},
             {QStringLiteral("signing"),
              QJsonObject{{QStringLiteral("required"), true},
                          {QStringLiteral("performed"), false},
                          {QStringLiteral("method"),
                           QStringLiteral("external-local-qt")}}},
             {QStringLiteral("transfers"), transfers},
             {QStringLiteral("selection"),
              QJsonObject{{QStringLiteral("selectedNodeId"),
                           QStringLiteral("node-4")}}}}},
    };
}

bool verifySignature(const QByteArray &publicKey, const QByteArray &message,
                     const QByteArray &signature)
{
    EVP_PKEY *key = EVP_PKEY_new_raw_public_key(
        EVP_PKEY_ED25519, nullptr,
        reinterpret_cast<const unsigned char *>(publicKey.constData()),
        publicKey.size());
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    const bool ok =
        key && context &&
        EVP_DigestVerifyInit(context, nullptr, nullptr, nullptr, key) == 1 &&
        EVP_DigestVerify(
            context,
            reinterpret_cast<const unsigned char *>(signature.constData()),
            signature.size(),
            reinterpret_cast<const unsigned char *>(message.constData()),
            message.size()) == 1;
    if (context)
        EVP_MD_CTX_free(context);
    if (key)
        EVP_PKEY_free(key);
    return ok;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ForkMeshTests"));
    QCoreApplication::setApplicationName(QStringLiteral("RewardPoolSigner"));

    const Fixture fixture = makeFixture();
    check(fixture.publicKey.size() == 32,
          "test fixture derives an Ed25519 public key");
    check(forkmesh::rewards::decodeBase58(fixture.source, 32) ==
              fixture.publicKey,
          "Solana base58 address round-trips");

    const QString keypairBase58 =
        forkmesh::rewards::encodeBase58(fixture.keypair);
    QString error;
    check(forkmesh::rewards::poolAddressForKeyMaterial(
              keypairBase58.toLatin1(), &error) == fixture.source,
          "base58 64-byte Solana keypair is accepted");
    check(forkmesh::rewards::poolAddressForKeyMaterial(
              forkmesh::rewards::encodeBase58(fixture.seed).toLatin1(),
              &error)
              .isEmpty(),
          "ambiguous 32-byte private seed is rejected");

    QJsonArray keyArray;
    for (char byte : fixture.keypair)
        keyArray.append(int(quint8(byte)));
    const QByteArray keyJson =
        QJsonDocument(keyArray).toJson(QJsonDocument::Compact);
    check(forkmesh::rewards::poolAddressForKeyMaterial(keyJson, &error) ==
              fixture.source,
          "Solana CLI JSON keypair is accepted");

    QTemporaryDir temporary;
    check(temporary.isValid(), "temporary vault directory is available");
    const QString vaultPath =
        temporary.filePath(QStringLiteral("private/pool-key.vault"));
    QString importedAddress;
    check(forkmesh::rewards::importPoolKey(
              keyJson, QStringLiteral("correct horse battery staple"),
              vaultPath, &importedAddress, &error),
          "existing pool key imports into encrypted local vault");
    check(importedAddress == fixture.source,
          "vault import exposes only the derived public pool address");
    check(QFileInfo(vaultPath).permissions() &
              (QFileDevice::ReadOwner | QFileDevice::WriteOwner),
          "vault remains owner-readable and owner-writable");
    QFile vault(vaultPath);
    check(vault.open(QIODevice::ReadOnly), "encrypted vault can be inspected");
    const QByteArray vaultBytes = vault.readAll();
    check(!vaultBytes.contains(keyJson) &&
              !vaultBytes.contains(keypairBase58.toLatin1()),
          "encrypted vault contains no plaintext imported key");
    check(forkmesh::rewards::poolVaultPublicAddress(vaultPath, &error) ==
              fixture.source,
          "vault status reads only its public address");

    const QDateTime now =
        QDateTime::fromString(QStringLiteral("2026-07-23T16:00:00.000Z"),
                              Qt::ISODateWithMs);
    QJsonObject intentObject = makeIntent(fixture, now);
    forkmesh::rewards::RewardTransferIntent intent;
    check(forkmesh::rewards::parseRewardTransferIntent(
              intentObject, fixture.source, QStringLiteral("devnet"), now,
              &intent, &error),
          "exact legacy System Program transfer intent validates");
    check(intent.lamports == fixture.lamports &&
              intent.destinationWallet == fixture.destination &&
              intent.message == fixture.message,
          "validated intent exposes the exact displayed transfer");

    QJsonObject signingJob = makeSigningJob(fixture, now, 2);
    signingJob[QStringLiteral("kind")] =
        QStringLiteral("community_reward_instant_distribution");
    QJsonObject signingJobPlan =
        signingJob.value(QStringLiteral("plan")).toObject();
    signingJobPlan[QStringLiteral("policy")] =
        QStringLiteral("instant-all-eligible-mirrors-v1");
    signingJob[QStringLiteral("plan")] = signingJobPlan;
    forkmesh::rewards::RewardSigningJob parsedJob;
    check(forkmesh::rewards::parseRewardSigningJob(
              signingJob, fixture.source, QStringLiteral("devnet"), now,
              &parsedJob, &error) &&
              parsedJob.transfers.size() == 2 &&
              parsedJob.totalLamports == fixture.lamports + 5000,
          "existing Worker signing-job plan validates before transaction creation");
    QJsonObject microjob = signingJob;
    microjob[QStringLiteral("kind")] = QStringLiteral("microjob_payout");
    QJsonObject microjobPlan =
        microjob.value(QStringLiteral("plan")).toObject();
    microjobPlan[QStringLiteral("policy")] =
        QStringLiteral("microjob-payout-v1");
    microjob[QStringLiteral("plan")] = microjobPlan;
    check(forkmesh::rewards::parseRewardSigningJob(
              microjob, fixture.source, QStringLiteral("devnet"), now,
              &parsedJob, &error) &&
              parsedJob.kind == QLatin1String("microjob_payout") &&
              parsedJob.transfers.size() == 2,
          "verified microjob payout plan validates under the local policy");
    QJsonArray excessiveMicrojobTransfers =
        microjobPlan.value(QStringLiteral("transfers")).toArray();
    QJsonObject excessiveMicrojobTransfer =
        excessiveMicrojobTransfers.first().toObject();
    excessiveMicrojobTransfer[QStringLiteral("lamports")] =
        QStringLiteral("500000001");
    excessiveMicrojobTransfers[0] = excessiveMicrojobTransfer;
    microjobPlan[QStringLiteral("transfers")] = excessiveMicrojobTransfers;
    microjob[QStringLiteral("plan")] = microjobPlan;
    check(!forkmesh::rewards::parseRewardSigningJob(
              microjob, fixture.source, QStringLiteral("devnet"), now,
              &parsedJob, &error),
          "microjob payout above the local half-SOL batch cap is rejected");
    QJsonObject unknownPolicy = signingJob;
    QJsonObject unknownPolicyPlan =
        unknownPolicy.value(QStringLiteral("plan")).toObject();
    unknownPolicyPlan[QStringLiteral("policy")] =
        QStringLiteral("unreviewed-policy-v1");
    unknownPolicy[QStringLiteral("plan")] = unknownPolicyPlan;
    check(!forkmesh::rewards::parseRewardSigningJob(
              unknownPolicy, fixture.source, QStringLiteral("devnet"), now,
              &parsedJob, &error),
          "unreviewed Worker reward policy is rejected");
    QJsonObject invalidRandomFanout = signingJob;
    invalidRandomFanout[QStringLiteral("kind")] =
        QStringLiteral("community_reward_random");
    QJsonObject invalidRandomPlan =
        invalidRandomFanout.value(QStringLiteral("plan")).toObject();
    invalidRandomPlan[QStringLiteral("policy")] =
        QStringLiteral("randomized-eligible-mirror-v2");
    invalidRandomFanout[QStringLiteral("plan")] = invalidRandomPlan;
    check(!forkmesh::rewards::parseRewardSigningJob(
              invalidRandomFanout, fixture.source, QStringLiteral("devnet"),
              now, &parsedJob, &error),
          "random reward plan cannot fan out to multiple wallets");
    QJsonObject excessiveRandom = makeSigningJob(fixture, now);
    QJsonObject excessiveRandomPlan =
        excessiveRandom.value(QStringLiteral("plan")).toObject();
    QJsonArray excessiveTransfers =
        excessiveRandomPlan.value(QStringLiteral("transfers")).toArray();
    QJsonObject excessiveTransfer = excessiveTransfers.first().toObject();
    excessiveTransfer[QStringLiteral("lamports")] =
        QStringLiteral("10000000001");
    excessiveTransfers[0] = excessiveTransfer;
    excessiveRandomPlan[QStringLiteral("transfers")] = excessiveTransfers;
    excessiveRandom[QStringLiteral("plan")] = excessiveRandomPlan;
    check(!forkmesh::rewards::parseRewardSigningJob(
              excessiveRandom, fixture.source, QStringLiteral("devnet"), now,
              &parsedJob, &error),
          "reward plan above the local policy amount cap is rejected");
    QJsonObject missingCustody = signingJob;
    QJsonObject missingCustodyPlan =
        missingCustody.value(QStringLiteral("plan")).toObject();
    missingCustodyPlan.remove(QStringLiteral("custody"));
    missingCustody[QStringLiteral("plan")] = missingCustodyPlan;
    check(!forkmesh::rewards::parseRewardSigningJob(
              missingCustody, fixture.source, QStringLiteral("devnet"), now,
              &parsedJob, &error),
          "Worker plan missing its non-custodial declaration is rejected");
    QJsonObject tamperedSigning = signingJob;
    QJsonObject tamperedPlan =
        tamperedSigning.value(QStringLiteral("plan")).toObject();
    QJsonObject signing =
        tamperedPlan.value(QStringLiteral("signing")).toObject();
    signing[QStringLiteral("performed")] = true;
    tamperedPlan[QStringLiteral("signing")] = signing;
    tamperedSigning[QStringLiteral("plan")] = tamperedPlan;
    check(!forkmesh::rewards::parseRewardSigningJob(
              tamperedSigning, fixture.source, QStringLiteral("devnet"), now,
              &parsedJob, &error),
          "Worker plan claiming prior signing is rejected");
    QJsonObject builtIntent;
    check(forkmesh::rewards::buildRewardTransactionIntent(
              signingJob, fixture.source, QStringLiteral("devnet"),
              fixture.blockhash, 123456, now, &builtIntent, &error),
          "verified Worker plan builds a packet-sized local transaction");
    forkmesh::rewards::RewardTransferIntent builtParsed;
    check(forkmesh::rewards::parseRewardTransferIntent(
              builtIntent, fixture.source, QStringLiteral("devnet"), now,
              &builtParsed, &error) &&
              builtParsed.transfers.size() == 2 &&
              builtParsed.totalLamports == fixture.lamports + 5000,
          "locally built multi-transfer transaction exactly matches Worker plan");

    QJsonObject changedAmount = intentObject;
    changedAmount[QStringLiteral("lamports")] =
        QString::number(fixture.lamports + 1);
    check(!forkmesh::rewards::parseRewardTransferIntent(
              changedAmount, fixture.source, QStringLiteral("devnet"), now,
              &intent, &error),
          "displayed amount mismatch fails closed");

    QJsonObject presigned = intentObject;
    QByteArray presignedBytes = fixture.transaction;
    presignedBytes[1] = char(1);
    presigned[QStringLiteral("unsignedTransaction")] =
        QString::fromLatin1(presignedBytes.toBase64());
    check(!forkmesh::rewards::parseRewardTransferIntent(
              presigned, fixture.source, QStringLiteral("devnet"), now,
              &intent, &error),
          "Worker-provided pre-signed transaction is rejected");

    QJsonObject extraInstruction = intentObject;
    QByteArray extraBytes = fixture.transaction;
    extraBytes[fixture.instructionCountOffset] = char(2);
    extraInstruction[QStringLiteral("unsignedTransaction")] =
        QString::fromLatin1(extraBytes.toBase64());
    check(!forkmesh::rewards::parseRewardTransferIntent(
              extraInstruction, fixture.source, QStringLiteral("devnet"), now,
              &intent, &error),
          "transaction with extra instructions is rejected");

    QJsonObject sensitive = intentObject;
    sensitive[QStringLiteral("privateKey")] = QStringLiteral("must-not-arrive");
    check(!forkmesh::rewards::parseRewardTransferIntent(
              sensitive, fixture.source, QStringLiteral("devnet"), now,
              &intent, &error),
          "intent containing a private-key field is rejected");

    QJsonObject expired = intentObject;
    expired[QStringLiteral("expiresAt")] =
        now.addSecs(-1).toUTC().toString(Qt::ISODateWithMs);
    check(!forkmesh::rewards::parseRewardTransferIntent(
              expired, fixture.source, QStringLiteral("devnet"), now,
              &intent, &error),
          "expired reward intent is rejected");

    forkmesh::rewards::SignedRewardTransaction signedTransaction;
    check(!forkmesh::rewards::signRewardTransferIntent(
              intentObject, QStringLiteral("devnet"),
              QStringLiteral("wrong passphrase"), vaultPath, now,
              &signedTransaction, &error),
          "wrong vault passphrase cannot sign");
    check(forkmesh::rewards::signRewardTransferIntent(
              intentObject, QStringLiteral("devnet"),
              QStringLiteral("correct horse battery staple"), vaultPath, now,
              &signedTransaction, &error),
          "confirmed intent signs inside the local vault boundary");
    const QByteArray signedBytes =
        QByteArray::fromBase64(signedTransaction.signedTransactionBase64.toLatin1());
    const QByteArray signature = signedBytes.mid(1, 64);
    check(signedTransaction.signerPublicKey == fixture.source &&
              signedTransaction.chainSignature ==
                  forkmesh::rewards::encodeBase58(signature) &&
              verifySignature(fixture.publicKey, fixture.message, signature),
          "signed transaction contains a valid local-pool Ed25519 signature");
    check(!signedTransaction.signedTransactionBase64.contains(keypairBase58),
          "signer response contains no private key material");

    QUrl rpc;
    check(forkmesh::rewards::validatePublicRpcUrl(
              QStringLiteral("https://api.devnet.solana.com"), &rpc, &error),
          "public HTTPS Solana RPC origin is accepted");
    check(!forkmesh::rewards::validatePublicRpcUrl(
              QStringLiteral("https://rpc.example/api-key"), &rpc, &error) &&
              !forkmesh::rewards::validatePublicRpcUrl(
                  QStringLiteral("https://rpc.example/?token=x"), &rpc,
                  &error) &&
              !forkmesh::rewards::validatePublicRpcUrl(
                  QStringLiteral("http://rpc.example"), &rpc, &error),
          "credential-bearing or non-HTTPS RPC configuration is rejected");

    const QString ownerKey(43, QLatin1Char('A'));
    const QString nonce(22, QLatin1Char('b'));
    Q_UNUSED(ownerKey);
    Q_UNUSED(nonce);
    const QByteArray fetchCanonical =
        forkmesh::rewards::signingJobsFetchCanonical(
            QStringLiteral("instance-owner"),
            QStringLiteral("1784822400000"));
    const QByteArray submitCanonical =
        forkmesh::rewards::signingJobSubmitCanonical(
            QStringLiteral("instance-owner"),
            QString(32, QLatin1Char('c')),
            signedTransaction.chainSignature,
            QStringLiteral("1784822400000"));
    check(fetchCanonical ==
              QByteArrayLiteral(
                  "forkmesh-reward-jobs-v1\ninstance-owner\n1784822400000"),
          "Worker signing-job fetch uses the deployed canonical form");
    check(submitCanonical.startsWith(
              QByteArrayLiteral(
                  "forkmesh-reward-submit-v1\ninstance-owner\n")) &&
              submitCanonical.endsWith(
                  QByteArrayLiteral("\n1784822400000")),
          "Worker signature receipt uses the deployed canonical form");

    const QString redacted = forkmesh::rewards::redactRewardText(
        QStringLiteral(
            "Authorization: Bearer token123 privateKey=supersecret"),
        {QStringLiteral("supersecret")});
    check(!redacted.contains(QStringLiteral("token123")) &&
              !redacted.contains(QStringLiteral("supersecret")),
          "reward diagnostics redact bearer and exact private material");

    forkmesh::rewards::secureErase(importedAddress);
    std::cout << (failures == 0 ? "reward-pool signer tests passed\n"
                               : "reward-pool signer tests failed\n");
    return failures == 0 ? 0 : 1;
}
