#include "MarkdownLiveHighlighter.h"

#include "DesignTokens.h"

#include <QFont>
#include <QPair>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextDocument>

#include <algorithm>
#include <utility>

MarkdownLiveHighlighter::MarkdownLiveHighlighter(QObject *parent)
    : QSyntaxHighlighter(parent)
{
}

void MarkdownLiveHighlighter::setActiveBlock(int block)
{
    if (block == m_active)
        return;
    const int previous = std::exchange(m_active, block);
    QTextDocument *doc = document();
    if (!doc)
        return;
    for (const int number : {previous, block}) {
        const QTextBlock found = doc->findBlockByNumber(number);
        if (found.isValid())
            rehighlightBlock(found);
    }
}

void MarkdownLiveHighlighter::highlightBlock(const QString &text)
{
    static const QRegularExpression fence(QStringLiteral("^\\s{0,3}(`{3,}|~{3,})"));
    static const QRegularExpression heading(QStringLiteral("^\\s{0,3}(#{1,6})\\s+"));
    static const QRegularExpression quote(QStringLiteral("^\\s{0,3}>\\s?"));
    static const QRegularExpression list(
        QStringLiteral("^\\s*([-*+]|\\d+[.)])\\s+(\\[[ xX]\\]\\s+)?"));
    static const QRegularExpression rule(QStringLiteral("^\\s{0,3}([-*_])(\\s*\\1){2,}\\s*$"));
    static const QRegularExpression code(QStringLiteral("`([^`]+)`"));
    static const QRegularExpression strong(
        QStringLiteral("(\\*\\*|__)(?=\\S)(.+?)(?<=\\S)\\1"));
    static const QRegularExpression emphasis(
        QStringLiteral("(?<![*_\\w])([*_])(?=[^\\s*_])(.+?)(?<=[^\\s*_])\\1(?![*_\\w])"));
    static const QRegularExpression strike(QStringLiteral("~~(?=\\S)(.+?)(?<=\\S)~~"));
    static const QRegularExpression link(
        QStringLiteral("!?\\[([^\\]]+)\\]\\(([^)\\s]+)(\\s+\"[^\"]*\")?\\)"));

    m_hide = currentBlock().blockNumber() != m_active;
    const bool inFence = previousBlockState() == 1;
    const bool fenceLine = fence.match(text).hasMatch();
    if (inFence || fenceLine) {
        change(0, int(text.size()), [](QTextCharFormat &format) {
            format.setFontFixedPitch(true);
            format.setFontFamilies({QStringLiteral("monospace")});
            format.setBackground(Theme::color(Theme::Role::BgSubtle));
        });
        if (fenceLine)
            marker(0, int(text.size()));
        setCurrentBlockState(fenceLine == inFence ? 0 : 1);
        return;
    }
    setCurrentBlockState(0);

    if (rule.match(text).hasMatch()) {
        marker(0, int(text.size()));
        return;
    }
    if (const QRegularExpressionMatch found = heading.match(text); found.hasMatch()) {
        static const qreal scales[] = {1.0, 1.7, 1.45, 1.25, 1.12, 1.05, 1.0};
        const qreal base = document()->defaultFont().pointSizeF() > 0
            ? document()->defaultFont().pointSizeF() : 10.0;
        const qreal size = base * scales[found.capturedLength(1)];
        change(0, int(text.size()), [size](QTextCharFormat &format) {
            format.setFontWeight(QFont::Bold);
            format.setFontPointSize(size);
        });
        marker(0, int(found.capturedLength(0)));
    } else if (const QRegularExpressionMatch found = quote.match(text); found.hasMatch()) {
        change(0, int(text.size()), [](QTextCharFormat &format) {
            format.setFontItalic(true);
            format.setForeground(Theme::color(Theme::Role::FgMuted));
        });
        marker(0, int(found.capturedLength(0)));
    } else if (const QRegularExpressionMatch found = list.match(text); found.hasMatch()) {
        change(0, int(found.capturedLength(0)), [](QTextCharFormat &format) {
            format.setForeground(Theme::color(Theme::Role::Accent));
        });
    }

    QList<QPair<int, int>> codeSpans;
    for (auto spans = code.globalMatch(text); spans.hasNext();) {
        const QRegularExpressionMatch found = spans.next();
        codeSpans.append({int(found.capturedStart(0)), int(found.capturedEnd(0))});
        change(int(found.capturedStart(0)), int(found.capturedLength(0)),
               [](QTextCharFormat &format) {
            format.setFontFixedPitch(true);
            format.setFontFamilies({QStringLiteral("monospace")});
            format.setBackground(Theme::color(Theme::Role::BgSubtle));
        });
        marker(int(found.capturedStart(0)), 1);
        marker(int(found.capturedEnd(0)) - 1, 1);
    }
    const auto inCode = [&codeSpans](int at) {
        return std::any_of(codeSpans.cbegin(), codeSpans.cend(),
                           [at](const QPair<int, int> &span) {
            return at >= span.first && at < span.second;
        });
    };
    const auto wrapped = [&](const QRegularExpression &pattern, int markerLength,
                             const std::function<void(QTextCharFormat &)> &style) {
        for (auto matches = pattern.globalMatch(text); matches.hasNext();) {
            const QRegularExpressionMatch found = matches.next();
            if (inCode(int(found.capturedStart(0))))
                continue;
            change(int(found.capturedStart(0)), int(found.capturedLength(0)), style);
            marker(int(found.capturedStart(0)), markerLength);
            marker(int(found.capturedEnd(0)) - markerLength, markerLength);
        }
    };
    wrapped(strong, 2, [](QTextCharFormat &format) { format.setFontWeight(QFont::Bold); });
    wrapped(emphasis, 1, [](QTextCharFormat &format) { format.setFontItalic(true); });
    wrapped(strike, 2, [](QTextCharFormat &format) { format.setFontStrikeOut(true); });
    for (auto links = link.globalMatch(text); links.hasNext();) {
        const QRegularExpressionMatch found = links.next();
        if (inCode(int(found.capturedStart(0))))
            continue;
        change(int(found.capturedStart(1)), int(found.capturedLength(1)),
               [](QTextCharFormat &format) {
            format.setForeground(Theme::color(Theme::Role::Accent));
            format.setFontUnderline(true);
        });
        marker(int(found.capturedStart(0)), int(found.capturedStart(1) - found.capturedStart(0)));
        marker(int(found.capturedEnd(1)), int(found.capturedEnd(0) - found.capturedEnd(1)));
    }
}

void MarkdownLiveHighlighter::change(int start, int length,
                                     const std::function<void(QTextCharFormat &)> &style)
{
    for (int at = start; at < start + length; ++at) {
        QTextCharFormat next = format(at);
        style(next);
        setFormat(at, 1, next);
    }
}

void MarkdownLiveHighlighter::marker(int start, int length)
{
    const bool hide = m_hide;
    change(start, length, [hide](QTextCharFormat &format) {
        if (hide) {
            format.setForeground(QColor(Qt::transparent));
            format.setFontPointSize(1);
        } else {
            format.setForeground(Theme::color(Theme::Role::FgSubtle));
        }
    });
}
