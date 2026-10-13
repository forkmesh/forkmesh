#pragma once

#include <QRegularExpression>
#include <QString>
#include <QStringList>

namespace ActionFailureSummary {

inline constexpr int kMaxCauseChars = 500;
inline constexpr int kMaxFixChars = 400;

struct Summary {
    QString step;
    QString cause;
    QString fix;
};

inline QString cleanLine(QString line)
{
    static const QRegularExpression ansi(QStringLiteral("\x1B\\[[0-9;?]*[A-Za-z]"));
    static const QRegularExpression clock(QStringLiteral("^\\d\\d:\\d\\d:\\d\\d\\s+"));
    line.remove(ansi);
    line.remove(QLatin1Char('\r'));
    line = line.trimmed();
    line.remove(clock);
    return line;
}

inline bool looksLikeFailure(const QString &line)
{
    static const QRegularExpression pattern(QStringLiteral(
        "^(FAIL(ED)?\\b|ERROR\\b|Error\\b|error\\b|fatal\\b|[A-Za-z_.]*(Error|Exception):"
        "|Traceback\\b)|: (error|fatal):"));
    return pattern.match(line).hasMatch();
}

inline QString clipped(const QString &text, int limit)
{
    if (text.size() <= limit)
        return text;
    return text.left(limit - 1).trimmed() + QChar(0x2026);
}

inline Summary summarize(const QString &log)
{
    const QString stepMark = QString::fromUtf8("==> \xF0\x9F\x94\xA7 ");
    const QString failMark = QString::fromUtf8("==> \xE2\x9D\x8C FAILED: ");
    const QStringList lines = log.split(QLatin1Char('\n'));
    int footer = -1;
    for (int i = lines.size() - 1; i >= 0; --i) {
        if (lines.at(i).startsWith(failMark)) {
            footer = i;
            break;
        }
    }
    const int end = footer >= 0 ? footer : static_cast<int>(lines.size());
    int header = -1;
    for (int i = end - 1; i >= 0; --i) {
        if (lines.at(i).startsWith(stepMark)) {
            header = i;
            break;
        }
    }
    Summary summary;
    if (header >= 0)
        summary.step = cleanLine(lines.at(header).mid(stepMark.size()));
    QString failure;
    QString last;
    for (int i = end - 1; i > header; --i) {
        const QString line = cleanLine(lines.at(i));
        if (line.isEmpty() || line.startsWith(QLatin1String("==> ")))
            continue;
        if (last.isEmpty())
            last = line;
        if (looksLikeFailure(line)) {
            failure = line;
            break;
        }
    }
    QString cause = !failure.isEmpty() ? failure : last;
    if (cause.isEmpty() && footer >= 0)
        cause = cleanLine(lines.at(footer).mid(failMark.size()));
    if (cause.startsWith(QLatin1String("FAILED: ")))
        cause = cause.mid(8).trimmed();
    const int fixAt = cause.indexOf(QLatin1String(" Fix: "));
    if (fixAt >= 0) {
        summary.fix = clipped(cause.mid(fixAt + 6).trimmed(), kMaxFixChars);
        cause = cause.left(fixAt).trimmed();
    }
    summary.cause = clipped(cause, kMaxCauseChars);
    return summary;
}

inline QString pingBody(const QString &heading, const QString &log)
{
    const Summary summary = summarize(log);
    QStringList parts{heading};
    if (!summary.step.isEmpty())
        parts << QStringLiteral("Step: %1").arg(summary.step);
    if (!summary.cause.isEmpty())
        parts << QStringLiteral("Cause: %1").arg(summary.cause);
    if (!summary.fix.isEmpty())
        parts << QStringLiteral("Fix: %1").arg(summary.fix);
    return parts.join(QLatin1Char('\n'));
}

}
