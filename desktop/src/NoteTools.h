#pragma once

#include <QJsonObject>
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
        return isNoteId(id) ? NoteLink{id, QString()} : NoteLink{};
    }
    if (scheme != QLatin1String("https") && scheme != QLatin1String("http"))
        return {};
    const QString host = url.host().toLower();
    if (host.isEmpty())
        return {};
    const QString prefix = QStringLiteral("/dashboard/notes");
    QString id;
    if (path == prefix)
        id = QUrlQuery(url).queryItemValue(QStringLiteral("note"),
                                           QUrl::FullyDecoded);
    else if (path.startsWith(prefix + QLatin1Char('/')))
        id = path.mid(prefix.size() + 1);
    id = id.trimmed().toLower();
    return isNoteId(id) ? NoteLink{id, host} : NoteLink{};
}

inline QString noteIdFromLink(const QString &value,
                              const QStringList &relayHosts)
{
    const NoteLink link = parseNoteLink(value);
    if (link.id.isEmpty())
        return {};
    if (link.host.isEmpty())
        return link.id;
    for (const QString &host : relayHosts) {
        if (!host.isEmpty() &&
            host.compare(link.host, Qt::CaseInsensitive) == 0)
            return link.id;
    }
    return {};
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

}
