#pragma once

#include "SmbClient.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>

#include <atomic>
#include <functional>
#include <memory>

class QThread;

namespace forkmesh::smb {

struct SavedShare {
    QString id;
    QString name;
    QString host;
    quint16 port = kDefaultPort;
    QString share;
    QString user;
    bool guest = false;
    bool remember = false;

    QString title() const;
    QString url(const QString &path = {}) const;
    Location location(const QString &path = {}) const;
    bool sameTarget(const SavedShare &other) const;
};

class ShareStore
{
public:
    explicit ShareStore(QString secretsPath);

    QList<SavedShare> shares() const;
    SavedShare share(const QString &id) const;
    SavedShare save(SavedShare share);
    void remove(const QString &id);
    QString password(const QString &id) const;
    bool setPassword(const QString &id, const QString &password);
    QString secretsPath() const { return m_secretsPath; }

private:
    QHash<QString, QString> readSecrets() const;
    bool writeSecrets(const QHash<QString, QString> &secrets) const;

    QString m_secretsPath;
};

struct ConnectionState;

class Connection
{
public:
    using Job = std::function<void(Client &)>;

    Connection(SavedShare share, Credentials credentials, TransportFactory factory = {});
    ~Connection();

    const SavedShare &share() const { return m_share; }
    void setCredentials(const Credentials &credentials);
    void post(Job job);
    void cancel() { *m_cancel = true; }
    void resetCancel() { *m_cancel = false; }
    std::shared_ptr<std::atomic_bool> cancelFlag() const { return m_cancel; }

private:
    SavedShare m_share;
    std::shared_ptr<ConnectionState> m_state;
    std::shared_ptr<std::atomic_bool> m_cancel = std::make_shared<std::atomic_bool>(false);
    QThread *m_thread = nullptr;
    QObject *m_context = nullptr;
};

}
