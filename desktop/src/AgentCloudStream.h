#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>

namespace forkmesh::agents {

struct CloudAgentStreamBatch {
    QList<QJsonObject> events;
    bool restarted = false;
    bool more = false;
};

class CloudAgentStreamDecoder {
public:
    static constexpr int kMaxPartialBytes = 1 << 20;

    CloudAgentStreamBatch apply(const QJsonObject &payload);
    void reset();
    qint64 after() const { return m_after; }
    qint64 job() const { return m_job; }

private:
    void restart(CloudAgentStreamBatch &batch);
    qint64 m_stream = -1;
    qint64 m_job = 0;
    qint64 m_after = 0;
    QByteArray m_partial;
    bool m_skipToLine = false;
};

class CloudAgentStreamWatch {
public:
    static constexpr qint64 kFallbackMs = 3000;
    static constexpr qint64 kRetryMs = 10000;

    bool follow(int sessionId, const QString &key, bool running, bool visible);
    bool subscribed() const { return m_sessionId != 0; }
    int sessionId() const { return m_sessionId; }
    QString key() const { return m_key; }
    void ring() { m_rang = true; }
    bool due(qint64 nowMs) const;
    void started(qint64 nowMs);
    CloudAgentStreamBatch finished(bool ok, const QJsonObject &payload, qint64 nowMs);
    bool inFlight() const { return m_inFlight; }
    qint64 after() const { return m_decoder.after(); }
    quint64 generation() const { return m_generation; }

private:
    int m_sessionId = 0;
    QString m_key;
    bool m_inFlight = false;
    bool m_rang = false;
    bool m_more = false;
    qint64 m_nextAtMs = 0;
    quint64 m_generation = 0;
    CloudAgentStreamDecoder m_decoder;
};

} // namespace forkmesh::agents
