#pragma once

#include <QColor>
#include <QDate>
#include <QList>
#include <QRectF>
#include <QString>
#include <QWidget>

namespace forkmesh::crm {

class CrmTeamChart final : public QWidget
{
public:
    struct Day
    {
        QDate date;
        int emails = 0;
        int outreach = 0;
        int contacts = 0;
        int total() const { return emails + outreach + contacts; }
    };

    explicit CrmTeamChart(QWidget *parent = nullptr);

    void setDays(const QList<Day> &days);
    const QList<Day> &days() const { return m_days; }
    static QColor seriesColor(int series, bool dark);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    bool event(QEvent *event) override;

private:
    QRectF plotRect() const;
    int axisStep() const;
    int dayAt(const QPointF &position) const;
    bool dark() const;

    QList<Day> m_days;
    int m_hover = -1;
};

} // namespace forkmesh::crm
