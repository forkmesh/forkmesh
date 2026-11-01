#pragma once

#include "MarkdownRender.h"
#include "NoteDocument.h"

#include <QAbstractTextDocumentLayout>
#include <QByteArray>
#include <QColor>
#include <QCryptographicHash>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QHash>
#include <QImage>
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QPdfWriter>
#include <QSaveFile>
#include <QSvgRenderer>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextFrame>
#include <QTextLayout>
#include <QTextList>
#include <QTextTable>
#include <QUrl>
#include <QUuid>

#include <cmath>
#include <functional>

namespace forkmesh::notes {

enum NoteRenderProperty {
    NoteObjectIndex = QTextFormat::UserProperty + 0x6e01,
    NoteKeepTogether,
    NoteKeepWithNext,
    NoteObjectKind,
};

inline QString svgWithoutExternalReferences(QString svg)
{
    static const QRegularExpression external(
        QStringLiteral("(\\s(?:xlink:)?href\\s*=\\s*)(\"|')(?!\\s*(?:data:|#))[^\"']*\\2"),
        QRegularExpression::CaseInsensitiveOption);
    svg.replace(external, QStringLiteral("\\1\\2\\2"));
    return svg;
}

inline void paintSvgFigure(QPainter *painter, const QString &svg, const QRectF &target)
{
    QSvgRenderer renderer(svgWithoutExternalReferences(svg).toUtf8());
    if (!renderer.isValid())
        return;
    renderer.setAspectRatioMode(Qt::KeepAspectRatio);
    renderer.render(painter, target);
}

inline bool svgFigureRenders(const QString &svg)
{
    QSvgRenderer renderer(svgWithoutExternalReferences(svg).toUtf8());
    return renderer.isValid();
}

inline QImage svgFigureImage(const QString &svg, const QSize &pixels,
                             const QColor &background = QColor(Qt::white))
{
    if (pixels.isEmpty())
        return {};
    QImage image(pixels, QImage::Format_ARGB32_Premultiplied);
    image.fill(background);
    QPainter painter(&image);
    painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    paintSvgFigure(&painter, svg, QRectF(QPointF(0, 0), QSizeF(pixels)));
    painter.end();
    return image;
}

struct NoteRenderCache {
    QHash<QString, QImage> images;

    QImage take(const QString &key, const std::function<QImage()> &make)
    {
        const auto found = images.constFind(key);
        if (found != images.constEnd())
            return found.value();
        if (images.size() >= 48)
            images.clear();
        const QImage image = make();
        images.insert(key, image);
        return image;
    }
};

struct NotePreviewStyle {
    qreal columnWidth = 600;
    qreal devicePixelRatio = 1;
    QColor muted = QColor(QStringLiteral("#59636e"));
    QFont font;
};

inline constexpr qreal kPreviewCardPadding = 12;
inline constexpr qreal kPreviewBreakHeight = 28;

inline QString noteObjectToken()
{
    return QStringLiteral("fmnoteobject") +
           QUuid::createUuid().toString(QUuid::Id128) + QStringLiteral("x");
}

inline QImage previewFigureCard(const NoteFigure &figure, const QSizeF &card, qreal dpr)
{
    const QSize pixels(qMax(1, qRound(card.width() * dpr)), qMax(1, qRound(card.height() * dpr)));
    QImage image(pixels, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    image.setDevicePixelRatio(dpr);
    QPainter painter(&image);
    painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    QPainterPath path;
    path.addRoundedRect(QRectF(0.5, 0.5, card.width() - 1, card.height() - 1), 8, 8);
    painter.fillPath(path, QColor(Qt::white));
    painter.setPen(QPen(QColor(QStringLiteral("#d0d7de")), 1));
    painter.drawPath(path);
    paintSvgFigure(&painter, figure.svg,
                   QRectF(kPreviewCardPadding, kPreviewCardPadding,
                          card.width() - 2 * kPreviewCardPadding,
                          card.height() - 2 * kPreviewCardPadding));
    painter.end();
    return image;
}

inline QImage previewPageBreak(qreal width, qreal dpr, const QColor &muted, QFont font)
{
    const QSizeF size(qMax<qreal>(40, width), kPreviewBreakHeight);
    QImage image(QSize(qRound(size.width() * dpr), qRound(size.height() * dpr)),
                 QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    image.setDevicePixelRatio(dpr);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    font.setPointSizeF(qMax<qreal>(7, font.pointSizeF() * 0.85));
    painter.setFont(font);
    const QString label = QStringLiteral("Page break");
    const qreal labelWidth = QFontMetricsF(font).horizontalAdvance(label) + 24;
    const qreal y = size.height() / 2;
    QPen pen(muted, 1, Qt::DashLine);
    painter.setPen(pen);
    const qreal left = (size.width() - labelWidth) / 2;
    painter.drawLine(QPointF(0, y), QPointF(qMax<qreal>(0, left), y));
    painter.drawLine(QPointF(qMin(size.width(), left + labelWidth), y),
                     QPointF(size.width(), y));
    painter.setPen(muted);
    painter.drawText(QRectF(left, 0, labelWidth, size.height()), Qt::AlignCenter, label);
    painter.end();
    return image;
}

inline QString svgKey(const QString &svg)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(svg.toUtf8(), QCryptographicHash::Sha1).toHex());
}

inline QTextImageFormat previewObjectImage(QTextDocument *doc, const NotePart &part, int index,
                                           const NotePreviewStyle &style, NoteRenderCache &cache)
{
    const qreal dpr = qMax<qreal>(1, style.devicePixelRatio);
    const qreal column = qMax<qreal>(40, style.columnWidth);
    QTextImageFormat format;
    format.setProperty(NoteObjectIndex, index);
    format.setProperty(NoteObjectKind, int(part.kind));
    if (part.kind == NotePartKind::Figure) {
        const QSizeF svg = figureDisplaySize(svgFigureSize(part.figure.svg),
                                             column - 2 * kPreviewCardPadding);
        const QSizeF card(qRound(svg.width() + 2 * kPreviewCardPadding),
                          qRound(svg.height() + 2 * kPreviewCardPadding));
        const QString key = QStringLiteral("fmnote-figure:%1:%2x%3@%4")
                                .arg(svgKey(part.figure.svg))
                                .arg(card.width())
                                .arg(card.height())
                                .arg(dpr);
        const QImage image = cache.take(key, [&] {
            return previewFigureCard(part.figure, card, dpr);
        });
        doc->addResource(QTextDocument::ImageResource, QUrl(key), image);
        format.setName(key);
        format.setWidth(card.width());
        format.setHeight(card.height());
    } else {
        const qreal width = qRound(column);
        const QString key = QStringLiteral("fmnote-pagebreak:%1@%2:%3")
                                .arg(width)
                                .arg(dpr)
                                .arg(style.muted.name(QColor::HexArgb));
        const QImage image = cache.take(key, [&] {
            return previewPageBreak(width, dpr, style.muted, style.font);
        });
        doc->addResource(QTextDocument::ImageResource, QUrl(key), image);
        format.setName(key);
        format.setWidth(width);
        format.setHeight(kPreviewBreakHeight);
    }
    return format;
}

inline void applyNotePreviewObjects(QTextDocument *doc, const QString &token,
                                    const QList<NotePart> &objects,
                                    const NotePreviewStyle &style, NoteRenderCache &cache)
{
    if (!doc || objects.isEmpty())
        return;
    struct Place {
        int start = 0;
        int length = 0;
        int index = 0;
        bool placeholder = false;
    };
    QList<Place> places;
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        const int index = notePlaceholderIndex(block.text(), token, int(objects.size()));
        if (index >= 0) {
            places.append({block.position(), block.length() - 1, index, true});
            continue;
        }
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid() || !fragment.charFormat().isImageFormat() ||
                !fragment.charFormat().hasProperty(NoteObjectIndex))
                continue;
            const int object = fragment.charFormat().intProperty(NoteObjectIndex);
            if (object >= 0 && object < objects.size())
                places.append({fragment.position(), fragment.length(), object, false});
        }
    }
    if (places.isEmpty())
        return;
    QTextCursor cursor(doc);
    cursor.beginEditBlock();
    for (auto it = places.crbegin(); it != places.crend(); ++it) {
        const NotePart &part = objects.at(it->index);
        const QTextImageFormat image = previewObjectImage(doc, part, it->index, style, cache);
        cursor.setPosition(it->start);
        cursor.setPosition(it->start + it->length, QTextCursor::KeepAnchor);
        if (!it->placeholder) {
            cursor.setCharFormat(image);
            continue;
        }
        cursor.removeSelectedText();
        QTextBlockFormat block = cursor.blockFormat();
        block.setAlignment(Qt::AlignHCenter);
        cursor.setBlockFormat(block);
        cursor.insertImage(image);
        if (part.kind == NotePartKind::Figure && !part.figure.caption.isEmpty()) {
            QTextCharFormat caption;
            caption.setForeground(style.muted);
            if (style.font.pointSizeF() > 0)
                caption.setFontPointSize(qMax<qreal>(7, style.font.pointSizeF() * 0.85));
            cursor.insertText(QString(QChar(QChar::LineSeparator)), caption);
            cursor.insertText(part.figure.caption, caption);
        }
    }
    cursor.endEditBlock();
}

struct NotePdfInput {
    QString title;
    QString emoji;
    QString markdown;
    PageLayout layout;
    QDate date;
};

struct NotePdfResult {
    bool ok = false;
    int pages = 0;
    QString error;
};

inline constexpr int kNotePdfResolution = 300;

inline QString escapedMarkdownText(const QString &text)
{
    QString out;
    out.reserve(text.size() * 2);
    for (const QChar c : text) {
        if (c.unicode() < 128 && c.isPrint() && !c.isLetterOrNumber() && !c.isSpace())
            out.append(QLatin1Char('\\'));
        out.append(c);
    }
    return out;
}

struct NotePdfMetrics {
    qreal dpi = kNotePdfResolution;
    qreal px(qreal points) const { return points * 96.0 / 72.0; }
    qreal css(qreal device) const { return device * 96.0 / dpi; }
    qreal device(qreal css) const { return css * dpi / 96.0; }
};

inline QImage notePdfEmojiImage(const QString &emoji, int pixels)
{
    QImage image(QSize(pixels * 2, pixels), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
    QFont font = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    font.setPixelSize(qMax(8, qRound(pixels * 0.78)));
    painter.setFont(font);
    painter.setPen(QColor(QStringLiteral("#1f2328")));
    painter.drawText(QRectF(0, 0, image.width(), image.height()), Qt::AlignCenter, emoji);
    painter.end();
    QRect used;
    for (int y = 0; y < image.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(line[x]) > 0)
                used |= QRect(x, y, 1, 1);
        }
    }
    if (used.isEmpty())
        return {};
    const int width = qMax(used.width(), pixels);
    const int left = qMax(0, used.center().x() - width / 2);
    return image.copy(QRect(left, 0, qMin(width, image.width() - left), image.height()));
}

inline void styleNotePdfDocument(QTextDocument *doc, const NotePdfMetrics &m)
{
    const QColor muted(QStringLiteral("#57606a"));
    const QColor border(QStringLiteral("#d0d7de"));
    const QColor panel(QStringLiteral("#f6f8fa"));
    const QColor link(QStringLiteral("#0969da"));
    const QStringList mono = QFontDatabase::systemFont(QFontDatabase::FixedFont).families();
    static const qreal headingSizes[] = {20, 15, 12.5, 11, 10, 10};

    QTextCharFormat codeText;
    codeText.setFontFamilies(mono);
    codeText.setFontFixedPitch(true);
    codeText.setFontPointSize(9);
    QTextCharFormat inlineCode = codeText;
    inlineCode.setBackground(panel);
    QTextCharFormat anchorText;
    anchorText.setForeground(link);
    anchorText.setFontUnderline(false);
    QTextCharFormat quoteText;
    quoteText.setForeground(muted);

    struct Span {
        int start = 0;
        int end = 0;
        QTextCharFormat format;
    };
    QList<Span> spans;
    struct HeadingRun {
        int start = 0;
        int end = 0;
        int level = 1;
    };
    QList<HeadingRun> headingRuns;
    QList<QPair<int, int>> codeRuns;
    QList<QTextTable *> tables;
    QTextCursor cursor(doc);
    cursor.beginEditBlock();
    QTextFrame *root = doc->rootFrame();
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        QTextBlockFormat format = block.blockFormat();
        const QTextCursor at(block);
        QTextTable *table = at.currentTable();
        if (table && !tables.contains(table))
            tables.append(table);
        const int quoteLevel = format.intProperty(QTextFormat::BlockQuoteLevel);
        const bool code = format.hasProperty(QTextFormat::BlockCodeFence) ||
                          format.nonBreakableLines();
        const int start = block.position();
        const int end = block.position() + block.length() - 1;
        if (code) {
            format.clearBackground();
            format.setTopMargin(0);
            format.setBottomMargin(0);
            format.setLineHeight(140, QTextBlockFormat::ProportionalHeight);
            format.setProperty(NoteKeepTogether, true);
            if (at.currentFrame() == root) {
                if (!codeRuns.isEmpty() && codeRuns.last().second + 1 == start)
                    codeRuns.last().second = end;
                else
                    codeRuns.append({start, end});
            }
            spans.append({start, end, codeText});
        } else if (format.headingLevel() > 0) {
            const int level = qBound(1, format.headingLevel(), 6);
            format.setTopMargin(m.px(16));
            format.setBottomMargin(m.px(6));
            format.setLineHeight(125, QTextBlockFormat::ProportionalHeight);
            format.setProperty(NoteKeepWithNext, true);
            headingRuns.append({start, end, level});
        } else if (format.hasProperty(QTextFormat::BlockTrailingHorizontalRulerWidth)) {
            format.setTopMargin(m.px(12));
            format.setBottomMargin(m.px(12));
            format.setForeground(border);
        } else if (table) {
            format.setTopMargin(0);
            format.setBottomMargin(0);
            format.setLineHeight(135, QTextBlockFormat::ProportionalHeight);
            QTextCharFormat cell;
            cell.setFontPointSize(9.5);
            spans.append({start, end, cell});
        } else {
            const bool listed = block.textList() != nullptr;
            format.setTopMargin(0);
            format.setBottomMargin(listed ? m.px(2) : m.px(8));
            if (listed && !(block.next().isValid() && block.next().textList()))
                format.setBottomMargin(m.px(8));
            format.setLineHeight(145, QTextBlockFormat::ProportionalHeight);
        }
        if (quoteLevel > 0) {
            format.setLeftMargin(m.device(m.px(12)) * quoteLevel);
            spans.append({start, end, quoteText});
        }
        cursor.setPosition(start);
        cursor.setBlockFormat(format);
        if (code)
            continue;
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid())
                continue;
            const QTextCharFormat chars = fragment.charFormat();
            const int fragmentEnd = fragment.position() + fragment.length();
            if (chars.isAnchor() && !chars.anchorHref().isEmpty())
                spans.append({fragment.position(), fragmentEnd, anchorText});
            if (chars.fontFixedPitch() && !chars.isImageFormat())
                spans.append({fragment.position(), fragmentEnd, inlineCode});
        }
    }
    for (const Span &span : std::as_const(spans)) {
        cursor.setPosition(span.start);
        cursor.setPosition(span.end, QTextCursor::KeepAnchor);
        cursor.mergeCharFormat(span.format);
    }
    for (const HeadingRun &run : std::as_const(headingRuns)) {
        const QTextBlock block = doc->findBlock(run.start);
        QList<QPair<int, QTextCharFormat>> fragments;
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid())
                continue;
            QTextCharFormat format = fragment.charFormat();
            format.clearProperty(QTextFormat::FontSizeAdjustment);
            format.setFontPointSize(format.fontFixedPitch() ? headingSizes[run.level - 1] * 0.9
                                                            : headingSizes[run.level - 1]);
            format.setFontWeight(QFont::DemiBold);
            fragments.append({fragment.position(), format});
        }
        for (int i = 0; i < fragments.size(); ++i) {
            const int stop = i + 1 < fragments.size() ? fragments.at(i + 1).first : run.end;
            cursor.setPosition(fragments.at(i).first);
            cursor.setPosition(stop, QTextCursor::KeepAnchor);
            cursor.setCharFormat(fragments.at(i).second);
        }
    }
    for (QTextTable *table : std::as_const(tables)) {
        QTextTableFormat format = table->format();
        format.setBorder(m.px(0.75));
        format.setBorderBrush(border);
        format.setBorderStyle(QTextFrameFormat::BorderStyle_Solid);
        format.setBorderCollapse(true);
        format.setCellSpacing(0);
        format.setCellPadding(0);
        format.setWidth(QTextLength(QTextLength::PercentageLength, 100));
        format.setTopMargin(0);
        format.setBottomMargin(m.px(10));
        format.setHeaderRowCount(qMin(1, table->rows()));
        table->setFormat(format);
        for (int row = 0; row < table->rows(); ++row) {
            for (int column = 0; column < table->columns(); ++column) {
                QTextTableCell cell = table->cellAt(row, column);
                QTextTableCellFormat cellFormat = cell.format().toTableCellFormat();
                cellFormat.setBorder(m.px(0.75));
                cellFormat.setBorderStyle(QTextFrameFormat::BorderStyle_Solid);
                cellFormat.setBorderBrush(border);
                cellFormat.setTopPadding(m.px(3));
                cellFormat.setBottomPadding(m.px(3));
                cellFormat.setLeftPadding(m.px(5));
                cellFormat.setRightPadding(m.px(5));
                if (row == 0) {
                    cellFormat.setBackground(panel);
                    cellFormat.setFontWeight(QFont::DemiBold);
                }
                cell.setFormat(cellFormat);
            }
        }
    }
    for (auto it = codeRuns.crbegin(); it != codeRuns.crend(); ++it) {
        cursor.setPosition(it->first);
        cursor.setPosition(it->second, QTextCursor::KeepAnchor);
        QTextFrameFormat frame;
        frame.setBackground(panel);
        frame.setBorder(m.px(0.5));
        frame.setBorderBrush(border);
        frame.setBorderStyle(QTextFrameFormat::BorderStyle_Solid);
        frame.setPadding(m.px(8));
        frame.setMargin(0);
        frame.setBottomMargin(m.px(10));
        cursor.insertFrame(frame);
    }
    cursor.endEditBlock();
}

inline void fitNotePdfImages(QTextDocument *doc, qreal columnWidth)
{
    struct Sized {
        int start = 0;
        int length = 0;
        QTextImageFormat format;
        QString alt;
        bool loaded = false;
    };
    QList<Sized> sized;
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid() || !fragment.charFormat().isImageFormat())
                continue;
            QTextImageFormat image = fragment.charFormat().toImageFormat();
            if (image.hasProperty(NoteObjectIndex))
                continue;
            QString name = image.name();
            const int hint = name.lastIndexOf(MarkdownRender::imageWidthHint());
            int declared = 0;
            if (hint >= 0) {
                declared = name.mid(hint + MarkdownRender::imageWidthHint().size()).toInt();
                name = name.left(hint);
                image.setName(name);
            }
            QImage source;
            if (name.startsWith(QLatin1String("data:image/"), Qt::CaseInsensitive)) {
                const int comma = name.indexOf(QLatin1Char(','));
                if (comma > 0 && name.left(comma).endsWith(QLatin1String(";base64"),
                                                           Qt::CaseInsensitive))
                    source.loadFromData(QByteArray::fromBase64(name.mid(comma + 1).toLatin1()));
                if (!source.isNull())
                    doc->addResource(QTextDocument::ImageResource, QUrl(name), source);
            } else {
                const QUrl url(name);
                if (url.isLocalFile() || url.scheme().isEmpty() ||
                    url.scheme() == QLatin1String("qrc")) {
                    const QVariant resource = doc->resource(QTextDocument::ImageResource, url);
                    if (resource.typeId() == QMetaType::QImage)
                        source = resource.value<QImage>();
                    else if (resource.typeId() == QMetaType::QPixmap)
                        source = resource.value<QPixmap>().toImage();
                    else if (resource.typeId() == QMetaType::QByteArray)
                        source.loadFromData(resource.toByteArray());
                }
            }
            Sized entry;
            entry.start = fragment.position();
            entry.length = fragment.length();
            entry.format = image;
            entry.loaded = !source.isNull() && source.width() > 0;
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
            entry.alt = image.stringProperty(QTextFormat::ImageAltText);
#endif
            if (entry.loaded) {
                const qreal natural = declared > 0 ? declared : source.width();
                const qreal width = qMin<qreal>(natural, columnWidth);
                entry.format.setWidth(width);
                entry.format.setHeight(qMax<qreal>(1, source.height() * width / source.width()));
            }
            sized.append(entry);
        }
    }
    QTextCursor cursor(doc);
    for (auto it = sized.crbegin(); it != sized.crend(); ++it) {
        cursor.setPosition(it->start);
        cursor.setPosition(it->start + it->length, QTextCursor::KeepAnchor);
        if (it->loaded) {
            cursor.setCharFormat(it->format);
            continue;
        }
        cursor.removeSelectedText();
        if (!it->alt.isEmpty()) {
            QTextCharFormat alt;
            alt.setForeground(QColor(QStringLiteral("#57606a")));
            cursor.insertText(it->alt, alt);
        }
    }
}

inline void keepNotePdfBlocksTogether(QTextDocument *doc, qreal pageHeight)
{
    QAbstractTextDocumentLayout *layout = doc->documentLayout();
    QTextFrame *root = doc->rootFrame();
    const auto lineY = [layout](const QTextBlock &block, bool first, bool bottom) {
        const QRectF rect = layout->blockBoundingRect(block);
        const QTextLayout *text = block.layout();
        if (!text || text->lineCount() == 0)
            return bottom ? rect.bottom() : rect.top();
        const QTextLine line = text->lineAt(first ? 0 : text->lineCount() - 1);
        return rect.top() + line.y() + (bottom ? line.height() : 0);
    };
    const auto pageOf = [pageHeight](qreal y) {
        return int(std::floor(qMax<qreal>(0, y) / pageHeight));
    };
    QTextCursor cursor(doc);
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        const QTextCursor at(block);
        if (at.currentTable())
            continue;
        QTextFrame *frame = at.currentFrame();
        if (frame != root) {
            if (frame->parentFrame() != root || frame->firstPosition() != block.position())
                continue;
            QTextFrameFormat format = frame->frameFormat();
            if (format.pageBreakPolicy() & QTextFormat::PageBreak_AlwaysBefore)
                continue;
            const QRectF rect = layout->frameBoundingRect(frame);
            if (pageOf(rect.top()) == pageOf(rect.bottom() - 0.5) ||
                rect.height() > pageHeight * 0.8)
                continue;
            format.setPageBreakPolicy(QTextFormat::PageBreak_AlwaysBefore);
            frame->setFrameFormat(format);
            continue;
        }
        const QTextBlockFormat format = block.blockFormat();
        if (format.pageBreakPolicy() & QTextFormat::PageBreak_AlwaysBefore)
            continue;
        const QTextBlock previous = block.previous();
        if (previous.isValid() &&
            (previous.blockFormat().pageBreakPolicy() & QTextFormat::PageBreak_AlwaysAfter))
            continue;
        qreal top = 0;
        qreal bottom = 0;
        qreal limit = 0;
        if (format.boolProperty(NoteKeepWithNext)) {
            const QTextBlock next = block.next();
            if (!next.isValid() ||
                (format.pageBreakPolicy() & QTextFormat::PageBreak_AlwaysAfter))
                continue;
            top = lineY(block, true, false);
            bottom = lineY(next, true, true);
            limit = pageHeight * 0.5;
        } else if (format.boolProperty(NoteKeepTogether)) {
            top = lineY(block, true, false);
            bottom = lineY(block, false, true);
            limit = pageHeight * 0.8;
        } else {
            continue;
        }
        if (pageOf(top) == pageOf(bottom - 0.5) || bottom - top > limit)
            continue;
        QTextBlockFormat moved = format;
        moved.setPageBreakPolicy(moved.pageBreakPolicy() | QTextFormat::PageBreak_AlwaysBefore);
        cursor.setPosition(block.position());
        cursor.setBlockFormat(moved);
    }
}

inline void insertNotePdfFigure(QTextCursor &cursor, QTextDocument *doc, const NoteFigure &figure,
                                int index, const NotePdfMetrics &m, qreal columnWidth)
{
    const QSizeF display = figureDisplaySize(svgFigureSize(figure.svg), columnWidth);
    const QSizeF size(m.device(display.width()), m.device(display.height()));
    qreal scale = 1;
    const qreal area = size.width() * size.height();
    if (area > 16e6)
        scale = std::sqrt(16e6 / area);
    const QImage image = svgFigureImage(
        figure.svg, QSize(qMax(1, qRound(size.width() * scale)), qMax(1, qRound(size.height() * scale))),
        QColor(Qt::transparent));
    const QString name = QStringLiteral("fmnote-print-figure:%1").arg(index);
    doc->addResource(QTextDocument::ImageResource, QUrl(name), image);
    QTextImageFormat format;
    format.setName(name);
    format.setWidth(display.width());
    format.setHeight(display.height());
    format.setProperty(NoteObjectIndex, index);
    cursor.insertImage(format);
    if (!figure.caption.isEmpty()) {
        QTextCharFormat caption;
        caption.setForeground(QColor(QStringLiteral("#57606a")));
        caption.setFontPointSize(9);
        cursor.insertText(QString(QChar(QChar::LineSeparator)), caption);
        cursor.insertText(figure.caption, caption);
    }
}

inline QPageSize::PageSizeId notePageSizeId(PaperSize size)
{
    switch (size) {
    case PaperSize::Letter: return QPageSize::Letter;
    case PaperSize::Legal: return QPageSize::Legal;
    case PaperSize::A4: break;
    }
    return QPageSize::A4;
}

inline NotePdfResult writeNotePdf(QIODevice *device, const NotePdfInput &input)
{
    NotePdfResult result;
    const PageLayout &layout = input.layout;
    QPdfWriter writer(device);
    writer.setResolution(kNotePdfResolution);
    writer.setCreator(QStringLiteral("ForkMesh"));
    writer.setTitle(noteDocumentTitle(input.title));
    writer.setPageLayout(QPageLayout(
        QPageSize(notePageSizeId(layout.size)),
        layout.orientation == PaperOrientation::Landscape ? QPageLayout::Landscape
                                                          : QPageLayout::Portrait,
        QMarginsF(0, 0, 0, 0), QPageLayout::Millimeter));
    const NotePdfMetrics m{qreal(writer.resolution())};
    const QSizeF paperMm = paperMillimetres(layout);
    const QSizeF page(paperMm.width() / 25.4 * m.dpi, paperMm.height() / 25.4 * m.dpi);
    const qreal margin = marginMillimetres(layout.margins) / 25.4 * m.dpi;
    const QSizeF content(page.width() - 2 * margin, page.height() - 2 * margin);

    QTextDocument doc;
    doc.documentLayout()->setPaintDevice(&writer);
    doc.setDocumentMargin(0);
    QFont font = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    font.setPointSizeF(11);
    doc.setDefaultFont(font);
    doc.setPageSize(content);

    const QString token = noteObjectToken();
    const PlaceholderMarkdown placeholders = withObjectPlaceholders(input.markdown, token);
    QStringList sections;
    if (layout.cover)
        sections.append(token + QStringLiteral("cover"));
    if (layout.contents)
        sections.append(token + QStringLiteral("contents"));
    if (!layout.cover)
        sections.append(QStringLiteral("# ") + escapedMarkdownText(noteDocumentTitle(input.title)));
    if (!placeholders.markdown.trimmed().isEmpty())
        sections.append(placeholders.markdown);
    doc.setMarkdown(MarkdownRender::forPreview(sections.join(QStringLiteral("\n\n"))));
    const qreal column = m.css(content.width());
    styleNotePdfDocument(&doc, m);
    fitNotePdfImages(&doc, column);

    QTextCursor cursor(&doc);
    cursor.beginEditBlock();
    QList<QTextBlock> marked;
    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next()) {
        if (block.text().startsWith(token))
            marked.append(block);
    }
    const QColor muted(QStringLiteral("#57606a"));
    for (auto it = marked.crbegin(); it != marked.crend(); ++it) {
        const QTextBlock block = *it;
        const QString text = block.text();
        cursor.setPosition(block.position());
        cursor.setPosition(block.position() + block.length() - 1, QTextCursor::KeepAnchor);
        cursor.removeSelectedText();
        QTextBlockFormat base;
        base.setTopMargin(0);
        base.setBottomMargin(0);
        if (text == token + QStringLiteral("cover")) {
            QTextBlockFormat lead = base;
            lead.setAlignment(Qt::AlignHCenter);
            lead.setTopMargin(m.css(content.height()) * 0.22);
            QTextBlockFormat center = base;
            center.setAlignment(Qt::AlignHCenter);
            QTextCharFormat titleText;
            titleText.setFontPointSize(28);
            titleText.setFontWeight(QFont::DemiBold);
            QTextCharFormat dateText;
            dateText.setFontPointSize(12);
            dateText.setForeground(muted);
            cursor.setBlockFormat(lead);
            const QImage emoji = input.emoji.isEmpty()
                ? QImage() : notePdfEmojiImage(input.emoji, qRound(m.device(m.px(48) * 1.2)));
            if (!emoji.isNull()) {
                doc.addResource(QTextDocument::ImageResource, QUrl(QStringLiteral("fmnote-print-emoji")), emoji);
                QTextImageFormat image;
                image.setName(QStringLiteral("fmnote-print-emoji"));
                image.setWidth(m.css(emoji.width()));
                image.setHeight(m.css(emoji.height()));
                cursor.insertImage(image);
                center.setTopMargin(m.px(10));
                cursor.insertBlock(center, titleText);
                center.setTopMargin(0);
            } else {
                cursor.setCharFormat(titleText);
            }
            cursor.insertText(noteDocumentTitle(input.title), titleText);
            center.setTopMargin(m.px(8));
            center.setPageBreakPolicy(QTextFormat::PageBreak_AlwaysAfter);
            cursor.insertBlock(center, dateText);
            cursor.insertText(exportDateLabel(input.date.isValid() ? input.date : QDate::currentDate()),
                              dateText);
            continue;
        }
        if (text == token + QStringLiteral("contents")) {
            QTextBlockFormat heading = base;
            heading.setBottomMargin(m.px(12));
            heading.setLineHeight(125, QTextBlockFormat::ProportionalHeight);
            QTextCharFormat headingText;
            headingText.setFontPointSize(20);
            headingText.setFontWeight(QFont::DemiBold);
            cursor.setBlockFormat(heading);
            cursor.insertText(QStringLiteral("Contents"), headingText);
            const QList<NoteHeading> headings = noteHeadings(input.markdown);
            for (const NoteHeading &entry : headings) {
                QTextBlockFormat line = base;
                line.setBottomMargin(m.px(5));
                line.setLeftMargin(entry.level == 2 ? m.device(m.px(16)) : 0);
                QTextCharFormat chars;
                chars.setFontPointSize(entry.level == 2 ? 10 : 11);
                cursor.insertBlock(line, chars);
                cursor.insertText(entry.text, chars);
            }
            QTextBlockFormat end = cursor.blockFormat();
            end.setPageBreakPolicy(QTextFormat::PageBreak_AlwaysAfter);
            cursor.setBlockFormat(end);
            continue;
        }
        const int index = notePlaceholderIndex(text, token, int(placeholders.objects.size()));
        if (index < 0)
            continue;
        const NotePart &part = placeholders.objects.at(index);
        if (part.kind == NotePartKind::PageBreak) {
            QTextBlockFormat format = base;
            format.setLineHeight(0, QTextBlockFormat::FixedHeight);
            format.setPageBreakPolicy(QTextFormat::PageBreak_AlwaysAfter);
            cursor.setBlockFormat(format);
            continue;
        }
        QTextBlockFormat figure = base;
        figure.setAlignment(Qt::AlignHCenter);
        figure.setTopMargin(m.px(10));
        figure.setBottomMargin(m.px(12));
        figure.setLineHeight(100, QTextBlockFormat::ProportionalHeight);
        figure.setProperty(NoteKeepTogether, true);
        cursor.setBlockFormat(figure);
        insertNotePdfFigure(cursor, &doc, part.figure, index, m, column);
    }
    cursor.endEditBlock();

    keepNotePdfBlocksTogether(&doc, content.height());

    const int total = qMax(1, doc.pageCount());
    QPainter painter;
    if (!painter.begin(&writer)) {
        result.error = QStringLiteral("Could not start the PDF.");
        return result;
    }
    painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing |
                           QPainter::SmoothPixmapTransform);
    QFont footer = font;
    footer.setPointSizeF(9);
    QAbstractTextDocumentLayout::PaintContext context;
    context.palette.setColor(QPalette::Text, QColor(QStringLiteral("#1f2328")));
    context.palette.setColor(QPalette::Link, QColor(QStringLiteral("#0969da")));
    context.palette.setColor(QPalette::Dark, QColor(QStringLiteral("#d0d7de")));
    for (int index = 0; index < total; ++index) {
        if (index > 0 && !writer.newPage()) {
            painter.end();
            result.error = QStringLiteral("Could not add a page to the PDF.");
            return result;
        }
        painter.fillRect(QRectF(QPointF(0, 0), page), QColor(Qt::white));
        painter.save();
        painter.translate(margin, margin - index * content.height());
        context.clip = QRectF(0, index * content.height(), content.width(), content.height());
        painter.setClipRect(context.clip);
        doc.documentLayout()->draw(&painter, context);
        painter.restore();
        if (showsPageNumber(index, layout)) {
            painter.setFont(footer);
            painter.setPen(muted);
            painter.drawText(QRectF(margin, page.height() - margin, content.width(), margin),
                             Qt::AlignCenter, pageNumberLabel(index + 1, total));
        }
    }
    if (!painter.end()) {
        result.error = QStringLiteral("Could not finish the PDF.");
        return result;
    }
    result.ok = true;
    result.pages = total;
    return result;
}

inline NotePdfResult writeNotePdfFile(const QString &path, const NotePdfInput &input)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        NotePdfResult failed;
        failed.error = file.errorString();
        return failed;
    }
    NotePdfResult result = writeNotePdf(&file, input);
    if (!result.ok) {
        file.cancelWriting();
        return result;
    }
    if (!file.commit()) {
        result.ok = false;
        result.error = file.errorString();
    }
    return result;
}

}
