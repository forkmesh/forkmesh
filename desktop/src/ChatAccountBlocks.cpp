#include "ChatAccountBlocks.h"

#include <QJsonArray>
#include <QRegularExpression>
#include <QSettings>

#include <algorithm>

namespace {

QString cleanAccount(const QString &account)
{
    static const QRegularExpression pattern(
        QStringLiteral("\\A[a-z0-9](?:[a-z0-9-]{0,38}[a-z0-9])?\\z"));
    QString name = account.trimmed().toLower();
    if (name.startsWith(QLatin1Char('@')))
        name.remove(0, 1);
    return pattern.match(name).hasMatch() ? name : QString();
}

}

ChatAccountBlocks::ChatAccountBlocks(QObject *parent)
    : QObject(parent), m_settingsKey(QStringLiteral("chat/blockedAccounts"))
{
    load();
}

void ChatAccountBlocks::setSettingsKey(const QString &key)
{
    if (key.isEmpty() || key == m_settingsKey)
        return;
    m_settingsKey = key;
    load();
    emit changed();
}

void ChatAccountBlocks::setPusher(Push pusher)
{
    m_pusher = std::move(pusher);
}

bool ChatAccountBlocks::isBlocked(const QString &account) const
{
    const QString name = cleanAccount(account);
    return !name.isEmpty() && m_accounts.contains(name);
}

void ChatAccountBlocks::setBlocked(const QString &account, bool blocked)
{
    const QString name = cleanAccount(account);
    if (name.isEmpty() || m_accounts.contains(name) == blocked)
        return;
    if (blocked && m_accounts.size() >= kMaxAccounts)
        return;
    if (blocked)
        m_accounts.insert(name);
    else
        m_accounts.remove(name);
    save();
    push();
    emit changed();
}

QStringList ChatAccountBlocks::accounts() const
{
    QStringList list(m_accounts.begin(), m_accounts.end());
    list.sort();
    return list;
}

void ChatAccountBlocks::mergeServerList(const QJsonObject &body)
{
    QStringList server;
    for (const QJsonValue &value : body.value(QStringLiteral("accounts")).toArray())
        server.append(value.toString());
    server = normalized(server);
    QSet<QString> merged(server.begin(), server.end());
    for (const QString &name : std::as_const(m_accounts))
        if (merged.size() < kMaxAccounts)
            merged.insert(name);
    const bool localChanged = merged != m_accounts;
    const bool serverStale = merged != QSet<QString>(server.begin(), server.end());
    m_accounts = merged;
    if (localChanged)
        save();
    if (serverStale)
        push();
    if (localChanged)
        emit changed();
}

QStringList ChatAccountBlocks::normalized(const QStringList &accounts)
{
    QStringList out;
    for (const QString &account : accounts) {
        const QString name = cleanAccount(account);
        if (!name.isEmpty() && !out.contains(name))
            out.append(name);
        if (out.size() >= kMaxAccounts)
            break;
    }
    out.sort();
    return out;
}

QJsonObject ChatAccountBlocks::body(const QStringList &accounts)
{
    return {{QStringLiteral("accounts"), QJsonArray::fromStringList(normalized(accounts))}};
}

QUrl ChatAccountBlocks::endpoint(const QUrl &apiBase)
{
    QUrl url = apiBase;
    url.setPath(QStringLiteral("/api/chat/blocks"));
    url.setQuery(QString());
    url.setFragment(QString());
    return url;
}

void ChatAccountBlocks::load()
{
    const QStringList saved =
        normalized(QSettings().value(m_settingsKey).toStringList());
    m_accounts = QSet<QString>(saved.begin(), saved.end());
}

void ChatAccountBlocks::save() const
{
    QSettings().setValue(m_settingsKey, accounts());
}

void ChatAccountBlocks::push() const
{
    if (m_pusher)
        m_pusher(body(accounts()));
}
