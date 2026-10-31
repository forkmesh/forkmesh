#pragma once

#include <QDir>
#include <QFileInfo>
#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <functional>

namespace forkmesh::promptdrop {

struct Plan {
    QStringList attachments;
    QStringList mentions;

    bool empty() const { return attachments.isEmpty() && mentions.isEmpty(); }
};

inline QStringList localPaths(const QList<QUrl> &urls)
{
    QStringList out;
    for (const QUrl &url : urls)
        if (url.isLocalFile() && !url.toLocalFile().isEmpty())
            out << QDir::cleanPath(url.toLocalFile());
    return out;
}

inline QString mentionFor(const QString &path, const QStringList &roots, bool directory)
{
    QString mention = path;
    for (const QString &root : roots) {
        if (root.isEmpty())
            continue;
        const QString relative = QDir(root).relativeFilePath(path);
        if (relative != QLatin1String(".") && !relative.startsWith(QLatin1String("..")) &&
            !QDir::isAbsolutePath(relative)) {
            mention = relative;
            break;
        }
    }
    if (directory && !mention.endsWith(QLatin1Char('/')))
        mention += QLatin1Char('/');
    if (mention.contains(QLatin1Char(' ')))
        mention = QLatin1Char('"') + mention + QLatin1Char('"');
    return QLatin1Char('@') + mention;
}

inline Plan plan(const QStringList &paths, const QStringList &roots,
                 const std::function<bool(const QString &)> &isMedia)
{
    Plan out;
    for (const QString &path : paths) {
        const QFileInfo info(path);
        if (!info.exists())
            continue;
        if (info.isFile() && isMedia && isMedia(path)) {
            if (!out.attachments.contains(path))
                out.attachments << path;
            continue;
        }
        const QString mention = mentionFor(path, roots, info.isDir());
        if (!out.mentions.contains(mention))
            out.mentions << mention;
    }
    return out;
}

inline QString insertion(const Plan &plan, const QString &before)
{
    if (plan.mentions.isEmpty())
        return {};
    QString text = plan.mentions.join(QLatin1Char(' ')) + QLatin1Char(' ');
    if (!before.isEmpty() && !before.at(before.size() - 1).isSpace())
        text.prepend(QLatin1Char(' '));
    return text;
}

}
