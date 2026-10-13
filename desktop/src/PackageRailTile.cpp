#include "PackageRailTile.h"

#include <QEnterEvent>
#include <QFontMetrics>
#include <QPainter>

namespace forkmesh::market {

QColor packageRailToneColor(NativePackageRailTone tone, bool dark)
{
    switch (tone) {
    case NativePackageRailTone::Good:
        return dark ? QColor("#3fb950") : QColor("#1a7f37");
    case NativePackageRailTone::Attention:
        return dark ? QColor("#d29922") : QColor("#9a6700");
    case NativePackageRailTone::Warning:
        return dark ? QColor("#f85149") : QColor("#cf222e");
    case NativePackageRailTone::Neutral:
        return dark ? QColor("#58a6ff") : QColor("#0969da");
    case NativePackageRailTone::Paused:
        break;
    }
    return dark ? QColor("#8b949e") : QColor("#6e7781");
}

QString packageRailTileToolTip(const NativePackageRailTile &tile)
{
    QStringList lines;
    lines << (tile.stateLabel.isEmpty() ? tile.label
                                        : tile.label + QString::fromUtf8(" \xC2\xB7 ") + tile.stateLabel);
    if (!tile.count.isEmpty())
        lines << (tile.countLabel.isEmpty() ? tile.count : tile.count + QLatin1Char(' ') + tile.countLabel);
    lines << tile.details;
    if (!tile.steps.isEmpty()) {
        lines << QString() << QStringLiteral("Next steps:");
        for (const NativePackageRailStep &step : tile.steps)
            lines << QString::fromUtf8("\xE2\x80\xA2 ") + step.text;
    }
    lines << QString() << (tile.opensPage ? QStringLiteral("Click for details, right-click for next steps.")
                                          : QStringLiteral("Click for next steps."));
    return lines.join(QLatin1Char('\n'));
}

QString packageRailStateKey(const NativePackageRailTile &tile)
{
    return QStringList{tile.id, tile.count, tile.countLabel,
                       QString::number(static_cast<int>(tile.tone)),
                       tile.stateLabel, QString::number(tile.pending)}
        .join(QChar(0x1f));
}

int packageRailUrgency(const NativePackageRailTile &tile)
{
    switch (tile.tone) {
    case NativePackageRailTone::Paused:
        return 0;
    case NativePackageRailTone::Warning:
        return 3;
    case NativePackageRailTone::Attention:
        return 2;
    case NativePackageRailTone::Neutral:
    case NativePackageRailTone::Good:
        break;
    }
    return tile.pending > 0 ? 1 : 0;
}

bool packageRailNeedsMoreAttention(const NativePackageRailTile &candidate,
                                   const NativePackageRailTile &current)
{
    const int urgency = packageRailUrgency(candidate);
    const int currentUrgency = packageRailUrgency(current);
    if (urgency != currentUrgency)
        return urgency > currentUrgency;
    return urgency > 0 && candidate.pending > current.pending;
}

QString packageRailReminderTitle(const NativePackageRailTile &tile)
{
    return QString::fromUtf8("ForkMesh \xE2\x80\x94 fix %1").arg(tile.label);
}

QString packageRailReminderBody(const NativePackageRailTile &tile)
{
    QString state = tile.stateLabel.isEmpty() ? QStringLiteral("needs attention")
                                              : tile.stateLabel.toLower();
    if (tile.pending > 0)
        state += tile.pending == 1 ? QStringLiteral(", 1 check failing")
                                   : QStringLiteral(", %1 checks failing").arg(tile.pending);
    QString body = QStringLiteral("No channel has changed in the last hour. %1 needs the most attention (%2).")
                       .arg(tile.label, state);
    for (const NativePackageRailStep &step : tile.steps) {
        if (step.id == QLatin1String("open"))
            continue;
        body += QStringLiteral(" Next step: ") + step.text;
        if (!step.text.endsWith(QLatin1Char('.')))
            body += QLatin1Char('.');
        break;
    }
    return body;
}

PackageRailTileButton::PackageRailTileButton(const NativePackageRailTile &tile,
                                             bool dark, QWidget *parent)
    : QAbstractButton(parent), m_tile(tile), m_dark(dark)
{
    setFixedSize(kSize, kSize);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::TabFocus);
    setToolTip(packageRailTileToolTip(tile));
    setAccessibleName(tile.count.isEmpty()
                          ? tile.label
                          : QStringLiteral("%1, %2 %3").arg(tile.label, tile.count, tile.countLabel));
    setAccessibleDescription(tile.stateLabel);
}

QSize PackageRailTileButton::sizeHint() const
{
    return {kSize, kSize};
}

void PackageRailTileButton::enterEvent(QEnterEvent *event)
{
    m_hovered = true;
    update();
    QAbstractButton::enterEvent(event);
}

void PackageRailTileButton::leaveEvent(QEvent *event)
{
    m_hovered = false;
    update();
    QAbstractButton::leaveEvent(event);
}

void PackageRailTileButton::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    const QColor tone = packageRailToneColor(m_tile.tone, m_dark);
    const bool paused = m_tile.tone == NativePackageRailTone::Paused;
    const QRectF box = QRectF(rect()).adjusted(0.75, 0.75, -0.75, -0.75);

    QColor fill = tone;
    fill.setAlphaF((m_dark ? 0.20 : 0.12) + (m_hovered || hasFocus() ? 0.12 : 0.0));
    QPen border(tone, 1.5);
    if (paused)
        border.setStyle(Qt::DashLine);
    painter.setPen(border);
    painter.setBrush(fill);
    painter.drawRoundedRect(box, 8, 8);

    if (paused)
        painter.setOpacity(0.6);
    const QColor ink = m_dark ? QColor("#e6edf3") : QColor("#1f2328");
    const QColor muted = m_dark ? QColor("#c9d1d9") : QColor("#57606a");

    QFont countFont = font();
    countFont.setBold(true);
    const int length = int(m_tile.count.size());
    countFont.setPixelSize(length <= 2 ? 18 : length == 3 ? 16 : length == 4 ? 14 : 12);
    painter.setFont(countFont);
    painter.setPen(ink);
    painter.drawText(QRect(2, 13, width() - 4, 22), Qt::AlignCenter, m_tile.count);

    QFont nameFont = font();
    nameFont.setPixelSize(8);
    painter.setFont(nameFont);
    painter.setPen(muted);
    const QRect nameRect(3, height() - 15, width() - 6, 12);
    painter.drawText(nameRect, Qt::AlignHCenter | Qt::AlignVCenter,
                     QFontMetrics(nameFont).elidedText(m_tile.label, Qt::ElideRight, nameRect.width()));
    painter.setOpacity(1.0);

    if (m_tile.pending > 0 && !paused) {
        const QRectF badge(width() - 16, 3, 13, 13);
        painter.setPen(Qt::NoPen);
        painter.setBrush(tone);
        painter.drawEllipse(badge);
        QFont badgeFont = font();
        badgeFont.setBold(true);
        badgeFont.setPixelSize(9);
        painter.setFont(badgeFont);
        painter.setPen(m_dark ? QColor("#0d1117") : QColor("#ffffff"));
        painter.drawText(badge, Qt::AlignCenter,
                         m_tile.pending > 9 ? QStringLiteral("9+") : QString::number(m_tile.pending));
    }
}

} // namespace forkmesh::market
