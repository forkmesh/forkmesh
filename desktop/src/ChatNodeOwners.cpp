#include "ChatNodeOwners.h"

#include "RoomSeal.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QRegularExpression>
#include <QUrlQuery>

namespace {

bool validAccount(const QString &account)
{
    static const QRegularExpression pattern(
        QStringLiteral("\\A[a-z0-9](?:[a-z0-9-]{0,38}[a-z0-9])?\\z"));
    return pattern.match(account).hasMatch();
}

}

ChatNodeOwners::ChatNodeOwners(QObject *parent) : QObject(parent) {}

void ChatNodeOwners::setFetcher(Fetcher fetcher)
{
    m_fetcher = std::move(fetcher);
}

void ChatNodeOwners::setClock(std::function<qint64()> clock)
{
    m_clock = std::move(clock);
}

void ChatNodeOwners::clear()
{
    m_entries.clear();
    m_inFlight.clear();
}

qint64 ChatNodeOwners::now() const
{
    return m_clock ? m_clock() : QDateTime::currentMSecsSinceEpoch();
}

bool ChatNodeOwners::isAnswered(const QString &node) const
{
    const auto it = m_entries.constFind(node);
    return it != m_entries.constEnd() && it->answeredAt > 0 &&
           now() - it->answeredAt < kAnswerLifeMs;
}

QString ChatNodeOwners::account(const QString &node)
{
    if (!forkmesh::chatv3::isNodeKey(node))
        return QString();
    const auto it = m_entries.constFind(node);
    const qint64 current = now();
    const bool fresh = it != m_entries.constEnd() && it->answeredAt > 0 &&
                       current - it->answeredAt < kAnswerLifeMs;
    const bool cooling = it != m_entries.constEnd() && it->failedAt > 0 &&
                         current - it->failedAt < kRetryAfterMs;
    if (!fresh && !cooling)
        request(node);
    return fresh ? it->account : QString();
}

QString ChatNodeOwners::shownAccount(const QString &node, const QString &claim)
{
    const QString verified = account(node);
    const QString wanted = claim.trimmed().toLower();
    if (verified.isEmpty())
        return QString();
    if (!wanted.isEmpty() && wanted != verified)
        return QString();
    return verified;
}

void ChatNodeOwners::request(const QString &node)
{
    if (!m_fetcher || m_inFlight.contains(node))
        return;
    if (m_entries.size() >= kMaxEntries && !m_entries.contains(node))
        m_entries.clear();
    m_inFlight.insert(node);
    QPointer<ChatNodeOwners> self(this);
    m_fetcher(node, [self, node](bool ok, const QString &answer) {
        if (!self)
            return;
        self->m_inFlight.remove(node);
        Entry &entry = self->m_entries[node];
        const QString account = answer.trimmed().toLower();
        if (!ok || (!account.isEmpty() && !validAccount(account))) {
            entry.failedAt = self->now();
            return;
        }
        const bool changed = entry.answeredAt <= 0 || entry.account != account;
        entry.account = account;
        entry.answeredAt = self->now();
        entry.failedAt = 0;
        if (changed)
            emit self->resolved(node, account);
    });
}

QUrl ChatNodeOwners::lookupUrl(const QUrl &apiBase, const QString &node)
{
    QUrl url = apiBase;
    url.setPath(QStringLiteral("/api/chat/node-owner"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("node"), node);
    url.setQuery(query);
    url.setFragment(QString());
    return url;
}

bool ChatNodeOwners::parseAnswer(const QByteArray &body, QString *account)
{
    const QJsonDocument document = QJsonDocument::fromJson(body);
    if (!document.isObject())
        return false;
    const QJsonValue value = document.object().value(QStringLiteral("account"));
    if (!value.isString())
        return false;
    if (account)
        *account = value.toString().trimmed().toLower();
    return true;
}
