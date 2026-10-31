#include "DjWidgets.h"
#include "DjModel.h"

#include <QConicalGradient>
#include <QDateTime>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace forkmesh::djstudio {

namespace theme {

QColor deck(int index)
{
    static const QColor colors[] = {QColor(0x23, 0xa6, 0xff), QColor(0xff, 0x3d, 0x3d), QColor(0x3d, 0xd6, 0x5c),
                                    QColor(0xff, 0x9f, 0x1c)};
    return colors[std::clamp(index, 0, 3)];
}

QColor background() { return QColor(0x15, 0x16, 0x18); }
QColor panel() { return QColor(0x22, 0x23, 0x26); }
QColor raised() { return QColor(0x31, 0x33, 0x37); }
QColor line() { return QColor(0x3c, 0x3e, 0x43); }
QColor text() { return QColor(0xe6, 0xe8, 0xeb); }
QColor dim() { return QColor(0x8c, 0x91, 0x99); }
QColor accent() { return QColor(0x23, 0xa6, 0xff); }

QFont font(int pixels, bool bold)
{
    QFont f;
    f.setPixelSize(pixels);
    f.setBold(bold);
    return f;
}

QFont mono(int pixels, bool bold)
{
    QFont f(QStringLiteral("DejaVu Sans Mono"));
    f.setStyleHint(QFont::Monospace);
    f.setPixelSize(pixels);
    f.setBold(bold);
    return f;
}

}

QPixmap circular(const QImage &image, int size)
{
    QPixmap out(size, size);
    out.fill(Qt::transparent);
    if (image.isNull() || image.width() < 2)
        return out;
    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    QPainterPath clip;
    clip.addEllipse(QRectF(0, 0, size, size));
    painter.setClipPath(clip);
    painter.drawImage(QRectF(0, 0, size, size), image);
    return out;
}

QColor waveColor(double low, double mid, double high, double brightness)
{
    const double sum = std::max(0.0001, low + mid + high);
    low /= sum;
    mid /= sum;
    high /= sum;
    const double r = low * 255 + mid * 60 + high * 70;
    const double g = low * 70 + mid * 220 + high * 150;
    const double b = low * 40 + mid * 70 + high * 255;
    const double peak = std::max({r, g, b, 1.0});
    const double scale = 255 / peak * std::clamp(brightness, 0.0, 1.0);
    return QColor(int(std::clamp(r * scale, 0.0, 255.0)), int(std::clamp(g * scale, 0.0, 255.0)),
                  int(std::clamp(b * scale, 0.0, 255.0)));
}

DjButton::DjButton(const QString &label, QWidget *parent) : QWidget(parent), m_label(label), m_color(theme::accent())
{
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_Hover);
    setMinimumSize(22, 18);
}

void DjButton::setLabel(const QString &label)
{
    if (m_label == label)
        return;
    m_label = label;
    update();
}

void DjButton::setLit(bool lit)
{
    if (m_lit == lit)
        return;
    m_lit = lit;
    update();
}

void DjButton::setColor(const QColor &color)
{
    m_color = color;
    update();
}

void DjButton::setBlink(bool blink)
{
    m_blink = blink;
    update();
}

void DjButton::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const bool blinkOff = m_blink && (QDateTime::currentMSecsSinceEpoch() / 350) % 2;
    const bool on = m_lit && !blinkOff;
    QColor face = on ? (m_filled ? m_color : m_color.darker(260)) : theme::raised();
    if (m_down)
        face = face.darker(130);
    else if (m_hover)
        face = face.lighter(118);
    QLinearGradient gradient(r.topLeft(), r.bottomLeft());
    gradient.setColorAt(0, face.lighter(112));
    gradient.setColorAt(1, face.darker(112));
    painter.setPen(QPen(on ? m_color : theme::line(), 1));
    painter.setBrush(gradient);
    const double radius = m_round ? std::min(r.width(), r.height()) / 2 : 3;
    painter.drawRoundedRect(r, radius, radius);
    if (!isEnabled())
        painter.setPen(theme::dim().darker(150));
    else
        painter.setPen(on ? (m_filled ? QColor(0x10, 0x10, 0x10) : m_color.lighter(130)) : theme::text());
    painter.setFont(theme::font(m_fontSize, true));
    painter.drawText(r, Qt::AlignCenter, m_label);
}

void DjButton::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::RightButton) {
        if (rightClicked)
            rightClicked();
        return;
    }
    if (event->button() != Qt::LeftButton)
        return;
    m_down = true;
    update();
    if (pressed)
        pressed();
}

void DjButton::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || !m_down)
        return;
    m_down = false;
    update();
    if (released)
        released();
    if (rect().contains(event->position().toPoint()) && clicked)
        clicked();
}

void DjButton::enterEvent(QEnterEvent *)
{
    m_hover = true;
    update();
}

void DjButton::leaveEvent(QEvent *)
{
    m_hover = false;
    update();
}

DjKnob::DjKnob(const QString &label, bool bipolar, QWidget *parent)
    : QWidget(parent), m_label(label), m_bipolar(bipolar), m_color(theme::accent())
{
    setCursor(Qt::SizeVerCursor);
    setMinimumSize(30, 40);
}

void DjKnob::setValue(double value)
{
    const double clamped = std::clamp(value, m_bipolar ? -1.0 : 0.0, 1.0);
    if (std::abs(clamped - m_value) < 1e-6)
        return;
    m_value = clamped;
    update();
}

void DjKnob::apply(double value)
{
    setValue(value);
    if (changed)
        changed(m_value);
}

void DjKnob::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const int labelHeight = m_label.isEmpty() ? 0 : 11;
    const double size = std::min<double>(width() - 2, height() - labelHeight - 2);
    const QRectF dial((width() - size) / 2, 1, size, size);
    const double start = 225;
    const double span = -270;
    const double normal = m_bipolar ? (m_value + 1) / 2 : m_value;
    painter.setPen(QPen(theme::line(), 3, Qt::SolidLine, Qt::FlatCap));
    painter.drawArc(dial.adjusted(2, 2, -2, -2), int(start * 16), int(span * 16));
    painter.setPen(QPen(m_marked ? QColor(0xff, 0x55, 0x55) : m_color, 3, Qt::SolidLine, Qt::FlatCap));
    const double from = m_bipolar ? 90 : start;
    const double to = start + span * normal;
    painter.drawArc(dial.adjusted(2, 2, -2, -2), int(from * 16), int((to - from) * 16));
    QRadialGradient face(dial.center() - QPointF(size * 0.15, size * 0.15), size * 0.6);
    face.setColorAt(0, QColor(0x5a, 0x5d, 0x63));
    face.setColorAt(1, QColor(0x22, 0x23, 0x26));
    painter.setPen(QPen(QColor(0x10, 0x10, 0x10), 1));
    painter.setBrush(face);
    const QRectF inner = dial.adjusted(size * 0.2, size * 0.2, -size * 0.2, -size * 0.2);
    painter.drawEllipse(inner);
    const double radians = qDegreesToRadians(to);
    const QPointF center = inner.center();
    const double reach = inner.width() / 2 - 1;
    painter.setPen(QPen(theme::text(), 2, Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(center + QPointF(std::cos(radians), -std::sin(radians)) * reach * 0.25,
                     center + QPointF(std::cos(radians), -std::sin(radians)) * reach);
    if (labelHeight) {
        painter.setPen(theme::dim());
        painter.setFont(theme::font(9, true));
        painter.drawText(QRectF(0, height() - labelHeight, width(), labelHeight), Qt::AlignCenter,
                         m_caption.isEmpty() ? m_label : m_caption);
    }
}

void DjKnob::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::RightButton) {
        if (rightClicked)
            rightClicked();
        return;
    }
    m_pressY = int(event->position().y());
    m_dragStart = m_value;
}

void DjKnob::mouseMoveEvent(QMouseEvent *event)
{
    if (!(event->buttons() & Qt::LeftButton))
        return;
    const double range = m_bipolar ? 2 : 1;
    const double fine = event->modifiers() & Qt::ShiftModifier ? 0.25 : 1;
    apply(m_dragStart + (m_pressY - event->position().y()) / 140.0 * range * fine);
}

void DjKnob::mouseDoubleClickEvent(QMouseEvent *) { apply(m_default); }

void DjKnob::wheelEvent(QWheelEvent *event)
{
    const double step = (m_bipolar ? 2 : 1) / 40.0;
    apply(m_value + (event->angleDelta().y() > 0 ? step : -step));
    event->accept();
}

DjFader::DjFader(Qt::Orientation orientation, bool bipolar, QWidget *parent)
    : QWidget(parent), m_orientation(orientation), m_bipolar(bipolar), m_color(theme::accent())
{
    setCursor(orientation == Qt::Vertical ? Qt::SizeVerCursor : Qt::SizeHorCursor);
    if (orientation == Qt::Vertical)
        setMinimumSize(22, 60);
    else
        setMinimumSize(60, 22);
}

void DjFader::setValue(double value)
{
    const double clamped = std::clamp(value, m_bipolar ? -1.0 : 0.0, 1.0);
    if (std::abs(clamped - m_value) < 1e-6)
        return;
    m_value = clamped;
    update();
}

void DjFader::apply(double value)
{
    setValue(value);
    if (changed)
        changed(m_value);
}

QRectF DjFader::track() const
{
    if (m_orientation == Qt::Vertical)
        return QRectF(width() / 2.0 - 2, 10, 4, height() - 20);
    return QRectF(14, height() / 2.0 - 2, width() - 28, 4);
}

double DjFader::fromPoint(const QPointF &point) const
{
    const QRectF t = track();
    double unit = m_orientation == Qt::Vertical ? 1 - (point.y() - t.top()) / t.height() : (point.x() - t.left()) / t.width();
    unit = std::clamp(unit, 0.0, 1.0);
    if (m_inverted)
        unit = 1 - unit;
    return m_bipolar ? unit * 2 - 1 : unit;
}

void DjFader::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF t = track();
    painter.setPen(theme::line());
    for (int i = 0; i <= m_ticks; ++i) {
        const double f = double(i) / m_ticks;
        if (m_orientation == Qt::Vertical) {
            const double y = t.top() + t.height() * f;
            const double w = (i == m_ticks / 2 && m_bipolar) ? 9 : 5;
            painter.drawLine(QPointF(t.left() - w - 2, y), QPointF(t.left() - 3, y));
            painter.drawLine(QPointF(t.right() + 3, y), QPointF(t.right() + w + 2, y));
        } else {
            const double x = t.left() + t.width() * f;
            const double h = (i == m_ticks / 2 && m_bipolar) ? 8 : 4;
            painter.drawLine(QPointF(x, t.top() - h - 2), QPointF(x, t.top() - 3));
        }
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0x0b, 0x0b, 0x0c));
    painter.drawRoundedRect(t, 2, 2);
    double unit = m_bipolar ? (m_value + 1) / 2 : m_value;
    if (m_inverted)
        unit = 1 - unit;
    painter.setBrush(m_color.darker(150));
    if (m_orientation == Qt::Vertical) {
        const double y = t.bottom() - t.height() * unit;
        if (m_bipolar)
            painter.drawRect(QRectF(t.left(), std::min(y, t.center().y()), t.width(), std::abs(y - t.center().y())));
        else
            painter.drawRect(QRectF(t.left(), y, t.width(), t.bottom() - y));
        const QRectF cap(width() / 2.0 - 11, y - 7, 22, 14);
        QLinearGradient g(cap.topLeft(), cap.bottomLeft());
        g.setColorAt(0, QColor(0x6a, 0x6d, 0x73));
        g.setColorAt(0.5, QColor(0x3a, 0x3c, 0x40));
        g.setColorAt(1, QColor(0x55, 0x58, 0x5e));
        painter.setBrush(g);
        painter.setPen(QPen(QColor(0x0e, 0x0e, 0x0e), 1));
        painter.drawRoundedRect(cap, 2, 2);
        painter.setPen(QPen(m_color, 2));
        painter.drawLine(QPointF(cap.left() + 3, cap.center().y()), QPointF(cap.right() - 3, cap.center().y()));
    } else {
        const double x = t.left() + t.width() * unit;
        const QRectF cap(x - 8, height() / 2.0 - 11, 16, 22);
        QLinearGradient g(cap.topLeft(), cap.topRight());
        g.setColorAt(0, QColor(0x6a, 0x6d, 0x73));
        g.setColorAt(0.5, QColor(0x3a, 0x3c, 0x40));
        g.setColorAt(1, QColor(0x55, 0x58, 0x5e));
        painter.setBrush(g);
        painter.setPen(QPen(QColor(0x0e, 0x0e, 0x0e), 1));
        painter.drawRoundedRect(cap, 2, 2);
        painter.setPen(QPen(m_color, 2));
        painter.drawLine(QPointF(cap.center().x(), cap.top() + 3), QPointF(cap.center().x(), cap.bottom() - 3));
    }
}

void DjFader::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    m_grab = fromPoint(event->position()) - m_value;
    const double reach = m_bipolar ? 0.12 : 0.06;
    if (std::abs(m_grab) > reach) {
        m_grab = 0;
        apply(fromPoint(event->position()));
    }
}

void DjFader::mouseMoveEvent(QMouseEvent *event)
{
    if (event->buttons() & Qt::LeftButton)
        apply(fromPoint(event->position()) - m_grab);
}

void DjFader::mouseDoubleClickEvent(QMouseEvent *) { apply(m_default); }

void DjFader::wheelEvent(QWheelEvent *event)
{
    const double step = (m_bipolar ? 2 : 1) / (event->modifiers() & Qt::ShiftModifier ? 400.0 : 60.0);
    apply(m_value + (event->angleDelta().y() > 0 ? step : -step) * (m_inverted ? -1 : 1));
    event->accept();
}

DjMeter::DjMeter(Qt::Orientation orientation, QWidget *parent) : QWidget(parent), m_orientation(orientation)
{
    if (orientation == Qt::Vertical)
        setFixedWidth(8);
    else
        setFixedHeight(8);
}

void DjMeter::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    const double value = level ? std::clamp(level(), 0.0, 1.2) : 0;
    const double peak = hold ? std::clamp(hold(), 0.0, 1.2) : 0;
    const int segments = m_orientation == Qt::Vertical ? std::max(6, height() / 5) : std::max(6, width() / 5);
    const double db = value > 0.0001 ? 20 * std::log10(value) : -60;
    const double unit = std::clamp((db + 42) / 45, 0.0, 1.0);
    const double peakDb = peak > 0.0001 ? 20 * std::log10(peak) : -60;
    const double peakUnit = std::clamp((peakDb + 42) / 45, 0.0, 1.0);
    painter.fillRect(rect(), QColor(0x0b, 0x0b, 0x0c));
    for (int i = 0; i < segments; ++i) {
        const double f = double(i) / segments;
        const bool on = f < unit || int(peakUnit * segments) == i;
        QColor c = f > 0.86 ? QColor(0xff, 0x3d, 0x3d) : f > 0.7 ? QColor(0xff, 0xc4, 0x1c) : QColor(0x3d, 0xd6, 0x5c);
        if (!on)
            c = c.darker(420);
        if (m_orientation == Qt::Vertical) {
            const double h = double(height()) / segments;
            painter.fillRect(QRectF(1, height() - (i + 1) * h + 1, width() - 2, h - 1.5), c);
        } else {
            const double w = double(width()) / segments;
            painter.fillRect(QRectF(i * w + 1, 1, w - 1.5, height() - 2), c);
        }
    }
}

DjPlatter::DjPlatter(DjModel *model, int deck, QWidget *parent) : QWidget(parent), m_model(model), m_deck(deck)
{
    setMinimumSize(110, 110);
    setCursor(Qt::OpenHandCursor);
    setToolTip(QStringLiteral("Drag the record to scratch (Shift-drag or the wheel nudges the tempo); a touch slows it like a fingertip on vinyl."));
}

void DjPlatter::resizeEvent(QResizeEvent *)
{
    m_vinyl = QPixmap();
    m_labelFor.clear();
}

void DjPlatter::bakeVinyl()
{
    const int side = std::min(width(), height());
    const qreal ratio = devicePixelRatioF();
    m_vinyl = QPixmap(int(side * ratio), int(side * ratio));
    m_vinyl.setDevicePixelRatio(ratio);
    m_vinyl.fill(Qt::transparent);
    QPainter painter(&m_vinyl);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF disc(side * 0.07, side * 0.07, side * 0.86, side * 0.86);
    QRadialGradient g(disc.center(), disc.width() / 2);
    g.setColorAt(0, QColor(0x1a, 0x1a, 0x1c));
    g.setColorAt(0.97, QColor(0x0c, 0x0c, 0x0d));
    g.setColorAt(1, QColor(0x2a, 0x2a, 0x2d));
    painter.setPen(Qt::NoPen);
    painter.setBrush(g);
    painter.drawEllipse(disc);
    for (int i = 0; i < 18; ++i) {
        const double inset = disc.width() * (0.04 + i * 0.017);
        painter.setPen(QPen(QColor(255, 255, 255, i % 3 ? 7 : 14), 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(disc.adjusted(inset, inset, -inset, -inset));
    }
    QConicalGradient shine(disc.center(), 45);
    shine.setColorAt(0, QColor(255, 255, 255, 0));
    shine.setColorAt(0.12, QColor(255, 255, 255, 22));
    shine.setColorAt(0.25, QColor(255, 255, 255, 0));
    shine.setColorAt(0.5, QColor(255, 255, 255, 0));
    shine.setColorAt(0.62, QColor(255, 255, 255, 18));
    shine.setColorAt(0.75, QColor(255, 255, 255, 0));
    shine.setColorAt(1, QColor(255, 255, 255, 0));
    painter.setPen(Qt::NoPen);
    painter.setBrush(shine);
    painter.drawEllipse(disc);
}

void DjPlatter::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    const int side = std::min(width(), height());
    painter.translate((width() - side) / 2.0, (height() - side) / 2.0);
    const QRectF whole(0, 0, side, side);
    DjDeck &deck = m_model->deck(m_deck);
    const QColor color = theme::deck(m_deck);
    const double at = DjModel::now();
    const double turn = m_model->turned(m_deck, at);
    const double position = m_model->shown(m_deck, at);
    painter.setPen(Qt::NoPen);
    QRadialGradient rim(whole.center(), side / 2.0);
    rim.setColorAt(0.86, QColor(0x2c, 0x2d, 0x31));
    rim.setColorAt(0.93, QColor(0x4a, 0x4c, 0x52));
    rim.setColorAt(1, QColor(0x1b, 0x1c, 0x1e));
    painter.setBrush(rim);
    painter.drawEllipse(whole.adjusted(1, 1, -1, -1));
    const QRectF ring = whole.adjusted(side * 0.035, side * 0.035, -side * 0.035, -side * 0.035);
    painter.setPen(QPen(QColor(0x0d, 0x0d, 0x0e), side * 0.03, Qt::SolidLine, Qt::FlatCap));
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(ring);
    if (deck.duration > 0) {
        const double progress = std::clamp(position / deck.duration, 0.0, 1.0);
        const bool ending = deck.duration - position < 30 && deck.playing;
        const QColor arc = ending && (QDateTime::currentMSecsSinceEpoch() / 400) % 2 ? QColor(0xff, 0x3d, 0x3d) : color;
        painter.setPen(QPen(arc, side * 0.03, Qt::SolidLine, Qt::FlatCap));
        painter.drawArc(ring, 90 * 16, int(-progress * 360 * 16));
    }
    if (m_vinyl.isNull() || m_vinyl.width() != int(side * devicePixelRatioF()))
        bakeVinyl();
    const double degrees = std::fmod(turn / studio::secondsPerTurn * 360, 360);
    painter.save();
    painter.translate(whole.center());
    painter.rotate(degrees);
    painter.translate(-whole.center());
    painter.drawPixmap(QPointF(0, 0), m_vinyl);
    const int labelSide = int(side * 0.4);
    if (m_labelFor != deck.videoId || m_label.width() != int(labelSide * devicePixelRatioF())) {
        const QImage art = m_model->artwork(deck.videoId);
        if (!art.isNull() && art.width() > 1) {
            m_label = circular(art, int(labelSide * devicePixelRatioF()));
            m_label.setDevicePixelRatio(devicePixelRatioF());
            m_labelFor = deck.videoId;
        } else {
            m_label = QPixmap();
            m_labelFor.clear();
        }
    }
    const QRectF labelRect(whole.center().x() - labelSide / 2.0, whole.center().y() - labelSide / 2.0, labelSide, labelSide);
    if (!m_label.isNull()) {
        painter.drawPixmap(labelRect.topLeft(), m_label);
    } else {
        QRadialGradient label(labelRect.center(), labelSide / 2.0);
        label.setColorAt(0, QColor(0xe0, 0x22, 0x22));
        label.setColorAt(1, QColor(0x9a, 0x10, 0x10));
        painter.setPen(Qt::NoPen);
        painter.setBrush(label);
        painter.drawEllipse(labelRect);
        painter.setPen(Qt::white);
        painter.setFont(theme::font(std::max(8, labelSide / 6), true));
        painter.drawText(labelRect, Qt::AlignCenter, QStringLiteral("FORKMESH\nDJ"));
    }
    painter.setPen(QPen(QColor(255, 255, 255, 230), std::max(2.0, side * 0.018), Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(QPointF(whole.center().x(), whole.center().y() - labelSide / 2.0 - 2),
                     QPointF(whole.center().x(), side * 0.09));
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0xcc, 0xcc, 0xcc));
    painter.drawEllipse(whole.center(), side * 0.02, side * 0.02);
    painter.restore();
    if (deck.scratching || m_down) {
        painter.setPen(QPen(color.lighter(130), 2));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(whole.adjusted(side * 0.06, side * 0.06, -side * 0.06, -side * 0.06));
    }
    if (!deck.loaded()) {
        painter.setPen(theme::dim());
        painter.setFont(theme::font(10, true));
        painter.drawText(whole.adjusted(0, side * 0.62, 0, 0), Qt::AlignHCenter | Qt::AlignTop, QStringLiteral("DROP A SONG"));
    }
}

double DjPlatter::angleAt(const QPointF &point) const
{
    const QPointF center(width() / 2.0, height() / 2.0);
    return std::atan2(point.y() - center.y(), point.x() - center.x());
}

void DjPlatter::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    m_down = true;
    m_scratching = false;
    m_nudging = event->modifiers() & Qt::ShiftModifier;
    m_pressPoint = event->position();
    m_lastAngle = angleAt(event->position());
    setCursor(Qt::ClosedHandCursor);
    if (!m_nudging)
        m_model->touchBegan(m_deck);
}

void DjPlatter::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_down)
        return;
    const double angle = angleAt(event->position());
    double delta = angle - m_lastAngle;
    while (delta > M_PI)
        delta -= 2 * M_PI;
    while (delta < -M_PI)
        delta += 2 * M_PI;
    if (m_nudging) {
        m_lastAngle = angle;
        m_model->bend(m_deck, std::clamp(delta * 12, -1.0, 1.0));
        return;
    }
    if (!m_scratching) {
        const QPointF moved = event->position() - m_pressPoint;
        const double side = std::min(width(), height());
        if (!studio::isScratch(moved.x(), moved.y(), std::max(studio::scratchThreshold * 0.6, side * 0.05)))
            return;
        m_model->touchEnded(m_deck);
        m_model->scratchBegan(m_deck);
        m_scratching = m_model->deck(m_deck).scratching;
        m_lastAngle = angle;
        return;
    }
    m_lastAngle = angle;
    m_model->scratchMoved(m_deck, delta);
}

void DjPlatter::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || !m_down)
        return;
    m_down = false;
    setCursor(Qt::OpenHandCursor);
    if (m_nudging) {
        m_model->bend(m_deck, 0);
    } else if (m_scratching) {
        m_model->scratchEnded(m_deck);
    } else {
        m_model->touchEnded(m_deck);
    }
    m_scratching = false;
    m_nudging = false;
}

void DjPlatter::wheelEvent(QWheelEvent *event)
{
    const double direction = event->angleDelta().y() > 0 ? 1 : -1;
    m_model->bend(m_deck, direction * 0.6);
    const int token = ++m_wheelToken;
    QTimer::singleShot(160, this, [this, token]() {
        if (token == m_wheelToken)
            m_model->bend(m_deck, 0);
    });
    event->accept();
}

DjScrollWave::DjScrollWave(DjModel *model, int deck, QWidget *parent) : QWidget(parent), m_model(model), m_deck(deck)
{
    setMinimumHeight(28);
    setCursor(Qt::SizeHorCursor);
    setToolTip(QStringLiteral("Drag to scrub the song; the wheel zooms."));
}

void DjScrollWave::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    const QRect r = rect();
    painter.fillRect(r, QColor(0x0c, 0x0d, 0x0f));
    DjDeck &deck = m_model->deck(m_deck);
    const QColor color = theme::deck(m_deck);
    const double at = DjModel::now();
    const double position = deck.loaded() ? m_model->shown(m_deck, at) : 0;
    const double perPixel = m_window / std::max(1, r.width());
    const double left = position - m_window / 2;
    const double mid = r.height() / 2.0;
    const int bars = deck.bars.size();
    const double loudest = deck.loudest > 0 ? deck.loudest : 1;
    if (deck.bpm > 0 && deck.loaded()) {
        const double beat = 60.0 / deck.bpm;
        const double first = std::ceil((left - deck.cue) / beat);
        for (double n = first;; ++n) {
            const double t = deck.cue + n * beat;
            const double x = (t - left) / perPixel;
            if (x > r.width())
                break;
            const bool bar = std::fmod(std::abs(n), 4.0) < 0.5;
            painter.setPen(QColor(255, 255, 255, bar ? 46 : 18));
            painter.drawLine(QPointF(x, 0), QPointF(x, r.height()));
        }
    }
    if (deck.loop && deck.loaded()) {
        const double x0 = (deck.loopStart - left) / perPixel;
        const double x1 = (deck.loopStart + deck.loopLength - left) / perPixel;
        painter.fillRect(QRectF(x0, 0, x1 - x0, r.height()), QColor(0x3d, 0xd6, 0x5c, 50));
    }
    if (bars > 0) {
        const double barSeconds = 1.0 / DjModel::barsPerSecond;
        for (int x = 0; x < r.width(); ++x) {
            const double t = left + x * perPixel;
            if (t < 0)
                continue;
            const int index = int(t / barSeconds);
            if (index >= bars)
                break;
            if (index >= deck.heard.size() || !deck.heard.at(index))
                continue;
            const DjWaveBar &bar = deck.bars.at(index);
            const double level = std::clamp(bar.level / loudest, 0.0, 1.0);
            const double h = std::max(1.0, level * (mid - 2));
            const bool past = t < position;
            painter.setPen(waveColor(bar.low, bar.mid, bar.high, past ? 0.55 : 1.0));
            painter.drawLine(QPointF(x + 0.5, mid - h), QPointF(x + 0.5, mid + h));
        }
    } else if (deck.loaded()) {
        painter.setPen(QColor(0x2e, 0x30, 0x34));
        painter.drawLine(QPointF(0, mid), QPointF(r.width(), mid));
        painter.setPen(theme::dim());
        painter.setFont(theme::font(9));
        painter.drawText(r.adjusted(6, 0, -6, 0), Qt::AlignRight | Qt::AlignVCenter,
                         QStringLiteral("the waveform appears as the song plays"));
    }
    painter.setFont(theme::font(9, true));
    auto marker = [&](double t, const QColor &c, const QString &label) {
        const double x = (t - left) / perPixel;
        if (x < -40 || x > r.width() + 2)
            return;
        painter.setPen(QPen(c, 1.5));
        painter.drawLine(QPointF(x, 0), QPointF(x, r.height()));
        QPolygonF flag;
        flag << QPointF(x, 0) << QPointF(x + 6, 0) << QPointF(x, 6);
        painter.setBrush(c);
        painter.setPen(Qt::NoPen);
        painter.drawPolygon(flag);
        painter.setPen(c);
        painter.drawText(QPointF(x + 3, 15), label);
    };
    if (deck.loaded()) {
        marker(deck.cue, QColor(0xff, 0xd0, 0x30), QStringLiteral("CUE"));
        for (int i = 0; i < 8; ++i) {
            if (deck.hotcues[i] >= 0)
                marker(deck.hotcues[i], QColor(0xff, 0x44, 0x44), QStringLiteral("Cue %1").arg(i + 1));
        }
        if (deck.duration > 0) {
            const double x = (deck.duration - left) / perPixel;
            if (x < r.width())
                painter.fillRect(QRectF(x, 0, r.width() - x, r.height()), QColor(0x30, 0x08, 0x08));
        }
    }
    painter.setPen(QPen(Qt::white, 1.5));
    painter.drawLine(QPointF(r.width() / 2.0, 0), QPointF(r.width() / 2.0, r.height()));
    painter.fillRect(QRect(0, 0, 3, r.height()), color);
    if (m_title && deck.loaded()) {
        painter.setPen(QColor(255, 255, 255, 120));
        painter.setFont(theme::font(9, true));
        painter.drawText(QRect(8, r.height() - 13, r.width() / 2 - 16, 12), Qt::AlignLeft | Qt::AlignVCenter,
                         QStringLiteral("%1  %2").arg(m_deck + 1).arg(deck.title));
    }
}

void DjScrollWave::mousePressEvent(QMouseEvent *event)
{
    DjDeck &deck = m_model->deck(m_deck);
    if (event->button() != Qt::LeftButton || !deck.loaded())
        return;
    m_dragging = true;
    m_dragX = event->position().x();
    m_dragTime = deck.position(DjModel::now());
}

void DjScrollWave::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_dragging)
        return;
    const double perPixel = m_window / std::max(1, width());
    const double target = m_dragTime - (event->position().x() - m_dragX) * perPixel;
    DjDeck &deck = m_model->deck(m_deck);
    if (std::abs(target - deck.position(DjModel::now())) > 0.04)
        m_model->seek(m_deck, target);
}

void DjScrollWave::mouseReleaseEvent(QMouseEvent *) { m_dragging = false; }

void DjScrollWave::wheelEvent(QWheelEvent *event)
{
    m_window = std::clamp(m_window * (event->angleDelta().y() > 0 ? 0.8 : 1.25), 3.0, 60.0);
    update();
    event->accept();
}

DjOverview::DjOverview(DjModel *model, int deck, QWidget *parent) : QWidget(parent), m_model(model), m_deck(deck)
{
    setMinimumHeight(18);
    setCursor(Qt::PointingHandCursor);
    setToolTip(QStringLiteral("The whole song: click to jump there."));
}

void DjOverview::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    const QRect r = rect();
    painter.fillRect(r, QColor(0x0c, 0x0d, 0x0f));
    DjDeck &deck = m_model->deck(m_deck);
    if (!deck.loaded() || deck.duration <= 0) {
        painter.setPen(QColor(0x2e, 0x30, 0x34));
        painter.drawLine(QPointF(0, r.height() / 2.0), QPointF(r.width(), r.height() / 2.0));
        return;
    }
    const double position = deck.position(DjModel::now());
    const double played = position / deck.duration * r.width();
    const int bars = deck.bars.size();
    const double loudest = deck.loudest > 0 ? deck.loudest : 1;
    const double mid = r.height() / 2.0;
    for (int x = 0; x < r.width(); ++x) {
        const int from = int(double(x) / r.width() * bars);
        const int to = std::max(from + 1, int(double(x + 1) / r.width() * bars));
        double level = 0;
        double low = 0;
        double midBand = 0;
        double high = 0;
        int counted = 0;
        for (int i = from; i < to && i < bars; ++i) {
            if (i >= deck.heard.size() || !deck.heard.at(i))
                continue;
            const DjWaveBar &bar = deck.bars.at(i);
            level = std::max(level, bar.level / loudest);
            low += bar.low;
            midBand += bar.mid;
            high += bar.high;
            ++counted;
        }
        if (!counted)
            continue;
        const double h = std::max(1.0, std::clamp(level, 0.0, 1.0) * (mid - 1));
        painter.setPen(waveColor(low, midBand, high, x < played ? 0.45 : 0.95));
        painter.drawLine(QPointF(x + 0.5, mid - h), QPointF(x + 0.5, mid + h));
    }
    painter.fillRect(QRectF(0, 0, played, r.height()), QColor(255, 255, 255, 18));
    for (int i = 0; i < 8; ++i) {
        if (deck.hotcues[i] < 0)
            continue;
        const double x = deck.hotcues[i] / deck.duration * r.width();
        painter.setPen(QColor(0xff, 0x44, 0x44));
        painter.drawLine(QPointF(x, 0), QPointF(x, r.height()));
    }
    const double cue = deck.cue / deck.duration * r.width();
    painter.setPen(QColor(0xff, 0xd0, 0x30));
    painter.drawLine(QPointF(cue, 0), QPointF(cue, r.height()));
    painter.setPen(QPen(Qt::white, 1.5));
    painter.drawLine(QPointF(played, 0), QPointF(played, r.height()));
}

void DjOverview::seekTo(double x)
{
    DjDeck &deck = m_model->deck(m_deck);
    if (deck.duration <= 0)
        return;
    m_model->seek(m_deck, std::clamp(x / std::max(1, width()), 0.0, 1.0) * deck.duration);
}

void DjOverview::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton)
        seekTo(event->position().x());
}

void DjOverview::mouseMoveEvent(QMouseEvent *event)
{
    if (event->buttons() & Qt::LeftButton)
        seekTo(event->position().x());
}

}
