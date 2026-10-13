#include "DiffIntraLine.h"

#include <QRegularExpression>

#include <vector>

namespace forkmesh::diffintra {

namespace {

constexpr qsizetype kMaxCells = 40000;
constexpr double kMaxChangedShare = 0.7;

struct Token {
    qsizetype start = 0;
    qsizetype length = 0;
};

QList<Token> tokenize(const QString &text)
{
    static const QRegularExpression re(QStringLiteral("\\w+|\\s+|[^\\w\\s]"),
                                       QRegularExpression::UseUnicodePropertiesOption);
    QList<Token> out;
    auto it = re.globalMatch(text);
    while (it.hasNext()) {
        const auto m = it.next();
        out.append({m.capturedStart(), m.capturedLength()});
    }
    return out;
}

QList<Range> changedRanges(const QList<Token> &tokens, const std::vector<bool> &keep)
{
    QList<Range> out;
    for (qsizetype k = 0; k < tokens.size(); ++k) {
        if (keep[size_t(k)])
            continue;
        const qsizetype start = tokens[k].start;
        const qsizetype end = start + tokens[k].length;
        if (!out.isEmpty() && out.last().end == start)
            out.last().end = end;
        else
            out.append({start, end});
    }
    return out;
}

double changedShare(const QList<Range> &ranges, qsizetype length)
{
    qsizetype covered = 0;
    for (const Range &r : ranges)
        covered += r.end - r.start;
    return double(covered) / double(length > 0 ? length : 1);
}

} // namespace

std::optional<Changes> intraLineChanges(const QString &oldText, const QString &newText)
{
    if (oldText == newText)
        return std::nullopt;
    const QList<Token> ta = tokenize(oldText);
    const QList<Token> tb = tokenize(newText);
    if (ta.isEmpty() || tb.isEmpty() || ta.size() * tb.size() > kMaxCells)
        return std::nullopt;

    const auto same = [&](qsizetype i, qsizetype j) {
        return QStringView(oldText).mid(ta[i].start, ta[i].length) ==
               QStringView(newText).mid(tb[j].start, tb[j].length);
    };

    qsizetype prefix = 0;
    while (prefix < ta.size() && prefix < tb.size() && same(prefix, prefix))
        ++prefix;
    qsizetype suffix = 0;
    while (suffix < ta.size() - prefix && suffix < tb.size() - prefix &&
           same(ta.size() - 1 - suffix, tb.size() - 1 - suffix))
        ++suffix;

    const qsizetype na = ta.size() - prefix - suffix;
    const qsizetype nb = tb.size() - prefix - suffix;
    std::vector<bool> keepA(size_t(ta.size()), true);
    std::vector<bool> keepB(size_t(tb.size()), true);
    if (na > 0 || nb > 0) {
        const qsizetype width = nb + 1;
        std::vector<int> table(size_t((na + 1) * width), 0);
        const auto at = [&](qsizetype i, qsizetype j) -> int & {
            return table[size_t(i * width + j)];
        };
        for (qsizetype i = na - 1; i >= 0; --i) {
            for (qsizetype j = nb - 1; j >= 0; --j)
                at(i, j) = same(prefix + i, prefix + j)
                               ? at(i + 1, j + 1) + 1
                               : qMax(at(i + 1, j), at(i, j + 1));
        }
        for (qsizetype k = 0; k < na; ++k)
            keepA[size_t(prefix + k)] = false;
        for (qsizetype k = 0; k < nb; ++k)
            keepB[size_t(prefix + k)] = false;
        qsizetype i = 0, j = 0;
        while (i < na && j < nb) {
            if (same(prefix + i, prefix + j)) {
                keepA[size_t(prefix + i)] = true;
                keepB[size_t(prefix + j)] = true;
                ++i;
                ++j;
            } else if (at(i + 1, j) >= at(i, j + 1)) {
                ++i;
            } else {
                ++j;
            }
        }
    }

    Changes out{changedRanges(ta, keepA), changedRanges(tb, keepB)};
    if (changedShare(out.oldRanges, oldText.size()) > kMaxChangedShare &&
        changedShare(out.newRanges, newText.size()) > kMaxChangedShare)
        return std::nullopt;
    return out;
}

QString emphasizedHtml(const QString &text, const QList<Range> &ranges,
                       const QString &spanClass)
{
    QString html;
    html.reserve(text.size() + ranges.size() * 32);
    qsizetype pos = 0;
    for (const Range &r : ranges) {
        html += text.mid(pos, r.start - pos).toHtmlEscaped();
        html += QStringLiteral("<span class='%1'>").arg(spanClass);
        html += text.mid(r.start, r.end - r.start).toHtmlEscaped();
        html += QStringLiteral("</span>");
        pos = r.end;
    }
    html += text.mid(pos).toHtmlEscaped();
    return html;
}

std::optional<CellsHtml> intraLineCellsHtml(const QString &oldText,
                                            const QString &newText,
                                            qsizetype maxHtmlChars)
{
    if (oldText.size() + newText.size() > maxHtmlChars)
        return std::nullopt;
    const std::optional<Changes> changes = intraLineChanges(oldText, newText);
    if (!changes)
        return std::nullopt;
    CellsHtml out{emphasizedHtml(oldText, changes->oldRanges, QStringLiteral("delx")),
                  emphasizedHtml(newText, changes->newRanges, QStringLiteral("addx"))};
    if (out.oldHtml.size() + out.newHtml.size() > maxHtmlChars)
        return std::nullopt;
    return out;
}

} // namespace forkmesh::diffintra
