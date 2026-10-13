#pragma once

#include "CoreNodeLogs.h"

#include <QHash>
#include <QList>
#include <QMap>
#include <QRectF>
#include <QString>
#include <QVector>
#include <QWidget>

class QPainter;
class QTimer;

namespace forkmesh {

class QueryInspector : public QWidget
{
public:
    enum class SortColumn { Calls, TotalTime, AverageTime, MaxTime, Rows };

    explicit QueryInspector(QWidget *parent = nullptr);

    void noteStatement(const QString &node,
                       const corelogs::StatementLine &statement, qint64 nowMs);
    void noteDone(const QString &node, const corelogs::StatementDone &done,
                  qint64 nowMs);
    void setSortColumn(SortColumn column);
    SortColumn sortColumn() const { return m_sort; }

    qint64 statementCount() const { return m_total; }
    qint64 timedCount() const { return m_timed; }
    qint64 unmatchedDoneCount() const { return m_unmatchedDone; }
    int shapeCount() const { return m_shapes.size(); }
    int recentCount() const { return m_recent.size(); }
    double percentileMs(double share) const;
    QStringList shapeOrder() const;
    qint64 routeCount(const QString &route) const { return m_routes.value(route); }
    qint64 userCount(const QString &user) const { return m_users.value(user); }
    qint64 kindCount(const QString &kind) const { return m_kinds.value(kind); }
    double recentMs(int newestIndex) const;
    QString toolTipAt(const QPointF &point) const;
    bool clickAt(const QPointF &point);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    static constexpr int kMaxShapes = 3000;
    static constexpr int kMaxNames = 1000;
    static constexpr int kRecent = 60;
    static constexpr int kLatencySamples = 4096;

protected:
    void paintEvent(QPaintEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    struct Shape {
        QString key;
        QString sql;
        QString verb;
        QString group;
        QString table;
        QString kind;
        QString code;
        int args = 0;
        qint64 count = 0;
        qint64 timed = 0;
        qint64 failed = 0;
        qint64 local = 0;
        double totalMs = 0.0;
        double maxMs = 0.0;
        qint64 rows = 0;
        qint64 changes = 0;
        qint64 lastMs = 0;
        QHash<QString, qint64> callers;
        QMap<QString, qint64> perNode;
    };
    struct Recent {
        qint64 atMs = 0;
        QString node;
        QString key;
        QString group;
        QString verb;
        QString table;
        QString kind;
        QString code;
        QString caller;
        QString sql;
        double ms = -1.0;
        qint64 rows = -1;
        qint64 changes = -1;
        QString served;
        bool failed = false;
    };
    struct Hit {
        QRectF area;
        QString text;
    };
    struct SortHit {
        QRectF area;
        SortColumn column;
    };
    struct Bar {
        QString label;
        double value = 0.0;
        QString detail;
        QColor colour;
    };

    void scheduleUpdate();
    static void bumpCapped(QHash<QString, qint64> &counts, const QString &key,
                           qint64 by, int cap);
    void trimShapes();
    double sortValue(const Shape &shape) const;
    bool dark() const;
    QColor ink(int alpha = 255) const;
    void paintBars(QPainter &p, const QRectF &area, const QString &title,
                   const QList<Bar> &bars, const QString &empty);
    QList<Bar> topBars(const QHash<QString, qint64> &counts, int limit,
                       const QColor &colour) const;
    void paintHeader(QPainter &p, const QRectF &area);
    void paintHistogram(QPainter &p, const QRectF &area);
    void paintTableTime(QPainter &p, const QRectF &area);
    void paintShapes(QPainter &p, const QRectF &area);
    void paintRecent(QPainter &p, const QRectF &area);
    QString shapeDetail(const Shape &shape) const;

    QHash<QString, Shape> m_shapes;
    QHash<QString, qint64> m_routes;
    QHash<QString, QString> m_routeKinds;
    QHash<QString, qint64> m_code;
    QHash<QString, qint64> m_kinds;
    QHash<QString, qint64> m_users;
    QHash<QString, double> m_tableMs;
    QHash<QString, qint64> m_tableCalls;
    QVector<Recent> m_recent;
    QVector<double> m_latencies;
    int m_latencyHead = 0;
    QMap<QString, QVector<qint64>> m_histogram;
    qint64 m_total = 0;
    qint64 m_timed = 0;
    qint64 m_failed = 0;
    qint64 m_local = 0;
    qint64 m_unmatchedDone = 0;
    qint64 m_background = 0;
    qint64 m_anonymous = 0;
    double m_maxMs = 0.0;
    SortColumn m_sort = SortColumn::Calls;
    bool m_sortChosen = false;
    bool m_dirty = false;
    QTimer *m_repaint = nullptr;
    QVector<Hit> m_hits;
    QVector<SortHit> m_sortHits;
};

} // namespace forkmesh
