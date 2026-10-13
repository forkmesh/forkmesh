#pragma once

#include <QColor>
#include <QList>
#include <QRectF>
#include <QString>
#include <QWidget>

#include <functional>

namespace forkmesh::crm {

// The header over a pipeline's contacts: one chevron per stage, in the
// pipeline's order, each carrying how many contacts sit at that stage and a
// color of its own. A click shows only that stage; clicking it again shows
// every stage.
class CrmStageStrip final : public QWidget
{
public:
    struct Stage
    {
        QString name;  // the stored stage slug
        QString label; // what the chevron reads
        int count = 0;
    };

    explicit CrmStageStrip(QWidget *parent = nullptr);

    void setStages(const QList<Stage> &stages, const QString &selected);
    // Called with the stage picked, or an empty string to show every stage.
    std::function<void(const QString &)> onPicked;

    // "in-progress" reads "In progress".
    static QString stageLabel(const QString &name);
    // Stages named like the usual pipeline steps keep a meaningful color
    // (new blue, in progress amber, won green, lost red); others take the
    // palette in order.
    static QColor stageColor(const QString &name, int index);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    bool event(QEvent *event) override;

private:
    struct Segment
    {
        qreal left = 0;
        qreal body = 0; // width up to where the arrow tip starts
    };
    qreal arrowDepth() const;
    qreal naturalBody(int index) const;
    QList<Segment> segments() const;
    int segmentAt(const QPointF &position) const;

    QList<Stage> m_stages;
    QString m_selected;
    int m_hover = -1;
};

} // namespace forkmesh::crm
