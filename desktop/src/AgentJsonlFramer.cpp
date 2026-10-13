#include "AgentJsonlFramer.h"

#include <QtGlobal>

#include <utility>

AgentJsonlFramer::AgentJsonlFramer(qsizetype maximumFrameBytes)
    : m_maximumFrameBytes(qMax<qsizetype>(1, maximumFrameBytes))
{}

AgentJsonlFramer::Result AgentJsonlFramer::append(QByteArray bytes,
                                                  bool flushRemainder)
{
    Result result;
    qsizetype searchFrom = m_buffer.size();
    if (!bytes.isEmpty())
        m_buffer.append(bytes);

    while (true) {
        const qsizetype newline = m_buffer.indexOf('\n', searchFrom);
        if (newline < 0)
            break;
        searchFrom = 0;
        QByteArray line = m_buffer.left(newline);
        m_buffer.remove(0, newline + 1);
        if (m_discardingOversizedFrame) {
            m_discardingOversizedFrame = false;
            continue;
        }
        acceptLine(std::move(line), result);
    }

    if (!m_discardingOversizedFrame &&
        m_buffer.size() > m_maximumFrameBytes) {
        m_buffer.clear();
        m_discardingOversizedFrame = true;
        result.errors.append(
            QStringLiteral("Runtime frame exceeded the %1-byte limit and was "
                           "discarded.")
                .arg(m_maximumFrameBytes));
    }

    if (flushRemainder) {
        if (m_discardingOversizedFrame) {
            m_discardingOversizedFrame = false;
        } else if (!m_buffer.isEmpty()) {
            acceptLine(std::move(m_buffer), result);
        }
        m_buffer.clear();
    }
    return result;
}

void AgentJsonlFramer::reset()
{
    m_buffer.clear();
    m_discardingOversizedFrame = false;
}

void AgentJsonlFramer::acceptLine(QByteArray line, Result &result)
{
    if (line.endsWith('\r'))
        line.chop(1);
    if (line.size() > m_maximumFrameBytes) {
        result.errors.append(
            QStringLiteral("Runtime frame exceeded the %1-byte limit and was "
                           "discarded.")
                .arg(m_maximumFrameBytes));
        return;
    }
    result.frames.append(std::move(line));
}
