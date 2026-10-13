#include "TaskMarkdown.h"

#include <QAbstractTextDocumentLayout>
#include <QList>
#include <QRegularExpression>
#include <QStringList>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextImageFormat>
#include <QTextList>
#include <QUrl>

namespace forkmesh::taskmarkdown {

namespace {

struct Span {
    int position = 0;
    int length = 0;
    QTextCharFormat format;
    bool image = false;
    bool safeAnchor = false;
    QString imageName;
};

QList<Span> spansToRewrite(const QTextDocument &document)
{
    QList<Span> spans;
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid())
                continue;
            const QTextCharFormat format = fragment.charFormat();
            if (format.isImageFormat()) {
                spans.append({fragment.position(), fragment.length(), format, true,
                              false, format.toImageFormat().name()});
            } else if (format.isAnchor()) {
                spans.append({fragment.position(), fragment.length(), format, false,
                              isSafeLink(format.anchorHref()), QString()});
            }
        }
    }
    return spans;
}

QString normalizedSource(const QString &markdown)
{
    QString source = markdown;
    source.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    source.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    return source;
}

void parseMarkdown(QTextDocument &document, const QString &source)
{
    document.setMarkdown(
        source, QTextDocument::MarkdownFeatures(
                    QTextDocument::MarkdownDialectGitHub |
                    QTextDocument::MarkdownNoHTML));
}

bool hasTaskMarker(const QTextBlock &block)
{
    return block.textList() &&
           block.blockFormat().marker() != QTextBlockFormat::MarkerType::NoMarker;
}

int markerCount(const QTextDocument &document)
{
    int count = 0;
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next())
        count += hasTaskMarker(block) ? 1 : 0;
    return count;
}

}

bool isSafeLink(const QString &href)
{
    const QString trimmed = href.trimmed();
    if (trimmed.isEmpty())
        return false;
    const QUrl url(trimmed, QUrl::StrictMode);
    if (!url.isValid())
        return false;
    const QString scheme = url.scheme().toLower();
    if (scheme == QLatin1String("mailto"))
        return !url.path().isEmpty();
    return (scheme == QLatin1String("http") || scheme == QLatin1String("https")) &&
           !url.host().isEmpty();
}

QString toHtml(const QString &markdown)
{
    const QString source = normalizedSource(markdown);
    if (source.trimmed().isEmpty())
        return QString();

    QTextDocument document;
    parseMarkdown(document, source);

    const QList<Span> spans = spansToRewrite(document);
    for (auto it = spans.crbegin(); it != spans.crend(); ++it) {
        QTextCursor cursor(&document);
        cursor.setPosition(it->position);
        cursor.setPosition(it->position + it->length, QTextCursor::KeepAnchor);
        if (it->image) {
            const QString name = it->imageName.trimmed();
            QTextCharFormat label;
            label.setFontItalic(true);
            if (isSafeLink(name)) {
                label.setAnchor(true);
                label.setAnchorHref(name);
            }
            cursor.insertText(name.isEmpty()
                                  ? QStringLiteral("[image]")
                                  : QStringLiteral("[image: %1]").arg(name),
                              label);
            continue;
        }
        QTextCharFormat plain = it->format;
        plain.clearProperty(QTextFormat::ForegroundBrush);
        if (it->safeAnchor) {
            cursor.setCharFormat(plain);
            continue;
        }
        plain.clearProperty(QTextFormat::AnchorHref);
        plain.clearProperty(QTextFormat::IsAnchor);
        plain.setFontUnderline(false);
        cursor.setCharFormat(plain);
    }

    const QString html = document.toHtml();
    const int bodyStart = html.indexOf(QLatin1String("<body"));
    const int bodyOpenEnd =
        bodyStart < 0 ? -1 : html.indexOf(QLatin1Char('>'), bodyStart);
    const int bodyEnd = html.lastIndexOf(QLatin1String("</body>"));
    if (bodyOpenEnd < 0 || bodyEnd < bodyOpenEnd)
        return source.toHtmlEscaped();
    return html.mid(bodyOpenEnd + 1, bodyEnd - bodyOpenEnd - 1).trimmed();
}

int taskItemCount(const QString &markdown)
{
    const QString source = normalizedSource(markdown);
    if (source.trimmed().isEmpty())
        return 0;
    QTextDocument document;
    parseMarkdown(document, source);
    return markerCount(document);
}

QString toggleTaskItem(const QString &markdown, int index)
{
    static const QRegularExpression item(QStringLiteral(
        "^((?:[ \\t]*>)*[ \\t]*(?:[-*+]|\\d{1,9}[.)])[ \\t]+\\[)([ xX])\\][ \\t]"));
    static const QRegularExpression fence(
        QStringLiteral("^(?:[ \\t]*>)*[ \\t]{0,3}(`{3,}|~{3,})"));
    const QString source = normalizedSource(markdown);
    QStringList lines = source.split(QLatin1Char('\n'));
    QList<int> itemLines;
    QString openFence;
    for (int i = 0; i < lines.size(); ++i) {
        const QRegularExpressionMatch fenceMatch = fence.match(lines.at(i));
        if (!openFence.isEmpty()) {
            const QString marker = fenceMatch.captured(1);
            if (fenceMatch.hasMatch() && marker.at(0) == openFence.at(0) &&
                marker.size() >= openFence.size())
                openFence.clear();
            continue;
        }
        if (fenceMatch.hasMatch()) {
            openFence = fenceMatch.captured(1);
            continue;
        }
        if (item.match(lines.at(i)).hasMatch())
            itemLines.append(i);
    }
    if (index < 0 || index >= itemLines.size() ||
        itemLines.size() != taskItemCount(source))
        return QString();
    QString &line = lines[itemLines.at(index)];
    const QRegularExpressionMatch match = item.match(line);
    line[match.capturedStart(2)] =
        match.captured(2) == QLatin1String(" ") ? QLatin1Char('x') : QLatin1Char(' ');
    return lines.join(QLatin1Char('\n'));
}

int taskItemIndexAt(const QTextDocument *document, const QPointF &position)
{
    if (!document || !document->documentLayout())
        return -1;
    const QTextBlock hit = document->documentLayout()->blockWithMarkerAt(position);
    if (!hit.isValid() || !hasTaskMarker(hit))
        return -1;
    int index = 0;
    for (QTextBlock block = document->begin(); block.isValid() && block != hit;
         block = block.next())
        index += hasTaskMarker(block) ? 1 : 0;
    return index;
}

}
