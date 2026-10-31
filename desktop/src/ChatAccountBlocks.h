#pragma once

#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <functional>

class ChatAccountBlocks : public QObject
{
    Q_OBJECT
public:
    using Push = std::function<void(const QJsonObject &body)>;

    static constexpr int kMaxAccounts = 500;

    explicit ChatAccountBlocks(QObject *parent = nullptr);

    void setSettingsKey(const QString &key);
    void setPusher(Push pusher);

    bool isBlocked(const QString &account) const;
    void setBlocked(const QString &account, bool blocked);
    QStringList accounts() const;
    void mergeServerList(const QJsonObject &body);

    static QStringList normalized(const QStringList &accounts);
    static QJsonObject body(const QStringList &accounts);
    static QUrl endpoint(const QUrl &apiBase);

signals:
    void changed();

private:
    void load();
    void save() const;
    void push() const;

    QString m_settingsKey;
    QSet<QString> m_accounts;
    Push m_pusher;
};
