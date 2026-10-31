#ifndef FORKMESH_ACCOUNT_SESSION_CACHE_H
#define FORKMESH_ACCOUNT_SESSION_CACHE_H

#include "AccountCapability.h"

#include <QHash>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <utility>

namespace forkmesh::accounts {

struct CachedSession
{
    QString account;
    QString instanceUrl;
    QString token;
    QString desktopPublicKey;
    bool desktopCapable = false;

    void clearSensitive()
    {
        token.fill(QChar(u'\0'));
        token.clear();
        desktopPublicKey.clear();
        desktopCapable = false;
    }
};

class SessionCache
{
public:
    SessionCache() = default;
    SessionCache(const SessionCache &) = delete;
    SessionCache &operator=(const SessionCache &) = delete;

    ~SessionCache() { clear(); }

    bool store(CachedSession session)
    {
        session.account = AccountCapability::normalizedAccount(session.account);
        session.instanceUrl = session.instanceUrl.trimmed();
        session.token = session.token.trimmed();
        if (session.account.isEmpty() || session.instanceUrl.isEmpty() ||
            session.token.isEmpty() || !transportOrigin(session.instanceUrl).isValid()) {
            session.clearSensitive();
            return false;
        }
        const QString cacheKey = key(session.account, session.instanceUrl);
        auto existing = m_sessions.find(cacheKey);
        if (existing != m_sessions.end()) {
            existing.value().clearSensitive();
            m_sessions.erase(existing);
        }
        m_sessions.insert(cacheKey, std::move(session));
        return true;
    }

    bool lookup(const QString &account, const QString &instanceUrl,
                CachedSession *session) const
    {
        const auto found = m_sessions.constFind(key(account, instanceUrl));
        if (found == m_sessions.cend())
            return false;
        if (session)
            *session = found.value();
        return true;
    }

    bool remove(const QString &account, const QString &instanceUrl,
                const QString &expectedToken = QString())
    {
        auto found = m_sessions.find(key(account, instanceUrl));
        if (found == m_sessions.end() ||
            (!expectedToken.isEmpty() && found->token != expectedToken)) {
            return false;
        }
        found.value().clearSensitive();
        m_sessions.erase(found);
        return true;
    }

    bool removeToken(const QString &token)
    {
        if (token.isEmpty())
            return false;
        for (auto it = m_sessions.begin(); it != m_sessions.end(); ++it) {
            if (it->token != token)
                continue;
            it.value().clearSensitive();
            m_sessions.erase(it);
            return true;
        }
        return false;
    }

    int removeInstance(const QString &instanceUrl)
    {
        const QString exact = instanceUrl.trimmed();
        int removed = 0;
        for (auto it = m_sessions.begin(); it != m_sessions.end();) {
            if (it->instanceUrl.trimmed() != exact) {
                ++it;
                continue;
            }
            it.value().clearSensitive();
            it = m_sessions.erase(it);
            ++removed;
        }
        return removed;
    }

    void clear()
    {
        for (CachedSession &session : m_sessions)
            session.clearSensitive();
        m_sessions.clear();
    }

    int size() const { return m_sessions.size(); }

    static bool maySendTo(const CachedSession &session, const QUrl &target)
    {
        const QUrl sessionOrigin = transportOrigin(session.instanceUrl);
        const QUrl targetOrigin = transportOrigin(target);
        return sessionOrigin.isValid() && targetOrigin.isValid() &&
               sessionOrigin.scheme() == targetOrigin.scheme() &&
               sessionOrigin.host().compare(targetOrigin.host(),
                                            Qt::CaseInsensitive) == 0 &&
               effectivePort(sessionOrigin) == effectivePort(targetOrigin);
    }

    static bool mayRestoreDesktopCapability(
        const CachedSession &session, const QString &currentPublicKey,
        const QString &validatedAccountPublicKey,
        const QStringList &accountDesktopKeys = QStringList())
    {
        const QString current = currentPublicKey.trimmed();
        return session.desktopCapable && !current.isEmpty() &&
               session.desktopPublicKey == current &&
               (validatedAccountPublicKey.trimmed() == current ||
                accountDesktopKeys.contains(current));
    }

private:
    static QString key(const QString &account, const QString &instanceUrl)
    {
        return AccountCapability::normalizedAccount(account) + QChar(0x1f) +
               instanceUrl.trimmed();
    }

    static QUrl transportOrigin(const QString &value)
    {
        return transportOrigin(QUrl(value));
    }

    static QUrl transportOrigin(QUrl url)
    {
        if (!url.isValid() || url.host().isEmpty() ||
            !url.userName().isEmpty() || !url.password().isEmpty()) {
            return {};
        }
        if (url.scheme().compare(QLatin1String("ws"), Qt::CaseInsensitive) == 0)
            url.setScheme(QStringLiteral("http"));
        else if (url.scheme().compare(QLatin1String("wss"),
                                     Qt::CaseInsensitive) == 0)
            url.setScheme(QStringLiteral("https"));
        if (url.scheme() != QLatin1String("http") &&
            url.scheme() != QLatin1String("https")) {
            return {};
        }
        url.setPath(QString());
        url.setQuery(QString());
        url.setFragment(QString());
        return url;
    }

    static int effectivePort(const QUrl &url)
    {
        return url.port(url.scheme() == QLatin1String("https") ? 443 : 80);
    }

    QHash<QString, CachedSession> m_sessions;
};

} // namespace forkmesh::accounts

#endif
