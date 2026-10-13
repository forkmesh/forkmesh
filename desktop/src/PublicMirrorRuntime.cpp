#include "PublicMirrorRuntime.h"
#include "SafeDelete.h"

#include "CoveCrypto.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>

#include <openssl/crypto.h>

#include <algorithm>

namespace {

constexpr qint64 kMaximumVaultBytes = 4LL * 1024 * 1024;
constexpr qint64 kMaximumMetadataBytes = 64LL * 1024;
constexpr qint64 kMaximumIdentityBytes = 64LL * 1024;
constexpr int kProcessTimeoutMs = 30 * 60 * 1000;
constexpr auto kAgeHeader = "age-encryption.org/v1\n";

struct IdentityEntry {
    QString archiveId;
    QString recipient;
    QByteArray secretIdentity;

    bool isValid() const
    {
        static const QRegularExpression recipientPattern(
            QStringLiteral("^age1[023456789acdefghjklmnpqrstuvwxyz]{20,100}$"));
        static const QRegularExpression secretPattern(
            QStringLiteral("^AGE-SECRET-KEY-1[023456789ACDEFGHJKLMNPQRSTUVWXYZ]{20,180}$"));
        return PublicMirrorRuntime::isArchiveId(archiveId) &&
               recipientPattern.match(recipient).hasMatch() &&
               secretPattern.match(
                   QString::fromLatin1(secretIdentity)).hasMatch();
    }
};

struct Vault {
    QHash<QString, IdentityEntry> entries;
};

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
    if ((expectedSize >= 0 && decoded.size() != expectedSize) ||
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

QStringList extraProgramSearchDirs()
{
    QStringList dirs{QStringLiteral("/opt/homebrew/bin"),
                     QStringLiteral("/usr/local/bin"),
                     QStringLiteral("/usr/bin"),
                     QStringLiteral("/bin")};
    const QString home = QDir::homePath();
    if (!home.isEmpty()) {
        dirs << QDir(home).filePath(QStringLiteral(".local/bin"))
             << QDir(home).filePath(QStringLiteral("bin"))
             << QDir(home).filePath(QStringLiteral("go/bin"))
             << QDir(home).filePath(QStringLiteral(".cargo/bin"));
    }
    return dirs;
}

QString resolveProgram(const QString &configured)
{
    const QString value = configured.trimmed();
    if (value.isEmpty() || value.contains(QChar(u'\0')) ||
        value.contains(QLatin1Char('\r')) ||
        value.contains(QLatin1Char('\n'))) {
        return {};
    }
    const QFileInfo explicitInfo(value);
    if (explicitInfo.isAbsolute()) {
        return explicitInfo.exists() && explicitInfo.isFile() &&
                       !explicitInfo.isSymLink() && explicitInfo.isExecutable()
                   ? explicitInfo.absoluteFilePath()
                   : QString();
    }
    const QString onPath = QStandardPaths::findExecutable(value);
    if (!onPath.isEmpty())
        return onPath;
    return QStandardPaths::findExecutable(value, extraProgramSearchDirs());
}

QProcessEnvironment safeEnvironment()
{
    QProcessEnvironment source = QProcessEnvironment::systemEnvironment();
    const QStringList keys = source.keys();
    static const QRegularExpression sensitive(
        QStringLiteral("(SECRET|TOKEN|PRIVATE|SEED|MNEMONIC|KEYPAIR|PASSWORD|"
                       "CREDENTIAL)"),
        QRegularExpression::CaseInsensitiveOption);
    for (const QString &key : keys) {
        if (sensitive.match(key).hasMatch() ||
            key.compare(QStringLiteral("CLOUDFLARE_API_TOKEN"),
                        Qt::CaseInsensitive) == 0 ||
            key.compare(QStringLiteral("CF_API_TOKEN"),
                        Qt::CaseInsensitive) == 0) {
            source.remove(key);
        }
    }
    return source;
}

QProcessEnvironment hardenedGitEnvironment()
{
    QProcessEnvironment environment = safeEnvironment();
    environment.insert(QStringLiteral("GIT_CONFIG_NOSYSTEM"),
                       QStringLiteral("1"));
    environment.insert(QStringLiteral("GIT_CONFIG_SYSTEM"),
                       QProcess::nullDevice());
    environment.insert(QStringLiteral("GIT_CONFIG_GLOBAL"),
                       QProcess::nullDevice());
    environment.insert(QStringLiteral("GIT_TERMINAL_PROMPT"),
                       QStringLiteral("0"));
    environment.insert(QStringLiteral("GIT_PROTOCOL_FROM_USER"),
                       QStringLiteral("0"));
    return environment;
}

bool runProcess(const QString &program, const QStringList &arguments,
                QByteArray *standardOutput, const QString &safeFailure,
                QString *error,
                const QProcessEnvironment &environment = safeEnvironment(),
                const QByteArray &standardInput = {})
{
    QProcess process;
    process.setProcessEnvironment(environment);
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.setStandardErrorFile(QProcess::nullDevice());
    process.start(program, arguments);
    if (!process.waitForStarted(10000)) {
        setError(error, safeFailure);
        return false;
    }
    if (!standardInput.isEmpty())
        process.write(standardInput);
    process.closeWriteChannel();
    if (!process.waitForFinished(kProcessTimeoutMs)) {
        process.kill();
        process.waitForFinished(5000);
        setError(error, safeFailure);
        return false;
    }
    if (process.exitStatus() != QProcess::NormalExit ||
        process.exitCode() != 0) {
        setError(error, safeFailure);
        return false;
    }
    QByteArray output = process.readAllStandardOutput();
    if (output.size() > 16 * 1024 * 1024) {
        clearBytes(&output);
        setError(error, safeFailure);
        return false;
    }
    if (standardOutput)
        *standardOutput = std::move(output);
    else
        clearBytes(&output);
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
    return url.isValid() &&
           url.scheme().compare(QStringLiteral("https"),
                                Qt::CaseInsensitive) == 0 &&
           !url.host().isEmpty() && url.userInfo().isEmpty() &&
           url.query().isEmpty() && url.fragment().isEmpty();
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

bool isLocalSource(const QString &source)
{
    const QFileInfo info(source);
    return info.exists() && info.isDir() && !info.isSymLink();
}

bool cloneSource(const QString &source, const QStringList &gitPrefixArgs,
                 const QString &destination,
                 const PublicMirrorRuntime::Tools &tools, QString *error,
                 bool originBranchesOnly = false)
{
    const bool local = isLocalSource(source);
    if (!local && (!safeHttpsSource(source) ||
                   !safeGitPrefix(gitPrefixArgs))) {
        setError(error,
                 QStringLiteral("The public mirror source must be a local "
                                "repository or a credential-free HTTPS URL."));
        return false;
    }
    if (local && !gitPrefixArgs.isEmpty()) {
        setError(error,
                 QStringLiteral("Authentication arguments are not accepted "
                                "for a local public mirror source."));
        return false;
    }
    const QString git = resolveProgram(tools.git);
    if (git.isEmpty()) {
        setError(error, QStringLiteral("Git is unavailable."));
        return false;
    }
    QStringList arguments;
    arguments << QStringLiteral("-c") << QStringLiteral("pack.threads=1")
              << QStringLiteral("-c")
              << QStringLiteral("core.deltaBaseCacheLimit=16m")
              << QStringLiteral("-c")
              << QStringLiteral("pack.deltaCacheSize=16m")
              << QStringLiteral("-c")
              << QStringLiteral("pack.windowMemory=16m");
    if (!local) {
        arguments << QStringLiteral("-c") << QStringLiteral("protocol.allow=never")
                  << QStringLiteral("-c") << QStringLiteral("protocol.https.allow=always")
                  << QStringLiteral("-c") << QStringLiteral("protocol.ext.allow=never")
                  << QStringLiteral("-c") << QStringLiteral("protocol.file.allow=never")
                  << QStringLiteral("-c") << QStringLiteral("credential.helper=")
                  << QStringLiteral("-c")
                  << QStringLiteral("core.hooksPath=") + QProcess::nullDevice()
                  << QStringLiteral("-c") << QStringLiteral("http.followRedirects=false")
                  << gitPrefixArgs;
    } else {
        arguments << QStringLiteral("-c") << QStringLiteral("protocol.allow=never")
                  << QStringLiteral("-c") << QStringLiteral("protocol.file.allow=always")
                  << QStringLiteral("-c")
                  << QStringLiteral("core.hooksPath=") + QProcess::nullDevice();
    }
    arguments << QStringLiteral("clone") << QStringLiteral("--mirror")
              << QStringLiteral("--no-local") << source << destination;
    if (!runProcess(
        git, arguments, nullptr,
        QStringLiteral("The public mirror Git operation failed."), error,
        hardenedGitEnvironment()))
        return false;
    if (!originBranchesOnly)
        return true;

    QByteArray listing;
    if (!runProcess(
            git,
            {QStringLiteral("-C"), destination,
             QStringLiteral("for-each-ref"),
             QStringLiteral("refs/remotes/origin"),
             QStringLiteral("--format=%(objectname) %(refname:lstrip=3)")},
            &listing,
            QStringLiteral("The managed public mirror refs are unavailable."),
            error, hardenedGitEnvironment()))
        return false;
    QMap<QString, QString> originBranches;
    static const QRegularExpression oidPattern(
        QStringLiteral("^(?:[0-9a-f]{40}|[0-9a-f]{64})$"));
    for (const QByteArray &rawLine : listing.split('\n')) {
        const QByteArray line = rawLine.trimmed();
        const qsizetype space = line.indexOf(' ');
        if (space <= 0 || space + 1 >= line.size())
            continue;
        const QString oid = QString::fromLatin1(line.left(space));
        const QString name = QString::fromUtf8(line.mid(space + 1));
        if (name == QLatin1String("HEAD") ||
            !oidPattern.match(oid).hasMatch())
            continue;
        if (!runProcess(
                git,
                {QStringLiteral("check-ref-format"),
                 QStringLiteral("--branch"), name},
                nullptr,
                QStringLiteral("The managed public mirror branch is invalid."),
                error, hardenedGitEnvironment()))
            return false;
        originBranches.insert(name, oid);
    }
    if (originBranches.isEmpty()) {
        setError(error, QStringLiteral(
                            "The managed checkout has no fetched origin branches."));
        return false;
    }

    listing.clear();
    if (!runProcess(
            git,
            {QStringLiteral("-C"), destination,
             QStringLiteral("for-each-ref"), QStringLiteral("refs/heads"),
             QStringLiteral("--format=%(refname:lstrip=2)")},
            &listing,
            QStringLiteral("The managed public mirror refs are unavailable."),
            error, hardenedGitEnvironment()))
        return false;
    for (const QByteArray &rawLine : listing.split('\n')) {
        const QString name = QString::fromUtf8(rawLine.trimmed());
        if (name.isEmpty() || originBranches.contains(name))
            continue;
        if (!runProcess(
                git,
                {QStringLiteral("-C"), destination,
                 QStringLiteral("update-ref"), QStringLiteral("-d"),
                 QStringLiteral("refs/heads/") + name},
                nullptr,
                QStringLiteral("The managed public mirror ref cleanup failed."),
                error, hardenedGitEnvironment()))
            return false;
    }
    for (auto it = originBranches.constBegin();
         it != originBranches.constEnd(); ++it) {
        if (!runProcess(
                git,
                {QStringLiteral("-C"), destination,
                 QStringLiteral("update-ref"),
                 QStringLiteral("refs/heads/") + it.key(), it.value()},
                nullptr,
                QStringLiteral("The managed public mirror ref update failed."),
                error, hardenedGitEnvironment()))
            return false;
    }
    return true;
}

bool isBareRepository(const QString &path,
                      const PublicMirrorRuntime::Tools &tools,
                      QString *error)
{
    const QFileInfo info(path);
    if (!info.isDir() || info.isSymLink()) {
        setError(error, QStringLiteral("The materialized public mirror is unavailable."));
        return false;
    }
    const QString git = resolveProgram(tools.git);
    QByteArray output;
    if (git.isEmpty() ||
        !runProcess(git,
                    {QStringLiteral("-C"), info.absoluteFilePath(),
                     QStringLiteral("rev-parse"),
                     QStringLiteral("--is-bare-repository")},
                    &output,
                    QStringLiteral("The materialized public mirror is invalid."),
                    error, hardenedGitEnvironment()) ||
        output.trimmed() != QByteArrayLiteral("true")) {
        setError(error, QStringLiteral("The materialized public mirror is invalid."));
        return false;
    }
    return true;
}

QString refsSha256(const QString &path,
                   const PublicMirrorRuntime::Tools &tools, QString *error)
{
    const QString git = resolveProgram(tools.git);
    QByteArray output;
    if (git.isEmpty() ||
        !runProcess(git,
                    {QStringLiteral("-C"), path,
                     QStringLiteral("for-each-ref"),
                     QStringLiteral("--sort=refname"),
                     QStringLiteral("--format=%(objectname) %(refname)"),
                     QStringLiteral("refs/heads/"),
                     QStringLiteral("refs/tags/")},
                    &output,
                    QStringLiteral("Could not verify the public mirror refs."),
                    error, hardenedGitEnvironment())) {
        return {};
    }
    return PublicMirrorRuntime::refsSha256FromForEachRef(output);
}

QString managedRefsSha256(const QString &path,
                          const PublicMirrorRuntime::Tools &tools,
                          QString *error)
{
    const QString git = resolveProgram(tools.git);
    QByteArray branches;
    QByteArray tags;
    if (git.isEmpty() ||
        !runProcess(
            git,
            {QStringLiteral("-C"), path, QStringLiteral("for-each-ref"),
             QStringLiteral("--sort=refname"),
             QStringLiteral("--format=%(objectname) refs/heads/%(refname:lstrip=3)"),
             QStringLiteral("refs/remotes/origin/")},
            &branches,
            QStringLiteral("Could not verify the managed public mirror refs."),
            error, hardenedGitEnvironment()) ||
        !runProcess(
            git,
            {QStringLiteral("-C"), path, QStringLiteral("for-each-ref"),
             QStringLiteral("--sort=refname"),
             QStringLiteral("--format=%(objectname) %(refname)"),
             QStringLiteral("refs/tags/")},
            &tags,
            QStringLiteral("Could not verify the managed public mirror refs."),
            error, hardenedGitEnvironment())) {
        return {};
    }
    QList<QByteArray> publishable;
    for (const QByteArray &rawLine : branches.split('\n')) {
        const QByteArray line = rawLine.trimmed();
        if (!line.isEmpty() &&
            !line.endsWith(QByteArrayLiteral(" refs/heads/HEAD")))
            publishable.append(line);
    }
    if (publishable.isEmpty()) {
        setError(error, QStringLiteral(
                            "The managed checkout has no fetched origin branches."));
        return {};
    }
    publishable.append(tags.trimmed());
    return PublicMirrorRuntime::refsSha256FromForEachRef(
        QByteArrayList(publishable).join('\n'));
}

QByteArray vaultKey(const QByteArray &secret, const QByteArray &salt)
{
    if (secret.size() < 32 || salt.size() != 32)
        return {};
    QByteArray input =
        QByteArrayLiteral("forkmesh-public-age-vault-key-v1\n");
    input += secret;
    input += '\n';
    input += salt;
    QByteArray key =
        QCryptographicHash::hash(input, QCryptographicHash::Sha256);
    clearBytes(&input);
    return key;
}

QJsonObject identityObject(const IdentityEntry &entry)
{
    return {
        {QStringLiteral("archiveId"), entry.archiveId},
        {QStringLiteral("recipient"), entry.recipient},
        {QStringLiteral("secretIdentity"),
         b64(entry.secretIdentity)},
    };
}

bool writeVault(const QString &vaultPath, const QByteArray &vaultSecret,
                const Vault &vault, QString *error)
{
    if (vaultSecret.size() < 32 ||
        !prepareOwnerDirectory(QFileInfo(vaultPath).absolutePath(), error)) {
        if (vaultSecret.size() < 32)
            setError(error, QStringLiteral("The device vault secret is unavailable."));
        return false;
    }
    const QFileInfo existing(vaultPath);
    if (existing.exists() && (existing.isSymLink() || !existing.isFile())) {
        setError(error, QStringLiteral("The public-mirror vault path is unsafe."));
        return false;
    }
    QJsonArray entries;
    QStringList ids = vault.entries.keys();
    ids.sort();
    for (const QString &id : ids) {
        const IdentityEntry entry = vault.entries.value(id);
        if (!entry.isValid()) {
            setError(error, QStringLiteral("The public-mirror vault contains an invalid identity."));
            return false;
        }
        entries.append(identityObject(entry));
    }
    QByteArray plaintext =
        QJsonDocument(QJsonObject{
                          {QStringLiteral("kind"),
                           QStringLiteral("forkmesh.public-age-identities")},
                          {QStringLiteral("v"), 1},
                          {QStringLiteral("entries"), entries},
                      })
            .toJson(QJsonDocument::Compact);
    const QByteArray salt = CoveCrypto::randomBytes(32);
    QByteArray key = vaultKey(vaultSecret, salt);
    const CoveCrypto crypto = CoveCrypto::withKey(key);
    clearBytes(&key);
    const QJsonObject cipher = crypto.encrypt(plaintext);
    clearBytes(&plaintext);
    if (cipher.isEmpty()) {
        setError(error, QStringLiteral("Could not encrypt the public-mirror identity vault."));
        return false;
    }
    const QJsonObject envelope{
        {QStringLiteral("kind"),
         QStringLiteral("forkmesh.public-age.identity-vault")},
        {QStringLiteral("v"), 1},
        {QStringLiteral("kdf"), QStringLiteral("sha256-device-secret-v1")},
        {QStringLiteral("salt"), b64(salt)},
        {QStringLiteral("cipher"), cipher},
    };
    QSaveFile file(vaultPath);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) ||
        !file.setPermissions(QFileDevice::ReadOwner |
                             QFileDevice::WriteOwner)) {
        file.cancelWriting();
        setError(error, QStringLiteral("Could not create the public-mirror identity vault."));
        return false;
    }
    const QByteArray bytes =
        QJsonDocument(envelope).toJson(QJsonDocument::Compact);
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        setError(error, QStringLiteral("Could not commit the public-mirror identity vault."));
        return false;
    }
    return true;
}

bool readVault(const QString &vaultPath, const QByteArray &vaultSecret,
               Vault *vault, bool allowMissing, QString *error)
{
    vault->entries.clear();
    const QFileInfo info(vaultPath);
    if (!info.exists())
        return allowMissing;
    if (!info.isFile() || info.isSymLink() || info.size() <= 0 ||
        info.size() > kMaximumVaultBytes) {
        setError(error, QStringLiteral("The public-mirror identity vault is invalid."));
        return false;
    }
    QFile file(vaultPath);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("Could not open the public-mirror identity vault."));
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(file.readAll(), &parseError);
    const QJsonObject envelope = document.object();
    if (parseError.error != QJsonParseError::NoError ||
        !document.isObject() ||
        envelope.value(QStringLiteral("kind")).toString() !=
            QLatin1String("forkmesh.public-age.identity-vault") ||
        envelope.value(QStringLiteral("v")).toInt() != 1 ||
        envelope.value(QStringLiteral("kdf")).toString() !=
            QLatin1String("sha256-device-secret-v1")) {
        setError(error, QStringLiteral("The public-mirror identity vault format is invalid."));
        return false;
    }
    const QByteArray salt = strictB64(envelope, QStringLiteral("salt"), 32);
    QByteArray key = vaultKey(vaultSecret, salt);
    if (key.isEmpty()) {
        setError(error, QStringLiteral("The device vault secret is unavailable."));
        return false;
    }
    const CoveCrypto crypto = CoveCrypto::withKey(key);
    clearBytes(&key);
    QByteArray plaintext =
        crypto.decrypt(envelope.value(QStringLiteral("cipher")).toObject());
    QJsonParseError innerError;
    const QJsonDocument inner =
        QJsonDocument::fromJson(plaintext, &innerError);
    clearBytes(&plaintext);
    const QJsonObject object = inner.object();
    if (innerError.error != QJsonParseError::NoError ||
        !inner.isObject() ||
        object.value(QStringLiteral("kind")).toString() !=
            QLatin1String("forkmesh.public-age-identities") ||
        object.value(QStringLiteral("v")).toInt() != 1 ||
        !object.value(QStringLiteral("entries")).isArray()) {
        setError(error, QStringLiteral("The public-mirror vault could not be authenticated."));
        return false;
    }
    for (const QJsonValue &value :
         object.value(QStringLiteral("entries")).toArray()) {
        const QJsonObject item = value.toObject();
        if (!value.isObject() || item.size() != 3) {
            setError(error, QStringLiteral("The public-mirror vault contains an invalid identity."));
            return false;
        }
        IdentityEntry entry;
        entry.archiveId =
            item.value(QStringLiteral("archiveId")).toString();
        entry.recipient =
            item.value(QStringLiteral("recipient")).toString();
        entry.secretIdentity =
            strictB64(item, QStringLiteral("secretIdentity"), -1);
        if (!entry.isValid() || vault->entries.contains(entry.archiveId)) {
            clearBytes(&entry.secretIdentity);
            setError(error, QStringLiteral("The public-mirror vault contains an invalid identity."));
            return false;
        }
        vault->entries.insert(entry.archiveId, std::move(entry));
    }
    return true;
}

IdentityEntry createIdentity(const QString &archiveId,
                             const PublicMirrorRuntime::Tools &tools,
                             QString *error)
{
    IdentityEntry entry;
    const QString ageKeygen = resolveProgram(tools.ageKeygen);
    if (ageKeygen.isEmpty()) {
        setError(error, QStringLiteral("age-keygen is required for encrypted public mirrors."));
        return entry;
    }
    QTemporaryDir temporary;
    if (!temporary.isValid() ||
        !prepareOwnerDirectory(temporary.path(), error)) {
        if (error && error->isEmpty())
            *error = QStringLiteral("Could not create the temporary age identity directory.");
        return entry;
    }
    const QString identityPath =
        QDir(temporary.path()).filePath(QStringLiteral("identity.txt"));
    if (!runProcess(ageKeygen,
                    {QStringLiteral("-o"), identityPath}, nullptr,
                    QStringLiteral("Could not create the public-mirror age identity."),
                    error)) {
        return entry;
    }
    const QFileInfo identityInfo(identityPath);
    QFile identityFile(identityPath);
    if (!identityInfo.isFile() || identityInfo.isSymLink() ||
        identityInfo.size() <= 0 ||
        identityInfo.size() > kMaximumIdentityBytes ||
        !identityFile.setPermissions(QFileDevice::ReadOwner |
                                     QFileDevice::WriteOwner) ||
        !identityFile.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("The generated public-mirror age identity is invalid."));
        return entry;
    }
    QByteArray identityBytes = identityFile.readAll();
    identityFile.close();
    QByteArray secret;
    for (const QByteArray &line : identityBytes.split('\n')) {
        const QByteArray trimmed = line.trimmed();
        if (trimmed.startsWith("AGE-SECRET-KEY-1")) {
            if (!secret.isEmpty()) {
                clearBytes(&identityBytes);
                clearBytes(&secret);
                setError(error, QStringLiteral("The generated public-mirror age identity is ambiguous."));
                return entry;
            }
            secret = trimmed;
        }
    }
    clearBytes(&identityBytes);
    QByteArray recipientOutput;
    if (secret.isEmpty() ||
        !runProcess(ageKeygen,
                    {QStringLiteral("-y"), identityPath}, &recipientOutput,
                    QStringLiteral("Could not derive the public-mirror age recipient."),
                    error)) {
        clearBytes(&secret);
        return entry;
    }
    const QString recipient =
        QString::fromLatin1(recipientOutput.trimmed());
    clearBytes(&recipientOutput);
    entry.archiveId = archiveId;
    entry.recipient = recipient;
    entry.secretIdentity = std::move(secret);
    if (!entry.isValid()) {
        clearBytes(&entry.secretIdentity);
        setError(error, QStringLiteral("The generated public-mirror age identity is invalid."));
        return {};
    }
    return entry;
}

QString metadataPath(const QString &archiveRoot, const QString &archiveId)
{
    return QDir(archiveRoot).filePath(archiveId + QStringLiteral(".json"));
}

bool fileSha256(const QString &path, QString *digest, QString *error)
{
    const QFileInfo info(path);
    if (!info.isFile() || info.isSymLink() || info.size() <= 0) {
        setError(error, QStringLiteral("The encrypted public mirror is unavailable."));
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("The encrypted public mirror is unavailable."));
        return false;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        setError(error, QStringLiteral("Could not authenticate the encrypted public mirror."));
        return false;
    }
    *digest = QString::fromLatin1(hash.result().toHex());
    return true;
}

bool hasAgeHeader(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) &&
           file.read(qsizetype(std::char_traits<char>::length(kAgeHeader))) ==
               QByteArray(kAgeHeader);
}

bool writeMetadata(const QString &archiveRoot,
                   const PublicMirrorRuntime::Metadata &metadata,
                   QString *error)
{
    if (!metadata.isValid() ||
        !prepareOwnerDirectory(archiveRoot, error))
        return false;
    const QJsonObject object{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("kind"),
         QStringLiteral("forkmesh.public-age.archive")},
        {QStringLiteral("archiveId"), metadata.archiveId},
        {QStringLiteral("scheme"),
         QStringLiteral("age-encrypted-tar-v1")},
        {QStringLiteral("ciphertextSha256"),
         metadata.ciphertextSha256},
        {QStringLiteral("keyReference"), metadata.keyReference},
        {QStringLiteral("recipient"), metadata.recipient},
        {QStringLiteral("expectedRefsSha256"),
         metadata.expectedRefsSha256},
        {QStringLiteral("createdAtMs"),
         QString::number(metadata.createdAtMs)},
        {QStringLiteral("updatedAtMs"),
         QString::number(metadata.updatedAtMs)},
    };
    QSaveFile file(metadataPath(archiveRoot, metadata.archiveId));
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) ||
        !file.setPermissions(QFileDevice::ReadOwner |
                             QFileDevice::WriteOwner)) {
        file.cancelWriting();
        setError(error, QStringLiteral("Could not create the encrypted public-mirror metadata."));
        return false;
    }
    const QByteArray bytes =
        QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        setError(error, QStringLiteral("Could not commit the encrypted public-mirror metadata."));
        return false;
    }
    return true;
}

bool copyAtomically(const QString &source, const QString &destination,
                    QString *error)
{
    QFile input(source);
    QSaveFile output(destination);
    output.setDirectWriteFallback(false);
    if (!input.open(QIODevice::ReadOnly) ||
        !output.open(QIODevice::WriteOnly) ||
        !output.setPermissions(QFileDevice::ReadOwner |
                               QFileDevice::WriteOwner)) {
        output.cancelWriting();
        setError(error, QStringLiteral("Could not stage the encrypted public mirror."));
        return false;
    }
    QByteArray buffer(1024 * 1024, Qt::Uninitialized);
    while (true) {
        const qint64 count = input.read(buffer.data(), buffer.size());
        if (count < 0 || (count > 0 && output.write(buffer.constData(), count) != count)) {
            output.cancelWriting();
            setError(error, QStringLiteral("Could not stage the encrypted public mirror."));
            return false;
        }
        if (count == 0)
            break;
    }
    if (!output.commit()) {
        setError(error, QStringLiteral("Could not commit the encrypted public mirror."));
        return false;
    }
    return true;
}

// code-health: allow(blocking-wait) only reached via sealTemporaryRepository from the QThread::create worker in syncPublicEncryptedRepository
bool encryptRepository(const QString &repositoryPath,
                       const QString &temporaryCiphertext,
                       const IdentityEntry &entry,
                       const PublicMirrorRuntime::Tools &tools,
                       QString *error)
{
    const QString age = resolveProgram(tools.age);
    const QString tar = resolveProgram(tools.tar);
    if (age.isEmpty() || tar.isEmpty()) {
        setError(error,
                 QStringLiteral("age and tar are required for encrypted public mirrors."));
        return false;
    }
    const QFileInfo repository(repositoryPath);
    if (!repository.isDir() || repository.isSymLink() ||
        repository.fileName() != QLatin1String("repository.git")) {
        setError(error, QStringLiteral("The canonical public mirror is invalid."));
        return false;
    }
    QProcess ageProcess;
    QProcess tarProcess;
    ageProcess.setProcessEnvironment(safeEnvironment());
    tarProcess.setProcessEnvironment(safeEnvironment());
    ageProcess.setProcessChannelMode(QProcess::SeparateChannels);
    tarProcess.setProcessChannelMode(QProcess::SeparateChannels);
    ageProcess.setStandardErrorFile(QProcess::nullDevice());
    tarProcess.setStandardErrorFile(QProcess::nullDevice());
    tarProcess.setStandardOutputProcess(&ageProcess);

    ageProcess.start(
        age,
        {QStringLiteral("--encrypt"), QStringLiteral("--recipient"),
         entry.recipient, QStringLiteral("--output"),
         temporaryCiphertext, QStringLiteral("-")});
    if (!ageProcess.waitForStarted(10000)) {
        setError(error, QStringLiteral("Could not start public-mirror encryption."));
        return false;
    }
    tarProcess.start(
        tar,
        {QStringLiteral("-C"), repository.dir().absolutePath(),
         QStringLiteral("-cf"), QStringLiteral("-"),
         QStringLiteral("repository.git")});
    if (!tarProcess.waitForStarted(10000)) {
        ageProcess.kill();
        ageProcess.waitForFinished(5000);
        setError(error, QStringLiteral("Could not start public-mirror archiving."));
        return false;
    }
    const bool tarFinished = tarProcess.waitForFinished(kProcessTimeoutMs);
    if (!tarFinished) {
        tarProcess.kill();
        tarProcess.waitForFinished(5000);
    }
    const bool ageFinished = ageProcess.waitForFinished(kProcessTimeoutMs);
    if (!ageFinished) {
        ageProcess.kill();
        ageProcess.waitForFinished(5000);
    }
    if (!tarFinished || !ageFinished ||
        tarProcess.exitStatus() != QProcess::NormalExit ||
        ageProcess.exitStatus() != QProcess::NormalExit ||
        tarProcess.exitCode() != 0 || ageProcess.exitCode() != 0 ||
        !hasAgeHeader(temporaryCiphertext)) {
        QFile::remove(temporaryCiphertext);
        setError(error, QStringLiteral("Could not encrypt the public mirror."));
        return false;
    }
    QFile permissions(temporaryCiphertext);
    if (!permissions.setPermissions(QFileDevice::ReadOwner |
                                    QFileDevice::WriteOwner)) {
        QFile::remove(temporaryCiphertext);
        setError(error, QStringLiteral("Could not restrict the encrypted public mirror."));
        return false;
    }
    return true;
}

bool safeExtractedTree(const QString &destination, QString *error)
{
    const QFileInfo root(destination);
    if (!root.isDir() || root.isSymLink()) {
        setError(error, QStringLiteral("The public-mirror extraction root is unsafe."));
        return false;
    }
    const QString repositoryPath =
        QDir(destination).filePath(QStringLiteral("repository.git"));
    const QFileInfo repository(repositoryPath);
    const QStringList top =
        QDir(destination).entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
    if (top != QStringList{QStringLiteral("repository.git")} ||
        !repository.isDir() || repository.isSymLink()) {
        setError(error, QStringLiteral("The public-mirror archive layout is invalid."));
        return false;
    }
    QDirIterator iterator(repositoryPath,
                          QDir::AllEntries | QDir::NoDotAndDotDot |
                              QDir::Hidden | QDir::System,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        iterator.next();
        const QFileInfo info = iterator.fileInfo();
        if (info.isSymLink() || (!info.isDir() && !info.isFile())) {
            setError(error, QStringLiteral("The public-mirror archive contains an unsafe entry."));
            return false;
        }
    }
    return true;
}

// code-health: allow(blocking-wait) only reached via materialize on the syncPublicEncryptedRepository worker, or the --materialize-public-mirror CLI mode
bool decryptInto(const QString &ciphertext, const IdentityEntry &entry,
                 const QString &destination,
                 const PublicMirrorRuntime::Tools &tools, QString *error)
{
    const QFileInfo destinationInfo(destination);
    if (!destinationInfo.isDir() || destinationInfo.isSymLink() ||
        !QDir(destination).entryList(QDir::AllEntries |
                                     QDir::NoDotAndDotDot).isEmpty()) {
        setError(error, QStringLiteral("The public-mirror extraction destination is unsafe."));
        return false;
    }
    QFile destinationPermissions(destination);
    if (!destinationPermissions.setPermissions(
            QFileDevice::ReadOwner | QFileDevice::WriteOwner |
            QFileDevice::ExeOwner)) {
        setError(error, QStringLiteral("Could not restrict the public-mirror extraction destination."));
        return false;
    }
    const QString age = resolveProgram(tools.age);
    const QString tar = resolveProgram(tools.tar);
    if (age.isEmpty() || tar.isEmpty()) {
        setError(error,
                 QStringLiteral("age and tar are required for encrypted public mirrors."));
        return false;
    }
    QTemporaryDir identityDirectory;
    if (!identityDirectory.isValid() ||
        !prepareOwnerDirectory(identityDirectory.path(), error)) {
        if (error && error->isEmpty())
            *error = QStringLiteral("Could not create the temporary age identity directory.");
        return false;
    }
    const QString identityPath =
        QDir(identityDirectory.path()).filePath(QStringLiteral("identity.txt"));
    QFile identity(identityPath);
    if (!identity.open(QIODevice::WriteOnly) ||
        !identity.setPermissions(QFileDevice::ReadOwner |
                                 QFileDevice::WriteOwner) ||
        identity.write(entry.secretIdentity) != entry.secretIdentity.size() ||
        identity.write("\n") != 1) {
        identity.close();
        QFile::remove(identityPath);
        setError(error, QStringLiteral("Could not stage the public-mirror age identity."));
        return false;
    }
    identity.close();

    QProcess ageProcess;
    QProcess tarProcess;
    ageProcess.setProcessEnvironment(safeEnvironment());
    tarProcess.setProcessEnvironment(safeEnvironment());
    ageProcess.setProcessChannelMode(QProcess::SeparateChannels);
    tarProcess.setProcessChannelMode(QProcess::SeparateChannels);
    ageProcess.setStandardErrorFile(QProcess::nullDevice());
    tarProcess.setStandardErrorFile(QProcess::nullDevice());
    ageProcess.setStandardOutputProcess(&tarProcess);
    tarProcess.start(
        tar,
        {QStringLiteral("-C"), destination, QStringLiteral("-xf"),
         QStringLiteral("-"), QStringLiteral("--no-same-owner"),
         QStringLiteral("--no-same-permissions")});
    if (!tarProcess.waitForStarted(10000)) {
        QFile::remove(identityPath);
        setError(error, QStringLiteral("Could not start public-mirror extraction."));
        return false;
    }
    ageProcess.start(
        age,
        {QStringLiteral("--decrypt"), QStringLiteral("--identity"),
         identityPath, ciphertext});
    if (!ageProcess.waitForStarted(10000)) {
        tarProcess.kill();
        tarProcess.waitForFinished(5000);
        QFile::remove(identityPath);
        setError(error, QStringLiteral("Could not start public-mirror decryption."));
        return false;
    }
    const bool ageFinished = ageProcess.waitForFinished(kProcessTimeoutMs);
    if (!ageFinished) {
        ageProcess.kill();
        ageProcess.waitForFinished(5000);
    }
    const bool tarFinished = tarProcess.waitForFinished(kProcessTimeoutMs);
    if (!tarFinished) {
        tarProcess.kill();
        tarProcess.waitForFinished(5000);
    }
    QFile::remove(identityPath);
    if (!ageFinished || !tarFinished ||
        ageProcess.exitStatus() != QProcess::NormalExit ||
        tarProcess.exitStatus() != QProcess::NormalExit ||
        ageProcess.exitCode() != 0 || tarProcess.exitCode() != 0 ||
        !safeExtractedTree(destination, error)) {
        if (error && error->isEmpty())
            *error = QStringLiteral("Could not decrypt the public mirror.");
        return false;
    }
    return true;
}

void note(const PublicMirrorRuntime::Progress &progress, const QString &line)
{
    if (progress)
        progress(line);
}

// Reopen a verified archive before making a full temporary mirror clone when
// the source's cheap refs fingerprint is unchanged. Read the source again
// after authentication so a ref update racing the first read falls back to the
// normal clone-and-seal path instead of reporting a stale sync as current.
bool reopenUnchangedArchive(
    const std::function<QString(QString *)> &readSourceRefs,
    const QString &archiveRoot, const QString &vaultPath,
    const QByteArray &vaultSecret, const QString &existingArchiveId,
    const PublicMirrorRuntime::Tools &tools, QString *error,
    const PublicMirrorRuntime::Progress &progress,
    PublicMirrorRuntime::SyncResult *result)
{
    if (!result ||
        !PublicMirrorRuntime::isArchiveId(existingArchiveId))
        return false;
    const PublicMirrorRuntime::Metadata previous =
        PublicMirrorRuntime::readMetadata(
            archiveRoot, existingArchiveId, nullptr);
    if (!previous.isValid())
        return false;

    note(progress, QStringLiteral("Checking whether the sealed mirror is current…"));
    QString refsError;
    const QString before = readSourceRefs(&refsError);
    if (before.isEmpty() || before != previous.expectedRefsSha256)
        return false;

    note(progress,
         QStringLiteral("Repository unchanged — reopening the sealed archive…"));
    QString reopenError;
    std::unique_ptr<PublicMirrorMaterialization> materialization =
        PublicMirrorRuntime::materialize(
            archiveRoot, vaultPath, vaultSecret, existingArchiveId,
            tools, &reopenError);
    if (!materialization) {
        note(progress,
             QStringLiteral("The existing seal failed verification — rebuilding it…"));
        return false;
    }

    refsError.clear();
    const QString after = readSourceRefs(&refsError);
    if (after.isEmpty() || after != before) {
        note(progress,
             QStringLiteral("Repository refs changed during verification — rebuilding the seal…"));
        return false;
    }

    result->created = false;
    result->metadata = previous;
    result->materialization = std::move(materialization);
    if (error)
        error->clear();
    return true;
}

PublicMirrorRuntime::SyncResult sealTemporaryRepository(
    std::unique_ptr<QTemporaryDir> directory, const QString &repositoryPath,
    const QString &archiveRoot, const QString &vaultPath,
    const QByteArray &vaultSecret, const QString &existingArchiveId,
    const PublicMirrorRuntime::Tools &tools, QString *error,
    const PublicMirrorRuntime::Progress &progress = {})
{
    PublicMirrorRuntime::SyncResult result;
    if (!isBareRepository(repositoryPath, tools, error) ||
        !prepareOwnerDirectory(archiveRoot, error))
        return result;

    const QString existing = existingArchiveId.trimmed();
    result.created = existing.isEmpty();
    result.metadata.archiveId =
        result.created
            ? QString::fromLatin1(CoveCrypto::randomBytes(32).toHex())
            : existing;
    if (!PublicMirrorRuntime::isArchiveId(result.metadata.archiveId)) {
        setError(error, QStringLiteral("The encrypted public-mirror archive id is invalid."));
        return {};
    }

    note(progress, QStringLiteral("Unlocking the mirror's age key…"));
    Vault vault;
    if (!readVault(vaultPath, vaultSecret, &vault, true, error))
        return {};
    IdentityEntry entry = vault.entries.value(result.metadata.archiveId);
    if (!entry.isValid()) {
        if (!result.created && vault.entries.contains(result.metadata.archiveId)) {
            setError(error, QStringLiteral("The public-mirror age identity is invalid."));
            return {};
        }
        entry = createIdentity(result.metadata.archiveId, tools, error);
        if (!entry.isValid())
            return {};
        vault.entries.insert(entry.archiveId, entry);
        if (!writeVault(vaultPath, vaultSecret, vault, error)) {
            clearBytes(&entry.secretIdentity);
            return {};
        }
    }

    note(progress, QStringLiteral("Reading the mirror's refs…"));
    const QString refs = refsSha256(repositoryPath, tools, error);
    if (refs.isEmpty()) {
        clearBytes(&entry.secretIdentity);
        return {};
    }
    const PublicMirrorRuntime::Metadata previous =
        result.created
            ? PublicMirrorRuntime::Metadata{}
            : PublicMirrorRuntime::readMetadata(
                  archiveRoot, result.metadata.archiveId, nullptr);
    if (previous.isValid() &&
        previous.expectedRefsSha256 == refs) {
        clearBytes(&entry.secretIdentity);
        note(progress,
             QStringLiteral("Already sealed — reopening the archive…"));
        std::unique_ptr<PublicMirrorMaterialization> authenticated =
            PublicMirrorRuntime::materialize(
                archiveRoot, vaultPath, vaultSecret,
                result.metadata.archiveId, tools, error);
        if (!authenticated)
            return {};
        result.metadata = previous;
        result.materialization = std::move(authenticated);
        return result;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    result.metadata.recipient = entry.recipient;
    result.metadata.keyReference =
        PublicMirrorRuntime::keyReference(result.metadata.archiveId);
    result.metadata.expectedRefsSha256 = refs;
    result.metadata.createdAtMs =
        previous.isValid() ? previous.createdAtMs : now;
    result.metadata.updatedAtMs = qMax(now, result.metadata.createdAtMs);

    QTemporaryDir cipherDirectory(archiveRoot +
                                  QStringLiteral("/.seal-XXXXXX"));
    if (!cipherDirectory.isValid() ||
        !prepareOwnerDirectory(cipherDirectory.path(), error)) {
        clearBytes(&entry.secretIdentity);
        setError(error, QStringLiteral("Could not create the encrypted public-mirror staging directory."));
        return {};
    }
    const QString stagedCiphertext =
        QDir(cipherDirectory.path()).filePath(QStringLiteral("archive.age"));
    note(progress, QStringLiteral("Encrypting the mirror with age…"));
    if (!encryptRepository(repositoryPath, stagedCiphertext, entry, tools,
                           error)) {
        clearBytes(&entry.secretIdentity);
        return {};
    }
    note(progress, QStringLiteral("Writing the sealed archive…"));
    const QString finalCiphertext =
        PublicMirrorRuntime::ciphertextPath(
            archiveRoot, result.metadata.archiveId);
    if (!copyAtomically(stagedCiphertext, finalCiphertext, error) ||
        !hasAgeHeader(finalCiphertext) ||
        !fileSha256(finalCiphertext,
                    &result.metadata.ciphertextSha256, error) ||
        !writeMetadata(archiveRoot, result.metadata, error)) {
        clearBytes(&entry.secretIdentity);
        return {};
    }
    clearBytes(&entry.secretIdentity);

    // Authenticate a fresh decrypt before exposing the result or allowing a
    // caller to remove any legacy durable plaintext.
    note(progress, QStringLiteral("Verifying the sealed archive…"));
    std::unique_ptr<PublicMirrorMaterialization> authenticated =
        PublicMirrorRuntime::materialize(
            archiveRoot, vaultPath, vaultSecret,
            result.metadata.archiveId, tools, error);
    if (!authenticated)
        return {};
    result.materialization = std::move(authenticated);
    return result;
}

bool parseIntegerString(const QJsonValue &value, qint64 *result)
{
    bool ok = false;
    const qint64 number = value.toString().toLongLong(&ok);
    if (!ok || number <= 0)
        return false;
    *result = number;
    return true;
}

} // namespace

QString PublicMirrorRuntime::refsSha256FromForEachRef(
    const QByteArray &output)
{
    QList<QByteArray> lines;
    for (QByteArray line : output.split('\n')) {
        line = line.trimmed();
        const qsizetype separator = line.indexOf(' ');
        if (line.isEmpty() || line.endsWith(QByteArrayLiteral("^{}")) ||
            separator <= 0 || separator + 1 >= line.size()) {
            continue;
        }
        const QByteArray refname = line.mid(separator + 1);
        if (!refname.startsWith(QByteArrayLiteral("refs/heads/")) &&
            !refname.startsWith(QByteArrayLiteral("refs/tags/"))) {
            continue;
        }
        lines.append(line);
    }
    std::sort(lines.begin(), lines.end(),
              [](const QByteArray &left, const QByteArray &right) {
                  return left.mid(left.indexOf(' ') + 1) <
                         right.mid(right.indexOf(' ') + 1);
              });
    const QByteArray canonical = QByteArrayList(lines).join('\n');
    return QString::fromLatin1(
        QCryptographicHash::hash(canonical, QCryptographicHash::Sha256)
            .toHex());
}

QString PublicMirrorRuntime::repositoryRefsSha256(
    const QString &repositoryPath, const Tools &tools, QString *error)
{
    return refsSha256(repositoryPath, tools, error);
}

PublicMirrorMaterialization::PublicMirrorMaterialization(
    std::unique_ptr<QTemporaryDir> directory, QString repositoryPath)
    : m_directory(std::move(directory)),
      m_repositoryPath(std::move(repositoryPath))
{
}

PublicMirrorMaterialization::~PublicMirrorMaterialization() = default;

PublicMirrorRuntime::Tools::Tools()
    : age(QStringLiteral("age")),
      ageKeygen(QStringLiteral("age-keygen")),
      tar(QStringLiteral("tar")),
      git(QStringLiteral("git"))
{
}

bool PublicMirrorMaterialization::isValid() const
{
    const QFileInfo info(m_repositoryPath);
    return m_directory && m_directory->isValid() && info.isDir() &&
           !info.isSymLink() &&
           QDir::cleanPath(info.absoluteFilePath()).startsWith(
               QDir::cleanPath(m_directory->path()) + QLatin1Char('/'));
}

bool PublicMirrorRuntime::Metadata::isValid() const
{
    static const QRegularExpression digest(
        QStringLiteral("^[0-9a-f]{64}$"));
    static const QRegularExpression recipientPattern(
        QStringLiteral("^age1[023456789acdefghjklmnpqrstuvwxyz]{20,100}$"));
    return PublicMirrorRuntime::isArchiveId(archiveId) &&
           digest.match(ciphertextSha256).hasMatch() &&
           keyReference ==
               PublicMirrorRuntime::keyReference(archiveId) &&
           recipientPattern.match(recipient).hasMatch() &&
           digest.match(expectedRefsSha256).hasMatch() &&
           createdAtMs > 0 && updatedAtMs >= createdAtMs;
}

bool PublicMirrorRuntime::isArchiveId(const QString &value)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[0-9a-f]{64}$"));
    return pattern.match(value).hasMatch();
}

QString PublicMirrorRuntime::ciphertextPath(const QString &archiveRoot,
                                            const QString &archiveId)
{
    if (!isArchiveId(archiveId))
        return {};
    return QDir(archiveRoot).filePath(archiveId + QStringLiteral(".age"));
}

QString PublicMirrorRuntime::keyReference(const QString &archiveId)
{
    return isArchiveId(archiveId)
               ? QStringLiteral("forkmesh-age:%1").arg(archiveId)
               : QString();
}

// code-health: allow(dir-walk-in-loop) startup-only: called once from main() before the event loop, and only for leftover ForkMesh mirror temp dirs
int PublicMirrorRuntime::cleanupStaleTemporaryDirectories(
    const QString &temporaryRoot, QString *error)
{
    const QFileInfo rootInfo(temporaryRoot);
    const QString root = rootInfo.canonicalFilePath();
    if (temporaryRoot.trimmed().isEmpty() || !rootInfo.isAbsolute() ||
        !rootInfo.isDir() || rootInfo.isSymLink() || root.isEmpty()) {
        setError(error, QStringLiteral(
                            "The mirror temporary cleanup root is unsafe."));
        return -1;
    }

    static const QRegularExpression ownedName(
        QStringLiteral(
            "^(?:forkmesh-mirror-runtime-[A-Za-z0-9_]+|"
            "forkmesh-public-mirror-[A-Za-z0-9]{6}|"
            "forkmesh-private-(?:sync|open)-[A-Za-z0-9]{6})$"));
    static const QRegularExpression legacyName(
        QStringLiteral("^ForkMesh-[A-Za-z0-9]{6}$"));

    int removed = 0;
    const QFileInfoList candidates =
        QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot,
                                 QDir::Name);
    for (const QFileInfo &candidate : candidates) {
        const QString name = candidate.fileName();
        const bool legacy = legacyName.match(name).hasMatch();
        if (!ownedName.match(name).hasMatch() && !legacy)
            continue;
        if (legacy &&
            !QFileInfo(QDir(candidate.absoluteFilePath())
                           .filePath(QStringLiteral("repository.git")))
                 .isDir()) {
            continue;
        }
        if (candidate.isSymLink() || !candidate.isDir() ||
            candidate.canonicalPath() != root) {
            setError(error, QStringLiteral(
                                "A mirror temporary directory is unsafe."));
            return -1;
        }
        if (!forkmesh::safedelete::removeTree(candidate.absoluteFilePath(), root)) {
            setError(error, QStringLiteral(
                                "A stale mirror temporary directory could not "
                                "be removed."));
            return -1;
        }
        ++removed;
    }
    if (error)
        error->clear();
    return removed;
}

bool PublicMirrorRuntime::toolingAvailable(const Tools &tools, QString *error)
{
    if (resolveProgram(tools.age).isEmpty()) {
        setError(error, QStringLiteral(
                            "The official age program is required for encrypted public mirrors."));
        return false;
    }
    if (resolveProgram(tools.ageKeygen).isEmpty()) {
        setError(error, QStringLiteral(
                            "The official age-keygen program is required for encrypted public mirrors."));
        return false;
    }
    if (resolveProgram(tools.tar).isEmpty()) {
        setError(error, QStringLiteral(
                            "tar is required for encrypted public mirrors."));
        return false;
    }
    if (resolveProgram(tools.git).isEmpty()) {
        setError(error, QStringLiteral(
                            "Git is required for encrypted public mirrors."));
        return false;
    }
    return true;
}

PublicMirrorRuntime::SyncResult PublicMirrorRuntime::syncRepository(
    const QString &repositoryPath, const QString &archiveRoot,
    const QString &vaultPath, const QByteArray &vaultSecret,
    const QString &existingArchiveId, const Tools &tools, QString *error,
    const Progress &progress)
{
    return syncSource(repositoryPath, {}, archiveRoot, vaultPath,
                      vaultSecret, existingArchiveId, tools, error, progress);
}

PublicMirrorRuntime::SyncResult PublicMirrorRuntime::syncManagedCheckout(
    const QString &repositoryPath, const QString &archiveRoot,
    const QString &vaultPath, const QByteArray &vaultSecret,
    const QString &existingArchiveId, const Tools &tools, QString *error,
    const Progress &progress)
{
    if (!toolingAvailable(tools, error))
        return {};
    SyncResult reopened;
    if (isLocalSource(repositoryPath) &&
        reopenUnchangedArchive(
            [&](QString *refsError) {
                return managedRefsSha256(repositoryPath, tools, refsError);
            },
            archiveRoot, vaultPath, vaultSecret, existingArchiveId,
            tools, error, progress, &reopened)) {
        return reopened;
    }
    if (error)
        error->clear();
    std::unique_ptr<QTemporaryDir> directory =
        std::make_unique<QTemporaryDir>(
            QDir::tempPath() +
            QStringLiteral("/forkmesh-public-mirror-XXXXXX"));
    if (!directory->isValid() ||
        !prepareOwnerDirectory(directory->path(), error)) {
        setError(error, QStringLiteral(
                            "Could not create the temporary public-mirror directory."));
        return {};
    }
    const QString repository =
        QDir(directory->path()).filePath(QStringLiteral("repository.git"));
    note(progress, QStringLiteral("Copying the managed checkout…"));
    if (!cloneSource(repositoryPath, {}, repository, tools, error, true))
        return {};
    return sealTemporaryRepository(
        std::move(directory), repository, archiveRoot, vaultPath,
        vaultSecret, existingArchiveId, tools, error, progress);
}

PublicMirrorRuntime::SyncResult PublicMirrorRuntime::syncSource(
    const QString &source, const QStringList &gitPrefixArgs,
    const QString &archiveRoot, const QString &vaultPath,
    const QByteArray &vaultSecret, const QString &existingArchiveId,
    const Tools &tools, QString *error, const Progress &progress)
{
    if (!toolingAvailable(tools, error))
        return {};
    SyncResult reopened;
    if (isLocalSource(source) &&
        reopenUnchangedArchive(
            [&](QString *refsError) {
                return refsSha256(source, tools, refsError);
            },
            archiveRoot, vaultPath, vaultSecret, existingArchiveId,
            tools, error, progress, &reopened)) {
        return reopened;
    }
    if (error)
        error->clear();
    std::unique_ptr<QTemporaryDir> directory =
        std::make_unique<QTemporaryDir>(
            QDir::tempPath() +
            QStringLiteral("/forkmesh-public-mirror-XXXXXX"));
    if (!directory->isValid() ||
        !prepareOwnerDirectory(directory->path(), error)) {
        setError(error, QStringLiteral("Could not create the temporary public-mirror directory."));
        return {};
    }
    const QString repository =
        QDir(directory->path()).filePath(QStringLiteral("repository.git"));
    note(progress, QStringLiteral("Copying the repository to seal…"));
    if (!cloneSource(source, gitPrefixArgs, repository, tools, error))
        return {};
    return sealTemporaryRepository(
        std::move(directory), repository, archiveRoot, vaultPath,
        vaultSecret, existingArchiveId, tools, error, progress);
}

PublicMirrorRuntime::Metadata PublicMirrorRuntime::readMetadata(
    const QString &archiveRoot, const QString &archiveId, QString *error)
{
    Metadata metadata;
    if (!isArchiveId(archiveId)) {
        setError(error, QStringLiteral("The encrypted public-mirror archive id is invalid."));
        return metadata;
    }
    const QFileInfo info(metadataPath(archiveRoot, archiveId));
    if (!info.isFile() || info.isSymLink() || info.size() <= 0 ||
        info.size() > kMaximumMetadataBytes) {
        setError(error, QStringLiteral("The encrypted public-mirror metadata is unavailable."));
        return metadata;
    }
    QFile file(info.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("The encrypted public-mirror metadata is unavailable."));
        return metadata;
    }
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(file.readAll(), &parseError);
    const QJsonObject object = document.object();
    static const QSet<QString> fields{
        QStringLiteral("schemaVersion"), QStringLiteral("kind"),
        QStringLiteral("archiveId"), QStringLiteral("scheme"),
        QStringLiteral("ciphertextSha256"),
        QStringLiteral("keyReference"), QStringLiteral("recipient"),
        QStringLiteral("expectedRefsSha256"),
        QStringLiteral("createdAtMs"), QStringLiteral("updatedAtMs")};
    QSet<QString> actual;
    for (auto it = object.constBegin(); it != object.constEnd(); ++it)
        actual.insert(it.key());
    if (parseError.error != QJsonParseError::NoError ||
        !document.isObject() || actual != fields ||
        object.value(QStringLiteral("schemaVersion")).toInt() != 1 ||
        object.value(QStringLiteral("kind")).toString() !=
            QLatin1String("forkmesh.public-age.archive") ||
        object.value(QStringLiteral("scheme")).toString() !=
            QLatin1String("age-encrypted-tar-v1")) {
        setError(error, QStringLiteral("The encrypted public-mirror metadata is invalid."));
        return {};
    }
    metadata.archiveId =
        object.value(QStringLiteral("archiveId")).toString();
    metadata.ciphertextSha256 =
        object.value(QStringLiteral("ciphertextSha256")).toString();
    metadata.keyReference =
        object.value(QStringLiteral("keyReference")).toString();
    metadata.recipient =
        object.value(QStringLiteral("recipient")).toString();
    metadata.expectedRefsSha256 =
        object.value(QStringLiteral("expectedRefsSha256")).toString();
    if (!parseIntegerString(object.value(QStringLiteral("createdAtMs")),
                            &metadata.createdAtMs) ||
        !parseIntegerString(object.value(QStringLiteral("updatedAtMs")),
                            &metadata.updatedAtMs) ||
        metadata.archiveId != archiveId || !metadata.isValid()) {
        setError(error, QStringLiteral("The encrypted public-mirror metadata is invalid."));
        return {};
    }
    QString actualDigest;
    const QString ciphertext = ciphertextPath(archiveRoot, archiveId);
    if (!hasAgeHeader(ciphertext) ||
        !fileSha256(ciphertext, &actualDigest, error) ||
        actualDigest != metadata.ciphertextSha256) {
        setError(error, QStringLiteral("The encrypted public mirror failed authentication."));
        return {};
    }
    return metadata;
}

std::unique_ptr<PublicMirrorMaterialization>
PublicMirrorRuntime::materialize(
    const QString &archiveRoot, const QString &vaultPath,
    const QByteArray &vaultSecret, const QString &archiveId,
    const Tools &tools, QString *error)
{
    const Metadata metadata =
        readMetadata(archiveRoot, archiveId, error);
    if (!metadata.isValid())
        return {};
    Vault vault;
    if (!readVault(vaultPath, vaultSecret, &vault, false, error))
        return {};
    IdentityEntry entry = vault.entries.value(archiveId);
    if (!entry.isValid() || entry.recipient != metadata.recipient) {
        clearBytes(&entry.secretIdentity);
        setError(error, QStringLiteral("The public-mirror decryption identity is unavailable."));
        return {};
    }
    std::unique_ptr<QTemporaryDir> directory =
        std::make_unique<QTemporaryDir>(
            QDir::tempPath() +
            QStringLiteral("/forkmesh-public-mirror-XXXXXX"));
    if (!directory->isValid() ||
        !prepareOwnerDirectory(directory->path(), error) ||
        !decryptInto(ciphertextPath(archiveRoot, archiveId), entry,
                     directory->path(), tools, error)) {
        clearBytes(&entry.secretIdentity);
        return {};
    }
    clearBytes(&entry.secretIdentity);
    const QString repository =
        QDir(directory->path()).filePath(QStringLiteral("repository.git"));
    QString integrityError;
    if (!isBareRepository(repository, tools, &integrityError) ||
        refsSha256(repository, tools, &integrityError) !=
            metadata.expectedRefsSha256) {
        setError(error, QStringLiteral("The decrypted public mirror failed integrity verification."));
        return {};
    }
    return std::unique_ptr<PublicMirrorMaterialization>(
        new PublicMirrorMaterialization(std::move(directory), repository));
}

QJsonObject PublicMirrorRuntime::materializeGatewayRequest(
    const QJsonObject &request, const QString &archiveRoot,
    const QString &vaultPath, const QByteArray &vaultSecret,
    const Tools &tools, QString *error)
{
    static const QSet<QString> fields{
        QStringLiteral("schemaVersion"), QStringLiteral("type"),
        QStringLiteral("scheme"), QStringLiteral("ciphertextPath"),
        QStringLiteral("ciphertextSha256"),
        QStringLiteral("keyReference"), QStringLiteral("destination")};
    QSet<QString> actual;
    for (auto it = request.constBegin(); it != request.constEnd(); ++it)
        actual.insert(it.key());
    const QString reference =
        request.value(QStringLiteral("keyReference")).toString();
    const QString prefix = QStringLiteral("forkmesh-age:");
    const QString archiveId =
        reference.startsWith(prefix) ? reference.mid(prefix.size())
                                     : QString();
    const Metadata metadata =
        readMetadata(archiveRoot, archiveId, nullptr);
    const QString destination =
        request.value(QStringLiteral("destination")).toString();
    const QFileInfo destinationInfo(destination);
    const QFileInfo requestedCiphertext(
        request.value(QStringLiteral("ciphertextPath")).toString());
    const QFileInfo configuredCiphertext(
        ciphertextPath(archiveRoot, archiveId));
    if (actual != fields ||
        request.value(QStringLiteral("schemaVersion")).toInt() != 1 ||
        request.value(QStringLiteral("type")).toString() !=
            QLatin1String("forkmesh.repository-archive-materialize") ||
        request.value(QStringLiteral("scheme")).toString() !=
            QLatin1String("age-encrypted-tar-v1") ||
        !metadata.isValid() || reference != metadata.keyReference ||
        request.value(QStringLiteral("ciphertextSha256")).toString() !=
            metadata.ciphertextSha256 ||
        !requestedCiphertext.isAbsolute() ||
        requestedCiphertext.canonicalFilePath() !=
            configuredCiphertext.canonicalFilePath() ||
        destination.trimmed().isEmpty() ||
        !destinationInfo.isAbsolute() || !destinationInfo.isDir() ||
        destinationInfo.isSymLink()) {
        setError(error, QStringLiteral("The public-mirror materialization request is invalid."));
        return {};
    }
    Vault vault;
    if (!readVault(vaultPath, vaultSecret, &vault, false, error))
        return {};
    IdentityEntry entry = vault.entries.value(archiveId);
    if (!entry.isValid() || entry.recipient != metadata.recipient) {
        clearBytes(&entry.secretIdentity);
        setError(error, QStringLiteral("The public-mirror decryption identity is unavailable."));
        return {};
    }
    if (!decryptInto(configuredCiphertext.absoluteFilePath(), entry,
                     destinationInfo.absoluteFilePath(), tools, error)) {
        clearBytes(&entry.secretIdentity);
        return {};
    }
    clearBytes(&entry.secretIdentity);
    const QString repository =
        QDir(destination).filePath(QStringLiteral("repository.git"));
    QString integrityError;
    if (!isBareRepository(repository, tools, &integrityError) ||
        refsSha256(repository, tools, &integrityError) !=
            metadata.expectedRefsSha256) {
        forkmesh::safedelete::removeTree(destinationInfo.absoluteFilePath(),
                                         destinationInfo.absolutePath());
        setError(error, QStringLiteral("The decrypted public mirror failed integrity verification."));
        return {};
    }
    return {
        {QStringLiteral("ok"), true},
        {QStringLiteral("repositoryPath"),
         QStringLiteral("repository.git")},
    };
}

bool PublicMirrorRuntime::removeManagedPlaintextMirror(
    const QString &mirrorPath, const QString &managedMirrorRoot,
    QString *error)
{
    const QFileInfo rootInfo(managedMirrorRoot);
    const QFileInfo targetInfo(mirrorPath);
    if (!rootInfo.isDir() || rootInfo.isSymLink() ||
        !targetInfo.isDir() || targetInfo.isSymLink() ||
        !targetInfo.fileName().endsWith(QStringLiteral(".git"))) {
        setError(error, QStringLiteral("The legacy public mirror path is not a managed bare repository."));
        return false;
    }
    const QString root =
        QDir::cleanPath(rootInfo.canonicalFilePath());
    const QString target =
        QDir::cleanPath(targetInfo.canonicalFilePath());
    if (root.isEmpty() || target.isEmpty() ||
        !target.startsWith(root + QLatin1Char('/'))) {
        setError(error, QStringLiteral("The legacy public mirror is outside the managed mirror root."));
        return false;
    }
    if (!forkmesh::safedelete::removeTree(target, root)) {
        setError(error, QStringLiteral("Could not remove the legacy plaintext public mirror."));
        return false;
    }
    return true;
}
