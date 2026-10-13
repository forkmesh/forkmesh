#include "PrivateMirrorRuntime.h"
#include "SafeDelete.h"

#include "CoveCrypto.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QUrl>

#include <openssl/crypto.h>

#include <limits>

namespace {

constexpr qint64 kMaximumVaultBytes = 1024 * 1024;
constexpr int kGitTimeoutMs = 10 * 60 * 1000;

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

void clearBytes(QByteArray *bytes)
{
    if (!bytes || bytes->isEmpty())
        return;
    OPENSSL_cleanse(bytes->data(), size_t(bytes->size()));
    bytes->clear();
}

void clearIdentity(MirrorCrypto::Identity *identity)
{
    if (!identity)
        return;
    clearBytes(&identity->x25519Priv);
    clearBytes(&identity->mlkemPriv);
    identity->x25519Pub.clear();
    identity->mlkemPub.clear();
}

QString b64(const QByteArray &value)
{
    return QString::fromLatin1(value.toBase64());
}

QByteArray strictB64(const QJsonObject &object, const QString &name,
                     qsizetype expectedSize)
{
    const QString encoded = object.value(name).toString();
    static const QRegularExpression pattern(
        QStringLiteral("^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|"
                       "[A-Za-z0-9+/]{3}=)?$"));
    if (encoded.isEmpty() || !pattern.match(encoded).hasMatch())
        return {};
    const QByteArray decoded = QByteArray::fromBase64(encoded.toLatin1());
    if (decoded.size() != expectedSize ||
        QString::fromLatin1(decoded.toBase64()) != encoded) {
        return {};
    }
    return decoded;
}

bool prepareOwnerDirectory(const QString &path, QString *error)
{
    if (path.trimmed().isEmpty()) {
        setError(error, QStringLiteral("An owner-only directory is required."));
        return false;
    }
    QDir directory(path);
    if (!directory.exists() && !QDir().mkpath(directory.absolutePath())) {
        setError(error, QStringLiteral("Could not create the owner-only directory."));
        return false;
    }
    const QFileInfo info(directory.absolutePath());
    if (!info.isDir() || info.isSymLink()) {
        setError(error, QStringLiteral("The owner-only path must be a real directory."));
        return false;
    }
    QFile permissions(directory.absolutePath());
    if (!permissions.setPermissions(QFileDevice::ReadOwner |
                                    QFileDevice::WriteOwner |
                                    QFileDevice::ExeOwner)) {
        setError(error, QStringLiteral("Could not restrict the owner-only directory."));
        return false;
    }
    return true;
}

QByteArray vaultKey(const QByteArray &secret, const QByteArray &salt)
{
    if (secret.size() < 32 || salt.size() != 32)
        return {};
    QByteArray input =
        QByteArrayLiteral("forkmesh-private-mirror-vault-key-v1\n");
    input += secret;
    input += '\n';
    input += salt;
    QByteArray key =
        QCryptographicHash::hash(input, QCryptographicHash::Sha256);
    clearBytes(&input);
    return key;
}

QJsonObject identityObject(const MirrorCrypto::Identity &identity)
{
    return {
        {QStringLiteral("kind"),
         QStringLiteral("forkmesh.private-mirror.identity")},
        {QStringLiteral("v"), 1},
        {QStringLiteral("x25519Public"), b64(identity.x25519Pub)},
        {QStringLiteral("x25519Private"), b64(identity.x25519Priv)},
        {QStringLiteral("mlkem768Public"), b64(identity.mlkemPub)},
        {QStringLiteral("mlkem768Private"), b64(identity.mlkemPriv)},
    };
}

MirrorCrypto::Identity parseIdentity(const QByteArray &plaintext)
{
    const QJsonObject object = QJsonDocument::fromJson(plaintext).object();
    if (object.value(QStringLiteral("kind")).toString() !=
            QLatin1String("forkmesh.private-mirror.identity") ||
        object.value(QStringLiteral("v")).toInt() != 1) {
        return {};
    }
    MirrorCrypto::Identity identity;
    identity.x25519Pub =
        strictB64(object, QStringLiteral("x25519Public"), 32);
    identity.x25519Priv =
        strictB64(object, QStringLiteral("x25519Private"), 32);
    identity.mlkemPub =
        strictB64(object, QStringLiteral("mlkem768Public"), 1184);
    identity.mlkemPriv =
        strictB64(object, QStringLiteral("mlkem768Private"), 2400);
    if (!identity.isValid()) {
        clearIdentity(&identity);
        return {};
    }
    return identity;
}

bool writeVault(const QString &vaultPath, const QByteArray &vaultSecret,
                const MirrorCrypto::Identity &identity, QString *error)
{
    if (!prepareOwnerDirectory(QFileInfo(vaultPath).absolutePath(), error))
        return false;
    const QFileInfo existing(vaultPath);
    if (existing.exists() && (existing.isSymLink() || !existing.isFile())) {
        setError(error, QStringLiteral("The private-mirror vault path is unsafe."));
        return false;
    }

    QByteArray plaintext =
        QJsonDocument(identityObject(identity)).toJson(QJsonDocument::Compact);
    const QByteArray salt = CoveCrypto::randomBytes(32);
    QByteArray key = vaultKey(vaultSecret, salt);
    const CoveCrypto crypto = CoveCrypto::withKey(key);
    clearBytes(&key);
    const QJsonObject cipher = crypto.encrypt(plaintext);
    clearBytes(&plaintext);
    if (cipher.isEmpty()) {
        setError(error, QStringLiteral("Could not encrypt the private-mirror identity."));
        return false;
    }
    const QJsonObject vault{
        {QStringLiteral("kind"),
         QStringLiteral("forkmesh.private-mirror.identity-vault")},
        {QStringLiteral("v"), 1},
        {QStringLiteral("kdf"), QStringLiteral("sha256-device-secret-v1")},
        {QStringLiteral("salt"), b64(salt)},
        {QStringLiteral("keyId"), identity.keyId()},
        {QStringLiteral("cipher"), cipher},
    };
    QSaveFile file(vaultPath);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) ||
        !file.setPermissions(QFileDevice::ReadOwner |
                             QFileDevice::WriteOwner)) {
        file.cancelWriting();
        setError(error, QStringLiteral("Could not create the private-mirror identity vault."));
        return false;
    }
    const QByteArray bytes =
        QJsonDocument(vault).toJson(QJsonDocument::Compact);
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        setError(error, QStringLiteral("Could not commit the private-mirror identity vault."));
        return false;
    }
    return true;
}

bool readVault(const QString &vaultPath, const QByteArray &vaultSecret,
               MirrorCrypto::Identity *identity, QString *error)
{
    const QFileInfo info(vaultPath);
    if (!info.exists() || !info.isFile() || info.isSymLink() ||
        info.size() <= 0 || info.size() > kMaximumVaultBytes) {
        setError(error, QStringLiteral("The private-mirror identity vault is invalid."));
        return false;
    }
    QFile file(vaultPath);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("Could not open the private-mirror identity vault."));
        return false;
    }
    const QJsonObject vault = QJsonDocument::fromJson(file.readAll()).object();
    if (vault.value(QStringLiteral("kind")).toString() !=
            QLatin1String("forkmesh.private-mirror.identity-vault") ||
        vault.value(QStringLiteral("v")).toInt() != 1 ||
        vault.value(QStringLiteral("kdf")).toString() !=
            QLatin1String("sha256-device-secret-v1")) {
        setError(error, QStringLiteral("The private-mirror identity vault format is invalid."));
        return false;
    }
    const QByteArray salt = strictB64(vault, QStringLiteral("salt"), 32);
    QByteArray key = vaultKey(vaultSecret, salt);
    if (key.isEmpty()) {
        setError(error, QStringLiteral("The device vault secret is unavailable."));
        return false;
    }
    const CoveCrypto crypto = CoveCrypto::withKey(key);
    clearBytes(&key);
    QByteArray plaintext =
        crypto.decrypt(vault.value(QStringLiteral("cipher")).toObject());
    MirrorCrypto::Identity loaded = parseIdentity(plaintext);
    clearBytes(&plaintext);
    if (!loaded.isValid() ||
        loaded.keyId() != vault.value(QStringLiteral("keyId")).toString()) {
        clearIdentity(&loaded);
        setError(error,
                 QStringLiteral("The private-mirror vault could not be authenticated."));
        return false;
    }
    *identity = std::move(loaded);
    return true;
}

bool safeGitPrefix(const QStringList &arguments)
{
    if (arguments.size() % 2 != 0)
        return false;
    for (int i = 0; i < arguments.size(); i += 2) {
        if (arguments.at(i) != QLatin1String("-c"))
            return false;
        const QString setting = arguments.at(i + 1);
        if (setting.size() > 4096 || setting.contains(QChar(u'\0')) ||
            setting.contains(QLatin1Char('\r')) ||
            setting.contains(QLatin1Char('\n')) ||
            !setting.startsWith(
                QStringLiteral("http.extraHeader=Authorization: Basic "))) {
            return false;
        }
    }
    return true;
}

bool safeHttpsSource(const QString &source)
{
    if (source.trimmed() != source || source.isEmpty() ||
        source.contains(QChar(u'\0')) ||
        source.contains(QLatin1Char('\r')) ||
        source.contains(QLatin1Char('\n'))) {
        return false;
    }
    const QUrl url(source, QUrl::StrictMode);
    const QString host = url.host().trimmed();
    QHostAddress literalAddress;
    return url.isValid() &&
           url.scheme().compare(QStringLiteral("https"),
                                Qt::CaseInsensitive) == 0 &&
           !host.isEmpty() && !literalAddress.setAddress(host) &&
           host.compare(QStringLiteral("localhost"),
                        Qt::CaseInsensitive) != 0 &&
           !host.endsWith(QStringLiteral(".localhost"),
                          Qt::CaseInsensitive) &&
           url.userInfo().isEmpty() && url.query().isEmpty() &&
           url.fragment().isEmpty();
}

// code-health: allow(blocking-wait) every caller (syncSource, materialize, resealRecipients) runs on QThread::create workers in MainWindowRepos
bool runGit(const QStringList &arguments, QByteArray *stdoutBytes,
            QString *error, bool remoteHttps = false)
{
    QProcess process;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("GIT_TERMINAL_PROMPT"),
                       QStringLiteral("0"));
    if (remoteHttps) {
        // A private remote clone must not inherit a system/global credential
        // helper, URL rewrite, protocol policy, or hook path. Only the explicit
        // HTTPS source and short-lived Authorization header are admitted.
        environment.insert(QStringLiteral("GIT_CONFIG_NOSYSTEM"),
                           QStringLiteral("1"));
        environment.insert(QStringLiteral("GIT_CONFIG_SYSTEM"),
                           QProcess::nullDevice());
        environment.insert(QStringLiteral("GIT_CONFIG_GLOBAL"),
                           QProcess::nullDevice());
        environment.insert(QStringLiteral("GIT_PROTOCOL_FROM_USER"),
                           QStringLiteral("0"));
        environment.insert(QStringLiteral("GIT_ALLOW_PROTOCOL"),
                           QStringLiteral("https"));
        // Keep loopback probes and explicitly local private remotes away from
        // inherited HTTP proxies. Besides avoiding credential disclosure to a
        // proxy, this makes an unavailable local endpoint fail immediately
        // instead of waiting for the proxy's network timeout.
        const QString loopbackNoProxy =
            QStringLiteral("127.0.0.1,localhost,::1");
        for (const QString &key :
             {QStringLiteral("NO_PROXY"), QStringLiteral("no_proxy")}) {
            QString value = environment.value(key).trimmed();
            if (!value.isEmpty())
                value += QLatin1Char(',');
            environment.insert(key, value + loopbackNoProxy);
        }
    }
    process.setProcessEnvironment(environment);
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.setStandardErrorFile(QProcess::nullDevice());
    QStringList hardenedArguments = arguments;
    if (remoteHttps) {
        const QStringList policy{
            QStringLiteral("-c"), QStringLiteral("protocol.allow=never"),
            QStringLiteral("-c"), QStringLiteral("protocol.https.allow=always"),
            QStringLiteral("-c"), QStringLiteral("protocol.ext.allow=never"),
            QStringLiteral("-c"), QStringLiteral("protocol.file.allow=never"),
            QStringLiteral("-c"), QStringLiteral("credential.helper="),
            QStringLiteral("-c"),
            QStringLiteral("core.hooksPath=") + QProcess::nullDevice(),
            QStringLiteral("-c"),
            QStringLiteral("http.followRedirects=false"),
        };
        hardenedArguments = policy + arguments;
    }
    process.start(QStringLiteral("git"), hardenedArguments);
    if (!process.waitForStarted(10000) ||
        !process.waitForFinished(kGitTimeoutMs)) {
        process.kill();
        process.waitForFinished(5000);
        setError(error, QStringLiteral("The private repository Git operation timed out."));
        return false;
    }
    if (process.exitStatus() != QProcess::NormalExit ||
        process.exitCode() != 0) {
        setError(error, QStringLiteral("The private repository Git operation failed."));
        return false;
    }
    QByteArray output = process.readAllStandardOutput();
    if (output.size() >
        PrivateMirrorRuntime::maximumSerializedReplicaBytes()) {
        clearBytes(&output);
        setError(error, QStringLiteral("The private repository exceeds the encrypted-replica limit."));
        return false;
    }
    if (stdoutBytes)
        *stdoutBytes = std::move(output);
    else
        clearBytes(&output);
    return true;
}

bool bundleRepository(const QString &repositoryPath, QByteArray *archive,
                      QString *error)
{
    const QFileInfo info(repositoryPath);
    if (!info.exists() || !info.isDir() || info.isSymLink()) {
        setError(error, QStringLiteral("The private repository source is unavailable."));
        return false;
    }
    QByteArray bytes;
    if (!runGit({QStringLiteral("-C"), info.absoluteFilePath(),
                 QStringLiteral("bundle"), QStringLiteral("create"),
                 QStringLiteral("-"), QStringLiteral("--branches"),
                 QStringLiteral("--tags")},
                &bytes, error) ||
        bytes.isEmpty()) {
        clearBytes(&bytes);
        if (error && error->isEmpty())
            *error = QStringLiteral("The private repository has no bundleable refs.");
        return false;
    }
    *archive = std::move(bytes);
    return true;
}

QList<QJsonObject> recipientsWithOwner(
    const MirrorCrypto::Identity &owner,
    const QList<QJsonObject> &additionalRecipients)
{
    QList<QJsonObject> recipients{owner.publicBundle()};
    QSet<QString> ids{owner.keyId()};
    for (const QJsonObject &bundle : additionalRecipients) {
        const QString id = MirrorCrypto::publicKeyId(bundle);
        if (id.isEmpty() || ids.contains(id))
            return {};
        ids.insert(id);
        recipients.append(bundle);
    }
    return recipients;
}

PrivateMirrorRuntime::SyncResult sealArchive(
    QByteArray *archive, const QString &replicaRoot,
    const QString &vaultPath, const QByteArray &vaultSecret,
    const QString &existingOpaqueId,
    const QList<QJsonObject> &additionalRecipientBundles, QString *error)
{
    PrivateMirrorRuntime::SyncResult result;
    MirrorCrypto::Identity owner;
    if (!PrivateMirrorRuntime::loadOrCreateIdentity(
            vaultPath, vaultSecret, &owner, error)) {
        clearBytes(archive);
        return result;
    }
    const QList<QJsonObject> recipients =
        recipientsWithOwner(owner, additionalRecipientBundles);
    if (recipients.isEmpty()) {
        clearIdentity(&owner);
        clearBytes(archive);
        setError(error, QStringLiteral("A private-mirror recipient bundle is invalid or duplicated."));
        return result;
    }
    // The sealed replica exists only on this device. When its file is gone
    // (a lost data folder, 2026-09-22) there is nothing left to replace, and
    // insisting on the old id failed every later sync with "The private
    // replica was not found." Seal a fresh replica from the source instead;
    // the caller records the new opaque id.
    const QString existingPath =
        PrivateMirrorStore::isOpaqueId(existingOpaqueId.trimmed())
            ? QDir(replicaRoot).filePath(existingOpaqueId.trimmed() +
                                         QStringLiteral(".fm-private"))
            : QString();
    if (existingOpaqueId.trimmed().isEmpty() ||
        (!existingPath.isEmpty() && !QFileInfo::exists(existingPath))) {
        result.opaqueId = PrivateMirrorStore::createReplica(
            replicaRoot, *archive, recipients, error);
        result.created = !result.opaqueId.isEmpty();
    } else {
        result.opaqueId = existingOpaqueId.trimmed();
        if (!PrivateMirrorStore::replaceReplica(
                replicaRoot, result.opaqueId, *archive, owner, recipients,
                error)) {
            result.opaqueId.clear();
        }
    }
    clearBytes(archive);
    clearIdentity(&owner);
    if (result.opaqueId.isEmpty())
        return {};
    if (!PrivateMirrorStore::inspectReplica(
            replicaRoot, result.opaqueId, &result.metadata, error)) {
        return {};
    }
    if (error)
        error->clear();
    return result;
}

} // namespace

PrivateMirrorMaterialization::PrivateMirrorMaterialization(
    std::unique_ptr<QTemporaryDir> directory, QString repositoryPath)
    : m_directory(std::move(directory)),
      m_repositoryPath(std::move(repositoryPath))
{
}

PrivateMirrorMaterialization::~PrivateMirrorMaterialization() = default;

bool PrivateMirrorMaterialization::isValid() const
{
    return m_directory && m_directory->isValid() &&
           !m_repositoryPath.isEmpty() && QDir(m_repositoryPath).exists();
}

bool PrivateMirrorRuntime::loadOrCreateIdentity(
    const QString &vaultPath, const QByteArray &vaultSecret,
    MirrorCrypto::Identity *identity, QString *error)
{
    if (!identity) {
        setError(error, QStringLiteral("An identity destination is required."));
        return false;
    }
    clearIdentity(identity);
    if (vaultSecret.size() < 32) {
        setError(error,
                 QStringLiteral("A 256-bit device vault secret is required."));
        return false;
    }
    if (QFileInfo::exists(vaultPath)) {
        const bool ok = readVault(vaultPath, vaultSecret, identity, error);
        if (ok && error)
            error->clear();
        return ok;
    }
    QString unavailable;
    if (!MirrorCrypto::postQuantumAvailable(&unavailable)) {
        setError(error, unavailable);
        return false;
    }
    MirrorCrypto::Identity generated = MirrorCrypto::generateIdentity();
    if (!generated.isValid()) {
        setError(error, QStringLiteral("Could not generate the private-mirror identity."));
        return false;
    }
    if (!writeVault(vaultPath, vaultSecret, generated, error)) {
        clearIdentity(&generated);
        return false;
    }
    *identity = std::move(generated);
    if (error)
        error->clear();
    return true;
}

PrivateMirrorRuntime::SyncResult PrivateMirrorRuntime::syncRepository(
    const QString &repositoryPath, const QString &replicaRoot,
    const QString &vaultPath, const QByteArray &vaultSecret,
    const QString &existingOpaqueId,
    const QList<QJsonObject> &additionalRecipientBundles, QString *error)
{
    QByteArray archive;
    if (!bundleRepository(repositoryPath, &archive, error))
        return {};
    return sealArchive(&archive, replicaRoot, vaultPath, vaultSecret,
                       existingOpaqueId, additionalRecipientBundles, error);
}

PrivateMirrorRuntime::SyncResult PrivateMirrorRuntime::syncSource(
    const QString &source, const QStringList &gitPrefixArgs,
    const QString &replicaRoot, const QString &vaultPath,
    const QByteArray &vaultSecret, const QString &existingOpaqueId,
    const QList<QJsonObject> &additionalRecipientBundles, QString *error)
{
    const QFileInfo localSource(source);
    if (localSource.exists() && localSource.isDir() &&
        !localSource.isSymLink()) {
        return syncRepository(localSource.absoluteFilePath(), replicaRoot,
                              vaultPath, vaultSecret, existingOpaqueId,
                              additionalRecipientBundles, error);
    }
    if (!safeHttpsSource(source) || !safeGitPrefix(gitPrefixArgs)) {
        setError(error, QStringLiteral("The private repository source is invalid."));
        return {};
    }
    auto directory =
        std::make_unique<QTemporaryDir>(
            QDir::tempPath() +
            QStringLiteral("/forkmesh-private-sync-XXXXXX"));
    if (!directory->isValid() ||
        !prepareOwnerDirectory(directory->path(), error)) {
        return {};
    }
    const QString temporaryMirror =
        QDir(directory->path()).filePath(QStringLiteral("source.git"));
    const QStringList cloneArgs =
        gitPrefixArgs +
        QStringList{QStringLiteral("clone"), QStringLiteral("--mirror"),
                    source.trimmed(), temporaryMirror};
    if (!runGit(cloneArgs, nullptr, error, /*remoteHttps=*/true))
        return {};
    return syncRepository(temporaryMirror, replicaRoot, vaultPath,
                          vaultSecret, existingOpaqueId,
                          additionalRecipientBundles, error);
}

bool PrivateMirrorRuntime::resealRecipients(
    const QString &replicaRoot, const QString &vaultPath,
    const QByteArray &vaultSecret, const QString &opaqueId,
    const QList<QJsonObject> &remainingRecipientBundles,
    PrivateMirrorStore::Metadata *metadata, QString *error)
{
    MirrorCrypto::Identity owner;
    if (!loadOrCreateIdentity(vaultPath, vaultSecret, &owner, error))
        return false;
    const QList<QJsonObject> recipients =
        recipientsWithOwner(owner, remainingRecipientBundles);
    if (recipients.isEmpty()) {
        clearIdentity(&owner);
        setError(error, QStringLiteral("A remaining private-mirror recipient is invalid or duplicated."));
        return false;
    }
    const bool rotated = PrivateMirrorStore::rotateRecipients(
        replicaRoot, opaqueId, owner, recipients, error);
    clearIdentity(&owner);
    if (!rotated)
        return false;
    PrivateMirrorStore::Metadata localMetadata;
    if (!PrivateMirrorStore::inspectReplica(
            replicaRoot, opaqueId,
            metadata ? metadata : &localMetadata, error)) {
        return false;
    }
    if (error)
        error->clear();
    return true;
}

std::unique_ptr<PrivateMirrorMaterialization>
PrivateMirrorRuntime::materialize(
    const QString &replicaRoot, const QString &vaultPath,
    const QByteArray &vaultSecret, const QString &opaqueId, QString *error)
{
    MirrorCrypto::Identity owner;
    if (!loadOrCreateIdentity(vaultPath, vaultSecret, &owner, error))
        return {};
    QByteArray archive = PrivateMirrorStore::openReplica(
        replicaRoot, opaqueId, owner, nullptr, error);
    clearIdentity(&owner);
    if (archive.isEmpty())
        return {};

    auto directory =
        std::make_unique<QTemporaryDir>(
            QDir::tempPath() +
            QStringLiteral("/forkmesh-private-open-XXXXXX"));
    if (!directory->isValid() ||
        !prepareOwnerDirectory(directory->path(), error)) {
        clearBytes(&archive);
        return {};
    }
    const QString bundlePath =
        QDir(directory->path()).filePath(QStringLiteral("replica.bundle"));
    QSaveFile bundle(bundlePath);
    bundle.setDirectWriteFallback(false);
    if (!bundle.open(QIODevice::WriteOnly) ||
        !bundle.setPermissions(QFileDevice::ReadOwner |
                               QFileDevice::WriteOwner) ||
        bundle.write(archive) != archive.size() ||
        !bundle.commit()) {
        bundle.cancelWriting();
        clearBytes(&archive);
        setError(error, QStringLiteral("Could not stage the private replica for local use."));
        return {};
    }
    clearBytes(&archive);

    const QString repositoryPath =
        QDir(directory->path()).filePath(QStringLiteral("repository.git"));
    const bool cloned =
        runGit({QStringLiteral("clone"), QStringLiteral("--mirror"),
                bundlePath, repositoryPath},
               nullptr, error);
    QFile::remove(bundlePath);
    if (!cloned)
        return {};
    QFile repositoryPermissions(repositoryPath);
    if (!repositoryPermissions.setPermissions(
            QFileDevice::ReadOwner | QFileDevice::WriteOwner |
            QFileDevice::ExeOwner)) {
        setError(error, QStringLiteral("Could not restrict the temporary private repository."));
        return {};
    }
    auto materialization = std::unique_ptr<PrivateMirrorMaterialization>(
        new PrivateMirrorMaterialization(std::move(directory),
                                         repositoryPath));
    if (!materialization->isValid()) {
        setError(error, QStringLiteral("The temporary private repository is invalid."));
        return {};
    }
    if (error)
        error->clear();
    return materialization;
}

bool PrivateMirrorRuntime::removeManagedPlaintextMirror(
    const QString &mirrorPath, const QString &managedMirrorRoot,
    QString *error)
{
    if (mirrorPath.trimmed().isEmpty() || !QFileInfo::exists(mirrorPath)) {
        if (error)
            error->clear();
        return true;
    }
    const QFileInfo mirrorInfo(mirrorPath);
    const QFileInfo rootInfo(managedMirrorRoot);
    if (!mirrorInfo.isDir() || mirrorInfo.isSymLink() ||
        !rootInfo.isDir() || rootInfo.isSymLink()) {
        setError(error, QStringLiteral("The legacy private mirror path is unsafe."));
        return false;
    }
    const QString mirror = QDir::cleanPath(mirrorInfo.canonicalFilePath());
    const QString root = QDir::cleanPath(rootInfo.canonicalFilePath());
    if (mirror.isEmpty() || root.isEmpty() || mirror == root ||
        !mirror.startsWith(root + QDir::separator()) ||
        !mirror.endsWith(QStringLiteral(".git"))) {
        setError(error,
                 QStringLiteral("The legacy private mirror is outside the managed mirror root."));
        return false;
    }
    if (!forkmesh::safedelete::removeTree(mirror, root)) {
        setError(error, QStringLiteral("Could not remove the legacy plaintext private mirror."));
        return false;
    }
    if (error)
        error->clear();
    return true;
}
