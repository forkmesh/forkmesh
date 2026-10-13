#include "AgentCapabilityStore.h"
#include "AppPaths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>
#include <QStandardPaths>

namespace {

constexpr qint64 kMaximumStoreBytes = 1024 * 1024;

bool fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return false;
}

bool validStorePath(const QString &path, QString *error)
{
    if (path.trimmed().isEmpty() || !QFileInfo(path).isAbsolute())
        return fail(error,
                    QStringLiteral("capability grant path must be absolute"));
    if (QFileInfo(path).isSymLink())
        return fail(error,
                    QStringLiteral("capability grant path must not be a symlink"));
    return true;
}

QFileDevice::Permissions ownerFilePermissions()
{
    return QFileDevice::ReadOwner | QFileDevice::WriteOwner;
}

QFileDevice::Permissions ownerDirectoryPermissions()
{
    return ownerFilePermissions() | QFileDevice::ExeOwner;
}

bool hasPublicPermissions(QFileDevice::Permissions permissions)
{
    constexpr QFileDevice::Permissions publicBits =
        QFileDevice::ReadGroup | QFileDevice::WriteGroup |
        QFileDevice::ExeGroup | QFileDevice::ReadOther |
        QFileDevice::WriteOther | QFileDevice::ExeOther;
    return (permissions & publicBits) != 0;
}

} // namespace

QString AgentCapabilityStore::defaultPath()
{
    const QString root =
        forkmesh::paths::dataRoot();
    return QDir(root).filePath(
        QStringLiteral("agent-runtime/capability-grants.json"));
}

bool AgentCapabilityStore::load(const QString &path,
                                AgentCapabilityBroker *broker,
                                QString *error)
{
    if (!broker)
        return fail(error, QStringLiteral("capability broker is required"));
    if (!validStorePath(path, error))
        return false;

    QFile file(path);
    if (!file.exists())
        return broker->loadPersistentGrants({}, error);
    const QFileInfo info(file);
    if (!info.isFile())
        return fail(error,
                    QStringLiteral("capability grant path is not a file"));
    if (hasPublicPermissions(info.permissions()))
        return fail(error,
                    QStringLiteral("capability grant file is not owner-only"));
    if (info.size() > kMaximumStoreBytes)
        return fail(error, QStringLiteral("capability grant file is too large"));
    if (!file.open(QIODevice::ReadOnly))
        return fail(error, QStringLiteral("could not open capability grants: %1")
                               .arg(file.errorString()));
    const QByteArray bytes = file.read(kMaximumStoreBytes + 1);
    if (bytes.size() > kMaximumStoreBytes)
        return fail(error, QStringLiteral("capability grant file is too large"));

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return fail(error, QStringLiteral("invalid capability grant JSON: %1")
                               .arg(parseError.errorString()));
    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("schemaVersion")).toInt(-1) != 1 ||
        !root.value(QStringLiteral("grants")).isArray())
        return fail(error,
                    QStringLiteral("unsupported capability grant document"));
    return broker->loadPersistentGrants(
        root.value(QStringLiteral("grants")).toArray(), error);
}

bool AgentCapabilityStore::save(const QString &path,
                                const AgentCapabilityBroker &broker,
                                QString *error)
{
    if (!validStorePath(path, error))
        return false;
    const QFileInfo target(path);
    QDir parent = target.dir();
    const bool createdParent = !parent.exists();
    if (createdParent && !QDir().mkpath(parent.absolutePath()))
        return fail(error,
                    QStringLiteral("could not create capability grant directory"));
    const QFileInfo parentInfo(parent.absolutePath());
    if (!parentInfo.isDir() || parentInfo.isSymLink())
        return fail(error, QStringLiteral(
                               "capability grant directory must be a real directory"));
    if (createdParent) {
        if (!QFile::setPermissions(parent.absolutePath(),
                                   ownerDirectoryPermissions()))
            return fail(
                error,
                QStringLiteral(
                    "could not make capability grant directory owner-only"));
    } else if (hasPublicPermissions(parentInfo.permissions())) {
        // Never chmod a caller-owned directory as a side effect. The default
        // path uses a dedicated agent-runtime child directory, which this
        // store creates privately on first use.
        return fail(error, QStringLiteral(
                               "capability grant directory is not owner-only"));
    }

    const QJsonDocument document(QJsonObject{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("grants"), broker.persistentGrants()}});
    const QByteArray bytes = document.toJson(QJsonDocument::Compact);
    if (bytes.size() > kMaximumStoreBytes)
        return fail(error, QStringLiteral("capability grant file is too large"));

    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly))
        return fail(error, QStringLiteral("could not write capability grants: %1")
                               .arg(file.errorString()));
    if (!file.setPermissions(ownerFilePermissions())) {
        file.cancelWriting();
        return fail(error,
                    QStringLiteral("could not make capability grants owner-only"));
    }
    if (file.write(bytes) != bytes.size()) {
        const QString writeError = file.errorString();
        file.cancelWriting();
        return fail(error, QStringLiteral("could not write capability grants: %1")
                               .arg(writeError));
    }
    if (!file.commit())
        return fail(error, QStringLiteral("could not commit capability grants: %1")
                               .arg(file.errorString()));
    if (!QFile::setPermissions(path, ownerFilePermissions()))
        return fail(error,
                    QStringLiteral("could not secure saved capability grants"));
    if (error)
        error->clear();
    return true;
}
