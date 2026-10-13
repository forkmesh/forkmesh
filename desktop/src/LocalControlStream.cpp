#include "LocalControlStream.h"

#include <QByteArray>
#include <QFile>

namespace forkmesh::localcontrol {

namespace {

qsizetype completeUtf8Prefix(const QByteArray &bytes)
{
    const qsizetype size = bytes.size();
    for (qsizetype back = 1; back <= 4 && back <= size; ++back) {
        const auto byte = static_cast<unsigned char>(bytes.at(size - back));
        if ((byte & 0xC0) == 0x80)
            continue;
        int needed = 1;
        if ((byte & 0xE0) == 0xC0)
            needed = 2;
        else if ((byte & 0xF0) == 0xE0)
            needed = 3;
        else if ((byte & 0xF8) == 0xF0)
            needed = 4;
        return needed > back ? size - back : size;
    }
    return size;
}

} // namespace

QString readLogGrowth(const QString &path, LogTail &tail, bool *rewritten)
{
    if (rewritten)
        *rewritten = false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (tail.bytes > 0 && rewritten)
            *rewritten = true;
        tail = {};
        return QString();
    }
    if (file.size() < tail.bytes) {
        tail = {};
        if (rewritten)
            *rewritten = true;
    }
    if (!file.seek(tail.bytes))
        return QString();
    QByteArray bytes = file.readAll();
    const qsizetype keep = completeUtf8Prefix(bytes);
    bytes.truncate(keep);
    tail.bytes += keep;
    bytes.removeIf([](char c) { return c == '\r'; });
    const QString text = QString::fromUtf8(bytes);
    tail.chars += text.size();
    return text;
}

void SendBudget::configure(qint64 windowMs, int maximum)
{
    m_windowMs = qMax<qint64>(1000, windowMs);
    const int max = qMax(2, maximum);
    m_limit = qMax(1, max - qMax(1, max / 10));
}

qint64 SendBudget::waitMs(qint64 nowMs)
{
    while (!m_sent.empty() && nowMs - m_sent.front() >= m_windowMs)
        m_sent.pop_front();
    if (int(m_sent.size()) < m_limit)
        return 0;
    return qMax<qint64>(1, m_sent.front() + m_windowMs - nowMs);
}

void SendBudget::spend(qint64 nowMs)
{
    m_sent.push_back(nowMs);
}

void SendBudget::reset()
{
    m_sent.clear();
}

int followIntervalMs(bool relayed)
{
    return relayed ? 80 : 15;
}

} // namespace forkmesh::localcontrol
