#pragma once

#include "AgentSummaryPreloadScript.h"
#include "AppPaths.h"

#include <QDir>
#include <QFile>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QString>
#include <QStringList>

namespace forkmesh::agentsummary {

inline const QString kOptionsVariable = QStringLiteral("BUN_OPTIONS");
inline const QString kFileName = QStringLiteral("no-agent-summary.js");

inline bool usablePath(const QString &path)
{
    static const QRegularExpression unsafe(QStringLiteral("[\\s\"'\\\\]"));
    return QDir::isAbsolutePath(path) && !path.contains(unsafe);
}

inline QString withPreload(const QString &options, const QString &path)
{
    const QString argument = QStringLiteral("--preload ") + path;
    const QString trimmed = options.simplified();
    if ((QLatin1Char(' ') + trimmed + QLatin1Char(' '))
            .contains(QLatin1Char(' ') + argument + QLatin1Char(' ')))
        return options;
    return trimmed.isEmpty() ? argument : trimmed + QLatin1Char(' ') + argument;
}

inline QString install(const QString &directory)
{
    if (directory.isEmpty())
        return QString();
    const QString path = QDir(directory).absoluteFilePath(kFileName);
    if (!usablePath(path) || !QDir().mkpath(directory))
        return QString();
    const QByteArray wanted = script();
    {
        QFile current(path);
        if (current.open(QIODevice::ReadOnly) && current.readAll() == wanted)
            return path;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(wanted) != wanted.size() ||
        !file.commit())
        return QString();
    return path;
}

inline QStringList candidateDirectories()
{
    return {forkmesh::paths::dataPath(QStringLiteral("agents")),
            forkmesh::paths::cacheRoot() + QStringLiteral("/agents")};
}

inline bool applyEnvironment(QProcessEnvironment &env,
                             const QStringList &directories = candidateDirectories())
{
#ifdef Q_OS_WIN
    Q_UNUSED(env);
    Q_UNUSED(directories);
    return false;
#else
    for (const QString &directory : directories) {
        const QString path = install(directory);
        if (path.isEmpty())
            continue;
        env.insert(kOptionsVariable, withPreload(env.value(kOptionsVariable), path));
        return true;
    }
    return false;
#endif
}

} // namespace forkmesh::agentsummary
