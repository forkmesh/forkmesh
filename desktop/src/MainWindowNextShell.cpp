#include "MainWindowInternal.h"
#include "MainWindowNextAccess.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QFrame>
#include <QScrollArea>
#include <QCoreApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QShortcut>
#include <QTimer>
#include <QVBoxLayout>
#include <QComboBox>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

namespace forkmesh::ui {

namespace {

const char kRailExpandedSetting[] = "ui/railExpanded";
const char kRailWidthSetting[] = "ui/railExpandedWidth";
const char kRailToggleName[] = "railToggleButton";
const char kRailHandleName[] = "railResizeHandle";
constexpr int kRailMockupWidth = 232;
constexpr int kRowIconLeft = 16;
constexpr int kRowLabelLeft = 42;
constexpr int kRowRightPad = 18;
constexpr int kRowPillHeight = 16;
constexpr int kTileIconPx = 16;
constexpr int kTileBadgeHeight = 14;

int &railWidthState()
{
    static int width = qBound(
        kRailExpandedMinWidth,
        QSettings()
            .value(QLatin1String(kRailWidthSetting), kRailMockupWidth)
            .toInt(),
        kRailExpandedMaxWidth);
    return width;
}

bool &railExpandedState()
{
    static bool expanded =
        QSettings().value(QLatin1String(kRailExpandedSetting), false).toBool();
    return expanded;
}

bool inFolderBody(const QWidget *button)
{
    const QWidget *parent = button ? button->parentWidget() : nullptr;
    return parent && parent->objectName() == QLatin1String("railFolderBody");
}

QFont sizedFont(const QWidget *widget, int px, QFont::Weight weight)
{
    QFont font = widget->font();
    font.setPixelSize(px);
    font.setWeight(weight);
    return font;
}

int pillWidth(const QFont &font, const QString &text, int height)
{
    return qMax(height, QFontMetrics(font).horizontalAdvance(text) + 8);
}

void paintPill(QPainter &painter, const QRectF &rect, const QString &text,
               const QColor &fill, const QColor &fg, const QFont &font)
{
    painter.setPen(Qt::NoPen);
    painter.setBrush(fill);
    painter.drawRoundedRect(rect, rect.height() / 2.0, rect.height() / 2.0);
    painter.setFont(font);
    painter.setPen(fg);
    painter.drawText(rect, Qt::AlignCenter, text);
}

void paintBody(QPainter &painter, const QRectF &body, bool accentAction,
               bool checked, bool hovered)
{
    QColor fill;
    if (accentAction)
        fill = Theme::color(hovered ? Theme::Role::AccentHover
                                    : Theme::Role::Accent);
    else if (checked)
        fill = Theme::color(Theme::Role::SurfaceActive);
    else if (hovered)
        fill = Theme::color(Theme::Role::SurfaceRaised);
    if (!fill.isValid())
        return;
    painter.setPen(Qt::NoPen);
    painter.setBrush(fill);
    painter.drawRoundedRect(body, 8, 8);
}

QPixmap sidebarGlyph(const QColor &color, qreal ratio)
{
    const int side = qMax(1, qRound(16 * ratio));
    QPixmap pixmap(side, side);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.scale(ratio, ratio);
    QPen pen(color, 1.3);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(1.5, 2.5, 13, 11), 2.5, 2.5);
    painter.drawLine(QPointF(6.2, 2.8), QPointF(6.2, 13.2));
    painter.end();
    pixmap.setDevicePixelRatio(ratio);
    return pixmap;
}

void relayoutRail(QBoxLayout *layout)
{
    if (!layout)
        return;
    for (int i = 0; i < layout->count(); ++i) {
        QLayoutItem *item = layout->itemAt(i);
        if (QWidget *widget = item ? item->widget() : nullptr) {
            widget->updateGeometry();
            widget->update();
        }
    }
    layout->invalidate();
}

void applySectionCardMargins(QWidget *card)
{
    if (!card || !card->layout())
        return;
    if (Theme::isNext())
        card->layout()->setContentsMargins(16, 14, 16, 16);
    else
        card->layout()->setContentsMargins(0, 0, 0, 0);
}

void refreshSectionCards(QWidget *root)
{
    if (!root)
        return;
    const QList<QWidget *> widgets = root->findChildren<QWidget *>();
    for (QWidget *widget : widgets) {
        if (widget->property("sectionCard").toBool())
            applySectionCardMargins(widget);
    }
}

void refreshShellContentCard(QWidget *card)
{
    if (!card || !card->parentWidget() || !card->layout())
        return;
    const bool next = Theme::isNext();
    card->layout()->setContentsMargins(next ? QMargins(4, 4, 4, 4) : QMargins());
    QList<QLayout *> pending = {card->parentWidget()->layout()};
    while (!pending.isEmpty()) {
        QLayout *layout = pending.takeFirst();
        if (!layout)
            continue;
        if (layout->indexOf(card) >= 0) {
            layout->setContentsMargins(next ? QMargins(0, 0, 8, 8) : QMargins());
            return;
        }
        for (int i = 0; i < layout->count(); ++i)
            if (QLayout *child = layout->itemAt(i)->layout())
                pending.append(child);
    }
}

void refreshRailToggle(QPushButton *button)
{
    if (!button)
        return;
    button->setVisible(Theme::isNext());
    button->setIcon(QIcon(sidebarGlyph(Theme::color(Theme::Role::FgMuted),
                                       button->devicePixelRatioF())));
    button->setToolTip(railExpanded() ? QStringLiteral("Collapse sidebar")
                                      : QStringLiteral("Expand sidebar"));
}

class RailResizeHandle : public QWidget
{
public:
    RailResizeHandle(MainWindow &window, QWidget *parent = nullptr)
        : QWidget(parent), m_window(window)
    {
        setObjectName(QLatin1String(kRailHandleName));
        setCursor(Qt::SizeHorCursor);
        setFixedWidth(5);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
        setMouseTracking(true);
        setToolTip(QStringLiteral("Drag to resize the sidebar; double-click to "
                                  "reset its width"));
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        const bool lit = m_dragging || underMouse();
        painter.fillRect(QRect(width() - 1, 0, 1, height()),
                         Theme::color(Theme::Role::Border));
        if (lit)
            painter.fillRect(QRect(width() - 3, 0, 3, height()),
                             Theme::color(Theme::Role::Accent, 160));
    }

    void enterEvent(QEnterEvent *event) override
    {
        update();
        QWidget::enterEvent(event);
    }

    void leaveEvent(QEvent *event) override
    {
        update();
        QWidget::leaveEvent(event);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton)
            return QWidget::mousePressEvent(event);
        m_dragging = true;
        m_startX = event->globalPosition().toPoint().x();
        m_startWidth = railExpandedItemWidth();
        update();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!m_dragging)
            return QWidget::mouseMoveEvent(event);
        NextAccess::setRailWidth(
            m_window,
            m_startWidth + event->globalPosition().toPoint().x() - m_startX,
            false);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (!m_dragging)
            return QWidget::mouseReleaseEvent(event);
        m_dragging = false;
        NextAccess::setRailWidth(m_window, railExpandedItemWidth(), true);
        update();
    }

    void mouseDoubleClickEvent(QMouseEvent *) override
    {
        NextAccess::setRailWidth(m_window, kRailMockupWidth, true);
    }

private:
    MainWindow &m_window;
    bool m_dragging = false;
    int m_startX = 0;
    int m_startWidth = 0;
};

void refreshRailHandle(QWidget *handle)
{
    if (handle)
        handle->setVisible(railExpanded());
}

}

bool railExpanded()
{
    return railExpandedState() && Theme::isNext();
}

void setRailExpanded(bool expanded)
{
    railExpandedState() = expanded;
}

int railExpandedItemWidth()
{
    return railWidthState();
}

void setRailExpandedItemWidth(int width)
{
    railWidthState() = qBound(kRailExpandedMinWidth, width, kRailExpandedMaxWidth);
}

bool railButtonRowLayout(const QWidget *button, bool captionAlwaysVisible)
{
    return !captionAlwaysVisible && railExpanded() && !inFolderBody(button);
}

QSize railButtonSizeHook(const QWidget *button, const RailButtonPaintState &state)
{
    if (!railButtonRowLayout(button, state.captionAlwaysVisible))
        return QSize();
    return QSize(railExpandedItemWidth(), kRailRowHeight);
}

bool paintRailButtonHook(QWidget *button, QPainter &painter,
                         const RailButtonPaintState &state)
{
    const bool row = railButtonRowLayout(button, state.captionAlwaysVisible);
    if (!Theme::isNext() && !row)
        return false;

    auto *push = qobject_cast<QPushButton *>(button);
    const bool enabled = button->isEnabled();
    const bool hovered = enabled && button->underMouse();
    const bool checked = push && push->isChecked();
    const bool lit = enabled && (checked || hovered);
    const bool accentAction = enabled && state.accent;
    const QColor fg = !enabled ? Theme::color(Theme::Role::FgSubtle)
                      : accentAction ? Theme::color(Theme::Role::OnAccent)
                      : state.alert ? Theme::color(Theme::Role::Warning)
                      : Theme::color(lit ? Theme::Role::Fg
                                         : Theme::Role::FgMuted);
    const QColor canvas = Theme::color(Theme::Role::BgSubtle);
    const int width = button->width();
    const int height = button->height();

    const QRectF body = row ? QRectF(8, 1, width - 16, height - 2)
                            : QRectF(3, 2, width - 6, height - 4);
    paintBody(painter, body, accentAction, checked, hovered);

    const int iconPx = kRailIconPx;
    const QRect iconRect =
        row ? QRect(kRowIconLeft, (height - iconPx) / 2, iconPx, iconPx)
            : QRect((width - iconPx) / 2,
                    state.captionVisible ? 6 : (height - iconPx) / 2, iconPx,
                    iconPx);
    if (state.iconPainter)
        state.iconPainter(painter, iconRect, fg);
    else if (push && button->property("fmSpinning").toBool() && !push->icon().isNull())
        push->icon().paint(&painter, iconRect, Qt::AlignCenter, QIcon::Normal,
                           QIcon::Off);
    else
        painter.drawPixmap(iconRect.topLeft(),
                           tintedOcticonPixmap(state.iconName, fg, iconPx));

    const QString badgeText =
        state.badge > 0 ? QString::number(state.badge) : QString();
    const QString syncCount = state.pendingSync > 99
                                  ? QStringLiteral("99+")
                                  : QString::number(state.pendingSync);

    if (row) {
        const QFont pillFont = sizedFont(button, 10, QFont::Medium);
        double right = width - kRowRightPad;
        const double pillTop = (height - kRowPillHeight) / 2.0;
        if (state.syncing) {
            const int s = 14;
            painter.drawPixmap(
                QPointF(right - s, (height - s) / 2.0),
                refreshPixmap(Theme::color(Theme::Role::AccentFg),
                              state.spinAngle, s));
            right -= s + 4;
        } else if (state.badge > 0) {
            if (state.badgeUrgent) {
                const int w = pillWidth(pillFont, badgeText, kRowPillHeight);
                paintPill(painter, QRectF(right - w, pillTop, w, kRowPillHeight),
                          badgeText, Theme::color(Theme::Role::Danger),
                          Theme::color(Theme::Role::OnAccent), pillFont);
                right -= w + 4;
            } else {
                const QFont countFont = sizedFont(button, 11, QFont::Normal);
                const QString count = QLocale().toString(state.badge);
                const int w = QFontMetrics(countFont).horizontalAdvance(count);
                painter.setFont(countFont);
                painter.setPen(Theme::color(Theme::Role::FgSubtle));
                painter.drawText(QRectF(right - w, 0, w, height),
                                 Qt::AlignVCenter | Qt::AlignRight, count);
                right -= w + 6;
            }
        }
        if (state.pendingSync > 0) {
            const QString text = syncCount + QChar(0x2191);
            const int w = pillWidth(pillFont, text, kRowPillHeight);
            paintPill(painter, QRectF(right - w, pillTop, w, kRowPillHeight),
                      text, Theme::color(Theme::Role::WarningSubtle),
                      Theme::color(Theme::Role::Warning), pillFont);
            right -= w + 4;
        }
        const QFont labelFont = sizedFont(button, 13, QFont::Normal);
        painter.setFont(labelFont);
        painter.setPen(fg);
        const int labelWidth = qMax(0, int(right) - kRowLabelLeft - 4);
        painter.drawText(QRect(kRowLabelLeft, 0, labelWidth, height),
                         Qt::AlignVCenter | Qt::AlignLeft,
                         QFontMetrics(labelFont).elidedText(
                             state.label, Qt::ElideRight, labelWidth));
    } else {
        if (state.pendingSync > 0) {
            const QFont pillFont = sizedFont(button, 9, QFont::DemiBold);
            const int h = 14;
            const QFontMetrics metrics(pillFont);
            const auto widthOf = [&](const QString &text) {
                return qMax(h, metrics.horizontalAdvance(text) + 8);
            };
            double rightLimit = width - 1;
            if (state.syncing)
                rightLimit = iconRect.right() - 14 / 2 + 2;
            else if (state.badge > 0)
                rightLimit = iconRect.right() + h / 2.0 + 1 - widthOf(badgeText);
            QString text = syncCount + QChar(0x2191);
            const double preferredLeft = iconRect.left() - 9;
            if (qMin(preferredLeft, rightLimit - widthOf(text)) < 1)
                text = syncCount;
            const int w = widthOf(text);
            const double left = qMax(1.0, qMin(preferredLeft, rightLimit - w));
            const QRectF pill(left, qMax(0.0, double(iconRect.top() - 5)), w, h);
            painter.setPen(QPen(canvas, 1));
            painter.setBrush(Theme::color(Theme::Role::Warning));
            painter.drawRoundedRect(pill, h / 2.0, h / 2.0);
            painter.setFont(pillFont);
            painter.setPen(Theme::color(Theme::Role::OnAccent));
            painter.drawText(pill, Qt::AlignCenter, text);
        }
        if (state.captionVisible) {
            const QFont captionFont = sizedFont(button, 10, QFont::Medium);
            painter.setFont(captionFont);
            painter.setPen(fg);
            painter.drawText(QRect(1, iconRect.bottom() + 2, width - 2, 14),
                             Qt::AlignHCenter | Qt::AlignTop,
                             QFontMetrics(captionFont).elidedText(
                                 state.label, Qt::ElideRight, width - 2));
        }
        if (state.syncing) {
            const int s = 14;
            const QPoint at(iconRect.right() - s / 2 + 4,
                            qMax(0, iconRect.top() - 4));
            painter.setPen(Qt::NoPen);
            painter.setBrush(canvas);
            painter.drawEllipse(QRect(at, QSize(s, s)).adjusted(-1, -1, 1, 1));
            painter.drawPixmap(at,
                               refreshPixmap(Theme::color(Theme::Role::AccentFg),
                                             state.spinAngle, s));
        } else if (state.badge > 0) {
            const QFont pillFont = sizedFont(button, 9, QFont::DemiBold);
            const int h = 14;
            const int w = pillWidth(pillFont, badgeText, h);
            const QRectF pill(iconRect.right() - w + h / 2.0 + 2,
                              qMax(0.0, double(iconRect.top() - 5)), w, h);
            paintPill(painter, pill, badgeText,
                      Theme::color(state.badgeUrgent ? Theme::Role::Danger
                                                     : Theme::Role::Accent),
                      Theme::color(Theme::Role::OnAccent), pillFont);
        }
    }

    if (!state.stateGlyph.isEmpty()) {
        const int s = 11;
        const QPoint at(qMin(width - s - 2, iconRect.right() - 3),
                        qMax(0, iconRect.top() - 5));
        painter.setPen(Qt::NoPen);
        painter.setBrush(canvas);
        painter.drawEllipse(QRect(at, QSize(s, s)).adjusted(-2, -2, 2, 2));
        const QColor glyphColor =
            currentThemeIsDark() ? state.stateGlyphDark : state.stateGlyphLight;
        painter.drawPixmap(
            at, tintedOcticonPixmap(state.stateGlyph,
                                    enabled && glyphColor.isValid() ? glyphColor
                                                                    : fg,
                                    s));
    }

    if (state.blink && state.blinkOn) {
        const int d = 8;
        const QRect lamp(iconRect.right() - 2, iconRect.bottom() - d + 1, d, d);
        painter.setPen(Qt::NoPen);
        painter.setBrush(canvas);
        painter.drawEllipse(lamp.adjusted(-2, -2, 2, 2));
        painter.setBrush(Theme::color(Theme::Role::Success));
        painter.drawEllipse(lamp);
    }

    if (state.desktopOffline) {
        const int d = 8;
        const QRect dot(iconRect.left() - d + 2, iconRect.bottom() - d + 1, d, d);
        painter.save();
        painter.setOpacity(state.desktopOfflineLevel);
        painter.setPen(Qt::NoPen);
        painter.setBrush(canvas);
        painter.drawEllipse(dot.adjusted(-2, -2, 2, 2));
        painter.setBrush(Theme::color(Theme::Role::Warning));
        painter.drawEllipse(dot);
        painter.restore();
    }
    return true;
}

QSize tabButtonSizeHook(const QWidget *button, const TabButtonPaintState &state)
{
    if (!state.inRail || !railExpanded() || inFolderBody(button))
        return QSize();
    return QSize(railExpandedItemWidth(), kRailRowHeight);
}

bool paintTabButtonHook(QWidget *button, QPainter &painter,
                        const TabButtonPaintState &state)
{
    const bool row = state.inRail && railExpanded() && !inFolderBody(button);
    if (!Theme::isNext() && !row)
        return false;

    auto *push = qobject_cast<QPushButton *>(button);
    const bool enabled = button->isEnabled();
    const bool hovered = enabled && button->underMouse();
    const bool checked = push && push->isChecked();
    const bool action = state.form == 0;
    const bool tab = state.form == 1;
    QColor fg = action ? Theme::color(Theme::Role::Fg)
                       : Theme::color(checked || hovered ? Theme::Role::Fg
                                                         : Theme::Role::FgMuted);
    if (!enabled)
        fg = Theme::color(Theme::Role::FgSubtle);
    const bool accented = state.accent.isValid() && enabled;
    const QColor glyphColor = accented ? state.accent : fg;
    const QColor captionColor =
        accented && (checked || hovered) ? state.accent : fg;
    const int width = button->width();
    const int height = button->height();
    const QString text = push ? push->text() : QString();

    if (row) {
        paintBody(painter, QRectF(4, 1, width - 8, height - 2), false,
                  tab && checked, hovered);
    } else if (action) {
        if (hovered)
            paintBody(painter, QRectF(0.5, 0.5, width - 1, height - 1), false,
                      false, true);
    } else if (tab && checked) {
        if (state.inRail)
            paintBody(painter, QRectF(3, 2, width - 6, height - 4), false, true,
                      false);
        else
            painter.fillRect(QRect(0, height - 2, width, 2),
                             accented ? state.accent
                                      : Theme::color(Theme::Role::AccentFg));
    }

    const int iconTop = state.badgeAbove ? kTileBadgeHeight + 1 : 6;
    const QRect iconRect =
        row ? QRect(kRowIconLeft, (height - kTileIconPx) / 2, kTileIconPx,
                    kTileIconPx)
            : QRect((width - kTileIconPx) / 2, iconTop, kTileIconPx, kTileIconPx);
    if (state.iconName.isEmpty()) {
        if (push)
            push->icon().paint(&painter, iconRect, Qt::AlignCenter,
                               enabled ? QIcon::Normal : QIcon::Disabled);
    } else {
        painter.drawPixmap(iconRect.topLeft(),
                           tintedOcticonPixmap(state.iconName, glyphColor,
                                               kTileIconPx));
    }

    if (state.attentionLight) {
        QColor light = Theme::color(Theme::Role::AccentFg);
        light.setAlpha(state.attentionLightOn ? 255 : 85);
        painter.setPen(Qt::NoPen);
        painter.setBrush(light);
        painter.drawEllipse(
            QRectF(iconRect.right() - 2, iconRect.top() - 2, 6, 6));
    }

    if (row) {
        const QFont pillFont = sizedFont(button, 10, QFont::Medium);
        double right = width - kRowRightPad;
        const double pillTop = (height - kRowPillHeight) / 2.0;
        if (state.alertBadge > 0) {
            const QString count = formatCount(state.alertBadge);
            const int w = pillWidth(pillFont, count, kRowPillHeight);
            paintPill(painter, QRectF(right - w, pillTop, w, kRowPillHeight),
                      count, Theme::color(Theme::Role::Danger),
                      Theme::color(Theme::Role::OnAccent), pillFont);
            right -= w + 4;
        }
        if (state.badge > 0) {
            const QString count = formatCount(state.badge);
            const int w = pillWidth(pillFont, count, kRowPillHeight);
            paintPill(painter, QRectF(right - w, pillTop, w, kRowPillHeight),
                      count,
                      Theme::color(checked ? Theme::Role::BorderStrong
                                           : Theme::Role::SurfaceActive),
                      Theme::color(Theme::Role::FgSecondary), pillFont);
            right -= w + 4;
        }
        const QFont labelFont = sizedFont(button, 13, QFont::Medium);
        painter.setFont(labelFont);
        painter.setPen(captionColor);
        const int labelWidth = qMax(0, int(right) - kRowLabelLeft - 4);
        painter.drawText(QRect(kRowLabelLeft, 0, labelWidth, height),
                         Qt::AlignVCenter | Qt::AlignLeft,
                         QFontMetrics(labelFont).elidedText(
                             text, Qt::ElideRight, labelWidth));
        return true;
    }

    const QFont captionFont = sizedFont(button, 10, QFont::Medium);
    painter.setFont(captionFont);
    painter.setPen(captionColor);
    painter.drawText(QRect(2, iconRect.bottom() + 2, width - 4, 14),
                     Qt::AlignHCenter | Qt::AlignTop,
                     QFontMetrics(captionFont).elidedText(text, Qt::ElideRight,
                                                          width - 4));

    const QFont badgeFont = sizedFont(button, 9, QFont::DemiBold);
    const auto badgeWidth = [&](qint64 count) {
        return count <= 0 ? 0
                          : pillWidth(badgeFont, formatCount(count),
                                      kTileBadgeHeight);
    };
    const int blue = badgeWidth(state.badge);
    const int red = badgeWidth(state.alertBadge);
    const int strip = blue + red + (blue > 0 && red > 0 ? 2 : 0);
    double rightEdge = state.badgeAbove
                           ? (width + strip) / 2.0
                           : iconRect.right() + kTileBadgeHeight / 2.0 + 2;
    const auto paintBadge = [&](qint64 count, const QColor &fill) {
        if (count <= 0)
            return;
        const int w = badgeWidth(count);
        const QRectF badge(qMax(0.0, rightEdge - w),
                           state.badgeAbove
                               ? 0.0
                               : qMax(0.0, double(iconRect.top() - 5)),
                           w, kTileBadgeHeight);
        paintPill(painter, badge, formatCount(count), fill,
                  fill.lightness() > 155 ? Theme::color(Theme::Role::Bg)
                                         : Theme::color(Theme::Role::OnAccent),
                  badgeFont);
        rightEdge = badge.left() - 2;
    };
    paintBadge(state.badge,
               accented ? state.accent : Theme::color(Theme::Role::Accent));
    paintBadge(state.alertBadge, Theme::color(Theme::Role::Danger));
    return true;
}

bool paintRailFolderPopoutHook(QWidget *popout, QPainter &painter,
                               bool highlighted)
{
    if (!Theme::isNext())
        return false;
    const QRectF frame = QRectF(popout->rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    painter.setPen(Theme::color(Theme::Role::BorderStrong));
    painter.setBrush(Theme::color(Theme::Role::Surface));
    painter.drawRoundedRect(frame, 8, 8);
    if (highlighted) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(Theme::color(Theme::Role::Accent, 50));
        painter.drawRoundedRect(frame, 8, 8);
    }
    return true;
}

QColor toggleSwitchTrackHook(bool checked, const QColor &classic)
{
    if (!Theme::isNext())
        return classic;
    return Theme::color(checked ? Theme::Role::Accent
                                : Theme::Role::BorderStrong);
}

QColor nextHoverFill(const QColor &classic)
{
    return Theme::isNext() ? Theme::color(Theme::Role::SurfaceRaised) : classic;
}

QColor nextAccentTint(const QColor &classic)
{
    if (!Theme::isNext())
        return classic;
    return Theme::color(Theme::Role::Accent, classic.alpha());
}

QColor nextSelectedFill(const QColor &classic)
{
    return Theme::isNext() ? Theme::color(Theme::Role::SurfaceActive) : classic;
}

void sectionCardHook(QWidget *card)
{
    if (!card)
        return;
    card->setProperty("sectionCard", true);
    card->setAttribute(Qt::WA_StyledBackground, true);
    applySectionCardMargins(card);
}

void NextAccess::install(MainWindow &window)
{
    kit::syncStockWidgets();
    installWelcome(window);
    installCommandPalette(window);
    refreshRailToggle(
        window.findChild<QPushButton *>(QLatin1String(kRailToggleName)));
    refreshShellContentCard(
        window.findChild<QWidget *>(QStringLiteral("shellContentCard")));
    refreshSidebarChrome(window);
}

namespace {

QList<void (*)(MainWindow &)> &themeHooks()
{
    static QList<void (*)(MainWindow &)> hooks;
    return hooks;
}

void runThemeHooks(MainWindow &window)
{
    const QList<void (*)(MainWindow &)> hooks = themeHooks();
    for (void (*hook)(MainWindow &) : hooks)
        hook(window);
}

}

void NextAccess::addThemeHook(void (*hook)(MainWindow &))
{
    if (hook && !themeHooks().contains(hook))
        themeHooks().append(hook);
}

void NextAccess::themeChanged(MainWindow &window)
{
    kit::syncStockWidgets();
    refreshRailToggle(
        window.findChild<QPushButton *>(QLatin1String(kRailToggleName)));
    refreshRailHandle(window.findChild<QWidget *>(QLatin1String(kRailHandleName)));
    refreshSectionCards(&window);
    refreshShellContentCard(
        window.findChild<QWidget *>(QStringLiteral("shellContentCard")));
    refreshSidebarChrome(window);
    if (auto *designCombo =
            window.findChild<QComboBox *>(QStringLiteral("appDesignCombo"))) {
        const int index = designCombo->findData(
            Theme::designSettingValue(Theme::activeDesign()));
        if (index >= 0 && index != designCombo->currentIndex()) {
            const QSignalBlocker blocker(designCombo);
            designCombo->setCurrentIndex(index);
        }
    }
    if (window.m_appNavigationRailLayout) {
        window.applyRailCaptionPreference();
        relayoutRail(window.m_appNavigationRailLayout);
        if (window.m_appNavigationRail)
            window.m_appNavigationRail->update();
    }
    runThemeHooks(window);
}

void NextAccess::showSection(MainWindow &window, int index)
{
    window.showSection(index);
}

int NextAccess::currentSection(const MainWindow &window)
{
    return window.m_sectionStack ? window.m_sectionStack->currentIndex() : -1;
}

int NextAccess::sectionCount(const MainWindow &window)
{
    return window.m_sectionStack ? window.m_sectionStack->count() : 0;
}

void NextAccess::setRailExpanded(MainWindow &window, bool expanded)
{
    QSettings().setValue(QLatin1String(kRailExpandedSetting), expanded);
    forkmesh::ui::setRailExpanded(expanded);
    window.applyRailCaptionPreference();
    relayoutRail(window.m_appNavigationRailLayout);
    refreshRailToggle(
        window.findChild<QPushButton *>(QLatin1String(kRailToggleName)));
    refreshRailHandle(window.findChild<QWidget *>(QLatin1String(kRailHandleName)));
    if (auto *check = window.findChild<QCheckBox *>(
            QStringLiteral("railExpandedCheck"))) {
        const QSignalBlocker blocker(check);
        check->setChecked(expanded);
    }
    refreshSidebarChrome(window);
}

void NextAccess::toggleRailExpanded(MainWindow &window)
{
    setRailExpanded(window, !railExpandedState());
}

void NextAccess::setRailWidth(MainWindow &window, int width, bool remember)
{
    const int before = railExpandedItemWidth();
    forkmesh::ui::setRailExpandedItemWidth(width);
    if (remember)
        QSettings().setValue(QLatin1String(kRailWidthSetting),
                             railExpandedItemWidth());
    if (before == railExpandedItemWidth())
        return;
    window.applyRailCaptionPreference();
    relayoutRail(window.m_appNavigationRailLayout);
    refreshSidebarChrome(window);
}

QWidget *NextAccess::railResizeHandle(MainWindow &window)
{
    auto *handle = new RailResizeHandle(window);
    refreshRailHandle(handle);
    return handle;
}

QWidget *NextAccess::overlayParent(MainWindow &window)
{
    return window.centralWidget();
}

QWidget *NextAccess::railToggleButton(MainWindow &window)
{
    auto *button = new QPushButton;
    button->setObjectName(QLatin1String(kRailToggleName));
    button->setCursor(Qt::PointingHandCursor);
    button->setFixedSize(28, 28);
    button->setIconSize(QSize(16, 16));
    button->setFocusPolicy(Qt::NoFocus);
    button->setAccessibleName(QStringLiteral("Toggle sidebar"));
    button->setFlat(true);
    refreshRailToggle(button);
    QObject::connect(button, &QPushButton::clicked, &window,
                     [&window] { toggleRailExpanded(window); });
    return button;
}


namespace {

QPixmap sidebarLogo(int px)
{
    const qreal ratio = qApp ? qApp->devicePixelRatio() : 1.0;
    QPixmap pixmap(QSize(px, px) * ratio);
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Theme::color(Theme::Role::Fg));
    painter.drawRoundedRect(QRectF(0, 0, px, px), 7, 7);
    const int glyph = px * 4 / 7;
    painter.drawPixmap((px - glyph) / 2, (px - glyph) / 2,
                       tintedOcticonPixmap(QStringLiteral("repo-forked"),
                                           Theme::color(Theme::Role::Bg), glyph));
    return pixmap;
}

QPixmap sidebarAvatar(const QString &name, bool online, int px)
{
    const qreal ratio = qApp ? qApp->devicePixelRatio() : 1.0;
    QPixmap pixmap(QSize(px + 2, px + 2) * ratio);
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Theme::color(Theme::Role::SurfaceActive));
    painter.drawEllipse(QRectF(0, 0, px, px));
    QString initials;
    for (const QString &part :
         name.split(QRegularExpression(QStringLiteral("[\\s._@-]+")),
                    Qt::SkipEmptyParts)) {
        if (part.at(0).isLetter())
            initials += part.at(0).toUpper();
        if (initials.size() == 2)
            break;
    }
    if (initials.isEmpty())
        initials = QStringLiteral("?");
    QFont font = painter.font();
    font.setPixelSize(px * 2 / 5);
    font.setWeight(QFont::Medium);
    painter.setFont(font);
    painter.setPen(Theme::color(Theme::Role::FgSecondary));
    painter.drawText(QRectF(0, 0, px, px), Qt::AlignCenter, initials);
    const qreal dot = px * 0.36;
    const QRectF lamp(px - dot + 1, px - dot + 1, dot, dot);
    painter.setPen(QPen(Theme::color(Theme::Role::BgSubtle), 2));
    painter.setBrush(Theme::color(online ? Theme::Role::Success
                                         : Theme::Role::FgSubtle));
    painter.drawEllipse(lamp);
    return pixmap;
}

QLabel *sidebarText(QWidget *parent, const char *name)
{
    auto *label = new QLabel(parent);
    label->setObjectName(QLatin1String(name));
    label->setTextFormat(Qt::PlainText);
    label->setAttribute(Qt::WA_TransparentForMouseEvents);
    return label;
}

QPushButton *sidebarIdentityButton(const char *name, QLabel **glyph,
                                   QLabel **title, QLabel **subtitle,
                                   const QString &chevron)
{
    auto *button = new QPushButton;
    button->setObjectName(QLatin1String(name));
    button->setCursor(Qt::PointingHandCursor);
    button->setFixedHeight(44);
    auto *row = new QHBoxLayout(button);
    row->setContentsMargins(6, 0, 8, 0);
    row->setSpacing(10);
    *glyph = sidebarText(button, "sidebarGlyph");
    row->addWidget(*glyph);
    auto *texts = new QVBoxLayout;
    texts->setContentsMargins(0, 0, 0, 0);
    texts->setSpacing(0);
    *title = sidebarText(button, "sidebarTitle");
    *subtitle = sidebarText(button, "sidebarSubtitle");
    texts->addWidget(*title);
    texts->addWidget(*subtitle);
    row->addLayout(texts, 1);
    auto *arrow = sidebarText(button, "sidebarChevron");
    arrow->setProperty("glyph", chevron);
    row->addWidget(arrow);
    return button;
}

QString elided(const QLabel *label, const QString &text, int width)
{
    return QFontMetrics(label->font()).elidedText(text, Qt::ElideRight,
                                                  qMax(20, width));
}

}

QWidget *NextAccess::buildSidebarHeader(MainWindow &window)
{
    auto *header = new QWidget;
    header->setObjectName(QStringLiteral("sidebarHeader"));
    auto *layout = new QVBoxLayout(header);
    layout->setContentsMargins(10, 8, 10, 6);
    layout->setSpacing(6);
    QLabel *glyph = nullptr;
    QLabel *title = nullptr;
    QLabel *subtitle = nullptr;
    QPushButton *workspace = sidebarIdentityButton(
        "sidebarWorkspace", &glyph, &title, &subtitle, QStringLiteral("chevron-down"));
    workspace->setToolTip(QStringLiteral("Account, instances and nodes"));
    layout->addWidget(workspace);
    QObject::connect(workspace, &QPushButton::clicked, &window,
                     [&window, workspace] {
                         window.setProperty(
                             "accountMenuAt",
                             workspace->mapToGlobal(QPoint(0, workspace->height() + 4)));
                         window.showUserAccountMenu();
                     });

    auto *fresh = new QPushButton;
    fresh->setObjectName(QStringLiteral("sidebarNewSession"));
    fresh->setCursor(Qt::PointingHandCursor);
    fresh->setFixedHeight(32);
    fresh->setToolTip(QStringLiteral("Start a new agent session"));
    auto *row = new QHBoxLayout(fresh);
    row->setContentsMargins(8, 0, 6, 0);
    row->setSpacing(8);
    auto *plus = sidebarText(fresh, "sidebarNewSessionIcon");
    row->addWidget(plus);
    auto *label = sidebarText(fresh, "sidebarNewSessionText");
    label->setText(QStringLiteral("New session"));
    row->addWidget(label, 1);
#ifdef Q_OS_MACOS
    const QStringList keys = {QString::fromUtf8("\xE2\x8C\x98"), QStringLiteral("N")};
#else
    const QStringList keys = {QStringLiteral("Ctrl"), QStringLiteral("N")};
#endif
    for (const QString &key : keys) {
        auto *kbd = sidebarText(fresh, "sidebarKbd");
        kbd->setText(key);
        kbd->setAlignment(Qt::AlignCenter);
        row->addWidget(kbd);
    }
    layout->addWidget(fresh);
    QObject::connect(fresh, &QPushButton::clicked, &window,
                     [&window] { window.startNewSessionInSessionsPage(); });
    auto *shortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_N), &window);
    shortcut->setContext(Qt::WindowShortcut);
    QObject::connect(shortcut, &QShortcut::activated, &window, [&window] {
        if (Theme::isNext())
            window.startNewSessionInSessionsPage();
    });

    auto *refresh = new QTimer(header);
    refresh->setInterval(2000);
    QObject::connect(refresh, &QTimer::timeout, header,
                     [&window] { refreshSidebarChrome(window); });
    refresh->start();
    return header;
}

QWidget *NextAccess::buildSidebarFooter(MainWindow &window)
{
    auto *footer = new QWidget;
    footer->setObjectName(QStringLiteral("sidebarFooter"));
    footer->setAttribute(Qt::WA_StyledBackground, true);
    auto *layout = new QVBoxLayout(footer);
    layout->setContentsMargins(10, 6, 10, 8);
    layout->setSpacing(0);
    QLabel *glyph = nullptr;
    QLabel *title = nullptr;
    QLabel *subtitle = nullptr;
    QPushButton *account = sidebarIdentityButton(
        "sidebarAccount", &glyph, &title, &subtitle, QStringLiteral("chevron-down"));
    account->setToolTip(QStringLiteral("Account, status and sign out"));
    layout->addWidget(account);
    QObject::connect(account, &QPushButton::clicked, &window, [&window, account] {
        window.setProperty("accountMenuAbove", account->mapToGlobal(QPoint(0, -4)));
        window.showUserAccountMenu();
    });
    return footer;
}

void NextAccess::refreshSidebarChrome(MainWindow &window)
{
    auto *header = window.findChild<QWidget *>(QStringLiteral("sidebarHeader"));
    auto *footer = window.findChild<QWidget *>(QStringLiteral("sidebarFooter"));
    const bool next = Theme::isNext();
    const bool expanded = railExpanded();
    for (QWidget *part : {header, footer})
        if (part)
            part->setVisible(next);
    if (!next)
        return;
    const int textWidth = railExpandedItemWidth() - 20 - 28 - 10 - 30;
    if (header) {
        auto *workspace = header->findChild<QPushButton *>(QStringLiteral("sidebarWorkspace"));
        if (workspace) {
            QString instance;
            if (window.m_activeServer >= 0 && window.m_activeServer < window.m_servers.size())
                instance = window.m_servers.at(window.m_activeServer).name.trimmed();
            if (instance.isEmpty())
                instance = QStringLiteral("ForkMesh");
            int nodes = 0;
            if (auto *rail = dynamic_cast<ActivityRailButton *>(window.m_networkNavButton))
                nodes = rail->badgeCount();
            QString detail = window.machineNodeName();
            if (nodes > 0)
                detail += (detail.isEmpty() ? QString() : QString::fromUtf8(" \xC2\xB7 ")) +
                          QStringLiteral("%1 node%2").arg(nodes).arg(nodes == 1 ? "" : "s");
            auto *glyph = workspace->findChild<QLabel *>(QStringLiteral("sidebarGlyph"));
            auto *title = workspace->findChild<QLabel *>(QStringLiteral("sidebarTitle"));
            auto *subtitle = workspace->findChild<QLabel *>(QStringLiteral("sidebarSubtitle"));
            if (glyph && glyph->property("themeKey").toString() != Theme::hex(Theme::Role::Fg)) {
                glyph->setProperty("themeKey", Theme::hex(Theme::Role::Fg));
                glyph->setPixmap(sidebarLogo(28));
            }
            if (title)
                title->setText(elided(title, instance, textWidth));
            if (subtitle) {
                subtitle->setText(elided(subtitle, detail, textWidth));
                subtitle->setVisible(!detail.isEmpty());
            }
            for (QLabel *label : {title, subtitle})
                if (label)
                    label->setVisible(expanded && !label->text().isEmpty());
        }
        if (auto *fresh = header->findChild<QPushButton *>(QStringLiteral("sidebarNewSession"))) {
            if (auto *plus = fresh->findChild<QLabel *>(QStringLiteral("sidebarNewSessionIcon")))
                plus->setPixmap(tintedOcticonPixmap(QStringLiteral("plus"),
                                                    Theme::color(Theme::Role::Fg), 16));
            for (QLabel *label : fresh->findChildren<QLabel *>())
                if (label->objectName() != QLatin1String("sidebarNewSessionIcon"))
                    label->setVisible(expanded);
        }
    }
    if (footer) {
        if (auto *account = footer->findChild<QPushButton *>(QStringLiteral("sidebarAccount"))) {
            const QString handle = window.topBarUserName();
            const QString name = handle.isEmpty() ? QStringLiteral("Guest") : handle;
            const bool online = !window.m_offlineMode && window.m_backend != nullptr;
            auto *glyph = account->findChild<QLabel *>(QStringLiteral("sidebarGlyph"));
            auto *title = account->findChild<QLabel *>(QStringLiteral("sidebarTitle"));
            auto *subtitle = account->findChild<QLabel *>(QStringLiteral("sidebarSubtitle"));
            const QString key = name + (online ? "1" : "0") + Theme::hex(Theme::Role::Fg);
            if (glyph && glyph->property("themeKey").toString() != key) {
                glyph->setProperty("themeKey", key);
                glyph->setPixmap(sidebarAvatar(name, online, 28));
            }
            if (title)
                title->setText(elided(title, name, textWidth));
            if (subtitle)
                subtitle->setText(window.m_offlineMode ? QStringLiteral("Offline")
                                  : window.m_accountEmail.isEmpty()
                                      ? (online ? QStringLiteral("Online")
                                                : QStringLiteral("Not connected"))
                                      : elided(subtitle, window.m_accountEmail, textWidth));
            for (QLabel *label : {title, subtitle})
                if (label)
                    label->setVisible(expanded);
        }
    }
    for (QWidget *part : {header, footer}) {
        if (!part)
            continue;
        for (QLabel *chevron : part->findChildren<QLabel *>(QStringLiteral("sidebarChevron"))) {
            chevron->setVisible(expanded);
            if (chevron->property("themeKey").toString() != Theme::hex(Theme::Role::FgSubtle)) {
                chevron->setProperty("themeKey", Theme::hex(Theme::Role::FgSubtle));
                chevron->setPixmap(tintedOcticonPixmap(
                    chevron->property("glyph").toString(),
                    Theme::color(Theme::Role::FgSubtle), 12));
            }
        }
    }
}


namespace {

QPixmap topDotPixmap(const QColor &color)
{
    const qreal ratio = qApp ? qApp->devicePixelRatio() : 1.0;
    QPixmap pixmap(QSize(6, 6) * ratio);
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawEllipse(QRectF(0, 0, 6, 6));
    return pixmap;
}

}

namespace {

QPixmap panelGlyph(const QColor &color, bool right)
{
    const qreal ratio = qApp ? qApp->devicePixelRatio() : 1.0;
    const int side = qMax(1, qRound(16 * ratio));
    QPixmap pixmap(side, side);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.scale(ratio, ratio);
    QPen pen(color, 1.3);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.drawRoundedRect(QRectF(1.5, 2.5, 13, 11), 2.5, 2.5);
    const qreal x = right ? 9.8 : 6.2;
    painter.drawLine(QPointF(x, 2.8), QPointF(x, 13.2));
    painter.end();
    pixmap.setDevicePixelRatio(ratio);
    return pixmap;
}

QPushButton *topIconButton(const char *name, const QString &tip)
{
    auto *button = new QPushButton;
    button->setObjectName(QLatin1String(name));
    button->setProperty("topIconButton", true);
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::NoFocus);
    button->setFixedSize(28, 28);
    button->setIconSize(QSize(16, 16));
    button->setToolTip(tip);
    button->setFlat(true);
    return button;
}

QPixmap bellPixmap(const QColor &color, const QColor &dot, const QColor &ring)
{
    QPixmap base = tintedOcticonPixmap(QStringLiteral("bell"), color, 16);
    if (!dot.isValid())
        return base;
    QPainter painter(&base);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(ring, 1.5));
    painter.setBrush(dot);
    painter.drawEllipse(QRectF(10, 0.5, 5.5, 5.5));
    return base;
}

}

void NextAccess::showPingsPopover(MainWindow &window, QWidget *anchor)
{
    auto *popup = new QFrame(&window, Qt::Popup);
    popup->setObjectName(QStringLiteral("pingsPopover"));
    popup->setAttribute(Qt::WA_DeleteOnClose);
    popup->setFixedWidth(384);
    auto *column = new QVBoxLayout(popup);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);

    auto *head = new QWidget(popup);
    head->setObjectName(QStringLiteral("pingsPopoverHead"));
    head->setFixedHeight(44);
    auto *headRow = new QHBoxLayout(head);
    headRow->setContentsMargins(16, 0, 12, 0);
    auto *title = new QLabel(QStringLiteral("Pings"), head);
    title->setObjectName(QStringLiteral("pingsPopoverTitle"));
    headRow->addWidget(title);
    headRow->addStretch(1);
    auto *markRead = new QPushButton(QStringLiteral("Mark all read"), head);
    markRead->setObjectName(QStringLiteral("pingsPopoverLink"));
    markRead->setCursor(Qt::PointingHandCursor);
    headRow->addWidget(markRead);
    column->addWidget(head);

    auto *listHost = new QWidget;
    listHost->setObjectName(QStringLiteral("pingsPopoverList"));
    auto *list = new QVBoxLayout(listHost);
    list->setContentsMargins(4, 4, 4, 4);
    list->setSpacing(0);
    int shown = 0;
    for (const auto &item : std::as_const(window.m_notifications)) {
        if (item.quiet)
            continue;
        const bool unseen = item.timestampMs > window.m_localPingsSeenAtMs ||
                            (item.dismissOnly && !item.acknowledged);
        auto *row = new QPushButton(listHost);
        row->setObjectName(QStringLiteral("pingsPopoverRow"));
        row->setCursor(Qt::PointingHandCursor);
        row->setFixedHeight(56);
        row->setToolTip(item.body.isEmpty() ? item.title
                                            : item.title + QLatin1Char('\n') + item.body);
        auto *line = new QHBoxLayout(row);
        line->setContentsMargins(12, 9, 12, 9);
        line->setSpacing(12);
        auto *dot = new QLabel(row);
        dot->setFixedSize(6, 6);
        const QColor tone = Theme::color(item.warning ? Theme::Role::Danger
                                         : unseen     ? Theme::Role::Accent
                                                      : Theme::Role::FgSubtle);
        dot->setStyleSheet(QStringLiteral("background:%1;border-radius:3px;")
                               .arg(tone.name()));
        line->addWidget(dot, 0, Qt::AlignTop);
        auto *texts = new QVBoxLayout;
        texts->setSpacing(2);
        auto *heading = new QLabel(row);
        heading->setObjectName(QStringLiteral("pingsPopoverRowTitle"));
        heading->setTextFormat(Qt::PlainText);
        QFont headingFont = heading->font();
        headingFont.setPixelSize(14);
        heading->setText(QFontMetrics(headingFont).elidedText(
            item.title.simplified(), Qt::ElideRight, 320));
        QString source = item.actor;
        if (source.isEmpty() && item.link.isValid())
            source = MainWindow::notificationLinkLabel(item.link);
        if (source.isEmpty())
            source = item.repo;
        if (source.isEmpty())
            source = QStringLiteral("ForkMesh");
        const QString age = formatShortRelativeTime(item.timestampMs / 1000);
        auto *meta = new QLabel(row);
        meta->setObjectName(QStringLiteral("pingsPopoverRowMeta"));
        meta->setTextFormat(Qt::PlainText);
        meta->setText(age.isEmpty() ? source
                                    : source + QString::fromUtf8(" \xC2\xB7 ") + age);
        texts->addWidget(heading);
        texts->addWidget(meta);
        line->addLayout(texts, 1);
        for (QLabel *label : row->findChildren<QLabel *>())
            label->setAttribute(Qt::WA_TransparentForMouseEvents);
        const qint64 id = item.id;
        const qint64 stamp = item.timestampMs;
        const QString key = item.title;
        QObject::connect(row, &QPushButton::clicked, &window,
                         [&window, popup, id, stamp, key] {
                             popup->close();
                             for (const auto &notice : std::as_const(window.m_notifications)) {
                                 if (id != 0 ? notice.id != id
                                             : (notice.timestampMs != stamp ||
                                                notice.title != key))
                                     continue;
                                 if (notice.runId > 0)
                                     window.openActionRunFromNotification(notice.runId);
                                 else if (notice.link.isValid())
                                     window.openNotificationLink(notice.link);
                                 else
                                     window.showNotifications();
                                 return;
                             }
                             window.showNotifications();
                         });
        list->addWidget(row);
        if (++shown >= 6)
            break;
    }
    if (shown == 0) {
        auto *empty = new QLabel(QStringLiteral("No pings yet."), listHost);
        empty->setObjectName(QStringLiteral("pingsPopoverEmpty"));
        empty->setAlignment(Qt::AlignCenter);
        empty->setMinimumHeight(80);
        list->addWidget(empty);
    }
    column->addWidget(listHost);

    int total = 0;
    if (auto *rail = dynamic_cast<ActivityRailButton *>(window.m_notificationButton))
        total = rail->badgeCount();
    auto *foot = new QWidget(popup);
    foot->setObjectName(QStringLiteral("pingsPopoverFoot"));
    auto *footRow = new QHBoxLayout(foot);
    footRow->setContentsMargins(4, 4, 4, 4);
    auto *all = new QPushButton(total > 0 ? QStringLiteral("View all %1 pings").arg(total)
                                          : QStringLiteral("View all pings"),
                                foot);
    all->setObjectName(QStringLiteral("pingsPopoverAll"));
    all->setCursor(Qt::PointingHandCursor);
    all->setFixedHeight(32);
    footRow->addWidget(all);
    column->addWidget(foot);

    QObject::connect(all, &QPushButton::clicked, &window, [&window, popup] {
        popup->close();
        window.showNotifications();
    });
    QObject::connect(markRead, &QPushButton::clicked, &window, [&window, popup] {
        window.markLocalPingsSeen();
        window.updateNotificationButton();
        popup->close();
    });
    popup->adjustSize();
    const QPoint corner = anchor->mapToGlobal(QPoint(anchor->width(), anchor->height() + 6));
    popup->move(corner.x() - popup->width(), corner.y());
    popup->show();
}

void NextAccess::installTopBar(MainWindow &window, QHBoxLayout *row,
                               QWidget *history, QWidget *before,
                               const QList<QPushButton *> &windowButtons)
{
    if (!Theme::isNext() || !row)
        return;
    auto *toggle = window.findChild<QPushButton *>(QLatin1String(kRailToggleName));
    int at = toggle && row->indexOf(toggle) >= 0 ? row->indexOf(toggle) + 1 : 1;
    if (history) {
        if (QWidget *holder = history->parentWidget())
            if (holder->layout())
                holder->layout()->removeWidget(history);
        row->insertWidget(at++, history);
    }
    auto *title = new QLabel;
    title->setObjectName(QStringLiteral("topPageTitle"));
    title->setTextFormat(Qt::PlainText);
    row->insertWidget(at++, title);

#ifdef Q_OS_MACOS
    const char *roles[] = {"close", "minimize", "zoom"};
    QList<QPushButton *> ordered;
    if (windowButtons.size() == 3)
        ordered = {windowButtons.at(2), windowButtons.at(0), windowButtons.at(1)};
    int slot = 0;
    for (QPushButton *button : std::as_const(ordered)) {
        row->removeWidget(button);
        button->setProperty("trafficLight", QLatin1String(roles[slot]));
        button->setIcon(QIcon());
        button->setFixedSize(12, 12);
        row->insertWidget(slot++, button, 0, Qt::AlignVCenter);
    }
    if (!ordered.isEmpty())
        row->insertSpacing(slot, 10);
#else
    Q_UNUSED(windowButtons);
#endif

    auto *cluster = new QWidget;
    cluster->setObjectName(QStringLiteral("topRightCluster"));
    auto *clusterRow = new QHBoxLayout(cluster);
    clusterRow->setContentsMargins(0, 0, 0, 0);
    clusterRow->setSpacing(4);
    auto *nodes = new QPushButton;
    nodes->setObjectName(QStringLiteral("topNodesButton"));
    nodes->setCursor(Qt::PointingHandCursor);
    nodes->setFocusPolicy(Qt::NoFocus);
    nodes->setFixedHeight(28);
    nodes->setToolTip(QStringLiteral("Nodes online"));
    clusterRow->addWidget(nodes);
    QPushButton *bell = topIconButton("topPingsButton", QStringLiteral("Pings"));
    clusterRow->addWidget(bell);
    QPushButton *theme = topIconButton("topThemeButton", QString());
    clusterRow->addWidget(theme);
    QPushButton *team = topIconButton("topTeamButton", QString());
    clusterRow->addWidget(team);
    const int beforeIndex = before ? row->indexOf(before) : -1;
    row->insertWidget(beforeIndex >= 0 ? beforeIndex : row->count(), cluster);

    QObject::connect(nodes, &QPushButton::clicked, &window, [&window] {
        if (window.m_networkNavButton)
            window.m_networkNavButton->click();
    });
    QObject::connect(bell, &QPushButton::clicked, &window,
                     [&window, bell] { showPingsPopover(window, bell); });
    QObject::connect(theme, &QPushButton::clicked, &window, [&window] {
        const QString value = Theme::activeDark() ? QStringLiteral("light")
                                                  : QStringLiteral("dark");
        if (window.m_themeCombo) {
            const int index = window.m_themeCombo->findData(value);
            if (index >= 0 && index != window.m_themeCombo->currentIndex()) {
                window.m_themeCombo->setCurrentIndex(index);
                return;
            }
        }
        QSettings().setValue(kThemeSetting, value);
        window.applyTheme(window.m_packageRegistry.get());
    });
    QObject::connect(team, &QPushButton::clicked, &window, [&window] {
        window.setTeammateRailEnabled(!window.teammateRailEnabled());
    });

    const auto refresh = [&window, title, nodes, bell, theme, team] {
        QString page;
        if (window.m_navGroup)
            if (auto *checked = dynamic_cast<ActivityRailButton *>(
                    window.m_navGroup->checkedButton()))
                page = checked->label();
        title->setText(page);
        title->setVisible(!page.isEmpty());
        int online = 0;
        if (auto *rail = dynamic_cast<ActivityRailButton *>(window.m_networkNavButton))
            online = rail->badgeCount();
        nodes->setText(QStringLiteral("%1 node%2").arg(online).arg(online == 1 ? "" : "s"));
        nodes->setIcon(QIcon(topDotPixmap(Theme::color(
            online > 0 ? Theme::Role::Success : Theme::Role::FgSubtle))));
        nodes->setIconSize(QSize(6, 6));
        int pings = 0;
        if (auto *rail = dynamic_cast<ActivityRailButton *>(window.m_notificationButton))
            pings = rail->badgeCount();
        const QColor muted = Theme::color(Theme::Role::FgMuted);
        bell->setIcon(QIcon(bellPixmap(muted,
                                       pings > 0 ? Theme::color(Theme::Role::Accent) : QColor(),
                                       Theme::color(Theme::Role::BgSubtle))));
        bell->setToolTip(pings > 0 ? QStringLiteral("%1 pings").arg(pings)
                                   : QStringLiteral("Pings"));
        const bool dark = Theme::activeDark();
        theme->setIcon(QIcon(tintedOcticonPixmap(dark ? QStringLiteral("sun")
                                                      : QStringLiteral("moon"),
                                                 muted, 16)));
        theme->setToolTip(dark ? QStringLiteral("Light mode") : QStringLiteral("Dark mode"));
        const bool teamOn = window.teammateRailEnabled();
        team->setIcon(QIcon(panelGlyph(teamOn ? Theme::color(Theme::Role::Fg) : muted, true)));
        team->setToolTip(teamOn ? QStringLiteral("Hide the team rail")
                                : QStringLiteral("Show the team rail"));
    };
    refresh();
    auto *timer = new QTimer(cluster);
    timer->setInterval(1000);
    QObject::connect(timer, &QTimer::timeout, cluster, refresh);
    timer->start();
    if (window.m_navGroup)
        QObject::connect(window.m_navGroup, &QButtonGroup::buttonToggled, cluster,
                         [refresh](QAbstractButton *, bool on) {
                             if (on)
                                 refresh();
                         });
}

}
