#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "AppPaths.h"
#include "AppViewMode.h"

#include <QApplication>
#include <QCursor>
#include <QDir>
#include <QElapsedTimer>
#include <QEnterEvent>
#include <QInputDialog>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QTextLayout>
#include <QtMath>

#include <algorithm>
#include <cmath>

using namespace forkmesh::ui;
using forkmesh::teammates::AgentChoice;
using forkmesh::teammates::Config;
using forkmesh::teammates::LoadResult;
using forkmesh::teammates::Store;

namespace {

constexpr int kTeammateRailWidth = 60;
constexpr int kTeammateRailFace = 44;
constexpr int kTeammateRailItemWidth = 54;
constexpr int kTeammateRailInset = 2;
constexpr int kTeammateRailSquare = kTeammateRailItemWidth - 2 * kTeammateRailInset;
constexpr int kTeammateRailFaceRadius = 7;
constexpr int kTeammateRailHud = 14;
constexpr int kTeammateRailMood = 16;
constexpr int kTeammateRailHudInset = 3;
constexpr int kTeammateRailMinWindowWidth = 1000;
constexpr int kTeammateTileMinWidth = 240;
constexpr int kTeammateTileFace = 240;
constexpr int kTeammateTileHud = 22;
constexpr int kTeammateTileMood = 24;
constexpr int kTeammateTileInset = 7;
constexpr int kTeammateTileRadius = 12;
constexpr int kTeammateTileCaptionLines = 5;
constexpr int kTeammateTileFrameMs = 66;

bool railStatusInFlight(const QString &status)
{
    return status == QLatin1String("Working") ||
           status == QLatin1String("Queued") ||
           status == QLatin1String("Waiting");
}

QColor railStatusColor(const QString &status, bool dark)
{
    if (status == QLatin1String("Working") || status == QLatin1String("Done"))
        return Theme::nextOr(Theme::Role::AccentFg,
                             dark ? QColor("#3fb950") : QColor("#1a7f37"));
    if (status == QLatin1String("Queued") || status == QLatin1String("Waiting"))
        return dark ? QColor("#d29922") : QColor("#9a6700");
    if (status == QLatin1String("Scheduled"))
        return Theme::nextOr(Theme::Role::AccentFg,
                             dark ? QColor("#58a6ff") : QColor("#0969da"));
    if (status == QLatin1String("Failed"))
        return dark ? QColor("#f85149") : QColor("#cf222e");
    return dark ? Theme::remapColor(QColor("#8b949e")) : Theme::remapColor(QColor("#6e7781"));
}

QColor railFreshnessColor(double staleness, bool dark)
{
    const double hue = (1.0 - qBound(0.0, staleness, 1.0)) * 130.0 / 360.0;
    return QColor::fromHsvF(hue, dark ? 0.72 : 0.78, dark ? 0.80 : 0.68);
}

enum class RailRing { Idle, Solid, Spinner, Paused };

constexpr qreal kRailPi = 3.14159265358979323846;

struct FacePose {
    qreal dx = 0.0;
    qreal dy = 0.0;
    qreal angle = 0.0;
    qreal scale = 1.0;
    qreal dim = 0.0;
};

qreal burst(qreal t, qreal period, qreal length)
{
    const qreal phase = std::fmod(t, period);
    if (phase >= length)
        return 0.0;
    return std::sin(kRailPi * phase / length);
}

FacePose facePoseAt(forkmesh::teammates::MoodMotion motion, qreal intensity, qreal t)
{
    using forkmesh::teammates::MoodMotion;
    const qreal amp = 0.55 + 0.45 * qBound<qreal>(0.0, intensity, 1.0);
    const qreal tau = 2.0 * kRailPi;
    FacePose pose;
    pose.scale = 1.0 + 0.016 * std::sin(tau * t / 3.4);
    pose.dx = 0.55 * std::sin(t * 1.3) + 0.35 * std::sin(t * 2.9 + 0.7);
    pose.dy = 0.45 * std::sin(t * 1.7 + 1.1) + 0.25 * std::sin(t * 3.3);
    pose.angle = 0.6 * std::sin(t * 0.9);
    switch (motion) {
    case MoodMotion::Still:
        break;
    case MoodMotion::Wave: {
        const qreal envelope = burst(t, 3.0, 1.6);
        pose.angle += 10.0 * amp * envelope * std::sin(tau * t * 2.1);
        pose.dx += 1.6 * amp * envelope * std::sin(tau * t * 2.1);
        pose.scale += 0.07 * envelope;
        pose.dy -= 1.2 * envelope;
        break;
    }
    case MoodMotion::Jump: {
        const qreal hop = burst(t, 1.5, 0.7);
        pose.dy -= 5.0 * amp * hop;
        pose.scale += 0.03 * hop;
        pose.angle += 3.0 * amp * std::sin(tau * t * 0.66);
        break;
    }
    case MoodMotion::Bounce:
        pose.dy -= 2.6 * amp * std::abs(std::sin(kRailPi * t * 1.25));
        break;
    case MoodMotion::Nod:
        pose.dy += 1.8 * amp * qMax<qreal>(0.0, std::sin(tau * t * 0.8));
        pose.scale -= 0.012 * qMax<qreal>(0.0, std::sin(tau * t * 0.8));
        break;
    case MoodMotion::Pulse:
        pose.scale += 0.045 * amp * (0.5 + 0.5 * std::sin(tau * t * 1.1));
        break;
    case MoodMotion::Float:
        pose.dy += 1.8 * std::sin(tau * t * 0.33);
        pose.angle += 2.2 * std::sin(tau * t * 0.21);
        break;
    case MoodMotion::Wiggle:
        pose.angle += 6.0 * amp * burst(t, 2.6, 1.2) * std::sin(tau * t * 1.6);
        break;
    case MoodMotion::Shake:
        pose.dx += 2.4 * amp * burst(t, 2.4, 0.7) * std::sin(tau * t * 7.0);
        break;
    case MoodMotion::Spin:
        pose.angle += 12.0 * amp * std::sin(tau * t * 0.7);
        pose.dx += 1.4 * std::cos(tau * t * 0.7);
        pose.dy += 1.4 * std::sin(tau * t * 0.7);
        break;
    case MoodMotion::Doze: {
        const qreal droop = 0.5 + 0.5 * std::sin(tau * t / 4.5);
        pose.angle += 7.0 * droop;
        pose.dy += 2.2 * droop;
        pose.scale = 1.0 + 0.012 * std::sin(tau * t / 5.0);
        pose.dim = 0.12 + 0.08 * droop;
        break;
    }
    }
    return pose;
}

QElapsedTimer &railFaceClock()
{
    static QElapsedTimer clock;
    if (!clock.isValid())
        clock.start();
    return clock;
}

class RailFaceButton : public QPushButton
{
public:
    RailFaceButton(QPixmap face, QColor color, RailRing ring, bool dark,
                   forkmesh::teammates::MoodMotion motion, qreal intensity,
                   qreal phase, int side)
        : m_face(std::move(face)), m_color(std::move(color)), m_ring(ring),
          m_dark(dark), m_motion(motion), m_intensity(intensity), m_phase(phase)
    {
        setFixedSize(side, side);
        setAttribute(Qt::WA_Hover);
        m_timer = new QTimer(this);
        m_timer->setInterval(40);
        connect(m_timer, &QTimer::timeout, this, [this] { update(); });
    }

protected:
    virtual QRectF faceArea(const QRectF &frame) const { return frame; }
    virtual void paintOverlay(QPainter &, const QRectF &) {}
    void setFrameInterval(int ms) { m_timer->setInterval(ms); }
    void setCornerRadius(qreal radius) { m_radius = radius; }
    void useBackdrop()
    {
        m_backdrop = m_face.scaled(10, 10, Qt::IgnoreAspectRatio,
                                   Qt::SmoothTransformation);
        QImage feathered =
            m_face.toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied);
        QPainter mask(&feathered);
        mask.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        QLinearGradient across(0, 0, feathered.width(), 0);
        across.setColorAt(0.0, QColor(0, 0, 0, 0));
        across.setColorAt(0.22, QColor(0, 0, 0, 255));
        across.setColorAt(0.78, QColor(0, 0, 0, 255));
        across.setColorAt(1.0, QColor(0, 0, 0, 0));
        mask.fillRect(feathered.rect(), across);
        QLinearGradient down(0, 0, 0, feathered.height());
        down.setColorAt(0.0, QColor(0, 0, 0, 255));
        down.setColorAt(0.72, QColor(0, 0, 0, 255));
        down.setColorAt(1.0, QColor(0, 0, 0, 0));
        mask.fillRect(feathered.rect(), down);
        mask.end();
        const qreal ratio = m_face.devicePixelRatio();
        m_face = QPixmap::fromImage(feathered);
        m_face.setDevicePixelRatio(ratio);
    }

    void showEvent(QShowEvent *event) override
    {
        m_timer->start();
        QPushButton::showEvent(event);
    }
    void hideEvent(QHideEvent *event) override
    {
        m_timer->stop();
        QPushButton::hideEvent(event);
    }
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        const QRectF frame = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        QPainterPath clip;
        clip.addRoundedRect(frame, m_radius, m_radius);
        painter.setClipPath(clip);
        painter.fillRect(frame, m_dark ? QColor("#0d1117") : QColor("#eaeef2"));
        if (!m_backdrop.isNull()) {
            const qreal cover = qMax(frame.width(), frame.height()) * 1.15;
            painter.drawPixmap(QRectF(frame.center() - QPointF(cover / 2, cover / 2),
                                      QSizeF(cover, cover)),
                               m_backdrop, QRectF(m_backdrop.rect()));
            painter.fillRect(frame, m_dark ? QColor(13, 17, 23, 120)
                                           : QColor(255, 255, 255, 50));
        }

        const qreal t = railFaceClock().elapsed() / 1000.0 + m_phase;
        const FacePose pose = facePoseAt(
            m_ring == RailRing::Paused ? forkmesh::teammates::MoodMotion::Doze : m_motion,
            m_intensity, t);
        const QRectF area = faceArea(frame);
        const qreal unit = qMin(area.width(), area.height());
        const qreal stretch = unit / kTeammateRailSquare;
        const qreal side = unit * 1.1;
        painter.save();
        painter.translate(area.center() + QPointF(pose.dx, pose.dy) * stretch);
        painter.rotate(pose.angle);
        painter.scale(pose.scale, pose.scale);
        painter.drawPixmap(QRectF(-side / 2, -side / 2, side, side), m_face,
                           QRectF(m_face.rect()));
        painter.restore();

        QRadialGradient vignette(frame.center(),
                                 qMax(frame.width(), frame.height()) * 0.72);
        vignette.setColorAt(0.55, QColor(0, 0, 0, 0));
        vignette.setColorAt(1.0, QColor(0, 0, 0, m_dark ? 120 : 90));
        painter.fillRect(frame, vignette);
        const qreal sweep = std::fmod(t * 9.0, frame.height() + 12.0) - 6.0;
        painter.fillRect(QRectF(frame.left(), frame.top() + sweep, frame.width(), 1.2),
                         QColor(255, 255, 255, 22));
        const qreal dim = m_ring == RailRing::Paused ? 0.45 : pose.dim;
        if (dim > 0.0) {
            QColor shade = m_dark ? QColor("#0d1117") : QColor("#ffffff");
            shade.setAlphaF(dim);
            painter.fillRect(frame, shade);
        }
        if (underMouse())
            painter.fillRect(frame, QColor(255, 255, 255, m_dark ? 18 : 30));
        paintOverlay(painter, frame);
        painter.setClipping(false);

        const QRectF border = frame.adjusted(0.7, 0.7, -0.7, -0.7);
        painter.setBrush(Qt::NoBrush);
        switch (m_ring) {
        case RailRing::Idle:
            break;
        case RailRing::Solid:
            painter.setPen(QPen(m_color, 2.0));
            painter.drawRoundedRect(border, m_radius, m_radius);
            break;
        case RailRing::Paused: {
            QPen pen(m_color, 1.4, Qt::DashLine);
            pen.setDashPattern({2.0, 2.0});
            painter.setPen(pen);
            painter.drawRoundedRect(border, m_radius, m_radius);
            break;
        }
        case RailRing::Spinner: {
            QColor track = m_color;
            track.setAlphaF(0.3);
            painter.setPen(QPen(track, 2.0));
            painter.drawRoundedRect(border, m_radius, m_radius);
            QPen march(m_color, 2.4, Qt::CustomDashLine, Qt::RoundCap);
            march.setDashPattern({6.0, 4.0});
            march.setDashOffset(-std::fmod(t * 12.0, 10.0));
            painter.setPen(march);
            painter.drawRoundedRect(border, m_radius, m_radius);
            break;
        }
        }
    }

private:
    QPixmap m_face;
    QPixmap m_backdrop;
    QColor m_color;
    RailRing m_ring;
    bool m_dark;
    forkmesh::teammates::MoodMotion m_motion;
    qreal m_intensity;
    qreal m_phase;
    qreal m_radius = kTeammateRailFaceRadius;
    QTimer *m_timer = nullptr;
};

QStringList wrappedCaptionLines(const QString &text, const QFont &font,
                                qreal width, int maxLines)
{
    QStringList lines;
    if (text.trimmed().isEmpty() || width <= 0 || maxLines <= 0)
        return lines;
    QTextLayout layout(text, font);
    layout.beginLayout();
    for (QTextLine line = layout.createLine(); line.isValid();
         line = layout.createLine()) {
        line.setLineWidth(width);
        if (lines.size() == maxLines - 1) {
            const QString rest = text.mid(line.textStart()).simplified();
            lines.append(QFontMetricsF(font).elidedText(rest, Qt::ElideRight, width));
            break;
        }
        lines.append(text.mid(line.textStart(), line.textLength()).trimmed());
    }
    layout.endLayout();
    return lines;
}

class RailVideoTile : public RailFaceButton
{
public:
    RailVideoTile(QPixmap face, QColor color, RailRing ring, bool dark,
                  forkmesh::teammates::MoodMotion motion, qreal intensity,
                  qreal phase, QString name, QString meta, QString caption)
        : RailFaceButton(std::move(face), std::move(color), ring, dark, motion,
                         intensity, phase, kTeammateTileMinWidth),
          m_name(std::move(name)), m_meta(std::move(meta)),
          m_caption(std::move(caption))
    {
        setCornerRadius(kTeammateTileRadius);
        setFrameInterval(kTeammateTileFrameMs);
        useBackdrop();
        setMinimumSize(kTeammateTileMinWidth, heightForWidth(kTeammateTileMinWidth));
        setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        QSizePolicy policy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        policy.setHeightForWidth(true);
        setSizePolicy(policy);
    }

    void addHud(QPushButton *button, Qt::Alignment corner, int row, bool hoverOnly)
    {
        button->setParent(this);
        button->setCursor(Qt::PointingHandCursor);
        m_hud.append({button, corner, row, hoverOnly});
        if (hoverOnly)
            button->hide();
        placeHud();
    }

    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override
    {
        return qRound(width * 0.5) + captionBandHeight();
    }
    QSize sizeHint() const override
    {
        return {kTeammateTileMinWidth + 40, heightForWidth(kTeammateTileMinWidth + 40)};
    }
    QSize minimumSizeHint() const override
    {
        return {kTeammateTileMinWidth, heightForWidth(kTeammateTileMinWidth)};
    }

protected:
    QRectF faceArea(const QRectF &frame) const override
    {
        return QRectF(frame.left(), frame.top(), frame.width(),
                      qMax<qreal>(1.0, frame.height() - captionBandHeight()));
    }

    void paintOverlay(QPainter &painter, const QRectF &frame) override
    {
        const QFont name = tileFont(12, QFont::DemiBold);
        const QFont meta = tileFont(10, QFont::Normal);
        const QFont caption = tileFont(11, QFont::Normal);
        const qreal pad = 8.0;
        const qreal width = frame.width() - 2 * pad;
        const QStringList lines =
            wrappedCaptionLines(m_caption, caption, width, kTeammateTileCaptionLines);
        const QFontMetricsF nameMetrics(name);
        const qreal lineHeight = QFontMetricsF(caption).lineSpacing();
        const qreal bandHeight = pad + nameMetrics.height() +
                                 (lines.isEmpty() ? 0.0 : 3.0 + lines.size() * lineHeight) +
                                 pad;
        const QRectF band(frame.left(), frame.bottom() - bandHeight, frame.width(),
                          bandHeight);
        QLinearGradient fade(band.topLeft() - QPointF(0, 22), band.topLeft());
        fade.setColorAt(0.0, QColor(0, 0, 0, 0));
        fade.setColorAt(1.0, QColor(0, 0, 0, 170));
        painter.fillRect(QRectF(band.left(), band.top() - 22, band.width(), 22), fade);
        painter.fillRect(band, QColor(0, 0, 0, 170));

        qreal y = band.top() + pad;
        const QString metaText =
            m_meta.isEmpty() ? QString() : QStringLiteral(" · %1").arg(m_meta);
        const qreal metaWidth = QFontMetricsF(meta).horizontalAdvance(metaText);
        const QString nameText = nameMetrics.elidedText(
            m_name, Qt::ElideRight, qMax<qreal>(24.0, width - metaWidth));
        painter.setFont(name);
        painter.setPen(QColor("#ffffff"));
        const QRectF nameRect(band.left() + pad, y, width, nameMetrics.height());
        painter.drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter, nameText);
        if (!metaText.isEmpty()) {
            painter.setFont(meta);
            painter.setPen(QColor(230, 237, 243, 190));
            painter.drawText(
                nameRect.adjusted(nameMetrics.horizontalAdvance(nameText), 0, 0, 0),
                Qt::AlignLeft | Qt::AlignVCenter,
                QFontMetricsF(meta).elidedText(
                    metaText, Qt::ElideRight,
                    width - nameMetrics.horizontalAdvance(nameText)));
        }
        y += nameMetrics.height() + 3.0;
        painter.setFont(caption);
        painter.setPen(QColor("#f0f6fc"));
        for (const QString &line : lines) {
            painter.drawText(QRectF(band.left() + pad, y, width, lineHeight),
                             Qt::AlignLeft | Qt::AlignVCenter, line);
            y += lineHeight;
        }
    }

    void resizeEvent(QResizeEvent *event) override
    {
        RailFaceButton::resizeEvent(event);
        placeHud();
    }
    void enterEvent(QEnterEvent *event) override
    {
        for (const Hud &hud : std::as_const(m_hud)) {
            if (hud.hoverOnly && hud.button) {
                hud.button->show();
                hud.button->raise();
            }
        }
        RailFaceButton::enterEvent(event);
    }
    void leaveEvent(QEvent *event) override
    {
        for (const Hud &hud : std::as_const(m_hud)) {
            if (hud.hoverOnly && hud.button)
                hud.button->hide();
        }
        RailFaceButton::leaveEvent(event);
    }

private:
    struct Hud {
        QPointer<QPushButton> button;
        Qt::Alignment corner;
        int row = 0;
        bool hoverOnly = false;
    };

    QFont tileFont(int pixelSize, QFont::Weight weight) const
    {
        QFont result = font();
        result.setPixelSize(pixelSize);
        result.setWeight(weight);
        return result;
    }

    int captionBandHeight() const
    {
        return qCeil(8.0 + QFontMetricsF(tileFont(12, QFont::DemiBold)).height() + 3.0 +
                     kTeammateTileCaptionLines *
                         QFontMetricsF(tileFont(11, QFont::Normal)).lineSpacing() +
                     8.0);
    }

    void placeHud()
    {
        for (const Hud &hud : std::as_const(m_hud)) {
            if (!hud.button)
                continue;
            const QSize size = hud.button->size();
            int x = kTeammateTileInset;
            if (hud.corner & Qt::AlignHCenter)
                x = (width() - size.width()) / 2;
            else if (hud.corner & Qt::AlignRight)
                x = width() - kTeammateTileInset - size.width();
            const int y = kTeammateTileInset + (kTeammateTileMood - size.height()) / 2 +
                          hud.row * (kTeammateTileMood + 4);
            hud.button->move(x, y);
            hud.button->raise();
        }
    }

    QString m_name;
    QString m_meta;
    QString m_caption;
    QList<Hud> m_hud;
};

class RailMoodBadge : public QPushButton
{
public:
    RailMoodBadge(const QString &emoji, bool attention, bool dark, int side)
        : m_emoji(emoji), m_attention(attention), m_dark(dark)
    {
        setFixedSize(side, side);
        setAttribute(Qt::WA_Hover);
        setFlat(true);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::TextAntialiasing);
        const QRectF bounds = QRectF(rect()).adjusted(0.6, 0.6, -0.6, -0.6);
        QColor fill = m_dark ? QColor(13, 17, 23, 190) : QColor(255, 255, 255, 205);
        if (underMouse())
            fill.setAlpha(240);
        QColor edge = m_attention ? (m_dark ? QColor("#d29922") : QColor("#bf8700"))
                                  : (m_dark ? QColor(139, 148, 158, 140)
                                            : QColor(140, 149, 159, 150));
        painter.setPen(QPen(edge, m_attention ? 1.4 : 1.0));
        painter.setBrush(fill);
        painter.drawEllipse(bounds);
        QFont font = painter.font();
        font.setFamilies({QStringLiteral("Noto Color Emoji"),
                          QStringLiteral("Apple Color Emoji"),
                          QStringLiteral("Segoe UI Emoji"), font.family()});
        font.setPixelSize(qMax(8, int(bounds.height() * 0.68)));
        painter.setFont(font);
        painter.setPen(m_dark ? QColor("#e6edf3") : QColor("#1f2328"));
        painter.drawText(bounds.adjusted(0, 0.5, 0, 0), Qt::AlignCenter, m_emoji);
    }

private:
    QString m_emoji;
    bool m_attention;
    bool m_dark;
};

class RailItem : public QWidget
{
public:
    void setTint(QColor tint, bool dark)
    {
        m_tint = std::move(tint);
        m_dark = dark;
        update();
    }
    void addHoverButton(QPushButton *button)
    {
        m_hoverButtons.append(button);
        button->hide();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        if (!m_tint.isValid())
            return;
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        QColor fill = m_tint;
        fill.setAlphaF(m_dark ? 0.32 : 0.26);
        QColor edge = m_tint;
        edge.setAlphaF(m_dark ? 0.70 : 0.60);
        painter.setPen(QPen(edge, 1.0));
        painter.setBrush(fill);
        painter.drawRoundedRect(box, kTeammateRailFaceRadius + 2, kTeammateRailFaceRadius + 2);
    }
    void enterEvent(QEnterEvent *event) override
    {
        for (QPushButton *button : std::as_const(m_hoverButtons)) {
            if (button) {
                button->show();
                button->raise();
            }
        }
        QWidget::enterEvent(event);
    }
    void leaveEvent(QEvent *event) override
    {
        for (QPushButton *button : std::as_const(m_hoverButtons)) {
            if (button)
                button->hide();
        }
        QWidget::leaveEvent(event);
    }

private:
    QList<QPointer<QPushButton>> m_hoverButtons;
    QColor m_tint;
    bool m_dark = false;
};

void saveTeammateDoneUnseen(const QSet<int> &sessions)
{
    QVariantList ids;
    for (int id : sessions)
        ids.append(id);
    QSettings().setValue(kTeammateDoneUnseenSetting, ids);
}

QString compactAgo(qint64 timestampMs, qint64 nowMs)
{
    if (timestampMs <= 0)
        return QStringLiteral("Never run");
    const qint64 seconds = qMax<qint64>(0, (nowMs - timestampMs) / 1000);
    if (seconds < 60)
        return QStringLiteral("Just now");
    if (seconds < 3600)
        return QStringLiteral("%1m ago").arg(seconds / 60);
    if (seconds < 86400)
        return QStringLiteral("%1h ago").arg(seconds / 3600);
    if (seconds < 86400 * 30)
        return QStringLiteral("%1d ago").arg(seconds / 86400);
    if (seconds < 86400 * 365)
        return QStringLiteral("%1mo ago").arg(seconds / (86400 * 30));
    return QStringLiteral("%1y ago").arg(seconds / (86400 * 365));
}

struct RailTeamContext {
    int size = 0;
    int working = 0;
    int failing = 0;
    bool creatorRunning = false;
    QDateTime now;
};

forkmesh::teammates::MoodSignals railMoodSignals(
    const Config &config, bool inFlight, bool doneUnseen,
    const AgentSession *session, const QList<const AgentSession *> &history,
    const RailTeamContext &team)
{
    forkmesh::teammates::MoodSignals s;
    const qint64 nowMs = team.now.toMSecsSinceEpoch();
    s.enabled = config.enabled;
    s.scheduleActive = config.schedule.active;
    s.scheduleMode = config.schedule.mode;
    s.windowState = forkmesh::teammates::runWindowState(config, team.now);
    s.staleness = inFlight ? 0.0 : forkmesh::teammates::runStaleness(config, team.now);
    if (config.enabled && config.schedule.active) {
        const QDateTime next =
            forkmesh::teammates::nextScheduledInvocation(config, team.now);
        if (next.isValid())
            s.nextRunInSecs = qMax<qint64>(0, team.now.secsTo(next));
    }
    s.skills = config.skills.size() + config.skillRefs.size();
    s.permissions = config.permissions.size();
    s.canReadRepositories =
        config.permissions.contains(QStringLiteral("read-repositories"));
    s.logMonitors = config.logMonitors.size();
    s.hasWorkflow = !config.workflow.isEmpty();
    s.hasDescription = !config.description.trimmed().isEmpty();
    s.hasInstructions = !config.instructions.trimmed().isEmpty();
    s.hasRelayChannel = !config.relayChannel.trimmed().isEmpty();
    s.improvementCycles = config.improvementCycles;
    s.improvementNotes = config.improvementNotes.size();
    s.activityCount = config.activity.size();
    s.createdAtMs = config.createdAtMs;
    s.updatedAtMs = config.updatedAtMs;
    s.lastRunAtMs = config.lastRunAtMs;
    s.lastTrigger = config.lastTrigger;

    s.inFlight = inFlight;
    s.doneUnseen = doneUnseen;
    if (session) {
        s.sessionStatus = session->status;
        s.runningMs = session->runningMs +
                      (session->runningSinceMs > 0
                           ? qMax<qint64>(0, nowMs - session->runningSinceMs)
                           : 0);
        s.lastDurationMs = session->durationMs;
        s.numTurns = session->numTurns;
        s.costUsd = session->costUsd;
        s.backgroundProcesses = session->backgroundProcesses;
        s.backgroundBusy = session->backgroundProcessesBusy;
        s.held = session->held;
        s.cloud = session->cloud;
        s.yolo = session->yolo;
        s.pendingSteer = !session->pendingSteerMessage.trimmed().isEmpty();
        s.prNumber = session->prNumber;
        s.taskGrade = session->taskGrade;
        s.lastError = session->lastError;
    }

    bool successRun = true;
    bool failureRun = true;
    qint64 durationTotal = 0;
    int durations = 0;
    double gradeTotal = 0.0;
    const QDate today = team.now.date();
    for (const AgentSession *run : history) {
        const qint64 at = qMax(run->createdAtMs, run->startedAtMs);
        if (at > 0 && QDateTime::fromMSecsSinceEpoch(at).date() == today)
            ++s.runsToday;
        if (at > 0 && nowMs - at < 7LL * 24 * 3600 * 1000)
            ++s.runsThisWeek;
        if (!AgentStatus::isTerminal(run->status))
            continue;
        ++s.recentRuns;
        s.recentCostUsd += run->costUsd;
        if (run->durationMs > 0) {
            durationTotal += run->durationMs;
            ++durations;
        }
        if (run->taskGrade > 0) {
            ++s.gradedRuns;
            gradeTotal += run->taskGrade;
        }
        const bool success = run->status == AgentStatus::Success;
        const bool failure = run->status == AgentStatus::Failed;
        s.recentSuccesses += success ? 1 : 0;
        s.recentFailures += failure ? 1 : 0;
        s.recentStopped += run->status == AgentStatus::Stopped ? 1 : 0;
        successRun = successRun && success;
        failureRun = failureRun && failure;
        s.successStreak += successRun ? 1 : 0;
        s.failureStreak += failureRun ? 1 : 0;
    }
    s.averageGrade = s.gradedRuns > 0 ? gradeTotal / s.gradedRuns : 0.0;
    s.averageDurationMs = durations > 0 ? durationTotal / durations : 0;

    s.nowMs = nowMs;
    s.hourOfDay = team.now.time().hour();
    s.weekday = team.now.date().dayOfWeek();
    s.teamSize = team.size;
    s.teamWorking = team.working;
    s.teamFailing = team.failing;
    s.creatorRunning = team.creatorRunning;
    return s;
}

struct RailFaceLook {
    RailRing ring = RailRing::Idle;
    QColor ringColor;
    forkmesh::teammates::RunWindowState windowState =
        forkmesh::teammates::RunWindowState::Off;
    qreal phase = 0.0;
    QString accessibleName;
    QString tooltip;
};

RailFaceLook railFaceLook(const Config &config, const QString &status, bool inFlight,
                          bool doneUnseen, int sessionId,
                          const forkmesh::teammates::Mood &mood,
                          const QString &modelLabel, bool dark)
{
    RailFaceLook look;
    const QString lastRun =
        config.lastRunAtMs > 0
            ? QStringLiteral("%1 · %2")
                  .arg(forkmesh::teammates::humanLastActive(config.lastRunAtMs),
                       QLocale().toString(
                           QDateTime::fromMSecsSinceEpoch(config.lastRunAtMs),
                           QLocale::ShortFormat))
            : QStringLiteral("Never");
    QString action = QStringLiteral("Click to open %1's page").arg(config.name);
    if (inFlight)
        action += sessionId > 0
                      ? QStringLiteral(" · ▶ corner opens session #%1").arg(sessionId)
                      : QStringLiteral(" · working");
    else if (doneUnseen)
        action = QStringLiteral("Done · click to see the result on %1's page")
                     .arg(config.name);
    else if (!config.enabled)
        action += QStringLiteral(" · paused, make available from the ⋯ menu");
    else
        action += QStringLiteral(" · ▶ runs it on %1").arg(modelLabel);

    look.ring = !config.enabled ? RailRing::Paused
                : inFlight      ? RailRing::Spinner
                : (doneUnseen ||
                   status == QLatin1String("Scheduled") ||
                   status == QLatin1String("Failed"))
                    ? RailRing::Solid
                    : RailRing::Idle;
    const QString ringStatus = !config.enabled ? QString()
                               : doneUnseen    ? QStringLiteral("Done")
                                               : status;
    look.ringColor = railStatusColor(ringStatus, dark);
    look.windowState = forkmesh::teammates::runWindowState(config);
    if (inFlight && look.windowState != forkmesh::teammates::RunWindowState::Off)
        look.windowState = forkmesh::teammates::RunWindowState::Fresh;
    look.phase = (qHash(config.id) % 1000) / 1000.0 * 7.0;
    look.accessibleName =
        QStringLiteral("%1, %2, feeling %3")
            .arg(config.name,
                 doneUnseen ? QStringLiteral("done, not seen yet") : status.toLower(),
                 mood.label.toLower());
    look.tooltip =
        QStringLiteral("<b>%1</b> · %2<br>%3 · feeling %4<br>Model: %5<br>Last run: %6"
                       "<br>Schedule: %7%8<br><i>%9</i>")
            .arg(config.name.toHtmlEscaped(),
                 config.specialty.toHtmlEscaped(), status.toHtmlEscaped(),
                 mood.label.toLower().toHtmlEscaped(),
                 modelLabel.toHtmlEscaped(), lastRun.toHtmlEscaped(),
                 (config.schedule.active
                      ? forkmesh::teammates::humanSchedule(config.schedule)
                      : QStringLiteral("On demand"))
                     .toHtmlEscaped(),
                 config.runWindowHours > 0
                     ? QStringLiteral("<br>Reminder: %1")
                           .arg(forkmesh::teammates::humanRunWindow(config)
                                    .toHtmlEscaped())
                     : QString(),
                 action.toHtmlEscaped());
    return look;
}

} // namespace

AgentChoice MainWindow::recentlyUsedAgentChoice() const
{
    const AgentSession *latest = nullptr;
    qint64 latestAt = 0;
    for (const AgentSession &session : m_agentSessions) {
        if (session.id <= 0 || session.hasTeammateAttribution() ||
            session.provider.trimmed().isEmpty())
            continue;
        const qint64 at = qMax(session.createdAtMs, session.startedAtMs);
        if (!latest || at > latestAt) {
            latest = &session;
            latestAt = at;
        }
    }
    if (!latest)
        return {};
    return {latest->provider.trimmed(), latest->model.trimmed()};
}

AgentChoice MainWindow::promptAreaAgentChoice() const
{
    const QString provider =
        m_quickAddAgentProvider
            ? m_quickAddAgentProvider->currentData().toString().trimmed()
            : QString();
    if (provider.isEmpty() || quickAddProviderIsPromptAction(provider) ||
        agentIsCloudflareAiProvider(provider))
        return recentlyUsedAgentChoice();
    QString model = agentSelectorTakesModel(provider)
                        ? selectedModelComboValue(m_quickAddClaudeModel).trimmed()
                        : QString();
    if (agentIsCodexProvider(provider) && !model.isEmpty())
        model = codexChatGptModelId(model);
    return {provider, model};
}

bool MainWindow::teammateRailEnabled() const
{
    return QSettings().value(kShowTeammateRailSetting, true).toBool();
}

void MainWindow::setTeammateRailEnabled(bool enabled)
{
    QSettings().setValue(kShowTeammateRailSetting, enabled);
    if (m_teammateRailCheck && m_teammateRailCheck->isChecked() != enabled) {
        const QSignalBlocker blocker(m_teammateRailCheck);
        m_teammateRailCheck->setChecked(enabled);
    }
    applyTeammateRailVisibility();
}

void MainWindow::applyTeammateRailVisibility()
{
    if (!m_teammateRail)
        return;
    const bool visible = teammateRailEnabled() &&
                         packageSurfaceShown(QStringLiteral("special-agents")) &&
                         !appViewModeHidesShellChrome(m_viewMode) &&
                         width() >= kTeammateRailMinWindowWidth;
    if (visible == !m_teammateRail->isHidden())
        return;
    m_teammateRail->setVisible(visible);
    if (visible)
        scheduleTeammateRailRefresh();
}

QWidget *MainWindow::buildTeammateRail()
{
    auto *content = new QWidget;
    content->setObjectName(QStringLiteral("teammateRailContent"));
    content->setContextMenuPolicy(Qt::CustomContextMenu);
    m_teammateRailLayout = new QVBoxLayout(content);
    m_teammateRailLayout->setContentsMargins(0, kRepoTabRowTopInset + 4, 0, 8);
    m_teammateRailLayout->setSpacing(8);

    auto *scroll = new QScrollArea;
    scroll->setObjectName(QStringLiteral("teammateRail"));
    scroll->setAccessibleName(QStringLiteral("Special Agents"));
    scroll->setWidget(content);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setFixedWidth(kTeammateRailWidth);
    scroll->setMinimumHeight(0);
    scroll->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Ignored);
    m_teammateRail = scroll;

    connect(content, &QWidget::customContextMenuRequested, this,
            [this, content](const QPoint &pos) {
                QMenu menu(this);
                menu.addAction(QStringLiteral("Open Special Agents"), this, [this] {
                    showSection(kTeamSectionIndex);
                });
                menu.addAction(QStringLiteral("Run all in one session"), this,
                               [this] {
                                   QTimer::singleShot(0, this, [this] {
                                       runTeammatesInOneSessionNow();
                                   });
                               });
                menu.addAction(QStringLiteral("Create needed special agent"), this,
                               [this] { createNeededTeammate(); });
                menu.addSeparator();
                QAction *hide =
                    menu.addAction(QStringLiteral("Hide special agents rail"));
                hide->setObjectName(QStringLiteral("teammateRailHideAction"));
                connect(hide, &QAction::triggered, this, [this] {
                    setTeammateRailEnabled(false);
                    flashMessage(QStringLiteral(
                        "Special agents rail hidden. Turn it back on in Settings > "
                        "Appearance."));
                });
                menu.exec(content->mapToGlobal(pos));
            });

    m_teammateRailRefreshTimer = new QTimer(this);
    m_teammateRailRefreshTimer->setSingleShot(true);
    m_teammateRailRefreshTimer->setInterval(250);
    connect(m_teammateRailRefreshTimer, &QTimer::timeout, this,
            [this] { refreshTeammateRail(); });
    m_teammateRailClock = new QTimer(this);
    m_teammateRailClock->setInterval(30 * 1000);
    connect(m_teammateRailClock, &QTimer::timeout, this,
            [this] { scheduleTeammateRailRefresh(); });
    m_teammateRailClock->start();

    const QVariantList creators =
        QSettings().value(kTeammateCreatorSessionsSetting).toList();
    for (const QVariant &id : creators) {
        if (id.toInt() > 0)
            m_teammateCreatorSessions.insert(id.toInt());
    }
    const QVariantList unseen =
        QSettings().value(kTeammateDoneUnseenSetting).toList();
    for (const QVariant &id : unseen) {
        if (id.toInt() > 0)
            m_teammateDoneUnseen.insert(id.toInt());
    }

    applyTeammateRailVisibility();
    refreshTeammateRail();
    return scroll;
}

void MainWindow::scheduleTeammateRailRefresh()
{
    scheduleTeammateHomeRefresh();
    if (!m_teammateRailRefreshTimer || !m_teammateRail ||
        m_teammateRail->isHidden())
        return;
    if (!m_teammateRailRefreshTimer->isActive())
        m_teammateRailRefreshTimer->start();
}

void MainWindow::markTeammateDoneUnseen(int sessionId)
{
    const AgentSession *session = findAgentSession(sessionId);
    if (!session || !session->hasTeammateAttribution())
        return;
    if (sessionId == m_selectedAgentSessionId && m_agentTable &&
        m_agentTable->isVisible() && isActiveWindow())
        return;
    for (auto it = m_teammateDoneUnseen.begin();
         it != m_teammateDoneUnseen.end();) {
        if (!findAgentSession(*it))
            it = m_teammateDoneUnseen.erase(it);
        else
            ++it;
    }
    m_teammateDoneUnseen.insert(sessionId);
    saveTeammateDoneUnseen(m_teammateDoneUnseen);
    scheduleTeammateRailRefresh();
}

void MainWindow::clearTeammateDoneUnseen(int sessionId)
{
    if (!m_teammateDoneUnseen.remove(sessionId))
        return;
    saveTeammateDoneUnseen(m_teammateDoneUnseen);
    scheduleTeammateRailRefresh();
}

QHash<QString, forkmesh::teammates::Mood>
MainWindow::assessTeammateMoods(const QList<Config> &teammates, qint64 nowMs)
{
    struct MoodRow {
        const Config *config = nullptr;
        QString status;
        bool inFlight = false;
        bool doneUnseen = false;
        int sessionId = 0;
    };
    const int creatorSession = activeTeammateCreatorSession();
    QList<MoodRow> states;
    for (const Config &config : teammates) {
        MoodRow row;
        row.config = &config;
        const AgentSession *session =
            config.lastSessionId > 0 ? findAgentSession(config.lastSessionId)
                                     : nullptr;
        row.status = teammateRosterStatus(config, session);
        row.inFlight = railStatusInFlight(row.status);
        if (!row.inFlight && config.enabled && session &&
            session->status == AgentStatus::Failed)
            row.status = QStringLiteral("Failed");
        row.sessionId = session ? session->id : 0;
        row.doneUnseen = !row.inFlight && row.sessionId > 0 &&
                         session->status == AgentStatus::Success &&
                         m_teammateDoneUnseen.contains(row.sessionId);
        states.append(row);
    }
    RailTeamContext team;
    team.now = QDateTime::fromMSecsSinceEpoch(nowMs);
    team.size = states.size();
    team.creatorRunning = creatorSession > 0;
    QHash<QString, QList<const AgentSession *>> history;
    for (const MoodRow &row : std::as_const(states)) {
        team.working += row.inFlight ? 1 : 0;
        team.failing += row.status == QLatin1String("Failed") ? 1 : 0;
        history.insert(row.config->id, {});
    }
    for (const AgentSession &session : m_agentSessions) {
        if (!session.hasTeammateAttribution())
            continue;
        QStringList ids = session.teammateIds;
        if (!session.teammateId.isEmpty() && !ids.contains(session.teammateId))
            ids.prepend(session.teammateId);
        for (const QString &id : std::as_const(ids)) {
            auto it = history.find(id);
            if (it != history.end())
                it->append(&session);
        }
    }
    for (auto it = history.begin(); it != history.end(); ++it) {
        std::sort(it->begin(), it->end(),
                  [](const AgentSession *left, const AgentSession *right) {
                      return qMax(left->createdAtMs, left->startedAtMs) >
                             qMax(right->createdAtMs, right->startedAtMs);
                  });
        if (it->size() > 12)
            it->resize(12);
    }
    QHash<QString, forkmesh::teammates::Mood> moods;
    for (const MoodRow &row : std::as_const(states)) {
        const AgentSession *session =
            row.sessionId > 0 ? findAgentSession(row.sessionId) : nullptr;
        moods.insert(row.config->id,
                     forkmesh::teammates::assessMood(railMoodSignals(
                         *row.config, row.inFlight, row.doneUnseen, session,
                         history.value(row.config->id), team)));
    }
    return moods;
}

void MainWindow::refreshTeammateRail()
{
    if (!m_teammateRailLayout)
        return;
    if (QApplication::activePopupWidget()) {
        scheduleTeammateRailRefresh();
        return;
    }
    const int repoIndex = activeTeammateRepoIndex();
    const QString directory = teammatesDirPath(repoIndex);
    const LoadResult loaded =
        directory.isEmpty() ? LoadResult{} : Store(directory).load();
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const bool dark = currentThemeIsDark();
    const AgentChoice recent = forkmesh::teammates::recentAgentChoice();
    const int creatorSession = activeTeammateCreatorSession();

    struct RailRow {
        Config config;
        QString status;
        bool inFlight = false;
        bool doneUnseen = false;
        int sessionId = 0;
        forkmesh::teammates::Mood mood;
    };
    QList<RailRow> rows;
    QStringList signature{directory, dark ? QStringLiteral("dark")
                                          : QStringLiteral("light"),
                          recent.provider, recent.model,
                          QString::number(creatorSession)};
    for (const Config &saved : loaded.teammates) {
        RailRow row;
        row.config = teammateWithRuntime(saved, repoIndex);
        const AgentSession *session =
            row.config.lastSessionId > 0
                ? findAgentSession(row.config.lastSessionId)
                : nullptr;
        row.status = teammateRosterStatus(row.config, session);
        row.inFlight = railStatusInFlight(row.status);
        if (!row.inFlight && row.config.enabled && session &&
            session->status == AgentStatus::Failed)
            row.status = QStringLiteral("Failed");
        row.sessionId = session ? session->id : 0;
        row.doneUnseen = !row.inFlight && row.sessionId > 0 &&
                         session->status == AgentStatus::Success &&
                         m_teammateDoneUnseen.contains(row.sessionId);
        rows.append(row);
    }
    std::stable_sort(rows.begin(), rows.end(),
                     [](const RailRow &left, const RailRow &right) {
                         return forkmesh::teammates::ranMoreRecently(
                             left.config, right.config);
                     });
    QList<Config> configs;
    for (const RailRow &row : std::as_const(rows))
        configs.append(row.config);
    const QHash<QString, forkmesh::teammates::Mood> moods =
        assessTeammateMoods(configs, nowMs);
    for (RailRow &row : rows)
        row.mood = moods.value(row.config.id);
    for (const RailRow &row : std::as_const(rows)) {
        signature << row.config.id << row.config.name << row.config.avatar
                  << row.config.specialty << row.status
                  << QString::number(row.sessionId)
                  << (row.doneUnseen ? QStringLiteral("done") : QString())
                  << compactAgo(row.config.lastRunAtMs, nowMs)
                  << QString::number(qRound(
                         forkmesh::teammates::runStaleness(
                             row.config, QDateTime::fromMSecsSinceEpoch(nowMs)) *
                         20))
                  << (row.config.enabled ? QStringLiteral("on")
                                         : QStringLiteral("off"))
                  << (row.config.schedule.active ? QStringLiteral("sched")
                                                 : QString())
                  << QString::number(static_cast<int>(
                         forkmesh::teammates::runWindowState(
                             row.config,
                             QDateTime::fromMSecsSinceEpoch(nowMs))))
                  << row.mood.key << row.mood.reasons.join(QLatin1Char(','));
    }
    const QString joined = signature.join(QChar(0x1f));
    if (joined == m_teammateRailSignature)
        return;
    m_teammateRailSignature = joined;

    while (QLayoutItem *item = m_teammateRailLayout->takeAt(0)) {
        if (QWidget *widget = item->widget())
            widget->deleteLater();
        delete item;
    }

    for (const RailRow &row : std::as_const(rows)) {
        const AgentSession *session =
            row.sessionId > 0 ? findAgentSession(row.sessionId) : nullptr;
        m_teammateRailLayout->addWidget(
            buildTeammateRailItem(row.config, row.status, row.inFlight,
                                  row.doneUnseen, session, row.mood),
            0, Qt::AlignHCenter);
    }

    auto *create = new QPushButton;
    create->setObjectName(QStringLiteral("teammateRailCreateButton"));
    create->setCursor(Qt::PointingHandCursor);
    create->setFixedSize(kTeammateRailFace, kTeammateRailFace);
    create->setAccessibleName(QStringLiteral("Create needed special agent"));
    create->setProperty("working", creatorSession > 0);
    if (creatorSession > 0) {
        setOcticon(create, QStringLiteral("sparkle"), 16);
        create->setToolTip(
            QStringLiteral("Looking through logs, commits, docs, errors and "
                           "chats for the special agent this repository needs "
                           "(session #%1). Click to watch.")
                .arg(creatorSession));
        connect(create, &QPushButton::clicked, this,
                [this, creatorSession] { switchToAgentsTab(creatorSession); });
    } else {
        setOcticon(create, QStringLiteral("plus"), 16);
        create->setToolTip(
            directory.isEmpty()
                ? QStringLiteral("Open a repository with a working tree to "
                                 "create a special agent for it")
                : QStringLiteral("Create needed special agent: an agent reads the "
                                 "logs, commits, docs, errors, chats and past "
                                 "agent runs, then adds the special agent this "
                                 "repository is missing"));
        create->setEnabled(!directory.isEmpty());
        connect(create, &QPushButton::clicked, this,
                [this] { createNeededTeammate(); });
    }
    auto *createCaption = new QLabel(creatorSession > 0
                                         ? QStringLiteral("Finding…")
                                         : QStringLiteral("Needed"));
    createCaption->setObjectName(QStringLiteral("teammateRailLastRun"));
    createCaption->setAlignment(Qt::AlignHCenter);
    auto *createHost = new QWidget;
    createHost->setObjectName(QStringLiteral("teammateRailCreate"));
    auto *createLayout = new QVBoxLayout(createHost);
    createLayout->setContentsMargins(0, 0, 0, 0);
    createLayout->setSpacing(2);
    createLayout->addWidget(create, 0, Qt::AlignHCenter);
    createLayout->addWidget(createCaption, 0, Qt::AlignHCenter);
    m_teammateRailLayout->addWidget(createHost, 0, Qt::AlignHCenter);
    m_teammateRailLayout->addStretch(1);
}

QWidget *MainWindow::buildTeammateRailItem(const Config &config,
                                           const QString &status,
                                           bool inFlight,
                                           bool doneUnseen,
                                           const AgentSession *session,
                                           const forkmesh::teammates::Mood &mood)
{
    const bool dark = currentThemeIsDark();
    auto *item = new RailItem;
    item->setObjectName(QStringLiteral("teammateRailItem"));
    item->setProperty("teammateId", config.id);
    item->setProperty("status", status);
    item->setProperty("doneUnseen", doneUnseen);
    const double staleness =
        inFlight ? 0.0 : forkmesh::teammates::runStaleness(config);
    item->setProperty("staleness", staleness);
    item->setTint(config.enabled ? railFreshnessColor(staleness, dark)
                                 : railStatusColor(QString(), dark),
                  dark);
    const AgentChoice primary = forkmesh::teammates::agentChoiceFor(config);
    const int sessionId = session ? session->id : 0;
    const QString modelLabel = agentModelLabel(primary.model).isEmpty()
                                   ? agentProviderName(primary.provider)
                                   : agentModelLabel(primary.model);
    const RailFaceLook look =
        railFaceLook(config, status, inFlight, doneUnseen, sessionId, mood,
                     modelLabel, dark);
    item->setProperty("runWindowState", static_cast<int>(look.windowState));
    auto *avatar = new RailFaceButton(
        teammateFacePixmap(config.avatar, primary.provider, primary.model,
                           kTeammateRailSquare * 11 / 10),
        look.ringColor, look.ring, dark, mood.motion, mood.intensity, look.phase,
        kTeammateRailSquare);
    avatar->setParent(item);
    avatar->move(kTeammateRailInset, kTeammateRailInset);
    avatar->setObjectName(QStringLiteral("teammateRailAvatar"));
    avatar->setProperty("ring", static_cast<int>(look.ring));
    avatar->setProperty("motion", forkmesh::teammates::moodMotionName(mood.motion));
    avatar->setCursor(Qt::PointingHandCursor);
    avatar->setAccessibleName(look.accessibleName);
    avatar->setToolTip(look.tooltip);
    avatar->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(avatar, &QPushButton::clicked, this,
            [this, id = config.id] { openTeammateHome(id); });
    connect(avatar, &QWidget::customContextMenuRequested, this,
            [this, id = config.id] { showTeammateRailMenu(id, nullptr); });
    item->setFixedSize(kTeammateRailItemWidth, kTeammateRailItemWidth);
    item->setProperty("mood", mood.key);

    auto *name = new QLabel(item);
    name->setObjectName(QStringLiteral("teammateRailName"));
    name->setAlignment(Qt::AlignHCenter);
    name->setAttribute(Qt::WA_TransparentForMouseEvents);
    name->ensurePolished();
    name->setText(name->fontMetrics().elidedText(
        config.name, Qt::ElideRight, kTeammateRailSquare - 6));
    name->adjustSize();
    name->resize(qMin(name->width(), kTeammateRailSquare), name->height());
    name->move((kTeammateRailItemWidth - name->width()) / 2,
               kTeammateRailInset + kTeammateRailSquare - name->height() - 2);

    const QList<QPushButton *> hud =
        addTeammateRailHud(item, config, inFlight, sessionId, mood,
                           QStringLiteral("teammateRail"), kTeammateRailHud,
                           kTeammateRailMood);
    const auto place = [](QPushButton *button, int x, int y) {
        button->move(x, y);
        button->raise();
    };
    place(hud.at(0), kTeammateRailHudInset,
          kTeammateRailHudInset + (kTeammateRailMood - kTeammateRailHud) / 2);
    place(hud.at(1), (kTeammateRailItemWidth - kTeammateRailMood) / 2,
          kTeammateRailHudInset);
    place(hud.at(2),
          kTeammateRailItemWidth - kTeammateRailHudInset - kTeammateRailHud,
          kTeammateRailHudInset + (kTeammateRailMood - kTeammateRailHud) / 2);
    if (hud.size() > 3) {
        place(hud.at(3),
              kTeammateRailItemWidth - kTeammateRailHudInset - kTeammateRailHud,
              kTeammateRailHudInset + kTeammateRailMood + 2);
        item->addHoverButton(hud.at(3));
    }
    return item;
}

QPushButton *MainWindow::buildTeammateVideoTile(const Config &config,
                                                const QString &status,
                                                bool inFlight, bool doneUnseen,
                                                const AgentSession *session,
                                                const forkmesh::teammates::Mood &mood,
                                                const QString &caption)
{
    const bool dark = currentThemeIsDark();
    const AgentChoice primary = forkmesh::teammates::agentChoiceFor(config);
    const int sessionId = session ? session->id : 0;
    QString ringStatus = status;
    if (!inFlight && config.enabled && session &&
        session->status == AgentStatus::Failed)
        ringStatus = QStringLiteral("Failed");
    const QString modelLabel = agentModelLabel(primary.model).isEmpty()
                                   ? agentProviderName(primary.provider)
                                   : agentModelLabel(primary.model);
    const RailFaceLook look =
        railFaceLook(config, ringStatus, inFlight, doneUnseen, sessionId, mood,
                     modelLabel, dark);
    const QString meta =
        QStringLiteral("%1 · %2")
            .arg(doneUnseen ? QStringLiteral("Done") : status,
                 compactAgo(config.lastRunAtMs,
                            QDateTime::currentMSecsSinceEpoch()));
    auto *tile = new RailVideoTile(
        teammateFacePixmap(config.avatar, primary.provider, primary.model,
                           kTeammateTileFace),
        look.ringColor, look.ring, dark, mood.motion, mood.intensity, look.phase,
        config.name, meta, caption);
    tile->setObjectName(QStringLiteral("teammateCard"));
    tile->setProperty("videoTile", true);
    tile->setProperty("teammateId", config.id);
    tile->setProperty("teammateName", config.name);
    tile->setProperty("caption", caption);
    tile->setProperty("doneUnseen", doneUnseen);
    tile->setProperty("ring", static_cast<int>(look.ring));
    tile->setProperty("mood", mood.key);
    tile->setProperty("runWindowState", static_cast<int>(look.windowState));
    tile->setCursor(Qt::PointingHandCursor);
    tile->setAccessibleName(look.accessibleName);
    tile->setAccessibleDescription(caption);
    tile->setToolTip(QStringLiteral("%1<br><br>%2")
                         .arg(look.tooltip, caption.toHtmlEscaped()));
    tile->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tile, &QPushButton::clicked, this,
            [this, id = config.id] { showTeammateHome(id); });
    connect(tile, &QWidget::customContextMenuRequested, this,
            [this, id = config.id] { showTeammateRailMenu(id, nullptr); });

    const QList<QPushButton *> hud =
        addTeammateRailHud(tile, config, inFlight, sessionId, mood,
                           QStringLiteral("teammateTile"), kTeammateTileHud,
                           kTeammateTileMood);
    tile->addHud(hud.at(0), Qt::AlignLeft, 0, false);
    tile->addHud(hud.at(1), Qt::AlignHCenter, 0, false);
    tile->addHud(hud.at(2), Qt::AlignRight, 0, false);
    if (hud.size() > 3)
        tile->addHud(hud.at(3), Qt::AlignRight, 1, true);
    return tile;
}

QList<QPushButton *> MainWindow::addTeammateRailHud(QWidget *host,
                                                    const Config &config,
                                                    bool inFlight, int sessionId,
                                                    const forkmesh::teammates::Mood &mood,
                                                    const QString &namePrefix,
                                                    int hudSide, int moodSide)
{
    const bool dark = currentThemeIsDark();
    QList<QPushButton *> buttons;
    const auto finish = [&buttons](QPushButton *button, int side) {
        button->setFixedSize(side, side);
        button->setCursor(Qt::PointingHandCursor);
        buttons.append(button);
    };

    QPushButton *play =
        makeTeammatePlayButton(config, inFlight, sessionId, hudSide, host);
    play->setObjectName(namePrefix + QStringLiteral("Play"));
    finish(play, hudSide);

    auto *feeling = new RailMoodBadge(mood.emoji, mood.wantsAttention, dark, moodSide);
    feeling->setParent(host);
    feeling->setObjectName(namePrefix + QStringLiteral("Mood"));
    feeling->setProperty("mood", mood.key);
    feeling->setAccessibleName(
        QStringLiteral("%1 feels %2").arg(config.name, mood.label.toLower()));
    feeling->setToolTip(
        QStringLiteral("<b>%1 %2</b><br>%3 %4.<br><i>Weighed %5 signals · click "
                       "to open %3's page</i>")
            .arg(mood.emoji, mood.label.toHtmlEscaped(), config.name.toHtmlEscaped(),
                 (mood.reasons.isEmpty()
                      ? QStringLiteral("is taking it easy")
                      : QLocale().createSeparatedList(mood.reasons))
                     .toHtmlEscaped())
            .arg(forkmesh::teammates::moodFactorCount()));
    connect(feeling, &QPushButton::clicked, this,
            [this, id = config.id] { openTeammateHome(id); });
    finish(feeling, moodSide);

    auto *more = new QPushButton(QString::fromUtf8("\xE2\x8B\xAF"), host);
    more->setObjectName(namePrefix + QStringLiteral("MenuButton"));
    more->setProperty("teammateRailHud", true);
    more->setAccessibleName(QStringLiteral("More for %1").arg(config.name));
    more->setToolTip(QStringLiteral("Every option for %1").arg(config.name));
    connect(more, &QPushButton::clicked, this,
            [this, more, id = config.id] { showTeammateRailMenu(id, more); });
    finish(more, hudSide);

    if (config.workflow == forkmesh::teammates::kWorkflowContributorPayouts) {
        auto *payouts = new QPushButton(host);
        payouts->setObjectName(namePrefix + QStringLiteral("ProcessPayouts"));
        payouts->setProperty("teammateRailHud", true);
        setOcticon(payouts, QStringLiteral("credit-card"), hudSide * 9 / 14);
        payouts->setAccessibleName(
            QStringLiteral("Process payments with %1").arg(config.name));
        payouts->setToolTip(
            QStringLiteral("Process payments: %1 reconciles reviewed "
                           "contributor rewards and pays them from the "
                           "desktop-linked wallet")
                .arg(config.name));
        connect(payouts, &QPushButton::clicked, this,
                [this, id = config.id] { processTeammatePayoutsFromRail(id); });
        finish(payouts, hudSide);
    }
    return buttons;
}

void MainWindow::showTeammateRailMenu(const QString &id, QWidget *anchor)
{
    const int repoIndex = activeTeammateRepoIndex();
    const LoadResult loaded = Store(teammatesDirPath(repoIndex)).load();
    const auto it = std::find_if(
        loaded.teammates.cbegin(), loaded.teammates.cend(),
        [&id](const Config &candidate) { return candidate.id == id; });
    if (it == loaded.teammates.cend()) {
        scheduleTeammateRailRefresh();
        return;
    }
    const Config teammate = teammateWithRuntime(*it, repoIndex);
    const AgentSession *session = ownTeammateSession(teammate);
    const int sessionId = session ? session->id : 0;
    const bool inFlight =
        railStatusInFlight(teammateRosterStatus(teammate, session));
    const bool scheduled =
        teammate.schedule.mode != forkmesh::teammates::kScheduleOnDemand;

    QMenu menu(this);
    menu.setObjectName(QStringLiteral("teammateRailMenu"));
    menu.setToolTipsVisible(true);
    auto add = [&menu](const QString &objectName, const QString &label,
                       const QString &icon, auto &&slot) {
        QAction *action = menu.addAction(label);
        action->setObjectName(objectName);
        if (!icon.isEmpty())
            action->setIcon(themedOcticon(icon, currentThemeIsDark()
                                                    ? Theme::remapColor(QColor("#c9d1d9"))
                                                    : Theme::remapColor(QColor("#24292f")),
                                          14));
        QObject::connect(action, &QAction::triggered, &menu,
                         std::forward<decltype(slot)>(slot));
        return action;
    };

    if (inFlight) {
        add(QStringLiteral("teammateRailStop"), QStringLiteral("Stop"),
            QStringLiteral("stop"), [this, id] {
                stopTeammate(id);
                scheduleTeammateRailRefresh();
            });
    } else {
        add(QStringLiteral("teammateRailRun"), QStringLiteral("Run now"),
            QStringLiteral("play"), [this, id] { runTeammateFromRail(id); });
    }
    if (teammate.workflow.isEmpty()) {
        add(QStringLiteral("teammateRailPrompt"),
            inFlight ? QStringLiteral("Send a message…")
                     : QStringLiteral("Run with a prompt…"),
            QStringLiteral("comment"), [this, id, name = teammate.name] {
                bool ok = false;
                const QString prompt = QInputDialog::getMultiLineText(
                    this, QStringLiteral("Prompt %1").arg(name),
                    QStringLiteral("What should %1 do?").arg(name), QString(),
                    &ok);
                if (ok && !prompt.trimmed().isEmpty())
                    sendTeammatePrompt(id, prompt);
                scheduleTeammateRailRefresh();
            });
    }
    QAction *openSession = add(
        QStringLiteral("teammateRailOpenSession"),
        sessionId > 0 ? QStringLiteral("Open agent session #%1").arg(sessionId)
                      : QStringLiteral("Open agent session"),
        QStringLiteral("terminal"),
        [this, sessionId] { switchToAgentsTab(sessionId); });
    openSession->setEnabled(sessionId > 0);
    menu.addSeparator();
    add(QStringLiteral("teammateRailPage"),
        QStringLiteral("Open %1's page").arg(teammate.name),
        QStringLiteral("home"), [this, id] { openTeammateHome(id); });
    add(QStringLiteral("teammateRailProfile"), QStringLiteral("Open profile"),
        QStringLiteral("person"),
        [this, id] { openTeamSectionForTeammate(id); });
    add(QStringLiteral("teammateRailEdit"), QStringLiteral("Edit settings"),
        QStringLiteral("gear"),
        [this, id] { openTeamSectionForTeammate(id, true); });
    add(QStringLiteral("teammateRailSkills"), QStringLiteral("Edit skills"),
        QStringLiteral("pencil"),
        [this, id] { openTeamSectionForTeammate(id, true, true); });
    add(QStringLiteral("teammateRailBriefing"), QStringLiteral("Show briefing"),
        QStringLiteral("history"), [this, id] {
            showSection(kTeamSectionIndex);
            QTimer::singleShot(0, this, [this, id] { showTeammateBriefing(id); });
        });
    if (teammate.workflow.isEmpty()) {
        add(QStringLiteral("teammateRailRefreshBriefing"),
            QStringLiteral("Refresh briefing"), QStringLiteral("sparkle"),
            [this, id] { generateTeammateBriefing(id); });
    }
    menu.addSeparator();
    if (scheduled) {
        if (teammate.schedule.active && teammate.enabled) {
            add(QStringLiteral("teammateRailPauseSchedule"),
                QStringLiteral("Pause schedule (%1)")
                    .arg(forkmesh::teammates::humanSchedule(teammate.schedule)),
                QStringLiteral("clock"),
                [this, id] { setTeammateScheduleActive(id, false); });
        } else {
            add(QStringLiteral("teammateRailStartSchedule"),
                QStringLiteral("Start schedule (%1)")
                    .arg(forkmesh::teammates::humanSchedule(teammate.schedule)),
                QStringLiteral("clock"),
                [this, id] { setTeammateScheduleActive(id, true); });
        }
    }
    if (teammate.enabled) {
        add(QStringLiteral("teammateRailPause"), QStringLiteral("Pause special agent"),
            QString(), [this, id] { setTeammateAvailable(id, false); });
    } else {
        add(QStringLiteral("teammateRailResume"),
            QStringLiteral("Make available"), QString(),
            [this, id] { setTeammateAvailable(id, true); });
    }
    menu.addSeparator();
    add(QStringLiteral("teammateRailCreate"),
        QStringLiteral("Create needed special agent"), QStringLiteral("plus"),
        [this] { createNeededTeammate(); });
    add(QStringLiteral("teammateRailRemove"), QStringLiteral("Remove special agent…"),
        QStringLiteral("trash"), [this, id] {
            deleteTeammate(id);
            scheduleTeammateRailRefresh();
        });
    menu.exec(anchor ? anchor->mapToGlobal(QPoint(0, anchor->height()))
                     : QCursor::pos());
}

void MainWindow::runTeammateFromRail(const QString &id)
{
    const int repoIndex = activeTeammateRepoIndex();
    const LoadResult loaded = Store(teammatesDirPath(repoIndex)).load();
    const auto it = std::find_if(
        loaded.teammates.cbegin(), loaded.teammates.cend(),
        [&id](const Config &candidate) { return candidate.id == id; });
    if (it == loaded.teammates.cend()) {
        flashMessage(QStringLiteral("That special agent no longer exists."), true);
        scheduleTeammateRailRefresh();
        return;
    }
    const Config teammate = teammateWithRuntime(*it, repoIndex);
    const AgentSession *session = ownTeammateSession(teammate);
    if (railStatusInFlight(teammateRosterStatus(teammate, session))) {
        stopTeammate(id);
        scheduleTeammateRailRefresh();
        return;
    }
    if (!teammate.enabled) {
        flashMessage(QStringLiteral("%1 is paused. Choose Make available from "
                                    "the special agent's menu first.")
                         .arg(teammate.name),
                     true);
        return;
    }
    if (!teammate.permissions.contains(QStringLiteral("read-repositories"))) {
        flashMessage(QStringLiteral("%1 needs Read repositories permission "
                                    "before work can start.")
                         .arg(teammate.name),
                     true);
        return;
    }
    const int sessionId =
        runTeammate(id, QStringLiteral("On demand"),
                    forkmesh::teammates::kTaskTypeOnDemand, repoIndex);
    if (sessionId > 0) {
        flashMessage(QStringLiteral("%1 is running · session #%2")
                         .arg(teammate.name)
                         .arg(sessionId));
    } else {
        flashMessage(QStringLiteral("%1 could not start. Open the special agent's "
                                    "profile for details.")
                         .arg(teammate.name),
                     true);
    }
    scheduleTeammateRailRefresh();
}

void MainWindow::processTeammatePayoutsFromRail(const QString &id)
{
    const LoadResult loaded =
        Store(teammatesDirPath(activeTeammateRepoIndex())).load();
    const auto it = std::find_if(
        loaded.teammates.cbegin(), loaded.teammates.cend(),
        [&id](const Config &candidate) { return candidate.id == id; });
    if (it == loaded.teammates.cend()) {
        flashMessage(QStringLiteral("That special agent no longer exists."), true);
        scheduleTeammateRailRefresh();
        return;
    }
    if (!it->enabled) {
        flashMessage(QStringLiteral("%1 is paused. Choose Make available from "
                                    "the special agent's menu first.")
                         .arg(it->name),
                     true);
        return;
    }
    if (!it->permissions.contains(QStringLiteral("process-payouts"))) {
        flashMessage(QStringLiteral("%1 needs Process reviewed contributor "
                                    "payouts permission before it can pay.")
                         .arg(it->name),
                     true);
        return;
    }
    runContributorPayoutTeammate(id, QStringLiteral("On demand · rail"));
    scheduleTeammateRailRefresh();
}

void MainWindow::openTeamSectionForTeammate(const QString &id, bool edit,
                                            bool focusSkills)
{
    m_teammatesStatusFilter.clear();
    showSection(kTeamSectionIndex);
    if (QPushButton *all = m_teammateMetricButtons.value(QString()))
        all->setChecked(true);
    QTimer::singleShot(0, this, [this, id, edit, focusSkills] {
        if (edit)
            openTeammateEditor(id, focusSkills);
        else
            showTeammateDetail(id);
    });
}

void MainWindow::setTeammateScheduleActive(const QString &id, bool active)
{
    if (active) {
        enableTeammateSchedule(id);
        scheduleTeammateRailRefresh();
        return;
    }
    Store store(teammatesDirPath());
    LoadResult loaded = store.load();
    auto it = std::find_if(
        loaded.teammates.begin(), loaded.teammates.end(),
        [&id](const Config &candidate) { return candidate.id == id; });
    if (it == loaded.teammates.end())
        return;
    it->schedule.active = false;
    QString error;
    if (!store.save(&*it, &error)) {
        flashMessage(error, true);
        return;
    }
    flashMessage(QStringLiteral("Paused %1's schedule.").arg(it->name));
    loadTeammatesPanel();
    scheduleTeammateRailRefresh();
}

void MainWindow::setTeammateAvailable(const QString &id, bool available)
{
    Store store(teammatesDirPath());
    LoadResult loaded = store.load();
    auto it = std::find_if(
        loaded.teammates.begin(), loaded.teammates.end(),
        [&id](const Config &candidate) { return candidate.id == id; });
    if (it == loaded.teammates.end())
        return;
    it->enabled = available;
    QString error;
    if (!store.save(&*it, &error)) {
        flashMessage(error, true);
        return;
    }
    flashMessage(available ? QStringLiteral("%1 is available for work.")
                                 .arg(it->name)
                           : QStringLiteral("Paused %1.").arg(it->name));
    loadTeammatesPanel();
    scheduleTeammateRailRefresh();
}

int MainWindow::activeTeammateCreatorSession() const
{
    for (const AgentSession &session : m_agentSessions) {
        if (m_teammateCreatorSessions.contains(session.id) &&
            (session.status == AgentStatus::Running ||
             session.status == AgentStatus::Queued ||
             session.status == AgentStatus::Waiting))
            return session.id;
    }
    return 0;
}

QString MainWindow::buildTeammateCreatorPrompt(int repoIndex) const
{
    if (repoIndex < 0 || repoIndex >= m_repositories.size())
        return {};
    const RepositoryRecord &repo = m_repositories.at(repoIndex);
    const LoadResult loaded = Store(teammatesDirPath(repoIndex)).load();

    QStringList teammates;
    for (const Config &teammate : loaded.teammates) {
        teammates << QStringLiteral("- %1 (%2)%3: %4")
                         .arg(teammate.name, teammate.id,
                              teammate.focus.isEmpty()
                                  ? QString()
                                  : QStringLiteral(" · ") + teammate.focus,
                              teammate.specialty);
    }

    QList<const AgentSession *> sessions;
    for (const AgentSession &session : m_agentSessions) {
        if (session.owner == repo.owner && session.name == repo.name)
            sessions.append(&session);
    }
    std::sort(sessions.begin(), sessions.end(),
              [](const AgentSession *a, const AgentSession *b) {
                  return a->createdAtMs > b->createdAtMs;
              });
    QStringList runs;
    QStringList failures;
    for (const AgentSession *session : std::as_const(sessions)) {
        if (runs.size() < 40) {
            runs << QStringLiteral("- #%1 [%2] %3%4")
                        .arg(session->id)
                        .arg(agentStatusText(session->status),
                             session->issueTitle.simplified().left(140),
                             session->teammateName.isEmpty()
                                 ? QString()
                                 : QStringLiteral(" (special agent %1)")
                                       .arg(session->teammateName));
        }
        if (failures.size() < 20 && !session->lastError.trimmed().isEmpty()) {
            failures << QStringLiteral("- #%1: %2")
                            .arg(session->id)
                            .arg(session->lastError.simplified().left(240));
        }
    }

    const QStringList avatars{
        QStringLiteral("auto"),     QStringLiteral("code-steward"),
        QStringLiteral("marketing"), QStringLiteral("mobile-developer"),
        QStringLiteral("quality"),  QStringLiteral("ceo"),
        QStringLiteral("sage"),     QStringLiteral("coo"),
        QStringLiteral("cpa"),      QStringLiteral("fixer"),
        QStringLiteral("tempo"),    QStringLiteral("reaper"),
        QStringLiteral("sales"),    QStringLiteral("sol"),
        QStringLiteral("terra"),    QStringLiteral("luna"),
        QStringLiteral("opus"),     QStringLiteral("fable"),
    };
    QStringList permissions = forkmesh::teammates::knownPermissions();
    permissions.removeAll(QStringLiteral("process-payouts"));
    permissions.removeAll(QStringLiteral("merge"));
    permissions.removeAll(QStringLiteral("run-git"));
    permissions.removeAll(QStringLiteral("run-tests"));
    permissions.removeAll(QStringLiteral("read-forkmesh-web"));

    QStringList sections;
    sections << QStringLiteral(
                    "Find the special agent this repository needs most and define it.\n"
                    "A ForkMesh special agent is a persistent AI agent with a role, "
                    "instructions, skills and a schedule. It is stored in "
                    ".forkmesh/teammates and runs on the model its operator "
                    "used most recently.");
    sections << QStringLiteral("Repository\n%1/%2 at %3")
                    .arg(repo.owner, repo.name,
                         QDir::toNativeSeparators(repo.localPath));
    sections << QStringLiteral(
                    "Read everything you can reach before you decide\n"
                    "- Commits: recent history, authors, churn hot spots, "
                    "reverts and fixes that keep coming back (git log --stat, "
                    "git shortlog).\n"
                    "- Docs: README, docs/, CONTRIBUTING, plans, roadmaps and "
                    "TODOs.\n"
                    "- Errors and logs: ForkMesh logs in %1 and diagnostics in "
                    "%2, test failures, crash reports and the failed runs "
                    "listed below.\n"
                    "- Chats, issues, tasks and notes through the ForkMesh MCP "
                    "tools when they are available (read_chat_messages, "
                    "search_issues, list_tasks, list_notes, search_notes).\n"
                    "- Past agent runs (listed below) and the existing "
                    "special agents, so the new one fills a real gap instead of "
                    "repeating a role that already exists.")
                    .arg(QDir::toNativeSeparators(forkmesh::paths::logsDir()),
                         QDir::toNativeSeparators(
                             forkmesh::paths::diagnosticsDir()));
    sections << QStringLiteral(
        "Only read. Do not edit files, commit, push, open pull requests, file "
        "issues or post to chats. ForkMesh saves the special agent from your reply.");
    sections << QStringLiteral("Existing special agents\n%1")
                    .arg(teammates.isEmpty() ? QStringLiteral("None yet.")
                                             : teammates.join(QLatin1Char('\n')));
    sections << QStringLiteral("Recent agent runs in this repository\n%1")
                    .arg(runs.isEmpty() ? QStringLiteral("None yet.")
                                        : runs.join(QLatin1Char('\n')));
    if (!failures.isEmpty())
        sections << QStringLiteral("Recent agent errors\n%1")
                        .arg(failures.join(QLatin1Char('\n')));
    sections << QStringLiteral(
                    "Reply format\n"
                    "Explain in a few sentences which gap you found and the "
                    "evidence for it. Then end your reply with exactly one "
                    "fenced ```json block holding the special agent:\n"
                    "{\n"
                    "  \"id\": \"kebab-case-id\",\n"
                    "  \"name\": \"Short first name\",\n"
                    "  \"avatar\": one of %1,\n"
                    "  \"specialty\": \"Role title\",\n"
                    "  \"focus\": \"Two or three words\",\n"
                    "  \"description\": \"What they own and why the repository needs "
                    "them\",\n"
                    "  \"instructions\": \"Concrete standing instructions "
                    "grounded in the evidence you found\",\n"
                    "  \"skills\": [\"Skill\", ...],\n"
                    "  \"schedule\": {\"mode\": \"on-demand\" | \"hourly\" | "
                    "\"daily\" | \"weekdays\" | \"weekly\", \"time\": \"HH:mm\"},\n"
                    "  \"permissions\": a subset of %2,\n"
                    "  \"logMonitors\": optional, e.g. [{\"badge\": \"ERROR\", "
                    "\"threshold\": 10, \"taskType\": \"error-logs\"}]\n"
                    "}\n"
                    "The special agent is saved with its schedule and monitors "
                    "paused until the operator starts them.")
                    .arg(avatars.join(QStringLiteral(", ")),
                         permissions.join(QStringLiteral(", ")));
    return sections.join(QStringLiteral("\n\n"));
}

void MainWindow::createNeededTeammate()
{
    const int running = activeTeammateCreatorSession();
    if (running > 0) {
        flashMessage(QStringLiteral("Already looking for the special agent this "
                                    "repository needs · session #%1")
                         .arg(running));
        switchToAgentsTab(running);
        return;
    }
    const int repoIndex = activeTeammateRepoIndex();
    if (repoIndex < 0 || teammatesDirPath(repoIndex).isEmpty()) {
        flashMessage(QStringLiteral("Open a repository with a working tree "
                                    "first; a special agent lives in the repository "
                                    "it works on."),
                     true);
        return;
    }
    const AgentChoice recent = forkmesh::teammates::recentAgentChoice();
    const QString provider =
        recent.provider.isEmpty() ? defaultAgentProvider() : recent.provider;
    const int sessionId = startAdHocAgentForRepo(
        repoIndex, buildTeammateCreatorPrompt(repoIndex), provider,
        /*createPr=*/false, recent.model,
        QStringLiteral("Create needed special agent"), false,
        /*switchToTab=*/false, QString(), QString(), QString(), 50,
        /*directCheckout=*/!QSettings().value(kTeammateWorktreesSetting, false).toBool());
    if (sessionId <= 0) {
        flashMessage(QStringLiteral("Could not start the special agent finder."),
                     true);
        return;
    }
    m_teammateCreatorSessions.insert(sessionId);
    QVariantList ids;
    for (int id : std::as_const(m_teammateCreatorSessions))
        ids.append(id);
    QSettings().setValue(kTeammateCreatorSessionsSetting, ids);
    flashMessage(QStringLiteral("Reading logs, commits, docs, errors and chats "
                                "for the special agent this repository needs · "
                                "session #%1")
                     .arg(sessionId));
    m_teammateRailSignature.clear();
    scheduleTeammateRailRefresh();
}

bool MainWindow::adoptCreatedTeammate(int repoIndex, const QString &output,
                                      QString *name)
{
    const QString directory = teammatesDirPath(repoIndex);
    if (directory.isEmpty()) {
        flashMessage(QStringLiteral("The special agent finder's repository has no "
                                    "working tree to store a special agent in."),
                     true);
        return false;
    }
    Config teammate;
    QString error;
    if (!forkmesh::teammates::teammateFromAgentOutput(output, &teammate,
                                                      &error)) {
        flashMessage(QStringLiteral("The special agent finder finished without a "
                                    "usable special agent: %1")
                         .arg(error),
                     true);
        return false;
    }
    Store store(directory);
    const LoadResult loaded = store.load();
    const QString baseId = teammate.id;
    for (int suffix = 2;
         std::any_of(loaded.teammates.cbegin(), loaded.teammates.cend(),
                     [&teammate](const Config &existing) {
                         return existing.id == teammate.id;
                     });
         ++suffix) {
        teammate.id = QStringLiteral("%1-%2").arg(baseId).arg(suffix);
    }
    if (!store.save(&teammate, &error)) {
        flashMessage(error, true);
        return false;
    }
    if (name)
        *name = teammate.name;
    flashMessage(QStringLiteral("%1 joined the special agents as %2.")
                     .arg(teammate.name, teammate.specialty));
    loadTeammatesPanel();
    m_teammateRailSignature.clear();
    refreshTeammateRail();
    return true;
}
