#pragma once

#include "ReticulumNode.h"

#include <QColor>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QPointF>
#include <QString>
#include <QVector>
#include <QWidget>

#include <functional>

class QTimer;

namespace forkmesh::reticulum {

enum class MapGroup { Peer, Node, Propagation, Transport, Other };

struct ChartPalette {
    bool dark = true;
    QColor surface;
    QColor raised;
    QColor border;
    QColor grid;
    QColor text;
    QColor muted;
    QColor accent;
    QColor good;
    QColor warning;
    QColor critical;
    QColor series(int slot) const;
    QColor group(MapGroup group) const;
    static ChartPalette forWidget(const QWidget *widget);
};

MapGroup mapGroup(const QString &kind);
QString mapGroupLabel(MapGroup group);
QString formatBytes(double bytes);
QString formatRate(double bytesPerSecond);
QString formatAge(const QDateTime &when, const QDateTime &now = QDateTime::currentDateTimeUtc());
QString formatDuration(qint64 seconds);
void paintGroupMarker(class QPainter *painter, MapGroup group, const QPointF &center, double radius,
                      const QColor &fill, const QColor &ring);

class StatTile : public QWidget
{
public:
    explicit StatTile(const QString &label, QWidget *parent = nullptr);
    void setValue(const QString &value, const QString &detail = {});
    void setTone(int tone);
    void setSpark(const QVector<double> &values);
    QString value() const { return m_value; }
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QString m_label;
    QString m_value;
    QString m_detail;
    int m_tone = 0;
    QVector<double> m_spark;
};

class TrafficChart : public QWidget
{
public:
    explicit TrafficChart(QWidget *parent = nullptr);
    void setSamples(const QVector<TrafficSample> &samples);
    void setTitle(const QString &title);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    QRectF plotRect() const;
    QVector<TrafficSample> m_samples;
    QString m_title;
    int m_hover = -1;
};

class NetworkMap : public QWidget
{
public:
    struct Spoke {
        QString id;
        QString name;
        bool online = false;
    };

    explicit NetworkMap(QWidget *parent = nullptr);
    ~NetworkMap() override;
    void setData(const QList<Spoke> &spokes, const QList<DestinationRecord> &destinations,
                 const QByteArray &self, bool selfOnline);
    void setSelected(const QByteArray &hash);
    void setFilter(const QString &text);
    void pulse(const QByteArray &hash);
    void setActivated(std::function<void(const QByteArray &)> handler) { m_activated = std::move(handler); }
    QByteArray hashAt(const QPointF &point) const;
    QPointF positionOf(const QByteArray &hash) const { return m_positions.value(hash); }
    int plottedCount() const { return m_positions.size(); }
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    void layoutNodes();
    QPointF center() const;
    double ringRadius(int hops) const;
    void ensureAnimation();

    QList<Spoke> m_spokes;
    QList<DestinationRecord> m_destinations;
    QHash<QByteArray, QPointF> m_positions;
    QHash<QString, QPair<double, double>> m_sectors;
    QHash<QByteArray, qint64> m_pulses;
    QByteArray m_self;
    QByteArray m_selected;
    QByteArray m_hover;
    QString m_filter;
    bool m_selfOnline = false;
    int m_maxRing = 4;
    QTimer *m_animation = nullptr;
    std::function<void(const QByteArray &)> m_activated;
};

}
