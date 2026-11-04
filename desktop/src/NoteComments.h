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
constexpr int kPresenceFollowMs = 1000;
constexpr int kPresenceChatMs = 2000;
constexpr int kMaxChatBody = 2000;
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

inline QStringList noteQuickReactions()
{
    return {QStringLiteral("\U0001F44D"), QStringLiteral("❤️"),
            QStringLiteral("\U0001F602"), QStringLiteral("\U0001F389"),
            QStringLiteral("\U0001F62E"), QStringLiteral("\U0001F440")};
}

struct NoteReaction {
    QString id;
    QString author;
    QString emoji;
    QString quote;
    QString prefix;
    QString suffix;
    int start = 0;
    int end = 0;
    qint64 createdAt = 0;
    bool wholeNote() const { return quote.isEmpty(); }
};

inline QList<NoteReaction> noteReactions(const QJsonArray &values)
{
    QList<NoteReaction> reactions;
    for (const QJsonValue &value : values) {
        const QJsonObject item = value.toObject();
        NoteReaction reaction;
        reaction.id = item.value(QLatin1String("id")).toString();
        reaction.emoji = item.value(QLatin1String("emoji")).toString();
        if (reaction.id.isEmpty() || reaction.emoji.isEmpty())
            continue;
        reaction.author = item.value(QLatin1String("author")).toString().trimmed();
        if (reaction.author.isEmpty())
            reaction.author = QStringLiteral("Someone");
        reaction.quote = item.value(QLatin1String("quote")).toString();
        reaction.prefix = item.value(QLatin1String("prefix")).toString();
        reaction.suffix = item.value(QLatin1String("suffix")).toString();
        reaction.start = item.value(QLatin1String("start")).toInt();
        reaction.end = item.value(QLatin1String("end")).toInt();
        reaction.createdAt = qint64(item.value(QLatin1String("createdAt")).toDouble());
        reactions.append(reaction);
    }
    std::stable_sort(reactions.begin(), reactions.end(),
                     [](const NoteReaction &a, const NoteReaction &b) {
        return a.createdAt < b.createdAt;
    });
    return reactions;
}

struct NoteReactionChip {
    QString emoji;
    QStringList people;
    QString mineId;
    int count() const { return int(people.size()); }
    bool mine() const { return !mineId.isEmpty(); }
};

struct NoteReactionGroup {
    NoteReaction first;
    CommentAnchor anchor;
    QList<NoteReactionChip> chips;
    bool wholeNote() const { return first.wholeNote(); }
    QString quote() const { return first.quote; }
    QString key() const
    {
        return wholeNote() ? QStringLiteral("note")
                           : QStringLiteral("%1:%2").arg(anchor.start).arg(anchor.end);
    }
};

inline QList<NoteReactionGroup> noteReactionGroups(const QList<NoteReaction> &reactions,
                                                   const QString &markdown,
                                                   const QString &me)
{
    QList<NoteReactionGroup> groups;
    QHash<QString, int> index;
    for (const NoteReaction &reaction : reactions) {
        NoteReactionGroup probe;
        probe.first = reaction;
        if (!reaction.wholeNote()) {
            probe.anchor = findCommentAnchor(markdown, reaction.quote, reaction.prefix,
                                             reaction.suffix, reaction.start);
            if (!probe.anchor.found())
                continue;
        }
        const QString key = probe.key();
        int at = index.value(key, -1);
        if (at < 0) {
            at = int(groups.size());
            index.insert(key, at);
            groups.append(probe);
        }
        QList<NoteReactionChip> &chips = groups[at].chips;
        auto chip = std::find_if(chips.begin(), chips.end(),
                                 [&reaction](const NoteReactionChip &item) {
            return item.emoji == reaction.emoji;
        });
        if (chip == chips.end()) {
            chips.append(NoteReactionChip{reaction.emoji, {}, {}});
            chip = chips.end() - 1;
        }
        const bool known = std::any_of(chip->people.cbegin(), chip->people.cend(),
                                       [&reaction](const QString &name) {
            return sameAccount(name, reaction.author);
        });
        if (!known)
            chip->people << reaction.author;
        if (chip->mineId.isEmpty() && sameAccount(reaction.author, me))
            chip->mineId = reaction.id;
    }
    std::stable_sort(groups.begin(), groups.end(),
                     [](const NoteReactionGroup &a, const NoteReactionGroup &b) {
        if (a.wholeNote() != b.wholeNote())
            return a.wholeNote();
        if (a.anchor.start != b.anchor.start)
            return a.anchor.start < b.anchor.start;
        return a.anchor.end < b.anchor.end;
    });
    return groups;
}

inline QString noteReactionGroupsSignature(const QList<NoteReactionGroup> &groups)
{
    QStringList parts;
    for (const NoteReactionGroup &group : groups) {
        QStringList chips;
        for (const NoteReactionChip &chip : group.chips)
            chips << QStringLiteral("%1%2%3").arg(chip.emoji).arg(chip.count())
                         .arg(chip.mine() ? QStringLiteral("*") : QString());
        parts << group.key() + QLatin1Char('=') + chips.join(QLatin1Char('/'));
    }
    return parts.join(QLatin1Char(','));
}

inline QString noteReactionChipLabel(const NoteReactionChip &chip)
{
    return QStringLiteral("%1 %2").arg(chip.emoji).arg(chip.count());
}

inline QString noteReactionChipTip(const NoteReactionChip &chip)
{
    return QStringLiteral("%1 reacted with %2")
        .arg(chip.people.join(QStringLiteral(", ")), chip.emoji);
}

inline QString noteReactionGroupTip(const NoteReactionGroup &group)
{
    QStringList lines;
    for (const NoteReactionChip &chip : group.chips)
        lines << noteReactionChipTip(chip);
    return lines.join(QLatin1Char('\n'));
}

inline QJsonObject noteReactionRequest(const QString &emoji, const CommentQuote &quote)
{
    QJsonObject body = commentQuoteFields(quote);
    body.insert(QStringLiteral("emoji"), emoji);
    return body;
}

inline QJsonObject noteReactionRequest(const QString &emoji, const NoteReaction &like)
{
    QJsonObject body{{QStringLiteral("emoji"), emoji}};
    if (like.wholeNote())
        return body;
    body.insert(QStringLiteral("quote"), like.quote);
    body.insert(QStringLiteral("prefix"), like.prefix);
    body.insert(QStringLiteral("suffix"), like.suffix);
    body.insert(QStringLiteral("start"), like.start);
    body.insert(QStringLiteral("end"), like.end);
    return body;
}

struct NoteReactionToggle {
    QByteArray method;
    QString reactionId;
    QJsonObject body;
};

inline NoteReactionToggle noteReactionToggle(const NoteReactionGroup &group,
                                             const NoteReactionChip &chip)
{
    if (chip.mine())
        return {QByteArrayLiteral("DELETE"), chip.mineId, {}};
    return {QByteArrayLiteral("POST"), QString(),
            noteReactionRequest(chip.emoji, group.first)};
}

inline QString noteReactionError(const QString &code, const QString &fallback)
{
    if (code == QLatin1String("invalid_reaction"))
        return QStringLiteral("React with a single emoji.");
    if (code == QLatin1String("too_many_reactions"))
        return QStringLiteral("This note already has the most reactions it can "
                              "hold (2000).");
    if (code == QLatin1String("reaction_not_found"))
        return QStringLiteral("That reaction was already removed.");
    if (code == QLatin1String("reaction_author_required"))
        return QStringLiteral("Only the person who reacted, or the note's owner, "
                              "can remove a reaction.");
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
    int line = 0;
    int column = 0;
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
        entry.line = qMax(0, peer.value(QLatin1String("line")).toInt());
        entry.column = qMax(0, peer.value(QLatin1String("column")).toInt());
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

inline NotePeer noteFollowedPeer(const QList<NotePeer> &peers, const QString &name)
{
    NotePeer found;
    for (const NotePeer &peer : peers) {
        if (peer.name != name || peer.line <= 0)
            continue;
        if (found.clientId.isEmpty() || (peer.editing() && !found.editing()))
            found = peer;
    }
    return found;
}

inline bool notePeerHere(const QList<NotePeer> &peers, const QString &name)
{
    return std::any_of(peers.cbegin(), peers.cend(),
                       [&name](const NotePeer &peer) { return peer.name == name; });
}

struct NoteEditingLine {
    int line = 0;
    QList<NotePeer> peers;
};

inline QList<NoteEditingLine> noteEditingLines(const QList<NotePeer> &peers,
                                               const QString &selfClientId,
                                               int lineCount)
{
    QList<NoteEditingLine> lines;
    for (const NotePeer &peer : peers) {
        if (!peer.editing() || peer.line <= 0 || peer.clientId == selfClientId)
            continue;
        const int line = lineCount > 0 ? qMin(peer.line, lineCount) : peer.line;
        auto found = std::find_if(lines.begin(), lines.end(),
                                  [line](const NoteEditingLine &entry) {
            return entry.line == line;
        });
        if (found == lines.end()) {
            lines.append(NoteEditingLine{line, {}});
            found = lines.end() - 1;
        }
        found->peers.append(peer);
    }
    std::stable_sort(lines.begin(), lines.end(),
                     [](const NoteEditingLine &a, const NoteEditingLine &b) {
        return a.line < b.line;
    });
    return lines;
}

inline QString noteEditingLineTip(const NoteEditingLine &line)
{
    QStringList names;
    for (const NotePeer &peer : line.peers)
        names << peer.name;
    if (names.size() == 1)
        return QStringLiteral("%1 is editing this line").arg(names.first());
    const QString last = names.takeLast();
    return QStringLiteral("%1 and %2 are editing this line")
        .arg(names.join(QStringLiteral(", ")), last);
}

inline QString noteEditingSignature(const QList<NoteEditingLine> &lines)
{
    QStringList parts;
    for (const NoteEditingLine &line : lines)
        for (const NotePeer &peer : line.peers)
            parts << QStringLiteral("%1@%2:%3").arg(peer.clientId, peer.name).arg(line.line);
    return parts.join(QLatin1Char(','));
}

struct NoteChatMessage {
    QString id;
    QString author;
    QString body;
    qint64 createdAt = 0;
};

inline QList<NoteChatMessage> noteChatMessages(const QJsonArray &values)
{
    QList<NoteChatMessage> messages;
    for (const QJsonValue &value : values) {
        const QJsonObject message = value.toObject();
        NoteChatMessage entry{message.value(QLatin1String("id")).toString(),
                              message.value(QLatin1String("author")).toString(),
                              message.value(QLatin1String("body")).toString(),
                              qint64(message.value(QLatin1String("createdAt")).toDouble())};
        if (entry.id.isEmpty())
            continue;
        if (entry.author.isEmpty())
            entry.author = QStringLiteral("Someone");
        messages.append(entry);
    }
    return messages;
}

inline int noteChatUnread(const QList<NoteChatMessage> &messages, const QString &me,
                          qint64 seenAt)
{
    return int(std::count_if(messages.cbegin(), messages.cend(),
                             [&](const NoteChatMessage &message) {
        return message.createdAt > seenAt &&
               message.author.compare(me, Qt::CaseInsensitive) != 0;
    }));
}

inline QString noteChatButtonLabel(int unread)
{
    return unread > 0 ? QStringLiteral("Chat (%1)").arg(unread > 99 ? QStringLiteral("99+")
                                                                     : QString::number(unread))
                      : QStringLiteral("Chat");
}

constexpr qint64 kNoteEditGroupMs = 30LL * 60 * 1000;

enum class NoteSideTab { Comments, Chat, Activity, History };

struct NoteActivityEntry {
    QString kind;
    QString actor;
    qint64 at = 0;
    int count = 1;
    int version = 0;
    int fromVersion = 0;
    int readers = 0;
    QJsonObject detail;
};

inline QList<NoteActivityEntry> noteActivityEntries(const QJsonObject &payload)
{
    QList<NoteActivityEntry> events;
    for (const QJsonValue &value : payload.value(QLatin1String("events")).toArray()) {
        const QJsonObject event = value.toObject();
        NoteActivityEntry entry;
        entry.kind = event.value(QLatin1String("kind")).toString();
        if (entry.kind.isEmpty())
            continue;
        entry.actor = event.value(QLatin1String("actor")).toString();
        entry.at = qint64(event.value(QLatin1String("at")).toDouble());
        entry.count = qMax(1, event.value(QLatin1String("count")).toInt(1));
        entry.version = event.value(QLatin1String("version")).toInt();
        entry.fromVersion = entry.version;
        entry.detail = event.value(QLatin1String("detail")).toObject();
        events.append(entry);
    }
    const QJsonObject counts = payload.value(QLatin1String("counts")).toObject();
    const int webViews = counts.value(QLatin1String("webViews")).toInt();
    if (webViews > 0) {
        NoteActivityEntry web;
        web.kind = QStringLiteral("web");
        web.at = qint64(counts.value(QLatin1String("webLastViewedAt")).toDouble());
        web.count = webViews;
        web.readers = counts.value(QLatin1String("webReaders")).toInt();
        events.append(web);
    }
    std::stable_sort(events.begin(), events.end(),
                     [](const NoteActivityEntry &a, const NoteActivityEntry &b) {
        return a.at > b.at;
    });
    QList<NoteActivityEntry> grouped;
    qint64 oldestAt = 0;
    for (const NoteActivityEntry &entry : std::as_const(events)) {
        if (entry.kind == QLatin1String("edit") && !grouped.isEmpty() &&
            grouped.constLast().kind == QLatin1String("edit") &&
            grouped.constLast().actor == entry.actor &&
            oldestAt - entry.at <= kNoteEditGroupMs) {
            NoteActivityEntry &last = grouped.last();
            last.fromVersion = entry.version;
            ++last.count;
            oldestAt = entry.at;
            continue;
        }
        NoteActivityEntry copy = entry;
        if (copy.kind == QLatin1String("edit"))
            copy.count = 1;
        oldestAt = entry.at;
        grouped.append(copy);
    }
    return grouped;
}

inline QString noteActivityText(const NoteActivityEntry &entry)
{
    const QString name = entry.detail.value(QLatin1String("name")).toString(
        QStringLiteral("someone"));
    const QString role = entry.detail.value(QLatin1String("role")).toString()
            == QLatin1String("editor")
        ? QStringLiteral("can edit")
        : QStringLiteral("can view");
    const QString ref = QStringLiteral("%1/%2#%3")
        .arg(entry.detail.value(QLatin1String("owner")).toString(),
             entry.detail.value(QLatin1String("repo")).toString())
        .arg(entry.detail.value(QLatin1String("number")).toInt());
    const QString &kind = entry.kind;
    if (kind == QLatin1String("view"))
        return entry.count > 1 ? QStringLiteral("viewed it %1 times").arg(entry.count)
                               : QStringLiteral("viewed it");
    if (kind == QLatin1String("web"))
        return QStringLiteral("%1 of the published page%2")
            .arg(entry.count == 1 ? QStringLiteral("1 read")
                                  : QStringLiteral("%1 reads").arg(entry.count),
                 entry.readers > 1 ? QStringLiteral(" by %1 readers").arg(entry.readers)
                                   : QString());
    if (kind == QLatin1String("edit"))
        return entry.count > 1
            ? QStringLiteral("edited it · v%1–v%2, %3 saves")
                  .arg(entry.fromVersion).arg(entry.version).arg(entry.count)
            : QStringLiteral("edited it · v%1").arg(entry.version);
    if (kind == QLatin1String("create"))
        return QStringLiteral("created the note");
    if (kind == QLatin1String("restore")) {
        const int as = entry.detail.value(QLatin1String("as")).toInt();
        return QStringLiteral("restored version %1%2")
            .arg(entry.detail.value(QLatin1String("version")).toInt())
            .arg(as ? QStringLiteral(" as v%1").arg(as) : QString());
    }
    if (kind == QLatin1String("share"))
        return QStringLiteral("shared it with %1 · %2").arg(name, role);
    if (kind == QLatin1String("role"))
        return QStringLiteral("changed %1 to %2").arg(name, role);
    if (kind == QLatin1String("unshare"))
        return QStringLiteral("stopped sharing with %1").arg(name);
    if (kind == QLatin1String("publish"))
        return QStringLiteral("published it to the web");
    if (kind == QLatin1String("unpublish"))
        return QStringLiteral("unpublished it");
    if (kind == QLatin1String("link"))
        return QStringLiteral("linked %1").arg(ref);
    if (kind == QLatin1String("unlink"))
        return QStringLiteral("unlinked %1").arg(ref);
    if (kind == QLatin1String("export"))
        return QStringLiteral("downloaded the Markdown");
    if (kind == QLatin1String("copy-link"))
        return QStringLiteral("copied the link");
    if (kind == QLatin1String("task"))
        return QStringLiteral("turned it into a task");
    if (kind == QLatin1String("issue"))
        return entry.detail.contains(QLatin1String("repo"))
            ? QStringLiteral("filed issue %1 from it").arg(ref)
            : QStringLiteral("filed an issue from it");
    if (kind == QLatin1String("comment"))
        return entry.detail.value(QLatin1String("reply")).toBool()
            ? QStringLiteral("replied to a comment")
            : QStringLiteral("commented");
    if (kind == QLatin1String("resolve"))
        return QStringLiteral("resolved a comment");
    if (kind == QLatin1String("reopen"))
        return QStringLiteral("reopened a comment");
    return kind;
}

inline QString noteActivityActor(const NoteActivityEntry &entry, const QString &me)
{
    if (entry.kind == QLatin1String("web"))
        return QString();
    if (entry.actor.isEmpty())
        return QStringLiteral("Someone");
    return entry.actor.compare(me, Qt::CaseInsensitive) == 0 ? QStringLiteral("You")
                                                             : entry.actor;
}

struct NoteVersionEntry {
    int version = 0;
    qint64 createdAt = 0;
    QString author;
};

inline QList<NoteVersionEntry> noteVersionEntries(const QJsonArray &values)
{
    QList<NoteVersionEntry> versions;
    for (const QJsonValue &value : values) {
        const QJsonObject item = value.toObject();
        const int number = item.value(QLatin1String("version")).toInt();
        if (number <= 0)
            continue;
        versions.append({number, qint64(item.value(QLatin1String("createdAt")).toDouble()),
                         item.value(QLatin1String("author")).toString()});
    }
    return versions;
}

inline QString noteOpenInLiveKey()
{
    return QStringLiteral("notes/openInLive");
}

enum class NoteOpenView { Write, Preview, Live };

inline NoteOpenView noteOpenView(bool preferLive, bool canEdit, bool empty)
{
    if (preferLive && canEdit)
        return NoteOpenView::Live;
    return empty ? NoteOpenView::Write : NoteOpenView::Preview;
}

inline int notePresencePollMs(bool following, bool chatOpen)
{
    return following ? kPresenceFollowMs : chatOpen ? kPresenceChatMs : kPresencePollMs;
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
