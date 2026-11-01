#pragma once

#include "NoteTools.h"

#include <QDate>
#include <QList>
#include <QLocale>
#include <QRegularExpression>
#include <QSizeF>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <optional>

namespace forkmesh::notes {

enum class PaperSize { A4, Letter, Legal };
enum class PaperOrientation { Portrait, Landscape };
enum class PageMargins { Narrow, Normal, Wide };

struct PageLayout {
    PaperSize size = PaperSize::A4;
    PaperOrientation orientation = PaperOrientation::Portrait;
    PageMargins margins = PageMargins::Normal;
    bool cover = false;
    bool contents = false;
    bool numbers = true;

    bool operator==(const PageLayout &other) const
    {
        return size == other.size && orientation == other.orientation &&
               margins == other.margins && cover == other.cover &&
               contents == other.contents && numbers == other.numbers;
    }
    bool operator!=(const PageLayout &other) const { return !(*this == other); }
};

inline constexpr int kSvgFigureMaxBytes = 262144;

inline QString paperSizeKey(PaperSize size)
{
    switch (size) {
    case PaperSize::Letter: return QStringLiteral("letter");
    case PaperSize::Legal: return QStringLiteral("legal");
    case PaperSize::A4: break;
    }
    return QStringLiteral("a4");
}

inline QString paperSizeLabel(PaperSize size)
{
    switch (size) {
    case PaperSize::Letter: return QStringLiteral("Letter");
    case PaperSize::Legal: return QStringLiteral("Legal");
    case PaperSize::A4: break;
    }
    return QStringLiteral("A4");
}

inline QString orientationKey(PaperOrientation orientation)
{
    return orientation == PaperOrientation::Landscape ? QStringLiteral("landscape")
                                                      : QStringLiteral("portrait");
}

inline QString orientationLabel(PaperOrientation orientation)
{
    return orientation == PaperOrientation::Landscape ? QStringLiteral("Landscape")
                                                      : QStringLiteral("Portrait");
}

inline QString marginsKey(PageMargins margins)
{
    switch (margins) {
    case PageMargins::Narrow: return QStringLiteral("narrow");
    case PageMargins::Wide: return QStringLiteral("wide");
    case PageMargins::Normal: break;
    }
    return QStringLiteral("normal");
}

inline QString marginsLabel(PageMargins margins)
{
    switch (margins) {
    case PageMargins::Narrow: return QStringLiteral("Narrow");
    case PageMargins::Wide: return QStringLiteral("Wide");
    case PageMargins::Normal: break;
    }
    return QStringLiteral("Normal");
}

inline QSizeF paperMillimetres(const PageLayout &layout)
{
    QSizeF paper(210.0, 297.0);
    if (layout.size == PaperSize::Letter)
        paper = QSizeF(215.9, 279.4);
    else if (layout.size == PaperSize::Legal)
        paper = QSizeF(215.9, 355.6);
    return layout.orientation == PaperOrientation::Landscape ? paper.transposed() : paper;
}

inline qreal marginMillimetres(PageMargins margins)
{
    switch (margins) {
    case PageMargins::Narrow: return 12.7;
    case PageMargins::Wide: return 38.1;
    case PageMargins::Normal: break;
    }
    return 25.4;
}

inline QStringList noteLines(const QString &markdown)
{
    QString text = markdown;
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    return text.split(QLatin1Char('\n'));
}

struct NoteFence {
    QChar mark;
    int length = 0;
    QString info;
};

inline std::optional<NoteFence> noteFenceOpening(const QString &line)
{
    const QString trimmed = line.trimmed();
    for (const QChar mark : {QLatin1Char('`'), QLatin1Char('~')}) {
        int run = 0;
        while (run < trimmed.size() && trimmed.at(run) == mark)
            ++run;
        if (run < 3)
            continue;
        const QString info = trimmed.mid(run);
        if (mark == QLatin1Char('`') && info.contains(QLatin1Char('`')))
            return std::nullopt;
        return NoteFence{mark, run, info};
    }
    return std::nullopt;
}

inline bool noteFenceCloses(const QString &line, const NoteFence &fence)
{
    const QString trimmed = line.trimmed();
    if (trimmed.size() < fence.length)
        return false;
    return std::all_of(trimmed.cbegin(), trimmed.cend(),
                       [&fence](QChar c) { return c == fence.mark; });
}

inline int noteFenceEnd(const QStringList &lines, int start, const NoteFence &fence)
{
    for (int index = start + 1; index < lines.size(); ++index) {
        if (noteFenceCloses(lines.at(index), fence))
            return index;
    }
    return int(lines.size()) - 1;
}

inline std::optional<QString> noteCommentBody(const QString &line)
{
    const QString trimmed = line.trimmed();
    if (trimmed.size() < 7 || !trimmed.startsWith(QStringLiteral("<!--")) ||
        !trimmed.endsWith(QStringLiteral("-->")))
        return std::nullopt;
    return trimmed.mid(4, trimmed.size() - 7);
}

inline std::optional<QString> pageLayoutSettings(const QString &line)
{
    const std::optional<QString> body = noteCommentBody(line);
    if (!body)
        return std::nullopt;
    const QString lead = body->trimmed();
    if (!lead.left(5).toLower().startsWith(QStringLiteral("page:")))
        return std::nullopt;
    return lead.mid(5);
}

inline bool isPageLayoutLine(const QString &line)
{
    return pageLayoutSettings(line).has_value();
}

inline bool isPageBreakLine(const QString &line)
{
    const std::optional<QString> body = noteCommentBody(line);
    return body && body->trimmed().toLower() == QLatin1String("pagebreak");
}

inline PageLayout parsePageLayoutSettings(const QString &settings)
{
    PageLayout layout;
    const auto flag = [](const QString &value, bool fallback) {
        if (value == QLatin1String("yes"))
            return true;
        if (value == QLatin1String("no"))
            return false;
        return fallback;
    };
    for (const QString &pair : settings.split(QLatin1Char(';'))) {
        const QStringList parts = pair.split(QLatin1Char('='));
        const QString key = parts.value(0).trimmed().toLower();
        const QString value = parts.value(1).trimmed().toLower();
        if (key == QLatin1String("size")) {
            if (value == QLatin1String("a4"))
                layout.size = PaperSize::A4;
            else if (value == QLatin1String("letter"))
                layout.size = PaperSize::Letter;
            else if (value == QLatin1String("legal"))
                layout.size = PaperSize::Legal;
        } else if (key == QLatin1String("orientation")) {
            if (value == QLatin1String("portrait"))
                layout.orientation = PaperOrientation::Portrait;
            else if (value == QLatin1String("landscape"))
                layout.orientation = PaperOrientation::Landscape;
        } else if (key == QLatin1String("margins")) {
            if (value == QLatin1String("narrow"))
                layout.margins = PageMargins::Narrow;
            else if (value == QLatin1String("normal"))
                layout.margins = PageMargins::Normal;
            else if (value == QLatin1String("wide"))
                layout.margins = PageMargins::Wide;
        } else if (key == QLatin1String("cover")) {
            layout.cover = flag(value, layout.cover);
        } else if (key == QLatin1String("contents")) {
            layout.contents = flag(value, layout.contents);
        } else if (key == QLatin1String("numbers")) {
            layout.numbers = flag(value, layout.numbers);
        }
    }
    return layout;
}

inline QList<int> pageLayoutLineIndices(const QStringList &lines)
{
    QList<int> found;
    int index = 0;
    while (index < lines.size()) {
        if (const std::optional<NoteFence> fence = noteFenceOpening(lines.at(index))) {
            index = noteFenceEnd(lines, index, *fence) + 1;
            continue;
        }
        if (isPageLayoutLine(lines.at(index)))
            found.append(index);
        ++index;
    }
    return found;
}

inline PageLayout notePageLayout(const QString &markdown)
{
    const QStringList lines = noteLines(markdown);
    const QList<int> found = pageLayoutLineIndices(lines);
    if (found.isEmpty())
        return {};
    return parsePageLayoutSettings(pageLayoutSettings(lines.at(found.first())).value_or(QString()));
}

inline bool hasPageLayoutLine(const QString &markdown)
{
    return !pageLayoutLineIndices(noteLines(markdown)).isEmpty();
}

inline QString pageLayoutLine(const PageLayout &layout)
{
    const auto word = [](bool value) {
        return value ? QStringLiteral("yes") : QStringLiteral("no");
    };
    return QStringLiteral("<!-- page: size=%1; orientation=%2; margins=%3; cover=%4; "
                          "contents=%5; numbers=%6 -->")
        .arg(paperSizeKey(layout.size), orientationKey(layout.orientation),
             marginsKey(layout.margins), word(layout.cover), word(layout.contents),
             word(layout.numbers));
}

inline QString withLayout(const QString &markdown, const PageLayout &layout)
{
    QStringList lines = noteLines(markdown);
    const QList<int> found = pageLayoutLineIndices(lines);
    if (!found.isEmpty())
        lines.removeAt(found.first());
    while (!lines.isEmpty() && lines.first().trimmed().isEmpty())
        lines.removeFirst();
    return pageLayoutLine(layout) + QStringLiteral("\n\n") + lines.join(QLatin1Char('\n'));
}

inline QString withoutLayoutLines(const QString &markdown)
{
    QStringList lines = noteLines(markdown);
    const QList<int> found = pageLayoutLineIndices(lines);
    for (auto it = found.crbegin(); it != found.crend(); ++it)
        lines.removeAt(*it);
    return lines.join(QLatin1Char('\n'));
}

struct NoteFigure {
    QString svg;
    QString caption;
};

inline QString afterSvgProlog(const QString &body)
{
    int at = 0;
    while (true) {
        while (at < body.size() && body.at(at).isSpace())
            ++at;
        if (body.mid(at, 5) == QLatin1String("<?xml")) {
            const int end = body.indexOf(QStringLiteral("?>"), at + 5);
            if (end >= 0) {
                at = end + 2;
                continue;
            }
        }
        if (body.mid(at, 4) == QLatin1String("<!--")) {
            const int end = body.indexOf(QStringLiteral("-->"), at + 4);
            if (end >= 0) {
                at = end + 3;
                continue;
            }
        }
        return body.mid(at);
    }
}

inline bool startsWithSvgElement(const QString &body)
{
    const QString rest = afterSvgProlog(body);
    if (rest.size() < 5 || rest.left(4).toLower() != QLatin1String("<svg"))
        return false;
    const QChar next = rest.at(4);
    return next.isSpace() || next == QLatin1Char('>') || next == QLatin1Char('/');
}

inline std::optional<NoteFigure> svgFigure(const QString &info, const QString &body)
{
    const QString words = info.trimmed();
    int wordEnd = 0;
    while (wordEnd < words.size() && !words.at(wordEnd).isSpace())
        ++wordEnd;
    if (words.left(wordEnd).toLower() != QLatin1String("svg"))
        return std::nullopt;
    if (body.toUtf8().size() > kSvgFigureMaxBytes || !startsWithSvgElement(body))
        return std::nullopt;
    return NoteFigure{body, words.mid(wordEnd).trimmed()};
}

struct NoteFigureSize {
    qreal width = 0;
    qreal aspect = 0.5;
    bool hasWidth() const { return width > 0; }
};

inline QString svgRootTag(const QString &svg)
{
    const QString start = afterSvgProlog(svg);
    const int end = start.indexOf(QLatin1Char('>'));
    return end < 0 ? start : start.left(end);
}

inline QString svgAttribute(const QString &tag, const QString &name)
{
    const QRegularExpression pattern(
        QStringLiteral("[ \\t\\r\\n]%1[ \\t\\r\\n]*=[ \\t\\r\\n]*(?:\"([^\"]*)\"|'([^']*)')")
            .arg(QRegularExpression::escape(name)));
    const QRegularExpressionMatch match = pattern.match(tag);
    if (!match.hasMatch())
        return QString();
    return match.capturedStart(1) >= 0 ? match.captured(1) : match.captured(2);
}

inline qreal svgLength(const QString &text)
{
    const QString trimmed = text.trimmed().toLower();
    int digits = 0;
    while (digits < trimmed.size() &&
           QStringLiteral("0123456789.+-").contains(trimmed.at(digits)))
        ++digits;
    bool ok = false;
    const qreal value = trimmed.left(digits).toDouble(&ok);
    if (!ok || value <= 0)
        return 0;
    const QString unit = trimmed.mid(digits).trimmed();
    if (unit.isEmpty() || unit == QLatin1String("px"))
        return value;
    if (unit == QLatin1String("pt"))
        return value * 4.0 / 3.0;
    if (unit == QLatin1String("pc") || unit == QLatin1String("em"))
        return value * 16.0;
    if (unit == QLatin1String("mm"))
        return value * 96.0 / 25.4;
    if (unit == QLatin1String("cm"))
        return value * 96.0 / 2.54;
    if (unit == QLatin1String("in"))
        return value * 96.0;
    return 0;
}

inline QSizeF svgViewBox(const QString &text)
{
    static const QRegularExpression separators(QStringLiteral("[\\s,]+"));
    const QStringList numbers = text.trimmed().split(separators, Qt::SkipEmptyParts);
    if (numbers.size() != 4)
        return {};
    bool okWidth = false;
    bool okHeight = false;
    const qreal width = numbers.at(2).toDouble(&okWidth);
    const qreal height = numbers.at(3).toDouble(&okHeight);
    if (!okWidth || !okHeight || width <= 0 || height <= 0)
        return {};
    return QSizeF(width, height);
}

inline NoteFigureSize svgFigureSize(const QString &svg)
{
    const QString tag = svgRootTag(svg);
    const qreal width = svgLength(svgAttribute(tag, QStringLiteral("width")));
    const qreal height = svgLength(svgAttribute(tag, QStringLiteral("height")));
    const QSizeF box = svgViewBox(svgAttribute(tag, QStringLiteral("viewBox")));
    NoteFigureSize size;
    if (width > 0 && height > 0)
        size = {width, height / width};
    else if (width > 0 && !box.isEmpty())
        size = {width, box.height() / box.width()};
    else if (height > 0 && !box.isEmpty())
        size = {height * box.width() / box.height(), box.height() / box.width()};
    else if (!box.isEmpty())
        size = {0, box.height() / box.width()};
    else if (width > 0)
        size = {width, 150.0 / width};
    else if (height > 0)
        size = {300, height / 300.0};
    else
        size = {300, 0.5};
    size.aspect = std::clamp(size.aspect, 0.05, 20.0);
    return size;
}

inline QSizeF figureDisplaySize(const NoteFigureSize &size, qreal columnWidth)
{
    const qreal column = qMax<qreal>(1, columnWidth);
    const qreal width = size.hasWidth() ? qMin(size.width, column) : column;
    return QSizeF(width, width * size.aspect);
}

enum class NotePartKind { Markdown, Figure, PageBreak };

struct NotePart {
    NotePartKind kind = NotePartKind::Markdown;
    QString markdown;
    NoteFigure figure;
};

inline QList<NotePart> noteParts(const QString &markdown)
{
    const QStringList lines = noteLines(markdown);
    QList<NotePart> parts;
    QStringList buffer;
    const auto flush = [&parts, &buffer] {
        while (!buffer.isEmpty() && buffer.first().trimmed().isEmpty())
            buffer.removeFirst();
        while (!buffer.isEmpty() && buffer.last().trimmed().isEmpty())
            buffer.removeLast();
        if (!buffer.isEmpty())
            parts.append({NotePartKind::Markdown, buffer.join(QLatin1Char('\n')), {}});
        buffer.clear();
    };
    int index = 0;
    while (index < lines.size()) {
        const QString &line = lines.at(index);
        if (const std::optional<NoteFence> fence = noteFenceOpening(line)) {
            const int end = noteFenceEnd(lines, index, *fence);
            const bool closed = end > index && noteFenceCloses(lines.at(end), *fence);
            const int bodyEnd = closed ? end : int(lines.size());
            const QString body = lines.mid(index + 1, qMax(0, bodyEnd - index - 1))
                                     .join(QLatin1Char('\n'));
            if (const std::optional<NoteFigure> figure = svgFigure(fence->info, body)) {
                flush();
                parts.append({NotePartKind::Figure, QString(), *figure});
            } else {
                buffer.append(lines.mid(index, end - index + 1));
            }
            index = end + 1;
            continue;
        }
        if (isPageBreakLine(line)) {
            flush();
            parts.append({NotePartKind::PageBreak, QString(), {}});
        } else if (!isPageLayoutLine(line)) {
            buffer.append(line);
        }
        ++index;
    }
    flush();
    return parts;
}

struct PlaceholderMarkdown {
    QString markdown;
    QList<NotePart> objects;
};

inline QString notePlaceholder(const QString &token, int index)
{
    return token + QString::number(index);
}

inline PlaceholderMarkdown withObjectPlaceholders(const QString &markdown,
                                                  const QString &token)
{
    PlaceholderMarkdown out;
    QStringList chunks;
    for (const NotePart &part : noteParts(markdown)) {
        if (part.kind == NotePartKind::Markdown) {
            chunks.append(part.markdown);
            continue;
        }
        chunks.append(notePlaceholder(token, int(out.objects.size())));
        out.objects.append(part);
    }
    out.markdown = chunks.join(QStringLiteral("\n\n"));
    return out;
}

inline int notePlaceholderIndex(const QString &blockText, const QString &token, int count)
{
    if (token.isEmpty() || !blockText.startsWith(token))
        return -1;
    bool ok = false;
    const int index = blockText.mid(token.size()).toInt(&ok);
    if (!ok || index < 0 || index >= count ||
        blockText != notePlaceholder(token, index))
        return -1;
    return index;
}

struct NoteHeading {
    int level = 0;
    QString text;
    bool operator==(const NoteHeading &other) const
    {
        return level == other.level && text == other.text;
    }
};

inline QString plainHeadingText(QString text)
{
    static const QRegularExpression image(QStringLiteral("!\\[([^\\]]*)\\]\\([^)]*\\)"));
    static const QRegularExpression link(QStringLiteral("\\[([^\\]]*)\\]\\([^)]*\\)"));
    static const QRegularExpression marks(QStringLiteral("\\*\\*|__|~~|`|\\*"));
    static const QRegularExpression escaped(QStringLiteral("\\\\([!-/:-@\\[-`{-~])"));
    static const QRegularExpression spaces(QStringLiteral("\\s+"));
    text.replace(image, QStringLiteral("\\1"));
    text.replace(link, QStringLiteral("\\1"));
    text.remove(marks);
    text.replace(escaped, QStringLiteral("\\1"));
    text.replace(spaces, QStringLiteral(" "));
    return text.trimmed();
}

inline QList<NoteHeading> noteHeadings(const QString &markdown)
{
    static const QRegularExpression atx(
        QStringLiteral("^ {0,3}(#{1,6})(?:[ \\t]+(.*?))?(?:[ \\t]+#+)?[ \\t]*$"));
    static const QRegularExpression setextOne(QStringLiteral("^ {0,3}=+[ \\t]*$"));
    static const QRegularExpression setextTwo(QStringLiteral("^ {0,3}-+[ \\t]*$"));
    static const QRegularExpression blockStart(
        QStringLiteral("^\\s*(?:[-*+]\\s|\\d+[.)]\\s|>|#|\\||<|```|~~~)|^ {4}"));
    QList<NoteHeading> headings;
    QStringList paragraph;
    for (const NotePart &part : noteParts(markdown)) {
        paragraph.clear();
        if (part.kind != NotePartKind::Markdown)
            continue;
        const QStringList lines = noteLines(part.markdown);
        int index = 0;
        while (index < lines.size()) {
            const QString &line = lines.at(index);
            if (const std::optional<NoteFence> fence = noteFenceOpening(line)) {
                index = noteFenceEnd(lines, index, *fence) + 1;
                paragraph.clear();
                continue;
            }
            ++index;
            const QRegularExpressionMatch heading = atx.match(line);
            if (heading.hasMatch()) {
                paragraph.clear();
                const int level = int(heading.captured(1).size());
                const QString text = plainHeadingText(heading.captured(2));
                if (level <= 2 && !text.isEmpty())
                    headings.append({level, text});
                continue;
            }
            if (!paragraph.isEmpty() &&
                (setextOne.match(line).hasMatch() || setextTwo.match(line).hasMatch())) {
                const QString text = plainHeadingText(paragraph.join(QLatin1Char(' ')));
                if (!text.isEmpty())
                    headings.append({setextOne.match(line).hasMatch() ? 1 : 2, text});
                paragraph.clear();
                continue;
            }
            if (line.trimmed().isEmpty() || blockStart.match(line).hasMatch())
                paragraph.clear();
            else
                paragraph.append(line.trimmed());
        }
    }
    return headings;
}

inline QString pdfFileName(const QString &title)
{
    QString name = exportFileName(title);
    name.chop(3);
    return name + QStringLiteral(".pdf");
}

inline QString noteDocumentTitle(const QString &title)
{
    const QString trimmed = title.trimmed();
    return trimmed.isEmpty() ? QStringLiteral("Untitled note") : trimmed;
}

inline QString exportDateLabel(const QDate &date)
{
    return QLocale(QLocale::English, QLocale::UnitedStates)
        .toString(date, QStringLiteral("MMMM d, yyyy"));
}

inline QString pageNumberLabel(int page, int total)
{
    return QStringLiteral("%1 / %2").arg(page).arg(total);
}

inline bool showsPageNumber(int pageIndex, const PageLayout &layout)
{
    return layout.numbers && !(layout.cover && pageIndex == 0);
}

}
