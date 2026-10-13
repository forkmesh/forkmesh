#pragma once

#include <QChar>
#include <QRegularExpression>
#include <QString>
#include <QUrl>

#include <functional>

// Links inside chat message text.
//
// Chat bodies travel as plain text, so a URL someone typed arrived as dead
// characters: handing a teammate something clickable meant sending the bare
// URL as a message of its own, away from the sentence explaining it. This
// resolves the two shapes people actually write — a pasted http(s) URL and
// markdown's [label](url) — into anchors, and leaves every other character
// escaped exactly as before.
//
// The web chat renderer (app/public/chat-rich-text.js) resolves the same two
// shapes with the same trailing-punctuation and safe-scheme rules, so one
// message links the same way whichever surface shows it.
namespace ChatLinkText {

// Anchors carry the accent the mention links already use, so a linked word
// reads as "clickable" the same way in both themes' message text.
inline const char *kLinkColor = "#58a6ff";

inline bool isAsciiLetter(QChar c)
{
    return (c >= QLatin1Char('a') && c <= QLatin1Char('z')) ||
           (c >= QLatin1Char('A') && c <= QLatin1Char('Z'));
}

inline bool isAsciiAlnum(QChar c)
{
    return isAsciiLetter(c) || (c >= QLatin1Char('0') && c <= QLatin1Char('9'));
}

// [A-Za-z0-9_], the \b word character class the mention pattern boundaries on.
inline bool isAsciiWordChar(QChar c)
{
    return isAsciiAlnum(c) || c == QLatin1Char('_');
}

// A chat message may only hand the browser a scheme that cannot execute
// anything or read the disk: javascript:, data: and file: targets are not
// links. The label of a rejected target is still shown — as plain text — which
// is what the web renderer does with it, so neither surface silently swallows
// the sentence a bad link was written into.
inline QString sanitizedHref(const QString &raw)
{
    constexpr int kMaxHrefChars = 2048;
    QString candidate;
    candidate.reserve(raw.size());
    for (const QChar c : raw) {
        // Control characters never belong in a target and are the classic way
        // to smuggle a scheme past a prefix check ("java\tscript:").
        if (c.unicode() < 0x20 || c.unicode() == 0x7f)
            continue;
        candidate += c;
    }
    candidate = candidate.trimmed();
    if (candidate.isEmpty() || candidate.size() > kMaxHrefChars)
        return {};

    // Tolerant mode percent-encodes the stray spaces and brackets people paste
    // rather than rejecting the URL outright; the scheme check below is what
    // decides whether the target is allowed, not the parser's strictness.
    const QUrl url(candidate, QUrl::TolerantMode);
    if (!url.isValid() || url.isRelative())
        return {};
    const QString scheme = url.scheme();
    if (scheme == QLatin1String("mailto"))
        return url.toString(QUrl::FullyEncoded);
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https"))
        return {};
    if (url.host().isEmpty())
        return {};
    return url.toString(QUrl::FullyEncoded);
}

// One escaped plain-text run of a message body: newlines survive as <br> the
// way a QLabel renders them, everything else is inert.
inline QString escapedSegment(QString segment)
{
    segment.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    segment.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    QString escaped = segment.toHtmlEscaped();
    escaped.replace(QLatin1Char('\n'), QStringLiteral("<br>"));
    return escaped;
}

// End of the bare URL starting at `start`, or `start` when there is none.
// Mirrors the web's /^https?:\/\/[^\s<>]+/ plus its trailing-punctuation trim,
// so "(see https://forkmesh.com/status)." links the address and leaves the
// sentence's own punctuation outside the anchor.
inline int rawUrlEnd(const QString &text, int start)
{
    const QStringView rest = QStringView(text).mid(start);
    int schemeChars = 0;
    if (rest.startsWith(QLatin1String("http://"), Qt::CaseInsensitive))
        schemeChars = 7;
    else if (rest.startsWith(QLatin1String("https://"), Qt::CaseInsensitive))
        schemeChars = 8;
    else
        return start;
    int end = start;
    while (end < text.size()) {
        const QChar c = text.at(end);
        if (c.isSpace() || c == QLatin1Char('<') || c == QLatin1Char('>'))
            break;
        ++end;
    }
    static const QString kTrailing = QStringLiteral(",.!?;:");
    while (end > start) {
        const QChar last = text.at(end - 1);
        if (kTrailing.contains(last)) {
            --end;
            continue;
        }
        // A closing parenthesis belongs to the address when the address opened
        // one — "…/Foo_(bar)" is a page, "(see …/status)" is a sentence.
        if (last == QLatin1Char(')')) {
            int depth = 0;
            for (int i = start; i < end; ++i) {
                if (text.at(i) == QLatin1Char('('))
                    ++depth;
                else if (text.at(i) == QLatin1Char(')'))
                    --depth;
            }
            if (depth < 0) {
                --end;
                continue;
            }
        }
        break;
    }
    // A scheme with nothing after it is punctuation, not a destination.
    return end > start + schemeChars ? end : start;
}

// End of the @handle whose '@' sits at `start`, or `start` when the text there
// is not a mention. Same shape as the pattern MessageRow linked before links
// existed: a leading letter, up to 63 characters of [A-Za-z0-9-] that cannot
// end on a dash, and a word boundary on both sides.
inline int mentionEnd(const QString &text, int start)
{
    constexpr int kMaxHandleChars = 63;
    if (start < 0 || start >= text.size() || text.at(start) != QLatin1Char('@'))
        return start;
    if (start > 0) {
        const QChar previous = text.at(start - 1);
        if (isAsciiWordChar(previous) || previous == QLatin1Char('-'))
            return start;
    }
    const int handleStart = start + 1;
    if (handleStart >= text.size() || !isAsciiLetter(text.at(handleStart)))
        return start;
    int end = handleStart + 1;
    while (end < text.size() && end - handleStart < kMaxHandleChars &&
           (isAsciiAlnum(text.at(end)) || text.at(end) == QLatin1Char('-')))
        ++end;
    while (end > handleStart + 1 && text.at(end - 1) == QLatin1Char('-'))
        --end;
    if (end < text.size() && isAsciiWordChar(text.at(end)))
        return start;
    return end;
}

// End of the ')' closing a [label](target) target that opens at `start`,
// counting nested parentheses so a Wikipedia-style URL keeps its own.
inline int linkTargetEnd(const QString &text, int start)
{
    int depth = 0;
    for (int index = start; index < text.size(); ++index) {
        const QChar c = text.at(index);
        if (c == QLatin1Char('\n'))
            return -1; // a link never spans lines; the web parses line by line
        if (c == QLatin1Char('(')) {
            ++depth;
            continue;
        }
        if (c != QLatin1Char(')'))
            continue;
        if (depth == 0)
            return index;
        --depth;
    }
    return -1;
}

// A [label](target) span opening at `start`. `labelEnd` indexes the ']' and
// `targetEnd` the ')'; both are -1 when the text there is not a link.
struct MarkdownLink {
    int labelEnd = -1;
    int targetEnd = -1;

    bool isValid() const { return labelEnd >= 0 && targetEnd > labelEnd; }
};

inline MarkdownLink markdownLinkAt(const QString &text, int start)
{
    if (start < 0 || start >= text.size() || text.at(start) != QLatin1Char('['))
        return {};
    // "[](target)" labels nothing: an anchor with no text would be an
    // invisible click target, so it stays literal the way the web leaves it.
    const int labelEnd = text.indexOf(QStringLiteral("]("), start + 1);
    if (labelEnd <= start + 1)
        return {};
    if (QStringView(text).mid(start, labelEnd - start).contains(QLatin1Char('\n')))
        return {};
    const int targetEnd = linkTargetEnd(text, labelEnd + 2);
    if (targetEnd <= labelEnd + 1)
        return {};
    return {labelEnd, targetEnd};
}

// Given an @handle, the profile key the mention should link to, or an empty
// string when nobody by that name is in the conversation (an unknown @name
// stays plain text, exactly as it did before).
using MentionResolver = std::function<QString(const QString &handle)>;

// Double-quoted attributes on purpose: an apostrophe is legal in a URL path
// and QString::toHtmlEscaped() does not escape it, so a single-quoted href
// would let "https://host/it's" close the attribute early.
inline QString anchor(const QString &href, const QString &innerHtml,
                      const char *extraStyle = "")
{
    return QStringLiteral("<a href=\"%1\" style=\"color:%2;%3\">%4</a>")
        .arg(href.toHtmlEscaped(), QString::fromLatin1(kLinkColor),
             QString::fromLatin1(extraStyle), innerHtml);
}

// One chat message body as the rich text a QLabel shows: escaped throughout,
// with anchors for @mentions, [label](target) links and bare URLs.
inline QString renderBodyHtml(const QString &text,
                              const MentionResolver &resolveMention = {})
{
    QString html;
    QString plain; // pending literal run, escaped in one go when a link starts
    const int size = text.size();
    int index = 0;

    auto flushPlain = [&html, &plain] {
        if (plain.isEmpty())
            return;
        html += escapedSegment(plain);
        plain.clear();
    };

    while (index < size) {
        const QChar c = text.at(index);

        if (c == QLatin1Char('[')) {
            const MarkdownLink link = markdownLinkAt(text, index);
            if (link.isValid()) {
                const QString label = text.mid(index + 1, link.labelEnd - index - 1);
                const QString href = sanitizedHref(
                    text.mid(link.labelEnd + 2, link.targetEnd - link.labelEnd - 2));
                flushPlain();
                if (href.isEmpty()) {
                    // A target we will not open: show what the author wrote
                    // instead of a dead anchor, the way the web renderer does.
                    html += escapedSegment(label);
                } else {
                    html += anchor(href, escapedSegment(label));
                }
                index = link.targetEnd + 1;
                continue;
            }
        }

        const int urlEnd = rawUrlEnd(text, index);
        if (urlEnd > index) {
            const QString url = text.mid(index, urlEnd - index);
            const QString href = sanitizedHref(url);
            flushPlain();
            if (href.isEmpty())
                html += escapedSegment(url);
            else
                html += anchor(href, escapedSegment(url));
            index = urlEnd;
            continue;
        }

        if (c == QLatin1Char('@') && resolveMention) {
            const int handleEnd = mentionEnd(text, index);
            if (handleEnd > index) {
                const QString handle = text.mid(index + 1, handleEnd - index - 1);
                const QString key = resolveMention(handle);
                if (!key.isEmpty()) {
                    flushPlain();
                    html += anchor(QStringLiteral("forkmesh-mention:") + key,
                                   escapedSegment(text.mid(index, handleEnd - index)),
                                   " font-weight:700; text-decoration:none;");
                    index = handleEnd;
                    continue;
                }
            }
        }

        plain += c;
        ++index;
    }
    flushPlain();
    return html;
}

// Whether an activated anchor is a mention rather than a link to the browser.
inline bool isMentionHref(const QString &href)
{
    return href.startsWith(QStringLiteral("forkmesh-mention:"));
}

inline QString flattenInlineMarkers(QString value)
{
    // The web composer's emphasis markers, dropped the way its own plain-text
    // fallback drops them (chat-rich-text.js plainTextFromRichSource) so a
    // message reads as prose here rather than as raw asterisks.
    static const QRegularExpression bold(QStringLiteral("\\*\\*([^*]+)\\*\\*"));
    static const QRegularExpression underline(QStringLiteral("\\+\\+([^+]+)\\+\\+"));
    static const QRegularExpression strike(QStringLiteral("~~([^~]+)~~"));
    static const QRegularExpression code(QStringLiteral("`([^`]+)`"));
    static const QRegularExpression italic(QStringLiteral("\\*([^*]+)\\*"));
    static const QRegularExpression escaped(
        QStringLiteral("\\\\([\\\\`*+~\\[\\]()])"));
    const QString replacement = QStringLiteral("\\1");
    value.replace(bold, replacement);
    value.replace(underline, replacement);
    value.replace(strike, replacement);
    value.replace(code, replacement);
    value.replace(italic, replacement);
    value.replace(escaped, replacement);
    return value;
}

// The plain text a web-composed message should show here, with its links
// intact.
//
// The web sends both a rich source and a flattened `text` fallback, and that
// fallback rewrites [label](url) down to "label" — so the address behind a
// linked word never reached this client at all. Flattening the source here
// instead keeps the link syntax that renderBodyHtml resolves, while dropping
// every marker the desktop does not render.
inline QString flattenRichSourceKeepingLinks(const QString &source)
{
    QString out;
    out.reserve(source.size());
    int segmentStart = 0;
    int index = 0;
    while (index < source.size()) {
        if (source.at(index) == QLatin1Char('[')) {
            const MarkdownLink link = markdownLinkAt(source, index);
            if (link.isValid()) {
                out += flattenInlineMarkers(
                    source.mid(segmentStart, index - segmentStart));
                const QString label =
                    source.mid(index + 1, link.labelEnd - index - 1);
                // The target is copied verbatim: an emphasis marker inside a
                // URL is part of the address, not formatting.
                out += QLatin1Char('[') + flattenInlineMarkers(label) +
                       source.mid(link.labelEnd, link.targetEnd - link.labelEnd + 1);
                index = link.targetEnd + 1;
                segmentStart = index;
                continue;
            }
        }
        ++index;
    }
    out += flattenInlineMarkers(source.mid(segmentStart));

    static const QRegularExpression blockPrefix(
        QStringLiteral("^(?:-\\s+|\\d+\\.\\s+|>\\s?)"),
        QRegularExpression::MultilineOption);
    out.remove(blockPrefix);
    return out;
}

// Where the composer's caret lands after the link button runs.
struct Insertion {
    QString text;
    int selectionStart = 0;
    int selectionLength = 0;
};

// The composer's "Insert link" action, as a value: wrap whatever is selected
// in [label](target) and leave the caret on the half the author still has to
// write. A URL already on the clipboard becomes the target — pasting the
// address first and then describing it is the common order — and the label is
// selected instead so typing renames it.
inline Insertion linkInsertion(const QString &value, int selectionStart,
                               int selectionEnd, const QString &clipboard = {})
{
    const int size = value.size();
    const int start = qBound(0, selectionStart, size);
    const int end = qBound(start, selectionEnd, size);
    const QString selected = value.mid(start, end - start);
    const QString clipboardHref = sanitizedHref(clipboard);
    const bool haveTarget = !clipboardHref.isEmpty();
    const QString label =
        selected.isEmpty() ? QStringLiteral("link text") : selected;
    const QString target =
        haveTarget ? clipboard.trimmed() : QStringLiteral("https://");

    Insertion insertion;
    insertion.text = value.left(start) + QLatin1Char('[') + label +
                     QStringLiteral("](") + target + QLatin1Char(')') +
                     value.mid(end);
    if (haveTarget && selected.isEmpty()) {
        insertion.selectionStart = start + 1;
        insertion.selectionLength = label.size();
    } else if (haveTarget) {
        insertion.selectionStart =
            start + 1 + label.size() + 2 + target.size() + 1;
        insertion.selectionLength = 0;
    } else {
        insertion.selectionStart = start + 1 + label.size() + 2;
        insertion.selectionLength = target.size();
    }
    return insertion;
}

} // namespace ChatLinkText
