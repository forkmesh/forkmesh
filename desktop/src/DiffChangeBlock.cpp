#include "DiffChangeBlock.h"

#include <QRegularExpression>

namespace forkmesh::diffblock {

namespace {

constexpr int kContextLines = 3;

bool isChangeLine(const QString &line)
{
    return line.startsWith(QLatin1Char('-')) || line.startsWith(QLatin1Char('+'));
}

bool isBlockLine(const QString &line)
{
    return isChangeLine(line) || line.startsWith(QLatin1Char('\\'));
}

bool isContextLine(const QString &line)
{
    return line.startsWith(QLatin1Char(' '));
}

QString blockPatchAt(const QStringList &header, const QStringList &lines,
                     qsizetype start, int oldStart, int newStart)
{
    qsizetype first = start;
    while (first > 0 && start - first < kContextLines &&
           isContextLine(lines.at(first - 1)))
        --first;
    const int before = int(start - first);

    qsizetype end = start;
    int dels = 0;
    int adds = 0;
    while (end < lines.size() && isBlockLine(lines.at(end))) {
        if (lines.at(end).startsWith(QLatin1Char('-')))
            ++dels;
        else if (lines.at(end).startsWith(QLatin1Char('+')))
            ++adds;
        ++end;
    }
    int after = 0;
    while (end < lines.size() && after < kContextLines &&
           isContextLine(lines.at(end))) {
        ++after;
        ++end;
    }
    if (after > 0 && end < lines.size() &&
        lines.at(end).startsWith(QLatin1Char('\\')))
        ++end;

    QString out = header.join(QLatin1Char('\n'));
    out += QStringLiteral("\n@@ -%1,%2 +%3,%4 @@\n")
               .arg(oldStart - before)
               .arg(before + dels + after)
               .arg(newStart - before)
               .arg(before + adds + after);
    for (qsizetype i = first; i < end; ++i) {
        out += lines.at(i);
        out += QLatin1Char('\n');
    }
    return out;
}

} // namespace

QStringList splitFilePatches(const QString &patch)
{
    QStringList sections;
    QString current;
    bool open = false;
    const QStringList lines = patch.split(QLatin1Char('\n'));
    for (qsizetype i = 0; i < lines.size(); ++i) {
        const QString &line = lines.at(i);
        if (line.startsWith(QLatin1String("diff --git "))) {
            if (open)
                sections.append(current);
            current.clear();
            open = true;
        }
        if (!open)
            continue;
        if (i + 1 == lines.size() && line.isEmpty())
            break;
        current += line;
        current += QLatin1Char('\n');
    }
    if (open)
        sections.append(current);
    return sections;
}

QString filePatchPath(const QString &filePatch)
{
    const QString line = filePatch.section(QLatin1Char('\n'), 0, 0);
    const qsizetype bpos = line.indexOf(QLatin1String(" b/"));
    return bpos >= 0 ? line.mid(bpos + 3) : line;
}

QString changeBlockPatch(const QString &filePatch, int oldStart, int newStart)
{
    static const QRegularExpression hunkRe(
        QStringLiteral("^@@ -(\\d+)(?:,\\d+)? \\+(\\d+)(?:,\\d+)? @@"));
    const QStringList lines = filePatch.split(QLatin1Char('\n'));
    QStringList header;
    qsizetype i = 0;
    for (; i < lines.size() && !lines.at(i).startsWith(QLatin1String("@@")); ++i) {
        const QString &line = lines.at(i);
        if (line.startsWith(QLatin1String("GIT binary patch")) ||
            line.startsWith(QLatin1String("Binary files ")))
            return QString();
        if (line.startsWith(QLatin1String("diff --git ")) ||
            line.startsWith(QLatin1String("--- ")) ||
            line.startsWith(QLatin1String("+++ ")))
            header.append(line);
    }
    if (header.size() != 3)
        return QString();

    int oldNo = 0;
    int newNo = 0;
    bool inHunk = false;
    while (i < lines.size()) {
        const QString &line = lines.at(i);
        if (line.startsWith(QLatin1String("@@"))) {
            const QRegularExpressionMatch m = hunkRe.match(line);
            if (!m.hasMatch())
                return QString();
            oldNo = m.captured(1).toInt();
            newNo = m.captured(2).toInt();
            inHunk = true;
            ++i;
            continue;
        }
        if (!inHunk) {
            ++i;
            continue;
        }
        if (isChangeLine(line)) {
            if (oldNo == oldStart && newNo == newStart)
                return blockPatchAt(header, lines, i, oldStart, newStart);
            while (i < lines.size() && isBlockLine(lines.at(i))) {
                if (lines.at(i).startsWith(QLatin1Char('-')))
                    ++oldNo;
                else if (lines.at(i).startsWith(QLatin1Char('+')))
                    ++newNo;
                ++i;
            }
            continue;
        }
        if (isContextLine(line)) {
            ++oldNo;
            ++newNo;
        } else if (!line.startsWith(QLatin1Char('\\'))) {
            inHunk = false;
        }
        ++i;
    }
    return QString();
}

} // namespace forkmesh::diffblock
