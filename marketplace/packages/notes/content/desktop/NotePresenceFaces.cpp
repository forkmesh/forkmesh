#include "NotePresenceFaces.h"

#include "DesignTokens.h"
#include "OcticonIcons.h"
#include "Theme.h"

#include <QAction>
#include <QGuiApplication>
#include <QHelpEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolTip>

namespace forkmesh::notes {

namespace {
constexpr int kFace = 24;
constexpr int kFaceStep = 18;
constexpr int kRing = 2;
constexpr int kBadge = 12;

QColor faceColor(const QString &name)
{
    return QColor(QString::fromLatin1(
        Theme::kSenderPalette[noteAvatarIndex(name.toLower(),
                                              Theme::kSenderPaletteSize)]));
}

QPixmap facePixmap(const NotePeer &peer, const QPixmap &picture, int side)
{
    const qreal dpr = qGuiApp ? qGuiApp->devicePixelRatio() : 1.0;
    QPixmap pixmap(qRound(side * dpr), qRound(side * dpr));
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    const QRectF circle(0, 0, side, side);
    if (!picture.isNull()) {
        QPainterPath clip;
        clip.addEllipse(circle);
        painter.setClipPath(clip);
        painter.drawPixmap(circle.toRect(), picture);
        painter.setClipping(false);
    } else {
        painter.setPen(Qt::NoPen);
        painter.setBrush(faceColor(peer.name));
        painter.drawEllipse(circle);
        QFont font = painter.font();
        font.setPixelSize(qMax(8, qRound(side * 0.42)));
        font.setWeight(QFont::DemiBold);
        painter.setFont(font);
        painter.setPen(QColor(Qt::white));
        painter.drawText(circle, Qt::AlignCenter, noteInitials(peer.name));
    }
    return pixmap;
}
}

NotePresenceFaces::NotePresenceFaces(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("notePresenceFaces"));
    setCursor(Qt::PointingHandCursor);
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setAccessibleName(QStringLiteral("People in this note"));
}

void NotePresenceFaces::setPeers(const QList<NotePeer> &peers)
{
    m_peers = peers;
    QStringList everyone;
    for (const NotePeer &peer : peers)
        everyone << notePeerLabel(peer);
    setAccessibleDescription(everyone.join(QStringLiteral(", ")));
    updateGeometry();
    update();
}

int NotePresenceFaces::slotCount() const
{
    const int faces = qMin(int(m_peers.size()), kPresenceFaces);
    return faces + (notePresenceOverflow(int(m_peers.size())).isEmpty() ? 0 : 1);
}

QSize NotePresenceFaces::sizeHint() const
{
    const int count = slotCount();
    const int width = count ? kFace + 2 * kRing + (count - 1) * kFaceStep : 0;
    return QSize(width, kFace + 2 * kRing);
}

QSize NotePresenceFaces::minimumSizeHint() const
{
    return sizeHint();
}

QRect NotePresenceFaces::slotRect(int slot) const
{
    const int top = (height() - kFace - 2 * kRing) / 2;
    return QRect(slot * kFaceStep, top, kFace + 2 * kRing, kFace + 2 * kRing);
}

int NotePresenceFaces::slotAt(const QPoint &point) const
{
    for (int slot = slotCount() - 1; slot >= 0; --slot) {
        if (slotRect(slot).contains(point))
            return slot;
    }
    return -1;
}

void NotePresenceFaces::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QColor ring = Theme::color(Theme::Role::Bg);
    const int faces = qMin(int(m_peers.size()), kPresenceFaces);
    const QString overflow = notePresenceOverflow(int(m_peers.size()));
    for (int slot = 0; slot < slotCount(); ++slot) {
        const QRect outer = slotRect(slot);
        const QRect inner = outer.adjusted(kRing, kRing, -kRing, -kRing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(ring);
        painter.drawEllipse(outer);
        if (slot >= faces) {
            painter.setBrush(Theme::color(Theme::Role::SurfaceActive));
            painter.drawEllipse(inner);
            QFont font = painter.font();
            font.setPixelSize(qMax(8, qRound(kFace * 0.4)));
            font.setWeight(QFont::DemiBold);
            painter.setFont(font);
            painter.setPen(Theme::color(Theme::Role::FgSecondary));
            painter.drawText(inner, Qt::AlignCenter, overflow);
            continue;
        }
        const NotePeer &peer = m_peers.at(slot);
        painter.drawPixmap(inner.topLeft(),
                           facePixmap(peer,
                                      picture ? picture(peer.name) : QPixmap(),
                                      kFace));
        if (!peer.editing())
            continue;
        const QRect badge(outer.right() - kBadge + 2, outer.bottom() - kBadge + 2,
                          kBadge, kBadge);
        painter.setPen(QPen(ring, 1.5));
        painter.setBrush(Theme::color(Theme::Role::Accent));
        painter.drawEllipse(badge);
        const int glyph = kBadge - 4;
        painter.drawPixmap(
            badge.left() + (kBadge - glyph) / 2, badge.top() + (kBadge - glyph) / 2,
            forkmesh::ui::themedOcticon(QStringLiteral("pencil"),
                                        Theme::color(Theme::Role::OnAccent), glyph)
                .pixmap(glyph, glyph));
    }
}

bool NotePresenceFaces::event(QEvent *event)
{
    if (event->type() == QEvent::ToolTip) {
        auto *help = static_cast<QHelpEvent *>(event);
        const int slot = slotAt(help->pos());
        const int faces = qMin(int(m_peers.size()), kPresenceFaces);
        if (slot < 0) {
            QToolTip::hideText();
            event->ignore();
            return true;
        }
        QStringList lines;
        if (slot < faces) {
            lines << notePeerLabel(m_peers.at(slot));
        } else {
            for (int i = faces; i < m_peers.size(); ++i)
                lines << notePeerLabel(m_peers.at(i));
        }
        QToolTip::showText(help->globalPos(), lines.join(QLatin1Char('\n')), this,
                           slotRect(slot));
        return true;
    }
    return QWidget::event(event);
}

void NotePresenceFaces::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && !m_peers.isEmpty()) {
        showEveryone();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void NotePresenceFaces::showEveryone()
{
    QMenu menu(this);
    menu.setObjectName(QStringLiteral("notePresenceMenu"));
    QAction *title = menu.addAction(
        m_peers.size() == 1 ? QStringLiteral("1 other person here")
                            : QStringLiteral("%1 others here").arg(m_peers.size()));
    title->setEnabled(false);
    menu.addSeparator();
    for (const NotePeer &peer : std::as_const(m_peers)) {
        QAction *action = menu.addAction(
            QIcon(facePixmap(peer, picture ? picture(peer.name) : QPixmap(), 20)),
            notePeerLabel(peer));
        const QString name = peer.name;
        connect(action, &QAction::triggered, this, [this, name] {
            if (openProfile)
                openProfile(name);
        });
    }
    menu.exec(mapToGlobal(QPoint(0, height())));
}

}
