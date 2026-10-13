#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QUrl>

namespace forkmesh::rewards {

struct RewardTransfer {
    QString destinationWallet;
    QString nodeId;
    quint64 lamports = 0;
};

// Worker-authored public plan as returned by /api/rewards/signing-jobs. It
// contains no blockhash, signature, or private material; the desktop turns it
// into an exact transaction only after local review.
struct RewardSigningJob {
    QString intentId;
    QString kind;
    QString status;
    QString network;
    QString sourceWallet;
    QString policy;
    QString transactionSignature;
    qint64 createdAtMs = 0;
    qint64 expiresAtMs = 0;
    quint64 totalLamports = 0;
    QList<RewardTransfer> transfers;
};

// The exact transfer that was independently authored by the reward scheduler
// and then verified locally. Only a legacy, single-signature set of System
// Program transfers is accepted; arbitrary programs and extra instructions
// fail closed.
struct RewardTransferIntent {
    QString intentId;
    QString type;
    QString network;
    QString sourceWallet;
    QString destinationWallet;
    QString recentBlockhash;
    QString roundId;
    QString selectedNodeId;
    quint64 lamports = 0;
    quint64 totalLamports = 0;
    quint64 lastValidBlockHeight = 0;
    QList<RewardTransfer> transfers;
    QDateTime createdAt;
    QDateTime expiresAt;
    QByteArray unsignedTransaction;
    QByteArray message;
    int signatureOffset = -1;
};

struct SignedRewardTransaction {
    QString intentId;
    QString signerPublicKey;
    QString chainSignature;
    QString signedTransactionBase64;
    QString transactionSha256;
};

// Default encrypted-vault location under the application's private data
// directory. Callers may pass an explicit path for tests or managed installs.
QString defaultPoolVaultPath();

// Inspect an existing Solana CLI/desktop-wallet secret key without saving it.
// Only an exact 64-byte Ed25519 keypair is accepted (JSON byte array or base58),
// and its embedded public half must match the private seed.
QString poolAddressForKeyMaterial(const QByteArray &keyMaterial,
                                  QString *error = nullptr);

// Import an existing key into a scrypt + AES-256-GCM encrypted local vault.
// The vault file and its directory must be reducible to owner-only permissions
// or the operation fails without writing.
bool importPoolKey(const QByteArray &keyMaterial, const QString &passphrase,
                   const QString &vaultPath, QString *publicAddress = nullptr,
                   QString *error = nullptr);

// The passphrase length the vault's key derivation itself enforces. Callers
// that gate a passphrase field must use this rather than a number of their own,
// or a passphrase the dialog accepted is rejected on the way to disk.
int minimumVaultPassphraseLength();

// Mint a fresh Ed25519 keypair from the system CSPRNG in the layout Solana
// wallets use: the 32-byte seed followed by its 32-byte public key. The bytes
// are the caller's to protect — wipe them with secureErase() once stored or
// backed up.
bool generateSolanaKeypair(QByteArray *keypair, QString *error = nullptr);

// Decrypt a vault written by importPoolKey() and hand back the 64-byte keypair
// it holds. This is the one function in this namespace that returns private
// material, and it exists so an owner can back up or move a key ForkMesh
// generated for them; callers must wipe `keypair` with secureErase().
bool exportVaultKeypair(const QString &vaultPath, const QString &passphrase,
                        QByteArray *keypair, QString *publicAddress = nullptr,
                        QString *error = nullptr);

// The two interchange formats every Solana wallet reads: the 64-element JSON
// byte array solana-keygen writes, and the base58 secret key phantom-style
// wallets import. Both return empty for anything but an exact 64-byte keypair.
QByteArray solanaKeypairJson(const QByteArray &keypair);
QString solanaKeypairBase58(const QByteArray &keypair);

// The address a 64-byte keypair belongs to, or "" when the halves disagree.
QString addressForKeypair(const QByteArray &keypair);

// Reads only the clear public address from a structurally valid encrypted
// envelope. It never decrypts the private seed.
QString poolVaultPublicAddress(const QString &vaultPath,
                               QString *error = nullptr);

// Unlock a vault written by importPoolKey() and Ed25519-sign exactly one
// caller-supplied Solana message with it. The decrypted seed exists only for
// the duration of this call and is wiped before it returns; no private
// material is ever returned. Shared with the member-tip signer
// (forkmesh::tips), which builds its own message and keeps its key in a
// separate vault file.
bool signSolanaMessageWithVault(const QString &vaultPath,
                                const QString &passphrase,
                                const QByteArray &message,
                                QString *signerAddress,
                                QByteArray *signature,
                                QString *error = nullptr);

// Validate the existing Worker's public signing-job envelope/plan. Submitted
// jobs are accepted for reconciliation display; only pending_signature jobs can
// be turned into a transaction by buildRewardTransactionIntent().
bool parseRewardSigningJob(const QJsonObject &job,
                           const QString &expectedPoolAddress,
                           const QString &expectedNetwork,
                           const QDateTime &now,
                           RewardSigningJob *parsed,
                           QString *error = nullptr);

bool buildRewardTransactionIntent(const QJsonObject &job,
                                  const QString &expectedPoolAddress,
                                  const QString &expectedNetwork,
                                  const QString &recentBlockhash,
                                  quint64 lastValidBlockHeight,
                                  const QDateTime &now,
                                  QJsonObject *intent,
                                  QString *error = nullptr);

bool parseRewardTransferIntent(const QJsonObject &object,
                               const QString &expectedPoolAddress,
                               const QString &expectedNetwork,
                               const QDateTime &now,
                               RewardTransferIntent *intent,
                               QString *error = nullptr);

// Re-validates the intent, unlocks the local vault, signs only the serialized
// Solana message, constructs the signed transaction, and scrubs private bytes
// before returning. No private material is returned.
bool signRewardTransferIntent(const QJsonObject &object,
                              const QString &expectedNetwork,
                              const QString &passphrase,
                              const QString &vaultPath,
                              const QDateTime &now,
                              SignedRewardTransaction *signedTransaction,
                              QString *error = nullptr);

// Reward signing deliberately accepts only a configured public HTTPS JSON-RPC
// origin: no credentials, query, fragment, or token-like path may be persisted.
bool validatePublicRpcUrl(const QString &configured, QUrl *normalized = nullptr,
                          QString *error = nullptr);

// Canonical forms already enforced by /api/rewards/signing-jobs. The Worker
// binds signerAccount to REWARD_SIGNER_ACCOUNT/INSTANCE_OWNER_ACCOUNT and
// verifies the existing ForkMesh owner identity signature plus timestamp.
QByteArray signingJobsFetchCanonical(const QString &signerAccount,
                                     const QString &timestampMs);
QByteArray signingJobSubmitCanonical(const QString &signerAccount,
                                     const QString &intentId,
                                     const QString &chainSignature,
                                     const QString &timestampMs);

QString encodeBase58(const QByteArray &bytes);
QByteArray decodeBase58(const QString &text, int expectedBytes = -1);
bool isValidSolanaSignature(const QString &signature);

// Defensive display/log filter for RPC/Worker errors. Private-key-shaped JSON
// fields, authorization headers, supplied exact secrets, controls, and excess
// output are removed.
QString redactRewardText(const QString &text,
                         const QStringList &exactSecrets = {});

void secureErase(QByteArray &bytes);
void secureErase(QString &text);

} // namespace forkmesh::rewards
