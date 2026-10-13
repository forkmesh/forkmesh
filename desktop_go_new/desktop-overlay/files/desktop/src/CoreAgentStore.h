#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <memory>

namespace forkmesh::core {
struct Endpoint;
}

namespace forkmesh::coreagentstore {

class Channel;

void attach(const forkmesh::core::Endpoint &endpoint);
void detach();
bool attached();
bool routes(const QString &storeRoot);
quint64 generation();

bool call(const QString &method, const QJsonObject &params, QJsonObject *result,
          QString *error, int timeoutMs = 30000);

class Lane
{
public:
    Lane();
    ~Lane();
    bool notify(const QString &method, const QJsonObject &params);
    bool sync(int timeoutMs = 30000);
    bool dirty() const { return m_dirty; }

private:
    bool ensure();

    std::unique_ptr<Channel> m_channel;
    quint64 m_generation = 0;
    bool m_dirty = false;
};

void notifyLogAppended(int sessionId);

} // namespace forkmesh::coreagentstore
