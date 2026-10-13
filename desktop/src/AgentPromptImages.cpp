#include "AgentPromptImages.h"
#include "AppPaths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QStandardPaths>
#include <QTemporaryFile>

namespace {

QString legacyDirectory()
{
    return QStandardPaths::writableLocation(QStandardPaths::TempLocation) +
           QStringLiteral("/forkmesh-agent-images");
}

} // namespace

QString AgentPromptImages::directory()
{
    QString base =
        forkmesh::paths::dataRoot();
    if (base.isEmpty())
        base = QDir::tempPath(); // no writable app data: better than nothing
    return base + QStringLiteral("/agent-images");
}

QString AgentPromptImages::save(const QImage &image)
{
    if (image.isNull())
        return QString();
    const QString dir = directory();
    if (!QDir().mkpath(dir))
        return QString();
    QTemporaryFile file(dir + QStringLiteral("/paste-XXXXXX.png"));
    file.setAutoRemove(false);
    if (!file.open())
        return QString();
    const QString path = file.fileName();
    const bool ok = image.save(&file, "PNG");
    file.close();
    if (!ok) {
        QFile::remove(path);
        return QString();
    }
    return path;
}

QString AgentPromptImages::resolve(const QString &path)
{
    if (path.isEmpty())
        return QString();
    if (QFileInfo::exists(path))
        return path;
    const QString moved =
        directory() + QLatin1Char('/') + QFileInfo(path).fileName();
    return QFileInfo::exists(moved) ? moved : QString();
}

void AgentPromptImages::migrateLegacy()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    QDir legacy(legacyDirectory());
    if (!legacy.exists())
        return;
    const QStringList names =
        legacy.entryList(QDir::Files | QDir::NoSymLinks, QDir::Name);
    if (names.isEmpty())
        return;
    const QString dir = directory();
    if (!QDir().mkpath(dir))
        return;
    for (const QString &name : names) {
        const QString target = dir + QLatin1Char('/') + name;
        if (QFileInfo::exists(target))
            continue;
        QFile::copy(legacy.filePath(name), target);
    }
}
