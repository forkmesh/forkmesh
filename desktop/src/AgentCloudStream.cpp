#include "AgentCloudStream.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>

namespace forkmesh::agents {

void CloudAgentStreamDecoder::reset()
{
    m_stream = -1;
    m_job = 0;
    m_after = 0;
    m_partial.clear();
    m_skipToLine = false;
}

void CloudAgentStreamDecoder::restart(CloudAgentStreamBatch &batch)
{
    batch.events.clear();
    batch.restarted = true;
    m_partial.clear();
    m_skipToLine = false;
}

CloudAgentStreamBatch CloudAgentStreamDecoder::apply(const QJsonObject &payload)
{
    CloudAgentStreamBatch batch;
    const qint64 stream =
        static_cast<qint64>(payload.value(QStringLiteral("stream")).toDouble());
    if (m_stream >= 0 && stream != m_stream) {
        restart(batch);
        m_job = 0;
    }
    m_stream = stream;
    if (payload.value(QStringLiteral("reset")).toBool()) {
        restart(batch);
        m_job = 0;
    }
    const QJsonArray chunks = payload.value(QStringLiteral("chunks")).toArray();
    if (payload.value(QStringLiteral("truncated")).toBool() && !chunks.isEmpty()) {
        m_partial.clear();
        m_skipToLine = true;
    }
    for (const QJsonValue &value : chunks) {
        const QJsonObject chunk = value.toObject();
        const qint64 job =
            static_cast<qint64>(chunk.value(QStringLiteral("job")).toDouble());
        if (job != m_job) {
            if (m_job != 0 || !batch.events.isEmpty())
                restart(batch);
            m_job = job;
        }
        m_partial += QByteArray::fromBase64(
            chunk.value(QStringLiteral("data")).toString().toLatin1());
        qsizetype start = 0;
        for (qsizetype end = m_partial.indexOf('\n'); end >= 0;
             end = m_partial.indexOf('\n', start)) {
            const QByteArray line = m_partial.mid(start, end - start).trimmed();
            start = end + 1;
            if (m_skipToLine) {
                m_skipToLine = false;
                continue;
            }
            if (line.isEmpty() || line.front() != '{')
                continue;
            QJsonParseError error{};
            const QJsonDocument document = QJsonDocument::fromJson(line, &error);
            if (error.error != QJsonParseError::NoError || !document.isObject())
                continue;
            const QJsonObject event = document.object();
            if (event.value(QStringLiteral("type")).toString().isEmpty())
                continue;
            batch.events.append(event);
        }
        m_partial.remove(0, start);
        if (m_partial.size() > kMaxPartialBytes) {
            m_partial.clear();
            m_skipToLine = true;
        }
    }
    const qint64 next =
        static_cast<qint64>(payload.value(QStringLiteral("nextSeq")).toDouble());
    if (next > 0 || batch.restarted)
        m_after = next;
    batch.more = payload.value(QStringLiteral("more")).toBool();
    return batch;
}

bool CloudAgentStreamWatch::follow(int sessionId, const QString &key, bool running,
                                   bool visible)
{
    const bool wanted = sessionId != 0 && !key.isEmpty() && running && visible;
    if (wanted && sessionId == m_sessionId && key == m_key)
        return false;
    if (!wanted && m_sessionId == 0)
        return false;
    m_decoder.reset();
    m_inFlight = false;
    m_rang = false;
    m_more = false;
    m_nextAtMs = 0;
    ++m_generation;
    m_sessionId = wanted ? sessionId : 0;
    m_key = wanted ? key : QString();
    return true;
}

bool CloudAgentStreamWatch::due(qint64 nowMs) const
{
    if (!subscribed() || m_inFlight)
        return false;
    return m_rang || m_more || nowMs >= m_nextAtMs;
}

void CloudAgentStreamWatch::started(qint64 nowMs)
{
    m_inFlight = true;
    m_rang = false;
    m_more = false;
    m_nextAtMs = nowMs + kFallbackMs;
}

CloudAgentStreamBatch CloudAgentStreamWatch::finished(bool ok, const QJsonObject &payload,
                                                      qint64 nowMs)
{
    m_inFlight = false;
    if (!ok) {
        m_nextAtMs = nowMs + kRetryMs;
        return {};
    }
    CloudAgentStreamBatch batch = m_decoder.apply(payload);
    m_more = batch.more;
    m_nextAtMs = nowMs + kFallbackMs;
    return batch;
}

} // namespace forkmesh::agents
