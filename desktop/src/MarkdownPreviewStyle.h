#pragma once

#include <QAbstractTextDocumentLayout>
#include <QClipboard>
#include <QEvent>
#include <QFont>
#include <QFontInfo>
#include <QGuiApplication>
#include <QList>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPair>
#include <QPointer>
#include <QScrollBar>
#include <QStringList>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextLayout>
#include <QTextList>
#include <QTextTable>
#include <QTimer>
#include <QToolButton>

namespace forkmesh::ui::markdownstyle {

struct Colors {
    QColor text;
    QColor muted;
    QColor border;
    QColor quoteBar;
    QColor codeBackground;
    QColor inlineCodeBackground;
    QColor rowBackground;
    QColor link;
    QStringList monoFamilies;
};

enum Property {
    CodeBlock = QTextFormat::UserProperty + 0x6d64,
    HeadingRule,
    Rule,
    InlineCode,
};

constexpr int kParagraphGap = 16;
constexpr int kListItemGap = 4;
constexpr int kHeadingTop = 24;
constexpr int kHeadingRuleGap = 8;
constexpr int kCodePadding = 16;
constexpr int kQuoteStep = 20;
constexpr int kQuoteBarWidth = 4;
constexpr int kListIndent = 28;
constexpr qreal kPillPadding = 4;

inline bool isCodeFormat(const QTextBlockFormat &format)
{
    return format.boolProperty(CodeBlock) || format.hasProperty(QTextFormat::BlockCodeFence) ||
           format.nonBreakableLines();
}

inline QPointF blockOrigin(const QTextDocument *doc, const QTextBlock &block)
{
    return doc->documentLayout()->blockBoundingRect(block).topLeft();
}

inline qreal contentRight(const QTextDocument *doc)
{
    const QTextFrame *root = doc->rootFrame();
    const QTextFrameFormat format = root->frameFormat();
    const QRectF frame = doc->documentLayout()->frameBoundingRect(const_cast<QTextFrame *>(root));
    return frame.right() - format.rightMargin() - format.padding() - format.border();
}

inline QString codeRunText(const QTextDocument *doc, int start, int end)
{
    QStringList lines;
    for (QTextBlock block = doc->findBlock(start); block.isValid() && block.position() <= end;
         block = block.next())
        lines.append(block.text());
    return lines.join(QLatin1Char('\n'));
}

class Decorations : public QObject
{
public:
    explicit Decorations(QTextBrowser *view) : QObject(view), m_view(view)
    {
        setObjectName(QStringLiteral("markdownDecorations"));
        view->viewport()->installEventFilter(this);
        view->viewport()->setMouseTracking(true);
    }

    static Decorations *of(QTextBrowser *view)
    {
        for (QObject *child : view->children()) {
            if (auto *found = dynamic_cast<Decorations *>(child))
                return found;
        }
        return new Decorations(view);
    }

    void setColors(const Colors &colors)
    {
        m_colors = colors;
        m_dirty = true;
        if (m_copy) {
            styleCopyButton();
            m_copy->hide();
        }
        m_view->viewport()->update();
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched != m_view->viewport())
            return QObject::eventFilter(watched, event);
        switch (event->type()) {
        case QEvent::Paint:
            paint(static_cast<QPaintEvent *>(event)->rect());
            break;
        case QEvent::MouseMove:
            hover(static_cast<QMouseEvent *>(event)->position());
            break;
        case QEvent::Leave:
            if (m_copy && !m_copy->underMouse())
                m_copy->hide();
            break;
        default:
            break;
        }
        return QObject::eventFilter(watched, event);
    }

private:
    struct Shape {
        enum Kind { Panel, Line, Bar, Pill };
        Kind kind = Panel;
        QRectF rect;
        int start = 0;
        int end = 0;
    };

    QPointF offset() const
    {
        return QPointF(-m_view->horizontalScrollBar()->value(),
                       -m_view->verticalScrollBar()->value());
    }

    void bind()
    {
        QTextDocument *doc = m_view->document();
        QAbstractTextDocumentLayout *layout = doc->documentLayout();
        if (doc == m_doc && layout == m_layout)
            return;
        if (m_doc)
            disconnect(m_doc, nullptr, this, nullptr);
        if (m_layout)
            disconnect(m_layout, nullptr, this, nullptr);
        m_doc = doc;
        m_layout = layout;
        m_dirty = true;
        const auto invalidate = [this] { m_dirty = true; };
        connect(doc, &QTextDocument::contentsChanged, this, invalidate);
        connect(layout, &QAbstractTextDocumentLayout::documentSizeChanged, this, invalidate);
        connect(layout, &QAbstractTextDocumentLayout::update, this, invalidate);
    }

    void collect()
    {
        bind();
        if (!m_dirty)
            return;
        m_dirty = false;
        m_shapes.clear();
        QTextDocument *doc = m_doc;
        QAbstractTextDocumentLayout *layout = m_layout;
        QTextFrame *root = doc->rootFrame();
        const qreal right = contentRight(doc);
        const auto lineRect = [](const QTextLayout *text, const QPointF &origin, bool last) {
            const QTextLine line = text->lineAt(last ? text->lineCount() - 1 : 0);
            return line.rect().translated(origin);
        };
        QList<qreal> quoteTops;
        QList<qreal> quoteLefts;
        qreal quoteBottom = 0;
        Shape panel;
        bool inPanel = false;
        const auto closeQuotes = [&](int keep) {
            while (quoteTops.size() > keep) {
                Shape bar;
                bar.kind = Shape::Bar;
                bar.rect = QRectF(quoteLefts.last(), quoteTops.last(), kQuoteBarWidth,
                                  qMax<qreal>(0, quoteBottom - quoteTops.last()));
                m_shapes.append(bar);
                quoteTops.removeLast();
                quoteLefts.removeLast();
            }
        };
        for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
            const QTextLayout *text = block.layout();
            if (!text || text->lineCount() == 0)
                continue;
            const QTextBlockFormat format = block.blockFormat();
            const bool inRoot = QTextCursor(block).currentFrame() == root;
            const QPointF origin = blockOrigin(doc, block);
            const QRectF first = lineRect(text, origin, false);
            const QRectF last = lineRect(text, origin, true);
            const bool code = inRoot && isCodeFormat(format);
            if (code) {
                if (!inPanel) {
                    panel = Shape();
                    panel.kind = Shape::Panel;
                    panel.start = block.position();
                    panel.rect = QRectF(QPointF(first.left() - kCodePadding, first.top() - kCodePadding),
                                        QPointF(right, last.bottom() + kCodePadding));
                    inPanel = true;
                }
                panel.end = block.position() + block.length() - 1;
                panel.rect.setBottom(last.bottom() + kCodePadding);
            } else if (inPanel) {
                m_shapes.append(panel);
                inPanel = false;
            }
            const int level = inRoot ? format.intProperty(QTextFormat::BlockQuoteLevel) : 0;
            closeQuotes(level);
            while (quoteTops.size() < level) {
                quoteTops.append(first.top());
                quoteLefts.append(origin.x() + kQuoteStep * quoteTops.size() - kQuoteStep);
            }
            if (level > 0)
                quoteBottom = last.bottom();
            if (inRoot && format.boolProperty(HeadingRule) &&
                format.bottomMargin() >= kHeadingRuleGap + 1 + kParagraphGap) {
                Shape rule;
                rule.kind = Shape::Line;
                rule.rect = QRectF(origin.x(), last.bottom() + kHeadingRuleGap, right - origin.x(), 1);
                m_shapes.append(rule);
            }
            if (format.boolProperty(Rule)) {
                const QRectF bounds = layout->blockBoundingRect(block);
                Shape rule;
                rule.kind = Shape::Line;
                rule.rect = QRectF(bounds.left(), bounds.top(), right - bounds.left(), 1);
                m_shapes.append(rule);
            }
            if (code)
                continue;
            QList<QPair<int, int>> ranges;
            for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
                const QTextFragment fragment = it.fragment();
                if (!fragment.isValid() || !fragment.charFormat().boolProperty(InlineCode) ||
                    fragment.charFormat().hasProperty(QTextFormat::BackgroundBrush))
                    continue;
                const int from = fragment.position() - block.position();
                if (!ranges.isEmpty() && ranges.last().second == from)
                    ranges.last().second = from + fragment.length();
                else
                    ranges.append({from, from + fragment.length()});
            }
            for (const QPair<int, int> &range : std::as_const(ranges)) {
                const int from = range.first;
                const int to = range.second;
                for (int i = 0; i < text->lineCount(); ++i) {
                    const QTextLine line = text->lineAt(i);
                    const int lineStart = line.textStart();
                    const int lineEnd = lineStart + line.textLength();
                    if (to <= lineStart || from >= lineEnd)
                        continue;
                    const qreal x1 = line.cursorToX(qMax(from, lineStart));
                    const qreal x2 = line.cursorToX(qMin(to, lineEnd));
                    Shape pill;
                    pill.kind = Shape::Pill;
                    pill.rect = QRectF(x1 - kPillPadding, line.y() + 1, x2 - x1 + kPillPadding,
                                       line.height() - 2)
                                    .translated(origin);
                    m_shapes.append(pill);
                }
            }
        }
        if (inPanel)
            m_shapes.append(panel);
        closeQuotes(0);
    }

    void paint(const QRect &exposed)
    {
        collect();
        if (m_shapes.isEmpty())
            return;
        const QPointF shift = offset();
        QPainter painter(m_view->viewport());
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        for (const Shape &shape : std::as_const(m_shapes)) {
            const QRectF rect = shape.rect.translated(shift);
            if (!rect.intersects(QRectF(exposed).adjusted(-2, -2, 2, 2)))
                continue;
            switch (shape.kind) {
            case Shape::Panel:
                painter.setBrush(m_colors.codeBackground);
                painter.drawRoundedRect(rect, 6, 6);
                break;
            case Shape::Pill:
                painter.setBrush(m_colors.inlineCodeBackground);
                painter.drawRoundedRect(rect, 4, 4);
                break;
            case Shape::Bar:
                painter.setBrush(m_colors.quoteBar);
                painter.drawRoundedRect(rect, 1, 1);
                break;
            case Shape::Line:
                painter.fillRect(rect, m_colors.border);
                break;
            }
        }
    }

    void styleCopyButton()
    {
        m_copy->setStyleSheet(
            QStringLiteral("QToolButton { background-color: %1; color: %2; border: 1px solid %3;"
                           " border-radius: 6px; padding: 2px 8px; font-size: 12px; }"
                           "QToolButton:hover { color: %4; border-color: %2; }")
                .arg(m_colors.codeBackground.name(), m_colors.muted.name(),
                     m_colors.border.name(), m_colors.text.name()));
    }

    static void relabel(QToolButton *button, const QString &label)
    {
        const int right = button->geometry().right();
        button->setText(label);
        button->adjustSize();
        button->move(right - button->width() + 1, button->y());
    }

    void hover(const QPointF &position)
    {
        collect();
        const QPointF point = position - offset();
        for (const Shape &shape : std::as_const(m_shapes)) {
            if (shape.kind != Shape::Panel || !shape.rect.contains(point))
                continue;
            if (!m_copy) {
                m_copy = new QToolButton(m_view->viewport());
                m_copy->setObjectName(QStringLiteral("markdownCopyButton"));
                m_copy->setCursor(Qt::PointingHandCursor);
                m_copy->setToolTip(QStringLiteral("Copy code"));
                m_copy->setText(QStringLiteral("Copy"));
                styleCopyButton();
                connect(m_copy, &QToolButton::clicked, this, [this] {
                    if (!m_doc)
                        return;
                    QGuiApplication::clipboard()->setText(codeRunText(m_doc, m_copyStart, m_copyEnd));
                    relabel(m_copy, QStringLiteral("Copied"));
                    QPointer<QToolButton> button = m_copy;
                    QTimer::singleShot(1500, button, [button] {
                        if (button)
                            relabel(button, QStringLiteral("Copy"));
                    });
                });
            }
            if (m_copy->isHidden() || m_copyStart != shape.start)
                m_copy->setText(QStringLiteral("Copy"));
            m_copyStart = shape.start;
            m_copyEnd = shape.end;
            m_copy->adjustSize();
            const QRectF rect = shape.rect.translated(offset());
            const int x = qMin(int(rect.right()), m_view->viewport()->width()) - m_copy->width() - 8;
            m_copy->move(qMax(int(rect.left()) + 8, x), int(rect.top()) + 8);
            m_copy->show();
            m_copy->raise();
            return;
        }
        if (m_copy && !m_copy->underMouse())
            m_copy->hide();
    }

    QTextBrowser *m_view = nullptr;
    QPointer<QTextDocument> m_doc;
    QPointer<QAbstractTextDocumentLayout> m_layout;
    QPointer<QToolButton> m_copy;
    Colors m_colors;
    QList<Shape> m_shapes;
    int m_copyStart = 0;
    int m_copyEnd = 0;
    bool m_dirty = true;
};

inline bool isQuote(const QTextBlock &block)
{
    return block.isValid() && block.blockFormat().intProperty(QTextFormat::BlockQuoteLevel) > 0;
}

inline bool continuesList(const QTextBlock &block, const QTextBlock &next)
{
    const QTextList *list = block.textList();
    const QTextList *following = next.isValid() ? next.textList() : nullptr;
    if (!list || !following)
        return false;
    return list == following || list->format().indent() != following->format().indent();
}

inline void removeEmptyLeadingCellBlocks(QTextDocument *doc, QTextCursor &cursor)
{
    const QList<QTextFrame *> frames = doc->rootFrame()->childFrames();
    for (QTextFrame *frame : frames) {
        auto *table = qobject_cast<QTextTable *>(frame);
        if (!table)
            continue;
        for (int row = 0; row < table->rows(); ++row) {
            for (int column = 0; column < table->columns(); ++column) {
                const QTextTableCell cell = table->cellAt(row, column);
                const QTextBlock first = doc->findBlock(cell.firstPosition());
                if (first.length() != 1 || first.position() >= cell.lastPosition())
                    continue;
                cursor.setPosition(first.position());
                cursor.deleteChar();
            }
        }
    }
}

inline void styleTables(QTextDocument *doc, QTextCursor &cursor, const Colors &colors)
{
    QTextCharFormat heading;
    heading.setFontWeight(QFont::DemiBold);
    const QList<QTextFrame *> frames = doc->rootFrame()->childFrames();
    for (QTextFrame *frame : frames) {
        auto *table = qobject_cast<QTextTable *>(frame);
        if (!table)
            continue;
        QTextTableFormat format = table->format();
        format.setBorder(1);
        format.setBorderStyle(QTextFrameFormat::BorderStyle_Solid);
        format.setBorderBrush(colors.border);
        format.setBorderCollapse(true);
        format.setCellSpacing(0);
        format.setCellPadding(0);
        const QTextBlock before = doc->findBlock(table->firstPosition()).previous();
        const QTextBlockFormat previous = before.isValid() ? before.blockFormat() : QTextBlockFormat();
        const bool spaced = isCodeFormat(previous) || previous.headingLevel() > 0;
        format.setTopMargin(spaced ? previous.bottomMargin() : previous.bottomMargin() / 2);
        format.setBottomMargin(kParagraphGap);
        format.setHeaderRowCount(1);
        table->setFormat(format);
        for (int row = 0; row < table->rows(); ++row) {
            for (int column = 0; column < table->columns(); ++column) {
                QTextTableCell cell = table->cellAt(row, column);
                QTextTableCellFormat cellFormat = cell.format().toTableCellFormat();
                cellFormat.setBorder(1);
                cellFormat.setBorderStyle(QTextFrameFormat::BorderStyle_Solid);
                cellFormat.setBorderBrush(colors.border);
                cellFormat.setTopPadding(6);
                cellFormat.setBottomPadding(6);
                cellFormat.setLeftPadding(13);
                cellFormat.setRightPadding(13);
                if (row > 0 && row % 2 == 0)
                    cellFormat.setBackground(colors.rowBackground);
                else
                    cellFormat.clearBackground();
                cell.setFormat(cellFormat);
                if (row > 0)
                    continue;
                cursor.setPosition(cell.firstCursorPosition().position());
                cursor.setPosition(cell.lastCursorPosition().position(), QTextCursor::KeepAnchor);
                cursor.mergeCharFormat(heading);
            }
        }
    }
}

inline void apply(QTextBrowser *view, const Colors &colors)
{
    if (QFontInfo(view->font()).fixedPitch()) {
        QFont font = view->font();
        font.setFamilies(QGuiApplication::font().families());
        view->setFont(font);
    }
    QTextDocument *doc = view->document();
    doc->setIndentWidth(kListIndent);
    const int basePixels = qMax(10, QFontInfo(view->font()).pixelSize());
    const int codePixels = qMax(10, qRound(basePixels * 0.88));

    QTextCharFormat codeText;
    if (!colors.monoFamilies.isEmpty())
        codeText.setFontFamilies(colors.monoFamilies);
    codeText.setFontFixedPitch(true);
    codeText.setProperty(QTextFormat::FontPixelSize, codePixels);
    QTextCharFormat inlineCode = codeText;
    inlineCode.setProperty(InlineCode, true);
    QTextCharFormat headingInlineCode = inlineCode;
    headingInlineCode.clearProperty(QTextFormat::FontPixelSize);
    QTextCharFormat padded;
    padded.setFontLetterSpacingType(QFont::AbsoluteSpacing);
    padded.setFontLetterSpacing(kPillPadding);
    QTextCharFormat headingText;
    headingText.setFontWeight(QFont::DemiBold);
    QTextCharFormat quoteText;
    quoteText.setForeground(colors.muted);
    QTextCharFormat anchorText;
    anchorText.setForeground(colors.link);
    anchorText.setFontUnderline(false);

    struct Span {
        int start = 0;
        int length = 0;
        bool heading = false;
    };
    QList<Span> codeSpans;
    QList<Span> anchors;
    QList<Span> quotes;
    QList<Span> headings;
    QList<Span> codeBlocks;

    QTextCursor cursor(doc);
    cursor.beginEditBlock();
    removeEmptyLeadingCellBlocks(doc, cursor);
    QTextFrame *root = doc->rootFrame();
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        QTextBlockFormat format = block.blockFormat();
        const bool inTable = QTextCursor(block).currentTable() != nullptr;
        const bool inRoot = QTextCursor(block).currentFrame() == root;
        const int quoteLevel = format.intProperty(QTextFormat::BlockQuoteLevel);
        const int start = block.position();
        const int length = block.length() - 1;
        if (isCodeFormat(format)) {
            const bool first = !block.previous().isValid() ||
                               !isCodeFormat(block.previous().blockFormat());
            const bool last = !block.next().isValid() || !isCodeFormat(block.next().blockFormat());
            format.setProperty(CodeBlock, true);
            format.clearBackground();
            format.setLeftMargin(kCodePadding + (quoteLevel > 0 ? kQuoteStep * quoteLevel : 0));
            format.setRightMargin(kCodePadding);
            format.setTopMargin(first ? (block.previous().isValid() ? 2 * kCodePadding : kCodePadding)
                                      : 0);
            format.setBottomMargin(last ? kCodePadding + kParagraphGap : 0);
            format.setLineHeight(140, QTextBlockFormat::ProportionalHeight);
            if (!inRoot)
                format.setBackground(colors.codeBackground);
            codeBlocks.append({start, length, false});
        } else if (format.hasProperty(QTextFormat::BlockTrailingHorizontalRulerWidth) ||
                   format.boolProperty(Rule)) {
            format.clearProperty(QTextFormat::BlockTrailingHorizontalRulerWidth);
            format.setProperty(Rule, true);
            format.setTopMargin(kHeadingTop);
            format.setBottomMargin(kHeadingTop);
            format.setLineHeight(1, QTextBlockFormat::FixedHeight);
        } else if (format.headingLevel() > 0) {
            const int level = format.headingLevel();
            const bool ruled = level <= 2 && inRoot;
            format.setProperty(HeadingRule, ruled);
            format.setTopMargin(block.previous().isValid() ? kHeadingTop : 0);
            format.setBottomMargin(ruled ? kHeadingRuleGap + 1 + kParagraphGap : kParagraphGap);
            format.setLineHeight(125, QTextBlockFormat::ProportionalHeight);
            headings.append({start, length, true});
        } else if (!inTable && block.length() == 1 && block.previous().isValid() &&
                   QTextCursor(block.previous()).currentTable()) {
            format.setTopMargin(0);
            format.setBottomMargin(0);
            format.setLineHeight(0, QTextBlockFormat::FixedHeight);
        } else if (inTable) {
            format.setTopMargin(0);
            format.setBottomMargin(0);
            format.setLineHeight(140, QTextBlockFormat::ProportionalHeight);
        } else {
            const QTextBlock next = block.next();
            const bool listContinues = continuesList(block, next);
            const bool quoteContinues = quoteLevel > 0 && isQuote(next);
            format.setTopMargin(0);
            format.setBottomMargin(listContinues ? kListItemGap : kParagraphGap);
            if (quoteContinues && !listContinues)
                format.setBottomMargin(kParagraphGap);
            format.setLineHeight(145, QTextBlockFormat::ProportionalHeight);
        }
        if (quoteLevel > 0 && !isCodeFormat(format)) {
            format.setLeftMargin(kQuoteStep * quoteLevel);
            format.setRightMargin(0);
            quotes.append({start, length, false});
        }
        cursor.setPosition(start);
        cursor.setBlockFormat(format);
        if (isCodeFormat(format))
            continue;
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid())
                continue;
            const QTextCharFormat charFormat = fragment.charFormat();
            if (charFormat.isAnchor() && !charFormat.anchorHref().isEmpty())
                anchors.append({fragment.position(), fragment.length(), false});
            if (!charFormat.fontFixedPitch() && !charFormat.boolProperty(InlineCode))
                continue;
            if (!codeSpans.isEmpty() &&
                codeSpans.last().start + codeSpans.last().length == fragment.position())
                codeSpans.last().length += fragment.length();
            else
                codeSpans.append({fragment.position(), fragment.length(),
                                  format.headingLevel() > 0});
        }
    }
    for (const Span &span : std::as_const(headings)) {
        cursor.setPosition(span.start);
        cursor.setPosition(span.start + span.length, QTextCursor::KeepAnchor);
        cursor.mergeCharFormat(headingText);
    }
    for (const Span &span : std::as_const(quotes)) {
        cursor.setPosition(span.start);
        cursor.setPosition(span.start + span.length, QTextCursor::KeepAnchor);
        cursor.mergeCharFormat(quoteText);
    }
    for (const Span &span : std::as_const(codeBlocks)) {
        cursor.setPosition(span.start);
        cursor.setPosition(span.start + span.length, QTextCursor::KeepAnchor);
        cursor.mergeCharFormat(codeText);
    }
    for (const Span &span : std::as_const(codeSpans)) {
        cursor.setPosition(span.start);
        cursor.setPosition(span.start + span.length, QTextCursor::KeepAnchor);
        cursor.mergeCharFormat(span.heading ? headingInlineCode : inlineCode);
        const QTextBlock block = doc->findBlock(span.start);
        if (span.start > block.position()) {
            cursor.setPosition(span.start - 1);
            cursor.setPosition(span.start, QTextCursor::KeepAnchor);
            cursor.mergeCharFormat(padded);
        }
        cursor.setPosition(span.start + span.length - 1);
        cursor.setPosition(span.start + span.length, QTextCursor::KeepAnchor);
        cursor.mergeCharFormat(padded);
    }
    for (const Span &span : std::as_const(anchors)) {
        cursor.setPosition(span.start);
        cursor.setPosition(span.start + span.length, QTextCursor::KeepAnchor);
        cursor.mergeCharFormat(anchorText);
    }
    styleTables(doc, cursor, colors);
    cursor.endEditBlock();
    Decorations::of(view)->setColors(colors);
}

}
