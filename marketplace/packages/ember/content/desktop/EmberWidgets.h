#pragma once

#include "EmberModel.h"

#include <QColor>
#include <QList>
#include <QString>
#include <QWidget>

#include <functional>

namespace forkmesh::ember {

class RangeSlider : public QWidget
{
public:
    explicit RangeSlider(QWidget *parent = nullptr);

    void setRange(double minimum, double maximum, double step);
    void setValues(double low, double high);
    double low() const { return m_low; }
    double high() const { return m_high; }
    void setHealthTrack(bool health) { m_health = health; update(); }
    void setSingle(bool single);
    void setOnChanged(std::function<void(double, double)> callback) { m_onChanged = std::move(callback); }
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    QRectF track() const;
    double valueAt(double x) const;
    double xFor(double value) const;
    void apply(double low, double high);

    double m_min = 1;
    double m_max = 10;
    double m_step = 0.5;
    double m_low = 1;
    double m_high = 10;
    bool m_health = true;
    bool m_single = false;
    int m_drag = -1;
    int m_focusHandle = 0;
    std::function<void(double, double)> m_onChanged;
};

class HealthRing : public QWidget
{
public:
    explicit HealthRing(QWidget *parent = nullptr);
    void setHealth(double health);
    QSize sizeHint() const override { return {84, 84}; }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    double m_health = 10;
};

class TrendChart : public QWidget
{
public:
    explicit TrendChart(QWidget *parent = nullptr);
    void setPoints(const QList<insight::TrendPoint> &points);
    void setMessage(const QString &text);
    int hoveredIndex() const { return m_hover; }
    QSize sizeHint() const override { return {320, 300}; }

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    QRectF plotRect(int band) const;
    double xFor(int index) const;

    QList<insight::TrendPoint> m_points;
    QString m_message;
    int m_hover = -1;
};

struct RowItem {
    int id = -1;
    QString title;
    QString detail;
    QString value;
    double fraction = 0;
    QColor color;
    int rank = 0;
    QStringList notes;
};

class RowList : public QWidget
{
public:
    explicit RowList(QWidget *parent = nullptr);
    void setRows(const QList<RowItem> &rows);
    void setEmptyText(const QString &text);
    void setSelectedId(int id);
    const QList<RowItem> &rows() const { return m_rows; }
    void setOnActivated(std::function<void(int)> callback) { m_onActivated = std::move(callback); }
    void setOnOpened(std::function<void(int)> callback) { m_onOpened = std::move(callback); }
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return {200, 60}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    int rowHeight(const RowItem &row) const;
    int rowAt(double y) const;

    QList<RowItem> m_rows;
    QString m_empty;
    int m_hover = -1;
    int m_selectedId = -1;
    std::function<void(int)> m_onActivated;
    std::function<void(int)> m_onOpened;
};

} // namespace forkmesh::ember
