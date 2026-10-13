#pragma once

#include <QByteArray>
#include <QList>
#include <QStringList>

// Incremental, bounded JSONL framing shared by runtime transports. JSON
// parsing stays with the adapter because each protocol has a different
// envelope; this class owns chunk reassembly and memory limits.
class AgentJsonlFramer
{
public:
    static constexpr qsizetype DefaultMaximumFrameBytes = 64 * 1024 * 1024;

    struct Result {
        QList<QByteArray> frames;
        QStringList errors;
    };

    explicit AgentJsonlFramer(
        qsizetype maximumFrameBytes = DefaultMaximumFrameBytes);

    Result append(QByteArray bytes, bool flushRemainder = false);
    void reset();
    qsizetype bufferedBytes() const { return m_buffer.size(); }
    qsizetype maximumFrameBytes() const { return m_maximumFrameBytes; }

private:
    void acceptLine(QByteArray line, Result &result);

    QByteArray m_buffer;
    qsizetype m_maximumFrameBytes;
    bool m_discardingOversizedFrame = false;
};
