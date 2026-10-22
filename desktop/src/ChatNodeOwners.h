#pragma once

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QUrl>

#include <functional>

class ChatNodeOwners : public QObject
{
    Q_OBJECT
public:
    using Reply = std::function<void(bool ok, const QString &account)>;
    using Fetcher = std::function<void(const QString &node, Reply reply)>;

    static constexpr qint64 kAnswerLifeMs = 60ll * 60 * 1000;
    static constexpr qint64 kRetryAfterMs = 60ll * 1000;
    static constexpr int kMaxEntries = 4096;

    explicit ChatNodeOwners(QObject *parent = nullptr);

    void setFetcher(Fetcher fetcher);
    void setClock(std::function<qint64()> clock);
    void clear();

    QString account(const QString &node);
    bool isAnswered(const QString &node) const;
    QString shownAccount(const QString &node, const QString &claim);

    static QUrl lookupUrl(const QUrl &apiBase, const QString &node);
    static bool parseAnswer(const QByteArray &body, QString *account);

signals:
    void resolved(const QString &node, const QString &account);

private:
    struct Entry {
        QString account;
        qint64 answeredAt = 0;
        qint64 failedAt = 0;
    };

    qint64 now() const;
    void request(const QString &node);

    Fetcher m_fetcher;
    std::function<qint64()> m_clock;
    QHash<QString, Entry> m_entries;
    QSet<QString> m_inFlight;
};
