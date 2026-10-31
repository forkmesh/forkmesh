#pragma once

#include <QColor>
#include <QFont>
#include <QPixmap>
#include <QString>
#include <QWidget>

#include <functional>

namespace forkmesh::djstudio {

class DjModel;

namespace theme {

QColor deck(int index);
QColor background();
QColor panel();
QColor raised();
QColor line();
QColor text();
QColor dim();
QColor accent();
QFont font(int pixels, bool bold = false);
QFont mono(int pixels, bool bold = false);

}

class DjButton : public QWidget
{
public:
    explicit DjButton(const QString &label, QWidget *parent = nullptr);

    void setLabel(const QString &label);
    QString label() const { return m_label; }
    void setLit(bool lit);
    bool lit() const { return m_lit; }
    void setColor(const QColor &color);
    void setBlink(bool blink);
    void setRound(bool round) { m_round = round; update(); }
    void setFontSize(int pixels) { m_fontSize = pixels; update(); }
    void setFilled(bool filled) { m_filled = filled; update(); }

    std::function<void()> clicked;
    std::function<void()> pressed;
    std::function<void()> released;
    std::function<void()> rightClicked;

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void enterEvent(QEnterEvent *) override;
    void leaveEvent(QEvent *) override;

private:
    QString m_label;
    QColor m_color;
    bool m_lit = false;
    bool m_down = false;
    bool m_hover = false;
    bool m_blink = false;
    bool m_round = false;
    bool m_filled = false;
    int m_fontSize = 10;
};

class DjKnob : public QWidget
{
public:
    DjKnob(const QString &label, bool bipolar, QWidget *parent = nullptr);

    void setValue(double value);
    double value() const { return m_value; }
    void setColor(const QColor &color) { m_color = color; update(); }
    void setDefault(double value) { m_default = value; }
    void setCaption(const QString &caption) { m_caption = caption; update(); }
    void setMarked(bool marked) { m_marked = marked; update(); }

    std::function<void(double)> changed;
    std::function<void()> rightClicked;

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    void apply(double value);

    QString m_label;
    QString m_caption;
    bool m_bipolar = false;
    bool m_marked = false;
    double m_value = 0;
    double m_default = 0;
    double m_dragStart = 0;
    int m_pressY = 0;
    QColor m_color;
};

class DjFader : public QWidget
{
public:
    DjFader(Qt::Orientation orientation, bool bipolar, QWidget *parent = nullptr);

    void setValue(double value);
    double value() const { return m_value; }
    void setColor(const QColor &color) { m_color = color; update(); }
    void setDefault(double value) { m_default = value; }
    void setInverted(bool inverted) { m_inverted = inverted; update(); }
    void setTicks(int ticks) { m_ticks = ticks; update(); }

    std::function<void(double)> changed;

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    double fromPoint(const QPointF &point) const;
    QRectF track() const;
    void apply(double value);

    Qt::Orientation m_orientation;
    bool m_bipolar = false;
    bool m_inverted = false;
    double m_value = 0;
    double m_default = 0;
    double m_grab = 0;
    int m_ticks = 10;
    QColor m_color;
};

class DjMeter : public QWidget
{
public:
    explicit DjMeter(Qt::Orientation orientation, QWidget *parent = nullptr);

    std::function<double()> level;
    std::function<double()> hold;

protected:
    void paintEvent(QPaintEvent *) override;

private:
    Qt::Orientation m_orientation;
};

class DjPlatter : public QWidget
{
public:
    DjPlatter(DjModel *model, int deck, QWidget *parent = nullptr);

    void setDeck(int deck) { m_deck = deck; update(); }

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void resizeEvent(QResizeEvent *) override;

private:
    double angleAt(const QPointF &point) const;
    void bakeVinyl();

    DjModel *m_model;
    int m_deck;
    bool m_down = false;
    bool m_scratching = false;
    bool m_nudging = false;
    QPointF m_pressPoint;
    double m_lastAngle = 0;
    QPixmap m_vinyl;
    QPixmap m_label;
    QString m_labelFor;
    int m_wheelToken = 0;
};

class DjScrollWave : public QWidget
{
public:
    DjScrollWave(DjModel *model, int deck, QWidget *parent = nullptr);

    void setDeck(int deck) { m_deck = deck; update(); }
    void setWindow(double seconds) { m_window = seconds; update(); }
    void setShowsTitle(bool shows) { m_title = shows; }

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    DjModel *m_model;
    int m_deck;
    double m_window = 12;
    bool m_title = true;
    bool m_dragging = false;
    double m_dragX = 0;
    double m_dragTime = 0;
};

class DjOverview : public QWidget
{
public:
    DjOverview(DjModel *model, int deck, QWidget *parent = nullptr);

    void setDeck(int deck) { m_deck = deck; update(); }

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;

private:
    void seekTo(double x);

    DjModel *m_model;
    int m_deck;
};

QPixmap circular(const QImage &image, int size);
QColor waveColor(double low, double mid, double high, double brightness);

}
