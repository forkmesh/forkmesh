#pragma once

#include <QList>
#include <QString>

#include <optional>

namespace forkmesh::diffintra {

struct Range {
    qsizetype start = 0;
    qsizetype end = 0;
    bool operator==(const Range &o) const { return start == o.start && end == o.end; }
};

struct Changes {
    QList<Range> oldRanges;
    QList<Range> newRanges;
};

struct CellsHtml {
    QString oldHtml;
    QString newHtml;
};

std::optional<Changes> intraLineChanges(const QString &oldText, const QString &newText);

QString emphasizedHtml(const QString &text, const QList<Range> &ranges,
                       const QString &spanClass);

std::optional<CellsHtml> intraLineCellsHtml(const QString &oldText,
                                            const QString &newText,
                                            qsizetype maxHtmlChars);

} // namespace forkmesh::diffintra
