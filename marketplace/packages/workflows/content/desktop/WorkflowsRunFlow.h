#pragma once

#include "WorkflowsCards.h"

#include <QElapsedTimer>
#include <QList>
#include <QPointer>
#include <QRectF>
#include <QString>
#include <QTimer>
#include <QWidget>

class QTableWidget;

namespace forkmesh::workflows {

struct FlowStep {
    QString name;
    QString status;
    QString note;
    QString detail;
};

class RunFlow final : public QWidget
{
public:
    explicit RunFlow(QWidget *parent = nullptr);
    void setTones(const Tones &tones);
    void setSteps(const QString &key, const QList<FlowStep> &steps, bool live, const QString &previousKey = QString());
    void clear();
    QString key() const { return m_key; }
    bool live() const { return m_live; }
    QList<FlowStep> steps() const;
    QString shownStatus(int index) const;
    bool animating() const { return m_timer.isActive(); }
    int focusX() const;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    bool event(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    struct Node {
        FlowStep step;
        QString from;
        qint64 shownAt = 0;
        qint64 changeAt = 0;
    };

    qint64 clock() const;
    QString statusAt(const Node &node, qint64 at) const;
    qreal slotWidth() const;
    qreal centerX(int index) const;
    qreal centerY() const;
    bool busy(qint64 at) const;
    void wake();
    void tick();

    Tones m_tones;
    QString m_key;
    QList<Node> m_nodes;
    bool m_live = false;
    QElapsedTimer m_clock;
    QTimer m_timer;
};

class RunSparkle final : public QWidget
{
public:
    static RunSparkle *burst(QTableWidget *table, const QString &runId, const Tones &tones);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    RunSparkle(QTableWidget *table, const QString &runId, const Tones &tones);
    QRectF rowRect();
    void tick();

    struct Spark {
        QPointF at;
        qreal size = 0;
        qreal delay = 0;
        qreal spin = 0;
        QColor color;
    };

    QPointer<QTableWidget> m_table;
    QString m_runId;
    Tones m_tones;
    QList<Spark> m_sparks;
    QRectF m_last;
    QRect m_painted;
    QElapsedTimer m_clock;
    QTimer m_timer;
};

} // namespace forkmesh::workflows
