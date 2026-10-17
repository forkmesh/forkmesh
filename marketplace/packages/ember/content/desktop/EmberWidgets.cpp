#include "EmberWidgets.h"

#include <QDateTime>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace forkmesh::ember {

namespace {

bool isDark(const QWidget *widget)
{
    return widget->palette().color(QPalette::Window).lightness() < 128;
}

QColor accent(const QWidget *widget)
{
    return isDark(widget) ? QColor(QStringLiteral("#58a6ff")) : QColor(QStringLiteral("#0969da"));
}

QColor faded(QColor color, int alpha)
{
    color.setAlpha(alpha);
    return color;
}

QString compact(double value)
{
    const QLocale locale;
    if (std::abs(value) >= 10000)
        return locale.toString(value / 1000.0, 'f', 0) + QStringLiteral("k");
    if (std::abs(value) >= 1000)
        return locale.toString(value / 1000.0, 'f', 1) + QStringLiteral("k");
    return locale.toString(qint64(std::lround(value)));
}

}

RangeSlider::RangeSlider(QWidget *parent)
    : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setMinimumHeight(34);
    setCursor(Qt::PointingHandCursor);
}

QSize RangeSlider::sizeHint() const
{
    return {240, 34};
}

void RangeSlider::setRange(double minimum, double maximum, double step)
{
    m_min = minimum;
    m_max = std::max(minimum + step, maximum);
    m_step = step;
    m_low = std::clamp(m_low, m_min, m_max);
    m_high = std::clamp(m_high, m_min, m_max);
    update();
}

void RangeSlider::setSingle(bool single)
{
    m_single = single;
    m_health = !single && m_health;
    update();
}

void RangeSlider::setValues(double low, double high)
{
    m_low = std::clamp(std::min(low, high), m_min, m_max);
    m_high = std::clamp(std::max(low, high), m_min, m_max);
    update();
}

QRectF RangeSlider::track() const
{
    return QRectF(10, 10, width() - 20, 6);
}

double RangeSlider::xFor(double value) const
{
    const QRectF t = track();
    return t.left() + (value - m_min) / (m_max - m_min) * t.width();
}

double RangeSlider::valueAt(double x) const
{
    const QRectF t = track();
    const double raw = m_min + (x - t.left()) / std::max(1.0, t.width()) * (m_max - m_min);
    const double snapped = m_min + std::round((raw - m_min) / m_step) * m_step;
    return std::clamp(snapped, m_min, m_max);
}

void RangeSlider::apply(double low, double high)
{
    low = std::clamp(low, m_min, m_max);
    high = std::clamp(high, m_min, m_max);
    if (low > high)
        std::swap(low, high);
    if (low == m_low && high == m_high)
        return;
    m_low = low;
    m_high = high;
    update();
    if (m_onChanged)
        m_onChanged(m_low, m_high);
}

void RangeSlider::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const bool dark = isDark(this);
    const QRectF t = track();
    if (m_health) {
        QLinearGradient g(t.topLeft(), t.topRight());
        const auto at = [this](double v) { return (v - m_min) / (m_max - m_min); };
        g.setColorAt(0, bandColor(insight::HealthBand::Unhealthy));
        g.setColorAt(std::clamp(at(3.9), 0.0, 1.0), bandColor(insight::HealthBand::Unhealthy));
        g.setColorAt(std::clamp(at(4.1), 0.0, 1.0), bandColor(insight::HealthBand::Problematic));
        g.setColorAt(std::clamp(at(7.9), 0.0, 1.0), bandColor(insight::HealthBand::Problematic));
        g.setColorAt(std::clamp(at(8.1), 0.0, 1.0), bandColor(insight::HealthBand::Healthy));
        g.setColorAt(1, bandColor(insight::HealthBand::Healthy));
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        p.drawRoundedRect(t, 3, 3);
        const QColor veil = dark ? QColor(13, 17, 23, 190) : QColor(255, 255, 255, 200);
        p.setBrush(veil);
        p.drawRect(QRectF(t.left() - 1, t.top() - 1, xFor(m_low) - t.left() + 1, t.height() + 2));
        p.drawRect(QRectF(xFor(m_high), t.top() - 1, t.right() - xFor(m_high) + 1, t.height() + 2));
    } else {
        p.setPen(Qt::NoPen);
        p.setBrush(dark ? QColor(255, 255, 255, 30) : QColor(31, 35, 40, 30));
        p.drawRoundedRect(t, 3, 3);
        p.setBrush(accent(this));
        p.drawRoundedRect(QRectF(xFor(m_low), t.top(), xFor(m_high) - xFor(m_low), t.height()), 3, 3);
    }
    if (m_single) {
        const QColor text = palette().color(QPalette::WindowText);
        const QPointF c(xFor(m_low), t.center().y());
        p.setPen(QPen(hasFocus() ? accent(this) : faded(text, 90), hasFocus() ? 2 : 1));
        p.setBrush(dark ? QColor(240, 246, 252) : QColor(255, 255, 255));
        p.drawEllipse(c, 7, 7);
        return;
    }
    const QColor text = palette().color(QPalette::WindowText);
    QFont f = font();
    f.setPixelSize(10);
    p.setFont(f);
    for (int handle = 0; handle < 2; ++handle) {
        const double v = handle == 0 ? m_low : m_high;
        const QPointF c(xFor(v), t.center().y());
        p.setPen(QPen(hasFocus() && m_focusHandle == handle ? accent(this) : faded(text, 90),
                      hasFocus() && m_focusHandle == handle ? 2 : 1));
        p.setBrush(dark ? QColor(240, 246, 252) : QColor(255, 255, 255));
        p.drawEllipse(c, 7, 7);
        p.setPen(faded(text, 190));
        const QString label = QLocale().toString(v, 'f', std::fmod(v, 1.0) == 0 ? 0 : 1);
        p.drawText(QRectF(c.x() - 20, t.bottom() + 5, 40, 14), Qt::AlignCenter, label);
    }
}

void RangeSlider::mousePressEvent(QMouseEvent *event)
{
    const double x = event->position().x();
    const double dl = std::abs(x - xFor(m_low));
    const double dh = std::abs(x - xFor(m_high));
    m_drag = m_single || dl < dh || (dl == dh && x < xFor(m_low)) ? 0 : 1;
    m_focusHandle = m_drag;
    mouseMoveEvent(event);
}

void RangeSlider::mouseMoveEvent(QMouseEvent *event)
{
    if (m_drag < 0)
        return;
    const double v = valueAt(event->position().x());
    if (m_single)
        apply(v, m_max);
    else if (m_drag == 0)
        apply(std::min(v, m_high), m_high);
    else
        apply(m_low, std::max(v, m_low));
}

void RangeSlider::mouseReleaseEvent(QMouseEvent *)
{
    m_drag = -1;
}

void RangeSlider::keyPressEvent(QKeyEvent *event)
{
    double delta = 0;
    if (event->key() == Qt::Key_Left || event->key() == Qt::Key_Down)
        delta = -m_step;
    else if (event->key() == Qt::Key_Right || event->key() == Qt::Key_Up)
        delta = m_step;
    else if (event->key() == Qt::Key_Tab && m_focusHandle == 0 && !m_single) {
        m_focusHandle = 1;
        update();
        return;
    } else
        return QWidget::keyPressEvent(event);
    if (m_single)
        apply(m_low + delta, m_max);
    else if (m_focusHandle == 0)
        apply(std::min(m_low + delta, m_high), m_high);
    else
        apply(m_low, std::max(m_high + delta, m_low));
}

HealthRing::HealthRing(QWidget *parent)
    : QWidget(parent)
{
    setFixedSize(84, 84);
}

void HealthRing::setHealth(double health)
{
    m_health = health;
    update();
}

void HealthRing::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const bool dark = isDark(this);
    const QRectF ring = QRectF(rect()).adjusted(7, 7, -7, -7);
    const QColor color = healthColor(m_health);
    p.setPen(QPen(dark ? QColor(255, 255, 255, 28) : QColor(31, 35, 40, 26), 8, Qt::SolidLine, Qt::FlatCap));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(ring);
    const double share = std::clamp((m_health - 1) / 9.0, 0.02, 1.0);
    p.setPen(QPen(color, 8, Qt::SolidLine, Qt::RoundCap));
    p.drawArc(ring, 90 * 16, -int(share * 360 * 16));
    QFont f = font();
    f.setPixelSize(22);
    f.setBold(true);
    p.setFont(f);
    p.setPen(palette().color(QPalette::WindowText));
    const QPointF c = QRectF(rect()).center();
    p.drawText(QRectF(c.x() - 30, c.y() - 20, 60, 28), Qt::AlignHCenter | Qt::AlignBottom,
               QLocale().toString(m_health, 'f', m_health >= 9.95 ? 0 : 1));
    f.setPixelSize(10);
    f.setBold(false);
    p.setFont(f);
    p.setPen(faded(palette().color(QPalette::WindowText), 150));
    p.drawText(QRectF(c.x() - 30, c.y() + 8, 60, 14), Qt::AlignHCenter | Qt::AlignTop, QStringLiteral("of 10"));
}

TrendChart::TrendChart(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setMinimumHeight(260);
}

void TrendChart::setPoints(const QList<insight::TrendPoint> &points)
{
    m_points = points;
    m_message.clear();
    m_hover = -1;
    update();
}

void TrendChart::setMessage(const QString &text)
{
    m_message = text;
    m_points.clear();
    m_hover = -1;
    update();
}

QRectF TrendChart::plotRect(int band) const
{
    const double top = 26;
    const double gap = 34;
    const double bottom = 70;
    const double h = (height() - top - gap - bottom) / 2;
    const double y = top + band * (h + gap);
    return QRectF(44, y, width() - 56, std::max(20.0, h));
}

double TrendChart::xFor(int index) const
{
    const QRectF r = plotRect(0);
    if (m_points.size() < 2)
        return r.center().x();
    const qint64 t0 = m_points.first().time;
    const qint64 t1 = m_points.last().time;
    if (t1 > t0)
        return r.left() + double(m_points[index].time - t0) / double(t1 - t0) * r.width();
    return r.left() + double(index) / (m_points.size() - 1) * r.width();
}

void TrendChart::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QColor text = palette().color(QPalette::WindowText);
    const bool dark = isDark(this);
    QFont f = font();
    f.setPixelSize(11);
    p.setFont(f);
    if (m_points.isEmpty()) {
        p.setPen(faded(text, 160));
        p.drawText(rect().adjusted(16, 16, -16, -16), Qt::AlignCenter | Qt::TextWordWrap,
                   m_message.isEmpty() ? QStringLiteral("Select a file to see how its size and complexity changed.")
                                       : m_message);
        return;
    }
    const QLocale locale;
    const QColor series[2] = {dark ? QColor(QStringLiteral("#8b949e")) : QColor(QStringLiteral("#57606a")),
                              QColor(QStringLiteral("#f0883e"))};
    const QString titles[2] = {QStringLiteral("Lines of code"), QStringLiteral("Complexity")};
    for (int band = 0; band < 2; ++band) {
        const QRectF r = plotRect(band);
        double maxV = 1;
        for (const insight::TrendPoint &pt : m_points)
            maxV = std::max<double>(maxV, band == 0 ? pt.codeLines : pt.complexity);
        const double niceMax = [maxV] {
            const double magnitude = std::pow(10, std::floor(std::log10(maxV)));
            for (double m : {1.0, 2.0, 2.5, 5.0, 10.0}) {
                if (m * magnitude >= maxV)
                    return m * magnitude;
            }
            return 10 * magnitude;
        }();
        const auto yFor = [&](double v) { return r.bottom() - v / niceMax * r.height(); };
        QFont title = f;
        title.setBold(true);
        p.setFont(title);
        p.setPen(text);
        const int last = m_hover >= 0 ? m_hover : m_points.size() - 1;
        const double current = band == 0 ? m_points[last].codeLines : m_points[last].complexity;
        p.drawText(QRectF(r.left() - 36, r.top() - 22, r.width(), 16), Qt::AlignLeft | Qt::AlignVCenter,
                   titles[band]);
        p.setFont(f);
        p.setPen(faded(text, 170));
        p.drawText(QRectF(r.left(), r.top() - 22, r.width(), 16), Qt::AlignRight | Qt::AlignVCenter,
                   locale.toString(qint64(current)));
        p.setPen(QPen(dark ? QColor(255, 255, 255, 22) : QColor(31, 35, 40, 24), 1));
        for (int g = 0; g <= 2; ++g) {
            const double y = yFor(niceMax * g / 2);
            p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
        }
        p.setPen(faded(text, 130));
        QFont axis = f;
        axis.setPixelSize(10);
        p.setFont(axis);
        for (int g = 0; g <= 2; ++g) {
            const double y = yFor(niceMax * g / 2);
            p.drawText(QRectF(0, y - 7, r.left() - 6, 14), Qt::AlignRight | Qt::AlignVCenter,
                       compact(niceMax * g / 2));
        }
        QPainterPath line;
        QPainterPath area;
        for (int i = 0; i < m_points.size(); ++i) {
            const QPointF pt(xFor(i), yFor(band == 0 ? m_points[i].codeLines : m_points[i].complexity));
            if (i == 0) {
                line.moveTo(pt);
                area.moveTo(pt.x(), r.bottom());
                area.lineTo(pt);
            } else {
                line.lineTo(pt);
                area.lineTo(pt);
            }
        }
        area.lineTo(xFor(m_points.size() - 1), r.bottom());
        area.closeSubpath();
        QLinearGradient fill(r.topLeft(), r.bottomLeft());
        fill.setColorAt(0, faded(series[band], 70));
        fill.setColorAt(1, faded(series[band], 0));
        p.setPen(Qt::NoPen);
        p.setBrush(fill);
        p.drawPath(area);
        p.setPen(QPen(series[band], 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPath(line);
        const int marker = m_hover >= 0 ? m_hover : m_points.size() - 1;
        const QPointF mp(xFor(marker), yFor(band == 0 ? m_points[marker].codeLines : m_points[marker].complexity));
        p.setPen(QPen(palette().color(QPalette::Base), 2));
        p.setBrush(series[band]);
        p.drawEllipse(mp, 4.5, 4.5);
    }
    const QRectF bottomPlot = plotRect(1);
    QFont axis = f;
    axis.setPixelSize(10);
    p.setFont(axis);
    p.setPen(faded(text, 140));
    const QString startText = QDateTime::fromSecsSinceEpoch(m_points.first().time).toString(QStringLiteral("MMM yyyy"));
    const QString endText = QDateTime::fromSecsSinceEpoch(m_points.last().time).toString(QStringLiteral("MMM yyyy"));
    p.drawText(QRectF(bottomPlot.left(), bottomPlot.bottom() + 4, 120, 14), Qt::AlignLeft, startText);
    p.drawText(QRectF(bottomPlot.right() - 120, bottomPlot.bottom() + 4, 120, 14), Qt::AlignRight, endText);
    if (m_hover >= 0 && m_hover < m_points.size()) {
        const double x = xFor(m_hover);
        p.setPen(QPen(faded(text, 90), 1, Qt::DashLine));
        p.drawLine(QPointF(x, plotRect(0).top()), QPointF(x, bottomPlot.bottom()));
    }
    const int shown = m_hover >= 0 ? m_hover : m_points.size() - 1;
    const insight::TrendPoint &pt = m_points[shown];
    const QString when = QDateTime::fromSecsSinceEpoch(pt.time).toString(QStringLiteral("d MMM yyyy"));
    const QString info = QStringLiteral("%1 · %2 · %3\nHealth %4 · worst function %5")
                             .arg(pt.commit.left(8), pt.author, when,
                                  locale.toString(pt.health, 'f', 1), locale.toString(pt.maxComplexity));
    p.setPen(faded(text, m_hover >= 0 ? 230 : 160));
    p.drawText(QRectF(bottomPlot.left() - 36, bottomPlot.bottom() + 22, width() - bottomPlot.left(), 34),
               Qt::AlignLeft | Qt::AlignTop, info);
}

void TrendChart::mouseMoveEvent(QMouseEvent *event)
{
    if (m_points.isEmpty())
        return;
    int best = -1;
    double bestD = 1e9;
    for (int i = 0; i < m_points.size(); ++i) {
        const double d = std::abs(xFor(i) - event->position().x());
        if (d < bestD) {
            bestD = d;
            best = i;
        }
    }
    if (best != m_hover) {
        m_hover = best;
        update();
    }
}

void TrendChart::leaveEvent(QEvent *event)
{
    QWidget::leaveEvent(event);
    m_hover = -1;
    update();
}

RowList::RowList(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
}

void RowList::setRows(const QList<RowItem> &rows)
{
    m_rows = rows;
    m_hover = -1;
    updateGeometry();
    update();
}

void RowList::setEmptyText(const QString &text)
{
    m_empty = text;
    update();
}

void RowList::setSelectedId(int id)
{
    m_selectedId = id;
    update();
}

int RowList::rowHeight(const RowItem &row) const
{
    return 48 + 15 * int(row.notes.size());
}

QSize RowList::sizeHint() const
{
    return {300, heightForWidth(300)};
}

int RowList::heightForWidth(int) const
{
    if (m_rows.isEmpty())
        return 60;
    int h = 0;
    for (const RowItem &row : m_rows)
        h += rowHeight(row);
    return h + 4;
}

int RowList::rowAt(double y) const
{
    double top = 0;
    for (int i = 0; i < m_rows.size(); ++i) {
        const int h = rowHeight(m_rows[i]);
        if (y >= top && y < top + h)
            return i;
        top += h;
    }
    return -1;
}

void RowList::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QColor text = palette().color(QPalette::WindowText);
    const bool dark = isDark(this);
    QFont f = font();
    f.setPixelSize(12);
    if (m_rows.isEmpty()) {
        p.setFont(f);
        p.setPen(faded(text, 150));
        p.drawText(rect().adjusted(4, 4, -4, -4), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, m_empty);
        return;
    }
    double top = 0;
    for (int i = 0; i < m_rows.size(); ++i) {
        const RowItem &row = m_rows[i];
        const double h = rowHeight(row);
        const QRectF r(0, top, width(), h);
        if (row.id == m_selectedId) {
            p.setPen(Qt::NoPen);
            p.setBrush(faded(accent(this), dark ? 40 : 28));
            p.drawRoundedRect(r.adjusted(0, 1, 0, -1), 6, 6);
        } else if (i == m_hover) {
            p.setPen(Qt::NoPen);
            p.setBrush(dark ? QColor(255, 255, 255, 12) : QColor(31, 35, 40, 10));
            p.drawRoundedRect(r.adjusted(0, 1, 0, -1), 6, 6);
        }
        double x = 8;
        if (row.rank > 0) {
            QFont rf = f;
            rf.setBold(true);
            rf.setPixelSize(11);
            p.setFont(rf);
            const QRectF badge(x, top + 9, 20, 20);
            p.setPen(Qt::NoPen);
            p.setBrush(row.color.isValid() ? row.color : faded(text, 60));
            p.drawEllipse(badge);
            p.setPen(QColor(13, 17, 23));
            p.drawText(badge, Qt::AlignCenter, QString::number(row.rank));
            x += 28;
        }
        QFont tf = f;
        tf.setWeight(QFont::DemiBold);
        p.setFont(tf);
        const QFontMetricsF tfm(tf);
        QFont vf = f;
        vf.setPixelSize(11);
        const double valueW = row.value.isEmpty() ? 0 : QFontMetricsF(vf).horizontalAdvance(row.value) + 10;
        p.setPen(text);
        p.drawText(QRectF(x, top + 6, width() - x - valueW - 8, 18), Qt::AlignLeft | Qt::AlignVCenter,
                   tfm.elidedText(row.title, Qt::ElideMiddle, width() - x - valueW - 8));
        if (!row.value.isEmpty()) {
            p.setFont(vf);
            p.setPen(faded(text, 200));
            p.drawText(QRectF(width() - valueW - 8, top + 6, valueW, 18), Qt::AlignRight | Qt::AlignVCenter, row.value);
        }
        QFont df = f;
        df.setPixelSize(11);
        p.setFont(df);
        const QFontMetricsF dfm(df);
        p.setPen(faded(text, 140));
        p.drawText(QRectF(x, top + 23, width() - x - 8, 14), Qt::AlignLeft | Qt::AlignVCenter,
                   dfm.elidedText(row.detail, Qt::ElideMiddle, width() - x - 8));
        const QRectF bar(x, top + 40, width() - x - 8, 3);
        p.setPen(Qt::NoPen);
        p.setBrush(dark ? QColor(255, 255, 255, 18) : QColor(31, 35, 40, 18));
        p.drawRoundedRect(bar, 1.5, 1.5);
        p.setBrush(row.color.isValid() ? row.color : accent(this));
        p.drawRoundedRect(QRectF(bar.left(), bar.top(), bar.width() * std::clamp(row.fraction, 0.0, 1.0), bar.height()),
                          1.5, 1.5);
        double ny = top + 47;
        p.setPen(faded(text, 165));
        for (const QString &note : row.notes) {
            p.drawText(QRectF(x, ny, width() - x - 8, 14), Qt::AlignLeft | Qt::AlignVCenter,
                       dfm.elidedText(QStringLiteral("· ") + note, Qt::ElideRight, width() - x - 8));
            ny += 15;
        }
        top += h;
    }
}

void RowList::mouseMoveEvent(QMouseEvent *event)
{
    const int row = rowAt(event->position().y());
    if (row != m_hover) {
        m_hover = row;
        setCursor(row >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }
}

void RowList::mousePressEvent(QMouseEvent *event)
{
    const int row = rowAt(event->position().y());
    if (row >= 0 && event->button() == Qt::LeftButton && m_onActivated)
        m_onActivated(m_rows[row].id);
}

void RowList::mouseDoubleClickEvent(QMouseEvent *event)
{
    const int row = rowAt(event->position().y());
    if (row >= 0 && m_onOpened)
        m_onOpened(m_rows[row].id);
}

void RowList::leaveEvent(QEvent *event)
{
    QWidget::leaveEvent(event);
    m_hover = -1;
    update();
}

} // namespace forkmesh::ember
