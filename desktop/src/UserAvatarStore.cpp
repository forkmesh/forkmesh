#include "UserAvatarStore.h"
#include "AppPaths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>

namespace {

// Where the picture lived before it got its own file.
const QString kLegacySetting = QStringLiteral("profile/avatarPng");

// Drop the legacy blob so the ini file stops carrying ~104 KB of base64. Only
// ever called once the picture is safely on disk.
void forgetLegacyBlob()
{
    QSettings settings;
    if (settings.contains(kLegacySetting))
        settings.remove(kLegacySetting);
}

} // namespace

QString UserAvatarStore::path()
{
    // No fallback to a temp directory here: the avatar has to survive a reboot,
    // and a path that does not would make migrating off QSettings a data loss.
    // Callers treat an empty path as "keep using QSettings".
    const QString base =
        forkmesh::paths::dataRoot();
    if (base.isEmpty())
        return QString();
    return base + QStringLiteral("/profile/avatar.png");
}

QByteArray UserAvatarStore::load()
{
    const QString file = path();
    if (file.isEmpty())
        return QSettings().value(kLegacySetting).toByteArray();

    QFile in(file);
    if (in.open(QIODevice::ReadOnly)) {
        const QByteArray png = in.readAll();
        // A migration that wrote the file but could not rewrite the ini leaves
        // the blob behind; clearing it here makes the move eventually converge.
        forgetLegacyBlob();
        return png;
    }

    // First run after the upgrade: move the blob out of the ini file.
    const QByteArray legacy = QSettings().value(kLegacySetting).toByteArray();
    if (!legacy.isEmpty())
        store(legacy);
    return legacy;
}

bool UserAvatarStore::store(const QByteArray &png)
{
    const QString file = path();
    if (file.isEmpty()) {
        QSettings().setValue(kLegacySetting, png);
        return true;
    }

    if (png.isEmpty()) {
        QFile::remove(file);
        forgetLegacyBlob();
        return true;
    }

    if (!QDir().mkpath(QFileInfo(file).absolutePath()))
        return false;
    QSaveFile out(file);
    if (!out.open(QIODevice::WriteOnly))
        return false;
    out.write(png);
    if (!out.commit())
        return false;
    forgetLegacyBlob();
    return true;
}
