#pragma once

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QLocale>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QUrlQuery>

namespace forkmesh::notes {

inline bool isNoteId(const QString &value)
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{32}$"));
    return pattern.match(value).hasMatch();
}

struct NoteLink {
    QString id;
    QString host;
    QString comment;
    QString page;
    bool isValid() const { return !id.isEmpty() || !page.isEmpty(); }
};

inline NoteLink parseNoteLink(const QString &value)
{
    const QString text = value.trimmed();
    if (text.isEmpty() || text.size() > 512)
        return {};
    const QUrl url(text, QUrl::StrictMode);
    if (!url.isValid() || !url.userInfo().isEmpty())
        return {};
    const QString scheme = url.scheme().toLower();
    QString path = url.path(QUrl::FullyDecoded);
    while (path.size() > 1 && path.endsWith(QLatin1Char('/')))
        path.chop(1);
    if (scheme == QLatin1String("forkmesh")) {
        if (url.host() != QLatin1String("note") || url.port(-1) != -1 ||
            url.hasQuery() || url.hasFragment() ||
            !path.startsWith(QLatin1Char('/')))
            return {};
        const QString id = path.mid(1).toLower();
        return isNoteId(id) ? NoteLink{id, QString(), QString(), QString()}
                            : NoteLink{};
    }
    if (scheme != QLatin1String("https") && scheme != QLatin1String("http"))
        return {};
    const QString host = url.host().toLower();
    if (host.isEmpty())
        return {};
    const QString prefix = QStringLiteral("/dashboard/notes");
    const QUrlQuery query(url);
    QString id;
    if (path == prefix)
        id = query.queryItemValue(QStringLiteral("note"), QUrl::FullyDecoded);
    else if (path.startsWith(prefix + QLatin1Char('/')))
        id = path.mid(prefix.size() + 1);
    id = id.trimmed().toLower();
    QString page = path == prefix
        ? query.queryItemValue(QStringLiteral("page"), QUrl::FullyDecoded)
              .trimmed()
              .toLower()
        : QString();
    if (!isNoteId(page))
        page.clear();
    if (!isNoteId(id)) {
        if (page.isEmpty())
            return {};
        id.clear();
    }
    QString comment = query.queryItemValue(QStringLiteral("comment"),
                                           QUrl::FullyDecoded)
                          .trimmed()
                          .toLower();
    if (!isNoteId(comment) || id.isEmpty())
        comment.clear();
    return NoteLink{id, host, comment, page};
}

inline NoteLink trustedNoteLink(const QString &value,
                                const QStringList &relayHosts)
{
    NoteLink link = parseNoteLink(value);
    if (!link.isValid() || link.host.isEmpty())
        return link;
    for (const QString &host : relayHosts) {
        if (!host.isEmpty() &&
            host.compare(link.host, Qt::CaseInsensitive) == 0)
            return link;
    }
    return {};
}

inline QString noteIdFromLink(const QString &value,
                              const QStringList &relayHosts)
{
    return trustedNoteLink(value, relayHosts).id;
}

inline QUrl noteOrigin(const QUrl &base)
{
    QUrl url = base;
    url.setUserInfo(QString());
    url.setPath(QString());
    url.setQuery(QString());
    url.setFragment(QString());
    return url;
}

inline QString noteAccessLink(const QUrl &base, const QString &id)
{
    if (id.isEmpty())
        return {};
    QUrl url = noteOrigin(base);
    url.setPath(QStringLiteral("/dashboard/notes"));
    url.setQuery(QStringLiteral("note=") + id);
    return url.toString();
}

inline QString pageAccessLink(const QUrl &base, const QString &page,
                              const QString &note = QString())
{
    if (page.isEmpty())
        return {};
    QUrl url = noteOrigin(base);
    url.setPath(QStringLiteral("/dashboard/notes"));
    url.setQuery(note.isEmpty() ? QStringLiteral("page=") + page
                                : QStringLiteral("page=%1&note=%2").arg(page, note));
    return url.toString();
}

inline QString notePublicLink(const QUrl &base, const QString &id)
{
    if (id.isEmpty())
        return {};
    QUrl url = noteOrigin(base);
    url.setPath(QStringLiteral("/notes/") + id);
    return url.toString();
}

struct ConversationRef {
    QString owner;
    QString repo;
    QString kind;
    int number = 0;
    bool isValid() const { return number > 0; }
};

inline QString conversationKind(const QString &word)
{
    const QString value = word.toLower();
    if (value == QLatin1String("issue") || value == QLatin1String("issues"))
        return QStringLiteral("issue");
    if (value == QLatin1String("pull") || value == QLatin1String("pulls"))
        return QStringLiteral("pull");
    if (value == QLatin1String("discussion") ||
        value == QLatin1String("discussions"))
        return QStringLiteral("discussion");
    return {};
}

inline ConversationRef conversationRef(const QString &owner,
                                       const QString &repo,
                                       const QString &kind,
                                       const QString &number)
{
    bool ok = false;
    const qlonglong value = number.toLongLong(&ok);
    if (!ok || value < 1 || value > 999999999 || kind.isEmpty())
        return {};
    return {owner, repo, kind, static_cast<int>(value)};
}

inline ConversationRef parseConversationRef(const QString &value,
                                            const QString &fallbackKind)
{
    const QString text = value.trimmed();
    static const QRegularExpression shorthand(
        QStringLiteral("^([\\w.-]+)/([\\w.-]+)#(\\d+)$"));
    const QRegularExpressionMatch shortMatch = shorthand.match(text);
    if (shortMatch.hasMatch())
        return conversationRef(shortMatch.captured(1), shortMatch.captured(2),
                               conversationKind(fallbackKind),
                               shortMatch.captured(3));
    const QUrl url = text.startsWith(QLatin1Char('/'))
        ? QUrl(QStringLiteral("https://relay.invalid") + text)
        : QUrl(text);
    if (!url.isValid())
        return {};
    const QStringList parts =
        url.path(QUrl::FullyDecoded).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (url.scheme() == QLatin1String("forkmesh")) {
        if (parts.size() != 3)
            return {};
        const QString kind = conversationKind(url.host());
        if (kind == QLatin1String("discussion"))
            return {};
        return conversationRef(parts.at(0), parts.at(1), kind, parts.at(2));
    }
    if (url.scheme() != QLatin1String("https") &&
        url.scheme() != QLatin1String("http"))
        return {};
    static const QRegularExpression segment(QStringLiteral("^[\\w.-]+$"));
    if (parts.size() >= 4 && segment.match(parts.at(0)).hasMatch() &&
        segment.match(parts.at(1)).hasMatch()) {
        const QString word = parts.at(2);
        if (word == QLatin1String("issues") || word == QLatin1String("pull") ||
            word == QLatin1String("pulls") ||
            word == QLatin1String("discussions"))
            return conversationRef(parts.at(0), parts.at(1),
                                   conversationKind(word), parts.at(3));
        return {};
    }
    if (parts.size() == 2 && segment.match(parts.at(0)).hasMatch() &&
        segment.match(parts.at(1)).hasMatch()) {
        return conversationRef(
            parts.at(0), parts.at(1),
            conversationKind(
                QUrlQuery(url).queryItemValue(QStringLiteral("tab"))),
            url.fragment());
    }
    return {};
}

inline QString conversationLabel(const QJsonObject &link)
{
    const QString kind = link.value(QLatin1String("kind")).toString();
    const QString word = kind == QLatin1String("pull")
        ? QStringLiteral("Pull request")
        : kind == QLatin1String("discussion") ? QStringLiteral("Discussion")
                                              : QStringLiteral("Issue");
    return QStringLiteral("%1 %2/%3#%4")
        .arg(word, link.value(QLatin1String("owner")).toString(),
             link.value(QLatin1String("repo")).toString())
        .arg(link.value(QLatin1String("number")).toInt());
}

inline QString conversationHref(const QJsonObject &link, const QUrl &base)
{
    const QString kind = link.value(QLatin1String("kind")).toString();
    const QString owner = link.value(QLatin1String("owner")).toString();
    const QString repo = link.value(QLatin1String("repo")).toString();
    const int number = link.value(QLatin1String("number")).toInt();
    if (kind == QLatin1String("issue") || kind == QLatin1String("pull"))
        return QStringLiteral("forkmesh://%1/%2/%3/%4")
            .arg(kind, owner, repo)
            .arg(number);
    QUrl url = noteOrigin(base);
    url.setPath(QStringLiteral("/%1/%2/discussions/%3")
                    .arg(owner, repo)
                    .arg(number));
    return url.toString();
}

inline bool sameConversation(const QJsonObject &a, const QJsonObject &b)
{
    return a.value(QLatin1String("owner")).toString().compare(
               b.value(QLatin1String("owner")).toString(),
               Qt::CaseInsensitive) == 0 &&
           a.value(QLatin1String("repo")).toString() ==
               b.value(QLatin1String("repo")).toString() &&
           a.value(QLatin1String("kind")).toString() ==
               b.value(QLatin1String("kind")).toString() &&
           a.value(QLatin1String("number")).toInt() ==
               b.value(QLatin1String("number")).toInt();
}

inline QString exportFileName(const QString &title)
{
    static const QRegularExpression unsafe(QStringLiteral("[^A-Za-z0-9_.-]+"));
    static const QRegularExpression edges(QStringLiteral("^-+|-+$"));
    QString slug = title;
    slug.replace(unsafe, QStringLiteral("-"));
    slug.remove(edges);
    slug = slug.toLower();
    if (slug.isEmpty())
        slug = QStringLiteral("note");
    return slug + QStringLiteral(".md");
}

inline QString exportMarkdown(const QString &title, const QString &markdown)
{
    const QString heading = title.trimmed().isEmpty()
        ? QStringLiteral("Untitled note") : title.trimmed();
    return QStringLiteral("# ") + heading + QStringLiteral("\n\n") + markdown;
}

inline QString shareQuery(const QString &text)
{
    QString query = text.trimmed();
    while (query.startsWith(QLatin1Char('@')))
        query.remove(0, 1);
    return query.trimmed().toLower();
}

inline bool shareQueryWorthAsking(const QString &query)
{
    return !query.isEmpty() && query.size() <= 63;
}

inline QString sharedNotice(const QString &name)
{
    return QStringLiteral("Shared with %1. Copy the link below to send it.")
        .arg(name);
}

inline QString shareError(const QString &code, const QString &name,
                          const QString &fallback)
{
    if (code == QLatin1String("principal_not_found"))
        return QStringLiteral("No account or organization is called %1.")
            .arg(name);
    if (code == QLatin1String("invalid_principal"))
        return QStringLiteral("Type the name of an account or organization.");
    if (code == QLatin1String("note_not_found"))
        return QStringLiteral("Only the note's owner can change who it is "
                              "shared with.");
    return fallback;
}

inline QString principalTypeLabel(const QString &type)
{
    return type == QLatin1String("organization") ? QStringLiteral("Organization")
                                                 : QStringLiteral("User");
}

inline const QStringList &noteFilters()
{
    static const QStringList filters{
        QStringLiteral("all"), QStringLiteral("mine"), QStringLiteral("shared"),
        QStringLiteral("public")};
    return filters;
}

inline QString noteFilterLabel(const QString &filter, int count)
{
    const QString word = filter == QLatin1String("mine") ? QStringLiteral("Mine")
        : filter == QLatin1String("shared") ? QStringLiteral("Shared")
        : filter == QLatin1String("public") ? QStringLiteral("Public")
                                            : QStringLiteral("All");
    return QStringLiteral("%1 %2").arg(word).arg(count);
}

inline bool noteMatchesFilter(const QString &filter, const QString &role,
                              const QString &visibility)
{
    if (filter == QLatin1String("mine"))
        return role == QLatin1String("owner");
    if (filter == QLatin1String("shared"))
        return role != QLatin1String("owner");
    if (filter == QLatin1String("public"))
        return visibility == QLatin1String("public");
    return true;
}

inline bool noteMatchesSearch(const QString &query, const QString &title,
                              const QString &markdown)
{
    const QString needle = query.trimmed();
    if (needle.isEmpty())
        return true;
    return title.contains(needle, Qt::CaseInsensitive) ||
           markdown.contains(needle, Qt::CaseInsensitive);
}

inline QString noteSearchQuery(const QString &text)
{
    return text.trimmed().toLower();
}

inline bool noteSearchWorthAsking(const QString &query)
{
    return !query.isEmpty() && query.size() <= 200;
}

inline QString noteEmptyListText(const QString &filter, const QString &query)
{
    if (!query.trimmed().isEmpty())
        return QStringLiteral("No notes match “%1”.").arg(query.trimmed());
    if (filter == QLatin1String("mine"))
        return QStringLiteral("You have no notes yet.");
    if (filter == QLatin1String("shared"))
        return QStringLiteral("Nobody has shared a note with you yet.");
    if (filter == QLatin1String("public"))
        return QStringLiteral("You haven't published any notes.");
    return QStringLiteral("No notes yet. Create one to get started.");
}

inline bool noteCanEdit(const QString &role)
{
    return role == QLatin1String("owner") || role == QLatin1String("editor");
}

inline bool isVersionConflict(const QJsonObject &payload)
{
    return payload.value(QLatin1String("error")).toString() ==
               QLatin1String("version_conflict") &&
           payload.value(QLatin1String("note")).isObject();
}

inline QString noteEditStamp(qint64 stamp, const QDateTime &now)
{
    if (stamp <= 0)
        return QString();
    const QLocale english = QLocale::c();
    const QDateTime when = QDateTime::fromMSecsSinceEpoch(stamp).toLocalTime();
    const QDate today = now.toLocalTime().date();
    const QString time = english.toString(when.time(), QStringLiteral("h:mm AP"));
    if (when.date() == today)
        return QStringLiteral("Today %1").arg(time);
    if (when.date() == today.addDays(-1))
        return QStringLiteral("Yesterday %1").arg(time);
    if (when.date().year() == today.year())
        return QStringLiteral("%1, %2")
            .arg(english.toString(when.date(), QStringLiteral("MMM d")), time);
    return english.toString(when.date(), QStringLiteral("MMM d, yyyy"));
}

inline QString noteEditFullTime(qint64 stamp)
{
    if (stamp <= 0)
        return QString();
    const QDateTime when = QDateTime::fromMSecsSinceEpoch(stamp).toLocalTime();
    return QLocale::c().toString(when, QStringLiteral("dddd, MMMM d, yyyy, h:mm AP"));
}

inline QString noteLastEditor(const QString &editor, const QString &me)
{
    const QString name = editor.trimmed();
    if (name.isEmpty() || name.compare(me.trimmed(), Qt::CaseInsensitive) == 0)
        return QString();
    return name;
}

inline QString noteLastEditLabel(qint64 stamp, const QString &editor,
                                 const QString &me, const QDateTime &now)
{
    const QString when = noteEditStamp(stamp, now);
    const QString other = noteLastEditor(editor, me);
    if (when.isEmpty())
        return other.isEmpty() ? QString() : QStringLiteral("by %1").arg(other);
    return other.isEmpty() ? when : QStringLiteral("%1 · by %2").arg(when, other);
}

inline QString noteLastEditTooltip(qint64 stamp, const QString &editor,
                                   const QString &me)
{
    const QString when = noteEditFullTime(stamp);
    if (when.isEmpty())
        return QString();
    const QString other = noteLastEditor(editor, me);
    return other.isEmpty() ? QStringLiteral("Last edited %1").arg(when)
                           : QStringLiteral("Last edited %1 by %2").arg(when, other);
}

inline QString noteStorageBadge(const QString &mode)
{
    if (mode == QLatin1String("cloud"))
        return QStringLiteral("Cloud");
    if (mode == QLatin1String("both"))
        return QStringLiteral("Local + cloud");
    return QStringLiteral("Local");
}

inline QString noteVisibilityBadge(const QJsonObject &status, bool synced)
{
    if (!synced)
        return QStringLiteral("Private");
    if (status.value(QLatin1String("visibility")).toString() ==
        QLatin1String("public"))
        return QStringLiteral("Public");
    return status.value(QLatin1String("shares")).toArray().isEmpty()
        ? QStringLiteral("Private")
        : QStringLiteral("Shared");
}

struct NoteSharePerson {
    QString name;
    bool organization = false;
};

constexpr int kNoteRowAvatars = 3;

inline QList<NoteSharePerson> noteSharePeople(const QJsonArray &shares)
{
    QList<NoteSharePerson> people;
    for (const QJsonValue &value : shares) {
        const QJsonObject share = value.toObject();
        const QString name = share.value(QLatin1String("name")).toString().trimmed();
        if (name.isEmpty())
            continue;
        people.append({name, share.value(QLatin1String("type")).toString() ==
                                 QLatin1String("organization")});
    }
    return people;
}

inline QString noteShareOverflow(int people)
{
    return people > kNoteRowAvatars
        ? QStringLiteral("+%1").arg(people - kNoteRowAvatars)
        : QString();
}

inline bool noteUnread(const QJsonObject &cloud, const QString &mode)
{
    if (mode == QLatin1String("local") || cloud.isEmpty())
        return false;
    return cloud.value(QLatin1String("unread")).toBool() ||
           cloud.value(QLatin1String("unreadComments")).toInt() > 0;
}

inline QJsonObject noteMarkedRead(QJsonObject cloud, qint64 now)
{
    cloud.insert(QStringLiteral("unread"), false);
    cloud.insert(QStringLiteral("unreadComments"), 0);
    cloud.insert(QStringLiteral("myLastViewedAt"), double(now));
    return cloud;
}

inline QJsonObject noteKeepFreshness(QJsonObject cloud,
                                     const QJsonObject &previous,
                                     const QString &editor)
{
    for (const char *field : {"unread", "lastEditedBy", "myLastViewedAt",
                              "unreadComments", "openComments"}) {
        const QLatin1String key(field);
        if (!cloud.contains(key) && previous.contains(key))
            cloud.insert(key, previous.value(key));
    }
    if (!editor.trimmed().isEmpty()) {
        cloud.insert(QStringLiteral("lastEditedBy"), editor.trimmed());
        cloud.insert(QStringLiteral("unread"), false);
    }
    return cloud;
}

inline QString noteRoleLine(const QString &role)
{
    if (role == QLatin1String("owner") || role.isEmpty())
        return QStringLiteral("You own this note");
    if (role == QLatin1String("editor"))
        return QStringLiteral("You can edit");
    return QStringLiteral("View only");
}

inline QString noteSharedLine(const QJsonArray &shares)
{
    QStringList people;
    for (const QJsonValue &value : shares) {
        const QJsonObject share = value.toObject();
        const QString name = share.value(QLatin1String("name")).toString().trimmed();
        if (name.isEmpty())
            continue;
        const QString access =
            share.value(QLatin1String("role")).toString() == QLatin1String("editor")
            ? QStringLiteral("can edit")
            : QStringLiteral("can view");
        people << (share.value(QLatin1String("type")).toString() ==
                           QLatin1String("organization")
                       ? QStringLiteral("%1 (organization, %2)").arg(name, access)
                       : QStringLiteral("%1 (%2)").arg(name, access));
    }
    return people.isEmpty()
        ? QStringLiteral("Not shared")
        : QStringLiteral("Shared with %1").arg(people.join(QStringLiteral(", ")));
}

inline QStringList noteAccessHeader(const QString &role, const QJsonArray &shares,
                                    const QString &visibility,
                                    const QString &storage, qint64 stamp,
                                    const QString &editor, const QString &me)
{
    QStringList lines{noteRoleLine(role), noteSharedLine(shares),
                      QStringLiteral("%1 · %2").arg(visibility, storage)};
    const QString edited = noteLastEditTooltip(stamp, editor, me);
    if (!edited.isEmpty())
        lines << edited;
    return lines;
}

inline QString noteCommentCountLabel(int open)
{
    return open == 1 ? QStringLiteral("1 open comment")
                     : QStringLiteral("%1 open comments").arg(open);
}

}
