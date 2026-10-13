#include "LogSource.h"

#include <QDir>
#include <QFileInfo>
#include <QLatin1String>
#include <QRegularExpression>

#include <utility>

namespace forkmesh {
namespace {

const char *const kThisFile = __FILE__;
const QLatin1String kThisFileSuffix("desktop/src/LogSource.cpp");

QString withForwardSlashes(const QString &raw)
{
    QString path = raw;
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    return path;
}

} // namespace

QString logSourceBuildRoot()
{
    static const QString root = [] {
        const QString self = withForwardSlashes(QString::fromUtf8(kThisFile));
        const int at = self.lastIndexOf(kThisFileSuffix);
        return at <= 0 ? QString() : self.left(at - 1);
    }();
    return root;
}

QString logSourceRelativePath(const char *absolutePath)
{
    if (!absolutePath || !*absolutePath)
        return QString();
    const QString path = QDir::cleanPath(
        withForwardSlashes(QString::fromUtf8(absolutePath)));
    const QString root = logSourceBuildRoot();
    if (!root.isEmpty() && path.size() > root.size() + 1 &&
        path.at(root.size()) == QLatin1Char('/') && path.startsWith(root))
        return path.mid(root.size() + 1);
    // Outside the checkout (a generated file, a header from a sibling tree):
    // the last two components still say enough to identify it.
    const int lastSlash = path.lastIndexOf(QLatin1Char('/'));
    if (lastSlash <= 0)
        return path;
    const int prevSlash = path.lastIndexOf(QLatin1Char('/'), lastSlash - 1);
    return prevSlash < 0 ? path : path.mid(prevSlash + 1);
}

QString logSourceSuffix(const QString &relativePath, int line,
                        LogSeverity severity)
{
    if (relativePath.isEmpty() || line <= 0)
        return QString();
    static const QRegularExpression safeRe(
        QStringLiteral("\\A[A-Za-z0-9._+/-]+\\z"));
    if (!safeRe.match(relativePath).hasMatch())
        return QString();
    QString severitySuffix;
    if (severity == LogSeverity::Informational)
        severitySuffix = QStringLiteral("|INFO");
    else if (severity == LogSeverity::Error)
        severitySuffix = QStringLiteral("|ERROR");
    return QStringLiteral("  [%1:%2%3]")
        .arg(relativePath)
        .arg(line)
        .arg(severitySuffix);
}

bool splitLogSource(const QString &message, QString *body, QString *path,
                    int *line, LogSeverity *severity)
{
    const auto fail = [&] {
        if (body)
            *body = message;
        if (severity)
            *severity = LogSeverity::Automatic;
        return false;
    };
    int at = message.size() - 1;
    if (at < 0 || message.at(at) != QLatin1Char(']'))
        return fail();
    int lineEnd = at;
    LogSeverity parsedSeverity = LogSeverity::Automatic;
    const auto stripSeverity = [&](const QLatin1String &suffix,
                                   LogSeverity value) {
        const int suffixStart = lineEnd - suffix.size();
        if (suffixStart >= 0 &&
            message.mid(suffixStart, suffix.size()) == suffix) {
            lineEnd = suffixStart;
            parsedSeverity = value;
            return true;
        }
        return false;
    };
    if (!stripSeverity(QLatin1String("|ERROR"), LogSeverity::Error))
        stripSeverity(QLatin1String("|INFO"), LogSeverity::Informational);
    at = lineEnd;
    while (at > 0 && message.at(at - 1).isDigit())
        --at;
    if (at == lineEnd) // "[…:]" — no line number
        return fail();
    const int lineStart = at;
    if (at == 0 || message.at(at - 1) != QLatin1Char(':'))
        return fail();
    --at;
    const int pathEnd = at;
    while (at > 0) {
        const QChar c = message.at(at - 1);
        if (c == QLatin1Char('[') || c == QLatin1Char(']') || c.isSpace())
            break;
        --at;
    }
    if (at == pathEnd || at == 0 || message.at(at - 1) != QLatin1Char('['))
        return fail();
    const int pathStart = at;
    --at; // the '['
    if (at == 0 || !message.at(at - 1).isSpace())
        return fail(); // a bracketed phrase glued to the message is not ours

    int bodyEnd = at;
    while (bodyEnd > 0 && message.at(bodyEnd - 1).isSpace())
        --bodyEnd;
    if (body)
        *body = message.left(bodyEnd);
    if (path)
        *path = message.mid(pathStart, pathEnd - pathStart);
    if (line)
        *line = message.mid(lineStart, lineEnd - lineStart).toInt();
    if (severity)
        *severity = parsedSeverity;
    return true;
}

QString logMessageBody(const QString &message)
{
    QString body;
    splitLogSource(message, &body, nullptr, nullptr);
    return body;
}

QString logSourceLabel(const QString &relativePath, int line)
{
    if (relativePath.isEmpty())
        return QString();
    const int slash = relativePath.lastIndexOf(QLatin1Char('/'));
    const QString name =
        slash < 0 ? relativePath : relativePath.mid(slash + 1);
    return line > 0 ? QStringLiteral("%1:%2").arg(name).arg(line) : name;
}

QString resolveLogSourcePath(const QString &relativePath,
                             const QStringList &extraRoots)
{
    if (relativePath.isEmpty())
        return QString();
    if (QFileInfo(relativePath).isAbsolute()) {
        return QFileInfo(relativePath).isFile() ? relativePath : QString();
    }

    QStringList roots;
    roots << logSourceBuildRoot() << extraRoots << QDir::currentPath();
    const int slash = relativePath.indexOf(QLatin1Char('/'));
    const QString firstComponent =
        slash > 0 ? relativePath.left(slash) : QString();

    for (const QString &root : std::as_const(roots)) {
        if (root.isEmpty())
            continue;
        const QString direct = QDir(root).filePath(relativePath);
        if (QFileInfo(direct).isFile())
            return QDir::cleanPath(direct);
        if (!firstComponent.isEmpty() &&
            QFileInfo(root).fileName() == firstComponent) {
            const QString inner =
                QDir(root).filePath(relativePath.mid(slash + 1));
            if (QFileInfo(inner).isFile())
                return QDir::cleanPath(inner);
        }
    }
    return QString();
}

} // namespace forkmesh
