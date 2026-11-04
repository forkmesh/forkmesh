#include "CopyFormatting.h"

#include <QApplication>
#include <QClipboard>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QStringList>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextList>
#include <QTextTable>

namespace forkmesh::copyformat {
namespace {

constexpr const char *kFormattedProperty = "forkmeshFormattedCopy";
constexpr qsizetype kMaxCopyLength = 500000;

QString cleanText(QString text)
{
    text.replace(QChar::LineSeparator, QLatin1Char('\n'));
    text.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
    text.replace(QChar::Nbsp, QLatin1Char(' '));
    text.remove(QChar::ObjectReplacementCharacter);
    return text;
}

QString listMarker(const QTextBlock &block)
{
    QTextList *list = block.textList();
    const QTextListFormat format = list->format();
    QString marker;
    switch (format.style()) {
    case QTextListFormat::ListDisc:
    case QTextListFormat::ListCircle:
    case QTextListFormat::ListSquare:
    case QTextListFormat::ListStyleUndefined:
        break;
    default:
        marker = list->itemText(block);
        break;
    }
    if (marker.isEmpty())
        marker = QStringLiteral("-");
    switch (block.blockFormat().marker()) {
    case QTextBlockFormat::MarkerType::Checked:
        marker += QStringLiteral(" [x]");
        break;
    case QTextBlockFormat::MarkerType::Unchecked:
        marker += QStringLiteral(" [ ]");
        break;
    default:
        break;
    }
    return QString(qMax(0, format.indent() - 1) * 2, QLatin1Char(' ')) + marker +
           QLatin1Char(' ');
}

QString cellText(const QTextTableCell &cell)
{
    QStringList parts;
    for (QTextFrame::iterator it = cell.begin(); !it.atEnd(); ++it) {
        const QTextBlock block = it.currentBlock();
        if (!block.isValid())
            continue;
        const QString text = cleanText(block.text()).simplified();
        if (!text.isEmpty())
            parts.append(text);
    }
    return parts.join(QLatin1Char(' '));
}

class Writer
{
public:
    void frame(QTextFrame *frame)
    {
        for (QTextFrame::iterator it = frame->begin(); !it.atEnd(); ++it) {
            if (QTextFrame *child = it.currentFrame()) {
                if (auto *table = qobject_cast<QTextTable *>(child))
                    this->table(table);
                else
                    this->frame(child);
            } else if (it.currentBlock().isValid()) {
                block(it.currentBlock());
            }
        }
    }

    QString result() const { return m_lines.join(QLatin1Char('\n')); }

private:
    enum class Kind { None, Paragraph, ListItem, Table };

    void separate(bool spaced)
    {
        if (spaced && !m_lines.isEmpty() && !m_lines.constLast().isEmpty())
            m_lines.append(QString());
    }

    void block(const QTextBlock &block)
    {
        const QTextBlockFormat format = block.blockFormat();
        QTextList *list = block.textList();
        const bool listItem = list != nullptr;
        QString text = cleanText(block.text());
        if (text.isEmpty() && format.hasProperty(QTextFormat::BlockTrailingHorizontalRulerWidth))
            text = QStringLiteral("---");
        if (m_kind == Kind::Table && text.isEmpty() && !listItem) {
            m_kind = Kind::Paragraph;
            m_spaceAfter = true;
            return;
        }
        if (m_kind != Kind::None) {
            const bool sameList = listItem && m_kind == Kind::ListItem &&
                                  (list == m_list || list->format().indent() != m_listIndent);
            separate(!sameList && (m_spaceAfter || format.topMargin() > 0 ||
                                   format.headingLevel() > 0));
        }
        const QString prefix = listItem ? listMarker(block) : QString();
        const QStringList parts = text.split(QLatin1Char('\n'));
        for (qsizetype i = 0; i < parts.size(); ++i)
            m_lines.append((i == 0 ? prefix : QString(prefix.size(), QLatin1Char(' '))) +
                           parts.at(i));
        m_kind = listItem ? Kind::ListItem : Kind::Paragraph;
        m_list = list;
        m_listIndent = listItem ? list->format().indent() : 0;
        m_spaceAfter = format.bottomMargin() > 0 || format.headingLevel() > 0;
    }

    void table(QTextTable *table)
    {
        separate(true);
        for (int row = 0; row < table->rows(); ++row) {
            QStringList cells;
            for (int column = 0; column < table->columns(); ++column) {
                const QTextTableCell cell = table->cellAt(row, column);
                if (cell.row() == row && cell.column() == column)
                    cells.append(cellText(cell));
            }
            m_lines.append(cells.join(QLatin1Char('\t')));
        }
        m_kind = Kind::Table;
        m_spaceAfter = true;
    }

    QStringList m_lines;
    Kind m_kind = Kind::None;
    QTextList *m_list = nullptr;
    int m_listIndent = 0;
    bool m_spaceAfter = false;
};

class ClipboardFormatter final : public QObject
{
public:
    explicit ClipboardFormatter(QClipboard *clipboard)
        : QObject(clipboard)
        , m_clipboard(clipboard)
    {
        connect(clipboard, &QClipboard::dataChanged, this,
                [this] { reformat(QClipboard::Clipboard); }, Qt::QueuedConnection);
        if (clipboard->supportsSelection())
            connect(clipboard, &QClipboard::selectionChanged, this,
                    [this] { reformat(QClipboard::Selection); }, Qt::QueuedConnection);
    }

private:
    void reformat(QClipboard::Mode mode)
    {
        const bool owned = mode == QClipboard::Selection ? m_clipboard->ownsSelection()
                                                         : m_clipboard->ownsClipboard();
        if (!owned || qobject_cast<QPlainTextEdit *>(QApplication::focusWidget()))
            return;
        if (QMimeData *copy = formattedCopy(m_clipboard->mimeData(mode)))
            m_clipboard->setMimeData(copy, mode);
    }

    QClipboard *m_clipboard;
};

}

QString plainText(const QTextDocument &document)
{
    Writer writer;
    writer.frame(document.rootFrame());
    return writer.result();
}

QString plainTextFromHtml(const QString &html)
{
    QTextDocument document;
    document.setHtml(html);
    return plainText(document);
}

QMimeData *formattedCopy(const QMimeData *source)
{
    if (!source || source->property(kFormattedProperty).toBool())
        return nullptr;
    const QStringList formats = source->formats();
    const QString plainFormat = QStringLiteral("text/plain");
    if (!formats.contains(plainFormat) || !formats.contains(QStringLiteral("text/html")) ||
        !(formats.contains(QStringLiteral("text/markdown")) ||
          formats.contains(QStringLiteral("application/vnd.oasis.opendocument.text"))))
        return nullptr;
    const QString original = source->text();
    if (!original.contains(QLatin1Char('\n')) || original.size() > kMaxCopyLength)
        return nullptr;
    const QString text = plainTextFromHtml(source->html());
    if (text.trimmed().isEmpty() || text == original)
        return nullptr;
    auto *copy = new QMimeData;
    for (const QString &format : formats) {
        if (format != plainFormat)
            copy->setData(format, source->data(format));
    }
    copy->setText(text);
    copy->setProperty(kFormattedProperty, true);
    return copy;
}

void installClipboardFormatting(QApplication *app)
{
    static bool installed = false;
    if (installed || !app || !QGuiApplication::clipboard())
        return;
    installed = true;
    new ClipboardFormatter(QGuiApplication::clipboard());
}

}
