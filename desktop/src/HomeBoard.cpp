#include "HomeBoard.h"

#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QFontMetrics>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QToolTip>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <memory>

namespace {

constexpr int kHoverRadius = 14;
constexpr int kPlateRadius = 16;
constexpr int kPillHeight = 20;
constexpr int kPulseMs = 80;
constexpr int kPulseSteps = 24;
constexpr int kFlipCooldownMs = 400;
constexpr int kEmptyHeight = 200;
constexpr double kPi = 3.14159265358979323846;

QFont resized(const QFont &base, double delta)
{
    QFont font = base;
    const double size = font.pointSizeF() > 0 ? font.pointSizeF() : 10.0;
    font.setPointSizeF(qMax(6.0, size + delta));
    return font;
}

bool darkPalette(const QPalette &colors)
{
    return colors.color(QPalette::Window).lightness() < 128;
}

QString badgeText(int count)
{
    return count > 999 ? QStringLiteral("999+") : QString::number(count);
}

QColor plateColor(const QString &group)
{
    if (group == QLatin1String("work"))
        return QColor("#2f6feb");
    if (group == QLatin1String("people"))
        return QColor("#2da44e");
    if (group == QLatin1String("network"))
        return QColor("#0e8f9c");
    if (group == QLatin1String("insights"))
        return QColor("#8250df");
    if (group == QLatin1String("packages"))
        return QColor("#d1661c");
    if (group == QLatin1String("tools"))
        return QColor("#57606a");
    if (group == QLatin1String("shortcuts"))
        return QColor("#5e60ce");
    return QColor("#6e7781");
}

}

HomeBoard::HomeBoard(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("homeBoard"));
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumWidth(kTileWidth + 2 * kGap);
    m_pages = {QStringList()};
    m_pulse = new QTimer(this);
    m_pulse->setInterval(kPulseMs);
    connect(m_pulse, &QTimer::timeout, this, [this] {
        m_phase = (m_phase + 1) % kPulseSteps;
        for (const PlacedTile &placed : std::as_const(m_tiles))
            if (tileOf(placed).live)
                update(placed.rect.adjusted(-2, -2, 2, 2));
    });
}

QStringList HomeBoard::presentKeys() const
{
    QStringList keys;
    for (const HomeGroup &group : m_groups)
        for (const HomeTile &tile : group.tiles)
            keys.append(tile.key);
    return keys;
}

bool HomeBoard::setGroups(const QVector<HomeGroup> &groups)
{
    if (groups == m_groups)
        return false;
    m_groups = groups;
    m_index.clear();
    for (int g = 0; g < m_groups.size(); ++g)
        for (int t = 0; t < m_groups.at(g).tiles.size(); ++t)
            m_index.insert(m_groups.at(g).tiles.at(t).key, {g, t});
    m_pages = HomeLayout::resolve(m_saved, presentKeys());
    m_page = qBound(0, m_page, int(m_pages.size()) - 1);
    if (dragging() && !m_index.contains(m_dragKey))
        m_dragKey.clear();
    m_hover = -1;
    relayout(width());
    update();
    notifyPages();
    return true;
}

void HomeBoard::setSavedPages(const HomeLayout::Pages &saved)
{
    m_saved = saved;
    m_pages = HomeLayout::resolve(m_saved, presentKeys());
    m_page = qBound(0, m_page, int(m_pages.size()) - 1);
    m_hover = -1;
    relayout(width());
    update();
    notifyPages();
}

void HomeBoard::applyLayout(const HomeLayout::Pages &saved)
{
    setSavedPages(saved);
    if (m_onLayoutChanged)
        m_onLayoutChanged(m_saved);
}

void HomeBoard::setPage(int page)
{
    page = qBound(0, page, int(m_pages.size()) - 1);
    if (page == m_page)
        return;
    m_page = page;
    m_hover = -1;
    relayout(width());
    update();
    notifyPages();
}

void HomeBoard::notifyPages()
{
    const int count = m_filter.isEmpty() ? int(m_pages.size()) : 0;
    if (count == m_notifiedCount && m_page == m_notifiedPage)
        return;
    m_notifiedCount = count;
    m_notifiedPage = m_page;
    if (m_onPagesChanged)
        m_onPagesChanged(count, m_page);
}

void HomeBoard::setFilter(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed == m_filter)
        return;
    if (dragging())
        endDrag(false);
    m_filter = trimmed;
    m_hover = -1;
    relayout(width());
    update();
    notifyPages();
}

bool HomeBoard::matches(const HomeGroup &group, const HomeTile &tile) const
{
    if (m_filter.isEmpty())
        return true;
    return group.title.contains(m_filter, Qt::CaseInsensitive) ||
           tile.title.contains(m_filter, Qt::CaseInsensitive) ||
           tile.detail.contains(m_filter, Qt::CaseInsensitive) ||
           tile.key.contains(m_filter, Qt::CaseInsensitive);
}

void HomeBoard::relayout(int width)
{
    m_tiles.clear();
    const int usable = qMax(kTileWidth, width - 2 * kGap);
    m_columns = qMax(1, (usable + kGap) / (kTileWidth + kGap));
    const int gridWidth = m_columns * kTileWidth + (m_columns - 1) * kGap;
    m_left = qMax(kGap, (width - gridWidth) / 2);

    QStringList order;
    if (m_filter.isEmpty()) {
        order = m_pages.value(m_page);
        if (dragging() && order.contains(m_dragKey)) {
            order.removeAll(m_dragKey);
            order.insert(qBound(0, m_dragTarget, int(order.size())), m_dragKey);
        }
    } else {
        for (const QStringList &page : std::as_const(m_pages))
            for (const QString &key : page) {
                const auto at = m_index.value(key, {-1, -1});
                if (at.first >= 0 &&
                    matches(m_groups.at(at.first),
                            m_groups.at(at.first).tiles.at(at.second)))
                    order.append(key);
            }
    }

    for (int i = 0; i < order.size(); ++i) {
        const auto at = m_index.value(order.at(i), {-1, -1});
        if (at.first < 0)
            continue;
        const int slot = m_tiles.size();
        const int row = slot / m_columns;
        const int column = slot % m_columns;
        m_tiles.append({at.first, at.second,
                        QRect(m_left + column * (kTileWidth + kGap),
                              kGap + row * (kTileHeight + kGap), kTileWidth,
                              kTileHeight)});
    }
    const int rows = (int(m_tiles.size()) + m_columns - 1) / m_columns;
    m_contentHeight =
        m_tiles.isEmpty() ? kEmptyHeight : kGap + rows * (kTileHeight + kGap);
    if (minimumHeight() != m_contentHeight)
        setMinimumHeight(m_contentHeight);
    syncAnimation();
}

bool HomeBoard::animating() const
{
    return m_pulse && m_pulse->isActive();
}

void HomeBoard::syncAnimation()
{
    const bool anyLive = std::any_of(
        m_tiles.cbegin(), m_tiles.cend(),
        [this](const PlacedTile &placed) { return tileOf(placed).live; });
    if (anyLive && isVisible())
        m_pulse->start();
    else
        m_pulse->stop();
}

const HomeTile &HomeBoard::tileOf(const PlacedTile &placed) const
{
    return m_groups.at(placed.group).tiles.at(placed.tile);
}

QStringList HomeBoard::visibleKeys() const
{
    QStringList keys;
    for (const PlacedTile &placed : m_tiles)
        keys.append(tileOf(placed).key);
    return keys;
}

QRect HomeBoard::tileRect(const QString &key) const
{
    for (const PlacedTile &placed : m_tiles)
        if (tileOf(placed).key == key)
            return placed.rect;
    return QRect();
}

QString HomeBoard::firstVisibleKey() const
{
    return m_tiles.isEmpty() ? QString() : tileOf(m_tiles.first()).key;
}

void HomeBoard::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    relayout(width());
}

void HomeBoard::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    syncAnimation();
}

void HomeBoard::hideEvent(QHideEvent *event)
{
    m_pulse->stop();
    if (dragging())
        endDrag(false);
    m_pressed = false;
    QWidget::hideEvent(event);
}

void HomeBoard::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    const QPalette colors = palette();
    if (m_tiles.isEmpty()) {
        painter.setPen(colors.color(QPalette::Disabled, QPalette::Text));
        const QString message =
            m_groups.isEmpty()   ? QStringLiteral("Nothing in the rail yet")
            : !m_filter.isEmpty() ? QStringLiteral("Nothing matches this search")
                                  : QStringLiteral(
                                        "This page is empty\nRight-click an "
                                        "icon and choose Move to Page to put "
                                        "it here");
        painter.drawText(rect().adjusted(kGap, kGap, -kGap, -kGap),
                         Qt::AlignCenter | Qt::TextWordWrap, message);
        return;
    }
    const QRect clip = event->rect();
    for (int i = 0; i < m_tiles.size(); ++i) {
        const PlacedTile &placed = m_tiles.at(i);
        const HomeTile &tile = tileOf(placed);
        if (dragging() && tile.key == m_dragKey)
            continue;
        if (!placed.rect.adjusted(-2, -2, 2, 2).intersects(clip))
            continue;
        paintTile(painter, tile, m_groups.at(placed.group).id, placed.rect,
                  i == m_hover);
    }
    if (dragging()) {
        const auto at = m_index.value(m_dragKey, {-1, -1});
        if (at.first >= 0) {
            painter.setOpacity(0.9);
            paintTile(painter, m_groups.at(at.first).tiles.at(at.second),
                      m_groups.at(at.first).id,
                      QRect(m_dragPos - m_dragOffset,
                            QSize(kTileWidth, kTileHeight)),
                      true);
            painter.setOpacity(1.0);
        }
    }
}

void HomeBoard::paintTile(QPainter &painter, const HomeTile &tile,
                          const QString &group, const QRect &rect,
                          bool hovered) const
{
    const QPalette colors = palette();
    const bool dark = darkPalette(colors);
    const QColor accent = dark ? QColor("#2ea043") : QColor("#1f883d");
    const QColor amber = dark ? QColor("#d29922") : QColor("#9a6700");
    const QColor red = dark ? QColor("#da3633") : QColor("#cf222e");
    const QColor text = colors.color(QPalette::Text);
    const QColor muted = colors.color(QPalette::Disabled, QPalette::Text);
    const QColor window = colors.color(QPalette::Window);

    if (hovered) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(dark ? QColor(255, 255, 255, 18) : QColor(0, 0, 0, 12));
        painter.drawRoundedRect(QRectF(rect), kHoverRadius, kHoverRadius);
    }

    const QRect plate(rect.left() + (rect.width() - kPlate) / 2, rect.top() + 12,
                      kPlate, kPlate);
    const QColor base = tile.alert ? QColor("#bf8700") : plateColor(group);
    QLinearGradient gradient(plate.topLeft(), plate.bottomLeft());
    gradient.setColorAt(0.0, base.lighter(118));
    gradient.setColorAt(1.0, base.darker(112));
    painter.setPen(QPen(dark ? QColor(255, 255, 255, 34) : QColor(0, 0, 0, 30), 1));
    painter.setBrush(gradient);
    painter.drawRoundedRect(QRectF(plate).adjusted(0.5, 0.5, -0.5, -0.5),
                            kPlateRadius, kPlateRadius);

    if (tile.live) {
        const double wave =
            0.5 + 0.5 * std::sin(2.0 * kPi * m_phase / kPulseSteps);
        QColor ring = accent;
        ring.setAlpha(int(80 + 150 * wave));
        painter.setPen(QPen(ring, 2.0 + wave));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(QRectF(plate).adjusted(-3.5, -3.5, 3.5, 3.5),
                                kPlateRadius + 3, kPlateRadius + 3);
    }

    if (m_iconProvider) {
        const QPixmap pixmap =
            m_iconProvider(tile.icon, QColor("#ffffff"), kGlyph);
        if (!pixmap.isNull())
            painter.drawPixmap(QRect(plate.center().x() - kGlyph / 2 + 1,
                                     plate.center().y() - kGlyph / 2 + 1,
                                     kGlyph, kGlyph),
                               pixmap);
    }

    QFont titleFont = resized(font(), 0.5);
    titleFont.setWeight(QFont::DemiBold);
    painter.setFont(titleFont);
    painter.setPen(text);
    const QRect titleRect(rect.left() + 4, plate.bottom() + 8,
                          rect.width() - 8, 20);
    painter.drawText(titleRect, Qt::AlignHCenter | Qt::AlignVCenter,
                     QFontMetrics(titleFont).elidedText(
                         tile.title, Qt::ElideRight, titleRect.width()));

    QFont smallFont = resized(font(), -1.0);
    painter.setFont(smallFont);
    painter.setPen(tile.urgent ? red
                   : tile.alert ? amber
                   : tile.live  ? accent
                                : muted);
    const QRect detailRect(rect.left() + 4, titleRect.bottom() + 1,
                           rect.width() - 8, 16);
    painter.drawText(detailRect, Qt::AlignHCenter | Qt::AlignVCenter,
                     QFontMetrics(smallFont).elidedText(
                         tile.detail, Qt::ElideRight, detailRect.width()));

    QFont pillFont = resized(font(), -1.0);
    pillFont.setBold(true);
    painter.setFont(pillFont);
    const QFontMetrics pillMetrics(pillFont);
    const auto drawPill = [&](const QRectF &pill, const QColor &color,
                              const QString &label) {
        painter.setPen(QPen(window, 2));
        painter.setBrush(color);
        painter.drawRoundedRect(pill, kPillHeight / 2.0, kPillHeight / 2.0);
        painter.setPen(QColor("#ffffff"));
        painter.drawText(pill, Qt::AlignCenter, label);
    };
    if (tile.badge > 0) {
        const QString label = badgeText(tile.badge);
        const int w =
            qMax(kPillHeight, pillMetrics.horizontalAdvance(label) + 12);
        drawPill(QRectF(plate.right() + 9 - w, plate.top() - 8, w, kPillHeight),
                 tile.urgent ? red : QColor("#1f6feb"), label);
    }
    if (tile.pending > 0) {
        const QString label = badgeText(tile.pending) + QChar(0x2191);
        const int w =
            qMax(kPillHeight, pillMetrics.horizontalAdvance(label) + 12);
        drawPill(QRectF(plate.left() - 9, plate.top() - 8, w, kPillHeight),
                 dark ? QColor("#bb8009") : QColor("#9a6700"), label);
    } else if (tile.live) {
        const int d = 12;
        painter.setPen(QPen(window, 2));
        painter.setBrush(accent);
        painter.drawEllipse(QRect(plate.left() - 4, plate.top() - 4, d, d));
    }
}

int HomeBoard::tileAt(const QPoint &pos) const
{
    for (int i = 0; i < m_tiles.size(); ++i) {
        if (dragging() && tileOf(m_tiles.at(i)).key == m_dragKey)
            continue;
        if (m_tiles.at(i).rect.contains(pos))
            return i;
    }
    return -1;
}

int HomeBoard::slotAt(const QPoint &pos) const
{
    const int column = qBound(
        0, (pos.x() - m_left + kGap / 2) / (kTileWidth + kGap), m_columns - 1);
    const int row = qMax(0, (pos.y() - kGap + kGap / 2) / (kTileHeight + kGap));
    const int last = qMax(0, int(m_pages.value(m_page).size()) - 1);
    return qBound(0, row * m_columns + column, last);
}

void HomeBoard::endDrag(bool commit)
{
    const QString key = m_dragKey;
    const int target = m_dragTarget;
    m_dragKey.clear();
    m_dragTarget = -1;
    unsetCursor();
    if (commit && m_pages.value(m_page).indexOf(key) != target) {
        applyLayout(HomeLayout::moveWithinPage(m_pages, key, target));
        return;
    }
    relayout(width());
    update();
}

void HomeBoard::mouseMoveEvent(QMouseEvent *event)
{
    const QPoint pos = event->position().toPoint();
    if (m_pressed && (event->buttons() & Qt::LeftButton)) {
        if (dragging()) {
            m_dragPos = pos;
            const int slot = slotAt(pos);
            if (slot != m_dragTarget) {
                m_dragTarget = slot;
                relayout(width());
            }
            update();
            return;
        }
        if (m_pressIndex >= 0 && m_pressIndex < m_tiles.size() &&
            m_filter.isEmpty() &&
            (pos - m_pressPos).manhattanLength() >=
                QApplication::startDragDistance()) {
            const PlacedTile &placed = m_tiles.at(m_pressIndex);
            m_dragKey = tileOf(placed).key;
            m_dragOffset = m_pressPos - placed.rect.topLeft();
            m_dragPos = pos;
            m_dragTarget = m_pages.value(m_page).indexOf(m_dragKey);
            m_hover = -1;
            setCursor(Qt::ClosedHandCursor);
            relayout(width());
            update();
            return;
        }
        if (m_pressIndex < 0 && !m_pulled &&
            pos.y() - m_pressPos.y() >= kPullDragPx && m_onPull &&
            (!m_pullGate || m_pullGate())) {
            m_pulled = true;
            m_onPull();
        }
        return;
    }
    const int hit = tileAt(pos);
    if (hit == m_hover)
        return;
    const int previous = m_hover;
    m_hover = hit;
    setCursor(hit >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
    if (previous >= 0 && previous < m_tiles.size())
        update(m_tiles.at(previous).rect.adjusted(-2, -2, 2, 2));
    if (hit >= 0)
        update(m_tiles.at(hit).rect.adjusted(-2, -2, 2, 2));
}

void HomeBoard::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    m_pressed = true;
    m_pulled = false;
    m_pressPos = event->position().toPoint();
    m_pressIndex = tileAt(m_pressPos);
}

void HomeBoard::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || !m_pressed)
        return;
    m_pressed = false;
    if (dragging()) {
        endDrag(true);
        return;
    }
    const int pressed = m_pressIndex;
    m_pressIndex = -1;
    if (pressed < 0 || pressed >= m_tiles.size() ||
        tileAt(event->position().toPoint()) != pressed || !m_onActivate)
        return;
    const QString key = tileOf(m_tiles.at(pressed)).key;
    m_onActivate(key);
}

void HomeBoard::wheelEvent(QWheelEvent *event)
{
    const QPoint delta = event->angleDelta();
    if (std::abs(delta.x()) > std::abs(delta.y())) {
        if (!m_filter.isEmpty() || m_pages.size() <= 1) {
            event->ignore();
            return;
        }
        event->accept();
        if (m_lastFlip.isValid() && m_lastFlip.elapsed() < kFlipCooldownMs) {
            m_swipe = 0;
            return;
        }
        m_swipe += delta.x();
        if (std::abs(m_swipe) >= kSwipeThreshold) {
            setPage(m_page + (m_swipe < 0 ? 1 : -1));
            m_swipe = 0;
            m_lastFlip.start();
        }
        return;
    }
    if (delta.y() > 0 && event->phase() != Qt::ScrollMomentum && m_onPull &&
        (!m_pullGate || m_pullGate())) {
        event->accept();
        m_pull += delta.y();
        if (m_pull >= kPullThreshold) {
            m_pull = 0;
            m_onPull();
        }
        return;
    }
    m_pull = 0;
    event->ignore();
}

void HomeBoard::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Left:
    case Qt::Key_PageUp:
        setPage(m_page - 1);
        return;
    case Qt::Key_Right:
    case Qt::Key_PageDown:
        setPage(m_page + 1);
        return;
    case Qt::Key_Escape:
        if (dragging()) {
            m_pressed = false;
            endDrag(false);
            return;
        }
        break;
    default:
        break;
    }
    const QString text = event->text();
    if (m_onType && !text.isEmpty() && text.at(0).isPrint() &&
        !(event->modifiers() &
          (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
        m_onType(text);
        return;
    }
    QWidget::keyPressEvent(event);
}

QMenu *HomeBoard::contextMenuFor(const QPoint &pos)
{
    auto *menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("homeBoardMenu"));
    const bool browsing = m_filter.isEmpty();
    const int hit = tileAt(pos);
    if (hit >= 0) {
        const HomeTile &tile = tileOf(m_tiles.at(hit));
        const QString key = tile.key;
        QAction *open = menu->addAction(QStringLiteral("Open %1").arg(tile.title));
        open->setObjectName(QStringLiteral("homeOpen"));
        connect(open, &QAction::triggered, this, [this, key] {
            if (m_onActivate)
                m_onActivate(key);
        });
        menu->addSeparator();

        const int from = HomeLayout::pageOf(m_pages, key);
        QMenu *move = menu->addMenu(QStringLiteral("Move to Page"));
        move->setObjectName(QStringLiteral("homeMoveMenu"));
        for (int i = 0; i < m_pages.size(); ++i) {
            QAction *to = move->addAction(QStringLiteral("Page %1").arg(i + 1));
            to->setObjectName(QStringLiteral("homeMoveToPage%1").arg(i + 1));
            to->setCheckable(true);
            to->setChecked(i == from);
            to->setEnabled(i != from);
            connect(to, &QAction::triggered, this, [this, key, i] {
                applyLayout(HomeLayout::moveToPage(m_pages, key, i));
            });
        }
        move->addSeparator();
        QAction *fresh = move->addAction(QStringLiteral("New Page"));
        fresh->setObjectName(QStringLiteral("homeMoveToNewPage"));
        connect(fresh, &QAction::triggered, this, [this, key] {
            applyLayout(
                HomeLayout::moveToPage(m_pages, key, int(m_pages.size())));
        });

        if (browsing && from >= 0) {
            const QStringList &keys = m_pages.at(from);
            const int index = keys.indexOf(key);
            QAction *left = menu->addAction(QStringLiteral("Move Left"));
            left->setObjectName(QStringLiteral("homeMoveLeft"));
            left->setEnabled(index > 0);
            connect(left, &QAction::triggered, this, [this, key, index] {
                applyLayout(HomeLayout::moveWithinPage(m_pages, key, index - 1));
            });
            QAction *right = menu->addAction(QStringLiteral("Move Right"));
            right->setObjectName(QStringLiteral("homeMoveRight"));
            right->setEnabled(index < keys.size() - 1);
            connect(right, &QAction::triggered, this, [this, key, index] {
                applyLayout(HomeLayout::moveWithinPage(m_pages, key, index + 1));
            });
        }
        if (m_tileMenu) {
            menu->addSeparator();
            m_tileMenu(menu, key);
        }
        menu->addSeparator();
    }
    if (browsing) {
        QAction *add = menu->addAction(QStringLiteral("Add Page"));
        add->setObjectName(QStringLiteral("homeAddPage"));
        connect(add, &QAction::triggered, this, [this] {
            applyLayout(HomeLayout::addPage(m_pages));
            setPage(int(m_pages.size()) - 1);
        });
        if (m_pages.size() > 1) {
            const int page = m_page;
            QAction *remove = menu->addAction(
                QStringLiteral("Remove Page %1").arg(page + 1));
            remove->setObjectName(QStringLiteral("homeRemovePage"));
            remove->setToolTip(QStringLiteral(
                "Its icons move to the page before it"));
            connect(remove, &QAction::triggered, this, [this, page] {
                applyLayout(HomeLayout::removePage(m_pages, page));
                setPage(qMax(0, page - 1));
            });
        }
    }
    QAction *reset = menu->addAction(QStringLiteral("Reset Home Screen"));
    reset->setObjectName(QStringLiteral("homeResetLayout"));
    reset->setToolTip(
        QStringLiteral("Put every icon back on one page in the rail's order"));
    reset->setEnabled(customized());
    connect(reset, &QAction::triggered, this, [this] {
        applyLayout(HomeLayout::Pages());
        setPage(0);
    });
    return menu;
}

void HomeBoard::contextMenuEvent(QContextMenuEvent *event)
{
    if (dragging())
        return;
    std::unique_ptr<QMenu> menu(contextMenuFor(event->pos()));
    menu->exec(event->globalPos());
}

void HomeBoard::leaveEvent(QEvent *event)
{
    QWidget::leaveEvent(event);
    if (m_hover < 0)
        return;
    const int previous = m_hover;
    m_hover = -1;
    if (!dragging())
        unsetCursor();
    if (previous < m_tiles.size())
        update(m_tiles.at(previous).rect.adjusted(-2, -2, 2, 2));
}

bool HomeBoard::event(QEvent *event)
{
    if (event->type() == QEvent::ToolTip) {
        auto *help = static_cast<QHelpEvent *>(event);
        const int hit = tileAt(help->pos());
        const QString tip =
            hit >= 0 ? tileOf(m_tiles.at(hit)).tooltip
            : m_filter.isEmpty()
                ? QStringLiteral("Pull down or start typing to search. "
                                 "Right-click to arrange icons into pages.")
                : QString();
        if (!tip.isEmpty())
            QToolTip::showText(help->globalPos(), tip, this);
        else
            QToolTip::hideText();
        return true;
    }
    return QWidget::event(event);
}

namespace {

constexpr int kDot = 8;
constexpr int kDotSpacing = 10;
constexpr int kDotsHeight = 24;

}

HomePageDots::HomePageDots(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("homePageDots"));
    setFixedHeight(kDotsHeight);
    setCursor(Qt::PointingHandCursor);
}

void HomePageDots::setPages(int count, int current)
{
    if (count == m_count && current == m_current)
        return;
    m_count = count;
    m_current = current;
    setToolTip(count > 1 ? QStringLiteral("Page %1 of %2").arg(current + 1).arg(count)
                         : QString());
    update();
}

QRect HomePageDots::dotRect(int index) const
{
    const int total = m_count * kDot + qMax(0, m_count - 1) * kDotSpacing;
    const int left = (width() - total) / 2;
    return QRect(left + index * (kDot + kDotSpacing), (height() - kDot) / 2,
                 kDot, kDot);
}

QSize HomePageDots::sizeHint() const
{
    return QSize(qMax(1, m_count) * (kDot + kDotSpacing), kDotsHeight);
}

void HomePageDots::paintEvent(QPaintEvent *)
{
    if (m_count <= 1)
        return;
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QColor on = palette().color(QPalette::Text);
    QColor off = on;
    off.setAlpha(70);
    painter.setPen(Qt::NoPen);
    for (int i = 0; i < m_count; ++i) {
        painter.setBrush(i == m_current ? on : off);
        painter.drawEllipse(dotRect(i));
    }
}

void HomePageDots::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || m_count <= 1 || !m_onPick)
        return;
    const QPoint pos = event->position().toPoint();
    for (int i = 0; i < m_count; ++i)
        if (dotRect(i).adjusted(-5, -8, 5, 8).contains(pos)) {
            m_onPick(i);
            return;
        }
}
