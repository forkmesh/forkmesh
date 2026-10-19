#include "ControlNode.h"
#include "AppPaths.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QLocale>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QSet>
#include <QStandardPaths>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace forkmesh::control {
namespace {

QString shellSingleQuote(QString value)
{
    value.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + value + QLatin1Char('\'');
}

bool isDnsName(const QString &value)
{
    if (value.isEmpty() || value.size() > 253 ||
        value.startsWith(QLatin1Char('.')) ||
        value.endsWith(QLatin1Char('.')) ||
        value.contains(QStringLiteral(".."))) {
        return false;
    }
    static const QRegularExpression labelPattern(
        QStringLiteral("^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?$"));
    const QStringList labels = value.split(QLatin1Char('.'));
    if (labels.size() < 2)
        return false;
    for (const QString &label : labels) {
        if (!labelPattern.match(label).hasMatch())
            return false;
    }
    return true;
}

QString normalizedDnsName(const QString &value)
{
    return value.trimmed().toLower();
}

bool isBase64UrlPublicKey(const QString &value)
{
    if (!QRegularExpression(QStringLiteral("^[A-Za-z0-9_-]{43}$"))
             .match(value)
             .hasMatch()) {
        return false;
    }
    return QByteArray::fromBase64(
               value.toLatin1(), QByteArray::Base64UrlEncoding)
               .size() == 32;
}

bool isSafeMainRelayUrl(const QString &value)
{
    if (value.trimmed().isEmpty())
        return true;
    const QUrl url(value.trimmed());
    if (!url.isValid() || url.scheme() != QLatin1String("https") ||
        url.host().isEmpty() || !url.userInfo().isEmpty() ||
        !url.query().isEmpty() || !url.fragment().isEmpty()) {
        return false;
    }
    return url.path().isEmpty() || url.path() == QLatin1String("/");
}

const QString kCompleteSystemBundleKind =
    QStringLiteral("forkmesh-complete-system-v1");

// A skipped-entry list is a diagnostic, not payload: record enough to explain
// what was left behind without letting a sandbox full of pipes bloat the JSON.
constexpr int kMaxRecordedSkippedEntries = 200;

QJsonObject encodeCompleteSetting(const QVariant &value, bool *ok)
{
    QByteArray exact;
    QDataStream stream(&exact, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_4);
    stream << value;
    if (stream.status() != QDataStream::Ok) {
        *ok = false;
        return {};
    }

    QJsonValue readable = QJsonValue::fromVariant(value);
    if (readable.isUndefined())
        readable = value.toString();
    return QJsonObject{
        {QStringLiteral("type"),
         QString::fromLatin1(value.typeName() ? value.typeName() : "invalid")},
        {QStringLiteral("value"), readable},
        {QStringLiteral("qtDataBase64"),
         QString::fromLatin1(exact.toBase64())},
    };
}

QVariant decodeCompleteSetting(const QJsonObject &encoded, bool *ok)
{
    const QJsonValue raw = encoded.value(QStringLiteral("qtDataBase64"));
    if (!raw.isString()) {
        *ok = false;
        return {};
    }
    const QByteArray text = raw.toString().toLatin1();
    const QByteArray exact = QByteArray::fromBase64(
        text, QByteArray::AbortOnBase64DecodingErrors);
    if (exact.toBase64() != text) {
        *ok = false;
        return {};
    }
    QDataStream stream(exact);
    stream.setVersion(QDataStream::Qt_6_4);
    QVariant value;
    stream >> value;
    if (stream.status() != QDataStream::Ok || !stream.atEnd()) {
        *ok = false;
        return {};
    }
    return value;
}

bool safeBundleRelativePath(const QString &path)
{
    if (path.isEmpty() || path.size() > 4096 || QDir::isAbsolutePath(path) ||
        path.contains(QChar(u'\0')) || path.contains(QLatin1Char('\\'))) {
        return false;
    }
    const QString clean = QDir::cleanPath(path);
    return clean == path && clean != QLatin1String(".") &&
           !clean.startsWith(QStringLiteral("../")) &&
           clean != QLatin1String("..");
}

QJsonObject encodedBundleFile(const QString &scope, const QString &relativePath,
                              const QString &absolutePath, bool *ok,
                              QString *error)
{
    const QFileInfo info(absolutePath);
    if (!info.isFile() || info.isSymLink()) {
        *ok = false;
        if (error)
            *error = QStringLiteral("A bundle input is not a regular file: %1")
                         .arg(absolutePath);
        return {};
    }
    QFile file(absolutePath);
    if (!file.open(QIODevice::ReadOnly)) {
        *ok = false;
        if (error)
            *error = QStringLiteral("Could not read a bundle input: %1")
                         .arg(absolutePath);
        return {};
    }
    const QByteArray bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        *ok = false;
        if (error)
            *error = QStringLiteral("Could not finish reading a bundle input: %1")
                         .arg(absolutePath);
        return {};
    }
    return QJsonObject{
        {QStringLiteral("scope"), scope},
        {QStringLiteral("path"), relativePath},
        {QStringLiteral("size"), QString::number(bytes.size())},
        {QStringLiteral("permissions"), int(info.permissions())},
        {QStringLiteral("sha256"),
         QString::fromLatin1(
             QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())},
        {QStringLiteral("dataBase64"),
         QString::fromLatin1(bytes.toBase64())},
    };
}

bool validateEncodedBundleFile(const QJsonObject &file, QString *error)
{
    const QString scope = file.value(QStringLiteral("scope")).toString();
    const QString path = file.value(QStringLiteral("path")).toString();
    if ((scope != QLatin1String("applicationData") &&
         scope != QLatin1String("settingsFile") &&
         scope != QLatin1String("workerEnvironment")) ||
        !safeBundleRelativePath(path)) {
        if (error)
            *error = QStringLiteral("The bundle contains an unsafe file path.");
        return false;
    }
    const QByteArray encoded =
        file.value(QStringLiteral("dataBase64")).toString().toLatin1();
    const QByteArray bytes = QByteArray::fromBase64(
        encoded, QByteArray::AbortOnBase64DecodingErrors);
    bool sizeOk = false;
    const qint64 declaredSize =
        file.value(QStringLiteral("size")).toString().toLongLong(&sizeOk);
    const QString declaredHash =
        file.value(QStringLiteral("sha256")).toString().toLower();
    const QString actualHash = QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    if (!sizeOk || declaredSize < 0 || bytes.size() != declaredSize ||
        encoded != bytes.toBase64() || declaredHash.size() != 64 ||
        declaredHash != actualHash) {
        if (error)
            *error = QStringLiteral("A file in the bundle failed its integrity check.");
        return false;
    }
    return true;
}

QString canonicalCandidate(const QString &path)
{
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile() || !info.isReadable())
        return {};
    return info.canonicalFilePath();
}

QStringList installedToolCandidates(const QString &fileName,
                                    const QString &applicationDir)
{
    QStringList candidates;
    const QString resourceOverride =
        qEnvironmentVariable("FORKMESH_RESOURCE_DIR").trimmed();
    if (!resourceOverride.isEmpty()) {
        candidates.append(
            QDir(resourceOverride)
                .absoluteFilePath(QStringLiteral("tools/") + fileName));
    }
    const QString appDir =
        applicationDir.trimmed().isEmpty()
            ? QCoreApplication::applicationDirPath()
            : applicationDir.trimmed();
    candidates.append(
        QDir(appDir).absoluteFilePath(
            QStringLiteral("../share/forkmesh/tools/") + fileName));
    candidates.append(
        QDir(appDir).absoluteFilePath(
            QStringLiteral("../Resources/forkmesh/tools/") + fileName));
    candidates.append(
        QDir(appDir).absoluteFilePath(
            QStringLiteral("resources/forkmesh/tools/") + fileName));
    return candidates;
}

QString completeWorkerBundle(const QString &scriptPath)
{
    const QDir root(QFileInfo(scriptPath).absoluteDir().absoluteFilePath(
        QStringLiteral("..")));
    const QStringList files{
        QStringLiteral("wrangler.toml"),
        QStringLiteral("pywrangler.sh"),
        QStringLiteral("pyproject.toml"),
        QStringLiteral("tools/build_dashboard_assets.py"),
        QStringLiteral("src/entry.py"),
        QStringLiteral("src/public_snapshots.py"),
    };
    const QDir worker(root.absoluteFilePath(QStringLiteral("app")));
    for (const QString &file : files) {
        const QFileInfo info(worker.absoluteFilePath(file));
        if (!info.isFile() || !info.isReadable())
            return {};
    }
    for (const QString &directory :
         {QStringLiteral("public"), QStringLiteral("migrations")}) {
        const QFileInfo info(worker.absoluteFilePath(directory));
        if (!info.isDir() || !info.isReadable())
            return {};
    }
    return worker.canonicalPath();
}

bool hasCompleteWorkerBundle(const QString &scriptPath)
{
    return !completeWorkerBundle(scriptPath).isEmpty();
}

bool appendPythonJsonString(const QString &value, QByteArray *output)
{
    if (!output)
        return false;
    output->append('"');
    for (qsizetype i = 0; i < value.size(); ++i) {
        const ushort code = value.at(i).unicode();
        switch (code) {
        case '"':
            output->append("\\\"");
            break;
        case '\\':
            output->append("\\\\");
            break;
        case '\b':
            output->append("\\b");
            break;
        case '\f':
            output->append("\\f");
            break;
        case '\n':
            output->append("\\n");
            break;
        case '\r':
            output->append("\\r");
            break;
        case '\t':
            output->append("\\t");
            break;
        default:
            if (code < 0x20 || code >= 0x7f) {
                output->append("\\u");
                output->append(
                    QByteArray::number(code, 16).rightJustified(4, '0'));
            } else {
                output->append(char(code));
            }
            break;
        }
    }
    output->append('"');
    return true;
}

bool appendPythonCanonicalJson(const QJsonValue &value, QByteArray *output,
                               QString *error)
{
    if (value.isNull() || value.isUndefined()) {
        output->append("null");
        return true;
    }
    if (value.isBool()) {
        output->append(value.toBool() ? "true" : "false");
        return true;
    }
    if (value.isDouble()) {
        const double number = value.toDouble();
        const qint64 integer = qint64(number);
        if (!std::isfinite(number) || double(integer) != number) {
            if (error)
                *error = QStringLiteral(
                    "Catalog canonical JSON accepts integer numbers only.");
            return false;
        }
        output->append(QByteArray::number(integer));
        return true;
    }
    if (value.isString())
        return appendPythonJsonString(value.toString(), output);
    if (value.isArray()) {
        output->append('[');
        const QJsonArray array = value.toArray();
        for (qsizetype i = 0; i < array.size(); ++i) {
            if (i)
                output->append(',');
            if (!appendPythonCanonicalJson(array.at(i), output, error))
                return false;
        }
        output->append(']');
        return true;
    }
    if (value.isObject()) {
        output->append('{');
        const QJsonObject object = value.toObject();
        QStringList keys = object.keys();
        std::sort(keys.begin(), keys.end());
        bool first = true;
        for (const QString &key : keys) {
            if (!first)
                output->append(',');
            first = false;
            appendPythonJsonString(key, output);
            output->append(':');
            if (!appendPythonCanonicalJson(object.value(key), output, error))
                return false;
        }
        output->append('}');
        return true;
    }
    if (error)
        *error = QStringLiteral("Catalog canonical JSON contains an unsupported value.");
    return false;
}

QJsonValue optionalBoundedTelemetryInteger(const QJsonValue &value,
                                           qint64 maximum)
{
    if (!value.isDouble())
        return QJsonValue(QJsonValue::Null);
    const double raw = value.toDouble();
    if (!std::isfinite(raw) || raw < 0.0 || std::floor(raw) != raw)
        return QJsonValue(QJsonValue::Null);
    return QJsonValue(qMin(raw, double(maximum)));
}

QPair<QJsonValue, QJsonValue> normalizedTelemetryUsagePair(
    const QJsonValue &usedValue, const QJsonValue &totalValue)
{
    constexpr qint64 kMaximumReportedBytes = qint64(1) << 50;
    QJsonValue used =
        optionalBoundedTelemetryInteger(usedValue, kMaximumReportedBytes);
    const QJsonValue total =
        optionalBoundedTelemetryInteger(totalValue, kMaximumReportedBytes);
    if (used.isNull() || total.isNull() || total.toDouble() <= 0.0) {
        return {QJsonValue(QJsonValue::Null),
                QJsonValue(QJsonValue::Null)};
    }
    if (used.toDouble() > total.toDouble())
        used = total;
    return {used, total};
}

bool isSafeSshEndpoint(const QString &host, const QString &sshUser)
{
    static const QRegularExpression hostPattern(
        QStringLiteral("^[A-Za-z0-9](?:[A-Za-z0-9.:-]{0,251}[A-Za-z0-9])?$"));
    static const QRegularExpression userPattern(
        QStringLiteral("^[A-Za-z_][A-Za-z0-9_.-]{0,63}$"));
    const QString cleanHost = host.trimmed();
    return cleanHost.size() <= 253 &&
           !cleanHost.contains(QStringLiteral("..")) &&
           hostPattern.match(cleanHost).hasMatch() &&
           userPattern.match(sshUser.trimmed()).hasMatch();
}

QJsonArray scrubSavedHostPasswords(
    const QJsonArray &hosts, QHash<QString, QString> *sessionPasswords,
    bool *changed)
{
    static const QStringList secretFields{
        QStringLiteral("pass"), QStringLiteral("password"),
        QStringLiteral("sshPassword"), QStringLiteral("adminPassword")};
    QJsonArray cleanHosts;
    bool removedSecret = false;
    for (const QJsonValue &value : hosts) {
        if (!value.isObject()) {
            cleanHosts.append(value);
            continue;
        }
        QJsonObject host = value.toObject();
        QString migratedPassword;
        for (const QString &field : secretFields) {
            if (!host.contains(field))
                continue;
            if (migratedPassword.isEmpty())
                migratedPassword = host.value(field).toString();
            host.remove(field);
            removedSecret = true;
        }
        if (sessionPasswords && !migratedPassword.isEmpty()) {
            const QString key = savedHostCredentialKey(
                host.value(QStringLiteral("name")).toString(),
                host.value(QStringLiteral("ip")).toString(),
                host.value(QStringLiteral("user")).toString());
            QString replacedPassword = sessionPasswords->take(key);
            replacedPassword.fill(QChar::Null);
            sessionPasswords->insert(key, migratedPassword);
        }
        cleanHosts.append(host);
    }
    if (changed)
        *changed = removedSecret;
    return cleanHosts;
}

} // namespace

QString sharedHostKeyDirectory()
{
    const QString sshStateDir = forkmesh::paths::sshDir();
    if (sshStateDir.isEmpty())
        return {};
    if (!QDir().mkpath(sshStateDir))
        return {};
    QFile::setPermissions(
        sshStateDir,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner |
            QFileDevice::ExeOwner);
    return sshStateDir;
}

QString sharedHostKeyPath()
{
    const QString sshStateDir = sharedHostKeyDirectory();
    if (sshStateDir.isEmpty())
        return {};
    const QString sharedPath =
        QDir(sshStateDir).filePath(QStringLiteral("forkmesh_shared_ed25519"));
    if (QFileInfo(sharedPath).isFile())
        return sharedPath;
    const QString legacyPath =
        QDir(sshStateDir).filePath(QStringLiteral("vultr_mirror_ed25519"));
    if (QFileInfo(legacyPath).isFile())
        return legacyPath;
    return sharedPath;
}

QString existingSharedHostIdentityFile()
{
    const QString keyPath = sharedHostKeyPath();
    return QFileInfo(keyPath).isFile() ? keyPath : QString();
}

HostSshCommand buildHostSshCommand(const QString &host,
                                   const QString &sshUser,
                                   const QString &sshPassword,
                                   const QString &remoteCommand,
                                   QString *error,
                                   const QString &identityFile)
{
    HostSshCommand command;
    if (!isSafeSshEndpoint(host, sshUser)) {
        if (error)
            *error = QStringLiteral("The saved SSH host or username is invalid.");
        return command;
    }
    if (remoteCommand.trimmed().isEmpty() ||
        remoteCommand.contains(QChar::Null)) {
        if (error)
            *error = QStringLiteral("The remote SSH command is invalid.");
        return command;
    }

    const QString sshStateDir = sharedHostKeyDirectory();
    if (sshStateDir.isEmpty()) {
        if (error)
            *error = QStringLiteral(
                "ForkMesh could not create its persistent SSH trust store.");
        return command;
    }
    const QString knownHostsPath =
        QDir(sshStateDir).filePath(QStringLiteral("known_hosts"));
    if (!QFileInfo::exists(knownHostsPath)) {
        QFile knownHosts(knownHostsPath);
        if (!knownHosts.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            if (error) {
                *error = QStringLiteral(
                    "ForkMesh could not initialize its persistent SSH trust "
                    "store.");
            }
            return command;
        }
        knownHosts.close();
    }
    QFile::setPermissions(knownHostsPath,
                          QFileDevice::ReadOwner |
                              QFileDevice::WriteOwner);

    QStringList arguments{
        QStringLiteral("-o"), QStringLiteral("StrictHostKeyChecking=accept-new"),
        QStringLiteral("-o"),
        QStringLiteral("UserKnownHostsFile=") + knownHostsPath,
        QStringLiteral("-o"), QStringLiteral("HashKnownHosts=yes"),
        QStringLiteral("-o"), QStringLiteral("UpdateHostKeys=yes"),
        QStringLiteral("-o"), QStringLiteral("ConnectTimeout=30"),
    };
    // Pin authentication to the ForkMesh-managed key the caller resolved — the
    // shared fleet key for every host, or a key a saved host recorded itself.
    // -i is argv-safe for any path; IdentitiesOnly stops the agent offering
    // unrelated keys first.
    const QString identity = identityFile.trimmed();
    if (!identity.isEmpty()) {
        if (identity.contains(QChar::Null) ||
            !QFileInfo(identity).isFile()) {
            if (error)
                *error = QStringLiteral(
                    "The managed SSH key for this host is missing.");
            return command;
        }
        arguments << QStringLiteral("-i") << identity
                  << QStringLiteral("-o")
                  << QStringLiteral("IdentitiesOnly=yes");
    }
    command.environment = QProcessEnvironment::systemEnvironment();
    command.environment.remove(QStringLiteral("SSHPASS"));
    if (sshPassword.isEmpty()) {
        command.program = QStringLiteral("ssh");
        arguments << QStringLiteral("-o") << QStringLiteral("BatchMode=yes")
                  << QStringLiteral("-o")
                  << QStringLiteral("PreferredAuthentications=publickey");
    } else {
        command.program = QStringLiteral("sshpass");
        command.environment.insert(QStringLiteral("SSHPASS"), sshPassword);
        arguments.prepend(QStringLiteral("ssh"));
        arguments.prepend(QStringLiteral("-e"));
        // Let an agent/default key win when available, then use the
        // session-only password. Never disable public-key authentication.
        arguments << QStringLiteral("-o")
                  << QStringLiteral(
                         "PreferredAuthentications=publickey,password");
    }
    arguments << sshUser.trimmed() + QLatin1Char('@') + host.trimmed()
              << remoteCommand;
    command.arguments = arguments;
    if (error)
        error->clear();
    return command;
}

HostSshCommand buildHostInteractiveSshCommand(const QString &host,
                                              const QString &sshUser,
                                              const QString &sshPassword,
                                              const QString &remoteCommand,
                                              QString *error,
                                              const QString &identityFile)
{
    HostSshCommand command = buildHostSshCommand(host, sshUser, sshPassword,
                                                 remoteCommand, error,
                                                 identityFile);
    if (command.program.isEmpty())
        return command;
    command.arguments.insert(command.arguments.size() - 2,
                             QStringLiteral("-tt"));
    return command;
}

HostSshCommand buildHostLoginShellSshCommand(const QString &host,
                                             const QString &sshUser,
                                             const QString &sshPassword,
                                             QString *error,
                                             const QString &identityFile)
{
    HostSshCommand command = buildHostSshCommand(host, sshUser, sshPassword,
                                                 QStringLiteral(":"), error,
                                                 identityFile);
    if (command.program.isEmpty())
        return command;
    command.arguments.removeLast();
    command.arguments.insert(command.arguments.size() - 1,
                             QStringLiteral("-tt"));
    return command;
}

QString hostSshCommandLine(const HostSshCommand &command)
{
    if (command.program.isEmpty())
        return {};
    const auto quote = [](const QString &word) {
        QString value = word;
        value.replace(QStringLiteral("'"), QStringLiteral("'\\''"));
        return QLatin1Char('\'') + value + QLatin1Char('\'');
    };
    QStringList words{quote(command.program)};
    for (const QString &argument : command.arguments)
        words << quote(argument);
    return words.join(QLatin1Char(' '));
}

QString managedReleaseArtifactSeedCommand(const QString &sha256)
{
    const QString hash = sha256.trimmed().toLower();
    static const QRegularExpression exactDigest(
        QStringLiteral("^[0-9a-f]{64}$"));
    if (!exactDigest.match(hash).hasMatch())
        return {};

    // The only substituted value has passed exactDigest, so the command is
    // fixed shell text rather than a command assembled from remote content.
    // Write beside the destination and rename only after hashing: neither a
    // short SSH stream nor a corrupt controller file can replace a blob that a
    // mirror was already serving. Root creates and delegates the exact digest
    // directory first because older nodes may have root-owned shard parents;
    // the unprivileged account still receives and verifies every byte.
    return QStringLiteral(
               "set -eu; umask 077; "
               "[ \"$(id -u)\" = 0 ] || { printf 'Release artifact seeding "
               "requires root.\\n' >&2; exit 77; }; "
               "id forkmesh-node >/dev/null 2>&1 || { printf 'The managed "
               "mirror service account is missing.\\n' >&2; exit 76; }; "
               "dir=/var/lib/forkmesh/repositories/forkmesh-forkmesh.git/"
               "forkmesh-releases/sha256/%1/%2; "
               "install -d -o forkmesh-node -g forkmesh-node -m 0700 "
               "\"$dir\"; "
               "exec runuser -u forkmesh-node -- /bin/sh -c '"
               "set -eu; umask 077; "
               "dir=/var/lib/forkmesh/repositories/forkmesh-forkmesh.git/"
               "forkmesh-releases/sha256/%1/%2; "
               "install -d -m 0700 \"$dir\"; "
               "tmp=\"$(mktemp \"$dir/.seed.XXXXXX\")\" || exit 70; "
               "cleanup() { rm -f -- \"$tmp\"; }; "
               "trap cleanup EXIT HUP INT TERM; "
               "cat >\"$tmp\"; "
               "actual=\"$(sha256sum \"$tmp\")\"; actual=\"${actual%% *}\"; "
               "[ \"$actual\" = %2 ] || { printf \"Release artifact checksum "
               "verification failed.\\n\" >&2; exit 74; }; "
               "chmod 0600 \"$tmp\"; mv -f -- \"$tmp\" \"$dir/data\"; "
               "trap - EXIT HUP INT TERM'")
        .arg(hash.left(2), hash);
}

QString managedReleaseArtifactInventoryCommand(const QStringList &sha256s)
{
    // Keep the remote command and its output bounded. Release artifacts are
    // large, so hundreds of retained objects are already far beyond the
    // normal fleet shape; 512 digests still fit comfortably in an SSH argv.
    if (sha256s.isEmpty() || sha256s.size() > 512)
        return {};
    static const QRegularExpression exactDigest(
        QStringLiteral("^[0-9a-f]{64}$"));
    QStringList hashes;
    QSet<QString> seen;
    for (const QString &value : sha256s) {
        const QString hash = value.trimmed().toLower();
        if (!exactDigest.match(hash).hasMatch() || seen.contains(hash))
            return {};
        hashes.append(hash);
        seen.insert(hash);
    }

    // Every substituted word passed exactDigest. The service account can read
    // its own CAS, while root remains only the authenticated SSH transport.
    // A corrupt object is reported as missing because the seed receiver below
    // will hash and atomically replace it before refs are reconciled.
    return QStringLiteral(
               "set -eu; "
               "[ \"$(id -u)\" = 0 ] || { printf 'Release artifact inventory "
               "requires root.\\n' >&2; exit 77; }; "
               "id forkmesh-node >/dev/null 2>&1 || { printf 'The managed "
               "mirror service account is missing.\\n' >&2; exit 76; }; "
               "exec runuser -u forkmesh-node -- /bin/sh -c '"
               "set -eu; "
               "root=/var/lib/forkmesh/repositories/forkmesh-forkmesh.git/"
               "forkmesh-releases/sha256; "
               "for hash in %1; do "
               "shard=\"$(printf %s \"$hash\" | cut -c1-2)\"; "
               "path=\"$root/$shard/$hash/data\"; "
               "if [ ! -f \"$path\" ] || [ -L \"$path\" ]; then "
               "printf \"%s\\n\" \"$hash\"; continue; fi; "
               "actual=\"$(sha256sum \"$path\")\"; actual=\"${actual%% *}\"; "
               "[ \"$actual\" = \"$hash\" ] || printf \"%s\\n\" \"$hash\"; "
               "done'")
        .arg(hashes.join(QLatin1Char(' ')));
}

QString buildHostAgentLoginRemoteCommand()
{
    return QStringLiteral(
        "sh -lc 'if test \"$(id -u)\" = 0 && id forkmesh-node "
        ">/dev/null 2>&1; then "
        "echo \"Opening the forkmesh-node service account shell.\"; "
        "exec su -s /bin/bash forkmesh-node; fi; "
        "export PATH=\"$HOME/.local/bin:$HOME/.claude/bin:$PATH\"; "
        "echo \"Finish the provider sign-ins on this mirror:\"; "
        "echo \"  claude        then run /login inside it\"; "
        "echo \"  codex login\"; "
        "echo; "
        "exec \"${SHELL:-/bin/sh}\" -l'");
}

QString savedHostCredentialKey(const QString &nodeName, const QString &host,
                               const QString &sshUser)
{
    const QJsonArray identity{
        nodeName.trimmed(), host.trimmed().toLower(), sshUser.trimmed()};
    return QString::fromUtf8(
        QJsonDocument(identity).toJson(QJsonDocument::Compact));
}

QString normalizeRemoteDiskPath(const QString &path)
{
    QString value = path.trimmed();
    if (value.isEmpty())
        value = QStringLiteral("/");
    if (!value.startsWith(QLatin1Char('/')) || value.contains(QChar::Null) ||
        value.contains(QLatin1Char('\n')) || value.contains(QLatin1Char('\r')) ||
        value.size() > 4096) {
        return {};
    }
    QStringList parts;
    const QStringList raw = value.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &part : raw) {
        if (part == QStringLiteral("."))
            continue;
        if (part == QStringLiteral("..")) {
            if (!parts.isEmpty())
                parts.removeLast();
            continue;
        }
        parts.append(part);
    }
    if (parts.isEmpty())
        return QStringLiteral("/");
    return QLatin1Char('/') + parts.join(QLatin1Char('/'));
}

QString buildHostDiskUsageCommand(const QString &path, QString *error)
{
    const QString target = normalizeRemoteDiskPath(path);
    if (target.isEmpty()) {
        if (error) {
            *error = QStringLiteral(
                "That is not a valid absolute path on the host.");
        }
        return {};
    }
    const auto sentinelText = [](const char *message) {
        return QString::fromLatin1(
            QByteArray(message).toBase64(QByteArray::Base64Encoding));
    };
    const QString unreadable = sentinelText(
        "ForkMesh could not read that directory on this host.");
    const QString noBase64 = sentinelText(
        "This host has no base64 command, so ForkMesh cannot read its size "
        "map safely.");
    const QString script =
        QStringLiteral(
            "set -u\n"
            "LC_ALL=C\n"
            "export LC_ALL\n"
            "p=%1\n"
            "if [ ! -d \"$p\" ] || [ ! -r \"$p\" ]; then\n"
            "  printf 'FORKMESH-DU1-ERROR %2\\n'\n"
            "  exit 0\n"
            "fi\n"
            "if ! command -v base64 >/dev/null 2>&1; then\n"
            "  printf 'FORKMESH-DU1-ERROR %3\\n'\n"
            "  exit 0\n"
            "fi\n"
            "tab=$(printf '\\t')\n"
            "du -x -k -a -d 1 -- \"$p\" 2>/dev/null | "
            "while IFS=\"$tab\" read -r sz nm; do\n"
            "  [ -n \"${sz:-}\" ] || continue\n"
            "  if [ \"$nm\" = \"$p\" ]; then t=T\n"
            "  elif [ -L \"$nm\" ]; then t=f\n"
            "  elif [ -d \"$nm\" ]; then t=d\n"
            "  else t=f\n"
            "  fi\n"
            "  printf 'FORKMESH-DU1 %s %s %s\\n' \"$t\" \"$sz\" "
            "\"$(printf '%s' \"$nm\" | base64 | tr -d '\\n')\"\n"
            "done\n"
            "printf 'FORKMESH-DU1-END\\n'\n")
            .arg(shellSingleQuote(target), unreadable, noBase64);
    if (error)
        error->clear();
    return QStringLiteral("sh -lc ") + shellSingleQuote(script);
}

QString buildHostMountUsageCommand()
{
    const auto sentinelText = [](const char *message) {
        return QString::fromLatin1(
            QByteArray(message).toBase64(QByteArray::Base64Encoding));
    };
    const QString noBase64 = sentinelText(
        "This host has no base64 command, so ForkMesh cannot list its mount "
        "points safely.");
    const QString noDf = sentinelText(
        "This host has no df command, so ForkMesh cannot read mount usage.");
    const QString script =
        QStringLiteral(
            "set -u\n"
            "LC_ALL=C\n"
            "export LC_ALL\n"
            "if ! command -v base64 >/dev/null 2>&1; then\n"
            "  printf 'FORKMESH-MOUNT1-ERROR %1\\n'\n"
            "  exit 0\n"
            "fi\n"
            "if ! command -v df >/dev/null 2>&1; then\n"
            "  printf 'FORKMESH-MOUNT1-ERROR %2\\n'\n"
            "  exit 0\n"
            "fi\n"
            "df -P -k -l 2>/dev/null | sed 1d | "
            "while IFS= read -r line; do\n"
            "  set -- $line\n"
            "  [ \"$#\" -ge 6 ] || continue\n"
            "  total=$2; used=$3; avail=$4\n"
            "  shift 5\n"
            "  mp=$*\n"
            "  case \"$total:$used:$avail\" in\n"
            "    *[!0-9:]*) continue ;;\n"
            "  esac\n"
            "  case \"$mp\" in\n"
            "    /*) ;;\n"
            "    *) continue ;;\n"
            "  esac\n"
            "  printf 'FORKMESH-MOUNT1 %s %s %s %s\\n' "
            "\"$total\" \"$used\" \"$avail\" "
            "\"$(printf '%s' \"$mp\" | base64 | tr -d '\\n')\"\n"
            "done\n"
            "printf 'FORKMESH-MOUNT1-END\\n'\n")
            .arg(noBase64, noDf);
    return QStringLiteral("sh -lc ") + shellSingleQuote(script);
}

HostMountUsageList parseHostMountUsage(const QByteArray &output)
{
    HostMountUsageList result;
    QSet<QString> seenPaths;
    const QStringList lines =
        QString::fromUtf8(output)
            .split(QRegularExpression(QStringLiteral("[\\r\\n]")),
                   Qt::SkipEmptyParts);
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (line == QStringLiteral("FORKMESH-MOUNT1-END")) {
            result.complete = true;
            continue;
        }
        if (line.startsWith(QStringLiteral("FORKMESH-MOUNT1-ERROR "))) {
            const QByteArray decoded = QByteArray::fromBase64(
                line.mid(22).trimmed().toLatin1());
            result.error =
                decoded.isEmpty()
                    ? QStringLiteral("The host refused the mount-usage read.")
                    : QString::fromUtf8(decoded);
            result.complete = true;
            continue;
        }
        if (!line.startsWith(QStringLiteral("FORKMESH-MOUNT1 ")))
            continue;
        const QStringList fields =
            line.mid(16).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (fields.size() != 4)
            continue;
        bool totalOk = false;
        bool usedOk = false;
        bool availableOk = false;
        const qint64 totalKib = fields.at(0).toLongLong(&totalOk);
        const qint64 usedKib = fields.at(1).toLongLong(&usedOk);
        const qint64 availableKib = fields.at(2).toLongLong(&availableOk);
        const QString path = normalizeRemoteDiskPath(QString::fromUtf8(
            QByteArray::fromBase64(fields.at(3).toLatin1())));
        if (!totalOk || !usedOk || !availableOk || totalKib <= 0 ||
            usedKib < 0 || availableKib < 0 || path.isEmpty() ||
            seenPaths.contains(path)) {
            continue;
        }
        seenPaths.insert(path);
        result.mounts.append(
            HostMountUsage{path, totalKib * 1024, usedKib * 1024,
                           availableKib * 1024});
    }
    std::sort(result.mounts.begin(), result.mounts.end(),
              [](const HostMountUsage &a, const HostMountUsage &b) {
                  if (a.path == QStringLiteral("/"))
                      return b.path != QStringLiteral("/");
                  if (b.path == QStringLiteral("/"))
                      return false;
                  return a.path.localeAwareCompare(b.path) < 0;
              });
    return result;
}

HostDiskUsage parseHostDiskUsage(const QByteArray &output, const QString &path)
{
    HostDiskUsage usage;
    usage.path = normalizeRemoteDiskPath(path);
    const QStringList lines =
        QString::fromUtf8(output)
            .split(QRegularExpression(QStringLiteral("[\\r\\n]")),
                   Qt::SkipEmptyParts);
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (line == QStringLiteral("FORKMESH-DU1-END")) {
            usage.complete = true;
            continue;
        }
        if (line.startsWith(QStringLiteral("FORKMESH-DU1-ERROR "))) {
            const QByteArray decoded = QByteArray::fromBase64(
                line.mid(19).trimmed().toLatin1());
            usage.error = decoded.isEmpty()
                ? QStringLiteral("The host refused the size-map read.")
                : QString::fromUtf8(decoded);
            usage.complete = true;
            continue;
        }
        if (!line.startsWith(QStringLiteral("FORKMESH-DU1 ")))
            continue;
        const QStringList fields =
            line.mid(13).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (fields.size() != 3)
            continue;
        bool ok = false;
        const qint64 kib = fields.at(1).toLongLong(&ok);
        if (!ok || kib < 0)
            continue;
        const QString decoded =
            QString::fromUtf8(QByteArray::fromBase64(fields.at(2).toLatin1()));
        if (decoded.isEmpty() || decoded.contains(QChar::Null))
            continue;
        const qint64 bytes = kib * 1024;
        if (fields.at(0) == QStringLiteral("T")) {
            usage.totalBytes = bytes;
            continue;
        }
        HostDiskEntry entry;
        entry.path = decoded;
        entry.name = decoded.section(QLatin1Char('/'), -1);
        if (entry.name.isEmpty())
            entry.name = decoded;
        entry.bytes = bytes;
        entry.directory = fields.at(0) == QStringLiteral("d");
        usage.entries.append(entry);
    }
    std::sort(usage.entries.begin(), usage.entries.end(),
              [](const HostDiskEntry &a, const HostDiskEntry &b) {
                  if (a.bytes != b.bytes)
                      return a.bytes > b.bytes;
                  return a.name.localeAwareCompare(b.name) < 0;
              });
    return usage;
}

QString formatDiskSize(qint64 bytes)
{
    if (bytes < 0)
        return QStringLiteral("—");
    if (bytes < 1024)
        return QStringLiteral("%1 B").arg(bytes);
    static const char *const units[] = {"KB", "MB", "GB", "TB", "PB"};
    double value = static_cast<double>(bytes) / 1024.0;
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    return QStringLiteral("%1 %2")
        .arg(value, 0, 'f', value >= 100.0 ? 0 : 1)
        .arg(QLatin1String(units[unit]));
}

QString nonRoutableAddressNote(const QString &host)
{
    static const QRegularExpression ipv4Pattern(
        QStringLiteral("^(\\d{1,3})\\.(\\d{1,3})\\.(\\d{1,3})\\.(\\d{1,3})$"));
    const QRegularExpressionMatch match = ipv4Pattern.match(host.trimmed());
    if (!match.hasMatch())
        return {};
    int octet[4] = {0, 0, 0, 0};
    for (int i = 0; i < 4; ++i) {
        octet[i] = match.captured(i + 1).toInt();
        if (octet[i] > 255)
            return {};
    }
    if (octet[0] == 10)
        return QStringLiteral("the private range 10.0.0.0/8 (RFC 1918)");
    if (octet[0] == 172 && octet[1] >= 16 && octet[1] <= 31)
        return QStringLiteral("the private range 172.16.0.0/12 (RFC 1918)");
    if (octet[0] == 192 && octet[1] == 168)
        return QStringLiteral("the private range 192.168.0.0/16 (RFC 1918)");
    if (octet[0] == 100 && octet[1] >= 64 && octet[1] <= 127)
        return QStringLiteral(
            "100.64.0.0/10, the carrier-grade NAT / shared address range "
            "(RFC 6598) that VPN meshes such as Tailscale also hand out");
    if (octet[0] == 169 && octet[1] == 254)
        return QStringLiteral("the link-local range 169.254.0.0/16");
    if (octet[0] == 127)
        return QStringLiteral("the loopback range 127.0.0.0/8");
    return {};
}

QString sshFailureSummary(int exitCode, const QString &outputTail)
{
    QString reason;
    const QStringList lines =
        outputTail.split(QRegularExpression(QStringLiteral("[\\r\\n]")),
                         Qt::SkipEmptyParts);
    for (int i = lines.size() - 1; i >= 0; --i) {
        const QString line = lines.at(i).trimmed();
        if (line.isEmpty())
            continue;
        reason = line;
        break;
    }
    if (reason.size() > 160)
        reason = reason.left(157) + QStringLiteral("...");
    if (reason.isEmpty())
        return QStringLiteral("exit %1").arg(exitCode);
    return QString::fromUtf8("exit %1 \xE2\x80\x94 %2")
        .arg(QString::number(exitCode), reason);
}

QString sshConnectionFailureHint(int exitCode, const QString &outputTail,
                                 const QString &host)
{
    if (exitCode != 255)
        return {};
    const QString tail = outputTail.toLower();
    const bool dropped =
        tail.contains(QStringLiteral("connection timed out")) ||
        tail.contains(QStringLiteral("operation timed out")) ||
        tail.contains(QStringLiteral("no route to host"));
    // Packets vanishing towards an address that is not routable on the public
    // internet is not a firewall at all — no network between here and there
    // can carry them. Say so instead of sending the operator off
    // to audit security groups that were never involved.
    if (dropped) {
        const QString range = nonRoutableAddressNote(host);
        if (!range.isEmpty())
            return QString::fromUtf8(
                       "%1 is in %2, so it is not reachable from the public "
                       "internet \xE2\x80\x94 the packets are dropped in "
                       "transit rather than by any firewall. Unless this "
                       "machine is on that same private network or VPN, use "
                       "the host's public address here (or connect to the "
                       "network that owns the range first).")
                .arg(host.trimmed(), range);
    }
    if (tail.contains(QStringLiteral("connection timed out")) ||
        tail.contains(QStringLiteral("operation timed out"))) {
        return QString::fromUtf8(
            "The connection to port 22 timed out \xE2\x80\x94 packets are "
            "being dropped, not rejected. This almost always means a firewall "
            "or cloud security group between here and the host is blocking "
            "SSH: check the provider's firewall/security-group rules for this "
            "instance (and any local network firewall) allow inbound TCP 22 "
            "from your IP, then confirm the address is correct and the host "
            "has finished booting.");
    }
    if (tail.contains(QStringLiteral("connection refused"))) {
        return QString::fromUtf8(
            "The host actively refused the connection on port 22 \xE2\x80\x94 "
            "SSH is not listening yet (a freshly booted instance can take a "
            "minute or two) or a firewall rule is rejecting the port outright. "
            "Wait a moment and retry; if it persists, check the provider's "
            "firewall/security-group settings for this instance.");
    }
    if (tail.contains(QStringLiteral("no route to host"))) {
        return QString::fromUtf8(
            "There is no network route to this host \xE2\x80\x94 check that "
            "the address is correct and that a firewall or security group "
            "is not dropping the traffic.");
    }
    if (tail.contains(QStringLiteral("host key verification failed"))) {
        return QString::fromUtf8(
            "The host's SSH key does not match the one ForkMesh already "
            "trusts for it \xE2\x80\x94 this usually means the instance was "
            "rebuilt/reinstalled at the same address. Remove the stale entry "
            "from this app's managed known_hosts file if you intended that, "
            "then retry.");
    }
    if (tail.contains(QStringLiteral("permission denied"))) {
        return QString::fromUtf8(
            "The host rejected the credentials \xE2\x80\x94 double-check the "
            "SSH user, password, and key for this saved host.");
    }
    return {};
}

bool sshFailureNeedsPassword(int exitCode, const QString &outputTail)
{
    if (exitCode != 255)
        return false;
    const QString tail = outputTail.toLower();
    return tail.contains(QStringLiteral("permission denied")) ||
           tail.contains(QStringLiteral("authentication failed")) ||
           tail.contains(
               QStringLiteral("no supported authentication methods"));
}

QJsonArray loadSavedHosts(QSettings &settings, const QString &settingsKey,
                          QHash<QString, QString> *sessionPasswords)
{
    const QJsonArray stored =
        QJsonDocument::fromJson(
            settings.value(settingsKey).toString().toUtf8())
            .array();
    bool changed = false;
    const QJsonArray clean =
        scrubSavedHostPasswords(stored, sessionPasswords, &changed);
    if (changed) {
        settings.setValue(
            settingsKey,
            QString::fromUtf8(
                QJsonDocument(clean).toJson(QJsonDocument::Compact)));
        settings.sync();
    }
    return clean;
}

void saveSavedHosts(QSettings &settings, const QString &settingsKey,
                    const QJsonArray &hosts)
{
    const QJsonArray clean =
        scrubSavedHostPasswords(hosts, nullptr, nullptr);
    settings.setValue(
        settingsKey,
        QString::fromUtf8(
            QJsonDocument(clean).toJson(QJsonDocument::Compact)));
    settings.sync();
}

bool savedHostMayBeActive(const QJsonObject &host)
{
    const QString status =
        host.value(QStringLiteral("status"))
            .toString(QStringLiteral("installed"))
            .trimmed()
            .toLower();
    return status != QLatin1String("added") &&
           status != QLatin1String("installing") &&
           status != QLatin1String("install failed") &&
           status != QLatin1String("uninstalling") &&
           status != QLatin1String("uninstalled");
}

bool savedHostInstallStatusMayBeStale(const QJsonObject &host, qint64 nowMs)
{
    const QString status = host.value(QStringLiteral("status"))
                               .toString()
                               .trimmed()
                               .toLower();
    if (status == QLatin1String("install failed"))
        return true;
    if (status != QLatin1String("installing"))
        return false;
    const qint64 since =
        host.value(QStringLiteral("statusChangedAt")).toVariant().toLongLong();
    return since <= 0 ||
           (nowMs > 0 && nowMs - since > kSavedHostInstallingStaleMs);
}

QJsonArray promoteSavedHostsProvenHealthy(const QJsonArray &savedHosts,
                                          const QJsonArray &mirrors,
                                          qint64 nowMs,
                                          QStringList *promotedNodes)
{
    if (promotedNodes)
        promotedNodes->clear();
    QSet<QString> healthy;
    for (const QJsonValue &value : mirrors) {
        const QJsonObject mirror = value.toObject();
        QString node = mirror.value(QStringLiteral("node")).toString().trimmed();
        if (node.isEmpty())
            node = mirror.value(QStringLiteral("owner")).toString().trimmed();
        // Deliberately narrower than "has a row": a relay lease can still list
        // a dead node, but not with a healthy, fresh endpoint.
        if (!node.isEmpty() &&
            mirror.value(QStringLiteral("status")).toString().compare(
                QStringLiteral("online"), Qt::CaseInsensitive) == 0 &&
            mirror.value(QStringLiteral("integrity")).toString().compare(
                QStringLiteral("ok"), Qt::CaseInsensitive) == 0 &&
            mirror.value(QStringLiteral("endpointHealthy")).toBool() &&
            mirror.value(QStringLiteral("endpointFresh")).toBool()) {
            healthy.insert(node.toLower());
        }
    }
    QJsonArray hosts = savedHosts;
    if (healthy.isEmpty())
        return hosts;
    for (int i = 0; i < hosts.size(); ++i) {
        QJsonObject host = hosts.at(i).toObject();
        const QString name =
            host.value(QStringLiteral("name")).toString().trimmed();
        if (name.isEmpty() || !healthy.contains(name.toLower()) ||
            !savedHostInstallStatusMayBeStale(host, nowMs)) {
            continue;
        }
        host.insert(QStringLiteral("status"), QStringLiteral("installed"));
        host.insert(QStringLiteral("statusChangedAt"), double(nowMs));
        hosts.replace(i, host);
        if (promotedNodes)
            promotedNodes->append(name);
    }
    return hosts;
}

QString validateMirrorActionsConfigurationRequest(
    const MirrorActionsConfigurationRequest &request)
{
    static const QRegularExpression requestIdPattern(
        QStringLiteral("^[a-f0-9]{32}$"));
    static const QRegularExpression hostPattern(
        QStringLiteral("^[A-Za-z0-9](?:[A-Za-z0-9.:-]{0,251}[A-Za-z0-9])?$"));
    static const QRegularExpression userPattern(
        QStringLiteral("^[A-Za-z_][A-Za-z0-9_.-]{0,63}$"));
    static const QRegularExpression nodePattern(
        QStringLiteral("^[a-z][a-z0-9-]{0,62}$"));
    static const QRegularExpression variablePattern(
        QStringLiteral("^[A-Za-z_][A-Za-z0-9_]{0,63}$"));

    if (!requestIdPattern.match(request.requestId).hasMatch())
        return QStringLiteral("The Actions request ID is invalid.");
    const QString host = request.host.trimmed();
    if (host.size() > 253 || host.contains(QStringLiteral("..")) ||
        !hostPattern.match(host).hasMatch()) {
        return QStringLiteral("The saved host address is invalid.");
    }
    if (!userPattern.match(request.sshUser.trimmed()).hasMatch())
        return QStringLiteral("The saved SSH username is invalid.");
    if (!nodePattern.match(request.nodeName.trimmed()).hasMatch())
        return QStringLiteral("The mirror node name is invalid.");
    if (!request.replaceVariables && !request.variables.isEmpty()) {
        return QStringLiteral(
            "Variable values require explicit replace-variables approval.");
    }
    if (request.variables.size() > 64)
        return QStringLiteral("At most 64 Actions variables can be sent at once.");

    qsizetype totalValueBytes = 0;
    for (auto it = request.variables.constBegin();
         it != request.variables.constEnd(); ++it) {
        if (!variablePattern.match(it.key()).hasMatch()) {
            return QStringLiteral(
                "Actions variable names must use letters, digits, and "
                "underscores, and cannot start with a digit.");
        }
        if (it.value().contains(QChar::Null)) {
            return QStringLiteral(
                "Actions variable values cannot contain NUL characters.");
        }
        const qsizetype valueBytes = it.value().toUtf8().size();
        if (valueBytes > 16 * 1024) {
            return QStringLiteral(
                "Each Actions variable must be 16 KiB or smaller.");
        }
        totalValueBytes += valueBytes;
        if (totalValueBytes > 48 * 1024) {
            return QStringLiteral(
                "The combined Actions variable values must be 48 KiB or smaller.");
        }
    }
    return {};
}

QByteArray buildMirrorActionsConfigurationPayload(
    const MirrorActionsConfigurationRequest &request, QString *error)
{
    const QString validation =
        validateMirrorActionsConfigurationRequest(request);
    if (!validation.isEmpty()) {
        if (error)
            *error = validation;
        return {};
    }

    QJsonObject root{
        {QStringLiteral("type"),
         QStringLiteral("forkmesh.mirror-actions-configuration")},
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("requestId"), request.requestId},
        {QStringLiteral("node"), request.nodeName.trimmed()},
        {QStringLiteral("actionsEnabled"), request.actionsEnabled},
    };
    if (request.replaceVariables) {
        QJsonObject values;
        for (auto it = request.variables.constBegin();
             it != request.variables.constEnd(); ++it) {
            values.insert(it.key(), it.value());
        }
        root.insert(
            QStringLiteral("variables"),
            QJsonObject{
                {QStringLiteral("mode"), QStringLiteral("replace")},
                {QStringLiteral("values"), values},
            });
    }
    QByteArray payload = QJsonDocument(root).toJson(QJsonDocument::Compact);
    payload.append('\n');
    if (payload.size() > 64 * 1024) {
        payload.fill('\0');
        payload.clear();
        if (error)
            *error = QStringLiteral(
                "The Actions configuration payload exceeds 64 KiB.");
        return {};
    }
    if (error)
        error->clear();
    return payload;
}

MirrorActionsSshCommand buildMirrorActionsSshCommand(
    const MirrorActionsConfigurationRequest &request,
    const QString &sshPassword, QString *error,
    const QString &identityFile)
{
    MirrorActionsSshCommand command;
    command.standardInput =
        buildMirrorActionsConfigurationPayload(request, error);
    if (command.standardInput.isEmpty())
        return command;

    const QString remoteCommand = QStringLiteral(
        "if command -v forkmesh >/dev/null 2>&1; then "
        "exec forkmesh --configure-mirror-actions-stdin; "
        "elif [ -x \"$HOME/.local/bin/forkmesh\" ]; then "
        "exec \"$HOME/.local/bin/forkmesh\" "
        "--configure-mirror-actions-stdin; "
        "else printf 'ForkMesh Actions helper is not installed.\\n' >&2; "
        "exit 127; fi");
    const HostSshCommand ssh = buildHostSshCommand(
        request.host, request.sshUser, sshPassword, remoteCommand, error,
        identityFile);
    if (ssh.program.isEmpty()) {
        command.standardInput.fill('\0');
        command.standardInput.clear();
        return command;
    }
    command.program = ssh.program;
    command.arguments = ssh.arguments;
    command.environment = ssh.environment;
    if (error)
        error->clear();
    return command;
}

QJsonObject parseMirrorActionsConfigurationResult(
    const QByteArray &output, const QString &expectedRequestId,
    const QString &expectedNodeName, QString *error)
{
    constexpr qsizetype kMaximumOutputBytes = 64 * 1024;
    constexpr auto kPrefix = "FORKMESH_ACTIONS_RESULT=";
    if (output.size() > kMaximumOutputBytes) {
        if (error)
            *error = QStringLiteral(
                "The mirror Actions helper returned too much output.");
        return {};
    }
    QByteArray encoded;
    int sentinels = 0;
    const QList<QByteArray> lines = output.split('\n');
    for (QByteArray line : lines) {
        if (line.endsWith('\r'))
            line.chop(1);
        if (!line.startsWith(kPrefix))
            continue;
        ++sentinels;
        encoded = line.mid(qstrlen(kPrefix));
    }
    if (sentinels != 1 || encoded.isEmpty() || encoded.size() > 4096) {
        if (error)
            *error = QStringLiteral(
                "The mirror Actions helper did not return one valid result.");
        return {};
    }
    if (!QRegularExpression(QStringLiteral("^[A-Za-z0-9_-]+$"))
             .match(QString::fromLatin1(encoded))
             .hasMatch()) {
        if (error)
            *error = QStringLiteral(
                "The mirror Actions helper result encoding is invalid.");
        return {};
    }
    const QByteArray decoded = QByteArray::fromBase64(
        encoded, QByteArray::Base64UrlEncoding |
                     QByteArray::AbortOnBase64DecodingErrors);
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(decoded, &parseError);
    const QJsonObject result = document.object();
    static const QSet<QString> allowedKeys{
        QStringLiteral("type"),
        QStringLiteral("schemaVersion"),
        QStringLiteral("ok"),
        QStringLiteral("requestId"),
        QStringLiteral("node"),
        QStringLiteral("actionsEnabled"),
        QStringLiteral("variablesReplaced"),
        QStringLiteral("variableCount"),
        QStringLiteral("errorCode"),
    };
    bool hasUnexpectedKey = false;
    for (auto it = result.constBegin(); it != result.constEnd(); ++it) {
        if (!allowedKeys.contains(it.key())) {
            hasUnexpectedKey = true;
            break;
        }
    }
    const QJsonValue variableCount =
        result.value(QStringLiteral("variableCount"));
    const bool validCount =
        variableCount.isDouble() &&
        variableCount.toDouble() >= 0 &&
        variableCount.toDouble() <= 64 &&
        std::floor(variableCount.toDouble()) == variableCount.toDouble();
    const QString errorCode =
        result.value(QStringLiteral("errorCode")).toString();
    static const QRegularExpression errorCodePattern(
        QStringLiteral("^[a-z][a-z0-9_]{0,63}$"));
    const bool validErrorCode =
        errorCode.isEmpty() ||
        errorCodePattern.match(errorCode).hasMatch();
    if (parseError.error != QJsonParseError::NoError ||
        !document.isObject() ||
        hasUnexpectedKey ||
        result.value(QStringLiteral("type")).toString() !=
            QLatin1String("forkmesh.mirror-actions-configuration-result") ||
        result.value(QStringLiteral("schemaVersion")).toInt() != 1 ||
        result.value(QStringLiteral("ok")).isBool() == false ||
        result.value(QStringLiteral("requestId")).toString() !=
            expectedRequestId ||
        result.value(QStringLiteral("node")).toString() !=
            expectedNodeName ||
        !result.value(QStringLiteral("actionsEnabled")).isBool() ||
        !result.value(QStringLiteral("variablesReplaced")).isBool() ||
        !validCount || !validErrorCode) {
        if (error)
            *error = QStringLiteral(
                "The mirror Actions helper result failed validation.");
        return {};
    }
    if (error)
        error->clear();
    return result;
}

QString validateCloudflareBootstrapRequest(
    const CloudflareBootstrapRequest &request,
    bool allowAutomaticTopology)
{
    const QString hostname = normalizedDnsName(request.hostname);
    const QString zone = normalizedDnsName(request.zoneName);
    const bool automaticTopology =
        allowAutomaticTopology && hostname.isEmpty() && zone.isEmpty();
    if (!automaticTopology) {
        if (!isDnsName(hostname))
            return QStringLiteral(
                "Enter a DNS hostname such as mirror.example.com (without a URL or path).");
        if (!isDnsName(zone))
            return QStringLiteral(
                "Enter the Cloudflare zone name such as example.com.");
        if (hostname != zone &&
            !hostname.endsWith(QLatin1Char('.') + zone)) {
            return QStringLiteral(
                "The deployment hostname must be inside the selected zone.");
        }
    }

    static const QRegularExpression resourceName(
        QStringLiteral("^[a-z][a-z0-9-]{0,62}$"));
    const QString node = request.nodeName.trimmed();
    if (!node.isEmpty() && !resourceName.match(node).hasMatch()) {
        return QStringLiteral(
            "The node name must start with a letter and contain only lowercase "
            "letters, digits, or hyphens (63 characters maximum).");
    }
    const QString account = request.accountId.trimmed();
    static const QRegularExpression accountId(
        QStringLiteral("^[A-Za-z0-9_-]{1,128}$"));
    if (!account.isEmpty() && !accountId.match(account).hasMatch())
        return QStringLiteral("The Cloudflare account ID contains invalid characters.");
    const QString label = request.relayLabel.trimmed();
    if (label.size() > 80) {
        return QStringLiteral("The relay label must be 80 characters or fewer.");
    }
    for (const QChar ch : label) {
        if (ch.unicode() < 32 || ch.unicode() == 127)
            return QStringLiteral("The relay label contains a control character.");
    }
    if (!isSafeMainRelayUrl(request.mainRelayUrl)) {
        return QStringLiteral(
            "The upstream relay must be an HTTPS origin with no credentials, "
            "query, fragment, or private path.");
    }
    return {};
}

QString findCloudflareBootstrapScript(const QString &sourceDir,
                                      const QString &applicationDir)
{
    QStringList candidates;
    const QString overridePath =
        qEnvironmentVariable("FORKMESH_CLOUDFLARE_BOOTSTRAP").trimmed();
    if (!overridePath.isEmpty())
        candidates.append(overridePath);

    const QString source = sourceDir.trimmed();
    if (!source.isEmpty()) {
        candidates.append(
            QDir(source).absoluteFilePath(QStringLiteral("../tools/cloudflare_bootstrap.py")));
        candidates.append(
            QDir(source).absoluteFilePath(QStringLiteral("tools/cloudflare_bootstrap.py")));
    }

    const QString appDir = applicationDir.trimmed().isEmpty()
                               ? QCoreApplication::applicationDirPath()
                               : applicationDir.trimmed();
    candidates.append(
        QDir(appDir).absoluteFilePath(QStringLiteral("../tools/cloudflare_bootstrap.py")));
    candidates.append(
        QDir(appDir).absoluteFilePath(QStringLiteral("tools/cloudflare_bootstrap.py")));
    candidates.append(
        installedToolCandidates(
            QStringLiteral("cloudflare_bootstrap.py"), appDir));

    for (const QString &candidate : candidates) {
        const QString resolved = canonicalCandidate(candidate);
        if (!resolved.isEmpty() && hasCompleteWorkerBundle(resolved))
            return resolved;
    }
    return {};
}

QString findCloudflareWorkerDirectory(const QString &sourceDir,
                                      const QString &applicationDir)
{
    const QString bootstrap =
        findCloudflareBootstrapScript(sourceDir, applicationDir);
    if (bootstrap.isEmpty())
        return {};
    return completeWorkerBundle(bootstrap);
}

QString findSiteDeployScript(const QString &sourceDir,
                             const QString &applicationDir)
{
    QStringList candidates;
    const QString overridePath =
        qEnvironmentVariable("FORKMESH_SITE_DEPLOY").trimmed();
    if (!overridePath.isEmpty())
        candidates.append(overridePath);
    const QString source = sourceDir.trimmed();
    if (!source.isEmpty()) {
        const QDir suppliedSource(source);
        candidates.append(suppliedSource.absoluteFilePath(
            QStringLiteral("app/deploy.sh")));
        candidates.append(suppliedSource.absoluteFilePath(
            QStringLiteral("tools/deploy.sh")));
        const QDir sourceRoot(QDir(source).absoluteFilePath(QStringLiteral("..")));
        candidates.append(sourceRoot.absoluteFilePath(
            QStringLiteral("app/deploy.sh")));
        candidates.append(sourceRoot.absoluteFilePath(
            QStringLiteral("tools/deploy.sh")));
    }
    const QString worker =
        findCloudflareWorkerDirectory(sourceDir, applicationDir);
    if (!worker.isEmpty()) {
        candidates.append(
            QDir(worker).absoluteFilePath(QStringLiteral("deploy.sh")));
    }
    const QString appDir = applicationDir.trimmed().isEmpty()
                               ? QCoreApplication::applicationDirPath()
                               : applicationDir.trimmed();
    const QDir installedRoot(
        QDir(appDir).absoluteFilePath(QStringLiteral("../share/forkmesh")));
    candidates.append(
        installedRoot.absoluteFilePath(QStringLiteral("deploy.sh")));
    candidates.append(
        installedRoot.absoluteFilePath(QStringLiteral("app/deploy.sh")));
    for (const QString &candidate : candidates) {
        const QString resolved = canonicalCandidate(candidate);
        if (!resolved.isEmpty())
            return resolved;
    }
    return {};
}

QStringList siteDeployArguments(const QString &script, const QString &target,
                                bool force)
{
    static const QStringList targets = {
        QStringLiteral("deploy"), QStringLiteral("changed"),
        QStringLiteral("app"), QStringLiteral("world"), QStringLiteral("www")};
    const QString trimmedScript = script.trimmed();
    const QString deployTarget = target.trimmed().toLower();
    if (trimmedScript.isEmpty() || !targets.contains(deployTarget))
        return {};
    QStringList arguments{trimmedScript, deployTarget};
    if (force)
        arguments.append(QStringLiteral("--force"));
    return arguments;
}

namespace {

QString findPinnedTool(const QString &fileName, const QString &overrideName,
                       const QString &sourceDir,
                       const QString &applicationDir)
{
    QStringList candidates;
    const QString overridePath =
        qEnvironmentVariable(overrideName.toLatin1().constData()).trimmed();
    if (!overridePath.isEmpty())
        candidates.append(overridePath);
    const QString source = sourceDir.trimmed();
    if (!source.isEmpty()) {
        candidates.append(
            QDir(source).absoluteFilePath(
                QStringLiteral("../tools/") + fileName));
        candidates.append(
            QDir(source).absoluteFilePath(
                QStringLiteral("tools/") + fileName));
    }
    const QString appDir =
        applicationDir.trimmed().isEmpty()
            ? QCoreApplication::applicationDirPath()
            : applicationDir.trimmed();
    candidates.append(
        QDir(appDir).absoluteFilePath(
            QStringLiteral("../tools/") + fileName));
    candidates.append(
        QDir(appDir).absoluteFilePath(
            QStringLiteral("tools/") + fileName));
    candidates.append(installedToolCandidates(fileName, appDir));
    for (const QString &candidate : candidates) {
        const QString resolved = canonicalCandidate(candidate);
        if (!resolved.isEmpty())
            return resolved;
    }
    return {};
}

} // namespace

QString findCloudflareTunnelBootstrapScript(
    const QString &sourceDir, const QString &applicationDir)
{
    return findPinnedTool(
        QStringLiteral("cloudflare_tunnel_bootstrap.py"),
        QStringLiteral("FORKMESH_CLOUDFLARE_TUNNEL_BOOTSTRAP"),
        sourceDir, applicationDir);
}

QString findMirrorGatewayScript(
    const QString &sourceDir, const QString &applicationDir)
{
    return findPinnedTool(
        QStringLiteral("mirror_gateway.py"),
        QStringLiteral("FORKMESH_MIRROR_GATEWAY"),
        sourceDir, applicationDir);
}

QString findMcpServerScript(
    const QString &sourceDir, const QString &applicationDir)
{
    return findPinnedTool(
        QStringLiteral("forkmesh_mcp_server.py"),
        QStringLiteral("FORKMESH_MCP_SERVER"),
        sourceDir, applicationDir);
}

QString findCloudflaredInstallerScript(
    const QString &sourceDir, const QString &applicationDir)
{
    return findPinnedTool(
        QStringLiteral("cloudflared_install.py"),
        QStringLiteral("FORKMESH_CLOUDFLARED_INSTALLER"),
        sourceDir, applicationDir);
}

QString findNextAutoscaleScript(
    const QString &sourceDir, const QString &applicationDir)
{
    return findPinnedTool(
        QStringLiteral("next_autoscale.py"),
        QStringLiteral("FORKMESH_NEXT_AUTOSCALE"),
        sourceDir, applicationDir);
}

QString findNextAutodeployScript(
    const QString &sourceDir, const QString &applicationDir)
{
    return findPinnedTool(
        QStringLiteral("next_autodeploy.py"),
        QStringLiteral("FORKMESH_NEXT_AUTODEPLOY"),
        sourceDir, applicationDir);
}

QString findNextFleetScript(
    const QString &sourceDir, const QString &applicationDir)
{
    return findPinnedTool(
        QStringLiteral("next_fleet.py"),
        QStringLiteral("FORKMESH_NEXT_FLEET"),
        sourceDir, applicationDir);
}

QString shlexQuote(const QString &value)
{
    QString escaped = value;
    escaped.replace(QLatin1Char('\''), QStringLiteral("'\"'\"'"));
    return QLatin1Char('\'') + escaped + QLatin1Char('\'');
}

QByteArray pythonCanonicalJson(const QJsonValue &value, QString *error)
{
    QByteArray output;
    if (!appendPythonCanonicalJson(value, &output, error))
        return {};
    if (error)
        error->clear();
    return output;
}

QByteArray catalogV2SigningPayload(QJsonObject normalizedRecord,
                                   QString *error)
{
    normalizedRecord.remove(QStringLiteral("signature"));
    const QByteArray canonical =
        pythonCanonicalJson(normalizedRecord, error);
    if (canonical.isEmpty())
        return {};
    const QByteArray digest =
        QCryptographicHash::hash(canonical, QCryptographicHash::Sha256).toHex();
    if (error)
        error->clear();
    return QByteArrayLiteral("forkmesh-catalog-v2\n") + digest;
}

QJsonObject normalizedCatalogHostTelemetry(const QJsonObject &data)
{
    const auto memoryUsage = normalizedTelemetryUsagePair(
        data.value(QStringLiteral("memUsedBytes")),
        data.value(QStringLiteral("memTotalBytes")));
    const auto diskUsage = normalizedTelemetryUsagePair(
        data.value(QStringLiteral("diskUsedBytes")),
        data.value(QStringLiteral("diskTotalBytes")));
    return {
        {QStringLiteral("cpuPercent"),
         optionalBoundedTelemetryInteger(
             data.value(QStringLiteral("cpuPercent")), 100)},
        {QStringLiteral("memUsedBytes"), memoryUsage.first},
        {QStringLiteral("memTotalBytes"), memoryUsage.second},
        {QStringLiteral("diskUsedBytes"), diskUsage.first},
        {QStringLiteral("diskTotalBytes"), diskUsage.second},
    };
}

QByteArray privateReplicaRouteSigningPayload(
    const QString &owner, const QString &repository, const QString &node,
    const QString &opaqueId, const QString &replicaSha256, quint64 keyEpoch,
    bool active, qint64 issuedAtMs, QString *error)
{
    const auto cleanSegment = [](const QString &value) {
        return !value.trimmed().isEmpty() &&
               !value.contains(QLatin1Char('\n')) &&
               !value.contains(QLatin1Char('\r')) &&
               value == value.trimmed();
    };
    static const QRegularExpression digestPattern(
        QStringLiteral("^[0-9a-f]{64}$"));
    if (!cleanSegment(owner) || !cleanSegment(repository) ||
        !cleanSegment(node) || !digestPattern.match(opaqueId).hasMatch() ||
        !digestPattern.match(replicaSha256).hasMatch() || keyEpoch == 0 ||
        issuedAtMs <= 0) {
        if (error)
            *error = QStringLiteral("The private-replica route fields are invalid.");
        return {};
    }
    if (error)
        error->clear();
    return QByteArrayLiteral("forkmesh-private-route-v1\n") +
           owner.toUtf8() + '\n' + repository.toUtf8() + '\n' +
           node.toUtf8() + '\n' + opaqueId.toUtf8() + '\n' +
           replicaSha256.toUtf8() + '\n' +
           QByteArray::number(keyEpoch) + '\n' +
           (active ? QByteArrayLiteral("1") : QByteArrayLiteral("0")) +
           '\n' + QByteArray::number(issuedAtMs);
}

CloudflareBootstrapCommand buildCloudflareBootstrapCommand(
    const CloudflareBootstrapRequest &request,
    const QString &apiToken,
    const QString &scriptPath,
    const QString &pythonProgram,
    const QString &signerProgram,
    const QString &nodePublicKey)
{
    CloudflareBootstrapCommand command;
    command.program = pythonProgram.trimmed();
    command.arguments = {scriptPath};
    if (request.hostname.trimmed().isEmpty() &&
        request.zoneName.trimmed().isEmpty()) {
        command.arguments << QStringLiteral("--auto-configure");
    } else {
        command.arguments
            << QStringLiteral("--hostname")
            << normalizedDnsName(request.hostname)
            << QStringLiteral("--zone")
            << normalizedDnsName(request.zoneName);
    }
    if (!request.accountId.trimmed().isEmpty()) {
        command.arguments << QStringLiteral("--account-id")
                          << request.accountId.trimmed();
    }
    if (!request.nodeName.trimmed().isEmpty()) {
        command.arguments << QStringLiteral("--node-name")
                          << request.nodeName.trimmed();
    }
    if (!request.relayLabel.trimmed().isEmpty()) {
        command.arguments << QStringLiteral("--relay-label")
                          << request.relayLabel.trimmed();
    }
    command.arguments << QStringLiteral("--main-relay-url")
                      << request.mainRelayUrl.trimmed();
    if (request.dryRun)
        command.arguments << QStringLiteral("--dry-run");
    command.arguments << QStringLiteral("--json-stdout");

    if (!nodePublicKey.trimmed().isEmpty() &&
        !signerProgram.trimmed().isEmpty()) {
        command.arguments << QStringLiteral("--mirror-public-key")
                          << nodePublicKey.trimmed()
                          << QStringLiteral("--manifest-signer-command")
                          << (shlexQuote(signerProgram.trimmed()) +
                              QStringLiteral(" --sign-mirror-manifest"));
    } else {
        command.arguments << QStringLiteral("--skip-mirror-manifest");
    }

    command.environment = QProcessEnvironment::systemEnvironment();
    for (const QString &name : {
             QStringLiteral("CLOUDFLARE_API_TOKEN"),
             QStringLiteral("CF_API_TOKEN"),
             QStringLiteral("CLOUDFLARE_TOKEN"),
             QStringLiteral("CF_TOKEN"),
         }) {
        command.environment.remove(name);
    }
    command.environment.insert(QStringLiteral("CLOUDFLARE_API_TOKEN"), apiToken);
    return command;
}

namespace {

// First stored value whose name matches one of `names`, case-insensitively and
// in the caller's preference order. Values that span lines (or embed NULs) are
// rejected: they are never a real credential and would break the child
// environment they are destined for.
QString storedCredential(const QMap<QString, QString> &variables,
                         const QStringList &names)
{
    for (const QString &name : names) {
        for (auto it = variables.constBegin(); it != variables.constEnd();
             ++it) {
            if (it.key().trimmed().compare(name, Qt::CaseInsensitive) != 0)
                continue;
            const QString value = it.value().trimmed();
            if (value.isEmpty() || value.contains(QChar(u'\0')) ||
                value.contains(QLatin1Char('\n')) ||
                value.contains(QLatin1Char('\r'))) {
                continue;
            }
            return value;
        }
    }
    return {};
}

}  // namespace

QString cloudflareApiTokenFromVariables(
    const QMap<QString, QString> &variables)
{
    return storedCredential(variables, {
                                           QStringLiteral("CLOUDFLARE_API_TOKEN"),
                                           QStringLiteral("CF_API_TOKEN"),
                                           QStringLiteral("CLOUDFLARE_TOKEN"),
                                           QStringLiteral("CF_TOKEN"),
                                       });
}

QString cloudflareAccountIdFromVariables(
    const QMap<QString, QString> &variables)
{
    return storedCredential(variables,
                            {
                                QStringLiteral("CLOUDFLARE_ACCOUNT_ID"),
                                QStringLiteral("CF_ACCOUNT_ID"),
                                QStringLiteral("CLOUDFLARE_ACCOUNT"),
                            });
}

namespace {

constexpr int kNodeVersionProbeTimeoutMs = 3000;

QStringList executableNames(const QString &base)
{
#ifdef Q_OS_WIN
    return {base + QStringLiteral(".cmd"), base + QStringLiteral(".exe"), base};
#else
    return {base};
#endif
}

QString executableIn(const QString &binDir, const QString &base)
{
    const QDir dir(binDir);
    for (const QString &name : executableNames(base)) {
        const QString path = dir.absoluteFilePath(name);
        if (QFileInfo(path).isExecutable())
            return path;
    }
    return {};
}

std::array<int, 3> nodeVersionTriple(const QString &text)
{
    static const QRegularExpression versionPattern(
        QStringLiteral("v(\\d{1,4})(?:\\.(\\d{1,6}))?(?:\\.(\\d{1,6}))?"));
    const auto match = versionPattern.match(text);
    if (!match.hasMatch())
        return {0, 0, 0};
    return {match.captured(1).toInt(), match.captured(2).toInt(),
            match.captured(3).toInt()};
}

QStringList versionedNodeBinDirs(const QString &root, const QString &suffix)
{
    QDir rootDir(root);
    if (!rootDir.exists())
        return {};
    QStringList versions =
        rootDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::NoSort);
    std::sort(versions.begin(), versions.end(),
              [](const QString &left, const QString &right) {
                  return nodeVersionTriple(left) > nodeVersionTriple(right);
              });
    QStringList bins;
    bins.reserve(versions.size());
    for (const QString &version : versions) {
        bins.append(QDir(rootDir.absoluteFilePath(version))
                        .absoluteFilePath(suffix));
    }
    return bins;
}

// code-health: allow(blocking-wait) spawns `<binDir>/node --version`, capped at kNodeVersionProbeTimeoutMs (3 s) and killed on timeout; reached only through findNodeToolchain, which the GUI thread calls when the Cloudflare log monitor starts or is switched on (not per frame), and which stops at the first install new enough
int probeNodeMajorVersion(const QString &binDir)
{
    const QString node = executableIn(binDir, QStringLiteral("node"));
    if (node.isEmpty())
        return 0;
    QProcess process;
    process.setProgram(node);
    process.setArguments({QStringLiteral("--version")});
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.setStandardInputFile(QProcess::nullDevice());
    process.start();
    if (!process.waitForFinished(kNodeVersionProbeTimeoutMs)) {
        process.kill();
        process.waitForFinished(500);
        return 0;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return 0;
    return nodeMajorVersionFromText(
        QString::fromUtf8(process.readAllStandardOutput()));
}

}  // namespace

int nodeMajorVersionFromText(const QString &text)
{
    static const QRegularExpression versionPattern(
        QStringLiteral("v?(\\d{1,4})\\.\\d{1,6}\\.\\d{1,6}"));
    const auto match = versionPattern.match(text.trimmed());
    return match.hasMatch() ? match.captured(1).toInt() : 0;
}

QStringList nodeBinDirectoryCandidates()
{
    QStringList dirs;
    const auto add = [&dirs](const QString &dir) {
        const QString trimmed = dir.trimmed();
        if (trimmed.isEmpty())
            return;
        const QString canonical = QFileInfo(trimmed).canonicalFilePath();
        if (canonical.isEmpty() || dirs.contains(canonical) ||
            !QFileInfo(canonical).isDir()) {
            return;
        }
        if (executableIn(canonical, QStringLiteral("npx")).isEmpty())
            return;
        dirs.append(canonical);
    };

    add(qEnvironmentVariable("FORKMESH_NODE_BIN"));
    const QString pathNpx =
        QStandardPaths::findExecutable(QStringLiteral("npx"));
    if (!pathNpx.isEmpty())
        add(QFileInfo(pathNpx).absolutePath());

    const QDir home = QDir::home();
    QString nvmDir = qEnvironmentVariable("NVM_DIR").trimmed();
    if (nvmDir.isEmpty())
        nvmDir = home.absoluteFilePath(QStringLiteral(".nvm"));
    QStringList managed;
    managed += versionedNodeBinDirs(
        QDir(nvmDir).absoluteFilePath(QStringLiteral("versions/node")),
        QStringLiteral("bin"));
    managed += versionedNodeBinDirs(
        home.absoluteFilePath(QStringLiteral(".local/share/fnm/node-versions")),
        QStringLiteral("installation/bin"));
    managed += versionedNodeBinDirs(
        home.absoluteFilePath(QStringLiteral(".volta/tools/image/node")),
        QStringLiteral("bin"));
    managed += versionedNodeBinDirs(
        home.absoluteFilePath(QStringLiteral(".asdf/installs/nodejs")),
        QStringLiteral("bin"));
    for (const QString &dir : managed)
        add(dir);
    return dirs;
}

NodeToolchain findNodeToolchain(int minimumMajor)
{
    NodeToolchain toolchain;
    const QStringList candidates = nodeBinDirectoryCandidates();
    for (const QString &dir : candidates) {
        const int major = probeNodeMajorVersion(dir);
        if (major <= 0 || major < minimumMajor)
            continue;
        toolchain.npx = executableIn(dir, QStringLiteral("npx"));
        if (toolchain.npx.isEmpty())
            continue;
        toolchain.binDir = dir;
        toolchain.majorVersion = major;
        break;
    }
    return toolchain;
}

CloudflareBootstrapCommand buildCloudflareTailCommand(
    const QString &apiToken,
    const QString &accountId,
    const QString &npxProgram,
    const QString &nodeBinDir)
{
    CloudflareBootstrapCommand command;
    const QString token = apiToken.trimmed();
    const QString account = accountId.trimmed();
    command.program = npxProgram.trimmed();
    if (token.isEmpty() || command.program.isEmpty())
        return command;

    static const QRegularExpression accountPattern(
        QStringLiteral("^[A-Za-z0-9_-]{1,128}$"));
    if (!account.isEmpty() &&
        !accountPattern.match(account).hasMatch()) {
        command.program.clear();
        return command;
    }

    command.arguments = {
        QStringLiteral("--yes"),
        QStringLiteral("wrangler@4.136.2"),
        QStringLiteral("tail"),
        QStringLiteral("--format"),
        QStringLiteral("json"),
    };
    command.environment = QProcessEnvironment::systemEnvironment();
    const QString nodeBin = nodeBinDir.trimmed();
    if (!nodeBin.isEmpty()) {
        const QString existingPath =
            command.environment.value(QStringLiteral("PATH"));
        command.environment.insert(
            QStringLiteral("PATH"),
            existingPath.isEmpty()
                ? nodeBin
                : nodeBin + QDir::listSeparator() + existingPath);
    }
    command.environment.remove(QStringLiteral("CLOUDFLARE_API_KEY"));
    command.environment.remove(QStringLiteral("CLOUDFLARE_EMAIL"));
    command.environment.insert(QStringLiteral("CLOUDFLARE_API_TOKEN"),
                               token);
    if (account.isEmpty()) {
        command.environment.remove(
            QStringLiteral("CLOUDFLARE_ACCOUNT_ID"));
    } else {
        command.environment.insert(
            QStringLiteral("CLOUDFLARE_ACCOUNT_ID"), account);
    }
    return command;
}

namespace {

QString tailLogArgument(const QJsonValue &value)
{
    if (value.isString())
        return value.toString();
    if (value.isDouble())
        return QString::number(value.toDouble());
    if (value.isBool())
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    if (value.isNull())
        return QStringLiteral("null");
    if (value.isObject())
        return QString::fromUtf8(
            QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
    if (value.isArray())
        return QString::fromUtf8(
            QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
    return {};
}

QString elideTailField(QString value, int limit)
{
    value.replace(QLatin1Char('\n'), QLatin1Char(' '));
    value.replace(QLatin1Char('\r'), QLatin1Char(' '));
    value = value.simplified();
    if (value.size() <= limit)
        return value;
    return value.left(qMax(0, limit - 1)) + QString::fromUtf8("\xE2\x80\xA6");
}

void advanceJsonDepth(const QString &line, int *depth, bool *inString,
                      bool *escaped)
{
    for (const QChar ch : line) {
        if (*inString) {
            if (*escaped) {
                *escaped = false;
            } else if (ch == QLatin1Char('\\')) {
                *escaped = true;
            } else if (ch == QLatin1Char('"')) {
                *inString = false;
            }
            continue;
        }
        if (ch == QLatin1Char('"')) {
            *inString = true;
            *escaped = false;
        } else if (ch == QLatin1Char('{') || ch == QLatin1Char('[')) {
            ++*depth;
        } else if (ch == QLatin1Char('}') || ch == QLatin1Char(']')) {
            --*depth;
        }
    }
    *inString = false;
    *escaped = false;
}

QString prettyTailOutcome(const QString &outcome)
{
    if (outcome == QLatin1String("ok"))
        return QStringLiteral("Ok");
    if (outcome == QLatin1String("canceled"))
        return QStringLiteral("Canceled");
    if (outcome == QLatin1String("responseStreamDisconnected"))
        return QStringLiteral("Client Disconnected");
    if (outcome == QLatin1String("exceededCpu"))
        return QStringLiteral("Exceeded CPU Limit");
    if (outcome == QLatin1String("exceededMemory"))
        return QStringLiteral("Exceeded Memory Limit");
    if (outcome == QLatin1String("exception"))
        return QStringLiteral("Exception Thrown");
    if (outcome.isEmpty() || outcome == QLatin1String("unknown"))
        return QStringLiteral("Unknown");
    return outcome;
}

// An event Wrangler has no branch for still arrives with its own field names,
// and those describe it perfectly well. Flattened one level down, because the
// interesting field of such an event is usually inside a single nested object.
void flattenTailEventFields(const QJsonObject &object, const QString &prefix,
                            int depth, QStringList *fields)
{
    // Enough to identify the shape, not so many that the digest becomes the
    // log line. Anything richer belongs in the Worker's own console output.
    constexpr int kMaximumFields = 6;
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (fields->size() >= kMaximumFields)
            return;
        const QString name =
            prefix.isEmpty() ? it.key() : prefix + QLatin1Char('.') + it.key();
        if (it.value().isObject() && depth > 0) {
            const QJsonObject child = it.value().toObject();
            if (child.isEmpty()) {
                *fields << name;
                continue;
            }
            flattenTailEventFields(child, name, depth - 1, fields);
            continue;
        }
        const QString text = elideTailField(tailLogArgument(it.value()), 80);
        *fields << (text.isEmpty() ? name : name + QLatin1Char('=') + text);
    }
}

// The detail object of a hibernated WebSocket wake-up, when that is what this
// event is. Matched on what a key *says* rather than on one exact spelling:
// Wrangler carries no branch for this shape, so there is no published name to
// pin to, and a rename must degrade to the generic field digest above rather
// than to a confidently wrong label. A shape that spells the detail inline
// (a "webSocketEventType" straight on the event) reports the event itself.
bool findWebSocketTailDetail(const QJsonObject &event, QJsonObject *detail)
{
    for (auto it = event.constBegin(); it != event.constEnd(); ++it) {
        if (!it.key().contains(QLatin1String("websocket"), Qt::CaseInsensitive))
            continue;
        *detail = it.value().isObject() ? it.value().toObject() : event;
        return true;
    }
    return false;
}

// What woke a hibernated Durable Object WebSocket, said in words.
//
// The frame's bytes are not here to show and never will be: the trace carries
// no payload, and a ForkMesh room's traffic is end-to-end encrypted besides.
// What the trace does carry is metadata — a message versus a close versus an
// error, and a close's code and whether it was clean — and that is worth more
// in the log than Wrangler's "Unknown Event" (adhoc #1635).
QString describeWebSocketTailEvent(const QJsonObject &detail)
{
    QString type;
    QString reason;
    int code = 0;
    bool sawClean = false;
    bool wasClean = false;
    for (auto it = detail.constBegin(); it != detail.constEnd(); ++it) {
        const QString key = it.key();
        if (it.value().isString() &&
            key.contains(QLatin1String("type"), Qt::CaseInsensitive)) {
            type = it.value().toString().trimmed();
        } else if (it.value().isString() &&
                   key.compare(QLatin1String("reason"),
                               Qt::CaseInsensitive) == 0) {
            // Who hung up and why. The relay closes with a reason on every
            // deliberate hang-up it makes — "stale", "text frames only",
            // "message too large" — and the viewer was dropping exactly the
            // word that says which of its own rules ended the connection.
            reason = it.value().toString().trimmed();
        } else if (it.value().isDouble() &&
                   key.compare(QLatin1String("code"),
                               Qt::CaseInsensitive) == 0) {
            code = it.value().toInt();
        } else if (it.value().isBool() &&
                   key.contains(QLatin1String("clean"), Qt::CaseInsensitive)) {
            sawClean = true;
            wasClean = it.value().toBool();
        }
    }
    QString head = type.isEmpty()
                       ? QStringLiteral("WebSocket wake-up")
                       : QStringLiteral("WebSocket %1")
                             .arg(elideTailField(type, 40));
    QStringList parts;
    if (code > 0)
        parts << QStringLiteral("code %1").arg(code);
    if (sawClean)
        parts << (wasClean ? QStringLiteral("clean")
                           : QStringLiteral("unclean"));
    if (!reason.isEmpty())
        parts << QStringLiteral("reason \"%1\"")
                     .arg(elideTailField(reason, 120));
    if (!parts.isEmpty())
        head += QStringLiteral(" (%1)").arg(parts.join(QStringLiteral(", ")));
    return head;
}

// The last resort: an event of a shape nobody here has a name for, described
// from its own keys. Still better than "Unknown Event" — it says what arrived,
// and it keeps saying it if Cloudflare adds a shape tomorrow.
QString describeOtherTailEvent(const QJsonObject &eventBody)
{
    if (eventBody.isEmpty())
        return QStringLiteral("Event with no detail");
    QStringList fields;
    flattenTailEventFields(eventBody, QString(), 2, &fields);
    return fields.isEmpty()
               ? QStringLiteral("Event with no detail")
               : QStringLiteral("Event %1")
                     .arg(fields.join(QStringLiteral(", ")));
}

// A duration field in milliseconds, negative when the event carries none. Zero
// is a real answer — a hibernated wake-up that does nothing costs no CPU — so
// "absent" cannot be spelled 0.
double tailDuration(const QJsonValue &value)
{
    if (!value.isDouble())
        return -1;
    const double ms = value.toDouble();
    return ms >= 0 ? ms : -1;
}

// Milliseconds for the log line. Sub-millisecond CPU times are the common case
// on the relay, so they keep a decimal rather than collapsing to "0ms".
QString tailDurationText(double ms)
{
    if (ms < 0)
        return {};
    if (ms > 0 && ms < 10)
        return QStringLiteral("%1ms").arg(ms, 0, 'g', 2);
    return QStringLiteral("%1ms").arg(qRound64(ms));
}

// Which deployed version served the event. Wrangler sends the whole version
// record; the tag is what a human named it, the message is what the deploy
// said, and the id is the last resort — shortened, because a full UUID in
// every line is a wall of hex that says nothing at a glance.
QString tailVersionLabel(const QJsonObject &version)
{
    const QString tag = version.value(QStringLiteral("tag")).toString().trimmed();
    if (!tag.isEmpty())
        return tag;
    const QString message =
        version.value(QStringLiteral("message")).toString().trimmed();
    if (!message.isEmpty())
        return message;
    return version.value(QStringLiteral("id")).toString().trimmed().left(8);
}

// The request's Cloudflare properties. The trace has carried these both as a
// nested object and as a JSON string in `cfJson`, depending on how the event
// was serialised, so both are accepted rather than betting on one.
QJsonObject tailRequestCf(const QJsonObject &request)
{
    const QJsonValue cf = request.value(QStringLiteral("cf"));
    if (cf.isObject())
        return cf.toObject();
    const QString encoded =
        cf.isString() ? cf.toString()
                      : request.value(QStringLiteral("cfJson")).toString();
    if (encoded.trimmed().isEmpty())
        return {};
    const QJsonDocument document = QJsonDocument::fromJson(encoded.toUtf8());
    return document.isObject() ? document.object() : QJsonObject();
}

QString tailHeader(const QJsonObject &headers, const char *wanted)
{
    for (auto it = headers.constBegin(); it != headers.constEnd(); ++it) {
        if (it.key().compare(QLatin1String(wanted), Qt::CaseInsensitive) == 0)
            return it.value().toString().trimmed();
    }
    return {};
}

// Only infer an account where the route itself names the already-authorized
// owner.  A generic `account=` parameter elsewhere may be an opaque blind key,
// so treating it as a display name would be both misleading and needlessly
// revealing.
QString tailClientIdentity(const QString &requestUrl)
{
    const QUrl url(requestUrl);
    if (!url.isValid() ||
        url.path() != QLatin1String("/api/nodes/events")) {
        return {};
    }
    const QString owner = QUrlQuery(url).queryItemValue(
        QStringLiteral("owner"), QUrl::FullyDecoded).trimmed();
    static const QRegularExpression safeOwner(
        QStringLiteral("^[A-Za-z0-9](?:[A-Za-z0-9_-]{0,62})$"));
    return safeOwner.match(owner).hasMatch() ? owner : QString();
}

bool isDurableObjectCodeUpdate(const QString &text)
{
    return text.contains(
        QLatin1String("Durable Object reset because its code was updated"),
        Qt::CaseInsensitive);
}

// The runtime reporting that the instance a hibernated socket belonged to has
// already ended: "Connection closed: this Durable Object instance is no longer
// active. Reconnect or retry the request." A code update is one way to get
// here and carries the more specific text above; an eviction, an overload
// reset, or a rolling restart are others, and none of them name a Worker bug —
// the message is the runtime's own advice to the client, and reconnecting is
// exactly what every ForkMesh chat client does. Matched on the distinctive
// clause rather than the whole sentence, and deliberately narrow: a reset that
// says *why* it happened ("exceeded its memory limit") is a different message
// and must keep alerting.
bool isDurableObjectInstanceGone(const QString &text)
{
    return text.contains(
        QLatin1String("Durable Object instance is no longer active"),
        Qt::CaseInsensitive);
}

// Cloudflare failing underneath the Worker, told apart from the Worker
// failing. Both markers are the ones the relay itself watches for
// (_D1_TRANSIENT_MARKERS and log_durable_object_abort in app/src/entry.py),
// kept deliberately in step: the two ends must agree on what is a platform
// blip, or the desktop reports as an application bug the very fault the Worker
// already treats as one-off and retries. The message that comes with it says
// nothing a reader can act on, so the reference id — the only thing that
// identifies the incident to Cloudflare — is pulled out for the line.
bool tailPlatformFault(const QString &text, QString *reference)
{
    static const QRegularExpression referenced(
        QStringLiteral("internal error;\\s*reference\\s*=\\s*([A-Za-z0-9_-]+)"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = referenced.match(text);
    if (match.hasMatch()) {
        *reference = match.captured(1);
        return true;
    }
    return text.contains(QLatin1String("internal error"),
                         Qt::CaseInsensitive) ||
           text.contains(QLatin1String("Exceeded allowed duration"),
                         Qt::CaseInsensitive);
}

// An epoch-milliseconds field as a wall clock, empty when it holds nothing.
QString tailClock(const QJsonValue &value)
{
    const qint64 ms = static_cast<qint64>(value.toDouble());
    return ms > 0 ? QDateTime::fromMSecsSinceEpoch(ms).toString(
                        QStringLiteral("HH:mm:ss"))
                  : QString();
}

// Content a desktop node serves, not the Worker: repository browse, release
// blobs, and git clone/push. A 502/503/504 on one of these means the node
// holding that repository is offline, lagging, or not yet attested — in a P2P
// mesh that is an ordinary condition, and the Worker answers it deliberately
// (an "unavailable"/"mirror_unavailable" body marked
// x-forkmesh-expected-degraded) rather than failing. The Worker's own error
// log has skipped exactly these since they buried real bugs; the tail viewer
// was still counting every one of them as "Cloud: Worker error". A tail event
// carries no response headers, so the same rule is applied here from the path.
// Room paths are deliberately absent: a room 5xx is the Worker's own Durable
// Object failing and must stay visible.
bool tailPathIsNodeContent(const QString &requestUrl)
{
    const QString target = requestUrl.trimmed();
    if (target.isEmpty())
        return false;
    const QUrl url(target);
    if (!url.isValid())
        return false;
    const QString path = url.path(QUrl::FullyEncoded);
    if (path.isEmpty())
        return false;
    // Kept in step with REPO_HOST_RE / RELEASE_BLOB_RE / GIT_INFO_RE /
    // GIT_PACK_RE / GIT_RECEIVE_RE in the Worker's urls.py.
    static const QRegularExpression repoAction(
        QStringLiteral("^/api/repo/[^/]+/[^/]+/(?:host|tree|blobs|blob|raw|"
                       "history|commit|compare|merge-preview|branches|search|"
                       "stats|sizes)$"));
    static const QRegularExpression releaseBlob(
        QStringLiteral("^/api/repo/[^/]+/[^/]+/releases/blob/sha256/"
                       "[0-9a-f]{64}$"));
    static const QRegularExpression gitTransport(
        QStringLiteral("^/[^/]+/[^/]+/(?:info/refs|git-upload-pack|"
                       "git-receive-pack)$"));
    return repoAction.match(path).hasMatch() ||
           releaseBlob.match(path).hasMatch() ||
           gitTransport.match(path).hasMatch();
}

// A stream that never closes a brace must not grow into the heap.
constexpr qsizetype kMaximumTailBufferBytes = 1024 * 1024;

}  // namespace

QStringList takeCloudflareTailRecords(QByteArray *buffer)
{
    QStringList records;
    if (!buffer || buffer->isEmpty())
        return records;

    qsizetype cursor = 0;      // next byte to scan
    qsizetype consumed = 0;    // everything before this has become a record
    qsizetype recordStart = -1; // where the open JSON document began
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (qsizetype newline = buffer->indexOf('\n', cursor); newline >= 0;
         newline = buffer->indexOf('\n', cursor)) {
        const qsizetype lineStart = cursor;
        const QString line =
            QString::fromUtf8(buffer->mid(lineStart, newline - lineStart));
        cursor = newline + 1;
        if (recordStart < 0) {
            const QString trimmed = line.trimmed();
            if (trimmed.isEmpty()) {
                consumed = cursor;
                continue;
            }
            if (!trimmed.startsWith(QLatin1Char('{'))) {
                records << trimmed;
                consumed = cursor;
                continue;
            }
            recordStart = lineStart;
            depth = 0;
            inString = false;
            escaped = false;
        } else if (line.startsWith(QLatin1Char('{'))) {
            records << QString::fromUtf8(
                              buffer->mid(recordStart, lineStart - recordStart))
                           .trimmed();
            recordStart = lineStart;
            depth = 0;
            inString = false;
            escaped = false;
        }
        advanceJsonDepth(line, &depth, &inString, &escaped);
        if (depth > 0)
            continue;
        records << QString::fromUtf8(
                          buffer->mid(recordStart, newline - recordStart))
                       .trimmed();
        recordStart = -1;
        consumed = cursor;
    }
    buffer->remove(0, consumed);
    if (buffer->size() > kMaximumTailBufferBytes)
        buffer->clear();
    return records;
}

namespace {

// What the record's exceptions and console logs say about how the invocation
// ended, for the classification in parseCloudflareTailLine.
struct TailMessageScan {
    bool errorLevelLog = false;
    bool expectedDegradedResponse = false;
    bool sawDeployReset = false;
    bool sawInstanceGone = false;
    bool sawOtherException = false;
};

// Folds the exceptions, console logs and Cloudflare diagnostics notes into
// event.messages (D1 statement logs into event.d1Statements), in that order.
TailMessageScan collectTailMessages(const QJsonObject &root,
                                    const QJsonArray &exceptions,
                                    const QJsonArray &logs,
                                    CloudflareTailEvent &event)
{
    bool errorLevelLog = false;
    bool expectedDegradedResponse = false;
    bool sawDeployReset = false;
    bool sawInstanceGone = false;
    bool sawOtherException = false;
    for (const QJsonValue &value : exceptions) {
        const QJsonObject exception = value.toObject();
        const QString name =
            exception.value(QStringLiteral("name")).toString().trimmed();
        const QString message =
            exception.value(QStringLiteral("message")).toString().trimmed();
        const QString text = name.isEmpty()
                                 ? message
                                 : (message.isEmpty()
                                        ? name
                                        : name + QStringLiteral(": ") + message);
        if (!text.isEmpty()) {
            sawDeployReset = sawDeployReset || isDurableObjectCodeUpdate(text);
            sawInstanceGone =
                sawInstanceGone || isDurableObjectInstanceGone(text);
            sawOtherException = sawOtherException ||
                                (!isDurableObjectCodeUpdate(text) &&
                                 !isDurableObjectInstanceGone(text));
            if (!event.platformFault) {
                QString reference;
                if (tailPlatformFault(text, &reference)) {
                    event.platformFault = true;
                    event.platformFaultReference = reference;
                }
            }
            if (!event.messages.contains(text))
                event.messages << text;
        }
    }
    for (const QJsonValue &value : logs) {
        const QJsonObject entry = value.toObject();
        const QString level =
            entry.value(QStringLiteral("level")).toString().trimmed().toLower();
        if (level == QLatin1String("error") || level == QLatin1String("fatal"))
            errorLevelLog = true;
        QStringList parts;
        const QJsonValue message = entry.value(QStringLiteral("message"));
        if (message.isArray()) {
            for (const QJsonValue &argument : message.toArray()) {
                const QString text = tailLogArgument(argument);
                if (!text.isEmpty())
                    parts << text;
            }
        } else {
            const QString text = tailLogArgument(message);
            if (!text.isEmpty())
                parts << text;
        }
        if (parts.isEmpty())
            continue;
        const QString joined = parts.join(QLatin1Char(' '));
        static const QRegularExpression d1StatementRe(
            QStringLiteral("^D1 [a-z0-9 ]+: "));
        if ((level.isEmpty() || level == QLatin1String("log")) &&
            d1StatementRe.match(joined).hasMatch()) {
            event.d1Statements << joined;
            continue;
        }
        const bool expectedDegradedMarker =
            joined == QLatin1String("forkmesh_expected_degraded_response");
        expectedDegradedResponse =
            expectedDegradedResponse || expectedDegradedMarker;
        const QString renderedMessage = expectedDegradedMarker
            ? QStringLiteral("expected backpressure: dependency temporarily "
                             "unavailable")
            : (level.isEmpty() || level == QLatin1String("log")
                   ? joined
                   : level + QStringLiteral(": ") + joined);
        if (!event.messages.contains(renderedMessage))
            event.messages << renderedMessage;
    }
    // Cloudflare's own notes on the invocation. No Worker code writes these,
    // which is exactly why they can survive a fault that produced no console
    // output at all — and on such an event they are the only description of
    // what happened. The viewer was decoding the record and throwing them away.
    const QJsonArray diagnostics =
        root.value(QStringLiteral("diagnosticsChannelEvents")).toArray();
    for (const QJsonValue &value : diagnostics) {
        const QJsonObject entry = value.toObject();
        const QString channel =
            entry.value(QStringLiteral("channel")).toString().trimmed();
        const QString note =
            tailLogArgument(entry.value(QStringLiteral("message"))).trimmed();
        const QString rendered =
            channel.isEmpty()
                ? note
                : (note.isEmpty() ? channel
                                  : channel + QStringLiteral(": ") + note);
        if (rendered.isEmpty() || event.messages.contains(rendered))
            continue;
        event.messages << rendered;
    }
    return {errorLevelLog, expectedDegradedResponse, sawDeployReset,
            sawInstanceGone, sawOtherException};
}

// Wrangler recognises eight event shapes and labels everything else
// "Unknown Event". A Durable Object's hibernated WebSocket wake-up is one
// of the ones it has no branch for — and on the relay that is most of the
// traffic, so the cloud log filled up with "Unknown Event - Ok" lines that
// said nothing at all (adhoc #1635). Its shapes are matched here the way it
// matches them, by which key the event carries; the rest are described from
// the event's own fields rather than thrown away.
// Sets event.kind and returns the line's head, before the " @ time" stamp.
QString tailEventHead(const QJsonObject &eventBody, const QString &verdict,
                      CloudflareTailEvent &event)
{
    const QJsonValue cron = eventBody.value(QStringLiteral("cron"));
    QJsonObject webSocketDetail;
    QString head;
    if (cron.isString()) {
        event.kind = QStringLiteral("scheduled");
        head = QStringLiteral("\"%1\" - %2")
                   .arg(elideTailField(cron.toString(), 40), verdict);
    } else if (eventBody.contains(QStringLiteral("request"))) {
        event.kind = QStringLiteral("fetch");
        head = QStringLiteral("%1 %2 - %3")
                   .arg(event.method.isEmpty() ? QStringLiteral("(no method)")
                                               : event.method.toUpper(),
                        event.url.isEmpty()
                            ? QStringLiteral("(no url)")
                            : elideTailField(event.url, 160),
                        verdict);
    } else if (eventBody.contains(QStringLiteral("mailFrom"))) {
        event.kind = QStringLiteral("email");
        // Filled in one call rather than chained: an address is attacker-chosen
        // text, and a "%3" inside it would be eaten by the next .arg() in a
        // chain. Every head below is built the same way for the same reason.
        head = QStringLiteral("Email from %1 to %2 (%3 bytes) - %4")
                   .arg(elideTailField(
                            eventBody.value(QStringLiteral("mailFrom"))
                                .toString(),
                            80),
                        elideTailField(
                            eventBody.value(QStringLiteral("rcptTo")).toString(),
                            80),
                        QString::number(
                            eventBody.value(QStringLiteral("rawSize")).toInt()),
                        verdict);
    } else if (eventBody.contains(QStringLiteral("queue"))) {
        event.kind = QStringLiteral("queue");
        const int batch = eventBody.value(QStringLiteral("batchSize")).toInt();
        head = QStringLiteral("Queue %1 (%2 message%3) - %4")
                   .arg(elideTailField(
                            eventBody.value(QStringLiteral("queue")).toString(),
                            60),
                        QString::number(batch),
                        batch == 1 ? QString() : QStringLiteral("s"), verdict);
    } else if (eventBody.contains(QStringLiteral("rpcMethod"))) {
        event.kind = QStringLiteral("rpc");
        head = QStringLiteral("%1.%2() - %3")
                   .arg(event.entrypoint.isEmpty()
                            ? QStringLiteral("(worker)")
                            : elideTailField(event.entrypoint, 60),
                        elideTailField(
                            eventBody.value(QStringLiteral("rpcMethod"))
                                .toString(),
                            80),
                        verdict);
    } else if (eventBody.contains(QStringLiteral("scheduledTime"))) {
        // Wrangler's own test for an alarm: a scheduled time with no cron
        // pattern beside it, which the branch above has already ruled out.
        event.kind = QStringLiteral("alarm");
        const QString due =
            tailClock(eventBody.value(QStringLiteral("scheduledTime")));
        head = due.isEmpty()
                   ? QStringLiteral("Alarm - %1").arg(verdict)
                   : QStringLiteral("Alarm due %1 - %2").arg(due, verdict);
    } else if (eventBody.contains(QStringLiteral("consumedEvents"))) {
        event.kind = QStringLiteral("tail");
        QStringList tailed;
        const QJsonArray consumed =
            eventBody.value(QStringLiteral("consumedEvents")).toArray();
        for (const QJsonValue &value : consumed) {
            const QString name = value.toObject()
                                     .value(QStringLiteral("scriptName"))
                                     .toString()
                                     .trimmed();
            if (!name.isEmpty() && !tailed.contains(name))
                tailed << name;
        }
        head = QStringLiteral("Tailing %1 - %2")
                   .arg(tailed.isEmpty()
                            ? QStringLiteral("(nothing named)")
                            : elideTailField(tailed.join(QStringLiteral(", ")),
                                             120),
                        verdict);
    } else if (eventBody.contains(QStringLiteral("type")) &&
               eventBody.contains(QStringLiteral("message"))) {
        // Not the Worker running at all: Cloudflare telling the tail itself
        // that it is shedding events. Worth reading, but it is not a failure
        // of anything the alert path watches.
        event.kind = QStringLiteral("tail-info");
        head = QStringLiteral("Tail %1: %2")
                   .arg(elideTailField(
                            eventBody.value(QStringLiteral("type")).toString(),
                            40),
                        elideTailField(
                            eventBody.value(QStringLiteral("message"))
                                .toString(),
                            200));
    } else if (findWebSocketTailDetail(eventBody, &webSocketDetail)) {
        event.kind = QStringLiteral("websocket");
        head = QStringLiteral("%1 - %2")
                   .arg(describeWebSocketTailEvent(webSocketDetail), verdict);
    } else {
        event.kind = QStringLiteral("other");
        head = QStringLiteral("%1 - %2")
                   .arg(describeOtherTailEvent(eventBody), verdict);
    }
    return head;
}

}  // namespace

CloudflareTailEvent parseCloudflareTailLine(const QString &line)
{
    CloudflareTailEvent event;
    const QString trimmed = line.trimmed();
    event.summary = elideTailField(trimmed, 1000);
    if (trimmed.isEmpty() || !trimmed.startsWith(QLatin1Char('{')))
        return event;
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(trimmed.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return event;
    const QJsonObject root = document.object();
    if (!root.contains(QStringLiteral("outcome")))
        return event;
    event.parsed = true;

    event.outcome = root.value(QStringLiteral("outcome")).toString().trimmed();
    // Which Worker, and which class inside it. Both sit beside the event rather
    // than in it, and for the shapes that carry no URL they are the only thing
    // that places the line.
    event.scriptName =
        root.value(QStringLiteral("scriptName")).toString().trimmed();
    event.entrypoint =
        root.value(QStringLiteral("entrypoint")).toString().trimmed();
    // What it cost, which build ran it, and whether Cloudflare threw part of
    // the record away before sending it (#1665).
    event.cpuTimeMs = tailDuration(root.value(QStringLiteral("cpuTime")));
    event.wallTimeMs = tailDuration(root.value(QStringLiteral("wallTime")));
    event.scriptVersion = tailVersionLabel(
        root.value(QStringLiteral("scriptVersion")).toObject());
    event.durableObjectId =
        root.value(QStringLiteral("durableObjectId")).toString().trimmed();
    event.requestId =
        root.value(QStringLiteral("requestId")).toString().trimmed();
    event.traceId = root.value(QStringLiteral("traceId")).toString().trimmed();
    event.truncated = root.value(QStringLiteral("truncated")).toBool();
    const QJsonObject eventBody =
        root.value(QStringLiteral("event")).toObject();
    const QJsonObject request =
        eventBody.value(QStringLiteral("request")).toObject();
    event.method = request.value(QStringLiteral("method")).toString().trimmed();
    event.url = request.value(QStringLiteral("url")).toString().trimmed();
    const QJsonObject headers =
        request.value(QStringLiteral("headers")).toObject();
    event.userAgent = tailHeader(headers, "user-agent");
    event.clientAddress = tailHeader(headers, "cf-connecting-ip");
    event.requestOrigin = tailHeader(headers, "origin");
    event.clientIdentity = tailClientIdentity(event.url);
    // Where the hit landed and where it came from. Only a request carries this,
    // and Wrangler drops it the same way it drops the agent.
    const QJsonObject requestCf = tailRequestCf(request);
    event.colo = requestCf.value(QStringLiteral("colo")).toString().trimmed();
    event.country =
        requestCf.value(QStringLiteral("country")).toString().trimmed();
    event.city = requestCf.value(QStringLiteral("city")).toString().trimmed();
    event.region =
        requestCf.value(QStringLiteral("region")).toString().trimmed();
    event.asn = QString::number(
        requestCf.value(QStringLiteral("asn")).toInteger());
    if (event.asn == QLatin1String("0"))
        event.asn.clear();
    event.asOrganization =
        requestCf.value(QStringLiteral("asOrganization")).toString().trimmed();
    event.httpProtocol =
        requestCf.value(QStringLiteral("httpProtocol")).toString().trimmed();
    event.tlsVersion =
        requestCf.value(QStringLiteral("tlsVersion")).toString().trimmed();
    if (requestCf.value(QStringLiteral("clientTcpRtt")).isDouble())
        event.clientTcpRttMs = qMax(
            0, requestCf.value(QStringLiteral("clientTcpRtt")).toInt());
    const QJsonValue response = eventBody.value(QStringLiteral("response"));
    if (response.isObject()) {
        event.status =
            response.toObject().value(QStringLiteral("status")).toInt();
    }

    const QJsonArray exceptions =
        root.value(QStringLiteral("exceptions")).toArray();
    const QJsonArray logs = root.value(QStringLiteral("logs")).toArray();
    const TailMessageScan scan =
        collectTailMessages(root, exceptions, logs, event);
    const bool errorLevelLog = scan.errorLevelLog;
    const bool expectedDegradedResponse = scan.expectedDegradedResponse;
    const bool sawDeployReset = scan.sawDeployReset;
    const bool sawInstanceGone = scan.sawInstanceGone;
    const bool sawOtherException = scan.sawOtherException;

    // "responseStreamDisconnected" is the other half of "canceled": the client
    // went away before the response stream ended. Every WebSocket that closes
    // reports it — the upgrade request's stream only ends when the socket does
    // — so on the relay it is the ordinary end of a chat connection, not a
    // failure of anything (adhoc #1658). An exception, an error-level log or a
    // 5xx below still flags the ones that really did break mid-stream.
    const bool badOutcome =
        !event.outcome.isEmpty() && event.outcome != QLatin1String("ok") &&
        event.outcome != QLatin1String("unknown") &&
        event.outcome != QLatin1String("canceled") &&
        event.outcome != QLatin1String("responseStreamDisconnected");
    // An offline node's repository content being re-requested answers 503 by
    // design (adhoc #1714: a mobile visitor's repo-card logo.png fanned out
    // over every mirror-owned catalog row). Only the status-based branch is
    // defused — an exception, an error-level log or a bad outcome on the same
    // path is still the Worker breaking.
    const bool offlineNodeContent = event.status >= 502 && event.status <= 504 &&
                                    tailPathIsNodeContent(event.url);
    // During a rollout Wrangler can emit an exception-shaped companion record
    // for the same 1006 close without including the code-update exception at
    // all. With no exception, error log, or 5xx there is no Worker fault to
    // diagnose; class it like the ordinary unclean close record. Any concrete
    // exception on that socket still follows the error path below.
    const QJsonObject webSocketWake =
        eventBody.value(QStringLiteral("getWebSocketEvent")).toObject();
    const bool webSocketCloseWake =
        webSocketWake.value(QStringLiteral("webSocketEventType")).toString()
        == QLatin1String("close");
    const bool bareUncleanSocketClose =
        webSocketCloseWake &&
        webSocketWake.value(QStringLiteral("code")).toInt() == 1006 &&
        exceptions.isEmpty() && !errorLevelLog && event.status < 500;
    // The same close, with the runtime saying in words why it had nothing to
    // deliver it to: the object instance was already gone. That is the end of
    // a connection, not the failure of one — the handler never ran, so there
    // is no Worker code to go and fix, and the client reconnects. Scoped to
    // the close wake-up: the identical text on a fetch means a request the
    // Worker was serving did fail, and that still alerts.
    const bool endedInstanceSocketClose =
        webSocketCloseWake && sawInstanceGone && !sawOtherException &&
        !errorLevelLog && event.status < 500;
    event.isDeployInterruption =
        sawDeployReset && !sawOtherException && !errorLevelLog &&
        event.status < 500;
    event.isError = !event.isDeployInterruption &&
                    !bareUncleanSocketClose && !endedInstanceSocketClose &&
                    (badOutcome || !exceptions.isEmpty() || errorLevelLog ||
                     (event.status >= 500 && !offlineNodeContent &&
                      !expectedDegradedResponse));
    // A fault that spent no measurable CPU and logged nothing did not happen
    // inside a handler: the invocation never got that far. On a Python Worker
    // that points at module start-up — the 10021 boundary entry.py's lazy
    // splits exist to stay under — or at the platform beneath it, and saying
    // so keeps a reader from combing handler code for a bug that is not in it.
    // Cloudflare reports cpuTime in whole milliseconds, so "0" is the floor
    // rather than a real zero; the claim is deliberately only about what the
    // numbers rule out.
    const bool faultedBeforeHandler = event.isError && !exceptions.isEmpty() &&
                                      logs.isEmpty() &&
                                      event.cpuTimeMs >= 0 &&
                                      event.cpuTimeMs < 1;

    // Rendered as Wrangler's own default line — "GET https://… - 200 Ok @ time"
    // — with the one thing its pretty printer throws away appended: the agent
    // behind the hit (adhoc #1623).
    const QString stamp =
        tailClock(root.value(QStringLiteral("eventTimestamp")));
    const QString outcome = prettyTailOutcome(event.outcome);
    const QString verdict = event.status > 0
                                ? QStringLiteral("%1 %2")
                                      .arg(event.status)
                                      .arg(outcome)
                                : outcome;
    QString head = tailEventHead(eventBody, verdict, event);
    if (!stamp.isEmpty())
        head += QStringLiteral(" @ ") + stamp;

    QStringList fields{head};
    // The agent string is the reason this viewer decodes the JSON at all, so a
    // request line never drops it — but only a request has one, and "UA (none)"
    // hung off an alarm or a WebSocket wake-up is noise pretending to be data.
    if (event.kind == QLatin1String("fetch")) {
        fields << (event.userAgent.isEmpty()
                       ? QStringLiteral("UA (none)")
                       : QStringLiteral("UA %1")
                             .arg(elideTailField(event.userAgent, 160)));
    } else if (!event.scriptName.isEmpty()) {
        // A request names its own host in the URL. Nothing else does, so for
        // the rest the Worker is the only thing in the line that says where
        // this happened.
        fields << elideTailField(event.scriptName, 60);
    }
    // Which class inside that Worker — for the WebSocket wake-ups, the Durable
    // Object that holds the connection. The RPC head already spells it out.
    if (!event.entrypoint.isEmpty() &&
        event.entrypoint != QLatin1String("default") &&
        event.kind != QLatin1String("rpc"))
        fields << elideTailField(event.entrypoint, 60);
    // The colo that answered, and the country the client dialled from.
    if (!event.colo.isEmpty() || !event.country.isEmpty()) {
        fields << (event.colo.isEmpty()
                       ? QStringLiteral("from %1")
                             .arg(elideTailField(event.country, 20))
                       : (event.country.isEmpty()
                              ? QStringLiteral("via %1")
                                    .arg(elideTailField(event.colo, 20))
                              : QStringLiteral("via %1 from %2")
                                    .arg(elideTailField(event.colo, 20),
                                         elideTailField(event.country, 20))));
    }
    QStringList clientPlace;
    if (!event.city.isEmpty())
        clientPlace << elideTailField(event.city, 40);
    if (!event.region.isEmpty() && event.region != event.city)
        clientPlace << elideTailField(event.region, 40);
    if (!clientPlace.isEmpty())
        fields << QStringLiteral("near %1").arg(clientPlace.join(", "));
    if (!event.clientIdentity.isEmpty())
        fields << QStringLiteral("owner %1")
                      .arg(elideTailField(event.clientIdentity, 64));
    if (!event.clientAddress.isEmpty())
        fields << QStringLiteral("client IP %1")
                      .arg(elideTailField(event.clientAddress, 64));
    if (!event.asn.isEmpty() || !event.asOrganization.isEmpty()) {
        const QString network =
            (event.asn.isEmpty() ? QString()
                                 : QStringLiteral("AS") + event.asn) +
            (!event.asn.isEmpty() && !event.asOrganization.isEmpty()
                 ? QStringLiteral(" ")
                 : QString()) +
            elideTailField(event.asOrganization, 80);
        fields << QStringLiteral("network %1").arg(network);
    }
    QStringList transport;
    if (!event.httpProtocol.isEmpty())
        transport << elideTailField(event.httpProtocol, 20);
    if (!event.tlsVersion.isEmpty())
        transport << elideTailField(event.tlsVersion, 20);
    if (event.clientTcpRttMs >= 0)
        transport << QStringLiteral("RTT %1ms").arg(event.clientTcpRttMs);
    if (!transport.isEmpty())
        fields << transport.join(QLatin1Char(' '));
    if (!event.scriptVersion.isEmpty()) {
        fields << QStringLiteral("version %1")
                      .arg(elideTailField(event.scriptVersion, 40));
    }
    if (!event.durableObjectId.isEmpty() &&
        (event.kind == QLatin1String("websocket") || event.isError ||
         event.isDeployInterruption)) {
        // Two faults minutes apart are either the same object failing twice or
        // two objects failing once, and those want opposite investigations — a
        // 16-character prefix is enough to read a healthy line by but not
        // enough to tell those apart, so a fault prints the whole id.
        fields << QStringLiteral("object %1")
                      .arg(elideTailField(
                          event.durableObjectId,
                          event.isError || event.isDeployInterruption ? 64
                                                                      : 16));
    }
    const QString correlation = !event.traceId.isEmpty()
                                    ? event.traceId
                                    : event.requestId;
    if (!correlation.isEmpty() &&
        (event.isError || event.isDeployInterruption)) {
        fields << QStringLiteral("trace %1")
                      .arg(elideTailField(correlation, 20));
    }
    // What the event cost. One line's timing means little; a column of them is
    // how a cold start is told apart from a Worker that is genuinely busy.
    const QString cpu = tailDurationText(event.cpuTimeMs);
    const QString wall = tailDurationText(event.wallTimeMs);
    if (!cpu.isEmpty() && !wall.isEmpty())
        fields << QStringLiteral("cpu %1, wall %2").arg(cpu, wall);
    else if (!cpu.isEmpty())
        fields << QStringLiteral("cpu %1").arg(cpu);
    else if (!wall.isEmpty())
        fields << QStringLiteral("wall %1").arg(wall);
    // Cloudflare sheds logs from an event that produced too many. Say so, so a
    // missing console line reads as dropped rather than as never written.
    if (event.truncated)
        fields << QStringLiteral("logs truncated");
    if (event.isDeployInterruption) {
        fields << QStringLiteral(
            "expected code-update handoff; connected clients will reconnect");
    } else {
        for (const QString &message : std::as_const(event.messages))
            fields << elideTailField(message, 400);
        // The exception text above is Cloudflare's, and on its own it reads
        // like a Worker bug with the traceback missing. Say that no traceback
        // was ever written, and carry the one identifier that can be acted on.
        if (event.platformFault) {
            fields << (event.platformFaultReference.isEmpty()
                           ? QStringLiteral(
                                 "Cloudflare-side fault, not Worker code: no "
                                 "traceback exists and no handler could have "
                                 "caught it")
                           : QStringLiteral(
                                 "Cloudflare-side fault, not Worker code: no "
                                 "traceback exists; quote reference %1 to "
                                 "Cloudflare support")
                                 .arg(elideTailField(
                                     event.platformFaultReference, 64)));
        }
        // The exception text above is the runtime declining to wake an object
        // that no longer exists. Left unexplained it reads as a chat relay
        // that dropped a connection on the floor, and the "faulted before
        // handler code ran" note below would have sent a reader combing
        // Worker start-up for a bug that was never there.
        if (endedInstanceSocketClose) {
            fields << QStringLiteral(
                "expected instance handoff: the Durable Object that owned this "
                "socket had already ended, and the client reconnects");
        }
        if (faultedBeforeHandler) {
            fields << QStringLiteral(
                "no CPU and no console output: the isolate faulted before "
                "handler code ran, so look at Worker start-up rather than the "
                "handler");
        }
    }
    // Nothing above named the caller, and on a wake-up nothing can: the
    // hibernation event carries no request, so there is no URL, agent, or
    // client to go and look at. Said on faults of either kind, so that a
    // missing field reads as absent by construction rather than as dropped.
    if ((event.isError || event.isDeployInterruption) &&
        event.kind == QLatin1String("websocket")) {
        fields << QStringLiteral(
            "client identity unavailable: WebSocket wake-up has no request metadata");
    }
    event.summary = fields.join(QString::fromUtf8(" \xC2\xB7 "));
    return event;
}

D1TailStatement parseD1TailStatement(const QString &line)
{
    D1TailStatement statement;
    static const QRegularExpression head(
        QStringLiteral("^D1 ([a-z0-9 ]+): "));
    const QRegularExpressionMatch headMatch = head.match(line);
    if (!headMatch.hasMatch()) {
        statement.sql = line.trimmed();
        return statement;
    }
    statement.kind = headMatch.captured(1);
    statement.sql = line.mid(headMatch.capturedEnd()).trimmed();
    // Anchored at the end and limited to module and identifier characters, so
    // SQL that merely contains "(from" is left whole.
    static const QRegularExpression origin(QStringLiteral(
        " \\(from ([A-Za-z0-9_.-]+\\.py)(?: in ([A-Za-z0-9_<>.]+))?\\)$"));
    const QRegularExpressionMatch originMatch = origin.match(statement.sql);
    if (originMatch.hasMatch()) {
        statement.originFile = originMatch.captured(1);
        statement.originFunction = originMatch.captured(2);
        statement.sql.truncate(originMatch.capturedStart());
    }
    statement.caller = takeD1StatementCaller(&statement.sql);
    return statement;
}

QString takeD1StatementCaller(QString *sql)
{
    static const QRegularExpression caller(
        QStringLiteral(" \\(for ([^()]+)\\)$"));
    const QRegularExpressionMatch match = caller.match(*sql);
    if (!match.hasMatch())
        return {};
    const QString text = match.captured(1).trimmed();
    sql->truncate(match.capturedStart());
    return text;
}

QString cloudflareTailRequester(const CloudflareTailEvent &event)
{
    QStringList fields;
    if (event.kind == QLatin1String("fetch")) {
        const QUrl url(event.url);
        const QString path = url.isValid() && !url.path().isEmpty()
            ? url.path()
            : event.url;
        fields << QStringLiteral("%1 %2").arg(
            event.method.isEmpty() ? QStringLiteral("(no method)")
                                   : event.method.toUpper(),
            path.isEmpty() ? QStringLiteral("(no url)")
                           : elideTailField(path, 120));
        fields << (event.userAgent.isEmpty()
                       ? QStringLiteral("UA (none)")
                       : QStringLiteral("UA %1")
                             .arg(elideTailField(event.userAgent, 160)));
        if (!event.clientAddress.isEmpty())
            fields << QStringLiteral("client IP %1")
                          .arg(elideTailField(event.clientAddress, 64));
        if (!event.requestOrigin.isEmpty())
            fields << QStringLiteral("origin %1")
                          .arg(elideTailField(event.requestOrigin, 80));
        if (!event.clientIdentity.isEmpty())
            fields << QStringLiteral("owner %1")
                          .arg(elideTailField(event.clientIdentity, 64));
        if (!event.country.isEmpty())
            fields << QStringLiteral("from %1")
                          .arg(elideTailField(event.country, 20));
        if (!event.asOrganization.isEmpty())
            fields << QStringLiteral("network %1")
                          .arg(elideTailField(event.asOrganization, 80));
        return fields.join(QString::fromUtf8(" \xC2\xB7 "));
    }
    if (!event.kind.isEmpty())
        fields << event.kind;
    if (!event.entrypoint.isEmpty() &&
        event.entrypoint != QLatin1String("default"))
        fields << elideTailField(event.entrypoint, 60);
    else if (!event.scriptName.isEmpty())
        fields << elideTailField(event.scriptName, 60);
    return fields.join(QString::fromUtf8(" \xC2\xB7 "));
}

namespace {

constexpr int kCloudTailRestartBackoffMs[] = {5'000, 15'000, 60'000, 300'000};
constexpr int kCloudTailRestartBackoffCount =
    static_cast<int>(sizeof(kCloudTailRestartBackoffMs) /
                     sizeof(kCloudTailRestartBackoffMs[0]));
constexpr int kCloudTailResumeDelayMs = 2'000;

}  // namespace

CloudTailRestartPlan planCloudflareTailRestart(int consecutiveFailures,
                                               qint64 uptimeMs)
{
    CloudTailRestartPlan plan;
    if (uptimeMs >= kCloudTailHealthyUptimeMs) {
        plan.restart = true;
        plan.delayMs = kCloudTailResumeDelayMs;
        return plan;
    }
    const int attempts = qMax(0, consecutiveFailures);
    if (attempts >= kCloudTailRestartBackoffCount) {
        plan.giveUp = true;
        return plan;
    }
    plan.restart = true;
    plan.delayMs = kCloudTailRestartBackoffMs[attempts];
    return plan;
}

bool cloudflareTailWorkerMissing(const QString &line)
{
    return line.contains(QStringLiteral("[code: 10007]")) ||
           line.contains(
               QStringLiteral("This Worker does not exist on your account"),
               Qt::CaseInsensitive);
}

QString cloudflareWorkerName(const QString &workerDirectory)
{
    if (workerDirectory.isEmpty())
        return {};
    QFile file(QDir(workerDirectory)
                   .absoluteFilePath(QStringLiteral("wrangler.toml")));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    static const QRegularExpression namePattern(
        QStringLiteral("^name\\s*=\\s*\"([^\"]+)\"\\s*(#.*)?$"));
    for (int lines = 0; !file.atEnd() && lines < 400; ++lines) {
        const QString line = QString::fromUtf8(file.readLine(4096)).trimmed();
        if (line.startsWith(QLatin1Char('[')))
            break;
        const QRegularExpressionMatch match = namePattern.match(line);
        if (match.hasMatch())
            return match.captured(1).trimmed();
    }
    return {};
}

namespace {

// One row per (Worker, class, object instance). Distinct instances are the
// point of the tally — "which room is hot" is not answerable from the
// per-namespace analytics — but a relay serving thousands of rooms must not
// grow a row for each, so past the cap everything lands in one bucket that
// still reports its own totals rather than silently vanishing.
constexpr int kCloudUsageMaxEntries = 240;
constexpr char kCloudUsageOverflowKey[] = "\x1f\x1foverflow";

QString cloudUsageKey(const CloudflareTailEvent &event)
{
    return event.scriptName + QChar(0x1f) + event.entrypoint + QChar(0x1f) +
           event.durableObjectId;
}

// QLocale() rather than QLocale::system(): the reader's grouping and decimal
// separators either way, but a test can pin one with QLocale::setDefault.
QString usageCount(qint64 value)
{
    return QLocale().toString(value);
}

QString usageNumber(double value, int decimals)
{
    return QLocale().toString(value, 'f', decimals);
}

// Durations are read side by side, so they share one scale rule rather than
// each site picking its own: sub-second in ms, beyond that in seconds.
QString usageDuration(double milliseconds)
{
    if (milliseconds <= 0.0)
        return QStringLiteral("—");
    if (milliseconds < 1000.0)
        return usageNumber(milliseconds, milliseconds < 10.0 ? 2 : 1) +
               QStringLiteral(" ms");
    return usageNumber(milliseconds / 1000.0, 1) + QStringLiteral(" s");
}

// Cloudflare publishes D1 storage ceilings in decimal MB/GB. Keep the
// displayed measurement on that scale so a 500 MB limit does not mysteriously
// turn into 476.8 MB beside its progress bar.
QString usageBytes(qint64 bytes)
{
    if (bytes < 0)
        return QStringLiteral("—");
    if (bytes < 1000)
        return QStringLiteral("%1 B").arg(usageCount(bytes));
    if (bytes < 1'000'000)
        return usageNumber(bytes / 1000.0, 1) + QStringLiteral(" KB");
    if (bytes < 1'000'000'000)
        return usageNumber(bytes / 1'000'000.0, 1) + QStringLiteral(" MB");
    return usageNumber(bytes / 1'000'000'000.0, 2) + QStringLiteral(" GB");
}

QString usageAgo(qint64 thenMs, qint64 nowMs)
{
    const qint64 delta = nowMs - thenMs;
    if (thenMs <= 0 || delta < 0)
        return QStringLiteral("—");
    if (delta < 1000)
        return QStringLiteral("now");
    if (delta < 60'000)
        return QStringLiteral("%1s ago").arg(delta / 1000);
    if (delta < 3'600'000)
        return QStringLiteral("%1m ago").arg(delta / 60'000);
    if (delta < 86'400'000)
        return QStringLiteral("%1h ago").arg(delta / 3'600'000);
    return QStringLiteral("%1d ago").arg(delta / 86'400'000);
}

// A Durable Object id is 64 hex characters and never read in full; the head
// is enough to tell two instances apart and to grep the tail for one.
QString usageShortId(const QString &id)
{
    const QString trimmed = id.trimmed();
    if (trimmed.size() <= 12)
        return trimmed;
    return trimmed.left(12) + QString::fromUtf8("\xE2\x80\xA6");
}

struct UsagePalette {
    QString text;
    QString muted;
    QString rule;
    QString headerBg;
    QString rowBg;
    QString track;
    QString ok;
    QString warn;
    QString danger;
    QString link;
};

UsagePalette usagePalette(bool dark)
{
    UsagePalette p;
    p.text = dark ? QStringLiteral("#e6edf3") : QStringLiteral("#1f2328");
    p.muted = dark ? QStringLiteral("#8b949e") : QStringLiteral("#57606a");
    p.rule = dark ? QStringLiteral("#30363d") : QStringLiteral("#d0d7de");
    p.headerBg = dark ? QStringLiteral("#161b22") : QStringLiteral("#f6f8fa");
    p.rowBg = dark ? QStringLiteral("#0d1117") : QStringLiteral("#ffffff");
    p.track = dark ? QStringLiteral("#21262d") : QStringLiteral("#eaeef2");
    p.ok = dark ? QStringLiteral("#3fb950") : QStringLiteral("#1a7f37");
    p.warn = dark ? QStringLiteral("#d29922") : QStringLiteral("#9a6700");
    p.danger = dark ? QStringLiteral("#f85149") : QStringLiteral("#cf222e");
    p.link = dark ? QStringLiteral("#58a6ff") : QStringLiteral("#0969da");
    return p;
}

// Qt's rich text has no progress bar and ignores CSS widths on block
// elements, so the meter is a two-cell table: percentage-width cells are the
// one sizing primitive it honours.
QString usageMeter(double percent, const QString &colour,
                   const UsagePalette &palette)
{
    const int filled = qBound(0, static_cast<int>(std::lround(percent)), 100);
    QString html = QStringLiteral(
        "<table width='100%' cellspacing='0' cellpadding='0'><tr>");
    if (filled > 0)
        html += QStringLiteral(
                    "<td width='%1%' bgcolor='%2' style='font-size:3px;'>"
                    "&nbsp;</td>")
                    .arg(filled)
                    .arg(colour);
    if (filled < 100)
        html += QStringLiteral(
                    "<td width='%1%' bgcolor='%2' style='font-size:3px;'>"
                    "&nbsp;</td>")
                    .arg(100 - filled)
                    .arg(palette.track);
    return html + QStringLiteral("</tr></table>");
}

QString usageSectionTitle(const QString &title, const QString &subtitle,
                          const UsagePalette &palette)
{
    QString html = QStringLiteral(
                       "<p style='margin-top:14px;margin-bottom:4px;'>"
                       "<b style='color:%1;'>%2</b>")
                       .arg(palette.text, title.toHtmlEscaped());
    if (!subtitle.isEmpty())
        html += QString::fromUtf8(
                    " <span style='color:%1;font-size:11px;'>\xC2\xB7 %2</span>")
                    .arg(palette.muted, subtitle.toHtmlEscaped());
    return html + QStringLiteral("</p>");
}

QString usageHeaderRow(const QStringList &columns, const UsagePalette &palette)
{
    QString html = QStringLiteral("<tr>");
    for (int index = 0; index < columns.size(); ++index)
        html += QStringLiteral(
                    "<td bgcolor='%1' style='padding:4px 6px;color:%2;"
                    "font-size:10px;'%3><b>%4</b></td>")
                    .arg(palette.headerBg, palette.muted,
                         index == 0 ? QString()
                                    : QStringLiteral(" align='right'"),
                         columns.at(index).toHtmlEscaped());
    return html + QStringLiteral("</tr>");
}

QString usageCell(const QString &value, const UsagePalette &palette,
                  bool first = false, const QString &colour = QString())
{
    return QStringLiteral(
               "<td style='padding:3px 6px;border-top:1px solid %1;color:%2;"
               "font-size:11px;'%3>%4</td>")
        .arg(palette.rule, colour.isEmpty() ? palette.text : colour,
             first ? QString() : QStringLiteral(" align='right'"), value);
}

// Zero is the common case in every count column; printing it in full turns
// the tables into a wall of noughts that hides the numbers that matter.
QString usageCountCell(qint64 value, const UsagePalette &palette,
                       const QString &nonZeroColour = QString())
{
    if (value <= 0)
        return usageCell(QStringLiteral("—"), palette, false, palette.muted);
    return usageCell(usageCount(value).toHtmlEscaped(), palette, false,
                     nonZeroColour);
}

QString usageNoticeParagraph(const QString &text, const UsagePalette &palette)
{
    return QStringLiteral("<p style='color:%1;font-size:11px;'>%2</p>")
        .arg(palette.muted, text.toHtmlEscaped());
}

QString usageTtl(double milliseconds)
{
    const qint64 minutes = static_cast<qint64>(milliseconds) / 60'000;
    if (minutes >= 60 && minutes % 60 == 0)
        return QStringLiteral("%1 h").arg(minutes / 60);
    return QStringLiteral("%1 min").arg(minutes);
}

// The D1 read guard: whether heavy readers are conserving and why, the links
// that change the switch, and how long each guarded reader's shared copy
// lives in either state.
QString d1ReadGuardHtml(const QJsonObject &guard, qint64 nowMs,
                        const UsagePalette &palette)
{
    if (guard.isEmpty())
        return QString();
    const QString mode = guard.value(QStringLiteral("mode")).toString();
    const int threshold = guard.value(QStringLiteral("threshold")).toInt();
    const bool conserving =
        guard.value(QStringLiteral("conserving")).toBool();
    const qint64 updatedAt = static_cast<qint64>(
        guard.value(QStringLiteral("updatedAt")).toDouble());
    const QString updatedBy =
        guard.value(QStringLiteral("updatedBy")).toString();
    QString html = usageSectionTitle(
        QStringLiteral("D1 read guard"),
        QStringLiteral("heavy readers share one cached copy per colo; "
                       "conserving keeps each copy longer"),
        palette);
    QString setBy;
    if (updatedAt > 0)
        setBy = QString::fromUtf8(" \xC2\xB7 set %1%2")
                    .arg(usageAgo(updatedAt, nowMs),
                         updatedBy.isEmpty()
                             ? QString()
                             : QStringLiteral(" by %1").arg(updatedBy));
    html += QString::fromUtf8(
                "<p style='font-size:11px;margin-top:0;'>"
                "<b style='color:%1;'>%2</b><span style='color:%3;'> "
                "\xC2\xB7 %4%5</span></p>")
                .arg(conserving ? palette.warn : palette.ok,
                     conserving ? QStringLiteral("Conserving")
                                : QStringLiteral("Normal"),
                     palette.muted,
                     guard.value(QStringLiteral("reason"))
                         .toString()
                         .toHtmlEscaped(),
                     setBy.toHtmlEscaped());

    // The current choice is bold text, every other one a link.
    const auto choice = [&palette](const QString &label,
                                   const QString &linkMode, int linkThreshold,
                                   bool current) {
        if (current)
            return QStringLiteral("<b style='color:%1;'>%2</b>")
                .arg(palette.text, label.toHtmlEscaped());
        return QStringLiteral(
                   "<a href='forkmesh-d1-guard:set?mode=%1&amp;threshold=%2' "
                   "style='color:%3;text-decoration:none;'>%4</a>")
            .arg(linkMode, QString::number(linkThreshold), palette.link,
                 label.toHtmlEscaped());
    };
    const int currentThreshold = threshold > 0 ? threshold : 60;
    html += QString::fromUtf8(
                "<p style='color:%1;font-size:11px;margin-top:0;'>Mode: %2 "
                "\xC2\xB7 %3 \xC2\xB7 %4 &nbsp;&nbsp; Auto threshold: %5 "
                "\xC2\xB7 %6 \xC2\xB7 %7</p>")
                .arg(palette.muted,
                     choice(QStringLiteral("Auto"), QStringLiteral("auto"),
                            currentThreshold, mode == QLatin1String("auto")),
                     choice(QStringLiteral("Always conserve"),
                            QStringLiteral("on"), currentThreshold,
                            mode == QLatin1String("on")),
                     choice(QStringLiteral("Off"), QStringLiteral("off"),
                            currentThreshold, mode == QLatin1String("off")),
                     choice(QStringLiteral("40%"), mode, 40, threshold == 40),
                     choice(QStringLiteral("60%"), mode, 60, threshold == 60),
                     choice(QStringLiteral("80%"), mode, 80, threshold == 80));

    const QJsonArray readers =
        guard.value(QStringLiteral("readers")).toArray();
    if (readers.isEmpty())
        return html;
    html += QStringLiteral("<table width='100%' cellspacing='0' "
                           "cellpadding='0'>");
    html += usageHeaderRow(
        {QStringLiteral("Guarded reader"), QStringLiteral("Normal copy"),
         QStringLiteral("Conserving copy"), QStringLiteral("Serving now")},
        palette);
    for (const QJsonValue &value : readers) {
        const QJsonObject reader = value.toObject();
        html += QStringLiteral("<tr>");
        html += usageCell(
            reader.value(QStringLiteral("label")).toString().toHtmlEscaped(),
            palette, true);
        html += usageCell(
            usageTtl(reader.value(QStringLiteral("normalTtlMs")).toDouble()),
            palette);
        html += usageCell(
            usageTtl(reader.value(QStringLiteral("conserveTtlMs")).toDouble()),
            palette);
        html += usageCell(
            usageTtl(reader.value(QStringLiteral("activeTtlMs")).toDouble()),
            palette, false, conserving ? palette.warn : QString());
        html += QStringLiteral("</tr>");
    }
    return html + QStringLiteral("</table>");
}

QString usageBudgetTable(const QJsonArray &budgets,
                         const UsagePalette &palette)
{
    QString html = QStringLiteral("<table width='100%' cellspacing='0' "
                                  "cellpadding='0'>");
    for (const QJsonValue &value : budgets) {
        const QJsonObject budget = value.toObject();
        const double percent =
            budget.value(QStringLiteral("percent")).toDouble();
        const QString colour = percent >= 80.0   ? palette.danger
                               : percent >= 50.0 ? palette.warn
                                                 : palette.ok;
        const int pinged =
            budget.value(QStringLiteral("pingedThreshold")).toInt();
        html +=
            QStringLiteral("<tr><td style='padding:3px 0 0 0;'>"
                           "<span style='color:%1;font-size:11px;'>%2</span>"
                           "</td><td align='right' style='padding:3px 0 0 0;'>"
                           "<span style='color:%3;font-size:11px;'>"
                           "<b>%4%</b></span>"
                           "<span style='color:%5;font-size:11px;'> · %6 / "
                           "%7 %8%9</span></td></tr>")
                .arg(palette.text,
                     budget.value(QStringLiteral("label"))
                         .toString()
                         .toHtmlEscaped(),
                     colour, usageNumber(percent, 1), palette.muted,
                     usageNumber(
                         budget.value(QStringLiteral("used")).toDouble(), 1),
                     usageCount(static_cast<qint64>(
                         budget.value(QStringLiteral("allowance")).toDouble())),
                     budget.value(QStringLiteral("unit"))
                         .toString()
                         .toHtmlEscaped(),
                     pinged > 0 ? QStringLiteral(" · alerted at %1%").arg(pinged)
                                : QString());
        html += QStringLiteral("<tr><td colspan='2' "
                               "style='padding:0 0 4px 0;'>%1</td></tr>")
                    .arg(usageMeter(percent, colour, palette));
    }
    return html + QStringLiteral("</table>");
}

QString usageCompactValue(double value)
{
    const double absolute = std::abs(value);
    if (absolute >= 1'000'000'000.0)
        return usageNumber(value / 1'000'000'000.0, 1) +
               QStringLiteral("B");
    if (absolute >= 1'000'000.0)
        return usageNumber(value / 1'000'000.0, 1) + QStringLiteral("M");
    if (absolute >= 1'000.0)
        return usageNumber(value / 1'000.0, 1) + QStringLiteral("K");
    return usageNumber(value, value < 100.0 ? 1 : 0);
}

QString usageProgressionTable(const QJsonArray &budgets,
                              const QJsonArray &timeline,
                              qint64 sampledAt,
                              const UsagePalette &palette)
{
    if (budgets.isEmpty() || timeline.isEmpty())
        return usageNoticeParagraph(
            QStringLiteral("Hourly provider buckets have not arrived yet."),
            palette);

    const QDateTime sampled =
        QDateTime::fromMSecsSinceEpoch(sampledAt).toUTC();
    const double elapsedDay = sampledAt > 0
                                  ? sampled.time().msecsSinceStartOfDay() /
                                        86'400'000.0
                                  : 0.0;
    QString html = QStringLiteral("<table width='100%' cellspacing='0' "
                                  "cellpadding='0'>");
    html += usageHeaderRow(
        {QStringLiteral("Metric"), QStringLiteral("Hourly trend (UTC)"),
         QStringLiteral("Now"), QStringLiteral("Estimated at 24:00")},
        palette);
    static const ushort levels[] = {
        0x2581, 0x2582, 0x2583, 0x2584,
        0x2585, 0x2586, 0x2587, 0x2588,
    };
    for (const QJsonValue &value : budgets) {
        const QJsonObject budget = value.toObject();
        const QString metric =
            budget.value(QStringLiteral("metric")).toString();
        const QString label = budget.value(QStringLiteral("label")).toString();
        const QString unit = budget.value(QStringLiteral("unit")).toString();
        const double used = budget.value(QStringLiteral("used")).toDouble();
        const double allowance =
            budget.value(QStringLiteral("allowance")).toDouble();
        QVector<double> cumulative;
        cumulative.reserve(timeline.size());
        double running = 0.0;
        QString firstAt;
        QString lastAt;
        for (const QJsonValue &pointValue : timeline) {
            const QJsonObject point = pointValue.toObject();
            const QString at = point.value(QStringLiteral("at")).toString();
            if (firstAt.isEmpty())
                firstAt = at;
            lastAt = at;
            running += qMax(0.0, point.value(metric).toDouble());
            cumulative.append(running);
        }
        QString trend;
        const double peak = cumulative.isEmpty() ? 0.0 : cumulative.constLast();
        for (const double amount : cumulative) {
            const int level =
                peak > 0.0
                    ? qBound(0,
                             int(std::ceil(amount / peak * 8.0)) - 1, 7)
                    : 0;
            trend.append(QChar(levels[level]));
        }
        const auto hourLabel = [](const QString &iso) {
            const QDateTime parsed = QDateTime::fromString(iso, Qt::ISODate);
            return parsed.isValid()
                       ? parsed.toUTC().toString(QStringLiteral("HH:mm"))
                       : QStringLiteral("—");
        };
        const QString trendCell =
            QStringLiteral("<span style='font-family:monospace;font-size:15px;"
                           "color:%1;'>%2</span><br><span style='color:%3;"
                           "font-size:9px;'>%4 → %5 · hourly</span>")
                .arg(palette.ok, trend.toHtmlEscaped(), palette.muted,
                     hourLabel(firstAt).toHtmlEscaped(),
                     hourLabel(lastAt).toHtmlEscaped());
        const bool canProject = elapsedDay >= (1.0 / 24.0);
        const double projected =
            canProject ? qMax(used, used / elapsedDay) : 0.0;
        const double projectedPercent =
            allowance > 0.0 ? projected * 100.0 / allowance : 0.0;
        const QString estimateColour = projectedPercent >= 80.0
                                           ? palette.danger
                                           : projectedPercent >= 50.0
                                                 ? palette.warn
                                                 : palette.ok;
        html += QStringLiteral("<tr>");
        html += usageCell(QStringLiteral("<b>%1</b>").arg(label.toHtmlEscaped()),
                          palette, true);
        html += usageCell(trendCell, palette);
        html += usageCell(
            QStringLiteral("%1 %2")
                .arg(usageCompactValue(used), unit.toHtmlEscaped()),
            palette);
        html += usageCell(
            canProject
                ? QStringLiteral("<b>%1 %2</b><br><span style='color:%3;'>"
                                 "%4% of limit</span>")
                      .arg(usageCompactValue(projected), unit.toHtmlEscaped(),
                           estimateColour,
                           usageNumber(projectedPercent, 1))
                : QStringLiteral("not enough data"),
            palette, false, canProject ? estimateColour : palette.muted);
        html += QStringLiteral("</tr>");
    }
    return html + QStringLiteral("</table>");
}

}  // namespace

void CloudUsageTally::record(const CloudflareTailEvent &event, qint64 nowMs)
{
    if (!event.parsed)
        return;
    if (m_sinceMs <= 0)
        m_sinceMs = nowMs;
    ++m_totalEvents;

    QString key = cloudUsageKey(event);
    auto slot = m_entries.find(key);
    if (slot == m_entries.end() && m_entries.size() >= kCloudUsageMaxEntries) {
        key = QString::fromLatin1(kCloudUsageOverflowKey);
        slot = m_entries.find(key);
    }
    if (slot == m_entries.end()) {
        CloudUsageLiveEntry fresh;
        if (key == QLatin1String(kCloudUsageOverflowKey)) {
            fresh.overflow = true;
        } else {
            fresh.scriptName = event.scriptName;
            fresh.entrypoint = event.entrypoint;
            fresh.durableObjectId = event.durableObjectId;
        }
        slot = m_entries.insert(key, fresh);
    }

    CloudUsageLiveEntry &entry = *slot;
    ++entry.events;
    // A deploy handoff is the rollout terminating hibernated sockets, not the
    // object erroring; counting it would make every deploy look like an
    // incident. handleCloudLogMonitorLine draws the same line.
    if (event.isError && !event.isDeployInterruption)
        ++entry.errors;
    if (event.platformFault)
        ++entry.platformFaults;
    if (event.wallTimeMs >= 0)
        entry.wallTimeMs += event.wallTimeMs;
    if (event.cpuTimeMs >= 0)
        entry.cpuTimeMs += event.cpuTimeMs;
    entry.lastSeenMs = qMax(entry.lastSeenMs, nowMs);
}

QList<CloudUsageLiveEntry> CloudUsageTally::ranked(int limit) const
{
    QList<CloudUsageLiveEntry> entries;
    entries.reserve(m_entries.size());
    CloudUsageLiveEntry overflow;
    for (auto it = m_entries.constBegin(); it != m_entries.constEnd(); ++it) {
        if (it->overflow)
            overflow = *it;
        else
            entries.append(*it);
    }
    std::sort(entries.begin(), entries.end(),
              [](const CloudUsageLiveEntry &a, const CloudUsageLiveEntry &b) {
                  if (a.events != b.events)
                      return a.events > b.events;
                  if (a.errors != b.errors)
                      return a.errors > b.errors;
                  if (a.scriptName != b.scriptName)
                      return a.scriptName < b.scriptName;
                  if (a.entrypoint != b.entrypoint)
                      return a.entrypoint < b.entrypoint;
                  return a.durableObjectId < b.durableObjectId;
              });
    if (limit > 0 && entries.size() > limit)
        entries.erase(entries.begin() + limit, entries.end());
    if (overflow.events > 0)
        entries.append(overflow);
    return entries;
}

void CloudUsageTally::clear()
{
    m_entries.clear();
    m_totalEvents = 0;
    m_sinceMs = 0;
}

namespace {

constexpr qint64 kWebRequestMinuteMs = 60000;

// Mirrors _API_NAMESPACES and _STATIC_GROUPS in app/src/api_metrics.py.
const QSet<QString> &webRequestNamespaces()
{
    static const QSet<QString> namespaces{
        QStringLiteral("accounts"), QStringLiteral("admin"),
        QStringLiteral("ai"), QStringLiteral("attribution"),
        QStringLiteral("badges"), QStringLiteral("blog"),
        QStringLiteral("bot"), QStringLiteral("calendar"),
        QStringLiteral("chat"), QStringLiteral("client-errors"),
        QStringLiteral("bootstrap"), QStringLiteral("desktop-errors"),
        QStringLiteral("federation"), QStringLiteral("feedback"),
        QStringLiteral("forkbot"), QStringLiteral("genie"),
        QStringLiteral("install-diag"), QStringLiteral("install-source"),
        QStringLiteral("integrations"), QStringLiteral("ledger"),
        QStringLiteral("leaderboards"), QStringLiteral("mainnode"),
        QStringLiteral("metrics"), QStringLiteral("mirror-requests"),
        QStringLiteral("mirrors"), QStringLiteral("network"),
        QStringLiteral("nodes"), QStringLiteral("notes"),
        QStringLiteral("notifications"), QStringLiteral("openapi.json"),
        QStringLiteral("orgs"), QStringLiteral("poll"),
        QStringLiteral("private-replicas"), QStringLiteral("referrals"),
        QStringLiteral("relay-mesh"), QStringLiteral("repo"),
        QStringLiteral("repositories"),
        QStringLiteral("repository-imports"), QStringLiteral("rewards"),
        QStringLiteral("room"), QStringLiteral("security"),
        QStringLiteral("security-scans"), QStringLiteral("ssh"),
        QStringLiteral("status"), QStringLiteral("sync"),
        QStringLiteral("tasks"), QStringLiteral("version"),
        QStringLiteral("world"),
    };
    return namespaces;
}

const QSet<QString> &webRequestStaticGroups()
{
    static const QSet<QString> groups{
        QStringLiteral("accounts/availability"),
        QStringLiteral("accounts/change-password"),
        QStringLiteral("accounts/forgot-password"),
        QStringLiteral("accounts/login"), QStringLiteral("accounts/logout"),
        QStringLiteral("accounts/profile"),
        QStringLiteral("accounts/reset-password"),
        QStringLiteral("accounts/sessions"), QStringLiteral("accounts/signup"),
        QStringLiteral("accounts/users"),
        QStringLiteral("attribution/capture"),
        QStringLiteral("attribution/events"),
        QStringLiteral("attribution/session"),
        QStringLiteral("bootstrap/readiness"),
        QStringLiteral("metrics/endpoints"), QStringLiteral("mirrors/https"),
        QStringLiteral("network/overview"), QStringLiteral("openapi.json"),
        QStringLiteral("repositories"), QStringLiteral("status"),
        QStringLiteral("sync"), QStringLiteral("version"),
        QStringLiteral("world/client"), QStringLiteral("world/context"),
        QStringLiteral("world/inactive"), QStringLiteral("world/online"),
        QStringLiteral("world/qa"), QStringLiteral("world/ticket"),
        QStringLiteral("world/visitors"), QStringLiteral("world/wallet"),
        QStringLiteral("world/ws"),
    };
    return groups;
}

QString webRequestStatusClass(int status)
{
    if (status >= 500)
        return QStringLiteral("5xx");
    if (status >= 400)
        return QStringLiteral("4xx");
    if (status >= 300)
        return QStringLiteral("3xx");
    return QStringLiteral("2xx");
}

}  // namespace

QString webRequestRouteGroup(const QString &path)
{
    QStringList segments = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (!segments.isEmpty() && segments.first() == QLatin1String("api"))
        segments.removeFirst();
    if (segments.isEmpty())
        return QStringLiteral("(root)");
    QStringList lowered;
    for (int i = 0; i < segments.size() && i < 4; ++i)
        lowered << segments.at(i).toLower();
    const QString &ns = lowered.first();
    if (!webRequestNamespaces().contains(ns))
        return QStringLiteral("other");
    // forkmesh/forkmesh content reads keep their own label (the /status
    // page's visitor evidence); see FLAGSHIP_ROUTE_GROUP in api_metrics.py.
    if (lowered.size() >= 3 && lowered.at(0) == QLatin1String("repo") &&
        lowered.at(1) == QLatin1String("forkmesh") &&
        lowered.at(2) == QLatin1String("forkmesh")) {
        static const QSet<QString> contentActions{
            QStringLiteral("tree"), QStringLiteral("blobs"),
            QStringLiteral("blob"), QStringLiteral("raw"),
            QStringLiteral("history"), QStringLiteral("commit"),
            QStringLiteral("compare"), QStringLiteral("branches"),
            QStringLiteral("search"), QStringLiteral("stats"),
            QStringLiteral("sizes")};
        const bool content =
            (segments.size() == 4 && contentActions.contains(lowered.at(3))) ||
            (segments.size() >= 5 &&
             lowered.at(3) == QLatin1String("releases") &&
             segments.at(4).toLower() == QLatin1String("blob"));
        if (content)
            return QStringLiteral("repo/forkmesh/forkmesh/content");
    }
    const QString candidate = lowered.join(QLatin1Char('/'));
    if (webRequestStaticGroups().contains(candidate))
        return candidate;
    return lowered.size() == 1 ? ns : ns + QStringLiteral("/*");
}

bool WebRequestTally::record(const CloudflareTailEvent &event, qint64 nowMs)
{
    // A Durable Object's fetch is a hop the Worker already answered for, and
    // counting it too would double the hit.
    if (!event.parsed || event.kind != QLatin1String("fetch") ||
        !event.entrypoint.isEmpty())
        return false;
    const QString path = QUrl(event.url).path();
    if (!path.startsWith(QLatin1String("/api/")))
        return false;
    // No response status: the Worker threw (the client got a 5xx page) or
    // the client went away first, which is no answer to count.
    if (event.status <= 0 && !event.isError)
        return false;
    const QString status =
        event.status > 0 ? webRequestStatusClass(event.status)
                         : QStringLiteral("5xx");

    const QString group = admitGroup(webRequestRouteGroup(path));
    const qint64 minute = (nowMs / kWebRequestMinuteMs) * kWebRequestMinuteMs;
    prune(nowMs);
    if (m_sinceMs <= 0)
        m_sinceMs = nowMs;

    Bucket &bucket = m_minutes[minute][group + QLatin1Char('\n') + status];
    const qint64 durationMs =
        event.wallTimeMs >= 0 ? qRound64(event.wallTimeMs) : 0;
    ++bucket.requests;
    bucket.durationSumMs += durationMs;
    bucket.durationMaxMs = qMax(bucket.durationMaxMs, durationMs);
    return true;
}

bool WebRequestTally::recordBatch(const QString &group,
                                  const QString &statusClass, qint64 count,
                                  qint64 durationSumMs, qint64 durationMaxMs,
                                  qint64 atMs, qint64 nowMs)
{
    static const QSet<QString> kClasses = {
        QStringLiteral("2xx"), QStringLiteral("3xx"), QStringLiteral("4xx"),
        QStringLiteral("5xx")};
    if (count <= 0 || atMs <= 0 || group.trimmed().isEmpty() ||
        !kClasses.contains(statusClass))
        return false;
    const qint64 minute = (atMs / kWebRequestMinuteMs) * kWebRequestMinuteMs;
    const qint64 nowMinute =
        (nowMs / kWebRequestMinuteMs) * kWebRequestMinuteMs;
    if (minute < nowMinute - kMaxMinutes * kWebRequestMinuteMs)
        return false;
    const QString admitted = admitGroup(group.trimmed().left(120));
    prune(nowMs);
    if (m_sinceMs <= 0 || atMs < m_sinceMs)
        m_sinceMs = atMs;

    Bucket &bucket = m_minutes[minute][admitted + QLatin1Char('\n') +
                                       statusClass];
    bucket.requests += count;
    bucket.durationSumMs += qMax<qint64>(0, durationSumMs);
    bucket.durationMaxMs = qMax(bucket.durationMaxMs, durationMaxMs);
    return true;
}

QString WebRequestTally::admitGroup(QString group)
{
    if (!m_groups.contains(group)) {
        if (m_groups.size() >= kMaxGroups &&
            group != QLatin1String("repo/forkmesh/forkmesh/content"))
            group = QStringLiteral("other");
        m_groups.insert(group);
    }
    return group;
}

void WebRequestTally::prune(qint64 nowMs)
{
    const qint64 oldest = (nowMs / kWebRequestMinuteMs) * kWebRequestMinuteMs -
                          kMaxMinutes * kWebRequestMinuteMs;
    while (!m_minutes.isEmpty() && m_minutes.firstKey() < oldest)
        m_minutes.erase(m_minutes.begin());
}

QJsonObject WebRequestTally::summary(int windowMinutes, qint64 nowMs) const
{
    windowMinutes = qBound(1, windowMinutes, int(kMaxMinutes));
    const qint64 since = (nowMs / kWebRequestMinuteMs) * kWebRequestMinuteMs -
                         qint64(windowMinutes) * kWebRequestMinuteMs;
    struct GroupTotals {
        qint64 requests = 0;
        qint64 errors = 0;
        qint64 durationSumMs = 0;
        qint64 durationMaxMs = 0;
        QMap<QString, qint64> classes;
    };
    QHash<QString, GroupTotals> groups;
    QJsonArray minutes;
    QMap<QString, qint64> classes{{QStringLiteral("2xx"), 0},
                                  {QStringLiteral("3xx"), 0},
                                  {QStringLiteral("4xx"), 0},
                                  {QStringLiteral("5xx"), 0}};
    for (auto minute = m_minutes.lowerBound(since);
         minute != m_minutes.constEnd(); ++minute) {
        qint64 minuteRequests = 0;
        qint64 minuteErrors = 0;
        for (auto it = minute->constBegin(); it != minute->constEnd(); ++it) {
            const qsizetype split = it.key().lastIndexOf(QLatin1Char('\n'));
            const QString group = it.key().left(split);
            const QString status = it.key().mid(split + 1);
            GroupTotals &totals = groups[group];
            totals.requests += it->requests;
            totals.durationSumMs += it->durationSumMs;
            totals.durationMaxMs = qMax(totals.durationMaxMs, it->durationMaxMs);
            totals.classes[status] += it->requests;
            classes[status] += it->requests;
            minuteRequests += it->requests;
            if (status == QLatin1String("5xx")) {
                totals.errors += it->requests;
                minuteErrors += it->requests;
            }
        }
        minutes.append(QJsonObject{{QStringLiteral("t"), minute.key()},
                                   {QStringLiteral("requests"), minuteRequests},
                                   {QStringLiteral("errors"), minuteErrors}});
    }

    QList<QPair<QString, GroupTotals>> ranked;
    ranked.reserve(groups.size());
    for (auto it = groups.constBegin(); it != groups.constEnd(); ++it)
        ranked.append({it.key(), it.value()});
    std::sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) {
        if (a.second.requests != b.second.requests)
            return a.second.requests > b.second.requests;
        return a.first < b.first;
    });
    const auto classesJson = [](const QMap<QString, qint64> &counts) {
        QJsonObject object;
        for (const char *key : {"2xx", "3xx", "4xx", "5xx"})
            object.insert(QLatin1String(key),
                          counts.value(QLatin1String(key)));
        return object;
    };
    QJsonArray groupsJson;
    for (const auto &[name, totals] : std::as_const(ranked)) {
        groupsJson.append(QJsonObject{
            {QStringLiteral("group"), name},
            {QStringLiteral("requests"), totals.requests},
            {QStringLiteral("errors"), totals.errors},
            {QStringLiteral("avg_ms"),
             totals.requests > 0 ? totals.durationSumMs / totals.requests : 0},
            {QStringLiteral("dur_ms_max"), totals.durationMaxMs},
            {QStringLiteral("classes"), classesJson(totals.classes)}});
    }
    return QJsonObject{{QStringLiteral("ok"), true},
                       {QStringLiteral("now"), nowMs},
                       {QStringLiteral("windowMinutes"), windowMinutes},
                       {QStringLiteral("countingSinceMs"), m_sinceMs},
                       {QStringLiteral("minutes"), minutes},
                       {QStringLiteral("classes"), classesJson(classes)},
                       {QStringLiteral("groups"), groupsJson}};
}

void WebRequestTally::clear()
{
    m_minutes.clear();
    m_groups.clear();
    m_sinceMs = 0;
}

QString renderCloudUsageHtml(const QJsonObject &report,
                             const QList<CloudUsageLiveEntry> &live,
                             qint64 liveSinceMs, qint64 totalLiveEvents,
                             qint64 nowMs, bool dark)
{
    const UsagePalette palette = usagePalette(dark);
    QString html = QStringLiteral("<div style='color:%1;'>").arg(palette.text);

    const bool haveReport = report.value(QStringLiteral("ok")).toBool();
    if (haveReport) {
        const QString day = report.value(QStringLiteral("day")).toString();
        const qint64 sampledAt = static_cast<qint64>(
            report.value(QStringLiteral("sampledAt")).toDouble());
        html += usageSectionTitle(
            QStringLiteral("Free-plan allowances, today"),
            QStringLiteral("%1 UTC, resets at midnight UTC%2")
                .arg(day,
                     sampledAt > 0
                         ? QStringLiteral(" · read %1")
                               .arg(usageAgo(sampledAt, nowMs))
                         : QString()),
            palette);

        html += usageBudgetTable(
            report.value(QStringLiteral("budgets")).toArray(), palette);
        html += usageSectionTitle(
            QStringLiteral("Usage progression and estimate"),
            QStringLiteral("hourly provider buckets; the midnight projection "
                           "extends today's average rate and may lag"),
            palette);
        html += usageProgressionTable(
            report.value(QStringLiteral("budgets")).toArray(),
            report.value(QStringLiteral("timeline")).toArray(), sampledAt,
            palette);

        const QJsonObject aborts =
            report.value(QStringLiteral("aborts")).toObject();
        const qint64 aborts24h = static_cast<qint64>(
            aborts.value(QStringLiteral("aborts24h")).toDouble());
        if (aborts24h > 0) {
            const qint64 duration24h = static_cast<qint64>(
                aborts.value(QStringLiteral("durationAborts24h")).toDouble());
            const qint64 aborts60m = static_cast<qint64>(
                aborts.value(QStringLiteral("aborts60m")).toDouble());
            html += QStringLiteral(
                        "<p style='color:%1;font-size:11px;'>"
                        "<b>%2</b> Durable Object abort%3 in 24h "
                        "(%4 in the last hour%5) — requests the platform cut "
                        "off mid-flight.</p>")
                        .arg(palette.danger, usageCount(aborts24h),
                             aborts24h == 1 ? QString() : QStringLiteral("s"),
                             usageCount(aborts60m),
                             duration24h > 0
                                 ? QStringLiteral(", %1 for exceeding the free "
                                                  "duration allowance")
                                       .arg(usageCount(duration24h))
                                 : QString());
        }

        const QJsonArray durable =
            report.value(QStringLiteral("durableObjects")).toArray();
        html += usageSectionTitle(
            QStringLiteral("Durable Objects by namespace"),
            durable.isEmpty()
                ? QStringLiteral("no usage recorded today")
                : QStringLiteral("%1 class%2, busiest first")
                      .arg(durable.size())
                      .arg(durable.size() == 1 ? QString()
                                               : QStringLiteral("es")),
            palette);
        if (!durable.isEmpty()) {
            qint64 rawDurableErrors = 0;
            html += QStringLiteral("<table width='100%' cellspacing='0' "
                                   "cellpadding='0'>");
            html += usageHeaderRow(
                {QStringLiteral("Class"), QStringLiteral("GB-s"),
                 QStringLiteral("Requests"), QStringLiteral("Raw errors"),
                 QStringLiteral("CPU"), QStringLiteral("Wall"),
                 QStringLiteral("Reads"), QStringLiteral("Writes"),
                 QStringLiteral("Deletes"), QStringLiteral("Sub-req"),
                 QStringLiteral("WS in"), QStringLiteral("WS out"),
                 QStringLiteral("Out")},
                palette);
            for (const QJsonValue &value : durable) {
                const QJsonObject row = value.toObject();
                const QString namespaceId =
                    row.value(QStringLiteral("namespaceId")).toString();
                QString name = row.value(QStringLiteral("label")).toString();
                if (name.isEmpty())
                    name = usageShortId(namespaceId);
                const QString script =
                    row.value(QStringLiteral("script")).toString();
                const qint64 errors = static_cast<qint64>(
                    row.value(QStringLiteral("errors")).toDouble());
                rawDurableErrors += errors;
                html += QStringLiteral("<tr>");
                html += usageCell(
                    QStringLiteral("<b>%1</b>%2")
                        .arg(name.toHtmlEscaped(),
                             script.isEmpty()
                                 ? QString()
                                 : QStringLiteral(
                                       "<span style='color:%1;'> · %2</span>")
                                       .arg(palette.muted,
                                            script.toHtmlEscaped())),
                    palette, true);
                html += usageCell(
                    usageNumber(
                        row.value(QStringLiteral("gbSeconds")).toDouble(), 2),
                    palette);
                html += usageCountCell(
                    static_cast<qint64>(
                        row.value(QStringLiteral("requests")).toDouble()),
                    palette);
                html += usageCountCell(errors, palette, palette.danger);
                html += usageCell(
                    usageDuration(
                        row.value(QStringLiteral("cpuSeconds")).toDouble() *
                        1000.0),
                    palette);
                html += usageCell(
                    usageDuration(
                        row.value(QStringLiteral("wallTimeMs")).toDouble()),
                    palette);
                for (const char *field :
                     {"storageReads", "storageWrites", "storageDeletes",
                      "subrequests", "websocketIn", "websocketOut"})
                    html += usageCountCell(
                        static_cast<qint64>(
                            row.value(QLatin1String(field)).toDouble()),
                        palette);
                html += usageCell(
                    formatDiskSize(static_cast<qint64>(
                        row.value(QStringLiteral("responseBytes")).toDouble())),
                    palette);
                html += QStringLiteral("</tr>");
            }
            html += QStringLiteral("</table>");
            if (rawDurableErrors > 0) {
                html += usageNoticeParagraph(
                    QStringLiteral(
                        "Cloudflare's namespace counter is raw and includes "
                        "recognized code-update and instance handoffs. The "
                        "live-tail Errors column below excludes those expected "
                        "handoffs; Faults and aborts remain actionable."),
                    palette);
            }
        }

        const QJsonArray workers =
            report.value(QStringLiteral("workers")).toArray();
        if (!workers.isEmpty()) {
            html += usageSectionTitle(
                QStringLiteral("Workers by script"),
                QStringLiteral("requests that reached the account today"),
                palette);
            html += QStringLiteral("<table width='100%' cellspacing='0' "
                                   "cellpadding='0'>");
            html += usageHeaderRow({QStringLiteral("Worker"),
                                    QStringLiteral("Requests"),
                                    QStringLiteral("Errors"),
                                    QStringLiteral("Sub-req"),
                                    QStringLiteral("GB-s")},
                                   palette);
            for (const QJsonValue &value : workers) {
                const QJsonObject row = value.toObject();
                html += QStringLiteral("<tr>");
                html += usageCell(QStringLiteral("<b>%1</b>").arg(
                                      row.value(QStringLiteral("script"))
                                          .toString()
                                          .toHtmlEscaped()),
                                  palette, true);
                html += usageCountCell(
                    static_cast<qint64>(
                        row.value(QStringLiteral("requests")).toDouble()),
                    palette);
                html += usageCountCell(
                    static_cast<qint64>(
                        row.value(QStringLiteral("errors")).toDouble()),
                    palette, palette.danger);
                html += usageCountCell(
                    static_cast<qint64>(
                        row.value(QStringLiteral("subrequests")).toDouble()),
                    palette);
                html += usageCell(
                    usageNumber(
                        row.value(QStringLiteral("gbSeconds")).toDouble(), 2),
                    palette);
                html += QStringLiteral("</tr>");
            }
            html += QStringLiteral("</table>");
        }
    } else {
        html += usageSectionTitle(QStringLiteral("Free-plan allowances, today"),
                                  QString(), palette);
        const QString error = report.value(QStringLiteral("error")).toString();
        if (error == QLatin1String("analytics_unconfigured"))
            html += usageNoticeParagraph(
                QStringLiteral(
                    "The relay has no analytics token, so account-wide usage "
                    "is unavailable. Set USAGE_ANALYTICS_API_TOKEN and "
                    "WORKERS_OBSERVABILITY_ACCOUNT_ID in app/.env.production "
                    "and deploy."),
                palette);
        else if (error == QLatin1String("forbidden") ||
                 error == QLatin1String("unauthorized"))
            // "forbidden" is this node deciding it cannot ask; "unauthorized"
            // is the relay rejecting the signature it did send. Both mean the
            // reader is not an admin here, which is the useful thing to say.
            html += usageNoticeParagraph(
                QStringLiteral("Account-wide usage is admin-only, and this "
                               "node is not signed in as an admin."),
                palette);
        else if (!error.isEmpty())
            html += usageNoticeParagraph(
                QStringLiteral("Cloudflare analytics could not be read (%1). "
                               "The live tail below still works.")
                    .arg(error),
                palette);
        else
            html += usageNoticeParagraph(
                QStringLiteral("Reading today's usage from Cloudflare…"),
                palette);
    }

    // The analytics API stops at the namespace; only the tail says which
    // individual object the traffic hit, which is the question "what is using
    // the Durable Objects the most" usually turns out to mean.
    html += usageSectionTitle(
        QStringLiteral("Live tail, by object"),
        live.isEmpty()
            ? QStringLiteral("no cloud events observed yet")
            : QStringLiteral("%1 event%2 since %3")
                  .arg(usageCount(totalLiveEvents))
                  .arg(totalLiveEvents == 1 ? QString() : QStringLiteral("s"),
                       usageAgo(liveSinceMs, nowMs)),
        palette);
    if (live.isEmpty()) {
        html += usageNoticeParagraph(
            QStringLiteral("Turn on the CLOUD log feed to see per-object "
                           "traffic as it happens."),
            palette);
    } else {
        html += QStringLiteral("<table width='100%' cellspacing='0' "
                               "cellpadding='0'>");
        html += usageHeaderRow({QStringLiteral("Object"),
                                QStringLiteral("Events"),
                                QStringLiteral("Share"),
                                QStringLiteral("Errors"),
                                QStringLiteral("Faults"),
                                QStringLiteral("Avg wall"),
                                QStringLiteral("Avg CPU"),
                                QStringLiteral("Last seen")},
                               palette);
        for (const CloudUsageLiveEntry &entry : live) {
            QString name;
            if (entry.overflow) {
                name = QStringLiteral(
                           "<i style='color:%1;'>other objects</i>")
                           .arg(palette.muted);
            } else {
                const QString primary =
                    entry.entrypoint.isEmpty()
                        ? (entry.scriptName.isEmpty()
                               ? QStringLiteral("(unnamed)")
                               : entry.scriptName)
                        : entry.entrypoint;
                name = QStringLiteral("<b>%1</b>").arg(primary.toHtmlEscaped());
                QStringList detail;
                if (!entry.entrypoint.isEmpty() && !entry.scriptName.isEmpty())
                    detail << entry.scriptName;
                if (!entry.durableObjectId.isEmpty())
                    detail << usageShortId(entry.durableObjectId);
                if (!detail.isEmpty())
                    name += QString::fromUtf8(
                                "<span style='color:%1;'> \xC2\xB7 %2</span>")
                                .arg(palette.muted,
                                     detail.join(QString::fromUtf8(
                                                     " \xC2\xB7 "))
                                         .toHtmlEscaped());
            }
            const double share =
                totalLiveEvents > 0
                    ? static_cast<double>(entry.events) * 100.0 /
                          static_cast<double>(totalLiveEvents)
                    : 0.0;
            html += QStringLiteral("<tr>");
            html += usageCell(name, palette, true);
            html += usageCell(usageCount(entry.events), palette);
            html += usageCell(usageNumber(share, 1) + QStringLiteral("%"),
                              palette, false, palette.muted);
            html += usageCountCell(entry.errors, palette, palette.danger);
            html += usageCountCell(entry.platformFaults, palette,
                                   palette.warn);
            html += usageCell(usageDuration(entry.events > 0
                                                ? entry.wallTimeMs /
                                                      double(entry.events)
                                                : 0.0),
                              palette);
            html += usageCell(usageDuration(entry.events > 0
                                                ? entry.cpuTimeMs /
                                                      double(entry.events)
                                                : 0.0),
                              palette);
            html += usageCell(usageAgo(entry.lastSeenMs, nowMs), palette, false,
                              palette.muted);
            html += QStringLiteral("</tr>");
        }
        html += QStringLiteral("</table>");
    }
    return html + QStringLiteral("</div>");
}

QString cloudflareD1DashboardUrl(const QString &databaseId)
{
    const QString base =
        QStringLiteral("https://dash.cloudflare.com/?to=/:account/workers/d1");
    static const QRegularExpression idRe(
        QStringLiteral("\\A[0-9a-fA-F-]{16,64}\\z"));
    if (databaseId.isEmpty() || !idRe.match(databaseId).hasMatch())
        return base;
    return base + QStringLiteral("/databases/") + databaseId;
}

QString renderD1UsageHtml(const QJsonObject &report, qint64 nowMs, bool dark,
                          const QHash<QString, D1QuerySourceLink> &querySources)
{
    const UsagePalette palette = usagePalette(dark);
    QString html = QStringLiteral("<div style='color:%1;'>").arg(palette.text);
    const bool haveReport = report.value(QStringLiteral("ok")).toBool();
    const QString day = report.value(QStringLiteral("day")).toString();
    const qint64 sampledAt = static_cast<qint64>(
        report.value(QStringLiteral("sampledAt")).toDouble());

    html += usageSectionTitle(
        QStringLiteral("D1 Free-plan allowances, today"),
        day.isEmpty()
            ? QString()
            : QStringLiteral("%1 UTC, daily rows reset at midnight UTC%2")
                  .arg(day,
                       sampledAt > 0
                           ? QStringLiteral(" · read %1")
                                 .arg(usageAgo(sampledAt, nowMs))
                           : QString()),
        palette);
    if (haveReport) {
        html += usageBudgetTable(
            report.value(QStringLiteral("budgets")).toArray(), palette);
        html += d1ReadGuardHtml(
            report.value(QStringLiteral("readGuard")).toObject(), nowMs,
            palette);
        html += usageSectionTitle(
            QStringLiteral("Usage progression and estimate"),
            QStringLiteral("hourly UTC buckets; the midnight projection "
                           "extends today's average rate"),
            palette);
        html += usageProgressionTable(
            report.value(QStringLiteral("budgets")).toArray(),
            report.value(QStringLiteral("timeline")).toArray(), sampledAt,
            palette);
        html += QStringLiteral(
                    "<p style='color:%1;font-size:11px;'><b>Delayed "
                    "estimate:</b> %2</p>")
                    .arg(
                        palette.warn,
                        QStringLiteral(
                            "these are account-wide provider analytics for "
                            "the UTC day, but they can trail live queries and "
                            "limit enforcement. Treat a provider limit alert "
                            "as authoritative until the daily reset.")
                            .toHtmlEscaped());

        const QJsonArray insights =
            report.value(QStringLiteral("queryInsights")).toArray();
        html += usageSectionTitle(
            QStringLiteral("Top D1 queries by rows read"),
            insights.isEmpty()
                ? QStringLiteral("no query groups reported yet")
                : QStringLiteral("%1 account-wide statement%2 today; bound "
                                 "values are not captured%3")
                      .arg(insights.size())
                      .arg(insights.size() == 1 ? QString()
                                                : QStringLiteral("s"),
                           querySources.isEmpty()
                               ? QString()
                               : QString::fromUtf8(
                                     " \xC2\xB7 open a file link to read the "
                                     "code that issues the statement")),
            palette);
        if (insights.isEmpty()) {
            html += usageNoticeParagraph(
                QStringLiteral("Query insights can arrive after the daily "
                               "allowance totals."),
                palette);
        } else {
            html += QStringLiteral("<table width='100%' cellspacing='0' "
                                   "cellpadding='0'>");
            html += usageHeaderRow(
                {QStringLiteral("Query"), QStringLiteral("Runs"),
                 QStringLiteral("Total reads"), QStringLiteral("Avg reads"),
                 QStringLiteral("Writes"), QStringLiteral("Avg time"),
                 QStringLiteral("Efficiency")},
                palette);
            for (const QJsonValue &value : insights) {
                const QJsonObject row = value.toObject();
                const QString queryText =
                    row.value(QStringLiteral("query")).toString();
                QString queryCell = QStringLiteral("<code>%1</code>")
                                        .arg(queryText.toHtmlEscaped());
                const D1QuerySourceLink link = querySources.value(queryText);
                if (!link.label.isEmpty()) {
                    // The provider names a statement, never a caller; this is
                    // the place in the local checkout that issues it.
                    queryCell +=
                        QStringLiteral(
                            "<div style='padding-top:2px;'>"
                            "<a href='%1' style='color:%2;font-size:10px;"
                            "text-decoration:none;'>%3</a>%4</div>")
                            .arg(link.href.toHtmlEscaped(), palette.link,
                                 link.label.toHtmlEscaped(),
                                 link.note.isEmpty()
                                     ? QString()
                                     : QStringLiteral(
                                           "<span style='color:%1;"
                                           "font-size:10px;'> %2</span>")
                                           .arg(palette.muted,
                                                link.note.toHtmlEscaped()));
                }
                const QString guardName =
                    row.value(QStringLiteral("guard")).toString();
                if (!guardName.isEmpty())
                    queryCell +=
                        QStringLiteral("<div style='padding-top:2px;color:%1;"
                                       "font-size:10px;'>guarded: %2</div>")
                            .arg(palette.ok, guardName.toHtmlEscaped());
                html += QStringLiteral("<tr>");
                html += usageCell(queryCell, palette, true);
                html += usageCountCell(static_cast<qint64>(
                                           row.value(QStringLiteral("runs"))
                                               .toDouble()),
                                       palette);
                html += usageCountCell(
                    static_cast<qint64>(
                        row.value(QStringLiteral("totalRowsRead")).toDouble()),
                    palette, palette.warn);
                html += usageCell(
                    usageNumber(
                        row.value(QStringLiteral("avgRowsRead")).toDouble(), 1),
                    palette);
                html += usageCountCell(
                    static_cast<qint64>(row.value(
                                               QStringLiteral("totalRowsWritten"))
                                               .toDouble()),
                    palette);
                html += usageCell(
                    usageDuration(row.value(QStringLiteral("avgDurationMs"))
                                      .toDouble()),
                    palette);
                html += usageCell(
                    usageNumber(row.value(QStringLiteral("efficiency"))
                                        .toDouble() *
                                    100.0,
                                1) +
                        QStringLiteral("%"),
                    palette);
                html += QStringLiteral("</tr>");
            }
            html += QStringLiteral("</table>");
        }
    } else {
        const QString error = report.value(QStringLiteral("error")).toString();
        if (error == QLatin1String("analytics_unconfigured"))
            html += usageNoticeParagraph(
                QStringLiteral("Daily D1 row usage is unavailable because the "
                               "relay has no analytics token. Set "
                               "USAGE_ANALYTICS_API_TOKEN and "
                               "WORKERS_OBSERVABILITY_ACCOUNT_ID and deploy; "
                               "bound-database storage is still measured "
                               "directly below."),
                palette);
        else if (error == QLatin1String("forbidden") ||
                 error == QLatin1String("unauthorized"))
            html += usageNoticeParagraph(
                QStringLiteral("D1 usage is admin-only, and this node is not "
                               "signed in as an admin."),
                palette);
        else if (!error.isEmpty())
            html += usageNoticeParagraph(
                QStringLiteral("Cloudflare D1 analytics could not be read "
                               "(%1). Bound-database storage is still shown "
                               "when available.")
                    .arg(error),
                palette);
        else
            html += usageNoticeParagraph(
                QStringLiteral("Reading today's D1 usage from Cloudflare…"),
                palette);
        // The switch stays reachable when analytics are not.
        html += d1ReadGuardHtml(
            report.value(QStringLiteral("readGuard")).toObject(), nowMs,
            palette);
    }

    const QJsonObject storage =
        report.value(QStringLiteral("storage")).toObject();
    const qint64 storedBytes = static_cast<qint64>(
        storage.value(QStringLiteral("bytes")).toDouble());
    const qint64 freeDatabaseBytes = static_cast<qint64>(
        storage.value(QStringLiteral("freeDatabaseLimitBytes")).toDouble());
    const qint64 includedAccountBytes = static_cast<qint64>(
        storage.value(QStringLiteral("includedAccountStorageBytes")).toDouble());
    const qint64 paidDatabaseBytes = static_cast<qint64>(
        storage.value(QStringLiteral("paidDatabaseLimitBytes")).toDouble());
    html += usageSectionTitle(
        QStringLiteral("Bound database storage"),
        storage.isEmpty()
            ? QStringLiteral("provider measurement unavailable")
            : QStringLiteral("provider-reported after the latest D1 query"),
        palette);
    if (!storage.isEmpty() && freeDatabaseBytes > 0) {
        const double storagePercent =
            double(storedBytes) * 100.0 / double(freeDatabaseBytes);
        const QString colour = storagePercent >= 80.0   ? palette.danger
                               : storagePercent >= 50.0 ? palette.warn
                                                        : palette.ok;
        html += QStringLiteral(
                    "<table width='100%' cellspacing='0' cellpadding='0'>"
                    "<tr><td style='padding:3px 0 0 0;color:%1;"
                    "font-size:11px;'>ForkMesh D1 storage</td>"
                    "<td align='right' style='padding:3px 0 0 0;"
                    "font-size:11px;'><b style='color:%2;'>%3%</b>"
                    "<span style='color:%4;'> · %5 / %6</span></td></tr>"
                    "<tr><td colspan='2' style='padding:0 0 4px 0;'>%7"
                    "</td></tr></table>")
                    .arg(palette.text, colour,
                         usageNumber(storagePercent, 1), palette.muted,
                         usageBytes(storedBytes),
                         usageBytes(freeDatabaseBytes),
                         usageMeter(storagePercent, colour, palette));
        QStringList limits;
        if (includedAccountBytes > 0)
            limits << QStringLiteral("%1 total storage included across the "
                                     "Free account")
                          .arg(usageBytes(includedAccountBytes));
        if (paidDatabaseBytes > 0)
            limits << QStringLiteral("%1 Paid per-database ceiling")
                          .arg(usageBytes(paidDatabaseBytes));
        if (!limits.isEmpty())
            html += usageNoticeParagraph(limits.join(QStringLiteral(" · ")),
                                         palette);
    } else {
        html += usageNoticeParagraph(
            QStringLiteral("The bound database did not return a storage "
                           "measurement."),
            palette);
    }

    if (haveReport) {
        const QJsonArray databases =
            report.value(QStringLiteral("databases")).toArray();
        html += usageSectionTitle(
            QStringLiteral("Today's usage by database"),
            databases.isEmpty()
                ? QStringLiteral("no D1 activity reported today")
                : QStringLiteral("%1 database%2, busiest first")
                      .arg(databases.size())
                      .arg(databases.size() == 1 ? QString()
                                                 : QStringLiteral("s")),
            palette);
        if (!databases.isEmpty()) {
            html += QStringLiteral("<table width='100%' cellspacing='0' "
                                   "cellpadding='0'>");
            html += usageHeaderRow(
                {QStringLiteral("Database"), QStringLiteral("Rows read"),
                 QStringLiteral("Rows written"), QStringLiteral("Read queries"),
                 QStringLiteral("Write queries")},
                palette);
            for (const QJsonValue &value : databases) {
                const QJsonObject row = value.toObject();
                const QString databaseId =
                    row.value(QStringLiteral("databaseId")).toString();
                html += QStringLiteral("<tr>");
                html += usageCell(
                    QStringLiteral("<b>%1</b><span style='color:%2;'> · %3"
                                   "</span>")
                        .arg(QStringLiteral("D1"), palette.muted,
                             usageShortId(databaseId).toHtmlEscaped()),
                    palette, true);
                for (const char *field :
                     {"rowsRead", "rowsWritten", "readQueries",
                      "writeQueries"})
                    html += usageCountCell(static_cast<qint64>(
                                               row.value(QLatin1String(field))
                                                   .toDouble()),
                                           palette);
                html += QStringLiteral("</tr>");
            }
            html += QStringLiteral("</table>");
        }
        if (report.value(QStringLiteral("detailTruncated")).toBool())
            html += usageNoticeParagraph(
                QStringLiteral("The per-database list is capped; the allowance "
                               "meters above still contain the complete "
                               "account-wide totals."),
                palette);
    }
    return html + QStringLiteral("</div>");
}

QString renderWorkerFootprintHtml(const QJsonObject &report, bool dark)
{
    const UsagePalette palette = usagePalette(dark);
    QString html = QStringLiteral("<div style='color:%1;'>").arg(palette.text);
    html += usageSectionTitle(
        QStringLiteral("Deployment growth budgets"),
        QStringLiteral("measured from the compact payload validated for upload"),
        palette);
    if (!report.value(QStringLiteral("ok")).toBool()) {
        const QString error = report.value(QStringLiteral("error")).toString();
        html += usageNoticeParagraph(
            error.isEmpty()
                ? QStringLiteral("Reading the deployed footprint report…")
                : QStringLiteral("The deployed footprint report could not be "
                                 "read (%1).").arg(error),
            palette);
        return html + QStringLiteral("</div>");
    }

    const QJsonObject staticLimits =
        report.value(QStringLiteral("staticLimits")).toObject();
    const QJsonObject largestAsset =
        report.value(QStringLiteral("largestStaticAsset")).toObject();
    auto budgetRow = [&](const QString &label, qint64 used, qint64 allowance,
                         bool bytes) {
        if (allowance <= 0)
            return;
        const double percent = double(used) * 100.0 / double(allowance);
        const QString colour = percent >= 80.0   ? palette.danger
                               : percent >= 50.0 ? palette.warn
                                                 : palette.ok;
        const QString usedText = bytes ? usageBytes(used) : usageCount(used);
        const QString allowanceText =
            bytes ? usageBytes(allowance) : usageCount(allowance);
        html += QStringLiteral(
                    "<table width='100%' cellspacing='0' cellpadding='0'>"
                    "<tr><td style='padding:3px 0 0 0;color:%1;"
                    "font-size:11px;'>%2</td>"
                    "<td align='right' style='padding:3px 0 0 0;"
                    "font-size:11px;'><b style='color:%3;'>%4%</b>"
                    "<span style='color:%5;'> · %6 / %7</span></td></tr>"
                    "<tr><td colspan='2' style='padding:0 0 4px 0;'>%8"
                    "</td></tr></table>")
                    .arg(palette.text, label.toHtmlEscaped(), colour,
                         usageNumber(percent, 1), palette.muted,
                         usedText.toHtmlEscaped(),
                         allowanceText.toHtmlEscaped(),
                         usageMeter(percent, colour, palette));
    };
    budgetRow(
        QStringLiteral("Initial World module graph"),
        static_cast<qint64>(report.value(
            QStringLiteral("initialWorldModuleBytes")).toDouble()),
        static_cast<qint64>(staticLimits.value(
            QStringLiteral("initialWorldModuleBytesSoft")).toDouble()),
        true);
    budgetRow(
        QStringLiteral("World static asset count"),
        static_cast<qint64>(report.value(
            QStringLiteral("staticAssetCount")).toDouble()),
        static_cast<qint64>(staticLimits.value(
            QStringLiteral("assetCount")).toDouble()),
        false);
    const QString largestName =
        largestAsset.value(QStringLiteral("name")).toString();
    budgetRow(
        largestName.isEmpty()
            ? QStringLiteral("Largest World static asset")
            : QStringLiteral("Largest static asset · %1").arg(largestName),
        static_cast<qint64>(largestAsset.value(
            QStringLiteral("bytes")).toDouble()),
        static_cast<qint64>(staticLimits.value(
            QStringLiteral("maxAssetBytes")).toDouble()),
        true);

    html += usageSectionTitle(
        QStringLiteral("Deployed footprint"),
        report.value(QStringLiteral("measurement")).toString(), palette);
    html += QStringLiteral("<table width='100%' cellspacing='0' "
                           "cellpadding='0'>");
    html += usageHeaderRow({QStringLiteral("Measurement"),
                            QStringLiteral("Current")}, palette);
    const auto totalRow = [&](const QString &label, const QString &value) {
        html += QStringLiteral("<tr>");
        html += usageCell(label.toHtmlEscaped(), palette, true);
        html += usageCell(value.toHtmlEscaped(), palette);
        html += QStringLiteral("</tr>");
    };
    totalRow(QStringLiteral("Estimated Python startup source"),
             usageBytes(static_cast<qint64>(report.value(
                 QStringLiteral("estimatedStartupSourceBytes")).toDouble())));
    totalRow(QStringLiteral("Attached Python source"),
             usageBytes(static_cast<qint64>(report.value(
                 QStringLiteral("attachedPythonBytes")).toDouble())));
    totalRow(QStringLiteral("On-demand Python source"),
             usageBytes(static_cast<qint64>(report.value(
                 QStringLiteral("onDemandSourceBytes")).toDouble())));
    totalRow(QStringLiteral("World static assets"),
             usageBytes(static_cast<qint64>(report.value(
                 QStringLiteral("staticAssetBytes")).toDouble())));
    totalRow(QStringLiteral("Attached Python modules"),
             usageCount(static_cast<qint64>(report.value(
                 QStringLiteral("moduleCount")).toDouble())));
    totalRow(QStringLiteral("Initial World modules"),
             usageCount(static_cast<qint64>(report.value(
                 QStringLiteral("initialWorldModuleCount")).toDouble())));
    html += QStringLiteral("</table>");

    const QJsonArray modules = report.value(QStringLiteral("modules")).toArray();
    if (!modules.isEmpty()) {
        html += usageSectionTitle(
            QStringLiteral("Largest Python modules"),
            QStringLiteral("source attribution, largest first"), palette);
        html += QStringLiteral("<table width='100%' cellspacing='0' "
                               "cellpadding='0'>");
        html += usageHeaderRow({QStringLiteral("Module"),
                                QStringLiteral("Load phase"),
                                QStringLiteral("Source")}, palette);
        const int shown = std::min(20, int(modules.size()));
        for (int index = 0; index < shown; ++index) {
            const QJsonObject module = modules.at(index).toObject();
            const QString phase =
                module.value(QStringLiteral("phase")).toString();
            html += QStringLiteral("<tr>");
            html += usageCell(
                QStringLiteral("<b>%1</b>")
                    .arg(module.value(QStringLiteral("name"))
                             .toString().toHtmlEscaped()),
                palette, true);
            html += usageCell(phase.toHtmlEscaped(), palette, false,
                              phase == QLatin1String("startup")
                                  ? palette.warn : palette.muted);
            html += usageCell(
                usageBytes(static_cast<qint64>(module.value(
                    QStringLiteral("bytes")).toDouble())), palette);
            html += QStringLiteral("</tr>");
        }
        html += QStringLiteral("</table>");
    }
    const QString note = report.value(QStringLiteral("note")).toString();
    if (!note.isEmpty())
        html += usageNoticeParagraph(note, palette);
    return html + QStringLiteral("</div>");
}

namespace {

// Minute slots are the cron's unit of time — the scheduler divides the epoch
// by the trigger interval and every cadence rule is arithmetic on the result
// — so the only useful way to read a due slot is the wait until it fires.
QString cronDueIn(qint64 minutes)
{
    if (minutes <= 0)
        return QStringLiteral("now");
    if (minutes < 60)
        return QStringLiteral("in %1 min").arg(minutes);
    if (minutes < 1440) {
        const qint64 rest = minutes % 60;
        return rest > 0
                   ? QStringLiteral("in %1h %2m").arg(minutes / 60).arg(rest)
                   : QStringLiteral("in %1h").arg(minutes / 60);
    }
    return QStringLiteral("in %1d").arg(minutes / 1440);
}

// Ticks are read as a sequence a minute apart, so wall-clock time tells them
// apart at a glance where "3m ago" on every row would not.
QString cronClock(qint64 msSinceEpoch)
{
    if (msSinceEpoch <= 0)
        return QString::fromUtf8("\xE2\x80\x94");
    return QDateTime::fromMSecsSinceEpoch(msSinceEpoch)
        .toString(QStringLiteral("HH:mm:ss"));
}

QString cronPhaseList(const QJsonArray &names, int limit)
{
    QStringList parts;
    for (const QJsonValue &value : names) {
        if (parts.size() >= limit) {
            parts << QStringLiteral("+%1").arg(names.size() - limit);
            break;
        }
        parts << value.toString();
    }
    return parts.join(QStringLiteral(", ")).toHtmlEscaped();
}

}  // namespace

QString renderCronActivityHtml(const QJsonObject &report, qint64 nowMs,
                               bool dark)
{
    const UsagePalette palette = usagePalette(dark);
    QString html = QStringLiteral("<div style='color:%1;'>").arg(palette.text);

    const QString error = report.value(QStringLiteral("error")).toString();
    if (!report.value(QStringLiteral("ok")).toBool()) {
        html += usageSectionTitle(QStringLiteral("Maintenance cron"), QString(),
                                  palette);
        if (error == QLatin1String("forbidden") ||
            error == QLatin1String("unauthorized"))
            // "forbidden" is this node deciding it cannot ask; "unauthorized"
            // is the relay rejecting the signature it did send. Both mean the
            // reader is not an admin here, which is the useful thing to say.
            html += usageNoticeParagraph(
                QStringLiteral("The maintenance schedule is admin-only, and "
                               "this node is not signed in as an admin."),
                palette);
        else if (!error.isEmpty())
            html += usageNoticeParagraph(
                QStringLiteral("The cron report could not be read (%1).")
                    .arg(error),
                palette);
        else
            html += usageNoticeParagraph(
                QString::fromUtf8("Reading the maintenance schedule from the "
                               "relay\xE2\x80\xA6"),
                palette);
        return html + QStringLiteral("</div>");
    }

    const QJsonObject trigger = report.value(QStringLiteral("trigger")).toObject();
    const QJsonObject summary = report.value(QStringLiteral("summary")).toObject();
    const QJsonArray phases = report.value(QStringLiteral("phases")).toArray();
    const QJsonArray ticks = report.value(QStringLiteral("ticks")).toArray();
    const QJsonArray settings = report.value(QStringLiteral("settings")).toArray();
    const qint64 currentSlot =
        static_cast<qint64>(report.value(QStringLiteral("slot")).toDouble());
    const qint64 observed =
        static_cast<qint64>(summary.value(QStringLiteral("ticks")).toDouble());

    html += usageSectionTitle(
        QStringLiteral("Maintenance loop"),
        QString::fromUtf8("cron %1 \xC2\xB7 %2 phases \xC2\xB7 at most %3 alarm "
                       "invocations a tick")
            .arg(trigger.value(QStringLiteral("cron"))
                     .toString(QStringLiteral("* * * * *")))
            .arg(static_cast<qint64>(
                trigger.value(QStringLiteral("phaseCount")).toDouble()))
            .arg(static_cast<qint64>(
                trigger.value(QStringLiteral("batchObjects")).toDouble())),
        palette);

    // The headline the tab exists for: how many Durable Object invocations a
    // minute of maintenance actually costs, against what the same phases cost
    // when each one woke an object of its own.
    const double perTick =
        summary.value(QStringLiteral("objectsPerTick")).toDouble();
    const double unbatched =
        summary.value(QStringLiteral("unbatchedObjectsPerTick")).toDouble();
    const qint64 gapMs =
        static_cast<qint64>(summary.value(QStringLiteral("gapMs")).toDouble());
    const qint64 failedTicks = static_cast<qint64>(
        summary.value(QStringLiteral("failedTicks")).toDouble());
    const qint64 intervalMs = qMax<qint64>(
        1, static_cast<qint64>(
               report.value(QStringLiteral("intervalMs")).toDouble()));

    html += QStringLiteral("<table width='100%' cellspacing='0' "
                           "cellpadding='0'>");
    html += usageHeaderRow({QStringLiteral("Measure"), QStringLiteral("Value"),
                            QStringLiteral("Against")},
                           palette);
    auto statRow = [&html, &palette](const QString &label,
                                     const QString &value,
                                     const QString &against,
                                     const QString &colour) {
        html += QStringLiteral("<tr>");
        html += usageCell(QStringLiteral("<b>%1</b>").arg(label.toHtmlEscaped()),
                          palette, true);
        html += usageCell(value, palette, false, colour);
        html += usageCell(against, palette, false, palette.muted);
        html += QStringLiteral("</tr>");
    };
    statRow(QStringLiteral("Alarm invocations per tick"),
            observed > 0 ? usageNumber(perTick, 2) : QString::fromUtf8("\xE2\x80\x94"),
            observed > 0 && unbatched > 0
                ? QString::fromUtf8("%1 unbatched \xC2\xB7 %2% saved")
                      .arg(usageNumber(unbatched, 2),
                           usageNumber((1.0 - perTick / unbatched) * 100.0, 0))
                : QStringLiteral("one object per due phase, before batching"),
            perTick > 0 && perTick <= unbatched ? palette.ok : palette.text);
    statRow(QStringLiteral("Phases per tick"),
            usageNumber(summary.value(QStringLiteral("phasesPerTick")).toDouble(),
                        2),
            QStringLiteral("%1 phase run%2 in the window")
                .arg(usageCount(static_cast<qint64>(
                    summary.value(QStringLiteral("phases")).toDouble())))
                .arg(summary.value(QStringLiteral("phases")).toDouble() == 1.0
                         ? QString()
                         : QStringLiteral("s")),
            palette.text);
    statRow(QStringLiteral("Subrequests per tick"),
            observed > 0
                ? usageNumber(
                      summary.value(QStringLiteral("subrequests")).toDouble() /
                          double(observed),
                      1)
                : QString::fromUtf8("\xE2\x80\x94"),
            QStringLiteral("%1 of the %2 one object may spend")
                .arg(usageCount(static_cast<qint64>(
                         summary.value(QStringLiteral("subrequests")).toDouble())))
                .arg(usageCount(static_cast<qint64>(
                    trigger.value(QStringLiteral("subrequestCap")).toDouble()))),
            palette.text);
    const qint64 lastTickAt = static_cast<qint64>(
        summary.value(QStringLiteral("lastTickAt")).toDouble());
    statRow(QStringLiteral("Last tick"),
            lastTickAt > 0 ? usageAgo(lastTickAt, nowMs)
                           : QString::fromUtf8("\xE2\x80\x94"),
            QStringLiteral("%1 tick%2 retained, %3 hour window")
                .arg(usageCount(observed))
                .arg(observed == 1 ? QString() : QStringLiteral("s"))
                .arg(static_cast<qint64>(
                         trigger.value(QStringLiteral("retainMs")).toDouble()) /
                     3'600'000),
            // A tick is a minute; three missed in a row is the loop stopping,
            // not a slow one. No history at all is neither, so it stays muted
            // rather than claiming a health it cannot know.
            lastTickAt <= 0          ? palette.muted
            : gapMs > 3 * intervalMs ? palette.danger
                                     : palette.ok);
    statRow(QStringLiteral("Ticks with a failure"),
            failedTicks > 0 ? usageCount(failedTicks)
                            : QStringLiteral("none"),
            QStringLiteral("a failing tick withholds the completion "
                           "heartbeat"),
            failedTicks > 0 ? palette.danger : palette.ok);
    html += QStringLiteral("</table>");

    if (error == QLatin1String("history_unavailable"))
        html += usageNoticeParagraph(
            QStringLiteral("The relay could not read its tick history (%1); "
                           "the schedule and settings below are still "
                           "current.")
                .arg(report.value(QStringLiteral("detail")).toString()),
            palette);
    else if (observed == 0)
        html += usageNoticeParagraph(
            QStringLiteral("No ticks recorded yet. The first minute after a "
                           "deploy writes the first row."),
            palette);

    // --- What the loop actually did, minute by minute.
    constexpr int kMaxTickRows = 60;
    html += usageSectionTitle(
        QStringLiteral("Recent ticks"),
        ticks.isEmpty()
            ? QStringLiteral("nothing retained")
            : QStringLiteral("newest first%1")
                  .arg(ticks.size() > kMaxTickRows
                           ? QStringLiteral(", %1 of %2 shown")
                                 .arg(kMaxTickRows)
                                 .arg(ticks.size())
                           : QString()),
        palette);
    if (!ticks.isEmpty()) {
        html += QStringLiteral("<table width='100%' cellspacing='0' "
                               "cellpadding='0'>");
        html += usageHeaderRow(
            {QStringLiteral("Tick"), QStringLiteral("Runs"),
             QStringLiteral("Phases"), QStringLiteral("Failed"),
             QStringLiteral("Sub-req"), QStringLiteral("Duration")},
            palette);
        int rendered = 0;
        for (const QJsonValue &value : ticks) {
            if (rendered++ >= kMaxTickRows)
                break;
            const QJsonObject tick = value.toObject();
            const QJsonArray ran = tick.value(QStringLiteral("ran")).toArray();
            const QJsonArray failed =
                tick.value(QStringLiteral("failed")).toArray();
            const QJsonArray skipped =
                tick.value(QStringLiteral("skipped")).toArray();
            QString detail = cronPhaseList(ran, 6);
            if (!skipped.isEmpty())
                detail += QString::fromUtf8(" \xC2\xB7 skipped %1")
                              .arg(cronPhaseList(skipped, 4));
            html += QStringLiteral("<tr>");
            html += usageCell(
                QStringLiteral("<b>%1</b>%2")
                    .arg(cronClock(static_cast<qint64>(
                             tick.value(QStringLiteral("ts")).toDouble())),
                         detail.isEmpty()
                             ? QString()
                             : QString::fromUtf8(
                                   "<span style='color:%1;'> \xC2\xB7 %2</span>")
                                   .arg(palette.muted, detail)),
                palette, true);
            html += usageCountCell(
                static_cast<qint64>(
                    tick.value(QStringLiteral("batches")).toDouble()),
                palette);
            html += usageCountCell(
                static_cast<qint64>(
                    tick.value(QStringLiteral("phases")).toDouble()),
                palette);
            html += failed.isEmpty()
                        ? usageCell(QString::fromUtf8("\xE2\x80\x94"), palette,
                                    false, palette.muted)
                        : usageCell(cronPhaseList(failed, 3), palette, false,
                                    palette.danger);
            html += usageCountCell(
                static_cast<qint64>(
                    tick.value(QStringLiteral("subrequests")).toDouble()),
                palette);
            html += usageCell(
                usageDuration(tick.value(QStringLiteral("ms")).toDouble()),
                palette);
            html += QStringLiteral("</tr>");
        }
        html += QStringLiteral("</table>");
    }

    // --- What it is scheduled to do, and what the window observed of each.
    html += usageSectionTitle(
        QStringLiteral("Scheduled phases"),
        phases.isEmpty()
            ? QStringLiteral("schedule unavailable")
            : QStringLiteral("%1 phase%2, most frequent first")
                  .arg(phases.size())
                  .arg(phases.size() == 1 ? QString() : QStringLiteral("s")),
        palette);
    if (!phases.isEmpty()) {
        html += QStringLiteral("<table width='100%' cellspacing='0' "
                               "cellpadding='0'>");
        html += usageHeaderRow(
            {QStringLiteral("Phase"), QStringLiteral("Cadence"),
             QStringLiteral("Next"), QStringLiteral("Runs"),
             QStringLiteral("Failed"), QStringLiteral("Skipped"),
             QStringLiteral("Last"), QStringLiteral("Time"),
             QStringLiteral("Peak sub")},
            palette);
        for (const QJsonValue &value : phases) {
            const QJsonObject row = value.toObject();
            QStringList notes;
            for (const QJsonValue &note :
                 row.value(QStringLiteral("notes")).toArray())
                notes << note.toString();
            const QString group = row.value(QStringLiteral("group")).toString();
            if (!group.isEmpty())
                notes.prepend(group);
            const QString lastFailure =
                row.value(QStringLiteral("lastFailure")).toString();
            const qint64 failures = static_cast<qint64>(
                row.value(QStringLiteral("failures")).toDouble());
            QString name =
                QStringLiteral("<b>%1</b>")
                    .arg(row.value(QStringLiteral("phase"))
                             .toString()
                             .toHtmlEscaped());
            QString sub = row.value(QStringLiteral("description")).toString();
            if (!notes.isEmpty())
                sub += QString::fromUtf8(" \xC2\xB7 %1")
                           .arg(notes.join(QStringLiteral(", ")));
            if (failures > 0 && !lastFailure.isEmpty())
                name += QStringLiteral(
                            "<br><span style='color:%1;font-size:10px;'>%2</span>")
                            .arg(palette.danger, lastFailure.toHtmlEscaped());
            name += QStringLiteral(
                        "<br><span style='color:%1;font-size:10px;'>%2</span>")
                        .arg(palette.muted, sub.toHtmlEscaped());
            const qint64 due = static_cast<qint64>(
                row.value(QStringLiteral("nextDueSlot")).toDouble());
            html += QStringLiteral("<tr>");
            html += usageCell(name, palette, true);
            html += usageCell(row.value(QStringLiteral("cadence"))
                                  .toString()
                                  .toHtmlEscaped(),
                              palette, false, palette.muted);
            html += usageCell(cronDueIn(due - currentSlot), palette, false,
                              palette.muted);
            html += usageCountCell(
                static_cast<qint64>(row.value(QStringLiteral("runs")).toDouble()),
                palette);
            html += usageCountCell(failures, palette, palette.danger);
            html += usageCountCell(
                static_cast<qint64>(
                    row.value(QStringLiteral("skips")).toDouble()),
                palette, palette.warn);
            html += usageCell(
                usageAgo(static_cast<qint64>(
                             row.value(QStringLiteral("lastRunAt")).toDouble()),
                         nowMs),
                palette, false, palette.muted);
            html += usageCell(
                usageDuration(row.value(QStringLiteral("lastMs")).toDouble()),
                palette);
            html += usageCountCell(
                static_cast<qint64>(
                    row.value(QStringLiteral("maxSubrequests")).toDouble()),
                palette);
            html += QStringLiteral("</tr>");
        }
        html += QStringLiteral("</table>");
    }

    // --- The switches that decide what a tick delivers.
    html += usageSectionTitle(
        QStringLiteral("Settings"),
        QStringLiteral("relay-owned; changing one is a deploy"), palette);
    if (settings.isEmpty()) {
        html += usageNoticeParagraph(
            QStringLiteral("The relay reported no cron settings."), palette);
    } else {
        html += QStringLiteral("<table width='100%' cellspacing='0' "
                               "cellpadding='0'>");
        html += usageHeaderRow({QStringLiteral("Setting"),
                                QStringLiteral("Value")},
                               palette);
        for (const QJsonValue &value : settings) {
            const QJsonObject row = value.toObject();
            html += QStringLiteral("<tr>");
            html += usageCell(
                QStringLiteral(
                    "<b>%1</b><br><span style='color:%2;font-size:10px;'>"
                    "%3</span>")
                    .arg(row.value(QStringLiteral("label"))
                             .toString()
                             .toHtmlEscaped(),
                         palette.muted,
                         row.value(QStringLiteral("detail"))
                             .toString()
                             .toHtmlEscaped()),
                palette, true);
            html += usageCell(row.value(QStringLiteral("value"))
                                  .toString()
                                  .toHtmlEscaped(),
                              palette);
            html += QStringLiteral("</tr>");
        }
        html += QStringLiteral("</table>");
    }
    return html + QStringLiteral("</div>");
}

QJsonObject parseCloudflareBootstrapResult(const QByteArray &output,
                                           QString *error)
{
    constexpr qsizetype kMaximumOutputBytes = 1024 * 1024;
    const QByteArray prefix("FORKMESH_BOOTSTRAP_RESULT=");
    if (output.size() > kMaximumOutputBytes) {
        if (error)
            *error = QStringLiteral("Cloudflare bootstrap output was too large.");
        return {};
    }
    QByteArray encoded;
    int matches = 0;
    for (const QByteArray &rawLine : output.split('\n')) {
        const QByteArray line = rawLine.trimmed();
        if (!line.startsWith(prefix))
            continue;
        ++matches;
        encoded = line.mid(prefix.size());
    }
    if (matches != 1 || encoded.isEmpty() || encoded.size() > 256 * 1024) {
        if (error)
            *error = QStringLiteral(
                "Cloudflare bootstrap returned no unique machine result.");
        return {};
    }
    const QByteArray json = QByteArray::fromBase64(
        encoded, QByteArray::Base64UrlEncoding |
                     QByteArray::AbortOnBase64DecodingErrors);
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (json.isEmpty() || parseError.error != QJsonParseError::NoError ||
        !document.isObject()) {
        if (error)
            *error = QStringLiteral(
                "Cloudflare bootstrap returned an invalid machine result.");
        return {};
    }
    const QJsonObject object = document.object();
    if (!object.value(QStringLiteral("ok")).toBool()) {
        if (error)
            *error = QStringLiteral(
                "Cloudflare bootstrap did not confirm a successful result.");
        return {};
    }
    if (error)
        error->clear();
    return object;
}

CloudflareBootstrapCommand buildCloudflareTunnelBootstrapCommand(
    const CloudflareBootstrapRequest &request,
    const QString &mirrorHostname,
    const QString &apiToken,
    const QString &scriptPath,
    const QString &pythonProgram,
    const QString &signerProgram,
    const QString &nodePublicKey,
    const QString &gatewayConfigPath,
    const QString &manifestOutputPath,
    const QString &connectorTokenPath)
{
    CloudflareBootstrapCommand command;
    command.program = pythonProgram.trimmed();
    command.arguments = {
        scriptPath,
        QStringLiteral("--hostname"),
        normalizedDnsName(mirrorHostname),
        QStringLiteral("--zone"),
        normalizedDnsName(request.zoneName),
        QStringLiteral("--node-name"),
        request.nodeName.trimmed().toLower(),
        QStringLiteral("--origin-host"),
        QStringLiteral("127.0.0.1"),
        QStringLiteral("--origin-port"),
        QStringLiteral("8790"),
        QStringLiteral("--gateway-config"),
        gatewayConfigPath,
        QStringLiteral("--mirror-public-key"),
        nodePublicKey.trimmed(),
        QStringLiteral("--manifest-signer-command"),
        shlexQuote(signerProgram.trimmed()) +
            QStringLiteral(" --sign-mirror-manifest"),
        QStringLiteral("--manifest-output"),
        manifestOutputPath,
        QStringLiteral("--tunnel-token-file"),
        connectorTokenPath,
    };
    if (!request.accountId.trimmed().isEmpty()) {
        command.arguments << QStringLiteral("--account-id")
                          << request.accountId.trimmed();
    }
    if (request.dryRun)
        command.arguments << QStringLiteral("--dry-run");
    command.environment = QProcessEnvironment::systemEnvironment();
    for (const QString &name : {
             QStringLiteral("CLOUDFLARE_API_TOKEN"),
             QStringLiteral("CF_API_TOKEN"),
             QStringLiteral("CLOUDFLARE_TOKEN"),
             QStringLiteral("CF_TOKEN"),
         }) {
        command.environment.remove(name);
    }
    command.environment.insert(
        QStringLiteral("CLOUDFLARE_API_TOKEN"), apiToken);
    command.environment.insert(
        QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
    return command;
}

QByteArray httpsMirrorRegistrationSigningPayload(
    const QString &node, const QString &baseUrl,
    const QString &publicKey, qint64 issuedAtMs, QString *error)
{
    static const QRegularExpression nodePattern(
        QStringLiteral("^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$"));
    static const QRegularExpression keyPattern(
        QStringLiteral("^[A-Za-z0-9_-]{43}$"));
    QUrl url(baseUrl.trimmed(), QUrl::StrictMode);
    if (!nodePattern.match(node).hasMatch() ||
        !keyPattern.match(publicKey).hasMatch() ||
        !url.isValid() ||
        url.scheme() != QLatin1String("https") ||
        url.host().isEmpty() || !url.userInfo().isEmpty() ||
        !url.query().isEmpty() || !url.fragment().isEmpty() ||
        (url.path() != QLatin1String("") &&
         url.path() != QLatin1String("/")) ||
        (url.port(-1) != -1 && url.port(-1) != 443) ||
        issuedAtMs <= 0) {
        if (error)
            *error = QStringLiteral(
                "The direct-HTTPS endpoint registration fields are invalid.");
        return {};
    }
    url.setScheme(QStringLiteral("https"));
    url.setPath(QString());
    url.setPort(-1);
    const QString normalized =
        url.toString(QUrl::RemoveUserInfo | QUrl::StripTrailingSlash)
            .toLower();
    if (error)
        error->clear();
    return QByteArrayLiteral("forkmesh-https-endpoint-v1\n") +
           node.toUtf8() + '\n' + normalized.toUtf8() + '\n' +
           publicKey.toUtf8() + '\n' +
           QByteArray::number(issuedAtMs);
}

QString redactProcessOutput(const QString &text,
                            const QStringList &exactSecrets)
{
    QString safe = text;
    safe.remove(QChar(u'\0'));
    static const QRegularExpression ansi(
        QStringLiteral("\\x1B(?:\\[[0-?]*[ -/]*[@-~]|\\][^\\x07]*(?:\\x07|\\x1B\\\\))"));
    safe.remove(ansi);

    for (const QString &secret : exactSecrets) {
        if (!secret.isEmpty())
            safe.replace(secret, QStringLiteral("<redacted>"));
    }

    static const QRegularExpression bearer(
        QStringLiteral("(?i)\\bBearer\\s+[A-Za-z0-9._~+/-]{8,}"));
    safe.replace(bearer, QStringLiteral("Bearer <redacted>"));
    static const QRegularExpression namedCredential(
        QStringLiteral(
            "(?i)\\b(CLOUDFLARE_API_TOKEN|CF_API_TOKEN|CLOUDFLARE_TOKEN|"
            "CF_TOKEN|authorization|api[_ -]?token|password|secret)"
            "(\\s*(?::|=)\\s*|\\s+Bearer\\s+)[^\\s,;]+"));
    safe.replace(namedCredential, QStringLiteral("\\1=<redacted>"));
    return safe;
}

bool isValidSolanaPublicAddress(const QString &address)
{
    const QByteArray input = address.trimmed().toLatin1();
    if (input.size() < 32 || input.size() > 44)
        return false;
    static const QByteArray alphabet(
        "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz");
    QByteArray decoded(1, '\0');
    for (const char ch : input) {
        const int value = alphabet.indexOf(ch);
        if (value < 0)
            return false;
        int carry = value;
        for (int i = decoded.size() - 1; i >= 0; --i) {
            carry += static_cast<unsigned char>(decoded.at(i)) * 58;
            decoded[i] = char(carry & 0xff);
            carry >>= 8;
        }
        while (carry > 0) {
            decoded.prepend(char(carry & 0xff));
            carry >>= 8;
        }
    }
    int leadingZeroes = 0;
    while (leadingZeroes < input.size() && input.at(leadingZeroes) == '1')
        ++leadingZeroes;
    while (!decoded.isEmpty() && decoded.front() == '\0')
        decoded.remove(0, 1);
    decoded.prepend(QByteArray(leadingZeroes, '\0'));
    return decoded.size() == 32;
}

QUrl worldUrlForRelay(const QString &relayUrl)
{
    QString value = relayUrl.trimmed();
    if (value.isEmpty())
        return {};
    if (!value.contains(QStringLiteral("://")))
        value.prepend(QStringLiteral("https://"));
    QUrl url(value);
    if (!url.isValid() || url.host().isEmpty())
        return {};
    if (url.scheme() == QLatin1String("wss"))
        url.setScheme(QStringLiteral("https"));
    else if (url.scheme() == QLatin1String("ws"))
        url.setScheme(QStringLiteral("http"));
    if (url.scheme() != QLatin1String("https") &&
        url.scheme() != QLatin1String("http")) {
        return {};
    }
    url.setUserInfo(QString());
    url.setPath(QStringLiteral("/world/"));
    url.setQuery(QString());
    url.setFragment(QString());
    return url;
}

QUrl worldDevServerUrl(const QString &configured)
{
    QString value = configured.trimmed();
    if (value.compare(QStringLiteral("off"), Qt::CaseInsensitive) == 0)
        return {};
    if (value.isEmpty())
        value = QStringLiteral("http://127.0.0.1:8788/world/");
    if (!value.contains(QStringLiteral("://")))
        value.prepend(QStringLiteral("http://"));
    QUrl url(value);
    if (!url.isValid() || url.scheme() != QLatin1String("http"))
        return {};
    const QString host = url.host().toLower();
    if (host != QLatin1String("localhost") &&
        host != QLatin1String("127.0.0.1") && host != QLatin1String("::1")) {
        return {};
    }
    url.setUserInfo(QString());
    if (url.path().isEmpty() || url.path() == QLatin1String("/"))
        url.setPath(QStringLiteral("/world/"));
    url.setQuery(QString());
    url.setFragment(QString());
    return url;
}

QStringList directMirrorRepositoryOwners(const QString &canonicalOwner,
                                         const QString &catalogOwner)
{
    static const QRegularExpression ownerPattern(
        QStringLiteral("^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$"));
    QStringList owners;
    for (const QString &raw : {canonicalOwner, catalogOwner}) {
        const QString owner = raw.trimmed().toLower();
        if (ownerPattern.match(owner).hasMatch() &&
            !owners.contains(owner)) {
            owners.append(owner);
        }
    }
    return owners;
}

bool isValidMirrorRouterPublicKey(const QString &value)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[A-Za-z0-9_-]{43}$"));
    if (!pattern.match(value).hasMatch())
        return false;
    return QByteArray::fromBase64(
               value.toLatin1(),
               QByteArray::Base64UrlEncoding |
                   QByteArray::AbortOnBase64DecodingErrors)
               .size() == 32;
}

bool directMirrorGatewayIsConfigured(const QString &nodeName,
                                     const QString &mirrorOrigin,
                                     const QString &routerPublicKey)
{
    static const QRegularExpression nodePattern(
        QStringLiteral("^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$"));
    return nodePattern.match(nodeName).hasMatch() &&
           !mirrorOrigin.isEmpty() &&
           isValidMirrorRouterPublicKey(routerPublicKey);
}

QByteArray mirrorManifestSigningPayload(const QJsonObject &request,
                                        const QString &expectedPublicKey,
                                        QString *error)
{
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return QByteArray();
    };
    if (!isBase64UrlPublicKey(expectedPublicKey))
        return fail(QStringLiteral("The local identity public key is invalid."));
    if (request.value(QStringLiteral("schemaVersion")).toInt() != 1 ||
        request.value(QStringLiteral("type")).toString() !=
            QLatin1String("forkmesh.mirror-endpoint-signing-request") ||
        request.value(QStringLiteral("algorithm")).toString() !=
            QLatin1String("Ed25519") ||
        request.value(QStringLiteral("encoding")).toString() !=
            QLatin1String("base64url-no-padding") ||
        request.value(QStringLiteral("canonicalization")).toString() !=
            QLatin1String("forkmesh-json-sort-v1")) {
        return fail(QStringLiteral("The manifest signing request has an unsupported format."));
    }
    if (request.value(QStringLiteral("publicKey")).toString() !=
        expectedPublicKey) {
        return fail(QStringLiteral("The manifest signing request targets another identity."));
    }
    const QString encoded =
        request.value(QStringLiteral("payloadBase64")).toString();
    if (encoded.isEmpty() || encoded.contains(QLatin1Char('=')) ||
        !QRegularExpression(QStringLiteral("^[A-Za-z0-9_-]+$"))
             .match(encoded)
             .hasMatch()) {
        return fail(QStringLiteral("The manifest signing payload is not valid base64url."));
    }
    const QByteArray payload = QByteArray::fromBase64(
        encoded.toLatin1(), QByteArray::Base64UrlEncoding);
    if (payload.isEmpty() || payload.size() > 64 * 1024)
        return fail(QStringLiteral("The manifest signing payload size is invalid."));
    const QString expectedDigest =
        QString::fromLatin1(
            QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex());
    const QString suppliedDigest =
        request.value(QStringLiteral("payloadSha256")).toString().toLower();
    if (suppliedDigest.size() != 64 || suppliedDigest != expectedDigest) {
        return fail(QStringLiteral("The manifest signing payload checksum does not match."));
    }
    const QJsonDocument manifest = QJsonDocument::fromJson(payload);
    if (!manifest.isObject())
        return fail(QStringLiteral("The manifest signing payload is not a JSON object."));
    const QJsonObject root = manifest.object();
    if (root.contains(QStringLiteral("signature")) ||
        root.value(QStringLiteral("type")).toString() !=
            QLatin1String("forkmesh.mirror-endpoint")) {
        return fail(QStringLiteral("The manifest signing payload has an invalid type."));
    }
    if (error)
        error->clear();
    return payload;
}

QString validateVultrMirrorRequest(const QString &apiKey,
                                   const QString &nodeName)
{
    static const QRegularExpression keyPattern(
        QStringLiteral("^[A-Za-z0-9]{20,128}$"));
    if (!keyPattern.match(apiKey.trimmed()).hasMatch()) {
        return QString::fromUtf8(
            "Enter your Vultr API key (Account \xE2\x86\x92 API in the Vultr "
            "panel). It is used from memory only and never saved to disk.");
    }
    static const QRegularExpression nodePattern(
        QStringLiteral("^[a-z][a-z0-9-]{0,62}$"));
    if (!nodePattern.match(nodeName.trimmed()).hasMatch()) {
        return QStringLiteral(
            "The node name must start with a letter and contain only lowercase "
            "letters, digits, or hyphens (63 characters maximum).");
    }
    return {};
}

bool vultrPlanHasIpv4(const QJsonObject &plan)
{
    const QString id =
        plan.value(QStringLiteral("id")).toString().trimmed().toLower();
    if (id.isEmpty())
        return false;
    return !id.endsWith(QLatin1String("-v6")) &&
           !id.contains(QLatin1String("-v6-"));
}

QJsonObject cheapestVultrPlan(const QJsonArray &plans)
{
    // ForkMesh keeps the authenticated public repository materialization in
    // private temporary storage while the durable copy remains age-encrypted.
    // Vultr's 512 MB plans mount /tmp at roughly half of RAM, which is smaller
    // than the flagship repository and makes an otherwise successful install
    // disappear during its first sync. One GiB is the minimum supported
    // automatic mirror size; operators can still install manually on custom
    // hosts whose temporary-storage layout meets the same runtime needs.
    constexpr double kMinimumMirrorRamMb = 1024.0;
    const auto hasUsLocation = [](const QJsonObject &plan) {
        static const QSet<QString> usRegions{
            QStringLiteral("ewr"), // Newark, New Jersey / New York metro
            QStringLiteral("atl"), // Atlanta
            QStringLiteral("ord"), // Chicago
            QStringLiteral("dfw"), // Dallas
            QStringLiteral("mia"), // Miami
            QStringLiteral("lax"), // Los Angeles
            QStringLiteral("sea"), // Seattle
            QStringLiteral("sjc"), // Silicon Valley
            QStringLiteral("hon"), // Honolulu
        };
        for (const QJsonValue &value :
             plan.value(QStringLiteral("locations")).toArray()) {
            if (usRegions.contains(value.toString().trimmed().toLower()))
                return true;
        }
        return false;
    };
    QJsonObject best;
    for (const QJsonValue &value : plans) {
        const QJsonObject plan = value.toObject();
        const double cost = plan.value(QStringLiteral("monthly_cost")).toDouble();
        const double ram = plan.value(QStringLiteral("ram")).toDouble();
        const QString id = plan.value(QStringLiteral("id")).toString();
        if (id.isEmpty() || !std::isfinite(cost) || cost <= 0.0 ||
            !std::isfinite(ram) || ram < kMinimumMirrorRamMb ||
            !vultrPlanHasIpv4(plan) || !hasUsLocation(plan)) {
            continue;
        }
        if (best.isEmpty()) {
            best = plan;
            continue;
        }
        const double bestCost =
            best.value(QStringLiteral("monthly_cost")).toDouble();
        if (cost < bestCost) {
            best = plan;
            continue;
        }
        if (cost > bestCost)
            continue;
        const double bestRam = best.value(QStringLiteral("ram")).toDouble();
        if (ram > bestRam ||
            (ram == bestRam &&
             id < best.value(QStringLiteral("id")).toString())) {
            best = plan;
        }
    }
    return best;
}

QString vultrPlanRegion(const QJsonObject &plan)
{
    static const QStringList preferredUsRegions{
        QStringLiteral("ewr"),
        QStringLiteral("atl"),
        QStringLiteral("ord"),
        QStringLiteral("dfw"),
        QStringLiteral("mia"),
        QStringLiteral("lax"),
        QStringLiteral("sea"),
        QStringLiteral("sjc"),
        QStringLiteral("hon"),
    };
    QSet<QString> locations;
    for (const QJsonValue &value :
         plan.value(QStringLiteral("locations")).toArray()) {
        const QString region = value.toString().trimmed().toLower();
        if (!region.isEmpty())
            locations.insert(region);
    }
    for (const QString &region : preferredUsRegions) {
        if (locations.contains(region))
            return region;
    }
    return {};
}

namespace {

// A plan's US regions in vultrPlanRegion's order of preference, found by
// asking it again with each answer removed so the order lives in one place.
QStringList vultrPlanRegions(const QJsonObject &plan)
{
    QStringList ordered;
    QJsonObject remaining = plan;
    for (int guard = 0; guard < 32; ++guard) {
        const QString region = vultrPlanRegion(remaining);
        if (region.isEmpty())
            break;
        ordered.append(region);
        QJsonArray left;
        for (const QJsonValue &value :
             remaining.value(QStringLiteral("locations")).toArray()) {
            if (value.toString().trimmed().toLower() != region)
                left.append(value);
        }
        remaining.insert(QStringLiteral("locations"), left);
    }
    return ordered;
}

bool vultrPlanEligible(const QJsonObject &plan)
{
    return !cheapestVultrPlan(QJsonArray{plan}).isEmpty();
}

} // namespace

QJsonObject vultrPlanChoices(const QJsonArray &plans)
{
    QList<QJsonObject> eligible;
    for (const QJsonValue &value : plans) {
        const QJsonObject plan = value.toObject();
        if (vultrPlanEligible(plan))
            eligible.append(plan);
    }
    std::sort(eligible.begin(), eligible.end(),
              [](const QJsonObject &a, const QJsonObject &b) {
                  const double ca = a.value(QStringLiteral("monthly_cost")).toDouble();
                  const double cb = b.value(QStringLiteral("monthly_cost")).toDouble();
                  if (ca != cb)
                      return ca < cb;
                  return a.value(QStringLiteral("id")).toString() <
                         b.value(QStringLiteral("id")).toString();
              });
    QJsonArray out;
    for (const QJsonObject &plan : std::as_const(eligible)) {
        out.append(QJsonObject{
            {QStringLiteral("id"), plan.value(QStringLiteral("id")).toString()},
            {QStringLiteral("monthlyCost"),
             plan.value(QStringLiteral("monthly_cost")).toDouble()},
            {QStringLiteral("ramMb"), plan.value(QStringLiteral("ram")).toDouble()},
            {QStringLiteral("diskGb"), plan.value(QStringLiteral("disk")).toDouble()},
            {QStringLiteral("vcpus"),
             plan.value(QStringLiteral("vcpu_count")).toDouble()},
            {QStringLiteral("regions"),
             QJsonArray::fromStringList(vultrPlanRegions(plan))},
        });
    }
    const QJsonObject best = cheapestVultrPlan(plans);
    return QJsonObject{
        {QStringLiteral("plans"), out},
        {QStringLiteral("defaultPlan"), best.value(QStringLiteral("id")).toString()},
        {QStringLiteral("defaultRegion"), vultrPlanRegion(best)},
    };
}

QJsonObject chooseVultrPlan(const QJsonArray &plans, const QString &requestedPlan,
                            const QString &requestedRegion, QString *region,
                            QString *error)
{
    QJsonObject plan;
    const QString wantedPlan = requestedPlan.trimmed();
    if (!wantedPlan.isEmpty()) {
        for (const QJsonValue &value : plans) {
            const QJsonObject candidate = value.toObject();
            if (candidate.value(QStringLiteral("id")).toString() == wantedPlan &&
                vultrPlanEligible(candidate)) {
                plan = candidate;
                break;
            }
        }
        if (plan.isEmpty()) {
            if (error)
                *error = QStringLiteral(
                    "Vultr plan \"%1\" is not one a mirror can run on.")
                             .arg(wantedPlan);
            return {};
        }
    } else {
        plan = cheapestVultrPlan(plans);
    }
    if (plan.isEmpty()) {
        if (error)
            *error = QStringLiteral(
                "No deployable US plan is available on this Vultr account.");
        return {};
    }
    const QStringList regions = vultrPlanRegions(plan);
    const QString wantedRegion = requestedRegion.trimmed().toLower();
    QString chosen;
    if (!wantedRegion.isEmpty()) {
        if (!regions.contains(wantedRegion)) {
            if (error)
                *error = QStringLiteral(
                    "Plan \"%1\" does not deploy in region \"%2\".")
                             .arg(plan.value(QStringLiteral("id")).toString(),
                                  wantedRegion);
            return {};
        }
        chosen = wantedRegion;
    } else {
        chosen = regions.value(0);
    }
    if (chosen.isEmpty()) {
        if (error)
            *error = QStringLiteral("The plan has no supported US region.");
        return {};
    }
    if (region)
        *region = chosen;
    return plan;
}

QJsonObject latestVultrDebianOs(const QJsonArray &osList)
{
    QJsonObject best;
    int bestVersion = -1;
    static const QRegularExpression versionPattern(
        QStringLiteral("\\b(\\d+)\\b"));
    for (const QJsonValue &value : osList) {
        const QJsonObject os = value.toObject();
        if (os.value(QStringLiteral("family")).toString().toLower() !=
                QLatin1String("debian") ||
            os.value(QStringLiteral("arch")).toString().toLower() !=
                QLatin1String("x64") ||
            os.value(QStringLiteral("id")).toInt() <= 0) {
            continue;
        }
        const QRegularExpressionMatch match =
            versionPattern.match(os.value(QStringLiteral("name")).toString());
        const int version = match.hasMatch() ? match.captured(1).toInt() : 0;
        if (version > bestVersion ||
            (version == bestVersion &&
             os.value(QStringLiteral("id")).toInt() >
                 best.value(QStringLiteral("id")).toInt())) {
            best = os;
            bestVersion = version;
        }
    }
    return best;
}

bool localBinaryRunsOnVultrMirror(const QString &kernelType,
                                  const QString &cpuArch)
{
    const QString os = kernelType.trimmed().toLower();
    QString arch = cpuArch.trimmed().toLower();
    if (arch == QLatin1String("amd64") || arch == QLatin1String("x64"))
        arch = QStringLiteral("x86_64");
    return os == QLatin1String("linux") && arch == QLatin1String("x86_64");
}

QJsonObject vultrInstanceCreatePayload(const QString &nodeName,
                                       const QString &planId,
                                       const QString &regionId,
                                       int osId,
                                       const QString &sshKeyId)
{
    return {
        {QStringLiteral("region"), regionId},
        {QStringLiteral("plan"), planId},
        {QStringLiteral("os_id"), osId},
        {QStringLiteral("label"), nodeName.trimmed()},
        {QStringLiteral("hostname"), nodeName.trimmed()},
        {QStringLiteral("sshkey_id"), QJsonArray{sshKeyId}},
        {QStringLiteral("backups"), QStringLiteral("disabled")},
        {QStringLiteral("enable_ipv6"), false},
        {QStringLiteral("activation_email"), false},
        {QStringLiteral("tags"),
         QJsonArray{QStringLiteral("forkmesh-mirror")}},
    };
}

QString vultrInstanceReadyIp(const QJsonObject &instance)
{
    if (instance.value(QStringLiteral("status")).toString() !=
            QLatin1String("active") ||
        instance.value(QStringLiteral("power_status")).toString() !=
            QLatin1String("running")) {
        return {};
    }
    const QString ip =
        instance.value(QStringLiteral("main_ip")).toString().trimmed();
    static const QRegularExpression ipv4Pattern(
        QStringLiteral("^(?:\\d{1,3}\\.){3}\\d{1,3}$"));
    if (ip.isEmpty() || ip == QLatin1String("0.0.0.0") ||
        !ipv4Pattern.match(ip).hasMatch()) {
        return {};
    }
    return ip;
}

bool vultrInstanceIsIpv6Only(const QJsonObject &instance)
{
    const QString v6 =
        instance.value(QStringLiteral("v6_main_ip")).toString().trimmed();
    if (v6.isEmpty() || !v6.contains(QLatin1Char(':')))
        return false;
    const QString ip =
        instance.value(QStringLiteral("main_ip")).toString().trimmed();
    return ip.isEmpty() || ip == QLatin1String("0.0.0.0");
}

QString nextMirrorNodeName(const QStringList &existingNames)
{
    static const QRegularExpression mirrorPattern(
        QStringLiteral("^mirror-?(\\d{1,4})$"));
    QSet<QString> used;
    int highest = 0;
    for (const QString &name : existingNames) {
        const QString normalized = name.trimmed().toLower();
        if (normalized.isEmpty())
            continue;
        used.insert(normalized);
        const QRegularExpressionMatch match = mirrorPattern.match(normalized);
        if (match.hasMatch())
            highest = std::max(highest, match.captured(1).toInt());
    }
    for (int i = std::max(1, highest + 1); i <= 9999; ++i) {
        const QString candidate = QStringLiteral("mirror%1").arg(i);
        if (!used.contains(candidate))
            return candidate;
    }
    return {};
}

QStringList vultrApiKeyVariableNames()
{
    return {
        QStringLiteral("VULTR_API_KEY"),
        QStringLiteral("VULTR_API_TOKEN"),
        QStringLiteral("VULTR_TOKEN"),
        QStringLiteral("VULTR_KEY"),
    };
}

QString vultrApiKeyFromVariables(const QMap<QString, QString> &variables)
{
    return storedCredential(variables, vultrApiKeyVariableNames());
}

bool vultrInstallNeedsLocalBinary(const QString &installOutput)
{
    // Matched on the installer's own wording for the two dead ends a retry
    // cannot clear: nothing in the mesh is serving the repo, and no prebuilt
    // release exists (or authenticates) for the instance's platform.
    static const QStringList markers = {
        QStringLiteral("No online ForkMesh node"),
        QStringLiteral("No prebuilt ForkMesh binary is published"),
        QStringLiteral("No prebuilt ForkMesh release passed"),
        QStringLiteral("falling back to a source build is disabled"),
    };
    for (const QString &marker : markers) {
        if (installOutput.contains(marker))
            return true;
    }
    return false;
}

namespace {
const char kVultrSshProbeMarker[] = "forkmesh-ssh-ready";
} // namespace

QString vultrSshProbeRemoteCommand()
{
    return QLatin1String("echo ") + QLatin1String(kVultrSshProbeMarker);
}

bool vultrSshProbeReady(int exitCode, const QString &outputTail)
{
    return exitCode == 0 &&
           outputTail.contains(QLatin1String(kVultrSshProbeMarker));
}

QString savedHostVultrInstanceId(const QJsonObject &host)
{
    const QString provider =
        host.value(QStringLiteral("provider")).toString().trimmed();
    if (provider.compare(QStringLiteral("Vultr"), Qt::CaseInsensitive) != 0)
        return {};
    return host.value(QStringLiteral("instanceId")).toString().trimmed();
}

QJsonObject mergeSavedHostRecord(const QJsonObject &previous,
                                 QJsonObject entry,
                                 const QJsonObject &metadata)
{
    // Every saved-host write goes through here so the provider facts survive a
    // status-only update — and so a host being saved for the FIRST time keeps
    // the metadata it was created with. The append path used to skip the
    // metadata merge entirely, so a freshly provisioned Vultr mirror was stored
    // with no instanceId: it was then invisible to the managed fleet, whose
    // reconciler saw zero healthy managed mirrors and created another server
    // every cycle, each one parked on "waiting for healthy public traffic".
    if (!entry.contains(QStringLiteral("identityFile")) &&
        previous.contains(QStringLiteral("identityFile"))) {
        entry.insert(QStringLiteral("identityFile"),
                     previous.value(QStringLiteral("identityFile")));
    }
    for (const QString &field : {QStringLiteral("provider"),
                                 QStringLiteral("planType"),
                                 QStringLiteral("displayName"),
                                 QStringLiteral("dnsHostname"),
                                 QStringLiteral("publicOrigin"),
                                 QStringLiteral("region"),
                                 QStringLiteral("monthlyCost"),
                                 QStringLiteral("ramMb"),
                                 QStringLiteral("diskGb"),
                                 QStringLiteral("instanceId")}) {
        if (previous.contains(field))
            entry.insert(field, previous.value(field));
    }
    for (auto it = metadata.constBegin(); it != metadata.constEnd(); ++it)
        entry.insert(it.key(), it.value());
    return entry;
}

QString vultrInstanceIdForAddress(const QJsonArray &instances,
                                  const QString &address)
{
    const QString wanted = address.trimmed().toLower();
    if (wanted.isEmpty())
        return {};
    QString match;
    for (const QJsonValue &value : instances) {
        const QJsonObject instance = value.toObject();
        const QString id = instance.value(QStringLiteral("id")).toString().trimmed();
        if (id.isEmpty())
            continue;
        bool hit = false;
        for (const QString &field : {QStringLiteral("main_ip"),
                                     QStringLiteral("v6_main_ip"),
                                     QStringLiteral("label"),
                                     QStringLiteral("hostname")}) {
            const QString candidate =
                instance.value(field).toString().trimmed().toLower();
            if (candidate.isEmpty() ||
                candidate == QLatin1String("0.0.0.0")) {
                continue;
            }
            if (candidate == wanted) {
                hit = true;
                break;
            }
        }
        if (!hit)
            continue;
        if (!match.isEmpty() && match != id)
            return {}; // ambiguous — fail closed rather than destroy a guess
        match = id;
    }
    return match;
}

QString validateVultrDestroyRequest(const QString &apiKey,
                                    const QString &instanceId)
{
    static const QRegularExpression keyPattern(
        QStringLiteral("^[A-Za-z0-9]{20,128}$"));
    if (!keyPattern.match(apiKey.trimmed()).hasMatch()) {
        return QString::fromUtf8(
            "Enter your Vultr API key (Account \xE2\x86\x92 API in the Vultr "
            "panel). It is used from memory only and never saved to disk.");
    }
    static const QRegularExpression idPattern(QStringLiteral(
        "^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-"
        "[0-9a-fA-F]{12}$"));
    if (!idPattern.match(instanceId.trimmed()).hasMatch()) {
        return QStringLiteral(
            "No Vultr instance is recorded for this host, so there is nothing "
            "safe to destroy. Delete it from the Vultr panel instead.");
    }
    return {};
}

bool mirrorCatalogEntryIsHealthy(const QJsonObject &mirror)
{
    return mirror.value(QStringLiteral("status"))
                   .toString()
                   .compare(QStringLiteral("online"),
                            Qt::CaseInsensitive) == 0 &&
           mirror.value(QStringLiteral("integrity"))
                   .toString()
                   .compare(QStringLiteral("ok"),
                            Qt::CaseInsensitive) == 0 &&
           mirror.value(QStringLiteral("lastSync")).toVariant().toLongLong() >
               0 &&
           mirror.value(QStringLiteral("cloneAvailable")).toBool() &&
           mirror.value(QStringLiteral("endpointHealthy")).toBool() &&
           mirror.value(QStringLiteral("endpointFresh")).toBool();
}

bool mirrorRepositoryProofServesContent(const QString &refsSha256)
{
    // SHA-256 of zero bytes: the refs digest of a repository with no refs at
    // all. A node freshly provisioned during a mesh outage signs proofs over
    // exactly this value — honest, healthy-looking, and serving nothing. On
    // 2026-08-19 such proofs passed deploy verification and direct fleet
    // discovery, so empty mirrors counted as healthy capacity and the
    // reconciler destroyed the last mirror that actually held data.
    static const QString kEmptyRefsSha256 = QStringLiteral(
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    const QString digest = refsSha256.trimmed().toLower();
    return digest.size() == 64 && digest != kEmptyRefsSha256;
}

QString mirrorSeedSshCommand(const QString &identityFile)
{
    QString quoted = identityFile;
    quoted.replace(QStringLiteral("'"), QStringLiteral("'\\''"));
    return QStringLiteral("ssh -i '") + quoted +
           QStringLiteral("' -o StrictHostKeyChecking=accept-new "
                          "-o ConnectTimeout=10");
}

QStringList mirrorSeedPushArguments(const QString &bareMirrorPath,
                                    const QString &address)
{
    const QString mirror = bareMirrorPath.trimmed();
    QString host = address.trimmed();
    if (mirror.isEmpty() || host.isEmpty())
        return {};
    if (host.contains(QLatin1Char(':')) && !host.startsWith(QLatin1Char('[')))
        host = QLatin1Char('[') + host + QLatin1Char(']');
    return {
        QStringLiteral("--git-dir=") + mirror,
        QStringLiteral("push"),
        QStringLiteral("--prune"),
        // Root SSH, node-owned objects: a plain root push leaves files the
        // daemon's own git refuses with "dubious ownership".
        QStringLiteral(
            "--receive-pack=runuser -u forkmesh-node -- git-receive-pack"),
        QStringLiteral("ssh://root@") + host +
            QStringLiteral(
                "/var/lib/forkmesh/repositories/forkmesh-forkmesh.git"),
        QStringLiteral("+refs/heads/*:refs/heads/*"),
        QStringLiteral("+refs/tags/*:refs/tags/*"),
    };
}

QString describeMirrorCatalogEntryGap(const QJsonObject &mirror)
{
    // Which half of the public traffic contract is still missing? Deploy
    // verification used to show only a poll counter, so a node that was
    // already installed and serving looked indistinguishable from one that
    // had failed — thirty minutes of "waiting for healthy public traffic"
    // with no cause on screen.
    if (mirror.isEmpty())
        return QStringLiteral("it has not published a catalog row yet");
    QStringList missing;
    if (mirror.value(QStringLiteral("status"))
            .toString()
            .compare(QStringLiteral("online"), Qt::CaseInsensitive) != 0) {
        missing.append(QStringLiteral("the mesh still lists it offline"));
    }
    if (mirror.value(QStringLiteral("lastSync")).toVariant().toLongLong() <= 0)
        missing.append(QStringLiteral("it has never published a sync"));
    if (mirror.value(QStringLiteral("integrity"))
            .toString()
            .compare(QStringLiteral("ok"), Qt::CaseInsensitive) != 0) {
        missing.append(QStringLiteral(
            "its published refs do not match a source-attested state"));
    }
    if (!mirror.value(QStringLiteral("cloneAvailable")).toBool())
        missing.append(QStringLiteral("it is not eligible to serve clones"));
    if (!mirror.value(QStringLiteral("endpointHealthy")).toBool()) {
        missing.append(QStringLiteral(
            "its direct-HTTPS endpoint is not answering healthy"));
    } else if (!mirror.value(QStringLiteral("endpointFresh")).toBool()) {
        missing.append(QStringLiteral("its endpoint lease has not been "
                                      "re-verified since it last changed"));
    }
    return missing.join(QStringLiteral("; "));
}

namespace {

QString decommissionedNodeKey(const QString &node)
{
    return node.trimmed().toLower();
}

QJsonObject pruneDecommissionedNodes(QJsonObject ledger, qint64 nowMs)
{
    QList<QPair<qint64, QString>> kept;
    for (auto it = ledger.constBegin(); it != ledger.constEnd(); ++it) {
        const qint64 stamp = qint64(it.value().toDouble());
        if (stamp <= 0 || nowMs - stamp > kDecommissionedNodeTtlMs)
            continue;
        kept.append({stamp, it.key()});
    }
    // Newest first, so an overflowing ledger drops the oldest tombstones —
    // the ones whose lease expired long ago and that suppress nothing.
    std::sort(kept.begin(), kept.end(),
              [](const QPair<qint64, QString> &a,
                 const QPair<qint64, QString> &b) {
                  return a.first != b.first ? a.first > b.first
                                            : a.second < b.second;
              });
    QJsonObject pruned;
    for (int i = 0; i < kept.size() && i < kDecommissionedNodeLimit; ++i)
        pruned.insert(kept.at(i).second, double(kept.at(i).first));
    return pruned;
}

} // namespace

QJsonObject recordDecommissionedNode(const QJsonObject &ledger,
                                     const QString &node, qint64 nowMs)
{
    const QString key = decommissionedNodeKey(node);
    if (key.isEmpty() || nowMs <= 0)
        return ledger;
    QJsonObject updated = ledger;
    updated.insert(key, double(nowMs));
    // Prune after inserting: the new entry is the newest, so it always
    // survives, and the ledger never grows past the cap by one.
    return pruneDecommissionedNodes(updated, nowMs);
}

QJsonObject forgetDecommissionedNode(const QJsonObject &ledger,
                                     const QString &node)
{
    const QString key = decommissionedNodeKey(node);
    if (key.isEmpty() || !ledger.contains(key))
        return ledger;
    QJsonObject updated = ledger;
    updated.remove(key);
    return updated;
}

bool mirrorNodeIsDecommissioned(const QJsonObject &ledger, const QString &node,
                                qint64 lastSeenMs, qint64 nowMs)
{
    const QString key = decommissionedNodeKey(node);
    if (key.isEmpty())
        return false;
    const qint64 stamp = qint64(ledger.value(key).toDouble());
    if (stamp <= 0 || (nowMs > 0 && nowMs - stamp > kDecommissionedNodeTtlMs))
        return false;
    // Heartbeats already in flight when the server died land just after the
    // stamp; a publication past the grace window means something is genuinely
    // alive under that name again.
    return lastSeenMs <= stamp + kDecommissionedNodeGraceMs;
}

bool agentCliCredentialsAreEmpty(const AgentCliCredentials &credentials)
{
    return credentials.claudeCredentials.trimmed().isEmpty() &&
           credentials.codexAuth.trimmed().isEmpty() &&
           agentCliEnvFileContents(credentials.env).isEmpty();
}

namespace {

bool agentCliEnvEntryIsUsable(const QString &name, const QString &value)
{
    static const QRegularExpression namePattern(
        QStringLiteral("^[A-Z][A-Z0-9_]{0,63}$"));
    if (!namePattern.match(name).hasMatch())
        return false;
    if (value.isEmpty() || value.size() > 4096)
        return false;
    for (const QChar ch : value) {
        if (ch.unicode() < 0x20 || ch.unicode() == 0x7f)
            return false;
    }
    return true;
}

} // namespace

QString describeAgentCliCredentials(const AgentCliCredentials &credentials)
{
    QStringList parts;
    if (!credentials.claudeCredentials.trimmed().isEmpty())
        parts.append(QStringLiteral("Claude Code login"));
    if (!credentials.codexAuth.trimmed().isEmpty())
        parts.append(QStringLiteral("Codex login"));
    QStringList names;
    for (auto it = credentials.env.constBegin();
         it != credentials.env.constEnd(); ++it) {
        if (agentCliEnvEntryIsUsable(it.key(), it.value()))
            names.append(it.key());
    }
    names.sort();
    parts += names;
    return parts.join(QStringLiteral(", "));
}

QByteArray agentCliEnvFileContents(const QMap<QString, QString> &env)
{
    QByteArray contents;
    for (auto it = env.constBegin(); it != env.constEnd(); ++it) {
        if (!agentCliEnvEntryIsUsable(it.key(), it.value()))
            continue;
        contents += QStringLiteral("export %1=%2\n")
                        .arg(it.key(), shellSingleQuote(it.value()))
                        .toUtf8();
    }
    return contents;
}

QByteArray buildAgentCliBootstrapPayload(const AgentCliCredentials &credentials,
                                         QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return QByteArray();
    };
    constexpr int kMaxSectionBytes = 256 * 1024;
    QByteArray payload;
    const auto appendSection = [&payload](const char *name,
                                          const QByteArray &data) {
        payload += QByteArray(name) + ' ' + data.toBase64() + '\n';
    };
    const QByteArray claude = credentials.claudeCredentials.trimmed();
    if (!claude.isEmpty()) {
        if (claude.size() > kMaxSectionBytes ||
            !QJsonDocument::fromJson(claude).isObject()) {
            return fail(QStringLiteral(
                "This device's Claude Code login file is not readable as a "
                "credential document."));
        }
        appendSection("claude", claude);
    }
    const QByteArray codex = credentials.codexAuth.trimmed();
    if (!codex.isEmpty()) {
        if (codex.size() > kMaxSectionBytes ||
            !QJsonDocument::fromJson(codex).isObject()) {
            return fail(QStringLiteral(
                "This device's Codex login file is not readable as a "
                "credential document."));
        }
        appendSection("codex", codex);
    }
    const QByteArray env = agentCliEnvFileContents(credentials.env);
    if (!env.isEmpty())
        appendSection("env", env);
    if (payload.isEmpty()) {
        return fail(QStringLiteral(
            "This device has no Claude Code or Codex login to copy."));
    }
    if (error)
        error->clear();
    return payload;
}

QString agentCliServicePublishScript()
{
    // Copy each freshly installed CLI beside the service copy, then rename it
    // over: a job already running keeps the binary it opened, and the next
    // job starts the new one. install(1) straight onto the target would
    // rewrite a file a running agent may still be reading.
    return QStringLiteral(
        "for program in claude codex; do "
        "source_path=$(command -v \"$program\") || continue; "
        "target=/usr/local/bin/$program; "
        "install -m 0755 \"$(readlink -f \"$source_path\")\" \"$target.new\" && "
        "mv -f \"$target.new\" \"$target\" || "
        "{ rm -f \"$target.new\"; echo \"Could not install $program for the "
        "forkmesh-node service.\"; }; done");
}

QString agentCliServiceRefreshScript()
{
    // Only CLIs the host already serves to forkmesh-node are refreshed, from
    // the same official installers the first install used, as root. The
    // mirror service cannot do this itself: it runs as forkmesh-node and
    // /usr/local/bin is root's. Never fatal, and no single quotes, so it can
    // sit inside any quoted remote command.
    return QStringLiteral(
        "( set +e; "
        "if test \"$(id -u)\" = 0 && id forkmesh-node >/dev/null 2>&1; then "
        "root_home=$(getent passwd root | cut -d: -f6); "
        "for program in claude codex; do "
        "test -x /usr/local/bin/$program || continue; "
        "if test $program = claude; then url=https://claude.ai/install.sh; "
        "else url=https://chatgpt.com/codex/install.sh; fi; "
        "echo \"Refreshing $program for the forkmesh-node service...\"; "
        "if curl -fsSL \"$url\" | HOME=\"$root_home\" bash; then "
        "source_path=$(PATH=\"$root_home/.local/bin:$root_home/.claude/bin:$PATH\" "
        "command -v $program) && "
        "install -m 0755 \"$(readlink -f \"$source_path\")\" "
        "\"/usr/local/bin/$program.new\" && "
        "mv -f \"/usr/local/bin/$program.new\" \"/usr/local/bin/$program\" && "
        "echo \"$program: $(/usr/local/bin/$program --version 2>&1 | head -n 1)\" || "
        "{ rm -f \"/usr/local/bin/$program.new\"; "
        "echo \"Could not replace $program; the previous copy stays.\"; }; "
        "else echo \"Could not download the $program installer; the previous "
        "copy stays.\"; fi; done; fi )");
}

QString withAgentCliServiceRefresh(const QString &pipeline)
{
    // The pipeline runs in a subshell, so its own exit (or set -e) ends only
    // that part; the refresh follows a successful update and the command
    // still exits with the update's status.
    return QStringLiteral("( %1 ); fm_update_status=$?; "
                          "if test \"$fm_update_status\" -eq 0; then %2; fi; "
                          "exit \"$fm_update_status\"")
        .arg(pipeline, agentCliServiceRefreshScript());
}

QString agentCliBootstrapRemoteCommand(bool withCredentials)
{
    // One fixed, secret-free command. The credential variant reads the stdin
    // payload into a private temp file *first*, so stdin is at EOF before the
    // piped installers run and no section can be mistaken for installer input.
    QStringList script;
    script << QStringLiteral("set -eu")
           << QStringLiteral("umask 077")
           << QStringLiteral("home=${HOME:-/root}");
    if (withCredentials) {
        script << QStringLiteral("tmp=$(mktemp)")
               << QStringLiteral("cat > \"$tmp\"")
               << QStringLiteral(
                      "sec() { sed -n \"s/^$1 //p\" \"$tmp\" | base64 -d; }")
               << QStringLiteral("has() { grep -q \"^$1 \" \"$tmp\"; }");
    }
    script << QStringLiteral("echo \"Installing Claude Code from claude.ai...\"")
           << QStringLiteral("curl -fsSL https://claude.ai/install.sh | bash")
           << QStringLiteral(
                  "echo \"Installing Codex from chatgpt.com...\"")
           << QStringLiteral(
                  "curl -fsSL https://chatgpt.com/codex/install.sh | sh")
           << QStringLiteral(
                  "export PATH=\"$home/.local/bin:$home/.claude/bin:$PATH\"")
           << QStringLiteral("agent_home=\"$home\"")
           << QStringLiteral("agent_owner=\"\"")
           << QStringLiteral(
                  "if test \"$(id -u)\" = 0 && id forkmesh-node "
                  ">/dev/null 2>&1; then "
                  "agent_home=$(getent passwd forkmesh-node | cut -d: -f6); "
                  "agent_owner=forkmesh-node; %1; "
                  "echo \"Installed agent CLIs for the forkmesh-node service.\"; "
                  "fi")
                  .arg(agentCliServicePublishScript());
    if (withCredentials) {
        script << QStringLiteral("if has claude; then mkdir -p \"$agent_home/.claude\"; "
                                 "sec claude > \"$agent_home/.claude/.credentials.json\"; "
                                 "chmod 600 \"$agent_home/.claude/.credentials.json\"; "
                                 "echo \"Copied the controller Claude Code login.\"; fi")
               << QStringLiteral("if has codex; then mkdir -p \"$agent_home/.codex\"; "
                                 "sec codex > \"$agent_home/.codex/auth.json\"; "
                                 "chmod 600 \"$agent_home/.codex/auth.json\"; "
                                 "echo \"Copied the controller Codex login.\"; fi")
               << QStringLiteral("if has env; then mkdir -p \"$agent_home/.forkmesh\"; "
                                 "sec env > \"$agent_home/.forkmesh/agent-env\"; "
                                 "chmod 600 \"$agent_home/.forkmesh/agent-env\"; "
                                 "for rc in \"$agent_home/.profile\" \"$agent_home/.bashrc\"; do "
                                 "touch \"$rc\"; "
                                 "grep -q .forkmesh/agent-env \"$rc\" || "
                                 "printf \"%s\\n\" \". \\\"$agent_home/.forkmesh/agent-env\\\"\" "
                                 ">> \"$rc\"; done; "
                                 "echo \"Installed the agent API keys in "
                                 "~/.forkmesh/agent-env.\"; fi")
               << QStringLiteral(
                      "if test -n \"$agent_owner\"; then "
                      "chown -R \"$agent_owner:$agent_owner\" "
                      "\"$agent_home/.claude\" \"$agent_home/.codex\" "
                      "\"$agent_home/.forkmesh\" "
                      "\"$agent_home/.profile\" \"$agent_home/.bashrc\" "
                      "2>/dev/null || true; fi")
               << QStringLiteral("rm -f \"$tmp\"");
    }
    script << QStringLiteral("echo \"Claude Code:\"")
           << QStringLiteral("command -v claude; claude --version")
           << QStringLiteral("echo \"Codex:\"")
           << QStringLiteral("command -v codex; codex --version")
           << (withCredentials
                   ? QStringLiteral(
                         "echo \"Installation complete. This mirror is signed "
                         "in with the controller agent access.\"")
                   : QStringLiteral(
                         "echo \"Installation complete. Provider login is "
                         "still required on this mirror.\""));
    return QStringLiteral("sh -lc '%1'")
        .arg(script.join(QStringLiteral("; ")));
}

QString cloudflareZoneNameFromVariables(
    const QMap<QString, QString> &variables)
{
    return storedCredential(variables,
                            {
                                QStringLiteral("CLOUDFLARE_ZONE"),
                                QStringLiteral("CLOUDFLARE_ZONE_NAME"),
                                QStringLiteral("CF_ZONE"),
                            });
}

QString vultrMirrorDnsHostname(const QString &nodeName,
                               const QString &zoneName)
{
    static const QRegularExpression labelPattern(
        QStringLiteral("^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$"));
    static const QRegularExpression zonePattern(
        QStringLiteral("^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?"
                       "(?:\\.[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?)+$"));
    const QString node = nodeName.trimmed().toLower();
    QString zone = zoneName.trimmed().toLower();
    while (zone.endsWith(QLatin1Char('.')))
        zone.chop(1);
    if (!labelPattern.match(node).hasMatch() ||
        !zonePattern.match(zone).hasMatch()) {
        return {};
    }
    const QString hostname = node + QLatin1Char('.') + zone;
    return hostname.size() <= 253 ? hostname : QString();
}

QJsonObject vultrMirrorDnsRecordPayload(const QString &hostname,
                                        const QString &ip)
{
    const QString name = hostname.trimmed().toLower();
    const QString address = ip.trimmed();
    static const QRegularExpression namePattern(
        QStringLiteral("^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?"
                       "(?:\\.[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?)+$"));
    if (!namePattern.match(name).hasMatch() || name.size() > 253)
        return {};
    const QStringList octets = address.split(QLatin1Char('.'));
    if (octets.size() != 4 || address == QLatin1String("0.0.0.0"))
        return {};
    for (const QString &octet : octets) {
        bool ok = false;
        const int value = octet.toInt(&ok);
        if (!ok || octet.isEmpty() || octet.size() > 3 || value < 0 ||
            value > 255) {
            return {};
        }
    }
    return {
        {QStringLiteral("type"), QStringLiteral("A")},
        {QStringLiteral("name"), name},
        {QStringLiteral("content"), address},
        {QStringLiteral("proxied"), false},
        {QStringLiteral("ttl"), 1},
        {QStringLiteral("comment"), QStringLiteral("ForkMesh mirror node")},
    };
}

QString cloudflareZoneId(const QJsonArray &zones, const QString &zoneName)
{
    QString zone = zoneName.trimmed().toLower();
    while (zone.endsWith(QLatin1Char('.')))
        zone.chop(1);
    if (zone.isEmpty())
        return {};
    static const QRegularExpression idPattern(
        QStringLiteral("^[A-Za-z0-9_-]{1,128}$"));
    QString found;
    for (const QJsonValue &value : zones) {
        const QJsonObject candidate = value.toObject();
        if (candidate.value(QStringLiteral("name")).toString().trimmed().toLower() !=
            zone) {
            continue;
        }
        const QString id = candidate.value(QStringLiteral("id")).toString().trimmed();
        if (!idPattern.match(id).hasMatch())
            return {};
        if (!found.isEmpty() && found != id)
            return {};
        found = id;
    }
    return found;
}

QString cloudflareDnsRecordId(const QJsonArray &records,
                              const QString &hostname,
                              const QString &recordType)
{
    const QString name = hostname.trimmed().toLower();
    const QString type = recordType.trimmed().toUpper();
    if (name.isEmpty() || type.isEmpty())
        return {};
    static const QRegularExpression idPattern(
        QStringLiteral("^[A-Za-z0-9_-]{1,128}$"));
    QString found;
    for (const QJsonValue &value : records) {
        const QJsonObject record = value.toObject();
        if (record.value(QStringLiteral("name")).toString().trimmed().toLower() !=
                name ||
            record.value(QStringLiteral("type")).toString().trimmed().toUpper() !=
                type) {
            continue;
        }
        const QString id = record.value(QStringLiteral("id")).toString().trimmed();
        if (!idPattern.match(id).hasMatch())
            return {};
        if (!found.isEmpty() && found != id)
            return {};
        found = id;
    }
    return found;
}


namespace {

const QRegularExpression &cloudflareIdPattern()
{
    static const QRegularExpression pattern(
        QStringLiteral("^[A-Za-z0-9]{1,64}$"));
    return pattern;
}

// The scope string Cloudflare tags a permission group with, and the resource
// key prefix a policy for it must use.
QString cloudflareScopeString(const QString &scope)
{
    if (scope == QLatin1String("account"))
        return QStringLiteral("com.cloudflare.api.account");
    if (scope == QLatin1String("zone"))
        return QStringLiteral("com.cloudflare.api.account.zone");
    if (scope == QLatin1String("user"))
        return QStringLiteral("com.cloudflare.api.user");
    return {};
}

QJsonObject cloudflareResultObject(const QJsonObject &response)
{
    const QJsonValue result = response.value(QStringLiteral("result"));
    return result.isObject() ? result.toObject() : response;
}

// code-health: allow(deep-nested-loop) walks one API token's policy JSON: a handful of policies, resources and nested keys
QStringList cloudflarePolicyResourceKeys(const QJsonObject &tokenDetail)
{
    QStringList keys;
    const QJsonArray policies =
        cloudflareResultObject(tokenDetail)
            .value(QStringLiteral("policies"))
            .toArray();
    for (const QJsonValue &value : policies) {
        const QJsonObject resources =
            value.toObject().value(QStringLiteral("resources")).toObject();
        for (auto it = resources.constBegin(); it != resources.constEnd();
             ++it) {
            keys.append(it.key());
            if (!it.value().isObject())
                continue;
            const QJsonObject nested = it.value().toObject();
            for (auto inner = nested.constBegin(); inner != nested.constEnd();
                 ++inner) {
                keys.append(inner.key());
            }
        }
    }
    keys.removeDuplicates();
    return keys;
}

// Resolve one permission group's id from GET /user/tokens/permission_groups.
// A name is only accepted when the catalog entry also carries the scope the
// requirement is bound to: several group names (e.g. "Logs Read") exist at both
// account and zone scope with different ids.
QString cloudflarePermissionGroupId(const QJsonArray &catalog,
                                    const QString &name, const QString &scope)
{
    const QString wantedScope = cloudflareScopeString(scope);
    if (wantedScope.isEmpty())
        return {};
    for (const QJsonValue &value : catalog) {
        const QJsonObject group = value.toObject();
        if (group.value(QStringLiteral("name"))
                .toString()
                .trimmed()
                .compare(name, Qt::CaseInsensitive) != 0) {
            continue;
        }
        bool scoped = false;
        const QJsonArray scopes =
            group.value(QStringLiteral("scopes")).toArray();
        for (const QJsonValue &entry : scopes) {
            if (entry.toString().trimmed() == wantedScope) {
                scoped = true;
                break;
            }
        }
        if (!scoped)
            continue;
        const QString id =
            group.value(QStringLiteral("id")).toString().trimmed();
        if (cloudflareIdPattern().match(id).hasMatch())
            return id;
    }
    return {};
}

} // namespace

QList<CloudflareTokenRequirement> cloudflareTokenRequirements()
{
    return {
        {QStringLiteral("account_settings"),
         QStringLiteral("Account Settings: Read"),
         QStringLiteral("Discovers the account that owns the Worker, the D1 "
                        "database and the Tunnel."),
         {QStringLiteral("Account Settings Read"),
          QStringLiteral("Account Settings Write")},
         QStringLiteral("account"),
         QStringLiteral("/accounts?per_page=1"),
         true},
        {QStringLiteral("workers_scripts"),
         QStringLiteral("Workers Scripts: Edit"),
         QStringLiteral("Uploads the relay Worker with its static assets, "
                        "Durable Objects, cron triggers and secrets."),
         {QStringLiteral("Workers Scripts Write")},
         QStringLiteral("account"),
         QStringLiteral("/accounts/{account}/workers/scripts"),
         true},
        {QStringLiteral("d1"),
         QStringLiteral("D1: Edit"),
         QStringLiteral("Creates and migrates the forkmesh D1 database the "
                        "Worker binds as DB."),
         {QStringLiteral("D1 Write")},
         QStringLiteral("account"),
         QStringLiteral("/accounts/{account}/d1/database?per_page=1"),
         true},
        {QStringLiteral("zone"),
         QStringLiteral("Zone: Read"),
         QStringLiteral("Finds the zone the relay and mirror hostnames are "
                        "derived from."),
         {QStringLiteral("Zone Read"), QStringLiteral("Zone Write")},
         QStringLiteral("zone"),
         QStringLiteral("/zones?per_page=1"),
         true},
        {QStringLiteral("dns"),
         QStringLiteral("DNS: Edit"),
         QStringLiteral("Publishes the proxied relay and mirror records, plus "
                        "the A record of each one-click mirror."),
         {QStringLiteral("DNS Write")},
         QStringLiteral("zone"),
         QStringLiteral("/zones/{zone}/dns_records?per_page=1"),
         true},
        {QStringLiteral("tunnel"),
         QStringLiteral("Cloudflare Tunnel: Edit"),
         QStringLiteral("Provisions the direct-HTTPS mirror gateway's Tunnel "
                        "and its connector credential."),
         {QStringLiteral("Cloudflare Tunnel Write"),
          QStringLiteral("Argo Tunnel Write")},
         QStringLiteral("account"),
         QStringLiteral("/accounts/{account}/cfd_tunnel?per_page=1"),
         true},
        {QStringLiteral("firewall"),
         QStringLiteral("Firewall Services: Edit"),
         QStringLiteral("Blocks the site's attacking web visitors at the edge from "
                        "Agent activity > Visitors: IP Access Rules by address, range, "
                        "network or country, and User Agent Blocking rules."),
         {QStringLiteral("Firewall Services Write")},
         QStringLiteral("zone"),
         QStringLiteral("/zones/{zone}/firewall/access_rules/rules?per_page=1"),
         false},
        {QStringLiteral("workers_tail"),
         QStringLiteral("Workers Tail: Read"),
         QStringLiteral("Streams the live Worker log shown in Network > Logs."),
         {QStringLiteral("Workers Tail Read")},
         QStringLiteral("account"),
         QString(),
         false},
        {QStringLiteral("api_tokens_read"),
         QStringLiteral("API Tokens: Read"),
         QStringLiteral("Lets this page report the token's exact policy instead "
                        "of inferring it from read-only probes."),
         {QStringLiteral("API Tokens Read")},
         QStringLiteral("user"),
         QStringLiteral("/user/tokens?per_page=1"),
         false},
        {QStringLiteral("api_tokens_write"),
         QStringLiteral("API Tokens: Edit"),
         QStringLiteral("Required by this page's own button that mints a "
                        "replacement token."),
         {QStringLiteral("API Tokens Write")},
         QStringLiteral("user"),
         QString(),
         false},
    };
}

QString cloudflareTokenProbePath(const CloudflareTokenRequirement &requirement,
                                 const QString &accountId,
                                 const QString &zoneId)
{
    QString path = requirement.probePath;
    if (path.isEmpty())
        return {};
    const auto substitute = [&path](const QString &placeholder,
                                    const QString &id) {
        if (!path.contains(placeholder))
            return true;
        const QString value = id.trimmed();
        if (!cloudflareIdPattern().match(value).hasMatch())
            return false;
        path.replace(placeholder, value);
        return true;
    };
    if (!substitute(QStringLiteral("{account}"), accountId) ||
        !substitute(QStringLiteral("{zone}"), zoneId)) {
        return {};
    }
    return path;
}

bool isPlausibleCloudflareApiToken(const QString &token)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[A-Za-z0-9_-]{20,160}$"));
    return pattern.match(token).hasMatch();
}

QString cloudflareTokenVerifyStatus(const QJsonObject &verifyResult)
{
    return cloudflareResultObject(verifyResult)
        .value(QStringLiteral("status"))
        .toString()
        .trimmed()
        .toLower();
}

QString cloudflareTokenVerifyId(const QJsonObject &verifyResult)
{
    const QString id = cloudflareResultObject(verifyResult)
                           .value(QStringLiteral("id"))
                           .toString()
                           .trimmed();
    return cloudflareIdPattern().match(id).hasMatch() ? id : QString();
}

QStringList cloudflareTokenPermissionGroupNames(const QJsonObject &tokenDetail)
{
    QStringList names;
    const QJsonArray policies =
        cloudflareResultObject(tokenDetail)
            .value(QStringLiteral("policies"))
            .toArray();
    for (const QJsonValue &value : policies) {
        const QJsonObject policy = value.toObject();
        // A deny policy subtracts access; reporting its groups as granted would
        // be a false positive on exactly the permission that is missing.
        if (policy.value(QStringLiteral("effect")).toString().trimmed().toLower() ==
            QLatin1String("deny")) {
            continue;
        }
        const QJsonArray groups =
            policy.value(QStringLiteral("permission_groups")).toArray();
        for (const QJsonValue &entry : groups) {
            const QString name = entry.toObject()
                                     .value(QStringLiteral("name"))
                                     .toString()
                                     .trimmed();
            if (!name.isEmpty())
                names.append(name);
        }
    }
    names.removeDuplicates();
    names.sort(Qt::CaseInsensitive);
    return names;
}

bool cloudflareTokenGrantsRequirement(
    const CloudflareTokenRequirement &requirement,
    const QStringList &grantedGroupNames)
{
    for (const QString &accepted : requirement.groupNames) {
        for (const QString &granted : grantedGroupNames) {
            if (granted.compare(accepted, Qt::CaseInsensitive) == 0)
                return true;
        }
    }
    return false;
}

QStringList cloudflareTokenAccountIds(const QJsonObject &tokenDetail)
{
    static const QRegularExpression accountKey(
        QStringLiteral("^com\\.cloudflare\\.api\\.account\\.([A-Za-z0-9]{1,64})$"));
    QStringList ids;
    const QStringList keys = cloudflarePolicyResourceKeys(tokenDetail);
    for (const QString &key : keys) {
        const QRegularExpressionMatch match = accountKey.match(key);
        if (match.hasMatch())
            ids.append(match.captured(1));
    }
    ids.removeDuplicates();
    return ids;
}

QString cloudflareTokenUserResourceKey(const QJsonObject &tokenDetail)
{
    static const QRegularExpression userKey(
        QStringLiteral("^com\\.cloudflare\\.api\\.user\\.[A-Za-z0-9]{1,64}$"));
    const QStringList keys = cloudflarePolicyResourceKeys(tokenDetail);
    for (const QString &key : keys) {
        if (userKey.match(key).hasMatch())
            return key;
    }
    return {};
}

QJsonObject cloudflareTokenCreatePayload(
    const QString &tokenName, const QString &accountId, const QString &zoneId,
    const QString &userResourceKey, const QJsonArray &permissionGroupCatalog,
    QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return QJsonObject();
    };
    if (error)
        error->clear();

    const QString name = tokenName.trimmed();
    static const QRegularExpression namePattern(
        QStringLiteral("^[A-Za-z0-9][A-Za-z0-9 ._:-]{0,119}$"));
    if (!namePattern.match(name).hasMatch())
        return fail(QStringLiteral("The token name is not a valid Cloudflare "
                                   "token name."));
    const QString account = accountId.trimmed();
    const QString zone = zoneId.trimmed();
    const QString user = userResourceKey.trimmed();
    static const QRegularExpression userKey(
        QStringLiteral("^com\\.cloudflare\\.api\\.user\\.[A-Za-z0-9]{1,64}$"));
    if (!account.isEmpty() && !cloudflareIdPattern().match(account).hasMatch())
        return fail(QStringLiteral("The Cloudflare account ID is malformed."));
    if (!zone.isEmpty() && !cloudflareIdPattern().match(zone).hasMatch())
        return fail(QStringLiteral("The Cloudflare zone ID is malformed."));
    if (!user.isEmpty() && !userKey.match(user).hasMatch())
        return fail(QStringLiteral("The Cloudflare user resource is malformed."));

    QJsonArray accountGroups;
    QJsonArray zoneGroups;
    QJsonArray userGroups;
    const QList<CloudflareTokenRequirement> requirements =
        cloudflareTokenRequirements();
    for (const CloudflareTokenRequirement &requirement : requirements) {
        const bool haveResource =
            (requirement.scope == QLatin1String("account") && !account.isEmpty()) ||
            (requirement.scope == QLatin1String("zone") && !zone.isEmpty()) ||
            (requirement.scope == QLatin1String("user") && !user.isEmpty());
        QString groupId;
        QString groupName;
        if (haveResource) {
            for (const QString &candidate : requirement.groupNames) {
                groupId = cloudflarePermissionGroupId(permissionGroupCatalog,
                                                      candidate,
                                                      requirement.scope);
                if (!groupId.isEmpty()) {
                    groupName = candidate;
                    break;
                }
            }
        }
        if (groupId.isEmpty()) {
            if (!requirement.required)
                continue;
            if (!haveResource) {
                return fail(
                    QStringLiteral("%1 needs a Cloudflare %2 ID; none is "
                                   "configured on this page.")
                        .arg(requirement.label, requirement.scope));
            }
            return fail(
                QStringLiteral("Cloudflare's permission-group catalog has no "
                               "%1 group for %2.")
                    .arg(requirement.groupNames.value(0), requirement.label));
        }
        const QJsonObject group{
            {QStringLiteral("id"), groupId},
            {QStringLiteral("name"), groupName},
        };
        if (requirement.scope == QLatin1String("account"))
            accountGroups.append(group);
        else if (requirement.scope == QLatin1String("zone"))
            zoneGroups.append(group);
        else
            userGroups.append(group);
    }

    QJsonArray policies;
    const auto addPolicy = [&policies](const QString &resourceKey,
                                       const QJsonArray &groups) {
        if (groups.isEmpty())
            return;
        policies.append(QJsonObject{
            {QStringLiteral("effect"), QStringLiteral("allow")},
            {QStringLiteral("resources"),
             QJsonObject{{resourceKey, QStringLiteral("*")}}},
            {QStringLiteral("permission_groups"), groups},
        });
    };
    addPolicy(QStringLiteral("com.cloudflare.api.account.") + account,
              accountGroups);
    addPolicy(QStringLiteral("com.cloudflare.api.account.zone.") + zone,
              zoneGroups);
    addPolicy(user, userGroups);
    if (policies.isEmpty())
        return fail(QStringLiteral("No Cloudflare permissions could be resolved "
                                   "for a replacement token."));
    return {
        {QStringLiteral("name"), name},
        {QStringLiteral("policies"), policies},
    };
}

QString cloudflareCreatedTokenValue(const QJsonObject &createResult)
{
    const QString value = cloudflareResultObject(createResult)
                              .value(QStringLiteral("value"))
                              .toString()
                              .trimmed();
    return isPlausibleCloudflareApiToken(value) ? value : QString();
}

QString updatedEnvAssignment(const QString &contents, const QString &name,
                             const QString &value)
{
    static const QRegularExpression namePattern(
        QStringLiteral("^[A-Za-z_][A-Za-z0-9_]{0,127}$"));
    if (!namePattern.match(name).hasMatch() ||
        value.contains(QLatin1Char('\n')) ||
        value.contains(QLatin1Char('\r'))) {
        return contents;
    }
    const QString assignment = name + QLatin1Char('=') + value;
    const QRegularExpression linePattern(
        QStringLiteral("^[ \\t]*") + QRegularExpression::escape(name) +
        QStringLiteral("[ \\t]*="));

    QStringList lines = contents.split(QLatin1Char('\n'));
    bool replaced = false;
    for (int index = 0; index < lines.size();) {
        QString line = lines.at(index);
        const bool carriage = line.endsWith(QLatin1Char('\r'));
        if (carriage)
            line.chop(1);
        if (!linePattern.match(line).hasMatch()) {
            ++index;
            continue;
        }
        if (!replaced) {
            lines[index] = carriage ? assignment + QLatin1Char('\r')
                                    : assignment;
            replaced = true;
            ++index;
            continue;
        }
        lines.removeAt(index);
    }
    if (!replaced) {
        while (!lines.isEmpty() && lines.constLast().trimmed().isEmpty())
            lines.removeLast();
        lines.append(assignment);
    }
    QString result = lines.join(QLatin1Char('\n'));
    if (!result.endsWith(QLatin1Char('\n')))
        result.append(QLatin1Char('\n'));
    return result;
}

bool completeSystemDataIsBulk(const QString &topLevelName)
{
    // Everything here is either re-fetchable from the mesh or pure cache, and
    // every one of them can reach gigabytes. The export page lists them with
    // their real size so the operator can still tick one on deliberately.
    static const QSet<QString> bulk{
        QStringLiteral("actions"),                // CI sandboxes and run logs
        QStringLiteral("agent-accounts"),         // per-account agent CLI homes
        QStringLiteral("agent-images"),
        QStringLiteral("agents"),                 // agent worktrees and scratch
        QStringLiteral("avatars"),
        QStringLiteral("deploy-backups"),
        QStringLiteral("favicons"),
        QStringLiteral("mirrors"),                // re-clonable bare mirrors
        QStringLiteral("network_log.txt"),
        QStringLiteral("network_log.d"),          // rotated 30-day log archive
        QStringLiteral("public-mirror-archives"),
        QStringLiteral("pull-meta"),              // cached forkmesh/pulls copies
        QStringLiteral("pull-requests"),
        QStringLiteral("repo-preview-cache"),
        QStringLiteral("cache"),
        QStringLiteral("logs"),
    };
    QString name = topLevelName.trimmed();
    if (name.startsWith(QLatin1String("core/")))
        name = name.mid(5);
    return bulk.contains(name);
}

// code-health: allow(deep-nested-loop, dir-walk-in-loop) only called from QtConcurrent::run in MainWindow::startSystemExportSurvey()
QList<CompleteSystemDataEntry> surveyCompleteSystemApplicationData(
    const QString &applicationDataDirectory)
{
    QList<CompleteSystemDataEntry> entries;
    const QString appData = QDir(applicationDataDirectory).absolutePath();
    const QFileInfo rootInfo(appData);
    if (!rootInfo.exists() || !rootInfo.isDir() || rootInfo.isSymLink())
        return entries;

    const QDir::Filters filters = QDir::AllEntries | QDir::NoDotAndDotDot |
                                  QDir::Hidden | QDir::System;
    QList<QPair<QString, QFileInfo>> top;
    for (const QFileInfo &info : QDir(appData).entryInfoList(filters, QDir::Name)) {
        if (info.isDir() && !info.isSymLink() && info.fileName() == QLatin1String("core")) {
            for (const QFileInfo &child :
                 QDir(info.absoluteFilePath()).entryInfoList(filters, QDir::Name))
                top.append({QStringLiteral("core/") + child.fileName(), child});
            continue;
        }
        top.append({info.fileName(), info});
    }
    for (const auto &named : top) {
        const QFileInfo &info = named.second;
        CompleteSystemDataEntry entry;
        entry.name = named.first;
        entry.bulk = completeSystemDataIsBulk(entry.name);
        if (info.isSymLink() || (!info.isDir() && !info.isFile())) {
            entry.skippedEntries = 1;
            entries.append(entry);
            continue;
        }
        if (!info.isDir()) {
            entry.fileCount = 1;
            entry.bytes = info.size();
            entries.append(entry);
            continue;
        }
        entry.directory = true;
        QStringList pending{info.absoluteFilePath()};
        while (!pending.isEmpty()) {
            const QFileInfoList children =
                QDir(pending.takeLast()).entryInfoList(filters, QDir::Name);
            for (const QFileInfo &child : children) {
                if (child.isSymLink() || (!child.isDir() && !child.isFile())) {
                    ++entry.skippedEntries;
                    continue;
                }
                if (child.isDir()) {
                    pending.append(child.absoluteFilePath());
                    continue;
                }
                ++entry.fileCount;
                entry.bytes += child.size();
            }
        }
        entries.append(entry);
    }
    return entries;
}

// code-health: allow(dir-walk-in-loop) explicit Export complete system action, after a confirm and save dialog, under a wait cursor
QJsonObject createCompleteSystemBundle(
    QSettings &settings, const QMap<QString, QString> &variables,
    const QString &applicationDataDirectory, const QString &settingsFile,
    const QString &workerEnvironmentFile, const QJsonObject &deployment,
    QString *error, const QStringList &excludedApplicationDataNames)
{
    if (error)
        error->clear();
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return QJsonObject();
    };

    settings.sync();
    if (settings.status() != QSettings::NoError)
        return fail(QStringLiteral("Could not flush the current settings."));

    QJsonObject encodedSettings;
    const QStringList settingKeys = settings.allKeys();
    for (const QString &key : settingKeys) {
        bool ok = true;
        const QJsonObject encoded = encodeCompleteSetting(settings.value(key), &ok);
        if (!ok) {
            return fail(QStringLiteral("Could not serialize setting %1.").arg(key));
        }
        encodedSettings.insert(key, encoded);
    }

    QJsonObject encodedVariables;
    for (auto it = variables.constBegin(); it != variables.constEnd(); ++it)
        encodedVariables.insert(it.key(), it.value());

    QSet<QString> excludedNames;
    for (const QString &name : excludedApplicationDataNames) {
        const QString trimmed = name.trimmed();
        if (!trimmed.isEmpty())
            excludedNames.insert(trimmed);
    }
    QStringList excludedList(excludedNames.constBegin(), excludedNames.constEnd());
    excludedList.sort(); // a bundle should not differ run to run over set order
    QJsonArray excluded;
    for (const QString &name : std::as_const(excludedList))
        excluded.append(name);

    QJsonArray directories;
    QJsonArray files;
    QJsonArray skipped;
    int skippedCount = 0;
    qint64 totalBytes = 0;
    const QString appData = QDir(applicationDataDirectory).absolutePath();
    if (QFileInfo(appData).exists()) {
        if (!QFileInfo(appData).isDir() || QFileInfo(appData).isSymLink()) {
            return fail(QStringLiteral(
                "The application-data location is not a regular directory."));
        }
        // A breadth-first walk so an excluded top-level name is never descended
        // into: those trees hold gigabytes of re-fetchable mirrors and sandbox
        // workspaces, and even stat-ing them costs seconds.
        QStringList pendingDirectories{QString()};
        while (!pendingDirectories.isEmpty()) {
            const QString relativeDir = pendingDirectories.takeFirst();
            const QString absoluteDir = relativeDir.isEmpty()
                                            ? appData
                                            : QDir(appData).filePath(relativeDir);
            const QFileInfoList children =
                QDir(absoluteDir)
                    .entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot |
                                       QDir::Hidden | QDir::System,
                                   QDir::Name);
            for (const QFileInfo &info : children) {
                const QString relativePath =
                    relativeDir.isEmpty()
                        ? info.fileName()
                        : relativeDir + QLatin1Char('/') + info.fileName();
                if (excludedNames.contains(relativePath))
                    continue;
                if (!safeBundleRelativePath(relativePath))
                    return fail(QStringLiteral("Application data contains an unsafe path."));
                // A JSON bundle carries bytes, so a symbolic link, FIFO, socket
                // or device node has nothing to carry. Record it and continue
                // rather than failing a whole recovery export over a stray
                // Actions-sandbox pipe.
                if (info.isSymLink() || (!info.isDir() && !info.isFile())) {
                    ++skippedCount;
                    if (skipped.size() < kMaxRecordedSkippedEntries) {
                        skipped.append(QJsonObject{
                            {QStringLiteral("path"), relativePath},
                            {QStringLiteral("reason"),
                             info.isSymLink()
                                 ? QStringLiteral("symbolic link")
                                 : QStringLiteral("not a regular file")},
                        });
                    }
                    continue;
                }
                if (info.isDir()) {
                    directories.append(QJsonObject{
                        {QStringLiteral("scope"), QStringLiteral("applicationData")},
                        {QStringLiteral("path"), relativePath},
                        {QStringLiteral("permissions"), int(info.permissions())},
                    });
                    pendingDirectories.append(relativePath);
                    continue;
                }
                bool ok = true;
                const QJsonObject encoded = encodedBundleFile(
                    QStringLiteral("applicationData"), relativePath,
                    info.absoluteFilePath(), &ok, error);
                if (!ok)
                    return {};
                totalBytes += info.size();
                files.append(encoded);
            }
        }
    }

    const auto appendSpecialFile = [&](const QString &scope,
                                       const QString &path) -> bool {
        if (path.trimmed().isEmpty() || !QFileInfo::exists(path))
            return true;
        const QFileInfo info(path);
        bool ok = true;
        const QJsonObject encoded = encodedBundleFile(
            scope, info.fileName(), info.absoluteFilePath(), &ok, error);
        if (!ok)
            return false;
        totalBytes += info.size();
        files.append(encoded);
        return true;
    };
    if (!appendSpecialFile(QStringLiteral("settingsFile"), settingsFile) ||
        !appendSpecialFile(QStringLiteral("workerEnvironment"),
                           workerEnvironmentFile)) {
        return {};
    }

    const QJsonObject bundle{
        {QStringLiteral("kind"), kCompleteSystemBundleKind},
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("exportedAt"),
         QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("security"),
         QJsonObject{
             {QStringLiteral("clearTextSecrets"), true},
             {QStringLiteral("ownerOnlyFile"), true},
             {QStringLiteral("description"),
              QStringLiteral("Contains identity keys, SSH keys, API keys, "
                             "variables, settings, and all application data.")},
         }},
        {QStringLiteral("deployment"), deployment},
        {QStringLiteral("variables"), encodedVariables},
        {QStringLiteral("settings"), encodedSettings},
        {QStringLiteral("directories"), directories},
        {QStringLiteral("files"), files},
        {QStringLiteral("excludedApplicationData"), excluded},
        {QStringLiteral("skipped"), skipped},
        {QStringLiteral("summary"),
         QJsonObject{
             {QStringLiteral("settingCount"), settingKeys.size()},
             {QStringLiteral("variableCount"), variables.size()},
             {QStringLiteral("directoryCount"), directories.size()},
             {QStringLiteral("fileCount"), files.size()},
             {QStringLiteral("skippedCount"), skippedCount},
             {QStringLiteral("excludedCount"), excluded.size()},
             {QStringLiteral("uncompressedBytes"),
              QString::number(totalBytes)},
         }},
    };
    QString validationError;
    if (!validateCompleteSystemBundle(bundle, &validationError))
        return fail(validationError);
    return bundle;
}

bool validateCompleteSystemBundle(const QJsonObject &bundle, QString *error)
{
    if (error)
        error->clear();
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (bundle.value(QStringLiteral("kind")).toString() !=
            kCompleteSystemBundleKind ||
        bundle.value(QStringLiteral("schemaVersion")).toInt() != 1) {
        return fail(QStringLiteral(
            "This is not a supported ForkMesh complete-system bundle."));
    }
    if (!bundle.value(QStringLiteral("settings")).isObject() ||
        !bundle.value(QStringLiteral("variables")).isObject() ||
        !bundle.value(QStringLiteral("deployment")).isObject() ||
        !bundle.value(QStringLiteral("directories")).isArray() ||
        !bundle.value(QStringLiteral("files")).isArray()) {
        return fail(QStringLiteral("The bundle is missing required sections."));
    }

    const QJsonObject settings =
        bundle.value(QStringLiteral("settings")).toObject();
    for (auto it = settings.constBegin(); it != settings.constEnd(); ++it) {
        if (it.key().isEmpty() || !it.value().isObject())
            return fail(QStringLiteral("The bundle contains an invalid setting."));
        bool ok = true;
        const QJsonObject encoded = it.value().toObject();
        const QVariant decoded = decodeCompleteSetting(encoded, &ok);
        const QString typeName = QString::fromLatin1(
            decoded.typeName() ? decoded.typeName() : "invalid");
        if (!ok || encoded.value(QStringLiteral("type")).toString() != typeName)
            return fail(QStringLiteral("A setting in the bundle is corrupted."));
    }
    const QJsonObject variables =
        bundle.value(QStringLiteral("variables")).toObject();
    for (auto it = variables.constBegin(); it != variables.constEnd(); ++it) {
        if (it.key().trimmed().isEmpty() || !it.value().isString())
            return fail(QStringLiteral("The bundle contains an invalid variable."));
    }
    QSet<QString> seenDirectories;
    for (const QJsonValue &value :
         bundle.value(QStringLiteral("directories")).toArray()) {
        if (!value.isObject())
            return fail(QStringLiteral("The bundle contains an invalid directory."));
        const QJsonObject directory = value.toObject();
        const QString path = directory.value(QStringLiteral("path")).toString();
        if (directory.value(QStringLiteral("scope")).toString() !=
                QLatin1String("applicationData") ||
            !safeBundleRelativePath(path) || seenDirectories.contains(path)) {
            return fail(QStringLiteral("The bundle contains an unsafe directory path."));
        }
        seenDirectories.insert(path);
    }
    QSet<QString> seenFiles;
    qint64 totalBytes = 0;
    for (const QJsonValue &value :
         bundle.value(QStringLiteral("files")).toArray()) {
        if (!value.isObject())
            return fail(QStringLiteral("The bundle contains an invalid file."));
        const QJsonObject file = value.toObject();
        if (!validateEncodedBundleFile(file, error))
            return false;
        const QString identity = file.value(QStringLiteral("scope")).toString() +
                                 QLatin1Char('/') +
                                 file.value(QStringLiteral("path")).toString();
        if (seenFiles.contains(identity))
            return fail(QStringLiteral("The bundle contains a duplicate file."));
        seenFiles.insert(identity);
        bool sizeOk = false;
        const qint64 size =
            file.value(QStringLiteral("size")).toString().toLongLong(&sizeOk);
        if (!sizeOk || size < 0 || totalBytes > std::numeric_limits<qint64>::max() - size)
            return fail(QStringLiteral("The bundle's file-size total is invalid."));
        totalBytes += size;
    }
    const QJsonObject summary =
        bundle.value(QStringLiteral("summary")).toObject();
    bool totalOk = false;
    const qint64 declaredTotal =
        summary.value(QStringLiteral("uncompressedBytes"))
            .toString()
            .toLongLong(&totalOk);
    if (!totalOk || declaredTotal != totalBytes ||
        summary.value(QStringLiteral("settingCount")).toInt(-1) !=
            settings.size() ||
        summary.value(QStringLiteral("variableCount")).toInt(-1) !=
            variables.size() ||
        summary.value(QStringLiteral("directoryCount")).toInt(-1) !=
            seenDirectories.size() ||
        summary.value(QStringLiteral("fileCount")).toInt(-1) !=
            seenFiles.size()) {
        return fail(QStringLiteral("The bundle summary does not match its contents."));
    }
    return true;
}

bool writeCompleteSystemBundle(const QString &path, const QJsonObject &bundle,
                               QString *error)
{
    if (error)
        error->clear();
    QString validationError;
    if (!validateCompleteSystemBundle(bundle, &validationError)) {
        if (error)
            *error = validationError;
        return false;
    }
    const QFileInfo target(path);
    if (path.trimmed().isEmpty() || target.isSymLink() ||
        (target.exists() && !target.isFile())) {
        if (error)
            *error = QStringLiteral("The selected export path is unsafe.");
        return false;
    }
    if (!QDir().mkpath(target.absolutePath())) {
        if (error)
            *error = QStringLiteral("Could not create the export directory.");
        return false;
    }
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        !file.setPermissions(QFileDevice::ReadOwner |
                             QFileDevice::WriteOwner)) {
        file.cancelWriting();
        if (error)
            *error = QStringLiteral("Could not create the owner-only export file.");
        return false;
    }
    const QByteArray json = QJsonDocument(bundle).toJson(QJsonDocument::Indented);
    if (file.write(json) != json.size() || !file.commit()) {
        file.cancelWriting();
        if (error)
            *error = QStringLiteral("Could not commit the complete-system export.");
        return false;
    }
    return true;
}

QJsonObject readCompleteSystemBundle(const QString &path, QString *error)
{
    if (error)
        error->clear();
    const QFileInfo info(path);
    if (!info.isFile() || info.isSymLink()) {
        if (error)
            *error = QStringLiteral("The selected bundle is not a regular file.");
        return {};
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("Could not read the selected bundle.");
        return {};
    }
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) {
            *error = QStringLiteral("The selected file is not valid JSON: %1")
                         .arg(parseError.errorString());
        }
        return {};
    }
    const QJsonObject bundle = document.object();
    QString validationError;
    if (!validateCompleteSystemBundle(bundle, &validationError)) {
        if (error)
            *error = validationError;
        return {};
    }
    return bundle;
}

QMap<QString, QString> completeSystemBundleVariables(const QJsonObject &bundle,
                                                     bool *ok)
{
    bool valid = true;
    QMap<QString, QString> variables;
    const QJsonValue source = bundle.value(QStringLiteral("variables"));
    if (!source.isObject()) {
        valid = false;
    } else {
        const QJsonObject object = source.toObject();
        for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
            if (it.key().trimmed().isEmpty() || !it.value().isString()) {
                valid = false;
                break;
            }
            variables.insert(it.key(), it.value().toString());
        }
    }
    if (ok)
        *ok = valid;
    return valid ? variables : QMap<QString, QString>();
}

QVariant completeSystemBundleSetting(const QJsonObject &bundle,
                                     const QString &key, bool *ok)
{
    bool valid = false;
    QVariant value;
    const QJsonValue settingsValue = bundle.value(QStringLiteral("settings"));
    if (settingsValue.isObject()) {
        const QJsonValue encoded = settingsValue.toObject().value(key);
        if (encoded.isObject()) {
            valid = true;
            value = decodeCompleteSetting(encoded.toObject(), &valid);
        }
    }
    if (ok)
        *ok = valid;
    return valid ? value : QVariant();
}

bool mergeCompleteSystemBundleSettings(const QJsonObject &bundle,
                                       QSettings &settings, QString *error)
{
    if (error)
        error->clear();
    QString validationError;
    if (!validateCompleteSystemBundle(bundle, &validationError)) {
        if (error)
            *error = validationError;
        return false;
    }
    const QJsonObject imported =
        bundle.value(QStringLiteral("settings")).toObject();
    for (auto it = imported.constBegin(); it != imported.constEnd(); ++it) {
        bool ok = true;
        const QVariant value = decodeCompleteSetting(it.value().toObject(), &ok);
        if (!ok) {
            if (error)
                *error = QStringLiteral("Could not decode an imported setting.");
            return false;
        }
        settings.setValue(it.key(), value);
    }
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        if (error)
            *error = QStringLiteral("Could not save the imported settings.");
        return false;
    }
    const QString fileName = settings.fileName();
    if (!fileName.isEmpty() && QFileInfo::exists(fileName) &&
        !QFile::setPermissions(fileName,
                               QFileDevice::ReadOwner |
                                   QFileDevice::WriteOwner)) {
        if (error)
            *error = QStringLiteral("Could not restrict the imported settings file.");
        return false;
    }
    return true;
}

QString siteDeployEnvFilePath(const QString &sourceDir,
                              const QString &applicationDir)
{
    const QString worker =
        findCloudflareWorkerDirectory(sourceDir, applicationDir);
    if (worker.isEmpty())
        return {};
    return QDir(worker).absoluteFilePath(QStringLiteral(".env.production"));
}

QString maskedTokenSuffix(const QString &token)
{
    const QString ellipsis = QString::fromUtf8("\xE2\x80\xA6");
    const QString trimmed = token.trimmed();
    return trimmed.size() < 8 ? ellipsis : ellipsis + trimmed.right(4);
}

} // namespace forkmesh::control
