#include "ReleaseDraft.h"

#include <QRegularExpression>

namespace forkmesh::releasedraft {

Latest parseLatest(const QByteArray &forEachRefLine)
{
    Latest latest;
    const QString line = QString::fromUtf8(forEachRefLine).section(QLatin1Char('\n'), 0, 0)
                             .trimmed();
    const int tab = line.indexOf(QLatin1Char('\t'));
    if (tab >= 0) {
        latest.tag = line.left(tab).trimmed();
        latest.title = line.mid(tab + 1).trimmed();
    } else {
        latest.tag = line;
    }
    return latest;
}

QString nextTag(const QString &previous)
{
    int end = -1;
    for (int i = previous.size() - 1; i >= 0; --i) {
        if (previous.at(i).isDigit()) {
            end = i;
            break;
        }
    }
    if (end < 0)
        return QString();
    int start = end;
    while (start > 0 && previous.at(start - 1).isDigit())
        --start;
    bool ok = false;
    const qulonglong n = previous.mid(start, end - start + 1).toULongLong(&ok);
    if (!ok)
        return QString();
    return previous.left(start) + QString::number(n + 1) + previous.mid(end + 1);
}

QString nextTitle(const QString &prevTag, const QString &prevTitle, const QString &tag)
{
    if (tag.isEmpty())
        return QString();
    if (prevTitle.isEmpty() || prevTitle == prevTag || prevTag.isEmpty())
        return tag;
    if (prevTitle.contains(prevTag))
        return QString(prevTitle).replace(prevTag, tag);
    return prevTitle;
}

bool tagNameValid(const QString &tag)
{
    static const QRegularExpression re(
        QStringLiteral("\\A[A-Za-z0-9][A-Za-z0-9._+-]{0,127}\\z"));
    return re.match(tag).hasMatch() && !tag.contains(QLatin1String("..")) &&
           !tag.endsWith(QLatin1String(".lock")) && !tag.endsWith(QLatin1Char('.'));
}

bool deployableTag(const QString &tag)
{
    static const QRegularExpression re(QStringLiteral("\\Av?[0-9]+\\.[0-9]+\\.[0-9]+\\z"));
    return re.match(tag).hasMatch();
}

QStringList logArgs(const QString &prevTag, const QString &targetRef)
{
    QStringList args{QStringLiteral("log"), QStringLiteral("--no-merges"),
                     QStringLiteral("--date-order"),
                     QStringLiteral("--format=%s%x1f%h%x1f%an")};
    if (prevTag.isEmpty())
        args << QStringLiteral("--max-count=%1").arg(kMaxLogCommits) << targetRef;
    else
        args << QStringLiteral("%1..%2").arg(prevTag, targetRef);
    return args;
}

QString notesFromLog(const QByteArray &log, const QString &prevTag,
                     const QString &tag, const QString &targetRef)
{
    QString text = QStringLiteral("## What's Changed\n");
    int shown = 0;
    for (const QByteArray &line : log.split('\n')) {
        const QString entry = QString::fromUtf8(line).trimmed();
        if (entry.isEmpty())
            continue;
        const QStringList f = entry.split(QLatin1Char('\x1f'));
        const QString subject = f.value(0).trimmed();
        const QString sha = f.value(1).trimmed();
        const QString author = f.value(2).trimmed();
        if (subject.isEmpty())
            continue;
        text += QStringLiteral("* %1 (`%2`)").arg(subject, sha);
        if (!author.isEmpty())
            text += QStringLiteral(" by %1").arg(author);
        text += QLatin1Char('\n');
        ++shown;
    }
    if (shown == 0)
        text += prevTag.isEmpty() ? QStringLiteral("* No commits yet.\n")
                                  : QStringLiteral("* No changes since %1.\n").arg(prevTag);
    if (!prevTag.isEmpty())
        text += QStringLiteral("\n**Full Changelog**: %1...%2")
                    .arg(prevTag, tag.isEmpty() ? targetRef : tag);
    return text;
}

QString tagMessage(const QString &title, const QString &notes, const QString &tag)
{
    QString message = title.trimmed();
    if (message.isEmpty())
        message = tag;
    const QString body = notes.trimmed();
    if (!body.isEmpty())
        message += QStringLiteral("\n\n") + body;
    return message;
}

}
