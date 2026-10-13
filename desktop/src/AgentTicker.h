#pragma once

#include <QList>
#include <QString>

namespace forkmesh::ui {

QString agentTickerLine(const QString &prose, int maxChars = 120);

class AgentTickerTrain
{
public:
    struct Car {
        QString text;
        qreal width = 0;
        qreal left = 0;
    };

    static constexpr qreal kSpeedPxPerSec = 80.0;
    static constexpr qreal kSeamPx = 5.0;
    static constexpr qint64 kHoldMs = 500;
    static constexpr qint64 kResyncMs = 60000;

    void setLine(const QString &text, qreal width, qreal laneWidth, qint64 nowMs);
    QList<Car> layout(qreal laneWidth, qint64 nowMs);
    QString line() const { return m_line; }
    bool isEmpty() const { return m_cars.isEmpty(); }

private:
    qreal offset(qint64 nowMs) const;
    void advance(qint64 nowMs);
    void pad(qreal laneWidth);

    QList<Car> m_cars;
    QString m_line;
    qint64 m_lapStartMs = 0;
};

}
