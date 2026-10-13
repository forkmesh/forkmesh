#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <functional>

class QObject;

namespace forkmesh::coreagentrun {

using Handler = std::function<void(const QString &event, const QJsonObject &data)>;
using Done = std::function<void(bool ok, const QJsonObject &result, const QString &error)>;

struct Backend {
    std::function<bool()> available;
    std::function<bool(const QString &method)> hasRoute;
    std::function<void(const QString &method, const QJsonObject &params, Done done)> request;
    std::function<bool(const QString &method, const QJsonObject &params)> notify;
    std::function<QStringList()> credentials;
};

void setBackend(Backend backend);
void dispatch(const QString &event, const QJsonObject &data);
void connectionChanged(bool connected);

bool available();
void pushCredentials();
void subscribe(const QString &key, QObject *context, Handler handler);
void unsubscribe(const QString &key, QObject *context);
void attach(const QString &key, qint64 after, QObject *context, Done done);
void detach(const QString &key);
void request(const QString &method, const QJsonObject &params, QObject *context, Done done);
bool notify(const QString &method, const QJsonObject &params);

bool deliveringStored(const QJsonObject &event);
bool deliveringAnyStored();
QString transientKey(const QString &runtime);

class StoredDelivery
{
public:
    explicit StoredDelivery(const QJsonObject *event);
    ~StoredDelivery();
    StoredDelivery(const StoredDelivery &) = delete;
    StoredDelivery &operator=(const StoredDelivery &) = delete;

private:
    const QJsonObject *m_previous;
};

} // namespace forkmesh::coreagentrun
