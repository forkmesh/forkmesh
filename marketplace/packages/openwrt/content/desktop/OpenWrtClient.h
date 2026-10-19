#pragma once

#include "OpenWrtModel.h"

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QNetworkReply>
#include <QPointer>
#include <QString>
#include <QUrl>

#include <functional>
#include <memory>

class QNetworkAccessManager;
class QNetworkRequest;
class QObject;

namespace forkmesh::openwrt {

class Client
{
public:
    using Done = std::function<void(const RpcResult &)>;
    using BytesDone = std::function<void(const QByteArray &, const QString &)>;

    explicit Client(QNetworkAccessManager *network);
    ~Client();

    void setBase(const QUrl &base);
    QUrl base() const { return m_base; }
    void setTrustedCertificate(const QString &fingerprint);
    QString trustedCertificate() const { return m_trustedCertificate; }

    void signIn(const QString &user, const QString &password, Done done);
    void signOut();
    bool signedIn() const { return !m_session.isEmpty(); }
    QString user() const { return m_user; }
    void rememberPassword(const QString &password) { m_password = password; }
    void setSessionRenewed(std::function<void()> callback) { m_sessionRenewed = std::move(callback); }

    void call(const QString &object, const QString &method, const QJsonObject &args, Done done, int timeoutMs = 20000);
    void exec(const QString &command, const QStringList &params, Done done, int timeoutMs = 120000);
    void renewSession(Done done);
    void upload(const QString &path, const QByteArray &data, Done done);
    void downloadBackup(BytesDone done);

private:
    struct Pending {
        QString object;
        QString method;
        QJsonObject args;
        Done done;
        int timeoutMs = 20000;
    };

    void send(const Pending &pending, bool mayRenew);
    void renewThen(const Pending &pending);
    QNetworkRequest request(const QString &path, int timeoutMs) const;
    void watchCertificate(QNetworkReply *reply, const std::shared_ptr<QString> &untrusted);
    RpcResult transportFailure(QNetworkReply *reply, const QString &untrusted) const;

    QNetworkAccessManager *m_network = nullptr;
    std::unique_ptr<QObject> m_guard;
    QUrl m_base;
    QString m_trustedCertificate;
    QString m_session;
    QString m_user;
    QString m_password;
    int m_nextId = 1;
    bool m_renewing = false;
    QList<Pending> m_waiting;
    QList<QPointer<QNetworkReply>> m_replies;
    std::function<void()> m_sessionRenewed;
};

}
