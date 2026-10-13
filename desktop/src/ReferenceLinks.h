#pragma once

#include <QString>

namespace ReferenceLinks {

inline bool isHexChar(QChar c)
{
    return c.isDigit() || (c >= QLatin1Char('a') && c <= QLatin1Char('f')) ||
           (c >= QLatin1Char('A') && c <= QLatin1Char('F'));
}

inline bool isTokenBoundary(const QString &text, int index)
{
    if (index < 0 || index >= text.size())
        return true;
    const QChar c = text.at(index);
    return !c.isLetterOrNumber() && c != QLatin1Char('_');
}

inline int readDigits(const QString &text, int start)
{
    int end = start;
    while (end < text.size() && text.at(end).isDigit())
        ++end;
    return end;
}

inline int readHexRun(const QString &text, int start)
{
    int end = start;
    while (end < text.size() && isHexChar(text.at(end)))
        ++end;
    return end;
}

inline int copyUrl(const QString &text, int start, QString *out)
{
    int end = start;
    while (end < text.size() && !text.at(end).isSpace())
        ++end;
    int linkEnd = end;
    while (linkEnd > start &&
           (text.at(linkEnd - 1) == QLatin1Char('.') ||
            text.at(linkEnd - 1) == QLatin1Char(',') ||
            text.at(linkEnd - 1) == QLatin1Char(';') ||
            text.at(linkEnd - 1) == QLatin1Char(':') ||
            text.at(linkEnd - 1) == QLatin1Char('!') ||
            text.at(linkEnd - 1) == QLatin1Char('?')))
        --linkEnd;
    *out += text.mid(start, linkEnd - start);
    *out += text.mid(linkEnd, end - linkEnd);
    return end;
}

inline int linkForkMeshUrl(const QString &text, int start, QString *out)
{
    int end = start;
    while (end < text.size() && !text.at(end).isSpace())
        ++end;
    int linkEnd = end;
    while (linkEnd > start &&
           (text.at(linkEnd - 1) == QLatin1Char('.') ||
            text.at(linkEnd - 1) == QLatin1Char(',') ||
            text.at(linkEnd - 1) == QLatin1Char(';') ||
            text.at(linkEnd - 1) == QLatin1Char(':') ||
            text.at(linkEnd - 1) == QLatin1Char('!') ||
            text.at(linkEnd - 1) == QLatin1Char('?')))
        --linkEnd;
    const QString url = text.mid(start, linkEnd - start);
    out->append(QStringLiteral("[%1](%1)").arg(url));
    out->append(text.mid(linkEnd, end - linkEnd));
    return end;
}

inline int copyProtectedBracketLink(const QString &text, int start, QString *out)
{
    const int labelEnd = text.indexOf(QLatin1Char(']'), start + 1);
    if (labelEnd < 0)
        return start;

    if (labelEnd + 1 < text.size() && text.at(labelEnd + 1) == QLatin1Char('(')) {
        const int targetEnd = text.indexOf(QLatin1Char(')'), labelEnd + 2);
        if (targetEnd < 0)
            return start;
        *out += text.mid(start, targetEnd - start + 1);
        return targetEnd + 1;
    }

    if (labelEnd + 1 < text.size() && text.at(labelEnd + 1) == QLatin1Char('[')) {
        const int refEnd = text.indexOf(QLatin1Char(']'), labelEnd + 2);
        if (refEnd < 0)
            return start;
        *out += text.mid(start, refEnd - start + 1);
        return refEnd + 1;
    }

    return start;
}

inline int copyBacktickSpan(const QString &text, int start, QString *out)
{
    int tickCount = 0;
    while (start + tickCount < text.size() &&
           text.at(start + tickCount) == QLatin1Char('`'))
        ++tickCount;
    if (tickCount <= 0)
        return start;

    const QString delimiter(tickCount, QLatin1Char('`'));
    const int close = text.indexOf(delimiter, start + tickCount);
    if (close < 0) {
        *out += text.mid(start, tickCount);
        return start + tickCount;
    }
    *out += text.mid(start, close - start + tickCount);
    return close + tickCount;
}

inline bool isReferenceDefinitionLineStart(const QString &text, int start)
{
    int i = start;
    while (i < text.size() &&
           (text.at(i) == QLatin1Char(' ') || text.at(i) == QLatin1Char('\t')))
        ++i;
    if (i >= text.size() || text.at(i) != QLatin1Char('['))
        return false;
    const int close = text.indexOf(QLatin1String("]:"), i + 1);
    if (close < 0)
        return false;
    const int newline = text.indexOf(QLatin1Char('\n'), i + 1);
    return newline < 0 || close < newline;
}

inline QString issueLink(const QString &label, const QString &number)
{
    return QStringLiteral("[%1](fm-issue:%2)").arg(label, number);
}

inline QString pullLink(const QString &label, const QString &number)
{
    return QStringLiteral("[%1](fm-pull:%2)").arg(label, number);
}

inline QString commitLink(const QString &sha)
{
    return QStringLiteral("[%1](fm-commit:%1)").arg(sha);
}

inline QString linkifyMarkdownReferences(const QString &input)
{
    QString out;
    bool fencedCode = false;
    bool lineStart = true;

    for (int i = 0; i < input.size();) {
        if (lineStart && input.mid(i, 3) == QLatin1String("```")) {
            fencedCode = !fencedCode;
            out += input.mid(i, 3);
            i += 3;
            lineStart = false;
            continue;
        }

        if (lineStart && isReferenceDefinitionLineStart(input, i)) {
            const int newline = input.indexOf(QLatin1Char('\n'), i);
            const int end = newline < 0 ? input.size() : newline;
            out += input.mid(i, end - i);
            i = end;
            lineStart = false;
            continue;
        }

        const QChar c = input.at(i);
        if (c == QLatin1Char('\n')) {
            out += c;
            ++i;
            lineStart = true;
            continue;
        }
        lineStart = false;

        if (c == QLatin1Char('`')) {
            i = copyBacktickSpan(input, i, &out);
            continue;
        }

        if (fencedCode) {
            out += c;
            ++i;
            continue;
        }

        if (c == QLatin1Char('[')) {
            const int copied = copyProtectedBracketLink(input, i, &out);
            if (copied != i) {
                i = copied;
                continue;
            }
        }

        if (input.mid(i).startsWith(QLatin1String("forkmesh://"))) {
            i = linkForkMeshUrl(input, i, &out);
            continue;
        }

        if (input.mid(i).startsWith(QLatin1String("https://")) ||
            input.mid(i).startsWith(QLatin1String("http://"))) {
            i = copyUrl(input, i, &out);
            continue;
        }

        const QString rest = input.mid(i);
        if (isTokenBoundary(input, i - 1) &&
            rest.startsWith(QLatin1String("pull request #"),
                            Qt::CaseInsensitive)) {
            const int digitsStart = i + 14;
            const int digitsEnd = readDigits(input, digitsStart);
            if (digitsEnd > digitsStart) {
                const QString label = input.mid(i, digitsEnd - i);
                out += pullLink(label, input.mid(digitsStart, digitsEnd - digitsStart));
                i = digitsEnd;
                continue;
            }
        }

        if (isTokenBoundary(input, i - 1) &&
            rest.startsWith(QLatin1String("pull #"), Qt::CaseInsensitive)) {
            const int digitsStart = i + 6;
            const int digitsEnd = readDigits(input, digitsStart);
            if (digitsEnd > digitsStart) {
                const QString label = input.mid(i, digitsEnd - i);
                out += pullLink(label, input.mid(digitsStart, digitsEnd - digitsStart));
                i = digitsEnd;
                continue;
            }
        }

        if (isTokenBoundary(input, i - 1) &&
            rest.startsWith(QLatin1String("PR #"), Qt::CaseInsensitive)) {
            const int digitsStart = i + 4;
            const int digitsEnd = readDigits(input, digitsStart);
            if (digitsEnd > digitsStart) {
                const QString label = input.mid(i, digitsEnd - i);
                out += pullLink(label, input.mid(digitsStart, digitsEnd - digitsStart));
                i = digitsEnd;
                continue;
            }
        }

        if (isTokenBoundary(input, i - 1) &&
            rest.startsWith(QLatin1String("issue #"), Qt::CaseInsensitive)) {
            const int digitsStart = i + 7;
            const int digitsEnd = readDigits(input, digitsStart);
            if (digitsEnd > digitsStart) {
                const QString label = input.mid(i, digitsEnd - i);
                out += issueLink(label, input.mid(digitsStart, digitsEnd - digitsStart));
                i = digitsEnd;
                continue;
            }
        }

        if (c == QLatin1Char('#') && isTokenBoundary(input, i - 1)) {
            const int digitsStart = i + 1;
            const int digitsEnd = readDigits(input, digitsStart);
            if (digitsEnd > digitsStart) {
                const QString number = input.mid(digitsStart, digitsEnd - digitsStart);
                out += issueLink(QStringLiteral("#") + number, number);
                i = digitsEnd;
                continue;
            }
        }

        if (isHexChar(c) && isTokenBoundary(input, i - 1)) {
            const int end = readHexRun(input, i);
            const int len = end - i;
            if (len >= 7 && len <= 40 && isTokenBoundary(input, end)) {
                out += commitLink(input.mid(i, len));
                i = end;
                continue;
            }
        }

        out += c;
        ++i;
    }
    return out;
}

} // namespace ReferenceLinks
