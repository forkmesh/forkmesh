#pragma once

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QStringView>

#include <algorithm>

namespace forkmesh::notes {

constexpr int kCommentContext = 32;
constexpr int kMaxCommentQuote = 1000;
constexpr int kMaxCommentBody = 4000;
constexpr int kPresencePollMs = 5000;
constexpr int kPresenceRetryMs = 30000;
constexpr int kPresenceTypingMs = 10000;
constexpr int kPresenceFaces = 4;

struct CommentAnchor {
    int start = -1;
    int end = -1;
    bool found() const { return start >= 0; }
};

inline CommentAnchor findCommentAnchor(const QString &markdown,
                                       const QString &quote,
                                       const QString &prefix,
                                       const QString &suffix, int start)
{
    CommentAnchor best;
    if (quote.isEmpty())
        return best;
    const QStringView text(markdown);
    int bestScore = -1;
    qint64 bestDistance = 0;
    for (qsizetype at = markdown.indexOf(quote); at >= 0;
         at = markdown.indexOf(quote, at + 1)) {
        int score = 0;
        if (text.left(at).endsWith(prefix))
            score += 2;
        if (text.mid(at + quote.size()).startsWith(suffix))
            score += 2;
        const qint64 distance = qAbs(qint64(at) - qint64(start));
        if (score > bestScore ||
            (score == bestScore && distance < bestDistance)) {
            bestScore = score;
            bestDistance = distance;
            best.start = int(at);
            best.end = int(at + quote.size());
        }
    }
    return best;
}

struct CommentQuote {
    QString quote;
    QString prefix;
    QString suffix;
    int start = 0;
    int end = 0;
    bool isEmpty() const { return quote.isEmpty(); }
};

inline CommentQuote commentQuoteAt(const QString &markdown, int start, int end)
{
    const int size = int(markdown.size());
    start = qBound(0, start, size);
    end = qBound(start, end, size);
    while (start < end && markdown.at(start).isSpace())
        ++start;
    while (end > start && markdown.at(end - 1).isSpace())
        --end;
    CommentQuote result;
    result.start = start;
    result.end = end;
    result.quote = markdown.mid(start, end - start);
    if (result.quote.isEmpty())
        return result;
    int from = qMax(0, start - kCommentContext);
    if (from > 0 && from < start && markdown.at(from).isLowSurrogate())
        ++from;
    result.prefix = markdown.mid(from, start - from);
    int to = qMin(size, end + kCommentContext);
    if (to < size && to > end && markdown.at(to - 1).isHighSurrogate())
        --to;
    result.suffix = markdown.mid(end, to - end);
    return result;
}

inline CommentQuote commentQuoteForText(const QString &markdown,
                                        const QString &text, int near = 0)
{
    const QString wanted = text.trimmed();
    const CommentAnchor anchor =
        findCommentAnchor(markdown, wanted, QString(), QString(), near);
    if (!anchor.found())
        return {};
    return commentQuoteAt(markdown, anchor.start, anchor.end);
}

inline QJsonObject commentQuoteFields(const CommentQuote &quote)
{
    if (quote.isEmpty())
        return {};
    return QJsonObject{{QStringLiteral("quote"), quote.quote},
                       {QStringLiteral("prefix"), quote.prefix},
                       {QStringLiteral("suffix"), quote.suffix},
                       {QStringLiteral("start"), quote.start},
                       {QStringLiteral("end"), quote.end}};
}

inline QString previewSearchText(const QString &quote)
{
    static const QRegularExpression marker(
        QStringLiteral("^\\s*(?:#{1,6}\\s+|>\\s*|[-*+]\\s+(?:\\[[ xX]\\]\\s+)?|"
                       "\\d+[.)]\\s+)*"));
    static const QRegularExpression link(
        QStringLiteral("!?\\[([^\\]]*)\\]\\([^)]*\\)"));
    static const QRegularExpression emphasis(QStringLiteral("\\*\\*|__|~~|`"));
    QString best;
    for (QString line : quote.split(QLatin1Char('\n'))) {
        line.remove(marker);
        line.replace(link, QStringLiteral("\\1"));
        line.remove(emphasis);
        line = line.trimmed();
        if (line.size() > best.size())
            best = line;
    }
    return best.size() >= 3 ? best : QString();
}

enum class CommentPlace { WholeNote, Inline, Detached };

struct CommentThread {
    QJsonObject root;
    QList<QJsonObject> replies;
    CommentAnchor anchor;
    CommentPlace place = CommentPlace::WholeNote;
    bool resolved = false;

    QString id() const { return root.value(QLatin1String("id")).toString(); }
    QString author() const
    {
        return root.value(QLatin1String("author")).toString();
    }
    QString quote() const
    {
        return root.value(QLatin1String("quote")).toString();
    }
};

inline qint64 commentStamp(const QJsonObject &comment, const char *field)
{
    return qint64(comment.value(QLatin1String(field)).toDouble());
}

inline CommentAnchor commentThreadAnchor(const QJsonObject &root,
                                         const QString &markdown)
{
    return findCommentAnchor(
        markdown, root.value(QLatin1String("quote")).toString(),
        root.value(QLatin1String("prefix")).toString(),
        root.value(QLatin1String("suffix")).toString(),
        root.value(QLatin1String("start")).toInt());
}

inline QList<CommentThread> commentThreads(const QJsonArray &comments,
                                           const QString &markdown)
{
    QList<CommentThread> threads;
    QHash<QString, int> index;
    for (const QJsonValue &value : comments) {
        const QJsonObject comment = value.toObject();
        const QString id = comment.value(QLatin1String("id")).toString();
        if (id.isEmpty() ||
            !comment.value(QLatin1String("parentId")).toString().isEmpty())
            continue;
        CommentThread thread;
        thread.root = comment;
        thread.resolved = commentStamp(comment, "resolvedAt") > 0;
        if (!thread.quote().isEmpty()) {
            thread.anchor = commentThreadAnchor(comment, markdown);
            thread.place = thread.anchor.found() ? CommentPlace::Inline
                                                 : CommentPlace::Detached;
        }
        index.insert(id, int(threads.size()));
        threads.append(thread);
    }
    for (const QJsonValue &value : comments) {
        const QJsonObject comment = value.toObject();
        const QString parent = comment.value(QLatin1String("parentId")).toString();
        if (parent.isEmpty() || !index.contains(parent))
            continue;
        threads[index.value(parent)].replies.append(comment);
    }
    for (CommentThread &thread : threads) {
        std::stable_sort(thread.replies.begin(), thread.replies.end(),
                         [](const QJsonObject &a, const QJsonObject &b) {
            return commentStamp(a, "createdAt") < commentStamp(b, "createdAt");
        });
    }
    std::stable_sort(threads.begin(), threads.end(),
                     [](const CommentThread &a, const CommentThread &b) {
        if (a.resolved != b.resolved)
            return !a.resolved;
        if (a.place != b.place)
            return int(a.place) < int(b.place);
        if (a.place == CommentPlace::Inline && a.anchor.start != b.anchor.start)
            return a.anchor.start < b.anchor.start;
        if (a.place == CommentPlace::Inline && a.anchor.end != b.anchor.end)
            return a.anchor.end < b.anchor.end;
        const qint64 aCreated = commentStamp(a.root, "createdAt");
        const qint64 bCreated = commentStamp(b.root, "createdAt");
        if (aCreated != bCreated)
            return aCreated < bCreated;
        return a.id() < b.id();
    });
    return threads;
}

inline int openThreadCount(const QList<CommentThread> &threads)
{
    return int(std::count_if(threads.begin(), threads.end(),
                             [](const CommentThread &thread) {
        return !thread.resolved;
    }));
}

inline QString commentThreadsSignature(const QList<CommentThread> &threads)
{
    QStringList parts;
    for (const CommentThread &thread : threads)
        parts << QStringLiteral("%1:%2:%3")
                     .arg(thread.id())
                     .arg(int(thread.place))
                     .arg(thread.resolved ? 1 : 0);
    return parts.join(QLatin1Char(','));
}

inline QString commentsButtonLabel(int open)
{
    return open > 0 ? QStringLiteral("Comments %1").arg(open)
                    : QStringLiteral("Comments");
}

inline QString resolvedFolderLabel(int resolved)
{
    return QStringLiteral("Resolved (%1)").arg(resolved);
}

inline bool sameAccount(const QString &a, const QString &b)
{
    return !a.trimmed().isEmpty() &&
           a.trimmed().compare(b.trimmed(), Qt::CaseInsensitive) == 0;
}

inline bool canResolveThread(bool canResolve, const QString &threadAuthor,
                             const QString &me)
{
    return canResolve || sameAccount(threadAuthor, me);
}

inline bool canDeleteComment(const QString &author, const QString &me,
                             bool noteOwner)
{
    return noteOwner || sameAccount(author, me);
}

inline int replyCount(const QJsonArray &comments, const QString &threadId)
{
    return int(std::count_if(comments.begin(), comments.end(),
                             [&threadId](const QJsonValue &value) {
        return !threadId.isEmpty() &&
               value.toObject().value(QLatin1String("parentId")).toString() ==
                   threadId;
    }));
}

inline QString deleteCommentQuestion(bool thread, int replies, bool mine,
                                     const QString &author)
{
    const QString whose = mine || author.trimmed().isEmpty()
        ? QString()
        : QStringLiteral(" by @%1").arg(author.trimmed());
    if (!thread)
        return QStringLiteral("Delete this reply%1?").arg(whose);
    if (replies == 1)
        return QStringLiteral("Delete this comment%1 and its reply?").arg(whose);
    if (replies > 1)
        return QStringLiteral("Delete this comment%1 and its %2 replies?")
            .arg(whose)
            .arg(replies);
    return QStringLiteral("Delete this comment%1?").arg(whose);
}

inline const QRegularExpression &mentionPattern()
{
    static const QRegularExpression pattern(
        QStringLiteral("(?<![\\w@])@([a-z](?:[a-z0-9-]{0,61}[a-z0-9])?)"),
        QRegularExpression::CaseInsensitiveOption |
            QRegularExpression::UseUnicodePropertiesOption);
    return pattern;
}

struct MentionSpan {
    int start = 0;
    int length = 0;
    QString name;
};

inline QList<MentionSpan> mentionSpans(const QString &body,
                                       const QStringList &mentions)
{
    QList<MentionSpan> spans;
    if (mentions.isEmpty())
        return spans;
    auto matches = mentionPattern().globalMatch(body);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        const QString name = match.captured(1);
        const bool kept = std::any_of(
            mentions.begin(), mentions.end(),
            [&name](const QString &m) { return sameAccount(m, name); });
        if (kept)
            spans.append({int(match.capturedStart(0)),
                          int(match.capturedLength(0)), name.toLower()});
    }
    return spans;
}

inline QString commentBodyHtml(const QString &body, const QStringList &mentions,
                               const QString &mentionColor)
{
    const auto plain = [](const QString &text) {
        return text.toHtmlEscaped().replace(QLatin1Char('\n'),
                                            QStringLiteral("<br>"));
    };
    QString html;
    int last = 0;
    for (const MentionSpan &span : mentionSpans(body, mentions)) {
        html += plain(body.mid(last, span.start - last));
        html += QStringLiteral("<span style=\"color:%1;font-weight:600\">%2</span>")
                    .arg(mentionColor,
                         body.mid(span.start, span.length).toHtmlEscaped());
        last = span.start + span.length;
    }
    html += plain(body.mid(last));
    return html;
}

struct MentionQuery {
    int at = -1;
    QString prefix;
    bool active() const { return at >= 0; }
};

inline MentionQuery mentionQueryAt(const QString &text, int cursor)
{
    cursor = qBound(0, cursor, int(text.size()));
    int at = -1;
    for (int i = cursor - 1; i >= 0 && cursor - i <= 64; --i) {
        const QChar c = text.at(i);
        if (c == QLatin1Char('@')) {
            at = i;
            break;
        }
        if (!(c.isLetterOrNumber() || c == QLatin1Char('-')))
            return {};
    }
    if (at < 0)
        return {};
    if (at > 0) {
        const QChar before = text.at(at - 1);
        if (before.isLetterOrNumber() || before == QLatin1Char('_') ||
            before == QLatin1Char('@'))
            return {};
    }
    return {at, text.mid(at + 1, cursor - at - 1)};
}

inline QStringList mentionMatches(const QStringList &people,
                                  const QString &prefix, const QString &me,
                                  int limit = 8)
{
    QStringList starts;
    QStringList contains;
    for (const QString &name : people) {
        if (name.isEmpty() || sameAccount(name, me) || starts.contains(name) ||
            contains.contains(name))
            continue;
        if (name.startsWith(prefix, Qt::CaseInsensitive))
            starts << name;
        else if (!prefix.isEmpty() && name.contains(prefix, Qt::CaseInsensitive))
            contains << name;
    }
    return (starts + contains).mid(0, limit);
}

inline QString commentError(const QString &code, const QString &fallback)
{
    if (code == QLatin1String("invalid_comment"))
        return QStringLiteral("Write a comment of at most %1 characters.")
            .arg(kMaxCommentBody);
    if (code == QLatin1String("too_many_comments"))
        return QStringLiteral("This note already has the most comments it can "
                              "hold (1000).");
    if (code == QLatin1String("comment_not_found"))
        return QStringLiteral("That comment was deleted.");
    if (code == QLatin1String("comment_author_required"))
        return QStringLiteral("Only the person who wrote a comment can change "
                              "it.");
    if (code == QLatin1String("resolve_not_allowed"))
        return QStringLiteral("Only the owner, editors and the thread's author "
                              "can resolve it.");
    return fallback;
}

inline QString commentTimeLabel(qint64 stamp, qint64 now)
{
    if (stamp <= 0)
        return QString();
    const qint64 seconds = qMax<qint64>(0, (now - stamp) / 1000);
    if (seconds < 60)
        return QStringLiteral("just now");
    if (seconds < 3600)
        return QStringLiteral("%1 min ago").arg(seconds / 60);
    if (seconds < 86400)
        return QStringLiteral("%1 h ago").arg(seconds / 3600);
    const QDateTime when = QDateTime::fromMSecsSinceEpoch(stamp).toLocalTime();
    const QDateTime today = QDateTime::fromMSecsSinceEpoch(now).toLocalTime();
    return when.toString(when.date().year() == today.date().year()
                             ? QStringLiteral("MMM d")
                             : QStringLiteral("MMM d, yyyy"));
}

struct NotePeer {
    QString clientId;
    QString name;
    QString state;
    QString via;
    bool editing() const { return state == QLatin1String("editing"); }
};

inline QList<NotePeer> notePeers(const QJsonArray &values)
{
    QList<NotePeer> peers;
    for (const QJsonValue &value : values) {
        const QJsonObject peer = value.toObject();
        NotePeer entry{peer.value(QLatin1String("clientId")).toString(),
                       peer.value(QLatin1String("name")).toString().trimmed(),
                       peer.value(QLatin1String("state")).toString(),
                       peer.value(QLatin1String("via")).toString()};
        if (entry.clientId.isEmpty() || entry.state == QLatin1String("left"))
            continue;
        if (entry.name.isEmpty())
            entry.name = QStringLiteral("Collaborator");
        if (!entry.editing())
            entry.state = QStringLiteral("viewing");
        peers.append(entry);
    }
    std::stable_sort(peers.begin(), peers.end(),
                     [](const NotePeer &a, const NotePeer &b) {
        if (a.editing() != b.editing())
            return a.editing();
        const int byName = a.name.compare(b.name, Qt::CaseInsensitive);
        if (byName != 0)
            return byName < 0;
        return a.clientId < b.clientId;
    });
    return peers;
}

inline QString notePeerLabel(const NotePeer &peer)
{
    return QStringLiteral("%1 · %2").arg(
        peer.name, peer.editing() ? QStringLiteral("editing")
                                  : QStringLiteral("viewing"));
}

inline QString notePresenceLine(const QList<NotePeer> &peers)
{
    QStringList editors;
    for (const NotePeer &peer : peers) {
        if (peer.editing())
            editors << peer.name;
    }
    if (editors.size() == 1)
        return QStringLiteral("%1 is editing…").arg(editors.first());
    if (editors.size() == 2)
        return QStringLiteral("%1 and %2 are editing…")
            .arg(editors.at(0), editors.at(1));
    if (editors.size() > 2)
        return QStringLiteral("%1 and %2 others are editing…")
            .arg(editors.first())
            .arg(editors.size() - 1);
    if (peers.size() == 1)
        return QStringLiteral("Live · %1 is viewing").arg(peers.first().name);
    if (peers.size() > 1)
        return QStringLiteral("Live · %1 others here").arg(peers.size());
    return QString();
}

inline QString notePresenceOverflow(int peers)
{
    return peers > kPresenceFaces
        ? QStringLiteral("+%1").arg(peers - kPresenceFaces)
        : QString();
}

inline QString notePresenceState(bool canEdit, qint64 lastTyped, qint64 now)
{
    return canEdit && lastTyped > 0 && now - lastTyped < kPresenceTypingMs
        ? QStringLiteral("editing")
        : QStringLiteral("viewing");
}

inline QString noteInitials(const QString &name)
{
    static const QRegularExpression drop(
        QStringLiteral("[^\\p{L}\\p{N}\\s_-]"),
        QRegularExpression::UseUnicodePropertiesOption);
    static const QRegularExpression split(QStringLiteral("[\\s_-]+"));
    QString cleaned = name;
    cleaned.remove(drop);
    const QStringList parts = cleaned.split(split, Qt::SkipEmptyParts);
    QString initials;
    for (const QString &part : parts) {
        initials += part.left(part.at(0).isHighSurrogate() ? 2 : 1).toUpper();
        if (initials.size() >= 2 || parts.size() == 1)
            break;
    }
    return initials.isEmpty() ? QStringLiteral("?") : initials;
}

inline int noteAvatarIndex(const QString &name, int colors)
{
    if (colors <= 0)
        return 0;
    quint32 hash = 0;
    for (const char32_t point : name.toUcs4())
        hash = hash * 31u + quint32(point);
    return int(hash % quint32(colors));
}

}
