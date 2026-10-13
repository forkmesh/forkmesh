#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

// Member-to-member tips. A user links their own Solana keypair once; it is
// stored in a passphrase-encrypted, owner-only vault on this machine and is
// never transmitted anywhere. Sending a tip builds exactly one legacy System
// Program transfer, signs it locally, and broadcasts it to a public JSON-RPC
// endpoint. The relay is told only about the resulting public chain signature
// so it can email the recipient.
//
// This reuses the vault format and Ed25519 signer from forkmesh::rewards (the
// operator-only community pool), but keeps its own vault file: a personal tip
// wallet and a community-pool signer are different keys with different blast
// radii and must not be interchangeable.
namespace forkmesh::tips {

// Default encrypted-vault location under the application's private data
// directory. Callers may pass an explicit path for tests.
QString defaultTipVaultPath();

// The linked wallet's public address, or "" when no wallet is linked. Reads
// only the clear-text address field; it never decrypts the private seed.
QString linkedTipAddress(const QString &vaultPath, QString *error = nullptr);

// Import an existing Solana keypair (solana-keygen JSON byte array, or a
// base58 64-byte secret key as exported by common wallets) into the local
// encrypted vault.
bool linkTipWallet(const QByteArray &keyMaterial, const QString &passphrase,
                   const QString &vaultPath, QString *publicAddress = nullptr,
                   QString *error = nullptr);

// The shortest passphrase the vault will actually accept. UI that gates a
// passphrase field must ask rather than hard-code a number.
int minimumVaultPassphraseLength();

// Mint a brand-new tip wallet on this machine, in the solana-keygen JSON form
// linkTipWallet() accepts. Nothing is stored: the caller links the returned
// material like any other key, so a generated wallet and an imported one take
// exactly one code path onto disk. `keypairJson` holds a private key — wipe it
// with forkmesh::rewards::secureErase().
bool generateTipKeypair(QByteArray *keypairJson,
                        QString *publicAddress = nullptr,
                        QString *error = nullptr);

// Unlock the vault and hand the secret key back in the two formats wallets
// import, so an owner can move a ForkMesh-generated wallet into Phantom or the
// Solana CLI, or keep an offline backup. Requires the vault passphrase, and
// both outputs are private material the caller must wipe.
bool exportTipWalletKey(const QString &passphrase, const QString &vaultPath,
                        QByteArray *keypairJson, QString *base58SecretKey,
                        QString *publicAddress = nullptr,
                        QString *error = nullptr);

// Remove the local vault. Returns false only when a vault exists and could not
// be deleted.
bool unlinkTipWallet(const QString &vaultPath, QString *error = nullptr);

// Parse a human-entered SOL amount ("0.05", "1", "1.000000001") into lamports.
// Rejects negatives, more than nine decimal places, and amounts outside
// [minimumTipLamports(), maximumTipLamports()].
bool parseSolAmount(const QString &text, quint64 *lamports,
                    QString *error = nullptr);
QString formatLamports(quint64 lamports);
quint64 minimumTipLamports();
quint64 maximumTipLamports();

// The relay's owner-reviewed contributor reward projection is still untrusted
// network input. Automated payouts accept only this narrow, fully bounded
// shape before any wallet operation begins.
struct ContributorPayout {
    QString rewardId;
    QString recipientName;
    QString recipientAddress;
    QString repository;
    QString sourceKind;
    int subjectNumber = 0;
    quint64 lamports = 0;
    qint64 createdAtMs = 0;
};

bool parseContributorPayout(const QJsonObject &object,
                            ContributorPayout *payout,
                            QString *error = nullptr);

// CFO automation is deliberately conservative: every response is parsed
// again, issue rewards remain capped at 0.005 SOL, pull rewards at 0.025 SOL,
// and at most 0.05 SOL is signed per local day. Requests are separated enough
// to stay comfortably below public RPC and Worker limits.
quint64 minimumContributorPayoutLamports();
quint64 maximumContributorPayoutLamports();
quint64 contributorPayoutDailyCapLamports();
int contributorPayoutSpacingMs();
int contributorPayoutFinalityPollMs();
int contributorPayoutRetryDelayMs();
bool noticeResponseConfirmsFinality(bool networkOk, int httpStatus,
                                    bool responseOk, bool rewardPaid);

// Prove that the supplied passphrase unlocks the linked wallet by signing a
// fixed off-chain authorization message. No transaction is built or sent, and
// the resulting signature is wiped immediately.
bool verifyTipWalletPassphrase(const QString &passphrase,
                               const QString &vaultPath,
                               QString *publicAddress = nullptr,
                               QString *error = nullptr);

// One reviewed, unsigned transfer. `message` is the serialized Solana message
// that gets signed; `unsignedTransaction` is that message behind a zeroed
// signature slot at `signatureOffset`.
struct TipTransfer {
    QString fromAddress;
    QString toAddress;
    QString recentBlockhash;
    quint64 lamports = 0;
    quint64 lastValidBlockHeight = 0;
    QByteArray message;
    QByteArray unsignedTransaction;
    int signatureOffset = -1;
};

struct SignedTip {
    QString signerAddress;
    QString chainSignature;
    QString signedTransactionBase64;
};

// Build the single-transfer transaction. Fails closed on a malformed address,
// an out-of-range amount, or an unusable blockhash. A self-tip is represented
// as one writable account referenced for both sides of the System transfer.
bool buildTipTransfer(const QString &fromAddress, const QString &toAddress,
                      quint64 lamports, const QString &recentBlockhash,
                      quint64 lastValidBlockHeight, TipTransfer *transfer,
                      QString *error = nullptr);

// Unlock the vault, sign the built message, and assemble the wire transaction.
// The vault's own address must match the transfer's source. No private
// material is returned and the decrypted seed is wiped before returning.
bool signTipTransfer(const TipTransfer &transfer, const QString &passphrase,
                     const QString &vaultPath, SignedTip *signedTip,
                     QString *error = nullptr);

bool isValidTipAddress(const QString &address);

} // namespace forkmesh::tips
