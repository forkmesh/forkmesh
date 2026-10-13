#pragma once

#include <QColor>
#include <QFrame>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include <functional>

class QButtonGroup;
class QHBoxLayout;
class QLabel;
class QPushButton;
class QVBoxLayout;

namespace forkmesh::marketing {

struct DashboardColors {
    bool dark = true;
    QColor fg;
    QColor muted;
    QColor line;
    QColor lineSoft;
    QColor card;
    QColor raised;
    QColor page;
    QColor good;
    QColor goodSoft;
    QColor warn;
    QColor warnSoft;
    QColor bad;
    QColor badSoft;
    QColor primary;
    QColor series[4];

    static DashboardColors from(const QWidget *widget);
};

QColor brandColor(const QString &channel, const DashboardColors &colors);
QColor brandInk(const QString &channel, const DashboardColors &colors);
QString initials(const QString &value);
QString dashboardStyleSheet(const DashboardColors &colors);
QWidget *clearWidget(QWidget *parent = nullptr);
QPushButton *linkButton(const QString &text, const QString &tip = QString());
QWidget *segmented(const QStringList &labels, QButtonGroup *group, int checked,
                   const QString &accessibleName);

class DashboardPanel final : public QFrame
{
public:
    explicit DashboardPanel(const QString &title, QWidget *parent = nullptr);
    QLabel *titleLabel() const { return m_title; }
    QLabel *hint() const { return m_hint; }
    QHBoxLayout *tools() const { return m_tools; }
    QVBoxLayout *body() const { return m_body; }
    void setCompact();

private:
    QHBoxLayout *m_head = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_hint = nullptr;
    QHBoxLayout *m_tools = nullptr;
    QVBoxLayout *m_body = nullptr;
};

class DashboardGrid final : public QWidget
{
public:
    explicit DashboardGrid(QWidget *parent = nullptr);
    void addPanel(QWidget *panel, int span);
    void setNarrowWidth(int width) { m_narrowWidth = width; }
    void setMovable(const QString &settingsKey);

protected:
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    int columnsFor(int width) const;
    void place(int columns);
    int entryAt(const QPoint &globalPos) const;
    void markDropTarget(int index);
    void finishDrag(bool drop);
    struct Entry {
        QWidget *panel;
        int span;
        QString key;
    };
    QList<Entry> m_entries;
    int m_narrowWidth = 1080;
    int m_mode = -1;
    QString m_orderKey;
    int m_dragIndex = -1;
    int m_dropIndex = -1;
    bool m_dragging = false;
    QPoint m_dragStart;
};

class KpiStrip final : public QWidget
{
public:
    struct Kpi {
        QString label;
        QString value;
        QString tooltip;
        QString delta;
        int tone = 0;
        QString note;
        QVector<double> spark;
    };

    explicit KpiStrip(QWidget *parent = nullptr);
    void setKpis(const QList<Kpi> &kpis);
    void setCompact(bool compact);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    bool event(QEvent *event) override;

private:
    int columns(int width) const;
    QRect cellRect(int index) const;
    QList<Kpi> m_kpis;
    bool m_compact = false;
};

class BarRows final : public QWidget
{
public:
    struct Row {
        QString label;
        double value = 0;
        double max = 0;
        QString text;
        QColor color;
        double marker = -1;
        QString tooltip;
    };

    explicit BarRows(QWidget *parent = nullptr);
    void setRows(const QList<Row> &rows, int labelWidth = 96, int textWidth = 56);
    void setSelected(int index);
    void setEmptyText(const QString &text);
    std::function<void(int)> onClicked;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    bool event(QEvent *event) override;

private:
    int rowAt(const QPoint &point) const;
    QList<Row> m_rows;
    QString m_empty;
    int m_labelWidth = 96;
    int m_textWidth = 56;
    int m_hover = -1;
    int m_selected = -1;
};

class ProgressLine final : public QWidget
{
public:
    explicit ProgressLine(QWidget *parent = nullptr);
    void setProgress(double fraction, const QColor &color, double marker = -1);
    QSize sizeHint() const override { return {120, 8}; }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    double m_fraction = 0;
    double m_marker = -1;
    QColor m_color;
};

class LineChart final : public QWidget
{
public:
    struct Point {
        qint64 at = 0;
        double value = 0;
        bool logged = true;
    };
    struct Series {
        QString name;
        QColor color;
        QList<Point> points;
        QList<Point> history;
    };

    explicit LineChart(QWidget *parent = nullptr);
    void setSeries(const QList<Series> &series, qint64 start, qint64 end,
                   const QString &empty);
    QSize sizeHint() const override { return {640, 250}; }
    QSize minimumSizeHint() const override { return {320, 200}; }

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    QRectF plotRect() const;
    QList<Series> m_series;
    QString m_empty;
    qint64 m_start = 0;
    qint64 m_end = 1;
    double m_top = 1;
    double m_step = 1;
    int m_hoverX = -1;
};

class ClickFrame final : public QFrame
{
public:
    explicit ClickFrame(QWidget *parent = nullptr);
    std::function<void()> onClicked;

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    bool m_pressed = false;
};

class TileGrid final : public QWidget
{
public:
    TileGrid(int minimumTileWidth, int spacing, QWidget *parent = nullptr);
    void setTiles(const QList<QWidget *> &tiles);
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    int columnsFor(int width) const;
    int tileHeight(QWidget *tile, int width) const;
    void relayout();
    QList<QWidget *> m_tiles;
    int m_minimum;
    int m_spacing;
};

} // namespace forkmesh::marketing
