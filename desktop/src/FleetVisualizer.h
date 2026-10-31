#pragma once

#include "CoreNodeLogs.h"

#include <QColor>
#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QMap>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QVector>
#include <QWidget>

#include <functional>

class QPainter;
class QTimer;

namespace forkmesh {

class FleetVisualizer : public QWidget
{
public:
    explicit FleetVisualizer(QWidget *parent = nullptr);

    std::function<QList<corelogs::NodeRow>()> nodeSource;
    std::function<QString()> idleReasonSource;
    std::function<void(const QString &table)> onTableClicked;

    void noteStatement(const QString &node, const QString &sql,
                       const QString &originFile, qint64 nowMs);
    void noteVitals(const QString &node, const corelogs::NodeVitals &vitals,
                    qint64 nowMs);
    void advance(double seconds);

    static QStringList verbOrder();
    static QString verbGroup(const QString &verb);
    static QColor verbColour(const QString &group, bool dark);
    static void paintMarker(QPainter &p, const QPointF &at, double radius,
                            const QString &group, const QColor &colour);

    qint64 statementCount() const { return m_totalStatements; }
    int particleCount() const { return m_particles.size(); }
    int tableCount() const { return m_tables.size(); }
    int nodeCount() const { return m_nodes.size(); }
    qint64 verbCount(const QString &group) const
    {
        return m_verbCounts.value(group);
    }
    QString toolTipAt(const QPointF &point) const;
    QString tableAt(const QPointF &point) const;
    bool clickAt(const QPointF &point);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    struct NodeState {
        QString name;
        bool connected = false;
        QString state;
        qint64 statements = 0;
        double pulse = 0.0;
        corelogs::NodeVitals vitals;
        qint64 vitalsReceivedMs = 0;
        QVector<double> cpuHistory;
        QVector<double> memHistory;
        QVector<qint64> filesHistory;
    };
    struct TableState {
        QString name;
        qint64 count = 0;
        QMap<QString, qint64> byVerb;
        double pulse = 0.0;
        QPointF position;
        QPointF target;
        bool placed = false;
    };
    struct Particle {
        QString node;
        QString table;
        QString group;
        double t = 0.0;
        double speed = 1.0;
        double bend = 0.0;
    };
    struct Hit {
        QRectF area;
        QString text;
        QString table;
    };

    NodeState &nodeState(const QString &node);
    void syncNodes();
    void updateTimer();
    void tick();
    int cardColumns(qreal width) const;
    int heightFor(int width) const;
    void updateMinimumHeight();
    QList<const NodeState *> orderedNodes() const;
    QStringList shownTables() const;
    QString tableSlot(const QString &table, const QStringList &shown) const;
    QHash<QString, QPointF> nodePositions(const QRectF &area) const;
    QHash<QString, QRectF> tableCells(const QRectF &area,
                                      const QStringList &shown) const;
    void paintHeader(QPainter &p, const QRectF &area);
    void paintStream(QPainter &p, const QRectF &area);
    void paintCallers(QPainter &p, const QRectF &area);
    void paintVitals(QPainter &p, const QRectF &area);
    void paintNodeCard(QPainter &p, const QRectF &card, const NodeState &node);
    bool dark() const;
    QColor ink(int alpha = 255) const;
    QColor surface() const;
    QColor statusColour(double share) const;
    QString statusWord(double share) const;

    QMap<QString, NodeState> m_nodes;
    QHash<QString, TableState> m_tables;
    QHash<QString, qint64> m_callers;
    QMap<QString, qint64> m_verbCounts;
    QVector<Particle> m_particles;
    QVector<int> m_perSecond;
    qint64 m_perSecondHeadMs = 0;
    qint64 m_totalStatements = 0;
    quint32 m_seed = 0x9e3779b9u;
    double m_phase = 0.0;
    QString m_idleReason;
    QTimer *m_timer = nullptr;
    QElapsedTimer m_clock;
    qint64 m_lastTickMs = 0;
    qint64 m_lastSyncMs = 0;
    QVector<Hit> m_hits;
};

} // namespace forkmesh
