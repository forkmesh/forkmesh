#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>

// Which relay a local repository record belongs to.
//
// The desktop keeps one settings array of repository records, but a record is
// only ever served, synced or announced to the relay it belongs to. A record
// stamped with a relay origin belongs there and nowhere else. An unstamped
// (legacy) record is resolved from the repository list each relay itself
// reports: the relay stores its own list, the desktop pulls it, and a relay
// that has never listed the repository does not get it published by accident.
namespace forkmesh::relayscope {

inline QString normalizedOrigin(QUrl url)
{
    if (!url.isValid() || url.host().isEmpty())
        return {};
    if (url.scheme().compare(QStringLiteral("ws"), Qt::CaseInsensitive) == 0)
        url.setScheme(QStringLiteral("http"));
    else if (url.scheme().compare(QStringLiteral("wss"), Qt::CaseInsensitive) == 0)
        url.setScheme(QStringLiteral("https"));
    url.setUserInfo(QString());
    url.setPath(QString());
    url.setQuery(QString());
    url.setFragment(QString());
    const QString scheme = url.scheme().toLower();
    if ((scheme == QLatin1String("http") && url.port(-1) == 80) ||
        (scheme == QLatin1String("https") && url.port(-1) == 443))
        url.setPort(-1);
    return url.toString(QUrl::RemoveUserInfo | QUrl::StripTrailingSlash)
        .toLower();
}

inline QString normalizedOriginText(const QString &url)
{
    thread_local QHash<QString, QString> memo;
    const auto hit = memo.constFind(url);
    if (hit != memo.constEnd())
        return hit.value();
    if (memo.size() >= 256)
        memo.clear();
    const QString origin = normalizedOrigin(QUrl(url));
    memo.insert(url, origin);
    return origin;
}

inline QString repositoryKey(const QString &owner, const QString &name)
{
    const QString cleanOwner = owner.trimmed().toLower();
    const QString cleanName = name.trimmed().toLower();
    if (cleanOwner.isEmpty() || cleanName.isEmpty())
        return {};
    return cleanOwner + QLatin1Char('/') + cleanName;
}

// The part of one relay's catalog response worth remembering: the repositories
// this account owns there, and the ones this machine already has a record for.
// The rest of a public catalog says nothing about what this desktop hosts.
inline QSet<QString> listedKeys(const QJsonArray &catalog,
                                const QString &accountOwner,
                                const QSet<QString> &localKeys)
{
    const QString account = accountOwner.trimmed().toLower();
    QSet<QString> keys;
    for (const QJsonValue &value : catalog) {
        const QJsonObject row = value.toObject();
        const QString owner =
            row.value(QStringLiteral("owner")).toString().trimmed().toLower();
        const QString key = repositoryKey(
            owner, row.value(QStringLiteral("name")).toString());
        if (key.isEmpty())
            continue;
        if ((!account.isEmpty() && owner == account) ||
            localKeys.contains(key))
            keys.insert(key);
    }
    return keys;
}

struct Scope {
    // Every configured relay origin, normalized.
    QStringList configuredOrigins;
    // The historical default relay, normalized.
    QString defaultOrigin;
    // origin -> repository keys that relay last reported. A relay that was
    // never asked has no entry; one that answered with nothing has an empty
    // set, which is a real answer.
    QHash<QString, QSet<QString>> relayLists;
};

// A record is listed under the owner it was added as and under the account that
// publishes it; either name is the same repository.
inline QStringList repositoryKeys(const QString &owner,
                                  const QString &publishingOwner,
                                  const QString &name)
{
    QStringList keys;
    for (const QString &candidate : {owner, publishingOwner}) {
        const QString key = repositoryKey(candidate, name);
        if (!key.isEmpty() && !keys.contains(key))
            keys.append(key);
    }
    return keys;
}

inline bool listsAny(const QSet<QString> &list, const QStringList &keys)
{
    for (const QString &key : keys) {
        if (list.contains(key))
            return true;
    }
    return false;
}

// The origin a record belongs to. `storedOrigin` is the record's stamp and is
// final when present.
inline QString resolveOrigin(const Scope &scope, const QString &storedOrigin,
                             const QString &cloneUrl, const QStringList &keys)
{
    const QString stored = normalizedOriginText(storedOrigin.trimmed());
    if (!stored.isEmpty())
        return stored;

    const QString cloneOrigin = normalizedOriginText(cloneUrl.trimmed());
    if (!cloneOrigin.isEmpty() &&
        scope.configuredOrigins.contains(cloneOrigin))
        return cloneOrigin;

    // Exactly one relay listing the repository settles it. Two relays listing
    // it is what a cross-relay publish leaves behind, so that stays with the
    // historical default rather than following whichever list arrived first.
    QString listedBy;
    int listings = 0;
    for (auto it = scope.relayLists.constBegin();
         it != scope.relayLists.constEnd(); ++it) {
        if (!listsAny(it.value(), keys))
            continue;
        listedBy = it.key();
        ++listings;
    }
    if (listings == 1)
        return listedBy;
    return scope.defaultOrigin;
}

inline bool belongsTo(const Scope &scope, const QString &activeOrigin,
                      const QString &storedOrigin, const QString &cloneUrl,
                      const QStringList &keys)
{
    const QString active = normalizedOriginText(activeOrigin.trimmed());
    return !active.isEmpty() &&
           resolveOrigin(scope, storedOrigin, cloneUrl, keys) == active;
}

// A legacy record may be stamped once its relay is known for certain: every
// configured relay has answered, the active one lists it and no other does.
inline bool shouldStamp(const Scope &scope, const QString &activeOrigin,
                        const QString &storedOrigin, const QStringList &keys)
{
    if (!normalizedOriginText(storedOrigin.trimmed()).isEmpty())
        return false;
    for (const QString &configured : scope.configuredOrigins) {
        if (!scope.relayLists.contains(configured))
            return false;
    }
    const QString active = normalizedOriginText(activeOrigin.trimmed());
    if (active.isEmpty() || !listsAny(scope.relayLists.value(active), keys))
        return false;
    for (auto it = scope.relayLists.constBegin();
         it != scope.relayLists.constEnd(); ++it) {
        if (it.key() != active && listsAny(it.value(), keys))
            return false;
    }
    return true;
}

} // namespace forkmesh::relayscope
