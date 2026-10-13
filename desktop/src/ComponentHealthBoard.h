#pragma once

// A dense board of tiny health cards: one per source file in the overview,
// one per function after drilling into a file. Small on purpose — the whole
// codebase should be legible as a single field of colour, with the numbers
// there to explain it rather than to be read one by one.
//
// Plain QWidget (no moc): callbacks are std::function, like StatsDashboard.

#include <QString>
#include <QVector>
#include <QWidget>

#include <functional>

struct HealthCard {
    QString id;       // file path, or "File.cpp:Class::method"
    QString title;    // what the card shows large
    QString subtitle; // component name, or "line 1234"
    int loc = 0;
    int stallMs = 0;
    int findings = 0;
    int health = 100;
    bool drillable = false;
    QString tooltip;
};

class ComponentHealthBoard : public QWidget
{
public:
    explicit ComponentHealthBoard(QWidget *parent = nullptr);

    void setCards(const QVector<HealthCard> &cards);
    void setFilter(const QString &text);
    void setOnActivate(std::function<void(const QString &)> handler)
    {
        m_onActivate = std::move(handler);
    }

    // Height the board needs at its current width. The owner puts the board in
    // a scroll area and resizes it to this, like the Stats dashboard.
    int contentHeight() const { return m_contentHeight; }
    int visibleCount() const { return m_visible.size(); }

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    bool event(QEvent *event) override;

private:
    struct Placed {
        int index = 0; // into m_cards
        QRect rect;
    };

    void relayout(int width);
    void paintCard(QPainter &painter, const Placed &placed, bool hovered) const;
    int cardAt(const QPoint &pos) const;
    QColor healthColor(int health) const;

    QVector<HealthCard> m_cards;
    QVector<Placed> m_visible;
    QString m_filter;
    int m_hover = -1;
    int m_contentHeight = 0;
    std::function<void(const QString &)> m_onActivate;
};
