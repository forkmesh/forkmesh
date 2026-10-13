#pragma once

#include "MirrorCrypto.h"

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

// Durable storage for an end-to-end encrypted private mirror replica.
// The store deliberately accepts no repository owner or repository name. Each
// replica is addressed only by an unguessable opaque id and contains a
// MirrorCrypto envelope. Recipient private keys remain in the caller's local
// identity vault and are never written beside the ciphertext.
class PrivateMirrorStore
{
public:
    struct Metadata {
        QString opaqueId;
        QString ownerKeyId;
        quint64 keyEpoch = 0;
        qint64 createdAtMs = 0;
        qint64 updatedAtMs = 0;
        // SHA-256 of the exact.fm-private file bytes. Direct HTTPS gateways
        // publish this as an ETag; the routing Worker compares it with the
        // owner's signed opaque-route binding before streaming.
        QString replicaFileSha256;
        QString ciphertextSha256;
        QStringList recipientKeyIds;
    };

    // Encrypt a non-empty bare-repository archive and persist it atomically.
    // The first public bundle is the replica owner's identity and remains
    // authoritative across replacement and recipient rotations.
    // Returns a 64-character opaque id, or an empty string on failure.
    static QString createReplica(
        const QString &storageRoot, const QByteArray &archive,
        const QList<QJsonObject> &recipientPublicBundles,
        QString *error = nullptr);

    // Authenticate and decrypt one replica for a current recipient. The
    // caller owns the returned plaintext and should keep its lifetime short.
    static QByteArray openReplica(
        const QString &storageRoot, const QString &opaqueId,
        const MirrorCrypto::Identity &identity,
        Metadata *metadata = nullptr, QString *error = nullptr);

    // Replace a replica's archive after a repository sync. The replacement is
    // sealed under a fresh random content key for the exact recipient set and
    // advances the epoch atomically; the previous plaintext is never written
    // to disk.
    static bool replaceReplica(
        const QString &storageRoot, const QString &opaqueId,
        const QByteArray &archive,
        const MirrorCrypto::Identity &currentOwner,
        const QList<QJsonObject> &recipientPublicBundles,
        QString *error = nullptr);

    // Re-encrypt the archive under a fresh content key for the exact new
    // recipient set and increment the key epoch. Removing a recipient from
    // this list revokes access to every subsequently stored epoch.
    static bool rotateRecipients(
        const QString &storageRoot, const QString &opaqueId,
        const MirrorCrypto::Identity &currentOwner,
        const QList<QJsonObject> &newRecipientPublicBundles,
        QString *error = nullptr);

    // Read non-sensitive local metadata after validating the ciphertext
    // digest. This never decrypts repository data.
    static bool inspectReplica(
        const QString &storageRoot, const QString &opaqueId,
        Metadata *metadata, QString *error = nullptr);

    // Import exact.fm-private bytes received over an authorized HTTPS route.
    // The outer file digest, embedded envelope digest, owner identity, and
    // monotonic key epoch are verified before an atomic owner-only write.
    // Returns the embedded opaque id or an empty string on any failure.
    static QString importReplica(
        const QString &storageRoot, const QByteArray &serializedReplica,
        const QString &expectedFileSha256,
        Metadata *metadata = nullptr, QString *error = nullptr);

    static bool isOpaqueId(const QString &value);

private:
    static QString replicaPath(const QString &storageRoot,
                               const QString &opaqueId);
};
