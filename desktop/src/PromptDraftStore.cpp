#include "PromptDraftStore.h"
#include "AppPaths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>

QString PromptDraftStore::path()
{
    if (forkmesh::paths::dataRoot().isEmpty())
        return QString();
    return forkmesh::paths::dataPath(QStringLiteral("prompt")) + QStringLiteral("/draft.txt");
}

QString PromptDraftStore::load()
{
    const QString file = path();
    if (file.isEmpty())
        return QString();
    QFile in(file);
    if (!in.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromUtf8(in.readAll());
}

bool PromptDraftStore::store(const QString &text)
{
    const QString file = path();
    if (file.isEmpty())
        return true;

    if (text.trimmed().isEmpty()) {
        QFile::remove(file);
        return !QFileInfo::exists(file);
    }

    if (!QDir().mkpath(QFileInfo(file).absolutePath()))
        return false;
    // QSaveFile writes beside the target and renames over it, so a crash in the
    // middle of a write leaves the previous draft rather than a torn one.
    QSaveFile out(file);
    if (!out.open(QIODevice::WriteOnly))
        return false;
    out.write(text.toUtf8());
    return out.commit();
}
