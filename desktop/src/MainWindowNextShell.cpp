#include "MainWindowInternal.h"
#include "MainWindowNextAccess.h"

#include <QCheckBox>
#include <QComboBox>
#include <QPainter>
#include <QPainterPath>

namespace forkmesh::ui {

namespace {

const char kRailExpandedSetting[] = "ui/railExpanded";
const char kRailToggleName[] = "railToggleButton";
constexpr int kRowIconLeft = 12;
constexpr int kRowLabelLeft = 36;
constexpr int kRowRightPad = 10;
constexpr int kRowPillHeight = 16;
constexpr int kTileIconPx = 16;
constexpr int kTileBadgeHeight = 14;

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
    painter.drawRoundedRect(body, 6, 6);
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

}

bool railExpanded()
{
    return railExpandedState() && Theme::isNext();
}

void setRailExpanded(bool expanded)
{
    railExpandedState() = expanded;
}

bool railButtonRowLayout(const QWidget *button, bool captionAlwaysVisible)
{
    return !captionAlwaysVisible && railExpanded() && !inFolderBody(button);
}

QSize railButtonSizeHook(const QWidget *button, const RailButtonPaintState &state)
{
    if (!railButtonRowLayout(button, state.captionAlwaysVisible))
        return QSize();
    return QSize(kRailExpandedItemWidth, kRailRowHeight);
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

    const QRectF body = row ? QRectF(4, 1, width - 8, height - 2)
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
            const int w = pillWidth(pillFont, badgeText, kRowPillHeight);
            const QRectF pill(right - w, pillTop, w, kRowPillHeight);
            if (state.badgeUrgent)
                paintPill(painter, pill, badgeText,
                          Theme::color(Theme::Role::Danger),
                          Theme::color(Theme::Role::OnAccent), pillFont);
            else
                paintPill(painter, pill, badgeText,
                          Theme::color(checked ? Theme::Role::BorderStrong
                                               : Theme::Role::SurfaceActive),
                          Theme::color(Theme::Role::FgSecondary), pillFont);
            right -= w + 4;
        }
        if (state.pendingSync > 0) {
            const QString text = syncCount + QChar(0x2191);
            const int w = pillWidth(pillFont, text, kRowPillHeight);
            paintPill(painter, QRectF(right - w, pillTop, w, kRowPillHeight),
                      text, Theme::color(Theme::Role::WarningSubtle),
                      Theme::color(Theme::Role::Warning), pillFont);
            right -= w + 4;
        }
        const QFont labelFont = sizedFont(button, 13, QFont::Medium);
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
    return QSize(kRailExpandedItemWidth, kRailRowHeight);
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
    refreshSectionCards(&window);
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
    if (auto *check = window.findChild<QCheckBox *>(
            QStringLiteral("railExpandedCheck"))) {
        const QSignalBlocker blocker(check);
        check->setChecked(expanded);
    }
}

void NextAccess::toggleRailExpanded(MainWindow &window)
{
    setRailExpanded(window, !railExpandedState());
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

}
