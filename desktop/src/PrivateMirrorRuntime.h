#pragma once

#include "MirrorCrypto.h"
#include "PrivateMirrorStore.h"

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <memory>

class QTemporaryDir;

class PrivateMirrorMaterialization
{
public:
    ~PrivateMirrorMaterialization();

    PrivateMirrorMaterialization(const PrivateMirrorMaterialization &) = delete;
    PrivateMirrorMaterialization &
    operator=(const PrivateMirrorMaterialization &) = delete;

    QString repositoryPath() const { return m_repositoryPath; }
    bool isValid() const;

private:
    friend class PrivateMirrorRuntime;
    PrivateMirrorMaterialization(std::unique_ptr<QTemporaryDir> directory,
                                 QString repositoryPath);

    std::unique_ptr<QTemporaryDir> m_directory;
    QString m_repositoryPath;
};

// Runtime boundary used by the Qt control node for private repositories.
// - A hybrid MirrorCrypto identity lives only in an AES-256-GCM encrypted,
// owner-readable vault whose unlock secret is supplied by the local device
// identity.
// - Git data is bundled in memory and persisted only through
// PrivateMirrorStore's opaque.fm-private ciphertext.
// - Every sync replaces the archive under a new content key and key epoch.
// - Authorized plaintext materialization is recipient-only and temporary.
class PrivateMirrorRuntime
{
public:
    struct SyncResult {
        QString opaqueId;
        PrivateMirrorStore::Metadata metadata;
        bool created = false;

        bool isValid() const
        {
            return PrivateMirrorStore::isOpaqueId(opaqueId) &&
                   metadata.opaqueId == opaqueId && metadata.keyEpoch > 0;
        }
    };

    // Load the encrypted private-mirror identity vault, creating it when it
    // does not exist. vaultSecret must be at least 256 bits of device-bound
    // entropy; it is never persisted.
    static bool loadOrCreateIdentity(const QString &vaultPath,
                                     const QByteArray &vaultSecret,
                                     MirrorCrypto::Identity *identity,
                                     QString *error = nullptr);

    // Bundle a local working tree or bare repository and create/update its
    // opaque encrypted replica. The owner is always a recipient. Each update
    // rotates the archive content key and authenticated epoch.
    static SyncResult syncRepository(
        const QString &repositoryPath, const QString &replicaRoot,
        const QString &vaultPath, const QByteArray &vaultSecret,
        const QString &existingOpaqueId = QString(),
        const QList<QJsonObject> &additionalRecipientBundles = {},
        QString *error = nullptr);

    // Clone a remote source into a mode-0700 temporary directory, then seal it.
    // gitPrefixArgs is deliberately restricted to repeated "-c key=value"
    // pairs, allowing the existing short-lived HTTPS Authorization header
    // without putting any secret into a path, log, or persistent config.
    static SyncResult syncSource(
        const QString &source, const QStringList &gitPrefixArgs,
        const QString &replicaRoot, const QString &vaultPath,
        const QByteArray &vaultSecret,
        const QString &existingOpaqueId = QString(),
        const QList<QJsonObject> &additionalRecipientBundles = {},
        QString *error = nullptr);

    static bool resealRecipients(
        const QString &replicaRoot, const QString &vaultPath,
        const QByteArray &vaultSecret, const QString &opaqueId,
        const QList<QJsonObject> &remainingRecipientBundles,
        PrivateMirrorStore::Metadata *metadata = nullptr,
        QString *error = nullptr);

    // Decrypt and clone a replica into a mode-0700 temporary directory. The
    // plaintext bundle is deleted immediately after git imports it.
    static std::unique_ptr<PrivateMirrorMaterialization> materialize(
        const QString &replicaRoot, const QString &vaultPath,
        const QByteArray &vaultSecret, const QString &opaqueId,
        QString *error = nullptr);

    static constexpr qint64 maximumSerializedReplicaBytes()
    {
        return 256LL * 1024 * 1024;
    }

    // Remove a legacy plaintext bare mirror only when it resolves beneath the
    // explicitly managed mirror root and has a.git suffix. This is called only
    // after a replacement encrypted replica has been authenticated.
    static bool removeManagedPlaintextMirror(const QString &mirrorPath,
                                             const QString &managedMirrorRoot,
                                             QString *error = nullptr);

    static bool repositoryControlChannelAllowed(bool isPrivate)
    {
        return !isPrivate;
    }

    // A private repository may contact the relay's private-mirror control
    // plane only after the user explicitly enables network serving. Keeping
    // this policy beside the encryption runtime gives sync and collaborator
    // management one fail-closed boundary for local-only repositories.
    static bool privateRepositoryCloudAccessAllowed(bool isPrivate,
                                                     bool publishToNetwork)
    {
        return isPrivate && publishToNetwork;
    }

    // A repository the user never published has no relay catalog entry, so it
    // has no inbox for anyone to file into and nothing to read back. The inbox
    // reads carry the owner and repository name in the query string of a signed
    // URL, so polling one anyway would hand the relay the name and the
    // existence of a repository the user deliberately kept local — and the
    // empty 200 it answers with is indistinguishable from a real drain. This
    // gates every relay inbox read (issues, pulls, discussions) on the same
    // fail-closed boundary, regardless of whether the repository is private.
    static bool repositoryRelayInboxAllowed(bool publishToNetwork)
    {
        return publishToNetwork;
    }

    // A private repository nobody touched must not be re-announced to the relay
    // on every timer tick: the recipient-key fetch that opens a quiet sync
    // carries the owner and repository name in its URL. A background pass is
    // skipped only while the sealed source's ref fingerprint still matches the
    // one recorded at the last successful seal and that seal is recent enough
    // to keep a revocation published from another device bounded. Anything
    // unknown (no fingerprint, unreadable source, a clock that moved backwards)
    // fails open into a normal sync.
    static bool privateSyncQuietPassIsIdle(const QString &recordedRefsHash,
                                           const QString &currentRefsHash,
                                           qint64 lastSyncMs, qint64 nowMs,
                                           qint64 idleWindowMs)
    {
        if (recordedRefsHash.isEmpty() || currentRefsHash.isEmpty() ||
            recordedRefsHash != currentRefsHash)
            return false;
        if (lastSyncMs <= 0 || nowMs < lastSyncMs || idleWindowMs <= 0)
            return false;
        return nowMs - lastSyncMs < idleWindowMs;
    }
};
