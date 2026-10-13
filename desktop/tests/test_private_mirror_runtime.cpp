#include "PrivateMirrorRuntime.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QUrl>

#include <cstdio>
#include <memory>

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition)
        return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

bool git(const QString &path, const QStringList &arguments,
         QByteArray *output = nullptr)
{
    QProcess process;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("GIT_AUTHOR_NAME"),
                       QStringLiteral("Private Mirror Test"));
    environment.insert(QStringLiteral("GIT_AUTHOR_EMAIL"),
                       QStringLiteral("private-mirror@example.invalid"));
    environment.insert(QStringLiteral("GIT_COMMITTER_NAME"),
                       QStringLiteral("Private Mirror Test"));
    environment.insert(QStringLiteral("GIT_COMMITTER_EMAIL"),
                       QStringLiteral("private-mirror@example.invalid"));
    process.setProcessEnvironment(environment);
    process.start(QStringLiteral("git"),
                  QStringList{QStringLiteral("-C"), path} + arguments);
    if (!process.waitForFinished(30000) || process.exitCode() != 0)
        return false;
    if (output)
        *output = process.readAllStandardOutput();
    return true;
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return file.write(bytes) == bytes.size();
}

bool excludesGroupAndOther(const QFileInfo &info)
{
    const auto permissions = info.permissions();
    return !(permissions &
             (QFileDevice::ReadGroup | QFileDevice::WriteGroup |
              QFileDevice::ExeGroup | QFileDevice::ReadOther |
              QFileDevice::WriteOther | QFileDevice::ExeOther));
}

QString sha256Hex(const QByteArray &bytes)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)
            .toHex());
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    QTemporaryDir root;
    check(root.isValid(), "temporary test root is available");
    QString unavailable;
    if (!MirrorCrypto::postQuantumAvailable(&unavailable)) {
        MirrorCrypto::Identity identity;
        QString error;
        check(!PrivateMirrorRuntime::loadOrCreateIdentity(
                  root.filePath(QStringLiteral("identity.vault")),
                  QByteArray(32, '\x42'), &identity, &error) &&
                  error == unavailable && !identity.isValid() &&
                  !QFileInfo::exists(root.filePath(QStringLiteral("identity.vault"))),
              "without ML-KEM no private-mirror identity is created");
        std::printf("SKIP: %s\n", unavailable.toUtf8().constData());
        return failures == 0 ? 0 : 1;
    }
    const QString source = root.filePath(QStringLiteral("owner-source"));
    check(QDir().mkpath(source), "private source directory is created");
    check(git(source, {QStringLiteral("init"), QStringLiteral("-q"),
                       QStringLiteral("-b"), QStringLiteral("main")}),
          "private source repository is initialized");
    const QByteArray firstSecret =
        QByteArrayLiteral("repository-plaintext-secret-v1");
    check(writeFile(QDir(source).filePath(QStringLiteral("secret.txt")),
                    firstSecret),
          "private source content is written");
    check(git(source, {QStringLiteral("add"), QStringLiteral("secret.txt")}) &&
              git(source, {QStringLiteral("commit"), QStringLiteral("-q"),
                           QStringLiteral("-m"), QStringLiteral("initial")}),
          "private source has a commit");

    const QString replicaRoot =
        root.filePath(QStringLiteral("opaque-replicas"));
    const QString vaultPath =
        root.filePath(QStringLiteral("identity/private-mirror-vault.json"));
    const QByteArray vaultSecret =
        QByteArrayLiteral("0123456789abcdef0123456789abcdef"
                          "device-bound-signature-material");
    QString error;
    const auto created = PrivateMirrorRuntime::syncRepository(
        source, replicaRoot, vaultPath, vaultSecret, {}, {}, &error);
    check(created.isValid() && created.created && error.isEmpty(),
          "control-node sync creates an opaque encrypted private replica");
    check(created.metadata.keyEpoch == 1 &&
              !created.metadata.ownerKeyId.isEmpty() &&
              created.metadata.replicaFileSha256.size() == 64 &&
              created.metadata.recipientKeyIds ==
                  QStringList{created.metadata.ownerKeyId},
          "new private replica records its owner, exact file digest, and first key epoch");

    const QString replicaPath =
        QDir(replicaRoot).filePath(created.opaqueId +
                                   QStringLiteral(".fm-private"));
    QFile replicaFile(replicaPath);
    check(replicaFile.open(QIODevice::ReadOnly),
          "encrypted replica file can be inspected");
    const QByteArray replicaBytes = replicaFile.readAll();
    replicaFile.close();
    check(created.metadata.replicaFileSha256 ==
              sha256Hex(replicaBytes),
          "route metadata hashes the exact serialized encrypted replica");
    QFile vaultFile(vaultPath);
    check(vaultFile.open(QIODevice::ReadOnly),
          "encrypted identity vault can be inspected");
    const QByteArray vaultBytes = vaultFile.readAll();
    vaultFile.close();
    check(!replicaBytes.contains(firstSecret) &&
              !vaultBytes.contains(firstSecret),
          "neither persistent private-mirror file contains repository plaintext");
    check(QFileInfo(replicaPath).fileName() ==
              created.opaqueId + QStringLiteral(".fm-private") &&
              !QFileInfo(replicaPath).fileName().contains(
                  QStringLiteral("owner-source")),
          "private replica has only an opaque filename");
    check(excludesGroupAndOther(QFileInfo(replicaPath)) &&
              excludesGroupAndOther(QFileInfo(vaultPath)) &&
              excludesGroupAndOther(QFileInfo(QFileInfo(vaultPath).absolutePath())),
          "replica, vault, and vault directory are owner-only");

    MirrorCrypto::Identity owner;
    check(PrivateMirrorRuntime::loadOrCreateIdentity(
              vaultPath, vaultSecret, &owner, &error) &&
              owner.isValid() && owner.keyId() == created.metadata.ownerKeyId,
          "device-bound secret unlocks the separate hybrid identity vault");
    check(!replicaBytes.contains(owner.x25519Priv.toBase64()) &&
              !replicaBytes.contains(owner.mlkemPriv.toBase64()) &&
              !vaultBytes.contains(owner.x25519Priv.toBase64()) &&
              !vaultBytes.contains(owner.mlkemPriv.toBase64()),
          "private identity halves occur neither beside ciphertext nor in vault plaintext");
    MirrorCrypto::Identity wrongIdentity;
    check(!PrivateMirrorRuntime::loadOrCreateIdentity(
              vaultPath, QByteArray(64, 'x'), &wrongIdentity, &error) &&
              !wrongIdentity.isValid(),
          "wrong device secret cannot unlock the identity vault");

    const QString collaboratorVaultPath =
        root.filePath(
            QStringLiteral("collaborator/private-mirror-vault.json"));
    const QByteArray collaboratorVaultSecret =
        QByteArrayLiteral("abcdef0123456789abcdef0123456789"
                          "collaborator-device-bound-material");
    MirrorCrypto::Identity collaborator;
    check(PrivateMirrorRuntime::loadOrCreateIdentity(
              collaboratorVaultPath, collaboratorVaultSecret,
              &collaborator, &error) &&
              collaborator.isValid(),
          "collaborator hybrid identity is generated in its encrypted local vault");
    const QByteArray secondSecret =
        QByteArrayLiteral("repository-plaintext-secret-v2");
    check(writeFile(QDir(source).filePath(QStringLiteral("secret.txt")),
                    secondSecret) &&
              git(source, {QStringLiteral("add"), QStringLiteral("secret.txt")}) &&
              git(source, {QStringLiteral("commit"), QStringLiteral("-q"),
                           QStringLiteral("-m"), QStringLiteral("update")}),
          "private source is updated before a sync");
    const auto updated = PrivateMirrorRuntime::syncRepository(
        source, replicaRoot, vaultPath, vaultSecret, created.opaqueId,
        {collaborator.publicBundle()}, &error);
    check(updated.isValid() && !updated.created &&
              updated.metadata.keyEpoch == 2 &&
              updated.metadata.ciphertextSha256 !=
                  created.metadata.ciphertextSha256,
          "private sync rotates content key, ciphertext, and key epoch");
    check(PrivateMirrorStore::openReplica(
              replicaRoot, created.opaqueId, collaborator, nullptr, &error)
              .startsWith(QByteArrayLiteral("# v2 git bundle")),
          "an explicitly authorized collaborator can decrypt the current bundle");

    // The data folder holding the replica can be lost while the repository
    // settings, and so the opaque id, survive (2026-09-22). The next sync must
    // seal a fresh replica rather than fail forever with "not found".
    const QString lostRoot = root.filePath(QStringLiteral("lost-replicas"));
    const auto resealed = PrivateMirrorRuntime::syncRepository(
        source, lostRoot, vaultPath, vaultSecret, created.opaqueId, {}, &error);
    check(resealed.isValid() && resealed.created &&
              resealed.opaqueId != created.opaqueId &&
              QFileInfo::exists(QDir(lostRoot).filePath(
                  resealed.opaqueId + QStringLiteral(".fm-private"))),
          "a sync whose replica file is gone seals a fresh replica instead of failing");

    QFile currentReplica(replicaPath);
    check(currentReplica.open(QIODevice::ReadOnly),
          "current encrypted replica can be streamed to a collaborator");
    const QByteArray epochTwoBytes = currentReplica.readAll();
    currentReplica.close();
    const QString epochTwoDigest = sha256Hex(epochTwoBytes);
    const QString collaboratorReplicaRoot =
        root.filePath(QStringLiteral("collaborator/received-replicas"));
    PrivateMirrorStore::Metadata importedMetadata;
    check(PrivateMirrorStore::importReplica(
              collaboratorReplicaRoot, epochTwoBytes, epochTwoDigest,
              &importedMetadata, &error) == created.opaqueId &&
              importedMetadata.keyEpoch == 2 &&
              importedMetadata.replicaFileSha256 == epochTwoDigest,
          "authorized HTTPS ciphertext imports only after exact outer and embedded digest verification");
    {
        auto collaboratorMaterialization =
            PrivateMirrorRuntime::materialize(
                collaboratorReplicaRoot, collaboratorVaultPath,
                collaboratorVaultSecret, created.opaqueId, &error);
        check(collaboratorMaterialization &&
                  collaboratorMaterialization->isValid(),
              "authorized collaborator materializes the imported ciphertext with its own vault key");
        if (collaboratorMaterialization) {
            QByteArray content;
            check(git(collaboratorMaterialization->repositoryPath(),
                      {QStringLiteral("show"),
                       QStringLiteral("refs/heads/main:secret.txt")},
                      &content) &&
                      content.trimmed() == secondSecret,
                  "collaborator materialization contains the authenticated private commit");
        }
    }
    QByteArray tamperedReplica = epochTwoBytes;
    if (!tamperedReplica.isEmpty())
        tamperedReplica[tamperedReplica.size() / 2] ^= 0x01;
    check(PrivateMirrorStore::importReplica(
              root.filePath(QStringLiteral("tampered-import")),
              tamperedReplica, epochTwoDigest, nullptr, &error)
              .isEmpty(),
          "ciphertext import rejects a byte-level transport digest mismatch");

    QJsonObject conflictingObject =
        QJsonDocument::fromJson(epochTwoBytes).object();
    conflictingObject.insert(
        QStringLiteral("updatedAt"),
        conflictingObject.value(QStringLiteral("updatedAt")).toDouble() +
            1.0);
    const QByteArray conflictingBytes =
        QJsonDocument(conflictingObject).toJson(
            QJsonDocument::Compact);
    check(PrivateMirrorStore::importReplica(
              collaboratorReplicaRoot, conflictingBytes,
              sha256Hex(conflictingBytes), nullptr, &error)
              .isEmpty(),
          "same-epoch ciphertext conflicts are rejected instead of overwriting a trusted replica");

    // A recipient is not the owner and cannot use the lower-level store API to
    // replace content or redefine the owner-recipient set.
    check(!PrivateMirrorStore::replaceReplica(
              replicaRoot, created.opaqueId, QByteArrayLiteral("malicious"),
              collaborator,
              {collaborator.publicBundle(), owner.publicBundle()}, &error),
          "a collaborator cannot impersonate the private-replica owner");

    QString materializedPath;
    {
        auto materialized = PrivateMirrorRuntime::materialize(
            replicaRoot, vaultPath, vaultSecret, created.opaqueId, &error);
        check(materialized && materialized->isValid(),
              "authorized owner can materialize a temporary bare repository");
        if (materialized) {
            materializedPath = materialized->repositoryPath();
            QByteArray content;
            check(git(materializedPath,
                      {QStringLiteral("show"),
                       QStringLiteral("refs/heads/main:secret.txt")},
                      &content) &&
                      content.trimmed() == secondSecret,
                  "temporary materialization contains the current private commit");
            check(excludesGroupAndOther(
                      QFileInfo(QFileInfo(materializedPath).absolutePath())) &&
                      excludesGroupAndOther(QFileInfo(materializedPath)),
                  "temporary materialization is owner-only");
            const QStringList stagedBundles =
                QDir(QFileInfo(materializedPath).absolutePath())
                    .entryList({QStringLiteral("*.bundle")}, QDir::Files);
            check(stagedBundles.isEmpty(),
                  "plaintext bundle is removed immediately after import");
        }
    }
    check(!materializedPath.isEmpty() &&
              !QFileInfo::exists(QFileInfo(materializedPath).absolutePath()),
          "temporary plaintext repository is removed at end of authorization scope");

    PrivateMirrorStore::Metadata revoked;
    check(PrivateMirrorRuntime::resealRecipients(
              replicaRoot, vaultPath, vaultSecret, created.opaqueId, {},
              &revoked, &error) &&
              revoked.keyEpoch == 3 &&
              revoked.recipientKeyIds ==
                  QStringList{revoked.ownerKeyId},
          "recipient revocation reseals under a fresh owner-only epoch");
    check(PrivateMirrorStore::openReplica(
              replicaRoot, created.opaqueId, collaborator, nullptr, &error)
              .isEmpty(),
          "revoked collaborator cannot decrypt the new epoch");
    QFile revokedReplica(replicaPath);
    check(revokedReplica.open(QIODevice::ReadOnly),
          "fresh revoked epoch can be read as opaque transport bytes");
    const QByteArray epochThreeBytes = revokedReplica.readAll();
    revokedReplica.close();
    check(PrivateMirrorStore::importReplica(
              collaboratorReplicaRoot, epochThreeBytes,
              sha256Hex(epochThreeBytes), nullptr, &error) ==
              created.opaqueId,
          "a newer authenticated epoch replaces the collaborator's older local ciphertext");
    check(PrivateMirrorStore::importReplica(
              collaboratorReplicaRoot, epochTwoBytes, epochTwoDigest,
              nullptr, &error)
              .isEmpty(),
          "an older encrypted epoch cannot roll back an imported replica");

    const QString managedRoot =
        root.filePath(QStringLiteral("managed-mirrors"));
    check(QDir().mkpath(managedRoot), "managed mirror root is created");
    const QString legacy =
        QDir(managedRoot).filePath(QStringLiteral("legacy-private.git"));
    QProcess clone;
    clone.start(QStringLiteral("git"),
                {QStringLiteral("clone"), QStringLiteral("--mirror"),
                 source, legacy});
    check(clone.waitForFinished(30000) && clone.exitCode() == 0 &&
              QFileInfo::exists(legacy),
          "legacy plaintext mirror exists before encrypted migration cleanup");
    check(PrivateMirrorRuntime::removeManagedPlaintextMirror(
              legacy, managedRoot, &error) &&
              !QFileInfo::exists(legacy),
          "authenticated migration removes managed plaintext private mirror");
    check(!PrivateMirrorRuntime::removeManagedPlaintextMirror(
              source, managedRoot, &error) &&
              QFileInfo::exists(source),
          "cleanup refuses paths outside the explicit managed mirror root");

    check(!PrivateMirrorRuntime::repositoryControlChannelAllowed(true) &&
              PrivateMirrorRuntime::repositoryControlChannelAllowed(false),
          "private identities are excluded from named control channels and no channel carries repository bytes");
    check(!PrivateMirrorRuntime::repositoryRelayInboxAllowed(false) &&
              PrivateMirrorRuntime::repositoryRelayInboxAllowed(true),
          "local-only repositories read no relay inbox and published ones still do");
    check(PrivateMirrorRuntime::maximumSerializedReplicaBytes() ==
              256LL * 1024 * 1024,
          "desktop ciphertext receive path has an explicit 256 MiB bound");

    const int replicaCountBeforeHostileSources =
        QDir(replicaRoot)
            .entryList({QStringLiteral("*.fm-private")},
                       QDir::Files)
            .size();
    const QStringList hostileSources{
        QStringLiteral("file:///tmp/private.git"),
        QStringLiteral("ext::sh -c touch /tmp/forkmesh-invalid"),
        QStringLiteral("https://user@example.invalid/private.git"),
        QStringLiteral("https://127.0.0.1:9/private.git"),
        QStringLiteral("https://example.invalid/private.git?token=secret"),
        QStringLiteral("https://example.invalid/private.git#secret"),
        QStringLiteral("https://example.invalid/private.git\ninjected"),
    };
    for (const QString &hostileSource : hostileSources) {
        const auto rejected = PrivateMirrorRuntime::syncSource(
            hostileSource, {}, replicaRoot, vaultPath, vaultSecret,
            QString(), {}, &error);
        check(!rejected.isValid(),
              "non-canonical or non-HTTPS remote source is rejected before Git");
        check(!error.contains(hostileSource),
              "remote-source rejection does not echo a potentially sensitive URL");
    }
    const QString headerSecret =
        QStringLiteral("dXNlcjp0b2tlbg==");
    const auto injectedHeader =
        PrivateMirrorRuntime::syncSource(
            QStringLiteral("https://127.0.0.1:9/private.git"),
            {QStringLiteral("-c"),
             QStringLiteral(
                 "http.extraHeader=Authorization: Basic ") +
                 headerSecret +
                 QStringLiteral("\r\nX-Injected: yes")},
            replicaRoot, vaultPath, vaultSecret, QString(), {},
            &error);
    check(!injectedHeader.isValid() &&
              !error.contains(headerSecret),
          "CRLF-bearing authorization configuration is rejected without echoing credentials");

    const QString hostileConfig =
        root.filePath(QStringLiteral("hostile-global-gitconfig"));
    const QString helperMarker =
        root.filePath(QStringLiteral("credential-helper-ran"));
    const QString localRewrite =
        QUrl::fromLocalFile(source).toString();
    check(writeFile(
              hostileConfig,
              QStringLiteral(
                  "[url \"%1\"]\n"
                  "    insteadOf = https://127.0.0.1:9/private.git\n"
                  "[protocol \"file\"]\n"
                  "    allow = always\n"
                  "[credential]\n"
                  "    helper = !touch %2\n")
                  .arg(localRewrite, helperMarker)
                  .toUtf8()),
          "hostile global Git configuration is staged");
    const QByteArray previousGlobalConfig =
        qgetenv("GIT_CONFIG_GLOBAL");
    qputenv("GIT_CONFIG_GLOBAL", hostileConfig.toUtf8());
    const auto hostileConfigResult =
        PrivateMirrorRuntime::syncSource(
            QStringLiteral("https://127.0.0.1:9/private.git"),
            {}, replicaRoot, vaultPath, vaultSecret, QString(), {},
            &error);
    if (previousGlobalConfig.isNull())
        qunsetenv("GIT_CONFIG_GLOBAL");
    else
        qputenv("GIT_CONFIG_GLOBAL", previousGlobalConfig);
    check(!hostileConfigResult.isValid() &&
              !QFileInfo::exists(helperMarker),
          "remote clone ignores global URL rewrites and credential helpers");
    check(QDir(replicaRoot)
                  .entryList({QStringLiteral("*.fm-private")},
                             QDir::Files)
                  .size() == replicaCountBeforeHostileSources,
          "rejected remote inputs create no encrypted replica or plaintext mirror");

    if (failures == 0)
        std::fprintf(stdout, "private-mirror runtime tests passed\n");
    return failures == 0 ? 0 : 1;
}
