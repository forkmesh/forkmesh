#pragma once

#include "PokerModel.h"

#include <QElapsedTimer>
#include <QHash>
#include <QPixmap>
#include <QSet>
#include <QWidget>

#include <functional>

class QTimer;

namespace forkmesh::poker {

class PokerTableView final : public QWidget
{
public:
    explicit PokerTableView(QWidget *parent = nullptr);

    void setTable(const PokerTable &table, const QList<PokerEvent> &fresh);
    void clearTable();
    const PokerTable &table() const { return m_table; }
    void setServerOffset(qint64 offset) { m_serverOffset = offset; }
    qint64 serverNow() const;
    void setAvatarSource(std::function<QPixmap(const QString &)> source);
    void avatarsChanged();
    void finishAnimations();
    bool animating() const { return !m_anims.isEmpty(); }
    int seatAt(const QPointF &point) const;

    std::function<void(int)> emptySeatClicked;
    std::function<void(const QString &, float)> soundRequested;

    QSize sizeHint() const override { return {960, 600}; }
    QSize minimumSizeHint() const override { return {520, 340}; }

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    enum class AnimKind { Card, Chips, Fold, Flip, Button, Sound };

    struct Anim {
        AnimKind kind = AnimKind::Card;
        qint64 start = 0;
        qint64 duration = 300;
        QPointF from;
        QPointF to;
        QSizeF size;
        PokerCard card;
        bool reveal = false;
        double fromAngle = 0;
        double toAngle = 0;
        qint64 amount = 0;
        QString key;
        QString sound;
        float volume = 1.0f;
        bool sounded = false;
    };

    void relayout();
    void ensureBackground();
    void paintBackground(QPainter &painter, const QSize &size);
    void paintDealer(QPainter &painter);
    void paintDealerTray(QPainter &painter);
    void paintDealerLine(QPainter &painter);
    void paintPot(QPainter &painter, const QSet<QString> &held);
    void paintBoard(QPainter &painter, const QSet<QString> &held);
    void paintSeat(QPainter &painter, const PokerSeat &seat, const PokerSeatGeometry &geometry,
                   const QSet<QString> &held);
    void paintEmptySeat(QPainter &painter, const PokerSeatGeometry &geometry);
    void paintAvatar(QPainter &painter, const PokerSeat &seat, const QPointF &centre, double radius);
    void paintPlate(QPainter &painter, const PokerSeat &seat, const QPointF &centre, double radius);
    void paintTag(QPainter &painter, const QString &text, const QPointF &anchor, const QColor &color);
    void paintButton(QPainter &painter, const QPointF &centre);
    void paintBanner(QPainter &painter);
    void paintMyHand(QPainter &painter, const PokerSeatGeometry &geometry);
    void paintAnim(QPainter &painter, const Anim &anim, qint64 now);
    void paintCard(QPainter &painter, const PokerCard &card, const QPointF &centre, const QSizeF &size,
                   double angle = 0, double widthScale = 1, double opacity = 1);
    void paintChipStack(QPainter &painter, const QPointF &base, qint64 amount, bool label, double opacity = 1);
    QPixmap cardPixmap(const PokerCard &card, const QSizeF &size);
    QPixmap chipPixmap(qint64 denomination, double radius);
    QPixmap avatarPixmap(const QString &name);

    QPointF holeCardCentre(int seat, int index) const;
    QSizeF holeCardSize(int seat) const;
    double holeCardAngle(int seat, int index) const;
    QPointF buttonPoint(int seat) const;
    bool holeCardsFaceUp(const PokerSeat &seat) const;
    QSet<QString> heldKeys(qint64 now) const;
    qint64 potShown(const QSet<QString> &held, qint64 now) const;
    void schedule(const PokerTable &previous, const QList<PokerEvent> &fresh);
    void addAnim(Anim anim);
    qint64 clockNow() const;
    qint64 animationsEnd() const;
    void startFrames();
    void frame();
    bool turnClockRunning() const;

    PokerTable m_table;
    PokerTable m_previous;
    PokerLayout m_layout;
    QPixmap m_background;
    QSize m_backgroundSize;
    qreal m_backgroundRatio = 0;
    QHash<QString, QPixmap> m_cards;
    QHash<QString, QPixmap> m_chips;
    QHash<QString, QPixmap> m_avatars;
    std::function<QPixmap(const QString &)> m_avatarSource;
    QList<Anim> m_anims;
    QElapsedTimer m_clock;
    QTimer *m_frames = nullptr;
    qint64 m_serverOffset = 0;
    int m_lastTick = -1;
    int m_hoverSeat = -1;
    qint64 m_heldPot = 0;
};

}
