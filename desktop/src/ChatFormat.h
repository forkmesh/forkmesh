#pragma once

#include "ChatLinkText.h"

#include <QJsonObject>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

#include <vector>

namespace forkmesh::chatformat {

inline constexpr int kMaxDepth = 8;
inline constexpr int kMarkerReach = 1000;

struct Node {
    enum class Type { Text, Strong, Em, Strike, Underline, Code, Link, Url, Mention };
    Type type = Type::Text;
    QString text;
    QString href;
    std::vector<Node> children;
};

struct Block {
    enum class Type { Paragraph, Bullets, Numbers, Quote, Pre };
    Type type = Type::Paragraph;
    QStringList lines;
};

struct Palette {
    QString codeBackground, border, muted;
};

inline Palette paletteFor(bool dark)
{
    if (dark)
        return {QStringLiteral("#161b22"), QStringLiteral("#30363d"),
                QStringLiteral("#8b949e")};
    return {QStringLiteral("#eff1f3"), QStringLiteral("#d0d7de"),
            QStringLiteral("#59636e")};
}

inline bool opensAfter(QChar c)
{
    static const QString kOpeners = QStringLiteral(
        "([{\"'“‘<>-–—/:;,.!?*_~+");
    return c.isSpace() || kOpeners.contains(c);
}

inline int closingMarker(const QString &text, int from, const QString &marker)
{
    int at = text.indexOf(marker, from + 1);
    while (at >= 0 && at - from <= kMarkerReach) {
        const QChar before = text.at(at - 1);
        const int afterIndex = at + int(marker.size());
        const QChar after = afterIndex < text.size() ? text.at(afterIndex) : QChar();
        const bool sameMarkerFollows =
            marker.size() == 1 && !after.isNull() && after == marker.at(0);
        if (!before.isSpace() && !ChatLinkText::isAsciiAlnum(after) &&
            !sameMarkerFollows)
            return at;
        at = text.indexOf(marker, at + 1);
    }
    return -1;
}

struct Marker {
    QString marker;
    Node::Type type = Node::Type::Text;
    int close = -1;
};

inline Marker markerAt(const QString &text, int index)
{
    if (index > 0 && !opensAfter(text.at(index - 1)))
        return {};
    static const std::vector<std::pair<QString, Node::Type>> kMarkers = {
        {QStringLiteral("**"), Node::Type::Strong},
        {QStringLiteral("~~"), Node::Type::Strike},
        {QStringLiteral("++"), Node::Type::Underline},
        {QStringLiteral("*"), Node::Type::Strong},
        {QStringLiteral("_"), Node::Type::Em},
        {QStringLiteral("~"), Node::Type::Strike},
    };
    for (const auto &[marker, type] : kMarkers) {
        if (!QStringView(text).mid(index).startsWith(marker))
            continue;
        const int next = index + int(marker.size());
        if (next >= text.size() || text.at(next).isSpace())
            continue;
        const int close = closingMarker(text, next, marker);
        if (close >= 0)
            return {marker, type, close};
    }
    return {};
}

inline Node textNode(const QString &text)
{
    Node node;
    node.text = text;
    return node;
}

inline std::vector<Node> parseInline(const QString &text, int depth = 0)
{
    std::vector<Node> nodes;
    if (depth > kMaxDepth) {
        if (!text.isEmpty())
            nodes.push_back(textNode(text));
        return nodes;
    }
    QString plain;
    auto flush = [&nodes, &plain] {
        if (plain.isEmpty())
            return;
        nodes.push_back(textNode(plain));
        plain.clear();
    };
    static const QRegularExpression slackLink(
        QStringLiteral("<((?:https?|mailto):[^\\s|<>]+)(?:\\|([^<>\\n]+))?>"),
        QRegularExpression::CaseInsensitiveOption);
    const int size = text.size();
    int index = 0;
    while (index < size) {
        const QChar c = text.at(index);
        if (c == QLatin1Char('\\') && index + 1 < size) {
            plain += text.at(index + 1);
            index += 2;
            continue;
        }
        if (c == QLatin1Char('[')) {
            const ChatLinkText::MarkdownLink link =
                ChatLinkText::markdownLinkAt(text, index);
            if (link.isValid()) {
                flush();
                std::vector<Node> children = parseInline(
                    text.mid(index + 1, link.labelEnd - index - 1), depth + 1);
                const QString href = ChatLinkText::sanitizedHref(
                    text.mid(link.labelEnd + 2, link.targetEnd - link.labelEnd - 2));
                if (href.isEmpty()) {
                    for (Node &child : children)
                        nodes.push_back(std::move(child));
                } else {
                    Node node;
                    node.type = Node::Type::Link;
                    node.href = href;
                    node.children = std::move(children);
                    nodes.push_back(std::move(node));
                }
                index = link.targetEnd + 1;
                continue;
            }
        }
        if (c == QLatin1Char('<')) {
            const QRegularExpressionMatch match = slackLink.match(
                text, index, QRegularExpression::NormalMatch,
                QRegularExpression::AnchorAtOffsetMatchOption);
            if (match.hasMatch()) {
                flush();
                const QString target = match.captured(1);
                QString label = match.captured(2).trimmed();
                if (label.isEmpty())
                    label = target.startsWith(QLatin1String("mailto:"),
                                              Qt::CaseInsensitive)
                                ? target.mid(7)
                                : target;
                const QString href = ChatLinkText::sanitizedHref(target);
                if (href.isEmpty()) {
                    nodes.push_back(textNode(label));
                } else {
                    Node node;
                    node.type = Node::Type::Link;
                    node.href = href;
                    node.children.push_back(textNode(label));
                    nodes.push_back(std::move(node));
                }
                index = match.capturedEnd(0);
                continue;
            }
        }
        if (QStringView(text).mid(index).startsWith(QLatin1String("```"))) {
            const int close = text.indexOf(QStringLiteral("```"), index + 3);
            if (close > index + 3) {
                flush();
                Node node;
                node.type = Node::Type::Code;
                node.text = text.mid(index + 3, close - index - 3);
                nodes.push_back(std::move(node));
                index = close + 3;
                continue;
            }
        }
        if (c == QLatin1Char('`')) {
            const int close = text.indexOf(QLatin1Char('`'), index + 1);
            if (close > index + 1) {
                flush();
                Node node;
                node.type = Node::Type::Code;
                node.text = text.mid(index + 1, close - index - 1);
                nodes.push_back(std::move(node));
                index = close + 1;
                continue;
            }
        }
        const Marker marker = markerAt(text, index);
        if (marker.close >= 0) {
            flush();
            Node node;
            node.type = marker.type;
            const int start = index + int(marker.marker.size());
            node.children = parseInline(text.mid(start, marker.close - start), depth + 1);
            nodes.push_back(std::move(node));
            index = marker.close + int(marker.marker.size());
            continue;
        }
        const int urlEnd = ChatLinkText::rawUrlEnd(text, index);
        if (urlEnd > index) {
            flush();
            Node node;
            node.type = Node::Type::Url;
            node.text = text.mid(index, urlEnd - index);
            node.href = ChatLinkText::sanitizedHref(node.text);
            nodes.push_back(std::move(node));
            index = urlEnd;
            continue;
        }
        if (c == QLatin1Char('@')) {
            const int handleEnd = ChatLinkText::mentionEnd(text, index);
            if (handleEnd > index) {
                flush();
                Node node;
                node.type = Node::Type::Mention;
                node.text = text.mid(index, handleEnd - index);
                nodes.push_back(std::move(node));
                index = handleEnd;
                continue;
            }
        }
        plain += c;
        ++index;
    }
    flush();
    return nodes;
}

inline bool listItem(const QString &line, bool *ordered, QString *content)
{
    static const QRegularExpression bullet(QStringLiteral("^\\s*[-*•]\\s+(.*)$"));
    static const QRegularExpression numbered(QStringLiteral("^\\s*\\d+[.)]\\s+(.*)$"));
    QRegularExpressionMatch match = bullet.match(line);
    if (match.hasMatch()) {
        *ordered = false;
        *content = match.captured(1);
        return true;
    }
    match = numbered.match(line);
    if (match.hasMatch()) {
        *ordered = true;
        *content = match.captured(1);
        return true;
    }
    return false;
}

inline bool quoteLine(const QString &line, QString *content)
{
    if (!line.startsWith(QLatin1Char('>')))
        return false;
    *content = line.mid(line.size() > 1 && line.at(1) == QLatin1Char(' ') ? 2 : 1);
    return true;
}

inline int fenceEnd(const QStringList &lines, int index, QString *body)
{
    const QString first = lines.at(index).mid(3);
    if (first.size() >= 3 && first.endsWith(QLatin1String("```"))) {
        *body = first.chopped(3);
        return index + 1;
    }
    for (int end = index + 1; end < lines.size(); ++end) {
        QString closing = lines.at(end);
        while (!closing.isEmpty() && closing.back().isSpace())
            closing.chop(1);
        if (!closing.endsWith(QLatin1String("```")))
            continue;
        QStringList parts{first};
        parts += lines.mid(index + 1, end - index - 1);
        parts << closing.chopped(3);
        if (parts.first().trimmed().isEmpty())
            parts.removeFirst();
        if (!parts.isEmpty() && parts.last().trimmed().isEmpty())
            parts.removeLast();
        *body = parts.join(QLatin1Char('\n'));
        return end + 1;
    }
    return -1;
}

inline QString normalized(QString source)
{
    source.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    source.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    source.remove(QChar(0));
    return source;
}

inline std::vector<Block> parseBlocks(const QString &source)
{
    const QStringList lines = normalized(source).split(QLatin1Char('\n'));
    std::vector<Block> blocks;
    bool inParagraph = false;
    int index = 0;
    while (index < lines.size()) {
        const QString &line = lines.at(index);
        QString body;
        bool ordered = false;
        QString content;
        const int fence =
            line.startsWith(QLatin1String("```")) ? fenceEnd(lines, index, &body) : -1;
        if (fence >= 0) {
            inParagraph = false;
            blocks.push_back({Block::Type::Pre, {body}});
            index = fence;
            continue;
        }
        if (listItem(line, &ordered, &content)) {
            inParagraph = false;
            Block block{ordered ? Block::Type::Numbers : Block::Type::Bullets, {}};
            bool nextOrdered = false;
            while (index < lines.size() &&
                   listItem(lines.at(index), &nextOrdered, &content) &&
                   nextOrdered == ordered) {
                block.lines << content;
                ++index;
            }
            blocks.push_back(block);
            continue;
        }
        if (quoteLine(line, &content)) {
            inParagraph = false;
            Block block{Block::Type::Quote, {}};
            while (index < lines.size() && quoteLine(lines.at(index), &content)) {
                block.lines << content;
                ++index;
            }
            blocks.push_back(block);
            continue;
        }
        if (!inParagraph) {
            blocks.push_back({Block::Type::Paragraph, {}});
            inParagraph = true;
        }
        blocks.back().lines << line;
        ++index;
    }
    return blocks;
}

inline QString inlinePlainText(const std::vector<Node> &nodes)
{
    QString out;
    for (const Node &node : nodes)
        out += node.children.empty() ? node.text : inlinePlainText(node.children);
    return out;
}

inline QString toPlainText(const QString &source)
{
    QStringList out;
    for (const Block &block : parseBlocks(source)) {
        switch (block.type) {
        case Block::Type::Pre:
            out << block.lines;
            break;
        case Block::Type::Bullets:
            for (const QString &item : block.lines)
                out << QStringLiteral("• ") + inlinePlainText(parseInline(item));
            break;
        case Block::Type::Numbers:
            for (int number = 0; number < block.lines.size(); ++number)
                out << QStringLiteral("%1. ").arg(number + 1) +
                           inlinePlainText(parseInline(block.lines.at(number)));
            break;
        case Block::Type::Quote:
            for (const QString &line : block.lines)
                out << QStringLiteral("“") + inlinePlainText(parseInline(line)) +
                           QStringLiteral("”");
            break;
        case Block::Type::Paragraph:
            for (const QString &line : block.lines)
                out << inlinePlainText(parseInline(line));
            break;
        }
    }
    return out.join(QLatin1Char('\n'));
}

inline QString notificationText(const QString &plain, int maxLines = 4,
                                int maxChars = 180)
{
    QStringList lines;
    int used = 0;
    bool truncated = false;
    for (const QString &raw : plain.split(QLatin1Char('\n'))) {
        const QString line = raw.simplified();
        if (line.isEmpty())
            continue;
        if (lines.size() >= maxLines || used >= maxChars) {
            truncated = true;
            break;
        }
        const int room = maxChars - used;
        if (line.size() > room) {
            lines << line.left(qMax(1, room - 1)).trimmed() + QStringLiteral("…");
            used = maxChars;
            continue;
        }
        lines << line;
        used += int(line.size());
    }
    if (truncated && !lines.isEmpty() && !lines.last().endsWith(QChar(0x2026)))
        lines.last() += QStringLiteral(" …");
    return lines.join(QLatin1Char('\n'));
}

inline QString inlineHtml(const std::vector<Node> &nodes,
                          const ChatLinkText::MentionResolver &resolveMention,
                          const Palette &palette)
{
    QString html;
    for (const Node &node : nodes) {
        switch (node.type) {
        case Node::Type::Text:
            html += ChatLinkText::escapedSegment(node.text);
            break;
        case Node::Type::Strong:
            html += QStringLiteral("<b>") +
                    inlineHtml(node.children, resolveMention, palette) +
                    QStringLiteral("</b>");
            break;
        case Node::Type::Em:
            html += QStringLiteral("<i>") +
                    inlineHtml(node.children, resolveMention, palette) +
                    QStringLiteral("</i>");
            break;
        case Node::Type::Strike:
            html += QStringLiteral("<s>") +
                    inlineHtml(node.children, resolveMention, palette) +
                    QStringLiteral("</s>");
            break;
        case Node::Type::Underline:
            html += QStringLiteral("<u>") +
                    inlineHtml(node.children, resolveMention, palette) +
                    QStringLiteral("</u>");
            break;
        case Node::Type::Code:
            html += QStringLiteral("<code style=\"background-color:%1\">&nbsp;%2&nbsp;</code>")
                        .arg(palette.codeBackground,
                             ChatLinkText::escapedSegment(node.text));
            break;
        case Node::Type::Link:
            html += ChatLinkText::anchor(
                node.href, inlineHtml(node.children, resolveMention, palette));
            break;
        case Node::Type::Url:
            html += node.href.isEmpty()
                        ? ChatLinkText::escapedSegment(node.text)
                        : ChatLinkText::anchor(node.href,
                                               ChatLinkText::escapedSegment(node.text));
            break;
        case Node::Type::Mention: {
            const QString key =
                resolveMention ? resolveMention(node.text.mid(1)) : QString();
            html += key.isEmpty()
                        ? ChatLinkText::escapedSegment(node.text)
                        : ChatLinkText::anchor(
                              QStringLiteral("forkmesh-mention:") + key,
                              ChatLinkText::escapedSegment(node.text),
                              " font-weight:700; text-decoration:none;");
            break;
        }
        }
    }
    return html;
}

inline QString inlineHtml(const QString &line,
                          const ChatLinkText::MentionResolver &resolveMention = {},
                          bool dark = true)
{
    return inlineHtml(parseInline(line), resolveMention, paletteFor(dark));
}

inline QString toHtml(const QString &source,
                      const ChatLinkText::MentionResolver &resolveMention = {},
                      bool dark = true)
{
    const Palette palette = paletteFor(dark);
    const std::vector<Block> blocks = parseBlocks(source);
    if (blocks.size() == 1 && blocks.front().type == Block::Type::Paragraph) {
        QStringList lines;
        for (const QString &line : blocks.front().lines)
            lines << inlineHtml(parseInline(line), resolveMention, palette);
        return lines.join(QStringLiteral("<br>"));
    }
    QString html;
    for (const Block &block : blocks) {
        switch (block.type) {
        case Block::Type::Pre:
            html += QStringLiteral(
                        "<table width=\"100%\" cellspacing=\"0\" cellpadding=\"0\" "
                        "style=\"margin-top:3px; margin-bottom:3px; "
                        "border-collapse:collapse\"><tr><td bgcolor=\"%1\" "
                        "style=\"padding:7px 10px; border:1px solid %2\">"
                        "<pre style=\"margin:0\">%3</pre></td></tr></table>")
                        .arg(palette.codeBackground, palette.border,
                             block.lines.join(QLatin1Char('\n')).toHtmlEscaped());
            break;
        case Block::Type::Bullets:
        case Block::Type::Numbers: {
            const QString tag = block.type == Block::Type::Bullets
                                    ? QStringLiteral("ul")
                                    : QStringLiteral("ol");
            html += QStringLiteral("<%1 style=\"margin-top:2px; margin-bottom:2px\">")
                        .arg(tag);
            for (const QString &item : block.lines)
                html += QStringLiteral("<li>") +
                        inlineHtml(parseInline(item), resolveMention, palette) +
                        QStringLiteral("</li>");
            html += QStringLiteral("</%1>").arg(tag);
            break;
        }
        case Block::Type::Quote: {
            QStringList lines;
            for (const QString &line : block.lines)
                lines << inlineHtml(parseInline(line), resolveMention, palette);
            html += QStringLiteral(
                        "<table cellspacing=\"0\" cellpadding=\"0\" "
                        "style=\"margin-top:3px; margin-bottom:3px\"><tr>"
                        "<td width=\"3\" bgcolor=\"%1\"></td>"
                        "<td style=\"padding-left:9px; color:%2\">%3</td>"
                        "</tr></table>")
                        .arg(palette.border, palette.muted,
                             lines.join(QStringLiteral("<br>")));
            break;
        }
        case Block::Type::Paragraph: {
            QStringList lines;
            for (const QString &line : block.lines)
                lines << inlineHtml(parseInline(line), resolveMention, palette);
            html += QStringLiteral("<p style=\"margin-top:0; margin-bottom:0\">%1</p>")
                        .arg(lines.join(QStringLiteral("<br>")));
            break;
        }
        }
    }
    return html;
}

inline ChatLinkText::Insertion wrapSelection(const QString &value, int selectionStart,
                                             int selectionEnd, const QString &before,
                                             const QString &after)
{
    const int size = value.size();
    const int start = qBound(0, selectionStart, size);
    const int end = qBound(start, selectionEnd, size);
    ChatLinkText::Insertion insertion;
    insertion.text = value.left(start) + before + value.mid(start, end - start) + after +
                     value.mid(end);
    insertion.selectionStart = start + int(before.size());
    insertion.selectionLength = end - start;
    return insertion;
}

inline QString bodySource(const QJsonObject &message, int maxChars)
{
    const QJsonObject rich = message.value(QStringLiteral("richText")).toObject();
    const QString source = rich.value(QStringLiteral("source")).toString();
    if (rich.value(QStringLiteral("v")).toInt() == 1 && !source.isEmpty() &&
        source.size() <= maxChars)
        return source;
    return message.value(QStringLiteral("text")).toString().left(maxChars);
}

} // namespace forkmesh::chatformat
