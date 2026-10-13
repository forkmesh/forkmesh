#include "GenieLandmarks.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QFontInfo>
#include <QFontMetrics>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QScrollBar>
#include <QTabBar>
#include <QTextEdit>

#include <algorithm>
#include <cmath>

namespace {

constexpr double kInsetX = 2;
constexpr double kInsetTop = 12;
constexpr double kGrid = 8;
constexpr double kSame = 0.5;
constexpr int kMaxMarks = 300;
constexpr int kMaxBlocks = 400;
constexpr int kMaxRows = 12;
constexpr int kNearest = 4;

double floorOf(const GenieStage &stage, double x)
{
    return stage.floor ? stage.floor(x) : 0;
}

struct Option {
    QList<QRectF> marks;
    QRectF span;
    bool shield = true;
};

Option option(const QList<QRectF> &rects, bool shield)
{
    Option o;
    o.marks = rects;
    o.shield = shield;
    for (const QRectF &r : rects)
        o.span = o.span.isNull() ? r : o.span.united(r);
    return o;
}

double lowestRise(GenieInteraction kind)
{
    return kind == GenieInteraction::Silk ? 90 : 6;
}

double highestRise(GenieInteraction kind)
{
    return kind == GenieInteraction::Silk ? 620 : 420;
}

bool heading(const QLabel *label, int base)
{
    const QString name = label->objectName().toLower();
    if (name.contains(QLatin1String("title")) || name.contains(QLatin1String("heading")))
        return true;
    return QFontInfo(label->font()).pixelSize() >= base * 1.2;
}

QRect labelText(const QLabel *label, const QRect &whole)
{
    const QString text = label->text();
    if (label->wordWrap() || Qt::mightBeRichText(text))
        return whole;
    const QFontMetrics metrics(label->font());
    const QRect inner = whole.marginsRemoved(label->contentsMargins());
    const int width = std::min(inner.width(), metrics.horizontalAdvance(text));
    const int height = std::min(inner.height(), metrics.height());
    int x = inner.left();
    if (label->alignment() & Qt::AlignHCenter)
        x = inner.center().x() - width / 2;
    else if (label->alignment() & Qt::AlignRight)
        x = inner.right() - width;
    int y = inner.center().y() - height / 2;
    if (label->alignment() & Qt::AlignTop)
        y = inner.top();
    else if (label->alignment() & Qt::AlignBottom)
        y = inner.bottom() - height;
    return QRect(x, y, width, height);
}

bool pointing(const QWidget *widget)
{
    return widget->testAttribute(Qt::WA_SetCursor) && widget->cursor().shape() == Qt::PointingHandCursor;
}

bool linked(const QLabel *label)
{
    if (pointing(label))
        return true;
    return (label->textInteractionFlags() & Qt::LinksAccessibleByMouse) &&
           label->text().contains(QLatin1String("<a "), Qt::CaseInsensitive);
}

struct Scanner {
    GenieScene scene;
    QList<const QWidget *> skip;
    int base = 13;
    int groups = 0;

    void mark(GenieMarkKind kind, const QRect &rect, const QRect &clip, int group = -1)
    {
        const QRect visible = rect.intersected(clip);
        if (visible.width() < 6 || visible.height() < 6 || scene.marks.size() >= kMaxMarks)
            return;
        scene.marks.append({kind, QRectF(visible), group});
    }

    void block(const QRect &rect, const QRect &clip)
    {
        const QRect visible = rect.intersected(clip);
        if (visible.isEmpty() || scene.blocks.size() >= kMaxBlocks)
            return;
        scene.blocks.append(QRectF(visible));
    }

    void rows(QAbstractItemView *view, const QPoint &at, const QRect &clip)
    {
        QWidget *port = view->viewport();
        if (!port)
            return;
        const QRect area(at + port->pos(), port->size());
        const QRect visible = area.intersected(clip);
        if (visible.isEmpty())
            return;
        const int group = groups++;
        int y = 1;
        for (int count = 0; count < kMaxRows && y < port->height(); ++count) {
            QModelIndex index = view->indexAt(QPoint(4, y));
            if (!index.isValid())
                index = view->indexAt(QPoint(port->width() / 2, y));
            if (!index.isValid())
                break;
            const QRect cell = view->visualRect(index);
            if (cell.height() <= 0 || cell.bottom() + 2 <= y)
                break;
            mark(GenieMarkKind::Row, QRect(area.left(), area.top() + cell.top(), area.width(), cell.height()), visible,
                 group);
            y = cell.bottom() + 2;
        }
    }

    void text(QWidget *widget, QWidget *port, bool readOnly, const QPoint &at, const QRect &clip)
    {
        if (readOnly || widget->height() >= 200)
            return;
        const QRect area = port ? QRect(at + port->pos(), port->size()) : QRect(at, widget->size());
        mark(GenieMarkKind::Input, area, clip);
        block(QRect(at, widget->size()), clip);
    }

    void visit(QWidget *parent, const QPoint &offset, const QRect &clip)
    {
        for (QObject *object : parent->children()) {
            auto *widget = qobject_cast<QWidget *>(object);
            if (!widget || widget->isWindow() || widget->isHidden() || skip.contains(widget))
                continue;
            if (scene.marks.size() >= kMaxMarks && scene.blocks.size() >= kMaxBlocks)
                return;
            const QRect rect(offset + widget->pos(), widget->size());
            const QRect inside = rect.intersected(clip);
            if (inside.isEmpty())
                continue;
            if (qobject_cast<QScrollBar *>(widget))
                continue;
            if (auto *tabs = qobject_cast<QTabBar *>(widget)) {
                const int group = groups++;
                for (int i = 0; i < tabs->count(); ++i) {
                    if (!tabs->isTabVisible(i))
                        continue;
                    const QRect tab = tabs->tabRect(i).translated(rect.topLeft());
                    mark(GenieMarkKind::Tab, tab, inside, group);
                    block(tab, inside);
                }
                continue;
            }
            if (qobject_cast<QAbstractButton *>(widget)) {
                mark(GenieMarkKind::Button, rect, inside);
                block(rect, inside);
                continue;
            }
            if (auto *combo = qobject_cast<QComboBox *>(widget)) {
                mark(combo->isEditable() ? GenieMarkKind::Input : GenieMarkKind::Button, rect, inside);
                block(rect, inside);
                continue;
            }
            if (qobject_cast<QLineEdit *>(widget) || qobject_cast<QAbstractSpinBox *>(widget)) {
                mark(GenieMarkKind::Input, rect, inside);
                block(rect, inside);
                continue;
            }
            if (qobject_cast<QAbstractSlider *>(widget)) {
                if (widget->width() > widget->height())
                    mark(GenieMarkKind::Progress, rect, inside);
                block(rect, inside);
                continue;
            }
            if (qobject_cast<QProgressBar *>(widget)) {
                mark(GenieMarkKind::Progress, rect, inside);
                continue;
            }
            if (auto *label = qobject_cast<QLabel *>(widget)) {
                if (!label->pixmap().isNull() || label->movie()) {
                    mark(GenieMarkKind::Picture, rect, inside);
                    block(rect, inside);
                    continue;
                }
                if (!label->text().trimmed().isEmpty())
                    mark(heading(label, base) ? GenieMarkKind::Heading : GenieMarkKind::Label,
                         labelText(label, rect), inside);
                if (linked(label))
                    block(rect, inside);
                continue;
            }
            if (qstrcmp(widget->metaObject()->className(), "QSvgWidget") == 0) {
                mark(GenieMarkKind::Picture, rect, inside);
                block(rect, inside);
                continue;
            }
            if (auto *edit = qobject_cast<QPlainTextEdit *>(widget)) {
                text(edit, edit->viewport(), edit->isReadOnly(), rect.topLeft(), inside);
                continue;
            }
            if (auto *edit = qobject_cast<QTextEdit *>(widget)) {
                text(edit, edit->viewport(), edit->isReadOnly(), rect.topLeft(), inside);
                continue;
            }
            if (auto *view = qobject_cast<QAbstractItemView *>(widget)) {
                if (!qobject_cast<QHeaderView *>(widget))
                    rows(view, rect.topLeft(), inside);
                continue;
            }
            if (pointing(widget) && widget->width() <= 360 && widget->height() <= 160)
                block(rect, inside);
            visit(widget, rect.topLeft(), inside);
        }
    }
};

}

namespace GenieKeepClear {

QRectF body(double x, double floor, double side)
{
    return QRectF(x - side / 2 + kInsetX, floor - side + kInsetTop, side - 2 * kInsetX, side - kInsetTop - 1);
}

double coverage(const QRectF &body, const QList<QRectF> &blocks)
{
    double sum = 0;
    for (const QRectF &r : blocks) {
        const QRectF overlap = body.intersected(r);
        if (overlap.width() > 0 && overlap.height() > 0)
            sum += overlap.width() * overlap.height();
    }
    return sum;
}

double coverageAt(const GenieStage &stage, double x, const QList<QRectF> &extra)
{
    const QRectF b = body(x, floorOf(stage, x), stage.side);
    return coverage(b, stage.blocks) + coverage(b, extra);
}

double clearSpot(const GenieStage &stage, double wanted, const QList<QRectF> &extra)
{
    const double lo = stage.left;
    const double hi = std::max(stage.left, stage.right);
    wanted = std::clamp(wanted, lo, hi);
    QList<double> xs;
    for (double x = lo; x < hi; x += kGrid)
        xs << x;
    xs << hi;
    double top = 1e18;
    double bottom = -1e18;
    QList<double> floors;
    floors.reserve(xs.size());
    for (const double x : std::as_const(xs)) {
        const double f = floorOf(stage, x);
        floors << f;
        top = std::min(top, f);
        bottom = std::max(bottom, f);
    }
    const double reach = top - stage.side + kInsetTop;
    QList<QRectF> near;
    for (const QList<QRectF> *list : {&stage.blocks, &extra}) {
        for (const QRectF &r : *list) {
            if (!r.isEmpty() && r.bottom() > reach && r.top() < bottom)
                near << r;
        }
    }
    if (near.isEmpty())
        return wanted;
    const double half = stage.side / 2 - kInsetX + 0.5;
    double best = wanted;
    double bestCover = coverage(body(wanted, floorOf(stage, wanted), stage.side), near);
    if (bestCover <= 0)
        return wanted;
    const auto consider = [&](double x, double floor) {
        const double cover = coverage(body(x, floor, stage.side), near);
        if (cover < bestCover - kSame ||
            (cover <= bestCover + kSame && std::abs(x - wanted) < std::abs(best - wanted))) {
            best = x;
            bestCover = cover;
        }
    };
    for (int i = 0; i < xs.size(); ++i)
        consider(xs.at(i), floors.at(i));
    for (const QRectF &r : std::as_const(near)) {
        for (const double edge : {r.left() - half, r.right() + half}) {
            const double x = std::clamp(edge, lo, hi);
            consider(x, floorOf(stage, x));
        }
    }
    return best;
}

}

namespace GeniePlay {

QString name(GenieInteraction kind)
{
    switch (kind) {
    case GenieInteraction::Peek:
        return QStringLiteral("Peek");
    case GenieInteraction::Read:
        return QStringLiteral("Read");
    case GenieInteraction::Wave:
        return QStringLiteral("Wave");
    case GenieInteraction::Point:
        return QStringLiteral("Point");
    case GenieInteraction::Sniff:
        return QStringLiteral("Sniff");
    case GenieInteraction::Cheer:
        return QStringLiteral("Cheer");
    case GenieInteraction::TabNod:
        return QStringLiteral("Tab nod");
    case GenieInteraction::Silk:
        return QStringLiteral("Silk");
    case GenieInteraction::Hop:
        return QStringLiteral("Hop");
    case GenieInteraction::Eureka:
        return QStringLiteral("Eureka");
    case GenieInteraction::Startle:
        return QStringLiteral("Startle");
    case GenieInteraction::Giggle:
        return QStringLiteral("Giggle");
    case GenieInteraction::Count:
        return QStringLiteral("Count");
    case GenieInteraction::Compare:
        return QStringLiteral("Compare");
    case GenieInteraction::Poof:
        return QStringLiteral("Poof");
    case GenieInteraction::Levitate:
        return QStringLiteral("Levitate");
    case GenieInteraction::Lamp:
        return QStringLiteral("Lamp");
    case GenieInteraction::Wishes:
        return QStringLiteral("Wishes");
    case GenieInteraction::SmokeRings:
        return QStringLiteral("Smoke rings");
    case GenieInteraction::Shimmer:
        return QStringLiteral("Shimmer");
    case GenieInteraction::Stardust:
        return QStringLiteral("Stardust");
    case GenieInteraction::Carpet:
        return QStringLiteral("Carpet");
    case GenieInteraction::Vortex:
        return QStringLiteral("Vortex");
    case GenieInteraction::Crystal:
        return QStringLiteral("Crystal");
    case GenieInteraction::Runes:
        return QStringLiteral("Runes");
    case GenieInteraction::Halo:
        return QStringLiteral("Halo");
    case GenieInteraction::Fireflies:
        return QStringLiteral("Fireflies");
    case GenieInteraction::Whirl:
        return QStringLiteral("Whirl");
    case GenieInteraction::Grow:
        return QStringLiteral("Grow");
    case GenieInteraction::Bottle:
        return QStringLiteral("Bottle");
    case GenieInteraction::Rainbow:
        return QStringLiteral("Rainbow");
    case GenieInteraction::Fireworks:
        return QStringLiteral("Fireworks");
    case GenieInteraction::Bubbles:
        return QStringLiteral("Bubbles");
    case GenieInteraction::Glitter:
        return QStringLiteral("Glitter");
    case GenieInteraction::JewelBeam:
        return QStringLiteral("Jewel beam");
    case GenieInteraction::Ripple:
        return QStringLiteral("Ripple");
    case GenieInteraction::Moons:
        return QStringLiteral("Moons");
    case GenieInteraction::Constellation:
        return QStringLiteral("Constellation");
    case GenieInteraction::Crescent:
        return QStringLiteral("Crescent");
    case GenieInteraction::Mirage:
        return QStringLiteral("Mirage");
    case GenieInteraction::Coins:
        return QStringLiteral("Coins");
    case GenieInteraction::Flourish:
        return QStringLiteral("Flourish");
    case GenieInteraction::Incense:
        return QStringLiteral("Incense");
    case GenieInteraction::Whirlwind:
        return QStringLiteral("Whirlwind");
    case GenieInteraction::None:
        break;
    }
    return QString();
}

QList<GenieInteraction> all()
{
    QList<GenieInteraction> kinds;
    for (int i = 1; i <= kGenieInteractionCount; ++i)
        kinds << GenieInteraction(i);
    return kinds;
}

bool magic(GenieInteraction kind)
{
    return int(kind) > int(GenieInteraction::Compare) && int(kind) <= kGenieInteractionCount;
}

bool fits(GenieInteraction kind, GenieMarkKind mark)
{
    using M = GenieMarkKind;
    switch (kind) {
    case GenieInteraction::Peek:
        return mark == M::Button || mark == M::Picture || mark == M::Heading || mark == M::Label || mark == M::Input;
    case GenieInteraction::Read:
        return mark == M::Heading || mark == M::Label || mark == M::Row;
    case GenieInteraction::Wave:
        return mark == M::Button || mark == M::Picture || mark == M::Heading;
    case GenieInteraction::Point:
        return mark == M::Button || mark == M::Input || mark == M::Tab || mark == M::Progress || mark == M::Picture;
    case GenieInteraction::Sniff:
        return mark == M::Picture;
    case GenieInteraction::Cheer:
        return mark == M::Progress;
    case GenieInteraction::TabNod:
        return mark == M::Tab;
    case GenieInteraction::Silk:
        return mark == M::Heading || mark == M::Picture || mark == M::Button || mark == M::Label;
    case GenieInteraction::Hop:
        return mark == M::Button || mark == M::Picture || mark == M::Label || mark == M::Input || mark == M::Heading;
    case GenieInteraction::Eureka:
        return mark == M::Input;
    case GenieInteraction::Startle:
        return mark == M::Button;
    case GenieInteraction::Giggle:
        return mark == M::Label || mark == M::Heading;
    case GenieInteraction::Count:
        return mark == M::Tab || mark == M::Row;
    case GenieInteraction::Compare:
        return mark == M::Button;
    case GenieInteraction::Poof:
        return mark == M::Button || mark == M::Picture || mark == M::Heading || mark == M::Label || mark == M::Input ||
               mark == M::Progress;
    case GenieInteraction::Levitate:
        return mark == M::Heading || mark == M::Picture || mark == M::Label || mark == M::Button;
    case GenieInteraction::Lamp:
        return mark == M::Input || mark == M::Button || mark == M::Picture || mark == M::Heading;
    case GenieInteraction::Wishes:
        return mark == M::Button || mark == M::Picture || mark == M::Heading || mark == M::Progress;
    case GenieInteraction::SmokeRings:
        return mark == M::Picture || mark == M::Heading || mark == M::Button || mark == M::Label;
    case GenieInteraction::Shimmer:
        return mark == M::Heading || mark == M::Label || mark == M::Button || mark == M::Progress || mark == M::Row;
    case GenieInteraction::Stardust:
        return mark == M::Heading || mark == M::Picture || mark == M::Button || mark == M::Label;
    case GenieInteraction::Carpet:
        return mark == M::Button || mark == M::Picture || mark == M::Heading || mark == M::Label || mark == M::Input;
    case GenieInteraction::Vortex:
        return mark == M::Picture || mark == M::Button || mark == M::Input;
    case GenieInteraction::Crystal:
        return mark == M::Picture || mark == M::Heading || mark == M::Label || mark == M::Row;
    case GenieInteraction::Runes:
        return mark == M::Picture || mark == M::Button || mark == M::Heading;
    case GenieInteraction::Halo:
        return mark == M::Picture || mark == M::Heading || mark == M::Button;
    case GenieInteraction::Fireflies:
        return mark == M::Picture || mark == M::Heading || mark == M::Label || mark == M::Row;
    case GenieInteraction::Whirl:
        return mark == M::Button || mark == M::Tab || mark == M::Heading || mark == M::Label || mark == M::Picture;
    case GenieInteraction::Grow:
        return mark == M::Button || mark == M::Picture || mark == M::Heading;
    case GenieInteraction::Bottle:
        return mark == M::Input || mark == M::Label || mark == M::Heading || mark == M::Picture;
    case GenieInteraction::Rainbow:
        return mark == M::Picture || mark == M::Heading || mark == M::Progress || mark == M::Button;
    case GenieInteraction::Fireworks:
        return mark == M::Progress || mark == M::Heading || mark == M::Button || mark == M::Picture;
    case GenieInteraction::Bubbles:
        return mark == M::Picture || mark == M::Input || mark == M::Label || mark == M::Row;
    case GenieInteraction::Glitter:
        return mark == M::Heading || mark == M::Label || mark == M::Row || mark == M::Picture;
    case GenieInteraction::JewelBeam:
        return mark == M::Button || mark == M::Picture || mark == M::Input || mark == M::Tab;
    case GenieInteraction::Ripple:
        return mark == M::Button || mark == M::Picture || mark == M::Input || mark == M::Progress;
    case GenieInteraction::Moons:
        return mark == M::Picture || mark == M::Button || mark == M::Heading;
    case GenieInteraction::Constellation:
        return mark == M::Picture || mark == M::Heading || mark == M::Label;
    case GenieInteraction::Crescent:
        return mark == M::Heading || mark == M::Picture;
    case GenieInteraction::Mirage:
        return mark == M::Button || mark == M::Input || mark == M::Picture || mark == M::Tab || mark == M::Heading;
    case GenieInteraction::Coins:
        return mark == M::Button || mark == M::Progress || mark == M::Picture;
    case GenieInteraction::Flourish:
        return mark == M::Heading || mark == M::Label || mark == M::Button || mark == M::Picture;
    case GenieInteraction::Incense:
        return mark == M::Label || mark == M::Heading || mark == M::Row || mark == M::Picture;
    case GenieInteraction::Whirlwind:
        return mark == M::Button || mark == M::Heading || mark == M::Picture || mark == M::Tab;
    case GenieInteraction::None:
        break;
    }
    return false;
}

GeniePlan plan(GenieInteraction kind, const QList<GenieMark> &marks, const GenieStage &stage, double from,
               quint32 roll)
{
    GeniePlan out;
    if (kind == GenieInteraction::None || !stage.floor || stage.right < stage.left)
        return out;
    QList<Option> options;
    if (kind == GenieInteraction::TabNod || kind == GenieInteraction::Count) {
        QMap<int, QList<GenieMark>> groups;
        for (const GenieMark &m : marks) {
            if (m.group >= 0 && fits(kind, m.kind))
                groups[m.group] << m;
        }
        const int most = kind == GenieInteraction::TabNod ? 4 : 5;
        for (auto it = groups.cbegin(); it != groups.cend(); ++it) {
            QList<GenieMark> group = it.value();
            if (group.size() < 2)
                continue;
            const bool rows = group.first().kind == GenieMarkKind::Row;
            if (rows) {
                std::sort(group.begin(), group.end(),
                          [](const GenieMark &a, const GenieMark &b) { return a.rect.top() < b.rect.top(); });
                group = group.mid(std::max(0, int(group.size()) - most));
            } else {
                std::sort(group.begin(), group.end(), [from](const GenieMark &a, const GenieMark &b) {
                    return std::abs(a.rect.center().x() - from) < std::abs(b.rect.center().x() - from);
                });
                group = group.mid(0, most);
                std::sort(group.begin(), group.end(),
                          [](const GenieMark &a, const GenieMark &b) { return a.rect.left() < b.rect.left(); });
            }
            QList<QRectF> rects;
            for (const GenieMark &m : std::as_const(group))
                rects << m.rect;
            options << option(rects, !rows);
        }
    } else if (kind == GenieInteraction::Compare) {
        QList<QRectF> buttons;
        for (const GenieMark &m : marks) {
            if (m.kind == GenieMarkKind::Button && m.rect.width() <= 260)
                buttons << m.rect;
        }
        for (int i = 0; i < buttons.size() && options.size() < 60; ++i) {
            for (int j = i + 1; j < buttons.size() && options.size() < 60; ++j) {
                const QRectF a = buttons.at(i);
                const QRectF b = buttons.at(j);
                const double dx = std::abs(a.center().x() - b.center().x());
                if (std::abs(a.center().y() - b.center().y()) > 30 || dx < 24 || dx > 320)
                    continue;
                options << option(a.center().x() < b.center().x() ? QList<QRectF>{a, b} : QList<QRectF>{b, a}, true);
            }
        }
    } else {
        for (const GenieMark &m : marks) {
            if (fits(kind, m.kind))
                options << option({m.rect}, m.kind != GenieMarkKind::Row);
        }
    }
    QList<Option> reachable;
    for (const Option &o : std::as_const(options)) {
        const double cx = o.span.center().x();
        if (cx < stage.left - 60 || cx > stage.right + 60)
            continue;
        const double floor = stage.floor(std::clamp(cx, stage.left, stage.right));
        const double rise = floor - o.span.center().y();
        if (rise < lowestRise(kind) || rise > highestRise(kind) || o.span.bottom() > floor + 4)
            continue;
        if (kind == GenieInteraction::Read && o.span.width() < 40)
            continue;
        reachable << o;
    }
    std::stable_sort(reachable.begin(), reachable.end(), [from](const Option &a, const Option &b) {
        return std::abs(a.span.center().x() - from) < std::abs(b.span.center().x() - from);
    });
    const int n = std::min(kNearest, int(reachable.size()));
    for (int k = 0; k < n; ++k) {
        const Option &o = reachable.at(int((roll + quint32(k)) % quint32(n)));
        const double cx = o.span.center().x();
        double wanted = cx;
        if (kind == GenieInteraction::Hop) {
            const double dir = cx >= from ? 1 : -1;
            wanted = cx - dir * (o.span.width() / 2 + stage.side / 2 - kInsetX);
        }
        const double spot = GenieKeepClear::clearSpot(stage, wanted, o.shield ? o.marks : QList<QRectF>());
        if (std::abs(spot - cx) > o.span.width() / 2 + 170)
            continue;
        if ((kind == GenieInteraction::Hop || kind == GenieInteraction::Poof) && std::abs(spot - from) < 70)
            continue;
        out.kind = kind;
        out.marks = o.marks;
        out.spot = spot;
        return out;
    }
    return out;
}

}

GenieInteractionBag::GenieInteractionBag(quint32 seed)
    : m_random(seed)
{
}

QList<GenieInteraction> GenieInteractionBag::shuffled()
{
    QList<GenieInteraction> kinds = GeniePlay::all();
    for (int i = int(kinds.size()) - 1; i > 0; --i)
        kinds.swapItemsAt(i, int(m_random.bounded(i + 1)));
    return kinds;
}

GenieInteraction GenieInteractionBag::take(const std::function<bool(GenieInteraction)> &possible)
{
    if (m_bag.isEmpty())
        m_bag = shuffled();
    for (int i = 0; i < m_bag.size(); ++i) {
        if (possible(m_bag.at(i)))
            return m_bag.takeAt(i);
    }
    QList<GenieInteraction> fresh;
    for (const GenieInteraction kind : shuffled()) {
        if (!m_bag.contains(kind))
            fresh << kind;
    }
    for (int i = 0; i < fresh.size(); ++i) {
        if (!possible(fresh.at(i)))
            continue;
        const int at = int(m_bag.size()) + i;
        m_bag += fresh;
        return m_bag.takeAt(at);
    }
    return GenieInteraction::None;
}

namespace GenieLandmarks {

GenieScene scan(QWidget *host, const QList<const QWidget *> &skip)
{
    Scanner scanner;
    if (!host)
        return scanner.scene;
    scanner.skip = skip;
    scanner.base = std::max(1, QFontInfo(QApplication::font()).pixelSize());
    scanner.visit(host, QPoint(), host->rect());
    return scanner.scene;
}

}
