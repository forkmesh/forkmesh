#include "AgentTicker.h"

#include <QRegularExpression>
#include <QStringList>

namespace forkmesh::ui {

namespace {

bool proseLine(const QString &line)
{
    if (line.startsWith(QLatin1Char('|')) || line.startsWith(QLatin1Char('{')) ||
        line.startsWith(QLatin1Char('[')) || line.startsWith(QLatin1Char('<')))
        return false;
    static const QRegularExpression rule(QStringLiteral("^([-*_=])\\1{2,}$"));
    return !rule.match(line).hasMatch();
}

QString plainLine(QString line)
{
    static const QRegularExpression heading(QStringLiteral("^#{1,6}\\s+"));
    static const QRegularExpression bullet(
        QStringLiteral("^(?:>\\s*)*(?:[-*+]\\s+|\\d+[.)]\\s+)?(?:\\[[ xX]\\]\\s+)?"));
    static const QRegularExpression link(QStringLiteral("!?\\[([^\\]]*)\\]\\([^)]*\\)"));
    static const QRegularExpression marks(QStringLiteral("\\*\\*|__|`|~~"));
    static const QRegularExpression emphasis(
        QStringLiteral("(^|[\\s(])[*_]([^*_\\s][^*_]*)[*_](?=[\\s).,:;!?]|$)"));
    line.remove(heading);
    line.remove(bullet);
    line.replace(link, QStringLiteral("\\1"));
    line.remove(marks);
    line.replace(emphasis, QStringLiteral("\\1\\2"));
    return line.trimmed();
}

}

QString agentTickerLine(const QString &prose, int maxChars)
{
    QList<QStringList> blocks;
    QStringList block;
    bool fenced = false;
    const QStringList lines = prose.split(QLatin1Char('\n'));
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (line.startsWith(QLatin1String("```")) || line.startsWith(QLatin1String("~~~"))) {
            fenced = !fenced;
            if (!block.isEmpty())
                blocks.append(block);
            block.clear();
            continue;
        }
        if (fenced || raw.startsWith(QLatin1String("    ")) ||
            raw.startsWith(QLatin1Char('\t')))
            continue;
        if (line.isEmpty() || !proseLine(line)) {
            if (!block.isEmpty())
                blocks.append(block);
            block.clear();
            continue;
        }
        const QString plain = plainLine(line);
        if (!plain.isEmpty())
            block.append(plain);
    }
    if (!block.isEmpty())
        blocks.append(block);
    if (blocks.isEmpty())
        return QString();
    QString text = blocks.last().join(QLatin1Char(' ')).simplified();
    if (maxChars > 1 && text.size() > maxChars)
        text = text.left(maxChars - 1).trimmed() + QChar(0x2026);
    return text;
}

qreal AgentTickerTrain::offset(qint64 nowMs) const
{
    return qMax<qint64>(0, nowMs - m_lapStartMs) * kSpeedPxPerSec / 1000.0;
}

void AgentTickerTrain::advance(qint64 nowMs)
{
    if (m_cars.isEmpty())
        return;
    if (nowMs - m_lapStartMs > kResyncMs) {
        const Car last = m_cars.last();
        m_cars = {last};
        m_lapStartMs = nowMs;
        return;
    }
    while (true) {
        const qreal lap = m_cars.first().width + kSeamPx;
        if (lap <= 0 || offset(nowMs) < lap)
            break;
        m_lapStartMs += qint64(lap * 1000.0 / kSpeedPxPerSec);
        if (m_cars.size() > 1)
            m_cars.removeFirst();
    }
}

void AgentTickerTrain::pad(qreal laneWidth)
{
    if (m_cars.isEmpty())
        return;
    const qreal reach = m_cars.first().width + kSeamPx + qMax<qreal>(laneWidth, 1);
    qreal length = 0;
    for (const Car &car : std::as_const(m_cars))
        length += car.width + kSeamPx;
    const Car tail = m_cars.last();
    while (length < reach && tail.width + kSeamPx > 0) {
        m_cars.append(tail);
        length += tail.width + kSeamPx;
    }
}

void AgentTickerTrain::setLine(const QString &text, qreal width, qreal laneWidth,
                               qint64 nowMs)
{
    if (text.isEmpty()) {
        m_cars.clear();
        m_line.clear();
        return;
    }
    if (text == m_line && !m_cars.isEmpty())
        return;
    m_line = text;
    const Car car{text, qMax<qreal>(width, 1), 0};
    if (m_cars.isEmpty()) {
        m_cars = {car};
        m_lapStartMs = nowMs + kHoldMs;
        pad(laneWidth);
        return;
    }
    advance(nowMs);
    qreal x = -offset(nowMs);
    int keep = 0;
    for (const Car &existing : std::as_const(m_cars)) {
        if (keep > 0 && x >= laneWidth)
            break;
        x += existing.width + kSeamPx;
        ++keep;
    }
    m_cars = m_cars.mid(0, keep);
    m_cars.append(car);
    pad(laneWidth);
}

QList<AgentTickerTrain::Car> AgentTickerTrain::layout(qreal laneWidth, qint64 nowMs)
{
    QList<Car> visible;
    if (m_cars.isEmpty())
        return visible;
    advance(nowMs);
    pad(laneWidth);
    qreal x = -offset(nowMs);
    for (const Car &car : std::as_const(m_cars)) {
        if (x >= laneWidth)
            break;
        if (x + car.width > 0)
            visible.append(Car{car.text, car.width, x});
        x += car.width + kSeamPx;
    }
    return visible;
}

}
